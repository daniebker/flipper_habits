#pragma once

#include <stdint.h>
#include <stdbool.h>

#define HABIT_MAX          16
#define HABIT_NAME_LEN     24
#define HABIT_TARGET_MAX   12
/* 53 weeks * 7 days covers every cell of the year grid, plus one spare slot. */
#define HABIT_HISTORY_DAYS 372

typedef struct {
    char name[HABIT_NAME_LEN];
    /* Completions required per day: 1 = daily, N = N times a day. */
    uint8_t target;
    uint8_t reserved;
    /*
     * Stable non-zero identifier, written to NFC tags so a tag keeps pointing at
     * the same habit after renames, deletions and reordering. It occupies what
     * used to be struct padding (always zero), so 0 means "not assigned yet" and
     * the file format is unchanged.
     */
    uint16_t id;
    /* Most recent day (days since 1970-01-01) the history ring is aligned to. */
    uint32_t last_day;
    /* Ring buffer of completion counts, indexed by day % HABIT_HISTORY_DAYS. */
    uint8_t counts[HABIT_HISTORY_DAYS];
} Habit;

typedef struct {
    uint8_t count;
    Habit habits[HABIT_MAX];
} HabitStore;

/* ---- Date helpers ---- */

/* Days since 1970-01-01 for a civil date. */
uint32_t habit_days_from_civil(int32_t year, uint32_t month, uint32_t day);
void habit_civil_from_days(uint32_t days, int32_t* year, uint32_t* month, uint32_t* day);
/* 0 = Sunday ... 6 = Saturday */
uint8_t habit_weekday(uint32_t days);
/* Today according to the Flipper RTC. */
uint32_t habit_today(void);

/* ---- Habit operations ---- */

/* Roll the history ring forward to `today`, clearing skipped days. */
void habit_sync(Habit* habit, uint32_t today);
uint8_t habit_get(const Habit* habit, uint32_t day);
void habit_set(Habit* habit, uint32_t day, uint8_t value);
bool habit_is_complete(const Habit* habit, uint32_t day);
/* Number of fully completed days in the 365 days ending on `today`. */
uint16_t habit_year_total(const Habit* habit, uint32_t today);
/* Consecutive completed days ending today (or yesterday if today is not yet done). */
uint16_t habit_streak(const Habit* habit, uint32_t today);

/* ---- Store ---- */

HabitStore* habit_store_alloc(void);
void habit_store_free(HabitStore* store);
/* Returns index of the new habit, or -1 if the store is full. */
int habit_store_add(HabitStore* store, const char* name, uint8_t target, uint32_t today);
void habit_store_remove(HabitStore* store, uint8_t index);
void habit_store_sync(HabitStore* store, uint32_t today);
/* Returns the index of the habit with this id, or -1. */
int habit_store_find_by_id(const HabitStore* store, uint16_t id);
/* Give every habit without an id a fresh one. Returns true if any were assigned. */
bool habit_store_ensure_ids(HabitStore* store);
bool habit_store_load(HabitStore* store);
bool habit_store_save(const HabitStore* store);
