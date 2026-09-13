#pragma once
/*
 * h8300h.h — H8/300H Tiny CPU emulator core for PokéWalker
 *
 * Ported from pokestride (3DS) by stripping all platform-specific code
 * and replacing them with callback interfaces.
 *
 * The PokéWalker uses a Renesas H8/38606 (H8/300H Tiny variant):
 *   - 3.6864 MHz system clock, 32.768 KHz sub-clock
 *   - 8 x 32-bit registers ER0-ER7 (ER7 = SP)
 *   - 64 KB address space: ROM 0x0000-0xBFFF, MMIO 0xF020-0xFFFF, RAM 0xF780-0xFF7F
 *   - Big-endian instruction encoding
 *
 * SPI peripherals (all via SSU at 0xF0E0-0xF0EB):
 *   - LCD (SSD1854): CS = PDR1 bit 0, D/C = PDR1 bit 1
 *   - EEPROM (M95512): CS = PDR1 bit 2
 *   - Accelerometer (BMA150): CS = PDR9 bit 0
 *
 * License: GPLv3 (same as pokestride)
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ──────────────────────────────────────────────────────────── */

#define H8_MEM_SIZE        0x10000   /* 64 KB address space */
#define H8_ROM_SIZE        0xC000    /* 49152 bytes ROM */
#define H8_RAM_START       0xF780
#define H8_RAM_END         0xFF80
#define H8_RAM_SIZE        (H8_RAM_END - H8_RAM_START)
#define H8_EEPROM_SIZE     0x10000   /* 64 KB */

#define H8_LCD_WIDTH       96
#define H8_LCD_HEIGHT      64

#define H8_SUB_CLOCK       32768
#define H8_SYSTEM_CLOCK    3686400

/* Button bits (PORT B) */
#define H8_BTN_ENTER       (1 << 0)
#define H8_BTN_LEFT        (1 << 2)
#define H8_BTN_RIGHT       (1 << 4)

/* Palette: 4-level grayscale (dark on light) */
#define H8_GRAY_0  0xFFFFFFFFu  /* white/lightest */
#define H8_GRAY_1  0xFFBBBBBBu
#define H8_GRAY_2  0xFF555555u
#define H8_GRAY_3  0xFF000000u  /* black/darkest */

/* ── Callback interfaces ────────────────────────────────────────────────── */

typedef void (*lcd_frame_callback)(const uint32_t *videoBuffer, void *userdata);
typedef void (*audio_event_callback)(uint16_t graValue, uint8_t volume, bool timerActive, void *userdata);
typedef uint32_t (*step_query_callback)(void *userdata);

/* ── Register reference types (internal) ────────────────────────────────── */

typedef struct { uint8_t idx; char loOrHiReg; uint8_t *ptr; } RegRef8;
typedef struct { uint8_t idx; char loOrHiReg; uint16_t *ptr; } RegRef16;
typedef struct { uint8_t idx; uint32_t *ptr; } RegRef32;

/* ── CCR flags ──────────────────────────────────────────────────────────── */

typedef struct {
    bool I, UI, H, U, N, Z, V, C;
} H8Flags;

/* ── SCI3 (IrDA serial) ────────────────────────────────────────────────── */

typedef struct {
    uint8_t *SMR3, *BRR3, *SCR3, *TDR3, *SSR3, *RDR3, *IrCR;
    uint32_t txCountdown;
    uint8_t  txPending;
    bool     txHasPending;
    uint8_t  rxBuf[256];
    uint16_t rxLen, rxPos;
    uint32_t rxCountdown, txIdleCountdown;
    uint8_t  lastReadSSR3;
} H8SCI3;

/* ── LCD controller (SSD1854) — SPI-connected ───────────────────────────── */
/* Memory layout matches pokestride: page * LCD_WIDTH * 2 + column * 2 + byte */
/* 2 bytes per column = 2 bitplanes for 4-level grayscale */
#define H8_LCD_MEM_WIDTH   128
#define H8_LCD_MEM_HEIGHT  176
#define H8_LCD_MEM_SIZE    (H8_LCD_MEM_WIDTH * H8_LCD_MEM_HEIGHT / 4)
#define H8_LCD_BYTES_PER_STRIPE 2

enum H8LCDState { H8LCD_EMPTY, H8LCD_READING_CONTRAST, H8LCD_READING_STARTLINE };

typedef struct {
    uint8_t  memory[H8_LCD_MEM_SIZE];  /* GDDRAM */
    uint8_t  contrast;
    enum H8LCDState state;
    uint8_t  currentColumn;
    uint8_t  currentPage;
    uint8_t  currentByte;
    bool     currentBuffer;       /* free-running half toggle (fallback) */
    uint8_t  displayStartLine;    /* 0x40 start-line value */
    bool     startLineSet;
    bool     startLineActive;
} H8LCD;

/* ── EEPROM (M95512 SPI EEPROM) ─────────────────────────────────────────── */

typedef struct {
    uint8_t  mem[H8_EEPROM_SIZE];  /* 64KB EEPROM data — own buffer, not overlapping ROM! */
    uint8_t  status;               /* WIP, WEL, BP0, BP1 etc. */
    uint8_t  buf[10];
    int      buf_off;
    uint8_t  next_read;
} H8EEPROM;

/* ── SSU (Synchronous Serial Unit — SPI master) ─────────────────────────── */

typedef struct {
    uint8_t *SSCRH, *SSCRL, *SSMR, *SSER, *SSSR;
    uint8_t *SSRDR, *SSTDR;
    uint8_t  shiftReg;
    bool     shiftValid;
    uint8_t  progress;     /* SSU transfer progress counter (0-7) */
} H8SSU;

/* ── Accelerometer (BMA150, simplified) ──────────────────────────────────── */

typedef struct {
    uint8_t  memory[256];
    struct {
        uint8_t address, offset, state;
    } buffer;
} H8Accel;

/* ── Timer B1 ───────────────────────────────────────────────────────────── */

typedef struct {
    bool    on;
    uint8_t TLBvalue;
    uint8_t *TMB1, *TCB1;
} H8TimerB;

/* ── Timer W (audio PWM) ────────────────────────────────────────────────── */

typedef struct {
    bool      on;
    uint8_t  *TMRW, *TCRW, *TIERW, *TSRW, *TIOR0, *TIOR1;
    uint16_t *TCNT, *GRA, *GRB, *GRC, *GRD;
} H8TimerW;

/* ── Main emulator state ────────────────────────────────────────────────── */

typedef struct {
    /* CPU registers */
    uint32_t er[8];
    H8Flags  flags;
    uint16_t pc;

    /* Memory (ROM + MMIO + RAM, NOT EEPROM) */
    uint8_t  memory[H8_MEM_SIZE];

    /* Peripherals */
    H8SCI3    sci3;
    H8LCD     lcd;
    H8TimerB  timerB;
    H8TimerW  timerW;
    H8EEPROM  eeprom;
    H8SSU     ssu;
    H8Accel   accel;

    /* Chip select state (from PORT1 writes) */
    uint8_t   pdr1;        /* PORT1 output latch */
    uint8_t   pdr9;        /* PORT9 output latch */

    /* Interrupt state */
    uint8_t  *IRQ_IENR1, *IRQ_IENR2, *IRQ_IRR1, *IRQ_IRR2;
    uint8_t  *RTCFLG, *CKSTPR1, *CKSTPR2;

    #define H8_INT_SAVE_DEPTH 8
    uint16_t interruptSavedAddressStack[H8_INT_SAVE_DEPTH];
    H8Flags  interruptSavedFlagsStack[H8_INT_SAVE_DEPTH];
    int      interruptSaveDepth;
    uint16_t interruptSavedAddress;
    H8Flags  interruptSavedFlags;

    /* Input queue */
    uint8_t  inputBuf[32];
    int      inputHead, inputTail, inputCount;

    /* Timing */
    uint64_t subClockCycles;
    bool     sleeping;
    int      entry;

    /* Audio event latch */
    bool     audioEventPending;
    uint16_t audioEventGRA;

    /* Callbacks */
    lcd_frame_callback   lcdCallback;
    audio_event_callback audioCallback;
    step_query_callback  stepCallback;
    void                *callbackUserdata;
} H8State;

/* ── Public API ─────────────────────────────────────────────────────────── */

void h8_init(H8State *state, const uint8_t *romData, const uint8_t *eepromData);
int  h8_step(H8State *state);
void h8_tick_subclock(H8State *state, int ticks);
void h8_tick_sci3(H8State *state);
void h8_tick_ssu(H8State *state);
void h8_set_keys(H8State *state, uint8_t buttons);
void h8_inject_steps(H8State *state, uint32_t steps);
uint32_t h8_get_steps(H8State *state);
uint32_t h8_get_lifetime_steps(H8State *state);
uint16_t h8_get_watts(H8State *state);
void h8_render_lcd(H8State *state, uint32_t *videoBuffer);
int  h8_save_eeprom(H8State *state, uint8_t *buffer);
void h8_load_eeprom(H8State *state, const uint8_t *buffer);
uint8_t h8_read_mem(H8State *state, uint16_t addr);
void h8_write_mem(H8State *state, uint16_t addr, uint8_t value);
void h8_set_callbacks(H8State *state, lcd_frame_callback lcd,
                       audio_event_callback audio, step_query_callback step,
                       void *userdata);

/* Set a FILE* for emulator logging (C side). Pass NULL to disable. */
void h8_set_log_file(FILE *f);
uint16_t h8_get_timer_w_gra(H8State *state);
uint8_t  h8_get_volume(H8State *state);
bool     h8_is_timer_w_active(H8State *state);
bool     h8_is_sleeping(H8State *state);
int      h8_get_entry(H8State *state);

#ifdef __cplusplus
}
#endif
