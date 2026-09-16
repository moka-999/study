/*
 * ===================================================================
 * QSPI Flash 驱动 — W25Q256JV (Quad SPI 模式)
 * ===================================================================
 * 命令模式:
 *   读: 0xEB Quad I/O Fast Read  (1-4-4) 指令=1线 地址=4线 数据=4线
 *   写: 0x32 Quad Page Program   (1-1-4) 指令=1线 地址=1线 数据=4线
 *   擦: 0x20 Sector Erase        (1-1-0) 指令=1线 地址=1线 无数据
 * 引脚: PB2(CLK), PB6(NCS), PF8(IO0), PF9(IO1), PF7(IO2), PF6(IO3)
 * ===================================================================
 */
#ifndef __QSPI_HW_H
#define __QSPI_HW_H

#include <stdint.h>

/* ── W25Q256JV 命令码 ── */
#define W25Q_CMD_WRITE_ENABLE  0x06
#define W25Q_CMD_READ_STATUS1  0x05
#define W25Q_CMD_READ_STATUS2  0x35
#define W25Q_CMD_READ_STATUS3  0x15
#define W25Q_CMD_WRITE_STATUS2 0x31
#define W25Q_CMD_READ_JEDEC_ID 0x9F
#define W25Q_CMD_SECTOR_ERASE  0x20
#define W25Q_CMD_BLOCK_ERASE32 0x52
#define W25Q_CMD_BLOCK_ERASE64 0xD8
#define W25Q_CMD_CHIP_ERASE    0xC7
#define W25Q_CMD_QUAD_READ     0xEB   /* 1-4-4: 指令1线 IO0 */
#define W25Q_CMD_QUAD_WRITE    0x32   /* 1-1-4: 指令1线 + 地址1线 + 数据4线 */

/* ── 芯片参数 ── */
#define W25Q_PAGE_SIZE         256
#define W25Q_SECTOR_SIZE       4096
#define W25Q_BLOCK32_SIZE      32768
#define W25Q_BLOCK64_SIZE      65536
#define W25Q_CAPACITY          0x2000000  /* 256Mbit = 32MB, 4B 寻址 */

/* ── 状态寄存器位 ── */
#define W25Q_SR1_BUSY          0x01
#define W25Q_SR2_QE            0x02

/* ── 0xEB Quad I/O Fast Read 时序 ──
 * CMD(1线,8clk) + ADDR(4线,8clk) + MODE(4线,2clk) + DUMMY(4clk) + DATA(4线)
 * DCYC=6 覆盖 mode(2clk) + dummy(4clk) */

/* ── API ── */

/**
 * @brief 复位W25Q256，进入四字节地址模式并使能/确认QE位。
 * @return 0成功；-1关键命令、状态访问或QE确认失败。
 * @note 须先完成MX_QUADSPI_Init；同步调用，独占QSPI设备，不验证JEDEC型号。
 *       配置QE时可能调用osDelay等待，须在允许RTOS等待的上下文使用。
 */
int qspi_init(void);

/**
 * @brief 读取STM32 QSPI外设寄存器供诊断。
 * @param[out] cr CR输出，可为NULL跳过。
 * @param[out] sr SR输出，可为NULL跳过。
 * @param[out] ccr CCR输出，可为NULL跳过。
 * @param[out] dcr DCR输出，可为NULL跳过。
 * @note 顺序读寄存器，不是原子快照，不发送Flash命令。
 */
void qspi_dump_regs(uint32_t *cr, uint32_t *sr, uint32_t *ccr, uint32_t *dcr);

/**
 * @brief 单线读取JEDEC的制造商、类型和容量三个字节。
 * @param[out] id 非NULL、至少3字节；仅返回0时有效。
 * @return 0成功；-1参数或HAL传输错误。
 * @note 不解释ID、不判断型号兼容；同步独占访问QSPI。
 */
int qspi_read_jedec_id(uint8_t *id);

/**
 * @brief 以单线命令读取一个Flash状态字节。
 * @param[in] cmd READ_STATUS1/2/3命令码，调用方保证合法。
 * @param[out] sr 非NULL输出，仅返回0时有效。
 * @return 0成功；-1参数或HAL传输错误。
 * @note 不校验命令白名单，调用方不可传写入/擦除等其他指令。
 */
int qspi_read_status(uint8_t cmd, uint8_t *sr);

/**
 * @brief 轮询SR1 BUSY位，忙时通过osDelay(1)让出执行。
 * @param[in] timeout_ms 当前实现的最大轮询次数，按每轮1tick等待近似毫秒。
 * @return 0观察到空闲；-1读状态失败；-2循环次数耗尽，参数0也直接返回-2。
 * @note 单位近似依赖1kHz内核tick；每轮另有HAL耗时，不是严格墙钟超时。
 *       必须在可调用osDelay的任务上下文使用，返回超时不表示器件已停止操作。
 */
int qspi_wait_busy(uint32_t timeout_ms);

/**
 * @brief 擦除一个4KiB单元并等待BUSY清除。
 * @param[in] addr Flash字节地址，须小于W25Q_CAPACITY且按W25Q_SECTOR_SIZE对齐。
 * @return 0擦除等待完成；-1地址/命令错误；-2等待次数耗尽。
 * @note 整个单元内容会丢失，不做数据保留；调用方确认范围并串行化事务。
 *       超时后不能假定擦除未发生或设备已空闲，等待约束见qspi_wait_busy。
 */
int qspi_erase_sector(uint32_t addr);

/**
 * @brief 擦除一个32KiB单元并等待BUSY清除。
 * @param[in] addr Flash字节地址，须小于W25Q_CAPACITY且按W25Q_BLOCK32_SIZE对齐。
 * @return 0擦除等待完成；-1地址/命令错误；-2等待次数耗尽。
 * @note 整个单元内容会丢失，不做数据保留；调用方确认范围并串行化事务。
 *       超时后不能假定擦除未发生或设备已空闲，等待约束见qspi_wait_busy。
 */
int qspi_erase_block32(uint32_t addr);

/**
 * @brief 擦除一个64KiB单元并等待BUSY清除。
 * @param[in] addr Flash字节地址，须小于W25Q_CAPACITY且按W25Q_BLOCK64_SIZE对齐。
 * @return 0擦除等待完成；-1地址/命令错误；-2等待次数耗尽。
 * @note 整个单元内容会丢失，不做数据保留；调用方确认范围并串行化事务。
 *       超时后不能假定擦除未发生或设备已空闲，等待约束见qspi_wait_busy。
 */
int qspi_erase_block64(uint32_t addr);

/**
 * @brief 发出整片32MiB擦除并同步等待完成。
 * @return 0等待到空闲；-1命令失败；-2等待次数耗尽。
 * @note 破坏全部Flash数据，调用前必须获得整片擦除授权并确认无须保留的
 *       元数据；不是常规记录更新接口。采用200000次等待上限，见qspi_wait_busy。
 */
int qspi_erase_chip(void);

/**
 * @brief 在单个256字节页内编程并等待完成，不自动擦除。
 * @param[in] addr Flash字节地址，整个范围须位于器件内且不能跨页。
 * @param[in] data 至少len字节；len为0时允许NULL。
 * @param[in] len 0..256字节，0时不访问硬件直接成功。
 * @return 0成功；-1参数/传输错误；-2编程等待次数耗尽。
 * @note NOR只能从1写成0，需要0变1时须先擦除；不验证回读，不保证失败时
 *       未写入。调用方须独占整个事务，输入在返回前保持有效。
 */
int qspi_write_page(uint32_t addr, const uint8_t *data, uint16_t len);

/**
 * @brief 四线读取W25Q256指定范围到调用方缓冲。
 * @param[in] addr Flash字节地址，整个范围不得超过32MiB。
 * @param[out] buf 至少len字节；len为0时允许NULL。
 * @param[in] len 字节数，0时直接成功，不检查地址。
 * @return 0成功；-1参数或HAL错误，失败时输出内容不保证完整。
 * @note 须先完成QE/四字节模式初始化；同步独占访问，返回后缓冲可复用。
 */
int qspi_read_data(uint32_t addr, uint8_t *buf, uint32_t len);

#endif
