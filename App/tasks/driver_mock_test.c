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
#include "ddr_power.h"
#include "mcp4725.h"
#include "pac1934.h"
#include "pwr_ctrl.h"
#include "tps62868.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MOCK_LOG_CAPACITY  128U

extern i2c_if_t i2c1_if;
extern i2c_if_t i2c2_if;
extern i2c_if_t i2c4_if;

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
    mock_i2c_record((mock_i2c_t *)interface->handle, MOCK_OP_WRITE,
                    address, reg, value, 1U);
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

    pac1934_t pac = {.bus = &mock.interface, .addr = 0x2CU,
                     .rsense_uohm = 10000U, .wait_ms = mock_pac_wait};
    mock_check(pac1934_init(&pac, 0x07U, 0U) == IF_OK, "PAC初始化");
    mock_check(mock_i2c_has_write(&mock, 0x2CU, 0x1CU, 0x80U),
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
    static const uint8_t channel0_packet[3] = {0x09, 0x80, 0x11};
    mock_spi_t mock;
    mock_spi_reset(&mock);

    ad7124_t adc = {.spi = &mock.interface, .cs_port = GPIOB,
                    .cs_pin = GPIO_PIN_12, .wait_ms = mock_pac_wait};
    mock_check(ad7124_init(&adc) == IF_OK, "AD7124初始化");
    mock_check(mock_spi_has_packet(&mock, reset_packet, sizeof(reset_packet)),
               "AD7124 64个1复位");
    mock_check(mock_spi_has_packet(&mock, adc_ctrl_packet, sizeof(adc_ctrl_packet)),
               "AD7124 ADC_CTRL编码");
    mock_check(mock_spi_has_packet(&mock, channel0_packet, sizeof(channel0_packet)),
               "AD7124 AIN0到AVSS映射");

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

    mock_check(ddr_power_build_profile(DDR_MEMORY_LPDDR4X,
                                       DDR_BIAS_PLUS_5_PERCENT,
                                       target) == IF_OK &&
               target[0] == 1890U && target[1] == 1155U && target[2] == 630U,
               "LPDDR4X加5%拉偏档位");
    mock_check(pwr_ctrl_apply_ddr_bias(DDR_BIAS_PLUS_5_PERCENT) == PWR_CTRL_OK &&
               ddr_power_get_mv(DDR_RAIL_VDD1) == 1890U &&
               ddr_power_get_mv(DDR_RAIL_VDD2) == 1155U &&
               ddr_power_get_mv(DDR_RAIL_VDDQ) == 630U,
               "LPDDR上电完成后允许拉偏");

    s_pac_waited_ms = 0U;
    pac1934_t pac = {.bus = &i2c4_if, .addr = 0x2CU,
                     .rsense_uohm = 10000U, .wait_ms = mock_pac_wait};
    mock_check(pac1934_init(&pac, 0x07U, 0U) == IF_OK &&
               pac1934_measure_all(&pac, idd) == IF_OK &&
               idd[0].current_ma == 312U && idd[1].current_ma == 625U &&
               idd[2].current_ma == 937U,
               "RK进入IDD条件后MCU采集三轨电流");

    mock_check(pwr_ctrl_request_off(30U) == PWR_CTRL_OK &&
               bsp_gpio_get(GPIO_FUNC_PWR_DDR_EN) == 0U,
               "RK完成CKE低准备后先关闭DDR三轨");
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
               ddr_power_get_bias_profile() == DDR_BIAS_NOMINAL &&
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
    mock_check(pwr_ctrl_apply_ddr_bias(DDR_BIAS_PLUS_5_PERCENT) == PWR_CTRL_ERR_FAULT &&
               pwr_ctrl_get_state() == PWR_CTRL_STATE_FAULT &&
               bsp_gpio_get(GPIO_FUNC_PWR_DDR_EN) == 0U &&
               bsp_gpio_get(GPIO_FUNC_PWR_3V3_EN) == 0U &&
               bsp_gpio_get(GPIO_FUNC_PWR_4V0_EN) == 0U &&
               mock_i2c_has_write(&mock, TPS62868_ADDR_VDD1,
                                   TPS62868_REG_VOUT1, 0x6DU) &&
               mock_i2c_has_write(&mock, TPS62868_ADDR_VDD1,
                                   TPS62868_REG_VOUT1, 0x64U),
               "拉偏中途失败回滚已改轨并进入全关故障态");
    pwr_ctrl_clear_fault();

    i2c1_if = saved_i2c1;
    i2c2_if = saved_i2c2;
    i2c4_if = saved_i2c4;
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
    test_lpddr_power_idd_bias_flow();

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
