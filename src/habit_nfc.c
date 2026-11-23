#include "habit_nfc.h"

#include <furi.h>
#include <nfc/nfc.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a_poller_sync.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller_sync.h>
#include <nfc/protocols/mf_classic/mf_classic_poller_sync.h>
#include <nfc/nfc_poller.h>
#include <nfc/protocols/iso15693_3/iso15693_3_poller.h>
#include <string.h>

#define TAG "HabitNfc"

#define TAG_MAGIC     "flipper-habit:"
#define TAG_MAGIC_LEN 14
/*
 * 48 bytes: the user memory of the smallest Ultralight (12 pages), and exactly
 * the three data blocks of one MIFARE Classic sector.
 */
#define TAG_MAX_BYTES 48
/* TLV header (2) + NDEF record header (4) + status byte and "en" (3) */
#define TAG_TEXT_OFFSET 9
/* Bytes that must be read to cover the magic and the 4 hex digits of the ID. */
#define TAG_SCAN_BYTES (TAG_TEXT_OFFSET + TAG_MAGIC_LEN + 4)

#define UL_FIRST_PAGE 4 /* first user memory page on Ultralight / NTAG */
#define UL_PAGE_SIZE  4

#define CLASSIC_FIRST_BLOCK 4 /* first data block of sector 1 */
#define CLASSIC_BLOCK_SIZE  16

/*
 * ISO 15693 (NFC Forum Type 5, e.g. NXP ICODE SLIX) memory starts with a
 * Capability Container, followed by the NDEF TLV.
 */
#define V_CC_SIZE      4
#define V_CC_SIZE_LONG 8 /* CC with magic E2 for tags over 2 KB */
#define V_MAX_BYTES    (V_CC_SIZE_LONG + TAG_MAX_BYTES)
/* Write responses come once the EEPROM is programmed: at most 20 ms. */
#define V_WRITE_FWT_FC 300000U
/* Longest an ISO 15693 session may take before it is abandoned. */
#define V_SESSION_TIMEOUT_MS 1000

#define POLL_INTERVAL_MS 150
/* Failed detections in a row before a tag counts as taken away. */
#define ABSENT_THRESHOLD 3
/* Attempts at a tag that is detected but not yet readable (still moving). */
#define READ_ATTEMPTS 3

typedef enum {
    HabitNfcModeScan,
    HabitNfcModeWrite,
} HabitNfcMode;

typedef enum {
    TagTypeNone, /* nothing in the field */
    TagTypeUltralight,
    TagTypeClassic,
    TagTypeVicinity, /* ISO 15693 */
    TagTypeUnsupported,
} TagType;

struct HabitNfc {
    Nfc* nfc;
    FuriThread* thread;
    volatile bool running;
    HabitNfcMode mode;
    HabitNfcCallback callback;
    void* context;

    uint16_t write_id;
    char write_name[24];

    /* Result of the ISO 15693 read done while detecting the tag. */
    bool vicinity_readable;
    uint16_t vicinity_id;
};

static uint16_t tag_parse(const uint8_t* buf, size_t len);
static size_t tag_build(uint16_t habit_id, const char* name, uint8_t* out);

static void habit_nfc_emit(HabitNfc* instance, HabitNfcEvent event, uint16_t id) {
    if(instance->callback) instance->callback(instance->context, event, id);
}

/* ---- ISO 15693 ---- */

/*
 * ISO 15693 has no sync helpers in the firmware API, so each read or write is
 * one poller session: the poller activates the tag (reading all of its blocks)
 * and our callback then works on it from the NFC worker thread.
 */
typedef struct {
    HabitNfc* instance;
    NfcPoller* poller;
    FuriSemaphore* done;
    bool write;
    bool present; /* a tag answered and was activated */
    bool ok; /* read: memory parsed; write: written and verified */
    uint16_t habit_id; /* read: the ID found, 0 if none */
} VicinitySession;

static bool vicinity_write_block(
    Iso15693_3Poller* poller,
    uint8_t block,
    const uint8_t* data,
    uint8_t block_size) {
    BitBuffer* tx = bit_buffer_alloc(3 + block_size);
    BitBuffer* rx = bit_buffer_alloc(8);
    bit_buffer_append_byte(tx, ISO15693_3_REQ_FLAG_SUBCARRIER_1 | ISO15693_3_REQ_FLAG_DATA_RATE_HI);
    bit_buffer_append_byte(tx, ISO15693_3_CMD_WRITE_BLOCK);
    bit_buffer_append_byte(tx, block);
    bit_buffer_append_bytes(tx, data, block_size);

    Iso15693_3Error error = iso15693_3_poller_send_frame(poller, tx, rx, V_WRITE_FWT_FC);
    bool ok = error == Iso15693_3ErrorNone && bit_buffer_get_size_bytes(rx) >= 1 &&
              !(bit_buffer_get_byte(rx, 0) & ISO15693_3_RESP_FLAG_ERROR);
    if(!ok) {
        FURI_LOG_W(
            TAG,
            "15693 write of block %u failed: %d, response %02X",
            block,
            error,
            bit_buffer_get_size_bytes(rx) ? bit_buffer_get_byte(rx, 0) : 0);
    }
    bit_buffer_free(tx);
    bit_buffer_free(rx);
    return ok;
}

/* Put the habit on an activated tag, keeping its Capability Container. */
static bool vicinity_write(VicinitySession* session, Iso15693_3Poller* poller) {
    HabitNfc* instance = session->instance;
    const Iso15693_3Data* data = (const Iso15693_3Data*)nfc_poller_get_data(session->poller);
    const uint8_t block_size = iso15693_3_get_block_size(data);
    const uint16_t block_count = iso15693_3_get_block_count(data);
    const size_t memory = (size_t)block_size * block_count;
    if(block_size == 0 || block_size > 32 || memory < V_CC_SIZE) return false;

    /* Image of the start of memory: CC, then the NDEF message. */
    uint8_t image[V_MAX_BYTES + 32];
    memset(image, 0, sizeof(image));
    const uint8_t* first = iso15693_3_get_block_data(data, 0);
    size_t cc_size = V_CC_SIZE;
    if(first[0] == 0xE1) {
        memcpy(image, first, V_CC_SIZE);
    } else if(first[0] == 0xE2 && memory >= V_CC_SIZE_LONG) {
        cc_size = V_CC_SIZE_LONG;
        for(size_t i = 0; i < V_CC_SIZE_LONG; i++) {
            image[i] = iso15693_3_get_block_data(data, i / block_size)[i % block_size];
        }
    } else {
        /* Blank tag: version 1.0, read/write, data area size in 8 byte units. */
        size_t mlen = memory / 8;
        image[0] = 0xE1;
        image[1] = 0x40;
        image[2] = mlen > 0xFF ? 0xFF : (uint8_t)mlen;
        image[3] = 0x00;
    }

    size_t used = cc_size + tag_build(instance->write_id, instance->write_name, &image[cc_size]);
    if(used > memory) {
        FURI_LOG_W(TAG, "15693 tag too small: %u bytes", (unsigned)memory);
        return false;
    }
    size_t blocks = (used + block_size - 1) / block_size;
    FURI_LOG_D(TAG, "15693 write: %u blocks of %u", (unsigned)blocks, block_size);

    for(size_t b = 0; b < blocks; b++) {
        const uint8_t* want = &image[b * block_size];
        /* Skip blocks that already match, e.g. a locked CC. */
        if(memcmp(iso15693_3_get_block_data(data, b), want, block_size) == 0) continue;
        if(!vicinity_write_block(poller, (uint8_t)b, want, block_size)) return false;
    }

    /* Read back to make sure the tag really holds the habit. */
    uint8_t check[V_MAX_BYTES + 32];
    for(size_t b = 0; b < blocks; b++) {
        if(iso15693_3_poller_read_block(poller, &check[b * block_size], b, block_size) !=
           Iso15693_3ErrorNone) {
            return false;
        }
    }
    return tag_parse(check, used) == instance->write_id;
}

static NfcCommand vicinity_callback(NfcGenericEvent event, void* context) {
    VicinitySession* session = context;
    const Iso15693_3PollerEvent* poller_event = event.event_data;

    if(poller_event->type == Iso15693_3PollerEventTypeReady) {
        session->present = true;
        if(session->write) {
            session->ok = vicinity_write(session, event.instance);
        } else {
            const Iso15693_3Data* data =
                (const Iso15693_3Data*)nfc_poller_get_data(session->poller);
            uint8_t block_size = iso15693_3_get_block_size(data);
            uint16_t block_count = iso15693_3_get_block_count(data);
            uint8_t buf[V_MAX_BYTES + 32];
            size_t len = 0;
            for(uint16_t b = 0; b < block_count && block_size && len + block_size <= sizeof(buf);
                b++) {
                memcpy(&buf[len], iso15693_3_get_block_data(data, b), block_size);
                len += block_size;
            }
            session->habit_id = tag_parse(buf, len);
            session->ok = len > 0;
        }
    }
    /* Either way one pass is all we want. */
    furi_semaphore_release(session->done);
    return NfcCommandStop;
}

/* Run one ISO 15693 session. Returns false if no tag answered. */
static bool vicinity_run(HabitNfc* instance, bool write, bool* ok, uint16_t* habit_id) {
    VicinitySession session = {
        .instance = instance,
        .poller = nfc_poller_alloc(instance->nfc, NfcProtocolIso15693_3),
        .done = furi_semaphore_alloc(1, 0),
        .write = write,
    };
    nfc_poller_start(session.poller, vicinity_callback, &session);
    furi_semaphore_acquire(session.done, V_SESSION_TIMEOUT_MS);
    nfc_poller_stop(session.poller);
    nfc_poller_free(session.poller);
    furi_semaphore_free(session.done);

    if(ok) *ok = session.ok;
    if(habit_id) *habit_id = session.habit_id;
    return session.present;
}

/* ---- Tag access ---- */

static TagType tag_detect(HabitNfc* instance) {
    Iso14443_3aData* data = iso14443_3a_alloc();
    TagType type = TagTypeNone;
    if(iso14443_3a_poller_sync_read(instance->nfc, data) == Iso14443_3aErrorNone) {
        uint8_t sak = iso14443_3a_get_sak(data);
        if(sak == 0x00) {
            type = TagTypeUltralight;
        } else if(sak & 0x18) {
            /* 0x08 1K, 0x18 4K, 0x09 Mini, 0x88 Infineon, 0x28 / 0x38 EV1 */
            type = TagTypeClassic;
        } else {
            type = TagTypeUnsupported;
        }
        FURI_LOG_D(TAG, "Tag detected, SAK %02X, type %d", sak, type);
    }
    iso14443_3a_free(data);

    if(type == TagTypeNone &&
       vicinity_run(instance, false, &instance->vicinity_readable, &instance->vicinity_id)) {
        type = TagTypeVicinity;
        FURI_LOG_D(TAG, "ISO 15693 tag detected");
    }
    return type;
}

/* Factory default key: blank MIFARE Classic cards ship with it on every sector. */
static MfClassicKey classic_default_key(void) {
    MfClassicKey key;
    memset(key.data, 0xFF, sizeof(key.data));
    return key;
}

/* Read at least `len` bytes from the start of the habit area. */
static bool tag_read_bytes(HabitNfc* instance, TagType type, uint8_t* out, size_t len) {
    if(type == TagTypeUltralight) {
        for(size_t off = 0; off < len; off += UL_PAGE_SIZE) {
            MfUltralightPage page;
            MfUltralightError error = mf_ultralight_poller_sync_read_page(
                instance->nfc, UL_FIRST_PAGE + off / UL_PAGE_SIZE, &page);
            if(error != MfUltralightErrorNone) {
                FURI_LOG_D(TAG, "Ultralight read failed: %d", error);
                return false;
            }
            memcpy(&out[off], page.data, UL_PAGE_SIZE);
        }
        return true;
    }
    if(type == TagTypeClassic) {
        MfClassicKey key = classic_default_key();
        for(size_t off = 0; off < len; off += CLASSIC_BLOCK_SIZE) {
            MfClassicBlock block;
            MfClassicError error = mf_classic_poller_sync_read_block(
                instance->nfc,
                CLASSIC_FIRST_BLOCK + off / CLASSIC_BLOCK_SIZE,
                &key,
                MfClassicKeyTypeA,
                &block);
            if(error != MfClassicErrorNone) {
                FURI_LOG_D(TAG, "Classic read failed: %d", error);
                return false;
            }
            memcpy(&out[off], block.data, CLASSIC_BLOCK_SIZE);
        }
        return true;
    }
    return false;
}

/* Write `len` bytes (a multiple of the tag's unit size) to the habit area. */
static bool tag_write_bytes(HabitNfc* instance, TagType type, const uint8_t* data, size_t len) {
    if(type == TagTypeUltralight) {
        for(size_t off = 0; off < len; off += UL_PAGE_SIZE) {
            MfUltralightPage page;
            memcpy(page.data, &data[off], UL_PAGE_SIZE);
            MfUltralightError error = mf_ultralight_poller_sync_write_page(
                instance->nfc, UL_FIRST_PAGE + off / UL_PAGE_SIZE, &page);
            if(error != MfUltralightErrorNone) {
                FURI_LOG_W(TAG, "Ultralight write failed at byte %u: %d", (unsigned)off, error);
                return false;
            }
        }
        return true;
    }
    if(type == TagTypeClassic) {
        MfClassicKey key = classic_default_key();
        for(size_t off = 0; off < len; off += CLASSIC_BLOCK_SIZE) {
            MfClassicBlock block;
            memcpy(block.data, &data[off], CLASSIC_BLOCK_SIZE);
            MfClassicError error = mf_classic_poller_sync_write_block(
                instance->nfc,
                CLASSIC_FIRST_BLOCK + off / CLASSIC_BLOCK_SIZE,
                &key,
                MfClassicKeyTypeA,
                &block);
            if(error != MfClassicErrorNone) {
                FURI_LOG_W(TAG, "Classic write failed at byte %u: %d", (unsigned)off, error);
                return false;
            }
        }
        return true;
    }
    return false;
}

/* ---- Habit payload ---- */

static int hex_value(uint8_t c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Look for a habit ID in the bytes read from a tag. Returns 0 if there is none. */
static uint16_t tag_parse(const uint8_t* buf, size_t len) {
    for(size_t pos = 0; pos + TAG_MAGIC_LEN + 4 <= len; pos++) {
        if(memcmp(&buf[pos], TAG_MAGIC, TAG_MAGIC_LEN) != 0) continue;
        uint16_t id = 0;
        for(size_t d = 0; d < 4; d++) {
            int value = hex_value(buf[pos + TAG_MAGIC_LEN + d]);
            if(value < 0) return 0;
            id = (uint16_t)((id << 4) | value);
        }
        return id;
    }
    return 0;
}

/* Read the tag's habit ID. Returns false if the tag could not be read at all. */
static bool tag_read_habit(HabitNfc* instance, TagType type, uint16_t* habit_id) {
    if(type == TagTypeVicinity) {
        /* Already read while detecting it. */
        *habit_id = instance->vicinity_id;
        return instance->vicinity_readable;
    }
    uint8_t buf[TAG_MAX_BYTES];
    if(!tag_read_bytes(instance, type, buf, TAG_SCAN_BYTES)) return false;
    /* tag_read_bytes rounds up to whole pages / blocks, all within buf. */
    *habit_id = tag_parse(buf, TAG_SCAN_BYTES);
    return true;
}

/* Build the NDEF message, zero-padded to TAG_MAX_BYTES. Returns the used length. */
static size_t tag_build(uint16_t habit_id, const char* name, uint8_t* out) {
    char text[TAG_MAX_BYTES];
    /* Everything except the text: TLV header, record header, lang, terminator. */
    const size_t overhead = TAG_TEXT_OFFSET + 1;
    int text_len = snprintf(
        text, TAG_MAX_BYTES - overhead + 1, TAG_MAGIC "%04X %s", (unsigned)habit_id, name);
    if(text_len > (int)(TAG_MAX_BYTES - overhead)) text_len = TAG_MAX_BYTES - overhead;

    memset(out, 0, TAG_MAX_BYTES);
    size_t pos = 0;
    out[pos++] = 0x03; /* NDEF message TLV */
    out[pos++] = (uint8_t)(4 + 3 + text_len); /* record length */
    out[pos++] = 0xD1; /* MB | ME | SR, TNF well-known */
    out[pos++] = 0x01; /* type length */
    out[pos++] = (uint8_t)(3 + text_len); /* payload length */
    out[pos++] = 'T'; /* text record */
    out[pos++] = 0x02; /* UTF-8, 2 character language code */
    out[pos++] = 'e';
    out[pos++] = 'n';
    memcpy(&out[pos], text, text_len);
    pos += text_len;
    out[pos++] = 0xFE; /* terminator TLV */
    return pos;
}

static bool tag_write_habit(HabitNfc* instance, TagType type) {
    if(type == TagTypeVicinity) {
        bool ok = false;
        return vicinity_run(instance, true, &ok, NULL) && ok;
    }
    uint8_t buf[TAG_MAX_BYTES];
    size_t used = tag_build(instance->write_id, instance->write_name, buf);
    size_t unit = type == TagTypeClassic ? CLASSIC_BLOCK_SIZE : UL_PAGE_SIZE;
    size_t len = (used + unit - 1) / unit * unit;

    if(!tag_write_bytes(instance, type, buf, len)) return false;

    /* Read back to make sure the tag really holds the habit. */
    uint16_t id = 0;
    return tag_read_habit(instance, type, &id) && id == instance->write_id;
}

/* ---- Worker ---- */

static int32_t habit_nfc_thread(void* context) {
    HabitNfc* instance = context;
    /* A tag is handled once, then ignored until it has been taken away. */
    bool handled = false;
    bool done = false;
    uint8_t misses = 0;
    uint8_t attempts = 0;

    while(instance->running) {
        TagType type = done ? TagTypeNone : tag_detect(instance);

        if(type == TagTypeNone) {
            if(misses < ABSENT_THRESHOLD) misses++;
            if(misses >= ABSENT_THRESHOLD) {
                handled = false;
                attempts = 0;
            }
        } else {
            misses = 0;
            if(!handled && instance->mode == HabitNfcModeScan) {
                uint16_t id = 0;
                if(type == TagTypeUnsupported) {
                    handled = true;
                    habit_nfc_emit(instance, HabitNfcEventUnknownTag, 0);
                } else if(tag_read_habit(instance, type, &id)) {
                    handled = true;
                    habit_nfc_emit(
                        instance, id ? HabitNfcEventHabitTag : HabitNfcEventUnknownTag, id);
                } else if(++attempts >= READ_ATTEMPTS) {
                    /* Present but unreadable, e.g. a Classic card with custom keys. */
                    handled = true;
                    habit_nfc_emit(instance, HabitNfcEventUnknownTag, 0);
                }
            } else if(!handled) {
                handled = true;
                if(type == TagTypeUnsupported) {
                    habit_nfc_emit(instance, HabitNfcEventUnsupportedTag, 0);
                } else if(tag_write_habit(instance, type)) {
                    done = true;
                    habit_nfc_emit(instance, HabitNfcEventWriteOk, instance->write_id);
                } else {
                    habit_nfc_emit(instance, HabitNfcEventWriteFail, instance->write_id);
                }
            }
        }
        furi_delay_ms(POLL_INTERVAL_MS);
    }
    return 0;
}

static void habit_nfc_start(HabitNfc* instance, HabitNfcMode mode) {
    habit_nfc_stop(instance);
    instance->mode = mode;
    instance->nfc = nfc_alloc();
    instance->running = true;
    instance->thread = furi_thread_alloc_ex("HabitNfcWorker", 4096, habit_nfc_thread, instance);
    furi_thread_start(instance->thread);
}

HabitNfc* habit_nfc_alloc(void) {
    HabitNfc* instance = malloc(sizeof(HabitNfc));
    memset(instance, 0, sizeof(HabitNfc));
    return instance;
}

void habit_nfc_free(HabitNfc* instance) {
    habit_nfc_stop(instance);
    free(instance);
}

void habit_nfc_set_callback(HabitNfc* instance, HabitNfcCallback callback, void* context) {
    instance->callback = callback;
    instance->context = context;
}

void habit_nfc_start_scan(HabitNfc* instance) {
    habit_nfc_start(instance, HabitNfcModeScan);
}

void habit_nfc_start_write(HabitNfc* instance, uint16_t habit_id, const char* name) {
    habit_nfc_stop(instance);
    instance->write_id = habit_id;
    strlcpy(instance->write_name, name, sizeof(instance->write_name));
    habit_nfc_start(instance, HabitNfcModeWrite);
}

void habit_nfc_stop(HabitNfc* instance) {
    if(!instance->thread) return;
    instance->running = false;
    furi_thread_join(instance->thread);
    furi_thread_free(instance->thread);
    instance->thread = NULL;
    nfc_free(instance->nfc);
    instance->nfc = NULL;
}
