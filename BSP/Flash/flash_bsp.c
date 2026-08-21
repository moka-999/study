/*
 * ===================================================================
 * Flash BSP — flash_if_t 实现 (QSPI W25Q256)
 * ===================================================================
 */
#include "hal_if.h"
#include "qspi_hw.h"
#include <string.h>

static uint8_t _sec[W25Q_SECTOR_SIZE];  /* RMW 扇区缓冲 */

/* ── 初始化 ── */
static int _init(flash_if_t *f) { (void)f; return qspi_init(); }

/* ── 读取 ── */
static int _read(flash_if_t *f, uint32_t a, uint8_t *b, uint32_t l)
{
    (void)f;
    if (!l) return IF_OK;
    if (!b || a >= W25Q_CAPACITY || l > W25Q_CAPACITY - a) return IF_ERR;
    return qspi_read_data(a, b, l);
}

/* ── 写入 (RMW) ── */
static int _write(flash_if_t *f, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    (void)f;
    if (!len) return IF_OK;
    if (!buf || addr >= W25Q_CAPACITY || len > W25Q_CAPACITY - addr) return IF_ERR;

    uint32_t ss = addr & ~(W25Q_SECTOR_SIZE - 1);
    uint32_t se = (addr + len + W25Q_SECTOR_SIZE - 1) & ~(W25Q_SECTOR_SIZE - 1);
    if (se > W25Q_CAPACITY) se = W25Q_CAPACITY;

    for (uint32_t s = ss; s < se; s += W25Q_SECTOR_SIZE) {
        if (qspi_read_data(s, _sec, W25Q_SECTOR_SIZE)) return IF_ERR;

        uint32_t cb = (addr > s) ? addr : s;
        uint32_t ce = (addr + len < s + W25Q_SECTOR_SIZE) ? addr + len : s + W25Q_SECTOR_SIZE;
        memcpy(_sec + (cb - s), buf + (cb - addr), ce - cb);

        if (qspi_erase_sector(s)) return IF_ERR;
        for (uint32_t p = 0; p < W25Q_SECTOR_SIZE; p += W25Q_PAGE_SIZE)
            if (qspi_write_page(s + p, _sec + p, W25Q_PAGE_SIZE)) return IF_ERR;
    }
    return IF_OK;
}

/* ── 范围擦除 ── */
static int _erase(flash_if_t *f, uint32_t addr, uint32_t len)
{
    (void)f;
    if (!len) return IF_OK;
    if (addr >= W25Q_CAPACITY || len > W25Q_CAPACITY - addr) return IF_ERR;
    uint32_t st = addr & ~(W25Q_SECTOR_SIZE - 1);
    uint32_t ed = (addr + len + W25Q_SECTOR_SIZE - 1) & ~(W25Q_SECTOR_SIZE - 1);
    if (ed > W25Q_CAPACITY) ed = W25Q_CAPACITY;

    for (uint32_t cur = st; cur < ed;) {
        uint32_t rem = ed - cur;
        if      (rem >= W25Q_BLOCK64_SIZE && !(cur & 0xFFFF))  { if (qspi_erase_block64(cur)) return IF_ERR; cur += W25Q_BLOCK64_SIZE; }
        else if (rem >= W25Q_BLOCK32_SIZE && !(cur & 0x7FFF))  { if (qspi_erase_block32(cur)) return IF_ERR; cur += W25Q_BLOCK32_SIZE; }
        else                                                    { if (qspi_erase_sector(cur))  return IF_ERR; cur += W25Q_SECTOR_SIZE;  }
    }
    return IF_OK;
}

/* ── 参数 ── */
static void _info(flash_if_t *f, uint32_t *tk, uint32_t *ss, uint32_t *ps)
{
    (void)f;
    if (tk) *tk = W25Q_CAPACITY / 1024;
    if (ss) *ss = W25Q_SECTOR_SIZE;
    if (ps) *ps = W25Q_PAGE_SIZE;
}

/* ── 工厂实例 ── */
flash_if_t flash_if = {
    .handle = NULL,
    .init   = _init,
    .read   = _read,
    .write  = _write,
    .erase  = _erase,
    .info   = _info,
};
