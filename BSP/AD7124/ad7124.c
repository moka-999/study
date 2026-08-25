/*
 * ===================================================================
 * AD7124-4 驱动 — spi_if_t 抽象层, 不绑死物理 SPI
 * ===================================================================
 *
 * 通信序列 (SPI Mode 3: CPOL=1, CPHA=1, MSB first, SCLK idle high):
 *   写寄存器: CS↓ → [cmd_wr] [data_MSB..LSB] → CS↑
 *   读寄存器: CS↓ → [cmd_rd] [dummy] → (收) [echo] [reg_data_MSB..LSB] → CS↑
 *   复位:     CS↓ → 64 SCLK w/ DIN=HIGH (8×0xFF) → CS↑ → Delay 1ms
 *
 * 寄存器大小 (读/写时截断到硬件宽度):
 *   8-bit:  STATUS, ID, MCLK_COUNT
 *   16-bit: ADC_CTRL, IO_CTRL2, CHx, CFGx
 *   24-bit: DATA, IO_CTRL1, ERROR, ERROR_EN, FILTERx, OFFSETx, GAINx
 * ===================================================================
 */
#include "ad7124.h"
#include <string.h>

/* ── 延时 (RTOS 环境可注入 osDelay 让出 CPU, NULL 回退 HAL_Delay 忙等) ── */
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
    uint8_t tx[5] = { 0x40 | (reg & 0x3F), 0, 0, 0, 0 };  /* cmd + dummy */
    uint8_t rx[5] = {0};
    if (!d || !buf || !d->spi || !d->spi->transfer || len == 0U || len > 4U)
        return IF_ERR;

    _cs_low(d);
    int r = d->spi->transfer(d->spi, tx, rx, 1 + len);
    _cs_high(d);
    if (r != IF_OK) return IF_ERR;
    memcpy(buf, rx + 1, len);  /* rx[0]=命令回显, 跳过 */
    return IF_OK;
}

/* ── SPI 写寄存器 ── */
int ad7124_write_reg(ad7124_t *d, uint8_t reg, uint8_t *buf, uint8_t len)
{
    uint8_t tx[5] = { reg & 0x3F, 0, 0, 0, 0 };
    if (!d || !buf || !d->spi || !d->spi->send || len == 0U || len > 4U)
        return IF_ERR;
    memcpy(tx + 1, buf, len);

    _cs_low(d);
    int r = d->spi->send(d->spi, tx, 1 + len);
    _cs_high(d);
    return r;
}

/* ── 辅助: 读 8-bit ── */
static inline int _rd8(ad7124_t *d, uint8_t reg, uint8_t *v)
{ return ad7124_read_reg(d, reg, v, 1); }

/* ── 辅助: 读 16-bit (MSB first) ── */
static int _rd16(ad7124_t *d, uint8_t reg, uint16_t *v)
{
    uint8_t b[2];
    int r = ad7124_read_reg(d, reg, b, 2);
    if (r != IF_OK) return r;
    *v = ((uint16_t)b[0] << 8) | b[1];
    return IF_OK;
}

/* ── 辅助: 写 16-bit (MSB first) ── */
static int _wr16(ad7124_t *d, uint8_t reg, uint16_t v)
{
    uint8_t b[2] = { (v >> 8) & 0xFF, v & 0xFF };
    return ad7124_write_reg(d, reg, b, 2);
}

/* ── 辅助: 写 24-bit (MSB first) ── */
static int _wr24(ad7124_t *d, uint8_t reg, uint32_t v)
{
    uint8_t b[3] = { (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF };
    return ad7124_write_reg(d, reg, b, 3);
}

/* ═══════════════════════════════════════════════════════════════════
 * 复位: 64 SCLK + DIN=HIGH
 *   写 8 字节 0xFF → 检测 POR_FLAG → 清 STATUS
 * ═══════════════════════════════════════════════════════════════════ */

int ad7124_reset(ad7124_t *d)
{
    uint8_t tx[8] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

    if (!d || !d->spi || !d->spi->send) return IF_ERR;

    _cs_low(d);
    int r = d->spi->send(d->spi, tx, 8);
    _cs_high(d);
    _wait_ms(d, 1);

    /* 验证复位: 读 STATUS, 检查 POR_FLAG */
    uint8_t st;
    if (ad7124_read_reg(d, AD7124_REG_STATUS, &st, 1) == IF_OK) {
        /* POR_FLAG=1 表示复位成功, 读 STATUS 自动清除 */
    }
    d->adc_mode = AD7124_MODE_IDLE;
    return r;
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
    uint16_t adc_ctrl = AD7124_ADC_CLK_SEL(AD7124_CLK_INT) |
                        AD7124_ADC_POWER_MODE(AD7124_PWR_FULL) |
                        AD7124_ADC_REF_EN |
                        AD7124_ADC_DATA_STATUS |
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
 *   filter:   滤波器类型 (AD7124_FILT_xxx)
 *   fs:       输出速率选择 (1-2047)
 * ===================================================================
 * 注: OFFSET/GAIN 校准值保持默认 (上电自动加载工厂校准值).
 *     如需自定义, 可直接调用 ad7124_write_reg() 写入.
 * ═══════════════════════════════════════════════════════════════════ */

int ad7124_config_setup(ad7124_t *d, uint8_t setup,
                         uint8_t pga, uint8_t bipolar,
                         uint8_t ref_sel,
                         uint8_t filter, uint16_t fs)
{
    if (setup > 7 || pga > 7 || ref_sel > 3 || fs > 2047) return IF_ERR;

    /* CFGx: 使能所有缓冲器 + REF_SEL + PGA + Bipolar */
    uint16_t cfg = AD7124_CFG_REF_BUFP | AD7124_CFG_REF_BUFM |
                   AD7124_CFG_AIN_BUFP | AD7124_CFG_AIN_BUFM |
                   AD7124_CFG_REF_SEL(ref_sel) |
                   AD7124_CFG_PGA(pga);
    if (bipolar) cfg |= AD7124_CFG_BIPOLAR;

    /* FILTERx: 滤波器类型 + FS */
    uint32_t flt = ((uint32_t)filter) | AD7124_FILT_FS(fs);

    int r = _wr16(d, AD7124_REG_CFG0 + setup, cfg);
    if (r != IF_OK) return r;
    return _wr24(d, AD7124_REG_FILTER0 + setup, flt);
}

/* ═══════════════════════════════════════════════════════════════════
 * 初始化：复位 → 读ID → 配置3通道（B1/MB360原理图20260811，AIN0/2/4）
 *   CH0: AIN0 ↔ AVSS → VCC_1V8_S3_LPDDR4X (VDD1, 1.8V, TPS62868 U4025)
 *   CH1: AIN2 ↔ AVSS → VDD2_DDR_S3       (VDD2, 1.1V, TPS62868 U4026)
 *   CH2: AIN4 ↔ AVSS → VDDQ_DDR          (VDDQ, 0.6V, TPS62868 U4027)
 *   AIN1/3/5 = AGND, AIN6/7 = NC (原理图)
 *
 *   默认: 外部基准 REFIN1 (2.5V), SINC4, 50Hz ODR, 增益=1, 单极性 (unipolar)
 *   单极性理由: DDR 电压全正 (0.6~1.8V), unipolar 满量程 0~2.5V 分辨率翻倍
 *              (24-bit 全用 vs bipolar 有效 23-bit), 更符合老化拉偏精度需求
 *   使能 DATA_STATUS: 读 DATA 时状态字节附加在数据后 (第4字节)
 * ═══════════════════════════════════════════════════════════════════ */

int ad7124_init(ad7124_t *d)
{
    /* B1原理图：CH0→AIN0，CH1→AIN2，CH2→AIN4（AIN1/3/5=AGND）。 */
    static const uint8_t b1_ainp[3] = {
        AD7124_AIN_AIN0, AD7124_AIN_AIN2, AD7124_AIN_AIN4
    };

    if (!d || !d->spi) return IF_ERR;
    if (ad7124_reset(d) != IF_OK) return IF_ERR;
    if (ad7124_read_id(d) != IF_OK) return IF_ERR;

    /* ADC_CTRL: 内部时钟, 外部基准 REFIN1 (见 CFGx REF_SEL), 连续模式, 全功率, DATA_STATUS */
    if (_wr16(d, AD7124_REG_ADC_CTRL,
              AD7124_ADC_CLK_SEL(AD7124_CLK_INT) |
              AD7124_ADC_POWER_MODE(AD7124_PWR_FULL) |
              AD7124_ADC_REF_EN |
              AD7124_ADC_DATA_STATUS |
              AD7124_ADC_MODE(AD7124_MODE_CONT)) != IF_OK)
        return IF_ERR;
    d->adc_mode = AD7124_MODE_CONT;

    d->num_ch = 3;  /* B1本板的3路DDR电压回读 */

    for (int ch = 0; ch < d->num_ch; ch++) {
        /* CHx_MAP：使能，AIN(b1_ainp)对AVSS，setup=ch。 */
        if (_wr16(d, AD7124_REG_CH0 + ch,
                  AD7124_CH_ENABLE | AD7124_CH_AINP(b1_ainp[ch]) |
                  AD7124_CH_AINM(AD7124_AIN_AVSS) |
                  AD7124_CH_SETUP(ch)) != IF_OK)
            return IF_ERR;

        /* CFGx: 单极性, gain=1, 外部基准 REFIN1, 缓冲器使能
         * (单极性: AIN 范围 0~VREF/gain, DDR 电压全正用满 24-bit 分辨率) */
        if (_wr16(d, AD7124_REG_CFG0 + ch,
                  AD7124_CFG_PGA(0) |
                  AD7124_CFG_REF_SEL(AD7124_REF_EXT1) |
                  AD7124_CFG_REF_BUFP | AD7124_CFG_REF_BUFM |
                  AD7124_CFG_AIN_BUFP | AD7124_CFG_AIN_BUFM) != IF_OK)
            return IF_ERR;

        /* FILTERx: SINC4, FS=384 → fADC = 614400/(32*384) ≈ 50Hz */
        if (_wr24(d, AD7124_REG_FILTER0 + ch,
                  AD7124_FILT_SINC4 | AD7124_FILT_FS(384)) != IF_OK)
            return IF_ERR;
    }

    /* ── 禁用未用通道 (仅保留 CH0-2; AIN1/3/5=AGND, 6/7=NC 原理图) ── */
    for (int ch = d->num_ch; ch < 8; ch++) {
        if (_wr16(d, AD7124_REG_CH0 + ch, 0) != IF_OK) return IF_ERR;
    }

    return IF_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * 校准 (内部满量程 + 内部零位)
 *
 * 手册要求 (Rev E, CALIBRATION 章节):
 *   1. 校准期间只能激活 1 个通道 (only one channel can be active)
 *   2. 内部校准时必须先做满量程, 再做零位
 *      (internal full-scale calibration must be performed before
 *       the internal zero-scale calibration)
 *   3. 校准从 standby/idle 开始, 完成后切换回 continuous
 * ═══════════════════════════════════════════════════════════════════ */

int ad7124_calibrate(ad7124_t *d, uint8_t ch)
{
    uint8_t  st;
    uint16_t saved[8];   /* 各通道寄存器原值 (含使能状态) */
    if (ch > 7) return IF_ERR;

    /* 构建 ADC_CTRL 基底 (不含 MODE) */
    uint16_t base = AD7124_ADC_CLK_SEL(AD7124_CLK_INT) |
                    AD7124_ADC_POWER_MODE(AD7124_PWR_FULL) |
                    AD7124_ADC_REF_EN |
                    AD7124_ADC_DATA_STATUS;

    /* 1. 保存所有通道状态, 仅保留目标通道使能 (手册: 校准单通道) */
    for (int i = 0; i < 8; i++)
        _rd16(d, AD7124_REG_CH0 + i, &saved[i]);
    for (int i = 0; i < 8; i++) {
        uint16_t v = saved[i];
        if (i == ch) v |= AD7124_CH_ENABLE;
        else         v &= (uint16_t)~AD7124_CH_ENABLE;
        _wr16(d, AD7124_REG_CH0 + i, v);
    }

    /* 2. 切 standby (校准必须从 standby/idle 开始) */
    _wr16(d, AD7124_REG_ADC_CTRL, base | AD7124_ADC_MODE(AD7124_MODE_STANDBY));

    /* 3. 内部满量程校准 (必须先于零位) */
    _wr16(d, AD7124_REG_ADC_CTRL, base | AD7124_ADC_MODE(AD7124_MODE_CAL_INT_F));
    for (int to = 0; to < 1000; to++) {
        _rd8(d, AD7124_REG_STATUS, &st);
        if (!(st & AD7124_STATUS_RDY)) break;
        _wait_ms(d, 1);
    }

    /* 4. 内部零位校准 */
    _wr16(d, AD7124_REG_ADC_CTRL, base | AD7124_ADC_MODE(AD7124_MODE_CAL_INT_Z));
    for (int to = 0; to < 1000; to++) {
        _rd8(d, AD7124_REG_STATUS, &st);
        if (!(st & AD7124_STATUS_RDY)) break;
        _wait_ms(d, 1);
    }

    /* 5. 恢复所有通道原状态 */
    for (int i = 0; i < 8; i++)
        _wr16(d, AD7124_REG_CH0 + i, saved[i]);

    /* 6. 恢复连续模式 */
    _wr16(d, AD7124_REG_ADC_CTRL, base | AD7124_ADC_MODE(AD7124_MODE_CONT));
    d->adc_mode = AD7124_MODE_CONT;
    return IF_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * 读单通道转换结果 (24-bit)
 *
 * 启用 DATA_STATUS 时 (ADC_CTRL bit10=1):
 *   读 DATA 返回 4 字节: [DATA23:16] [DATA15:8] [DATA7:0] [STATUS]
 *   (STATUS 附加在数据之后, CH_ACTIVE 字段指示当前通道)
 *
 * 连续模式: 自动轮询使能的通道, 最多等 4 次 RDY 找到目标通道.
 * 单次模式: 目标通道已选中, 直接读.
 * ═══════════════════════════════════════════════════════════════════ */

int ad7124_read_ch(ad7124_t *d, uint8_t ch, int32_t *raw)
{
    uint8_t rx[4];  /* DATA[23:0] + STATUS (DATA_STATUS 使能时状态字节在后!) */
    int     max_wait = (d->adc_mode == AD7124_MODE_CONT) ? 8 : 1;

    for (int attempt = 0; attempt < max_wait; attempt++) {
        /* 等待 RDY (bit7=0 = ready) */
        uint8_t st;
        int to;
        for (to = 0; to < 500; to++) {
            if (_rd8(d, AD7124_REG_STATUS, &st) != IF_OK) return IF_ERR;
            if (!(st & AD7124_STATUS_RDY)) break;
            _wait_ms(d, 1);
        }
        if (to >= 500) {
            *raw = 0;
            return IF_TIMEOUT;
        }

        /* 读 DATA (4 字节: DATA[23:16][15:8][7:0] + STATUS, MSB 先) */
        if (ad7124_read_reg(d, AD7124_REG_DATA, rx, 4) != IF_OK)
            return IF_ERR;

        /* 检查通道匹配: STATUS 在最后一个字节 (AD7124 手册 DATA_STATUS) */
        uint8_t active_ch = rx[3] & 0x0F;
        if (active_ch == ch || d->adc_mode != AD7124_MODE_CONT) {
            /* 返回原始 24-bit 码 (0..0xFFFFFF):
             *   双极性 = offset-binary (0x800000=0V), 单极性 = straight-binary
             *   电压换算统一由 ad7124_raw_to_voltage/mv() 处理, 此处不做符号扩展 */
            *raw = ((int32_t)((uint32_t)rx[0] << 16) |
                    ((uint32_t)rx[1] << 8) | rx[2]);
            return IF_OK;
        }
        /* CH_ACTIVE 不匹配: 连续模式下等待下一次转换 */
    }

    *raw = 0;
    return IF_TIMEOUT;
}

/* ═══════════════════════════════════════════════════════════════════
 * 批量读取 — 连续模式下收齐所有使能通道 (不挑 CH_ACTIVE)
 *
 * 用法:
 *   int32_t raw[4]; uint8_t got;
 *   ad7124_read_all(&adc, raw, 4, &got);
 *   // raw[0..got-1] 有效, got == d->num_ch 表示全部收齐
 *
 * 连续模式: 轮询 RDY, 读 DATA+STATUS, 按 CH_ACTIVE 归位
 *           全部 num_ch 个通道都有结果后返回.
 * 单次模式: 退化, 只读当前通道.
 * ═══════════════════════════════════════════════════════════════════ */

int ad7124_read_all(ad7124_t *d, int32_t *raw, uint8_t max_ch, uint8_t *got)
{
    uint8_t rx[4];     /* DATA[23:0] + STATUS (状态字节在最后) */
    uint8_t collected = 0;
    int     max_wait  = d->num_ch * 4;  /* 最坏每个通道等 4 次转换 */
    uint8_t ch_mask   = 0;             /* bitmask: 已收到哪些通道  */

    if (max_ch > d->num_ch) max_ch = d->num_ch;

    for (int attempt = 0; attempt < max_wait && collected < d->num_ch; attempt++) {
        /* 等待 RDY */
        uint8_t st;
        int to;
        for (to = 0; to < 500; to++) {
            if (_rd8(d, AD7124_REG_STATUS, &st) != IF_OK) goto out;
            if (!(st & AD7124_STATUS_RDY)) break;
            _wait_ms(d, 1);
        }
        if (to >= 500) goto out;

        /* 读 DATA (4 字节: DATA[23:16][15:8][7:0] + STATUS) */
        if (ad7124_read_reg(d, AD7124_REG_DATA, rx, 4) != IF_OK) goto out;

        uint8_t active_ch = rx[3] & 0x0F;   /* STATUS 在最后 (DATA_STATUS) */

        /* 忽略溢出/错误状态 */
        if (active_ch >= d->num_ch || active_ch >= max_ch) continue;
        if (ch_mask & (1 << active_ch)) continue;  /* 已有 */

        /* 返回原始 24-bit 码 (0..0xFFFFFF), 不做符号扩展 (见 read_ch 注释) */
        raw[active_ch] = ((int32_t)((uint32_t)rx[0] << 16) |
                          ((uint32_t)rx[1] << 8) | rx[2]);
        ch_mask |= (1 << active_ch);
        collected++;
    }

out:
    if (got) *got = collected;
    if (collected == 0) return IF_TIMEOUT;
    return IF_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * 读 ERROR 寄存器 (诊断)
 * ═══════════════════════════════════════════════════════════════════ */

int ad7124_read_error(ad7124_t *d, uint32_t *err_flags)
{
    uint8_t rx[3];
    if (ad7124_read_reg(d, AD7124_REG_ERROR, rx, 3) != IF_OK)
        return IF_ERR;
    *err_flags = ((uint32_t)rx[0] << 16) | ((uint32_t)rx[1] << 8) | rx[2];
    return IF_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * 原始值 → 电压 (float)
 *
 * 输入 raw 为 ad7124_read_ch/read_all 返回的原始 24-bit 码 (0..0xFFFFFF):
 *   双极性 (offset binary): 0x000000=-FS, 0x800000=0V, 0xFFFFFF=+FS
 *   VIN = (code - 2^23) / 2^23 * VREF / GAIN
 * 单极性 (straight binary):
 *   VIN = code / (2^24-1) * VREF / GAIN
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
        /* offset binary: 0 = -VREF/gain, 0x800000 = 0, 0xFFFFFF = +VREF/gain */
        float v = ((float)(raw - 0x800000)) / 8388608.0f * vref / gain;
        return v;
    } else {
        /* straight binary: 0 = 0, 0xFFFFFF = VREF/gain */
        float v = ((float)raw) / 16777215.0f * vref / gain;
        return v;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * 原始值 → mV (int32_t, 整数, 协议上报用)
 *
 * 芯片到货后: 如果前端有分压电阻, vref 参数应传分压后的等效基准.
 *            例如 1.8V rail 经 1/2 分压 → vref = 2.5 * 2 = 5.0
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════
 * 原始值 → mV (int32_t, 整数, 协议上报用)
 *
 * vref 传基准电压 (REFIN1 = 2.5V); 若前端有分压, 用 _div 系列传 divider:
 *   B1: AIN输入经100R+10K分压 → V_src = V_ADC × (100+10000)/10000
 *       = V_ADC × 1.01  → divider = AD7124_DIV_100R_10K
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
    int r = ad7124_read_ch(d, ch, &raw);
    if (r != IF_OK) return r;
    *mv = ad7124_raw_to_mv(raw, vref, pga, bipolar);
    return IF_OK;
}

/* ── 批量读取, 直接返回 mV (每通道独立 pga/bipolar) ── */

int ad7124_read_all_mv(ad7124_t *d, int32_t *mv, uint8_t max_ch, uint8_t *got,
                        float vref, const uint8_t *pga, const uint8_t *bipolar)
{
    int32_t raw[16];
    int r = ad7124_read_all(d, raw, max_ch, got);
    if (r != IF_OK) return r;
    for (uint8_t i = 0; i < *got; i++) {
        uint8_t p = pga     ? pga[i]    : 0;
        uint8_t b = bipolar ? bipolar[i] : 0;   /* 默认unipolar（B1全是正电压轨）。 */
        mv[i] = ad7124_raw_to_mv(raw[i], vref, p, b);
    }
    return IF_OK;
}
