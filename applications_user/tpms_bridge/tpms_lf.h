#pragma once

#include <stdint.h>

/* A TPMS sensor at rest stays silent: it wakes up either from wheel
 * rotation or from a 125 kHz low-frequency field — the same way factory
 * activation tools do it. The field is emitted by the RFID coil on the
 * back of the Flipper, so the sensor has to be held right against it.
 */

/* PORT: none of the supported ESP32-S3 boards can emit a 125 kHz field. T-Embed and Paradoxt carry
 * an RDM6300, a UART reader that decodes EM4100 by itself and gives no control over its coil. On
 * ThorChimera and Thormini there is no RFID at all. furi_hal_rfid_tim_read_start/stop are
 * therefore empty stubs here. With this at 0, the wake keys, the "wake" hints and the CLI "wake"
 * option are disabled instead of pretending to fire a pulse. Sensors still transmit when the
 * wheel turns (driving), after a pressure change, or when woken by an external TPMS tool. */
#define TPMS_LF_WAKE_SUPPORTED 0

#define TPMS_LF_FREQUENCY_HZ 125000.0f
#define TPMS_LF_DUTY_CYCLE   0.5f

/** Default duration of a single field pulse, ms. */
#define TPMS_LF_PULSE_MS 700

/** How often the pulse repeats in auto-wake mode, ms. */
#define TPMS_LF_PERIOD_MS 5000

void tpms_lf_field_start(void);
void tpms_lf_field_stop(void);

/** Field pulse, blocking for its whole duration. */
void tpms_lf_wake(uint32_t duration_ms);
