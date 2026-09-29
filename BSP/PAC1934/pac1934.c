/*
 * PAC1934同步驱动：单极性、连续采样；I2C地址为未左移的7位地址。
 * 只负责器件配置、数据有效性及单位换算，不定义RK状态或IDD业务窗口。
 */
#include "pac1934.h"
#include <string.h>

/* 通信/配置不可信时清除就绪掩码；量程上沿只拒绝本次数据，不重置器件。 */
static int pac_fail(pac1934_t *d, pac1934_error_t error)
{
    if (d) {
        d->last_error = error;
        if (error == PAC1934_ERROR_IO || error == PAC1934_ERROR_ID ||
            error == PAC1934_ERROR_CONFIG || error == PAC1934_ERROR_DATA)
            d->active_mask = 0U;
    }
    return IF_ERR;
}

static int read_bytes(pac1934_t *d, uint8_t reg, uint8_t *data, uint16_t size)
{
    if (!d->bus || !d->bus->read_bytes ||
        d->bus->read_bytes(d->bus, d->addr, reg, data, size) != IF_OK)
        return pac_fail(d, PAC1934_ERROR_IO);
    return IF_OK;
}

static int write_register(pac1934_t *d, uint8_t reg, uint8_t value)
{
    if (!d->bus || !d->bus->write_reg ||
        d->bus->write_reg(d->bus, d->addr, reg, value) != IF_OK)
        return pac_fail(d, PAC1934_ERROR_IO);
    return IF_OK;
}

/* REFRESH系列是Send Byte命令，不附加数据字节。 */
static int send_command(pac1934_t *d, uint8_t command)
{
    if (!d->bus || !d->bus->write_cmd ||
        d->bus->write_cmd(d->bus, d->addr, command) != IF_OK)
        return pac_fail(d, PAC1934_ERROR_IO);
    return IF_OK;
}

/* 软件bit0..3对应CH1..4，但器件的关闭位按bit7..4反向排列。 */
static uint8_t channel_disable_bits(uint8_t enabled)
{
    uint8_t disabled = 0U;
    for (uint8_t ch = 0U; ch < 4U; ++ch)
        if (!(enabled & (1U << ch))) disabled |= (uint8_t)(0x80U >> ch);
    return disabled;
}

/* ACT核对实际配置；LAT核对本次结果所属配置，不能只验证写入ACK。 */
static int check_configuration(pac1934_t *d, uint8_t enabled,
                               uint8_t rate, uint8_t first_register)
{
    uint8_t config[3];
    if (read_bytes(d, first_register, config, sizeof(config)) != IF_OK) return IF_ERR;
    /* CTRL的bit0是累加器溢出状态，不影响本驱动的瞬时读数，也不在此清除。 */
    if ((config[0] & 0xFEU) != PAC1934_CTRL_SAMPLE_RATE(rate) ||
        config[1] != channel_disable_bits(enabled) || config[2] != 0U)
        return pac_fail(d, PAC1934_ERROR_CONFIG);
    return IF_OK;
}

int pac1934_init(pac1934_t *d, uint8_t ch_mask, uint8_t sample_rate)
{
    uint8_t product, manufacturer;
    static const uint8_t conversion_ms[4] = {1U, 4U, 16U, 125U};
    if (!d) return IF_ERR;
    d->active_mask = 0U;
    d->last_error = PAC1934_ERROR_NONE;
    if (!d->wait_ms || d->addr > 0x7FU || ch_mask == 0U ||
        (ch_mask & ~PAC1934_CH_MASK_ALL) != 0U || sample_rate > 3U)
        return pac_fail(d, PAC1934_ERROR_PARAM);
    /* 通用兼容值不代表B1贴装值；正式板级层始终显式传入各板电阻参数。 */
    if (d->rsense_uohm == 0U) d->rsense_uohm = 10000U;

    /* 覆盖刚上电/退出PWRDN后的20ms器件启动要求，不是DDR稳定窗口。 */
    d->wait_ms(20U);
    if (read_bytes(d, PAC1934_REG_PRODUCT_ID, &product, 1U) != IF_OK ||
        read_bytes(d, PAC1934_REG_MANUF_ID, &manufacturer, 1U) != IF_OK) return IF_ERR;
    if (product != PAC1934_PRODUCT_ID || manufacturer != PAC1934_MANUF_ID)
        return pac_fail(d, PAC1934_ERROR_ID);

    if (write_register(d, PAC1934_REG_CHANNEL_DIS, channel_disable_bits(ch_mask)) != IF_OK ||
        write_register(d, PAC1934_REG_NEG_PWR, 0U) != IF_OK ||
        write_register(d, PAC1934_REG_CTRL, PAC1934_CTRL_SAMPLE_RATE(sample_rate)) != IF_OK ||
        send_command(d, PAC1934_REG_REFRESH) != IF_OK) return IF_ERR;

    /*
     * 旧配置可能为8SPS，新速率须等旧转换周期结束，最长125ms。
     * 一次保守等待126ms再回读，避免重初始化时将合法切换误判为失败。
     */
    d->wait_ms(126U);
    if (check_configuration(d, ch_mask, sample_rate, PAC1934_REG_CTRL_ACT) != IF_OK)
        return IF_ERR;
    /* 再给新配置一个完整转换周期；首次读数仍由读取API刷新锁存。 */
    d->wait_ms(conversion_ms[sample_rate]);
    d->sample_rate = sample_rate;
    d->active_mask = ch_mask;
    return IF_OK;
}

static uint16_t read_be16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

/* 调用方已验证配置与通道；局部缓冲保证任一步失败都不交付半份结果。 */
static int read_channel(pac1934_t *d, uint8_t ch, pac1934_chan_t *out)
{
    uint8_t bytes[4];
    pac1934_chan_t value = {0};
    if (read_bytes(d, PAC1934_REG_VBUS1 + ch, bytes, 2U) != IF_OK) return IF_ERR;
    value.vbus = read_be16(bytes);
    if (read_bytes(d, PAC1934_REG_VSENSE1 + ch, bytes, 2U) != IF_OK) return IF_ERR;
    value.vsense = read_be16(bytes);
    if (read_bytes(d, PAC1934_REG_VPOWER1 + ch, bytes, 4U) != IF_OK) return IF_ERR;
    uint32_t wire_power = read_be32(bytes);
    /* 有效功率码在bit31..4；bit3..0按手册必须为0，不能把总线坏值当测量值。 */
    if ((wire_power & 0x0FU) != 0U) return pac_fail(d, PAC1934_ERROR_DATA);
    value.vpower = wire_power >> 4;
    /* 只报告达到编码上沿/疑似饱和，不据此宣判DDR FAIL。 */
    if (value.vbus == UINT16_MAX || value.vsense == UINT16_MAX ||
        value.vpower == 0x0FFFFFFFU) return pac_fail(d, PAC1934_ERROR_RANGE);
    *out = value;
    return IF_OK;
}

static int check_sample_configuration(pac1934_t *d)
{
    if (check_configuration(d, d->active_mask, d->sample_rate,
                            PAC1934_REG_CTRL_ACT) != IF_OK) return IF_ERR;
    return check_configuration(d, d->active_mask, d->sample_rate, PAC1934_REG_CTRL_LAT);
}

int pac1934_read_ch(pac1934_t *d, uint8_t ch, pac1934_chan_t *out)
{
    if (!d || !out || ch >= 4U || !(d->active_mask & (1U << ch)))
        return pac_fail(d, PAC1934_ERROR_PARAM);
    d->last_error = PAC1934_ERROR_NONE;
    /* 本接口不发REFRESH；调用方必须已刷新并完成最短等待，且持有事务锁。 */
    if (check_sample_configuration(d) != IF_OK) return IF_ERR;
    return read_channel(d, ch, out);
}

int pac1934_read_all(pac1934_t *d, pac1934_chan_t *out)
{
    pac1934_chan_t values[4] = {0};
    if (!d || !out || !d->wait_ms || d->active_mask == 0U ||
        (d->active_mask & ~PAC1934_CH_MASK_ALL) != 0U || d->sample_rate > 3U)
        return pac_fail(d, PAC1934_ERROR_PARAM);
    d->last_error = PAC1934_ERROR_NONE;
    if (send_command(d, PAC1934_REG_REFRESH_V) != IF_OK) return IF_ERR;
    d->wait_ms(1U);
    if (check_sample_configuration(d) != IF_OK) return IF_ERR;
    /* 不依赖跨通道自动递增，避免禁用通道造成寄存器指针跳跃。 */
    for (uint8_t ch = 0U; ch < 4U; ++ch)
        if ((d->active_mask & (1U << ch)) && read_channel(d, ch, &values[ch]) != IF_OK)
            return IF_ERR;
    memcpy(out, values, sizeof(values));
    return IF_OK;
}

int pac1934_measure_all(pac1934_t *d, pac1934_measurement_t *out)
{
    pac1934_chan_t raw[4];
    pac1934_measurement_t values[4] = {0};
    if (!d || !out || d->rsense_uohm == 0U) return pac_fail(d, PAC1934_ERROR_PARAM);
    if (pac1934_read_all(d, raw) != IF_OK) return IF_ERR;
    for (uint8_t ch = 0U; ch < 4U; ++ch) {
        if (!(d->active_mask & (1U << ch))) continue;
        values[ch].vbus_mv = pac1934_vbus_mv(raw[ch].vbus);
        values[ch].current_ua = pac1934_current_ua(raw[ch].vsense, d->rsense_uohm);
        values[ch].current_ma = (uint32_t)(values[ch].current_ua / 1000U);
        values[ch].power_mw = pac1934_power_mw(raw[ch].vpower, d->rsense_uohm);
    }
    memcpy(out, values, sizeof(values));
    return IF_OK;
}

uint32_t pac1934_vbus_mv(uint16_t raw)
{
    return (uint32_t)raw * 32000U / 65536U;
}

uint64_t pac1934_current_ua(uint16_t raw, uint32_t rsense_uohm)
{
    /* 单极性100mV满量程；64位保留所有非零微欧参数的计算范围。 */
    if (rsense_uohm == 0U) return 0U;
    return (uint64_t)raw * UINT64_C(100000000000) / rsense_uohm / 65536U;
}

uint32_t pac1934_current_ma(uint16_t raw, uint32_t rsense_uohm)
{
    return (uint32_t)(pac1934_current_ua(raw, rsense_uohm) / 1000U);
}

uint32_t pac1934_power_mw(uint32_t raw, uint32_t rsense_uohm)
{
    /* raw是已经右移对齐的28位功率码，不是线上四字节值。 */
    if (rsense_uohm == 0U || raw > 0x0FFFFFFFU) return 0U;
    return (uint32_t)((uint64_t)raw * UINT64_C(3200000000) / rsense_uohm / 268435456U);
}
