#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * Background NFC worker for habit tags.
 *
 * A habit tag holds an NDEF text record
 * "flipper-habit:<ID as 4 hex digits> <habit name>". The app only relies on
 * the ID; the name is there so the tag is readable on a phone.
 *
 * Supported tags:
 *   - NTAG21x / MIFARE Ultralight: stored from page 4 (start of user memory)
 *   - MIFARE Classic: stored in blocks 4-6 (sector 1), which must still use
 *     the factory default key FFFFFFFFFFFF
 *   - ISO 15693 / NFC Forum Type 5 (e.g. NXP ICODE SLIX): stored after the
 *     Capability Container in block 0, which is written if the tag is blank
 */

typedef struct HabitNfc HabitNfc;

typedef enum {
    HabitNfcEventHabitTag, /* scan: a habit tag was presented, `habit_id` is set */
    HabitNfcEventUnknownTag, /* scan: a tag without habit data was presented */
    HabitNfcEventWriteOk, /* write: tag written and verified */
    HabitNfcEventWriteFail, /* write: tag present but could not be written */
    HabitNfcEventUnsupportedTag, /* write: tag type cannot hold a habit */
} HabitNfcEvent;

/* Called from the worker thread. */
typedef void (*HabitNfcCallback)(void* context, HabitNfcEvent event, uint16_t habit_id);

HabitNfc* habit_nfc_alloc(void);
void habit_nfc_free(HabitNfc* nfc);
void habit_nfc_set_callback(HabitNfc* nfc, HabitNfcCallback callback, void* context);

/* Keep looking for habit tags. Each tag reports once until it is taken away. */
void habit_nfc_start_scan(HabitNfc* nfc);
/* Wait for a tag and write the habit to it. Retries with the next tag on failure. */
void habit_nfc_start_write(HabitNfc* nfc, uint16_t habit_id, const char* name);
/* Stop the worker and switch the NFC field off. Safe to call when not running. */
void habit_nfc_stop(HabitNfc* nfc);
