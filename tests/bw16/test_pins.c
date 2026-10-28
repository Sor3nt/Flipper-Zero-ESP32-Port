#include <furi_hal_shared_pins.h>
#include <soc/gpio_struct.h>
#include <soc/gpio_periph.h>
#include <soc/gpio_sig_map.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
TestGpio GPIO;
uintptr_t GPIO_PIN_MUX_REG[49];
static uint32_t mux[49];
static uint64_t reserved;
bool esp_gpio_is_reserved(uint64_t mask) { return (reserved & mask)!=0; }
uint64_t esp_gpio_revoke(uint64_t mask) { uint64_t old=reserved; reserved&=~mask; return old; }
int main(void) {
    int a=0,b=0;
    assert(!furi_hal_shared_pins_acquire(NULL));
    assert(!furi_hal_shared_pins_save(&a));
    assert(furi_hal_shared_pins_acquire(&a));
    assert(!furi_hal_shared_pins_acquire(&b));
    assert(!furi_hal_shared_pins_acquire(&a));
    furi_hal_shared_pins_release(&b);
    assert(!furi_hal_shared_pins_acquire(&b));
    for(int i=0;i<49;++i) {
        mux[i]=100+i; GPIO_PIN_MUX_REG[i]=(uintptr_t)&mux[i];
        GPIO.pin[i].val=200+i; GPIO.func_out_sel_cfg[i].val=300+i;
    }
    GPIO.out1.val=0xaaaa; GPIO.enable1.val=0x5555;
    GPIO.func_in_sel_cfg[U1RXD_IN_IDX].val=1234;
    reserved=1ULL<<44;
    assert(furi_hal_shared_pins_save(&a));
    assert(!furi_hal_shared_pins_save(&a));
    furi_hal_shared_pins_release(&a); /* Cannot release before restoration. */
    assert(!furi_hal_shared_pins_acquire(&b));
    mux[43]=mux[44]=0;
    GPIO.pin[43].val=GPIO.pin[44].val=0;
    GPIO.func_out_sel_cfg[43].val=GPIO.func_out_sel_cfg[44].val=0;
    GPIO.func_in_sel_cfg[U1RXD_IN_IDX].val=4321;
    GPIO.pin[42].val=999; /* Unrelated pin changes must survive restoration. */
    reserved|=1ULL<<43;
    furi_hal_shared_pins_restore(&b);
    assert(mux[43]==0);
    furi_hal_shared_pins_restore(&a);
    assert(mux[43]==143 && mux[44]==144);
    assert(GPIO.pin[43].val==243 && GPIO.pin[44].val==244 && GPIO.pin[42].val==999);
    assert(GPIO.func_out_sel_cfg[43].val==343 && GPIO.func_out_sel_cfg[44].val==344);
    assert(GPIO.func_in_sel_cfg[U1RXD_IN_IDX].val==1234);
    unsigned mask=(1U<<11)|(1U<<12);
    assert(GPIO.enable1_w1tc.val==mask && GPIO.enable1_w1ts.val==(0x5555 & mask));
    assert(GPIO.out1_w1tc.val==((unsigned)~0xaaaa & mask) && GPIO.out1_w1ts.val==(0xaaaa & mask));
    assert(reserved==(1ULL<<44));
    furi_hal_shared_pins_release(&a);
    assert(furi_hal_shared_pins_acquire(&b));
    furi_hal_shared_pins_release(&b);
    puts("pins: exclusive lease, owner checks, GPIO/mux/route/reservation restore PASS");
}
