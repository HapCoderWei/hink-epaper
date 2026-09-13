#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ota_recovery.h"

static ota_recovery_record_t make_record(uint8_t state, uint32_t sequence)
{
    ota_recovery_record_t record;
    memset(&record, 0, sizeof(record));
    record.state = state;
    record.source_slot = OTA_RECOVERY_SLOT_A;
    record.target_slot = OTA_RECOVERY_SLOT_B;
    record.sequence = sequence;
    record.source_version = 4U;
    record.target_version = 5U;
    record.candidate_crc32 = 0x12345678UL;
    record.backup_crc32 = 0x89ABCDEFUL;
    return record;
}

static void test_record_round_trip_and_corruption(void)
{
    uint8_t encoded[OTA_RECOVERY_RECORD_SIZE];
    uint8_t corrupted[OTA_RECOVERY_RECORD_SIZE];
    ota_recovery_record_t input;
    ota_recovery_record_t output;
    uint32_t byte;

    for (input.state = OTA_RECOVERY_STATE_BASELINE;
         input.state <= OTA_RECOVERY_STATE_ROLLBACK_DONE;
         ++input.state)
    {
        input = make_record(input.state, 123U + input.state);
        ota_recovery_encode(&input, encoded);
        assert(ota_recovery_decode(encoded, &output));
        assert(input.state == output.state);
        assert(input.source_slot == output.source_slot);
        assert(input.target_slot == output.target_slot);
        assert(input.sequence == output.sequence);
        assert(input.source_version == output.source_version);
        assert(input.target_version == output.target_version);
        assert(input.candidate_crc32 == output.candidate_crc32);
        assert(input.backup_crc32 == output.backup_crc32);
    }

    input = make_record(OTA_RECOVERY_STATE_INSTALL_INTENT, 9U);
    ota_recovery_encode(&input, encoded);
    for (byte = 0U; byte < OTA_RECOVERY_RECORD_SIZE; ++byte)
    {
        memcpy(corrupted, encoded, sizeof(corrupted));
        corrupted[byte] ^= 1U;
        assert(!ota_recovery_decode(corrupted, &output));
    }
}

static void test_torn_journal_records(void)
{
    uint8_t journal[OTA_RECOVERY_JOURNAL_SIZE];
    uint8_t first[OTA_RECOVERY_RECORD_SIZE];
    uint8_t second[OTA_RECOVERY_RECORD_SIZE];
    ota_recovery_record_t record;
    ota_recovery_scan_t scan;
    uint32_t written;

    record = make_record(OTA_RECOVERY_STATE_INSTALL_INTENT, 10U);
    ota_recovery_encode(&record, first);
    record = make_record(OTA_RECOVERY_STATE_MARKERS_SWITCHED, 11U);
    ota_recovery_encode(&record, second);

    for (written = 0U; written <= OTA_RECOVERY_RECORD_SIZE; ++written)
    {
        memset(journal, 0xFF, sizeof(journal));
        memcpy(journal, first, sizeof(first));
        memcpy(&journal[OTA_RECOVERY_RECORD_SIZE], second, written);
        ota_recovery_scan_journal(journal, sizeof(journal), &scan);
        assert(scan.found);
        if (written == OTA_RECOVERY_RECORD_SIZE)
        {
            assert(scan.latest.sequence == 11U);
            assert(scan.latest.state == OTA_RECOVERY_STATE_MARKERS_SWITCHED);
            assert(scan.valid_records == 2U);
            assert(scan.corrupt_records == 0U);
            assert(scan.next_index == 2U);
        }
        else
        {
            assert(scan.latest.sequence == 10U);
            assert(scan.valid_records == 1U);
            assert(scan.corrupt_records == (written == 0U ? 0U : 1U));
            assert(scan.next_index == (written == 0U ? 1U : 2U));
        }
    }
}

static void test_scan_uses_newest_sequence(void)
{
    uint8_t journal[OTA_RECOVERY_JOURNAL_SIZE];
    uint8_t encoded[OTA_RECOVERY_RECORD_SIZE];
    ota_recovery_record_t record;
    ota_recovery_scan_t scan;

    memset(journal, 0xFF, sizeof(journal));
    record = make_record(OTA_RECOVERY_STATE_CONFIRMED, 0xFFFFFFFFUL);
    ota_recovery_encode(&record, encoded);
    memcpy(&journal[0], encoded, sizeof(encoded));
    record = make_record(OTA_RECOVERY_STATE_BASELINE, 0U);
    ota_recovery_encode(&record, encoded);
    memcpy(&journal[2U * OTA_RECOVERY_RECORD_SIZE], encoded, sizeof(encoded));
    journal[OTA_RECOVERY_RECORD_SIZE] = 0U;

    ota_recovery_scan_journal(journal, sizeof(journal), &scan);
    assert(scan.found && scan.latest.sequence == 0U);
    assert(scan.valid_records == 2U);
    assert(scan.corrupt_records == 1U);
    assert(scan.next_index == 3U);

    memset(journal, 0xFF, sizeof(journal));
    record = make_record(OTA_RECOVERY_STATE_MARKERS_SWITCHED, 1U);
    ota_recovery_encode(&record, encoded);
    memcpy(&journal[0], encoded, sizeof(encoded));
    record = make_record(OTA_RECOVERY_STATE_TRIAL_ATTEMPT, 2U);
    ota_recovery_encode(&record, encoded);
    memcpy(&journal[OTA_RECOVERY_RECORD_SIZE], encoded, sizeof(encoded));
    record.sequence = 3U;
    ota_recovery_encode(&record, encoded);
    memcpy(&journal[2U * OTA_RECOVERY_RECORD_SIZE], encoded, sizeof(encoded));
    ota_recovery_scan_journal(journal, sizeof(journal), &scan);
    assert(scan.trial_attempts == 2U);
}

static void test_backup_masking(void)
{
    uint8_t source[4096];
    uint8_t backup[4096];
    uint32_t i;

    for (i = 0U; i < sizeof(source); ++i)
        source[i] = (uint8_t)(i * 17U + 3U);
    memcpy(&source[OTA_RECOVERY_BOOT_MARKER_OFFSET], "KNLT", 4U);
    ota_recovery_make_masked_backup(source, backup, sizeof(source));
    for (i = 0U; i < sizeof(source); ++i)
    {
        assert(backup[i] ==
               (i == OTA_RECOVERY_BOOT_MARKER_OFFSET ? 0xFFU : source[i]));
    }
    assert(ota_recovery_crc32(source, sizeof(source)) !=
           ota_recovery_crc32(backup, sizeof(backup)));
    assert(ota_recovery_select_boot(1U, 1U) == OTA_RECOVERY_SLOT_A);
    assert(ota_recovery_select_boot(0U, 1U) == OTA_RECOVERY_SLOT_B);
    assert(ota_recovery_select_boot(0U, 0U) == 0xFFU);
}

static ota_recovery_action_t decide(uint8_t state, uint8_t running,
                                    uint8_t a_valid, uint8_t b_valid,
                                    uint8_t backup_valid, uint8_t attempts,
                                    uint8_t max_attempts)
{
    ota_recovery_observation_t observation;
    memset(&observation, 0, sizeof(observation));
    observation.has_record = state != 0U;
    observation.latest = make_record(state, 20U);
    observation.running_slot = running;
    observation.slot_a_valid = a_valid;
    observation.slot_b_valid = b_valid;
    observation.backup_valid = backup_valid;
    observation.trial_attempts = attempts;
    observation.max_trial_attempts = max_attempts;
    return ota_recovery_decide(&observation);
}

static void test_recovery_decisions(void)
{
    assert(decide(0U, OTA_RECOVERY_SLOT_A, 1U, 0U, 0U, 0U, 0U) ==
           OTA_RECOVERY_ACTION_NORMAL);
    assert(decide(OTA_RECOVERY_STATE_INSTALL_INTENT, OTA_RECOVERY_SLOT_A,
                  1U, 0U, 1U, 0U, 3U) ==
           OTA_RECOVERY_ACTION_RESUME_INSTALL);
    assert(decide(OTA_RECOVERY_STATE_INSTALL_INTENT, OTA_RECOVERY_SLOT_A,
                  1U, 1U, 1U, 0U, 3U) ==
           OTA_RECOVERY_ACTION_RESUME_SWITCH);
    assert(decide(OTA_RECOVERY_STATE_INSTALL_INTENT, OTA_RECOVERY_SLOT_B,
                  1U, 1U, 1U, 0U, 3U) ==
           OTA_RECOVERY_ACTION_START_TRIAL);
    assert(decide(OTA_RECOVERY_STATE_MARKERS_SWITCHED, OTA_RECOVERY_SLOT_B,
                  0U, 1U, 1U, 0U, 3U) ==
           OTA_RECOVERY_ACTION_START_TRIAL);
    assert(decide(OTA_RECOVERY_STATE_TRIAL_ATTEMPT, OTA_RECOVERY_SLOT_B,
                  0U, 1U, 1U, 2U, 3U) ==
           OTA_RECOVERY_ACTION_START_TRIAL);
    assert(decide(OTA_RECOVERY_STATE_TRIAL_ATTEMPT, OTA_RECOVERY_SLOT_B,
                  0U, 1U, 1U, 3U, 3U) ==
           OTA_RECOVERY_ACTION_ROLLBACK_SOURCE);
    assert(decide(OTA_RECOVERY_STATE_TRIAL_ATTEMPT, OTA_RECOVERY_SLOT_B,
                  0U, 1U, 0U, 3U, 3U) ==
           OTA_RECOVERY_ACTION_SWIRE_RESCUE);
    assert(decide(OTA_RECOVERY_STATE_CONFIRMED, OTA_RECOVERY_SLOT_B,
                  0U, 1U, 1U, 1U, 3U) ==
           OTA_RECOVERY_ACTION_NORMAL);
    assert(decide(OTA_RECOVERY_STATE_ROLLBACK_INTENT, OTA_RECOVERY_SLOT_B,
                  0U, 1U, 1U, 3U, 3U) ==
           OTA_RECOVERY_ACTION_ROLLBACK_SOURCE);
    assert(decide(OTA_RECOVERY_STATE_ROLLBACK_INTENT, OTA_RECOVERY_SLOT_A,
                  1U, 1U, 0U, 3U, 3U) ==
           OTA_RECOVERY_ACTION_CLEANUP_FAILED_TARGET);
    assert(decide(OTA_RECOVERY_STATE_ROLLBACK_DONE, OTA_RECOVERY_SLOT_A,
                  1U, 1U, 1U, 3U, 3U) ==
           OTA_RECOVERY_ACTION_CLEANUP_FAILED_TARGET);
    assert(decide(OTA_RECOVERY_STATE_ROLLBACK_DONE, OTA_RECOVERY_SLOT_A,
                  1U, 0U, 1U, 3U, 3U) ==
           OTA_RECOVERY_ACTION_NORMAL);

    /* No bootable marker, inconsistent running slot, or missing backup at the
     * rollback boundary must never be guessed around. */
    assert(decide(OTA_RECOVERY_STATE_INSTALL_INTENT, OTA_RECOVERY_SLOT_A,
                  0U, 0U, 1U, 0U, 3U) ==
           OTA_RECOVERY_ACTION_SWIRE_RESCUE);
    assert(decide(OTA_RECOVERY_STATE_CONFIRMED, OTA_RECOVERY_SLOT_A,
                  1U, 1U, 1U, 0U, 3U) ==
           OTA_RECOVERY_ACTION_SWIRE_RESCUE);
}

int main(void)
{
    test_record_round_trip_and_corruption();
    test_torn_journal_records();
    test_scan_uses_newest_sequence();
    test_backup_masking();
    test_recovery_decisions();
    puts("OTA recovery host tests passed");
    return 0;
}
