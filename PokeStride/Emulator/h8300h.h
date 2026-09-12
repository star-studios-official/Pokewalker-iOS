#pragma once
/*
 * h8300h.h — H8/300H Tiny CPU emulator core for PokéWalker
 *
 * Ported from pokestride (3DS) by stripping all platform-specific code
 * (3DS I2C, IR hardware, NDSP audio, citro2d rendering) and replacing
 * them with callback interfaces.
 *
 * The PokéWalker uses a Renesas H8/38606 (H8/300H Tiny variant):
 *   - 3.6864 MHz system clock, 32.768 KHz sub-clock
 *   - 8 x 32-bit registers ER0-ER7 (ER7 = SP), accessible as 16-bit (r/e) or 8-bit (h/l)
 *   - 64 KB address space: ROM 0x0000-0xBFFF, MMIO 0xF020-0xFFFF, RAM 0xF780-0xFF7F
 *   - Big-endian instruction encoding
 *   - Condition code register (CCR): I H U N Z V C
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ──────────────────────────────────────────────────────────── */

#define H8_MEM_SIZE        0x10000   /* 64 KB address space */
#define H8_ROM_SIZE        0xC000    /* 49152 bytes ROM */
#define H8_RAM_START       0xF780
#define H8_RAM_END         0xFF80
#define H8_RAM_SIZE        (H8_RAM_END - H8_RAM_START)  /* 2048 bytes */
#define H8_EEPROM_SIZE     0x10000   /* 64 KB */

#define H8_LCD_WIDTH       96
#define H8_LCD_HEIGHT      64
#define H8_LCD_MEM_WIDTH   128
#define H8_LCD_MEM_HEIGHT  176
#define H8_LCD_MEM_SIZE    (H8_LCD_MEM_WIDTH * H8_LCD_MEM_HEIGHT / 4)

#define H8_SUB_CLOCK       32768     /* 32.768 KHz sub-clock */
#define H8_SYSTEM_CLOCK    3686400   /* 3.6864 MHz */

/* Button bits (PORT B) */
#define H8_BTN_ENTER       (1 << 0)
#define H8_BTN_LEFT        (1 << 2)
#define H8_BTN_RIGHT       (1 << 4)

/* Palette: 4-level grayscale (white → black) */
#define H8_GRAY_0  0xFF333333u  /* lightest */
#define H8_GRAY_1  0xFF666666u
#define H8_GRAY_2  0xFF999999u
#define H8_GRAY_3  0xFFCCCCCCu  /* darkest */

/* ── Callback interfaces (platform-specific) ────────────────────────────── */

/* Called when the LCD framebuffer is ready to be displayed.
 * videoBuffer is 96*64 = 6144 uint32_t pixels in BGRA8 format.
 * Called once per video frame (when the ROM does a page flip). */
typedef void (*lcd_frame_callback)(const uint32_t *videoBuffer, void *userdata);

/* Called when Timer W produces an audio event.
 * graValue: Timer W GRA register (determines frequency: 32768/(2*GRA) Hz)
 * volume: 0=off, 1=half, 2=full
 * timerActive: whether Timer W is running */
typedef void (*audio_event_callback)(uint16_t graValue, uint8_t volume, bool timerActive, void *userdata);

/* Called when steps should be injected (from HealthKit pedometer).
 * Returns the number of steps actually added. */
typedef uint32_t (*step_query_callback)(void *userdata);

/* ── Register reference types (used internally) ─────────────────────────── */

typedef struct { uint8_t idx; char loOrHiReg; uint8_t *ptr; } RegRef8;
typedef struct { uint8_t idx; char loOrHiReg; uint16_t *ptr; } RegRef16;
typedef struct { uint8_t idx; uint32_t *ptr; } RegRef32;

/* ── CCR flags ──────────────────────────────────────────────────────────── */

typedef struct {
    bool I;   /* Interrupt mask */
    bool UI;  /* User interrupt */
    bool H;   /* Half-carry */
    bool U;   /* User bit */
    bool N;   /* Negative */
    bool Z;   /* Zero */
    bool V;   /* Overflow */
    bool C;   /* Carry */
} H8Flags;

/* ── Peripheral state ───────────────────────────────────────────────────── */

/* SCI3 (IrDA serial) — only used for ROM's own TX/RX path */
typedef struct {
    uint8_t *SMR3;   /* 0xFF98 */
    uint8_t *BRR3;   /* 0xFF99 */
    uint8_t *SCR3;   /* 0xFF9A */
    uint8_t *TDR3;   /* 0xFF9B */
    uint8_t *SSR3;   /* 0xFF9C */
    uint8_t *RDR3;   /* 0xFF9D */
    uint8_t *IrCR;   /* 0xFFA7 */
    uint32_t txCountdown;
    uint8_t  txPending;
    bool     txHasPending;
    uint8_t  rxBuf[256];
    uint16_t rxLen;
    uint16_t rxPos;
    uint32_t rxCountdown;
    uint32_t txIdleCountdown;
    uint8_t  lastReadSSR3;
} H8SCI3;

/* LCD controller (SSD1854) */
typedef struct {
    uint8_t *memory;      /* GDDRAM (128 pages × 128 columns × 2 bitplanes) */
    uint8_t  contrast;
    uint8_t  currentColumn;
    uint8_t  currentPage;
    uint8_t  currentByte;
    bool     currentBuffer;
    uint8_t  displayStartLine;
    bool     startLineSet;
    bool     startLineActive;
} H8LCD;

/* Timer B1 */
typedef struct {
    bool    on;
    uint8_t TLBvalue;
    uint8_t *TMB1;
    uint8_t *TCB1;
} H8TimerB;

/* Timer W (audio PWM) */
typedef struct {
    bool      on;
    uint8_t  *TMRW;
    uint8_t  *TCRW;
    uint8_t  *TIERW;
    uint8_t  *TSRW;
    uint8_t  *TIOR0;
    uint8_t  *TIOR1;
    uint16_t *TCNT;
    uint16_t *GRA;
    uint16_t *GRB;
    uint16_t *GRC;
    uint16_t *GRD;
} H8TimerW;

/* EEPROM (SPI interface, 64 KB M95512) */
typedef struct {
    uint8_t *memory;
    uint8_t  status;
    struct {
        uint8_t  hiAddress;
        uint8_t  loAddress;
        uint8_t  state;   /* 0=empty, 1=status, 2=addr_hi, 3=addr_lo, 4=bytes */
        uint16_t offset;
        uint8_t  isWrite;
    } buffer;
} H8EEPROM;

/* Accelerometer (BMA150, simplified) */
typedef struct {
    uint8_t *memory;
    struct {
        uint8_t address;
        uint8_t offset;
        uint8_t state;
    } buffer;
} H8Accel;

/* ── Main emulator state ────────────────────────────────────────────────── */

typedef struct {
    /* CPU registers */
    uint32_t er[8];        /* ER0-ER7 (ER7 = SP) */
    H8Flags  flags;
    uint16_t pc;

    /* Memory */
    uint8_t  memory[H8_MEM_SIZE];

    /* Peripherals */
    H8SCI3    sci3;
    H8LCD     lcd;
    H8TimerB  timerB;
    H8TimerW  timerW;
    H8EEPROM  eeprom;
    H8Accel   accel;

    /* Interrupt state */
    uint8_t  *IRQ_IENR1;
    uint8_t  *IRQ_IENR2;
    uint8_t  *IRQ_IRR1;
    uint8_t  *IRQ_IRR2;
    uint8_t  *RTCFLG;
    uint8_t  *CKSTPR1;
    uint8_t  *CKSTPR2;

    /* Interrupt context save (nested interrupts) */
    #define H8_INT_SAVE_DEPTH 8
    uint16_t interruptSavedAddressStack[H8_INT_SAVE_DEPTH];
    H8Flags  interruptSavedFlagsStack[H8_INT_SAVE_DEPTH];
    int      interruptSaveDepth;
    uint16_t interruptSavedAddress;
    H8Flags  interruptSavedFlags;

    /* Input queue */
    uint8_t  inputBuf[32];
    int      inputHead;
    int      inputTail;
    int      inputCount;

    /* Timing */
    uint64_t subClockCycles;
    bool     sleeping;
    int      entry;       /* ROM entry point (read from reset vector) */

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

/* Initialize the emulator. Loads ROM data into memory, sets up MMIO pointers,
 * reads entry point from reset vector, and configures peripheral state.
 * romData: 49152 bytes of PokéWalker ROM (pwflash.rom)
 * eepromData: 65536 bytes of EEPROM (pweep.rom), or NULL for fresh state */
void h8_init(H8State *state, const uint8_t *romData, const uint8_t *eepromData);

/* Run one instruction. Returns the number of cycles consumed.
 * Call repeatedly in a loop, passing sub-clock ticks to h8_tick_subclock(). */
int h8_step(H8State *state);

/* Tick the sub-clock (32768 Hz). Handles Timer B1 overflow and Timer W.
 * Call once per sub-clock tick (or batch multiple ticks). */
void h8_tick_subclock(H8State *state, int ticks);

/* Tick the SCI3 baud rate countdowns (for byte timing).
 * Call from the main loop. */
void h8_tick_sci3(H8State *state);

/* Inject a button press (ENTER, LEFT, or RIGHT). */
void h8_set_keys(H8State *state, uint8_t buttons);

/* Inject steps from HealthKit pedometer. */
void h8_inject_steps(H8State *state, uint32_t steps);

/* Read current step count from walker RAM (0xF79C). */
uint32_t h8_get_steps(H8State *state);

/* Read lifetime steps from walker RAM (0xF780). */
uint32_t h8_get_lifetime_steps(H8State *state);

/* Read current watts from walker RAM (0xF78E). */
uint16_t h8_get_watts(H8State *state);

/* Render the LCD framebuffer into videoBuffer (96×64 uint32_t BGRA pixels). */
void h8_render_lcd(H8State *state, uint32_t *videoBuffer);

/* Save EEPROM state to buffer (must be at least 65536 bytes).
 * Returns 0 on success. */
int h8_save_eeprom(H8State *state, uint8_t *buffer);

/* Load EEPROM state from buffer. */
void h8_load_eeprom(H8State *state, const uint8_t *buffer);

/* Read a byte from H8 memory (for debugging). */
uint8_t h8_read_mem(H8State *state, uint16_t addr);

/* Write a byte to H8 memory (for debugging/IR injection). */
void h8_write_mem(H8State *state, uint16_t addr, uint8_t value);

/* Register callbacks. */
void h8_set_callbacks(H8State *state,
                       lcd_frame_callback lcd,
                       audio_event_callback audio,
                       step_query_callback step,
                       void *userdata);

/* Get Timer W GRA value and volume (for audio). */
uint16_t h8_get_timer_w_gra(H8State *state);
uint8_t  h8_get_volume(H8State *state);
bool     h8_is_timer_w_active(H8State *state);

/* Check if CPU is in SLEEP mode. */
bool h8_is_sleeping(H8State *state);

/* Get the ROM entry point. */
int h8_get_entry(H8State *state);

#ifdef __cplusplus
}
#endif
