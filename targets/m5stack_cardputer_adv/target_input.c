/**
 * target_input.c — M5Stack Cardputer-ADV TCA8418 keyboard driver
 *
 * TCA8418 key ID derivation (from hardware capture, kamrrillo/Cardputer-ADV-Keyboard):
 *   The TCA8418 scans column-by-column. For a 4-row × 14-column physical layout
 *   mapped to TCA8418 rows 0-3 and columns wired in pairs with gaps, the key IDs are:
 *
 *   Physical layout (key ID in parentheses):
 *
 *   ROW 1:  esc(1)  1(5)  2(11) 3(15) 4(21) 5(25) 6(31) 7(35) 8(41) 9(45) 0(51) -(55) =(61) del(65)
 *   ROW 2:  tab(2)  q(6)  w(12) e(16) r(22) t(26) y(32) u(36) i(42) o(46) p(52) [(56) ](62)  \(66)
 *   ROW 3:   fn(3)  Aa(7) a(13) s(17) d(23) f(27) g(33) h(37) j(43) k(47) l(53) ^(57) '(63) ent(67)
 *   ROW 4: ctrl(4) opt(8) alt(14) z(18) x(24) c(28) v(34) b(38) n(44) m(48) <(54) v(58) >(64) spc(68)
 *
 *   Gaps at IDs 9-10, 19-20, 29-30, 39-40, 49-50, 59-60 are TCA8418 matrix holes
 *   (wiring skips TCA columns 2, 12, 22, 32, 42, 52 in the 10-column key numbering scheme).
 *
 * Navigation key mapping:
 *   InputKeyBack  ← esc   (ID  1, top-left key)
 *   InputKeyOk    ← enter (ID 67)
 *   InputKeyUp    ← ↑     (ID 57, Fn+; keycap)
 *   InputKeyLeft  ← ←     (ID 54, Fn+, keycap)
 *   InputKeyDown  ← ↓     (ID 58, Fn+. keycap)
 *   InputKeyRight ← →     (ID 64, Fn+/ keycap)
 *
 * Text key publishing:
 *   Non-navigation keys publish InputTypeText events via a separate pubsub handle.
 *   The event carries the resolved ASCII character (base layer; shift/fn handled by caller).
 *   Pass NULL for text_pubsub to disable text key events.
 */

#include "target_input.h"
#include <boards/board.h>
#include <driver/gpio.h>
#include <driver/i2c.h>
#include <esp_err.h>
#include <esp_log.h>
#include <furi.h>

#define TAG "InputCardputerAdv"

/* InputTypeText is now a real value in the base InputType enum (input.h): the
 * ADV physical keyboard publishes printable characters as InputTypeText events
 * (ASCII in InputEvent.key) on the same pubsub as nav events. gui_input()
 * special-cases them and routes to the focused view; text_input consumes them. */

/* ---- Timing ---- */
#define INPUT_LONG_PRESS_MS     500U
#define INPUT_REPEAT_MS         200U
#define KEY_DEBOUNCE_MS         50U

static uint32_t last_key_time[88] = {0};  /* Track last press time for each key */

static bool should_process_key(uint8_t key_id, bool pressed) {
    /* Never debounce-suppress a release: dropping one leaves a modifier
     * (Shift/Ctrl/Fn/Alt/Opt) stuck on, or a nav key auto-repeating forever.
     * The TCA8418 already applies hardware debounce, so only gate presses. */
    if(!pressed) return true;

    uint32_t now = furi_get_tick();
    uint32_t* last_time = &last_key_time[key_id % 88];

    if(now - *last_time < furi_ms_to_ticks(KEY_DEBOUNCE_MS)) {
        return false;  /* Debounce */
    }

    *last_time = now;
    return true;
}

/* =========================================================================
 * TCA8418 register addresses
 * ========================================================================= */
#define TCA8418_REG_CFG             0x01
#define TCA8418_REG_INT_STAT        0x02
#define TCA8418_REG_KEY_LCK_EC      0x03
#define TCA8418_REG_KEY_EVENT_A     0x04
#define TCA8418_REG_KP_GPIO1        0x1D
#define TCA8418_REG_KP_GPIO2        0x1E
#define TCA8418_REG_KP_GPIO3        0x1F
#define TCA8418_REG_DEBOUNCE1       0x29
#define TCA8418_REG_DEBOUNCE2       0x2A
#define TCA8418_REG_DEBOUNCE3       0x2B

/* CFG register bits */
#define TCA8418_CFG_KE_IEN          (1 << 0)   /* Key event interrupt enable */
#define TCA8418_CFG_INT_CFG         (1 << 1)   /* INT pin: 0=level, 1=pulse */
#define TCA8418_CFG_OVR_FLOW_M      (1 << 2)   /* Overflow interrupt mask */
#define TCA8418_CFG_AI              (1 << 7)   /* Auto-increment (not needed here) */

/* Key event byte masks */
#define TCA8418_KEY_PRESS_MASK      0x80
#define TCA8418_KEY_ID_MASK         0x7F
#define TCA8418_FIFO_EMPTY          0x00

/* =========================================================================
 * TCA8418 key ID constants  (from hardware capture)
 * ========================================================================= */

/* Navigation keys */
#define TCA_KEY_ESC     1   /* ` key — InputKeyBack */
#define TCA_KEY_TAB     2
#define TCA_KEY_FN      3
#define TCA_KEY_CTRL    4
#define TCA_KEY_1       5
#define TCA_KEY_Q       6
#define TCA_KEY_SHIFT   7   /* Aa key */
#define TCA_KEY_OPT     8
/* IDs 9, 10 — matrix holes, never generated */
#define TCA_KEY_2       11
#define TCA_KEY_W       12
#define TCA_KEY_A       13
#define TCA_KEY_ALT     14
#define TCA_KEY_3       15
#define TCA_KEY_E       16
#define TCA_KEY_S       17
#define TCA_KEY_Z       18
/* IDs 19, 20 — matrix holes */
#define TCA_KEY_4       21
#define TCA_KEY_R       22
#define TCA_KEY_D       23
#define TCA_KEY_X       24
#define TCA_KEY_5       25
#define TCA_KEY_T       26
#define TCA_KEY_F       27
#define TCA_KEY_C       28
/* IDs 29, 30 — matrix holes */
#define TCA_KEY_6       31
#define TCA_KEY_Y       32
#define TCA_KEY_G       33
#define TCA_KEY_V       34
#define TCA_KEY_7       35
#define TCA_KEY_U       36
#define TCA_KEY_H       37
#define TCA_KEY_B       38
/* IDs 39, 40 — matrix holes */
#define TCA_KEY_8       41
#define TCA_KEY_I       42
#define TCA_KEY_J       43
#define TCA_KEY_N       44
#define TCA_KEY_9       45
#define TCA_KEY_O       46
#define TCA_KEY_K       47
#define TCA_KEY_M       48
/* IDs 49, 50 — matrix holes */
#define TCA_KEY_0       51
#define TCA_KEY_P       52
#define TCA_KEY_L       53
#define TCA_KEY_LARROW  54   /* ← — InputKeyLeft  */
#define TCA_KEY_MINUS   55
#define TCA_KEY_LBRACE  56
#define TCA_KEY_UARROW  57   /* ↑ — InputKeyUp    */
#define TCA_KEY_DARROW  58   /* ↓ — InputKeyDown  */
/* IDs 59, 60 — matrix holes */
#define TCA_KEY_EQUALS  61
#define TCA_KEY_RBRACE  62
#define TCA_KEY_SQUOTE  63
#define TCA_KEY_RARROW  64   /* → — InputKeyRight */
#define TCA_KEY_DEL     65   /* Backspace */
#define TCA_KEY_BSLASH  66
#define TCA_KEY_ENTER   67   /* — InputKeyOk */
#define TCA_KEY_SPACE   68

/* =========================================================================
 * Key info table — every valid TCA8418 key ID mapped to base/shift chars
 * Index: key_id (1-68). Entries for matrix holes are {0, 0} = "invalid".
 * base_char=0  means the key is a modifier/special (no printable character).
 * ========================================================================= */
typedef struct {
    char base;   /* Unshifted character; 0 = non-printable/modifier */
    char shift;  /* Shifted character;  0 = same as base or N/A     */
} TcaKeyInfo;

static const TcaKeyInfo tca_key_table[69] = {
    /* 0  */ { 0,   0   },  /* unused (IDs are 1-based) */
    /* 1  */ { '`', '~' },  /* esc  / ~ */
    /* 2  */ { '\t', '\t'}, /* tab */
    /* 3  */ { 0,   0   },  /* fn (modifier) */
    /* 4  */ { 0,   0   },  /* ctrl (modifier) */
    /* 5  */ { '1', '!' },
    /* 6  */ { 'q', 'Q' },
    /* 7  */ { 0,   0   },  /* shift / Aa (modifier) */
    /* 8  */ { 0,   0   },  /* opt (modifier) */
    /* 9  */ { 0,   0   },  /* matrix hole */
    /* 10 */ { 0,   0   },  /* matrix hole */
    /* 11 */ { '2', '@' },
    /* 12 */ { 'w', 'W' },
    /* 13 */ { 'a', 'A' },
    /* 14 */ { 0,   0   },  /* alt (modifier) */
    /* 15 */ { '3', '#' },
    /* 16 */ { 'e', 'E' },
    /* 17 */ { 's', 'S' },
    /* 18 */ { 'z', 'Z' },
    /* 19 */ { 0,   0   },  /* matrix hole */
    /* 20 */ { 0,   0   },  /* matrix hole */
    /* 21 */ { '4', '$' },
    /* 22 */ { 'r', 'R' },
    /* 23 */ { 'd', 'D' },
    /* 24 */ { 'x', 'X' },
    /* 25 */ { '5', '%' },
    /* 26 */ { 't', 'T' },
    /* 27 */ { 'f', 'F' },
    /* 28 */ { 'c', 'C' },
    /* 29 */ { 0,   0   },  /* matrix hole */
    /* 30 */ { 0,   0   },  /* matrix hole */
    /* 31 */ { '6', '^' },
    /* 32 */ { 'y', 'Y' },
    /* 33 */ { 'g', 'G' },
    /* 34 */ { 'v', 'V' },
    /* 35 */ { '7', '&' },
    /* 36 */ { 'u', 'U' },
    /* 37 */ { 'h', 'H' },
    /* 38 */ { 'b', 'B' },
    /* 39 */ { 0,   0   },  /* matrix hole */
    /* 40 */ { 0,   0   },  /* matrix hole */
    /* 41 */ { '8', '*' },
    /* 42 */ { 'i', 'I' },
    /* 43 */ { 'j', 'J' },
    /* 44 */ { 'n', 'N' },
    /* 45 */ { '9', '(' },
    /* 46 */ { 'o', 'O' },
    /* 47 */ { 'k', 'K' },
    /* 48 */ { 'm', 'M' },
    /* 49 */ { 0,   0   },  /* matrix hole */
    /* 50 */ { 0,   0   },  /* matrix hole */
    /* 51 */ { '0', ')' },
    /* 52 */ { 'p', 'P' },
    /* 53 */ { 'l', 'L' },
    /* 54 */ { 0,   0   },  /* ← arrow (nav) */
    /* 55 */ { '-', '_' },
    /* 56 */ { '[', '{' },
    /* 57 */ { 0,   0   },  /* ↑ arrow (nav) */
    /* 58 */ { 0,   0   },  /* ↓ arrow (nav) */
    /* 59 */ { 0,   0   },  /* matrix hole */
    /* 60 */ { 0,   0   },  /* matrix hole */
    /* 61 */ { '=', '+' },
    /* 62 */ { ']', '}' },
    /* 63 */ { '\'','"' },
    /* 64 */ { 0,   0   },  /* → arrow (nav) */
    /* 65 */ { '\b','\b'},  /* backspace / del */
    /* 66 */ { '\\','|' },
    /* 67 */ { '\n','\n'},  /* enter */
    /* 68 */ { ' ', ' ' },  /* space */
};

/* Returns true if key_id is a valid TCA8418 key (not a matrix hole, not out-of-range) */
static inline bool tca_key_is_valid(uint8_t key_id) {
    if (key_id == 0 || key_id > 68) return false;
    /* Matrix holes: 9, 10, 19, 20, 29, 30, 39, 40, 49, 50, 59, 60 */
    if (key_id >= 9  && key_id <= 10) return false;
    if (key_id >= 19 && key_id <= 20) return false;
    if (key_id >= 29 && key_id <= 30) return false;
    if (key_id >= 39 && key_id <= 40) return false;
    if (key_id >= 49 && key_id <= 50) return false;
    if (key_id >= 59 && key_id <= 60) return false;
    return true;
}

/* =========================================================================
 * Per-key state (navigation keys only)
 * ========================================================================= */
typedef struct {
    InputKey  furi_key;
    uint8_t   tca_id;
    bool      pressed;
    uint32_t  press_started_at;
    bool      long_press_sent;
    uint32_t  last_repeat_at;
} NavKeyState;

static NavKeyState nav_states[6] = {
    { InputKeyBack,  KB_ADV_KEY_BACK,  false, 0, false, 0 },
    { InputKeyOk,    KB_ADV_KEY_OK,    false, 0, false, 0 },
    { InputKeyUp,    KB_ADV_KEY_UP,    false, 0, false, 0 },
    { InputKeyDown,  KB_ADV_KEY_DOWN,  false, 0, false, 0 },
    { InputKeyLeft,  KB_ADV_KEY_LEFT,  false, 0, false, 0 },
    { InputKeyRight, KB_ADV_KEY_RIGHT, false, 0, false, 0 },
};

/* Modifier state — tracked for text resolution */
typedef struct {
    bool shift;  /* Aa key held (TCA_KEY_SHIFT=7) */
    bool ctrl;   /* Ctrl key held (TCA_KEY_CTRL=4) */
    bool fn;     /* Fn key held (TCA_KEY_FN=3) */
    bool alt;    /* Alt key held (TCA_KEY_ALT=14) */
    bool opt;    /* Opt key held (TCA_KEY_OPT=8) */
} ModifierState;

static ModifierState mods = {0};

/* =========================================================================
 * I²C helpers (legacy driver)
 * ========================================================================= */
static esp_err_t tca_write(uint8_t reg, uint8_t val) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (KB_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(KB_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t tca_read(uint8_t reg, uint8_t *out) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (KB_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (KB_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, out, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(KB_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return ret;
}

/* =========================================================================
 * TCA8418 hardware init
 * =========================================================================
 * The TCA8418 boots in sleep mode — it will never report keypresses until
 * the CFG register is written. This is the mandatory wake sequence.
 *
 * KP_GPIO1 covers ROW0-ROW7 and COL0 of the key matrix config register.
 * KP_GPIO2 covers COL1-COL9.
 *
 * For the Cardputer-ADV (4 phys rows, 14 phys cols mapped to TCA rows 0-7,
 * cols scattered across C0–C9 with gaps):
 *   KP_GPIO1 = 0xFF — rows R0-R7 as keypad (bits 0-7 set)
 *   KP_GPIO2 = 0xFF — cols C0-C7 as keypad (bits 0-7 set; matches wiring)
 *
 * *** If keys still don't respond after this init, run i2c_scan and confirm
 * 0x34 is present, then check KP_GPIO1/2 against the schematic. The M5Stack
 * schematic PDF is at:
 * https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1178/Sch_M5CardputerAdv_v1.0...pdf
 * ========================================================================= */
static esp_err_t tca8418_hw_init(void) {
    esp_err_t ret;

    /*
     * KP_GPIO1 (0x1D): bits 0-3 = ROW0-ROW3 as keypad scan rows.
     * KP_GPIO2 (0x1E): bits 0-7 = COL0-COL7 as keypad scan columns.
     *   Bit 8 of the column count (COL8/COL9) lives in KP_GPIO3 bits 0-1.
     * KP_GPIO3 (0x1F): bits 0-1 = COL8-COL9 as keypad.
     *
     * The Cardputer-ADV uses all 10 TCA column lines (C0-C9, but with 6
     * physical column pairs skipping TCA columns at positions 2, 12, 22,
     * 32, 42, 52 in the key numbering — see file header). Enable all of them.
     * It also uses all 8 rows (R0-R7).
     */
    ret = tca_write(TCA8418_REG_KP_GPIO1, 0xFF); /* R0-R7 = keypad rows */
    if (ret != ESP_OK) return ret;
    ret = tca_write(TCA8418_REG_KP_GPIO2, 0xFF); /* C0-C7 = keypad cols */
    if (ret != ESP_OK) return ret;
    ret = tca_write(TCA8418_REG_KP_GPIO3, 0x03); /* C8-C9 = keypad cols */
    if (ret != ESP_OK) return ret;

    /* Enable debounce on all configured pins */
    ret = tca_write(TCA8418_REG_DEBOUNCE1, 0xFF);
    if (ret != ESP_OK) return ret;
    ret = tca_write(TCA8418_REG_DEBOUNCE2, 0xFF);
    if (ret != ESP_OK) return ret;
    ret = tca_write(TCA8418_REG_DEBOUNCE3, 0x03);
    if (ret != ESP_OK) return ret;

    /*
     * CFG register:
     *   KE_IEN   (bit 0) = 1 — enable key event interrupt
     *   INT_CFG  (bit 1) = 1 — INT pin pulses on event (cleaner for polling)
     *   OVR_FLOW_M (bit 2) = 1 — mask overflow interrupt (don't fire on FIFO full)
     *
     * This write also wakes the TCA8418 from sleep mode (default CFG=0x00).
     */
    ret = tca_write(TCA8418_REG_CFG,
                    TCA8418_CFG_KE_IEN |
                    TCA8418_CFG_INT_CFG |
                    TCA8418_CFG_OVR_FLOW_M);
    if (ret != ESP_OK) return ret;

    /* Clear any stale interrupt flag */
    ret = tca_write(TCA8418_REG_INT_STAT, 0x01);
    return ret;
}

/* =========================================================================
 * Input publishing helpers
 * ========================================================================= */
static void publish_nav(
    FuriPubSub* pubsub,
    InputKey    key,
    InputType   type,
    uint32_t    sequence)
{
    InputEvent event = {
        .sequence_source  = INPUT_SEQUENCE_SOURCE_KEYBOARD,
        .sequence_counter = sequence,
        .key              = key,
        .type             = type,
    };
    furi_pubsub_publish(pubsub, &event);
}

/*
 * Resolve the character for a key press given current modifier state.
 * Returns 0 if the key has no printable character in the current layer.
 *
 * Fn layer: `;`→up, `,`→left, `.`→down, `/`→right are handled as nav
 * events directly — they don't appear as text characters here.
 */
static char resolve_text_char(uint8_t key_id, const ModifierState* m) {
    if (key_id == 0 || key_id > 68) return 0;
    const TcaKeyInfo* info = &tca_key_table[key_id];
    if (info->base == 0) return 0;  /* Modifier or arrow key — no character */

    /* Fn layer number keys → F1-F10 (not text, skip) */
    if (m->fn) {
        switch (key_id) {
        case TCA_KEY_ESC:    return 0x1B; /* ESC character (ASCII 27) when Fn+` */
        case TCA_KEY_DEL:    return 0x7F; /* DEL character (forward delete) */
        /* Numbers become F-keys — not text characters */
        default: return 0;
        }
    }

    /* Shift or ctrl forces uppercase / shifted symbol */
    if (m->shift || m->ctrl) {
        return info->shift ? info->shift : info->base;
    }
    return info->base;
}

/* Install the legacy I2C master driver for the TCA8418 keyboard bus.
 * The ADV repo performed this inside furi_hal_i2c_get_bus_node(); the current
 * base's furi_hal_i2c HAL has no such hook (and uses the newer i2c_master
 * driver on a different port), so this target owns KB_I2C_PORT itself via the
 * legacy API. Guarded so repeated init calls are no-ops; INVALID_STATE means
 * the driver is already installed, which is fine. */
static bool kb_i2c_installed = false;
static esp_err_t kb_i2c_install(void) {
    if(kb_i2c_installed) return ESP_OK;
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = KB_PIN_SDA,
        .scl_io_num = KB_PIN_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = KB_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(KB_I2C_PORT, &conf);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = i2c_driver_install(KB_I2C_PORT, conf.mode, 0, 0, 0);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    kb_i2c_installed = true;
    ESP_LOGI(TAG, "TCA8418 legacy I2C on GPIO%d/GPIO%d (port %d)", KB_PIN_SDA, KB_PIN_SCL, KB_I2C_PORT);
    return ESP_OK;
}

/* =========================================================================
 * Public interface
 * ========================================================================= */

/* TEMP DIAGNOSTIC: probe every address on the keyboard I2C bus and log which
 * ACK. Used to locate the ES8311 audio codec (expected 0x18) and BMI270 (0x68)
 * so the speaker driver can address the codec. Remove once audio is wired up. */
static void kb_i2c_scan(void) {
    ESP_LOGW(TAG, "I2C scan on GPIO%d/GPIO%d (port %d):", KB_PIN_SDA, KB_PIN_SCL, KB_I2C_PORT);
    for(uint8_t addr = 0x08; addr < 0x78; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(KB_I2C_PORT, cmd, pdMS_TO_TICKS(20));
        i2c_cmd_link_delete(cmd);
        if(ret == ESP_OK) {
            ESP_LOGW(TAG, "  I2C device found at 0x%02X", addr);
        }
    }
    ESP_LOGW(TAG, "I2C scan done");
}

void target_input_init(void) {
    /* --- I²C master bus for the TCA8418 (legacy driver, self-contained) --- */
    if(kb_i2c_install() != ESP_OK) {
        ESP_LOGE(TAG, "Keyboard I2C bus init failed");
        return;
    }

    kb_i2c_scan();

    /* --- Initialise TCA8418 --- */
    esp_err_t ret = tca8418_hw_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tca8418_hw_init failed: %s", esp_err_to_name(ret));
        return;
    }

    /* Verify the TCA8418 is alive: read CFG back */
    uint8_t cfg_readback = 0;
    if (tca_read(TCA8418_REG_CFG, &cfg_readback) == ESP_OK) {
        ESP_LOGI(TAG, "TCA8418 CFG readback=0x%02X (expect 0x07)", cfg_readback);
    } else {
        ESP_LOGW(TAG, "TCA8418 CFG readback failed — check I2C addr (0x%02X) and SDA/SCL (GPIO%d/GPIO%d)",
                 KB_I2C_ADDR, KB_PIN_SDA, KB_PIN_SCL);
    }

    ESP_LOGI(TAG, "Cardputer-ADV TCA8418 input initialised (back=id%u ok=id%u up=id%u down=id%u left=id%u right=id%u)",
             TCA_KEY_ESC, TCA_KEY_ENTER, TCA_KEY_UARROW,
             TCA_KEY_DARROW, TCA_KEY_LARROW, TCA_KEY_RARROW);
}

/*
 * target_input_poll — drain TCA8418 FIFO and publish input events.
 *
 * @param pubsub          Nav event pubsub (InputEvent, existing Flipper channel)
 * @param sequence_counter  Incrementing sequence number (caller owns)
 */
void target_input_poll(
    FuriPubSub* pubsub,
    uint32_t*   sequence_counter)
{
    FuriPubSub* text_pubsub = pubsub; // publish typed chars on the same channel the GUI listens to
    uint32_t now              = furi_get_tick();
    uint32_t long_press_ticks = furi_ms_to_ticks(INPUT_LONG_PRESS_MS);
    uint32_t repeat_ticks     = furi_ms_to_ticks(INPUT_REPEAT_MS);

    /* --- Drain the TCA8418 FIFO ---------------------------------------- */
    uint8_t event_count = 0;
    if (tca_read(TCA8418_REG_KEY_LCK_EC, &event_count) != ESP_OK) goto long_press_check;
    event_count &= 0x0F;  /* Low nibble = event count (0-10) */

    for (uint8_t i = 0; i < event_count; i++) {
        uint8_t raw = 0;
        if (tca_read(TCA8418_REG_KEY_EVENT_A, &raw) != ESP_OK) break;

        bool    pressed = (raw & TCA8418_KEY_PRESS_MASK) != 0;
        uint8_t key_id  = raw & TCA8418_KEY_ID_MASK;

        if (key_id == TCA8418_FIFO_EMPTY) break;

        /* Validate: warn once for truly unknown IDs */
        if (!tca_key_is_valid(key_id)) {
            ESP_LOGW(TAG, "TCA8418: unexpected key_id=0x%02X(%u) — not in known key table",
                     key_id, key_id);
            continue;
        }

        /* Debounce check (presses only; releases must always pass) */
        if(!should_process_key(key_id, pressed)) continue;

        ESP_LOGD(TAG, "TCA8418 key_id=%u (%s) %s",
                 key_id, tca_key_table[key_id].base ? (char[]){tca_key_table[key_id].base, 0} : "mod",
                 pressed ? "PRESS" : "RELEASE");

        /* --- Update modifier state --- */
        if (key_id == TCA_KEY_SHIFT) { mods.shift = pressed; continue; }
        if (key_id == TCA_KEY_CTRL)  { mods.ctrl  = pressed; continue; }
        if (key_id == TCA_KEY_FN)    { mods.fn    = pressed; continue; }
        if (key_id == TCA_KEY_ALT)   { mods.alt   = pressed; continue; }
        if (key_id == TCA_KEY_OPT)   { mods.opt   = pressed; continue; }

        /* --- Navigation key handling --- */
        NavKeyState* nav = NULL;
        for (int j = 0; j < 6; j++) {
            if (nav_states[j].tca_id == key_id) { nav = &nav_states[j]; break; }
        }

        /* Special case: Both Esc (1) and Del (65) can act as Back */
        if (!nav && key_id == 1) {
             for (int j = 0; j < 6; j++) {
                if (nav_states[j].furi_key == InputKeyBack) { nav = &nav_states[j]; break; }
            }
        }

        /* NOTE: W/A/S/D/L/R and Space used to be remapped to nav here ("intuitive
         * WASD nav"), but that made them un-typeable — the user couldn't enter
         * those letters in a WiFi password. The ADV has dedicated arrow keys
         * (Fn+;/,/./ ) plus Enter (OK) and Del/Esc (Back) for navigation, so
         * those letter keys now fall through to the text path and type normally. */

        if (nav) {
            if (pressed) {
                nav->pressed           = true;
                nav->press_started_at  = now;
                nav->long_press_sent   = false;
                publish_nav(pubsub, nav->furi_key, InputTypePress, ++(*sequence_counter));
            } else {
                nav->pressed = false;
                if (!nav->long_press_sent) {
                    publish_nav(pubsub, nav->furi_key, InputTypeShort, ++(*sequence_counter));
                }
                publish_nav(pubsub, nav->furi_key, InputTypeRelease, ++(*sequence_counter));
            }
            continue;
        }

        /* --- Text key handling ----------------------------------------- */
        if (pressed && text_pubsub) {
            char ch = resolve_text_char(key_id, &mods);
            if (ch != 0) {
                /* Publish as InputTypeText; key field carries ASCII value.
                 * Receivers check type==InputTypeText to distinguish from nav. */
                InputEvent event = {
                    .sequence_source  = INPUT_SEQUENCE_SOURCE_KEYBOARD,
                    .sequence_counter = ++(*sequence_counter),
                    .key              = (InputKey)ch,
                    .type             = InputTypeText,
                };
                furi_pubsub_publish(text_pubsub, &event);
            }
        }
    }

    /* Clear interrupt flag after draining */
    tca_write(TCA8418_REG_INT_STAT, 0x01);

long_press_check:
    /* --- Long-press and repeat for held nav keys ----------------------- */
    for (int i = 0; i < 6; i++) {
        NavKeyState* nav = &nav_states[i];
        if (!nav->pressed) continue;

        uint32_t held = now - nav->press_started_at;
        if (!nav->long_press_sent && held >= long_press_ticks) {
            nav->long_press_sent = true;
            nav->last_repeat_at  = now;
            publish_nav(pubsub, nav->furi_key, InputTypeLong, ++(*sequence_counter));
        } else if (nav->long_press_sent && (now - nav->last_repeat_at) >= repeat_ticks) {
            nav->last_repeat_at = now;
            publish_nav(pubsub, nav->furi_key, InputTypeRepeat, ++(*sequence_counter));
        }
    }
}
