/* Host tests compile the actual epd.c state machine, actual EPD_init/idle
 * functions and led.c against fake GPIO/SPI/time, not an alternate algorithm.
 * They check sequencing only; they cannot verify panel silicon or current. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../firmware/src/epd.h"
#define RAM
#define _attribute_ram_code_
#define EPD_RESET 1
#define EPD_CS 2
#define EPD_DC 3
#define EPD_CLK 4
#define EPD_MOSI 5
#define EPD_BUSY 6
#define LED_GREEN 7
#define LED_RED 8
#define LED_BLUE 9
#define AS_GPIO 0
#define PM_PIN_UP_DOWN_FLOAT 0
#define PM_PIN_PULLUP_1M 1
#define SUSPEND_ADV 1
#define SUSPEND_CONN 2
static uint32_t now;
static int levels[10], inputs[10], outputs[10], pulls[10];
static int busy, reset_low_count, green_low_count, mask;
static uint8_t commands[64], current_command;
static unsigned command_count, black_count, red_count, sleep_count;
static void gpio_write(int p, int v) {
    levels[p] = v;
    if (p == EPD_RESET && !v) ++reset_low_count;
    if (p == LED_GREEN && !v) ++green_low_count;
}
static void gpio_set_func(int p, int v) {(void)p;(void)v;}
static void gpio_set_input_en(int p, int v) {inputs[p]=v;}
static void gpio_set_output_en(int p, int v) {outputs[p]=v;}
static void gpio_setup_up_down_resistor(int p, int v) {pulls[p]=v;}
static uint32_t clock_time(void) {return now;}
static int clock_time_exceed(uint32_t t, uint32_t d) {return (uint32_t)(now-t)>d;}
static void WaitMs(uint32_t ms) {now += ms*1000;}
static void bls_pm_setSuspendMask(int v) {mask=v;}
#define EPD_IS_BUSY() (busy)
static void EPD_WriteCmd(uint8_t c) {
    assert(command_count < sizeof commands);
    commands[command_count++]=current_command=c;
}
static void EPD_WriteData(uint8_t d) {
    if(current_command==0x10) ++black_count;
    if(current_command==0x13) ++red_count;
    if(current_command==0x07) {assert(d==0xa5); ++sleep_count;}
}
void set_led_color(uint8_t color);
#include "production.inc"
static void reset_fixture(void) {
    now=0; busy=0; reset_low_count=green_low_count=0; mask=0;
    command_count=black_count=red_count=sleep_count=0;
    memset(levels,0,sizeof levels); memset(inputs,0,sizeof inputs);
    memset(outputs,0,sizeof outputs); memset(pulls,0,sizeof pulls);
    epd_update_state=EPD_STATE_IDLE; epd_state_started=0;
    init_led();
}
static void assert_idle(void) {
    assert(epd_update_state==EPD_STATE_IDLE);
    assert(levels[EPD_CS]==1 && levels[EPD_RESET]==1);
    assert(levels[EPD_CLK]==0 && levels[EPD_MOSI]==0 && levels[EPD_DC]==0);
    assert(inputs[EPD_BUSY]==0 && outputs[EPD_BUSY]==0);
    assert(pulls[EPD_BUSY]==PM_PIN_UP_DOWN_FLOAT);
    assert(levels[LED_GREEN]==1 && !green_low_count);
}
static void test_boot_sleep(void) {
    reset_fixture(); epd_prepare_boot_sleep();
    assert(command_count==1 && commands[0]==0x02);
    assert(reset_low_count==1 && inputs[EPD_BUSY]==1);
    now += 100001; epd_state_handler(); assert_idle();
    assert(command_count==2 && commands[1]==0x07 && sleep_count==1);
    assert(reset_low_count==1); /* no reset assertion AFTER deep sleep */
}
static void test_refresh_after_boot_sleep(void) {
    test_boot_sleep(); command_count=sleep_count=0;
    epd_make_validation_pattern(); EPD_Display(epd_buffer,sizeof epd_buffer,1);
    assert(mask==(SUSPEND_ADV|SUSPEND_CONN));
    assert(inputs[EPD_BUSY]==1 && outputs[EPD_BUSY]==0);
    assert(black_count==4000 && red_count==4000);
    assert(commands[command_count-1]==0x12);
    unsigned before=command_count;
    EPD_Display(epd_buffer,sizeof epd_buffer,1); /* reject reentrant refresh */
    assert(command_count==before);
    busy=1; epd_state_handler(); assert(epd_update_state==EPD_STATE_WAIT_REFRESH_DONE);
    busy=0; epd_state_handler(); assert(commands[command_count-1]==0x02);
    now+=100001; epd_state_handler(); assert_idle();
    assert(sleep_count==1 && reset_low_count==2);
}
static void test_stuck_busy_cleanup(void) {
    reset_fixture(); busy=1; EPD_Display(epd_buffer,sizeof epd_buffer,1);
    assert(epd_update_state==EPD_STATE_WAIT_POWER_OFF);
    assert(commands[command_count-1]==0x02);
    now+=EPD_POWER_OFF_TIMEOUT_US+1; epd_state_handler();
    assert_idle(); assert(sleep_count==1); /* best effort, not silicon ACK */
}
static void test_refresh_timeout(void) {
    reset_fixture(); EPD_Display(epd_buffer,sizeof epd_buffer,1);
    busy=1; epd_state_handler(); now+=EPD_REFRESH_TIMEOUT_US+1;
    epd_state_handler(); assert(epd_update_state==EPD_STATE_WAIT_POWER_OFF);
    busy=0; now+=100001; epd_state_handler(); assert_idle();
}
static void test_busy_edge_fallback(void) {
    reset_fixture(); EPD_Display(epd_buffer,sizeof epd_buffer,1);
    now+=EPD_BUSY_ASSERT_TIMEOUT_US+1; epd_state_handler();
    assert(epd_update_state==EPD_STATE_FALLBACK_DELAY);
    now+=EPD_BUSY_FALLBACK_US+1; epd_state_handler();
    assert(epd_update_state==EPD_STATE_WAIT_POWER_OFF);
    now+=100001; epd_state_handler(); assert_idle();
}
int main(void) {
    test_boot_sleep(); test_refresh_after_boot_sleep();
    test_stuck_busy_cleanup(); test_refresh_timeout(); test_busy_edge_fallback();
    puts("PASS: boot/no-refresh, two planes, GPIO idle, BUSY re-enable, reentrancy, timeout and fallback cleanup. Hardware current untested.");
    return 0;
}
