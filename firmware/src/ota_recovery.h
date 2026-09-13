#pragma once

#include <stdint.h>

#define OTA_RECOVERY_RECORD_SIZE          32U
#define OTA_RECOVERY_JOURNAL_SIZE         0x1000U
#define OTA_RECOVERY_JOURNAL_RECORDS      \
    (OTA_RECOVERY_JOURNAL_SIZE / OTA_RECOVERY_RECORD_SIZE)
#define OTA_RECOVERY_BOOT_MARKER_OFFSET   8U
#define OTA_RECOVERY_JOURNAL_ADDRESS      0x3F000UL
#define OTA_RECOVERY_BACKUP_ADDRESS       0x40000UL
#define OTA_RECOVERY_BACKUP_SIZE          0x1000U
#define OTA_RECOVERY_SLOT_A               0U
#define OTA_RECOVERY_SLOT_B               1U

typedef enum
{
    OTA_RECOVERY_STATE_BASELINE = 1,
    OTA_RECOVERY_STATE_INSTALL_INTENT = 2,
    OTA_RECOVERY_STATE_MARKERS_SWITCHED = 3,
    OTA_RECOVERY_STATE_TRIAL_ATTEMPT = 4,
    OTA_RECOVERY_STATE_CONFIRMED = 5,
    OTA_RECOVERY_STATE_ROLLBACK_INTENT = 6,
    OTA_RECOVERY_STATE_ROLLBACK_DONE = 7,
} ota_recovery_state_t;

typedef struct
{
    uint8_t state;
    uint8_t source_slot;
    uint8_t target_slot;
    uint32_t sequence;
    uint32_t source_version;
    uint32_t target_version;
    uint32_t candidate_crc32;
    uint32_t backup_crc32;
} ota_recovery_record_t;

typedef struct
{
    uint8_t found;
    uint8_t full;
    uint16_t next_index;
    uint16_t valid_records;
    uint16_t corrupt_records;
    uint8_t trial_attempts;
    ota_recovery_record_t latest;
} ota_recovery_scan_t;

typedef enum
{
    OTA_RECOVERY_ACTION_NORMAL = 0,
    OTA_RECOVERY_ACTION_RESUME_INSTALL = 1,
    OTA_RECOVERY_ACTION_RESUME_SWITCH = 2,
    OTA_RECOVERY_ACTION_START_TRIAL = 3,
    OTA_RECOVERY_ACTION_ROLLBACK_SOURCE = 4,
    OTA_RECOVERY_ACTION_CLEANUP_FAILED_TARGET = 5,
    OTA_RECOVERY_ACTION_SWIRE_RESCUE = 6,
} ota_recovery_action_t;

typedef struct
{
    uint8_t has_record;
    ota_recovery_record_t latest;
    uint8_t running_slot;
    uint8_t slot_a_valid;
    uint8_t slot_b_valid;
    uint8_t backup_valid;
    uint8_t trial_attempts;
    uint8_t max_trial_attempts;
} ota_recovery_observation_t;

uint32_t ota_recovery_crc32(const uint8_t *data, uint32_t length);
void ota_recovery_encode(const ota_recovery_record_t *record,
                         uint8_t output[OTA_RECOVERY_RECORD_SIZE]);
uint8_t ota_recovery_decode(const uint8_t input[OTA_RECOVERY_RECORD_SIZE],
                            ota_recovery_record_t *record);
void ota_recovery_scan_journal(const uint8_t *journal, uint32_t length,
                               ota_recovery_scan_t *result);
void ota_recovery_scan_begin(ota_recovery_scan_t *result,
                             uint16_t record_count);
void ota_recovery_scan_entry(ota_recovery_scan_t *result,
                             const uint8_t entry[OTA_RECOVERY_RECORD_SIZE],
                             uint16_t index);
void ota_recovery_make_masked_backup(const uint8_t *source, uint8_t *backup,
                                     uint32_t length);
uint8_t ota_recovery_select_boot(uint8_t slot_a_valid, uint8_t slot_b_valid);
ota_recovery_action_t ota_recovery_decide(
    const ota_recovery_observation_t *observation);
