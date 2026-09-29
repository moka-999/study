/* 直接运行正式AD7124与组合采样层；寄存器/SPI为故障注入桩，不代表模拟精度。 */
#include "ad7124.h"
#include "memory_monitor.h"
#include <math.h>
#include <string.h>

#define STR1(x) #x
#define STR(x) STR1(x)
#define CHECK(c) do { if (!(c)) fail("FAIL AD7124 line " STR(__LINE__) ": " #c "\n"); } while (0)

static uint32_t regs[0x39], before[0x39];
static uint32_t calls, fail_at, fail_from, writes, waits;
static uint8_t stream[64], stream_len, stream_pos, status_bits;
static uint8_t stuck_mode, cal_mode, cal_error, full_count, zero_count;
static GPIO_TypeDef gpio;
static int selected;
static uint32_t last_release_wait, error_reads, cal_delay, por_reads;
static uint8_t ignore_diagnostics;

/* QEMU半主机输出复用项目现有测试约定；不依赖目标板串口。 */
static void message(const char *text)
{
    register uint32_t r0 __asm__("r0") = 4U;
    register const char *r1 __asm__("r1") = text;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
}

static _Noreturn void finish(uint32_t code)
{
    const uint32_t block[] = {0x20026U, code};
    register uint32_t r0 __asm__("r0") = 0x20U;
    register const uint32_t *r1 __asm__("r1") = block;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
    for (;;) { }
}

static _Noreturn void fail(const char *text) { message(text); finish(1U); }
void test_abort(void) { fail("FAIL CPU exception\n"); }

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
    CHECK(port == &gpio && pin == 1U);
    CHECK(selected == (state == GPIO_PIN_SET));
    if (state == GPIO_PIN_RESET && last_release_wait != UINT32_MAX)
        CHECK(waits > last_release_wait);
    if (state == GPIO_PIN_SET) last_release_wait = waits;
    selected = state == GPIO_PIN_RESET;
}
void HAL_Delay(uint32_t ms) { waits += ms; }

static int io_failed(void)
{
    ++calls;
    CHECK(selected);
    return calls == fail_at || (fail_from && calls >= fail_from);
}

/* 事务失败可能已经写入寄存器，故意先更新模型再报错，验证不确定写后的恢复。 */
static int send_bytes(spi_if_t *bus, uint8_t *data, uint16_t length)
{
    (void)bus;
    int failed = io_failed();
    if (length == 8U) {
        for (unsigned i = 0U; i < 8U; ++i) CHECK(data[i] == 0xFFU);
        uint32_t id = regs[AD7124_REG_ID];
        memset(regs, 0, sizeof(regs));
        regs[AD7124_REG_ID] = id;
        regs[AD7124_REG_ERROR_EN] = AD7124_ERR_SPI_IGNORE;
        cal_mode = AD7124_MODE_CONT;
        return failed ? IF_ERR : IF_OK;
    }
    CHECK(length >= 2U && length <= 4U && !(data[0] & 0x40U));
    uint8_t reg = data[0] & 0x3FU;
    uint32_t value = 0U;
    for (uint16_t i = 1U; i < length; ++i) value = (value << 8) | data[i];
    CHECK(reg < 0x39U);
    if (!(reg == AD7124_REG_ERROR_EN && ignore_diagnostics)) regs[reg] = value;
    ++writes;
    if (reg == AD7124_REG_ADC_CTRL) {
        cal_mode = (value >> 2) & 15U;
        if (cal_mode == AD7124_MODE_CAL_INT_F || cal_mode == AD7124_MODE_CAL_INT_Z) {
            unsigned enabled = 0U;
            uint8_t setup = 0U;
            for (unsigned i = 0U; i < 16U; ++i) {
                if (regs[AD7124_REG_CH0 + i] & AD7124_CH_ENABLE) {
                    ++enabled;
                    setup = (regs[AD7124_REG_CH0 + i] >> 12) & 7U;
                }
            }
            CHECK(enabled == 1U);
            if (cal_mode == AD7124_MODE_CAL_INT_F) {
                CHECK((regs[AD7124_REG_CFG0 + setup] & 7U) != 0U);
                CHECK(((value >> 6) & 3U) < AD7124_PWR_FULL);
                CHECK(regs[AD7124_REG_OFFSET0 + setup] == 0x800000U);
                ++full_count;
            } else {
                ++zero_count;
            }
        }
    }
    return failed ? IF_ERR : IF_OK;
}

static int transfer_bytes(spi_if_t *bus, uint8_t *tx, uint8_t *rx, uint16_t length)
{
    (void)bus;
    if (io_failed()) return IF_ERR;
    uint8_t reg = tx[0] & 0x3FU;
    CHECK(reg < 0x39U && (tx[0] & 0x40U) && length >= 2U && length <= 5U);
    memset(rx, 0, length);
    uint32_t value = regs[reg];
    if (reg == AD7124_REG_STATUS) {
        value = status_bits;
        if (por_reads != 0U) { value |= AD7124_STATUS_POR_FLAG; --por_reads; }
        if (cal_mode == AD7124_MODE_CAL_INT_F || cal_mode == AD7124_MODE_CAL_INT_Z) {
            if (cal_mode == stuck_mode) value |= AD7124_STATUS_RDY;
            else if (cal_delay != 0U) { --cal_delay; value |= AD7124_STATUS_RDY; }
            else if (cal_error) value |= AD7124_STATUS_ERROR_FLAG;
            else {
                /* 模拟校准完成后更新本setup系数并自动回IDLE。 */
                for (unsigned i = 0U; i < 16U; ++i) {
                    if (regs[AD7124_REG_CH0 + i] & AD7124_CH_ENABLE) {
                        uint8_t setup = (regs[AD7124_REG_CH0 + i] >> 12) & 7U;
                        regs[(cal_mode == AD7124_MODE_CAL_INT_F ? AD7124_REG_GAIN0 :
                               AD7124_REG_OFFSET0) + setup] =
                            cal_mode == AD7124_MODE_CAL_INT_F ? 0x512345U : 0x800123U;
                    }
                }
                regs[AD7124_REG_ADC_CTRL] = (regs[AD7124_REG_ADC_CTRL] & ~0x3CU) |
                                           AD7124_ADC_MODE(AD7124_MODE_IDLE);
                cal_mode = AD7124_MODE_IDLE;
            }
        }
    } else if (reg == AD7124_REG_DATA) {
        CHECK(length == 5U && stream_len != 0U);
        uint8_t status = stream[stream_pos++ % stream_len];
        uint32_t raw = 0x100000U * (1U + (status & 15U));
        rx[1] = (uint8_t)(raw >> 16);
        rx[2] = (uint8_t)(raw >> 8);
        rx[3] = (uint8_t)raw;
        rx[4] = status;
        return IF_OK;
    }
    if (reg == AD7124_REG_ERROR) {
        ++error_reads;
        regs[reg] = 0U;
        status_bits &= (uint8_t)~AD7124_STATUS_ERROR_FLAG;
    }
    for (uint16_t i = 1U; i < length; ++i)
        rx[i] = (uint8_t)(value >> (8U * (length - i - 1U)));
    return IF_OK;
}

static spi_if_t spi = {.send = send_bytes, .transfer = transfer_bytes};

static ad7124_t reset_case(void)
{
    memset(regs, 0, sizeof(regs));
    calls = fail_at = fail_from = writes = waits = 0U;
    stream_pos = status_bits = stuck_mode = cal_error = full_count = zero_count = 0U;
    selected = 0;
    last_release_wait = UINT32_MAX;
    error_reads = cal_delay = por_reads = ignore_diagnostics = 0U;
    regs[AD7124_REG_ID] = AD7124_ID_AD7124_4_REV_E;
    stream_len = 3U;
    stream[0] = 2U; stream[1] = 0U; stream[2] = 1U;
    regs[AD7124_REG_ADC_CTRL] = AD7124_ADC_REF_EN | AD7124_ADC_DATA_STATUS |
                              AD7124_ADC_POWER_MODE(AD7124_PWR_FULL);
    cal_mode = AD7124_MODE_CONT;
    for (unsigned i = 0U; i < 16U; ++i) {
        regs[AD7124_REG_CH0 + i] = AD7124_CH_SETUP(i % 8U) | AD7124_CH_AINP(i % 8U);
        if (i < 3U || i == 15U) regs[AD7124_REG_CH0 + i] |= AD7124_CH_ENABLE;
    }
    for (unsigned i = 0U; i < 8U; ++i) {
        regs[AD7124_REG_CFG0 + i] = AD7124_CFG_REF_SEL(AD7124_REF_INT);
        regs[AD7124_REG_GAIN0 + i] = 0x500000U + i;
        regs[AD7124_REG_OFFSET0 + i] = 0x800010U + i;
    }
    memcpy(before, regs, sizeof(regs));
    ad7124_t adc = {.spi = &spi, .cs_port = &gpio, .cs_pin = 1U,
                    .num_ch = 3U, .adc_mode = AD7124_MODE_CONT, .wait_ms = HAL_Delay};
    return adc;
}

static void unchanged(const int32_t *values, unsigned count)
{
    for (unsigned i = 0U; i < count; ++i) CHECK(values[i] == 777);
    CHECK(!selected);
}

static void test_samples(void)
{
    ad7124_t adc = reset_case();
    int32_t values[17];
    for (unsigned i = 0U; i < 17U; ++i) values[i] = 777;
    uint8_t got = 99U;
    CHECK(ad7124_read_all(&adc, values, 17U, &got) == IF_OK && got == 3U);
    CHECK(values[0] == 0x100000 && values[1] == 0x200000 && values[2] == 0x300000);
    unchanged(values + 3, 14U);
    for (unsigned i = 0U; i < 17U; ++i) values[i] = 777;

    /* 缺首/中/尾通道及全重复：任何缺口都不能返回成功或交付局部数组。 */
    for (uint8_t missing = 0U; missing < 3U; ++missing) {
        adc = reset_case(); stream_len = 2U;
        stream[0] = (missing + 1U) % 3U; stream[1] = (missing + 2U) % 3U;
        CHECK(ad7124_read_all_mv(&adc, values, 3U, &got, 2.5f, NULL, NULL) == IF_TIMEOUT);
        CHECK(got == 2U); unchanged(values, 17U);
    }
    adc = reset_case(); stream_len = 1U; stream[0] = 2U;
    CHECK(ad7124_read_all(&adc, values, 3U, &got) == IF_TIMEOUT && got == 1U);
    unchanged(values, 17U);

    /* 6个SPI事务分别失败；包括已拿到部分结果后的STATUS/DATA读失败。 */
    for (unsigned i = 1U; i <= 6U; ++i) {
        adc = reset_case(); fail_at = i;
        CHECK(ad7124_read_all(&adc, values, 3U, &got) == IF_ERR);
        unchanged(values, 17U);
    }
    adc = reset_case(); status_bits = AD7124_STATUS_RDY;
    CHECK(ad7124_read_all(&adc, values, 3U, &got) == IF_TIMEOUT && got == 0U);
    CHECK(waits == 500U); unchanged(values, 17U);
    adc = reset_case(); status_bits = AD7124_STATUS_ERROR_FLAG;
    CHECK(ad7124_read_all(&adc, values, 3U, &got) == IF_ERR && calls == 2U && error_reads == 1U);
    unchanged(values, 17U);
    adc = reset_case(); stream[1] |= AD7124_STATUS_ERROR_FLAG;
    CHECK(ad7124_read_all(&adc, values, 3U, &got) == IF_ERR && got == 1U);
    unchanged(values, 17U);

    adc = reset_case();
    CHECK(ad7124_read_all(&adc, values, 2U, &got) == IF_ERR && got == 0U && calls == 0U);
    CHECK(ad7124_read_all(NULL, values, 3U, &got) == IF_ERR);
    CHECK(ad7124_read_all(&adc, NULL, 3U, &got) == IF_ERR);
    CHECK(ad7124_read_all_mv(&adc, values, 3U, &got, NAN, NULL, NULL) == IF_ERR);
    CHECK(ad7124_read_all_mv(&adc, NULL, 3U, &got, 2.5f, NULL, NULL) == IF_ERR);
    adc.num_ch = 0U; CHECK(ad7124_read_all(&adc, values, 3U, &got) == IF_ERR);
    adc.num_ch = 17U; CHECK(ad7124_read_all(&adc, values, 17U, &got) == IF_ERR);
    unchanged(values, 17U);

    adc = reset_case(); adc.num_ch = 16U; stream_len = 16U;
    for (unsigned i = 0U; i < 16U; ++i) stream[i] = (uint8_t)(15U - i);
    CHECK(ad7124_read_all(&adc, values, 16U, NULL) == IF_OK && values[16] == 777);
    for (unsigned i = 0U; i < 16U; ++i) CHECK(values[i] == (int32_t)(((i + 1U) * 0x100000U) & 0xFFFFFFU));
    adc = reset_case();
    CHECK(ad7124_read_all_mv(&adc, values, 3U, NULL, 2.5f, NULL, NULL) == IF_OK);
    CHECK(values[0] == 156 && values[1] == 312 && values[2] == 468);

    adc = reset_case(); values[0] = 777; adc.adc_mode = AD7124_MODE_SINGLE;
    CHECK(ad7124_read_ch(&adc, 0U, values) == IF_TIMEOUT && values[0] == 777);
    stream_pos = 0U; stream[0] = 0U;
    CHECK(ad7124_read_ch(&adc, 0U, values) == IF_OK && values[0] == 0x100000);
    values[0] = 777; stream_pos = 0U; stream[0] = AD7124_STATUS_ERROR_FLAG;
    CHECK(ad7124_read_ch(&adc, 0U, values) == IF_ERR && values[0] == 777);
    CHECK(ad7124_read_ch(&adc, 16U, values) == IF_ERR);
    CHECK(ad7124_read_ch(NULL, 0U, values) == IF_ERR);
    CHECK(ad7124_read_ch_mv(&adc, 0U, 2.5f, 0U, 0U, NULL) == IF_ERR);
    CHECK(ad7124_read_error(&adc, NULL) == IF_ERR);
    adc = reset_case();
    CHECK(ad7124_set_mode(&adc, AD7124_MODE_UNKNOWN) == IF_ERR && calls == 0U);
    CHECK(ad7124_set_mode(&adc, 9U) == IF_ERR && calls == 0U);
}

static void check_restored(void)
{
    CHECK(regs[AD7124_REG_ADC_CTRL] == before[AD7124_REG_ADC_CTRL]);
    for (unsigned i = 0U; i < 16U; ++i)
        CHECK(regs[AD7124_REG_CH0 + i] == before[AD7124_REG_CH0 + i]);
    CHECK(!selected);
}

static void test_calibration(void)
{
    ad7124_t adc = reset_case();
    CHECK(ad7124_calibrate(&adc, 1U) == IF_OK && full_count == 0U && zero_count == 1U);
    CHECK(regs[AD7124_REG_GAIN0 + 1U] == before[AD7124_REG_GAIN0 + 1U]);
    CHECK(regs[AD7124_REG_OFFSET0 + 1U] == 0x800123U);
    check_restored();

    adc = reset_case(); regs[AD7124_REG_CFG0 + 7U] |= 2U;
    CHECK(ad7124_calibrate(&adc, 15U) == IF_OK && full_count == 1U && zero_count == 1U);
    CHECK(regs[AD7124_REG_GAIN0 + 7U] == 0x512345U);
    CHECK(regs[AD7124_REG_OFFSET0 + 7U] == 0x800123U);
    check_restored();
    uint32_t operation_count = calls;

    /* 枚举成功路径的每一个读/写故障点，不能有任何失败被误报成功。 */
    for (uint32_t point = 1U; point <= operation_count; ++point) {
        adc = reset_case(); regs[AD7124_REG_CFG0 + 7U] |= 2U; fail_at = point;
        CHECK(ad7124_calibrate(&adc, 15U) == IF_ERR && !selected);
        if (adc.num_ch != 0U) {
            check_restored();
            CHECK(regs[AD7124_REG_GAIN0 + 7U] == before[AD7124_REG_GAIN0 + 7U]);
            CHECK(regs[AD7124_REG_OFFSET0 + 7U] == before[AD7124_REG_OFFSET0 + 7U]);
        } else {
            CHECK(adc.adc_mode == 0xFFU);
        }
        if (point <= 20U) CHECK(writes == 0U);
    }
    for (uint8_t mode = AD7124_MODE_CAL_INT_Z; mode <= AD7124_MODE_CAL_INT_F; ++mode) {
        adc = reset_case(); regs[AD7124_REG_CFG0] |= 1U; stuck_mode = mode;
        CHECK(ad7124_calibrate(&adc, 0U) == IF_TIMEOUT && waits >= 20000U && waits < 20100U);
        CHECK(regs[AD7124_REG_GAIN0] == before[AD7124_REG_GAIN0]);
        CHECK(regs[AD7124_REG_OFFSET0] == before[AD7124_REG_OFFSET0]);
        check_restored();
    }
    adc = reset_case(); cal_error = 1U;
    CHECK(ad7124_calibrate(&adc, 0U) == IF_ERR); check_restored();
    adc = reset_case(); fail_from = 22U;
    CHECK(ad7124_calibrate(&adc, 0U) == IF_ERR && adc.num_ch == 0U && adc.adc_mode == 0xFFU);
    adc = reset_case();
    CHECK(ad7124_calibrate(NULL, 0U) == IF_ERR);
    CHECK(ad7124_calibrate(&adc, 16U) == IF_ERR && calls == 0U);
    regs[AD7124_REG_ADC_CTRL] |= AD7124_ADC_MODE(AD7124_MODE_CAL_INT_F);
    CHECK(ad7124_calibrate(&adc, 0U) == IF_ERR && writes == 0U);
}

static void test_filter(void)
{
    ad7124_t adc = reset_case();
    CHECK(ad7124_config_setup(&adc, 2U, 0U, 0U, AD7124_REF_INT,
                             AD7124_FILT_SINC3 | AD7124_FILT_REJ60, 384U) == IF_OK);
    CHECK(regs[AD7124_REG_FILTER0 + 2U] == 0x500180U);
    static const uint32_t invalid[] = {1U, 1U << 15, 1U << 21, 3U << 21, 6U << 21, 1U << 24};
    uint32_t saved_calls = calls;
    for (unsigned i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        CHECK(ad7124_config_setup(&adc, 0U, 0U, 0U, 2U, invalid[i], 384U) == IF_ERR);
    CHECK(ad7124_config_setup(&adc, 0U, 0U, 0U, 2U, 0U, 0U) == IF_ERR);
    CHECK(ad7124_config_setup(&adc, 0U, 0U, 0U, 2U, 0U, 2048U) == IF_ERR);
    CHECK(calls == saved_calls);
    CHECK(AD7124_FS_9P6HZ == 2000U);
}

/* 仅桩化其它器件，以实际组合层验证坏ADC不会冒充有效电压，也不丢弃PAC数据。 */
int pac1934_init(pac1934_t *d, uint8_t channels, uint8_t control)
{ (void)d; (void)channels; (void)control; return IF_OK; }
int pac1934_measure_all(pac1934_t *d, pac1934_measurement_t *out)
{ (void)d; memset(out, 0, 4U * sizeof(*out)); out[0].current_ma = 123U; return IF_OK; }
int ddr_power_read_status(uint8_t rail, uint8_t *status)
{ (void)rail; *status = 0U; return IF_OK; }

static void test_monitor(void)
{
    memory_monitor_t monitor = {0};
    memory_monitor_sample_t sample;
    monitor.adc = reset_case(); monitor.adc_ready = 1U; monitor.pac_ready = 1U;
    stream_len = 1U; stream[0] = 2U;
    CHECK(memory_monitor_sample(&monitor, &sample) == IF_ERR);
    CHECK(!(sample.valid_mask & MEMORY_MONITOR_VALID_VOLTAGE));
    CHECK(sample.valid_mask & MEMORY_MONITOR_VALID_IDD);
    CHECK(sample.idd[0].current_ma == 123U);
    CHECK(sample.voltage_mv[0] == 0 && sample.voltage_mv[1] == 0 && sample.voltage_mv[2] == 0);
    CHECK(monitor.adc_ready == 0U);
    monitor.adc = reset_case();
    CHECK(memory_monitor_init(&monitor) == IF_OK && monitor.adc_ready == 1U);
    CHECK(memory_monitor_sample(&monitor, &sample) == IF_OK);
#ifdef AD_TEST_B1
    CHECK(sample.voltage_mv[0] == 158 && sample.voltage_mv[1] == 315 && sample.voltage_mv[2] == 473);
#else
    CHECK(sample.voltage_mv[0] == 156 && sample.voltage_mv[1] == 312 && sample.voltage_mv[2] == 468);
#endif
}

static void test_initialization_and_diagnostics(void)
{
    ad7124_t adc = reset_case();
    por_reads = 3U;
    CHECK(ad7124_init(&adc) == IF_OK);
    CHECK(adc.num_ch == 3U && adc.adc_mode == AD7124_MODE_CONT);
    CHECK(regs[AD7124_REG_ERROR_EN] == AD7124_DIAGNOSTIC_MASK);
    CHECK(regs[AD7124_REG_ADC_CTRL] & AD7124_ADC_CS_EN);
    for (unsigned i = 3U; i < 16U; ++i) CHECK(regs[AD7124_REG_CH0 + i] == 0U);
    uint32_t initialization_calls = calls;
    CHECK(waits >= initialization_calls + 4U);
    /* 每个复位、轮询、诊断、配置和回读故障均须使初始化失败。 */
    for (uint32_t point = 1U; point <= initialization_calls; ++point) {
        adc = reset_case(); por_reads = 3U; fail_at = point;
        CHECK(ad7124_init(&adc) != IF_OK && adc.num_ch == 0U && !selected);
    }
    adc = reset_case(); por_reads = 101U;
    CHECK(ad7124_reset(&adc) == IF_TIMEOUT && writes == 0U);
    CHECK(adc.adc_mode == AD7124_MODE_UNKNOWN);
    adc = reset_case(); regs[AD7124_REG_ID] = 0xFFU;
    CHECK(ad7124_init(&adc) == IF_ERR);
    adc = reset_case(); ignore_diagnostics = 1U;
    CHECK(ad7124_init(&adc) == IF_ERR && adc.num_ch == 0U);

    int32_t value = 777;
    adc = reset_case(); status_bits = AD7124_STATUS_ERROR_FLAG;
    regs[AD7124_REG_ERROR] = AD7124_ERR_ADC_SAT | AD7124_ERR_ADC_CONV;
    CHECK(ad7124_read_ch(&adc, 0U, &value) == IF_ERR && value == 777);
    CHECK(adc.last_error == (AD7124_ERR_ADC_SAT | AD7124_ERR_ADC_CONV) && error_reads == 1U);
    CHECK(regs[AD7124_REG_ERROR] == 0U);
    CHECK(ad7124_init(&adc) == IF_OK);
    CHECK(adc.last_error == (AD7124_ERR_ADC_SAT | AD7124_ERR_ADC_CONV));
    adc = reset_case(); status_bits = AD7124_STATUS_ERROR_FLAG; fail_at = 2U;
    CHECK(ad7124_read_ch(&adc, 0U, &value) == IF_ERR);
    CHECK(adc.last_error == AD7124_ERROR_UNREADABLE);
    adc = reset_case(); status_bits = AD7124_STATUS_POR_FLAG;
    CHECK(ad7124_read_ch(&adc, 0U, &value) == IF_ERR && value == 777);
    CHECK(adc.num_ch == 0U && adc.adc_mode == AD7124_MODE_UNKNOWN);
    adc = reset_case(); status_bits = AD7124_STATUS_POR_FLAG | AD7124_STATUS_ERROR_FLAG;
    regs[AD7124_REG_ERROR] = AD7124_ERR_ADC_CONV;
    CHECK(ad7124_read_ch(&adc, 0U, &value) == IF_ERR && value == 777);
    CHECK(adc.num_ch == 0U && adc.adc_mode == AD7124_MODE_UNKNOWN);
    CHECK(adc.last_error == AD7124_ERR_ADC_CONV && error_reads == 1U);
    adc = reset_case(); stream[0] = AD7124_STATUS_POR_FLAG;
    CHECK(ad7124_read_ch(&adc, 0U, &value) == IF_ERR && value == 777);

    /* 合法慢校准超过一秒仍应完成。 */
    adc = reset_case(); regs[AD7124_REG_CFG0] |= 1U; cal_delay = 14000U;
    CHECK(ad7124_calibrate(&adc, 0U) == IF_OK && waits > 14000U);
    check_restored();
    CHECK(ad7124_raw_to_voltage(0x800000, 2.5f, 0U, 0U) == 1.25f);
    CHECK(ad7124_raw_to_voltage(0xFFFFFF, 2.5f, 0U, 0U) < 2.5f);
}

void test_main(void)
{
    test_samples(); test_calibration(); test_filter(); test_monitor();
    test_initialization_and_diagnostics();
    message("PASS: actual AD7124 snapshots, calibration fault sweep, filter and monitor\n");
    finish(0U);
}
