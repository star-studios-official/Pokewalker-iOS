/*
 * h8300h.c — H8/300H Tiny CPU emulator core for PokéWalker
 *
 * Ported from pokestride (github.com/edgarburgues/pokestride)
 * 3DS-specific code replaced with callback interfaces for cross-platform use.
 *
 * This version adds full SSU/SPI emulation for:
 *   - LCD (SSD1854): chip-select via PDR1 bits 0-1
 *   - EEPROM (M95512): chip-select via PDR1 bit 2
 *   - Accelerometer (BMA150): chip-select via PDR9 bit 0
 *
 * License: GPLv3 (same as pokestride)
 */

#include "h8300h.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ── Logging ────────────────────────────────────────────────────────────── */

static FILE *g_logFile = NULL;

void h8_set_log_file(FILE *f) {
    g_logFile = f;
}

#define LOG(fmt, ...) do { \
    if (g_logFile) { fprintf(g_logFile, fmt "\n", ##__VA_ARGS__); fflush(g_logFile); } \
} while(0)

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

/* SSU registers */
#define SSU_SSCRH  0xF0E0
#define SSU_SSCRL  0xF0E1
#define SSU_SSMR   0xF0E2
#define SSU_SSER   0xF0E3
#define SSU_SSSR   0xF0E4
#define SSU_SSRDR  0xF0E9
#define SSU_SSTDR  0xF0EB

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

/* SSSR bits */
#define SSSR_TDRE (1 << 2)
#define SSSR_RDRF (1 << 1)
#define SSSR_TEND (1 << 3)

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
    f->I  = value & (1 << 7);
    f->UI = value & (1 << 6);
    f->H  = value & (1 << 5);
    f->U  = value & (1 << 4);
    f->N  = value & (1 << 3);
    f->Z  = value & (1 << 2);
    f->V  = value & (1 << 1);
    f->C  = value & (1 << 0);
}

static uint8_t packFlags(H8Flags *f) {
    return (uint8_t)(
        (f->I  ? (1<<7) : 0) |
        (f->UI ? (1<<6) : 0) |
        (f->H  ? (1<<5) : 0) |
        (f->U  ? (1<<4) : 0) |
        (f->N  ? (1<<3) : 0) |
        (f->Z  ? (1<<2) : 0) |
        (f->V  ? (1<<1) : 0) |
        (f->C  ? (1<<0) : 0));
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

/* ── SSU/SPI chip-select routing ────────────────────────────────────────── */

/*
 * PDR1 bits:
 *   bit 0 = LCD CS (active low)
 *   bit 1 = LCD D/C (0=command, 1=data when LCD CS is low)
 *   bit 2 = EEPROM CS (active low)
 * PDR9 bits:
 *   bit 0 = Accel CS (active low)
 */

static bool lcdSelected(H8State *s) {
    return !(s->pdr1 & 0x01);  /* LCD CS active low */
}

static bool eepromSelected(H8State *s) {
    return !(s->pdr1 & 0x04);  /* EEPROM CS active low */
}

static bool accelSelected(H8State *s) {
    return !(s->pdr9 & 0x01);  /* Accel CS active low */
}

static bool lcdDataMode(H8State *s) {
    return (s->pdr1 & 0x02) != 0;  /* D/C bit: 1 = data, 0 = command */
}

/* ── LCD (SSD1854) SPI protocol ─────────────────────────────────────────── */

static uint8_t LCD_CMD_MAP[256];
static void lcdInitCmdMap(void);

static void lcd_process_cmd(H8State *s, uint8_t byte) {
    H8LCD *lcd = &s->lcd;
    lcd->cmdBuf[lcd->cmdBufOff++] = byte;

    uint8_t mapIdx = LCD_CMD_MAP[lcd->cmdBuf[0]];
    uint8_t expectedArgs = 0;
    /* Decode expected args from map index (0-6 = 0 args, 7 = 2 args, etc.) */
    switch (mapIdx) {
        case 0x07: expectedArgs = 2; break; /* Set Display Start Line */
        case 0x08: expectedArgs = 2; break; /* Set Display Offset */
        case 0x09: expectedArgs = 2; break; /* Set Multiplex Ratio */
        case 0x0A: expectedArgs = 2; break; /* Set N-line Inversion */
        case 0x0D: case 0x0E: case 0x0F: case 0x10:
            expectedArgs = 2; break; /* Window corners */
        case 0x13: expectedArgs = 2; break; /* Set Contrast */
        case 0x15: case 0x16: case 0x17: case 0x18:
        case 0x19: case 0x1A: case 0x1B: case 0x1C:
            expectedArgs = 2; break; /* Gray modes */
        default: expectedArgs = 0; break;
    }

    if (lcd->cmdBufOff > expectedArgs) {
        /* Execute command */
        uint8_t cmd = lcd->cmdBuf[0];
        /* Column address: lower 4 bits of 0x00-0x0F */
        if (cmd <= 0x0F) {
            lcd->column_address = (lcd->column_address & 0xF0) | ((cmd & 0x0F) << 1);
        }
        /* Upper column address: 0x10-0x1F */
        else if (cmd >= 0x10 && cmd <= 0x1F) {
            lcd->column_address = (lcd->column_address & 0x0F) | ((cmd & 0x0F) << 5);
        }
        /* Display start line: 0x40-0x7F */
        else if (cmd >= 0x40 && cmd <= 0x7F) {
            lcd->display_start_line = cmd & 0x3F;
        }
        /* Contrast: 0x81 + value */
        else if (cmd == 0x81 && lcd->cmdBufOff >= 2) {
            lcd->contrast = lcd->cmdBuf[1] & 0x3F;
        }
        /* Segment remap: 0xA0 or 0xA1 */
        else if (cmd == 0xA0 || cmd == 0xA1) {
            lcd->segment_remap = (cmd & 0x01) != 0;
        }
        /* Normal display: 0xA6 */
        else if (cmd == 0xA6) {
            lcd->inverse_display = false;
        }
        /* Inverse display: 0xA7 */
        else if (cmd == 0xA7) {
            lcd->inverse_display = true;
        }
        /* Display OFF: 0xAE */
        else if (cmd == 0xAE) {
            lcd->display_on = false;
        }
        /* Display ON: 0xAF */
        else if (cmd == 0xAF) {
            lcd->display_on = true;
        }
        /* Set page address: 0xB0-0xBF (lower nibble = page) */
        else if (cmd >= 0xB0 && cmd <= 0xBF) {
            lcd->page_address = cmd & 0x0F;
        }
        /* All display ON: 0xA5 */
        else if (cmd == 0xA5) {
            lcd->entire_display_on = true;
        }
        /* Normal display mode: 0xA4 */
        else if (cmd == 0xA4) {
            lcd->entire_display_on = false;
        }
        /* NOP for unknown */
        lcd->cmdBufOff = 0;
    }
}

static void lcd_process_data(H8State *s, uint8_t byte) {
    H8LCD *lcd = &s->lcd;
    /* Write byte to GDDRAM at current page/column */
    if (lcd->page_address < 128 && lcd->column_address < 128) {
        lcd->ram[lcd->page_address][lcd->column_address] = byte;
    }
    /* Auto-advance column address */
    lcd->column_address++;
    if (lcd->column_address >= 128) {
        lcd->column_address = 0;
    }
}

/* ── EEPROM (M95512) SPI protocol ───────────────────────────────────────── */

enum { EEPROM_IDLE, EEPROM_READ, EEPROM_WRITE, EEPROM_WRSR };

static void eeprom_write(H8State *s, uint8_t byte) {
    H8EEPROM *e = &s->eeprom;
    if (e->buf_off == -1) {
        /* Start of new command */
        switch (byte) {
            case 0x03: /* READ */
                e->buf_off = 0;
                e->buf[0] = byte;
                e->status = EEPROM_READ;
                break;
            case 0x02: /* WRITE */
                e->buf_off = 0;
                e->buf[0] = byte;
                e->status = EEPROM_WRITE;
                break;
            case 0x05: /* RDSR */
                e->buf_off = -1;
                e->next_read = e->status & 0x03; /* WIP and WEL bits */
                break;
            case 0x06: /* WREN */
                e->buf_off = -1;
                e->status |= 0x02; /* Set WEL */
                e->next_read = 0;
                break;
            case 0x04: /* WRDI */
                e->buf_off = -1;
                e->status &= ~0x02; /* Clear WEL */
                e->next_read = 0;
                break;
            case 0x01: /* WRSR */
                e->buf_off = 0;
                e->buf[0] = byte;
                e->status = EEPROM_WRSR;
                break;
            default:
                e->buf_off = -1;
                break;
        }
    } else {
        /* Ongoing command */
        e->buf_off++;
        if (e->buf_off < 10) {
            e->buf[e->buf_off] = byte;
        }
        switch (e->buf[0]) {
            case 0x03: /* READ */
                if (e->buf_off == 1) { e->buf[1] = byte; }
                else if (e->buf_off == 2) { e->buf[2] = byte; }
                else if (e->buf_off >= 3) {
                    uint16_t addr = ((uint16_t)e->buf[1] << 8) | e->buf[2];
                    addr += (uint16_t)(e->buf_off - 3);
                    addr &= 0xFFFF;
                    e->next_read = e->mem[addr];
                }
                break;
            case 0x02: /* WRITE */
                if (e->buf_off == 1) { e->buf[1] = byte; }
                else if (e->buf_off == 2) { e->buf[2] = byte; }
                else if (e->buf_off >= 3) {
                    if (e->status & 0x02) { /* WEL must be set */
                        uint16_t addr = ((uint16_t)e->buf[1] << 8) | e->buf[2];
                        addr += (uint16_t)(e->buf_off - 3);
                        addr &= 0xFFFF;
                        e->mem[addr] = byte;
                    }
                    e->next_read = 0;
                }
                break;
            case 0x01: /* WRSR */
                if (e->buf_off >= 1) {
                    e->status = (e->status & 0xFC) | (byte & 0x03);
                    e->buf_off = -1;
                }
                break;
        }
    }
}

static uint8_t eeprom_read(H8State *s) {
    return s->eeprom.next_read;
}

static void eeprom_stop(H8State *s) {
    s->eeprom.buf_off = -1;
    s->eeprom.status &= ~0x02; /* Clear WEL */
}

/* ── SSU SPI transfer ───────────────────────────────────────────────────── */

static uint8_t ssu_transfer(H8State *s, uint8_t mosi_byte) {
    uint8_t miso_byte = 0xFF;

    if (lcdSelected(s)) {
        /* LCD transfer */
        if (lcdDataMode(s)) {
            lcd_process_data(s, mosi_byte);
        } else {
            lcd_process_cmd(s, mosi_byte);
        }
        miso_byte = 0x00; /* LCD doesn't send meaningful data back */
    } else if (eepromSelected(s)) {
        /* EEPROM transfer */
        eeprom_write(s, mosi_byte);
        miso_byte = eeprom_read(s);
    } else if (accelSelected(s)) {
        /* Accelerometer transfer — simplified: return 0 */
        miso_byte = 0x00;
    }

    return miso_byte;
}

/* ── Memory-mapped I/O intercepts ───────────────────────────────────────── */

static void setMem8(H8State *s, uint32_t addr, uint8_t value) {
    addr &= 0xFFFF;

    /* SCI3 SCR3 write */
    if (addr == 0xFF9A) {
        uint8_t prev = s->memory[addr];
        s->memory[addr] = value;
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

    /* SCI3 TDR3 write */
    if (addr == 0xFF9B && (s->memory[0xFF9A] & SCI3_TE)) {
        s->memory[addr] = value;
        s->sci3.txPending = value;
        s->sci3.txHasPending = true;
        s->memory[0xFF9C] &= ~(SCI3_TDRE | SCI3_TEND);
        s->sci3.txCountdown = 320;
        s->sci3.txIdleCountdown = 0;
        return;
    }

    /* PORT1 (PDR1) at 0xFFD4 — chip select control */
    if (addr == PORT1_ADDR) {
        uint8_t oldPdr1 = s->pdr1;
        s->pdr1 = value;
        s->memory[addr] = value;

        /* Detect EEPROM CS rising edge → end of SPI transaction */
        if ((oldPdr1 & 0x04) && !(value & 0x04)) {
            /* EEPROM CS going low — start of transaction */
        } else if (!(oldPdr1 & 0x04) && (value & 0x04)) {
            /* EEPROM CS going high — end of transaction */
            eeprom_stop(s);
        }
        return;
    }

    /* PORT9 (PDR9) at 0xFFDC — accelerometer chip select */
    if (addr == PORT9_ADDR) {
        s->pdr9 = value;
        s->memory[addr] = value;
        return;
    }

    /* SSU SSTDR write at 0xF0EB — SPI transmit */
    if (addr == SSU_SSTDR) {
        s->ssu.sstdr = value;
        /* Perform SPI transfer */
        s->ssu.shiftReg = ssu_transfer(s, value);
        s->ssu.shiftValid = true;
        /* Set status flags: TDRE=1 (TX buffer empty), RDRF=1 (RX data ready) */
        s->memory[SSU_SSSR] |= SSSR_TDRE | SSSR_RDRF;
        return;
    }

    /* SSU SSSR write at 0xF0E4 — status register (read-1-write-0) */
    if (addr == SSU_SSSR) {
        uint8_t old = s->memory[addr];
        /* Only allow clearing flags, not setting them */
        s->memory[addr] = old & value;
        return;
    }

    /* SSU register writes — just store */
    if (addr >= SSU_SSCRH && addr <= SSU_SSER) {
        s->memory[addr] = value;
        return;
    }

    s->memory[addr] = value;
}

static uint8_t getMem8(H8State *s, uint32_t addr) {
    addr &= 0xFFFF;
    uint8_t value = s->memory[addr];

    /* SCI3 SSR3 read — latch flags */
    if (addr == 0xFF9C) {
        s->sci3.lastReadSSR3 = value;
    }

    /* SCI3 RDR3 read */
    if (addr == 0xFF9D) {
        s->memory[0xFF9C] &= ~SCI3_RDRF;
        if (s->sci3.rxPos < s->sci3.rxLen) {
            s->sci3.rxCountdown = 320;
        }
    }

    /* SSU SSRDR read at 0xF0E9 — SPI receive */
    if (addr == SSU_SSRDR) {
        if (s->ssu.shiftValid) {
            value = s->ssu.shiftReg;
            s->ssu.shiftValid = false;
        }
        return value;
    }

    /* SSU SSSR read at 0xF0E4 — always show TDRE=1, RDRF=1, TEND=1 */
    if (addr == SSU_SSSR) {
        return SSSR_TDRE | SSSR_RDRF | SSSR_TEND;
    }

    /* PORT1 read */
    if (addr == PORT1_ADDR) {
        return s->pdr1;
    }

    /* PORT9 read */
    if (addr == PORT9_ADDR) {
        return s->pdr9;
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
    H8LCD *lcd = &s->lcd;
    int startLine;

    if (lcd->display_start_line > 0 || lcd->display_on) {
        startLine = lcd->display_start_line & 0x3F;
    } else {
        startLine = 0;
    }

    for (int y = 0; y < H8_LCD_HEIGHT; y++) {
        int physRow = (startLine + y) % 64;
        int page = physRow >> 3;
        int yBit = physRow & 7;

        for (int x = 0; x < H8_LCD_WIDTH; x++) {
            int col = x;
            if (lcd->segment_remap) {
                col = 127 - x;
            }
            /* SSD1854 maps to LCD columns: offset by 32 for 96-wide display */
            int memCol = (col + 32) & 0x7F;

            uint8_t byte = (page < 128 && memCol < 128) ? lcd->ram[page][memCol] : 0;
            uint8_t pixel = (byte >> yBit) & 1;

            /* 2-bit palette from the single bitplane: bit set = dark, bit clear = light */
            /* But PokéWalker uses dual-bitplane 4-level grayscale from the two byte writes per column */
            /* For simplicity, use the single bit we have */
            uint32_t color;
            if (lcd->entire_display_on) {
                color = H8_GRAY_3; /* all pixels on */
            } else if (pixel) {
                color = lcd->inverse_display ? H8_GRAY_0 : H8_GRAY_3;
            } else {
                color = lcd->inverse_display ? H8_GRAY_3 : H8_GRAY_0;
            }

            videoBuffer[y * H8_LCD_WIDTH + x] = color;
        }
    }
}

/* ── Sub-clock tick ─────────────────────────────────────────────────────── */

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
                uint8_t cks = (*s->timerW.TCRW >> 4) & 0x07;
                uint8_t divider;
                switch (cks) {
                    case 4: divider = 1; break;
                    case 5: divider = 2; break;
                    case 6: divider = 4; break;
                    case 7: divider = 8; break;
                    default: divider = 1; break;
                }

                static uint8_t twDivCounter = 0;
                if (++twDivCounter >= divider) {
                    twDivCounter = 0;
                    setMem16(s, TCNT_ADDRESS, getMem16(s, TCNT_ADDRESS) + 1);
                }
            }

            if (getMem16(s, TCNT_ADDRESS) == getMem16(s, 0xF0F8)) {
                if (*s->timerW.TCRW & CCLR) {
                    setMem16(s, TCNT_ADDRESS, 0);
                }
                *s->timerW.TSRW |= 0x1;

                uint16_t gra = *s->timerW.GRA;
                uint8_t vol = s->memory[0xF7C6];
                if (s->audioCallback) {
                    s->audioCallback(gra, vol, true, s->callbackUserdata);
                }

                if (*s->timerW.TIERW & 0x1) {
                    if (!s->flags.I) {
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

        s->timerB.on = (*s->CKSTPR1 & TB1CKSTP) && (*s->timerB.TMB1 & TMB_COUNTING);
        s->timerW.on = (*s->CKSTPR2 & TWCKSTP) && (*s->timerW.TMRW & CTS);

        s->subClockCycles++;
    }
}

/* ── SCI3 tick ──────────────────────────────────────────────────────────── */

void h8_tick_sci3(H8State *s) {
    if (s->sci3.txHasPending && s->sci3.txCountdown > 0) {
        if (--s->sci3.txCountdown == 0) {
            s->memory[0xFF9C] |= SCI3_TDRE;
            s->sci3.txHasPending = false;
            s->sci3.txIdleCountdown = 320;
        }
    }

    if (!s->sci3.txHasPending && s->sci3.txIdleCountdown > 0) {
        if (--s->sci3.txIdleCountdown == 0) {
            s->memory[0xFF9C] |= SCI3_TEND;
        }
    }

    if (s->sci3.rxPos < s->sci3.rxLen && s->sci3.rxCountdown > 0) {
        if (--s->sci3.rxCountdown == 0) {
            s->memory[0xFF9D] = s->sci3.rxBuf[s->sci3.rxPos++];
            s->memory[0xFF9C] |= SCI3_RDRF;
            if (s->sci3.rxPos < s->sci3.rxLen) {
                s->sci3.rxCountdown = 320;
            }
        }
    }
}

/* ── Interrupt context save/restore ─────────────────────────────────────── */

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
        case 0x0: return true;
        case 0x1: return false;
        case 0x2: return f->C || f->Z;
        case 0x3: return !f->C && !f->Z;
        case 0x4: return !f->C;
        case 0x5: return f->C;
        case 0x6: return !f->Z;
        case 0x7: return f->Z;
        case 0x8: return !f->V;
        case 0x9: return f->V;
        case 0xA: return !f->N;
        case 0xB: return f->N;
        case 0xC: return (f->N == f->V);
        case 0xD: return (f->N != f->V);
        case 0xE: return !f->Z && (f->N == f->V);
        case 0xF: return f->Z || (f->N != f->V);
        default: return false;
    }
}

/* ── Cycle count table ─────────────────────────────────────────────────── */

static const uint8_t h8_cycles[256] = {
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    12,12,2,2,8,6,10,2,6,4,6,8,8,6,6,8,
    2,2,2,2,2,2,2,2,4,4,6,6,6,6,6,6,
    2,2,2,2,2,2,2,2,10,4,6,2,8,8,6,6,
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
};

/* ══════════════════════════════════════════════════════════════════════════
 * PUBLIC API
 * ══════════════════════════════════════════════════════════════════════════ */

void h8_set_callbacks(H8State *s, lcd_frame_callback lcd,
                       audio_event_callback audio, step_query_callback step,
                       void *userdata) {
    s->lcdCallback = lcd;
    s->audioCallback = audio;
    s->stepCallback = step;
    s->callbackUserdata = userdata;
}

void h8_init(H8State *s, const uint8_t *romData, const uint8_t *eepromData) {
    /* Zero out everything first */
    memset(s, 0, sizeof(H8State));

    /* Load ROM into memory (must happen before anything else) */
    if (romData) {
        memcpy(s->memory, romData, H8_ROM_SIZE);
        LOG("h8_init: ROM loaded %d bytes, first bytes: %02X %02X %02X %02X",
            H8_ROM_SIZE, romData[0], romData[1], romData[2], romData[3]);
    } else {
        LOG("h8_init: WARNING - no ROM data!");
    }

    /* CRITICAL: Load EEPROM into its OWN buffer (NOT into s->memory!) */
    if (eepromData) {
        memcpy(s->eeprom.mem, eepromData, H8_EEPROM_SIZE);
        LOG("h8_init: EEPROM loaded %d bytes", H8_EEPROM_SIZE);
    } else {
        /* Initialize fresh EEPROM with "nintendo" magic */
        memcpy(s->eeprom.mem, "nintendo", 8);
        LOG("h8_init: Fresh EEPROM initialized with magic marker");
    }

    /* Read entry point from reset vector */
    s->entry = (s->memory[0] << 8) | s->memory[1];
    s->pc = (uint16_t)s->entry;
    LOG("h8_init: Entry point = 0x%04X, PC = 0x%04X", s->entry, s->pc);

    /* Set up SP */
    s->er[7] = 0x0000FF80;
    LOG("h8_init: SP = 0x%08X", s->er[7]);

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

    /* SCI3 pointers */
    s->sci3.SMR3 = &s->memory[0xFF98];
    s->sci3.BRR3 = &s->memory[0xFF99];
    s->sci3.SCR3 = &s->memory[0xFF9A];
    s->sci3.TDR3 = &s->memory[0xFF9B];
    s->sci3.SSR3 = &s->memory[0xFF9C];
    s->sci3.RDR3 = &s->memory[0xFF9D];
    s->sci3.IrCR = &s->memory[0xFFA7];

    /* Initialize LCD command map */
    lcdInitCmdMap();

    /* Initialize LCD state */
    s->lcd.display_on = true;
    s->lcd.contrast = 0x20;
    s->lcd.mux_ratio = 0x3F;

    /* Initialize EEPROM state */
    s->eeprom.buf_off = -1;

    /* Initialize SSU defaults */
    s->memory[SSU_SSSR] = SSSR_TDRE;

    /* Initialize PORT defaults */
    s->pdr1 = 0x05;  /* Both LCD CS and EEPROM CS high (deselected) */
    s->memory[PORT1_ADDR] = s->pdr1;
    s->pdr9 = 0x01;  /* Accel CS high (deselected) */
    s->memory[PORT9_ADDR] = s->pdr9;

    LOG("h8_init: Complete. ROM entry=0x%04X, LCD CS=high, EEPROM CS=high",
        s->entry);
}

void h8_set_keys(H8State *s, uint8_t buttons) {
    LOG("h8_set_keys: buttons=0x%02X, IRQ_I=%d", buttons, s->flags.I);
    if (!s->flags.I && (buttons & H8_BTN_ENTER)) {
        *s->IRQ_IRR1 |= IRRI0;
    } else {
        pushInput(s, buttons);
        pushInput(s, 0);  /* Simulate key release */
        s->sleeping = false;
    }
}

void h8_inject_steps(H8State *s, uint32_t steps) {
    uint32_t todaySteps = getMem32(s, 0xF79C);
    uint32_t lifetimeSteps = getMem32(s, 0xF780);
    LOG("h8_inject_steps: injecting %u steps (before: today=%u, lifetime=%u)",
        steps, todaySteps, lifetimeSteps);

    todaySteps += steps;
    lifetimeSteps += steps;

    if (todaySteps > 99999) todaySteps = 99999;
    if (lifetimeSteps > 9999999) lifetimeSteps = 9999999;

    setMem32(s, 0xF79C, todaySteps);
    setMem32(s, 0xF780, lifetimeSteps);

    uint8_t wattDivider = s->memory[0xF792];
    uint16_t watts = getMem16(s, 0xF78E);
    wattDivider += (uint8_t)steps;
    while (wattDivider >= 20) {
        wattDivider -= 20;
        if (watts < 9999) watts++;
    }
    s->memory[0xF792] = wattDivider;
    setMem16(s, 0xF78E, watts);

    LOG("h8_inject_steps: after: today=%u, lifetime=%u, watts=%u",
        todaySteps, lifetimeSteps, watts);
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
    memcpy(buffer, s->eeprom.mem, H8_EEPROM_SIZE);
    LOG("h8_save_eeprom: saved %d bytes", H8_EEPROM_SIZE);
    return 0;
}

void h8_load_eeprom(H8State *s, const uint8_t *buffer) {
    memcpy(s->eeprom.mem, buffer, H8_EEPROM_SIZE);
    LOG("h8_load_eeprom: loaded %d bytes", H8_EEPROM_SIZE);
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

/* ══════════════════════════════════════════════════════════════════════════
 * INSTRUCTION EXECUTION
 * ══════════════════════════════════════════════════════════════════════════ */

int h8_step(H8State *s) {
    uint32_t cycles = 2;
    if (s->sleeping) return 0;

    /* ── ROM-specific hooks (original pwflash.rom, entry=0x02C4) ── */
    if (s->entry == 0x02C4) {
        if (s->pc == 0x336)  { s->pc += 4; return 0; }
        if (s->pc == 0x350)  { s->pc += 4; s->er[0] = (s->er[0] & ~0xFF); return 0; }
        if (s->pc == 0x7700) { s->pc += 2; return 0; }
        if (s->pc == 0x9b84) {
            if (!inputEmpty(s))
                setMem8(s, 0xffde, popInput(s));
        }
    }

    /* ── Compiled ROM hooks (entry=0x3F0E) ── */
    if (s->entry == 0x3F0E) {
        if (s->pc >= 0x118E && s->pc < 0x1230) {
            uint16_t sp = (uint16_t)(s->er[7] & 0xFFFF);
            s->pc = (s->memory[sp] << 8) | s->memory[sp + 1];
            s->er[7] += 2;
            return 0;
        }
        if (s->pc == 0x10A8) {
            uint16_t sp = (uint16_t)(s->er[7] & 0xFFFF);
            s->pc = (s->memory[sp] << 8) | s->memory[sp + 1];
            s->er[7] += 2;
            return 0;
        }
        if (s->pc == 0x2ABE) {
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
    case 0x00 ... 0x0F:
        switch (aL) {
        case 0x0: break; /* NOP */

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
                    case 0x0: {
                        uint32_t addr = ef | 0xFF0000;
                        RegRef32 rd = getRegRef32(s, dL);
                        uint32_t val = getMem32(s, addr);
                        setFlagsMOV(&s->flags, val, 32);
                        *rd.ptr = val;
                        s->pc += 4;
                    } break;
                    case 0x8: {
                        uint32_t addr = cdef & 0xFFFF | 0xFF0000;
                        RegRef32 rs = getRegRef32(s, dL);
                        setFlagsMOV(&s->flags, *rs.ptr, 32);
                        setMem32(s, addr, *rs.ptr);
                        s->pc += 4;
                    } break;
                    default: goto unimpl;
                    }
                } else if (c == 0x6D) {
                    if (!(dH & 8)) {
                        RegRef32 rs = getRegRef32(s, dH);
                        RegRef32 rd = getRegRef32(s, dL);
                        uint32_t val = getMem32(s, *rs.ptr);
                        *rs.ptr += 4;
                        setFlagsMOV(&s->flags, val, 32);
                        *rd.ptr = val;
                    } else {
                        RegRef32 rs = getRegRef32(s, dL);
                        RegRef32 rd = getRegRef32(s, dH);
                        *rd.ptr -= 4;
                        setFlagsMOV(&s->flags, *rs.ptr, 32);
                        setMem32(s, *rd.ptr, *rs.ptr);
                    }
                    s->pc += 2;
                } else if (c == 0x6F) {
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
                    RegRef32 rs = getRegRef32(s, dH);
                    RegRef32 rd = getRegRef32(s, dL);
                    uint32_t val = *rs.ptr & *rd.ptr;
                    setFlagsMOV(&s->flags, val, 32);
                    *rd.ptr = val;
                    s->pc += 2;
                } else if (c == 0x64) {
                    RegRef32 rs = getRegRef32(s, dH);
                    RegRef32 rd = getRegRef32(s, dL);
                    uint32_t val = *rs.ptr | *rd.ptr;
                    setFlagsMOV(&s->flags, val, 32);
                    *rd.ptr = val;
                    s->pc += 2;
                } else if (c == 0x65) {
                    RegRef32 rs = getRegRef32(s, dH);
                    RegRef32 rd = getRegRef32(s, dL);
                    uint32_t val = *rs.ptr ^ *rd.ptr;
                    setFlagsMOV(&s->flags, val, 32);
                    *rd.ptr = val;
                    s->pc += 2;
                }
                break;

            case 0x4: s->pc += 2; break; /* LDC/STC prefix */
            case 0x8: s->sleeping = true; break; /* SLEEP */

            case 0xC: /* MULXS */
                if (bL == 0x0 && cH == 0x6) {
                    if (cL == 0x0) {
                        RegRef8 rs = getRegRef8(s, dH);
                        RegRef16 rd = getRegRef16(s, dL);
                        int8_t lo = *rd.ptr & 0xFF;
                        *rd.ptr = (uint16_t)((int16_t)*rs.ptr * (int16_t)lo);
                        s->flags.Z = *rd.ptr == 0;
                        s->flags.N = *rd.ptr & 0x8000;
                        s->pc += 2;
                    } else if (cL == 0x2) {
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

            case 0xF:
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

        case 0x4: setFlags(&s->flags, packFlags(&s->flags) | b); break;
        case 0x5: setFlags(&s->flags, packFlags(&s->flags) ^ b); break;
        case 0x6: setFlags(&s->flags, packFlags(&s->flags) & b); break;
        case 0x7: setFlags(&s->flags, b); break;

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

        case 0xB:
            switch (bH) {
            case 0x0: { RegRef32 rd = getRegRef32(s, bL); *rd.ptr += 1; } break;
            case 0x8: { RegRef32 rd = getRegRef32(s, bL); *rd.ptr += 2; } break;
            case 0x9: { RegRef32 rd = getRegRef32(s, bL); *rd.ptr += 4; } break;
            case 0x5: {
                RegRef16 rd = getRegRef16(s, bL);
                setFlagsINC(&s->flags, *rd.ptr, 1, 16);
                *rd.ptr += 1;
            } break;
            case 0x7: {
                RegRef32 rd = getRegRef32(s, bL);
                setFlagsINC(&s->flags, *rd.ptr, 1, 32);
                *rd.ptr += 1;
            } break;
            case 0xD: {
                RegRef16 rd = getRegRef16(s, bL);
                setFlagsINC(&s->flags, *rd.ptr, 2, 16);
                *rd.ptr += 2;
            } break;
            case 0xF: {
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

        case 0xE: goto unimpl;

        case 0xF:
            if (bH >= 0x8) {
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
            case 0x0: { RegRef8 rd = getRegRef8(s, bL); s->flags.C = *rd.ptr & 0x1; *rd.ptr >>= 1; setFlagsMOV(&s->flags, *rd.ptr, 8); } break;
            case 0x1: { RegRef16 rd = getRegRef16(s, bL); s->flags.C = *rd.ptr & 0x1; *rd.ptr >>= 1; setFlagsMOV(&s->flags, *rd.ptr, 16); } break;
            case 0x3: { RegRef32 rd = getRegRef32(s, bL); s->flags.C = *rd.ptr & 0x1; *rd.ptr >>= 1; setFlagsMOV(&s->flags, *rd.ptr, 32); } break;
            case 0x9: { RegRef16 rd = getRegRef16(s, bL); s->flags.C = *rd.ptr & 0x1; *rd.ptr = (*rd.ptr >> 1) | (*rd.ptr & 0x8000); setFlagsMOV(&s->flags, *rd.ptr, 16); } break;
            case 0xB: { RegRef32 rd = getRegRef32(s, bL); s->flags.C = *rd.ptr & 0x1; *rd.ptr = (*rd.ptr >> 1) | (*rd.ptr & 0x80000000); setFlagsMOV(&s->flags, *rd.ptr, 32); } break;
            default: goto unimpl;
            }
            break;

        case 0x2: /* Rotate left */
            switch (bH) {
            case 0x0: { RegRef8 rd = getRegRef8(s, bL); bool oldC = s->flags.C; s->flags.C = *rd.ptr & 0x80; *rd.ptr = (uint8_t)((*rd.ptr << 1) | oldC); setFlagsMOV(&s->flags, *rd.ptr, 8); } break;
            case 0x1: { RegRef16 rd = getRegRef16(s, bL); bool oldC = s->flags.C; s->flags.C = *rd.ptr & 0x8000; *rd.ptr = (uint16_t)((*rd.ptr << 1) | oldC); setFlagsMOV(&s->flags, *rd.ptr, 16); } break;
            case 0x3: { RegRef32 rd = getRegRef32(s, bL); bool oldC = s->flags.C; s->flags.C = *rd.ptr & 0x80000000; *rd.ptr = (*rd.ptr << 1) | oldC; setFlagsMOV(&s->flags, *rd.ptr, 32); } break;
            case 0x8: { RegRef8 rd = getRegRef8(s, bL); s->flags.C = *rd.ptr & 0x80; *rd.ptr = (uint8_t)((*rd.ptr << 1) | s->flags.C); setFlagsMOV(&s->flags, *rd.ptr, 8); } break;
            case 0x9: { RegRef16 rd = getRegRef16(s, bL); s->flags.C = *rd.ptr & 0x8000; *rd.ptr = (uint16_t)((*rd.ptr << 1) | s->flags.C); setFlagsMOV(&s->flags, *rd.ptr, 16); } break;
            case 0xB: { RegRef32 rd = getRegRef32(s, bL); s->flags.C = *rd.ptr & 0x80000000; *rd.ptr = (*rd.ptr << 1) | s->flags.C; setFlagsMOV(&s->flags, *rd.ptr, 32); } break;
            default: goto unimpl;
            }
            break;

        case 0x3: /* Rotate right */
            switch (bH) {
            case 0x0: { RegRef8 rd = getRegRef8(s, bL); bool oldC = s->flags.C; s->flags.C = *rd.ptr & 0x1; *rd.ptr = (uint8_t)((*rd.ptr >> 1) | (oldC << 7)); setFlagsMOV(&s->flags, *rd.ptr, 8); } break;
            case 0x1: { RegRef16 rd = getRegRef16(s, bL); bool oldC = s->flags.C; s->flags.C = *rd.ptr & 0x1; *rd.ptr = (uint16_t)((*rd.ptr >> 1) | (oldC << 15)); setFlagsMOV(&s->flags, *rd.ptr, 16); } break;
            case 0x3: { RegRef32 rd = getRegRef32(s, bL); bool oldC = s->flags.C; s->flags.C = *rd.ptr & 0x1; *rd.ptr = (*rd.ptr >> 1) | ((uint32_t)oldC << 31); setFlagsMOV(&s->flags, *rd.ptr, 32); } break;
            case 0x8: { RegRef8 rd = getRegRef8(s, bL); uint8_t bit0 = *rd.ptr & 0x1; s->flags.C = bit0; *rd.ptr = (uint8_t)((*rd.ptr >> 1) | (bit0 << 7)); setFlagsMOV(&s->flags, *rd.ptr, 8); } break;
            case 0x9: { RegRef16 rd = getRegRef16(s, bL); uint16_t bit0 = *rd.ptr & 0x1; s->flags.C = bit0; *rd.ptr = (uint16_t)((*rd.ptr >> 1) | (bit0 << 15)); setFlagsMOV(&s->flags, *rd.ptr, 16); } break;
            case 0xB: { RegRef32 rd = getRegRef32(s, bL); uint32_t bit0 = *rd.ptr & 0x1; s->flags.C = bit0; *rd.ptr = (*rd.ptr >> 1) | (bit0 << 31); setFlagsMOV(&s->flags, *rd.ptr, 32); } break;
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

        case 0x7:
            switch (bH) {
            case 0x0: { RegRef8 rd = getRegRef8(s, bL); *rd.ptr = ~*rd.ptr; setFlagsMOV(&s->flags, *rd.ptr, 8); } break;
            case 0x1: { RegRef16 rd = getRegRef16(s, bL); *rd.ptr = ~*rd.ptr; setFlagsMOV(&s->flags, *rd.ptr, 16); } break;
            case 0x5: { RegRef16 rd = getRegRef16(s, bL); *rd.ptr &= 0x00FF; setFlagsMOV(&s->flags, *rd.ptr, 16); } break;
            case 0x7: { RegRef32 rd = getRegRef32(s, bL); *rd.ptr &= 0x0000FFFF; setFlagsMOV(&s->flags, *rd.ptr, 32); } break;
            case 0x8: { RegRef8 rd = getRegRef8(s, bL); setFlagsSUB(&s->flags, 0, *rd.ptr, 8); if (*rd.ptr != 0x80) *rd.ptr = (uint8_t)(-(int8_t)*rd.ptr); } break;
            case 0x9: { RegRef16 rd = getRegRef16(s, bL); setFlagsSUB(&s->flags, 0, *rd.ptr, 16); if (*rd.ptr != 0x8000) *rd.ptr = (uint16_t)(-(int16_t)*rd.ptr); } break;
            case 0xD: { RegRef16 rd = getRegRef16(s, bL); if (*rd.ptr & 0x80) *rd.ptr = (*rd.ptr & 0xFF) | 0xFF00; else *rd.ptr &= 0xFF; setFlagsMOV(&s->flags, *rd.ptr, 16); } break;
            case 0xF: { RegRef32 rd = getRegRef32(s, bL); if (*rd.ptr & 0x8000) *rd.ptr = (*rd.ptr & 0xFFFF) | 0xFFFF0000; else *rd.ptr &= 0xFFFF; setFlagsMOV(&s->flags, *rd.ptr, 32); } break;
            default: goto unimpl;
            }
            break;

        case 0x8: { RegRef8 rs = getRegRef8(s, bH); RegRef8 rd = getRegRef8(s, bL); setFlagsSUB(&s->flags, *rd.ptr, *rs.ptr, 8); *rd.ptr -= *rs.ptr; } break;
        case 0x9: { RegRef16 rs = getRegRef16(s, bH); RegRef16 rd = getRegRef16(s, bL); setFlagsSUB(&s->flags, *rd.ptr, *rs.ptr, 16); *rd.ptr -= *rs.ptr; } break;

        case 0xA:
            switch (bH) {
            case 0x0: { RegRef8 rd = getRegRef8(s, bL); setFlagsINC(&s->flags, *rd.ptr, (uint32_t)-1, 8); *rd.ptr -= 1; } break;
            case 0x8 ... 0xF: { RegRef32 rs = getRegRef32(s, bH); RegRef32 rd = getRegRef32(s, bL); setFlagsSUB(&s->flags, *rd.ptr, *rs.ptr, 32); *rd.ptr -= *rs.ptr; } break;
            default: goto unimpl;
            }
            break;

        case 0xB:
            switch (bH) {
            case 0x0: { RegRef32 rd = getRegRef32(s, bL); *rd.ptr -= 1; } break;
            case 0x8: { RegRef32 rd = getRegRef32(s, bL); *rd.ptr -= 2; } break;
            case 0x9: { RegRef32 rd = getRegRef32(s, bL); *rd.ptr -= 4; } break;
            case 0x5: { RegRef16 rd = getRegRef16(s, bL); setFlagsINC(&s->flags, *rd.ptr, (uint32_t)-1, 16); *rd.ptr -= 1; } break;
            case 0x7: { RegRef32 rd = getRegRef32(s, bL); setFlagsINC(&s->flags, *rd.ptr, (uint32_t)-1, 32); *rd.ptr -= 1; } break;
            case 0xD: { RegRef16 rd = getRegRef16(s, bL); setFlagsINC(&s->flags, *rd.ptr, (uint32_t)-2, 16); *rd.ptr -= 2; } break;
            case 0xF: { RegRef32 rd = getRegRef32(s, bL); setFlagsINC(&s->flags, *rd.ptr, (uint32_t)-2, 32); *rd.ptr -= 2; } break;
            default: goto unimpl;
            }
            break;

        case 0xC: { RegRef8 rs = getRegRef8(s, bH); RegRef8 rd = getRegRef8(s, bL); setFlagsSUB(&s->flags, *rd.ptr, *rs.ptr, 8); } break;
        case 0xD: { RegRef16 rs = getRegRef16(s, bH); RegRef16 rd = getRegRef16(s, bL); setFlagsSUB(&s->flags, *rd.ptr, *rs.ptr, 16); } break;

        case 0xE: { /* SUBX Rs, Rd */
            RegRef8 rs = getRegRef8(s, bH);
            RegRef8 rd = getRegRef8(s, bL);
            setFlagsSUB(&s->flags, *rd.ptr, *rs.ptr + s->flags.C, 8);
            *rd.ptr -= *rs.ptr;
            *rd.ptr -= s->flags.C;
        } break;

        case 0xF:
            if (bH >= 0x8) {
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

    /* 0x20-0x2F: MOV.B @aa:8, Rd */
    case 0x20 ... 0x2F: {
        RegRef8 rd = getRegRef8(s, aL);
        uint16_t addr = (uint16_t)(b << 8) | 0xFF00;
        uint8_t val = getMem8(s, addr);
        setFlagsMOV(&s->flags, val, 8);
        *rd.ptr = val;
        s->pc += 2;
    } break;

    /* 0x30-0x3F: MOV.B Rs, @aa:8 */
    case 0x30 ... 0x3F: {
        RegRef8 rs = getRegRef8(s, aL);
        uint16_t addr = (uint16_t)(b << 8) | 0xFF00;
        setFlagsMOV(&s->flags, *rs.ptr, 8);
        setMem8(s, addr, *rs.ptr);
        s->pc += 2;
    } break;

    /* 0x40-0x4F: MOV.B @aa:16, Rd / MOV.B Rs, @aa:16 */
    case 0x40 ... 0x4F: {
        if (aL & 0x8) {
            /* MOV.B Rs, @aa:16 */
            RegRef8 rs = getRegRef8(s, aL & 0x7);
            setFlagsMOV(&s->flags, *rs.ptr, 8);
            setMem8(s, cdef & 0xFFFF, *rs.ptr);
        } else {
            /* MOV.B @aa:16, Rd */
            RegRef8 rd = getRegRef8(s, aL);
            uint8_t val = getMem8(s, cdef & 0xFFFF);
            setFlagsMOV(&s->flags, val, 8);
            *rd.ptr = val;
        }
        s->pc += 4;
    } break;

    /* 0x50-0x5F: MOV.B @(d:16, ERs), Rd / MOV.B Rs, @(d:16, ERs) */
    case 0x50 ... 0x5F: {
        int opType = (bL >> 3) & 0x1;
        if (opType == 0) {
            /* MOV.B @(d:16, ERs), Rd */
            RegRef8 rd = getRegRef8(s, bL & 0x7);
            RegRef16 rs = getRegRef16(s, bH);
            uint32_t addr = *rs.ptr + (int16_t)cd;
            uint8_t val = getMem8(s, addr);
            setFlagsMOV(&s->flags, val, 8);
            *rd.ptr = val;
        } else {
            /* MOV.B Rs, @(d:16, ERs) */
            RegRef8 rs = getRegRef8(s, bL & 0x7);
            RegRef16 rd = getRegRef16(s, bH);
            uint32_t addr = *rd.ptr + (int16_t)cd;
            setFlagsMOV(&s->flags, *rs.ptr, 8);
            setMem8(s, addr, *rs.ptr);
        }
        s->pc += 4;
    } break;

    /* 0x60-0x6F: MOV.B @ERs, Rd / MOV.B Rs, @ERs / MOV.B @ERs+, Rd / MOV.B Rs, @-ERd */
    case 0x60 ... 0x6F: {
        int sub = bL & 0xF;
        if (sub < 0x8) {
            /* MOV.B @ERs, Rd */
            RegRef8 rd = getRegRef8(s, bL);
            RegRef16 rs = getRegRef16(s, bH);
            uint8_t val = getMem8(s, *rs.ptr);
            setFlagsMOV(&s->flags, val, 8);
            *rd.ptr = val;
        } else {
            /* MOV.B Rs, @ERs */
            RegRef8 rs = getRegRef8(s, bL & 0x7);
            RegRef16 rd = getRegRef16(s, bH);
            setFlagsMOV(&s->flags, *rs.ptr, 8);
            setMem8(s, *rd.ptr, *rs.ptr);
        }
    } break;

    /* 0x70-0x7F: MOV.B @(d:8, ERs), Rd / MOV.B Rs, @(d:8, ERs) / MOV.B @PC+ Rd */
    case 0x70 ... 0x7F: {
        if (aL == 0xC || aL == 0xD || aL == 0xE || aL == 0xF) {
            /* MOV.B @PC+, Rd (PC-relative) */
            RegRef8 rd = getRegRef8(s, aL & 0x7);
            uint16_t addr = s->pc + 2;
            uint8_t val = getMem8(s, addr);
            setFlagsMOV(&s->flags, val, 8);
            *rd.ptr = val;
            s->pc += 4;
        } else {
            int opType = (bL >> 3) & 0x1;
            if (opType == 0) {
                /* MOV.B @(d:8, ERs), Rd */
                RegRef8 rd = getRegRef8(s, bL & 0x7);
                RegRef16 rs = getRegRef16(s, bH);
                uint32_t addr = *rs.ptr + b;
                uint8_t val = getMem8(s, addr);
                setFlagsMOV(&s->flags, val, 8);
                *rd.ptr = val;
            } else {
                /* MOV.B Rs, @(d:8, ERs) */
                RegRef8 rs = getRegRef8(s, bL & 0x7);
                RegRef16 rd = getRegRef16(s, bH);
                uint32_t addr = *rd.ptr + b;
                setFlagsMOV(&s->flags, *rs.ptr, 8);
                setMem8(s, addr, *rs.ptr);
            }
            s->pc += 2;
        }
    } break;

    /* 0x80-0x8F: ADD.B #imm:8, Rd */
    case 0x80 ... 0x8F: {
        RegRef8 rd = getRegRef8(s, aL);
        setFlagsADD(&s->flags, *rd.ptr, b, 8);
        *rd.ptr += b;
    } break;

    /* 0x90-0x9F: ADD.W #imm:16, Rd */
    case 0x90 ... 0x9F: {
        RegRef16 rd = getRegRef16(s, aL);
        setFlagsADD(&s->flags, *rd.ptr, (uint16_t)cd, 16);
        *rd.ptr += (uint16_t)cd;
        s->pc += 2;
    } break;

    /* 0xA0-0xAF: CMP.B #imm:8, Rd / CMP.W #imm:16, Rd */
    case 0xA0 ... 0xAF: {
        if (aL < 0x8) {
            RegRef8 rd = getRegRef8(s, aL);
            setFlagsSUB(&s->flags, *rd.ptr, b, 8);
        } else {
            RegRef16 rd = getRegRef16(s, aL & 0x7);
            setFlagsSUB(&s->flags, *rd.ptr, (uint16_t)cd, 16);
            s->pc += 2;
        }
    } break;

    /* 0xB0-0xBF: SUB.B / SUB.W #imm, Rd */
    case 0xB0 ... 0xBF: {
        if (aL < 0x8) {
            RegRef8 rd = getRegRef8(s, aL);
            setFlagsSUB(&s->flags, *rd.ptr, b, 8);
            *rd.ptr -= b;
        } else {
            RegRef16 rd = getRegRef16(s, aL & 0x7);
            setFlagsSUB(&s->flags, *rd.ptr, (uint16_t)cd, 16);
            *rd.ptr -= (uint16_t)cd;
            s->pc += 2;
        }
    } break;

    /* 0xC0-0xCF: OR.B #imm:8, Rd / AND.B / XOR.B */
    case 0xC0 ... 0xCF: {
        RegRef8 rd = getRegRef8(s, aL);
        uint8_t val;
        switch (bH) {
        case 0x0: val = *rd.ptr | b; break; /* OR */
        case 0x1: val = *rd.ptr & b; break; /* AND */
        case 0x2: val = *rd.ptr ^ b; break; /* XOR */
        default: val = *rd.ptr; break;
        }
        setFlagsMOV(&s->flags, val, 8);
        *rd.ptr = val;
    } break;

    /* 0xD0-0xDF: MOV.B #imm:8, Rd */
    case 0xD0 ... 0xDF: {
        RegRef8 rd = getRegRef8(s, aL);
        setFlagsMOV(&s->flags, b, 8);
        *rd.ptr = b;
    } break;

    /* 0xE0-0xEF: MOV.W #imm:16, Rd */
    case 0xE0 ... 0xEF: {
        RegRef16 rd = getRegRef16(s, aL);
        setFlagsMOV(&s->flags, (uint16_t)cd, 16);
        *rd.ptr = (uint16_t)cd;
        s->pc += 2;
    } break;

    /* 0xF0-0xFF: MOV.L #imm:32, ERd / BRA/BRN/conditional branches */
    case 0xF0 ... 0xFF:
        if (aL < 0x8) {
            /* MOV.L #imm:32, ERd */
            RegRef32 rd = getRegRef32(s, aL);
            uint32_t imm = ((uint32_t)cd << 16) | ef;
            setFlagsMOV(&s->flags, imm, 32);
            *rd.ptr = imm;
            s->pc += 4;
        } else {
            /* Branch: BRA/BRN/BHI/BLS/BCC/BCS/BNE/BEQ/BVC/BVS/BPL/BMI/BGE/BLT/BGT/BLE */
            if (checkCondition(&s->flags, aL & 0xF)) {
                int8_t offset = (int8_t)b;
                s->pc = (uint16_t)(s->pc + 2 + (offset * 2));
            }
            s->pc += 2;
        }
        break;

    default: goto unimpl;
    }

    s->pc += 2;
    return (int)cycles;

unimpl:
    LOG("h8_step: UNIMPL at PC=0x%04X, opcode=0x%04X%04X",
        s->pc, ab, cd);
    s->pc += 2;
    return (int)cycles;
}

/* ── Static init for LCD command map ────────────────────────────────────── */

static void lcdInitCmdMap(void) {
    static bool inited = false;
    if (inited) return;
    inited = true;

    /* All 0x00-0x0F: set lower column address → 0 args */
    for (int i = 0x00; i <= 0x0F; i++) LCD_CMD_MAP[i] = 0x00;
    /* All 0x10-0x1F: set upper column address → 0 args */
    for (int i = 0x10; i <= 0x1F; i++) LCD_CMD_MAP[i] = 0x01;
    /* 0x20-0x2F: 0 args */
    for (int i = 0x20; i <= 0x2F; i++) LCD_CMD_MAP[i] = 0x02;
    /* 0x30-0x3F: 0 args */
    for (int i = 0x30; i <= 0x3F; i++) LCD_CMD_MAP[i] = 0x03;
    /* 0x40-0x7F: set display start line → 0 args */
    for (int i = 0x40; i <= 0x7F; i++) LCD_CMD_MAP[i] = 0x04;
    /* 0x80: contrast → 1 arg (index 0x13) */
    LCD_CMD_MAP[0x81] = 0x13;
    /* 0x82-0x8F: other */
    for (int i = 0x82; i <= 0x8F; i++) LCD_CMD_MAP[i] = 0x12;
    /* 0xA0-0xA1: segment remap → 0 args */
    LCD_CMD_MAP[0xA0] = 0x1F;
    LCD_CMD_MAP[0xA1] = 0x1F;
    /* 0xA2-0xA3: 0 args */
    LCD_CMD_MAP[0xA2] = 0x20;
    LCD_CMD_MAP[0xA3] = 0x20;
    /* 0xA4: normal display → 0 args */
    LCD_CMD_MAP[0xA4] = 0x21;
    /* 0xA5: all display on → 0 args */
    LCD_CMD_MAP[0xA5] = 0x21;
    /* 0xA6: normal → 0 args */
    LCD_CMD_MAP[0xA6] = 0x22;
    /* 0xA7: inverse → 0 args */
    LCD_CMD_MAP[0xA7] = 0x22;
    /* 0xA8: power save → 0 args */
    LCD_CMD_MAP[0xA8] = 0x24;
    /* 0xA9-0xAF: various */
    LCD_CMD_MAP[0xA9] = 0x26;
    LCD_CMD_MAP[0xAA] = 0x28;
    LCD_CMD_MAP[0xAB] = 0x28;
    LCD_CMD_MAP[0xAC] = 0x27;
    LCD_CMD_MAP[0xAD] = 0x27;
    LCD_CMD_MAP[0xAE] = 0x28; /* Display OFF */
    LCD_CMD_MAP[0xAF] = 0x28; /* Display ON */
    /* 0xB0-0xBF: set page address → 0 args */
    for (int i = 0xB0; i <= 0xBF; i++) LCD_CMD_MAP[i] = 0x29;
    /* 0xC0-0xDF: 0 args */
    for (int i = 0xC0; i <= 0xDF; i++) LCD_CMD_MAP[i] = 0x2B;
    /* 0xE0-0xFF: various */
    for (int i = 0xE0; i <= 0xFF; i++) LCD_CMD_MAP[i] = 0x2B;
    LCD_CMD_MAP[0xE1] = 0x2C; /* exit power save */
    LCD_CMD_MAP[0xE2] = 0x2D; /* software reset */
    LCD_CMD_MAP[0xE3] = 0x2E;
    LCD_CMD_MAP[0xE4] = 0x2F;
    LCD_CMD_MAP[0xE5] = 0x31;
    LCD_CMD_MAP[0xE6] = 0x33;
    LCD_CMD_MAP[0xE7] = 0x34;
    LCD_CMD_MAP[0xE8] = 0x35;
}
