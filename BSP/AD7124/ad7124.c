/*
 * ===================================================================
 * AD7124-4 驱动 — spi_if_t 抽象层, 不绑死物理 SPI
 * ===================================================================
 *
 * 通信序列 (SPI模式3：CPOL=1、CPHA=1，高位先行，时钟空闲为高):
 *   写寄存器: CS↓ → [cmd_wr] [data_MSB..LSB] → CS↑
 *   读寄存器: CS↓ → [cmd_rd] [dummy] → (收) [echo] [reg_data_MSB..LSB] → CS↑
 *   复位:     CS↓ → DIN保持高电平发送64个时钟 (8×0xFF) → CS↑ → 等待POR结束
 *
 * 寄存器大小（调用方须传匹配的len，通用读写接口不自动推断宽度）:
 *   8-bit:  STATUS, ID, MCLK_COUNT
 *   16-bit: ADC_CTRL, IO_CTRL2, CHx, CFGx
 *   24-bit: DATA, IO_CTRL1, ERROR, ERROR_EN, FILTERx, OFFSETx, GAINx
 * ===================================================================
 */
#include "ad7124.h"
#include <math.h>
#include <string.h>

/* AD7124-4有16个逻辑通道；B1只使能连续编号的CH0..2。 */
#define AD7124_CHANNEL_COUNT 16U
#define AD7124_SAMPLE_POLLS  500U
/* 覆盖低功耗、FS=2047的内部校准；每次SPI之后至少等待1ms。 */
#define AD7124_CAL_POLLS     20000U
/* 初始化固定全功率、Sinc4/FS=384；零位校准约80ms，单路保留500轮余量。 */
#define AD7124_INIT_CAL_POLLS 500U

static int calibrate_channel(ad7124_t *d, uint8_t ch, uint32_t polls);

/* 回调必须保证实际等待不少于ms；不能直接使用可能提前返回的osDelay(ms)。 */
static void _wait_ms(ad7124_t *d, uint32_t ms)
{
    if (d->wait_ms) d->wait_ms(ms);
    else            HAL_Delay(ms);
}

/* ── CS 控制 ── */
static void _cs_low(ad7124_t *d)
{ HAL_GPIO_WritePin(d->cs_port, d->cs_pin, GPIO_PIN_RESET); }

static void _cs_high(ad7124_t *d)
{ HAL_GPIO_WritePin(d->cs_port, d->cs_pin, GPIO_PIN_SET); }

/* ── SPI 读寄存器 ── */
int ad7124_read_reg(ad7124_t *d, uint8_t reg, uint8_t *buf, uint8_t len)
{
    uint8_t tx[5] = { 0x40 | (reg & 0x3F), 0, 0, 0, 0 };  /* 命令及读数据用的占位字节。 */
    uint8_t rx[5] = {0};
    if (!d || !buf || !d->cs_port || !d->spi || !d->spi->transfer ||
        len == 0U || len > 4U)
        return IF_ERR;

    _cs_low(d);
    int r = d->spi->transfer(d->spi, tx, rx, 1 + len);
    _cs_high(d);
    /* 事务间隔覆盖全/中/低功耗的t12，不能把CS脉冲间隔当作零。 */
    _wait_ms(d, 1U);
    if (r != IF_OK) return IF_ERR;
    memcpy(buf, rx + 1, len);  /* 命令阶段收到的rx[0]不是寄存器数据，跳过。 */
    return IF_OK;
}

/* ── SPI 写寄存器 ── */
int ad7124_write_reg(ad7124_t *d, uint8_t reg, uint8_t *buf, uint8_t len)
{
    uint8_t tx[5] = { reg & 0x3F, 0, 0, 0, 0 };
    if (!d || !buf || !d->cs_port || !d->spi || !d->spi->send ||
        len == 0U || len > 4U)
        return IF_ERR;
    memcpy(tx + 1, buf, len);

    _cs_low(d);
    int r = d->spi->send(d->spi, tx, 1 + len);
    _cs_high(d);
    /* 事务间隔覆盖全/中/低功耗的t12，不能把CS脉冲间隔当作零。 */
    _wait_ms(d, 1U);
    return r;
}

/* ── 辅助: 读 8-bit ── */
static inline int _rd8(ad7124_t *d, uint8_t reg, uint8_t *v)
{ return ad7124_read_reg(d, reg, v, 1); }

/* 先保存故障证据，再读ERROR清除锁存；本次数据始终拒绝交付。 */
static int check_status(ad7124_t *d, uint8_t status)
{
    d->last_status = status;
    if (status & AD7124_STATUS_POR_FLAG) {
        d->adc_mode = AD7124_MODE_UNKNOWN;
        d->num_ch = 0U;
    }
    if (status & AD7124_STATUS_ERROR_FLAG) {
        uint32_t error;
        if (ad7124_read_error(d, &error) != IF_OK) {
            d->last_error = AD7124_ERROR_UNREADABLE;
        }
        return IF_ERR;
    }
    return (status & AD7124_STATUS_POR_FLAG) ? IF_ERR : IF_OK;
}

/* 有界轮询；每次寄存器事务已保证至少1ms间隔，SPI耗时另计。 */
static int wait_ready(ad7124_t *d, uint32_t polls)
{
    for (uint32_t i = 0U; i < polls; ++i) {
        uint8_t status;
        int result = _rd8(d, AD7124_REG_STATUS, &status);
        if (result != IF_OK) return result;
        if (check_status(d, status) != IF_OK) return IF_ERR;
        if (!(status & AD7124_STATUS_RDY)) return IF_OK;
    }
    return IF_TIMEOUT;
}

/* DATA_STATUS在数据后附带状态；即使轮询时正常，读数据时也要再次判错。 */
static int read_conversion(ad7124_t *d, uint8_t data[4])
{
    int result = wait_ready(d, AD7124_SAMPLE_POLLS);
    if (result != IF_OK) return result;
    result = ad7124_read_reg(d, AD7124_REG_DATA, data, 4U);
    if (result != IF_OK) return result;
    return check_status(d, data[3]);
}

int ad7124_try_read_conversion(ad7124_t *d, uint32_t *raw, uint8_t *channel)
{
    uint8_t status;
    uint8_t data[4];
    if (!d || !raw || !channel || d->num_ch == 0U ||
        d->num_ch > AD7124_CHANNEL_COUNT ||
        (d->adc_mode != AD7124_MODE_CONT && d->adc_mode != AD7124_MODE_SINGLE))
        return IF_ERR;

    /* 只探测一次RDY，不在电压转换期间占住采样任务及PAC的访问机会。 */
    if (_rd8(d, AD7124_REG_STATUS, &status) != IF_OK ||
        check_status(d, status) != IF_OK) return IF_ERR;
    if (status & AD7124_STATUS_RDY) return 0;
    if (ad7124_read_reg(d, AD7124_REG_DATA, data, 4U) != IF_OK ||
        check_status(d, data[3]) != IF_OK) return IF_ERR;
    uint8_t active = AD7124_STATUS_CH_ACTIVE(data[3]);
    if (active >= d->num_ch) return IF_ERR;
    *raw = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
    *channel = active;
    return 1;
}

/* ── 辅助：读16位，高字节在前 ── */
static int _rd16(ad7124_t *d, uint8_t reg, uint16_t *v)
{
    uint8_t b[2];
    int r = ad7124_read_reg(d, reg, b, 2);
    if (r != IF_OK) return r;
    *v = ((uint16_t)b[0] << 8) | b[1];
    return IF_OK;
}

/* 诊断回读24位寄存器；失败不写输出。 */
static int _rd24(ad7124_t *d, uint8_t reg, uint32_t *v)
{
    uint8_t b[3];
    int result = ad7124_read_reg(d, reg, b, 3U);
    if (result != IF_OK) return result;
    *v = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
    return IF_OK;
}

/* ── 辅助：写16位，高字节在前 ── */
static int _wr16(ad7124_t *d, uint8_t reg, uint16_t v)
{
    uint8_t b[2] = { (v >> 8) & 0xFF, v & 0xFF };
    return ad7124_write_reg(d, reg, b, 2);
}

/* ── 辅助：写24位，高字节在前 ── */
static int _wr24(ad7124_t *d, uint8_t reg, uint32_t v)
{
    uint8_t b[3] = { (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF };
    return ad7124_write_reg(d, reg, b, 3);
}

/* 诊断使能必须回读一致，不能把SPI写成功当成配置已生效。 */
static int write_error_enable(ad7124_t *d, uint32_t mask)
{
    uint32_t actual;
    if (_wr24(d, AD7124_REG_ERROR_EN, mask) != IF_OK ||
        _rd24(d, AD7124_REG_ERROR_EN, &actual) != IF_OK || actual != mask)
        return IF_ERR;
    return IF_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * 复位: 64 SCLK + DIN=HIGH
 *   写 8 字节 0xFF → 轮询 STATUS.POR_FLAG 清零 → 等待配置就绪
 * ═══════════════════════════════════════════════════════════════════ */

int ad7124_reset(ad7124_t *d)
{
    uint8_t tx[8] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    if (!d || !d->cs_port || !d->spi || !d->spi->send ||
        !d->spi->transfer) return IF_ERR;
    d->adc_mode = AD7124_MODE_UNKNOWN;
    d->num_ch = 0U;
    _cs_low(d);
    int result = d->spi->send(d->spi, tx, sizeof(tx));
    _cs_high(d);
    _wait_ms(d, 1U);
    if (result != IF_OK) return result;

    /* POR结束后再留4ms内部配置余量；读失败或POR不退出都不能继续写配置。 */
    for (uint32_t poll = 0U; poll < 100U; ++poll) {
        uint8_t status;
        if (_rd8(d, AD7124_REG_STATUS, &status) != IF_OK) return IF_ERR;
        if ((status & AD7124_STATUS_POR_FLAG) == 0U) {
            _wait_ms(d, 4U);
            d->adc_mode = AD7124_MODE_CONT; /* ADC_CTRL复位值0，属于连续模式。 */
            return IF_OK;
        }
    }
    return IF_TIMEOUT;
}

/* ── 读 ID ── */
int ad7124_read_id(ad7124_t *d)
{
    uint8_t id;
    if (ad7124_read_reg(d, AD7124_REG_ID, &id, 1) != IF_OK) return IF_ERR;
    d->id = id;
    return (id == AD7124_ID_AD7124_4_STD ||
            id == AD7124_ID_AD7124_4_B ||
            id == AD7124_ID_AD7124_4_REV_E) ? IF_OK : IF_ERR;
}

/* ═══════════════════════════════════════════════════════════════════
 * 工作模式切换
 * ═══════════════════════════════════════════════════════════════════ */

int ad7124_set_mode(ad7124_t *d, uint8_t mode)
{
    /* UNKNOWN为软件故障标记，不能截断成保留模式后写入器件。 */
    if (!d || d->use_avdd_reference > 1U || mode > AD7124_MODE_CAL_SYS_F) return IF_ERR;
    uint16_t adc_ctrl = AD7124_ADC_CLK_SEL(AD7124_CLK_INT) |
                        AD7124_ADC_POWER_MODE(AD7124_PWR_FULL) |
                        (d->use_avdd_reference ? 0U : AD7124_ADC_REF_EN) |
                        AD7124_ADC_DATA_STATUS | AD7124_ADC_CS_EN |
                        AD7124_ADC_MODE(mode);

    int r = _wr16(d, AD7124_REG_ADC_CTRL, adc_ctrl);
    if (r == IF_OK) d->adc_mode = mode;
    return r;
}

/* ═══════════════════════════════════════════════════════════════════
 * 通道配置
 *   ch:     通道编号 (0-15)
 *   enable: 1=使能, 0=禁用
 *   setup:  关联配置组 (0-7)
 *   ainp:   正输入选择 (AD7124_AIN_xxx)
 *   ainm:   负输入选择 (AD7124_AIN_xxx)
 * ═══════════════════════════════════════════════════════════════════ */

int ad7124_config_channel(ad7124_t *d, uint8_t ch,
                          uint8_t enable, uint8_t setup,
                          uint8_t ainp, uint8_t ainm)
{
    if (ch > 15 || setup > 7) return IF_ERR;

    uint16_t val = AD7124_CH_SETUP(setup) |
                   AD7124_CH_AINP(ainp) |
                   AD7124_CH_AINM(ainm);
    if (enable) val |= AD7124_CH_ENABLE;

    return _wr16(d, AD7124_REG_CH0 + ch, val);
}

/* ═══════════════════════════════════════════════════════════════════
 * Setup 配置 (CFGx + FILTERx)
 *   setup:    配置组 (0-7), 同时写 CFGx, FILTERx
 *   pga:      增益 0-7 → 1,2,4,8,16,32,64,128
 *   bipolar:  1=双极性, 0=单极性
 *   ref_sel:  基准选择 (AD7124_REF_xxx)
 *   filter:   FILTERx高8位的原始位图，使用32位参数，FS由独立参数提供
 *   fs:       输出速率选择 (1-2047)
 * ===================================================================
 * 注: OFFSET/GAIN 校准值保持默认 (上电自动加载工厂校准值).
 *     如需自定义, 可直接调用 ad7124_write_reg() 写入.
 * ═══════════════════════════════════════════════════════════════════ */

int ad7124_config_setup(ad7124_t *d, uint8_t setup,
                         uint8_t pga, uint8_t bipolar,
                         uint8_t ref_sel,
                         uint32_t filter, uint16_t fs)
{
    uint32_t type = filter >> 21;
    if (setup > 7 || pga > 7 || ref_sel > 3 || fs == 0U || fs > 2047 ||
        (filter & ~0xFF0000UL) != 0U ||
        type == 1U || type == 3U || type == 6U) return IF_ERR;

    /* CFGx：使能所有缓冲器，设置基准、增益及双极性选项。 */
    uint16_t cfg = AD7124_CFG_REF_BUFP | AD7124_CFG_REF_BUFM |
                   AD7124_CFG_AIN_BUFP | AD7124_CFG_AIN_BUFM |
                   AD7124_CFG_REF_SEL(ref_sel) |
                   AD7124_CFG_PGA(pga);
    if (bipolar) cfg |= AD7124_CFG_BIPOLAR;

    /* FILTERx: 滤波器类型 + FS */
    uint32_t flt = filter | AD7124_FILT_FS(fs);

    int r = _wr16(d, AD7124_REG_CFG0 + setup, cfg);
    if (r != IF_OK) return r;
    return _wr24(d, AD7124_REG_FILTER0 + setup, flt);
}

/* ═══════════════════════════════════════════════════════════════════
 * 初始化：复位 → 读ID → 配置3路差分通道 → 逐路校零 → 连续扫描
 *   CH0: AIN0 - AIN1 → VCC_1V8_S3_LPDDR4X (VDD1, 1.8V, TPS62868 U4025)
 *   CH1: AIN2 - AIN3 → VDD2_DDR_S3       (VDD2, 1.1V, TPS62868 U4026)
 *   CH2: AIN4 - AIN5 → VDDQ_DDR          (VDDQ, 0.6V, TPS62868 U4027)
 *   AIN1/3/5由原理图接AGND，AIN6/7未使用。
 *
 *   默认：内部2.5V基准、SINC4、FS=384、增益1、单极性。
 *   单通道连续输出约50SPS；三通道扫描每通道需约80ms建立，整轮约240ms。
 *   使能 DATA_STATUS: 读 DATA 时状态字节附加在数据后 (第4字节)
 * ═══════════════════════════════════════════════════════════════════ */

static int initialize_device(ad7124_t *d)
{
    /* 正负端一一按B1原理图配对，不用内部AVSS代替板上AGND回路。 */
    static const uint8_t b1_ainp[3] = {
        AD7124_AIN_AIN0, AD7124_AIN_AIN2, AD7124_AIN_AIN4
    };
    static const uint8_t b1_ainm[3] = {
        AD7124_AIN_AIN1, AD7124_AIN_AIN3, AD7124_AIN_AIN5
    };

    if (!d || !d->spi || d->grounded_ainm > 1U || d->use_avdd_reference > 1U) return IF_ERR;
    uint8_t reference = d->use_avdd_reference ? AD7124_REF_AVDD : AD7124_REF_INT;
    d->init_stage = AD7124_INIT_RESET;
    if (ad7124_reset(d) != IF_OK) return IF_ERR;
    d->init_stage = AD7124_INIT_ID;
    if (ad7124_read_id(d) != IF_OK) return IF_ERR;

    /* AVDD模式关闭内部基准；三个CFGx均选择同一基准，切换MODE时也保持。 */
    d->init_stage = AD7124_INIT_CONTROL;
    if (_wr16(d, AD7124_REG_ADC_CTRL,
              AD7124_ADC_CLK_SEL(AD7124_CLK_INT) |
              AD7124_ADC_POWER_MODE(AD7124_PWR_FULL) |
              (d->use_avdd_reference ? 0U : AD7124_ADC_REF_EN) |
              AD7124_ADC_DATA_STATUS | AD7124_ADC_CS_EN |
              AD7124_ADC_MODE(AD7124_MODE_IDLE)) != IF_OK)
        return IF_ERR;
    d->adc_mode = AD7124_MODE_UNKNOWN;
    d->init_stage = AD7124_INIT_CONFIG;
    for (int ch = 0; ch < 3; ch++) {
        d->init_channel = (uint8_t)ch;
        /* CHx_MAP：使能原理图对应的AIN正/负端，setup=ch。 */
        if (_wr16(d, AD7124_REG_CH0 + ch,
                  AD7124_CH_ENABLE | AD7124_CH_AINP(b1_ainp[ch]) |
                  AD7124_CH_AINM(b1_ainm[ch]) |
                  AD7124_CH_SETUP(ch)) != IF_OK)
            return IF_ERR;

        /* 增益1、负端接AGND；校零时正端内部短接负端，双端均关闭缓冲，
         * 避免地电位低于缓冲器所需的AVSS+100mV。内部/AVDD基准无需REFIN缓冲。 */
        if (_wr16(d, AD7124_REG_CFG0 + ch,
                  AD7124_CFG_PGA(0) |
                  AD7124_CFG_REF_SEL(reference)) != IF_OK)
            return IF_ERR;

        /* SINC4、FS=384；切换通道需四个滤波周期建立，不是每路50SPS。 */
        if (_wr24(d, AD7124_REG_FILTER0 + ch,
                  AD7124_FILT_SINC4 | AD7124_FILT_FS(384)) != IF_OK)
            return IF_ERR;
    }

    /* ── 禁用未用通道 (仅保留 CH0-2; AIN1/3/5=AGND, 6/7=NC 原理图) ── */
    for (int ch = 3; ch < (int)AD7124_CHANNEL_COUNT; ch++) {
        if (_wr16(d, AD7124_REG_CH0 + ch, 0) != IF_OK) return IF_ERR;
    }

    /* 先完成输入配置，再使能诊断；按手册读取ERROR清除使能时的锁存。 */
    d->init_stage = AD7124_INIT_DIAGNOSTICS;
    d->init_channel = 0xFFU;
    /* 接地负端位于UV比较器边界，B1关闭负端UV；正端UV及其它诊断保留。 */
    uint32_t diagnostics = AD7124_DIAGNOSTIC_MASK;
    if (d->grounded_ainm) diagnostics &= ~AD7124_ERR_AINM_UV;
    if (write_error_enable(d, diagnostics) != IF_OK) return IF_ERR;

    /* 清除初始化锁存后逐个校准setup；增益1只校零位，保留出厂GAIN。
     * 三路全部成功才允许采样，校准期间的读数不作为测量结果交付。 */
    uint32_t startup_error;
    if (ad7124_read_error(d, &startup_error) != IF_OK) return IF_ERR;
    d->init_stage = AD7124_INIT_CALIBRATION;
    for (uint8_t ch = 0U; ch < 3U; ++ch) {
        d->init_channel = ch;
        int result = calibrate_channel(d, ch, AD7124_INIT_CAL_POLLS);
        if (result != IF_OK) {
            d->adc_mode = AD7124_MODE_UNKNOWN;
            d->num_ch = 0U;
            return result;
        }
    }
    d->init_stage = AD7124_INIT_CONTINUOUS;
    d->init_channel = 0xFFU;
    if (ad7124_set_mode(d, AD7124_MODE_CONT) != IF_OK) {
        d->adc_mode = AD7124_MODE_UNKNOWN;
        return IF_ERR;
    }
    d->num_ch = 3U;
    d->init_stage = AD7124_INIT_READY;
    return IF_OK;
}

int ad7124_init(ad7124_t *d)
{
    if (!d) return IF_ERR;
    d->init_stage = AD7124_INIT_NONE;
    d->init_channel = 0xFFU;
    d->current_error = 0U;
    d->last_status = 0U;
    memset(&d->cal_snapshot, 0, sizeof(d->cal_snapshot));
    d->num_ch = 0U;
    d->adc_mode = AD7124_MODE_UNKNOWN;
    d->init_result = initialize_device(d);
    return d->init_result;
}

/* ═══════════════════════════════════════════════════════════════════
 * 内部校准（Rev.E第53页）：增益1只做零位，保留工厂增益；其它增益
 * 先将OFFSET置0x800000，再在低/中功耗下校满量程，最后校零位。
 * 保存全部16个通道、ADC_CTRL和本setup系数，禁止从未读成功的备份恢复。
 * 校准启动后的错误使句柄失效；不在器件仍忙时写恢复值，调用方须重新初始化。
 * ═══════════════════════════════════════════════════════════════════ */

static int calibrate_channel(ad7124_t *d, uint8_t ch, uint32_t polls)
{
    uint16_t channels[AD7124_CHANNEL_COUNT], control, config, idle_readback;
    uint8_t saved_offset[3], saved_gain[3];
    int result, restore_failed = 0;
    uint8_t calibration_started = 0U;
    uint8_t diagnostics_changed = 0U;
    if (!d || !d->cs_port || !d->spi || !d->spi->send || !d->spi->transfer ||
        ch >= AD7124_CHANNEL_COUNT) return IF_ERR;
    memset(&d->cal_snapshot, 0, sizeof(d->cal_snapshot));

    /* 所有备份都读完才开始改动；不接管正在校准/掉电/连续读的设备。 */
    result = _rd16(d, AD7124_REG_ADC_CTRL, &control);
    if (result != IF_OK) return result;
    uint8_t original_mode = (control >> 2) & 0x0FU;
    if (original_mode > AD7124_MODE_IDLE || original_mode == AD7124_MODE_POWERDOWN ||
        (control & AD7124_ADC_CONT_READ)) return IF_ERR;
    for (uint8_t i = 0U; i < AD7124_CHANNEL_COUNT; ++i) {
        result = _rd16(d, AD7124_REG_CH0 + i, &channels[i]);
        if (result != IF_OK) return result;
    }
    uint8_t setup = (channels[ch] >> 12) & 7U;
    if ((result = _rd16(d, AD7124_REG_CFG0 + setup, &config)) != IF_OK ||
        (result = ad7124_read_reg(d, AD7124_REG_OFFSET0 + setup, saved_offset, 3U)) != IF_OK ||
        (result = ad7124_read_reg(d, AD7124_REG_GAIN0 + setup, saved_gain, 3U)) != IF_OK)
        return result;

    uint16_t base = control & (uint16_t)~AD7124_ADC_MODE(0xFU);
    uint16_t cal_base = base;
    if ((config & 7U) != 0U && ((base >> 6) & 3U) >= AD7124_PWR_FULL) {
        cal_base = (base & (uint16_t)~AD7124_ADC_POWER_MODE(3U)) |
                   AD7124_ADC_POWER_MODE(AD7124_PWR_MID);
    }
    /* 写失败也可能已部分生效；在完整恢复前不允许把软件模式当作可信状态。 */
    d->adc_mode = AD7124_MODE_UNKNOWN;
    result = _wr16(d, AD7124_REG_ADC_CTRL, base | AD7124_ADC_MODE(AD7124_MODE_IDLE));
    if (result != IF_OK) goto restore;
    for (uint8_t i = 0U; i < AD7124_CHANNEL_COUNT; ++i) {
        uint16_t value = channels[i] & (uint16_t)~AD7124_CH_ENABLE;
        if (i == ch) value |= AD7124_CH_ENABLE;
        result = _wr16(d, AD7124_REG_CH0 + i, value);
        if (result != IF_OK) goto restore;
    }

    /* 在校准前采集实际配置和错误；校准忙后不额外访问配置寄存器。
     * ERROR读取清锁存，但只留诊断证据，不据此忽略后续校准/采样错误。 */
    ad7124_cal_snapshot_t *snapshot = &d->cal_snapshot;
    if ((result = _rd16(d, AD7124_REG_ADC_CTRL, &snapshot->control)) != IF_OK ||
        (result = _rd16(d, AD7124_REG_CH0 + ch, &snapshot->channel)) != IF_OK ||
        (result = _rd16(d, AD7124_REG_CFG0 + setup, &snapshot->config)) != IF_OK ||
        (result = _rd24(d, AD7124_REG_FILTER0 + setup, &snapshot->filter)) != IF_OK ||
        (result = _rd24(d, AD7124_REG_ERROR_EN, &snapshot->error_enable)) != IF_OK ||
        (result = _rd8(d, AD7124_REG_STATUS, &snapshot->status)) != IF_OK ||
        (result = ad7124_read_error(d, &snapshot->error)) != IF_OK) goto restore;
    snapshot->valid = 1U;
    if (snapshot->control != (base | AD7124_ADC_MODE(AD7124_MODE_IDLE)) ||
        snapshot->channel != (channels[ch] | AD7124_CH_ENABLE) ||
        snapshot->config != config || (snapshot->status & AD7124_STATUS_POR_FLAG)) {
        result = IF_ERR;
        goto restore;
    }

    if (d->grounded_ainm) {
        /* 只适用已确认的B1三路接地输入；变更通道或增益后必须重新评审。 */
        uint8_t reference = d->use_avdd_reference ? AD7124_REF_AVDD : AD7124_REF_INT;
        uint16_t expected = AD7124_CH_ENABLE | AD7124_CH_SETUP(ch) |
                            AD7124_CH_AINP(ch * 2U) | AD7124_CH_AINM(ch * 2U + 1U);
        if (d->grounded_ainm != 1U || d->use_avdd_reference > 1U || ch >= 3U ||
            snapshot->channel != expected || config != AD7124_CFG_REF_SEL(reference) ||
            (control & AD7124_ADC_REF_EN) != (d->use_avdd_reference ? 0U : AD7124_ADC_REF_EN) ||
            (snapshot->error & ~(AD7124_ERR_AINP_UV | AD7124_ERR_AINM_UV))) {
            result = IF_ERR;
            goto restore;
        }
        /* 内部校零把正端短到接地负端，期间两端UV均不适用；不屏蔽其它错误。 */
        diagnostics_changed = 1U;
        uint32_t error;
        if ((result = write_error_enable(d, snapshot->error_enable &
                ~(AD7124_ERR_AINP_UV | AD7124_ERR_AINM_UV))) != IF_OK ||
            (result = ad7124_read_error(d, &error)) != IF_OK) goto restore;
        if (error & ~(AD7124_ERR_AINP_UV | AD7124_ERR_AINM_UV)) {
            result = IF_ERR;
            goto restore;
        }
    }

    if ((config & 7U) != 0U) {
        result = _wr24(d, AD7124_REG_OFFSET0 + setup, 0x800000UL);
        if (result != IF_OK) goto restore;
        /* SPI报错时器件也可能已启动，之后任何异常都不能盲写恢复寄存器。 */
        calibration_started = 1U;
        result = _wr16(d, AD7124_REG_ADC_CTRL,
                       cal_base | AD7124_ADC_MODE(AD7124_MODE_CAL_INT_F));
        if (result != IF_OK) goto restore;
        result = wait_ready(d, polls);
        if (result != IF_OK) goto restore;
    }
    calibration_started = 1U;
    result = _wr16(d, AD7124_REG_ADC_CTRL,
                   cal_base | AD7124_ADC_MODE(AD7124_MODE_CAL_INT_Z));
    if (result == IF_OK) result = wait_ready(d, polls);

restore:
    /* 校准忙时寄存器访问受限；失败后保持未知，不以SPI写成功冒充退出校准。 */
    if (calibration_started && result != IF_OK) goto invalid;
    /* 只在校准已正常结束或尚未启动时恢复，并确认IDLE配置确实生效。 */
    if (_wr16(d, AD7124_REG_ADC_CTRL,
              base | AD7124_ADC_MODE(AD7124_MODE_IDLE)) != IF_OK ||
        _rd16(d, AD7124_REG_ADC_CTRL, &idle_readback) != IF_OK ||
        idle_readback != (base | AD7124_ADC_MODE(AD7124_MODE_IDLE))) goto invalid;
    /* 校准成功/未启动才恢复原诊断；失败且可能仍忙时留待下次复位重初始化。 */
    if (diagnostics_changed) {
        uint32_t error;
        if (write_error_enable(d, d->cal_snapshot.error_enable) != IF_OK ||
            ad7124_read_error(d, &error) != IF_OK ||
            (error & ~(AD7124_ERR_AINP_UV | AD7124_ERR_AINM_UV))) goto invalid;
    }
    if (result != IF_OK) {
        if (ad7124_write_reg(d, AD7124_REG_OFFSET0 + setup, saved_offset, 3U) != IF_OK)
            restore_failed = 1;
        if (ad7124_write_reg(d, AD7124_REG_GAIN0 + setup, saved_gain, 3U) != IF_OK)
            restore_failed = 1;
    }
    /* 一项恢复失败仍尝试其余通道；有失败就保持空闲，不恢复连续转换。 */
    for (uint8_t i = 0U; i < AD7124_CHANNEL_COUNT; ++i) {
        if (_wr16(d, AD7124_REG_CH0 + i, channels[i]) != IF_OK) restore_failed = 1;
    }
    if (restore_failed || _wr16(d, AD7124_REG_ADC_CTRL, control) != IF_OK) goto invalid;
    d->adc_mode = original_mode;
    return result;

invalid:
    d->adc_mode = AD7124_MODE_UNKNOWN;
    d->num_ch = 0U;
    return result == IF_TIMEOUT ? IF_TIMEOUT : IF_ERR;
}

int ad7124_calibrate(ad7124_t *d, uint8_t ch)
{
    /* 维护接口仍覆盖低功耗、低速率；初始化使用固定配置对应的较短预算。 */
    return calibrate_channel(d, ch, AD7124_CAL_POLLS);
}

/* ═══════════════════════════════════════════════════════════════════
 * 读单通道转换结果 (24-bit)
 *
 * 启用 DATA_STATUS 时 (ADC_CTRL bit10=1):
 *   读 DATA 返回 4 字节: [DATA23:16] [DATA15:8] [DATA7:0] [STATUS]
 *   (STATUS 附加在数据之后, CH_ACTIVE 字段指示当前通道)
 *
 * 连续模式：最多读取16次转换寻找目标通道。
 * 单次模式：只读取一次，同样核对状态中的通道号；不暗中重新触发转换。
 * ═══════════════════════════════════════════════════════════════════ */

int ad7124_read_ch(ad7124_t *d, uint8_t ch, int32_t *raw)
{
    if (!d || !raw || ch >= AD7124_CHANNEL_COUNT ||
        (d->adc_mode != AD7124_MODE_CONT && d->adc_mode != AD7124_MODE_SINGLE))
        return IF_ERR;

    uint8_t attempts = d->adc_mode == AD7124_MODE_CONT ? AD7124_CHANNEL_COUNT : 1U;
    for (uint8_t i = 0U; i < attempts; ++i) {
        uint8_t data[4];
        int result = read_conversion(d, data);
        if (result != IF_OK) return result;
        /* 单次模式同样必须核对通道，不能把另一通道的数据标成目标通道。 */
        if (AD7124_STATUS_CH_ACTIVE(data[3]) == ch) {
            *raw = (int32_t)(((uint32_t)data[0] << 16) |
                            ((uint32_t)data[1] << 8) | data[2]);
            return IF_OK;
        }
    }
    return IF_TIMEOUT;
}

/* ═══════════════════════════════════════════════════════════════════
 * 完整快照：先在局部数组按CH_ACTIVE归位，收齐才交付，失败不改raw。
 * got只用于统计不同通道的收集数；失败时不能按got消费输出前缀。
 * 连续通道约定为0..num_ch-1；单次模式仅支持num_ch=1，不暗中重触发ADC。
 * ═══════════════════════════════════════════════════════════════════ */

int ad7124_read_all(ad7124_t *d, int32_t *raw, uint8_t max_ch, uint8_t *got)
{
    int32_t values[AD7124_CHANNEL_COUNT];
    uint8_t collected = 0;
    uint16_t mask = 0U;
    int result = IF_TIMEOUT;
    if (got) *got = 0U;
    if (!d || !raw || d->num_ch == 0U || d->num_ch > AD7124_CHANNEL_COUNT ||
        max_ch < d->num_ch ||
        (d->adc_mode != AD7124_MODE_CONT &&
         !(d->adc_mode == AD7124_MODE_SINGLE && d->num_ch == 1U))) return IF_ERR;

    for (uint8_t attempt = 0U; attempt < d->num_ch * 4U; ++attempt) {
        uint8_t data[4];
        result = read_conversion(d, data);
        if (result != IF_OK) break;
        uint8_t channel = AD7124_STATUS_CH_ACTIVE(data[3]);
        result = IF_TIMEOUT;
        if (channel >= d->num_ch || (mask & (1U << channel))) continue;
        values[channel] = (int32_t)(((uint32_t)data[0] << 16) |
                                   ((uint32_t)data[1] << 8) | data[2]);
        mask |= (uint16_t)(1U << channel);
        collected++;
        if (collected == d->num_ch) {
            memcpy(raw, values, d->num_ch * sizeof(values[0]));
            result = IF_OK;
            break;
        }
    }
    if (got) *got = collected;
    return result;
}

/* ═══════════════════════════════════════════════════════════════════
 * 读 ERROR 寄存器 (诊断)
 * ═══════════════════════════════════════════════════════════════════ */

int ad7124_read_error(ad7124_t *d, uint32_t *err_flags)
{
    uint8_t rx[3];
    if (!d || !err_flags) return IF_ERR;
    if (ad7124_read_reg(d, AD7124_REG_ERROR, rx, 3) != IF_OK) {
        d->current_error = AD7124_ERROR_UNREADABLE;
        return IF_ERR;
    }
    *err_flags = ((uint32_t)rx[0] << 16) | ((uint32_t)rx[1] << 8) | rx[2];
    d->current_error = *err_flags;
    if (*err_flags != 0U) d->last_error = *err_flags;
    return IF_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * 原始值 → 电压 (float)
 *
 * 输入 raw 为 ad7124_read_ch/read_all 返回的原始 24-bit 码 (0..0xFFFFFF):
 *   双极性（偏移二进制）: 0x000000=-FS, 0x800000=0V, 0xFFFFFF接近+FS
 *   VIN = (code - 2^23) / 2^23 * VREF / GAIN
 * 单极性（直接二进制）:
 *   VIN = code / (2^24) * VREF / GAIN
 *
 * vref:    基准电压 (V), 外部=2.5, 内部=2.5
 * pga:     PGA 设置 (0-7, 对应增益 1/2/4/8/16/32/64/128)
 * bipolar: 1=双极性, 0=单极性
 * ═══════════════════════════════════════════════════════════════════ */

float ad7124_raw_to_voltage(int32_t raw, float vref, uint8_t pga, uint8_t bipolar)
{
    static const uint8_t gain_map[] = { 1,2,4,8,16,32,64,128 };
    float gain = (float)gain_map[pga & 0x7];

    if (bipolar) {
        /* 偏移二进制：0对应负满量程，0x800000对应0V，0xFFFFFF接近正满量程。 */
        float v = ((float)(raw - 0x800000)) / 8388608.0f * vref / gain;
        return v;
    } else {
        /* 直接二进制：0对应0V，0xFFFFFF比满量程VREF/gain低一个LSB。 */
        float v = ((float)raw) / 16777216.0f * vref / gain;
        return v;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * 原始值 → mV (int32_t, 整数, 协议上报用)
 *
 * vref 为所选基准电压，单位 V。返回 ADC 差分输入端的整数 mV。
 * ═══════════════════════════════════════════════════════════════════ */

int32_t ad7124_raw_to_mv(int32_t raw, float vref, uint8_t pga, uint8_t bipolar)
{
    float v = ad7124_raw_to_voltage(raw, vref, pga, bipolar);
    return (int32_t)(v * 1000.0f);
}

float ad7124_raw_to_voltage_div(int32_t raw, float vref, uint8_t pga,
                                uint8_t bipolar, float divider)
{
    float v = ad7124_raw_to_voltage(raw, vref, pga, bipolar);
    return v * divider;
}

int32_t ad7124_raw_to_mv_div(int32_t raw, float vref, uint8_t pga,
                             uint8_t bipolar, float divider)
{
    return (int32_t)(ad7124_raw_to_voltage_div(raw, vref, pga, bipolar, divider) * 1000.0f);
}

/* ── 读单通道, 直接返回 mV ── */

int ad7124_read_ch_mv(ad7124_t *d, uint8_t ch, float vref, uint8_t pga,
                       uint8_t bipolar, int32_t *mv)
{
    int32_t raw;
    /* 上限只防毫伏转换溢出；器件输入/基准的实际电气限值由调用方保证。 */
    if (!mv || !isfinite(vref) || vref <= 0.0f || vref >= 2147483.0f || pga > 7U)
        return IF_ERR;
    int r = ad7124_read_ch(d, ch, &raw);
    if (r != IF_OK) return r;
    *mv = ad7124_raw_to_mv(raw, vref, pga, bipolar);
    return IF_OK;
}

/* ── 批量读取, 直接返回 mV (每通道独立 pga/bipolar) ── */

int ad7124_read_all_mv(ad7124_t *d, int32_t *mv, uint8_t max_ch, uint8_t *got,
                        float vref, const uint8_t *pga, const uint8_t *bipolar)
{
    int32_t raw[AD7124_CHANNEL_COUNT];
    if (got) *got = 0U;
    if (!d || !mv || d->num_ch == 0U || d->num_ch > AD7124_CHANNEL_COUNT ||
        max_ch < d->num_ch || !isfinite(vref) || vref <= 0.0f ||
        vref >= 2147483.0f) return IF_ERR;
    for (uint8_t i = 0U; pga && i < d->num_ch; ++i) {
        if (pga[i] > 7U) return IF_ERR;
    }
    int r = ad7124_read_all(d, raw, max_ch, got);
    if (r != IF_OK) return r;
    /* read_all只交付完整快照，所有索引都已有转换值。 */
    for (uint8_t i = 0; i < d->num_ch; i++) {
        uint8_t p = pga     ? pga[i]    : 0;
        uint8_t b = bipolar ? bipolar[i] : 0;   /* 默认单极性，B1三轨均为正电压。 */
        mv[i] = ad7124_raw_to_mv(raw[i], vref, p, b);
    }
    return IF_OK;
}
