#include "habit_data.h"

#include <furi.h>
#include <furi_hal_rtc.h>
#include <furi_hal_random.h>
#include <storage/storage.h>
#include <string.h>

#define TAG "HabitData"

#define HABIT_FILE_PATH    APP_DATA_PATH("habits.dat")
#define HABIT_TMP_PATH     APP_DATA_PATH("habits.tmp")
#define HABIT_FILE_MAGIC   0x31544248UL /* "HBT1" */
#define HABIT_FILE_VERSION 1

/* The on-disk format is the raw struct: adding the id must not change its size. */
_Static_assert(sizeof(Habit) == 404, "Habit layout changed: bump HABIT_FILE_VERSION");

/* ---- Date helpers (Howard Hinnant's civil date algorithms) ---- */

uint32_t habit_days_from_civil(int32_t year, uint32_t month, uint32_t day) {
    year -= month <= 2;
    const int32_t era = (year >= 0 ? year : year - 399) / 400;
    const uint32_t yoe = (uint32_t)(year - era * 400);
    const uint32_t doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(era * 146097 + (int32_t)doe - 719468);
}

void habit_civil_from_days(uint32_t days, int32_t* year, uint32_t* month, uint32_t* day) {
    const int32_t z = (int32_t)days + 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;
    *day = doy - (153 * mp + 2) / 5 + 1;
    *month = mp < 10 ? mp + 3 : mp - 9;
    *year = (int32_t)yoe + era * 400 + (*month <= 2);
}

uint8_t habit_weekday(uint32_t days) {
    /* 1970-01-01 was a Thursday. */
    return (uint8_t)((days + 4) % 7);
}

uint32_t habit_today(void) {
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    return habit_days_from_civil(dt.year, dt.month, dt.day);
}

/* ---- Habit operations ---- */

void habit_sync(Habit* habit, uint32_t today) {
    if(habit->last_day == 0) {
        memset(habit->counts, 0, sizeof(habit->counts));
        habit->last_day = today;
        return;
    }
    /* If the clock moved backwards keep the history as-is. */
    if(today <= habit->last_day) return;

    uint32_t gap = today - habit->last_day;
    if(gap >= HABIT_HISTORY_DAYS) {
        memset(habit->counts, 0, sizeof(habit->counts));
    } else {
        for(uint32_t d = habit->last_day + 1; d <= today; d++) {
            habit->counts[d % HABIT_HISTORY_DAYS] = 0;
        }
    }
    habit->last_day = today;
}

static bool habit_day_in_range(const Habit* habit, uint32_t day) {
    return day <= habit->last_day && day + HABIT_HISTORY_DAYS > habit->last_day;
}

uint8_t habit_get(const Habit* habit, uint32_t day) {
    if(!habit_day_in_range(habit, day)) return 0;
    return habit->counts[day % HABIT_HISTORY_DAYS];
}

void habit_set(Habit* habit, uint32_t day, uint8_t value) {
    if(!habit_day_in_range(habit, day)) return;
    habit->counts[day % HABIT_HISTORY_DAYS] = value;
}

bool habit_is_complete(const Habit* habit, uint32_t day) {
    return habit_get(habit, day) >= habit->target;
}

uint16_t habit_year_total(const Habit* habit, uint32_t today) {
    uint16_t total = 0;
    for(uint32_t i = 0; i < 365 && i <= today; i++) {
        if(habit_is_complete(habit, today - i)) total++;
    }
    return total;
}

uint16_t habit_streak(const Habit* habit, uint32_t today) {
    uint32_t day = today;
    /* Today still in progress shouldn't break the streak. */
    if(!habit_is_complete(habit, day)) {
        if(day == 0) return 0;
        day--;
    }
    uint16_t streak = 0;
    while(habit_is_complete(habit, day)) {
        streak++;
        if(day == 0) break;
        day--;
    }
    return streak;
}

/* ---- Store ---- */

HabitStore* habit_store_alloc(void) {
    HabitStore* store = malloc(sizeof(HabitStore));
    memset(store, 0, sizeof(HabitStore));
    return store;
}

void habit_store_free(HabitStore* store) {
    free(store);
}

int habit_store_add(HabitStore* store, const char* name, uint8_t target, uint32_t today) {
    if(store->count >= HABIT_MAX) return -1;
    Habit* habit = &store->habits[store->count];
    memset(habit, 0, sizeof(Habit));
    strlcpy(habit->name, name, HABIT_NAME_LEN);
    habit->target = target ? target : 1;
    habit_sync(habit, today);
    store->count++;
    habit_store_ensure_ids(store);
    return store->count - 1;
}

int habit_store_find_by_id(const HabitStore* store, uint16_t id) {
    if(id == 0) return -1;
    for(uint8_t i = 0; i < store->count; i++) {
        if(store->habits[i].id == id) return i;
    }
    return -1;
}

bool habit_store_ensure_ids(HabitStore* store) {
    bool assigned = false;
    for(uint8_t i = 0; i < store->count; i++) {
        Habit* habit = &store->habits[i];
        if(habit->id != 0) continue;
        uint16_t id;
        do {
            id = (uint16_t)furi_hal_random_get();
        } while(id == 0 || habit_store_find_by_id(store, id) >= 0);
        habit->id = id;
        assigned = true;
    }
    return assigned;
}

void habit_store_remove(HabitStore* store, uint8_t index) {
    if(index >= store->count) return;
    memmove(
        &store->habits[index],
        &store->habits[index + 1],
        sizeof(Habit) * (store->count - index - 1));
    store->count--;
    memset(&store->habits[store->count], 0, sizeof(Habit));
}

void habit_store_sync(HabitStore* store, uint32_t today) {
    for(uint8_t i = 0; i < store->count; i++) {
        habit_sync(&store->habits[i], today);
    }
}

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t habit_size;
    uint8_t count;
    uint8_t reserved[3];
} HabitFileHeader;

static bool habit_store_load_path(Storage* storage, HabitStore* store, const char* path) {
    File* file = storage_file_alloc(storage);
    bool ok = false;

    do {
        if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) break;

        HabitFileHeader header;
        if(storage_file_read(file, &header, sizeof(header)) != sizeof(header)) break;
        if(header.magic != HABIT_FILE_MAGIC || header.version != HABIT_FILE_VERSION ||
           header.habit_size != sizeof(Habit) || header.count > HABIT_MAX) {
            FURI_LOG_W(TAG, "Unrecognised data file, starting fresh");
            break;
        }

        size_t size = sizeof(Habit) * header.count;
        if(storage_file_read(file, store->habits, size) != size) break;

        store->count = header.count;
        for(uint8_t i = 0; i < store->count; i++) {
            Habit* habit = &store->habits[i];
            habit->name[HABIT_NAME_LEN - 1] = '\0';
            if(habit->target == 0 || habit->target > HABIT_TARGET_MAX) habit->target = 1;
        }
        ok = true;
    } while(false);

    if(!ok) store->count = 0;

    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

bool habit_store_load(HabitStore* store) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool ok = habit_store_load_path(storage, store, HABIT_FILE_PATH);
    if(!ok && storage_common_exists(storage, HABIT_TMP_PATH)) {
        /* A save was interrupted after the old file was replaced: recover the new copy. */
        FURI_LOG_W(TAG, "Recovering habits from temp file");
        ok = habit_store_load_path(storage, store, HABIT_TMP_PATH);
        if(ok) storage_common_rename(storage, HABIT_TMP_PATH, HABIT_FILE_PATH);
    }
    furi_record_close(RECORD_STORAGE);
    return ok;
}

/*
 * Writes to a temp file first and renames it over the real file, so a crash or
 * power loss mid-write never leaves a half-written habits.dat behind.
 */
bool habit_store_save(const HabitStore* store) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, STORAGE_APP_DATA_PATH_PREFIX);
    File* file = storage_file_alloc(storage);
    bool ok = false;

    do {
        if(!storage_file_open(file, HABIT_TMP_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) break;

        HabitFileHeader header = {
            .magic = HABIT_FILE_MAGIC,
            .version = HABIT_FILE_VERSION,
            .habit_size = sizeof(Habit),
            .count = store->count,
        };
        if(storage_file_write(file, &header, sizeof(header)) != sizeof(header)) break;

        size_t size = sizeof(Habit) * store->count;
        if(storage_file_write(file, store->habits, size) != size) break;
        if(!storage_file_sync(file)) break;
        ok = true;
    } while(false);

    storage_file_close(file);
    storage_file_free(file);

    if(ok) {
        ok = storage_common_rename(storage, HABIT_TMP_PATH, HABIT_FILE_PATH) == FSE_OK;
    } else {
        storage_common_remove(storage, HABIT_TMP_PATH);
    }
    if(!ok) FURI_LOG_E(TAG, "Failed to save habits");

    furi_record_close(RECORD_STORAGE);
    return ok;
}
