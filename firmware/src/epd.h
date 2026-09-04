#pragma once

#include <stdint.h>

#define EPD_VISIBLE_WIDTH       122U
#define EPD_VISIBLE_HEIGHT      250U
#define EPD_ROW_BYTES           16U
#define EPD_PLANE_SIZE          (EPD_ROW_BYTES * EPD_VISIBLE_HEIGHT)
#define epd_buffer_size         (EPD_PLANE_SIZE * 2U)

#define EPD_BLACK_PLANE_OFFSET  0U
#define EPD_RED_PLANE_OFFSET    EPD_PLANE_SIZE

void set_EPD_model(uint8_t model_nr);
void init_epd(void);
uint8_t EPD_read_temp(void);
void EPD_Display(unsigned char *image, int size, uint8_t full_or_partial);
void epd_display_tiff(uint8_t *pData, int iSize);
void epd_display(uint32_t time_is, uint16_t battery_mv, int16_t temperature, uint8_t full_or_partial);
void epd_set_sleep(void);
void epd_prepare_boot_sleep(void);
uint8_t epd_state_handler(void);
void epd_display_char(uint8_t data);
void epd_clear(void);
void epd_make_validation_pattern(void);
