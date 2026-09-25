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
#include <stddef.h>
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

/* NOT static, and that is deliberate: tools/run_rom.py reads the
 * interpreter's state out of the ELF BY NAME, and link-time optimisation is
 * free to drop the symbol of a file-local object however much it is `used` --
 * which does not break the ROM, it silently SKIPS the check that reads it.
 * External linkage is the only thing that guarantees the name survives. */
Nes6502 g_cpu;
/* NOT static, for the same reason g_cpu is not: the checks in
 * tools/run_rom.py read this out of the ELF by name, and link-time
 * optimisation is free to rename or fold a file-local array — it did, the
 * day the dancers started reading this file's ROM view, and the audio check
 * quietly went from passing to SALTADO. */
uint8_t g_nes_ram[0x800];
static bool g_ready;

/* The APU as it was after the previous frame. Channels are only touched when
 * one of their registers actually CHANGES VALUE — see nes_audio_frame. */
/* Where the test harness finds things inside Nes6502, exported rather than
 * worked out by hand on the other side. tools/run_rom.py reads the emulated
 * APU register file and the interpreter's fault flag out of a running ROM,
 * and both live at offsets that move whenever this struct gains a field —
 * which is exactly what happened when the length counters needed to know
 * about WRITES and not just values. An offset copied into a Python constant
 * goes quietly wrong at that point and the checks start reading a
 * neighbouring byte. This makes the ELF the single place that knows. */
/* `used`, or link-time optimisation throws it away: nothing in
 * the program reads it -- the whole point is that something OUTSIDE the
 * program does -- and LTO can see that across the whole image where a single
 * translation unit could not. (`retain` too, once; this toolchain ignores it
 * with a warning, and the image is byte for byte the same without it.) */
__attribute__((used))
const uint16_t kNes6502Probe[3] = {
    (uint16_t)offsetof(Nes6502, bus.apu),
    (uint16_t)offsetof(Nes6502, faulted),
    0x18,                                  /* how many APU registers there are */
};

static uint8_t g_prev[0x18];
/* The 6502's whole address space as the engine's code sees it, laid out in
 * external WRAM at startup: the program bytes at their own address, zero
 * everywhere else. Every instruction fetch is one load from here (see
 * Nes6502Bus.code), so it is worth both the copy and the 64KB: external
 * WRAM answers in fewer cycles than the cartridge bus, and unlike internal
 * WRAM it has the room. */
__attribute__((section(".ewram"), aligned(4))) static uint8_t g_code[0x10000];

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

/* THE SWEEP, which the NES does in hardware and this engine leans on: the
 * line clear is pulse 2 with `$4005 = $8A` — on, pitch rising, shift 2 — so
 * each of its notes is a fast upward zip, and without the sweep it was the
 * zip's first and lowest note held flat ("mas grave"). The GBA's own sweep is
 * on channel 1 only and works on the frequency rather than the period, so it
 * is done here, as the NES does it, on the period:
 *
 *  - clocked twice a frame (the frame counter's half-frames, 120Hz), through
 *    a divider of P+1 clocks, reloaded by a write to $4001/$4005;
 *  - target = period +/- (period >> shift), and pulse 1's negate subtracts
 *    one more (ones' complement), pulse 2's does not;
 *  - on a clock with the divider at zero, the sweep on and shift non-zero,
 *    and the channel not muted, the period becomes the target;
 *  - MUTED while the period is under 8 or the target over $7FF — whether the
 *    sweep is on or not, which is the NES's and not a choice;
 *  - and a write to a period byte replaces THAT byte of the period the sweep
 *    has moved, the other byte staying where the sweep left it. */
typedef struct {
    uint16_t period;     /* the timer period the channel is really playing */
    uint8_t divider;
    bool reload;
    bool muted;
} PulseSweep;
static PulseSweep g_sweep[2];

static uint16_t sweep_target(int channel, uint16_t period, uint8_t reg) {
    uint16_t change = (uint16_t)(period >> (reg & 7));
    if (reg & 0x08)
        return (uint16_t)(period - change - (channel == 0 ? 1 : 0));
    return (uint16_t)(period + change);
}

static bool sweep_mutes(int channel, uint16_t period, uint8_t reg) {
    if (period < 8) return true;
    return !(reg & 0x08) && sweep_target(channel, period, reg) > 0x7FF;
}

static void apply_pulse(int channel, const uint8_t *apu, bool enabled) {
    int base = channel == 0 ? R_SQ1_VOL : R_SQ2_VOL;
    uint8_t vol = apu[base + 0];
    uint16_t period = g_sweep[channel].period;

    uint16_t duty = (uint16_t)((vol >> 6) & 3);
    uint16_t cnt_h = (uint16_t)((duty << 6) | envelope_bits(vol));
    /* Silence by volume; the GBA has no per-channel mute. */
    if (!enabled || g_sweep[channel].muted) cnt_h &= 0x0FFF;

    /* Bit 15 restarts the channel, and it has to be set: on this hardware a
     * new volume in the envelope register only takes effect on a restart.
     * That is affordable because the engine writes a channel's registers only
     * when something about it changes — a held note produces no writes at all
     * — so this is not a per-frame retrigger. It also matches the NES, which
     * resets a pulse's phase when the engine writes its high period byte.
     * Bit 14 is left clear so the length counter never stops the note. */
    uint16_t cnt_x = (uint16_t)(0x8000 | (gba_rate(period) & 0x7FF));

    if (channel == 0) {
        REG_SOUND1CNT_L = 0;         /* the GBA's sweep is not the NES's; see above */
        REG_SOUND1CNT_H = cnt_h;
        REG_SOUND1CNT_X = cnt_x;
    } else {
        REG_SOUND2CNT_L = cnt_h;
        REG_SOUND2CNT_H = cnt_x;
    }
}

/* The frame's writes, into the period the channel is playing. */
static void sweep_take_writes(int channel, const uint8_t *apu, uint32_t written) {
    int base = channel == 0 ? R_SQ1_VOL : R_SQ2_VOL;
    PulseSweep *sw = &g_sweep[channel];
    if (written & (1u << (base + 2)))
        sw->period = (uint16_t)((sw->period & 0x700) | apu[base + 2]);
    if (written & (1u << (base + 3)))
        sw->period = (uint16_t)((sw->period & 0x0FF) | ((apu[base + 3] & 7) << 8));
    if (written & (1u << (base + 1))) sw->reload = true;
    sw->muted = sweep_mutes(channel, sw->period, apu[base + 1]);
}

/* The frame's two half-frame clocks. True if what the channel sounds like
 * changed: a new period, or muted or let go. */
static bool sweep_clock(int channel, const uint8_t *apu) {
    int base = channel == 0 ? R_SQ1_VOL : R_SQ2_VOL;
    uint8_t reg = apu[base + 1];
    PulseSweep *sw = &g_sweep[channel];
    uint16_t was_period = sw->period;
    bool was_muted = sw->muted;
    for (int half = 0; half < 2; half++) {
        bool muted = sweep_mutes(channel, sw->period, reg);
        if (sw->divider == 0 && (reg & 0x80) && (reg & 7) && !muted)
            sw->period = sweep_target(channel, sw->period, reg);
        if (sw->divider == 0 || sw->reload) {
            sw->divider = (uint8_t)((reg >> 4) & 7);
            sw->reload = false;
        } else {
            sw->divider--;
        }
    }
    sw->muted = sweep_mutes(channel, sw->period, reg);
    return sw->period != was_period || sw->muted != was_muted;
}

/* ...and a swept period reaching the GBA WITHOUT a restart, which would
 * reset the waveform's phase sixty times a second. */
static void sweep_retune(int channel) {
    uint16_t rate = (uint16_t)(gba_rate(g_sweep[channel].period) & 0x7FF);
    if (channel == 0) REG_SOUND1CNT_X = rate;
    else REG_SOUND2CNT_H = rate;
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

    nes6502_init(&g_cpu, g_nes_ram, g_code, kAudioPrg, AUDIO_PRG_BASE, AUDIO_PRG_SIZE);
    /* THE CARTRIDGE'S FIRST FRAME, done here: queue the engine's reset and
     * let one update consume it, before any track is asked for. The reset
     * wipes the queue as it runs, so anything queued alongside it would be
     * lost — hence a frame of its own, as on the cartridge. Without this the
     * engine ran on a zeroed RAM that no reset had ever laid out, and every
     * tune that wanted a voice off the free list played short. See
     * NES_AUDIO_RESET. */
    nes6502_call(&g_cpu, AUDIO_SET_TRACK_ADDR, NES_AUDIO_RESET, 20000);
    nes6502_call(&g_cpu, AUDIO_UPDATE_ADDR, 0, 60000);
    for (int i = 0; i < 0x18; i++) g_prev[i] = 0;
    g_ready = true;
}

void nes_audio_play(uint8_t track_id) {
    if (!g_ready || nes6502_faulted(&g_cpu)) return;
    nes6502_call(&g_cpu, AUDIO_SET_TRACK_ADDR, track_id, 20000);
}

/* The rest of the cartridge, for callers that are not the sound engine; see
 * the note in nes_audio.h. One machine, one RAM — deliberately. */
bool nes_rom_call(uint16_t addr, uint8_t a, uint32_t max_steps) {
    if (!g_ready || nes6502_faulted(&g_cpu)) return false;
    return nes6502_call(&g_cpu, addr, a, max_steps);
}

uint8_t *nes_rom_ram(void) { return g_nes_ram; }

/* ONE BYTE OF THE CARTRIDGE, by its own address. The 64KB view the
 * interpreter fetches from holds the whole slice at the addresses the 6502
 * knows it by, so a caller that wants the cartridge's DATA rather than its
 * code — a table of pointers, a list of tile ids — can read it here instead
 * of having it extracted into a header of its own. The dancers' choreography
 * is read this way; see the driver in gba/hud.c. */
uint8_t nes_rom_peek(uint16_t addr) { return g_code[addr]; }

/* ...and what the last nes_rom_call left in A, for the routines whose answer
 * is a return value rather than a write to RAM. */
uint8_t nes_rom_acc(void) { return g_cpu.a; }

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
     * actually changes, so diffing costs nothing and fixes it.
     *
     * $4015 IS THE NOTE-OFF. This engine does not rely on the APU's length
     * counters at all — it sets the halt bit on every note it starts
     * ($4000 = $B7) and then ends notes by clearing the channel's bit in
     * $4015, several times a frame as it works through the voices. So the
     * enable bits are part of "changed", and a port that models the length
     * counters instead gets nothing for the trouble: they never count down
     * in this game. Measured, not assumed. */
    uint32_t written = g_cpu.bus.apu_written;
    g_cpu.bus.apu_written = 0;
    for (int c = 0; c < 2; c++) {
        uint8_t bit = (uint8_t)(1 << c);
        int base = c == 0 ? R_SQ1_VOL : R_SQ2_VOL;
        sweep_take_writes(c, apu, written);
        bool was_muted = g_sweep[c].muted;
        bool restart = changed(apu, base, base + 3, bit);
        /* The sweep moves the note between the engine's writes, on the
         * NES's clock; see PulseSweep. A new note restarts as before, with
         * whatever period the clocks leave it on. */
        bool moved = sweep_clock(c, apu);
        if (restart || (moved && g_sweep[c].muted != was_muted))
            apply_pulse(c, apu, enables & bit);
        else if (moved)
            sweep_retune(c);
    }
    if (changed(apu, R_TRI_LIN, R_TRI_HI, 0x04)) apply_triangle(apu, enables & 0x04);
    if (changed(apu, R_NOISE_VOL, 0x0F, 0x08)) apply_noise(apu, enables & 0x08);

    for (int i = 0; i < 0x18; i++) g_prev[i] = apu[i];
}

/* ----------------------------------------------------------------------- *
 * A CAPTURED EFFECT, replayed
 *
 * The four builds do not agree on the noise a menu makes, and only one of
 * their sound engines is in this ROM. So a prototype's tick and chirp travel
 * as what they are on the cartridge — the four registers of one pulse
 * channel, frame by frame, captured off the dump — and are replayed here,
 * through the SAME conversion the engine's own notes go through. That is the
 * point of putting this in this file rather than beside the hand-entered
 * tunes: nothing about the NES pulse channel is described twice.
 *
 * IT SHARES THE CHANNEL the way everything else here does. nes_audio_frame
 * only touches a channel whose registers CHANGED, so an effect holds the
 * channel until the engine's own next note takes it back — which is exactly
 * what happens on the cartridge, where an effect and a tune are voices
 * competing for the same three channels.
 * ----------------------------------------------------------------------- */
static const uint8_t *g_fx_regs;    /* four bytes a frame, or NULL */
static uint32_t g_fx_write;         /* one bit a frame: does it write? */
static uint8_t g_fx_frames;         /* how many frames long */
static uint8_t g_fx_at;             /* which frame it is on */
static uint8_t g_fx_channel;

void nes_audio_effect(const uint8_t *regs, uint32_t write, uint8_t frames,
                       uint8_t channel) {
    if (channel > 1 || frames == 0) { g_fx_regs = 0; return; }
    g_fx_regs = regs;
    g_fx_write = write;
    g_fx_frames = frames;
    g_fx_channel = channel;
    g_fx_at = 0;
}

void nes_audio_effect_frame(void) {
    if (!g_fx_regs) return;
    if (g_fx_at >= g_fx_frames) { g_fx_regs = 0; return; }
    uint8_t at = g_fx_at++;
    if (!(g_fx_write & (1u << at))) return;
    const uint8_t *r = g_fx_regs + at * 4;
    /* $4000/$4004 is DDLC VVVV and $4002/$4003 the eleven-bit period, exactly
     * as apply_pulse reads them off the emulated APU. */
    uint16_t period = (uint16_t)(r[2] | ((r[3] & 0x07) << 8));
    uint16_t cnt_h = (uint16_t)((((r[0] >> 6) & 3) << 6) | envelope_bits(r[0]));
    uint16_t cnt_x = (uint16_t)(0x8000 | (gba_rate(period) & 0x7FF));
    if (g_fx_channel == 0) {
        REG_SOUND1CNT_L = 0;         /* no sweep: these builds sweep by hand */
        REG_SOUND1CNT_H = cnt_h;
        REG_SOUND1CNT_X = cnt_x;
    } else {
        REG_SOUND2CNT_L = cnt_h;
        REG_SOUND2CNT_H = cnt_x;
    }
}
