/*
 * ============================================================================
 * 阿波罗 V2 板载 W25Q256 真板自检
 * ============================================================================
 *
 * 本任务只在 ENABLE_FLASH_SELF_TEST=1 的隔离镜像中创建，USART1 专用于日志。
 * 所有预期擦写都限制在 Flash 最后 64 KiB 的自检区：
 *   0x01FF0000..0x01FF3FFF  版本/FRU 双副本临时槽
 *   0x01FFD000..0x01FFDFFF  MCU 复位保持标记
 *   0x01FFE000..0x01FFFFFF  原始驱动读写测试
 *
 * 高地址测试同时读取其 24 位别名 0x00FFE000。如果驱动错误地丢失最高地址位，
 * 自检会发现别名区变化并按测试前快照恢复，避免留下低地址数据破坏。
 * ============================================================================
 */
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "usart.h"
#include "flash_bsp.h"
#include "flash_layout.h"
#include "qspi_hw.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

extern UART_HandleTypeDef huart1;

#define SELF_TEST_MARKER_ADDRESS   0x01FFD000UL
#define SELF_TEST_RAW_ADDRESS      0x01FFE000UL
#define SELF_TEST_RAW_SIZE         (2UL * W25Q_SECTOR_SIZE)
#define SELF_TEST_ALIAS_ADDRESS    (SELF_TEST_RAW_ADDRESS & 0x00FFFFFFUL)
#define SELF_TEST_MARKER_MAGIC     0x54533257UL /* 小端字节序 ASCII "W2ST" */
#define SELF_TEST_MARKER_PHASE     0x00000001UL

typedef struct {
    uint32_t magic;
    uint32_t phase;
    uint32_t crc32;
} reset_marker_t;

/* 静态缓冲不占测试任务栈；8 KiB 快照只用于错误寻址时恢复低地址别名区。 */
static uint8_t s_alias_snapshot[SELF_TEST_RAW_SIZE];
static uint8_t s_io_buffer[512];
static uint8_t s_expected_buffer[512];

/*
 * SWD 诊断镜像：串口接线或终端异常时仍可从 ELF 符号地址读取最终结果。
 * [0]=magic, [1]=启动轮次, [2]=最终状态, [3]=当前/失败步骤, [4..]=测量值。
 */
volatile uint32_t flash_test_diag[12];

static void test_puts(const char *text)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)text, (uint16_t)strlen(text), 500U);
}

static void test_printf(const char *format, ...)
{
    char buffer[192];
    va_list arguments;
    int length;

    va_start(arguments, format);
    length = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);

    if (length <= 0) {
        return;
    }
    if ((size_t)length >= sizeof(buffer)) {
        length = (int)sizeof(buffer) - 1;
    }
    HAL_UART_Transmit(&huart1, (uint8_t *)buffer, (uint16_t)length, 500U);
}

static int buffer_is_value(const uint8_t *buffer, uint32_t length, uint8_t value)
{
    for (uint32_t index = 0U; index < length; ++index) {
        if (buffer[index] != value) {
            test_printf("    首个不匹配偏移=%lu，期望=%02X，实际=%02X\r\n",
                        index, value, buffer[index]);
            return 0;
        }
    }
    return 1;
}

static int erase_and_verify_blank(uint32_t address, uint32_t length)
{
    if (flash_if.erase(&flash_if, address, length) != IF_OK) {
        return 0;
    }

    for (uint32_t offset = 0U; offset < length; offset += sizeof(s_io_buffer)) {
        uint32_t chunk = length - offset;
        if (chunk > sizeof(s_io_buffer)) {
            chunk = sizeof(s_io_buffer);
        }
        if (flash_if.read(&flash_if, address + offset, s_io_buffer, chunk) != IF_OK ||
            !buffer_is_value(s_io_buffer, chunk, 0xFFU)) {
            return 0;
        }
    }
    return 1;
}

static int restore_alias_snapshot(void)
{
    if (flash_if.erase(&flash_if, SELF_TEST_ALIAS_ADDRESS,
                       SELF_TEST_RAW_SIZE) != IF_OK) {
        return 0;
    }
    return flash_if.write(&flash_if, SELF_TEST_ALIAS_ADDRESS,
                          s_alias_snapshot, SELF_TEST_RAW_SIZE) == IF_OK;
}

static int verify_identification_and_geometry(void)
{
    uint8_t jedec_id[3] = {0xFFU, 0xFFU, 0xFFU};
    uint8_t status1 = 0xFFU;
    uint8_t status2 = 0xFFU;
    uint8_t status3 = 0xFFU;
    uint32_t total_kb = 0U;
    uint32_t sector_size = 0U;
    uint32_t page_size = 0U;

    test_puts("[1] 器件身份、QE 与容量参数...\r\n");
    if (qspi_read_jedec_id(jedec_id) != IF_OK ||
        jedec_id[0] != 0xEFU || jedec_id[1] != 0x40U || jedec_id[2] != 0x19U) {
        test_printf("    FAIL: JEDEC=%02X %02X %02X，期望 EF 40 19\r\n",
                    jedec_id[0], jedec_id[1], jedec_id[2]);
        return 0;
    }
    flash_test_diag[4] = ((uint32_t)jedec_id[0] << 16U) |
                         ((uint32_t)jedec_id[1] << 8U) | jedec_id[2];

    if (qspi_read_status(W25Q_CMD_READ_STATUS1, &status1) != IF_OK ||
        qspi_read_status(W25Q_CMD_READ_STATUS2, &status2) != IF_OK ||
        qspi_read_status(W25Q_CMD_READ_STATUS3, &status3) != IF_OK ||
        (status2 & W25Q_SR2_QE) == 0U) {
        test_printf("    FAIL: SR1=%02X SR2=%02X SR3=%02X，QE 必须为 1\r\n",
                    status1, status2, status3);
        return 0;
    }
    flash_test_diag[5] = ((uint32_t)status1 << 16U) |
                         ((uint32_t)status2 << 8U) | status3;

    flash_if.info(&flash_if, &total_kb, &sector_size, &page_size);
    if (total_kb != 32768U || sector_size != 4096U || page_size != 256U) {
        test_printf("    FAIL: 容量=%luKiB 扇区=%lu 页=%lu\r\n",
                    total_kb, sector_size, page_size);
        return 0;
    }
    flash_test_diag[6] = total_kb;

    test_printf("    PASS: JEDEC=EF 40 19，SR1=%02X SR2=%02X SR3=%02X，32MiB/4KiB/256B\r\n",
                status1, status2, status3);
    return 1;
}

static int verify_api_boundaries(void)
{
    uint8_t data[16] = {0};

    test_puts("[2] 参数边界保护...\r\n");
    if (qspi_write_page(SELF_TEST_RAW_ADDRESS + 250U, data,
                        sizeof(data)) != IF_ERR ||
        qspi_read_data(W25Q_CAPACITY - 16U, data, 17U) != IF_ERR ||
        qspi_erase_sector(SELF_TEST_RAW_ADDRESS + 1U) != IF_ERR ||
        flash_if.read(&flash_if, W25Q_CAPACITY, data, 1U) != IF_ERR ||
        flash_if.write(&flash_if, W25Q_CAPACITY, data, 1U) != IF_ERR) {
        test_puts("    FAIL: 至少一个越界或未对齐请求未被拒绝\r\n");
        return 0;
    }
    test_puts("    PASS: 跨页、越界和非对齐擦除均被拒绝\r\n");
    return 1;
}

static int verify_high_address_and_raw_io(void)
{
    uint8_t alias_after[sizeof(s_io_buffer)];

    test_printf("[3] 4 字节地址与原始读写 @0x%08lX...\r\n",
                SELF_TEST_RAW_ADDRESS);

    /* 先保存完整低地址别名区，异常时可以原样恢复。 */
    if (flash_if.read(&flash_if, SELF_TEST_ALIAS_ADDRESS, s_alias_snapshot,
                      sizeof(s_alias_snapshot)) != IF_OK) {
        test_puts("    FAIL: 无法读取 24 位别名区快照\r\n");
        return 0;
    }

    if (!erase_and_verify_blank(SELF_TEST_RAW_ADDRESS, SELF_TEST_RAW_SIZE)) {
        test_puts("    FAIL: 高地址 8KiB 擦除态校验失败\r\n");
        return 0;
    }

    /* 从别名原值派生测试数据，确保错误回卷时低地址一定发生可检测变化。 */
    for (uint32_t index = 0U; index < sizeof(s_expected_buffer); ++index) {
        s_expected_buffer[index] =
            s_alias_snapshot[128U + index] ^ (uint8_t)(0xA5U + index);
    }
    if (flash_if.write(&flash_if, SELF_TEST_RAW_ADDRESS + 128U,
                       s_expected_buffer, sizeof(s_expected_buffer)) != IF_OK ||
        flash_if.read(&flash_if, SELF_TEST_RAW_ADDRESS + 128U,
                      s_io_buffer, sizeof(s_io_buffer)) != IF_OK ||
        memcmp(s_io_buffer, s_expected_buffer, sizeof(s_io_buffer)) != 0) {
        test_puts("    FAIL: 跨三个页的 512B 写入或回读不一致\r\n");
        return 0;
    }

    if (flash_if.read(&flash_if, SELF_TEST_ALIAS_ADDRESS + 128U,
                      alias_after, sizeof(alias_after)) != IF_OK) {
        test_puts("    FAIL: 无法复查低地址别名区\r\n");
        return 0;
    }
    if (memcmp(alias_after, s_alias_snapshot + 128U, sizeof(alias_after)) != 0) {
        int restored = restore_alias_snapshot();
        flash_test_diag[11] = restored ? 2U : 3U;
        test_printf("    FAIL: 检测到 24 位地址回卷，别名区恢复%s\r\n",
                    restored ? "成功" : "失败");
        return 0;
    }
    flash_test_diag[11] = 1U;

    /* 同扇区分两次更新，验证 flash_if 的 Read-Modify-Write 不破坏前半数据。 */
    memset(s_expected_buffer, 0xAB, 256U);
    if (flash_if.write(&flash_if, SELF_TEST_RAW_ADDRESS,
                       s_expected_buffer, 256U) != IF_OK) {
        test_puts("    FAIL: RMW 第一次写入失败\r\n");
        return 0;
    }
    memset(s_expected_buffer, 0xCD, 256U);
    if (flash_if.write(&flash_if, SELF_TEST_RAW_ADDRESS + 256U,
                       s_expected_buffer, 256U) != IF_OK ||
        flash_if.read(&flash_if, SELF_TEST_RAW_ADDRESS,
                      s_io_buffer, sizeof(s_io_buffer)) != IF_OK ||
        !buffer_is_value(s_io_buffer, 256U, 0xABU) ||
        !buffer_is_value(s_io_buffer + 256U, 256U, 0xCDU)) {
        test_puts("    FAIL: 同扇区 RMW 数据保护失败\r\n");
        return 0;
    }

    test_puts("    PASS: 擦除态、跨页写、32 位地址及扇区 RMW 正常\r\n");
    return 1;
}

static int verify_version_records(void)
{
    firmware_version_info_t expected;
    firmware_version_info_t loaded;
    uint32_t generation = 0U;
    uint8_t corrupt_byte;

    test_puts("[4] 固件版本 A/B 双副本与 CRC 回退...\r\n");
    if (flash_if.erase(&flash_if, FLASH_LAYOUT_VERSION_SLOT_A,
                       2U * FLASH_LAYOUT_SECTOR_SIZE) != IF_OK) {
        return 0;
    }

    expected = *firmware_version_current();
    expected.flags |= FIRMWARE_VERSION_FLAG_IMAGE_VALID;
    expected.image_length = 0x00123456UL;
    expected.image_crc32 = 0x89ABCDEFUL;

    if (firmware_version_save(&flash_if, &expected, &generation) != FLASH_STORE_OK ||
        generation != 1U ||
        firmware_version_save(&flash_if, &expected, &generation) != FLASH_STORE_OK ||
        generation != 2U ||
        firmware_version_load(&flash_if, &loaded, &generation) != FLASH_STORE_OK ||
        generation != 2U || memcmp(&loaded, &expected, sizeof(loaded)) != 0) {
        test_puts("    FAIL: 双副本保存或最新 generation 选择错误\r\n");
        return 0;
    }

    /* 第二次保存落在 B 槽；只破坏载荷，不更新头部 CRC，必须自动回退 A。 */
    if (flash_if.read(&flash_if,
                      FLASH_LAYOUT_VERSION_SLOT_B + sizeof(flash_record_header_t),
                      &corrupt_byte, 1U) != IF_OK) {
        return 0;
    }
    corrupt_byte ^= 0x01U;
    if (flash_if.write(&flash_if,
                       FLASH_LAYOUT_VERSION_SLOT_B + sizeof(flash_record_header_t),
                       &corrupt_byte, 1U) != IF_OK ||
        firmware_version_load(&flash_if, &loaded, &generation) != FLASH_STORE_OK ||
        generation != 1U || memcmp(&loaded, &expected, sizeof(loaded)) != 0) {
        test_puts("    FAIL: B 槽 CRC 损坏后未回退到 A 槽\r\n");
        return 0;
    }

    test_puts("    PASS: generation=2 可选中，损坏 B 后回退 generation=1\r\n");
    flash_test_diag[8] = generation;
    return 1;
}

static void fill_test_fru(fru_info_t *fru, const char *site_label)
{
    static const uint8_t mac[6] = {0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U};
    static const uint8_t ipv4[4] = {192U, 168U, 1U, 201U};
    static const uint8_t netmask[4] = {255U, 255U, 255U, 0U};
    static const uint8_t gateway[4] = {192U, 168U, 1U, 1U};

    fru_info_init(fru);
    snprintf(fru->product_name, sizeof(fru->product_name), "LPDDR4-B1");
    snprintf(fru->part_number, sizeof(fru->part_number), "B1-APOLLO-TEST");
    snprintf(fru->serial_number, sizeof(fru->serial_number), "SELFTEST-0001");
    snprintf(fru->hardware_revision, sizeof(fru->hardware_revision), "V2");
    snprintf(fru->manufacturing_date, sizeof(fru->manufacturing_date), "2026-08-24");
    memcpy(fru->mac_address, mac, sizeof(mac));
    fru->mac_valid = 1U;
    fru->network_mode = FRU_NETWORK_MODE_STATIC;
    memcpy(fru->configured_ipv4, ipv4, sizeof(ipv4));
    memcpy(fru->configured_netmask, netmask, sizeof(netmask));
    memcpy(fru->configured_gateway, gateway, sizeof(gateway));
    fru->service_port = 8888U;
    snprintf(fru->site_label, sizeof(fru->site_label), "%s", site_label);
}

static int verify_fru_records(void)
{
    fru_info_t first;
    fru_info_t second;
    fru_info_t loaded;
    uint32_t generation = 0U;
    uint32_t corrupt_address;
    uint8_t corrupt_byte;

    test_puts("[5] FRU A/B 双副本、网络定位字段与 CRC 回退...\r\n");
    if (flash_if.erase(&flash_if, FLASH_LAYOUT_FRU_SLOT_A,
                       2U * FLASH_LAYOUT_SECTOR_SIZE) != IF_OK) {
        return 0;
    }

    fill_test_fru(&first, "SITE-A");
    fill_test_fru(&second, "SITE-B");
    if (fru_info_save(&flash_if, &first, &generation) != FLASH_STORE_OK ||
        generation != 1U ||
        fru_info_save(&flash_if, &second, &generation) != FLASH_STORE_OK ||
        generation != 2U ||
        fru_info_load(&flash_if, &loaded, &generation) != FLASH_STORE_OK ||
        generation != 2U || memcmp(&loaded, &second, sizeof(loaded)) != 0 ||
        loaded.configured_ipv4[0] != 192U || loaded.configured_ipv4[3] != 201U ||
        loaded.service_port != 8888U) {
        test_puts("    FAIL: FRU 最新副本或网络定位字段不一致\r\n");
        return 0;
    }

    corrupt_address = FLASH_LAYOUT_FRU_SLOT_B + sizeof(flash_record_header_t) +
                      offsetof(fru_info_t, site_label);
    if (flash_if.read(&flash_if, corrupt_address, &corrupt_byte, 1U) != IF_OK) {
        return 0;
    }
    corrupt_byte ^= 0x01U;
    if (flash_if.write(&flash_if, corrupt_address, &corrupt_byte, 1U) != IF_OK ||
        fru_info_load(&flash_if, &loaded, &generation) != FLASH_STORE_OK ||
        generation != 1U || memcmp(&loaded, &first, sizeof(loaded)) != 0) {
        test_puts("    FAIL: FRU B 槽损坏后未回退到 A 槽\r\n");
        return 0;
    }

    test_puts("    PASS: IP=192.168.1.201:8888，损坏 B 后回退 generation=1\r\n");
    flash_test_diag[9] = generation;
    return 1;
}

static int marker_is_valid(const reset_marker_t *marker)
{
    return marker->magic == SELF_TEST_MARKER_MAGIC &&
           marker->phase == SELF_TEST_MARKER_PHASE &&
           marker->crc32 == flash_crc32((const uint8_t *)marker,
                                        offsetof(reset_marker_t, crc32));
}

static int read_reset_marker(reset_marker_t *marker)
{
    return flash_if.read(&flash_if, SELF_TEST_MARKER_ADDRESS,
                         (uint8_t *)marker, sizeof(*marker)) == IF_OK;
}

static int write_reset_marker(void)
{
    reset_marker_t marker;
    reset_marker_t verify;

    marker.magic = SELF_TEST_MARKER_MAGIC;
    marker.phase = SELF_TEST_MARKER_PHASE;
    marker.crc32 = flash_crc32((const uint8_t *)&marker,
                               offsetof(reset_marker_t, crc32));

    return flash_if.erase(&flash_if, SELF_TEST_MARKER_ADDRESS,
                          FLASH_LAYOUT_SECTOR_SIZE) == IF_OK &&
           flash_if.write(&flash_if, SELF_TEST_MARKER_ADDRESS,
                          (const uint8_t *)&marker, sizeof(marker)) == IF_OK &&
           read_reset_marker(&verify) && marker_is_valid(&verify);
}

void StartQspiTestTask(void *argument)
{
    reset_marker_t marker;
    uint32_t qspi_cr;
    uint32_t qspi_sr;
    uint32_t qspi_ccr;
    uint32_t qspi_dcr;
    int second_boot;
    int passed;

    (void)argument;
    memset((void *)flash_test_diag, 0, sizeof(flash_test_diag));
    flash_test_diag[0] = 0x57323554UL; /* "W25T" */
    vTaskDelay(pdMS_TO_TICKS(1500U));
    test_puts("\r\n=== W25Q256 HARDWARE SELF-TEST ===\r\n");

    qspi_dump_regs(&qspi_cr, &qspi_sr, &qspi_ccr, &qspi_dcr);
    test_printf("QSPI: CR=%08lX SR=%08lX CCR=%08lX DCR=%08lX\r\n",
                qspi_cr, qspi_sr, qspi_ccr, qspi_dcr);

    if (flash_if.init(&flash_if) != IF_OK) {
        flash_test_diag[2] = 0xE0010000UL;
        test_puts("--- W25Q256 HARDWARE TEST FAIL: 初始化失败 ---\r\n");
        vTaskDelete(NULL);
    }

    if (!read_reset_marker(&marker)) {
        flash_test_diag[2] = 0xE0020000UL;
        test_puts("--- W25Q256 HARDWARE TEST FAIL: 无法读取复位标记 ---\r\n");
        vTaskDelete(NULL);
    }
    second_boot = marker_is_valid(&marker);
    flash_test_diag[1] = second_boot ? 2U : 1U;
    if (second_boot) {
        test_puts("[0] PASS: W25Q256 数据跨 MCU 软件复位保持\r\n");
        if (flash_if.erase(&flash_if, SELF_TEST_MARKER_ADDRESS,
                           FLASH_LAYOUT_SECTOR_SIZE) != IF_OK) {
            flash_test_diag[2] = 0xE0030000UL;
            test_puts("--- W25Q256 HARDWARE TEST FAIL: 清除复位标记失败 ---\r\n");
            vTaskDelete(NULL);
        }
    } else {
        test_puts("[0] 首轮启动：完成其余测试后写标记并软件复位\r\n");
    }

    passed = 1;
    flash_test_diag[3] = 1U;
    if (!verify_identification_and_geometry()) passed = 0;
    flash_test_diag[7] |= passed ? (1UL << 0U) : 0U;
    if (passed) {
        flash_test_diag[3] = 2U;
        if (!verify_api_boundaries()) passed = 0;
        flash_test_diag[7] |= passed ? (1UL << 1U) : 0U;
    }
    if (passed) {
        flash_test_diag[3] = 3U;
        if (!verify_high_address_and_raw_io()) passed = 0;
        flash_test_diag[7] |= passed ? (1UL << 2U) : 0U;
    }
    if (passed) {
        flash_test_diag[3] = 4U;
        if (!verify_version_records()) passed = 0;
        flash_test_diag[7] |= passed ? (1UL << 3U) : 0U;
    }
    if (passed) {
        flash_test_diag[3] = 5U;
        if (!verify_fru_records()) passed = 0;
        flash_test_diag[7] |= passed ? (1UL << 4U) : 0U;
    }

    if (!passed) {
        flash_test_diag[2] = 0xE1000000UL | flash_test_diag[3];
        test_puts("--- W25Q256 HARDWARE TEST FAIL ---\r\n");
        vTaskDelete(NULL);
    }

    if (!second_boot) {
        if (!write_reset_marker()) {
            flash_test_diag[2] = 0xE0040000UL;
            test_puts("--- W25Q256 HARDWARE TEST FAIL: 复位标记写入校验失败 ---\r\n");
            vTaskDelete(NULL);
        }
        test_puts("首轮全部通过，执行 NVIC_SystemReset 验证持久化...\r\n");
        flash_test_diag[2] = 0x100D0001UL;
        vTaskDelay(pdMS_TO_TICKS(100U));
        NVIC_SystemReset();
    }

    flash_test_diag[2] = 0x600D600DUL;
    flash_test_diag[3] = 6U;
    test_puts("--- W25Q256 HARDWARE TEST PASS ---\r\n");
    test_puts("测试只使用最后 64KiB；正式版本/FRU 地址未擦写。\r\n");
    vTaskDelete(NULL);
}
