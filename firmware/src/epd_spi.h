#pragma once

/* VCI/VDDIO are always supplied. Use controller commands 0x02/0x07
 * to power down its internal drive and request deep sleep. */

#define EPD_ENABLE_WRITE_CMD() gpio_write(EPD_DC, 0)
#define EPD_ENABLE_WRITE_DATA() gpio_write(EPD_DC, 1)

#define EPD_IS_BUSY() (!gpio_read(EPD_BUSY))


void EPD_init(void);
void EPD_idle_pins(void);
void EPD_SPI_Write(unsigned char value);
uint8_t EPD_SPI_read(void);
void EPD_WriteCmd(unsigned char cmd);
void EPD_WriteData(unsigned char data);
void EPD_CheckStatus(int max_ms);
void EPD_CheckStatus_inverted(int max_ms);
void EPD_send_lut(uint8_t lut[], int len);
void EPD_send_empty_lut(uint8_t lut, int len);
void EPD_LoadImage(unsigned char *image, int size, uint8_t cmd);
