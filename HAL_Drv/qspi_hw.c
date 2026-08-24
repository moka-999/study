/*
 * ===================================================================
 * QSPI Flash 驱动 — STM32H7 HAL 实现 (W25Q256JV, Quad SPI)
 * ===================================================================
 *
 * 四线 Quad SPI 模式 (QE=1, 4B 寻址):
 *   读: 0xEB Quad I/O Fast Read  指令=1线 地址=4线 数据=4线
 *   写: 0x32 Quad Page Program   指令=1线 地址=1线 数据=4线
 *   擦: 0x20 Sector Erase        指令=1线 地址=1线 (无数据)
 *   控: 单线 (JEDEC ID, SR, Write Enable 等控制命令)
 *
 * 时钟: QSPI_CLK = D1HCLK / (Prescaler+1)
 * 实际 CLK = 240MHz / 4 ≈ 60MHz
 * ===================================================================
 */
#include "main.h"
#include "qspi_hw.h"
#include "hal_if.h"
#include <string.h>

extern QSPI_HandleTypeDef hqspi;

/* ── 超时 ── */
#define QSPI_TO  (HAL_QPSI_TIMEOUT_DEFAULT_VALUE)
#define ERASE_TO_MS  1000
#define WRITE_TO_MS  50
#define BUSY_TO_MS   500

/* ── 工具: 快速写使能 (单线) ── */
static int _write_enable(void)
{
    QSPI_CommandTypeDef c = {0};
    c.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    c.Instruction     = W25Q_CMD_WRITE_ENABLE;
    return HAL_QSPI_Command(&hqspi, &c, QSPI_TO);
}

/* ═══════════════════════════════════════════════════════════════════
 * 初始化 — 进入 Quad SPI 模式
 * ═══════════════════════════════════════════════════════════════════ */

int qspi_init(void)
{
    uint8_t sr2;

    /* HAL_QSPI_Init 已在 MX_QUADSPI_Init() 调用. */

    /* ── 1. 用四线发 0xFF 退出 QPI (若之前误入 QPI 模式) ── */
    {
        QSPI_CommandTypeDef cq = {0};
        cq.InstructionMode = QSPI_INSTRUCTION_4_LINES;
        cq.Instruction     = 0xFF;
        HAL_QSPI_Command(&hqspi, &cq, QSPI_TO);
    }

    /* ── 2. 软件复位: 0x66 + 0x99 ── */
    {
        QSPI_CommandTypeDef c0 = {0};
        c0.InstructionMode = QSPI_INSTRUCTION_1_LINE;
        c0.Instruction     = 0x66;
        if (HAL_QSPI_Command(&hqspi, &c0, QSPI_TO) != HAL_OK) return -1;

        QSPI_CommandTypeDef c1 = {0};
        c1.InstructionMode = QSPI_INSTRUCTION_1_LINE;
        c1.Instruction     = 0x99;
        if (HAL_QSPI_Command(&hqspi, &c1, QSPI_TO) != HAL_OK) return -1;

        HAL_Delay(1);
    }

    /* ── 3. 进入 4 字节地址模式 (0xB7), 寻址 32MB ── */
    {
        QSPI_CommandTypeDef c2 = {0};
        c2.InstructionMode = QSPI_INSTRUCTION_1_LINE;
        c2.Instruction     = 0xB7;
        if (HAL_QSPI_Command(&hqspi, &c2, QSPI_TO) != HAL_OK) return IF_ERR;
    }

    /* ── 4. 使能 QE (Quad Enable) ── */
    {
        if (qspi_read_status(W25Q_CMD_READ_STATUS2, &sr2) != IF_OK) return IF_ERR;
        if ((sr2 & W25Q_SR2_QE) == 0U) {
            QSPI_CommandTypeDef command = {0};
            uint8_t new_sr2 = sr2 | W25Q_SR2_QE;

            if (_write_enable() != HAL_OK) return IF_ERR;
            command.InstructionMode = QSPI_INSTRUCTION_1_LINE;
            command.Instruction     = W25Q_CMD_WRITE_STATUS2;
            command.DataMode        = QSPI_DATA_1_LINE;
            command.NbData          = 1;

            if (HAL_QSPI_Command(&hqspi, &command, QSPI_TO) != HAL_OK ||
                HAL_QSPI_Transmit(&hqspi, &new_sr2, QSPI_TO) != HAL_OK ||
                qspi_wait_busy(100U) != IF_OK ||
                qspi_read_status(W25Q_CMD_READ_STATUS2, &sr2) != IF_OK ||
                (sr2 & W25Q_SR2_QE) == 0U) {
                return IF_ERR;
            }
        }
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * 读 JEDEC ID (单线, 无需地址)
 * ═══════════════════════════════════════════════════════════════════ */

int qspi_read_jedec_id(uint8_t *id)
{
    if (id == NULL) return IF_ERR;

    QSPI_CommandTypeDef c = {0};
    c.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    c.Instruction     = W25Q_CMD_READ_JEDEC_ID;
    c.AddressMode     = QSPI_ADDRESS_NONE;
    c.DataMode        = QSPI_DATA_1_LINE;
    c.DummyCycles     = 0;
    c.NbData          = 3;

    if (HAL_QSPI_Command(&hqspi, &c, QSPI_TO) != HAL_OK) return -1;
    return HAL_QSPI_Receive(&hqspi, id, QSPI_TO) == HAL_OK ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════════
 * 读状态寄存器 (单线, 1 字节)
 * ═══════════════════════════════════════════════════════════════════ */

int qspi_read_status(uint8_t cmd, uint8_t *sr)
{
    if (sr == NULL) return IF_ERR;

    QSPI_CommandTypeDef c = {0};
    c.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    c.Instruction     = cmd;
    c.AddressMode     = QSPI_ADDRESS_NONE;
    c.DataMode        = QSPI_DATA_1_LINE;
    c.DummyCycles     = 0;
    c.NbData          = 1;

    if (HAL_QSPI_Command(&hqspi, &c, QSPI_TO) != HAL_OK) return -1;
    return HAL_QSPI_Receive(&hqspi, sr, QSPI_TO) == HAL_OK ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════════
 * 读 QSPI 外设寄存器 — 诊断用
 * ═══════════════════════════════════════════════════════════════════ */

void qspi_dump_regs(uint32_t *cr, uint32_t *sr, uint32_t *ccr, uint32_t *dcr)
{
    if (cr != NULL)  *cr  = QUADSPI->CR;
    if (sr != NULL)  *sr  = QUADSPI->SR;
    if (ccr != NULL) *ccr = QUADSPI->CCR;
    if (dcr != NULL) *dcr = QUADSPI->DCR;
}

/* ═══════════════════════════════════════════════════════════════════
 * 等待空闲 (轮询 BUSY 位)
 * ═══════════════════════════════════════════════════════════════════ */

int qspi_wait_busy(uint32_t timeout_ms)
{
    uint8_t sr;
    uint32_t elapsed = 0;

    while (elapsed < timeout_ms) {
        if (qspi_read_status(W25Q_CMD_READ_STATUS1, &sr) != 0)
            return -1;
        if (!(sr & W25Q_SR1_BUSY))
            return 0;
        /* B1当前是无RTOS引脚/驱动基线，使用HAL毫秒延时等待Flash就绪。 */
        HAL_Delay(1U);
        elapsed++;
    }
    return -2;
}

/* ═══════════════════════════════════════════════════════════════════
 * 扇区擦除 (4KB), 单线指令+地址, 无数据
 * ═══════════════════════════════════════════════════════════════════ */

int qspi_erase_sector(uint32_t addr)
{
    if (addr >= W25Q_CAPACITY || (addr % W25Q_SECTOR_SIZE) != 0U) return IF_ERR;
    if (_write_enable() != HAL_OK) return -1;

    QSPI_CommandTypeDef c = {0};
    c.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    c.Instruction     = W25Q_CMD_SECTOR_ERASE;
    c.AddressMode     = QSPI_ADDRESS_1_LINE;
    c.AddressSize     = QSPI_ADDRESS_32_BITS;
    c.Address         = addr;
    c.DataMode        = QSPI_DATA_NONE;
    c.DummyCycles     = 0;

    if (HAL_QSPI_Command(&hqspi, &c, QSPI_TO) != HAL_OK) return -1;
    return qspi_wait_busy(ERASE_TO_MS);
}

/* ═══════════════════════════════════════════════════════════════════
 * 块擦除 (32KB/64KB), 单线指令+地址, 无数据
 * ═══════════════════════════════════════════════════════════════════ */

int qspi_erase_block32(uint32_t addr)
{
    if (addr >= W25Q_CAPACITY || (addr % W25Q_BLOCK32_SIZE) != 0U) return IF_ERR;
    if (_write_enable() != HAL_OK) return -1;

    QSPI_CommandTypeDef c = {0};
    c.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    c.Instruction     = W25Q_CMD_BLOCK_ERASE32;
    c.AddressMode     = QSPI_ADDRESS_1_LINE;
    c.AddressSize     = QSPI_ADDRESS_32_BITS;
    c.Address         = addr;
    c.DataMode        = QSPI_DATA_NONE;
    c.DummyCycles     = 0;

    if (HAL_QSPI_Command(&hqspi, &c, QSPI_TO) != HAL_OK) return -1;
    return qspi_wait_busy(3000);  /* 32KB 擦除典型 120-800ms */
}

int qspi_erase_block64(uint32_t addr)
{
    if (addr >= W25Q_CAPACITY || (addr % W25Q_BLOCK64_SIZE) != 0U) return IF_ERR;
    if (_write_enable() != HAL_OK) return -1;

    QSPI_CommandTypeDef c = {0};
    c.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    c.Instruction     = W25Q_CMD_BLOCK_ERASE64;
    c.AddressMode     = QSPI_ADDRESS_1_LINE;
    c.AddressSize     = QSPI_ADDRESS_32_BITS;
    c.Address         = addr;
    c.DataMode        = QSPI_DATA_NONE;
    c.DummyCycles     = 0;

    if (HAL_QSPI_Command(&hqspi, &c, QSPI_TO) != HAL_OK) return -1;
    return qspi_wait_busy(3000);  /* 64KB 擦除典型 150-1000ms */
}

int qspi_erase_chip(void)
{
    if (_write_enable() != HAL_OK) return -1;

    QSPI_CommandTypeDef c = {0};
    c.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    c.Instruction     = W25Q_CMD_CHIP_ERASE;
    c.AddressMode     = QSPI_ADDRESS_NONE;
    c.DataMode        = QSPI_DATA_NONE;
    c.DummyCycles     = 0;

    if (HAL_QSPI_Command(&hqspi, &c, QSPI_TO) != HAL_OK) return -1;
    return qspi_wait_busy(200000);  /* 整片擦除 32MB 典型 60-200s */
}

/* ═══════════════════════════════════════════════════════════════════
 * 页写入 — Quad Page Program (1-1-4)
 * 指令=1线, 地址=1线, 数据=4线
 * ═══════════════════════════════════════════════════════════════════ */

int qspi_write_page(uint32_t addr, const uint8_t *data, uint16_t len)
{
    if (len == 0U) return IF_OK;
    if (data == NULL || len > W25Q_PAGE_SIZE || addr >= W25Q_CAPACITY ||
        len > W25Q_CAPACITY - addr ||
        (addr / W25Q_PAGE_SIZE) != ((addr + len - 1U) / W25Q_PAGE_SIZE)) {
        return IF_ERR;
    }
    if (_write_enable() != HAL_OK) return -1;

    QSPI_CommandTypeDef c = {0};
    c.InstructionMode   = QSPI_INSTRUCTION_1_LINE;
    c.Instruction       = W25Q_CMD_QUAD_WRITE;   /* 0x32 */
    c.AddressMode       = QSPI_ADDRESS_1_LINE;
    c.AddressSize       = QSPI_ADDRESS_32_BITS;
    c.Address           = addr;
    c.DataMode          = QSPI_DATA_4_LINES;      /* 四线数据 */
    c.DummyCycles       = 0;
    c.NbData            = len;

    if (HAL_QSPI_Command(&hqspi, &c, QSPI_TO) != HAL_OK) return -1;
    if (HAL_QSPI_Transmit(&hqspi, (uint8_t *)data, QSPI_TO) != HAL_OK) return -1;
    return qspi_wait_busy(WRITE_TO_MS);
}

/* ═══════════════════════════════════════════════════════════════════
 * 读取数据 — Quad I/O Fast Read (1-4-4)
 * 指令=1线(IO0), 地址=4线, Mode=4线, Dummy=4, 数据=4线
 *
 * 时序 (0xEB):
 *   [CMD:8bit/1线=8clk on IO0] [ADDR:32bit/4线=8clk] [MODE:8bit/4线=2clk]
 *   [DUMMY:4clk] [DATA:N*8bit/4线=N*2clk]
 *
 * DCYC=6 覆盖 mode(2clk) + dummy(4clk) 共 6 个时钟周期
 * ═══════════════════════════════════════════════════════════════════ */

int qspi_read_data(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (len == 0U) return IF_OK;
    if (buf == NULL || addr >= W25Q_CAPACITY || len > W25Q_CAPACITY - addr) {
        return IF_ERR;
    }

    QSPI_CommandTypeDef c = {0};
    c.InstructionMode   = QSPI_INSTRUCTION_1_LINE;   /* 0xEB 指令只在 IO0 上发! */
    c.Instruction       = W25Q_CMD_QUAD_READ;        /* 0xEB */
    c.AddressMode       = QSPI_ADDRESS_4_LINES;
    c.AddressSize       = QSPI_ADDRESS_32_BITS;
    c.Address           = addr;
    c.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    c.DataMode          = QSPI_DATA_4_LINES;
    c.DummyCycles       = 6;   /* 6 clk: 2 mode + 4 dummy */
    c.NbData            = len;

    if (HAL_QSPI_Command(&hqspi, &c, QSPI_TO) != HAL_OK) return -1;
    return HAL_QSPI_Receive(&hqspi, buf, QSPI_TO) == HAL_OK ? 0 : -1;
}
