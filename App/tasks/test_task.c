/*
 * ===================================================================
 * 驱动验证任务 — SPI2 回环 + I2C 地址扫描
 * ===================================================================
 * 输出: USART1 (PA9/PA10 → CH340 → PC, 115200 8N1)
 * QSPI 测试已独立到 qspi_test.c
 * ===================================================================
 */
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "usart.h"
#include "ad7124.h"
#include "tps62868.h"
#include "ddr_power.h"
#include "pac1934.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

extern UART_HandleTypeDef huart1;

/* AD7124 轮询等待回调: 注入 vTaskDelay 让出 CPU (HAL_Delay 是 TIM 忙等) */
static void _adc_wait_ms(uint32_t ms)
{ vTaskDelay(pdMS_TO_TICKS(ms)); }

static void _uart_puts(const char *s)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)s, strlen(s), 100);
}

static void _uart_print(const char *fmt, ...)
{
    char buf[128];
    va_list va;
    va_start(va, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    if (n > 0) HAL_UART_Transmit(&huart1, (uint8_t *)buf, n, 100);
}

static void _hex_dump(const char *label, const uint8_t *data, int len)
{
    _uart_print("  %s [%d]: ", label, len);
    for (int i = 0; i < len; i++)
        _uart_print("%02X ", data[i]);
    _uart_puts("\r\n");
}

/* ═══════════════════════════════════════════════════════════════════
 * SPI2 回环测试
 * ═══════════════════════════════════════════════════════════════════ */

static int _test_spi2_loopback(void)
{
    _uart_puts("--- SPI2 Loopback ---\r\n");
    _uart_puts("  (需要短接 PB15(MOSI) <-> PB14(MISO))\r\n");

    extern SPI_HandleTypeDef hspi2;

    /* 降低速率到 ~197KHz (MBR=7), 跳线友好 */
    uint32_t cfg1_save = hspi2.Instance->CFG1;
    uint32_t cfg1_new = (cfg1_save & ~(7u << 28)) | (7u << 28);
    hspi2.Instance->CFG1 = cfg1_new;

    if (!(hspi2.Instance->CR1 & SPI_CR1_SPE))
        SET_BIT(hspi2.Instance->CR1, SPI_CR1_SPE);

    uint8_t tx[16], rx[16];
    for (int i = 0; i < 16; i++) tx[i] = (uint8_t)(0xA0 + i);

    int ret = HAL_SPI_TransmitReceive(&hspi2, tx, rx, 16, 100);
    hspi2.Instance->CFG1 = cfg1_save;

    if (ret != HAL_OK) {
        _uart_print("  FAIL: SPI Transfer error (%d)\r\n", ret);
        return -1;
    }

    _hex_dump("TX", tx, 16);
    _hex_dump("RX", rx, 16);

    int ok = (memcmp(tx, rx, 16) == 0);
    _uart_print("  %s\r\n", ok ? "LOOPBACK OK" : "LOOPBACK FAIL");
    _uart_puts("--- SPI2 TEST END ---\r\n\r\n");
    return ok ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════════
 * I2C 地址扫描
 * ═══════════════════════════════════════════════════════════════════ */

static void _i2c_scan(const char *name, I2C_HandleTypeDef *hi2c)
{
    _uart_print("--- %s Scan ---\r\n", name);
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (HAL_I2C_IsDeviceReady(hi2c, (uint16_t)(addr << 1), 1, 10) == HAL_OK) {
            _uart_print("  Device @ 0x%02X (7bit)\r\n", addr);
            found++;
        }
    }
    if (found == 0) _uart_puts("  (no device found)\r\n");
    _uart_print("  %d device(s) total\r\n\r\n", found);
}

/* ═══════════════════════════════════════════════════════════════════
 * AD7124 驱动验证 (逻辑分析仪, 不挂芯片)
 * ═══════════════════════════════════════════════════════════════════ */

static void _test_ad7124(void)
{
    _uart_puts("--- AD7124 Driver Test (SPI2, PB12=CS) ---\r\n");
    _uart_puts("  (挂逻辑分析仪: CS/SCK/MOSI/MISO)\r\n");

    /* PB12 作 CS (输出推挽, 初始高) */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Pin   = GPIO_PIN_12;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &g);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_SET);

    /* 降速到 ~197KHz (MBR=7), 逻辑分析仪友好 */
    extern SPI_HandleTypeDef hspi2;
    uint32_t cfg1_save = hspi2.Instance->CFG1;
    uint32_t cfg1_new = (cfg1_save & ~(7u << 28)) | (7u << 28);
    hspi2.Instance->CFG1 = cfg1_new;

    extern spi_if_t spi2_if;
    ad7124_t adc = { .spi = &spi2_if, .cs_port = GPIOB, .cs_pin = GPIO_PIN_12,
                     .id = 0, .wait_ms = _adc_wait_ms };

    _uart_puts("[1] Reset (64 SCLK)... ");
    int ret = ad7124_reset(&adc);
    _uart_print("%s\r\n", ret == 0 ? "OK" : "FAIL");

    _uart_puts("[2] Read ID... ");
    ret = ad7124_read_id(&adc);
    _uart_print("%s (id=0x%02X, dev=%d, rev=%d)\r\n",
               ret == 0 ? "OK" : "FAIL", adc.id,
               (adc.id >> 4) & 0xF, adc.id & 0x0F);

    _uart_puts("[3] Init (config CH0-2, CH3 disabled)... ");
    ret = ad7124_init(&adc);
    _uart_print("%s (%d)\r\n", ret == 0 ? "OK" : "FAIL", ret);

    _uart_puts("[4] Read CH0 raw... ");
    int32_t raw = 0;
    ret = ad7124_read_ch(&adc, 0, &raw);
    _uart_print("%s (raw=%ld)\r\n", ret == 0 ? "OK" : "FAIL", raw);

    _uart_puts("[5] Read all 3ch... ");
    int32_t raw_all[4]; uint8_t got;
    ret = ad7124_read_all(&adc, raw_all, 4, &got);
    _uart_print("%s (got=%d: CH0=%ld CH1=%ld CH2=%ld)\r\n",
               ret == 0 ? "OK" : "FAIL", got, raw_all[0], raw_all[1], raw_all[2]);

    _uart_puts("--- AD7124 TEST DONE ---\r\n\r\n");
    _uart_puts("  逻辑分析仪验证点:\r\n");
    _uart_puts("  - Reset: CS↓ 8×0xFF CS↑ (64 SCLK)\r\n");
    _uart_puts("  - Read ID: CS↓ 0x45 0x00 CS↑\r\n");
    _uart_puts("  - Init: 10 个寄存器写 (3ch×3 + ADC_CTRL + CH3=0), CS↓ cmd+data CS↑\r\n");
    _uart_puts("  - Read CH0: CS↓ 0x42 0x00 0x00 0x00 0x00 CS↑ (STATUS + DATA24)\r\n\r\n");

    hspi2.Instance->CFG1 = cfg1_save;  /* 恢复原始速率 */
}

/* ═══════════════════════════════════════════════════════════════════
 * TPS62868 驱动验证 (I2C1, 扫描 0x40-0x4F)
 * ═══════════════════════════════════════════════════════════════════ */

static void _test_tps62868(void)
{
    extern i2c_if_t i2c1_if;

    _uart_puts("--- TPS62868 Test (I2C1, scan 0x40-0x4F) ---\r\n");
    uint8_t addrs[16];
    int n = tps62868_scan(&i2c1_if, addrs, 16);
    _uart_print("scan: %d device(s):", n);
    for (int i = 0; i < n; i++) _uart_print(" 0x%02X", addrs[i]);
    _uart_puts("\r\n");

    if (n > 0) {
        tps62868_t tp = { .bus = &i2c1_if, .addr = TPS62868_ADDR,
                          .factor_x10 = TPS62868_FACTOR_2X0 };
        uint8_t st;
        _uart_puts("set_vout1(1800mV, factor=2)... ");
        _uart_print("%s\r\n",
                    tps62868_set_vout1(&tp, 1800) == IF_OK ? "OK" : "FAIL");
        _uart_puts("read_status... ");
        _uart_print("%s (0x%02X)\r\n",
                    tps62868_read_status(&tp, &st) == IF_OK ? "OK" : "FAIL", st);
    }
    _uart_puts("--- TPS62868 TEST DONE ---\r\n\r\n");
}

/* ═══════════════════════════════════════════════════════════════════
 * DDR 电源三路验证 (V2: VDD1/i2c1 + VDDQ/i2c4 可测, VDD2/i2c2 未配)
 * ═══════════════════════════════════════════════════════════════════ */

static void _test_ddr_power(void)
{
    _uart_puts("--- DDR Power Test (3 rails) ---\r\n");
    int r = ddr_power_init();
    _uart_print("init: %s\r\n", r == IF_OK ? "OK" : "PARTIAL");
    uint16_t mv[3] = { DDR_VDD1_DEFAULT_MV, DDR_VDD2_DEFAULT_MV, DDR_VDDQ_DEFAULT_MV };
    r = ddr_power_set(DDR_RAIL_MASK_ALL, mv);
    _uart_print("set all rails: %s\r\n", r == IF_OK ? "OK" : "PARTIAL");
    for (int i = 0; i < 3; i++) {
        uint8_t st;
        if (ddr_power_read_status((uint8_t)i, &st) == IF_OK)
            _uart_print("rail%d: status=0x%02X\r\n", i, st);
        else
            _uart_print("rail%d: NA (bus unavailable)\r\n", i);
    }
    _uart_puts("--- DDR POWER TEST DONE ---\r\n\r\n");
}

/* ═══════════════════════════════════════════════════════════════════
 * PAC1934T 电流/功率采样验证 (I2C4, 0x2C)
 * ═══════════════════════════════════════════════════════════════════ */

extern i2c_if_t i2c4_if;

static void _test_pac1934(void)
{
    _uart_puts("--- PAC1934T Test (I2C4 0x2C) ---\r\n");

    static pac1934_t pac;
    pac.bus = &i2c4_if;
    pac.addr = 0x2C;
    pac.rsense_uohm = 10000;   /* 0.01Ω */

    /* 初始化: 使能 ch1-3, 禁 ch4, 1024 SPS */
    int r = pac1934_init(&pac, 0x07, 0);
    _uart_print("init: %s\r\n", r == IF_OK ? "OK" : "FAIL (ID?)");

    if (r == IF_OK) {
        /* 等 8 个转换周期后再读 (滚动平均稳定) */
        vTaskDelay(pdMS_TO_TICKS(50));
        pac1934_chan_t ch[4];
        if (pac1934_read_all(&pac, ch) == IF_OK) {
            for (int i = 0; i < 4; i++) {
                if (i == 3) { _uart_puts("ch4: disabled\r\n"); continue; }
                uint32_t mv  = pac1934_vbus_mv(ch[i].vbus);
                uint32_t ma  = pac1934_current_ma(ch[i].vsense, pac.rsense_uohm);
                uint32_t mw  = pac1934_power_mw(ch[i].vpower, pac.rsense_uohm);
                _uart_print("ch%d: %lumV %lumA %lumW (vbus=%u vsense=%u vpow=%lu)\r\n",
                            i + 1, mv, ma, mw,
                            ch[i].vbus, ch[i].vsense, (unsigned long)ch[i].vpower);
            }
        } else {
            _uart_puts("read_all: FAIL\r\n");
        }
    }
    _uart_puts("--- PAC1934 TEST DONE ---\r\n\r\n");
}

/* ═══════════════════════════════════════════════════════════════════
 * 入口
 * ═══════════════════════════════════════════════════════════════════ */

void StartTestTask(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(1000));

    _uart_puts("\r\n\r\n");
    _uart_puts("========================================\r\n");
    _uart_puts("  Apollo V2 — Driver Self Test\r\n");
    _uart_puts("========================================\r\n\r\n");

    _test_spi2_loopback();

    extern I2C_HandleTypeDef hi2c1, hi2c3;
    _i2c_scan("I2C1 (PB8/PB9)", &hi2c1);
    _i2c_scan("I2C3 (PH7/PH8)", &hi2c3);

    _test_ad7124();
    _test_tps62868();
    _test_ddr_power();
    _test_pac1934();

    _uart_puts("========================================\r\n");
    _uart_puts("  Self Test Complete\r\n");
    _uart_puts("========================================\r\n");

    vTaskDelete(NULL);
}
