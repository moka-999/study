/* 真正运行PAC驱动、组合采样层与最短等待；总线和时钟是模型，不代表真板精度。 */
#include "pac1934.h"
#include "memory_monitor.h"
#include "device_wait.h"
#include <string.h>

#define S1(x) #x
#define S(x) S1(x)
#define CHECK(c) do { if (!(c)) fail("FAIL PAC " S(__LINE__) ": " #c "\n"); } while (0)

static uint64_t now_us, busy_until, switch_at;
static uint32_t rtos_phase_us;
static uint32_t calls, fail_at, writes, channel_reads[4], delayed_rate;
static uint8_t config[3], active[3], latched[3], pending[3];
static uint16_t vbus[4], vsense[4];
static uint32_t power[4];
static uint8_t product, manufacturer, corrupt_reg, corrupt_index;
static int corrupt_enabled, adc_fails;
static GPIO_TypeDef gpio;
static spi_if_t spi;

/* 复用项目QEMU半主机约定，不访问串口、网络或真实Flash。 */
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

uint32_t HAL_GetTick(void) { return (uint32_t)(now_us / 1000U); }
void vTaskDelay(TickType_t ticks)
{
    CHECK(ticks != 0U);
    /* 故意允许第一次休眠只过1µs：不能把一次vTaskDelay(1)当作满1ms。 */
    now_us = ((now_us + rtos_phase_us) / 1000U + ticks) * 1000U - rtos_phase_us;
}

static int io_failed(uint8_t address)
{
    CHECK(address == 0x16U && now_us >= busy_until);
    if (switch_at && now_us >= switch_at) {
        memcpy(active, pending, 3U);
        switch_at = 0U;
    }
    return ++calls == fail_at;
}

static int read_bytes(i2c_if_t *bus, uint8_t address, uint8_t reg,
                      uint8_t *out, uint16_t size)
{
    (void)bus;
    if (io_failed(address)) return IF_ERR;
    if (reg == 0xFDU || reg == 0xFEU) {
        CHECK(size == 1U);
        out[0] = reg == 0xFDU ? product : manufacturer;
        return IF_OK;
    }
    if (reg == 0x21U || reg == 0x24U) {
        CHECK(size == 3U);
        memcpy(out, reg == 0x21U ? active : latched, 3U);
        if (corrupt_enabled && reg == corrupt_reg) out[corrupt_index] ^= 0x10U;
        return IF_OK;
    }
    uint8_t ch;
    uint32_t value;
    if (reg >= 0x07U && reg <= 0x0AU) {
        ch = reg - 0x07U; value = vbus[ch]; CHECK(size == 2U);
    } else if (reg >= 0x0BU && reg <= 0x0EU) {
        ch = reg - 0x0BU; value = vsense[ch]; CHECK(size == 2U);
    } else {
        CHECK(reg >= 0x17U && reg <= 0x1AU && size == 4U);
        ch = reg - 0x17U; value = power[ch];
    }
    /* 独立按手册bit7..4判断是否误读关闭通道，不能复用被测函数的掩码算法。 */
    CHECK(!(latched[1] & (0x80U >> ch)));
    ++channel_reads[ch];
    for (uint16_t i = 0U; i < size; ++i)
        out[i] = (uint8_t)(value >> (8U * (size - i - 1U)));
    return IF_OK;
}

static int write_reg(i2c_if_t *bus, uint8_t address, uint8_t reg, uint8_t value)
{
    (void)bus;
    int failed = io_failed(address);
    CHECK(reg == 0x01U || reg == 0x1CU || reg == 0x1DU);
    /* 先写再报错，覆盖ACK丢失但器件实际已改变的情况。 */
    config[reg == 0x01U ? 0U : reg == 0x1CU ? 1U : 2U] = value;
    ++writes;
    return failed ? IF_ERR : IF_OK;
}
static int command(i2c_if_t *bus, uint8_t address, uint8_t cmd)
{
    (void)bus;
    int failed = io_failed(address);
    CHECK(cmd == 0x00U || cmd == 0x1FU);
    memcpy(latched, active, 3U);
    memcpy(pending, config, 3U);
    switch_at = now_us + (delayed_rate ? 125000U : 1000U);
    busy_until = now_us + 1000U;
    return failed ? IF_ERR : IF_OK;
}
static i2c_if_t bus = {.read_bytes = read_bytes, .write_reg = write_reg, .write_cmd = command};

static pac1934_t reset_case(void)
{
    now_us = 999U;
    rtos_phase_us = 0U;
    busy_until = switch_at = 0U;
    calls = fail_at = writes = delayed_rate = 0U;
    corrupt_enabled = adc_fails = 0;
    memset(config, 0, sizeof(config)); memset(active, 0, sizeof(active));
    memset(latched, 0, sizeof(latched)); memset(channel_reads, 0, sizeof(channel_reads));
    product = 0x5BU; manufacturer = 0x5DU;
    for (uint32_t i = 0U; i < 4U; ++i) {
        vbus[i] = (uint16_t)(0x1000U * (i + 1U));
        vsense[i] = (uint16_t)(0x0800U * (i + 1U));
        power[i] = (0x00080000U * (i + 1U) * (i + 1U)) << 4;
    }
    pac1934_t d = {.bus = &bus, .addr = 0x16U, .rsense_uohm = PAC1934_RSENSE_B1_UOHM,
                    .wait_ms = device_wait_ms};
    return d;
}

static void test_wait(void)
{
    const uint32_t offsets[] = {0U, 1U, 999U};
    const uint32_t durations[] = {0U, 1U, 20U, 126U, 1001U};
    for (unsigned phase = 0U; phase < 2U; ++phase)
      for (unsigned wrap = 0U; wrap < 2U; ++wrap)
        for (unsigned i = 0U; i < 3U; ++i)
            for (unsigned j = 0U; j < 5U; ++j) {
                /* TIM1与SysTick不一定同相；另外覆盖半个tick的相位偏移。 */
                rtos_phase_us = phase * 500U;
                now_us = (wrap ? (uint64_t)(UINT32_MAX - 10U) * 1000U : 0U) + offsets[i];
                uint64_t begin = now_us;
                device_wait_ms(durations[j]);
                CHECK(now_us - begin >= (uint64_t)durations[j] * 1000U);
                CHECK(durations[j] != 0U || now_us == begin);
                CHECK(now_us - begin <= ((uint64_t)durations[j] + 4U) * 1000U);
            }
}

static void test_mapping_and_units(void)
{
    static const uint8_t disabled[16] =
        {0xF0,0x70,0xB0,0x30,0xD0,0x50,0x90,0x10,0xE0,0x60,0xA0,0x20,0xC0,0x40,0x80,0x00};
    for (uint8_t mask = 1U; mask <= 15U; ++mask) {
        pac1934_t d = reset_case();
        uint8_t rate = mask % 4U;
        delayed_rate = 1U; /* 模拟旧8SPS到新配置最长125ms切换。 */
        CHECK(pac1934_init(&d, mask, rate) == IF_OK);
        CHECK(config[1] == disabled[mask] && config[2] == 0U && config[0] == (rate << 6));
        delayed_rate = 0U;
        pac1934_measurement_t out[4];
        memset(out, 0xA5, sizeof(out));
        CHECK(pac1934_measure_all(&d, out) == IF_OK);
        for (uint8_t ch = 0U; ch < 4U; ++ch) {
            if (mask & (1U << ch)) {
                CHECK(channel_reads[ch] == 3U && out[ch].vbus_mv == 2000U * (ch + 1U));
                uint32_t factor = 10000U == PAC1934_RSENSE_B1_UOHM ? 1U : 2U;
                CHECK(out[ch].current_ua == (uint64_t)312500U * (ch + 1U) / factor);
                CHECK(out[ch].current_ma == out[ch].current_ua / 1000U);
                CHECK(out[ch].power_mw == 625U * (ch + 1U) * (ch + 1U) / factor);
            } else {
                CHECK(channel_reads[ch] == 0U && out[ch].current_ua == 0U &&
                      out[ch].current_ma == 0U && out[ch].vbus_mv == 0U && out[ch].power_mw == 0U);
            }
        }
    }
    pac1934_t d = reset_case();
    d.rsense_uohm = 20000U;
    CHECK(pac1934_init(&d, 7U, 0U) == IF_OK);
    vsense[0] = 8U;
    pac1934_measurement_t out[4];
    CHECK(pac1934_measure_all(&d, out) == IF_OK && out[0].current_ma == 0U && out[0].current_ua == 610U);
    CHECK(pac1934_current_ua(65535U, 1U) == UINT64_C(99998474121));
    CHECK(pac1934_current_ua(123U, 0U) == 0U && pac1934_current_ma(123U, 0U) == 0U);
    CHECK(pac1934_power_mw(0x10000000U, 10000U) == 0U);
    /* 长时间累加溢出不影响瞬时读数，不因CTRL bit0误报配置异常。 */
    active[0] |= 1U; config[0] |= 1U;
    CHECK(pac1934_measure_all(&d, out) == IF_OK);
}

static void test_failures(void)
{
    pac1934_t d = reset_case();
    CHECK(pac1934_init(&d, 7U, 0U) == IF_OK);
    uint32_t init_calls = calls;
    for (uint32_t point = 1U; point <= init_calls; ++point) {
        d = reset_case(); fail_at = point;
        CHECK(pac1934_init(&d, 7U, 0U) == IF_ERR && d.active_mask == 0U);
        CHECK(d.last_error == PAC1934_ERROR_IO);
    }

    d = reset_case();
    CHECK(pac1934_init(&d, 7U, 0U) == IF_OK);
    uint32_t before = calls;
    pac1934_measurement_t out[4], saved[4];
    CHECK(pac1934_measure_all(&d, out) == IF_OK);
    uint32_t read_calls = calls - before;
    memset(saved, 0xA5, sizeof(saved));
    for (uint32_t point = 1U; point <= read_calls; ++point) {
        d = reset_case(); CHECK(pac1934_init(&d, 7U, 0U) == IF_OK);
        fail_at = calls + point; memcpy(out, saved, sizeof(out));
        CHECK(pac1934_measure_all(&d, out) == IF_ERR && memcmp(out, saved, sizeof(out)) == 0);
        CHECK(d.last_error == PAC1934_ERROR_IO && d.active_mask == 0U);
    }
    for (uint8_t reg = 0x21U; reg <= 0x24U; reg += 3U)
        for (uint8_t field = 0U; field < 3U; ++field) {
            d = reset_case(); CHECK(pac1934_init(&d, 7U, 0U) == IF_OK);
            corrupt_enabled = 1; corrupt_reg = reg; corrupt_index = field;
            memcpy(out, saved, sizeof(out));
            CHECK(pac1934_measure_all(&d, out) == IF_ERR && d.last_error == PAC1934_ERROR_CONFIG);
            CHECK(d.active_mask == 0U && memcmp(out, saved, sizeof(out)) == 0);
        }

    for (uint8_t kind = 0U; kind < 4U; ++kind) {
        d = reset_case(); CHECK(pac1934_init(&d, 7U, 0U) == IF_OK);
        if (kind == 0U) power[1] |= 1U;
        if (kind == 1U) vbus[1] = 0xFFFFU;
        if (kind == 2U) vsense[1] = 0xFFFFU;
        if (kind == 3U) power[1] = 0xFFFFFFF0U;
        memcpy(out, saved, sizeof(out));
        CHECK(pac1934_measure_all(&d, out) == IF_ERR && memcmp(out, saved, sizeof(out)) == 0);
        CHECK(d.last_error == (kind ? PAC1934_ERROR_RANGE : PAC1934_ERROR_DATA));
        CHECK(d.active_mask == (kind ? 7U : 0U));
    }

    /* 原始批量及单通道读取同样不能泄漏半份数据。 */
    pac1934_chan_t raw[4], old[4];
    memset(old, 0xA5, sizeof(old));
    d = reset_case(); CHECK(pac1934_init(&d, 7U, 0U) == IF_OK);
    memcpy(raw, old, sizeof(raw)); fail_at = calls + 7U;
    CHECK(pac1934_read_all(&d, raw) == IF_ERR && memcmp(raw, old, sizeof(raw)) == 0);
    d = reset_case(); CHECK(pac1934_init(&d, 7U, 0U) == IF_OK);
    CHECK(pac1934_read_all(&d, raw) == IF_OK);
    memcpy(raw, old, sizeof(raw)); fail_at = calls + 5U;
    CHECK(pac1934_read_ch(&d, 1U, raw) == IF_ERR && memcmp(raw, old, sizeof(raw)) == 0);

    d = reset_case(); product = 0U;
    CHECK(pac1934_init(&d, 7U, 0U) == IF_ERR && d.last_error == PAC1934_ERROR_ID && !writes);
    d = reset_case(); manufacturer = 0xFFU;
    CHECK(pac1934_init(&d, 7U, 0U) == IF_ERR && d.last_error == PAC1934_ERROR_ID && !writes);
    d = reset_case(); d.wait_ms = NULL;
    CHECK(pac1934_init(&d, 7U, 0U) == IF_ERR && !calls);
    d = reset_case();
    CHECK(pac1934_init(&d, 0U, 0U) == IF_ERR && !calls);
    CHECK(pac1934_init(&d, 16U, 0U) == IF_ERR && !calls);
    CHECK(pac1934_init(&d, 7U, 4U) == IF_ERR && !calls);
    CHECK(pac1934_init(NULL, 7U, 0U) == IF_ERR);
    CHECK(pac1934_read_all(&d, NULL) == IF_ERR && !calls);
    CHECK(pac1934_measure_all(NULL, out) == IF_ERR);
    d.addr = 0x80U;
    CHECK(pac1934_init(&d, 7U, 0U) == IF_ERR && !calls);
    d = reset_case(); CHECK(pac1934_init(&d, 7U, 0U) == IF_OK);
    CHECK(pac1934_read_ch(&d, 4U, raw) == IF_ERR);
    CHECK(pac1934_read_ch(&d, 3U, raw) == IF_ERR);
    d.rsense_uohm = 0U;
    CHECK(pac1934_measure_all(&d, out) == IF_ERR);
}

/* 只桩化其它器件，运行真实组合层并验证故障后的下一轮重新初始化。 */
int ad7124_init(ad7124_t *d) { d->num_ch = 3U; return IF_OK; }
int ad7124_read_all_mv(ad7124_t *d, int32_t *out, uint8_t count, uint8_t *got,
                       float ref, const uint8_t *gain, const uint8_t *bipolar)
{
    (void)d; (void)ref; (void)gain; (void)bipolar;
    if (adc_fails) { *got = 0U; return IF_ERR; }
    for (uint8_t i = 0U; i < count; ++i) out[i] = 1000;
    *got = count;
    return IF_OK;
}
int ddr_power_read_status(uint8_t rail, uint8_t *status)
{ (void)rail; *status = 0U; return IF_OK; }

static void test_monitor_recovery(void)
{
    (void)reset_case();
    memory_monitor_t monitor;
    memory_monitor_sample_t sample;
    memory_monitor_configure(&monitor, &bus, &spi, &gpio, 1U, device_wait_ms);
    CHECK(monitor.pac.rsense_uohm == PAC1934_RSENSE_B1_UOHM);
    CHECK(memory_monitor_init(&monitor) == IF_OK);
    CHECK(memory_monitor_sample(&monitor, &sample) == IF_OK);
    fail_at = calls + 1U;
    CHECK(memory_monitor_sample(&monitor, &sample) == IF_ERR);
    CHECK(!monitor.pac_ready && !(sample.valid_mask & MEMORY_MONITOR_VALID_IDD));
    CHECK(sample.valid_mask & MEMORY_MONITOR_VALID_VOLTAGE);
    /* REFRESH失败也可能已经执行；模拟下一轮后台监测，不在失败点立即重发。 */
    device_wait_ms(2000U);
    fail_at = 0U;
    CHECK(memory_monitor_init(&monitor) == IF_OK && monitor.pac_ready);
    CHECK(memory_monitor_sample(&monitor, &sample) == IF_OK);
    adc_fails = 1;
    CHECK(memory_monitor_sample(&monitor, &sample) == IF_ERR);
    CHECK(!(sample.valid_mask & MEMORY_MONITOR_VALID_VOLTAGE));
    CHECK(sample.valid_mask & MEMORY_MONITOR_VALID_IDD);
}

void test_main(void)
{
    test_wait(); test_mapping_and_units(); test_failures(); test_monitor_recovery();
    message("PASS: actual PAC1934 mapping, power, wait, precision, faults and recovery\n");
    finish(0U);
}
