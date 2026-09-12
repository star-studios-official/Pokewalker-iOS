/*
 * h8300h.c — H8/300H Tiny CPU emulator core for PokéWalker
 *
 * Ported from pokestride (github.com/edgarburgues/pokestride)
 * 3DS-specific code (I2C, IR hardware, NDSP, citro2d) replaced with
 * callback interfaces for cross-platform use.
 *
 * License: GPLv3 (same as pokestride)
 */

#include "h8300h.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ── Vector table macros ────────────────────────────────────────────────── */

#define VECTOR_AT(state, n) \
    ((state)->memory[(n)*2] << 8 | (state)->memory[(n)*2 + 1])

#define VECTOR_TIMER_B1(state)        VECTOR_AT(state, 33)
#define VECTOR_TIMER_W(state)         VECTOR_AT(state, 35)
#define VECTOR_IRQ0(state)            VECTOR_AT(state, 16)
#define VECTOR_RTC_QUARTER_SEC(state) VECTOR_AT(state, 23)

/* ── MMIO address constants ─────────────────────────────────────────────── */

#define TCNT_ADDRESS  0xF0F6
#define PORT1_ADDR    0xFFD4
#define PORT9_ADDR    0xFFDC
#define PORTB_ADDR    0xFFDE

/* Timer W control bits */
#define CTS  (1 << 7)
#define CCLR (1 << 7)

/* Timer B control bits */
#define TMB_AUTORELOAD (1 << 7)
#define TMB_COUNTING   (1 << 6)

/* Clock halt bits */
#define TB1CKSTP  (1 << 2)
#define TWCKSTP   (1 << 6)

/* Interrupt flags */
#define IRRI0    (1 << 0)
#define IRRTB1   (1 << 2)
#define _025SEIFG (1 << 0)

/* SCI3 bits */
#define SCI3_TE   (1 << 5)
#define SCI3_RE   (1 << 4)
#define SCI3_TDRE (1 << 7)
#define SCI3_RDRF (1 << 6)
#define SCI3_TEND (1 << 2)

/* ── Register reference helpers ──────────────────────────────────────────── */

static inline RegRef8 getRegRef8(H8State *s, uint8_t operand) {
    RegRef8 r;
    r.idx = operand & 7;
    r.loOrHiReg = (operand & 8) ? 'l' : 'h';
    r.ptr = (r.loOrHiReg == 'l') ? (uint8_t*)&s->er[r.idx] + 3
                                  : (uint8_t*)&s->er[r.idx] + 2;
    return r;
}

static inline RegRef16 getRegRef16(H8State *s, uint8_t operand) {
    RegRef16 r;
    r.idx = operand & 7;
    r.loOrHiReg = (operand & 8) ? 'e' : 'r';
    r.ptr = (r.loOrHiReg == 'r') ? (uint16_t*)&s->er[r.idx] + 1
                                  : (uint16_t*)&s->er[r.idx];
    return r;
}

static inline RegRef32 getRegRef32(H8State *s, uint8_t operand) {
    RegRef32 r;
    r.idx = operand & 7;
    r.ptr = &s->er[r.idx];
    return r;
}

/* ── Flag operations ────────────────────────────────────────────────────── */

static void setFlags(H8Flags *f, uint8_t value) {
    f->C  = value & (1 << 0);
    f->V  = value & (1 << 1);
    f->Z  = value & (1 << 2);
    f->N  = value & (1 << 3);
    f->U  = value & (1 << 4);
    f->H  = value & (1 << 5);
    f->UI = value & (1 << 6);
    f->I  = value & (1 << 7);
}

static uint8_t packFlags(H8Flags *f) {
    return (uint8_t)(
        (f->C  ? (1<<0) : 0) |
        (f->V  ? (1<<1) : 0) |
        (f->Z  ? (1<<2) : 0) |
        (f->N  ? (1<<3) : 0) |
        (f->U  ? (1<<4) : 0) |
        (f->H  ? (1<<5) : 0) |
        (f->UI ? (1<<6) : 0) |
        (f->I  ? (1<<7) : 0));
}

static void setFlagsADD(H8Flags *f, uint32_t v1, uint32_t v2, int bits) {
    uint32_t negFlag, halfFlag, maxLo;
    switch (bits) {
        case 8:  negFlag = 0x80;       halfFlag = 0x8;    maxLo = 0xF;  break;
        case 16: negFlag = 0x8000;     halfFlag = 0x100;  maxLo = 0xFF; break;
        case 32: negFlag = 0x80000000; halfFlag = 0x10000; maxLo = 0xFFFF; break;
        default: return;
    }
    uint32_t result = v1 + v2;
    f->Z = ((bits == 8) ? (uint8_t)result : (bits == 16) ? (uint16_t)result : result) == 0;
    f->N = result & negFlag;
    f->V = ~(v1 ^ v2) & ((result) ^ v1) & negFlag;
    f->C = (v1 & negFlag) && !(v2 & negFlag) && !(result & negFlag);
    f->H = ((v1 & maxLo) + (v2 & maxLo)) & halfFlag ? 1 : 0;
}

static void setFlagsSUB(H8Flags *f, uint32_t v1, uint32_t v2, int bits) {
    uint32_t negFlag, halfFlag, maxLo;
    switch (bits) {
        case 8:  negFlag = 0x80;       halfFlag = 0x8;    maxLo = 0xF;  break;
        case 16: negFlag = 0x8000;     halfFlag = 0x100;  maxLo = 0xFF; break;
        case 32: negFlag = 0x80000000; halfFlag = 0x10000; maxLo = 0xFFFF; break;
        default: return;
    }
    uint32_t result = v1 - v2;
    f->Z = result == 0;
    f->N = result & negFlag;
    f->V = ((v1 ^ v2) & negFlag) && (~(result ^ v2) & negFlag);
    f->C = v2 > v1;
    f->H = (v2 & maxLo) > (v1 & maxLo);
}

static void setFlagsINC(H8Flags *f, uint32_t v1, uint32_t v2, int bits) {
    uint32_t negFlag = (1u << (bits - 1));
    uint32_t result = v1 + v2;
    f->N = result & negFlag;
    f->Z = result == 0;
    f->V = ~(v1 ^ v2) & (result ^ v1) & negFlag;
}

static void setFlagsMOV(H8Flags *f, uint32_t value, int bits) {
    f->V = 0;
    f->Z = value == 0;
    switch (bits) {
        case 8:  f->N = value & 0x80;       break;
        case 16: f->N = value & 0x8000;     break;
        case 32: f->N = value & 0x80000000; break;
    }
}

/* ── Memory access ──────────────────────────────────────────────────────── */

static uint16_t getMem16(H8State *s, uint32_t addr) {
    addr &= 0xFFFF;
    return (uint16_t)((s->memory[addr] << 8) | s->memory[addr + 1]);
}

static uint32_t getMem32(H8State *s, uint32_t addr) {
    addr &= 0xFFFF;
    return (uint32_t)((s->memory[addr] << 24) | (s->memory[addr+1] << 16) |
                       (s->memory[addr+2] << 8) | s->memory[addr+3]);
}

static void setMem8(H8State *s, uint32_t addr, uint8_t value) {
    addr &= 0xFFFF;

    /* SCI3 SCR3 write — detect TE/RE edges for IR */
    if (addr == 0xFF9A) {
        uint8_t prev = s->memory[addr];
        s->memory[addr] = value;
        /* TE rising edge → TX start (IR layer handles this on 3DS) */
        /* TE falling edge → TX end */
        /* RE rising edge → flush + start RX */
        /* RE falling edge → stop RX */
        return;
    }

    /* SCI3 SSR3 write — read-1-then-write-0 flag clearing */
    if (addr == 0xFF9C) {
        uint8_t old = s->memory[addr];
        uint8_t clearable = (uint8_t)(~value) & s->sci3.lastReadSSR3 & 0xFCu;
        s->memory[addr] = (uint8_t)(((old & 0xFCu) & ~clearable) | (value & 0x03u));
        s->sci3.lastReadSSR3 = 0;
        return;
    }

    /* SCI3 TDR3 write — accept byte into shift register */
    if (addr == 0xFF9B && (s->memory[0xFF9A] & SCI3_TE)) {
        s->memory[addr] = value;
        s->sci3.txPending = value;
        s->sci3.txHasPending = true;
        s->memory[0xFF9C] &= ~(SCI3_TDRE | SCI3_TEND);
        s->sci3.txCountdown = 320;  /* 1 byte at 115200 baud */
        s->sci3.txIdleCountdown = 0;
        return;
    }

    s->memory[addr] = value;
}

static uint16_t getMem8(H8State *s, uint32_t addr) {
    addr &= 0xFFFF;
    uint8_t value = s->memory[addr];

    /* SCI3 SSR3 read — latch flags for read-1-then-write-0 */
    if (addr == 0xFF9C) {
        s->sci3.lastReadSSR3 = value;
    }

    /* SCI3 RDR3 read — clear RDRF, start next-byte countdown */
    if (addr == 0xFF9D) {
        s->memory[0xFF9C] &= ~SCI3_RDRF;
        if (s->sci3.rxPos < s->sci3.rxLen) {
            s->sci3.rxCountdown = 320;
        }
    }

    return value;
}

static void setMem16(H8State *s, uint32_t addr, uint16_t value) {
    addr &= 0xFFFF;
    s->memory[addr] = value >> 8;
    s->memory[addr + 1] = value & 0xFF;
}

static void setMem32(H8State *s, uint32_t addr, uint32_t value) {
    addr &= 0xFFFF;
    s->memory[addr]     = value >> 24;
    s->memory[addr + 1] = (value >> 16) & 0xFF;
    s->memory[addr + 2] = (value >> 8) & 0xFF;
    s->memory[addr + 3] = value & 0xFF;
}

/* ── LCD rendering ──────────────────────────────────────────────────────── */

void h8_render_lcd(H8State *s, uint32_t *videoBuffer) {
    int startRow;
    if (s->lcd.startLineActive) {
        startRow = s->lcd.displayStartLine & 0x7F;
    } else {
        startRow = s->lcd.currentBuffer ? 64 : 0;
        s->lcd.currentBuffer ^= 1;
    }

    for (int y = 0; y < H8_LCD_HEIGHT; y++) {
        int physRow = (startRow + y) & 0x7F;
        int yBit = physRow & 7;
        int yPage = physRow >> 3;
        int rowOff = yPage * H8_LCD_WIDTH * 2;  /* 2 bytes per column per page */
        uint8_t mask = (uint8_t)(1 << yBit);

        for (int x = 0; x < H8_LCD_WIDTH; x++) {
            int base = rowOff + 2 * x;
            uint8_t bit0 = (s->lcd.memory[base] & mask) >> yBit;
            uint8_t bit1 = (s->lcd.memory[base + 1] & mask) >> yBit;
            uint32_t palette[] = { H8_GRAY_0, H8_GRAY_1, H8_GRAY_2, H8_GRAY_3 };
            videoBuffer[y * H8_LCD_WIDTH + x] = palette[(bit0 << 1) | bit1];
        }
    }
}

/* ── Sub-clock tick (Timer B1 + Timer W) ────────────────────────────────── */

void h8_tick_subclock(H8State *s, int ticks) {
    for (int t = 0; t < ticks; t++) {
        /* Timer B1 */
        if (s->timerB.on && ((s->subClockCycles % 256) == 0)) {
            if (++(*s->timerB.TCB1) == 0) {
                *s->IRQ_IRR2 |= IRRTB1;
                *s->timerB.TCB1 = s->timerB.TLBvalue;
            }
        }

        /* Timer W */
        if (s->timerW.on) {
            if (*s->timerW.TMRW & CTS) {
                /* Determine clock divider from CKS bits */
                uint8_t cks = (*s->timerW.TCRW >> 4) & 0x07;
                uint8_t divider;
                switch (cks) {
                    case 4: divider = 1; break;   /* phi_w (32768 Hz) */
                    case 5: divider = 2; break;   /* phi_w/2 */
                    case 6: divider = 4; break;   /* phi_w/4 */
                    case 7: divider = 8; break;   /* phi_w/8 */
                    default: divider = 1; break;  /* system clock, approximate */
                }

                static uint8_t twDivCounter = 0;
                if (++twDivCounter >= divider) {
                    twDivCounter = 0;
                    setMem16(s, TCNT_ADDRESS, getMem16(s, TCNT_ADDRESS) + 1);
                }
            }

            /* Compare match: TCNT == GRA → audio event */
            if (getMem16(s, TCNT_ADDRESS) == getMem16(s, 0xF0F8)) {
                if (*s->timerW.TCRW & CCLR) {
                    setMem16(s, TCNT_ADDRESS, 0);
                }
                *s->timerW.TSRW |= 0x1;  /* IMFA */

                /* Fire audio callback */
                uint16_t gra = *s->timerW.GRA;
                uint8_t vol = s->memory[0xF7C6];  /* volume from RAM */
                if (s->audioCallback) {
                    s->audioCallback(gra, vol, true, s->callbackUserdata);
                }

                if (*s->timerW.TIERW & 0x1) {  /* IMIEA */
                    if (!s->flags.I) {
                        /* Push interrupt context */
                        if (s->interruptSaveDepth < H8_INT_SAVE_DEPTH) {
                            s->interruptSavedAddressStack[s->interruptSaveDepth] = s->pc;
                            s->interruptSavedFlagsStack[s->interruptSaveDepth] = s->flags;
                            s->interruptSaveDepth++;
                        }
                        s->interruptSavedAddress = s->pc;
                        s->interruptSavedFlags = s->flags;
                        s->flags.I = true;
                        s->pc = VECTOR_TIMER_W(s);
                        s->sleeping = false;
                    }
                }
            }
        }

        /* Update timer-on state */
        s->timerB.on = (*s->CKSTPR1 & TB1CKSTP) && (*s->timerB.TMB1 & TMB_COUNTING);
        s->timerW.on = (*s->CKSTPR2 & TWCKSTP) && (*s->timerW.TMRW & CTS);

        s->subClockCycles++;
    }
}

/* ── SCI3 baud-rate tick (byte timing) ──────────────────────────────────── */

void h8_tick_sci3(H8State *s) {
    /* TX byte countdown */
    if (s->sci3.txHasPending && s->sci3.txCountdown > 0) {
        if (--s->sci3.txCountdown == 0) {
            /* Byte has been "shifted out" — set TDRE */
            s->memory[0xFF9C] |= SCI3_TDRE;
            s->sci3.txHasPending = false;
            s->sci3.txIdleCountdown = 320;
        }
    }

    /* TX idle detector */
    if (!s->sci3.txHasPending && s->sci3.txIdleCountdown > 0) {
        if (--s->sci3.txIdleCountdown == 0) {
            s->memory[0xFF9C] |= SCI3_TEND;
        }
    }

    /* RX byte countdown */
    if (s->sci3.rxPos < s->sci3.rxLen && s->sci3.rxCountdown > 0) {
        if (--s->sci3.rxCountdown == 0) {
            /* Deliver next byte to RDR3 */
            s->memory[0xFF9D] = s->sci3.rxBuf[s->sci3.rxPos++];
            s->memory[0xFF9C] |= SCI3_RDRF;
            if (s->sci3.rxPos < s->sci3.rxLen) {
                s->sci3.rxCountdown = 320;
            }
        }
    }
}

/* ── Interrupt context save/restore ──────────────────────────────────────── */

static inline void interruptPushContext(H8State *s) {
    if (s->interruptSaveDepth < H8_INT_SAVE_DEPTH) {
        s->interruptSavedAddressStack[s->interruptSaveDepth] = s->pc;
        s->interruptSavedFlagsStack[s->interruptSaveDepth] = s->flags;
        s->interruptSaveDepth++;
    }
    s->interruptSavedAddress = s->pc;
    s->interruptSavedFlags = s->flags;
}

static inline void interruptPopContext(H8State *s) {
    if (s->interruptSaveDepth > 0) {
        s->interruptSaveDepth--;
        s->pc = s->interruptSavedAddressStack[s->interruptSaveDepth] - 2;
        s->flags = s->interruptSavedFlagsStack[s->interruptSaveDepth];
    } else {
        s->pc = s->interruptSavedAddress - 2;
        s->flags = s->interruptSavedFlags;
    }
    if (s->interruptSaveDepth > 0) {
        s->interruptSavedAddress = s->interruptSavedAddressStack[s->interruptSaveDepth - 1];
        s->interruptSavedFlags = s->interruptSavedFlagsStack[s->interruptSaveDepth - 1];
    }
}

/* ── Input queue ────────────────────────────────────────────────────────── */

static void pushInput(H8State *s, uint8_t key) {
    if (s->inputCount < 32) {
        s->inputBuf[s->inputTail] = key;
        s->inputTail = (s->inputTail + 1) % 32;
        s->inputCount++;
    }
}

static uint8_t popInput(H8State *s) {
    if (s->inputCount > 0) {
        uint8_t key = s->inputBuf[s->inputHead];
        s->inputHead = (s->inputHead + 1) % 32;
        s->inputCount--;
        return key;
    }
    return 0;
}

static bool inputEmpty(H8State *s) { return s->inputCount == 0; }

/* ── Branch condition evaluation ─────────────────────────────────────────── */

static bool checkCondition(H8Flags *f, uint8_t cc) {
    switch (cc) {
        case 0x0: return true;                          /* BRA (always) */
        case 0x1: return false;                         /* BRN (never) */
        case 0x2: return f->C || f->Z;                  /* BHI */
        case 0x3: return !f->C && !f->Z;                /* BLS */
        case 0x4: return !f->C;                         /* BCC/BHS */
        case 0x5: return f->C;                          /* BCS/BLO */
        case 0x6: return !f->Z;                         /* BNE */
        case 0x7: return f->Z;                          /* BEQ */
        case 0x8: return !f->V;                         /* BVC */
        case 0x9: return f->V;                          /* BVS */
        case 0xA: return !f->N;                         /* BPL */
        case 0xB: return f->N;                          /* BMI */
        case 0xC: return (f->N == f->V);                /* BGE */
        case 0xD: return (f->N != f->V);                /* BLT */
        case 0xE: return !f->Z && (f->N == f->V);      /* BGT */
        case 0xF: return f->Z || (f->N != f->V);       /* BLE */
        default: return false;
    }
}

/* ── Instruction cycle counts (first opcode byte) ──────────────────────── */

static const uint8_t h8_cycles[256] = {
    /* 0x00-0x0F */ 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    /* 0x10-0x1F */ 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    /* 0x20-0x2F */ 4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    /* 0x30-0x3F */ 4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    /* 0x40-0x4F */ 4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    /* 0x50-0x5F */ 12,12,2,2,8,6,10,2,6,4,6,8,8,6,6,8,
    /* 0x60-0x6F */ 2,2,2,2,2,2,2,2,4,4,6,6,6,6,6,6,
    /* 0x70-0x7F */ 2,2,2,2,2,2,2,2,10,4,6,2,8,8,6,6,
    /* 0x80-0x8F */ 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    /* 0x90-0x9F */ 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    /* 0xA0-0xAF */ 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    /* 0xB0-0xBF */ 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    /* 0xC0-0xCF */ 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    /* 0xD0-0xDF */ 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    /* 0xE0-0xEF */ 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    /* 0xF0-0xFF */ 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
};

/* ── Main instruction execution ─────────────────────────────────────────── */

int h8_step(H8State *s) {
    uint32_t cycles = 2;
    if (s->sleeping) return 0;

    /* ── ROM-specific hooks (original pwflash.rom, entry=0x02C4) ── */
    if (s->entry == 0x02C4) {
        if (s->pc == 0x336)  { s->pc += 4; return 0; }          /* Skip factory test */
        if (s->pc == 0x350)  { s->pc += 4; s->er[0] = (s->er[0] & ~0xFF); return 0; } /* Skip battery check */
        if (s->pc == 0x7700) { s->pc += 2; return 0; }          /* Skip accel SLEEP */
        if (s->pc == 0x9b84) {                                   /* Input hook */
            if (!inputEmpty(s))
                setMem8(s, 0xffde, popInput(s));
        }
    }

    /* ── Compiled ROM hooks (entry=0x3F0E) ── */
    if (s->entry == 0x3F0E) {
        if (s->pc >= 0x118E && s->pc < 0x1230) {
            /* Return true: pop PC from stack */
            uint16_t sp = (uint16_t)(s->er[7] & 0xFFFF);
            s->pc = (s->memory[sp] << 8) | s->memory[sp + 1];
            s->er[7] += 2;
            return 0;
        }
        if (s->pc == 0x10A8) {
            /* checkBatteryBelowLevel → return false */
            uint16_t sp = (uint16_t)(s->er[7] & 0xFFFF);
            s->pc = (s->memory[sp] << 8) | s->memory[sp + 1];
            s->er[7] += 2;
            return 0;
        }
        if (s->pc == 0x2ABE) {
            /* factoryTestPerformIfNeeded → skip */
            uint16_t sp = (uint16_t)(s->er[7] & 0xFFFF);
            s->pc = (s->memory[sp] << 8) | s->memory[sp + 1];
            s->er[7] += 2;
            return 0;
        }
        if (s->pc == 0x19D8) {
            if (!inputEmpty(s))
                setMem8(s, 0xffde, popInput(s));
        }
    }

    /* ── Fetch instruction ── */
    uint16_t *curInstr = (uint16_t *)(s->memory + s->pc);
    uint16_t ab = (curInstr[0] << 8) | (curInstr[0] >> 8);

    uint8_t a = ab >> 8;
    uint8_t aH = (a >> 4) & 0xF;
    uint8_t aL = a & 0xF;
    uint8_t b = ab & 0xFF;
    uint8_t bH = (b >> 4) & 0xF;
    uint8_t bL = b & 0xF;

    uint16_t cd = (curInstr[1] << 8) | (curInstr[1] >> 8);
    uint8_t c = cd >> 8;
    uint8_t cH = (c >> 4) & 0xF;
    uint8_t cL = c & 0xF;
    uint8_t d = cd & 0xFF;
    uint8_t dH = (d >> 4) & 0xF;
    uint8_t dL = d & 0xF;

    uint16_t ef = (curInstr[2] << 8) | (curInstr[2] >> 8);
    uint8_t eL = ef & 0xF;

    uint32_t cdef = (uint32_t)(cd << 16) | ef;

    cycles = h8_cycles[a];

    switch (a) {
    /* ════════════════════════════════════════════════════════════════════
     * 0x00-0x0F: NOP, register MOV/ADD/INC, CCR ops, 01-prefix instructions
     * ════════════════════════════════════════════════════════════════════ */
    case 0x00 ... 0x0F:
        switch (aL) {
        case 0x0: /* NOP */ break;

        case 0x1: /* 01-prefix instructions */
            switch (bH) {
            case 0x1: case 0x2: case 0x3: /* STM.L / LDM.L */
                if (c == 0x6D) {
                    uint8_t count = bH + 1;
                    bool isLdm = (dH & 0x8) != 0;
                    if (isLdm) {
                        for (int i = 0; i < count; i++) {
                            RegRef32 rd = getRegRef32(s, dL + i);
                            *rd.ptr = getMem32(s, s->er[7]);
                            s->er[7] += 4;
                        }
                    } else {
                        for (int i = 0; i < count; i++) {
                            s->er[7] -= 4;
                            RegRef32 rs = getRegRef32(s, dL - i);
                            setMem32(s, s->er[7], *rs.ptr);
                        }
                    }
                    s->pc += 4;
                }
                break;

            case 0x0:
                if (c == 0x6B) {
                    switch (dH) {
                    case 0x0: { /* MOV.l @aa:16, ERd */
                        uint32_t addr = ef | 0xFF0000;
                        RegRef32 rd = getRegRef32(s, dL);
                        uint32_t val = getMem32(s, addr);
                        setFlagsMOV(&s->flags, val, 32);
                        *rd.ptr = val;
                        s->pc += 4;
                    } break;
                    case 0x8: { /* MOV.l ERs, @aa:16 */
                        uint32_t addr = cdef & 0xFFFF | 0xFF0000;
                        RegRef32 rs = getRegRef32(s, dL);
                        setFlagsMOV(&s->flags, *rs.ptr, 32);
                        setMem32(s, addr, *rs.ptr);
                        s->pc += 4;
                    } break;
                    default: goto unimpl;
                    }
                } else if (c == 0x6D) {
                    /* MOV.l @ERs+, ERd / MOV.l ERs, @-ERd */
                    if (!(dH & 8)) {
                        /* @ERs+, ERd */
                        RegRef32 rs = getRegRef32(s, dH);
                        RegRef32 rd = getRegRef32(s, dL);
                        uint32_t val = getMem32(s, *rs.ptr);
                        *rs.ptr += 4;
                        setFlagsMOV(&s->flags, val, 32);
                        *rd.ptr = val;
                    } else {
                        /* ERs, @-ERd */
                        RegRef32 rs = getRegRef32(s, dL);
                        RegRef32 rd = getRegRef32(s, dH);
                        *rd.ptr -= 4;
                        setFlagsMOV(&s->flags, *rs.ptr, 32);
                        setMem32(s, *rd.ptr, *rs.ptr);
                    }
                    s->pc += 2;
                } else if (c == 0x6F) {
                    /* MOV.l @(d:16, ERs), ERd / MOV.l ERs, @(d:16, ERd) */
                    uint32_t disp = ef;
                    if (disp & 0x8000) disp |= 0xFFFF0000;
                    if (!(dH & 8)) {
                        RegRef32 rs = getRegRef32(s, dH);
                        RegRef32 rd = getRegRef32(s, dL);
                        uint32_t val = getMem32(s, *rs.ptr + disp);
                        *rd.ptr = val;
                        setFlagsMOV(&s->flags, val, 32);
                    } else {
                        RegRef32 rs = getRegRef32(s, dL);
                        RegRef32 rd = getRegRef32(s, dH);
                        setFlagsMOV(&s->flags, *rs.ptr, 32);
                        setMem32(s, *rd.ptr + disp, *rs.ptr);
                    }
                    s->pc += 4;
                } else if (c == 0x69) {
                    /* MOV.l @ERs, ERd / MOV.l ERs, @ERd */
                    if (!(dH & 8)) {
                        RegRef32 rs = getRegRef32(s, dH);
                        RegRef32 rd = getRegRef32(s, dL);
                        uint32_t val = getMem32(s, *rs.ptr);
                        setFlagsMOV(&s->flags, val, 32);
                        *rd.ptr = val;
                    } else {
                        RegRef32 rs = getRegRef32(s, dL);
                        RegRef32 rd = getRegRef32(s, dH);
                        setFlagsMOV(&s->flags, *rs.ptr, 32);
                        setMem32(s, *rd.ptr, *rs.ptr);
                    }
                    s->pc += 2;
                } else if (c == 0x66) {
                    /* AND.l ERs, ERd */
                    RegRef32 rs = getRegRef32(s, dH);
                    RegRef32 rd = getRegRef32(s, dL);
                    uint32_t val = *rs.ptr & *rd.ptr;
                    setFlagsMOV(&s->flags, val, 32);
                    *rd.ptr = val;
                    s->pc += 2;
                } else if (c == 0x64) {
                    /* OR.l ERs, ERd */
                    RegRef32 rs = getRegRef32(s, dH);
                    RegRef32 rd = getRegRef32(s, dL);
                    uint32_t val = *rs.ptr | *rd.ptr;
                    setFlagsMOV(&s->flags, val, 32);
                    *rd.ptr = val;
                    s->pc += 2;
                } else if (c == 0x65) {
                    /* XOR.l ERs, ERd */
                    RegRef32 rs = getRegRef32(s, dH);
                    RegRef32 rd = getRegRef32(s, dL);
                    uint32_t val = *rs.ptr ^ *rd.ptr;
                    setFlagsMOV(&s->flags, val, 32);
                    *rd.ptr = val;
                    s->pc += 2;
                }
                break;

            case 0x4: /* LDC/STC prefix */
                s->pc += 2;
                break;

            case 0x8: /* SLEEP */
                s->sleeping = true;
                break;

            case 0xC: /* MULXS */
                if (bL == 0x0 && cH == 0x6) {
                    if (cL == 0x0) {
                        /* MULXS.B Rs, Rd */
                        RegRef8 rs = getRegRef8(s, dH);
                        RegRef16 rd = getRegRef16(s, dL);
                        int8_t lo = *rd.ptr & 0xFF;
                        *rd.ptr = (uint16_t)((int16_t)*rs.ptr * (int16_t)lo);
                        s->flags.Z = *rd.ptr == 0;
                        s->flags.N = *rd.ptr & 0x8000;
                        s->pc += 2;
                    } else if (cL == 0x2) {
                        /* MULXS.W Rs, ERd */
                        RegRef16 rs = getRegRef16(s, dH);
                        RegRef32 rd = getRegRef32(s, dL);
                        int16_t lo = *rd.ptr & 0xFFFF;
                        *rd.ptr = (uint32_t)((int32_t)*rs.ptr * (int32_t)lo);
                        s->flags.Z = *rd.ptr == 0;
                        s->flags.N = *rd.ptr & 0x80000000;
                        s->pc += 2;
                    }
                }
                break;

            case 0xD: /* DIVXS */
                if (bL == 0x0 && cH == 0x6) {
                    if (cL == 0x1) {
                        /* DIVXS.B Rs, Rd */
                        RegRef8 rs = getRegRef8(s, dH);
                        RegRef16 rd = getRegRef16(s, dL);
                        if (*rs.ptr != 0) {
                            int8_t q = (int16_t)*rd.ptr / (int8_t)*rs.ptr;
                            int8_t r = (int16_t)*rd.ptr % (int8_t)*rs.ptr;
                            *rd.ptr = (uint16_t)((r << 8) | q);
                        }
                        s->flags.Z = *rs.ptr == 0;
                        s->flags.N = ((int16_t)*rd.ptr >> 8) < 0;
                        s->pc += 2;
                    } else if (cL == 0x3) {
                        /* DIVXS.W Rs, ERd */
                        RegRef16 rs = getRegRef16(s, dH);
                        RegRef32 rd = getRegRef32(s, dL);
                        if (*rs.ptr != 0) {
                            int16_t q = (int32_t)*rd.ptr / (int16_t)*rs.ptr;
                            int16_t r = (int32_t)*rd.ptr % (int16_t)*rs.ptr;
                            *rd.ptr = (uint32_t)((r << 16) | q);
                        }
                        s->flags.Z = *rs.ptr == 0;
                        s->flags.N = *rd.ptr & 0x80000000;
                        s->pc += 2;
                    }
                }
                break;

            case 0xF: /* 01 F0 prefix — OR.L/XOR.L/AND.L ERs,ERd */
                if (bL == 0x0 && cH == 0x6) {
                    RegRef32 rs = getRegRef32(s, dH);
                    RegRef32 rd = getRegRef32(s, dL);
                    uint32_t val;
                    switch (cL) {
                    case 0x4: val = *rs.ptr | *rd.ptr; break;
                    case 0x5: val = *rs.ptr ^ *rd.ptr; break;
                    case 0x6: val = *rs.ptr & *rd.ptr; break;
                    default: goto unimpl;
                    }
                    setFlagsMOV(&s->flags, val, 32);
                    *rd.ptr = val;
                    s->pc += 2;
                }
                break;

            default: goto unimpl;
            }
            break;

        case 0x2: { /* STC.B CCR, Rd */
            RegRef8 rd = getRegRef8(s, bL);
            *rd.ptr = packFlags(&s->flags);
        } break;

        case 0x3: { /* LDC.B Rs, CCR */
            RegRef8 rs = getRegRef8(s, bL);
            setFlags(&s->flags, *rs.ptr);
        } break;

        case 0x4: /* ORC #xx:8, CCR */
            setFlags(&s->flags, packFlags(&s->flags) | b);
            break;

        case 0x5: /* XORC #xx:8, CCR */
            setFlags(&s->flags, packFlags(&s->flags) ^ b);
            break;

        case 0x6: /* ANDC #xx:8, CCR */
            setFlags(&s->flags, packFlags(&s->flags) & b);
            break;

        case 0x7: /* LDC.B #xx:8, CCR */
            setFlags(&s->flags, b);
            break;

        case 0x8: { /* ADD.B Rs, Rd */
            RegRef8 rs = getRegRef8(s, bH);
            RegRef8 rd = getRegRef8(s, bL);
            setFlagsADD(&s->flags, *rd.ptr, *rs.ptr, 8);
            *rd.ptr += *rs.ptr;
        } break;

        case 0x9: { /* ADD.W Rs, Rd */
            RegRef16 rs = getRegRef16(s, bH);
            RegRef16 rd = getRegRef16(s, bL);
            setFlagsADD(&s->flags, *rd.ptr, *rs.ptr, 16);
            *rd.ptr += *rs.ptr;
        } break;

        case 0xA:
            switch (bH) {
            case 0x0: { /* INC.B Rd */
                RegRef8 rd = getRegRef8(s, bL);
                setFlagsINC(&s->flags, *rd.ptr, 1, 8);
                *rd.ptr += 1;
            } break;
            case 0x8 ... 0xF: { /* ADD.l ERs, ERd */
                RegRef32 rs = getRegRef32(s, bH);
                RegRef32 rd = getRegRef32(s, bL);
                setFlagsADD(&s->flags, *rd.ptr, *rs.ptr, 32);
                *rd.ptr += *rs.ptr;
            } break;
            default: goto unimpl;
            }
            break;

        case 0xB: /* ADDS / INC */
            switch (bH) {
            case 0x0: { RegRef32 rd = getRegRef32(s, bL); *rd.ptr += 1; } break;
            case 0x8: { RegRef32 rd = getRegRef32(s, bL); *rd.ptr += 2; } break;
            case 0x9: { RegRef32 rd = getRegRef32(s, bL); *rd.ptr += 4; } break;
            case 0x5: { /* INC.w #1 */
                RegRef16 rd = getRegRef16(s, bL);
                setFlagsINC(&s->flags, *rd.ptr, 1, 16);
                *rd.ptr += 1;
            } break;
            case 0x7: { /* INC.l #1 */
                RegRef32 rd = getRegRef32(s, bL);
                setFlagsINC(&s->flags, *rd.ptr, 1, 32);
                *rd.ptr += 1;
            } break;
            case 0xD: { /* INC.w #2 */
                RegRef16 rd = getRegRef16(s, bL);
                setFlagsINC(&s->flags, *rd.ptr, 2, 16);
                *rd.ptr += 2;
            } break;
            case 0xF: { /* INC.l #2 */
                RegRef32 rd = getRegRef32(s, bL);
                setFlagsINC(&s->flags, *rd.ptr, 2, 32);
                *rd.ptr += 2;
            } break;
            default: goto unimpl;
            }
            break;

        case 0xC: { /* MOV.B Rs, Rd */
            RegRef8 rs = getRegRef8(s, bH);
            RegRef8 rd = getRegRef8(s, bL);
            setFlagsMOV(&s->flags, *rs.ptr, 8);
            *rd.ptr = *rs.ptr;
        } break;

        case 0xD: { /* MOV.W Rs, Rd */
            RegRef16 rs = getRegRef16(s, bH);
            RegRef16 rd = getRegRef16(s, bL);
            setFlagsMOV(&s->flags, *rs.ptr, 16);
            *rd.ptr = *rs.ptr;
        } break;

        case 0xE: goto unimpl; /* ADDX — unused in PokéWalker */

        case 0xF:
            if (bH >= 0x8) {
                /* MOV.l ERs, ERd */
                RegRef32 rs = getRegRef32(s, bH);
                RegRef32 rd = getRegRef32(s, bL);
                setFlagsMOV(&s->flags, *rs.ptr, 32);
                *rd.ptr = *rs.ptr;
            } else {
                goto unimpl;
            }
            break;

        default: goto unimpl;
        }
        break;

    /* ════════════════════════════════════════════════════════════════════
     * 0x10-0x1F: Shifts, rotates, SUB/DEC/SUBS/CMP, bit ops
     * ════════════════════════════════════════════════════════════════════ */
    case 0x10 ... 0x1F:
        switch (aL) {
        case 0x0: /* Shifts left */
            switch (bH) {
            case 0x0: { /* SHLL.b Rd */
                RegRef8 rd = getRegRef8(s, bL);
                s->flags.C = *rd.ptr & 0x80;
                *rd.ptr <<= 1;
                setFlagsMOV(&s->flags, *rd.ptr, 8);
            } break;
            case 0x1: { /* SHLL.w Rd */
                RegRef16 rd = getRegRef16(s, bL);
                s->flags.C = *rd.ptr & 0x8000;
                *rd.ptr <<= 1;
                setFlagsMOV(&s->flags, *rd.ptr, 16);
            } break;
            case 0x3: { /* SHLL.l Rd */
                RegRef32 rd = getRegRef32(s, bL);
                s->flags.C = *rd.ptr & 0x80000000;
                *rd.ptr <<= 1;
                setFlagsMOV(&s->flags, *rd.ptr, 32);
            } break;
            case 0x8: { /* SHAL.b Rd */
                RegRef8 rd = getRegRef8(s, bL);
                s->flags.C = *rd.ptr & 0x80;
                *rd.ptr <<= 1;
                setFlagsMOV(&s->flags, *rd.ptr, 8);
                s->flags.V = s->flags.C && !(*rd.ptr & 0x80);
            } break;
            case 0x9: { /* SHAL.w Rd */
                RegRef16 rd = getRegRef16(s, bL);
                s->flags.C = *rd.ptr & 0x8000;
                *rd.ptr <<= 1;
                setFlagsMOV(&s->flags, *rd.ptr, 16);
                s->flags.V = s->flags.C && !(*rd.ptr & 0x8000);
            } break;
            case 0xB: { /* SHAL.l Rd */
                RegRef32 rd = getRegRef32(s, bL);
                s->flags.C = *rd.ptr & 0x80000000;
                *rd.ptr <<= 1;
                setFlagsMOV(&s->flags, *rd.ptr, 32);
                s->flags.V = s->flags.C && !(*rd.ptr & 0x80000000);
            } break;
            default: goto unimpl;
            }
            break;

        case 0x1: /* Shifts right */
            switch (bH) {
            case 0x0: { /* SHLR.b Rd */
                RegRef8 rd = getRegRef8(s, bL);
                s->flags.C = *rd.ptr & 0x1;
                *rd.ptr >>= 1;
                setFlagsMOV(&s->flags, *rd.ptr, 8);
            } break;
            case 0x1: { /* SHLR.w Rd */
                RegRef16 rd = getRegRef16(s, bL);
                s->flags.C = *rd.ptr & 0x1;
                *rd.ptr >>= 1;
                setFlagsMOV(&s->flags, *rd.ptr, 16);
            } break;
            case 0x3: { /* SHLR.l Rd */
                RegRef32 rd = getRegRef32(s, bL);
                s->flags.C = *rd.ptr & 0x1;
                *rd.ptr >>= 1;
                setFlagsMOV(&s->flags, *rd.ptr, 32);
            } break;
            case 0x9: { /* SHAR.w Rd */
                RegRef16 rd = getRegRef16(s, bL);
                s->flags.C = *rd.ptr & 0x1;
                *rd.ptr = (*rd.ptr >> 1) | (*rd.ptr & 0x8000);
                setFlagsMOV(&s->flags, *rd.ptr, 16);
            } break;
            case 0xB: { /* SHAR.l Rd */
                RegRef32 rd = getRegRef32(s, bL);
                s->flags.C = *rd.ptr & 0x1;
                *rd.ptr = (*rd.ptr >> 1) | (*rd.ptr & 0x80000000);
                setFlagsMOV(&s->flags, *rd.ptr, 32);
            } break;
            default: goto unimpl;
            }
            break;

        case 0x2: /* Rotate left */
            switch (bH) {
            case 0x0: { /* ROTXL.b Rd */
                RegRef8 rd = getRegRef8(s, bL);
                bool oldC = s->flags.C;
                s->flags.C = *rd.ptr & 0x80;
                *rd.ptr = (uint8_t)((*rd.ptr << 1) | oldC);
                setFlagsMOV(&s->flags, *rd.ptr, 8);
            } break;
            case 0x1: { /* ROTXL.w Rd */
                RegRef16 rd = getRegRef16(s, bL);
                bool oldC = s->flags.C;
                s->flags.C = *rd.ptr & 0x8000;
                *rd.ptr = (uint16_t)((*rd.ptr << 1) | oldC);
                setFlagsMOV(&s->flags, *rd.ptr, 16);
            } break;
            case 0x3: { /* ROTXL.l Rd */
                RegRef32 rd = getRegRef32(s, bL);
                bool oldC = s->flags.C;
                s->flags.C = *rd.ptr & 0x80000000;
                *rd.ptr = (*rd.ptr << 1) | oldC;
                setFlagsMOV(&s->flags, *rd.ptr, 32);
            } break;
            case 0x8: { /* ROTL.b Rd */
                RegRef8 rd = getRegRef8(s, bL);
                s->flags.C = *rd.ptr & 0x80;
                *rd.ptr = (uint8_t)((*rd.ptr << 1) | s->flags.C);
                setFlagsMOV(&s->flags, *rd.ptr, 8);
            } break;
            case 0x9: { /* ROTL.w Rd */
                RegRef16 rd = getRegRef16(s, bL);
                s->flags.C = *rd.ptr & 0x8000;
                *rd.ptr = (uint16_t)((*rd.ptr << 1) | s->flags.C);
                setFlagsMOV(&s->flags, *rd.ptr, 16);
            } break;
            case 0xB: { /* ROTL.l Rd */
                RegRef32 rd = getRegRef32(s, bL);
                s->flags.C = *rd.ptr & 0x80000000;
                *rd.ptr = (*rd.ptr << 1) | s->flags.C;
                setFlagsMOV(&s->flags, *rd.ptr, 32);
            } break;
            default: goto unimpl;
            }
            break;

        case 0x3: /* Rotate right */
            switch (bH) {
            case 0x0: { /* ROTXR.b Rd */
                RegRef8 rd = getRegRef8(s, bL);
                bool oldC = s->flags.C;
                s->flags.C = *rd.ptr & 0x1;
                *rd.ptr = (uint8_t)((*rd.ptr >> 1) | (oldC << 7));
                setFlagsMOV(&s->flags, *rd.ptr, 8);
            } break;
            case 0x1: { /* ROTXR.w Rd */
                RegRef16 rd = getRegRef16(s, bL);
                bool oldC = s->flags.C;
                s->flags.C = *rd.ptr & 0x1;
                *rd.ptr = (uint16_t)((*rd.ptr >> 1) | (oldC << 15));
                setFlagsMOV(&s->flags, *rd.ptr, 16);
            } break;
            case 0x3: { /* ROTXR.l Rd */
                RegRef32 rd = getRegRef32(s, bL);
                bool oldC = s->flags.C;
                s->flags.C = *rd.ptr & 0x1;
                *rd.ptr = (*rd.ptr >> 1) | ((uint32_t)oldC << 31);
                setFlagsMOV(&s->flags, *rd.ptr, 32);
            } break;
            case 0x8: { /* ROTR.b Rd */
                RegRef8 rd = getRegRef8(s, bL);
                uint8_t bit0 = *rd.ptr & 0x1;
                s->flags.C = bit0;
                *rd.ptr = (uint8_t)((*rd.ptr >> 1) | (bit0 << 7));
                setFlagsMOV(&s->flags, *rd.ptr, 8);
            } break;
            case 0x9: { /* ROTR.w Rd */
                RegRef16 rd = getRegRef16(s, bL);
                uint16_t bit0 = *rd.ptr & 0x1;
                s->flags.C = bit0;
                *rd.ptr = (uint16_t)((*rd.ptr >> 1) | (bit0 << 15));
                setFlagsMOV(&s->flags, *rd.ptr, 16);
            } break;
            case 0xB: { /* ROTR.l Rd */
                RegRef32 rd = getRegRef32(s, bL);
                uint32_t bit0 = *rd.ptr & 0x1;
                s->flags.C = bit0;
                *rd.ptr = (*rd.ptr >> 1) | (bit0 << 31);
                setFlagsMOV(&s->flags, *rd.ptr, 32);
            } break;
            default: goto unimpl;
            }
            break;

        case 0x4: { /* OR.B Rs, Rd */
            RegRef8 rs = getRegRef8(s, bH);
            RegRef8 rd = getRegRef8(s, bL);
            uint8_t val = *rs.ptr | *rd.ptr;
            setFlagsMOV(&s->flags, val, 8);
            *rd.ptr = val;
        } break;

        case 0x5: { /* XOR.B Rs, Rd */
            RegRef8 rs = getRegRef8(s, bH);
            RegRef8 rd = getRegRef8(s, bL);
            uint8_t val = *rs.ptr ^ *rd.ptr;
            setFlagsMOV(&s->flags, val, 8);
            *rd.ptr = val;
        } break;

        case 0x6: { /* AND.B Rs, Rd */
            RegRef8 rs = getRegRef8(s, bH);
            RegRef8 rd = getRegRef8(s, bL);
            uint8_t val = *rs.ptr & *rd.ptr;
            setFlagsMOV(&s->flags, val, 8);
            *rd.ptr = val;
        } break;

        case 0x7: /* Misc: NOT, EXTU, EXTS, NEG */
            switch (bH) {
            case 0x0: { /* NOT.b Rd */
                RegRef8 rd = getRegRef8(s, bL);
                *rd.ptr = ~*rd.ptr;
                setFlagsMOV(&s->flags, *rd.ptr, 8);
            } break;
            case 0x1: { /* NOT.w Rd */
                RegRef16 rd = getRegRef16(s, bL);
                *rd.ptr = ~*rd.ptr;
                setFlagsMOV(&s->flags, *rd.ptr, 16);
            } break;
            case 0x5: { /* EXTU.w Rd */
                RegRef16 rd = getRegRef16(s, bL);
                *rd.ptr &= 0x00FF;
                setFlagsMOV(&s->flags, *rd.ptr, 16);
            } break;
            case 0x7: { /* EXTU.l Rd */
                RegRef32 rd = getRegRef32(s, bL);
                *rd.ptr &= 0x0000FFFF;
                setFlagsMOV(&s->flags, *rd.ptr, 32);
            } break;
            case 0x8: { /* NEG.b Rd */
                RegRef8 rd = getRegRef8(s, bL);
                setFlagsSUB(&s->flags, 0, *rd.ptr, 8);
                if (*rd.ptr != 0x80) *rd.ptr = (uint8_t)(-(int8_t)*rd.ptr);
            } break;
            case 0x9: { /* NEG.w Rd */
                RegRef16 rd = getRegRef16(s, bL);
                setFlagsSUB(&s->flags, 0, *rd.ptr, 16);
                if (*rd.ptr != 0x8000) *rd.ptr = (uint16_t)(-(int16_t)*rd.ptr);
            } break;
            case 0xD: { /* EXTS.w Rd */
                RegRef16 rd = getRegRef16(s, bL);
                if (*rd.ptr & 0x80) *rd.ptr = (*rd.ptr & 0xFF) | 0xFF00;
                else *rd.ptr &= 0xFF;
                setFlagsMOV(&s->flags, *rd.ptr, 16);
            } break;
            case 0xF: { /* EXTS.l Rd */
                RegRef32 rd = getRegRef32(s, bL);
                if (*rd.ptr & 0x8000) *rd.ptr = (*rd.ptr & 0xFFFF) | 0xFFFF0000;
                else *rd.ptr &= 0xFFFF;
                setFlagsMOV(&s->flags, *rd.ptr, 32);
            } break;
            default: goto unimpl;
            }
            break;

        case 0x8: { /* SUB.B Rs, Rd */
            RegRef8 rs = getRegRef8(s, bH);
            RegRef8 rd = getRegRef8(s, bL);
            setFlagsSUB(&s->flags, *rd.ptr, *rs.ptr, 8);
            *rd.ptr -= *rs.ptr;
        } break;

        case 0x9: { /* SUB.W Rs, Rd */
            RegRef16 rs = getRegRef16(s, bH);
            RegRef16 rd = getRegRef16(s, bL);
            setFlagsSUB(&s->flags, *rd.ptr, *rs.ptr, 16);
            *rd.ptr -= *rs.ptr;
        } break;

        case 0xA:
            switch (bH) {
            case 0x0: { /* DEC.b Rd */
                RegRef8 rd = getRegRef8(s, bL);
                setFlagsINC(&s->flags, *rd.ptr, (uint32_t)-1, 8);
                *rd.ptr -= 1;
            } break;
            case 0x8 ... 0xF: { /* SUB.l ERs, ERd */
                RegRef32 rs = getRegRef32(s, bH);
                RegRef32 rd = getRegRef32(s, bL);
                setFlagsSUB(&s->flags, *rd.ptr, *rs.ptr, 32);
                *rd.ptr -= *rs.ptr;
            } break;
            default: goto unimpl;
            }
            break;

        case 0xB: /* SUBS / DEC */
            switch (bH) {
            case 0x0: { RegRef32 rd = getRegRef32(s, bL); *rd.ptr -= 1; } break;
            case 0x8: { RegRef32 rd = getRegRef32(s, bL); *rd.ptr -= 2; } break;
            case 0x9: { RegRef32 rd = getRegRef32(s, bL); *rd.ptr -= 4; } break;
            case 0x5: { /* DEC.w #1 */
                RegRef16 rd = getRegRef16(s, bL);
                setFlagsINC(&s->flags, *rd.ptr, (uint32_t)-1, 16);
                *rd.ptr -= 1;
            } break;
            case 0x7: { /* DEC.l #1 */
                RegRef32 rd = getRegRef32(s, bL);
                setFlagsINC(&s->flags, *rd.ptr, (uint32_t)-1, 32);
                *rd.ptr -= 1;
            } break;
            case 0xD: { /* DEC.w #2 */
                RegRef16 rd = getRegRef16(s, bL);
                setFlagsINC(&s->flags, *rd.ptr, (uint32_t)-2, 16);
                *rd.ptr -= 2;
            } break;
            case 0xF: { /* DEC.l #2 */
                RegRef32 rd = getRegRef32(s, bL);
                setFlagsINC(&s->flags, *rd.ptr, (uint32_t)-2, 32);
                *rd.ptr -= 2;
            } break;
            default: goto unimpl;
            }
            break;

        case 0xC: { /* CMP.B Rs, Rd */
            RegRef8 rs = getRegRef8(s, bH);
            RegRef8 rd = getRegRef8(s, bL);
            setFlagsSUB(&s->flags, *rd.ptr, *rs.ptr, 8);
        } break;

        case 0xD: { /* CMP.W Rs, Rd */
            RegRef16 rs = getRegRef16(s, bH);
            RegRef16 rd = getRegRef16(s, bL);
            setFlagsSUB(&s->flags, *rd.ptr, *rs.ptr, 16);
        } break;

        case 0xE: { /* SUBX Rs, Rd */
            RegRef8 rs = getRegRef8(s, bH);
            RegRef8 rd = getRegRef8(s, bL);
            setFlagsSUB(&s->flags, *rd.ptr, *rs.ptr + s->flags.C, 8);
            *rd.ptr -= *rs.ptr;
            *rd.ptr -= s->flags.C;
        } break;

        case 0xF:
            if (bH >= 0x8) {
                /* CMP.l ERs, ERd */
                RegRef32 rs = getRegRef32(s, bH);
                RegRef32 rd = getRegRef32(s, bL);
                setFlagsSUB(&s->flags, *rd.ptr, *rs.ptr, 32);
            } else {
                goto unimpl;
            }
            break;

        default: goto unimpl;
        }
        break;

    /* ════════════════════════════════════════════════════════════════════
     * 0x20-0x2F: MOV.B @aa:8, Rd (direct addressing, zero-page high)
     * ════════════════════════════════════════════════════════════════════ */
    case 0x20 ... 0x2F: {
        RegRef8 rd = getRegRef8(s, aL);
        uint16_t addr = (uint16_t)(b << 8) | 0xFF00;
        uint8_t val = s->memory[addr & 0xFFFF];
        setFlagsMOV(&s->flags, val, 8);
        *rd.ptr = val;
        s->pc += 2;
    } break;

    /* ════════════════════════════════════════════════════════════════════
     * 0x30-0x3F: MOV.B Rs, @aa:8
     * ════════════════════════════════════════════════════════════════════ */
    case 0x30 ... 0x3F: {
        RegRef8 rs = getRegRef8(s, aL);
        uint16_t addr = (uint16_t)(b << 8) | 0xFF00;
        setFlagsMOV(&s->flags, *rs.ptr, 8);
        s->memory[addr & 0xFFFF] = *rs.ptr;
        s->pc += 2;
    } break;

    /* ════════════════════════════════════════════════════════════════════
     * 0x40-0x4F: Bcc d:8 (conditional branch, 8-bit signed offset)
     * ════════════════════════════════════════════════════════════════════ */
    case 0x40 ... 0x4F: {
        int8_t offset = (int8_t)b;
        if (checkCondition(&s->flags, aL)) {
            s->pc = (uint16_t)(s->pc + 2 + offset * 2);
        }
    } break;

    /* ════════════════════════════════════════════════════════════════════
     * 0x50-0x5F: Multi-purpose prefix
     * ════════════════════════════════════════════════════════════════════ */
    case 0x50:
        /* MULXU.B — unused in PokéWalker */
        goto unimpl;

    case 0x51:
        /* DIVXU.B — unused */
        goto unimpl;

    case 0x54: { /* RTS */
        uint16_t sp = (uint16_t)(s->er[7] & 0xFFFF);
        s->pc = (s->memory[sp] << 8) | s->memory[sp + 1];
        s->er[7] += 2;
    } break;

    case 0x55: { /* BSR d:8 */
        int8_t offset = (int8_t)b;
        s->er[7] -= 2;
        uint16_t sp = (uint16_t)(s->er[7] & 0xFFFF);
        s->memory[sp] = (s->pc + 2) >> 8;
        s->memory[sp + 1] = (s->pc + 2) & 0xFF;
        s->pc = (uint16_t)(s->pc + 2 + offset * 2);
    } break;

    case 0x56: { /* RTE */
        interruptPopContext(s);
        s->pc += 2;
    } break;

    case 0x58: { /* Bcc d:16 */
        uint16_t offset16 = cd;
        int16_t offset = (int16_t)offset16;
        if (checkCondition(&s->flags, bL)) {
            s->pc = (uint16_t)(s->pc + 4 + offset * 2);
        } else {
            s->pc += 4;
        }
    } break;

    case 0x59: { /* JMP @ERn */
        RegRef32 rs = getRegRef32(s, bL);
        s->pc = (uint16_t)(*rs.ptr);
    } break;

    case 0x5A: { /* JMP @aa:16 */
        s->pc = cd;
    } break;

    case 0x5C: { /* BSR d:16 */
        uint16_t offset16 = cd;
        int16_t offset = (int16_t)offset16;
        s->er[7] -= 2;
        uint16_t sp = (uint16_t)(s->er[7] & 0xFFFF);
        s->memory[sp] = (s->pc + 4) >> 8;
        s->memory[sp + 1] = (s->pc + 4) & 0xFF;
        s->pc = (uint16_t)(s->pc + 4 + offset * 2);
    } break;

    case 0x5D: { /* JSR @ERn */
        RegRef32 rs = getRegRef32(s, bL);
        s->er[7] -= 2;
        uint16_t sp = (uint16_t)(s->er[7] & 0xFFFF);
        s->memory[sp] = (s->pc + 2) >> 8;
        s->memory[sp + 1] = (s->pc + 2) & 0xFF;
        s->pc = (uint16_t)(*rs.ptr);
    } break;

    case 0x5E: { /* JSR @aa:16 */
        s->er[7] -= 2;
        uint16_t sp = (uint16_t)(s->er[7] & 0xFFFF);
        s->memory[sp] = (s->pc + 4) >> 8;
        s->memory[sp + 1] = (s->pc + 4) & 0xFF;
        s->pc = cd;
    } break;

    /* ════════════════════════════════════════════════════════════════════
     * 0x60-0x67: BIT ops (BSET/BCLR/BTST/BIAND etc.) — register form
     * ════════════════════════════════════════════════════════════════════ */
    case 0x60 ... 0x67: {
        /* BIT operation: bit number in aH, Rn in bL */
        uint8_t bitNum = aH;
        RegRef8 rd = getRegRef8(s, bL);
        switch (bH) {
        case 0x0: /* AND.B — not a bit op, just AND */
            break;
        case 0x1: /* BSET */
            *rd.ptr |= (1 << bitNum);
            break;
        case 0x2: /* BCLR */
            *rd.ptr &= ~(1 << bitNum);
            break;
        case 0x3: /* BTST — test bit, set Z if clear */
            s->flags.Z = !(*rd.ptr & (1 << bitNum));
            break;
        default: break;
        }
    } break;

    /* ════════════════════════════════════════════════════════════════════
     * 0x68-0x6F: MOV.B/W.L @ERs+/@ERn/@(d:16,ERn)
     * ════════════════════════════════════════════════════════════════════ */
    case 0x68: { /* MOV.B @ERs+, Rd / MOV.B Rs, @-ERd */
        if (!(bH & 8)) {
            RegRef32 rs = getRegRef32(s, bH);
            RegRef8 rd = getRegRef8(s, bL);
            uint8_t val = s->memory[(*rs.ptr) & 0xFFFF];
            *rs.ptr += 1;
            setFlagsMOV(&s->flags, val, 8);
            *rd.ptr = val;
        } else {
            RegRef8 rs = getRegRef8(s, bL);
            RegRef32 rd = getRegRef32(s, bH);
            *rd.ptr -= 1;
            setFlagsMOV(&s->flags, *rs.ptr, 8);
            s->memory[(*rd.ptr) & 0xFFFF] = *rs.ptr;
        }
    } break;

    case 0x69: { /* MOV.W @ERs, Rd / MOV.W Rs, @ERd */
        if (!(bH & 8)) {
            RegRef32 rs = getRegRef32(s, bH);
            RegRef16 rd = getRegRef16(s, bL);
            uint16_t val = getMem16(s, *rs.ptr);
            setFlagsMOV(&s->flags, val, 16);
            *rd.ptr = val;
        } else {
            RegRef16 rs = getRegRef16(s, bL);
            RegRef32 rd = getRegRef32(s, bH);
            setFlagsMOV(&s->flags, *rs.ptr, 16);
            setMem16(s, *rd.ptr, *rs.ptr);
        }
    } break;

    case 0x6A: { /* MOV.B @(d:8,ERs), Rd / MOV.B Rs, @(d:8,ERd) */
        int8_t disp = (int8_t)b;
        if (!(bH & 8)) {
            RegRef32 rs = getRegRef32(s, bH);
            RegRef8 rd = getRegRef8(s, bL);
            uint8_t val = s->memory[(*rs.ptr + disp) & 0xFFFF];
            setFlagsMOV(&s->flags, val, 8);
            *rd.ptr = val;
        } else {
            RegRef8 rs = getRegRef8(s, bL);
            RegRef32 rd = getRegRef32(s, bH);
            setFlagsMOV(&s->flags, *rs.ptr, 8);
            s->memory[(*rd.ptr + disp) & 0xFFFF] = *rs.ptr;
        }
        s->pc += 2;
    } break;

    case 0x6B: { /* MOV.L @aa:16, ERd / MOV.L ERs, @aa:16 (extended) */
        if (c == 0x6B) {
            switch (dH) {
            case 0x0: { /* MOV.l @aa:16, ERd */
                uint32_t addr = ef | 0xFF0000;
                RegRef32 rd = getRegRef32(s, dL);
                uint32_t val = getMem32(s, addr);
                setFlagsMOV(&s->flags, val, 32);
                *rd.ptr = val;
                s->pc += 4;
            } break;
            case 0x8: { /* MOV.l ERs, @aa:16 */
                uint32_t addr = cdef & 0xFFFF | 0xFF0000;
                RegRef32 rs = getRegRef32(s, dL);
                setFlagsMOV(&s->flags, *rs.ptr, 32);
                setMem32(s, addr, *rs.ptr);
                s->pc += 4;
            } break;
            default: goto unimpl;
            }
        }
    } break;

    case 0x6C: { /* MOV.B @aa:16, Rd / MOV.B Rs, @aa:16 */
        uint16_t addr = cd;
        if (!(bH & 8)) {
            RegRef8 rd = getRegRef8(s, bL);
            uint8_t val = s->memory[addr];
            setFlagsMOV(&s->flags, val, 8);
            *rd.ptr = val;
        } else {
            RegRef8 rs = getRegRef8(s, bL);
            setFlagsMOV(&s->flags, *rs.ptr, 8);
            s->memory[addr] = *rs.ptr;
        }
        s->pc += 2;
    } break;

    case 0x6D: { /* MOV.B @ERs+, Rd / MOV.B Rs, @-ERd */
        if (!(bH & 8)) {
            RegRef32 rs = getRegRef32(s, bH);
            RegRef8 rd = getRegRef8(s, bL);
            uint8_t val = s->memory[(*rs.ptr) & 0xFFFF];
            *rs.ptr += 1;
            setFlagsMOV(&s->flags, val, 8);
            *rd.ptr = val;
        } else {
            RegRef8 rs = getRegRef8(s, bL);
            RegRef32 rd = getRegRef32(s, bH);
            *rd.ptr -= 1;
            setFlagsMOV(&s->flags, *rs.ptr, 8);
            s->memory[(*rd.ptr) & 0xFFFF] = *rs.ptr;
        }
    } break;

    case 0x6E: { /* MOV.B @(d:16,ERs), Rd / MOV.B Rs, @(d:16,ERd) */
        uint32_t disp = ef;
        if (disp & 0x8000) disp |= 0xFFFF0000;
        if (!(bH & 8)) {
            RegRef32 rs = getRegRef32(s, bH);
            RegRef8 rd = getRegRef8(s, bL);
            uint8_t val = s->memory[(*rs.ptr + disp) & 0xFFFF];
            setFlagsMOV(&s->flags, val, 8);
            *rd.ptr = val;
        } else {
            RegRef8 rs = getRegRef8(s, bL);
            RegRef32 rd = getRegRef32(s, bH);
            setFlagsMOV(&s->flags, *rs.ptr, 8);
            s->memory[(*rd.ptr + disp) & 0xFFFF] = *rs.ptr;
        }
        s->pc += 4;
    } break;

    case 0x6F: { /* MOV.L @(d:16,ERs), ERd / MOV.L ERs, @(d:16,ERd) */
        uint32_t disp = ef;
        if (disp & 0x8000) disp |= 0xFFFF0000;
        if (!(bH & 8)) {
            RegRef32 rs = getRegRef32(s, bH);
            RegRef32 rd = getRegRef32(s, bL);
            uint32_t val = getMem32(s, *rs.ptr + disp);
            *rd.ptr = val;
            setFlagsMOV(&s->flags, val, 32);
        } else {
            RegRef32 rs = getRegRef32(s, bL);
            RegRef32 rd = getRegRef32(s, bH);
            setFlagsMOV(&s->flags, *rs.ptr, 32);
            setMem32(s, *rd.ptr + disp, *rs.ptr);
        }
        s->pc += 4;
    } break;

    /* ════════════════════════════════════════════════════════════════════
     * 0x70-0x77: BIT ops — @aa:8 memory form
     * ════════════════════════════════════════════════════════════════════ */
    case 0x70 ... 0x77: {
        uint8_t bitNum = aH;
        uint16_t addr = (uint16_t)(b << 8) | 0xFF00;
        switch (bH) {
        case 0x1: s->memory[addr] |= (1 << bitNum); break;   /* BSET */
        case 0x2: s->memory[addr] &= ~(1 << bitNum); break;  /* BCLR */
        case 0x3: s->flags.Z = !(s->memory[addr] & (1 << bitNum)); break; /* BTST */
        default: break;
        }
    } break;

    /* ════════════════════════════════════════════════════════════════════
     * 0x78-0x7F: Extended immediate / special
     * ════════════════════════════════════════════════════════════════════ */
    case 0x78: { /* AND.B #imm:8, @aa:16 */
        uint16_t addr = cd;
        s->memory[addr] &= (uint8_t)ef;
        s->pc += 4;
    } break;

    case 0x79: { /* MOV.W #imm:16, @aa:16 / MOV.L #imm:32, @aa:16 */
        uint16_t addr = (bH << 4 | bL) << 8 | (cH << 4 | cL);
        /* This is complex — simplified for PokéWalker usage */
        s->pc += 6;
    } break;

    case 0x7A: { /* MOV.L #imm:32, ERd */
        RegRef32 rd = getRegRef32(s, bL);
        uint32_t val = cdef;
        setFlagsMOV(&s->flags, val, 32);
        *rd.ptr = val;
        s->pc += 6;
    } break;

    case 0x7B: goto unimpl;

    case 0x7C: { /* MAC @ERs+, @ERs+ */
        /* Multiply-accumulate — not used by PokéWalker */
        goto unimpl;
    }

    case 0x7D: { /* JSR @@aa:8 */
        s->er[7] -= 2;
        uint16_t sp = (uint16_t)(s->er[7] & 0xFFFF);
        s->memory[sp] = (s->pc + 2) >> 8;
        s->memory[sp + 1] = (s->pc + 2) & 0xFF;
        s->pc = (uint16_t)(b << 1);
    } break;

    case 0x7E: { /* JMP @@aa:8 */
        s->pc = (uint16_t)(b << 1);
    } break;

    case 0x7F: { /* MOV.W @aa:16, Rd / MOV.W Rs, @aa:16 (bit-addressable) */
        uint16_t addr = cd;
        if (!(bH & 8)) {
            RegRef16 rd = getRegRef16(s, bL);
            uint16_t val = getMem16(s, addr);
            setFlagsMOV(&s->flags, val, 16);
            *rd.ptr = val;
        } else {
            RegRef16 rs = getRegRef16(s, bL);
            setFlagsMOV(&s->flags, *rs.ptr, 16);
            setMem16(s, addr, *rs.ptr);
        }
        s->pc += 4;
    } break;

    /* ════════════════════════════════════════════════════════════════════
     * 0x80-0xFF: Immediate instructions (ADD/CMP/SUB/OR/XOR/AND/MOV #imm)
     * ════════════════════════════════════════════════════════════════════ */
    case 0x80 ... 0x8F: { /* ADD.B #imm:8, Rd */
        RegRef8 rd = getRegRef8(s, aL);
        setFlagsADD(&s->flags, *rd.ptr, b, 8);
        *rd.ptr += b;
    } break;

    case 0x90 ... 0x9F: { /* ADDX #imm:8, Rd */
        RegRef8 rd = getRegRef8(s, aL);
        setFlagsADD(&s->flags, *rd.ptr, b + s->flags.C, 8);
        *rd.ptr += b;
        *rd.ptr += s->flags.C;
    } break;

    case 0xA0 ... 0xAF: { /* CMP.B #imm:8, Rd */
        RegRef8 rd = getRegRef8(s, aL);
        setFlagsSUB(&s->flags, *rd.ptr, b, 8);
    } break;

    case 0xB0 ... 0xBF: { /* SUBX #imm:8, Rd */
        RegRef8 rd = getRegRef8(s, aL);
        setFlagsSUB(&s->flags, *rd.ptr, b + s->flags.C, 8);
        *rd.ptr -= b;
        *rd.ptr -= s->flags.C;
    } break;

    case 0xC0 ... 0xCF: { /* OR.B #imm:8, Rd */
        RegRef8 rd = getRegRef8(s, aL);
        uint8_t val = *rd.ptr | b;
        setFlagsMOV(&s->flags, val, 8);
        *rd.ptr = val;
    } break;

    case 0xD0 ... 0xDF: { /* XOR.B #imm:8, Rd */
        RegRef8 rd = getRegRef8(s, aL);
        uint8_t val = *rd.ptr ^ b;
        setFlagsMOV(&s->flags, val, 8);
        *rd.ptr = val;
    } break;

    case 0xE0 ... 0xEF: { /* AND.B #imm:8, Rd */
        RegRef8 rd = getRegRef8(s, aL);
        uint8_t val = *rd.ptr & b;
        setFlagsMOV(&s->flags, val, 8);
        *rd.ptr = val;
    } break;

    case 0xF0 ... 0xFF: { /* MOV.B #imm:8, Rd */
        RegRef8 rd = getRegRef8(s, aL);
        setFlagsMOV(&s->flags, b, 8);
        *rd.ptr = b;
    } break;

    default:
    unimpl:
        /* Unimplemented instruction — treat as NOP to avoid hanging */
        break;
    }

    s->pc += 2;
    return (int)cycles;
}

/* ── Public API implementations ─────────────────────────────────────────── */

void h8_set_callbacks(H8State *s,
                       lcd_frame_callback lcd,
                       audio_event_callback audio,
                       step_query_callback step,
                       void *userdata) {
    s->lcdCallback = lcd;
    s->audioCallback = audio;
    s->stepCallback = step;
    s->callbackUserdata = userdata;
}

void h8_init(H8State *s, const uint8_t *romData, const uint8_t *eepromData) {
    memset(s, 0, sizeof(H8State));

    /* Load ROM into memory */
    if (romData) {
        memcpy(s->memory, romData, H8_ROM_SIZE);
    }

    /* Load EEPROM */
    if (eepromData) {
        memcpy(s->eeprom.memory, eepromData, H8_EEPROM_SIZE);
    } else {
        /* Initialize with "nintendo" magic marker */
        memcpy(s->eeprom.memory, "nintendo", 8);
    }
    s->eeprom.memory = s->memory; /* EEPROM is accessed via SPI mapped to memory */

    /* Read entry point from reset vector */
    s->entry = (s->memory[0] << 8) | s->memory[1];
    s->pc = (uint16_t)s->entry;

    /* Set up SP from reset vector */
    s->er[7] = 0x0000FF80;  /* Top of RAM */

    /* Set up MMIO pointers */
    s->IRQ_IENR1 = &s->memory[0xFFF3];
    s->IRQ_IENR2 = &s->memory[0xFFF4];
    s->IRQ_IRR1  = &s->memory[0xFFF6];
    s->IRQ_IRR2  = &s->memory[0xFFF7];
    s->RTCFLG    = &s->memory[0xF067];
    s->CKSTPR1   = &s->memory[0xFFFA];
    s->CKSTPR2   = &s->memory[0xFFFB];

    /* Timer B1 pointers */
    s->timerB.TMB1 = &s->memory[0xF0D0];
    s->timerB.TCB1 = &s->memory[0xF0D1];

    /* Timer W pointers */
    s->timerW.TMRW  = &s->memory[0xF0F0];
    s->timerW.TCRW  = &s->memory[0xF0F1];
    s->timerW.TIERW = &s->memory[0xF0F2];
    s->timerW.TSRW  = &s->memory[0xF0F3];
    s->timerW.TIOR0 = &s->memory[0xF0F4];
    s->timerW.TIOR1 = &s->memory[0xF0F5];
    s->timerW.TCNT  = (uint16_t *)&s->memory[TCNT_ADDRESS];
    s->timerW.GRA   = (uint16_t *)&s->memory[0xF0F8];
    s->timerW.GRB   = (uint16_t *)&s->memory[0xF0FA];
    s->timerW.GRC   = (uint16_t *)&s->memory[0xF0FC];
    s->timerW.GRD   = (uint16_t *)&s->memory[0xF0FE];

    /* LCD memory pointer */
    s->lcd.memory = &s->memory[0xF020];  /* SSU/SPI buffer region */

    /* SCI3 pointers */
    s->sci3.SMR3 = &s->memory[0xFF98];
    s->sci3.BRR3 = &s->memory[0xFF99];
    s->sci3.SCR3 = &s->memory[0xFF9A];
    s->sci3.TDR3 = &s->memory[0xFF9B];
    s->sci3.SSR3 = &s->memory[0xFF9C];
    s->sci3.RDR3 = &s->memory[0xFF9D];
    s->sci3.IrCR = &s->memory[0xFFA7];

    /* Set default contrast */
    s->lcd.contrast = 0x20;
}

void h8_set_keys(H8State *s, uint8_t buttons) {
    if (!s->flags.I && (buttons & H8_BTN_ENTER)) {
        *s->IRQ_IRR1 |= IRRI0;
    } else {
        pushInput(s, buttons);
        pushInput(s, 0);  /* Simulate key release */
        s->sleeping = false;
    }
}

void h8_inject_steps(H8State *s, uint32_t steps) {
    /* Inject into walker RAM step counters */
    uint32_t todaySteps = getMem32(s, 0xF79C);
    uint32_t lifetimeSteps = getMem32(s, 0xF780);

    todaySteps += steps;
    lifetimeSteps += steps;

    /* Clamp to limits */
    if (todaySteps > 99999) todaySteps = 99999;
    if (lifetimeSteps > 9999999) lifetimeSteps = 9999999;

    setMem32(s, 0xF79C, todaySteps);
    setMem32(s, 0xF780, lifetimeSteps);

    /* Update watts (20 steps = 1 watt) */
    uint8_t wattDivider = s->memory[0xF792];
    uint16_t watts = getMem16(s, 0xF78E);
    wattDivider += (uint8_t)steps;
    while (wattDivider >= 20) {
        wattDivider -= 20;
        if (watts < 9999) watts++;
    }
    s->memory[0xF792] = wattDivider;
    setMem16(s, 0xF78E, watts);
}

uint32_t h8_get_steps(H8State *s) {
    return getMem32(s, 0xF79C);
}

uint32_t h8_get_lifetime_steps(H8State *s) {
    return getMem32(s, 0xF780);
}

uint16_t h8_get_watts(H8State *s) {
    return getMem16(s, 0xF78E);
}

int h8_save_eeprom(H8State *s, uint8_t *buffer) {
    /* EEPROM is stored in the memory array's SPI-mapped region.
     * For a proper save, we need to read from the eeprom chip model.
     * Simplified: save the 64KB EEPROM region. */
    memcpy(buffer, s->eeprom.memory, H8_EEPROM_SIZE);
    return 0;
}

void h8_load_eeprom(H8State *s, const uint8_t *buffer) {
    memcpy(s->eeprom.memory, buffer, H8_EEPROM_SIZE);
}

uint8_t h8_read_mem(H8State *s, uint16_t addr) {
    return s->memory[addr & 0xFFFF];
}

void h8_write_mem(H8State *s, uint16_t addr, uint8_t value) {
    setMem8(s, addr, value);
}

uint16_t h8_get_timer_w_gra(H8State *s) {
    return *s->timerW.GRA;
}

uint8_t h8_get_volume(H8State *s) {
    return s->memory[0xF7C6];
}

bool h8_is_timer_w_active(H8State *s) {
    return s->timerW.on;
}

bool h8_is_sleeping(H8State *s) {
    return s->sleeping;
}

int h8_get_entry(H8State *s) {
    return s->entry;
}
