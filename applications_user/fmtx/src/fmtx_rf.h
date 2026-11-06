#ifndef yo3gnd_rf_08a1
#define yo3gnd_rf_08a1

#include <stdbool.h>
#include <stdint.h>
#include <lib/subghz/devices/devices.h>

#define RINGSZ 4096U

typedef enum {
    FmtxRadioAuto,
    FmtxRadioExternal,
    FmtxRadioInternal,
} FmtxRadio;

typedef struct {
    uint32_t hz;
    const uint8_t* regs;
    // The radio in use between rfstart() and rfstop(): the board's CC1101 ("cc1101_int") or the
    // external module ("cc1101_ext", chosen in Sub-GHz > Radio Settings).
    const SubGhzDevice* device;
    volatile uint16_t head;
    volatile uint16_t tail;
    int16_t ring[RINGSZ];
    volatile bool on;
    volatile bool prime;
    volatile bool drain;
    uint16_t lock_decisions;
    volatile uint32_t played_samples;
    int16_t s;
    int32_t err;
    uint8_t sphase;
    uint8_t sample_decisions;
    uint8_t slot;
    uint16_t run; // consecutive equal PDM bits (see rfbit)
    volatile uint8_t gain;
    FmtxRadio selected;
    bool bit;
    bool awake;
    bool external; // `device` is the external module (it must be end()ed)
    bool devices_started; // subghz_devices_init() is pending its deinit
} Rf;

/** Allocate the radio state. The refill callback runs in the RMT interrupt and reads this
 *  struct, so it is placed in internal RAM when that is possible. */
Rf* rfalloc(void);
void rffree(Rf* rf);
void rfinit(Rf* rf, uint32_t hz);
const uint8_t* rfregs(void);
void rfselect(Rf* rf, FmtxRadio radio);
/** true if the external module is chosen in Sub-GHz settings and answers (no side effects). */
bool rfexternal(Rf* rf);
bool rfstart(Rf* rf);
bool rfresume(Rf* rf);
void rfpause(Rf* rf);
void rfstop(Rf* rf);
void rfledidle(void);
void rfledoff(void);
bool rfput(Rf* rf, int16_t s);
uint16_t rfused(const Rf* rf);
void rfhold(Rf* rf, uint8_t decisions);
void rfgain(Rf* rf, uint8_t gain);
void rfend(Rf* rf);
bool rfdone(const Rf* rf);
bool rfdrain(Rf* rf, uint32_t timeout_ms);
uint32_t rfplayed(const Rf* rf);
void rfrst(Rf* rf);

#endif
