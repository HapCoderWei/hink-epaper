#include <stdint.h>
#include "tl_common.h"
#include "app.h"
#include "main.h"
#include "drivers.h"
#include "stack/ble/ble.h"
#include "vendor/common/blt_common.h"

#include "ble.h"
#include "flash.h"
#include "epd.h"
#include "epd_spi.h"

// Settings
extern settings_struct settings;

_attribute_ram_code_ void user_init_normal(void)
{
    random_generator_init();
    init_ble();
    init_flash();
    /* The panel rail is always powered. Do not equate GPIO levels with
     * power-off: explicitly request controller sleep, without a refresh. */
    epd_prepare_boot_sleep();
    set_adv_data(0, 100, 3000);
}

_attribute_ram_code_ void user_init_deepRetn(void)
{ // after sleep this will get executed
    blc_ll_initBasicMCU();
    rf_set_power_level_index(RF_POWER_P3p01dBm);
    blc_ll_recoverDeepRetention();
}

_attribute_ram_code_ void main_loop(void)
{
    blt_sdk_main_loop();

    if (epd_state_handler())
    {
        /* The controller keeps refreshing without MCU intervention.  Ordinary
         * suspend switches off the CPU/RF/high-speed clocks between BLE events
         * while retaining the live GPIO configuration used by the panel.
         * Avoid deep retention only for this short state-machine window. */
        bls_pm_setSuspendMask(SUSPEND_ADV | SUSPEND_CONN);
    }
    else
    {
        /* Diagnostic: use ordinary suspend, keeping digital GPIO levels
         * valid between radio events. Do NOT restore the baseline retention
         * mask here, or the initialization-only experiment is ineffective. */
        blt_pm_proc();
    }
}
