/* tengen_nvlog — the save journal in flash. See the header for why it is
 * shaped the way it is; this is how.
 *
 * A SECTOR:   [ header, 16 bytes ][ record ][ record ] ... [ erased ]
 *
 *   header    u32 magic "TNV1" | u32 generation | u16 seal | 6 unused.
 *             Of two sectors with a header and a commit, the higher
 *             generation is the journal; the other is what it replaced,
 *             kept as it was until its turn comes round again.
 *   record    u16 off | u16 len | u16 seal of those two
 *             | len bytes, padded to a word with 0xFF | u16 seal of it all
 *   commit    a record whose `off` is NV_MARK_OFF and whose eight bytes are
 *             the commit's number and the image's hash.
 *
 * Every seal is a CRC-16 that is never 0xFFFF (seal16), so nothing that
 * was left erased can pass for something that was written.
 */
#include "tengen_nvlog.h"

#include <string.h>

#define NV_MAGIC     0x31564E54u    /* "TNV1" */
#define NV_HEADER    16u
#define NV_REC_HEAD  6u
#define NV_REC_SEAL  2u
#define NV_MARK_OFF  0xC0DEu        /* no save is that long */
#define NV_MARK_LEN  8u
/* Two runs of the image closer than this go out as one record: a record's
 * own eight bytes cost more than the gap. */
#define NV_JOIN_GAP  8u

static uint16_t crc16(uint16_t crc, const uint8_t *p, uint32_t n) {
    static const uint16_t t[16] = {
        0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
        0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    };
    while (n--) {
        uint8_t b = *p++;
        crc = (uint16_t)((crc << 4) ^ t[(crc >> 12) ^ (b >> 4)]);
        crc = (uint16_t)((crc << 4) ^ t[(crc >> 12) ^ (b & 15)]);
    }
    return crc;
}

/* A CRC as it is stored: never the erased word. */
static uint16_t seal16(uint16_t crc) {
    return crc == 0xFFFFu ? 0xFFFEu : crc;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t *p, uint32_t v) {
    wr16(p, (uint16_t)v);
    wr16(p + 2, (uint16_t)(v >> 16));
}

static uint32_t padded(uint32_t len) {
    return (len + 1u) & ~1u;
}
static uint32_t record_bytes(uint32_t len) {
    return NV_REC_HEAD + padded(len) + NV_REC_SEAL;
}

static bool blank(const NvDevice *dev, int half, uint32_t from, uint32_t to) {
    if (dev->blank) return dev->blank(dev->ctx, half, from, to);
    const uint8_t *mem = dev->mem[half];
    for (uint32_t i = from; i < to; i++)
        if (mem[i] != NV_FILL) return false;
    return true;
}

/* The header of a sector that is a journal now: its generation. */
static bool header_live(const uint8_t *mem, uint32_t *gen) {
    if (rd32(mem) != NV_MAGIC) return false;
    if (rd16(mem + 8) != seal16(crc16(0x1D0F, mem, 8))) return false;
    *gen = rd32(mem + 4);
    return true;
}

/* What a walk of a sector's records finds. */
typedef struct {
    uint32_t commit_end;    /* just past the last commit marker */
    uint32_t end;           /* just past the last record that adds up */
    uint32_t seq, hash;     /* the last commit's */
    bool have_commit;
    bool clean;             /* nothing but erased bytes from `end` on */
} NvWalk;

/* One record at `pos`: its length in the sector, or 0 if what is there is
 * not a record (erased, cut short, or anything else). */
static uint32_t record_at(const uint8_t *mem, uint32_t size, uint32_t pos,
                          uint32_t image_size, uint16_t *off, uint16_t *len) {
    if (pos + NV_REC_HEAD + NV_REC_SEAL > size) return 0;
    const uint8_t *r = mem + pos;
    *off = rd16(r);
    *len = rd16(r + 2);
    uint16_t head = seal16(crc16(0x2A5F, r, 4));
    if (rd16(r + 4) != head) return 0;
    if (*off == NV_MARK_OFF) {
        if (*len != NV_MARK_LEN) return 0;
    } else if (*len == 0 || (uint32_t)*off + *len > image_size) {
        return 0;
    }
    uint32_t total = record_bytes(*len);
    if (pos + total > size) return 0;
    uint16_t all = seal16(crc16(head, r + NV_REC_HEAD, padded(*len)));
    if (rd16(r + NV_REC_HEAD + padded(*len)) != all) return 0;
    return total;
}

static void walk(const NvDevice *dev, int half, uint32_t image_size, NvWalk *w) {
    const uint8_t *mem = dev->mem[half];
    uint32_t size = dev->size;
    uint32_t pos = NV_HEADER;
    w->commit_end = NV_HEADER;
    w->seq = w->hash = 0;
    w->have_commit = false;
    for (;;) {
        uint16_t off, len;
        uint32_t n = record_at(mem, size, pos, image_size, &off, &len);
        if (!n) break;
        pos += n;
        if (off == NV_MARK_OFF) {
            const uint8_t *d = mem + pos - n + NV_REC_HEAD;
            w->seq = rd32(d);
            w->hash = rd32(d + 4);
            w->commit_end = pos;
            w->have_commit = true;
        }
    }
    w->end = pos;
    w->clean = blank(dev, half, pos, size);
}

void nvlog_mount(NvLog *log, const NvDevice *dev, uint32_t image_size) {
    memset(log, 0, sizeof *log);
    log->dev = *dev;
    log->image_size = image_size;
    log->active = -1;
    /* THE NEWER SECTOR FIRST, and the older only if the newer turns out to
     * hold no commit: a walk reads the sector to its end, a tenth of a
     * second on the console for the largest, and the older one's cannot
     * change the answer once the newer has one. */
    uint32_t gen[2] = { 0, 0 };
    bool live[2];
    for (int h = 0; h < 2; h++) live[h] = header_live(dev->mem[h], &gen[h]);
    int first = live[1] && (!live[0] || gen[1] > gen[0]) ? 1 : 0;
    for (int k = 0; k < 2; k++) {
        int h = first ^ k;
        if (!live[h]) continue;
        NvWalk w;
        walk(dev, h, image_size, &w);
        /* A sector with a header and no commit is a fresh one the power
         * went off on: there is nothing in it to go back to. */
        if (!w.have_commit) continue;
        log->active = (int8_t)h;
        log->gen = gen[h];
        log->end = w.commit_end;
        log->seq = w.seq;
        log->hash = w.hash;
        log->have_commit = true;
        /* RECORDS AFTER THE LAST COMMIT ARE NOT TO BE ADOPTED. They are
         * what a commit had written when the power went, and a marker put
         * after them later would make them part of the save — half of a
         * change, played as if it were whole (a table without its
         * checksum is no table). So a sector with anything at all past its
         * last commit takes no more records: the next save writes the
         * image afresh in the other one. */
        log->appendable = w.clean && w.end == w.commit_end;
        break;
    }
}

bool nvlog_replay(const NvLog *log, uint8_t *image) {
    memset(image, NV_FILL, log->image_size);
    if (log->active < 0) return false;
    const uint8_t *mem = log->dev.mem[log->active];
    uint32_t pos = NV_HEADER;
    while (pos < log->end) {
        uint16_t off, len;
        uint32_t n = record_at(mem, log->dev.size, pos, log->image_size, &off, &len);
        if (!n) break;                  /* cannot be: mount walked it */
        if (off != NV_MARK_OFF) memcpy(image + off, mem + pos + NV_REC_HEAD, len);
        pos += n;
    }
    return true;
}

uint32_t nvlog_used(const NvLog *log) {
    return log->active < 0 ? 0 : log->end;
}

static bool put(NvLog *log, int half, uint32_t at, const uint8_t *src, uint32_t n) {
    if (log->dev.program(log->dev.ctx, half, at, src, n)) return true;
    log->error = NV_ERR_PROGRAM;
    return false;
}

/* One record, programmed in the order that makes a cut-short one show: its
 * head, its bytes, and last the seal that says all of it is there. */
static bool put_record(NvLog *log, int half, uint32_t *pos, uint16_t off,
                       const uint8_t *data, uint32_t len) {
    uint8_t head[NV_REC_HEAD], tail[2], seal[2];
    wr16(head, off);
    wr16(head + 2, (uint16_t)len);
    uint16_t crc = seal16(crc16(0x2A5F, head, 4));
    wr16(head + 4, crc);
    crc = crc16(crc, data, len & ~1u);
    if (!put(log, half, *pos, head, NV_REC_HEAD)) return false;
    if ((len & ~1u) && !put(log, half, *pos + NV_REC_HEAD, data, len & ~1u))
        return false;
    if (len & 1u) {
        tail[0] = data[len - 1];
        tail[1] = NV_FILL;
        crc = crc16(crc, tail, 2);
        if (!put(log, half, *pos + NV_REC_HEAD + (len & ~1u), tail, 2)) return false;
    }
    wr16(seal, seal16(crc));
    if (!put(log, half, *pos + NV_REC_HEAD + padded(len), seal, 2)) return false;
    *pos += record_bytes(len);
    return true;
}

static bool put_commit(NvLog *log, int half, uint32_t *pos, uint32_t seq,
                       uint32_t hash) {
    uint8_t mark[NV_MARK_LEN];
    wr32(mark, seq);
    wr32(mark + 4, hash);
    return put_record(log, half, pos, NV_MARK_OFF, mark, NV_MARK_LEN);
}

/* The next run of the image worth a record, from `i` on: where it starts
 * and how long, or false at the end. Bytes that are still the fill are
 * what an image reads as with no record at all. */
static bool next_run(const uint8_t *image, uint32_t size, uint32_t i,
                     uint32_t *start, uint32_t *len) {
    while (i < size && image[i] == NV_FILL) i++;
    if (i >= size) return false;
    *start = i;
    uint32_t last = i;                  /* the last byte that is not fill */
    for (i++; i < size && i - last <= NV_JOIN_GAP; i++)
        if (image[i] != NV_FILL) last = i;
    *len = last + 1 - *start;
    return true;
}

/* THE WHOLE IMAGE INTO THE OTHER SECTOR: what a full sector, a tail that
 * cannot be added to and a first save all come to. The order is the whole
 * of its safety. Until the new sector has its commit it does not count
 * (mount wants one), so the old one is still the save; once it has, it is
 * the newer generation and wins. Switch off anywhere and one whole save is
 * found.
 *
 * THE OLD SECTOR IS LEFT AS IT IS, not marked and not erased: it is erased
 * when its turn comes to be written, and until then it is the save as of
 * the last time this was done — which is what is read if the new one is
 * ever lost. (It had a word programmed to zero to say it was retired. The
 * generations already say which is newer, and a save some weeks old is a
 * better thing to find than none.) */
static bool write_into(NvLog *log, int target, const uint8_t *image, uint32_t hash) {
    const NvDevice *dev = &log->dev;
    uint32_t start, len, need = NV_HEADER + record_bytes(NV_MARK_LEN);
    for (uint32_t i = 0; next_run(image, log->image_size, i, &start, &len); i = start + len)
        need += record_bytes(len);
    if (need > dev->size) {
        log->error = NV_ERR_SPACE;
        return false;
    }
    if (!blank(dev, target, 0, dev->size)) {
        log->erases++;
        if (!dev->erase(dev->ctx, target)) {
            log->error = NV_ERR_ERASE;
            return false;
        }
    }
    uint8_t head[NV_HEADER];
    memset(head, NV_FILL, sizeof head);
    wr32(head, NV_MAGIC);
    wr32(head + 4, log->gen + 1);
    wr16(head + 8, seal16(crc16(0x1D0F, head, 8)));
    if (!put(log, target, 0, head, NV_HEADER)) return false;
    uint32_t pos = NV_HEADER;
    for (uint32_t i = 0; next_run(image, log->image_size, i, &start, &len); i = start + len)
        if (!put_record(log, target, &pos, (uint16_t)start, image + start, len))
            return false;
    if (!put_commit(log, target, &pos, log->seq + 1, hash)) return false;
    log->active = (int8_t)target;
    log->gen++;
    log->end = pos;
    log->seq++;
    log->hash = hash;
    log->have_commit = true;
    log->appendable = true;
    log->compactions++;
    return true;
}

/* ...into the sector that is not the journal; and when there is no journal
 * at all, into either: a first save that finds the first sector will not
 * erase or take a word has nothing to lose by trying the second. (With a
 * journal there is only the one other, and what is in the journal stays
 * the save if it fails.) */
static bool write_fresh(NvLog *log, const uint8_t *image, uint32_t hash) {
    if (log->active >= 0) return write_into(log, log->active ^ 1, image, hash);
    if (write_into(log, 0, image, hash)) return true;
    if (log->error == NV_ERR_SPACE) return false;
    log->error = NV_OK;
    return write_into(log, 1, image, hash);
}

bool nvlog_sync(NvLog *log, const uint8_t *image, uint32_t hash) {
    log->error = NV_OK;
    return write_fresh(log, image, hash);
}

uint32_t nvlog_room(const NvLog *log) {
    if (log->active < 0 || !log->appendable) return 0;
    return log->dev.size - log->end;
}

bool nvlog_appends(const NvLog *log, const NvRange *ranges, int count) {
    uint32_t need = record_bytes(NV_MARK_LEN);
    for (int i = 0; i < count; i++) {
        if (ranges[i].lo >= ranges[i].hi || ranges[i].hi > log->image_size) continue;
        need += record_bytes((uint32_t)ranges[i].hi - ranges[i].lo);
    }
    return need <= nvlog_room(log);
}

bool nvlog_commit(NvLog *log, const uint8_t *image, const NvRange *ranges,
                  int count, uint32_t hash) {
    log->error = NV_OK;
    if (!nvlog_appends(log, ranges, count)) return write_fresh(log, image, hash);
    uint32_t pos = log->end;
    bool ok = true;
    for (int i = 0; ok && i < count; i++) {
        if (ranges[i].lo >= ranges[i].hi || ranges[i].hi > log->image_size) continue;
        ok = put_record(log, log->active, &pos, ranges[i].lo, image + ranges[i].lo,
                        (uint32_t)ranges[i].hi - ranges[i].lo);
    }
    if (ok) ok = put_commit(log, log->active, &pos, log->seq + 1, hash);
    if (!ok) {
        /* A word that would not take. What is in the sector now ends in
         * something cut short, so nothing more goes after it — and the
         * image is whole in memory: into the other sector with it. */
        log->appendable = false;
        log->error = NV_OK;
        return write_fresh(log, image, hash);
    }
    log->end = pos;
    log->seq++;
    log->hash = hash;
    log->commits++;
    return true;
}

static uint32_t hash_weight(uint32_t off) {
    return (off * 0x9E3779B1u + 0x7F4A7C15u) | 1u;
}

uint32_t nv_hash_step(uint32_t off, uint8_t was, uint8_t now) {
    return ((uint32_t)now - (uint32_t)was) * hash_weight(off);
}

uint32_t nv_hash(const uint8_t *image, uint32_t n) {
    /* hash_weight(i) for each i in turn, by adding instead of multiplying:
     * this runs over the whole save at power-on. */
    uint32_t h = 0, w = 0x7F4A7C15u;
    for (uint32_t i = 0; i < n; i++, w += 0x9E3779B1u) h += image[i] * (w | 1u);
    return h;
}
