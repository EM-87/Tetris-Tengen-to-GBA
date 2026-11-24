/* tengen_nvlog — THE SAVE, KEPT IN THE CARTRIDGE'S OWN FLASH ROM.
 *
 * The port keeps its high scores, options and paused games in the
 * cartridge's save RAM, which a battery holds while the console is off.
 * The boards most reproductions are built on today have the RAM and no
 * battery: what is written there lasts until the power switch. On those the
 * only memory that outlives it is the flash chip the game itself is in, so
 * the save goes there as well — a journal of what changed, appended to a
 * sector the program does not use.
 *
 * FLASH IS NOT RAM, and the shape of this file is what that costs:
 *
 *   * a bit can be programmed from 1 to 0 and never back; only erasing a
 *     whole sector (64 or 128 KB, half a second or more with the game
 *     frozen) makes ones again, and a sector lasts some 100,000 erasures;
 *   * so nothing is ever rewritten in place. A change is a RECORD appended
 *     after the last one — where it goes in the save, how long, the bytes,
 *     a checksum — and a COMMIT marker closes the records that belong
 *     together. Reading the save back is playing the records over an empty
 *     image, in order, up to the last commit;
 *   * when the sector is full the image is written out afresh into the
 *     OTHER of two sectors, which then is the journal (the newer of the
 *     two) — so there is always one whole copy, whenever the power goes;
 *   * a record or a commit the power cut short has a checksum that does not
 *     add up, and everything from there on is ignored. What is read back is
 *     the save as of some commit that finished: never half of one.
 *
 * Nothing here knows what a flash chip is. It asks a device for three
 * things — the two sectors as they read, a way to program words into one,
 * a way to erase one — and gba/cartsave.c gives it the cartridge; the tests
 * give it a model that can lose its power at any word (tests/test_nvlog.c).
 */
#ifndef TENGEN_NVLOG_H
#define TENGEN_NVLOG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The two sectors and the things done to them. `mem[h]` is sector h
 * as it reads, `size` bytes of it (the same for both; even). `program`
 * writes `n` bytes (even) from `src` at byte `at` (even) of sector `half`,
 * clearing bits only, and says whether they then read back as written;
 * `erase` sets the whole sector to 0xFF and says whether it did. Either may
 * take as long as the chip does. `src` is never inside `mem`. */
typedef struct {
    const uint8_t *mem[2];
    uint32_t size;
    bool (*program)(void *ctx, int half, uint32_t at, const uint8_t *src, uint32_t n);
    bool (*erase)(void *ctx, int half);
    /* Whether bytes from..to-1 of a sector are all 0xFF. May be null, and
     * then they are simply read; a device that can read its sector a word
     * at a time says so here, since the journal looks over all that is
     * left of a sector every time it is mounted. */
    bool (*blank)(void *ctx, int half, uint32_t from, uint32_t to);
    void *ctx;
} NvDevice;

/* A run of the save image that changed: bytes lo..hi-1. */
typedef struct {
    uint16_t lo, hi;
} NvRange;

enum {
    NV_OK = 0,
    NV_ERR_ERASE,       /* a sector would not erase */
    NV_ERR_PROGRAM,     /* a word would not program */
    NV_ERR_SPACE,       /* the image does not fit a sector */
};

typedef struct {
    NvDevice dev;
    uint32_t image_size;    /* bytes of save the journal covers */
    int8_t active;          /* the sector in use, or -1: no journal yet */
    bool appendable;        /* its tail is clean: records can be added */
    bool have_commit;
    uint8_t error;          /* NV_*: the last operation's */
    uint32_t gen;           /* the active sector's generation */
    uint32_t end;           /* where the next record goes in it */
    uint32_t seq;           /* the last commit's number... */
    uint32_t hash;          /* ...and the image's hash as it recorded it */
    /* What has been done since the mount, for the cartridge report. */
    uint16_t commits, compactions, erases;
} NvLog;

/* The fill of an image nothing has been recorded for, which is also what
 * an erased sector reads as. */
#define NV_FILL 0xFF

/* Looks at the two sectors and finds the journal, if there is one. Reads
 * only: nothing is programmed or erased. */
void nvlog_mount(NvLog *log, const NvDevice *dev, uint32_t image_size);

/* The save as of the last commit: `image` filled with NV_FILL and every
 * committed record played over it. False, and the image all fill, if there
 * is no journal. */
bool nvlog_replay(const NvLog *log, uint8_t *image);

/* Makes `count` runs of `image` and `hash` part of the save: appended and
 * committed, or — if the sector cannot take them — the whole image written
 * afresh into the other one. True when it is on the flash for good.
 * `image` must not be in the flash itself. */
bool nvlog_commit(NvLog *log, const uint8_t *image, const NvRange *ranges,
                  int count, uint32_t hash);

/* The whole image written afresh, whatever the journal held (or did not):
 * what a first save does, and one after the save RAM turned out newer than
 * the journal. */
bool nvlog_sync(NvLog *log, const uint8_t *image, uint32_t hash);

/* How much of the active sector is taken, in bytes (0 without a journal),
 * and how much is left for more records (0 too if it takes no more). */
uint32_t nvlog_used(const NvLog *log);
uint32_t nvlog_room(const NvLog *log);

/* Whether nvlog_commit would add those runs after what is there — a few
 * words programmed — or have to write the image afresh into the other
 * sector, which is an erase and a second or more with nothing else
 * running. For choosing the moment. */
bool nvlog_appends(const NvLog *log, const NvRange *ranges, int count);

/* THE IMAGE'S HASH, which can be kept up a byte at a time: every byte times
 * a number of its own place, added up. A byte that changes from `was` to
 * `now` at `off` moves it by nv_hash_step; nv_hash is the whole sum. Not a
 * strong checksum and not meant as one: it tells a save RAM that still
 * holds what was written from one that has decayed with the power off. */
uint32_t nv_hash_step(uint32_t off, uint8_t was, uint8_t now);
uint32_t nv_hash(const uint8_t *image, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* TENGEN_NVLOG_H */
