/*
 * TPS62868/69、PAC1934、AD7124、MCP4725事务级Mock验证。
 *
 * 本任务只在ENABLE_DRIVER_MOCK_TEST=1的隔离镜像中创建。它调用正式驱动
 * 源码，但用内存中的I2C/SPI接口代替不存在的外接芯片，从而验证寄存器
 * 地址、数据编码、错误传播和换算公式。结果经USART1输出，不控制电源。
 */
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "usart.h"
#include "ad7124.h"
#include "bsp_gpio.h"
#include "board_interfaces.h"
#include "crc.h"
#include "ddr_power.h"
#include "frame.h"
#include "mcp4725.h"
#include "memory_monitor.h"
#include "pac1934.h"
#include "pc_dispatch.h"
#include "pc_protocol.h"
#include "pc_state_machine.h"
#include "pc_test_message.h"
#include "pwr_ctrl.h"
#include "rk_link.h"
#include "state_machine.h"
#include "uart_router.h"
#include "tps62868.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MOCK_LOG_CAPACITY  128U

typedef enum {
    MOCK_OP_READ,
    MOCK_OP_WRITE,
    MOCK_OP_WRITE_BYTES,
    MOCK_OP_COMMAND,
} mock_op_kind_t;

typedef struct {
    mock_op_kind_t kind;
    uint8_t address;
    uint8_t reg;
    uint8_t value;
    uint16_t length;
} mock_i2c_op_t;

typedef struct {
    i2c_if_t interface; /* 必须放首字段，回调可安全转换回完整Mock对象。 */
    mock_i2c_op_t log[MOCK_LOG_CAPACITY];
    uint8_t ready[128];
    uint8_t product_id;
    uint8_t manufacturer_id;
    uint8_t tps_status;
    uint8_t count;
    uint8_t read_before_wait;
    uint8_t fail_at_operation; /* 从1计数，0不注入；仅影响寄存器读/写。 */
} mock_i2c_t;

typedef struct {
    spi_if_t interface; /* 必须放首字段。 */
    uint8_t tx[MOCK_LOG_CAPACITY][8];
    uint8_t length[MOCK_LOG_CAPACITY];
    uint8_t count;
    uint8_t operation_count;
    uint8_t fail_at_operation;
    uint8_t id_value;
    uint8_t data_channel;
} mock_spi_t;

static uint32_t s_pass_count;
static uint32_t s_fail_count;
static uint32_t s_pac_waited_ms;

static void mock_uart_printf(const char *format, ...)
{
    char buffer[160];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length <= 0) return;
    if ((size_t)length >= sizeof(buffer)) length = (int)sizeof(buffer) - 1;
    HAL_UART_Transmit(&huart1, (uint8_t *)buffer, (uint16_t)length, 200U);
}

static void mock_check(int condition, const char *name)
{
    if (condition) {
        s_pass_count++;
    } else {
        s_fail_count++;
        mock_uart_printf("[FAIL] %s\r\n", name);
    }
}

static void mock_i2c_record(mock_i2c_t *mock, mock_op_kind_t kind,
                            uint8_t address, uint8_t reg,
                            uint8_t value, uint16_t length)
{
    if (mock->count >= MOCK_LOG_CAPACITY) return;
    mock_i2c_op_t *entry = &mock->log[mock->count++];
    entry->kind = kind;
    entry->address = address;
    entry->reg = reg;
    entry->value = value;
    entry->length = length;
}

static int mock_i2c_read_bytes(i2c_if_t *interface, uint8_t address,
                               uint8_t reg, uint8_t *buffer, uint16_t length)
{
    mock_i2c_t *mock = (mock_i2c_t *)interface->handle;
    mock_i2c_record(mock, MOCK_OP_READ, address, reg, 0U, length);
    if (mock->fail_at_operation != 0U && mock->count == mock->fail_at_operation) {
        return IF_ERR;
    }
    memset(buffer, 0, length);

    if (reg == PAC1934_REG_PRODUCT_ID && length == 1U) {
        buffer[0] = mock->product_id;
    } else if (reg == PAC1934_REG_MANUF_ID && length == 1U) {
        buffer[0] = mock->manufacturer_id;
    } else if (reg == TPS62868_REG_STATUS && length == 1U) {
        buffer[0] = mock->tps_status;
    } else if (reg >= PAC1934_REG_VBUS1 && reg <= PAC1934_REG_VBUS1 + 3U && length == 2U) {
        uint16_t raw = (uint16_t)(0x1000U * (reg - PAC1934_REG_VBUS1 + 1U));
        if (s_pac_waited_ms < 1U) mock->read_before_wait = 1U;
        buffer[0] = (uint8_t)(raw >> 8);
        buffer[1] = (uint8_t)raw;
    } else if (reg >= PAC1934_REG_VSENSE1 && reg <= PAC1934_REG_VSENSE1 + 3U && length == 2U) {
        uint16_t raw = (uint16_t)(0x0800U * (reg - PAC1934_REG_VSENSE1 + 1U));
        if (s_pac_waited_ms < 1U) mock->read_before_wait = 1U;
        buffer[0] = (uint8_t)(raw >> 8);
        buffer[1] = (uint8_t)raw;
    } else if (reg >= PAC1934_REG_VPOWER1 && reg <= PAC1934_REG_VPOWER1 + 3U && length == 4U) {
        uint32_t raw = 0x01000000UL * (reg - PAC1934_REG_VPOWER1 + 1U);
        if (s_pac_waited_ms < 1U) mock->read_before_wait = 1U;
        buffer[0] = (uint8_t)(raw >> 24);
        buffer[1] = (uint8_t)(raw >> 16);
        buffer[2] = (uint8_t)(raw >> 8);
        buffer[3] = (uint8_t)raw;
    }
    return IF_OK;
}

static int mock_i2c_read_reg(i2c_if_t *interface, uint8_t address,
                             uint8_t reg, uint8_t *value)
{
    return mock_i2c_read_bytes(interface, address, reg, value, 1U);
}

static int mock_i2c_write_reg(i2c_if_t *interface, uint8_t address,
                              uint8_t reg, uint8_t value)
{
    mock_i2c_t *mock = (mock_i2c_t *)interface->handle;
    mock_i2c_record(mock, MOCK_OP_WRITE, address, reg, value, 1U);
    if (mock->fail_at_operation != 0U && mock->count == mock->fail_at_operation) {
        return IF_ERR;
    }
    return IF_OK;
}

static int mock_i2c_write_command(i2c_if_t *interface, uint8_t address,
                                  uint8_t command)
{
    mock_i2c_record((mock_i2c_t *)interface->handle, MOCK_OP_COMMAND,
                    address, command, 0U, 0U);
    return IF_OK;
}

static int mock_i2c_write_bytes(i2c_if_t *interface, uint8_t address,
                                const uint8_t *buffer, uint16_t length)
{
    if (!buffer || length != 2U) return IF_ERR;
    mock_i2c_record((mock_i2c_t *)interface->handle, MOCK_OP_WRITE_BYTES,
                    address, buffer[0], buffer[1], length);
    return IF_OK;
}

static int mock_i2c_is_ready(i2c_if_t *interface, uint8_t address)
{
    mock_i2c_t *mock = (mock_i2c_t *)interface->handle;
    return address < 128U && mock->ready[address] ? IF_OK : IF_ERR;
}

static void mock_i2c_reset(mock_i2c_t *mock)
{
    memset(mock, 0, sizeof(*mock));
    mock->interface.handle = mock;
    mock->interface.read_reg = mock_i2c_read_reg;
    mock->interface.write_reg = mock_i2c_write_reg;
    mock->interface.read_bytes = mock_i2c_read_bytes;
    mock->interface.write_bytes = mock_i2c_write_bytes;
    mock->interface.write_cmd = mock_i2c_write_command;
    mock->interface.is_ready = mock_i2c_is_ready;
    mock->product_id = PAC1934_PRODUCT_ID;
    mock->manufacturer_id = PAC1934_MANUF_ID;
    mock->tps_status = TPS62868_STAT_THERMAL_WARN |
                       TPS62868_STAT_HICCUP | TPS62868_STAT_UVLO;
}

static int mock_i2c_has_write(const mock_i2c_t *mock, uint8_t address,
                              uint8_t reg, uint8_t value)
{
    for (uint8_t i = 0U; i < mock->count; i++) {
        const mock_i2c_op_t *entry = &mock->log[i];
        if (entry->kind == MOCK_OP_WRITE && entry->address == address &&
            entry->reg == reg && entry->value == value) return 1;
    }
    return 0;
}

static int mock_i2c_has_command(const mock_i2c_t *mock, uint8_t command)
{
    for (uint8_t i = 0U; i < mock->count; i++) {
        if (mock->log[i].kind == MOCK_OP_COMMAND && mock->log[i].reg == command)
            return 1;
    }
    return 0;
}

static int mock_i2c_has_bytes(const mock_i2c_t *mock, uint8_t address,
                              uint8_t first, uint8_t second)
{
    for (uint8_t i = 0U; i < mock->count; i++) {
        const mock_i2c_op_t *entry = &mock->log[i];
        if (entry->kind == MOCK_OP_WRITE_BYTES && entry->address == address &&
            entry->reg == first && entry->value == second && entry->length == 2U)
            return 1;
    }
    return 0;
}

static int mock_spi_should_fail(mock_spi_t *mock)
{
    mock->operation_count++;
    return mock->fail_at_operation != 0U &&
           mock->operation_count == mock->fail_at_operation;
}

static void mock_spi_record(mock_spi_t *mock, const uint8_t *data, uint16_t length)
{
    if (mock->count >= MOCK_LOG_CAPACITY) return;
    if (length > sizeof(mock->tx[0])) length = sizeof(mock->tx[0]);
    memcpy(mock->tx[mock->count], data, length);
    mock->length[mock->count] = (uint8_t)length;
    mock->count++;
}

static int mock_spi_send(spi_if_t *interface, uint8_t *data, uint16_t length)
{
    mock_spi_t *mock = (mock_spi_t *)interface;
    mock_spi_record(mock, data, length);
    return mock_spi_should_fail(mock) ? IF_ERR : IF_OK;
}

static int mock_spi_transfer(spi_if_t *interface, uint8_t *tx,
                             uint8_t *rx, uint16_t length)
{
    mock_spi_t *mock = (mock_spi_t *)interface;
    mock_spi_record(mock, tx, length);
    if (mock_spi_should_fail(mock)) return IF_ERR;

    memset(rx, 0, length);
    uint8_t reg = tx[0] & 0x3FU;
    if (reg == AD7124_REG_STATUS && length >= 2U) {
        rx[1] = 0U; /* RDY=0，转换已完成。 */
    } else if (reg == AD7124_REG_ID && length >= 2U) {
        rx[1] = mock->id_value;
    } else if (reg == AD7124_REG_DATA && length >= 5U) {
        static const uint32_t raw[3] = {0x123456UL, 0x654321UL, 0xABCDEFUL};
        uint8_t channel = mock->data_channel++ % 3U;
        rx[1] = (uint8_t)(raw[channel] >> 16);
        rx[2] = (uint8_t)(raw[channel] >> 8);
        rx[3] = (uint8_t)raw[channel];
        rx[4] = channel;
    }
    return IF_OK;
}

static void mock_spi_reset(mock_spi_t *mock)
{
    memset(mock, 0, sizeof(*mock));
    mock->interface.transfer = mock_spi_transfer;
    mock->interface.send = mock_spi_send;
    mock->id_value = AD7124_ID_AD7124_4_REV_E;
}

static int mock_spi_has_packet(const mock_spi_t *mock,
                               const uint8_t *packet, uint8_t length)
{
    for (uint8_t i = 0U; i < mock->count; i++) {
        if (mock->length[i] == length && memcmp(mock->tx[i], packet, length) == 0)
            return 1;
    }
    return 0;
}

static void mock_pac_wait(uint32_t ms)
{
    s_pac_waited_ms += ms;
}

static void test_tps62868(void)
{
    mock_i2c_t mock;
    uint8_t addresses[3] = {0};
    uint8_t status = 0U;
    mock_i2c_reset(&mock);
    mock.ready[TPS62868_ADDR_VDD1] = 1U;
    mock.ready[TPS62868_ADDR_VDD2] = 1U;
    mock.ready[TPS62868_ADDR_VDDQ] = 1U;

    mock_check(tps62868_scan(&mock.interface, addresses, 3U) == 3,
               "TPS地址扫描数量");
    mock_check(addresses[0] == 0x41U && addresses[1] == 0x49U && addresses[2] == 0x4FU,
               "TPS地址扫描顺序");

    tps62868_t vdd1 = {.bus = &mock.interface, .addr = 0x49U,
                       .factor_x10 = TPS62868_FACTOR_2X0};
    tps62868_t vdd2 = {.bus = &mock.interface, .addr = 0x41U,
                       .factor_x10 = TPS62868_FACTOR_1X0};
    tps62868_t vddq = {.bus = &mock.interface, .addr = 0x4FU,
                       .factor_x10 = TPS62868_FACTOR_1X0};
    mock_check(tps62868_set_vout1(&vdd1, 1800U) == IF_OK &&
               mock_i2c_has_write(&mock, 0x49U, 0x01U, 0x64U), "TPS VDD1编码");
    mock_check(tps62868_set_vout1(&vdd2, 1100U) == IF_OK &&
               mock_i2c_has_write(&mock, 0x41U, 0x01U, 0x8CU), "TPS VDD2编码");
    mock_check(tps62868_set_vout1(&vddq, 600U) == IF_OK &&
               mock_i2c_has_write(&mock, 0x4FU, 0x01U, 0x28U), "TPS VDDQ编码");
    mock_check(tps62868_set_vout1(&vdd1, 1805U) == IF_ERR,
               "TPS拒绝不可表示电压");
    mock_check(tps62868_set_ramp(&vdd1, TPS62868_RAMP_20MV_US) == IF_OK &&
               mock_i2c_has_write(&mock, 0x49U, 0x03U, 0x6CU), "TPS CONTROL镜像");
    mock_check(tps62868_read_status(&vdd1, &status) == IF_OK && status == 0x19U,
               "TPS STATUS读取");
}

static void test_mcp4725(void)
{
    static const uint8_t expected[BOOT_MODE_COUNT][2] = {
        {0x00U, 0x00U}, /* USB/Maskrom: 0mV, code=0 */
        {0x01U, 0x74U}, /* SD:          300mV, code=372 */
        {0x02U, 0xE9U}, /* eMMC:        600mV, code=745 */
        {0x04U, 0x5DU}, /* FSPI M0:     900mV, code=1117 */
        {0x05U, 0xD1U}, /* FSPI M1:    1200mV, code=1489 */
        {0x07U, 0x45U}, /* FSPI M2:    1500mV, code=1861 */
        {0x08U, 0xBAU}, /* eMMC-SD:    1800mV, code=2234 */
    };
    mock_i2c_t mock;
    uint16_t code = 0U;
    mock_i2c_reset(&mock);
    mcp4725_t dac = {.bus = &mock.interface, .address = MCP4725_ADDR_A0_LOW,
                     .vdd_mv = 3300U};

    for (uint8_t mode = 0U; mode < BOOT_MODE_COUNT; mode++) {
        mock_check(mcp4725_set_boot_mode(&dac, (boot_mode_t)mode) == IF_OK &&
                   mock_i2c_has_bytes(&mock, MCP4725_ADDR_A0_LOW,
                                       expected[mode][0], expected[mode][1]),
                   "MCP4725 Boot模式与Fast Write编码");
    }
    mock_check(mcp4725_mv_to_code(1800U, 3300U, &code) == IF_OK && code == 2234U,
               "MCP4725按实测VDD换算DAC码");
    mock_check(mcp4725_write_fast(&dac, code, MCP4725_POWER_DOWN_100K) == IF_OK &&
               mock_i2c_has_bytes(&mock, MCP4725_ADDR_A0_LOW, 0x28U, 0xBAU),
               "MCP4725 100K掉电模式编码");
    mock_check(mcp4725_set_boot_mode(&dac, BOOT_MODE_COUNT) == IF_ERR,
               "MCP4725拒绝非法Boot模式");
    dac.bus->write_bytes = NULL;
    mock_check(mcp4725_set_boot_mode(&dac, BOOT_MODE_EMMC) == IF_ERR,
               "MCP4725传播I2C发送能力缺失");
}

static void test_pac1934(void)
{
    mock_i2c_t mock;
    pac1934_chan_t channel[4];
    pac1934_measurement_t measurement[4];
    mock_i2c_reset(&mock);
    s_pac_waited_ms = 0U;
    mock_check(PAC1934_ADDR_B1 == 0x16U,
               "PAC器件层使用未左移的7位地址0x16");

    pac1934_t pac = {.bus = &mock.interface, .addr = PAC1934_ADDR_B1,
                     .rsense_uohm = 10000U, .wait_ms = mock_pac_wait};
    mock_check(pac1934_init(&pac, 0x07U, 0U) == IF_OK, "PAC初始化");
    mock_check(mock_i2c_has_write(&mock, PAC1934_ADDR_B1, 0x1CU, 0x80U),
               "PAC CH4关闭位位于bit4");
    mock_check(mock_i2c_has_command(&mock, PAC1934_REG_REFRESH), "PAC REFRESH命令");
    mock_check(pac1934_read_all(&pac, channel) == IF_OK, "PAC三通道读取");
    mock_check(s_pac_waited_ms >= 1U && mock.read_before_wait == 0U,
               "PAC REFRESH_V后等待1ms");
    mock_check(channel[0].vbus == 0x1000U && channel[2].vpower == 0x03000000UL &&
               channel[3].vbus == 0U && channel[3].vsense == 0U,
               "PAC通道映射且不读取CH4");
    s_pac_waited_ms = 0U;
    mock_check(pac1934_measure_all(&pac, measurement) == IF_OK &&
               measurement[0].vbus_mv == 2000U &&
               measurement[0].current_ma == 312U &&
               measurement[0].power_mw == 20000U &&
               measurement[3].current_ma == 0U,
               "PAC三轨IDD工程量换算");
    mock_check(pac1934_vbus_mv(0x8000U) == 16000U &&
               pac1934_current_ma(0x8000U, 10000U) == 5000U &&
               pac1934_power_mw(0x08000000UL, 10000U) == 160000U,
               "PAC整数换算公式");

    mock.product_id = 0U;
    mock_check(pac1934_init(&pac, 0x07U, 0U) == IF_ERR, "PAC拒绝错误产品ID");
}

static void test_ad7124(void)
{
    static const uint8_t reset_packet[8] = {0xFF, 0xFF, 0xFF, 0xFF,
                                             0xFF, 0xFF, 0xFF, 0xFF};
    static const uint8_t adc_ctrl_packet[3] = {0x01, 0x05, 0x80};
    static const uint8_t channel0_packet[3] = {0x09, 0x80, 0x01};
    static const uint8_t channel1_packet[3] = {0x0A, 0x90, 0x43};
    static const uint8_t channel2_packet[3] = {0x0B, 0xA0, 0x85};
    static const uint8_t config0_packet[3] = {0x19, 0x00, 0x50};
    mock_spi_t mock;
    mock_spi_reset(&mock);

    ad7124_t adc = {.spi = &mock.interface, .cs_port = GPIOB,
                    .cs_pin = GPIO_PIN_12, .wait_ms = mock_pac_wait};
    mock_check(ad7124_init(&adc) == IF_OK, "AD7124初始化");
    mock_check(mock_spi_has_packet(&mock, reset_packet, sizeof(reset_packet)),
               "AD7124 64个1复位");
    mock_check(mock_spi_has_packet(&mock, adc_ctrl_packet, sizeof(adc_ctrl_packet)),
               "AD7124 ADC_CTRL编码");
    mock_check(mock_spi_has_packet(&mock, channel0_packet, sizeof(channel0_packet)) &&
               mock_spi_has_packet(&mock, channel1_packet, sizeof(channel1_packet)) &&
               mock_spi_has_packet(&mock, channel2_packet, sizeof(channel2_packet)),
               "AD7124原理图三路差分通道映射");
    mock_check(mock_spi_has_packet(&mock, config0_packet, sizeof(config0_packet)),
               "AD7124内部基准且接地负端不开缓冲");

    int32_t raw[3] = {0};
    uint8_t got = 0U;
    mock.data_channel = 0U;
    mock_check(ad7124_read_all(&adc, raw, 3U, &got) == IF_OK && got == 3U &&
               raw[0] == 0x123456 && raw[1] == 0x654321 && raw[2] == 0xABCDEF,
               "AD7124 DATA_STATUS三通道归位");
    int32_t full_scale_mv = ad7124_raw_to_mv_div(0xFFFFFF, 2.5f, 0U, 0U,
                                                  AD7124_DIV_100R_10K);
    mock_check(full_scale_mv >= 2524 && full_scale_mv <= 2525,
               "AD7124 1.01分压换算");

    mock_spi_reset(&mock);
    adc.spi = &mock.interface;
    mock.id_value = 0x00U;
    mock_check(ad7124_init(&adc) == IF_ERR, "AD7124拒绝浮空MISO的0x00 ID");

    mock_spi_reset(&mock);
    adc.spi = &mock.interface;
    mock.fail_at_operation = 3U; /* 复位和读ID成功，ADC_CTRL第一次写入失败。 */
    mock_check(ad7124_init(&adc) == IF_ERR, "AD7124传播寄存器写失败");
}

static void test_memory_monitor(void)
{
    i2c_if_t saved_i2c1 = i2c1_if;
    i2c_if_t saved_i2c2 = i2c2_if;
    i2c_if_t saved_i2c4 = i2c4_if;
    mock_i2c_t i2c_mock;
    mock_spi_t spi_mock;
    memory_monitor_t monitor;
    memory_monitor_sample_t sample;

    mock_i2c_reset(&i2c_mock);
    mock_spi_reset(&spi_mock);
    i2c1_if = i2c_mock.interface;
    i2c2_if = i2c_mock.interface;
    i2c4_if = i2c_mock.interface;
    s_pac_waited_ms = 0U;

    memory_monitor_configure(&monitor, &i2c4_if, &spi_mock.interface,
                             GPIOB, GPIO_PIN_12, mock_pac_wait);
    mock_check(memory_monitor_init(&monitor) == IF_OK,
               "监测层初始化PAC与AD");
    mock_check(memory_monitor_sample(&monitor, &sample) == IF_OK &&
               sample.valid_mask == MEMORY_MONITOR_VALID_ALL &&
               sample.idd[0].current_ma == 312U,
               "监测层生成三路结构化快照");
    memory_monitor_invalidate(&monitor);
    mock_check(memory_monitor_sample(&monitor, &sample) == IF_ERR &&
               sample.valid_mask == MEMORY_MONITOR_VALID_TPS_STATUS,
               "监测层下电后拒绝AD/PAC旧就绪态");

    mock_spi_reset(&spi_mock);
    spi_mock.id_value = 0x00U;
    memory_monitor_configure(&monitor, &i2c4_if, &spi_mock.interface,
                             GPIOB, GPIO_PIN_12, mock_pac_wait);
    mock_check(memory_monitor_init(&monitor) == IF_ERR &&
               memory_monitor_sample(&monitor, &sample) == IF_ERR &&
               sample.valid_mask == (MEMORY_MONITOR_VALID_IDD |
                                     MEMORY_MONITOR_VALID_TPS_STATUS),
               "AD失败时仍保留PAC与TPS有效快照");

    i2c1_if = saved_i2c1;
    i2c2_if = saved_i2c2;
    i2c4_if = saved_i2c4;
}

static void test_lpddr_power_idd_bias_flow(void)
{
    static const pwr_ctrl_config_t config = {
        .rail_order = PWR_CTRL_RAIL_ORDER_4V0_FIRST,
        .first_to_second_delay_ms = 10U,
        .second_to_ddr_delay_ms = 10U,
        .ddr_enable_to_config_delay_ms = 1U,
        .ddr_to_system_off_delay_ms = 10U,
        .system_rail_off_spacing_ms = 10U,
    };
    i2c_if_t saved_i2c1 = i2c1_if;
    i2c_if_t saved_i2c2 = i2c2_if;
    i2c_if_t saved_i2c4 = i2c4_if;
    mock_i2c_t mock;
    uint16_t target[DDR_RAIL_COUNT];
    const int16_t normal_steps[DDR_RAIL_COUNT] = {2, -3, 5};
    const int16_t lp4x_min_steps[DDR_RAIL_COUNT] = {-50, -35, -10};
    const int16_t lp4x_max_steps[DDR_RAIL_COUNT] = {77, 28, 53};
    const int16_t vdd1_too_high[DDR_RAIL_COUNT] = {78, 0, 0};
    pac1934_measurement_t idd[4];

    mock_i2c_reset(&mock);
    i2c1_if = mock.interface;
    i2c2_if = mock.interface;
    i2c4_if = mock.interface;

    pwr_ctrl_init(&config);
    mock_check(pwr_ctrl_select_memory_type(DDR_MEMORY_LPDDR4X) == PWR_CTRL_OK,
               "LPDDR4X类型在OFF状态选择");
    mock_check(pwr_ctrl_request_on(0U) == PWR_CTRL_OK &&
               bsp_gpio_get(GPIO_FUNC_PWR_4V0_EN) == 1U &&
               bsp_gpio_get(GPIO_FUNC_PWR_3V3_EN) == 0U,
               "LPDDR上电先启4V0");
    pwr_ctrl_process(10U);
    mock_check(bsp_gpio_get(GPIO_FUNC_PWR_3V3_EN) == 1U,
               "LPDDR上电再启3V3");
    pwr_ctrl_process(20U);
    mock_check(bsp_gpio_get(GPIO_FUNC_PWR_DDR_EN) == 1U,
               "LPDDR上电最后启DDR三轨");
    pwr_ctrl_process(21U);
    mock_check(pwr_ctrl_get_state() == PWR_CTRL_STATE_ON &&
               mock_i2c_has_write(&mock, TPS62868_ADDR_VDD1,
                                   TPS62868_REG_VOUT1, 0x64U) &&
               mock_i2c_has_write(&mock, TPS62868_ADDR_VDD2,
                                   TPS62868_REG_VOUT1, 0x8CU) &&
               mock_i2c_has_write(&mock, TPS62868_ADDR_VDDQ,
                                   TPS62868_REG_VOUT1, 0x28U),
               "LPDDR4X按1.8/1.1/0.6V标称初始化");

    mock_check(ddr_power_build_step_targets(DDR_MEMORY_LPDDR4X,
                                            normal_steps, target) == IF_OK &&
               target[0] == 1840U && target[1] == 1040U && target[2] == 700U,
               "LPDDR4X三路独立20mV步进目标");
    mock_check(ddr_power_build_step_targets(DDR_MEMORY_LPDDR4X,
                                            lp4x_min_steps, target) == IF_OK &&
               target[0] == 800U && target[1] == 400U && target[2] == 400U,
               "LPDDR4X全三路20mV步进下边界");
    mock_check(ddr_power_build_step_targets(DDR_MEMORY_LPDDR4X,
                                            lp4x_max_steps, target) == IF_OK &&
               target[0] == 3340U && target[1] == 1660U && target[2] == 1660U,
               "LPDDR4X全三路20mV步进上边界");
    mock_check(ddr_power_build_step_targets(DDR_MEMORY_LPDDR4X,
                                            vdd1_too_high, target) == IF_ERR,
               "20mV步进超出TPS可写范围时拒绝");
    mock_check(pwr_ctrl_apply_ddr_steps(normal_steps) == PWR_CTRL_OK &&
               ddr_power_get_mv(DDR_RAIL_VDD1) == 1840U &&
               ddr_power_get_mv(DDR_RAIL_VDD2) == 1040U &&
               ddr_power_get_mv(DDR_RAIL_VDDQ) == 700U,
               "LPDDR上电完成后允许独立步进拉偏");

    s_pac_waited_ms = 0U;
    pac1934_t pac = {.bus = &i2c4_if, .addr = PAC1934_ADDR_B1,
                     .rsense_uohm = 10000U, .wait_ms = mock_pac_wait};
    mock_check(pac1934_init(&pac, 0x07U, 0U) == IF_OK &&
               pac1934_measure_all(&pac, idd) == IF_OK &&
               idd[0].current_ma == 312U && idd[1].current_ma == 625U &&
               idd[2].current_ma == 937U,
               "假设RK已进入IDD条件，Mock采集三轨电流（未验证握手）");

    mock_check(pwr_ctrl_request_off(30U) == PWR_CTRL_OK &&
               bsp_gpio_get(GPIO_FUNC_PWR_DDR_EN) == 0U,
               "假设RK已确认安全下电，Mock先关闭DDR三轨（未验证握手）");
    pwr_ctrl_process(40U);
    mock_check(bsp_gpio_get(GPIO_FUNC_PWR_3V3_EN) == 0U,
               "LPDDR下电再关闭3V3");
    pwr_ctrl_process(50U);
    mock_check(pwr_ctrl_get_state() == PWR_CTRL_STATE_OFF &&
               bsp_gpio_get(GPIO_FUNC_PWR_4V0_EN) == 0U,
               "LPDDR下电最后关闭4V0");

    /* 拉偏后重新上电必须回标称，同时覆盖LPDDR4的1.1V VDDQ配置。 */
    mock.count = 0U;
    mock_check(pwr_ctrl_select_memory_type(DDR_MEMORY_LPDDR4) == PWR_CTRL_OK,
               "LPDDR4类型在再次上电前选择");
    mock_check(pwr_ctrl_request_on(60U) == PWR_CTRL_OK,
               "LPDDR重新上电请求");
    pwr_ctrl_process(70U);
    pwr_ctrl_process(80U);
    pwr_ctrl_process(81U);
    mock_check(pwr_ctrl_get_state() == PWR_CTRL_STATE_ON &&
               ddr_power_get_mv(DDR_RAIL_VDD1) == 1800U &&
               ddr_power_get_mv(DDR_RAIL_VDD2) == 1100U &&
               ddr_power_get_mv(DDR_RAIL_VDDQ) == 1100U &&
               mock_i2c_has_write(&mock, TPS62868_ADDR_VDDQ,
                                   TPS62868_REG_VOUT1, 0x8CU),
               "拉偏不跨电源周期且LPDDR4 VDDQ恢复1.1V");

    (void)pwr_ctrl_request_off(90U);
    pwr_ctrl_process(100U);
    pwr_ctrl_process(110U);

    /* 模拟第二路I2C写失败：第一路已改压时必须回滚，随后三路使能全关。 */
    mock_check(pwr_ctrl_select_memory_type(DDR_MEMORY_LPDDR4X) == PWR_CTRL_OK &&
               pwr_ctrl_request_on(120U) == PWR_CTRL_OK,
               "LPDDR故障注入前完成第三次上电请求");
    pwr_ctrl_process(130U);
    pwr_ctrl_process(140U);
    pwr_ctrl_process(141U);
    mock.count = 0U;
    i2c2_if.write_reg = NULL;
    mock_check(pwr_ctrl_apply_ddr_steps(normal_steps) == PWR_CTRL_ERR_FAULT &&
               pwr_ctrl_get_state() == PWR_CTRL_STATE_FAULT &&
               bsp_gpio_get(GPIO_FUNC_PWR_DDR_EN) == 0U &&
               bsp_gpio_get(GPIO_FUNC_PWR_3V3_EN) == 0U &&
               bsp_gpio_get(GPIO_FUNC_PWR_4V0_EN) == 0U &&
               mock_i2c_has_write(&mock, TPS62868_ADDR_VDD1,
                                   TPS62868_REG_VOUT1, 0x68U) &&
               mock_i2c_has_write(&mock, TPS62868_ADDR_VDD1,
                                   TPS62868_REG_VOUT1, 0x64U),
               "拉偏中途失败回滚已改轨并进入全关故障态");
    pwr_ctrl_clear_fault();

    i2c1_if = saved_i2c1;
    i2c2_if = saved_i2c2;
    i2c4_if = saved_i2c4;
}

/*
 * 仅在隔离Mock镜像运行：零延时为推进状态机的虚拟参数，不是B1时序推荐。
 * 验证正式C实现的目标快照、非OFF拒绝、运行拉偏隔离及9个I2C故障点。
 */
static void test_lpddr_training_startup(void)
{
    const pwr_ctrl_config_t config = { .rail_order = PWR_CTRL_RAIL_ORDER_4V0_FIRST };
    const uint16_t target[DDR_RAIL_COUNT] = {1820U, 1080U, 620U};
    const uint16_t invalid[DDR_RAIL_COUNT] = {1805U, 1100U, 600U};
    const uint16_t overflow[DDR_RAIL_COUNT] = {3360U, 1100U, 600U};
    const int16_t bias[DDR_RAIL_COUNT] = {2, 1, -1};
    uint16_t borrowed[DDR_RAIL_COUNT];
    i2c_if_t saved_i2c1 = i2c1_if;
    i2c_if_t saved_i2c2 = i2c2_if;
    i2c_if_t saved_i2c4 = i2c4_if;
    mock_i2c_t mock;

    mock_i2c_reset(&mock);
    i2c1_if = mock.interface;
    i2c2_if = mock.interface;
    i2c4_if = mock.interface;
    pwr_ctrl_init(NULL);
    mock_check(pwr_ctrl_request_training_on(DDR_MEMORY_LPDDR4X, target, 0U) ==
                   PWR_CTRL_ERR_NOT_CONFIGURED && mock.count == 0U,
               "自定义训练目标不能绕过未配置时序门禁");
    pwr_ctrl_init(&config);
    mock_check(pwr_ctrl_request_training_on((ddr_memory_type_t)-1, target, 0U) == PWR_CTRL_ERR_PARAM &&
               pwr_ctrl_request_training_on((ddr_memory_type_t)2, target, 0U) == PWR_CTRL_ERR_PARAM &&
               pwr_ctrl_request_training_on(DDR_MEMORY_LPDDR4X, NULL, 0U) == PWR_CTRL_ERR_PARAM &&
               pwr_ctrl_request_training_on(DDR_MEMORY_LPDDR4X, invalid, 0U) == PWR_CTRL_ERR_PARAM &&
               pwr_ctrl_request_training_on(DDR_MEMORY_LPDDR4X, overflow, 0U) == PWR_CTRL_ERR_PARAM &&
               pwr_ctrl_get_state() == PWR_CTRL_STATE_OFF && mock.count == 0U &&
               bsp_gpio_get(GPIO_FUNC_PWR_4V0_EN) == 0U,
               "训练请求拒绝非法类型/空参数/步进/越界且不打开电源");

    memcpy(borrowed, target, sizeof(borrowed));
    mock_check(pwr_ctrl_request_training_on(DDR_MEMORY_LPDDR4X, borrowed, 0U) == PWR_CTRL_OK &&
               mock.count == 0U, "训练目标在开电前复制，TPS未就绪时不写总线");
    memset(borrowed, 0, sizeof(borrowed));
    mock_check(pwr_ctrl_request_training_on(DDR_MEMORY_LPDDR4, target, 0U) == PWR_CTRL_ERR_BUSY,
               "上电过渡态不允许替换训练目标");
    for (uint8_t step = 0U; step < 3U; step++) pwr_ctrl_process(0U);
    mock_check(pwr_ctrl_get_state() == PWR_CTRL_STATE_ON && mock.count == 9U &&
               mock_i2c_has_write(&mock, TPS62868_ADDR_VDD1, TPS62868_REG_VOUT1, 0x66U) &&
               mock_i2c_has_write(&mock, TPS62868_ADDR_VDD2, TPS62868_REG_VOUT1, 0x88U) &&
               mock_i2c_has_write(&mock, TPS62868_ADDR_VDDQ, TPS62868_REG_VOUT1, 0x2CU) &&
               !mock_i2c_has_write(&mock, TPS62868_ADDR_VDD1, TPS62868_REG_VOUT1, 0x64U),
               "直接写1.82/1.08/0.62V，不先写标称，也不借用已释放请求缓冲");
    mock_check(pwr_ctrl_request_training_on(DDR_MEMORY_LPDDR4, target, 0U) == PWR_CTRL_ERR_BUSY &&
               mock.count == 9U && ddr_power_get_memory_type() == DDR_MEMORY_LPDDR4X,
               "ON时拒绝新训练请求，不转成实时调压");
    mock_check(pwr_ctrl_apply_ddr_steps(bias) == PWR_CTRL_OK &&
               ddr_power_get_mv(DDR_RAIL_VDD1) == 1840U, "仍可独立验证运行期拉偏");
    (void)pwr_ctrl_request_off(0U);
    pwr_ctrl_process(0U);
    pwr_ctrl_process(0U);
    (void)pwr_ctrl_request_on(0U);
    for (uint8_t step = 0U; step < 3U; step++) pwr_ctrl_process(0U);
    mock_check(ddr_power_get_mv(DDR_RAIL_VDD1) == target[0] &&
               ddr_power_get_mv(DDR_RAIL_VDD2) == target[1] &&
               ddr_power_get_mv(DDR_RAIL_VDDQ) == target[2],
               "再次冷启动恢复选定训练值，运行拉偏不污染训练目标");

    for (uint8_t failure = 1U; failure <= 9U; failure++) {
        pwr_ctrl_init(&config);
        (void)pwr_ctrl_select_memory_type(DDR_MEMORY_LPDDR4X);
        mock_i2c_reset(&mock);
        mock.fail_at_operation = failure;
        (void)pwr_ctrl_request_training_on(DDR_MEMORY_LPDDR4X, target, 0U);
        for (uint8_t step = 0U; step < 3U; step++) pwr_ctrl_process(0U);
        mock_check(pwr_ctrl_get_state() == PWR_CTRL_STATE_FAULT && mock.count == failure &&
                   bsp_gpio_get(GPIO_FUNC_PWR_4V0_EN) == 0U &&
                   bsp_gpio_get(GPIO_FUNC_PWR_3V3_EN) == 0U &&
                   bsp_gpio_get(GPIO_FUNC_PWR_DDR_EN) == 0U &&
                   ddr_power_get_mv(DDR_RAIL_VDD1) == DDR_VDD1_DEFAULT_MV &&
                   pwr_ctrl_request_training_on(DDR_MEMORY_LPDDR4X, target, 0U) == PWR_CTRL_ERR_FAULT,
                   "9个事务逐点故障：立即停止、全关、不提交成功电压缓存、不自动重试");
        pwr_ctrl_clear_fault();
        mock.count = 0U;
        mock.fail_at_operation = 0U;
        (void)pwr_ctrl_request_training_on(DDR_MEMORY_LPDDR4X, target, 0U);
        for (uint8_t step = 0U; step < 3U; step++) pwr_ctrl_process(0U);
        mock_check(pwr_ctrl_get_state() == PWR_CTRL_STATE_ON && mock.count == 9U,
                   "故障解除后显式重试重新配置全部三轨");
    }

    pwr_ctrl_init(&config);
    (void)pwr_ctrl_select_memory_type(DDR_MEMORY_LPDDR4);
    mock_i2c_reset(&mock);
    (void)pwr_ctrl_request_on(0U);
    for (uint8_t step = 0U; step < 3U; step++) pwr_ctrl_process(0U);
    mock_check(ddr_power_get_mv(DDR_RAIL_VDD1) == 1800U &&
               ddr_power_get_mv(DDR_RAIL_VDDQ) == 1100U,
               "旧类型选择入口显式撤销自定义训练目标，兼容标称启动");

    pwr_ctrl_init(NULL);
    (void)pwr_ctrl_select_memory_type(DDR_MEMORY_LPDDR4X);
    i2c1_if = saved_i2c1;
    i2c2_if = saved_i2c2;
    i2c4_if = saved_i2c4;
}

static void mock_write_be16(uint8_t *target, uint16_t value)
{
    target[0] = (uint8_t)(value >> 8);
    target[1] = (uint8_t)value;
}

static void mock_write_be32(uint8_t *target, uint32_t value)
{
    target[0] = (uint8_t)(value >> 24);
    target[1] = (uint8_t)(value >> 16);
    target[2] = (uint8_t)(value >> 8);
    target[3] = (uint8_t)value;
}

/*
 * 直接运行正式 PC-MCU 组帧器和 cJSON，不使用 Python 模型代替 C 代码。
 * 本函数验证框架合同与恶意输入边界，业务字段另由专用结构测试覆盖。
 */
static void test_pc_protocol_and_json(void)
{
    static const char valid_json[] =
        "{\"board\":\"B1\",\"ip\":\"192.168.1.201\"}";
    static const char duplicate_json[] =
        "{\"board\":\"B1\",\"nested\":{\"id\":1,\"id\":2}}";
    static const char trailing_json[] = "{\"ok\":true}x";
    static const char array_json[] = "[1,2,3]";
    static const uint8_t invalid_utf8[] = {
        '{', '"', 'x', '"', ':', '"', 0xEDU, 0xA0U, 0x80U, '"', '}'
    };
    uint8_t packet[PC_PROTOCOL_HEADER_SIZE + sizeof(valid_json) - 1U] = {0};
    pc_sm_t stream;
    pc_protocol_frame_t frame;
    cJSON *root = NULL;
    size_t error_offset = 0U;
    pc_sm_feed_result_t feed_result = PC_SM_FEED_MORE;
    const uint32_t packet_length = (uint32_t)sizeof(packet);

    mock_write_be32(&packet[0], packet_length);
    mock_write_be16(&packet[4], PC_PROTOCOL_VERSION);
    mock_write_be16(&packet[6], PC_PROTOCOL_FLAG_JSON);
    mock_write_be16(&packet[8], 0U);
    mock_write_be16(&packet[10], 0x0304U);
    mock_write_be32(&packet[12], 0x11223344UL);
    mock_write_be16(&packet[16], 0U);
    memcpy(&packet[PC_PROTOCOL_HEADER_SIZE], valid_json,
           sizeof(valid_json) - 1U);

    pc_sm_init(&stream);
    mock_check(stream.state == PC_SM_STATE_HEADER,
               "PC状态机初始化进入HEADER");
    for (size_t i = 0U; i < sizeof(packet); i++) {
        feed_result = pc_sm_feed(&stream, packet[i]);
        if (i + 1U == PC_PROTOCOL_HEADER_SIZE) {
            mock_check(stream.state == PC_SM_STATE_BODY,
                       "PC状态机固定头收齐后进入BODY");
        }
        if (i + 1U < sizeof(packet) && feed_result != PC_SM_FEED_MORE) {
            break;
        }
    }
    mock_check(feed_result == PC_SM_FEED_FRAME_READY,
               "PC协议逐字节半包组帧");
    mock_check(stream.state == PC_SM_STATE_FRAME_READY,
               "PC状态机正文收齐后进入FRAME_READY");
    mock_check(pc_sm_get_frame(&stream, &frame) == 0 &&
               frame.length == packet_length && frame.version == 1U &&
               frame.flag == 8U && frame.service_id == 0U &&
               frame.command_id == 0x0304U &&
               frame.sequence == 0x11223344UL && frame.reserved == 0U,
               "PC协议18字节大端头解码");
    mock_check(pc_json_parse_object(frame.json, frame.json_length,
                                    &root, &error_offset) == PC_JSON_OK &&
               root != NULL &&
               cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "board")),
               "PC协议JSON对象由cJSON解析");
    cJSON_Delete(root);
    root = NULL;

    mock_check(pc_sm_feed(&stream, 0U) == PC_SM_FEED_ERROR &&
               stream.state == PC_SM_STATE_ERROR &&
               pc_sm_error(&stream) == PC_SM_ERROR_STATE,
               "完整帧处理前拒绝覆盖接收状态");

    pc_sm_reset(&stream);
    mock_check(stream.state == PC_SM_STATE_HEADER,
               "PC状态机复位后返回HEADER");
    static const uint8_t short_length[4] = {0U, 0U, 0U, 17U};
    for (size_t i = 0U; i < sizeof(short_length); i++) {
        feed_result = pc_sm_feed(&stream, short_length[i]);
    }
    mock_check(feed_result == PC_SM_FEED_ERROR &&
               pc_sm_error(&stream) == PC_SM_ERROR_LENGTH,
               "PC协议拒绝小于18字节的Length");

    pc_sm_reset(&stream);
    static const uint8_t oversized_length[4] = {0U, 0U, 0x10U, 1U};
    for (size_t i = 0U; i < sizeof(oversized_length); i++) {
        feed_result = pc_sm_feed(&stream, oversized_length[i]);
    }
    mock_check(feed_result == PC_SM_FEED_ERROR &&
               pc_sm_error(&stream) == PC_SM_ERROR_LENGTH,
               "PC协议拒绝超过4KiB的Length");

    memcpy(packet, (uint8_t[]){0U, 0U, 0U, PC_PROTOCOL_HEADER_SIZE,
                               0U, 2U}, 6U);
    pc_sm_reset(&stream);
    for (size_t i = 0U; i < 6U; i++) {
        feed_result = pc_sm_feed(&stream, packet[i]);
    }
    mock_check(feed_result == PC_SM_FEED_ERROR &&
               pc_sm_error(&stream) == PC_SM_ERROR_VERSION,
               "PC协议尽早拒绝错误Version");

    memcpy(packet, (uint8_t[]){0U, 0U, 0U, PC_PROTOCOL_HEADER_SIZE,
                               0U, 1U, 0U, 7U}, 8U);
    pc_sm_reset(&stream);
    for (size_t i = 0U; i < 8U; i++) {
        feed_result = pc_sm_feed(&stream, packet[i]);
    }
    mock_check(feed_result == PC_SM_FEED_ERROR &&
               pc_sm_error(&stream) == PC_SM_ERROR_FLAG,
               "PC协议尽早拒绝错误Flag");

    mock_check(pc_json_parse_object((const uint8_t *)duplicate_json,
                                    strlen(duplicate_json), &root,
                                    &error_offset) == PC_JSON_ERROR_DUPLICATE_KEY,
               "cJSON递归拒绝重复键");
    mock_check(pc_json_parse_object((const uint8_t *)trailing_json,
                                    strlen(trailing_json), &root,
                                    &error_offset) == PC_JSON_ERROR_TRAILING_DATA,
               "cJSON拒绝根对象后的尾随数据");
    mock_check(pc_json_parse_object((const uint8_t *)array_json,
                                    strlen(array_json), &root,
                                    &error_offset) == PC_JSON_ERROR_ROOT_TYPE,
               "cJSON要求PC包体根节点为对象");
    mock_check(pc_json_parse_object(invalid_utf8, sizeof(invalid_utf8),
                                    &root, &error_offset) == PC_JSON_ERROR_UTF8 &&
               error_offset == 6U,
               "PC协议拒绝UTF-8代理区编码");
}

/*
 * 补充状态查表的边界检查，不重复上面的Version/Flag/JSON语法用例。
 * 只调用内存解析API，不访问Flash、RK或socket。局部只放一个约4.1KiB的
 * pc_sm_t；4KiB报文按字节生成，避免同时分配第二个大数组挤占8KiB任务栈。
 */
static void test_pc_state_table_boundaries(void)
{
    static const pc_sm_state_t invalid_states[] = {
        (pc_sm_state_t)-1,
        PC_SM_STATE_COUNT,
        (pc_sm_state_t)123,
    };
    uint8_t packet[PC_PROTOCOL_HEADER_SIZE + 2U] = {0};
    pc_sm_t stream;
    pc_protocol_frame_t frame;
    pc_sm_feed_result_t feed = PC_SM_FEED_MORE;
    cJSON *root = NULL;

    mock_write_be32(packet, PC_PROTOCOL_HEADER_SIZE);
    mock_write_be16(packet + 4, PC_PROTOCOL_VERSION);
    mock_write_be16(packet + 6, PC_PROTOCOL_FLAG_JSON);
    mock_write_be16(packet + 10, PC_COMMAND_CLIENT_HEARTBEAT);
    mock_write_be32(packet + 12, 0x11223344UL);
    packet[PC_PROTOCOL_HEADER_SIZE] = '{';
    packet[PC_PROTOCOL_HEADER_SIZE + 1U] = '}';
    memset(&stream, 0xA5, sizeof(stream));
    pc_sm_init(&stream);

    for (uint32_t i = 0U; i < PC_PROTOCOL_HEADER_SIZE - 1U; i++) {
        feed = pc_sm_feed(&stream, packet[i]);
    }
    mock_check(feed == PC_SM_FEED_MORE && stream.state == PC_SM_STATE_HEADER &&
               stream.received == PC_PROTOCOL_HEADER_SIZE - 1U &&
               pc_sm_get_frame(&stream, &frame) == -1,
               "PC第17字节仍在HEADER且不能提前取帧");
    feed = pc_sm_feed(&stream, packet[PC_PROTOCOL_HEADER_SIZE - 1U]);
    mock_check(feed == PC_SM_FEED_FRAME_READY &&
               stream.state == PC_SM_STATE_FRAME_READY &&
               pc_sm_get_frame(&stream, &frame) == 0 && frame.json_length == 0U &&
               pc_json_parse_object(frame.json, frame.json_length, &root, NULL) ==
                   PC_JSON_ERROR_ARGUMENT && root == NULL,
               "PC长度18在帧层直接READY，空正文由JSON层拒绝");

    /* 18字节后的哨兵用于发现终态handler是否错误地先写缓冲、再拒绝。 */
    mock_check(pc_sm_feed(&stream, 0xCCU) == PC_SM_FEED_ERROR &&
               stream.state == PC_SM_STATE_ERROR && pc_sm_error(&stream) == PC_SM_ERROR_STATE &&
               stream.received == PC_PROTOCOL_HEADER_SIZE &&
               memcmp(stream.buffer, packet, PC_PROTOCOL_HEADER_SIZE) == 0 &&
               stream.buffer[PC_PROTOCOL_HEADER_SIZE] == 0xA5U,
               "PC READY拒收时不覆盖已收帧或增加received");
    mock_check(pc_sm_feed(&stream, 0xDDU) == PC_SM_FEED_ERROR &&
               pc_sm_error(&stream) == PC_SM_ERROR_STATE &&
               stream.received == PC_PROTOCOL_HEADER_SIZE &&
               memcmp(stream.buffer, packet, PC_PROTOCOL_HEADER_SIZE) == 0 &&
               stream.buffer[PC_PROTOCOL_HEADER_SIZE] == 0xA5U,
               "PC ERROR再次拒收时仍保留原缓冲和received");

    /* COUNT只是表的边界，不是合法状态；负值转换后也不得用来索引函数表。 */
    for (size_t i = 0U; i < sizeof(invalid_states) / sizeof(invalid_states[0]); i++) {
        pc_sm_reset(&stream);
        stream.buffer[0] = 0xA5U;
        stream.state = invalid_states[i];
        mock_check(pc_sm_feed(&stream, 0xCCU) == PC_SM_FEED_ERROR &&
                   stream.state == PC_SM_STATE_ERROR &&
                   pc_sm_error(&stream) == PC_SM_ERROR_STATE &&
                   stream.received == 0U && stream.buffer[0] == 0xA5U,
                   "PC负状态/COUNT/越界状态在查表前安全拒绝");
    }

    /* 模拟前连接只收到半个头便断开；init必须清掉计数，不能拼进新连接。 */
    pc_sm_reset(&stream);
    mock_write_be32(packet, sizeof(packet));
    for (uint32_t i = 0U; i < 7U; i++) (void)pc_sm_feed(&stream, packet[i]);
    pc_sm_init(&stream);
    int reset_ok = stream.state == PC_SM_STATE_HEADER && stream.received == 0U &&
                   stream.expected_length == 0U && pc_sm_error(&stream) == PC_SM_ERROR_NONE &&
                   !pc_sm_is_receiving(&stream) && pc_sm_frame_data(&stream) == NULL;
    mock_write_be32(packet + 12, 0x55667788UL);
    for (uint32_t i = 0U; i < sizeof(packet); i++) feed = pc_sm_feed(&stream, packet[i]);
    mock_check(reset_ok && feed == PC_SM_FEED_FRAME_READY &&
               pc_sm_get_frame(&stream, &frame) == 0 && frame.sequence == 0x55667788UL &&
               frame.json_length == 2U && memcmp(frame.json, "{}", 2U) == 0,
               "PC新连接初始化丢弃旧半帧，完整解析新连接报文");

    /* 同一连接处理完一帧后reset，再处理不同Length/命令/Seq的下一帧。 */
    pc_sm_reset(&stream);
    mock_write_be32(packet, PC_PROTOCOL_HEADER_SIZE);
    mock_write_be16(packet + 10, PC_COMMAND_QUERY_TESTER_INFO);
    mock_write_be32(packet + 12, 0x99AABBCCUL);
    for (uint32_t i = 0U; i < PC_PROTOCOL_HEADER_SIZE; i++) {
        feed = pc_sm_feed(&stream, packet[i]);
    }
    mock_check(feed == PC_SM_FEED_FRAME_READY && pc_sm_get_frame(&stream, &frame) == 0 &&
               frame.command_id == PC_COMMAND_QUERY_TESTER_INFO &&
               frame.sequence == 0x99AABBCCUL && frame.json_length == 0U,
               "PC连续第二帧不继承前帧长度或Seq");

    pc_sm_reset(&stream);
    mock_write_be32(packet, PC_PROTOCOL_MAX_PDU);
    int prefix_ok = 1;
    for (uint32_t i = 0U; i < PC_PROTOCOL_MAX_PDU - 1U; i++) {
        uint8_t byte = i < PC_PROTOCOL_HEADER_SIZE ? packet[i] :
                       (i == PC_PROTOCOL_HEADER_SIZE ? '{' : ' ');
        if (pc_sm_feed(&stream, byte) != PC_SM_FEED_MORE) prefix_ok = 0;
    }
    prefix_ok = prefix_ok && stream.state == PC_SM_STATE_BODY &&
                stream.received == PC_PROTOCOL_MAX_PDU - 1U;
    feed = pc_sm_feed(&stream, '}');
    mock_check(prefix_ok && feed == PC_SM_FEED_FRAME_READY &&
               pc_sm_get_frame(&stream, &frame) == 0 && frame.length == PC_PROTOCOL_MAX_PDU &&
               frame.json_length == PC_PROTOCOL_MAX_JSON_SIZE && frame.json[0] == '{' &&
               frame.json[PC_PROTOCOL_MAX_JSON_SIZE - 1U] == '}',
               "PC满4KiB帧仅在第4096字节进入READY，正文4078字节");

    feed = pc_sm_feed(&stream, 0xCCU);
    int buffer_unchanged = 1;
    for (uint32_t i = 0U; i < PC_PROTOCOL_MAX_PDU; i++) {
        uint8_t expected = i < PC_PROTOCOL_HEADER_SIZE ? packet[i] :
                           (i == PC_PROTOCOL_HEADER_SIZE ? '{' :
                            (i == PC_PROTOCOL_MAX_PDU - 1U ? '}' : ' '));
        if (stream.buffer[i] != expected) buffer_unchanged = 0;
    }
    mock_check(feed == PC_SM_FEED_ERROR && pc_sm_error(&stream) == PC_SM_ERROR_STATE &&
               stream.received == PC_PROTOCOL_MAX_PDU && buffer_unchanged,
               "PC满帧后拒收额外字节，4KiB缓冲逐字节保持不变");

    /* 防御损坏的内部计数：即便误处于BODY，缓冲已满也不能继续写入。 */
    stream.state = PC_SM_STATE_BODY;
    stream.error = PC_SM_ERROR_NONE;
    mock_check(pc_sm_feed(&stream, 0xDDU) == PC_SM_FEED_ERROR &&
               pc_sm_error(&stream) == PC_SM_ERROR_STATE &&
               stream.received == PC_PROTOCOL_MAX_PDU &&
               stream.buffer[PC_PROTOCOL_MAX_PDU - 1U] == '}',
               "PC BODY遇到已满缓冲时拒绝越界写入");
}

static void test_pc_test_json_contract(void)
{
    static const char request_json[] =
        "{\"algorithmName\":\"algorithmName\",\"algorithmIndex\":1}";
    static const char trailing_comma_json[] =
        "{\"algorithmName\":\"algorithmName\",\"algorithmIndex\":1,}";
    static const pc_chip_test_result_t results[] = {
        {.site_id = "", .mirror_index = 1U, .test_status = 1, .result_desc = ""},
        {.site_id = "", .mirror_index = 1U, .test_status = 1, .result_desc = "读写校验通过"},
        {.site_id = "", .mirror_index = 1U, .test_status = 1, .result_desc = NULL},
    };
    static const char expected_result_json[] =
        "{\"algorithmName\":\"algorithmName\",\"algorithmIndex\":1,"
        "\"testResults\":[{\"siteId\":\"\",\"mirrorIndex\":1,\"resultDesc\":\"\",\"testStatus\":1},"
        "{\"siteId\":\"\",\"mirrorIndex\":1,\"resultDesc\":\"读写校验通过\",\"testStatus\":1},"
        "{\"siteId\":\"\",\"mirrorIndex\":1,\"resultDesc\":\"\",\"testStatus\":1}]}";
    cJSON *root = NULL;
    pc_test_start_view_t request;
    /* cJSON文档要求PrintPreallocated容量比实际JSON至少多5字节。 */
    char result_json[sizeof(expected_result_json) + 5U] = {0};
    uint8_t response_packet[PC_PROTOCOL_HEADER_SIZE +
                            sizeof(expected_result_json) - 1U];
    uint32_t response_length = 0U;

    mock_check(pc_json_parse_object((const uint8_t *)request_json,
                                    strlen(request_json), &root, NULL) ==
                   PC_JSON_OK &&
               pc_test_start_parse(root, &request) == PC_TEST_JSON_OK &&
               strcmp(request.algorithm_name, "algorithmName") == 0 &&
               request.algorithm_index == 1U,
               "START_TEST算法JSON字段解析");
    cJSON_Delete(root);
    root = NULL;

    mock_check(pc_json_parse_object((const uint8_t *)trailing_comma_json,
                                    strlen(trailing_comma_json), &root, NULL) ==
                   PC_JSON_ERROR_SYNTAX,
               "算法JSON严格拒绝尾随逗号");

    root = pc_test_result_create("algorithmName", 1U, results,
                                 sizeof(results) / sizeof(results[0]));
    const cJSON *array = root ? cJSON_GetObjectItemCaseSensitive(
                                    root, "testResults") : NULL;
    const cJSON *first = array ? cJSON_GetArrayItem(array, 0) : NULL;
    const cJSON *mirror = first ? cJSON_GetObjectItemCaseSensitive(
                                      first, "mirrorIndex") : NULL;
    const cJSON *status = first ? cJSON_GetObjectItemCaseSensitive(
                                      first, "testStatus") : NULL;
    mock_check(root != NULL && cJSON_IsArray(array) &&
               cJSON_GetArraySize(array) == 3 && first != NULL &&
               cJSON_IsString(cJSON_GetObjectItemCaseSensitive(first, "siteId")) &&
               cJSON_IsNumber(mirror) && mirror->valueint == 1 &&
               cJSON_IsNumber(status) && status->valueint == 1 &&
               cJSON_PrintPreallocated(root, result_json,
                                       sizeof(result_json), 0) != 0 &&
               strcmp(result_json, expected_result_json) == 0,
               "TEST_RESULT_REPORT含resultDesc：空串、中文和NULL默认值");

    const uint32_t mcu_sequence = 7U;
    pc_sm_t response_stream;
    pc_protocol_frame_t response_frame;
    pc_sm_feed_result_t response_feed = PC_SM_FEED_MORE;
    mock_check(pc_protocol_build_response(
                   PC_COMMAND_TEST_RESULT_REPORT, mcu_sequence,
                   (const uint8_t *)result_json, strlen(result_json),
                   response_packet, sizeof(response_packet),
                   &response_length) == 0,
               "PC响应组帧器接受测试结果JSON");
    pc_sm_init(&response_stream);
    for (uint32_t i = 0U; i < response_length; i++) {
        response_feed = pc_sm_feed(&response_stream, response_packet[i]);
    }
    mock_check(response_feed == PC_SM_FEED_FRAME_READY &&
               pc_sm_get_frame(&response_stream, &response_frame) == 0 &&
               response_frame.service_id == 0U &&
               response_frame.command_id == PC_COMMAND_TEST_RESULT_REPORT &&
               response_frame.sequence == mcu_sequence,
               "PC响应使用ServiceID=0和MCU独立Seq");
    cJSON_Delete(root);
}

/*
 * 直接执行正式dispatch，覆盖查表命中、未命中和拒绝路径。
 * 不发送合法的测试仪信息查询，避免Mock镜像改写W25Q256正式FRU扇区；
 * 仅检查非法查询在字段校验阶段被拒绝，真实FRU读取另由Flash/网络测试验收。
 */
static void test_pc_dispatch(void)
{
    static const uint8_t empty_json[] = {'{', '}'};
    static const struct {
        uint16_t command_id;
        const char *json;
        pc_dispatch_result_t expected;
        const char *label;
    } cases[] = {
        {PC_COMMAND_CLIENT_HEARTBEAT, "{\"state\":1}", PC_DISPATCH_RESPONSE_READY,
         "PC查表用0x0006拒绝携带字段的心跳"},
        {PC_COMMAND_QUERY_TESTER_INFO, "{\"extra\":1}", PC_DISPATCH_RESPONSE_READY,
         "PC查表用0x0006拒绝非空信息查询，不访问Flash"},
        {PC_COMMAND_SERVER_HEARTBEAT, "{}", PC_DISPATCH_RESPONSE_READY,
         "PC查表用0x0006拒绝反向MCU心跳"},
        {PC_COMMAND_TESTER_INFO_RESP, "{}", PC_DISPATCH_RESPONSE_READY,
         "PC查表用0x0006拒绝反向测试仪信息响应"},
        {PC_COMMAND_START_TEST, "{\"algorithmName\":\"demo\",\"algorithmIndex\":1}",
         PC_DISPATCH_NO_RESPONSE, "PC查表校验合法START_TEST，不提前回复"},
        {PC_COMMAND_START_TEST, "{}", PC_DISPATCH_RESPONSE_READY,
         "PC查表用0x0006拒绝START_TEST缺失必填字段"},
        {PC_COMMAND_START_TEST, "{\"algorithmName\":\"demo\",\"algorithmIndex\":1.5}",
         PC_DISPATCH_RESPONSE_READY, "PC查表用0x0006拒绝非整数algorithmIndex"},
        {PC_COMMAND_POWER_ON, "{}", PC_DISPATCH_RESPONSE_READY,
         "PC查表处理上电业务结果"},
        {PC_COMMAND_POWER_OFF, "{}", PC_DISPATCH_RESPONSE_READY,
         "PC查表处理OFF幂等下电"},
        {PC_COMMAND_RESET_RK, "{}", PC_DISPATCH_RESPONSE_READY,
         "PC查表拒绝复位已关闭RK"},
        {PC_COMMAND_SET_BOOT_MODE, "{\"bootMode\":\"emmc\"}",
         PC_DISPATCH_RESPONSE_READY, "PC查表处理Boot模式写入结果"},
        {PC_COMMAND_QUERY_BOOT_MODE, "{}", PC_DISPATCH_RESPONSE_READY,
         "PC查表处理Boot模式查询结果"},
        {PC_COMMAND_BOOT_MODE_RESP, "{}", PC_DISPATCH_RESPONSE_READY,
         "PC查表拒绝反向Boot响应"},
        {0xFFFFU, "{}", PC_DISPATCH_RESPONSE_READY,
         "PC查表未命中返回notSupported"},
        {0xFFFFU, "{\"extra\":1}", PC_DISPATCH_RESPONSE_READY,
         "PC未知命令合法对象返回notSupported"},
        {0xFFFFU, "[]", PC_DISPATCH_ERROR,
         "PC未知命令也必须使用JSON对象"},
        {PC_COMMAND_CLIENT_HEARTBEAT, "{", PC_DISPATCH_ERROR,
         "PC查表前拒绝非法JSON"},
    };
    pc_protocol_frame_t request = {
        .service_id = 0U,
        .command_id = PC_COMMAND_CLIENT_HEARTBEAT,
        .sequence = 0x55667788UL,
        .json = empty_json,
        .json_length = sizeof(empty_json),
    };
    pc_dispatch_response_t response;
    pc_sm_t response_sm;
    pc_protocol_frame_t response_frame;
    pc_sm_feed_result_t feed = PC_SM_FEED_MORE;

    mock_check(pc_dispatch_handle(&request, 9U, &response) ==
                   PC_DISPATCH_RESPONSE_READY &&
               response.length != 0U,
               "PC dispatch生成心跳响应");

    pc_sm_init(&response_sm);
    for (uint32_t i = 0U; i < response.length; i++) {
        feed = pc_sm_feed(&response_sm, response.data[i]);
    }
    mock_check(feed == PC_SM_FEED_FRAME_READY &&
               pc_sm_get_frame(&response_sm, &response_frame) == 0 &&
               response_frame.service_id == 0U &&
               response_frame.command_id == PC_COMMAND_SERVER_HEARTBEAT &&
               response_frame.sequence == 9U &&
               response_frame.json_length == sizeof(empty_json) &&
               memcmp(response_frame.json, empty_json, sizeof(empty_json)) == 0,
               "PC dispatch响应命令使用ServiceID=0和MCU独立Seq");

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
        request.command_id = cases[i].command_id;
        request.json = (const uint8_t *)cases[i].json;
        request.json_length = (uint32_t)strlen(cases[i].json);
        /* 模拟前一帧留下的有效长度，确保错误/无响应不会发送旧PDU。 */
        response.length = 123U;
        mock_check(pc_dispatch_handle(&request, 10U + (uint32_t)i,
                                      &response) == cases[i].expected &&
                   (cases[i].expected == PC_DISPATCH_RESPONSE_READY
                        ? response.length > PC_PROTOCOL_HEADER_SIZE
                        : response.length == 0U),
                   cases[i].label);
    }

    mock_check(pc_dispatch_handle(NULL, 1U, &response) == PC_DISPATCH_ERROR &&
               pc_dispatch_handle(&request, 1U, NULL) == PC_DISPATCH_ERROR,
               "PC dispatch拒绝空参数");

    /* 分发器本身不保存上次处理结果；错误帧之后再次调用仍可处理合法命令。 */
    request.command_id = PC_COMMAND_CLIENT_HEARTBEAT;
    request.json = empty_json;
    request.json_length = sizeof(empty_json);
    mock_check(pc_dispatch_handle(&request, 30U, &response) == PC_DISPATCH_RESPONSE_READY &&
               response.length == PC_PROTOCOL_HEADER_SIZE + sizeof(empty_json),
               "PC查表错误路径后再次处理合法心跳");
}

/*
 * 只测试适配入口在接触HAL前的拒绝路径，不需要外部I2C/SPI器件。
 * 新RK序号分配函数也可在此隔离镜像验证；不读取/写入Flash。
 */
static void test_interface_boundaries(void)
{
    uint8_t byte = 0U;
    spi_if_t unavailable_spi = {0};

    mock_check(i2c1_if.read_reg(NULL, 0x49U, 0U, &byte) == IF_ERR &&
               i2c1_if.read_reg(&i2c1_if, 0x49U, 0U, NULL) == IF_ERR,
               "I2C拒绝空接口和空读取缓冲");
    mock_check(i2c2_if.read_reg(&i2c2_if, 0x41U, 0U, &byte) == IF_ERR &&
               i2c2_if.write_cmd(&i2c2_if, 0x41U, 0U) == IF_ERR,
               "V2未配置I2C2安全拒绝访问");
    mock_check(i2c1_if.is_ready(&i2c1_if, 0x80U) == IF_ERR &&
               i2c1_if.write_bytes(&i2c1_if, 0x49U, NULL, 1U) == IF_ERR &&
               i2c1_if.write_bytes(&i2c1_if, 0x49U, &byte, 0U) == IF_ERR,
               "I2C拒绝超范围地址和非法写长度");
    mock_check(spi2_if.transfer(NULL, &byte, &byte, 1U) == IF_ERR &&
               spi2_if.send(&unavailable_spi, &byte, 1U) == IF_ERR,
               "SPI拒绝空接口和空硬件句柄");
    mock_check(spi2_if.transfer(&spi2_if, &byte, NULL, 1U) == IF_ERR &&
               spi2_if.send(&spi2_if, &byte, 0U) == IF_ERR,
               "SPI拒绝空接收缓冲和零长度");
    mock_check(uart_send(-1, &byte, 1U) == IF_ERR &&
               uart_send(2, &byte, 1U) == IF_ERR &&
               uart_poll_rx(-1, &byte, 1U) == 0,
               "UART路由拒绝负槽位和越界槽位");
    mock_check(uart1_if.send(NULL, &byte, 1U) == IF_ERR &&
               uart1_if.send(&uart1_if, NULL, 1U) == IF_ERR,
               "UART适配拒绝空参数且不写TX环");

    uint64_t first = rk_link_next_sequence();
    mock_check(rk_link_next_sequence() == first + 1U,
               "RK共享序号入口连续分配且不依赖心跳任务");
}

/*
 * 为packet[13..]中预先写好的TLV补齐当前线格式；允许构造CRC正确但TLV
 * 损坏的报文，避免只测打包器能够生成的合法数据。仅本测试调用，长度不
 * 超过TLV_PAYLOAD_MAX；Seq固定为01..08，CRC仍为小端、且不覆盖Seq。
 */
static uint16_t mock_rk_seal_packet(uint8_t *packet, uint16_t payload_length)
{
    packet[0] = TLV_SYNC_HEADER;
    mock_write_be16(packet + 1, payload_length);
    uint16_t crc = crc16_modbus(packet, 3U);
    packet[3] = (uint8_t)crc;
    packet[4] = (uint8_t)(crc >> 8);
    for (uint8_t i = 0U; i < 8U; i++) packet[5U + i] = i + 1U;
    crc = crc16_modbus(packet + 1, 2U);
    crc = crc16_modbus_continue(crc, packet + 13, payload_length);
    packet[13U + payload_length] = (uint8_t)crc;
    packet[14U + payload_length] = (uint8_t)(crc >> 8);
    packet[15U + payload_length] = TLV_SYNC_FOOTER;
    return 16U + payload_length;
}

/* 每次只喂一帧，保留其中最后一个非零事件；不复位，便于验证坏帧后恢复。 */
static int mock_rk_feed_packet(tlv_sm_t *stream, const uint8_t *packet,
                               uint16_t length)
{
    int event = 0;
    for (uint16_t i = 0U; i < length; i++) {
        int result = tlv_sm_feed(stream, packet[i]);
        if (result != 0) event = result;
    }
    return event;
}

/* 检查整个输出对象，包括旧Seq、单元表和参数区都被撤销，而不只检查count。 */
static int mock_rk_frame_is_clear(const tlv_frame_t *frame)
{
    const uint8_t *bytes = (const uint8_t *)frame;
    for (size_t i = 0U; i < sizeof(*frame); i++) {
        if (bytes[i] != 0U) return 0;
    }
    return 1;
}

/*
 * 直接执行正式RK解包器和状态机，全部报文在内存中构造，不访问Flash、
 * RK串口或GPIO。三个大对象合计约3.3KiB，复用同一缓冲避免挤占8KiB任务栈。
 * 这组检查仅证明解析边界，不等于RK真实业务、异步结果或硬件联调通过。
 */
static void test_rk_tlv_validation(void)
{
    static const uint8_t result_tlv[] = {0x50U, 0U, 0U, 3U, 0U, 0x29U, 1U};
    static const struct {
        uint8_t tlv[13];
        uint16_t length;
        const char *label;
    } malformed[] = {
        {{0x50U, 0U, 0U, 0U}, 4U, "RK拒绝命令容器缺失CmdID并清空输出"},
        {{0x50U, 0U, 0U, 1U, 0U}, 5U, "RK拒绝仅1字节CmdID并清空输出"},
        {{0U, 0U, 0U, 1U, 0xABU}, 5U, "RK拒绝非空心跳并清空输出"},
        {{0x50U, 0U, 0U, 3U, 0U, 1U, 0xABU,
          0x50U, 0U, 0U, 3U, 0U, 0x29U}, 13U,
         "RK后段Value截断时撤销前段命令和参数"},
    };
    static const uint16_t counted_types[] = {TLV_TYPE_CMD, 0x5002U,
                                             TLV_TYPE_HEARTBEAT};
    uint8_t packet[TLV_FRAME_MAX] = {0};
    tlv_sm_t stream;
    tlv_frame_t frame;
    uint16_t length;
    int event;

    /* 短Length在帧头阶段就拒绝；直接调用解包API也必须清除旧输出。 */
    for (uint16_t short_length = 0U; short_length < TLV_PAYLOAD_MIN; short_length++) {
        (void)mock_rk_seal_packet(packet, short_length);
        tlv_sm_init(&stream);
        event = mock_rk_feed_packet(&stream, packet, 5U);
        memset(&frame, 0xCC, sizeof(frame));
        mock_check(event == 0 && stream.state != TLV_SM_DATA &&
                   tlv_unpack(packet + 5, short_length, &frame) == -1 &&
                   mock_rk_frame_is_clear(&frame),
                   "RK拒绝0至3字节TLV区且清空输出");
    }
    memset(&frame, 0xCC, sizeof(frame));
    mock_check(tlv_unpack(packet + 5, TLV_PAYLOAD_MAX + 1U, &frame) == -1 &&
               mock_rk_frame_is_clear(&frame), "RK拒绝超长TLV区且清空输出");
    memset(&frame, 0xCC, sizeof(frame));
    mock_check(tlv_unpack(NULL, TLV_PAYLOAD_MIN, &frame) == -1 &&
               mock_rk_frame_is_clear(&frame), "RK拒绝空输入且清空输出");
    mock_check(tlv_unpack(packet + 5, TLV_PAYLOAD_MIN, NULL) == -1,
               "RK拒绝空输出指针");

    tlv_sm_init(&stream);
    for (size_t i = 0U; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        memcpy(packet + 13, malformed[i].tlv, malformed[i].length);
        length = mock_rk_seal_packet(packet, malformed[i].length);
        event = mock_rk_feed_packet(&stream, packet, length);
        memset(&frame, 0xCC, sizeof(frame));
        mock_check(event == 1 && tlv_sm_get_frame(&stream, &frame) == -1 &&
                   mock_rk_frame_is_clear(&frame), malformed[i].label);
    }
    for (uint16_t tail = 1U; tail <= 3U; tail++) {
        memcpy(packet + 13, result_tlv, sizeof(result_tlv));
        memset(packet + 13 + sizeof(result_tlv), 0, tail);
        length = mock_rk_seal_packet(packet, sizeof(result_tlv) + tail);
        event = mock_rk_feed_packet(&stream, packet, length);
        memset(&frame, 0xCC, sizeof(frame));
        mock_check(event == 1 && tlv_sm_get_frame(&stream, &frame) == -1 &&
                   mock_rk_frame_is_clear(&frame),
                   "RK拒绝尾部1至3字节残缺TLV头并撤销前缀");
    }

    /* 前面的坏TLV帧之后，不重新初始化状态机，也应能处理下一帧。 */
    memcpy(packet + 13, result_tlv, sizeof(result_tlv));
    length = mock_rk_seal_packet(packet, sizeof(result_tlv));
    event = mock_rk_feed_packet(&stream, packet, length);
    mock_check(event == 1 && tlv_sm_get_frame(&stream, &frame) == 1 &&
               frame.sequence == UINT64_C(0x0102030405060708) &&
               frame.units[0].cmd == 0x0029U && frame.units[0].data_len == 1U &&
               frame.payload[0] == 1U,
               "RK坏TLV后恢复，且允许结果CmdID 0x0029");
    packet[14] = 2U; /* Type=0x5002仍属未知Type，本次不扩展线协议。 */
    length = mock_rk_seal_packet(packet, sizeof(result_tlv));
    event = mock_rk_feed_packet(&stream, packet, length);
    mock_check(event == 1 && tlv_sm_get_frame(&stream, &frame) == 0 &&
               frame.count == 0U && frame.payload_used == 0U,
               "RK合法0x5002结构被跳过，零输出不当成错误");

    /* 数量上限按线上全部TLV计数，不只计算解包后保存的命令单元。 */
    for (size_t kind = 0U; kind < sizeof(counted_types) / sizeof(counted_types[0]); kind++) {
        uint16_t unit_length = counted_types[kind] == TLV_TYPE_CMD ? 6U : 4U;
        for (uint16_t count = 16U; count <= 17U; count++) {
            for (uint16_t i = 0U; i < count; i++) {
                uint8_t *unit = packet + 13 + i * unit_length;
                mock_write_be16(unit, counted_types[kind]);
                mock_write_be16(unit + 2, unit_length - 4U);
                if (unit_length == 6U) mock_write_be16(unit + 4, 0x0029U);
            }
            length = mock_rk_seal_packet(packet, count * unit_length);
            event = mock_rk_feed_packet(&stream, packet, length);
            memset(&frame, 0xCC, sizeof(frame));
            int result = event == 1 ? tlv_sm_get_frame(&stream, &frame) : -2;
            int expected = kind == 0U ? 16 : (kind == 1U ? 0 : 1);
            mock_check(event == 1 &&
                       (count == 16U ? result == expected :
                        result == -1 && mock_rk_frame_is_clear(&frame)),
                       count == 16U ? "RK接受16个命令/未知Type/心跳" :
                                      "RK拒绝17个命令/未知Type/心跳且清空输出");
        }
    }
    for (uint16_t i = 0U; i < 15U; i++) {
        memcpy(packet + 13 + i * 6U, result_tlv, 6U);
        packet[13U + i * 6U + 3U] = 2U;
    }
    memset(packet + 13 + 90, 0, 4U); /* 15个命令+1个空心跳，线上共16个。 */
    length = mock_rk_seal_packet(packet, 94U);
    event = mock_rk_feed_packet(&stream, packet, length);
    mock_check(event == 1 && tlv_sm_get_frame(&stream, &frame) == 15,
               "RK混合16个TLV有效，心跳不挤占命令输出");
    memcpy(packet + 13 + 94, (uint8_t[]){0x50U, 2U, 0U, 0U}, 4U);
    length = mock_rk_seal_packet(packet, 98U);
    event = mock_rk_feed_packet(&stream, packet, length);
    memset(&frame, 0xCC, sizeof(frame));
    mock_check(event == 1 && tlv_sm_get_frame(&stream, &frame) == -1 &&
               mock_rk_frame_is_clear(&frame),
               "RK命令加心跳加未知Type共17个时原子拒绝");

    memset(packet + 13, 0xA6, TLV_PAYLOAD_MAX);
    mock_write_be16(packet + 13, TLV_TYPE_CMD);
    mock_write_be16(packet + 15, TLV_PAYLOAD_MAX - 4U);
    mock_write_be16(packet + 17, 0x0029U);
    length = mock_rk_seal_packet(packet, TLV_PAYLOAD_MAX);
    event = mock_rk_feed_packet(&stream, packet, length);
    mock_check(event == 1 && tlv_sm_get_frame(&stream, &frame) == 1 &&
               length == TLV_FRAME_MAX && frame.units[0].data_len == TLV_CMD_DATA_MAX &&
               frame.payload_used == TLV_CMD_DATA_MAX &&
               memcmp(frame.payload, packet + 19, TLV_CMD_DATA_MAX) == 0,
               "RK最大1040字节帧完整解析1018字节命令参数");

    memcpy(packet + 13, result_tlv, sizeof(result_tlv));
    length = mock_rk_seal_packet(packet, sizeof(result_tlv));
    packet[length - 3U] ^= 1U; /* 仅破坏FrameCRC，帧头仍合法。 */
    mock_check(mock_rk_feed_packet(&stream, packet, length) == -1 &&
               stream.state == TLV_SM_IDLE, "RK帧CRC错误不产生可取帧事件");
    packet[length - 3U] ^= 1U;
    event = mock_rk_feed_packet(&stream, packet, length);
    mock_check(event == 1 && tlv_sm_get_frame(&stream, &frame) == 1 &&
               frame.units[0].cmd == 0x0029U,
               "RK坏CRC帧之后无需重启即可解析下一合法帧");
}

void StartDriverMockTestTask(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(500U));
    mock_uart_printf("\r\n=== TPS/PAC/AD/MCP driver mock test ===\r\n");

    test_tps62868();
    test_mcp4725();
    test_pac1934();
    test_ad7124();
    test_memory_monitor();
    test_lpddr_power_idd_bias_flow();
    test_lpddr_training_startup();
    test_pc_protocol_and_json();
    test_pc_state_table_boundaries();
    test_pc_test_json_contract();
    test_pc_dispatch();
    test_interface_boundaries();
    test_rk_tlv_validation();

    mock_uart_printf("RESULT: %lu PASS, %lu FAIL\r\n",
                     (unsigned long)s_pass_count, (unsigned long)s_fail_count);
    mock_uart_printf("=== driver mock test complete ===\r\n");
    /* 结果周期重发，避免烧录复位后串口工具打开较慢而错过唯一一行。 */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000U));
        mock_uart_printf("RESULT: %lu PASS, %lu FAIL\r\n",
                         (unsigned long)s_pass_count,
                         (unsigned long)s_fail_count);
    }
}
