/* Host sequencing tests for the FPC-A002/GDEY0213Z98 SSD1680 build. They use
 * the production epd.c and GPIO setup copied into production.inc by run.sh. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define HINK_PANEL_PROFILE 2
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
static int busy, reset_low_count, mask;
static uint8_t commands[64], current_command;
static unsigned command_count, black_count, red_count, sleep_count;
static void gpio_write(int p, int v) {
    levels[p] = v;
    if (p == EPD_RESET && !v) ++reset_low_count;
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
    if(current_command==0x24) ++black_count;
    if(current_command==0x26) ++red_count;
    if(current_command==0x10) {assert(d==0x01); ++sleep_count;}
}
void set_led_color(uint8_t color);
#include "production.inc"
static void reset_fixture(void) {
    now=0; busy=0; reset_low_count=mask=0;
    command_count=black_count=red_count=sleep_count=0;
    memset(levels,0,sizeof levels); memset(inputs,0,sizeof inputs);
    memset(outputs,0,sizeof outputs); memset(pulls,0,sizeof pulls);
    epd_update_state=EPD_STATE_IDLE; epd_state_started=0;
    init_led();
}
static void assert_idle(void) {
    assert(epd_update_state==EPD_STATE_IDLE);
    assert(levels[EPD_CS]==1 && levels[EPD_RESET]==1);
    assert(inputs[EPD_BUSY]==0 && outputs[EPD_BUSY]==0);
    assert(pulls[EPD_BUSY]==PM_PIN_UP_DOWN_FLOAT);
}
static void test_boot_sleep(void) {
    reset_fixture(); epd_prepare_boot_sleep();
    assert(command_count==0 && reset_low_count==1 && inputs[EPD_BUSY]==1);
    now+=100001; epd_state_handler(); assert_idle();
    assert(command_count==1 && commands[0]==0x10 && sleep_count==1);
}
static void test_refresh_sequence(void) {
    static const uint8_t expected[] = {
        0x12, 0x01, 0x11, 0x44, 0x45, 0x4e, 0x4f,
        0x3c, 0x18, 0x21, 0x24, 0x26, 0x20
    };
    reset_fixture(); epd_make_validation_pattern();
    EPD_Display(epd_buffer,sizeof epd_buffer,1);
    assert(mask==(SUSPEND_ADV|SUSPEND_CONN));
    assert(command_count==sizeof expected);
    assert(memcmp(commands,expected,sizeof expected)==0);
    assert(black_count==4000 && red_count==4000 && sleep_count==0);
    busy=1; epd_state_handler();
    assert(epd_update_state==EPD_STATE_WAIT_REFRESH_DONE);
    busy=0; epd_state_handler();
    assert(epd_update_state==EPD_STATE_WAIT_POWER_OFF);
    now+=100001; epd_state_handler(); assert_idle();
    assert(commands[command_count-1]==0x10 && sleep_count==1);
}
static void test_ready_timeout_cleanup(void) {
    reset_fixture(); busy=1;
    EPD_Display(epd_buffer,sizeof epd_buffer,1);
    assert(epd_update_state==EPD_STATE_WAIT_POWER_OFF);
    now+=EPD_POWER_OFF_TIMEOUT_US+1; epd_state_handler(); assert_idle();
    assert(commands[command_count-1]==0x10 && sleep_count==1);
}
int main(void) {
    test_boot_sleep(); test_refresh_sequence(); test_ready_timeout_cleanup();
    puts("PASS: SSD1680 active-high BUSY, 0x24/0x26 planes, 0x20 refresh and 0x10 sleep sequencing.");
    return 0;
}
