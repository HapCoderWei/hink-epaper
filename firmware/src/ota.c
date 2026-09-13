#include <stdint.h>
#include "tl_common.h"
#include "drivers.h"
#include "stack/ble/ble.h"
#include "drivers/8258/flash.h"
#include "ota.h"
#include "ota_recovery.h"
#include "main.h"

/* OTA v2, milestone M2-C: journal the guarded dual-slot install, confirm a
 * healthy trial boot, and restore the masked source sector after 3 failures. */
#define OTA_V2_PROTOCOL_VERSION      1U
#define OTA_V2_BOARD_ID              0x213AU
#define OTA_V2_SLOT_A_START          0x00000UL
#define OTA_V2_SLOT_B_START          0x20000UL
#define OTA_V2_SLOT_SIZE             0x1F000UL
#define OTA_V2_SECTOR_SIZE           0x1000UL
#define OTA_V2_PAGE_SIZE             0x100UL
#define OTA_V2_MAX_DATA              24U
#define OTA_V2_RESPONSE_SIZE         32U
#define OTA_V2_RESPONSE_MAGIC        0xA2U
#define OTA_V2_MANIFEST_SIZE         24U
#define OTA_V2_MANIFEST_FORMAT       1U
#define OTA_V2_BOOT_MARKER_OFFSET    8U
#define OTA_V2_TELINK_LENGTH_OFFSET  0x18U
#define OTA_V2_VERIFY_CHUNK          64U
#define OTA_V2_MILESTONE             2U
#define OTA_V2_CAP_INSTALL           0x01U
#define OTA_V2_CAP_RECOVERY_LOG      0x02U
#define OTA_V2_ARM_CONFIRM           0x314D5241UL /* "ARM1", little endian */
#define OTA_V2_INSTALL_CONFIRM       0x31534E49UL /* "INS1", little endian */
#define OTA_V2_INSTALL_DELAY_US      300000UL
#define OTA_V2_TRIAL_CONFIRM_US      5000000UL
#define OTA_V2_TRIAL_WATCHDOG_MS     12000U
#define OTA_V2_MAX_TRIAL_ATTEMPTS    3U

/* Hostile-reset diagnostics are opt-in and must never be present in a normal
 * release image. Values select a persisted checkpoint after which the image
 * reboots once; the journal state prevents repeating the same injection. */
#ifndef HINK_OTA_SW_RESET_POINT
#define HINK_OTA_SW_RESET_POINT       0U
#endif

#ifndef HINK_FW_VERSION
#define HINK_FW_VERSION              0U
#endif

enum
{
    OTA_V2_CMD_INFO       = 0x10,
    OTA_V2_CMD_BEGIN      = 0x11,
    OTA_V2_CMD_ERASE_NEXT = 0x12,
    OTA_V2_CMD_DATA       = 0x13,
    OTA_V2_CMD_FINISH     = 0x14,
    OTA_V2_CMD_STATUS     = 0x15,
    OTA_V2_CMD_ABORT      = 0x16,
    OTA_V2_CMD_ARM_INSTALL = 0x17,
    OTA_V2_CMD_INSTALL     = 0x18,
};

enum
{
    OTA_V2_STATUS_OK                 = 0,
    OTA_V2_STATUS_BAD_LENGTH         = 1,
    OTA_V2_STATUS_BAD_PROTOCOL       = 2,
    OTA_V2_STATUS_BAD_TARGET         = 3,
    OTA_V2_STATUS_BAD_STATE          = 4,
    OTA_V2_STATUS_RANGE              = 5,
    OTA_V2_STATUS_ORDER              = 6,
    OTA_V2_STATUS_FLASH_VERIFY       = 7,
    OTA_V2_STATUS_CRC_MISMATCH       = 8,
    OTA_V2_STATUS_UNKNOWN_CMD        = 9,
    OTA_V2_STATUS_BAD_MANIFEST       = 10,
    OTA_V2_STATUS_BAD_IMAGE_HEADER   = 11,
    OTA_V2_STATUS_CONFIRM_REQUIRED   = 12,
    OTA_V2_STATUS_BOOT_MARK_VERIFY   = 13,
    OTA_V2_STATUS_UNSUPPORTED_LAYOUT = 14,
    OTA_V2_STATUS_RECOVERY_BACKUP    = 15,
    OTA_V2_STATUS_RECOVERY_JOURNAL   = 16,
};

enum
{
    OTA_V2_PHASE_IDLE      = 0,
    OTA_V2_PHASE_ERASING   = 1,
    OTA_V2_PHASE_RECEIVING = 2,
    OTA_V2_PHASE_VERIFIED  = 3,
    OTA_V2_PHASE_ERROR     = 4,
    OTA_V2_PHASE_ARMED     = 5,
    OTA_V2_PHASE_INSTALL_PENDING = 6,
};

typedef struct
{
    uint32_t image_size;
    uint32_t expected_crc32;
    uint32_t received;
    uint32_t crc32_state;
    uint32_t target_base;
    uint8_t sectors_total;
    uint8_t sectors_erased;
    uint8_t phase;
    uint8_t last_status;
    uint8_t layout_valid;
    uint32_t install_started;
    uint32_t candidate_version;
} ota_v2_session_t;

RAM ota_v2_session_t ota_v2_session;
RAM uint8_t ota_v2_response[OTA_V2_RESPONSE_SIZE];
RAM uint8_t ota_v2_write_buffer[OTA_V2_MAX_DATA];
RAM uint8_t ota_v2_verify_buffer[OTA_V2_VERIFY_CHUNK];
RAM uint8_t ota_v2_marker_buffer[4];
RAM uint8_t ota_v2_target_marker[4] = {'K', 'N', 'L', 'T'};
RAM uint8_t ota_v2_retired_marker[4] = {0x00U, 'N', 'L', 'T'};
RAM ota_recovery_scan_t ota_v2_recovery_scan;
RAM uint8_t ota_v2_recovery_state;
RAM uint8_t ota_v2_recovery_action;
RAM uint8_t ota_v2_recovery_backup_valid;
RAM uint8_t ota_v2_trial_active;
RAM uint8_t ota_v2_trial_runtime_ready;
RAM uint8_t ota_v2_rollback_pending;
RAM uint32_t ota_v2_trial_started;

static uint8_t ota_v2_get_slots(uint8_t *current_slot, uint8_t *target_slot);
static void ota_v2_reset_session(void);
static uint8_t ota_v2_validate_flash_image(void);
static uint8_t ota_v2_rebuild_session_from_record(void);
static _attribute_ram_code_ void ota_v2_resume_switch(void);
static _attribute_ram_code_ uint8_t ota_v2_append_existing_recovery_record(
    uint8_t state);

#ifdef OTA_V2_HOST_TEST
extern void ota_v2_test_watchdog_start(void);
extern void ota_v2_test_watchdog_stop(void);
extern void ota_v2_test_watchdog_clear(void);
#define OTA_V2_WATCHDOG_START() ota_v2_test_watchdog_start()
#define OTA_V2_WATCHDOG_STOP() ota_v2_test_watchdog_stop()
#define OTA_V2_WATCHDOG_CLEAR() ota_v2_test_watchdog_clear()
#else
static void ota_v2_watchdog_start(void)
{
    wd_set_interval_ms(OTA_V2_TRIAL_WATCHDOG_MS, CLOCK_SYS_CLOCK_1MS);
    wd_start();
}
#define OTA_V2_WATCHDOG_START() ota_v2_watchdog_start()
#define OTA_V2_WATCHDOG_STOP() wd_stop()
#define OTA_V2_WATCHDOG_CLEAR() wd_clear()
#endif

static uint16_t ota_v2_read_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t ota_v2_read_u32_le(const uint8_t *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t ota_v2_read_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           ((uint32_t)p[3]);
}

static void ota_v2_write_u32_le(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t ota_v2_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    uint32_t i;
    uint8_t bit;

    for (i = 0; i < len; ++i)
    {
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
        }
    }
    return crc;
}

static uint8_t ota_v2_boot_marker_valid(uint32_t base)
{
    flash_read_page(base + OTA_V2_BOOT_MARKER_OFFSET, 4U,
                    ota_v2_marker_buffer);
    return memcmp(ota_v2_marker_buffer, ota_v2_target_marker, 4U) == 0;
}

static uint32_t ota_v2_crc_flash_raw(uint32_t address, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t chunk;

    while (length != 0U)
    {
        chunk = length > OTA_V2_VERIFY_CHUNK ? OTA_V2_VERIFY_CHUNK : length;
        flash_read_page(address, chunk, ota_v2_verify_buffer);
        crc = ota_v2_crc32_update(crc, ota_v2_verify_buffer, chunk);
        address += chunk;
        length -= chunk;
    }
    return crc ^ 0xFFFFFFFFUL;
}

static void ota_v2_scan_recovery_journal(void)
{
    uint16_t index;

    ota_recovery_scan_begin(&ota_v2_recovery_scan,
                            OTA_RECOVERY_JOURNAL_RECORDS);
    for (index = 0U; index < OTA_RECOVERY_JOURNAL_RECORDS; ++index)
    {
        flash_read_page(OTA_RECOVERY_JOURNAL_ADDRESS +
                            (uint32_t)index * OTA_RECOVERY_RECORD_SIZE,
                        OTA_RECOVERY_RECORD_SIZE, ota_v2_verify_buffer);
        ota_recovery_scan_entry(&ota_v2_recovery_scan,
                                ota_v2_verify_buffer, index);
    }
}

void ota_v2_recovery_init(void)
{
    ota_recovery_observation_t observation;
    uint8_t current_slot;
    uint8_t target_slot;

    memset(&observation, 0, sizeof(observation));
    ota_v2_scan_recovery_journal();
    ota_v2_get_slots(&current_slot, &target_slot);
    observation.has_record = ota_v2_recovery_scan.found;
    observation.latest = ota_v2_recovery_scan.latest;
    observation.running_slot = current_slot;
    observation.slot_a_valid = ota_v2_boot_marker_valid(OTA_V2_SLOT_A_START);
    observation.slot_b_valid = ota_v2_boot_marker_valid(OTA_V2_SLOT_B_START);

    ota_v2_recovery_backup_valid = 0U;
    if (ota_v2_recovery_scan.found &&
        ota_v2_recovery_scan.latest.backup_crc32 != 0U)
    {
        flash_read_page(OTA_RECOVERY_BACKUP_ADDRESS +
                            OTA_RECOVERY_BOOT_MARKER_OFFSET,
                        1U, ota_v2_marker_buffer);
        ota_v2_recovery_backup_valid =
            ota_v2_marker_buffer[0] == 0xFFU &&
            ota_v2_crc_flash_raw(OTA_RECOVERY_BACKUP_ADDRESS,
                                 OTA_RECOVERY_BACKUP_SIZE) ==
                ota_v2_recovery_scan.latest.backup_crc32;
    }
    observation.backup_valid = ota_v2_recovery_backup_valid;
    observation.trial_attempts = ota_v2_recovery_scan.trial_attempts;
    observation.max_trial_attempts = OTA_V2_MAX_TRIAL_ATTEMPTS;

    ota_v2_recovery_state = ota_v2_recovery_scan.found
                                ? ota_v2_recovery_scan.latest.state
                                : 0U;
    ota_v2_recovery_action = (uint8_t)ota_recovery_decide(&observation);
    if ((ota_v2_recovery_action == OTA_RECOVERY_ACTION_RESUME_INSTALL ||
         ota_v2_recovery_action == OTA_RECOVERY_ACTION_RESUME_SWITCH) &&
        !ota_v2_rebuild_session_from_record())
    {
        ota_v2_recovery_action = OTA_RECOVERY_ACTION_SWIRE_RESCUE;
        return;
    }
    ota_v2_trial_active = 0U;
    ota_v2_trial_runtime_ready = 0U;
    ota_v2_rollback_pending = 0U;

    if (ota_v2_recovery_action == OTA_RECOVERY_ACTION_START_TRIAL)
    {
        if (!ota_v2_append_existing_recovery_record(
                OTA_RECOVERY_STATE_TRIAL_ATTEMPT))
        {
            ota_v2_recovery_action = OTA_RECOVERY_ACTION_SWIRE_RESCUE;
            return;
        }
        ota_v2_recovery_state = OTA_RECOVERY_STATE_TRIAL_ATTEMPT;
        ota_v2_trial_active = 1U;
        OTA_V2_WATCHDOG_START();
    }
    else if (ota_v2_recovery_action == OTA_RECOVERY_ACTION_ROLLBACK_SOURCE ||
             ota_v2_recovery_action ==
                 OTA_RECOVERY_ACTION_CLEANUP_FAILED_TARGET)
    {
        ota_v2_rollback_pending = 1U;
    }
}

void ota_v2_recovery_runtime_ready(void)
{
    if (ota_v2_trial_active)
    {
        ota_v2_trial_started = clock_time();
        ota_v2_trial_runtime_ready = 1U;
    }
}

_attribute_ram_code_ uint8_t ota_v2_recovery_requires_awake(void)
{
    /* The TLSR8258 timer watchdog uses the system clock, which is paused by
     * BLE suspend. Keep the MCU awake only for the bounded trial window so a
     * stalled candidate is guaranteed to reach the watchdog deadline. */
    return ota_v2_trial_active;
}

static void ota_v2_reset_session(void)
{
    memset(&ota_v2_session, 0, sizeof(ota_v2_session));
    ota_v2_session.crc32_state = 0xFFFFFFFFUL;
    ota_v2_session.phase = OTA_V2_PHASE_IDLE;
}

static uint8_t ota_v2_get_slots(uint8_t *current_slot, uint8_t *target_slot)
{
    uint32_t target_base = ota_v2_session.layout_valid
                               ? ota_v2_session.target_base
                               : ota_program_offset;

    if (target_base == OTA_V2_SLOT_A_START)
    {
        *current_slot = 1U;
        *target_slot = 0U;
        return 1U;
    }
    if (target_base == OTA_V2_SLOT_B_START)
    {
        *current_slot = 0U;
        *target_slot = 1U;
        return 1U;
    }
    *current_slot = 0xFFU;
    *target_slot = 0xFFU;
    return 0U;
}

static void ota_v2_send_status(uint8_t command, uint8_t status)
{
    uint32_t visible_crc = ota_v2_session.crc32_state ^ 0xFFFFFFFFUL;
    uint8_t current_slot;
    uint8_t target_slot;

    ota_v2_get_slots(&current_slot, &target_slot);
    ota_v2_session.last_status = status;
    ota_v2_response[0] = OTA_V2_RESPONSE_MAGIC;
    ota_v2_response[1] = OTA_V2_PROTOCOL_VERSION;
    ota_v2_response[2] = command;
    ota_v2_response[3] = status;
    ota_v2_response[4] = ota_v2_session.phase;
    ota_v2_response[5] = ota_v2_session.sectors_erased;
    ota_v2_response[6] = ota_v2_session.sectors_total;
    ota_v2_response[7] = OTA_V2_MAX_DATA;
    ota_v2_write_u32_le(&ota_v2_response[8], ota_v2_session.received);
    ota_v2_write_u32_le(&ota_v2_response[12], ota_v2_session.image_size);
    ota_v2_write_u32_le(&ota_v2_response[16], visible_crc);
    ota_v2_response[20] = OTA_V2_CAP_INSTALL | OTA_V2_CAP_RECOVERY_LOG;
    ota_v2_response[21] = current_slot;
    ota_v2_response[22] = target_slot;
    ota_v2_response[23] = OTA_V2_MILESTONE;
    ota_v2_write_u32_le(&ota_v2_response[24], HINK_FW_VERSION);
    ota_v2_response[28] = ota_v2_recovery_state;
    ota_v2_response[29] = ota_v2_recovery_action;
    ota_v2_response[30] = (uint8_t)ota_v2_recovery_scan.valid_records;
    ota_v2_response[31] = (uint8_t)ota_v2_recovery_scan.corrupt_records;
    bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, ota_v2_response, sizeof(ota_v2_response));
}

static _attribute_ram_code_ uint8_t ota_v2_write_and_verify_marker(
    uint32_t address, const uint8_t *marker)
{
    uint8_t i;

    flash_write_page(address, 4U, (uint8_t *)marker);
    flash_read_page(address, 4U, ota_v2_marker_buffer);
    for (i = 0U; i < 4U; ++i)
    {
        if (ota_v2_marker_buffer[i] != marker[i])
        {
            return 0U;
        }
    }
    return 1U;
}

static _attribute_ram_code_ uint8_t ota_v2_prepare_source_backup(
    uint32_t source_base, uint32_t *backup_crc32)
{
    uint32_t offset = 0U;
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t chunk;

    flash_erase_sector(OTA_RECOVERY_BACKUP_ADDRESS);
    while (offset < OTA_RECOVERY_BACKUP_SIZE)
    {
        chunk = OTA_RECOVERY_BACKUP_SIZE - offset;
        if (chunk > OTA_V2_VERIFY_CHUNK)
        {
            chunk = OTA_V2_VERIFY_CHUNK;
        }
        flash_read_page(source_base + offset, chunk, ota_v2_verify_buffer);
        if (offset <= OTA_RECOVERY_BOOT_MARKER_OFFSET &&
            OTA_RECOVERY_BOOT_MARKER_OFFSET < offset + chunk)
        {
            ota_v2_verify_buffer[OTA_RECOVERY_BOOT_MARKER_OFFSET - offset] =
                0xFFU;
        }
        crc = ota_v2_crc32_update(crc, ota_v2_verify_buffer, chunk);
        flash_write_page(OTA_RECOVERY_BACKUP_ADDRESS + offset, chunk,
                         ota_v2_verify_buffer);
        offset += chunk;
    }

    *backup_crc32 = crc ^ 0xFFFFFFFFUL;
    flash_read_page(OTA_RECOVERY_BACKUP_ADDRESS +
                        OTA_RECOVERY_BOOT_MARKER_OFFSET,
                    1U, ota_v2_marker_buffer);
    return ota_v2_marker_buffer[0] == 0xFFU &&
           ota_v2_crc_flash_raw(OTA_RECOVERY_BACKUP_ADDRESS,
                                OTA_RECOVERY_BACKUP_SIZE) == *backup_crc32;
}

static _attribute_ram_code_ uint8_t ota_v2_store_recovery_record(
    const ota_recovery_record_t *record)
{
    ota_recovery_record_t verified;
    uint32_t address;

    if (ota_v2_recovery_scan.full)
    {
        return 0U;
    }
    ota_recovery_encode(record, ota_v2_verify_buffer);

    address = OTA_RECOVERY_JOURNAL_ADDRESS +
              (uint32_t)ota_v2_recovery_scan.next_index *
                  OTA_RECOVERY_RECORD_SIZE;
    flash_write_page(address, OTA_RECOVERY_RECORD_SIZE,
                     ota_v2_verify_buffer);
    flash_read_page(address, OTA_RECOVERY_RECORD_SIZE,
                    ota_v2_verify_buffer);
    if (!ota_recovery_decode(ota_v2_verify_buffer, &verified) ||
        verified.state != record->state ||
        verified.sequence != record->sequence ||
        verified.source_slot != record->source_slot ||
        verified.target_slot != record->target_slot ||
        verified.source_version != record->source_version ||
        verified.target_version != record->target_version ||
        verified.candidate_crc32 != record->candidate_crc32 ||
        verified.backup_crc32 != record->backup_crc32)
    {
        return 0U;
    }

    /* Keep the status snapshot current immediately after a verified append.
     * Every append rescans before choosing its Flash address, but INFO may be
     * served before the next boot/rescan (notably just after trial confirm). */
    ota_v2_recovery_scan.valid_records++;
    if (record->state == OTA_RECOVERY_STATE_INSTALL_INTENT ||
        record->state == OTA_RECOVERY_STATE_MARKERS_SWITCHED)
    {
        ota_v2_recovery_scan.trial_attempts = 0U;
    }
    else if (record->state == OTA_RECOVERY_STATE_TRIAL_ATTEMPT &&
             ota_v2_recovery_scan.trial_attempts != 0xFFU)
    {
        ota_v2_recovery_scan.trial_attempts++;
    }
    ota_v2_recovery_scan.latest = verified;
    ota_v2_recovery_scan.found = 1U;
    ota_v2_recovery_state = record->state;
    return 1U;
}

static _attribute_ram_code_ uint8_t ota_v2_append_recovery_record(
    uint8_t state, uint32_t source_base, uint32_t backup_crc32)
{
    ota_recovery_record_t record;

    ota_v2_scan_recovery_journal();
    memset(&record, 0, sizeof(record));
    record.state = state;
    record.source_slot = source_base == OTA_V2_SLOT_A_START
                             ? OTA_RECOVERY_SLOT_A
                             : OTA_RECOVERY_SLOT_B;
    record.target_slot = record.source_slot == OTA_RECOVERY_SLOT_A
                             ? OTA_RECOVERY_SLOT_B
                             : OTA_RECOVERY_SLOT_A;
    record.sequence = ota_v2_recovery_scan.found
                          ? ota_v2_recovery_scan.latest.sequence + 1U
                          : 1U;
    record.source_version = HINK_FW_VERSION;
    record.target_version = ota_v2_session.candidate_version;
    record.candidate_crc32 = ota_v2_session.expected_crc32;
    record.backup_crc32 = backup_crc32;
    if (!ota_v2_store_recovery_record(&record))
    {
        return 0U;
    }
    ota_v2_recovery_backup_valid = 1U;
    return 1U;
}

static _attribute_ram_code_ uint8_t ota_v2_append_existing_recovery_record(
    uint8_t state)
{
    ota_recovery_record_t record;

    ota_v2_scan_recovery_journal();
    if (!ota_v2_recovery_scan.found)
    {
        return 0U;
    }
    record = ota_v2_recovery_scan.latest;
    record.state = state;
    record.sequence++;
    return ota_v2_store_recovery_record(&record);
}

#ifdef OTA_V2_HOST_TEST
extern void ota_v2_test_reboot(void);
#define OTA_V2_REBOOT() ota_v2_test_reboot()
#else
static _attribute_ram_code_ void ota_v2_reboot(void)
{
    analog_write(SYS_DEEP_ANA_REG,
                 analog_read(SYS_DEEP_ANA_REG) & ~SYS_NEED_REINIT_EXT32K);
    REG_ADDR8(0x6f) = 0x20;
    while (1)
    {
    }
}
#define OTA_V2_REBOOT() ota_v2_reboot()
#endif

static _attribute_ram_code_ void ota_v2_resume_switch(void)
{
    uint32_t source_base;
    uint32_t target_base;
    uint8_t irq_state;

    source_base = ota_v2_recovery_scan.latest.source_slot ==
                          OTA_RECOVERY_SLOT_A
                      ? OTA_V2_SLOT_A_START
                      : OTA_V2_SLOT_B_START;
    target_base = ota_v2_recovery_scan.latest.target_slot ==
                          OTA_RECOVERY_SLOT_A
                      ? OTA_V2_SLOT_A_START
                      : OTA_V2_SLOT_B_START;
    irq_state = irq_disable();

    if (!ota_v2_boot_marker_valid(target_base) &&
        !ota_v2_write_and_verify_marker(target_base + OTA_V2_BOOT_MARKER_OFFSET,
                                        ota_v2_target_marker))
    {
        ota_v2_recovery_action = OTA_RECOVERY_ACTION_SWIRE_RESCUE;
        irq_restore(irq_state);
        return;
    }
    if (ota_v2_boot_marker_valid(source_base) &&
        !ota_v2_write_and_verify_marker(source_base + OTA_V2_BOOT_MARKER_OFFSET,
                                        ota_v2_retired_marker))
    {
        ota_v2_recovery_action = OTA_RECOVERY_ACTION_SWIRE_RESCUE;
        irq_restore(irq_state);
        return;
    }
    if (!ota_v2_append_existing_recovery_record(
            OTA_RECOVERY_STATE_MARKERS_SWITCHED))
    {
        ota_v2_recovery_action = OTA_RECOVERY_ACTION_SWIRE_RESCUE;
        irq_restore(irq_state);
        return;
    }

    irq_restore(irq_state);
    OTA_V2_REBOOT();
}

static _attribute_ram_code_ void ota_v2_run_rollback(void)
{
    uint32_t source_base;
    uint32_t target_base;
    uint32_t offset;
    uint32_t chunk;
    uint8_t irq_state;
    uint8_t rollback_intent_written = 0U;
    (void)rollback_intent_written;

    ota_v2_rollback_pending = 0U;
    ota_v2_scan_recovery_journal();
    if (!ota_v2_recovery_scan.found)
    {
        ota_v2_recovery_action = OTA_RECOVERY_ACTION_SWIRE_RESCUE;
        return;
    }

    source_base = ota_v2_recovery_scan.latest.source_slot ==
                          OTA_RECOVERY_SLOT_A
                      ? OTA_V2_SLOT_A_START
                      : OTA_V2_SLOT_B_START;
    target_base = source_base == OTA_V2_SLOT_A_START
                      ? OTA_V2_SLOT_B_START
                      : OTA_V2_SLOT_A_START;
    irq_state = irq_disable();

    if (ota_v2_recovery_action ==
        OTA_RECOVERY_ACTION_CLEANUP_FAILED_TARGET)
    {
        if (ota_v2_boot_marker_valid(target_base) &&
            !ota_v2_write_and_verify_marker(
                target_base + OTA_V2_BOOT_MARKER_OFFSET,
                ota_v2_retired_marker))
        {
            ota_v2_recovery_action = OTA_RECOVERY_ACTION_SWIRE_RESCUE;
            irq_restore(irq_state);
            return;
        }
        if (!ota_v2_append_existing_recovery_record(
                OTA_RECOVERY_STATE_ROLLBACK_DONE))
        {
            /* The old source is already the sole bootable image. Stay up
             * instead of creating a reboot loop when the journal is full. */
            ota_v2_recovery_action = OTA_RECOVERY_ACTION_NORMAL;
            irq_restore(irq_state);
            return;
        }
        OTA_V2_REBOOT();
        return;
    }

    if (!ota_v2_recovery_backup_valid)
    {
        ota_v2_recovery_action = OTA_RECOVERY_ACTION_SWIRE_RESCUE;
        irq_restore(irq_state);
        return;
    }

    if (ota_v2_recovery_scan.latest.state !=
        OTA_RECOVERY_STATE_ROLLBACK_INTENT)
    {
        if (!ota_v2_append_existing_recovery_record(
                OTA_RECOVERY_STATE_ROLLBACK_INTENT))
        {
            ota_v2_recovery_action = OTA_RECOVERY_ACTION_SWIRE_RESCUE;
            irq_restore(irq_state);
            return;
        }
        rollback_intent_written = 1U;
    }

#if HINK_OTA_SW_RESET_POINT == 6
    if (rollback_intent_written)
    {
        OTA_V2_REBOOT();
    }
#endif

    flash_erase_sector(source_base);
    for (offset = 0U; offset < OTA_RECOVERY_BACKUP_SIZE; offset += chunk)
    {
        chunk = OTA_RECOVERY_BACKUP_SIZE - offset;
        if (chunk > OTA_V2_VERIFY_CHUNK)
        {
            chunk = OTA_V2_VERIFY_CHUNK;
        }
        flash_read_page(OTA_RECOVERY_BACKUP_ADDRESS + offset, chunk,
                        ota_v2_verify_buffer);
        flash_write_page(source_base + offset, chunk,
                         ota_v2_verify_buffer);
    }

    flash_read_page(source_base + OTA_RECOVERY_BOOT_MARKER_OFFSET,
                    1U, ota_v2_marker_buffer);
    if (ota_v2_marker_buffer[0] != 0xFFU ||
        ota_v2_crc_flash_raw(source_base, OTA_RECOVERY_BACKUP_SIZE) !=
            ota_v2_recovery_scan.latest.backup_crc32 ||
        !ota_v2_write_and_verify_marker(
            source_base + OTA_V2_BOOT_MARKER_OFFSET,
            ota_v2_target_marker))
    {
        ota_v2_recovery_action = OTA_RECOVERY_ACTION_SWIRE_RESCUE;
        irq_restore(irq_state);
        return;
    }

    if (!ota_v2_write_and_verify_marker(
            target_base + OTA_V2_BOOT_MARKER_OFFSET,
            ota_v2_retired_marker))
    {
        /* The restored source is already bootable. Reboot lets ROM either
         * choose it directly or retry this rollback from the candidate. */
        OTA_V2_REBOOT();
        return;
    }

    (void)ota_v2_append_existing_recovery_record(
        OTA_RECOVERY_STATE_ROLLBACK_DONE);
    OTA_V2_REBOOT();
}

_attribute_ram_code_ void ota_v2_process(void)
{
    uint32_t current_base;
    uint32_t backup_crc32;
    uint8_t irq_state;

    if (ota_v2_rollback_pending)
    {
        ota_v2_run_rollback();
        return;
    }

    if (ota_v2_recovery_action == OTA_RECOVERY_ACTION_RESUME_INSTALL ||
        ota_v2_recovery_action == OTA_RECOVERY_ACTION_RESUME_SWITCH)
    {
        ota_v2_resume_switch();
        return;
    }

    if (ota_v2_trial_active)
    {
#ifdef HINK_OTA_TRIAL_FAIL_TEST
        /* Deliberate M2-D candidate: after two seconds of otherwise normal
         * startup, stop feeding the trial watchdog so repeated boots exercise
         * the persisted attempt counter and rollback path. */
        if (ota_v2_trial_runtime_ready &&
            clock_time_exceed(ota_v2_trial_started, 2000000UL))
        {
            return;
        }
#endif
        OTA_V2_WATCHDOG_CLEAR();
        if (ota_v2_trial_runtime_ready &&
            clock_time_exceed(ota_v2_trial_started,
                              OTA_V2_TRIAL_CONFIRM_US))
        {
            if (ota_v2_append_existing_recovery_record(
                    OTA_RECOVERY_STATE_CONFIRMED))
            {
                ota_v2_recovery_state = OTA_RECOVERY_STATE_CONFIRMED;
                ota_v2_recovery_action = OTA_RECOVERY_ACTION_NORMAL;
            }
            else
            {
                ota_v2_recovery_action =
                    OTA_RECOVERY_ACTION_SWIRE_RESCUE;
            }
            ota_v2_trial_active = 0U;
            ota_v2_trial_runtime_ready = 0U;
            OTA_V2_WATCHDOG_STOP();
        }
    }

    if (ota_v2_session.phase != OTA_V2_PHASE_INSTALL_PENDING ||
        !clock_time_exceed(ota_v2_session.install_started,
                           OTA_V2_INSTALL_DELAY_US))
    {
        return;
    }

    current_base = ota_v2_session.target_base == OTA_V2_SLOT_A_START
                       ? OTA_V2_SLOT_B_START
                       : OTA_V2_SLOT_A_START;
    irq_state = irq_disable();

    /* No boot marker changes until the complete masked source sector and the
     * INSTALL_INTENT record have both survived a full Flash readback. */
    if (!ota_v2_prepare_source_backup(current_base, &backup_crc32))
    {
        ota_v2_session.phase = OTA_V2_PHASE_ERROR;
        ota_v2_session.last_status = OTA_V2_STATUS_RECOVERY_BACKUP;
        irq_restore(irq_state);
        return;
    }
    if (!ota_v2_append_recovery_record(
            OTA_RECOVERY_STATE_INSTALL_INTENT, current_base, backup_crc32))
    {
        ota_v2_session.phase = OTA_V2_PHASE_ERROR;
        ota_v2_session.last_status = OTA_V2_STATUS_RECOVERY_JOURNAL;
        irq_restore(irq_state);
        return;
    }

#if HINK_OTA_SW_RESET_POINT == 2
    OTA_V2_REBOOT();
#endif

    /* The candidate becomes bootable first. The running image is retired only
     * after the complete four-byte target marker was read back exactly. */
    if (!ota_v2_write_and_verify_marker(
            ota_v2_session.target_base + OTA_V2_BOOT_MARKER_OFFSET,
            ota_v2_target_marker))
    {
        ota_v2_session.phase = OTA_V2_PHASE_ERROR;
        ota_v2_session.last_status = OTA_V2_STATUS_BOOT_MARK_VERIFY;
        irq_restore(irq_state);
        return;
    }

    if (!ota_v2_write_and_verify_marker(
            current_base + OTA_V2_BOOT_MARKER_OFFSET,
            ota_v2_retired_marker))
    {
        ota_v2_session.phase = OTA_V2_PHASE_ERROR;
        ota_v2_session.last_status = OTA_V2_STATUS_BOOT_MARK_VERIFY;
        irq_restore(irq_state);
        return;
    }

    if (!ota_v2_append_recovery_record(
            OTA_RECOVERY_STATE_MARKERS_SWITCHED, current_base, backup_crc32))
    {
        ota_v2_session.phase = OTA_V2_PHASE_ERROR;
        ota_v2_session.last_status = OTA_V2_STATUS_RECOVERY_JOURNAL;
        irq_restore(irq_state);
        return;
    }

#if HINK_OTA_SW_RESET_POINT == 3
    OTA_V2_REBOOT();
#endif

    OTA_V2_REBOOT();
}

static uint32_t ota_v2_crc_flash_virtual(uint32_t base, uint32_t length,
                                         uint8_t *boot_marker_masked)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t offset = 0U;
    uint32_t chunk;

    *boot_marker_masked = 1U;
    while (offset < length)
    {
        chunk = length - offset;
        if (chunk > OTA_V2_VERIFY_CHUNK)
        {
            chunk = OTA_V2_VERIFY_CHUNK;
        }
        flash_read_page(base + offset, chunk, ota_v2_verify_buffer);
        if (offset <= OTA_V2_BOOT_MARKER_OFFSET &&
            OTA_V2_BOOT_MARKER_OFFSET < offset + chunk)
        {
            uint32_t marker_index = OTA_V2_BOOT_MARKER_OFFSET - offset;
            if (ota_v2_verify_buffer[marker_index] != 0xFFU &&
                ota_v2_verify_buffer[marker_index] != 0x4BU)
            {
                *boot_marker_masked = 0U;
            }
            ota_v2_verify_buffer[marker_index] = 0x4BU;
        }
        crc = ota_v2_crc32_update(crc, ota_v2_verify_buffer, chunk);
        offset += chunk;
    }
    return crc ^ 0xFFFFFFFFUL;
}

static uint8_t ota_v2_validate_flash_image(void)
{
    uint32_t payload_length;
    uint32_t manifest_address;
    uint32_t manifest_crc;
    uint32_t payload_crc;
    uint32_t expected_payload_crc;
    uint32_t telink_crc;
    uint32_t telink_length;
    uint8_t boot_marker_masked;

    if (ota_v2_session.image_size < (OTA_V2_MANIFEST_SIZE + 32U))
    {
        return OTA_V2_STATUS_BAD_MANIFEST;
    }

    if (ota_v2_crc_flash_virtual(ota_v2_session.target_base,
                                 ota_v2_session.image_size,
                                 &boot_marker_masked) != ota_v2_session.expected_crc32)
    {
        return OTA_V2_STATUS_CRC_MISMATCH;
    }
    if (!boot_marker_masked)
    {
        return OTA_V2_STATUS_BAD_IMAGE_HEADER;
    }

    manifest_address = ota_v2_session.target_base +
                       ota_v2_session.image_size - OTA_V2_MANIFEST_SIZE;
    flash_read_page(manifest_address, OTA_V2_MANIFEST_SIZE, ota_v2_verify_buffer);
    payload_length = ota_v2_read_u32_le(&ota_v2_verify_buffer[8]);
    if (memcmp(ota_v2_verify_buffer, "HOTA", 4U) != 0 ||
        ota_v2_verify_buffer[4] != OTA_V2_MANIFEST_FORMAT ||
        ota_v2_verify_buffer[5] != 0U ||
        ota_v2_read_u16_le(&ota_v2_verify_buffer[6]) != OTA_V2_BOARD_ID ||
        payload_length != ota_v2_session.image_size - OTA_V2_MANIFEST_SIZE)
    {
        return OTA_V2_STATUS_BAD_MANIFEST;
    }
    manifest_crc = ota_v2_crc32_update(0xFFFFFFFFUL,
                                       ota_v2_verify_buffer, 20U) ^ 0xFFFFFFFFUL;
    if (manifest_crc != ota_v2_read_u32_le(&ota_v2_verify_buffer[20]))
    {
        return OTA_V2_STATUS_BAD_MANIFEST;
    }
    expected_payload_crc = ota_v2_read_u32_le(&ota_v2_verify_buffer[12]);
    ota_v2_session.candidate_version =
        ota_v2_read_u32_le(&ota_v2_verify_buffer[16]);

    payload_crc = ota_v2_crc_flash_virtual(ota_v2_session.target_base,
                                            payload_length,
                                            &boot_marker_masked);
    if (!boot_marker_masked ||
        payload_crc != expected_payload_crc)
    {
        return OTA_V2_STATUS_BAD_MANIFEST;
    }

    flash_read_page(ota_v2_session.target_base, 28U, ota_v2_verify_buffer);
    telink_length = ota_v2_read_u32_le(&ota_v2_verify_buffer[OTA_V2_TELINK_LENGTH_OFFSET]);
    if ((ota_v2_verify_buffer[OTA_V2_BOOT_MARKER_OFFSET] != 0xFFU &&
         ota_v2_verify_buffer[OTA_V2_BOOT_MARKER_OFFSET] != 0x4BU) ||
        ota_v2_verify_buffer[9] != 'N' ||
        ota_v2_verify_buffer[10] != 'L' ||
        ota_v2_verify_buffer[11] != 'T' ||
        telink_length + 4U != payload_length)
    {
        return OTA_V2_STATUS_BAD_IMAGE_HEADER;
    }

    flash_read_page(ota_v2_session.target_base + payload_length - 4U,
                    4U, ota_v2_verify_buffer);
    telink_crc = ota_v2_read_u32_be(ota_v2_verify_buffer);
    payload_crc = ota_v2_crc_flash_virtual(ota_v2_session.target_base,
                                            payload_length - 4U,
                                            &boot_marker_masked);
    if (!boot_marker_masked || payload_crc != telink_crc)
    {
        return OTA_V2_STATUS_BAD_IMAGE_HEADER;
    }

    return OTA_V2_STATUS_OK;
}

static uint8_t ota_v2_rebuild_session_from_record(void)
{
    uint32_t target_base;
    uint32_t telink_length;
    uint32_t payload_length;
    uint32_t image_size;

    target_base = ota_v2_recovery_scan.latest.target_slot ==
                          OTA_RECOVERY_SLOT_A
                      ? OTA_V2_SLOT_A_START
                      : OTA_V2_SLOT_B_START;
    flash_read_page(target_base + OTA_V2_TELINK_LENGTH_OFFSET, 4U,
                    ota_v2_verify_buffer);
    telink_length = ota_v2_read_u32_le(ota_v2_verify_buffer);
    if (telink_length < 32U || telink_length > OTA_V2_SLOT_SIZE - 28U)
    {
        return 0U;
    }
    payload_length = telink_length + 4U;
    image_size = payload_length + OTA_V2_MANIFEST_SIZE;
    if (image_size > OTA_V2_SLOT_SIZE)
    {
        return 0U;
    }

    ota_v2_reset_session();
    ota_v2_session.image_size = image_size;
    ota_v2_session.expected_crc32 =
        ota_v2_recovery_scan.latest.candidate_crc32;
    ota_v2_session.target_base = target_base;
    ota_v2_session.layout_valid = 1U;
    if (ota_v2_validate_flash_image() != OTA_V2_STATUS_OK ||
        ota_v2_session.candidate_version !=
            ota_v2_recovery_scan.latest.target_version)
    {
        ota_v2_reset_session();
        return 0U;
    }
    return 1U;
}

_attribute_ram_code_ int custom_otaWrite(void *p)
{
    rf_packet_att_write_t *req = (rf_packet_att_write_t *)p;
    uint8_t *payload = (uint8_t *)&req->value;
    uint16_t data_len;
    uint8_t command;
    uint8_t validation_status;
    uint32_t address;
    uint32_t offset;
    uint32_t image_size;
    uint32_t expected_crc32;
    uint16_t board_id;
    uint8_t write_len;
    uint8_t i;

    if (req->l2capLen < 4U)
    {
        ota_v2_send_status(0U, OTA_V2_STATUS_BAD_LENGTH);
        return 0;
    }

    data_len = req->l2capLen - 3U;
    command = payload[0];

    switch (command)
    {
    case OTA_V2_CMD_INFO:
    case OTA_V2_CMD_STATUS:
        ota_v2_send_status(command,
                           data_len == 1U ? OTA_V2_STATUS_OK
                                         : OTA_V2_STATUS_BAD_LENGTH);
        break;

    case OTA_V2_CMD_BEGIN:
        if (data_len != 12U)
        {
            ota_v2_send_status(command, OTA_V2_STATUS_BAD_LENGTH);
            break;
        }
        if (payload[1] != OTA_V2_PROTOCOL_VERSION)
        {
            ota_v2_send_status(command, OTA_V2_STATUS_BAD_PROTOCOL);
            break;
        }
        board_id = (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);
        if (board_id != OTA_V2_BOARD_ID)
        {
            ota_v2_send_status(command, OTA_V2_STATUS_BAD_TARGET);
            break;
        }
        image_size = ota_v2_read_u32_le(&payload[4]);
        expected_crc32 = ota_v2_read_u32_le(&payload[8]);
        if (image_size < (OTA_V2_MANIFEST_SIZE + 32U) ||
            image_size > OTA_V2_SLOT_SIZE)
        {
            ota_v2_send_status(command, OTA_V2_STATUS_RANGE);
            break;
        }

        ota_v2_reset_session();
        if (ota_program_offset != OTA_V2_SLOT_A_START &&
            ota_program_offset != OTA_V2_SLOT_B_START)
        {
            ota_v2_session.phase = OTA_V2_PHASE_ERROR;
            ota_v2_send_status(command, OTA_V2_STATUS_UNSUPPORTED_LAYOUT);
            break;
        }
        ota_v2_session.image_size = image_size;
        ota_v2_session.expected_crc32 = expected_crc32;
        ota_v2_session.target_base = ota_program_offset;
        ota_v2_session.layout_valid = 1U;
        ota_v2_session.sectors_total =
            (uint8_t)((image_size + OTA_V2_SECTOR_SIZE - 1U) / OTA_V2_SECTOR_SIZE);
        ota_v2_session.phase = OTA_V2_PHASE_ERASING;
        ota_v2_send_status(command, OTA_V2_STATUS_OK);
        break;

    case OTA_V2_CMD_ERASE_NEXT:
        if (data_len != 1U)
        {
            ota_v2_send_status(command, OTA_V2_STATUS_BAD_LENGTH);
            break;
        }
        if (!ota_v2_session.layout_valid ||
            ota_v2_session.phase != OTA_V2_PHASE_ERASING ||
            ota_v2_session.sectors_erased >= ota_v2_session.sectors_total)
        {
            ota_v2_send_status(command, OTA_V2_STATUS_BAD_STATE);
            break;
        }

        address = ota_v2_session.target_base +
                  ((uint32_t)ota_v2_session.sectors_erased * OTA_V2_SECTOR_SIZE);
        flash_erase_sector(address);
        ota_v2_session.sectors_erased++;
        if (ota_v2_session.sectors_erased == ota_v2_session.sectors_total)
        {
            ota_v2_session.phase = OTA_V2_PHASE_RECEIVING;
        }
        ota_v2_send_status(command, OTA_V2_STATUS_OK);
        break;

    case OTA_V2_CMD_DATA:
        if (data_len < 6U || data_len > (5U + OTA_V2_MAX_DATA))
        {
            ota_v2_send_status(command, OTA_V2_STATUS_BAD_LENGTH);
            break;
        }
        if (!ota_v2_session.layout_valid ||
            ota_v2_session.phase != OTA_V2_PHASE_RECEIVING)
        {
            ota_v2_send_status(command, OTA_V2_STATUS_BAD_STATE);
            break;
        }

        offset = ota_v2_read_u32_le(&payload[1]);
        write_len = (uint8_t)(data_len - 5U);
        if (offset != ota_v2_session.received)
        {
            ota_v2_send_status(command, OTA_V2_STATUS_ORDER);
            break;
        }
        if (offset + write_len > ota_v2_session.image_size ||
            ((offset & (OTA_V2_PAGE_SIZE - 1U)) + write_len) > OTA_V2_PAGE_SIZE)
        {
            ota_v2_send_status(command, OTA_V2_STATUS_RANGE);
            break;
        }

        memcpy(ota_v2_write_buffer, &payload[5], write_len);
        if (offset <= OTA_V2_BOOT_MARKER_OFFSET &&
            OTA_V2_BOOT_MARKER_OFFSET < offset + write_len)
        {
            i = (uint8_t)(OTA_V2_BOOT_MARKER_OFFSET - offset);
            if (ota_v2_write_buffer[i] != 0x4BU)
            {
                ota_v2_session.phase = OTA_V2_PHASE_ERROR;
                ota_v2_send_status(command, OTA_V2_STATUS_BAD_IMAGE_HEADER);
                break;
            }
            ota_v2_write_buffer[i] = 0xFFU;
        }

        address = ota_v2_session.target_base + offset;
        flash_write_page(address, write_len, ota_v2_write_buffer);
        flash_read_page(address, write_len, ota_v2_verify_buffer);
        if (memcmp(ota_v2_verify_buffer, ota_v2_write_buffer, write_len) != 0)
        {
            ota_v2_session.phase = OTA_V2_PHASE_ERROR;
            ota_v2_send_status(command, OTA_V2_STATUS_FLASH_VERIFY);
            break;
        }

        ota_v2_session.crc32_state =
            ota_v2_crc32_update(ota_v2_session.crc32_state, &payload[5], write_len);
        ota_v2_session.received += write_len;
        ota_v2_send_status(command, OTA_V2_STATUS_OK);
        break;

    case OTA_V2_CMD_FINISH:
        if (data_len != 1U)
        {
            ota_v2_send_status(command, OTA_V2_STATUS_BAD_LENGTH);
            break;
        }
        if (!ota_v2_session.layout_valid ||
            ota_v2_session.phase != OTA_V2_PHASE_RECEIVING ||
            ota_v2_session.received != ota_v2_session.image_size)
        {
            ota_v2_send_status(command, OTA_V2_STATUS_BAD_STATE);
            break;
        }
        if ((ota_v2_session.crc32_state ^ 0xFFFFFFFFUL) !=
            ota_v2_session.expected_crc32)
        {
            ota_v2_session.phase = OTA_V2_PHASE_ERROR;
            ota_v2_send_status(command, OTA_V2_STATUS_CRC_MISMATCH);
            break;
        }
        validation_status = ota_v2_validate_flash_image();
        if (validation_status != OTA_V2_STATUS_OK)
        {
            ota_v2_session.phase = OTA_V2_PHASE_ERROR;
            ota_v2_send_status(command, validation_status);
            break;
        }
        ota_v2_session.phase = OTA_V2_PHASE_VERIFIED;
        ota_v2_send_status(command, OTA_V2_STATUS_OK);
        break;

    case OTA_V2_CMD_ARM_INSTALL:
        if (data_len != 9U)
        {
            ota_v2_send_status(command, OTA_V2_STATUS_BAD_LENGTH);
            break;
        }
        if (!ota_v2_session.layout_valid ||
            ota_v2_session.phase != OTA_V2_PHASE_VERIFIED)
        {
            ota_v2_send_status(command, OTA_V2_STATUS_BAD_STATE);
            break;
        }
        if (ota_v2_read_u32_le(&payload[1]) != OTA_V2_ARM_CONFIRM ||
            ota_v2_read_u32_le(&payload[5]) != ota_v2_session.expected_crc32)
        {
            ota_v2_send_status(command, OTA_V2_STATUS_CONFIRM_REQUIRED);
            break;
        }
        validation_status = ota_v2_validate_flash_image();
        if (validation_status != OTA_V2_STATUS_OK)
        {
            ota_v2_session.phase = OTA_V2_PHASE_ERROR;
            ota_v2_send_status(command, validation_status);
            break;
        }
        ota_v2_session.phase = OTA_V2_PHASE_ARMED;
        ota_v2_send_status(command, OTA_V2_STATUS_OK);
        break;

    case OTA_V2_CMD_INSTALL:
        if (data_len != 9U)
        {
            ota_v2_send_status(command, OTA_V2_STATUS_BAD_LENGTH);
            break;
        }
        if (!ota_v2_session.layout_valid ||
            ota_v2_session.phase != OTA_V2_PHASE_ARMED)
        {
            ota_v2_send_status(command, OTA_V2_STATUS_BAD_STATE);
            break;
        }
        if (ota_v2_read_u32_le(&payload[1]) != OTA_V2_INSTALL_CONFIRM ||
            ota_v2_read_u32_le(&payload[5]) != ota_v2_session.expected_crc32)
        {
            ota_v2_send_status(command, OTA_V2_STATUS_CONFIRM_REQUIRED);
            break;
        }
        validation_status = ota_v2_validate_flash_image();
        if (validation_status != OTA_V2_STATUS_OK)
        {
            ota_v2_session.phase = OTA_V2_PHASE_ERROR;
            ota_v2_send_status(command, validation_status);
            break;
        }
        ota_v2_session.install_started = clock_time();
        ota_v2_session.phase = OTA_V2_PHASE_INSTALL_PENDING;
        ota_v2_send_status(command, OTA_V2_STATUS_OK);
        break;

    case OTA_V2_CMD_ABORT:
        if (data_len != 1U)
        {
            ota_v2_send_status(command, OTA_V2_STATUS_BAD_LENGTH);
            break;
        }
        if (ota_v2_session.phase == OTA_V2_PHASE_INSTALL_PENDING)
        {
            ota_v2_send_status(command, OTA_V2_STATUS_BAD_STATE);
        }
        else
        {
            ota_v2_reset_session();
            ota_v2_send_status(command, OTA_V2_STATUS_OK);
        }
        break;

    default:
        /* All inherited ATC commands remain unavailable. */
        ota_v2_send_status(command, OTA_V2_STATUS_UNKNOWN_CMD);
        break;
    }

    return 0;
}
