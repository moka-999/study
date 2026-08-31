/*
 * ============================================================================
 * W25Q256 Flash BSP：flash_if_t 的 QSPI 实现
 * ============================================================================
 */
#include "flash_bsp.h"
#include "qspi_hw.h"

#include <string.h>

/*
 * 单个 4 KiB 扇区的 Read-Modify-Write 缓冲。
 * 该实例不可重入，上层若从多个任务访问 Flash，必须使用同一把互斥锁串行化。
 */
static uint8_t s_sector_buffer[W25Q_SECTOR_SIZE];

static int page_is_erased(const uint8_t *page)
{
    for (uint32_t index = 0U; index < W25Q_PAGE_SIZE; ++index) {
        if (page[index] != 0xFFU) {
            return 0;
        }
    }
    return 1;
}

static int flash_init(flash_if_t *flash)
{
    (void)flash;
    return qspi_init();
}

static int flash_read(flash_if_t *flash, uint32_t address,
                      uint8_t *buffer, uint32_t length)
{
    (void)flash;
    if (length == 0U) return IF_OK;
    if (buffer == NULL || address >= W25Q_CAPACITY ||
        length > W25Q_CAPACITY - address) return IF_ERR;
    return qspi_read_data(address, buffer, length);
}

/*
 * 任意范围写入：逐扇区 RMW，保护同扇区内未修改的数据。
 * NOR Flash 不能把 0 直接改回 1，因此内容变化时统一擦除目标扇区。
 */
static int flash_write(flash_if_t *flash, uint32_t address,
                       const uint8_t *buffer, uint32_t length)
{
    uint32_t first_sector;
    uint32_t sector_end;

    (void)flash;
    if (length == 0U) return IF_OK;
    if (buffer == NULL || address >= W25Q_CAPACITY ||
        length > W25Q_CAPACITY - address) return IF_ERR;

    first_sector = address & ~(W25Q_SECTOR_SIZE - 1U);
    sector_end = (address + length + W25Q_SECTOR_SIZE - 1U) &
                 ~(W25Q_SECTOR_SIZE - 1U);
    if (sector_end > W25Q_CAPACITY) sector_end = W25Q_CAPACITY;

    for (uint32_t sector = first_sector; sector < sector_end;
         sector += W25Q_SECTOR_SIZE) {
        uint32_t copy_begin = (address > sector) ? address : sector;
        uint32_t copy_end = (address + length < sector + W25Q_SECTOR_SIZE) ?
                            address + length : sector + W25Q_SECTOR_SIZE;
        uint32_t copy_length = copy_end - copy_begin;

        if (qspi_read_data(sector, s_sector_buffer, W25Q_SECTOR_SIZE) != IF_OK) {
            return IF_ERR;
        }

        /* 相同内容不擦写，减少 NOR Flash 磨损和无效等待。 */
        if (memcmp(s_sector_buffer + (copy_begin - sector),
                   buffer + (copy_begin - address), copy_length) == 0) {
            continue;
        }
        memcpy(s_sector_buffer + (copy_begin - sector),
               buffer + (copy_begin - address), copy_length);

        if (qspi_erase_sector(sector) != IF_OK) return IF_ERR;
        for (uint32_t page = 0U; page < W25Q_SECTOR_SIZE;
             page += W25Q_PAGE_SIZE) {
            /* 擦除态页面无需重新写入，可显著缩短稀疏记录更新时间。 */
            if (!page_is_erased(s_sector_buffer + page) &&
                qspi_write_page(sector + page, s_sector_buffer + page,
                                W25Q_PAGE_SIZE) != IF_OK) {
                return IF_ERR;
            }
        }
    }
    return IF_OK;
}

/* 范围擦除会向外扩展到完整 4 KiB 扇区。 */
static int flash_erase(flash_if_t *flash, uint32_t address, uint32_t length)
{
    uint32_t erase_begin;
    uint32_t erase_end;

    (void)flash;
    if (length == 0U) return IF_OK;
    if (address >= W25Q_CAPACITY || length > W25Q_CAPACITY - address) return IF_ERR;

    erase_begin = address & ~(W25Q_SECTOR_SIZE - 1U);
    erase_end = (address + length + W25Q_SECTOR_SIZE - 1U) &
                ~(W25Q_SECTOR_SIZE - 1U);
    if (erase_end > W25Q_CAPACITY) erase_end = W25Q_CAPACITY;

    for (uint32_t current = erase_begin; current < erase_end;) {
        uint32_t remaining = erase_end - current;
        if (remaining >= W25Q_BLOCK64_SIZE &&
            (current % W25Q_BLOCK64_SIZE) == 0U) {
            if (qspi_erase_block64(current) != IF_OK) return IF_ERR;
            current += W25Q_BLOCK64_SIZE;
        } else if (remaining >= W25Q_BLOCK32_SIZE &&
                   (current % W25Q_BLOCK32_SIZE) == 0U) {
            if (qspi_erase_block32(current) != IF_OK) return IF_ERR;
            current += W25Q_BLOCK32_SIZE;
        } else {
            if (qspi_erase_sector(current) != IF_OK) return IF_ERR;
            current += W25Q_SECTOR_SIZE;
        }
    }
    return IF_OK;
}

static void flash_info(flash_if_t *flash, uint32_t *total_kb,
                       uint32_t *sector_size, uint32_t *page_size)
{
    (void)flash;
    if (total_kb != NULL) *total_kb = W25Q_CAPACITY / 1024U;
    if (sector_size != NULL) *sector_size = W25Q_SECTOR_SIZE;
    if (page_size != NULL) *page_size = W25Q_PAGE_SIZE;
}

flash_if_t flash_if = {
    .handle = NULL,
    .init   = flash_init,
    .read   = flash_read,
    .write  = flash_write,
    .erase  = flash_erase,
    .info   = flash_info,
};
