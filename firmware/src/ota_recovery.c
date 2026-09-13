#include <stdint.h>
#include <string.h>
#include "ota_recovery.h"

#define OTA_RECOVERY_FORMAT_VERSION 1U
#define OTA_RECOVERY_NO_BOOT_SLOT    0xFFU

static const uint8_t ota_recovery_magic[4] = {'H', '2', 'R', 'J'};

static uint32_t ota_recovery_read_u32_le(const uint8_t *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void ota_recovery_write_u32_le(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint8_t ota_recovery_state_valid(uint8_t state)
{
    return state >= OTA_RECOVERY_STATE_BASELINE &&
           state <= OTA_RECOVERY_STATE_ROLLBACK_DONE;
}

static uint8_t ota_recovery_slot_valid(uint8_t slot)
{
    return slot == OTA_RECOVERY_SLOT_A || slot == OTA_RECOVERY_SLOT_B;
}

static uint8_t ota_recovery_is_erased(const uint8_t *data, uint32_t length)
{
    uint32_t i;
    for (i = 0U; i < length; ++i)
    {
        if (data[i] != 0xFFU)
        {
            return 0U;
        }
    }
    return 1U;
}

static uint8_t ota_recovery_sequence_after(uint32_t left, uint32_t right)
{
    return left != right && (uint32_t)(left - right) < 0x80000000UL;
}

static uint8_t ota_recovery_marker_valid(
    const ota_recovery_observation_t *observation, uint8_t slot)
{
    return slot == OTA_RECOVERY_SLOT_A ? observation->slot_a_valid
                                       : observation->slot_b_valid;
}

uint32_t ota_recovery_crc32(const uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t i;
    uint8_t bit;

    for (i = 0U; i < length; ++i)
    {
        crc ^= data[i];
        for (bit = 0U; bit < 8U; ++bit)
        {
            crc = (crc >> 1) ^
                  (0xEDB88320UL & (0UL - (crc & 1UL)));
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

void ota_recovery_encode(const ota_recovery_record_t *record,
                         uint8_t output[OTA_RECOVERY_RECORD_SIZE])
{
    memcpy(output, ota_recovery_magic, sizeof(ota_recovery_magic));
    output[4] = OTA_RECOVERY_FORMAT_VERSION;
    output[5] = record->state;
    output[6] = record->source_slot;
    output[7] = record->target_slot;
    ota_recovery_write_u32_le(&output[8], record->sequence);
    ota_recovery_write_u32_le(&output[12], record->source_version);
    ota_recovery_write_u32_le(&output[16], record->target_version);
    ota_recovery_write_u32_le(&output[20], record->candidate_crc32);
    ota_recovery_write_u32_le(&output[24], record->backup_crc32);
    ota_recovery_write_u32_le(&output[28],
                              ota_recovery_crc32(output, 28U));
}

uint8_t ota_recovery_decode(const uint8_t input[OTA_RECOVERY_RECORD_SIZE],
                            ota_recovery_record_t *record)
{
    if (memcmp(input, ota_recovery_magic, sizeof(ota_recovery_magic)) != 0 ||
        input[4] != OTA_RECOVERY_FORMAT_VERSION ||
        !ota_recovery_state_valid(input[5]) ||
        !ota_recovery_slot_valid(input[6]) ||
        !ota_recovery_slot_valid(input[7]) ||
        input[6] == input[7] ||
        ota_recovery_crc32(input, 28U) != ota_recovery_read_u32_le(&input[28]))
    {
        return 0U;
    }

    record->state = input[5];
    record->source_slot = input[6];
    record->target_slot = input[7];
    record->sequence = ota_recovery_read_u32_le(&input[8]);
    record->source_version = ota_recovery_read_u32_le(&input[12]);
    record->target_version = ota_recovery_read_u32_le(&input[16]);
    record->candidate_crc32 = ota_recovery_read_u32_le(&input[20]);
    record->backup_crc32 = ota_recovery_read_u32_le(&input[24]);
    return 1U;
}

void ota_recovery_scan_journal(const uint8_t *journal, uint32_t length,
                               ota_recovery_scan_t *result)
{
    uint32_t count = length / OTA_RECOVERY_RECORD_SIZE;
    uint32_t index;

    ota_recovery_scan_begin(result, (uint16_t)count);

    for (index = 0U; index < count; ++index)
    {
        ota_recovery_scan_entry(
            result, journal + index * OTA_RECOVERY_RECORD_SIZE,
            (uint16_t)index);
    }
}

void ota_recovery_scan_begin(ota_recovery_scan_t *result,
                             uint16_t record_count)
{
    memset(result, 0, sizeof(*result));
    result->next_index = record_count;
    result->full = 1U;
}

void ota_recovery_scan_entry(ota_recovery_scan_t *result,
                             const uint8_t entry[OTA_RECOVERY_RECORD_SIZE],
                             uint16_t index)
{
    ota_recovery_record_t decoded;

    if (ota_recovery_is_erased(entry, OTA_RECOVERY_RECORD_SIZE))
    {
        if (result->full)
        {
            result->next_index = index;
            result->full = 0U;
        }
        return;
    }

    if (!ota_recovery_decode(entry, &decoded))
    {
        result->corrupt_records++;
        return;
    }

    result->valid_records++;
    if (decoded.state == OTA_RECOVERY_STATE_INSTALL_INTENT ||
        decoded.state == OTA_RECOVERY_STATE_MARKERS_SWITCHED)
    {
        result->trial_attempts = 0U;
    }
    else if (decoded.state == OTA_RECOVERY_STATE_TRIAL_ATTEMPT &&
             result->trial_attempts != 0xFFU)
    {
        result->trial_attempts++;
    }
    if (!result->found ||
        ota_recovery_sequence_after(decoded.sequence,
                                    result->latest.sequence))
    {
        result->latest = decoded;
        result->found = 1U;
    }
}

void ota_recovery_make_masked_backup(const uint8_t *source, uint8_t *backup,
                                     uint32_t length)
{
    memcpy(backup, source, length);
    if (length > OTA_RECOVERY_BOOT_MARKER_OFFSET)
    {
        backup[OTA_RECOVERY_BOOT_MARKER_OFFSET] = 0xFFU;
    }
}

uint8_t ota_recovery_select_boot(uint8_t slot_a_valid, uint8_t slot_b_valid)
{
    if (slot_a_valid)
    {
        return OTA_RECOVERY_SLOT_A;
    }
    if (slot_b_valid)
    {
        return OTA_RECOVERY_SLOT_B;
    }
    return OTA_RECOVERY_NO_BOOT_SLOT;
}

ota_recovery_action_t ota_recovery_decide(
    const ota_recovery_observation_t *observation)
{
    uint8_t source_valid;
    uint8_t target_valid;

    if (!observation->has_record)
    {
        return ota_recovery_marker_valid(observation,
                                         observation->running_slot)
                   ? OTA_RECOVERY_ACTION_NORMAL
                   : OTA_RECOVERY_ACTION_SWIRE_RESCUE;
    }

    if (!ota_recovery_slot_valid(observation->latest.source_slot) ||
        !ota_recovery_slot_valid(observation->latest.target_slot) ||
        observation->latest.source_slot == observation->latest.target_slot ||
        !ota_recovery_slot_valid(observation->running_slot))
    {
        return OTA_RECOVERY_ACTION_SWIRE_RESCUE;
    }

    source_valid = ota_recovery_marker_valid(
        observation, observation->latest.source_slot);
    target_valid = ota_recovery_marker_valid(
        observation, observation->latest.target_slot);

    if (!ota_recovery_marker_valid(observation,
                                   observation->running_slot))
    {
        return OTA_RECOVERY_ACTION_SWIRE_RESCUE;
    }

    switch (observation->latest.state)
    {
    case OTA_RECOVERY_STATE_BASELINE:
        return source_valid ? OTA_RECOVERY_ACTION_NORMAL
                            : OTA_RECOVERY_ACTION_SWIRE_RESCUE;

    case OTA_RECOVERY_STATE_INSTALL_INTENT:
        if (source_valid && !target_valid)
        {
            return OTA_RECOVERY_ACTION_RESUME_INSTALL;
        }
        if (target_valid && observation->running_slot ==
                                observation->latest.target_slot)
        {
            return OTA_RECOVERY_ACTION_START_TRIAL;
        }
        if (source_valid && target_valid && observation->running_slot ==
                                               observation->latest.source_slot)
        {
            return OTA_RECOVERY_ACTION_RESUME_SWITCH;
        }
        return OTA_RECOVERY_ACTION_SWIRE_RESCUE;

    case OTA_RECOVERY_STATE_MARKERS_SWITCHED:
        if (target_valid && observation->running_slot ==
                                observation->latest.target_slot)
        {
            return OTA_RECOVERY_ACTION_START_TRIAL;
        }
        if (source_valid && target_valid && observation->running_slot ==
                                               observation->latest.source_slot)
        {
            return OTA_RECOVERY_ACTION_RESUME_SWITCH;
        }
        return OTA_RECOVERY_ACTION_SWIRE_RESCUE;

    case OTA_RECOVERY_STATE_TRIAL_ATTEMPT:
        if (!target_valid || observation->running_slot !=
                                 observation->latest.target_slot)
        {
            return OTA_RECOVERY_ACTION_SWIRE_RESCUE;
        }
        if (observation->max_trial_attempts != 0U &&
            observation->trial_attempts >= observation->max_trial_attempts)
        {
            return observation->backup_valid
                       ? OTA_RECOVERY_ACTION_ROLLBACK_SOURCE
                       : OTA_RECOVERY_ACTION_SWIRE_RESCUE;
        }
        return OTA_RECOVERY_ACTION_START_TRIAL;

    case OTA_RECOVERY_STATE_CONFIRMED:
        return target_valid && observation->running_slot ==
                                   observation->latest.target_slot
                   ? OTA_RECOVERY_ACTION_NORMAL
                   : OTA_RECOVERY_ACTION_SWIRE_RESCUE;

    case OTA_RECOVERY_STATE_ROLLBACK_INTENT:
        if (source_valid && observation->running_slot ==
                                observation->latest.source_slot)
        {
            return OTA_RECOVERY_ACTION_CLEANUP_FAILED_TARGET;
        }
        return observation->backup_valid
                   ? OTA_RECOVERY_ACTION_ROLLBACK_SOURCE
                   : OTA_RECOVERY_ACTION_SWIRE_RESCUE;

    case OTA_RECOVERY_STATE_ROLLBACK_DONE:
        if (!source_valid || observation->running_slot !=
                                 observation->latest.source_slot)
        {
            return OTA_RECOVERY_ACTION_SWIRE_RESCUE;
        }
        return target_valid ? OTA_RECOVERY_ACTION_CLEANUP_FAILED_TARGET
                            : OTA_RECOVERY_ACTION_NORMAL;

    default:
        return OTA_RECOVERY_ACTION_SWIRE_RESCUE;
    }
}
