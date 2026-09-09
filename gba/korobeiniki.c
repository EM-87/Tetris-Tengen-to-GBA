/*
 * korobeiniki.c — the fifth tune, entered by hand. See korobeiniki.h for why
 * this one file is not extracted from the cartridge like everything else.
 *
 * THE ARRANGEMENT. Korobeiniki is a folk song from the 1860s and long out of
 * copyright; what is written below is the traditional melody in A minor, in
 * the two-part shape every Tetris arrangement uses — a fast A section twice,
 * then a slower B section — with a plain root/fifth accompaniment underneath.
 * It is not a transcription of anyone's recording.
 *
 * THE FREQUENCIES ARE NOT TYPED BY EAR. A GBA pulse channel runs at
 *
 *     f = 131072 / (2048 - R)      so      R = 2048 - 131072 / f
 *
 * and the note table below is that formula evaluated for equal temperament
 * with A4 = 440 Hz, rounded. Anyone can check any row of it with a
 * calculator, which is the point.
 */
#include "korobeiniki.h"

#include "gba_hw.h"

/* The same registers nes_audio.c drives; declared again here rather than
 * shared, because these two are meant to stay independent. */
#define REG_SOUND1CNT_L (*(vu16 *)0x04000060)
#define REG_SOUND1CNT_H (*(vu16 *)0x04000062)
#define REG_SOUND1CNT_X (*(vu16 *)0x04000064)
#define REG_SOUND2CNT_L (*(vu16 *)0x04000068)
#define REG_SOUND2CNT_H (*(vu16 *)0x0400006C)

/* ----------------------------------------------------------------------- *
 * Notes
 *
 * Index into kNoteReg. REST is silence. The names are the pitches, so the
 * score below reads as a score.
 * ----------------------------------------------------------------------- */
enum {
    REST = 0,
    A2, B2, C3, D3, E3, F3, G3, A3, B3,
    C4, D4, E4, F4, G4, GS4, A4, B4,
    C5, D5, E5, F5, G5, GS5, A5,
    NOTE_COUNT
};

/* R = 2048 - 131072/f, f = 440 * 2^((midi-69)/12). Entry 0 is unused. */
static const uint16_t kNoteReg[NOTE_COUNT] = {
    0,
    856,  986, 1046, 1155, 1253, 1297, 1379, 1452, 1517,  /* A2..B3 */
    1547, 1602, 1650, 1673, 1714, 1732, 1750, 1783,       /* C4..B4 */
    1798, 1825, 1849, 1860, 1881, 1890, 1899,             /* C5..A5 */
};

/* One event: a pitch and how long it lasts, in sixteenth notes. */
typedef struct { uint8_t note, len; } Event;

/* THE A SECTION, played twice. Sixteenths, so a "4" is a quarter note and
 * every line below adds up to 16 — one bar of 4/4. */
static const Event kMelodyA[] = {
    {E5,4},{B4,2},{C5,2},{D5,4},{C5,2},{B4,2},
    {A4,4},{A4,2},{C5,2},{E5,4},{D5,2},{C5,2},
    {B4,6},{C5,2},{D5,4},{E5,4},
    /* The phrase ENDS on a quarter. Writing that last A4 as an eighth with a
     * rest after it swallowed the note the ear is waiting for, which is what
     * "it's missing a note" was. */
    {C5,4},{A4,4},{A4,4},{REST,4},

    {D5,6},{F5,2},{A5,4},{G5,2},{F5,2},
    {E5,6},{C5,2},{E5,4},{D5,2},{C5,2},
    {B4,4},{B4,2},{C5,2},{D5,4},{E5,4},
    {C5,4},{A4,4},{A4,4},{REST,4},
};

/* THE B SECTION: the same harmony at half the speed, an octave of long
 * notes over the walking bass below. */
static const Event kMelodyB[] = {
    {E5,8},{C5,8},
    {D5,8},{B4,8},
    {C5,8},{A4,8},
    {GS4,8},{B4,4},{REST,4},
    {E5,8},{C5,8},
    {D5,8},{B4,8},
    {C5,4},{E5,4},{A5,8},
    {GS5,8},{REST,8},
};

/* The accompaniment: roots and fifths, one per half bar. */
static const Event kBassA[] = {
    {A2,8},{E3,8},
    {A2,8},{E3,8},
    {A2,8},{E3,8},
    {A2,8},{E3,8},

    {D3,8},{A3,8},
    {C3,8},{G3,8},
    {A2,8},{E3,8},
    {A2,8},{E3,8},
};

static const Event kBassB[] = {
    {A2,8},{E3,8},
    {A2,8},{E3,8},
    {F3,8},{C3,8},
    {E3,8},{E3,8},
    {A2,8},{E3,8},
    {A2,8},{E3,8},
    {F3,8},{A3,8},
    {E3,8},{REST,8},
};

/* ----------------------------------------------------------------------- *
 * The sequencer
 *
 * Two voices reading their own lists, each holding a note for its length.
 *
 * TEMPO. At 60Hz a sixteenth of N frames puts a crotchet at 900/N beats a
 * minute, so 6 frames is 150 and 7 is 128.6. This tune is played at 150; the
 * 7 it had was audibly a drag.
 *
 * ARTICULATION. Every note is cut one frame before its length runs out
 * instead of being left to a decaying envelope. At 150bpm an eighth note is
 * twelve frames and any envelope slow enough to sustain a quarter ran straight
 * through the eighths, smearing them together; a hard note-off is both
 * cleaner and independent of the tempo.
 * ----------------------------------------------------------------------- */
#define FRAMES_PER_SIXTEENTH 6
#define NOTE_OFF_FRAMES 1

/* The A section twice, then B, then round again — the shape the tune has had
 * since long before anyone put it in a video game. */
typedef struct {
    const Event *events;
    uint16_t count;
} Section;

static const Section kMelody[] = {
    { kMelodyA, sizeof kMelodyA / sizeof kMelodyA[0] },
    { kMelodyA, sizeof kMelodyA / sizeof kMelodyA[0] },
    { kMelodyB, sizeof kMelodyB / sizeof kMelodyB[0] },
};
static const Section kBass[] = {
    { kBassA, sizeof kBassA / sizeof kBassA[0] },
    { kBassA, sizeof kBassA / sizeof kBassA[0] },
    { kBassB, sizeof kBassB / sizeof kBassB[0] },
};
#define SECTION_COUNT (sizeof kMelody / sizeof kMelody[0])

typedef struct {
    uint8_t section;    /* which of the three */
    uint16_t index;     /* which event within it */
    uint16_t ticks;     /* frames left of the current note */
} Voice;

static Voice g_lead, g_bass;
static bool g_playing;

/* Duty 2 (a square wave) for the lead, duty 1 for the bass so the two are
 * told apart. Envelope step 0 means the volume does not change, which is what
 * is wanted now that the note-off does the articulation. */
#define LEAD_ENVELOPE  ((11 << 12) | (0 << 11) | (0 << 8) | (2 << 6))
#define BASS_ENVELOPE  ((8 << 12) | (0 << 11) | (0 << 8) | (1 << 6))

static void voice_reset(Voice *v) {
    v->section = 0;
    v->index = 0;
    v->ticks = 0;
}

/* Advances one voice. Returns the note to start now, NOTE_HOLD to leave the
 * channel alone, or NOTE_RELEASE for the gap between this note and the next. */
#define NOTE_HOLD    (-1)
#define NOTE_RELEASE (-2)

static int voice_step(Voice *v, const Section *score) {
    if (v->ticks > 0) {
        v->ticks--;
        return v->ticks < NOTE_OFF_FRAMES ? NOTE_RELEASE : NOTE_HOLD;
    }
    if (v->index >= score[v->section].count) {
        v->index = 0;
        v->section = (uint8_t)((v->section + 1) % SECTION_COUNT);
    }
    const Event *e = &score[v->section].events[v->index++];
    v->ticks = (uint16_t)(e->len * FRAMES_PER_SIXTEENTH - 1);
    return e->note;
}

void korobeiniki_start(void) {
    voice_reset(&g_lead);
    voice_reset(&g_bass);
    g_playing = true;
}

void korobeiniki_stop(void) {
    if (!g_playing) return;
    g_playing = false;
    /* Silence both, and leave the registers where the cartridge's engine
     * expects to find them: it writes a channel whenever its own APU state
     * changes, so the next note or effect reclaims these anyway. */
    REG_SOUND1CNT_H = 0;     /* channel 1's envelope... */
    REG_SOUND2CNT_L = 0;     /* ...and channel 2's, which is its _L */
}

bool korobeiniki_playing(void) { return g_playing; }

void korobeiniki_frame(void) {
    if (!g_playing) return;

    int lead = voice_step(&g_lead, kMelody);
    if (lead == NOTE_RELEASE || lead == REST) {
        REG_SOUND1CNT_H = 0;
    } else if (lead != NOTE_HOLD) {
        REG_SOUND1CNT_L = 0;                     /* no sweep */
        REG_SOUND1CNT_H = LEAD_ENVELOPE;
        REG_SOUND1CNT_X = (uint16_t)(kNoteReg[lead] | 0x8000);
    }

    int bass = voice_step(&g_bass, kBass);
    if (bass == NOTE_RELEASE || bass == REST) {
        REG_SOUND2CNT_L = 0;
    } else if (bass != NOTE_HOLD) {
        REG_SOUND2CNT_L = BASS_ENVELOPE;
        REG_SOUND2CNT_H = (uint16_t)(kNoteReg[bass] | 0x8000);
    }
}
