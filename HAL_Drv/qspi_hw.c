/*
 * ===================================================================
 * QSPI Flash 驱动 — STM32H7 HAL 实现 (W25Q256FV/JV, Quad SPI)
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
#include "quadspi.h"
#include "qspi_hw.h"
#include "hal_if.h"
#include "cmsis_os2.h"
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
    uint8_t status;
    if (qspi_wait_busy(BUSY_TO_MS) != IF_OK ||
        qspi_read_status(W25Q_CMD_READ_STATUS2, &status) != IF_OK ||
        (status & W25Q_SR2_SUS) != 0U) return IF_ERR;
    QSPI_CommandTypeDef c = {0};
    c.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    c.Instruction     = W25Q_CMD_WRITE_ENABLE;
    if (HAL_QSPI_Command(&hqspi, &c, QSPI_TO) != HAL_OK ||
        qspi_read_status(W25Q_CMD_READ_STATUS1, &status) != IF_OK ||
        (status & W25Q_SR1_WEL) == 0U) return IF_ERR;
    return IF_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * 初始化 — 进入 Quad SPI 模式
 * ═══════════════════════════════════════════════════════════════════ */

int qspi_init(void)
{
    uint8_t status, id[3];
    QSPI_CommandTypeDef command = {0};

    /* 退出QPI后先检查忙/挂起状态；绝不复位正在编程或擦除的NOR。 */
    command.InstructionMode = QSPI_INSTRUCTION_4_LINES;
    command.Instruction = 0xFFU;
    if (HAL_QSPI_Command(&hqspi, &command, QSPI_TO) != HAL_OK ||
        qspi_wait_busy(BUSY_TO_MS) != IF_OK ||
        qspi_read_status(W25Q_CMD_READ_STATUS2, &status) != IF_OK ||
        (status & W25Q_SR2_SUS) != 0U ||
        qspi_read_jedec_id(id) != IF_OK ||
        id[0] != 0xEFU || id[1] != 0x40U || id[2] != 0x19U) return IF_ERR;

    /* 取消残留写使能，进入四字节地址模式并回读ADS；不修改保护位。 */
    command.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    command.Instruction = 0x04U;
    if (HAL_QSPI_Command(&hqspi, &command, QSPI_TO) != HAL_OK) return IF_ERR;
    command.Instruction = 0xB7U;
    if (HAL_QSPI_Command(&hqspi, &command, QSPI_TO) != HAL_OK ||
        qspi_read_status(W25Q_CMD_READ_STATUS3, &status) != IF_OK ||
        (status & 1U) == 0U) return IF_ERR;

    if (qspi_read_status(W25Q_CMD_READ_STATUS2, &status) != IF_OK) return IF_ERR;
    if ((status & W25Q_SR2_QE) == 0U) {
        uint8_t wanted = status | W25Q_SR2_QE;
        if (_write_enable() != IF_OK) return IF_ERR;
        command.Instruction = W25Q_CMD_WRITE_STATUS2;
        command.DataMode = QSPI_DATA_1_LINE;
        command.NbData = 1U;
        if (HAL_QSPI_Command(&hqspi, &command, QSPI_TO) != HAL_OK ||
            HAL_QSPI_Transmit(&hqspi, &wanted, QSPI_TO) != HAL_OK ||
            qspi_wait_busy(100U) != IF_OK ||
            qspi_read_status(W25Q_CMD_READ_STATUS2, &status) != IF_OK ||
            status != wanted) return IF_ERR;
    }
    return IF_OK;
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
    if (sr == NULL || (cmd != W25Q_CMD_READ_STATUS1 &&
        cmd != W25Q_CMD_READ_STATUS2 && cmd != W25Q_CMD_READ_STATUS3)) return IF_ERR;

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
    uint32_t begin = HAL_GetTick();
    for (;;) {
        uint8_t status;
        if (qspi_read_status(W25Q_CMD_READ_STATUS1, &status) != IF_OK) return IF_ERR;
        if ((status & W25Q_SR1_BUSY) == 0U) return IF_OK;
        if ((uint32_t)(HAL_GetTick() - begin) >= timeout_ms) return IF_TIMEOUT;
        if (osKernelGetState() == osKernelRunning) osDelay(1U);
        else HAL_Delay(1U);
    }
}

/* 校验整段擦除结果；单次只占256字节栈，不申请整块缓冲。 */
static int verify_erased(uint32_t address, uint32_t size, uint32_t timeout_ms)
{
    int result = qspi_wait_busy(timeout_ms);
    if (result != IF_OK) return result;
    uint8_t data[W25Q_PAGE_SIZE];
    for (uint32_t offset = 0U; offset < size; offset += sizeof(data)) {
        if (qspi_read_data(address + offset, data, sizeof(data)) != IF_OK) return IF_ERR;
        for (uint32_t i = 0U; i < sizeof(data); ++i)
            if (data[i] != 0xFFU) return IF_ERR;
        /* 大范围维护校验让出CPU；调用方仍须保证独占Flash及看门狗维护。 */
        if ((offset & 0xFFFFU) == 0U && osKernelGetState() == osKernelRunning)
            osDelay(1U);
    }
    return IF_OK;
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
    return verify_erased(addr, W25Q_SECTOR_SIZE, ERASE_TO_MS);
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
    return verify_erased(addr, W25Q_BLOCK32_SIZE, 3000U);
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
    return verify_erased(addr, W25Q_BLOCK64_SIZE, 3000U);
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
    return verify_erased(0U, W25Q_CAPACITY, 410000U); /* 最大400s，另留10s余量。 */
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
    int result = qspi_wait_busy(WRITE_TO_MS);
    if (result != IF_OK) return result;
    uint8_t verify[W25Q_PAGE_SIZE];
    if (qspi_read_data(addr, verify, len) != IF_OK || memcmp(data, verify, len) != 0)
        return IF_ERR;
    return IF_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * 读取数据 — Quad I/O Fast Read (1-4-4)
 * 指令=1线(IO0), 地址=4线, Mode=4线, Dummy=4, 数据=4线
 *
 * 时序 (0xEB):
 *   [CMD:8bit/1线=8clk on IO0] [ADDR:32bit/4线=8clk] [MODE:8bit/4线=2clk]
 *   [DUMMY:4clk] [DATA:N*8bit/4线=N*2clk]
 *
 * MODE显式发送0xFF，禁止进入连续读；MODE之后再留4个空时钟。
 * ═══════════════════════════════════════════════════════════════════ */

int qspi_read_data(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (len == 0U) return IF_OK;
    if (buf == NULL || addr >= W25Q_CAPACITY || len > W25Q_CAPACITY - addr) {
        return IF_ERR;
    }

    uint8_t status;
    if (qspi_wait_busy(BUSY_TO_MS) != IF_OK ||
        qspi_read_status(W25Q_CMD_READ_STATUS2, &status) != IF_OK ||
        (status & W25Q_SR2_SUS) != 0U) return IF_ERR;
    QSPI_CommandTypeDef c = {0};
    c.InstructionMode   = QSPI_INSTRUCTION_1_LINE;   /* 0xEB 指令只在 IO0 上发! */
    c.Instruction       = W25Q_CMD_QUAD_READ;        /* 0xEB */
    c.AddressMode       = QSPI_ADDRESS_4_LINES;
    c.AddressSize       = QSPI_ADDRESS_32_BITS;
    c.Address           = addr;
    c.AlternateByteMode = QSPI_ALTERNATE_BYTES_4_LINES;
    c.AlternateBytesSize = QSPI_ALTERNATE_BYTES_8_BITS;
    c.AlternateBytes    = 0xFFU;
    c.DataMode          = QSPI_DATA_4_LINES;
    c.DummyCycles       = 4U;
    c.NbData            = len;

    if (HAL_QSPI_Command(&hqspi, &c, QSPI_TO) != HAL_OK) return -1;
    return HAL_QSPI_Receive(&hqspi, buf, QSPI_TO) == HAL_OK ? 0 : -1;
}
