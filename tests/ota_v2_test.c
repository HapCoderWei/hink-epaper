#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RAM
#define _attribute_ram_code_
#define OTA_CMD_OUT_DP_H 27
#define OTA_V2_HOST_TEST 1
#define HINK_FW_VERSION 2U

#include "ota_recovery.h"

typedef struct
{
    uint16_t l2capLen;
    uint8_t value[64];
} rf_packet_att_write_t;

uint32_t ota_program_offset;

static uint8_t fake_flash[0x41000];
static uint8_t last_notification[32];
static int last_notification_len;
static uint32_t expected_target;
static uint32_t active_start;
static int active_bank_touched;
static int erase_count;
static int corrupt_next_write;
static uint32_t corrupt_write_address;
static uint32_t fake_clock;
static int reboot_count;
static int irq_disabled;
static int irq_restored;
static int watchdog_started;
static int watchdog_stopped;
static int watchdog_cleared;
static jmp_buf power_cut_jump;
static int power_cut_enabled;
static int cut_write_call;
static uint32_t cut_write_prefix;
static int write_call_count;
static int cut_erase_enabled;
static uint32_t cut_erase_address;
static uint32_t cut_erase_prefix;
static int allow_source_restore;

static uint32_t clock_time(void) { return fake_clock; }
static int clock_time_exceed(uint32_t reference, uint32_t span)
{
    return (uint32_t)(fake_clock - reference) > span;
}
static uint8_t irq_disable(void)
{
    irq_disabled++;
    return 0x5aU;
}
static void irq_restore(uint8_t state)
{
    assert(state == 0x5aU);
    irq_restored++;
}
void ota_v2_test_reboot(void) { reboot_count++; }
void ota_v2_test_watchdog_start(void) { watchdog_started++; }
void ota_v2_test_watchdog_stop(void) { watchdog_stopped++; }
void ota_v2_test_watchdog_clear(void) { watchdog_cleared++; }

static int overlaps(uint32_t address, uint32_t len, uint32_t start, uint32_t size)
{
    return address < start + size && start < address + len;
}

static void flash_erase_sector(unsigned long address)
{
    int target_erase = address >= expected_target &&
                       address + 0x1000UL <= expected_target + 0x1F000UL;
    int backup_erase = address == OTA_RECOVERY_BACKUP_ADDRESS;
    int source_restore_erase = allow_source_restore &&
                               address == active_start;
    assert((address & 0xFFFUL) == 0U);
    assert(target_erase || backup_erase || source_restore_erase);
    assert(address + 0x1000UL <= sizeof(fake_flash));
    if (overlaps((uint32_t)address, 0x1000U, active_start, 0x1F000U))
        active_bank_touched = 1;
    if (power_cut_enabled && cut_erase_enabled &&
        address == cut_erase_address)
    {
        assert(cut_erase_prefix <= 0x1000U);
        memset(&fake_flash[address], 0xff, cut_erase_prefix);
        power_cut_enabled = 0;
        longjmp(power_cut_jump, 1);
    }
    memset(&fake_flash[address], 0xff, 0x1000);
    erase_count++;
}

static void flash_write_page(unsigned long address, unsigned long len, unsigned char *data)
{
    unsigned long i;
    int target_write = address >= expected_target &&
                       address + len <= expected_target + 0x1F000UL;
    int current_marker_write = address == active_start + 8U && len == 4U;
    int recovery_write =
        (address >= OTA_RECOVERY_JOURNAL_ADDRESS &&
         address + len <= OTA_RECOVERY_JOURNAL_ADDRESS +
                              OTA_RECOVERY_JOURNAL_SIZE) ||
        (address >= OTA_RECOVERY_BACKUP_ADDRESS &&
         address + len <= OTA_RECOVERY_BACKUP_ADDRESS +
                              OTA_RECOVERY_BACKUP_SIZE);
    int source_restore_write =
        allow_source_restore && address >= active_start &&
        address + len <= active_start + OTA_RECOVERY_BACKUP_SIZE;
    assert(target_write || current_marker_write || recovery_write ||
           source_restore_write);
    assert(address + len <= sizeof(fake_flash));
    if (overlaps((uint32_t)address, (uint32_t)len, active_start, 0x1F000U))
        active_bank_touched = 1;
    write_call_count++;
    if (power_cut_enabled && !cut_erase_enabled &&
        write_call_count == cut_write_call)
    {
        assert(cut_write_prefix <= len);
        for (i = 0; i < cut_write_prefix; ++i)
            fake_flash[address + i] &= data[i];
        power_cut_enabled = 0;
        longjmp(power_cut_jump, 1);
    }
    for (i = 0; i < len; ++i)
        fake_flash[address + i] &= data[i];
    if (corrupt_next_write && len &&
        (!corrupt_write_address || address == corrupt_write_address))
    {
        fake_flash[address] ^= 1;
        corrupt_next_write = 0;
    }
}

static void flash_read_page(unsigned long address, unsigned long len, unsigned char *data)
{
    assert(address + len <= sizeof(fake_flash));
    memcpy(data, &fake_flash[address], len);
}

static int bls_att_pushNotifyData(uint16_t handle, uint8_t *data, int len)
{
    assert(handle == OTA_CMD_OUT_DP_H);
    assert(len <= (int)sizeof(last_notification));
    memcpy(last_notification, data, len);
    last_notification_len = len;
    return 0;
}

#include "production_ota.inc"

#define TEST_PAYLOAD_SIZE 320U
#define TEST_IMAGE_SIZE (TEST_PAYLOAD_SIZE + 24U)

static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void put_u32_be(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint32_t crc32(const uint8_t *data, uint32_t len)
{
    return ota_v2_crc32_update(0xFFFFFFFFUL, data, len) ^ 0xFFFFFFFFUL;
}

static void refresh_manifest_crc(uint8_t *image)
{
    put_u32(&image[TEST_PAYLOAD_SIZE + 20U],
            crc32(&image[TEST_PAYLOAD_SIZE], 20U));
}

static void refresh_manifest(uint8_t *image, uint32_t firmware_version)
{
    uint8_t *manifest = &image[TEST_PAYLOAD_SIZE];
    memcpy(manifest, "HOTA", 4U);
    manifest[4] = 1U;
    manifest[5] = 0U;
    put_u16(&manifest[6], 0x213AU);
    put_u32(&manifest[8], TEST_PAYLOAD_SIZE);
    put_u32(&manifest[12], crc32(image, TEST_PAYLOAD_SIZE));
    put_u32(&manifest[16], firmware_version);
    refresh_manifest_crc(image);
}

static void refresh_telink_crc(uint8_t *image)
{
    put_u32_be(&image[TEST_PAYLOAD_SIZE - 4U],
               crc32(image, TEST_PAYLOAD_SIZE - 4U));
}

static void make_valid_image(uint8_t *image)
{
    uint32_t i;
    for (i = 0; i < TEST_PAYLOAD_SIZE - 4U; ++i)
        image[i] = (uint8_t)(i * 29U + 7U);
    memcpy(&image[8], "KNLT", 4U);
    put_u32(&image[0x18], TEST_PAYLOAD_SIZE - 4U);
    refresh_telink_crc(image);
    refresh_manifest(image, 1U);
}

static void invoke(const uint8_t *payload, uint16_t len)
{
    rf_packet_att_write_t request;
    memset(&request, 0, sizeof(request));
    request.l2capLen = len + 3U;
    memcpy(request.value, payload, len);
    last_notification_len = 0;
    custom_otaWrite(&request);
    assert(last_notification_len == 32);
    assert(last_notification[0] == 0xA2);
    assert(last_notification[1] == 1);
    assert(last_notification[2] == payload[0]);
    assert(last_notification[20] == 3);
    assert(last_notification[23] == 2);
    assert(last_notification[24] == 2 && last_notification[25] == 0 &&
           last_notification[26] == 0 && last_notification[27] == 0);
}

static void reset_environment(uint32_t target)
{
    memset(fake_flash, 0x5a, sizeof(fake_flash));
    memset(&fake_flash[OTA_RECOVERY_JOURNAL_ADDRESS], 0xFF,
           OTA_RECOVERY_JOURNAL_SIZE);
    memset(&fake_flash[OTA_RECOVERY_BACKUP_ADDRESS], 0xFF,
           OTA_RECOVERY_BACKUP_SIZE);
    memset(last_notification, 0, sizeof(last_notification));
    ota_v2_reset_session();
    ota_program_offset = target;
    expected_target = target;
    active_start = target == 0U ? 0x20000U : 0U;
    memcpy(&fake_flash[active_start + 8U], "KNLT", 4U);
    active_bank_touched = 0;
    erase_count = 0;
    corrupt_next_write = 0;
    corrupt_write_address = 0U;
    fake_clock = 100U;
    reboot_count = 0;
    irq_disabled = 0;
    irq_restored = 0;
    watchdog_started = 0;
    watchdog_stopped = 0;
    watchdog_cleared = 0;
    power_cut_enabled = 0;
    cut_write_call = 0;
    cut_write_prefix = 0U;
    write_call_count = 0;
    cut_erase_enabled = 0;
    cut_erase_address = 0U;
    cut_erase_prefix = 0U;
    allow_source_restore = 0;
    ota_v2_recovery_init();
}

static void begin_image(uint32_t size, uint32_t expected_crc)
{
    uint8_t command[12] = {0x11, 1, 0x3a, 0x21};
    put_u32(&command[4], size);
    put_u32(&command[8], expected_crc);
    invoke(command, sizeof(command));
}

static void erase_all(void)
{
    uint8_t erase[] = {0x12};
    while (last_notification[5] < last_notification[6])
        invoke(erase, sizeof(erase));
    assert(last_notification[4] == 2);
}

static void send_data(uint32_t offset, const uint8_t *data, uint8_t len)
{
    uint8_t command[5 + 24] = {0x13};
    put_u32(&command[1], offset);
    memcpy(&command[5], data, len);
    invoke(command, (uint16_t)(5U + len));
}

static void upload_all(const uint8_t *image, uint32_t size)
{
    uint32_t offset = 0U;
    while (offset < size)
    {
        uint32_t page_remaining = 0x100U - (offset & 0xFFU);
        uint32_t len = size - offset;
        if (len > 24U)
            len = 24U;
        if (len > page_remaining)
            len = page_remaining;
        send_data(offset, image + offset, (uint8_t)len);
        assert(last_notification[3] == 0);
        offset += len;
    }
}

static void start_transfer(const uint8_t *image, uint32_t expected_crc)
{
    begin_image(TEST_IMAGE_SIZE, expected_crc);
    assert(last_notification[3] == 0);
    erase_all();
    upload_all(image, TEST_IMAGE_SIZE);
}

static void finish_with_status(uint8_t expected_status)
{
    uint8_t finish[] = {0x14};
    invoke(finish, sizeof(finish));
    assert(last_notification[3] == expected_status);
    assert(last_notification[4] == (expected_status == 0U ? 3U : 4U));
}

static void confirm_command(uint8_t command, uint32_t confirmation,
                            uint32_t image_crc)
{
    uint8_t packet[9];
    packet[0] = command;
    put_u32(&packet[1], confirmation);
    put_u32(&packet[5], image_crc);
    invoke(packet, sizeof(packet));
}

static uint32_t stage_verified_image(const uint8_t *image)
{
    uint32_t image_crc = crc32(image, TEST_IMAGE_SIZE);
    start_transfer(image, image_crc);
    finish_with_status(OTA_V2_STATUS_OK);
    return image_crc;
}

static void arm_and_queue_install(uint32_t image_crc)
{
    confirm_command(OTA_V2_CMD_ARM_INSTALL, OTA_V2_ARM_CONFIRM, image_crc);
    assert(last_notification[3] == OTA_V2_STATUS_OK);
    assert(last_notification[4] == OTA_V2_PHASE_ARMED);
    confirm_command(OTA_V2_CMD_INSTALL, OTA_V2_INSTALL_CONFIRM, image_crc);
    assert(last_notification[3] == OTA_V2_STATUS_OK);
    assert(last_notification[4] == OTA_V2_PHASE_INSTALL_PENDING);
}

static void test_install_guards_and_success(const uint8_t *image)
{
    uint32_t image_crc;
    uint32_t i;
    ota_recovery_record_t recovery_record;
    uint8_t abort[] = {OTA_V2_CMD_ABORT};

    reset_environment(OTA_V2_SLOT_B_START);
    confirm_command(OTA_V2_CMD_ARM_INSTALL, OTA_V2_ARM_CONFIRM,
                    crc32(image, TEST_IMAGE_SIZE));
    assert(last_notification[3] == OTA_V2_STATUS_BAD_STATE);
    confirm_command(OTA_V2_CMD_INSTALL, OTA_V2_INSTALL_CONFIRM,
                    crc32(image, TEST_IMAGE_SIZE));
    assert(last_notification[3] == OTA_V2_STATUS_BAD_STATE);

    reset_environment(OTA_V2_SLOT_B_START);
    image_crc = stage_verified_image(image);
    confirm_command(OTA_V2_CMD_ARM_INSTALL, 0U, image_crc);
    assert(last_notification[3] == OTA_V2_STATUS_CONFIRM_REQUIRED);
    assert(last_notification[4] == OTA_V2_PHASE_VERIFIED);
    confirm_command(OTA_V2_CMD_ARM_INSTALL, OTA_V2_ARM_CONFIRM,
                    image_crc ^ 1U);
    assert(last_notification[3] == OTA_V2_STATUS_CONFIRM_REQUIRED);
    confirm_command(OTA_V2_CMD_ARM_INSTALL, OTA_V2_ARM_CONFIRM, image_crc);
    assert(last_notification[4] == OTA_V2_PHASE_ARMED);
    confirm_command(OTA_V2_CMD_INSTALL, 0U, image_crc);
    assert(last_notification[3] == OTA_V2_STATUS_CONFIRM_REQUIRED);
    assert(last_notification[4] == OTA_V2_PHASE_ARMED);
    confirm_command(OTA_V2_CMD_INSTALL, OTA_V2_INSTALL_CONFIRM,
                    image_crc ^ 1U);
    assert(last_notification[3] == OTA_V2_STATUS_CONFIRM_REQUIRED);
    confirm_command(OTA_V2_CMD_INSTALL, OTA_V2_INSTALL_CONFIRM, image_crc);
    assert(last_notification[4] == OTA_V2_PHASE_INSTALL_PENDING);
    invoke(abort, sizeof(abort));
    assert(last_notification[3] == OTA_V2_STATUS_BAD_STATE);

    ota_v2_process();
    assert(reboot_count == 0);
    assert(fake_flash[OTA_V2_SLOT_B_START + 8U] == 0xFFU);
    assert(memcmp(&fake_flash[OTA_V2_SLOT_A_START + 8U], "KNLT", 4U) == 0);
    fake_clock += OTA_V2_INSTALL_DELAY_US + 1U;
    ota_v2_process();
    assert(reboot_count == 1);
    assert(memcmp(&fake_flash[OTA_V2_SLOT_B_START + 8U], "KNLT", 4U) == 0);
    assert(fake_flash[OTA_V2_SLOT_A_START + 8U] == 0x00U);
    assert(memcmp(&fake_flash[OTA_V2_SLOT_A_START + 9U], "NLT", 3U) == 0);
    assert(irq_disabled == 1 && irq_restored == 0);
    for (i = 0U; i < OTA_RECOVERY_BACKUP_SIZE; ++i)
    {
        uint8_t expected = i == OTA_RECOVERY_BOOT_MARKER_OFFSET
                               ? 0xFFU
                               : fake_flash[OTA_V2_SLOT_A_START + i];
        assert(fake_flash[OTA_RECOVERY_BACKUP_ADDRESS + i] == expected);
    }
    assert(ota_recovery_decode(
        &fake_flash[OTA_RECOVERY_JOURNAL_ADDRESS], &recovery_record));
    assert(recovery_record.state == OTA_RECOVERY_STATE_INSTALL_INTENT);
    assert(recovery_record.source_slot == OTA_RECOVERY_SLOT_A);
    assert(recovery_record.target_slot == OTA_RECOVERY_SLOT_B);
    assert(recovery_record.target_version == 1U);
    assert(ota_recovery_decode(
        &fake_flash[OTA_RECOVERY_JOURNAL_ADDRESS + OTA_RECOVERY_RECORD_SIZE],
        &recovery_record));
    assert(recovery_record.state == OTA_RECOVERY_STATE_MARKERS_SWITCHED);
}

static void test_recovery_write_guards(const uint8_t *image)
{
    uint32_t image_crc;

    reset_environment(OTA_V2_SLOT_B_START);
    image_crc = stage_verified_image(image);
    arm_and_queue_install(image_crc);
    corrupt_next_write = 1;
    corrupt_write_address = OTA_RECOVERY_BACKUP_ADDRESS;
    fake_clock += OTA_V2_INSTALL_DELAY_US + 1U;
    ota_v2_process();
    assert(ota_v2_session.phase == OTA_V2_PHASE_ERROR);
    assert(ota_v2_session.last_status == OTA_V2_STATUS_RECOVERY_BACKUP);
    assert(memcmp(&fake_flash[OTA_V2_SLOT_A_START + 8U], "KNLT", 4U) == 0);
    assert(fake_flash[OTA_V2_SLOT_B_START + 8U] == 0xFFU);
    assert(reboot_count == 0);

    reset_environment(OTA_V2_SLOT_B_START);
    image_crc = stage_verified_image(image);
    arm_and_queue_install(image_crc);
    corrupt_next_write = 1;
    corrupt_write_address = OTA_RECOVERY_JOURNAL_ADDRESS;
    fake_clock += OTA_V2_INSTALL_DELAY_US + 1U;
    ota_v2_process();
    assert(ota_v2_session.phase == OTA_V2_PHASE_ERROR);
    assert(ota_v2_session.last_status == OTA_V2_STATUS_RECOVERY_JOURNAL);
    assert(memcmp(&fake_flash[OTA_V2_SLOT_A_START + 8U], "KNLT", 4U) == 0);
    assert(fake_flash[OTA_V2_SLOT_B_START + 8U] == 0xFFU);
    assert(reboot_count == 0);
}

static ota_recovery_action_t cold_boot_action_after_cut(void)
{
    uint8_t a_valid =
        memcmp(&fake_flash[OTA_V2_SLOT_A_START + 8U], "KNLT", 4U) == 0;
    uint8_t b_valid =
        memcmp(&fake_flash[OTA_V2_SLOT_B_START + 8U], "KNLT", 4U) == 0;
    uint8_t running = ota_recovery_select_boot(a_valid, b_valid);

    assert(running != 0xFFU);
    ota_program_offset = running == OTA_RECOVERY_SLOT_A
                             ? OTA_V2_SLOT_B_START
                             : OTA_V2_SLOT_A_START;
    ota_v2_reset_session();
    ota_v2_recovery_init();
    return (ota_recovery_action_t)ota_v2_recovery_action;
}

static void prepare_power_cut_install(const uint8_t *image, uint32_t target)
{
    uint32_t image_crc;
    reset_environment(target);
    image_crc = stage_verified_image(image);
    arm_and_queue_install(image_crc);
    fake_clock += OTA_V2_INSTALL_DELAY_US + 1U;
    write_call_count = 0;
}

static void test_every_install_write_cut(const uint8_t *image)
{
    int call;
    int direction;
    uint32_t prefix;
    uint32_t length;
    uint32_t target;
    ota_recovery_action_t expected;

    for (direction = 0; direction < 2; ++direction)
    {
        target = direction == 0 ? OTA_V2_SLOT_B_START : OTA_V2_SLOT_A_START;
        for (call = 1; call <= 68; ++call)
        {
            length = call <= 64 ? 64U :
                     (call == 65 || call == 68 ? 32U : 4U);
            for (prefix = 0U; prefix <= length; ++prefix)
            {
                prepare_power_cut_install(image, target);
                cut_write_call = call;
                cut_write_prefix = prefix;
                power_cut_enabled = 1;
                if (setjmp(power_cut_jump) == 0)
                {
                    ota_v2_process();
                    assert(!"expected injected power cut");
                }

                if (call <= 64 || (call == 65 && prefix < 32U))
                    expected = OTA_RECOVERY_ACTION_NORMAL;
                else if ((call == 65 && prefix == 32U) ||
                         (call == 66 && prefix == 0U))
                    expected = OTA_RECOVERY_ACTION_RESUME_INSTALL;
                else if (((call == 66 && prefix >= 1U) ||
                          (call == 67 && prefix == 0U)) &&
                         target == OTA_V2_SLOT_B_START)
                    expected = OTA_RECOVERY_ACTION_RESUME_SWITCH;
                else
                    expected = OTA_RECOVERY_ACTION_START_TRIAL;

                assert(cold_boot_action_after_cut() == expected);
            }
        }
    }
}

static void test_backup_erase_power_cuts(const uint8_t *image)
{
    static const uint32_t prefixes[] = {0U, 1U, 2048U, 4095U, 4096U};
    uint32_t index;

    for (index = 0U; index < sizeof(prefixes) / sizeof(prefixes[0]); ++index)
    {
        prepare_power_cut_install(image, OTA_V2_SLOT_B_START);
        cut_erase_enabled = 1;
        cut_erase_address = OTA_RECOVERY_BACKUP_ADDRESS;
        cut_erase_prefix = prefixes[index];
        power_cut_enabled = 1;
        if (setjmp(power_cut_jump) == 0)
        {
            ota_v2_process();
            assert(!"expected injected erase power cut");
        }
        assert(cold_boot_action_after_cut() == OTA_RECOVERY_ACTION_NORMAL);
        assert(memcmp(&fake_flash[OTA_V2_SLOT_A_START + 8U],
                      "KNLT", 4U) == 0);
        assert(fake_flash[OTA_V2_SLOT_B_START + 8U] == 0xFFU);
    }
}

static void complete_install(const uint8_t *image, uint32_t target)
{
    uint32_t image_crc;
    reset_environment(target);
    image_crc = stage_verified_image(image);
    arm_and_queue_install(image_crc);
    fake_clock += OTA_V2_INSTALL_DELAY_US + 1U;
    ota_v2_process();
    assert(reboot_count == 1);
}

static void test_trial_confirmation(const uint8_t *image)
{
    ota_recovery_scan_t scan;

    complete_install(image, OTA_V2_SLOT_B_START);
    assert(cold_boot_action_after_cut() ==
           OTA_RECOVERY_ACTION_START_TRIAL);
    assert(ota_v2_trial_active == 1U);
    assert(watchdog_started == 1);
    ota_v2_recovery_runtime_ready();
    fake_clock += OTA_V2_TRIAL_CONFIRM_US + 1U;
    ota_v2_process();
    assert(ota_v2_trial_active == 0U);
    assert(watchdog_cleared == 1);
    assert(watchdog_stopped == 1);
    assert(ota_v2_recovery_state == OTA_RECOVERY_STATE_CONFIRMED);
    assert(ota_v2_recovery_action == OTA_RECOVERY_ACTION_NORMAL);
    ota_recovery_scan_journal(
        &fake_flash[OTA_RECOVERY_JOURNAL_ADDRESS],
        OTA_RECOVERY_JOURNAL_SIZE, &scan);
    assert(scan.latest.state == OTA_RECOVERY_STATE_CONFIRMED);
    assert(scan.trial_attempts == 1U);
}

static void test_trial_failure_rolls_back(const uint8_t *image)
{
    ota_recovery_scan_t scan;
    uint32_t target;
    uint32_t source;
    uint32_t direction;
    uint32_t attempt;
    uint32_t i;

    for (direction = 0U; direction < 2U; ++direction)
    {
        target = direction == 0U ? OTA_V2_SLOT_B_START
                                 : OTA_V2_SLOT_A_START;
        source = target == OTA_V2_SLOT_A_START ? OTA_V2_SLOT_B_START
                                                : OTA_V2_SLOT_A_START;
        complete_install(image, target);

        for (attempt = 1U; attempt <= OTA_V2_MAX_TRIAL_ATTEMPTS; ++attempt)
        {
            assert(cold_boot_action_after_cut() ==
                   OTA_RECOVERY_ACTION_START_TRIAL);
            assert(ota_v2_trial_active == 1U);
        }
        assert(cold_boot_action_after_cut() ==
               OTA_RECOVERY_ACTION_ROLLBACK_SOURCE);
        assert(ota_v2_rollback_pending == 1U);

        allow_source_restore = 1;
        ota_v2_process();
        assert(reboot_count == 2);
        assert(memcmp(&fake_flash[source + OTA_V2_BOOT_MARKER_OFFSET],
                      "KNLT", 4U) == 0);
        assert(fake_flash[target + OTA_V2_BOOT_MARKER_OFFSET] == 0x00U);
        for (i = 0U; i < OTA_RECOVERY_BACKUP_SIZE; ++i)
        {
            uint8_t expected =
                i == OTA_RECOVERY_BOOT_MARKER_OFFSET
                    ? (uint8_t)'K'
                    : fake_flash[OTA_RECOVERY_BACKUP_ADDRESS + i];
            assert(fake_flash[source + i] == expected);
        }
        ota_recovery_scan_journal(
            &fake_flash[OTA_RECOVERY_JOURNAL_ADDRESS],
            OTA_RECOVERY_JOURNAL_SIZE, &scan);
        assert(scan.latest.state == OTA_RECOVERY_STATE_ROLLBACK_DONE);
        assert(scan.trial_attempts == OTA_V2_MAX_TRIAL_ATTEMPTS);

        ota_program_offset = source == OTA_V2_SLOT_A_START
                                 ? OTA_V2_SLOT_B_START
                                 : OTA_V2_SLOT_A_START;
        ota_v2_reset_session();
        ota_v2_recovery_init();
        assert(ota_v2_recovery_action == OTA_RECOVERY_ACTION_NORMAL);
    }
}

static void prepare_failed_trial_rollback(const uint8_t *image,
                                          uint32_t target)
{
    uint32_t attempt;

    complete_install(image, target);
    for (attempt = 0U; attempt < OTA_V2_MAX_TRIAL_ATTEMPTS; ++attempt)
    {
        assert(cold_boot_action_after_cut() ==
               OTA_RECOVERY_ACTION_START_TRIAL);
    }
    assert(cold_boot_action_after_cut() ==
           OTA_RECOVERY_ACTION_ROLLBACK_SOURCE);
    assert(ota_v2_rollback_pending == 1U);
    allow_source_restore = 1;
    write_call_count = 0;
}

static void test_rollback_erase_power_cuts(const uint8_t *image)
{
    static const uint32_t prefixes[] = {0U, 1U, 8U, 9U, 2048U, 4095U, 4096U};
    uint32_t direction;
    uint32_t index;
    uint32_t target;
    uint32_t source;

    for (direction = 0U; direction < 2U; ++direction)
    {
        target = direction == 0U ? OTA_V2_SLOT_B_START
                                 : OTA_V2_SLOT_A_START;
        source = target == OTA_V2_SLOT_A_START ? OTA_V2_SLOT_B_START
                                                : OTA_V2_SLOT_A_START;
        for (index = 0U; index < sizeof(prefixes) / sizeof(prefixes[0]);
             ++index)
        {
            prepare_failed_trial_rollback(image, target);
            cut_erase_enabled = 1;
            cut_erase_address = source;
            cut_erase_prefix = prefixes[index];
            power_cut_enabled = 1;
            if (setjmp(power_cut_jump) == 0)
            {
                ota_v2_process();
                assert(!"expected injected rollback erase power cut");
            }
            assert(cold_boot_action_after_cut() ==
                   OTA_RECOVERY_ACTION_ROLLBACK_SOURCE);
        }
    }
}

static void test_every_rollback_write_cut(const uint8_t *image)
{
    int call;
    int direction;
    uint32_t prefix;
    uint32_t length;
    uint32_t target;
    uint32_t source;
    ota_recovery_action_t expected;

    for (direction = 0; direction < 2; ++direction)
    {
        target = direction == 0 ? OTA_V2_SLOT_B_START
                                : OTA_V2_SLOT_A_START;
        source = target == OTA_V2_SLOT_A_START ? OTA_V2_SLOT_B_START
                                                : OTA_V2_SLOT_A_START;
        for (call = 1; call <= 68; ++call)
        {
            length = call == 1 || call == 68 ? 32U :
                     (call <= 65 ? 64U : 4U);
            for (prefix = 0U; prefix <= length; ++prefix)
            {
                prepare_failed_trial_rollback(image, target);
                cut_write_call = call;
                cut_write_prefix = prefix;
                power_cut_enabled = 1;
                if (setjmp(power_cut_jump) == 0)
                {
                    ota_v2_process();
                    assert(!"expected injected rollback write power cut");
                }

                if (call <= 65 || (call == 66 && prefix == 0U))
                    expected = OTA_RECOVERY_ACTION_ROLLBACK_SOURCE;
                else if ((call == 66 ||
                          (call == 67 && prefix == 0U)) &&
                         source == OTA_V2_SLOT_B_START)
                    expected = OTA_RECOVERY_ACTION_ROLLBACK_SOURCE;
                else if (call == 68 && prefix == 32U)
                    expected = OTA_RECOVERY_ACTION_NORMAL;
                else
                    expected = OTA_RECOVERY_ACTION_CLEANUP_FAILED_TARGET;

                assert(cold_boot_action_after_cut() == expected);
            }
        }
    }
}

static void test_install_revalidation(const uint8_t *image)
{
    uint32_t image_crc;

    reset_environment(OTA_V2_SLOT_B_START);
    image_crc = stage_verified_image(image);
    fake_flash[OTA_V2_SLOT_B_START + 100U] ^= 1U;
    confirm_command(OTA_V2_CMD_ARM_INSTALL, OTA_V2_ARM_CONFIRM, image_crc);
    assert(last_notification[3] == OTA_V2_STATUS_CRC_MISMATCH);
    assert(last_notification[4] == OTA_V2_PHASE_ERROR);
    assert(memcmp(&fake_flash[OTA_V2_SLOT_A_START + 8U], "KNLT", 4U) == 0);

    reset_environment(OTA_V2_SLOT_B_START);
    image_crc = stage_verified_image(image);
    confirm_command(OTA_V2_CMD_ARM_INSTALL, OTA_V2_ARM_CONFIRM, image_crc);
    fake_flash[OTA_V2_SLOT_B_START + 100U] ^= 1U;
    confirm_command(OTA_V2_CMD_INSTALL, OTA_V2_INSTALL_CONFIRM, image_crc);
    assert(last_notification[3] == OTA_V2_STATUS_CRC_MISMATCH);
    assert(last_notification[4] == OTA_V2_PHASE_ERROR);
    assert(reboot_count == 0);
}

static void test_marker_failures(const uint8_t *image)
{
    uint32_t image_crc;

    reset_environment(OTA_V2_SLOT_B_START);
    image_crc = stage_verified_image(image);
    arm_and_queue_install(image_crc);
    corrupt_next_write = 1;
    corrupt_write_address = OTA_V2_SLOT_B_START + 8U;
    fake_clock += OTA_V2_INSTALL_DELAY_US + 1U;
    ota_v2_process();
    assert(ota_v2_session.phase == OTA_V2_PHASE_ERROR);
    assert(ota_v2_session.last_status == OTA_V2_STATUS_BOOT_MARK_VERIFY);
    assert(memcmp(&fake_flash[OTA_V2_SLOT_A_START + 8U], "KNLT", 4U) == 0);
    assert(reboot_count == 0 && irq_disabled == 1 && irq_restored == 1);
    assert(active_bank_touched == 0);

    reset_environment(OTA_V2_SLOT_B_START);
    image_crc = stage_verified_image(image);
    arm_and_queue_install(image_crc);
    corrupt_next_write = 1;
    corrupt_write_address = OTA_V2_SLOT_A_START + 8U;
    fake_clock += OTA_V2_INSTALL_DELAY_US + 1U;
    ota_v2_process();
    assert(ota_v2_session.phase == OTA_V2_PHASE_ERROR);
    assert(ota_v2_session.last_status == OTA_V2_STATUS_BOOT_MARK_VERIFY);
    assert(memcmp(&fake_flash[OTA_V2_SLOT_B_START + 8U], "KNLT", 4U) == 0);
    assert(fake_flash[OTA_V2_SLOT_A_START + 8U] != 0x00U);
    assert(reboot_count == 0 && irq_disabled == 1 && irq_restored == 1);
}

static void assert_candidate_matches_masked(const uint8_t *image)
{
    uint32_t i;
    for (i = 0; i < TEST_IMAGE_SIZE; ++i)
    {
        uint8_t expected = i == 8U ? 0xFFU : image[i];
        assert(fake_flash[expected_target + i] == expected);
    }
}

static void test_slot(uint32_t target, uint8_t current_slot, uint8_t target_slot,
                      const uint8_t *image)
{
    uint8_t info[] = {0x10};
    reset_environment(target);
    invoke(info, sizeof(info));
    assert(last_notification[21] == current_slot);
    assert(last_notification[22] == target_slot);

    start_transfer(image, crc32(image, TEST_IMAGE_SIZE));
    assert(fake_flash[target + 8U] == 0xFFU);
    finish_with_status(0U);
    assert_candidate_matches_masked(image);
    assert(active_bank_touched == 0);
}

static void test_manifest_failure(uint8_t *image)
{
    reset_environment(0x20000U);
    start_transfer(image, crc32(image, TEST_IMAGE_SIZE));
    finish_with_status(10U);
    assert(active_bank_touched == 0);
}

static void test_header_failure(uint8_t *image)
{
    reset_environment(0x20000U);
    start_transfer(image, crc32(image, TEST_IMAGE_SIZE));
    finish_with_status(11U);
    assert(active_bank_touched == 0);
}

int main(void)
{
    uint8_t info[] = {0x10};
    uint8_t command[5];
    uint8_t image[TEST_IMAGE_SIZE];
    uint8_t changed[TEST_IMAGE_SIZE];
    uint32_t i;

    make_valid_image(image);

    /* Both SDK-selected layouts write only the inactive slot. */
    test_slot(0x20000U, 0U, 1U, image);
    test_slot(0x00000U, 1U, 0U, image);

    /* Unsupported offsets are rejected before the first erase. */
    reset_environment(0x10000U);
    begin_image(TEST_IMAGE_SIZE, crc32(image, TEST_IMAGE_SIZE));
    assert(last_notification[3] == 14U);
    assert(last_notification[4] == 4U);
    assert(last_notification[21] == 0xFFU && last_notification[22] == 0xFFU);
    assert(erase_count == 0);
    command[0] = 0x12;
    invoke(command, 1U);
    assert(last_notification[3] == 4U);
    assert(erase_count == 0);

    /* Old ATC commands remain unavailable. */
    reset_environment(0x20000U);
    for (i = 0U; i <= 7U; ++i)
    {
        command[0] = (uint8_t)i;
        invoke(command, 1U);
        assert(last_notification[3] == 9U);
    }
    assert(active_bank_touched == 0 && erase_count == 0);

    /* The original upload must contain KNLT, but flash keeps byte 8 erased. */
    memcpy(changed, image, sizeof(changed));
    changed[8] = 0x4AU;
    reset_environment(0x20000U);
    begin_image(TEST_IMAGE_SIZE, crc32(changed, TEST_IMAGE_SIZE));
    erase_all();
    send_data(0U, changed, 24U);
    assert(last_notification[3] == 11U);
    assert(last_notification[4] == 4U);
    assert(fake_flash[0x20008U] == 0xFFU);

    /* Browser/session CRC and the independent full-flash CRC are separate. */
    reset_environment(0x20000U);
    start_transfer(image, crc32(image, TEST_IMAGE_SIZE) ^ 1U);
    finish_with_status(8U);

    reset_environment(0x20000U);
    start_transfer(image, crc32(image, TEST_IMAGE_SIZE));
    fake_flash[0x20000U + 100U] ^= 1U;
    finish_with_status(8U);

    /* Manifest board, length, payload CRC and manifest CRC fail independently. */
    memcpy(changed, image, sizeof(changed));
    put_u16(&changed[TEST_PAYLOAD_SIZE + 6U], 0x9999U);
    refresh_manifest_crc(changed);
    test_manifest_failure(changed);

    memcpy(changed, image, sizeof(changed));
    put_u32(&changed[TEST_PAYLOAD_SIZE + 8U], TEST_PAYLOAD_SIZE + 1U);
    refresh_manifest_crc(changed);
    test_manifest_failure(changed);

    memcpy(changed, image, sizeof(changed));
    changed[TEST_PAYLOAD_SIZE + 12U] ^= 1U;
    refresh_manifest_crc(changed);
    test_manifest_failure(changed);

    memcpy(changed, image, sizeof(changed));
    changed[TEST_PAYLOAD_SIZE + 16U] ^= 1U;
    test_manifest_failure(changed);

    /* Telink KNLT, length and native big-endian CRC are all required. */
    memcpy(changed, image, sizeof(changed));
    changed[9] = 'X';
    refresh_telink_crc(changed);
    refresh_manifest(changed, 1U);
    test_header_failure(changed);

    memcpy(changed, image, sizeof(changed));
    put_u32(&changed[0x18], TEST_PAYLOAD_SIZE - 8U);
    refresh_telink_crc(changed);
    refresh_manifest(changed, 1U);
    test_header_failure(changed);

    memcpy(changed, image, sizeof(changed));
    changed[TEST_PAYLOAD_SIZE - 1U] ^= 1U;
    refresh_manifest(changed, 1U);
    test_header_failure(changed);

    /* Wrong order, duplicate, page crossing, range and readback corruption. */
    reset_environment(0x20000U);
    begin_image(TEST_IMAGE_SIZE, crc32(image, TEST_IMAGE_SIZE));
    send_data(0U, image, 24U);
    assert(last_notification[3] == 4U);
    erase_all();
    send_data(1U, image, 8U);
    assert(last_notification[3] == 6U);
    for (i = 0U; i < 240U; i += 24U)
    {
        send_data(i, image + i, 24U);
        assert(last_notification[3] == 0U);
    }
    send_data(240U, image + 240U, 10U);
    assert(last_notification[3] == 0U);
    send_data(250U, image + 250U, 10U);
    assert(last_notification[3] == 5U);
    send_data(250U, image + 250U, 6U);
    assert(last_notification[3] == 0U);
    send_data(250U, image + 250U, 6U);
    assert(last_notification[3] == 6U);

    reset_environment(0x20000U);
    begin_image(56U, crc32(image, 56U));
    erase_all();
    send_data(0U, image, 24U);
    send_data(24U, image + 24U, 24U);
    send_data(48U, image + 48U, 9U);
    assert(last_notification[3] == 5U);

    reset_environment(0x20000U);
    begin_image(TEST_IMAGE_SIZE, crc32(image, TEST_IMAGE_SIZE));
    erase_all();
    corrupt_next_write = 1;
    send_data(0U, image, 24U);
    assert(last_notification[3] == 7U);
    assert(last_notification[4] == 4U);
    assert(active_bank_touched == 0);

    /* INFO remains harmless and reports the selected inactive slot. */
    reset_environment(0x20000U);
    invoke(info, sizeof(info));
    assert(last_notification[3] == 0U);
    assert(last_notification[21] == 0U && last_notification[22] == 1U);

    test_install_guards_and_success(image);
    test_install_revalidation(image);
    test_recovery_write_guards(image);
    test_backup_erase_power_cuts(image);
    test_every_install_write_cut(image);
    test_trial_confirmation(image);
    test_trial_failure_rolls_back(image);
    test_rollback_erase_power_cuts(image);
    test_every_rollback_write_cut(image);
    test_marker_failures(image);

    puts("PASS: OTA v2 M2-C journals installs, confirms healthy trials, and rolls back failed trials.");
    return 0;
}
