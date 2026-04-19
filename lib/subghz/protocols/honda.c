#include "honda.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "../blocks/custom_btn_i.h"

#define TAG "SubGhzProtocolHonda"

/* ══════════════════════════════════════════════════════════════════════════
 * Rolling-code tables
 * ════════════════════════════════════════════════════════════════════════ */
static const uint8_t honda_table_a[16][16] = { HONDA_TABLE_A };
static const uint8_t honda_table_b[16][16] = { HONDA_TABLE_B };
static const uint8_t honda_table_c[16][16] = { HONDA_TABLE_C };
static const uint8_t honda_table_d[16][16] = { HONDA_TABLE_D };
static const uint8_t honda_table_e[16][16] __attribute__((unused)) = { HONDA_TABLE_E };

static const SubGhzBlockConst subghz_protocol_honda_const = {
    .te_short                = HONDA_TE_SHORT,
    .te_long                 = HONDA_TE_LONG,
    .te_delta                = HONDA_TE_DELTA,
    .min_count_bit_for_found = HONDA_MIN_BITS,
};

/* ══════════════════════════════════════════════════════════════════════════
 * Bit-reversal helpers
 * ════════════════════════════════════════════════════════════════════════ */
static inline uint8_t _bit_rev8(uint8_t v) {
    v = (uint8_t)(((v & 0xF0u) >> 4) | ((v & 0x0Fu) << 4));
    v = (uint8_t)(((v & 0xCCu) >> 2) | ((v & 0x33u) << 2));
    v = (uint8_t)(((v & 0xAAu) >> 1) | ((v & 0x55u) << 1));
    return v;
}
static inline uint8_t _bit_rev4(uint8_t v) {
    return (uint8_t)(_bit_rev8(v & 0x0Fu) >> 4);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Frame data structure
 * ════════════════════════════════════════════════════════════════════════ */
typedef struct {
    bool     type_b;
    uint8_t  type_b_header;
    uint8_t  button;
    uint32_t serial;
    uint32_t counter;
    uint8_t  checksum;
    uint8_t  mode;
    uint8_t  hw_mode;
} HondaFrameData;

/* ══════════════════════════════════════════════════════════════════════════
 * Checksum / counter helpers
 * ════════════════════════════════════════════════════════════════════════ */
static void _honda_to_buf(const HondaFrameData* f, uint8_t buf[11]) {
    memset(buf, 0, 11u);
    if(!f->type_b) {
        buf[0] = (uint8_t)((f->button << 4) | ((f->serial >> 24) & 0x0Fu));
        buf[1] = (uint8_t)((f->serial >> 16) & 0xFFu);
        buf[2] = (uint8_t)((f->serial >>  8) & 0xFFu);
        buf[3] = (uint8_t)( f->serial        & 0xFFu);
        buf[4] = (uint8_t)((f->counter >> 16) & 0xFFu);
        buf[5] = (uint8_t)((f->counter >>  8) & 0xFFu);
        buf[6] = (uint8_t)( f->counter        & 0xFFu);
        buf[7] = (uint8_t)((f->mode & 0x0Fu) << 4);
        buf[8] = f->checksum;
    } else {
        buf[0] = (uint8_t)((f->type_b_header << 4) | (f->button & 0x0Fu));
        buf[1] = (uint8_t)((f->serial >> 20) & 0xFFu);
        buf[2] = (uint8_t)((f->serial >> 12) & 0xFFu);
        buf[3] = (uint8_t)((f->serial >>  4) & 0xFFu);
        buf[4] = (uint8_t)(((f->serial & 0x0Fu) << 4) |
                            ((f->counter >> 20) & 0x0Fu));
        buf[5] = (uint8_t)((f->counter >> 12) & 0xFFu);
        buf[6] = (uint8_t)((f->counter >>  4) & 0xFFu);
        buf[7] = (uint8_t)(((f->mode & 0x0Fu) << 4) | (f->counter & 0x0Fu));
        buf[8] = f->checksum;
    }
}

static uint8_t _honda_rolling_checksum(const uint8_t buf[11], bool mode_is_c) {
    uint8_t cnt_byte  = buf[3];
    uint8_t prev_csum = buf[8];
    const uint8_t (*tbl_lo)[16] = mode_is_c ? honda_table_b : honda_table_a;
    const uint8_t (*tbl_hi)[16] = honda_table_d;
    const uint8_t (*tbl_pm)[16] = honda_table_c;
    uint8_t new_lo = prev_csum & 0x0Fu;
    uint8_t new_hi = (prev_csum >> 4) & 0x0Fu;
    uint8_t idx = _bit_rev8(cnt_byte) & 0x0Fu;
    for(uint8_t row = 0; row < 16u; row++) {
        if(tbl_lo[row][idx] == (prev_csum & 0x0Fu)) {
            new_lo = tbl_pm[row][idx];
            break;
        }
    }
    uint8_t idx_hi = _bit_rev8(cnt_byte >> 4) & 0x0Fu;
    for(uint8_t row = 0; row < 16u; row++) {
        if(tbl_hi[row][idx_hi] == ((prev_csum >> 4) & 0x0Fu)) {
            new_hi = tbl_pm[row][idx_hi];
            break;
        }
    }
    return (uint8_t)((new_hi << 4) | new_lo);
}

static uint8_t _honda_xor_checksum(const uint8_t* data, uint8_t len) {
    uint8_t c = 0;
    for(uint8_t i = 0; i < len; i++) c ^= data[i];
    return c;
}

static void _honda_counter_increment(HondaFrameData* f) {
    uint8_t buf[11];
    _honda_to_buf(f, buf);
    uint8_t lo = _bit_rev4(buf[3] & 0x0Fu);
    lo = (lo + 1u) & 0x0Fu;
    buf[3] = (buf[3] & 0xF0u) | _bit_rev4(lo);
    if((f->counter & 0x0Fu) == 0x0Fu) {
        uint8_t hi = _bit_rev4((buf[3] >> 4) & 0x0Fu);
        hi = (hi + 1u) & 0x0Fu;
        buf[3] = (buf[3] & 0x0Fu) | (uint8_t)(_bit_rev4(hi) << 4);
        if(((f->counter >> 4) & 0x0Fu) == 0x0Fu) {
            uint8_t b2lo = _bit_rev4(buf[2] & 0x0Fu);
            b2lo = (b2lo + 1u) & 0x0Fu;
            buf[2] = (buf[2] & 0xF0u) | _bit_rev4(b2lo);
        }
    }
    f->counter = (f->counter + 1u) & 0x00FFFFFFu;
    bool mode_was_c = (f->mode == 0xCu);
    f->mode = mode_was_c ? 0x2u : 0xCu;
    _honda_to_buf(f, buf);
    f->checksum = _honda_rolling_checksum(buf, !mode_was_c);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Pack / unpack
 * ════════════════════════════════════════════════════════════════════════ */
static uint64_t _honda_pack(const HondaFrameData* f) {
    uint64_t v = 0;
    v |= (uint64_t)(f->type_b ? 1u : 0u)           << 63;
    v |= (uint64_t)(f->type_b_header & 0x07u)       << 60;
    v |= (uint64_t)(f->button        & 0x0Fu)       << 56;
    v |= (uint64_t)(f->serial        & 0x0FFFFFFFu) << 28;
    v |= (uint64_t)(f->counter       & 0x00FFFFFFu) <<  4;
    v |= (uint64_t)(f->hw_mode       & 0x0Fu);
    return v;
}

static void _honda_unpack(uint64_t raw, HondaFrameData* f) {
    f->type_b        = (raw >> 63) & 0x01u;
    f->type_b_header = (uint8_t)((raw >> 60) & 0x07u);
    f->button        = (uint8_t)((raw >> 56) & 0x0Fu);
    f->serial        = (uint32_t)((raw >> 28) & 0x0FFFFFFFu);
    f->counter       = (uint32_t)((raw >>  4) & 0x00FFFFFFu);
    f->hw_mode       = (uint8_t)(raw & 0x0Fu);
    f->mode          = 0x2u;
    uint8_t buf[11];
    _honda_to_buf(f, buf);
    f->checksum = _honda_xor_checksum(buf, 7u);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Button mapping
 * ════════════════════════════════════════════════════════════════════════ */
uint8_t subghz_protocol_honda_btn_to_custom(uint8_t btn) {
    switch(btn) {
    case HONDA_BTN_LOCK:       return 1u;
    case HONDA_BTN_UNLOCK:     return 2u;
    case HONDA_BTN_TRUNK:      return 3u;
    case HONDA_BTN_PANIC:      return 4u;
    case HONDA_BTN_RSTART:     return 5u;
    default:                   return 1u;
    }
}
uint8_t subghz_protocol_honda_custom_to_btn(uint8_t custom) {
    switch(custom) {
    case 1u: return HONDA_BTN_LOCK;
    case 2u: return HONDA_BTN_UNLOCK;
    case 3u: return HONDA_BTN_TRUNK;
    case 4u: return HONDA_BTN_PANIC;
    case 5u: return HONDA_BTN_RSTART;
    default: return HONDA_BTN_LOCK;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * TIMING ESTIMATOR
 * ════════════════════════════════════════════════════════════════════════ */

static void _sort_u32(uint32_t* arr, uint16_t n) {
    for(uint16_t i = 1; i < n; i++) {
        uint32_t key = arr[i];
        int16_t  j   = (int16_t)i - 1;
        while(j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

static uint32_t _estimate_T(const uint32_t* durations, uint16_t count) {
    if(count < 4u) return 0u;

    uint32_t tmp[HONDA_FSK_COLLECT_N];
    uint16_t n = (count < HONDA_FSK_COLLECT_N) ? count : HONDA_FSK_COLLECT_N;
    for(uint16_t i = 0u; i < n; i++) tmp[i] = durations[i];
    _sort_u32(tmp, n);

    FURI_LOG_D(TAG, "T-est: n=%u min=%lu med=%lu max=%lu",
               n,
               (unsigned long)tmp[0],
               (unsigned long)tmp[n / 2u],
               (unsigned long)tmp[n - 1u]);

    uint32_t T = tmp[n / 4u];

    if(T < 40u || T > 800u) {
        FURI_LOG_D(TAG, "T-est: implausible T=%lu, reject", (unsigned long)T);
        return 0u;
    }

    FURI_LOG_I(TAG, "T-est: T=%lu µs (baud~%lu)", (unsigned long)T,
               (unsigned long)(1000000u / (T * 2u)));
    return T;
}

/* ══════════════════════════════════════════════════════════════════════════
 * RAW EDGE BUFFER
 * ════════════════════════════════════════════════════════════════════════ */
typedef struct {
    bool     level;
    uint32_t duration;
} HondaEdge;

/* ══════════════════════════════════════════════════════════════════════════
 * DECODER STATE
 * ════════════════════════════════════════════════════════════════════════ */
typedef struct SubGhzProtocolDecoderHonda {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder        decoder;
    SubGhzBlockGeneric        generic;

    HondaEdge edges[HONDA_RAW_EDGE_BUF];
    uint16_t  edge_count;

    uint32_t  dur_collect[HONDA_FSK_COLLECT_N];
    uint16_t  dur_count;
    uint32_t  T_est;

    uint8_t  hbits[HONDA_MAN_BIT_BUF];
    uint16_t hbit_count;

    HondaFrameData frame;
    bool           frame_valid;

    uint32_t dbg_min_dur;
    uint32_t dbg_max_dur;
    uint64_t dbg_sum_dur;
    uint32_t dbg_dur_count;
} SubGhzProtocolDecoderHonda;

/* ══════════════════════════════════════════════════════════════════════════
 * ENCODER STATE
 * ════════════════════════════════════════════════════════════════════════ */
#define HONDA_ENC_BUF_SIZE  1024u

typedef struct SubGhzProtocolEncoderHonda {
    SubGhzProtocolEncoderBase   base;
    SubGhzProtocolBlockEncoder  encoder;
    SubGhzBlockGeneric          generic;
    HondaFrameData              frame;
    uint8_t                     active_button;
} SubGhzProtocolEncoderHonda;

/* ══════════════════════════════════════════════════════════════════════════
 * Forward declarations
 * ════════════════════════════════════════════════════════════════════════ */
const SubGhzProtocolDecoder subghz_protocol_honda_decoder;
const SubGhzProtocolEncoder subghz_protocol_honda_encoder;
const SubGhzProtocol        subghz_protocol_honda;

/* ══════════════════════════════════════════════════════════════════════════
 * DECODER RESET
 * ════════════════════════════════════════════════════════════════════════ */
static void _decoder_reset_burst(SubGhzProtocolDecoderHonda* inst) {

    inst->edge_count  = 0u;
    inst->dur_count   = 0u;
    inst->hbit_count  = 0u;
    inst->dbg_min_dur = 0xFFFFFFFFu;
    inst->dbg_max_dur = 0u;
    inst->dbg_sum_dur = 0u;
    inst->dbg_dur_count = 0u;
}

static void _decoder_full_reset(SubGhzProtocolDecoderHonda* inst) {
    _decoder_reset_burst(inst);
    inst->T_est = 0u;
}

/* ══════════════════════════════════════════════════════════════════════════
 * COLLECT edges into buffer, filter noise
 * ════════════════════════════════════════════════════════════════════════ */
static void _collect_edge(SubGhzProtocolDecoderHonda* inst,
                          bool level, uint32_t duration) {

    if(duration < HONDA_FSK_DUR_MIN_US || duration > HONDA_FSK_DUR_MAX_US) {
        return;
    }

    if(duration < inst->dbg_min_dur) inst->dbg_min_dur = duration;
    if(duration > inst->dbg_max_dur) inst->dbg_max_dur = duration;
    inst->dbg_sum_dur += duration;
    inst->dbg_dur_count++;

    if(inst->edge_count < HONDA_RAW_EDGE_BUF) {
        inst->edges[inst->edge_count].level    = level;
        inst->edges[inst->edge_count].duration = duration;
        inst->edge_count++;
    }

    if(inst->dur_count < HONDA_FSK_COLLECT_N) {
        inst->dur_collect[inst->dur_count++] = duration;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * ESTIMATE T from collected durations
 * ════════════════════════════════════════════════════════════════════════ */
static bool _update_T_estimate(SubGhzProtocolDecoderHonda* inst) {
    if(inst->T_est != 0u) return true;

    if(inst->dur_count < HONDA_FSK_COLLECT_N / 2u) {
        FURI_LOG_D(TAG, "T-est: not enough samples (%u)", inst->dur_count);
        return false;
    }

    uint32_t T = _estimate_T(inst->dur_collect, inst->dur_count);
    if(T == 0u) return false;

    inst->T_est = T;
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 * QUANTIZE edges into half-bits
 * ════════════════════════════════════════════════════════════════════════ */

static bool _quantize_to_halfbits(SubGhzProtocolDecoderHonda* inst) {
    uint32_t T   = inst->T_est;
    uint32_t tol = (T * HONDA_FSK_TOL_PCT) / 100u;

    uint32_t lo_1T = T - tol;
    uint32_t hi_1T = T + tol;
    uint32_t lo_2T = 2u * T - tol;
    uint32_t hi_2T = 2u * T + tol;

    FURI_LOG_D(TAG,
        "Quantize: T=%lu tol=%lu 1T=[%lu,%lu] 2T=[%lu,%lu]",
        (unsigned long)T,    (unsigned long)tol,
        (unsigned long)lo_1T, (unsigned long)hi_1T,
        (unsigned long)lo_2T, (unsigned long)hi_2T);

    inst->hbit_count = 0u;
    uint16_t anomalies = 0u;

    for(uint16_t i = 0u; i < inst->edge_count; i++) {
        uint32_t dur = inst->edges[i].duration;
        uint8_t  lvl = inst->edges[i].level ? 1u : 0u;

        if(dur >= lo_1T && dur <= hi_1T) {
            if(inst->hbit_count < HONDA_MAN_BIT_BUF)
                inst->hbits[inst->hbit_count++] = lvl;

        } else if(dur >= lo_2T && dur <= hi_2T) {
            if(inst->hbit_count + 1u < HONDA_MAN_BIT_BUF) {
                inst->hbits[inst->hbit_count++] = lvl;
                inst->hbits[inst->hbit_count++] = lvl;
            }
        } else {
            anomalies++;
            FURI_LOG_D(TAG, "Quantize: anomaly dur=%lu at edge %u",
                       (unsigned long)dur, i);
        }
    }

    uint32_t anomaly_pct = (inst->edge_count > 0u)
        ? ((uint32_t)anomalies * 100u) / (uint32_t)inst->edge_count
        : 0u;

    FURI_LOG_D(TAG,
        "Quantize: %u edges -> %u half-bits, anomalies=%u (%lu%%)",
        inst->edge_count,
        inst->hbit_count,
        anomalies,
        (unsigned long)anomaly_pct);

    if(anomaly_pct > 30u) {
        FURI_LOG_D(TAG, "Quantize: too many anomalies -> reject burst");
        return false;
    }

    return (inst->hbit_count >= HONDA_FSK_MIN_PREAMBLE_BITS * 2u);
}

/* ══════════════════════════════════════════════════════════════════════════
 * LOG raw half-bit stream for debugging
 * ════════════════════════════════════════════════════════════════════════ */
static void _log_halfbits(const SubGhzProtocolDecoderHonda* inst) {
    if(inst->hbit_count == 0u) return;

    char buf[65];
    uint16_t n = inst->hbit_count < 64u ? inst->hbit_count : 64u;
    for(uint16_t i = 0u; i < n; i++)
        buf[i] = inst->hbits[i] ? '1' : '0';
    buf[n] = '\0';

    FURI_LOG_D(TAG, "HalfBits[0..%u]: %s%s",
               inst->hbit_count - 1u, buf,
               inst->hbit_count > 64u ? "..." : "");
}

/* ══════════════════════════════════════════════════════════════════════════
 * MANCHESTER DECODER
 * ════════════════════════════════════════════════════════════════════════ */

static inline void _push_bit_lsb(uint8_t* out_bytes,
                                   uint16_t  bit_pos,
                                   uint8_t   bit_val) {
    uint16_t byte_idx = bit_pos / 8u;
    uint8_t  bit_idx  = (uint8_t)(bit_pos % 8u);
    if(bit_val)
        out_bytes[byte_idx] |=  (uint8_t)(1u << bit_idx);
    else
        out_bytes[byte_idx] &= (uint8_t)(~(1u << bit_idx));
}

static uint16_t _manchester_decode(
    const uint8_t* hbits,
    uint16_t       hbit_count,
    uint8_t*       out_bytes,
    uint16_t       out_max_bits,
    bool           invert
) {
    uint16_t out_bits = 0u;
    uint16_t i        = 0u;

    while(i + 1u < hbit_count && hbits[i] == hbits[i + 1u]) i++;

    if(i >= hbit_count) return 0u;

    while(i + 1u < hbit_count && out_bits < out_max_bits) {
        uint8_t h0 = hbits[i];
        uint8_t h1 = hbits[i + 1u];

        if(h0 != h1) {
            uint8_t bit;
            if(!invert)
                bit = (h0 == 1u && h1 == 0u) ? 1u : 0u;
            else
                bit = (h0 == 0u && h1 == 1u) ? 1u : 0u;

            _push_bit_lsb(out_bytes, out_bits, bit);
            out_bits++;
            i += 2u;
        } else {
            i++;
        }
    }

    return out_bits;
}

/* ══════════════════════════════════════════════════════════════════════════
 * FRAME VALIDATION
 * ════════════════════════════════════════════════════════════════════════ */
static bool _valid_button(uint8_t btn) {
    switch(btn) {
    case HONDA_BTN_LOCK:
    case HONDA_BTN_UNLOCK:
    case HONDA_BTN_TRUNK:
    case HONDA_BTN_PANIC:
    case HONDA_BTN_RSTART:
    case HONDA_BTN_LOCK2PRESS:
        return true;
    default:
        return false;
    }
}

static uint32_t _extract_bits_lsb(const uint8_t* buf,
                                    uint16_t start, uint16_t len) {
    uint32_t val = 0u;
    for(uint16_t i = 0u; i < len; i++) {
        uint16_t pos = start + i;
        uint8_t  b   = (uint8_t)((buf[pos / 8u] >> (pos % 8u)) & 1u);
        val |= (uint32_t)b << i;
    }
    return val;
}

static bool _try_parse_at_offset(
    SubGhzProtocolDecoderHonda* inst,
    const uint8_t* bits,
    uint16_t       total_bits,
    uint16_t       start
) {
    if(start + HONDA_FRAME_BITS_M1 <= total_bits) {
        uint8_t  btn     = (uint8_t)_extract_bits_lsb(bits, start +  0u,  4u);
        uint32_t serial  =          _extract_bits_lsb(bits, start +  4u, 28u);
        uint32_t counter =          _extract_bits_lsb(bits, start + 32u, 24u);
        uint8_t  mode_n  = (uint8_t)_extract_bits_lsb(bits, start + 56u,  4u);
        uint8_t  csum    = (uint8_t)_extract_bits_lsb(bits, start + 60u,  8u);

        uint8_t fb[7];
        fb[0] = (uint8_t)((btn << 4) | ((serial >> 24) & 0x0Fu));
        fb[1] = (uint8_t)((serial >> 16) & 0xFFu);
        fb[2] = (uint8_t)((serial >>  8) & 0xFFu);
        fb[3] = (uint8_t)( serial        & 0xFFu);
        fb[4] = (uint8_t)((counter >> 16) & 0xFFu);
        fb[5] = (uint8_t)((counter >>  8) & 0xFFu);
        fb[6] = (uint8_t)( counter        & 0xFFu);
        uint8_t calc = _honda_xor_checksum(fb, 7u);

        FURI_LOG_D(TAG,
            "Parse@%u M1-TA btn=%X ser=%07lX cnt=%06lX "
            "mode=%X csum=%02X calc=%02X",
            start, btn,
            (unsigned long)serial, (unsigned long)counter,
            mode_n, csum, calc);

        if(calc == csum && _valid_button(btn) &&
           serial != 0u && serial != 0x0FFFFFFFu) {
            inst->frame.type_b        = false;
            inst->frame.type_b_header = 0u;
            inst->frame.button        = btn;
            inst->frame.serial        = serial;
            inst->frame.counter       = counter;
            inst->frame.checksum      = csum;
            inst->frame.mode  = ((mode_n == 0x2u)||(mode_n == 0xCu))
                                 ? mode_n : 0x2u;
            inst->frame.hw_mode = 1u;
            inst->frame_valid   = true;
            FURI_LOG_I(TAG, "M1 TypeA OK @bit%u btn=%u ser=%07lX cnt=%06lX",
                       start, btn,
                       (unsigned long)serial, (unsigned long)counter);
            return true;
        }
    }

    if(start + HONDA_FRAME_BITS_M1 <= total_bits) {
        uint8_t  hdr     = (uint8_t)_extract_bits_lsb(bits, start +  0u,  4u);
        uint8_t  btn     = (uint8_t)_extract_bits_lsb(bits, start +  4u,  4u);
        uint32_t serial  =          _extract_bits_lsb(bits, start +  8u, 28u);
        uint32_t counter =          _extract_bits_lsb(bits, start + 36u, 24u);
        uint8_t  mode_n  = (uint8_t)_extract_bits_lsb(bits, start + 60u,  4u);
        uint8_t  csum    = (uint8_t)_extract_bits_lsb(bits, start + 64u,  8u);

        uint8_t fb[7];
        fb[0] = (uint8_t)((hdr << 4) | (btn & 0x0Fu));
        fb[1] = (uint8_t)((serial >> 20) & 0xFFu);
        fb[2] = (uint8_t)((serial >> 12) & 0xFFu);
        fb[3] = (uint8_t)((serial >>  4) & 0xFFu);
        fb[4] = (uint8_t)(((serial & 0x0Fu) << 4) |
                           ((counter >> 20) & 0x0Fu));
        fb[5] = (uint8_t)((counter >> 12) & 0xFFu);
        fb[6] = (uint8_t)((counter >>  4) & 0xFFu);
        uint8_t calc = _honda_xor_checksum(fb, 7u);

        FURI_LOG_D(TAG,
            "Parse@%u M1-TB hdr=%X btn=%X ser=%07lX cnt=%06lX "
            "csum=%02X calc=%02X",
            start, hdr, btn,
            (unsigned long)serial, (unsigned long)counter,
            csum, calc);

        if(calc == csum && _valid_button(btn) &&
           serial != 0u && serial != 0x0FFFFFFFu) {
            inst->frame.type_b        = true;
            inst->frame.type_b_header = hdr;
            inst->frame.button        = btn;
            inst->frame.serial        = serial;
            inst->frame.counter       = counter;
            inst->frame.checksum      = csum;
            inst->frame.mode  = ((mode_n == 0x2u)||(mode_n == 0xCu))
                                 ? mode_n : 0x2u;
            inst->frame.hw_mode = 1u;
            inst->frame_valid   = true;
            FURI_LOG_I(TAG,
                "M1 TypeB OK @bit%u hdr=%X btn=%u ser=%07lX cnt=%06lX",
                start, hdr, btn,
                (unsigned long)serial, (unsigned long)counter);
            return true;
        }
    }

    if(start + HONDA_FRAME_BITS_M2 <= total_bits) {
        uint8_t  btn     = (uint8_t)_extract_bits_lsb(bits, start +  0u,  4u);
        uint32_t serial  =          _extract_bits_lsb(bits, start +  4u, 28u);
        uint32_t counter =          _extract_bits_lsb(bits, start + 32u, 24u);
        uint8_t  mode_n  = (uint8_t)_extract_bits_lsb(bits, start + 56u,  4u);
        uint8_t  csum    = (uint8_t)_extract_bits_lsb(bits, start + 60u,  6u);

        uint8_t fb[7];
        fb[0] = (uint8_t)((btn << 4) | ((serial >> 24) & 0x0Fu));
        fb[1] = (uint8_t)((serial >> 16) & 0xFFu);
        fb[2] = (uint8_t)((serial >>  8) & 0xFFu);
        fb[3] = (uint8_t)( serial        & 0xFFu);
        fb[4] = (uint8_t)((counter >> 16) & 0xFFu);
        fb[5] = (uint8_t)((counter >>  8) & 0xFFu);
        fb[6] = (uint8_t)( counter        & 0xFFu);
        uint8_t calc = _honda_xor_checksum(fb, 7u) & 0x3Fu;

        if(calc == csum && _valid_button(btn) &&
           serial != 0u && serial != 0x0FFFFFFFu) {
            inst->frame.type_b        = false;
            inst->frame.type_b_header = 0u;
            inst->frame.button        = btn;
            inst->frame.serial        = serial;
            inst->frame.counter       = counter;
            inst->frame.checksum      = csum;
            inst->frame.mode  = ((mode_n == 0x2u)||(mode_n == 0xCu))
                                 ? mode_n : 0x2u;
            inst->frame.hw_mode = 2u;
            inst->frame_valid   = true;
            FURI_LOG_I(TAG, "M2 TypeA OK @bit%u btn=%u ser=%07lX cnt=%06lX",
                       start, btn,
                       (unsigned long)serial, (unsigned long)counter);
            return true;
        }
    }

    return false;
}

/* ══════════════════════════════════════════════════════════════════════════
 * SLIDING WINDOW FRAME SEARCH
 * ════════════════════════════════════════════════════════════════════════ */
static bool _search_frames(SubGhzProtocolDecoderHonda* inst,
                            const uint8_t* decoded_bits,
                            uint16_t       total_decoded_bits) {
    if(total_decoded_bits < HONDA_MIN_BITS) {
        FURI_LOG_D(TAG, "Search: only %u bits, need %u → skip",
                   total_decoded_bits, (uint16_t)HONDA_MIN_BITS);
        return false;
    }

    {
        char buf[129];
        uint16_t n = total_decoded_bits < 128u ? total_decoded_bits : 128u;
        for(uint16_t i = 0u; i < n; i++) {
            uint8_t b = (uint8_t)((decoded_bits[i / 8u] >> (i % 8u)) & 1u);
            buf[i] = b ? '1' : '0';
        }
        buf[n] = '\0';
        FURI_LOG_D(TAG, "DecodedBits[0..%u]: %s%s",
                   total_decoded_bits - 1u, buf,
                   total_decoded_bits > 128u ? "..." : "");
    }

    uint16_t max_start = total_decoded_bits > HONDA_MIN_BITS
        ? total_decoded_bits - HONDA_MIN_BITS : 0u;

    for(uint16_t start = 0u; start <= max_start; start++) {
        if(_try_parse_at_offset(inst, decoded_bits, total_decoded_bits, start))
            return true;
    }

    FURI_LOG_D(TAG, "Search: no valid frame found in %u bits",
               total_decoded_bits);
    return false;
}

/* ══════════════════════════════════════════════════════════════════════════
 * PROCESS BURST
 * ════════════════════════════════════════════════════════════════════════ */

static void _process_burst(SubGhzProtocolDecoderHonda* inst) {
    if(inst->dbg_dur_count > 0u) {
        uint32_t avg = (uint32_t)(inst->dbg_sum_dur / inst->dbg_dur_count);
        FURI_LOG_D(TAG,
            "Burst: %u edges, %lu valid durs, min=%lu avg=%lu max=%lu us",
            inst->edge_count,
            (unsigned long)inst->dbg_dur_count,
            (unsigned long)inst->dbg_min_dur,
            (unsigned long)avg,
            (unsigned long)inst->dbg_max_dur);
    }

    if(inst->edge_count < HONDA_FSK_MIN_PREAMBLE_BITS) {
        FURI_LOG_D(TAG, "Burst: too few edges (%u) -> discard",
                   inst->edge_count);
        return;
    }

    if(!_update_T_estimate(inst)) {
        FURI_LOG_D(TAG, "Burst: T estimation failed -> discard");
        return;
    }

    if(!_quantize_to_halfbits(inst)) {
        FURI_LOG_D(TAG, "Burst: quantization failed -> discard");
        return;
    }

    _log_halfbits(inst);

    uint8_t decoded_bits[HONDA_MAN_BIT_BUF / 8u + 1u];

    for(uint8_t invert = 0u; invert <= 1u; invert++) {
        memset(decoded_bits, 0, sizeof(decoded_bits));

        uint16_t n_decoded = _manchester_decode(
            inst->hbits,
            inst->hbit_count,
            decoded_bits,
            (uint16_t)(sizeof(decoded_bits) * 8u),
            (bool)invert);

        FURI_LOG_D(TAG, "Manchester conv=%s -> %u bits",
                   invert ? "B(L>H=1)" : "A(H>L=1)", n_decoded);

        if(n_decoded < HONDA_MIN_BITS) {
            FURI_LOG_D(TAG,
                "Manchester: too few bits (%u) with conv=%s",
                n_decoded, invert ? "B" : "A");
            continue;
        }

        if(_search_frames(inst, decoded_bits, n_decoded)) {
            inst->generic.data = _honda_pack(&inst->frame);
            inst->generic.data_count_bit =
                (inst->frame.hw_mode == 1u)
                    ? (uint8_t)HONDA_FRAME_BITS_M1
                    : (uint8_t)HONDA_FRAME_BITS_M2;
            inst->generic.serial = inst->frame.serial;
            inst->generic.btn    = inst->frame.button;
            inst->generic.cnt    = inst->frame.counter;

            uint8_t custom =
                subghz_protocol_honda_btn_to_custom(inst->frame.button);
            if(subghz_custom_btn_get_original() == 0u)
                subghz_custom_btn_set_original(custom);
            subghz_custom_btn_set_max(HONDA_CUSTOM_BTN_MAX);

            if(inst->base.callback)
                inst->base.callback(&inst->base, inst->base.context);

            return;
        }
    }

    FURI_LOG_D(TAG,
        "Burst: no valid frame in either Manchester convention");
}

/* ══════════════════════════════════════════════════════════════════════════
 * DECODER FEED
 * ════════════════════════════════════════════════════════════════════════ */
void subghz_protocol_decoder_honda_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderHonda* inst = context;

    bool is_gap = false;
    if(duration >= HONDA_FSK_GAP_US) {
        is_gap = true;
    } else if(inst->T_est > 0u && duration >= inst->T_est * 5u) {
        is_gap = true;
    }

    if(is_gap) {
        FURI_LOG_D(TAG, "Gap: dur=%lu µs, processing burst (%u edges)",
                   (unsigned long)duration, inst->edge_count);
        _process_burst(inst);
        _decoder_reset_burst(inst);
        return;
    }

    if(duration < HONDA_FSK_DUR_MIN_US) {
        return;
    }

    _collect_edge(inst, level, duration);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Protocol descriptor tables
 * ════════════════════════════════════════════════════════════════════════ */
const SubGhzProtocolDecoder subghz_protocol_honda_decoder = {
    .alloc         = subghz_protocol_decoder_honda_alloc,
    .free          = subghz_protocol_decoder_honda_free,
    .feed          = subghz_protocol_decoder_honda_feed,
    .reset         = subghz_protocol_decoder_honda_reset,
    .get_hash_data = subghz_protocol_decoder_honda_get_hash_data,
    .serialize     = subghz_protocol_decoder_honda_serialize,
    .deserialize   = subghz_protocol_decoder_honda_deserialize,
    .get_string    = subghz_protocol_decoder_honda_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_honda_encoder = {
    .alloc       = subghz_protocol_encoder_honda_alloc,
    .free        = subghz_protocol_encoder_honda_free,
    .deserialize = subghz_protocol_encoder_honda_deserialize,
    .stop        = subghz_protocol_encoder_honda_stop,
    .yield       = subghz_protocol_encoder_honda_yield,
};

const SubGhzProtocol subghz_protocol_honda = {
    .name    = SUBGHZ_PROTOCOL_HONDA_NAME,
    .type    = SubGhzProtocolTypeDynamic,
    .flag    = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 |
               SubGhzProtocolFlag_FM  | SubGhzProtocolFlag_Decodable |
               SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
               SubGhzProtocolFlag_Send | SubGhzProtocolFlag_Automotive,
    .decoder = &subghz_protocol_honda_decoder,
    .encoder = &subghz_protocol_honda_encoder,
};

/* ══════════════════════════════════════════════════════════════════════════
 * Decoder lifecycle
 * ════════════════════════════════════════════════════════════════════════ */
void* subghz_protocol_decoder_honda_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderHonda* inst = malloc(sizeof(SubGhzProtocolDecoderHonda));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolDecoderHonda));
    inst->base.protocol         = &subghz_protocol_honda;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->frame_valid           = false;
    _decoder_full_reset(inst);
    FURI_LOG_I(TAG, "decoder allocated (FSK adaptive mode)");
    return inst;
}

void subghz_protocol_decoder_honda_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_honda_reset(void* context) {
    furi_assert(context);
    _decoder_full_reset(context);
}

uint8_t subghz_protocol_decoder_honda_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHonda* inst = context;
    return (uint8_t)(inst->generic.data       ^
                    (inst->generic.data >>  8) ^
                    (inst->generic.data >> 16) ^
                    (inst->generic.data >> 24) ^
                    (inst->generic.data >> 32));
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_serialize(
    void* context, FlipperFormat* flipper_format, SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderHonda* inst = context;
    return subghz_block_generic_serialize(&inst->generic, flipper_format, preset);
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_deserialize(
    void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderHonda* inst = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &inst->generic, flipper_format,
        subghz_protocol_honda_const.min_count_bit_for_found);
    if(ret == SubGhzProtocolStatusOk) {
        _honda_unpack(inst->generic.data, &inst->frame);
        inst->frame_valid    = true;
        inst->generic.serial = inst->frame.serial;
        inst->generic.btn    = inst->frame.button;
        inst->generic.cnt    = inst->frame.counter;
        uint8_t custom =
            subghz_protocol_honda_btn_to_custom(inst->frame.button);
        if(subghz_custom_btn_get_original() == 0u)
            subghz_custom_btn_set_original(custom);
        subghz_custom_btn_set_max(HONDA_CUSTOM_BTN_MAX);
    }
    return ret;
}

void subghz_protocol_decoder_honda_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderHonda* inst = context;
    if(!inst->frame_valid && inst->generic.data != 0u) {
        _honda_unpack(inst->generic.data, &inst->frame);
        inst->frame_valid = true;
    }
    const char* btn_name;
    switch(inst->frame.button) {
    case HONDA_BTN_LOCK:       btn_name = "Lock";         break;
    case HONDA_BTN_UNLOCK:     btn_name = "Unlock";       break;
    case HONDA_BTN_TRUNK:      btn_name = "Trunk/Hatch";  break;
    case HONDA_BTN_PANIC:      btn_name = "Panic";        break;
    case HONDA_BTN_RSTART:     btn_name = "Remote Start"; break;
    case HONDA_BTN_LOCK2PRESS: btn_name = "Lock x2";      break;
    default:                   btn_name = "Unknown";      break;
    }
    furi_string_cat_printf(
        output,
        "%s M%u %s %ubit\r\n"
        "Btn:%s (0x%X)\r\n"
        "Ser:%07lX\r\n"
        "Cnt:%06lX Chk:%02X Mode:%X\r\n"
        "T=%luµs baud~%lu\r\n",
        inst->generic.protocol_name,
        inst->frame.hw_mode,
        inst->frame.type_b ? "TB" : "TA",
        inst->generic.data_count_bit,
        btn_name, inst->frame.button,
        (unsigned long)inst->frame.serial,
        (unsigned long)inst->frame.counter,
        inst->frame.checksum, inst->frame.mode,
        (unsigned long)inst->T_est,
        inst->T_est > 0u
            ? (unsigned long)(1000000u / (inst->T_est * 2u)) : 0ul);
}

/* ══════════════════════════════════════════════════════════════════════════
 * ENCODER
 * ════════════════════════════════════════════════════════════════════════ */
static void _honda_build_frame_bits(const HondaFrameData* f, uint8_t out[11]) {
    memset(out, 0, 11u);
#define HONDA_SET_BIT(buf, n, val) \
    do { \
        uint16_t _n = (uint16_t)(n); \
        if(val) (buf)[_n/8u] |=  (uint8_t)(1u<<(_n%8u)); \
        else    (buf)[_n/8u] &= (uint8_t)(~(1u<<(_n%8u))); \
    } while(0)

    if(!f->type_b) {
        for(int i=0;i<4; i++) HONDA_SET_BIT(out,    i, (f->button >>i)&1u);
        for(int i=0;i<28;i++) HONDA_SET_BIT(out,  4+i, (f->serial >>i)&1u);
        for(int i=0;i<24;i++) HONDA_SET_BIT(out, 32+i, (f->counter>>i)&1u);
        for(int i=0;i<4; i++) HONDA_SET_BIT(out, 56+i, (f->mode   >>i)&1u);
        uint8_t fb[7];
        fb[0]=(uint8_t)((f->button<<4)|((f->serial>>24)&0x0Fu));
        fb[1]=(uint8_t)((f->serial>>16)&0xFFu);
        fb[2]=(uint8_t)((f->serial>> 8)&0xFFu);
        fb[3]=(uint8_t)( f->serial     &0xFFu);
        fb[4]=(uint8_t)((f->counter>>16)&0xFFu);
        fb[5]=(uint8_t)((f->counter>> 8)&0xFFu);
        fb[6]=(uint8_t)( f->counter    &0xFFu);
        uint8_t csum=_honda_xor_checksum(fb,7u);
        for(int i=0;i<8;i++) HONDA_SET_BIT(out,60+i,(csum>>i)&1u);
    } else {
        for(int i=0;i<4; i++) HONDA_SET_BIT(out,    i,(f->type_b_header>>i)&1u);
        for(int i=0;i<4; i++) HONDA_SET_BIT(out,  4+i,(f->button       >>i)&1u);
        for(int i=0;i<28;i++) HONDA_SET_BIT(out,  8+i,(f->serial       >>i)&1u);
        for(int i=0;i<24;i++) HONDA_SET_BIT(out, 36+i,(f->counter      >>i)&1u);
        for(int i=0;i<4; i++) HONDA_SET_BIT(out, 60+i,(f->mode         >>i)&1u);
        uint8_t fb[7];
        fb[0]=(uint8_t)((f->type_b_header<<4)|(f->button&0x0Fu));
        fb[1]=(uint8_t)((f->serial>>20)&0xFFu);
        fb[2]=(uint8_t)((f->serial>>12)&0xFFu);
        fb[3]=(uint8_t)((f->serial>> 4)&0xFFu);
        fb[4]=(uint8_t)(((f->serial&0x0Fu)<<4)|((f->counter>>20)&0x0Fu));
        fb[5]=(uint8_t)((f->counter>>12)&0xFFu);
        fb[6]=(uint8_t)((f->counter>> 4)&0xFFu);
        uint8_t csum=_honda_xor_checksum(fb,7u);
        for(int i=0;i<8;i++) HONDA_SET_BIT(out,64+i,(csum>>i)&1u);
    }
#undef HONDA_SET_BIT
}

static void _honda_build_upload(SubGhzProtocolEncoderHonda* inst) {
    LevelDuration* buf = inst->encoder.upload;
    size_t idx = 0u;
    buf[idx++] = level_duration_make(false, (uint32_t)HONDA_GUARD_TIME_US);
    for(uint16_t p = 0u; p < (uint16_t)(HONDA_PREAMBLE_CYCLES_M1 * 2u); p++) {
        buf[idx++] = level_duration_make((p&1u)==0u, (uint32_t)HONDA_TE_SHORT);
        furi_check(idx < HONDA_ENC_BUF_SIZE - 4u);
    }
    buf[idx++] = level_duration_make(false, 740u);
    uint8_t frame_bits[11] = {0};
    _honda_build_frame_bits(&inst->frame, frame_bits);
    for(uint8_t b = 0u; b < (uint8_t)HONDA_FRAME_BITS_M1; b++) {
        uint8_t  bit = (uint8_t)((frame_bits[b/8u] >> (b%8u)) & 1u);
        uint32_t te  = bit ? (uint32_t)HONDA_TE_LONG : (uint32_t)HONDA_TE_SHORT;
        buf[idx++] = level_duration_make(true,  te);
        buf[idx++] = level_duration_make(false, te);
        furi_check(idx < HONDA_ENC_BUF_SIZE - 2u);
    }
    buf[idx++] = level_duration_make(false, (uint32_t)HONDA_GUARD_TIME_US);
    inst->encoder.size_upload = idx;
    inst->encoder.front       = 0u;
}

void* subghz_protocol_encoder_honda_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderHonda* inst = malloc(sizeof(SubGhzProtocolEncoderHonda));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolEncoderHonda));
    inst->base.protocol         = &subghz_protocol_honda;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->encoder.repeat        = 3u;
    inst->encoder.upload = malloc(HONDA_ENC_BUF_SIZE * sizeof(LevelDuration));
    furi_check(inst->encoder.upload);
    inst->encoder.is_running = false;
    return inst;
}

void subghz_protocol_encoder_honda_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderHonda* inst = context;
    free(inst->encoder.upload);
    free(inst);
}

void subghz_protocol_encoder_honda_stop(void* context) {
    furi_assert(context);
    ((SubGhzProtocolEncoderHonda*)context)->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_honda_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderHonda* inst = context;
    if(inst->encoder.repeat == 0u || !inst->encoder.is_running)
        return level_duration_reset();
    LevelDuration ret = inst->encoder.upload[inst->encoder.front];
    if(++inst->encoder.front >= inst->encoder.size_upload) {
        inst->encoder.repeat--;
        inst->encoder.front = 0u;
    }
    return ret;
}

SubGhzProtocolStatus subghz_protocol_encoder_honda_deserialize(
    void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderHonda* inst = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&inst->generic, flipper_format);
    if(ret != SubGhzProtocolStatusOk) return ret;
    _honda_unpack(inst->generic.data, &inst->frame);
    uint8_t custom = subghz_protocol_honda_btn_to_custom(inst->frame.button);
    if(subghz_custom_btn_get_original() == 0u)
        subghz_custom_btn_set_original(custom);
    subghz_custom_btn_set_max(HONDA_CUSTOM_BTN_MAX);
    uint8_t active_custom = subghz_custom_btn_get();
    inst->active_button =
        (active_custom == SUBGHZ_CUSTOM_BTN_OK)
            ? subghz_protocol_honda_custom_to_btn(
                  subghz_custom_btn_get_original())
            : subghz_protocol_honda_custom_to_btn(active_custom);
    inst->frame.counter =
        (inst->frame.counter +
         furi_hal_subghz_get_rolling_counter_mult()) & 0x00FFFFFFu;
    _honda_counter_increment(&inst->frame);
    inst->frame.button = inst->active_button;
    inst->generic.data = _honda_pack(&inst->frame);
    inst->generic.cnt  = inst->frame.counter;
    inst->generic.btn  = inst->active_button;
    flipper_format_rewind(flipper_format);
    uint8_t key_data[8];
    for(int i = 0; i < 8; i++)
        key_data[i] = (uint8_t)(inst->generic.data >> (56 - i*8));
    flipper_format_update_hex(flipper_format, "Key", key_data, 8u);
    _honda_build_upload(inst);
    inst->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_encoder_honda_set_button(void* context, uint8_t btn) {
    furi_assert(context);
    SubGhzProtocolEncoderHonda* inst = context;
    inst->active_button      = btn & 0x0Fu;
    inst->encoder.is_running = false;
    _honda_counter_increment(&inst->frame);
    inst->frame.button    = inst->active_button;
    inst->generic.data    = _honda_pack(&inst->frame);
    inst->generic.cnt     = inst->frame.counter;
    _honda_build_upload(inst);
    inst->encoder.repeat     = 3u;
    inst->encoder.is_running = true;
}
