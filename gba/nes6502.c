/*
 * nes6502.c — the interpreter. See nes6502.h for why it exists at all.
 *
 * Everything here is marked IWRAM_CODE: it is the only thing in this port
 * that runs a few thousand instructions every single frame, and internal WRAM
 * is a 32-bit bus with no wait states while the cartridge is 16-bit with
 * several. Built as ARM rather than Thumb for the same reason — Thumb's
 * smaller code only pays off on the 16-bit cartridge bus.
 */
#include "nes6502.h"

#define IWRAM_CODE __attribute__((section(".iwram"), long_call, target("arm")))

/* Flag bits. */
#define F_C 0x01
#define F_Z 0x02
#define F_I 0x04
#define F_D 0x08
#define F_B 0x10
#define F_U 0x20
#define F_V 0x40
#define F_N 0x80

/* A return address that cannot be real code here, so landing on it means the
 * subroutine we called has returned. */
#define SENTINEL 0xFFF0u

IWRAM_CODE static uint8_t bus_read(Nes6502 *cpu, uint16_t addr) {
    if (addr < 0x2000) return cpu->bus.ram[addr & 0x7FF];
    if (addr >= 0x4000 && addr <= 0x4017) return cpu->bus.apu[addr - 0x4000];
    /* The program's own bytes, or zero for anything outside what the sound
     * engine was measured to touch: the view is built that way, so reading
     * it is deterministic rather than a read past an array. */
    return cpu->bus.code[addr];
}

IWRAM_CODE static void bus_write(Nes6502 *cpu, uint16_t addr, uint8_t value) {
    if (addr < 0x2000) {
        cpu->bus.ram[addr & 0x7FF] = value;
        return;
    }
    if (addr >= 0x4000 && addr <= 0x4017) cpu->bus.apu[addr - 0x4000] = value;
}

void nes6502_init(Nes6502 *cpu, uint8_t *ram, uint8_t *code_view,
                   const uint8_t *prg, uint16_t prg_base, uint32_t prg_size) {
    for (int i = 0; i < 0x800; i++) ram[i] = 0;
    /* A word at a time: 64KB of byte stores into external WRAM is most of a
     * frame, and this runs before the title is drawn. The view is
     * word-aligned by its caller. */
    {
        uint32_t *words = (uint32_t *)code_view;
        for (uint32_t i = 0; i < 0x10000 / 4; i++) words[i] = 0;
    }
    if (((uintptr_t)prg & 3) == 0 && (prg_base & 3) == 0 &&
        (uint32_t)prg_base + prg_size <= 0x10000) {
        const uint32_t *src = (const uint32_t *)prg;
        uint32_t *dst = (uint32_t *)(code_view + prg_base);
        uint32_t n = prg_size / 4;
        for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
        for (uint32_t i = n * 4; i < prg_size; i++) code_view[prg_base + i] = prg[i];
    } else {
        for (uint32_t i = 0; i < prg_size && prg_base + i < 0x10000; i++)
            code_view[prg_base + i] = prg[i];
    }
    cpu->bus.ram = ram;
    cpu->bus.code = code_view;
    for (int i = 0; i < 0x18; i++) cpu->bus.apu[i] = 0;
    cpu->a = cpu->x = cpu->y = 0;
    cpu->sp = 0xFD;
    cpu->p = F_U | F_I;
    cpu->pc = 0;
    cpu->faulted = false;
}

IWRAM_CODE bool nes6502_call(Nes6502 *cpu, uint16_t addr, uint8_t a,
                              uint32_t max_steps) {
    if (cpu->faulted) return false;

    cpu->a = a;
    /* Push the sentinel as the return address, low byte last, the way JSR
     * leaves (address - 1) on the stack. */
    uint16_t ret = SENTINEL - 1;
    cpu->bus.ram[0x100 + cpu->sp] = (uint8_t)(ret >> 8);
    cpu->sp--;
    cpu->bus.ram[0x100 + cpu->sp] = (uint8_t)ret;
    cpu->sp--;
    cpu->pc = addr;

    uint8_t A = cpu->a, X = cpu->x, Y = cpu->y, S = cpu->sp, P = cpu->p;
    uint16_t PC = cpu->pc;
    /* THE FETCH IS ONE LOAD. Every instruction fetches one to three bytes,
     * and going through bus_read's chain of range checks for each of them
     * was the biggest single cost of a frame. The title screen, which runs
     * the fireworks and the sound engine on top of its own drawing, had none
     * of that to give: it missed a vblank in a hundred and fifty once the
     * engine had a free list to walk (see NES_AUDIO_RESET). The 64KB view
     * makes the check unnecessary — see Nes6502Bus.code — and one load per
     * byte is also the SMALLEST way to write it, which matters: this switch
     * is ~200 fetch sites in internal WRAM, and a fast path inlined at each
     * of them once grew it past the stacks. */
    const uint8_t *const code = cpu->bus.code;

/* Locals kept in registers; these macros are what makes that readable. */
#define RD(addr_) bus_read(cpu, (uint16_t)(addr_))
#define WR(addr_, v_) bus_write(cpu, (uint16_t)(addr_), (uint8_t)(v_))
#define FETCH() code[PC++]
#define FETCH16() (PC += 2, (uint16_t)(code[(uint16_t)(PC - 2)] | (code[(uint16_t)(PC - 1)] << 8)))
#define SETF(mask_, on_) (P = (on_) ? (uint8_t)(P | (mask_)) : (uint8_t)(P & ~(mask_)))
#define ZN(v_) do { uint8_t t_ = (uint8_t)(v_); \
                     P = (uint8_t)((P & (uint8_t)~(F_Z | F_N)) | (t_ == 0 ? F_Z : 0) | (t_ & F_N)); } while (0)
#define PUSH(v_) do { cpu->bus.ram[0x100 + S] = (uint8_t)(v_); S--; } while (0)
#define POP() (cpu->bus.ram[0x100 + (uint8_t)(++S)])

/* Addressing modes, each yielding an effective address. */
#define A_ZP()   ((uint16_t)FETCH())
#define A_ZPX()  ((uint16_t)(uint8_t)(FETCH() + X))
#define A_ZPY()  ((uint16_t)(uint8_t)(FETCH() + Y))
#define A_ABS()  FETCH16()
#define A_ABX()  ((uint16_t)(FETCH16() + X))
#define A_ABY()  ((uint16_t)(FETCH16() + Y))

    uint16_t ea;
    uint8_t v;

    for (uint32_t step = 0; step < max_steps; step++) {
        if (PC == SENTINEL) {
            cpu->a = A; cpu->x = X; cpu->y = Y; cpu->sp = S; cpu->p = P;
            cpu->pc = PC;
            return true;
        }

        uint8_t op = FETCH();
        switch (op) {
        /* -- loads ------------------------------------------------------ */
        case 0xA9: A = FETCH(); ZN(A); break;
        case 0xA5: A = RD(A_ZP()); ZN(A); break;
        case 0xB5: A = RD(A_ZPX()); ZN(A); break;
        case 0xAD: A = RD(A_ABS()); ZN(A); break;
        case 0xBD: A = RD(A_ABX()); ZN(A); break;
        case 0xB9: A = RD(A_ABY()); ZN(A); break;
        case 0xA1: { uint8_t z = (uint8_t)(FETCH() + X);
                     ea = (uint16_t)(RD(z) | (RD((uint8_t)(z + 1)) << 8));
                     A = RD(ea); ZN(A); } break;
        case 0xB1: { uint8_t z = FETCH();
                     ea = (uint16_t)((RD(z) | (RD((uint8_t)(z + 1)) << 8)) + Y);
                     A = RD(ea); ZN(A); } break;
        case 0xA2: X = FETCH(); ZN(X); break;
        case 0xA6: X = RD(A_ZP()); ZN(X); break;
        case 0xB6: X = RD(A_ZPY()); ZN(X); break;
        case 0xAE: X = RD(A_ABS()); ZN(X); break;
        case 0xBE: X = RD(A_ABY()); ZN(X); break;
        case 0xA0: Y = FETCH(); ZN(Y); break;
        case 0xA4: Y = RD(A_ZP()); ZN(Y); break;
        case 0xB4: Y = RD(A_ZPX()); ZN(Y); break;
        case 0xAC: Y = RD(A_ABS()); ZN(Y); break;
        case 0xBC: Y = RD(A_ABX()); ZN(Y); break;

        /* -- stores ----------------------------------------------------- */
        case 0x85: WR(A_ZP(), A); break;
        case 0x95: WR(A_ZPX(), A); break;
        case 0x8D: WR(A_ABS(), A); break;
        case 0x9D: WR(A_ABX(), A); break;
        case 0x99: WR(A_ABY(), A); break;
        case 0x81: { uint8_t z = (uint8_t)(FETCH() + X);
                     ea = (uint16_t)(RD(z) | (RD((uint8_t)(z + 1)) << 8));
                     WR(ea, A); } break;
        case 0x91: { uint8_t z = FETCH();
                     ea = (uint16_t)((RD(z) | (RD((uint8_t)(z + 1)) << 8)) + Y);
                     WR(ea, A); } break;
        case 0x86: WR(A_ZP(), X); break;
        case 0x96: WR(A_ZPY(), X); break;
        case 0x8E: WR(A_ABS(), X); break;
        case 0x84: WR(A_ZP(), Y); break;
        case 0x94: WR(A_ZPX(), Y); break;
        case 0x8C: WR(A_ABS(), Y); break;

        /* -- transfers -------------------------------------------------- */
        case 0xAA: X = A; ZN(X); break;
        case 0xA8: Y = A; ZN(Y); break;
        case 0x8A: A = X; ZN(A); break;
        case 0x98: A = Y; ZN(A); break;
        case 0xBA: X = S; ZN(X); break;
        case 0x9A: S = X; break;

        /* -- stack ------------------------------------------------------ */
        case 0x48: PUSH(A); break;
        case 0x68: A = POP(); ZN(A); break;
        case 0x08: PUSH(P | F_B | F_U); break;
        case 0x28: P = (uint8_t)((POP() | F_U) & ~F_B); break;

        /* -- logic ------------------------------------------------------ */
#define LOGIC(fetch_, oper_) do { A = (uint8_t)(A oper_ (fetch_)); ZN(A); } while (0)
        case 0x29: LOGIC(FETCH(), &); break;
        case 0x25: LOGIC(RD(A_ZP()), &); break;
        case 0x35: LOGIC(RD(A_ZPX()), &); break;
        case 0x2D: LOGIC(RD(A_ABS()), &); break;
        case 0x3D: LOGIC(RD(A_ABX()), &); break;
        case 0x39: LOGIC(RD(A_ABY()), &); break;
        case 0x21: { uint8_t z = (uint8_t)(FETCH() + X);
                     ea = (uint16_t)(RD(z) | (RD((uint8_t)(z + 1)) << 8));
                     LOGIC(RD(ea), &); } break;
        case 0x31: { uint8_t z = FETCH();
                     ea = (uint16_t)((RD(z) | (RD((uint8_t)(z + 1)) << 8)) + Y);
                     LOGIC(RD(ea), &); } break;
        case 0x09: LOGIC(FETCH(), |); break;
        case 0x05: LOGIC(RD(A_ZP()), |); break;
        case 0x15: LOGIC(RD(A_ZPX()), |); break;
        case 0x0D: LOGIC(RD(A_ABS()), |); break;
        case 0x1D: LOGIC(RD(A_ABX()), |); break;
        case 0x19: LOGIC(RD(A_ABY()), |); break;
        case 0x01: { uint8_t z = (uint8_t)(FETCH() + X);
                     ea = (uint16_t)(RD(z) | (RD((uint8_t)(z + 1)) << 8));
                     LOGIC(RD(ea), |); } break;
        case 0x11: { uint8_t z = FETCH();
                     ea = (uint16_t)((RD(z) | (RD((uint8_t)(z + 1)) << 8)) + Y);
                     LOGIC(RD(ea), |); } break;
        case 0x49: LOGIC(FETCH(), ^); break;
        case 0x45: LOGIC(RD(A_ZP()), ^); break;
        case 0x55: LOGIC(RD(A_ZPX()), ^); break;
        case 0x4D: LOGIC(RD(A_ABS()), ^); break;
        case 0x5D: LOGIC(RD(A_ABX()), ^); break;
        case 0x59: LOGIC(RD(A_ABY()), ^); break;
        case 0x41: { uint8_t z = (uint8_t)(FETCH() + X);
                     ea = (uint16_t)(RD(z) | (RD((uint8_t)(z + 1)) << 8));
                     LOGIC(RD(ea), ^); } break;
        case 0x51: { uint8_t z = FETCH();
                     ea = (uint16_t)((RD(z) | (RD((uint8_t)(z + 1)) << 8)) + Y);
                     LOGIC(RD(ea), ^); } break;
#undef LOGIC

        case 0x24: case 0x2C:
            v = RD(op == 0x24 ? A_ZP() : A_ABS());
            SETF(F_Z, (A & v) == 0);
            SETF(F_N, v & 0x80);
            SETF(F_V, v & 0x40);
            break;

        /* -- arithmetic -------------------------------------------------
         * NO DECIMAL MODE, because the NES has none: the 2A03 is a 6502 with
         * the BCD circuitry disabled in silicon, so SED sets the D flag and
         * ADC/SBC carry on in binary regardless. This used to implement it,
         * on the reasoning that a silently wrong ADC would be hard to debug;
         * but a BCD ADC on a machine that never does one is the wrong answer
         * in the one case it ever fires. The game keeps its scores in ASCII
         * digits and never sets D anyway. */
#define ADC(value_) do { \
        uint8_t m_ = (uint8_t)(value_); \
        unsigned t_ = (unsigned)A + m_ + ((P & F_C) ? 1u : 0u); \
        SETF(F_C, t_ > 0xFF); \
        SETF(F_V, (~(A ^ m_) & (A ^ (uint8_t)t_) & 0x80) != 0); \
        A = (uint8_t)t_; ZN(A); } while (0)

#define SBC(value_) ADC((uint8_t)((uint8_t)(value_) ^ 0xFF))

        case 0x69: ADC(FETCH()); break;
        case 0x65: ADC(RD(A_ZP())); break;
        case 0x75: ADC(RD(A_ZPX())); break;
        case 0x6D: ADC(RD(A_ABS())); break;
        case 0x7D: ADC(RD(A_ABX())); break;
        case 0x79: ADC(RD(A_ABY())); break;
        case 0x61: { uint8_t z = (uint8_t)(FETCH() + X);
                     ea = (uint16_t)(RD(z) | (RD((uint8_t)(z + 1)) << 8));
                     ADC(RD(ea)); } break;
        case 0x71: { uint8_t z = FETCH();
                     ea = (uint16_t)((RD(z) | (RD((uint8_t)(z + 1)) << 8)) + Y);
                     ADC(RD(ea)); } break;
        case 0xE9: SBC(FETCH()); break;
        case 0xE5: SBC(RD(A_ZP())); break;
        case 0xF5: SBC(RD(A_ZPX())); break;
        case 0xED: SBC(RD(A_ABS())); break;
        case 0xFD: SBC(RD(A_ABX())); break;
        case 0xF9: SBC(RD(A_ABY())); break;
        case 0xE1: { uint8_t z = (uint8_t)(FETCH() + X);
                     ea = (uint16_t)(RD(z) | (RD((uint8_t)(z + 1)) << 8));
                     SBC(RD(ea)); } break;
        case 0xF1: { uint8_t z = FETCH();
                     ea = (uint16_t)((RD(z) | (RD((uint8_t)(z + 1)) << 8)) + Y);
                     SBC(RD(ea)); } break;
#undef ADC
#undef SBC

#define CMP(reg_, value_) do { uint8_t m_ = (uint8_t)(value_); \
        SETF(F_C, (reg_) >= m_); ZN((uint8_t)((reg_) - m_)); } while (0)
        case 0xC9: CMP(A, FETCH()); break;
        case 0xC5: CMP(A, RD(A_ZP())); break;
        case 0xD5: CMP(A, RD(A_ZPX())); break;
        case 0xCD: CMP(A, RD(A_ABS())); break;
        case 0xDD: CMP(A, RD(A_ABX())); break;
        case 0xD9: CMP(A, RD(A_ABY())); break;
        case 0xC1: { uint8_t z = (uint8_t)(FETCH() + X);
                     ea = (uint16_t)(RD(z) | (RD((uint8_t)(z + 1)) << 8));
                     CMP(A, RD(ea)); } break;
        case 0xD1: { uint8_t z = FETCH();
                     ea = (uint16_t)((RD(z) | (RD((uint8_t)(z + 1)) << 8)) + Y);
                     CMP(A, RD(ea)); } break;
        case 0xE0: CMP(X, FETCH()); break;
        case 0xE4: CMP(X, RD(A_ZP())); break;
        case 0xEC: CMP(X, RD(A_ABS())); break;
        case 0xC0: CMP(Y, FETCH()); break;
        case 0xC4: CMP(Y, RD(A_ZP())); break;
        case 0xCC: CMP(Y, RD(A_ABS())); break;
#undef CMP

        /* -- increment / decrement -------------------------------------- */
        case 0xE6: ea = A_ZP();  v = (uint8_t)(RD(ea) + 1); WR(ea, v); ZN(v); break;
        case 0xF6: ea = A_ZPX(); v = (uint8_t)(RD(ea) + 1); WR(ea, v); ZN(v); break;
        case 0xEE: ea = A_ABS(); v = (uint8_t)(RD(ea) + 1); WR(ea, v); ZN(v); break;
        case 0xFE: ea = A_ABX(); v = (uint8_t)(RD(ea) + 1); WR(ea, v); ZN(v); break;
        case 0xC6: ea = A_ZP();  v = (uint8_t)(RD(ea) - 1); WR(ea, v); ZN(v); break;
        case 0xD6: ea = A_ZPX(); v = (uint8_t)(RD(ea) - 1); WR(ea, v); ZN(v); break;
        case 0xCE: ea = A_ABS(); v = (uint8_t)(RD(ea) - 1); WR(ea, v); ZN(v); break;
        case 0xDE: ea = A_ABX(); v = (uint8_t)(RD(ea) - 1); WR(ea, v); ZN(v); break;
        case 0xE8: X++; ZN(X); break;
        case 0xC8: Y++; ZN(Y); break;
        case 0xCA: X--; ZN(X); break;
        case 0x88: Y--; ZN(Y); break;

        /* -- shifts ------------------------------------------------------ */
#define ASL(v_) ({ uint8_t s_ = (uint8_t)(v_); SETF(F_C, s_ & 0x80); \
                    s_ = (uint8_t)(s_ << 1); ZN(s_); s_; })
#define LSR(v_) ({ uint8_t s_ = (uint8_t)(v_); SETF(F_C, s_ & 0x01); \
                    s_ = (uint8_t)(s_ >> 1); ZN(s_); s_; })
#define ROL(v_) ({ uint8_t s_ = (uint8_t)(v_); uint8_t c_ = (P & F_C) ? 1 : 0; \
                    SETF(F_C, s_ & 0x80); s_ = (uint8_t)((s_ << 1) | c_); ZN(s_); s_; })
#define ROR(v_) ({ uint8_t s_ = (uint8_t)(v_); uint8_t c_ = (P & F_C) ? 0x80 : 0; \
                    SETF(F_C, s_ & 0x01); s_ = (uint8_t)((s_ >> 1) | c_); ZN(s_); s_; })
        case 0x0A: A = ASL(A); break;
        case 0x06: ea = A_ZP();  WR(ea, ASL(RD(ea))); break;
        case 0x16: ea = A_ZPX(); WR(ea, ASL(RD(ea))); break;
        case 0x0E: ea = A_ABS(); WR(ea, ASL(RD(ea))); break;
        case 0x1E: ea = A_ABX(); WR(ea, ASL(RD(ea))); break;
        case 0x4A: A = LSR(A); break;
        case 0x46: ea = A_ZP();  WR(ea, LSR(RD(ea))); break;
        case 0x56: ea = A_ZPX(); WR(ea, LSR(RD(ea))); break;
        case 0x4E: ea = A_ABS(); WR(ea, LSR(RD(ea))); break;
        case 0x5E: ea = A_ABX(); WR(ea, LSR(RD(ea))); break;
        case 0x2A: A = ROL(A); break;
        case 0x26: ea = A_ZP();  WR(ea, ROL(RD(ea))); break;
        case 0x36: ea = A_ZPX(); WR(ea, ROL(RD(ea))); break;
        case 0x2E: ea = A_ABS(); WR(ea, ROL(RD(ea))); break;
        case 0x3E: ea = A_ABX(); WR(ea, ROL(RD(ea))); break;
        case 0x6A: A = ROR(A); break;
        case 0x66: ea = A_ZP();  WR(ea, ROR(RD(ea))); break;
        case 0x76: ea = A_ZPX(); WR(ea, ROR(RD(ea))); break;
        case 0x6E: ea = A_ABS(); WR(ea, ROR(RD(ea))); break;
        case 0x7E: ea = A_ABX(); WR(ea, ROR(RD(ea))); break;
#undef ASL
#undef LSR
#undef ROL
#undef ROR

        /* -- jumps and calls --------------------------------------------- */
        case 0x4C: PC = A_ABS(); break;
        case 0x6C: {
            /* JMP ($nnnn), page-wrap bug included: 'fixing' it here would be
             * a divergence from the hardware the engine was written for. */
            uint16_t ptr = FETCH16();
            uint8_t lo = RD(ptr);
            uint8_t hi = RD((uint16_t)((ptr & 0xFF00) | ((ptr + 1) & 0xFF)));
            PC = (uint16_t)(lo | (hi << 8));
        } break;
        case 0x20: {
            uint16_t target = FETCH16();
            uint16_t r = (uint16_t)(PC - 1);
            PUSH(r >> 8);
            PUSH(r & 0xFF);
            PC = target;
        } break;
        case 0x60: { uint8_t lo = POP(); PC = (uint16_t)((lo | (POP() << 8)) + 1); } break;
        case 0x40: { P = (uint8_t)((POP() | F_U) & ~F_B);
                     uint8_t lo = POP(); PC = (uint16_t)(lo | (POP() << 8)); } break;

        /* -- branches ----------------------------------------------------- */
#define BRANCH(cond_) do { int8_t off_ = (int8_t)FETCH(); \
        if (cond_) PC = (uint16_t)(PC + off_); } while (0)
        case 0x10: BRANCH(!(P & F_N)); break;
        case 0x30: BRANCH(P & F_N); break;
        case 0x50: BRANCH(!(P & F_V)); break;
        case 0x70: BRANCH(P & F_V); break;
        case 0x90: BRANCH(!(P & F_C)); break;
        case 0xB0: BRANCH(P & F_C); break;
        case 0xD0: BRANCH(!(P & F_Z)); break;
        case 0xF0: BRANCH(P & F_Z); break;
#undef BRANCH

        /* -- flags and nop ------------------------------------------------ */
        case 0x18: SETF(F_C, 0); break;
        case 0x38: SETF(F_C, 1); break;
        case 0x58: SETF(F_I, 0); break;
        case 0x78: SETF(F_I, 1); break;
        case 0xB8: SETF(F_V, 0); break;
        case 0xD8: SETF(F_D, 0); break;
        case 0xF8: SETF(F_D, 1); break;
        case 0xEA: break;

        default:
            /* A BRK or an undocumented opcode means this is no longer the code
             * we think it is. Stop for good rather than making noise. */
            cpu->faulted = true;
            return false;
        }
    }

    /* Ran away. Same reasoning as above. */
    cpu->faulted = true;
    return false;

#undef RD
#undef WR
#undef FETCH
#undef FETCH16
#undef SETF
#undef ZN
#undef PUSH
#undef POP
#undef A_ZP
#undef A_ZPX
#undef A_ZPY
#undef A_ABS
#undef A_ABX
#undef A_ABY
}
