/*
 * ============================================================================
 * W25Q256 Flash BSP：flash_if_t 的 QSPI 实现
 * ============================================================================
 */
#include "flash_bsp.h"
#include "qspi_hw.h"
#include "FreeRTOS.h"
#include "semphr.h"

#include <string.h>

/*
 * 单个 4 KiB 扇区的 Read-Modify-Write 缓冲。
 * 该实例不可重入，上层若从多个任务访问 Flash，必须使用同一把互斥锁串行化。
 */
static uint8_t s_sector_buffer[W25Q_SECTOR_SIZE];
static SemaphoreHandle_t s_transaction_lock;
static int s_initialized;

int flash_transaction_prepare(void)
{
    if (s_transaction_lock == NULL) s_transaction_lock = xSemaphoreCreateMutex();
    return s_transaction_lock != NULL ? 0 : -1;
}

int flash_transaction_begin(uint32_t timeout_ms)
{
    if (s_transaction_lock == NULL ||
        xSemaphoreTake(s_transaction_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return -1;
    if (!s_initialized) {
        if (flash_if.init(&flash_if) != IF_OK) { flash_transaction_end(); return -1; }
        s_initialized = 1;
    }
    return 0;
}

void flash_transaction_end(void)
{
    (void)xSemaphoreGive(s_transaction_lock);
}

static int page_is_erased(const uint8_t *page)
{
    for (uint32_t index = 0U; index < W25Q_PAGE_SIZE; ++index) {
        if (page[index] != 0xFFU) {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief 通过固定QSPI实例初始化W25Q256。
 * @param[in] flash 接口占位参数，当前未使用。
 * @return qspi_init的结果：IF_OK成功，IF_ERR初始化失败。
 * @note 只初始化器件访问模式，不创建版本/FRU记录。
 */
static int flash_init(flash_if_t *flash)
{
    (void)flash;
    return qspi_init();
}

/**
 * @brief 同步读取指定Flash范围。
 * @param[in] flash 接口占位参数，当前未使用。
 * @param[in] address 字节地址，非零长度时整个范围须在器件内。
 * @param[out] buffer 至少length字节，非零长度须非NULL。
 * @param[in] length 字节数，0直接成功。
 * @return IF_OK完成；IF_ERR参数/IO错误，失败时输出不保证完整。
 */
static int flash_read(flash_if_t *flash, uint32_t address,
                      uint8_t *buffer, uint32_t length)
{
    (void)flash;
    if (length == 0U) return IF_OK;
    if (buffer == NULL || address >= W25Q_CAPACITY ||
        length > W25Q_CAPACITY - address) return IF_ERR;
    return qspi_read_data(address, buffer, length);
}

/**
 * @brief 逐扇区读改写，保存目标范围之外的数据，跳过未变化内容。
 * @param[in] flash 接口占位参数，当前未使用。
 * @param[in] address Flash字节地址，整个范围须在器件内。
 * @param[in] buffer 至少length字节，非零长度须非NULL，返回前保持有效。
 * @param[in] length 字节数，0直接成功。
 * @return IF_OK全部操作完成；IF_ERR参数或任一步读/擦/写失败。
 * @note 共用4KiB缓冲，不可重入；必须串行化整个事务。NOR不能直接将0改1，
 *       内容变化时会擦完整扇区再恢复数据；掉电/失败可能丢目标扇区内容或
 *       仅写完前面的扇区，函数不回滚。关键记录使用上层A/B副本保护。
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

/**
 * @brief 将范围向外扩展到4KiB边界后，选择对齐块逐段擦除。
 * @param[in] flash 接口占位参数，当前未使用。
 * @param[in] address Flash字节起始地址。
 * @param[in] length 请求范围字节数，0不操作直接成功。
 * @return IF_OK全部擦除完成；IF_ERR范围或底层错误，可能已擦除部分单元。
 * @note 扩展到的边界扇区不会保留未请求区域，调用方必须确认实际擦除范围。
 *       使用32/64KiB块优化时也不超出扩展后的范围。
 */
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

/**
 * @brief 返回固定W25Q256容量和擦写几何参数，不访问器件。
 * @param[in] flash 接口占位参数，当前未使用。
 * @param[out] total_kb 总容量，单位KiB；NULL跳过。
 * @param[out] sector_size 扇区字节数；NULL跳过。
 * @param[out] page_size 页字节数；NULL跳过。
 */
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
