/*
 * ===================================================================
 * QSPI Flash 驱动 — W25Q256FV/JV (Quad SPI 模式)
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

/* ── W25Q256FV/JV 命令码 ── */
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
#define W25Q_CMD_READ_DATA_4B  0x13   /* 单线四字节地址读取，用于非对齐前缀 */
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
#define W25Q_SR1_WEL           0x02
#define W25Q_SR2_SUS           0x80
#define W25Q_SR2_QE            0x02

/* ── 0xEB Quad I/O Fast Read 时序 ──
 * CMD(1线,8clk) + ADDR(4线,8clk) + MODE(4线,2clk) + DUMMY(4clk) + DATA(4线)
 * MODE=0xFF显式驱动2个时钟；DCYC=4仅表示其后的空时钟 */

/* ── API ── */

/**
 * @brief 检查空闲与型号，进入四字节地址模式并确认QE/ADS。
 * @return 0成功；-1器件忙超时、挂起、型号错误或关键命令/回读失败。
 * @note 须先完成MX_QUADSPI_Init；独占QSPI。不会复位NOR或清除写保护，
 *       防止MCU单独重启中断先前擦写；失败后不得发布持久化信息为有效。
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
 * @param[in] cmd 仅允许READ_STATUS1/2/3命令码。
 * @param[out] sr 非NULL输出，仅返回0时有效。
 * @return 0成功；-1参数或HAL传输错误。
 * @note 非白名单命令拒绝发送。
 */
int qspi_read_status(uint8_t cmd, uint8_t *sr);

/**
 * @brief 轮询SR1 BUSY位，以HAL毫秒时基计算超时。
 * @param[in] timeout_ms 等待上限毫秒，0只检查一次；允许32位时基正常回绕。
 * @return 0观察到空闲；-1读状态失败；-2超时。
 * @note 调度后让出CPU，调度前用HAL_Delay；不可在中断/时基停止时调用。
 *       每次HAL事务仍受其自身超时约束；超时不表示NOR操作已终止。
 */
int qspi_wait_busy(uint32_t timeout_ms);

/**
 * @brief 擦除一个4KiB单元并回读整个单元确认全为0xFF。
 * @param[in] addr Flash字节地址，须小于W25Q_CAPACITY且按W25Q_SECTOR_SIZE对齐。
 * @return 0擦除并校验成功；-1地址/命令/回读错误；-2忙等待超时。
 * @note 整个单元内容会丢失，不做数据保留；调用方确认范围并串行化事务。
 *       超时后不能假定擦除未发生或设备已空闲，等待约束见qspi_wait_busy。
 */
int qspi_erase_sector(uint32_t addr);

/**
 * @brief 擦除一个32KiB单元并回读整个单元确认全为0xFF。
 * @param[in] addr Flash字节地址，须小于W25Q_CAPACITY且按W25Q_BLOCK32_SIZE对齐。
 * @return 0擦除并校验成功；-1地址/命令/回读错误；-2忙等待超时。
 * @note 整个单元内容会丢失，不做数据保留；调用方确认范围并串行化事务。
 *       超时后不能假定擦除未发生或设备已空闲，等待约束见qspi_wait_busy。
 */
int qspi_erase_block32(uint32_t addr);

/**
 * @brief 擦除一个64KiB单元并回读整个单元确认全为0xFF。
 * @param[in] addr Flash字节地址，须小于W25Q_CAPACITY且按W25Q_BLOCK64_SIZE对齐。
 * @return 0擦除并校验成功；-1地址/命令/回读错误；-2忙等待超时。
 * @note 整个单元内容会丢失，不做数据保留；调用方确认范围并串行化事务。
 *       超时后不能假定擦除未发生或设备已空闲，等待约束见qspi_wait_busy。
 */
int qspi_erase_block64(uint32_t addr);

/**
 * @brief 发出整片32MiB擦除并同步等待完成。
 * @return 0整片回读均为0xFF；-1命令/回读失败；-2忙等待超时。
 * @note 破坏全部Flash数据，调用前必须获得整片擦除授权并确认无须保留的
 *       元数据；不是常规记录更新接口。忙等待上限410秒，另加32MiB校验耗时；维护者负责看门狗。
 */
int qspi_erase_chip(void);

/**
 * @brief 在单个256字节页内编程并等待完成，不自动擦除。
 * @param[in] addr Flash字节地址，整个范围须位于器件内且不能跨页。
 * @param[in] data 至少len字节；len为0时允许NULL。
 * @param[in] len 0..256字节，0时不访问硬件直接成功。
 * @return 0编程且回读相符；-1参数/传输/校验错误；-2编程等待超时。
 * @note NOR只能从1写成0，需要0变1时须先擦除；逐字节验证回读，不保证失败时
 *       未写入。调用方须独占整个事务，输入在返回前保持有效。
 */
int qspi_write_page(uint32_t addr, const uint8_t *data, uint16_t len);

/**
 * @brief 读取W25Q256任意字节范围；非对齐前缀走单线，其余走四线。
 * @param[in] addr Flash字节地址，整个范围不得超过32MiB。
 * @param[out] buf 至少len字节；len为0时允许NULL。
 * @param[in] len 字节数，0时直接成功，不检查地址。
 * @return 0成功；-1参数或HAL错误，失败时输出内容不保证完整。
 * @note 须先完成QE/四字节模式初始化；同步独占访问，返回后缓冲可复用。
 *       Quad读起始地址须4字节对齐；最多先用13h读取1..3字节，不扩大请求范围。
 *       QSPI时钟须不高于13h读取的50MHz上限；当前板级配置为40MHz。
 */
int qspi_read_data(uint32_t addr, uint8_t *buf, uint32_t len);

#endif
