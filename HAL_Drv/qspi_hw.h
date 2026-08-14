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

/* 初始化: 复位, 退出 QPI, 清除 4B 地址, 使能 QE 进入 Quad SPI */
int qspi_init(void);

/* 诊断: 读取 QSPI 外设寄存器 (CR, SR, CCR, DCR) */
void qspi_dump_regs(uint32_t *cr, uint32_t *sr, uint32_t *ccr, uint32_t *dcr);

/* 读 JEDEC ID (3 字节: 制造商 + 类型 + 容量), 单线 */
int qspi_read_jedec_id(uint8_t *id);

/* 读状态寄存器, 单线 */
int qspi_read_status(uint8_t cmd, uint8_t *sr);

/* 等待 Flash 空闲 (轮询 BUSY 位) */
int qspi_wait_busy(uint32_t timeout_ms);

/* 扇区擦除 (4KB), 阻塞至完成, 单线指令 */
int qspi_erase_sector(uint32_t addr);

/* 块擦除 (32KB/64KB), 阻塞至完成 */
int qspi_erase_block32(uint32_t addr);
int qspi_erase_block64(uint32_t addr);

/* 整片擦除, 耗时 ~60-200s */
int qspi_erase_chip(void);

/* 页写入 (≤256 字节, 不能跨页), 四线数据 */
int qspi_write_page(uint32_t addr, const uint8_t *data, uint16_t len);

/* 读取数据 (任意长度), 四线 Quad I/O Fast Read */
int qspi_read_data(uint32_t addr, uint8_t *buf, uint32_t len);

#endif
