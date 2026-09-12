#include "app_config.h"

#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"

void bootloader_hooks_include(void) {
}

static void mute_pin(uint32_t pin) {
    gpio_ll_func_sel(&GPIO, pin, PIN_FUNC_GPIO);
    gpio_ll_set_level(&GPIO, pin, 0);
    gpio_ll_pullup_dis(&GPIO, pin);
    gpio_ll_pulldown_en(&GPIO, pin);
    gpio_ll_input_disable(&GPIO, pin);
    gpio_ll_od_disable(&GPIO, pin);
    gpio_ll_output_enable(&GPIO, pin);
}

void bootloader_before_init(void) {
    mute_pin(APP_SPK_BCLK_PIN);
    mute_pin(APP_SPK_LRCK_PIN);
    mute_pin(APP_SPK_DATA_PIN);
}
