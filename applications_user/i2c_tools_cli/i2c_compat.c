#include "i2c_compat.h"

static bool i2c_compat_ready = false;

void i2c_compat_init(void) {
    i2c_compat_ready = furi_hal_i2c_external_available();
}

void i2c_compat_deinit(void) {
    i2c_compat_ready = false;
}

bool i2c_compat_available(void) {
    return i2c_compat_ready;
}

bool i2c_compat_refused(uint8_t addr7, bool write) {
    const FuriHalI2cAccess access = furi_hal_i2c_get_access(addr7);
    return access == FuriHalI2cAccessExclusive || (write && access == FuriHalI2cAccessReadOnly);
}
