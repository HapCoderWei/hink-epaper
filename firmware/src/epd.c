#include <stdint.h>
#include "tl_common.h"
#include "drivers.h"
#include "stack/ble/ble.h"

#include "main.h"
#include "epd.h"
#include "epd_spi.h"
#include "led.h"

/*
 * HINK-E0213A162-FPC-A0, 2.13-inch black/white/red panel.
 *
 * The controller stores 128 pixels per row even though only 122 are visible.
 * Two active-low 1-bit planes are transferred in this order:
 *   black plane: 4000 bytes (command 0x10)
 *   red plane:   4000 bytes (command 0x13)
 *
 * This sequence and the GPIO mapping were verified on the target price tag.
 */

#define EPD_RESET_PHASE_MS              200U
#define EPD_POWER_ON_TIMEOUT_US    30000000UL
#define EPD_BUSY_ASSERT_TIMEOUT_US  2000000UL
#define EPD_REFRESH_TIMEOUT_US     60000000UL
#define EPD_BUSY_FALLBACK_US       30000000UL
#define EPD_POWER_OFF_TIMEOUT_US   30000000UL

enum
{
    EPD_STATE_IDLE = 0,
    EPD_STATE_WAIT_BUSY_ASSERT,
    EPD_STATE_WAIT_REFRESH_DONE,
    EPD_STATE_WAIT_POWER_OFF,
    EPD_STATE_FALLBACK_DELAY
};

RAM uint8_t epd_buffer[epd_buffer_size];
RAM uint8_t epd_update_state = EPD_STATE_IDLE;
RAM uint32_t epd_state_started = 0;

static _attribute_ram_code_ uint8_t epd_wait_ready(uint32_t timeout_us)
{
    uint32_t started = clock_time();

    /* BUSY is active low on this panel. */
    while (EPD_IS_BUSY())
    {
        if (clock_time_exceed(started, timeout_us))
            return 0;
        WaitMs(1);
    }
    return 1;
}

static _attribute_ram_code_ void epd_write_cmd_data(uint8_t command,
                                                     const uint8_t *data,
                                                     uint8_t length)
{
    uint8_t i;

    EPD_WriteCmd(command);
    for (i = 0; i < length; ++i)
        EPD_WriteData(data[i]);
}

static _attribute_ram_code_ void epd_hard_reset(void)
{
    gpio_write(EPD_RESET, 1);
    WaitMs(EPD_RESET_PHASE_MS);
    gpio_write(EPD_RESET, 0);
    WaitMs(EPD_RESET_PHASE_MS);
    gpio_write(EPD_RESET, 1);
    WaitMs(EPD_RESET_PHASE_MS);
}

static _attribute_ram_code_ uint8_t epd_write_verified_init(void)
{
    static const uint8_t booster[] = {0x17, 0x17, 0x17};

    epd_write_cmd_data(0x06, booster, sizeof(booster));
    EPD_WriteCmd(0x04); /* POWER_ON */
    if (!epd_wait_ready(EPD_POWER_ON_TIMEOUT_US))
        return 0;
    epd_write_cmd_data(0x00, (const uint8_t[]){0x8f}, 1);
    epd_write_cmd_data(0x50, (const uint8_t[]){0xf0}, 1);

    /* Deliberately do not send 0x61. It broke this panel during testing. */
    return 1;
}

static _attribute_ram_code_ void epd_stream_plane(uint8_t command,
                                                   const uint8_t *plane)
{
    uint16_t i;

    EPD_WriteCmd(command);
    for (i = 0; i < EPD_PLANE_SIZE; ++i)
        EPD_WriteData(plane[i]);
    EPD_WriteCmd(0x92); /* DATA_STOP */
}

static _attribute_ram_code_ void epd_finish(uint8_t success)
{
    epd_write_cmd_data(0x07, (const uint8_t[]){0xa5}, 1); /* DEEP_SLEEP */
    WaitMs(10); /* Keep reset deasserted while the command is processed. */
    EPD_idle_pins();
    epd_update_state = EPD_STATE_IDLE;
    /* The current Web client only confirms writes, not optical success.
     * Do not imply acknowledgement of sleep by this SPI-only controller. */
    (void)success;
    set_led_color(0);
}

static _attribute_ram_code_ void epd_begin_power_off(void)
{
    EPD_WriteCmd(0x02); /* POWER_OFF */
    epd_state_started = clock_time();
    epd_update_state = EPD_STATE_WAIT_POWER_OFF;
}

static _attribute_ram_code_ void epd_fail(void)
{
    /* No external power switch exists. On an error, attempt internal power
     * down and bounded sleep cleanup rather than leave the controller active.
     * WAIT_POWER_OFF has its own timeout, so this cannot loop indefinitely. */
    epd_begin_power_off();
    set_led_color(0);
}

_attribute_ram_code_ void epd_prepare_boot_sleep(void)
{
    /* At power-on the panel is powered even when no user asks to refresh.
     * Reset to accept commands, power off internal drive, then asynchronously
     * wait for BUSY before 0x07/A5. Never send 0x04 or 0x12 here: the retained
     * optical image must not be deliberately refreshed at every MCU reset. */
    EPD_init();
    epd_hard_reset();
    epd_begin_power_off();
}

void set_EPD_model(uint8_t model_nr)
{
    (void)model_nr;
}

_attribute_ram_code_ uint8_t EPD_read_temp(void)
{
    /* This controller/panel combination is not queried for temperature. */
    return 0;
}

_attribute_ram_code_ void EPD_Display(unsigned char *image, int size,
                                      uint8_t full_or_partial)
{
    (void)full_or_partial;

    if (epd_update_state != EPD_STATE_IDLE || image == 0 ||
        size < (int)epd_buffer_size)
        return;

    /* This function may run inside a GATT callback, before app.main_loop gets
     * another chance to change the mask.  Force suspend-only immediately so
     * the first post-command sleep cannot enter deep retention mid-refresh. */
    bls_pm_setSuspendMask(SUSPEND_ADV | SUSPEND_CONN);

    set_led_color(0);
    EPD_init();
    WaitMs(10);
    epd_hard_reset();
    if (!epd_write_verified_init())
    {
        epd_fail();
        return;
    }

    epd_stream_plane(0x10, image + EPD_BLACK_PLANE_OFFSET);
    epd_stream_plane(0x13, image + EPD_RED_PLANE_OFFSET);

    EPD_WriteCmd(0x12); /* DISPLAY_REFRESH */
    epd_state_started = clock_time();
    epd_update_state = EPD_STATE_WAIT_BUSY_ASSERT;
}

_attribute_ram_code_ void epd_set_sleep(void)
{
    if (epd_update_state != EPD_STATE_IDLE)
        epd_begin_power_off();
}

_attribute_ram_code_ uint8_t epd_state_handler(void)
{
    switch (epd_update_state)
    {
    case EPD_STATE_IDLE:
        break;

    case EPD_STATE_WAIT_BUSY_ASSERT:
        if (EPD_IS_BUSY())
        {
            epd_state_started = clock_time();
            epd_update_state = EPD_STATE_WAIT_REFRESH_DONE;
        }
        else if (clock_time_exceed(epd_state_started,
                                  EPD_BUSY_ASSERT_TIMEOUT_US))
        {
            /* Some controller revisions do not expose the BUSY falling edge.
             * Preserve the verified 30-second conservative fallback. */
            epd_state_started = clock_time();
            epd_update_state = EPD_STATE_FALLBACK_DELAY;
        }
        break;

    case EPD_STATE_WAIT_REFRESH_DONE:
        if (!EPD_IS_BUSY())
            epd_begin_power_off();
        else if (clock_time_exceed(epd_state_started, EPD_REFRESH_TIMEOUT_US))
            epd_fail();
        break;

    case EPD_STATE_FALLBACK_DELAY:
        if (clock_time_exceed(epd_state_started, EPD_BUSY_FALLBACK_US))
            epd_begin_power_off();
        break;

    case EPD_STATE_WAIT_POWER_OFF:
        if (clock_time_exceed(epd_state_started, 100000UL) && !EPD_IS_BUSY())
            epd_finish(1);
        else if (clock_time_exceed(epd_state_started,
                                  EPD_POWER_OFF_TIMEOUT_US))
            epd_finish(0);
        break;

    default:
        epd_fail();
        break;
    }

    return epd_update_state;
}

_attribute_ram_code_ void epd_clear(void)
{
    memset(epd_buffer, 0xff, sizeof(epd_buffer));
    EPD_Display(epd_buffer, sizeof(epd_buffer), 1);
}

_attribute_ram_code_ void epd_make_validation_pattern(void)
{
    uint16_t y;
    uint8_t byte_x;
    uint8_t bit;

    memset(epd_buffer, 0xff, sizeof(epd_buffer));

    for (y = 0; y < EPD_VISIBLE_HEIGHT; ++y)
    {
        for (byte_x = 0; byte_x < EPD_ROW_BYTES; ++byte_x)
        {
            uint8_t black_byte = 0xff;
            uint8_t red_byte = 0xff;

            for (bit = 0; bit < 8; ++bit)
            {
                uint16_t x = (uint16_t)byte_x * 8U + bit;
                uint8_t mask = (uint8_t)(0x80U >> bit);

                if (x >= EPD_VISIBLE_WIDTH)
                    continue;

                if (y < 80U)
                {
                    black_byte &= (uint8_t)~mask;
                }
                else if (y < 165U)
                {
                    uint8_t border = (x == 8U || x == 113U || y == 85U ||
                                      y == 159U);
                    uint8_t diagonal =
                        (x > 8U && x < 114U &&
                         ((uint16_t)(x - 9U) == (uint16_t)(y - 86U) * 105U / 73U ||
                          (uint16_t)(113U - x) == (uint16_t)(y - 86U) * 105U / 73U));
                    if (border || diagonal)
                        black_byte &= (uint8_t)~mask;
                }
                else
                {
                    red_byte &= (uint8_t)~mask;
                }
            }

            epd_buffer[y * EPD_ROW_BYTES + byte_x] = black_byte;
            epd_buffer[EPD_RED_PLANE_OFFSET + y * EPD_ROW_BYTES + byte_x] = red_byte;
        }
    }
}

_attribute_ram_code_ void epd_display_tiff(uint8_t *pData, int iSize)
{
    int copy_size = iSize;

    memset(epd_buffer, 0xff, sizeof(epd_buffer));
    if (copy_size > (int)EPD_PLANE_SIZE)
        copy_size = EPD_PLANE_SIZE;
    if (copy_size > 0)
        memcpy(epd_buffer, pData, copy_size);
    EPD_Display(epd_buffer, sizeof(epd_buffer), 1);
}

_attribute_ram_code_ void epd_display(uint32_t time_is, uint16_t battery_mv,
                                      int16_t temperature,
                                      uint8_t full_or_partial)
{
    (void)time_is;
    (void)battery_mv;
    (void)temperature;
    (void)full_or_partial;
}

_attribute_ram_code_ void epd_display_char(uint8_t data)
{
    (void)data;
}
