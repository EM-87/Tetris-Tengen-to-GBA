/*
 * nes_audio.c — NES APU writes in, GBA sound registers out.
 *
 * The engine producing those writes is the cartridge's own (see nes_audio.h
 * and nes6502.c). This file is the one place where something is genuinely
 * translated rather than executed, so every mapping below is spelled out.
 *
 * The two chips are close relatives:
 *
 *   NES pulse 1/2   -> GBA sound 1/2. Same four duty cycles in the same
 *                      order, so duty maps across unchanged.
 *   NES triangle    -> GBA sound 3, the wave channel, loaded with the NES
 *                      triangle's own 32-step 4-bit ramp. Both divide their
 *                      rate by 32 for one cycle of the wave, which is why the
 *                      frequency conversion below is the same expression as
 *                      the pulses'.
 *   NES noise       -> GBA sound 4. Both are LFSRs with a long and a short
 *                      (metallic) mode, but the NES picks its rate from a
 *                      16-entry table while the GBA builds one from a divisor
 *                      and a shift, so those had to be matched by frequency;
 *                      see kNoiseDiv/kNoiseShift.
 *
 * Frequency, for all three tone channels:
 *   NES pulse    f = 1789773 / (16 * (P + 1))
 *   NES triangle f = 1789773 / (32 * (P + 1))
 *   GBA pulse    f = 131072 / (2048 - R)
 *   GBA wave     f = 65536  / (2048 - R)
 * so in both cases R = 2048 - (P + 1) * 2097152 / 1789773, and that constant
 * is 1.171728..., kept below as a 16.16 fixed-point multiplier.
 */
#include "nes_audio.h"
#include "nes6502.h"
#include "gba_hw.h"
#include "audio_prg.h"

/* ----------------------------------------------------------------------- *
 * GBA sound registers. Kept here rather than in gba_hw.h because nothing
 * else in the port makes a sound.
 * ----------------------------------------------------------------------- */
#define REG_SOUND1CNT_L (*(vu16 *)0x04000060)  /* sweep */
#define REG_SOUND1CNT_H (*(vu16 *)0x04000062)  /* duty / length / envelope */
#define REG_SOUND1CNT_X (*(vu16 *)0x04000064)  /* frequency / control */
#define REG_SOUND2CNT_L (*(vu16 *)0x04000068)
#define REG_SOUND2CNT_H (*(vu16 *)0x0400006C)
#define REG_SOUND3CNT_L (*(vu16 *)0x04000070)  /* wave bank / enable */
#define REG_SOUND3CNT_H (*(vu16 *)0x04000072)  /* length / volume */
#define REG_SOUND3CNT_X (*(vu16 *)0x04000074)
#define REG_SOUND4CNT_L (*(vu16 *)0x04000078)
#define REG_SOUND4CNT_H (*(vu16 *)0x0400007C)
#define REG_SOUNDCNT_L  (*(vu16 *)0x04000080)  /* PSG mixing */
#define REG_SOUNDCNT_H  (*(vu16 *)0x04000082)  /* PSG / DirectSound volume */
#define REG_SOUNDCNT_X  (*(vu16 *)0x04000084)  /* master enable */
#define MEM_WAVE_RAM    ((vu16 *)0x04000090)

/* NES APU registers, by index into the emulated $4000-$4017 block. */
#define R_SQ1_VOL   0x00
#define R_SQ1_SWEEP 0x01
#define R_SQ1_LO    0x02
#define R_SQ1_HI    0x03
#define R_SQ2_VOL   0x04
#define R_SQ2_LO    0x06
#define R_SQ2_HI    0x07
#define R_TRI_LIN   0x08
#define R_TRI_LO    0x0A
#define R_TRI_HI    0x0B
#define R_NOISE_VOL 0x0C
#define R_NOISE_LO  0x0E
#define R_SND_CHN   0x15

/* NES noise periods (NTSC) matched to the nearest GBA divisor/shift pair by
 * frequency. Generated once and checked in: the NES table is fixed hardware,
 * and the GBA's rate is 524288 / divisor / 2^(shift+1) with divisor 0 meaning
 * a half. Twelve of the sixteen land within 7%; the top three NES rates are
 * above anything the GBA can produce and sit 17% high, which no music in this
 * game uses. */
static const uint8_t kNoiseDiv[16]   = {0, 0, 0, 5, 5, 7, 5, 3, 7, 5, 7, 5, 7, 5, 5, 5};
static const uint8_t kNoiseShift[16] = {0, 1, 2, 0, 1, 1, 2, 3, 2, 3, 3, 4, 4, 5, 6, 7};

/* 2097152 / 1789773 in 16.16 fixed point. */
#define NES_TO_GBA_PERIOD 76795u

static Nes6502 g_cpu;
static uint8_t g_nes_ram[0x800];
static bool g_ready;

/* The APU as it was after the previous frame. Channels are only touched when
 * one of their registers actually CHANGES VALUE — see nes_audio_frame. */
static uint8_t g_prev[0x18];

/* The engine's program bytes, copied out of cartridge ROM into external WRAM
 * at startup. Every 6502 instruction fetch reads from here, so it is worth
 * the copy: external WRAM answers in fewer cycles than the cartridge bus, and
 * unlike internal WRAM there is room for all 12KB of it. */
__attribute__((section(".ewram"))) static uint8_t g_prg[AUDIO_PRG_SIZE];

/* The NES triangle's own waveform, written into GBA wave RAM: its 32-step
 * 4-bit ramp, 15 down to 0 and back up, which is exactly the shape wave RAM
 * holds. Both banks get the same 32 samples so the port never has to care
 * which one the hardware decides to play. */
static void load_triangle_wave(void) {
    uint16_t wave[8];
    for (int w = 0; w < 8; w++) {
        uint16_t value = 0;
        for (int byte = 0; byte < 2; byte++) {
            int i = (w * 2 + byte) * 2;          /* first of two samples */
            int s0 = (i < 16) ? (15 - i) : (i - 16);
            int s1 = ((i + 1) < 16) ? (15 - (i + 1)) : ((i + 1) - 16);
            /* Within a byte the first sample is the HIGH nibble. */
            value |= (uint16_t)(((s0 << 4) | s1) << (byte * 8));
        }
        wave[w] = value;
    }
    for (int bank = 0; bank < 2; bank++) {
        REG_SOUND3CNT_L = (uint16_t)(bank ? 0x0040 : 0x0000);
        for (int w = 0; w < 8; w++) MEM_WAVE_RAM[w] = wave[w];
    }
    REG_SOUND3CNT_L = 0x0080;   /* one bank of 32 samples, channel on */
}

static uint16_t gba_rate(uint16_t nes_period) {
    uint32_t scaled = ((uint32_t)(nes_period + 1) * NES_TO_GBA_PERIOD) >> 16;
    if (scaled >= 2048) return 0;      /* lower than the GBA can go; clamp */
    return (uint16_t)(2048 - scaled);
}

/* NES volume/envelope byte -> GBA envelope field.
 *
 * Bit 4 set means "constant volume", which is what this engine uses: the
 * volume shaping is done in software, a new value written every frame. Bit 4
 * clear means the NES's own hardware envelope, whose period is in the low
 * nibble; the GBA has the same idea, so that maps across as a decay. */
static uint16_t envelope_bits(uint8_t vol_byte) {
    uint8_t v = vol_byte & 0x0F;
    if (vol_byte & 0x10) return (uint16_t)(v << 12);          /* fixed volume */
    return (uint16_t)((15 << 12) | (v & 7) << 8);             /* decay from full */
}

static void apply_pulse(int channel, const uint8_t *apu, bool enabled) {
    int base = channel == 0 ? R_SQ1_VOL : R_SQ2_VOL;
    uint8_t vol = apu[base + 0];
    uint16_t period = (uint16_t)(apu[base + 2] | ((apu[base + 3] & 0x07) << 8));

    uint16_t duty = (uint16_t)((vol >> 6) & 3);
    uint16_t cnt_h = (uint16_t)((duty << 6) | envelope_bits(vol));
    if (!enabled) cnt_h &= 0x0FFF;   /* silence by volume; the GBA has no per-channel mute */

    /* Bit 15 restarts the channel, and it has to be set: on this hardware a
     * new volume in the envelope register only takes effect on a restart.
     * That is affordable because the engine writes a channel's registers only
     * when something about it changes — a held note produces no writes at all
     * — so this is not a per-frame retrigger. It also matches the NES, which
     * resets a pulse's phase when the engine writes its high period byte.
     * Bit 14 is left clear so the length counter never stops the note. */
    uint16_t cnt_x = (uint16_t)(0x8000 | (gba_rate(period) & 0x7FF));

    if (channel == 0) {
        REG_SOUND1CNT_L = 0;         /* no sweep: this engine does its own */
        REG_SOUND1CNT_H = cnt_h;
        REG_SOUND1CNT_X = cnt_x;
    } else {
        REG_SOUND2CNT_L = cnt_h;
        REG_SOUND2CNT_H = cnt_x;
    }
}

static void apply_triangle(const uint8_t *apu, bool enabled) {
    uint16_t period = (uint16_t)(apu[R_TRI_LO] | ((apu[R_TRI_HI] & 0x07) << 8));
    /* The NES triangle has no volume control at all: it is on or it is off,
     * decided by $4015 and by the linear counter's reload value. */
    bool on = enabled && (apu[R_TRI_LIN] & 0x7F) != 0;
    REG_SOUND3CNT_L = on ? 0x0080 : 0x0000;
    REG_SOUND3CNT_H = on ? (uint16_t)(1 << 13) : 0;   /* volume code 1 = 100% */
    REG_SOUND3CNT_X = (uint16_t)(0x8000 | (gba_rate(period) & 0x7FF));
}

static void apply_noise(const uint8_t *apu, bool enabled) {
    uint8_t vol = apu[R_NOISE_VOL];
    uint8_t lo = apu[R_NOISE_LO];
    uint16_t cnt_l = envelope_bits(vol);
    if (!enabled) cnt_l &= 0x0FFF;

    uint8_t index = lo & 0x0F;
    uint16_t width = (lo & 0x80) ? 0x0008 : 0x0000;   /* short mode -> 7-bit */
    uint16_t cnt_h = (uint16_t)(kNoiseDiv[index] |
                                 width |
                                 (kNoiseShift[index] << 4) |
                                 0x8000);   /* restart, no length counter */
    REG_SOUND4CNT_L = cnt_l;
    REG_SOUND4CNT_H = cnt_h;
}

void nes_audio_init(void) {
    REG_SOUNDCNT_X = 0x0080;                 /* master enable, before anything else */
    REG_SOUNDCNT_L = 0xFF77;                 /* all four PSG channels, both sides, full */
    REG_SOUNDCNT_H = 0x0002;                 /* PSG at full volume, no DirectSound */
    load_triangle_wave();

    for (unsigned i = 0; i < AUDIO_PRG_SIZE; i++) g_prg[i] = kAudioPrg[i];
    nes6502_init(&g_cpu, g_nes_ram, g_prg, AUDIO_PRG_BASE, AUDIO_PRG_SIZE);
    for (int i = 0; i < 0x18; i++) g_prev[i] = 0;
    g_ready = true;
}

void nes_audio_play(uint8_t track_id) {
    if (!g_ready || nes6502_faulted(&g_cpu)) return;
    nes6502_call(&g_cpu, AUDIO_SET_TRACK_ADDR, track_id, 20000);
}

/* True if any of a channel's registers hold a different value than they did
 * last frame. */
static bool changed(const uint8_t *apu, int first, int last, uint8_t enable_bit) {
    for (int i = first; i <= last; i++)
        if (apu[i] != g_prev[i]) return true;
    return ((apu[R_SND_CHN] ^ g_prev[R_SND_CHN]) & enable_bit) != 0;
}

void nes_audio_frame(void) {
    if (!g_ready || nes6502_faulted(&g_cpu)) return;

    /* The step limit is a hang guard: the engine's worst frame is around 2300
     * instructions, so this is an order of magnitude of headroom. */
    if (!nes6502_call(&g_cpu, AUDIO_UPDATE_ADDR, 0, 60000)) return;

    const uint8_t *apu = g_cpu.bus.apu;
    uint8_t enables = apu[R_SND_CHN];

    /* CHANGED VALUES, not writes. The engine rewrites $4015 every single
     * frame with the same contents, and applying a channel means restarting
     * it — that is unavoidable, because a GBA sound channel only picks up a
     * new envelope volume on a restart. Acting on the write rather than on
     * the change therefore retriggered all four channels sixty times a
     * second, which is audible as a buzz chopping up every held note. The
     * engine only changes a channel's registers when its note or volume
     * actually changes, so diffing costs nothing and fixes it. */
    if (changed(apu, R_SQ1_VOL, R_SQ1_HI, 0x01)) apply_pulse(0, apu, enables & 0x01);
    if (changed(apu, R_SQ2_VOL, R_SQ2_HI, 0x02)) apply_pulse(1, apu, enables & 0x02);
    if (changed(apu, R_TRI_LIN, R_TRI_HI, 0x04)) apply_triangle(apu, enables & 0x04);
    if (changed(apu, R_NOISE_VOL, 0x0F, 0x08)) apply_noise(apu, enables & 0x08);

    for (int i = 0; i < 0x18; i++) g_prev[i] = apu[i];
}
