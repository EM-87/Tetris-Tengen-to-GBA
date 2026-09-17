/*
 * handtunes.h — THE ONLY THINGS IN THIS PORT THAT ARE NOT ON THE CARTRIDGE.
 *
 * Everything else here comes out of the ROM: the tiles, the palettes, the
 * screens, the rules, and the music — which the port does not even convert,
 * it runs the cartridge's own sound engine (see nes_audio.h). This file is
 * the deliberate exception, and it is marked as one so nobody later mistakes
 * what is in it for something that was traced.
 *
 * Tengen's cartridge has four tunes: Loginska, Bradinsky, Karinka and Troika
 * (constants.asm.txt:39-42). Two well-known Russian songs are NOT among them
 * and are entered here by hand instead, hidden behind L+R on the selection
 * screen:
 *
 *   KOROBEINIKI  the pedlars' song from the 1860s that most people know as
 *                "the Tetris theme", because Nintendo's Game Boy version used
 *                it. Long out of copyright everywhere.
 *   KATIUSKA     Katyusha. NOT a folk song and NOT public domain: Matvei
 *                Blanter wrote it in 1938 and died in 1990, so in Russia and
 *                the EU the melody is in copyright until 2061. It is here
 *                because it was asked for, and this comment is here so the
 *                difference between the two is on the record. Karinka, the
 *                cartridge's own third tune, IS Kalinka (1860, Larionov) and
 *                needed nothing added.
 *
 * Neither can go through the cartridge's engine: feeding it one would mean
 * writing new data in a music format nobody has documented and patching it
 * into the ROM image, which is exactly the kind of thing this project does
 * not do. So this is a small sequencer of its own, writing the GBA's own PSG
 * directly, and it takes the two pulse channels only while it is playing.
 *
 * It coexists with the cartridge's engine rather than replacing it. When one
 * of these tunes is chosen the engine is told to play MUSIC_SILENCE and keeps
 * running, so every sound effect is still the ROM's — and, exactly as on the
 * cartridge, an effect briefly steals a channel from the music and the next
 * note takes it back.
 */
#ifndef HANDTUNES_H
#define HANDTUNES_H

#include <stdbool.h>
#include <stdint.h>

enum {
    HANDTUNE_KOROBEINIKI,
    HANDTUNE_KATIUSKA,
    HANDTUNE_COUNT
};

/* Starts the given tune from the top. Call after silencing the cartridge's
 * engine. */
void handtune_start(uint8_t tune);

/* Releases the two pulse channels and forgets the tune. The engine's next
 * write reclaims them. */
void handtune_stop(void);

/* PAUSE, NOT STOP. Silences the channels but leaves the two voices standing
 * where they are, so handtune_resume picks the melody up on the next note
 * instead of starting it again from the first bar. This is what a pause owes
 * a tune, and what the cartridge's own MUSIC_SUSPEND/MUSIC_RESUME pair gives
 * the tracks its engine plays. */
void handtune_suspend(void);
void handtune_resume(void);

/* Which tune is loaded — HANDTUNE_COUNT when none is, including after a stop.
 * Lets a caller tell "carry on with this one" from "change to that one". */
uint8_t handtune_current(void);

/* One frame of the sequencer. Safe to call when stopped (it does nothing). */
void handtune_frame(void);

bool handtune_playing(void);

#endif /* HANDTUNES_H */
