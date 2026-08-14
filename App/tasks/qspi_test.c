/*
 * ===================================================================
 * QSPI W25Q256 测试 — 验证 flash_if (BSP层 OOP 接口)
 * ===================================================================
 * 测试项: 跨页写入 / RMW 扇区保护 / 范围擦除
 * 输出: USART1, 115200 8N1
 * ===================================================================
 */
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "usart.h"
#include "flash_bsp.h"
#include "qspi_hw.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

extern UART_HandleTypeDef huart1;

static void _puts(const char *s)
{ HAL_UART_Transmit(&huart1, (uint8_t *)s, strlen(s), 100); }

static void _print(const char *fmt, ...)
{
    char buf[128]; va_list va;
    va_start(va, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    if (n > 0) HAL_UART_Transmit(&huart1, (uint8_t *)buf, n, 100);
}

/* ── 验证 buf[off..off+len-1] == expected ── */
static int _verify(const uint8_t *buf, uint32_t off, uint32_t len, uint8_t expected)
{
    int mism = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (buf[off + i] != expected) {
            if (mism < 3) _print("    MISMATCH [%lu]: exp=0x%02X got=0x%02X\r\n", off + i, expected, buf[off + i]);
            mism++;
        }
    }
    if (mism) _print("    %d mismatches (exp 0x%02X)\r\n", mism, expected);
    return mism;
}

/* ═══════════════════════════════════════════════════════════════════
 * QSPI W25Q256 自检 — flash_if OOP 接口
 * ═══════════════════════════════════════════════════════════════════ */

void StartQspiTestTask(void *argument)
{
    uint8_t id[3], sr1, sr2, sr3;
    int ret, fail = 0, mismatch;
    (void)argument;

    vTaskDelay(pdMS_TO_TICKS(1000));
    _puts("\r\n--- QSPI W25Q256 BSP Test ---\r\n");

    /* ── [0] 外设诊断 (qspi_hw 直读) ── */
    _puts("[0] QSPI Diag:\r\n");
    {
        uint32_t cr, sr, ccr, dcr;
        qspi_dump_regs(&cr, &sr, &ccr, &dcr);
        _print("    CR=0x%08lX SR=0x%08lX CCR=0x%08lX DCR=0x%08lX\r\n", cr, sr, ccr, dcr);
    }

    /* ── [1] flash_if.init ── */
    _puts("[1] flash_if.init... ");
    ret = flash_if.init(&flash_if);
    _print("%s (%d)\r\n", ret == 0 ? "OK" : "FAIL", ret);
    if (ret) goto done;

    /* ── [2] JEDEC ID ── */
    _puts("[2] JEDEC ID... ");
    ret = qspi_read_jedec_id(id);
    if (ret == 0) {
        _print("OK: %02X %02X %02X\r\n", id[0], id[1], id[2]);
        if (id[0] != 0xEF || id[1] != 0x40 || id[2] != 0x19) _puts("    WARN: expected EF 40 19\r\n");
    } else { _print("FAIL (%d)\r\n", ret); goto done; }

    /* ── [3] 状态寄存器 ── */
    _puts("[3] Status:\r\n");
    ret  = qspi_read_status(W25Q_CMD_READ_STATUS1, &sr1);
    ret |= qspi_read_status(W25Q_CMD_READ_STATUS2, &sr2);
    ret |= qspi_read_status(W25Q_CMD_READ_STATUS3, &sr3);
    if (ret) { _print("    FAIL (%d)\r\n", ret); goto done; }
    _print("    SR1=0x%02X (BUSY=%d) SR2=0x%02X (QE=%d) SR3=0x%02X\r\n",
           sr1, sr1&1, sr2, (sr2>>1)&1, sr3);

    /* ══════════════════════════════════════════════════════════════
     * [4] flash_if.erase + flash_if.write + flash_if.read 基础测试
     * ══════════════════════════════════════════════════════════════ */
    _puts("[4] Basic R/W via flash_if...\r\n");

    uint32_t addr = W25Q_CAPACITY - W25Q_SECTOR_SIZE * 2;  /* 末尾 8KB */
    uint8_t  wbuf[512], rbuf[512];

    /* erase 8KB */
    _print("    Erase 8KB @0x%08lX... ", addr);
    ret = flash_if.erase(&flash_if, addr, W25Q_SECTOR_SIZE * 2);
    _print("%s (%d)\r\n", ret == 0 ? "OK" : "FAIL", ret);
    if (ret) { fail = 1; goto done; }

    /* write 256B page across page boundary (addr+128 crosses page) */
    memset(wbuf, 0, sizeof(wbuf));
    for (int i = 0; i < 512; i++) wbuf[i] = (uint8_t)(i & 0xFF);

    _print("    Write 512B (cross-page) @0x%08lX... ", addr + 128);
    ret = flash_if.write(&flash_if, addr + 128, wbuf, 512);
    _print("%s (%d)\r\n", ret == 0 ? "OK" : "FAIL", ret);
    if (ret) { fail = 1; goto done; }

    /* read back */
    _print("    Read 512B @0x%08lX... ", addr + 128);
    ret = flash_if.read(&flash_if, addr + 128, rbuf, 512);
    _print("%s (%d)\r\n", ret == 0 ? "OK" : "FAIL", ret);
    if (ret) { fail = 1; goto done; }

    mismatch = memcmp(wbuf, rbuf, 512);
    _print("    Cross-page verify: %s\r\n", mismatch == 0 ? "ALL OK" : "FAIL");
    if (mismatch) fail = 1;

    /* ── [5] RMW 保护测试: 写第一扇区前半, 再写后半, 验证前半不受影响 ── */
    _puts("[5] RMW sector protection...\r\n");

    /* 重新擦除 8KB */
    flash_if.erase(&flash_if, addr, W25Q_SECTOR_SIZE * 2);

    /* 扇区 0 前半写 0xAB */
    memset(wbuf, 0xAB, 256);
    _print("    Write 0xAB * 256 @0x%08lX... ", addr);
    ret = flash_if.write(&flash_if, addr, wbuf, 256);
    _print("%s (%d)\r\n", ret == 0 ? "OK" : "FAIL", ret);
    if (ret) { fail = 1; goto done; }

    /* 扇区 0 后半写 0xCD (同一扇区, 触发 RMW) */
    memset(wbuf, 0xCD, 256);
    _print("    Write 0xCD * 256 @0x%08lX (RMW)... ", addr + 256);
    ret = flash_if.write(&flash_if, addr + 256, wbuf, 256);
    _print("%s (%d)\r\n", ret == 0 ? "OK" : "FAIL", ret);
    if (ret) { fail = 1; goto done; }

    /* 读回整扇区 */
    uint8_t sec[512];
    ret = flash_if.read(&flash_if, addr, sec, 512);
    _print("    Read 512B back... %s (%d)\r\n", ret == 0 ? "OK" : "FAIL", ret);
    if (ret) { fail = 1; goto done; }

    /* 验证前半仍是 0xAB, 后半是 0xCD */
    int m1 = _verify(sec, 0,   256, 0xAB);
    int m2 = _verify(sec, 256, 256, 0xCD);
    _print("    RMW verify: %s (0xAB:%s 0xCD:%s)\r\n",
           (m1 == 0 && m2 == 0) ? "PASS" : "FAIL",
           m1 == 0 ? "OK" : "FAIL", m2 == 0 ? "OK" : "FAIL");
    if (m1 || m2) fail = 1;

    /* ── [6] flash_if.info ── */
    _puts("[6] flash_if.info...\r\n");
    {
        uint32_t total, sector, page;
        flash_if.info(&flash_if, &total, &sector, &page);
        _print("    Total=%luKB Sector=%luB Page=%luB\r\n", total, sector, page);
    }

    /* ── 结果 ── */
    _print("\r\n--- QSPI BSP TEST %s ---\r\n\r\n", fail ? "FAIL" : "PASS");

done:
    if (fail) _print("    FAILED at step above\r\n");
    vTaskDelete(NULL);
}
