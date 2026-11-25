/* records_v1.h — THE RECORDS EXCHANGE AS THE BUILDS UP TO 2.22 SPOKE IT,
 * kept word for word (src/tengen_link.c at that release, its names given a
 * v1_ in front) so that the tests can put the version of today on a wire
 * with one of those: a Single-Pak copy sent by an older cartridge still has
 * to hand its records to a newer one, and the other way round. Nothing here
 * is to be mended or tidied; it is what is out there. */
#ifndef RECORDS_V1_H
#define RECORDS_V1_H

#include <string.h>

#include "../src/tengen_link.h"

#define V1_RECORDS_MSG (2 + TENGEN_RECORDS_MAX * TENGEN_RECORD_BYTES)

typedef struct {
    uint8_t out[V1_RECORDS_MSG];
    uint8_t out_len, out_pos;
    bool out_held;          /* a save's `k` waits for the merge */
    uint8_t in[V1_RECORDS_MSG];
    uint8_t in_pos, in_len; /* in_len 0: not known yet */
    bool storage;           /* this console has a save */
    bool partner_storage;   /* ...and the other one (known once in_pos > 0) */
    /* For a console with a save: the other's records, once all in, for the
     * caller to merge (v1_records_saved says how many it kept). */
    TengenRecord theirs[TENGEN_RECORDS_MAX];
    uint8_t theirs_n;
    bool theirs_ready;
    /* For a console without one: how many of its records the other kept. */
    uint8_t saved;
    uint8_t linger;
    bool complete, failed;
    uint16_t idle;
} V1RecordSync;

#define V1_REC_SEQ   0x100u
#define V1_REC_ACK   0x200u
#define V1_REC_HAVE  0x400u

static void v1_put_record(uint8_t *at, const TengenRecord *r) {
    at[0] = r->table;
    at[1] = (uint8_t)(r->score >> 16);
    at[2] = (uint8_t)(r->score >> 8);
    at[3] = (uint8_t)r->score;
    at[4] = (uint8_t)(r->lines >> 8);
    at[5] = (uint8_t)r->lines;
    for (int c = 0; c < 3; c++) at[6 + c] = r->initials[c];
}

static void v1_get_record(const uint8_t *at, TengenRecord *r) {
    r->table = at[0];
    r->score = ((uint32_t)at[1] << 16) | ((uint32_t)at[2] << 8) | at[3];
    r->lines = (uint16_t)((at[4] << 8) | at[5]);
    for (int c = 0; c < 3; c++) r->initials[c] = at[6 + c];
}

static void v1_records_start(V1RecordSync *sync, bool storage,
                          const TengenRecord *mine, int n) {
    memset(sync, 0, sizeof(*sync));
    sync->storage = storage;
    if (storage) {
        sync->out[0] = 1;
        sync->out_len = 2;
        sync->out_held = true;          /* `k` once the other's are merged */
        return;
    }
    if (n < 0) n = 0;
    if (n > TENGEN_RECORDS_MAX) n = TENGEN_RECORDS_MAX;
    sync->out[0] = 0;
    sync->out[1] = (uint8_t)n;
    for (int i = 0; i < n; i++)
        v1_put_record(&sync->out[2 + i * TENGEN_RECORD_BYTES], &mine[i]);
    sync->out_len = (uint8_t)(2 + n * TENGEN_RECORD_BYTES);
}

/* How many bytes of `out` may go: all of them, bar a held `k`. */
static uint8_t v1_records_sendable(const V1RecordSync *sync) {
    return sync->out_held ? 1 : sync->out_len;
}

static uint16_t v1_records_word(const V1RecordSync *sync) {
    uint16_t word = (uint16_t)(TENGEN_LOBBY_RECORDS << TENGEN_LOBBY_TAG_SHIFT);
    if (sync->out_pos < v1_records_sendable(sync)) {
        word |= V1_REC_HAVE | sync->out[sync->out_pos];
        if (sync->out_pos & 1) word |= V1_REC_SEQ;
    }
    /* The receipt: the sequence bit of the last byte taken, 1 before any. */
    if (sync->in_pos == 0 || ((sync->in_pos - 1) & 1)) word |= V1_REC_ACK;
    return word;
}

static bool v1_records_done(const V1RecordSync *sync) {
    return sync->out_pos == sync->out_len && sync->in_len &&
           sync->in_pos == sync->in_len;
}

static void v1_records_saved(V1RecordSync *sync, uint8_t kept) {
    if (!sync->storage || !sync->out_held) return;
    sync->out[1] = kept;
    sync->out_held = false;
}

static void v1_records_take(V1RecordSync *sync, uint8_t byte) {
    if (sync->in_len && sync->in_pos >= sync->in_len) return;
    sync->in[sync->in_pos++] = byte;
    if (sync->in_pos == 1) {
        sync->partner_storage = (byte & 1) != 0;
        if (sync->partner_storage) sync->in_len = 2;
        /* Two consoles with saves have nothing to give each other. */
        if (sync->partner_storage) v1_records_saved(sync, 0);
    } else if (sync->in_pos == 2 && !sync->partner_storage) {
        uint8_t n = byte > TENGEN_RECORDS_MAX ? TENGEN_RECORDS_MAX : byte;
        sync->in_len = (uint8_t)(2 + n * TENGEN_RECORD_BYTES);
    }
    if (!sync->in_len || sync->in_pos < sync->in_len) return;
    /* All of it. */
    if (sync->partner_storage) {
        sync->saved = sync->in[1];
    } else {
        sync->theirs_n = (uint8_t)((sync->in_len - 2) / TENGEN_RECORD_BYTES);
        for (int i = 0; i < sync->theirs_n; i++)
            v1_get_record(&sync->in[2 + i * TENGEN_RECORD_BYTES], &sync->theirs[i]);
        if (sync->storage) sync->theirs_ready = true;
        else sync->theirs_n = 0;        /* nowhere to keep them */
        /* ...and a console without a save answers nothing to it. */
    }
}

static void v1_records_apply(V1RecordSync *sync, bool got, uint16_t word) {
    if (sync->complete || sync->failed) return;
    if (!got || (!tengen_link_is_match_word(word) &&
                 (TengenLobbyTag)((word) >> TENGEN_LOBBY_TAG_SHIFT) != TENGEN_LOBBY_RECORDS)) {
        /* Nothing, or a word from before this (a GO the master is still
         * sending while it waits for its echo): not an answer. */
        if (++sync->idle >= TENGEN_RECORDS_TIMEOUT) sync->failed = true;
        return;
    }
    if (tengen_link_is_match_word(word)) {
        /* The other one finished, which it can only do with everything
         * across both ways: this one has all of it too. */
        sync->complete = true;
        return;
    }
    sync->idle = 0;
    /* Its receipt for the byte in flight. */
    if (sync->out_pos < v1_records_sendable(sync) &&
        (((word & V1_REC_ACK) != 0) == ((sync->out_pos & 1) != 0)))
        sync->out_pos++;
    /* Its byte, if it is the next one. */
    if ((word & V1_REC_HAVE) &&
        (((word & V1_REC_SEQ) != 0) == ((sync->in_pos & 1) != 0)))
        v1_records_take(sync, (uint8_t)(word & 0xFF));
    if (v1_records_done(sync) && ++sync->linger > TENGEN_RECORDS_LINGER)
        sync->complete = true;
}

#endif /* RECORDS_V1_H */
