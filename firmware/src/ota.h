#pragma once

/* OTA v2 M1-C stages only to the inactive slot and requires two explicit,
 * CRC-bound confirmation commands before the deferred boot-marker switch. */
int custom_otaWrite(void *p);
void ota_v2_process(void);
