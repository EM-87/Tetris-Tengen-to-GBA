/*
 * korobeiniki.h — THE ONE THING IN THIS PORT THAT IS NOT ON THE CARTRIDGE.
 *
 * Everything else here comes out of the ROM: the tiles, the palettes, the
 * screens, the rules, and the music — which the port does not even convert,
 * it runs the cartridge's own sound engine (see nes_audio.h). This file is
 * the deliberate exception, and it is marked as one so nobody later mistakes
 * it for something that was traced.
 *
 * Tengen's cartridge has four tunes: Loginska, Bradinsky, Karinka and Troika
 * (constants.asm.txt:39-42). Korobeiniki — the Russian pedlars' song from the
 * 1860s that most people know as "the Tetris theme" because Nintendo's
 * Game Boy version used it — IS NOT AMONG THEM. There is no arrangement of it
 * in this ROM to extract, so it is entered here by hand as a fifth tune, hidden
 * behind L+R on the selection screen.
 *
 * That means it cannot go through the cartridge's engine either: feeding it
 * one would mean writing new data in a music format nobody has documented and
 * patching it into the ROM image, which is exactly the kind of thing this
 * project does not do. So this is a small sequencer of its own, writing the
 * GBA's own PSG directly, and it takes the two pulse channels only while it
 * is playing.
 *
 * It coexists with the cartridge's engine rather than replacing it. When this
 * tune is chosen the engine is told to play MUSIC_SILENCE and keeps running,
 * so every sound effect is still the ROM's — and, exactly as on the
 * cartridge, an effect briefly steals a channel from the music and the next
 * note takes it back.
 */
#ifndef KOROBEINIKI_H
#define KOROBEINIKI_H

#include <stdbool.h>

/* Starts from the top. Call after silencing the cartridge's engine. */
void korobeiniki_start(void);

/* Releases the two pulse channels. The engine's next write reclaims them. */
void korobeiniki_stop(void);

/* One frame of the sequencer. Safe to call when stopped (it does nothing). */
void korobeiniki_frame(void);

bool korobeiniki_playing(void);

#endif /* KOROBEINIKI_H */
