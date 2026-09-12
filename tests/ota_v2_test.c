#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RAM
#define _attribute_ram_code_
#define OTA_CMD_OUT_DP_H 27
#define OTA_V2_HOST_TEST 1
#define HINK_FW_VERSION 2U

typedef struct
{
    uint16_t l2capLen;
    uint8_t value[64];
} rf_packet_att_write_t;

uint32_t ota_program_offset;

static uint8_t fake_flash[0x40000];
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

static int overlaps(uint32_t address, uint32_t len, uint32_t start, uint32_t size)
{
    return address < start + size && start < address + len;
}

static void flash_erase_sector(unsigned long address)
{
    assert((address & 0xFFFUL) == 0U);
    assert(address >= expected_target);
    assert(address + 0x1000UL <= expected_target + 0x1F000UL);
    assert(address + 0x1000UL <= sizeof(fake_flash));
    if (overlaps((uint32_t)address, 0x1000U, active_start, 0x1F000U))
        active_bank_touched = 1;
    memset(&fake_flash[address], 0xff, 0x1000);
    erase_count++;
}

static void flash_write_page(unsigned long address, unsigned long len, unsigned char *data)
{
    unsigned long i;
    int target_write = address >= expected_target &&
                       address + len <= expected_target + 0x1F000UL;
    int current_marker_write = address == active_start + 8U && len == 4U;
    assert(target_write || current_marker_write);
    assert(address + len <= sizeof(fake_flash));
    if (overlaps((uint32_t)address, (uint32_t)len, active_start, 0x1F000U))
        active_bank_touched = 1;
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
    assert(last_notification_len == 28);
    assert(last_notification[0] == 0xA2);
    assert(last_notification[1] == 1);
    assert(last_notification[2] == payload[0]);
    assert(last_notification[20] == 1);
    assert(last_notification[23] == 1);
    assert(last_notification[24] == 2 && last_notification[25] == 0 &&
           last_notification[26] == 0 && last_notification[27] == 0);
}

static void reset_environment(uint32_t target)
{
    memset(fake_flash, 0x5a, sizeof(fake_flash));
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
    test_marker_failures(image);

    puts("PASS: OTA v2 M1-C requires two confirmations and safely switches boot markers.");
    return 0;
}
