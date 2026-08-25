/*
 * ===================================================================
 * I2C 接口 — STM32 HAL 实现
 * ===================================================================
 * 实现 i2c_if_t 接口, 底层调用 HAL_I2C_Mem_xxx 系列函数。
 * 移植指南: 新 MCU 改这个文件, i2c_if_t 实例名不变 → 应用层零改动。
 * ===================================================================
 */
#include "hal_if.h"
#include "i2c.h"

/* ── 单寄存器读 ── */
static int _rd(i2c_if_t *b, uint8_t d, uint8_t r, uint8_t *v)
{ if (!b->handle) return IF_ERR; return HAL_I2C_Mem_Read(b->handle,d<<1,r,I2C_MEMADD_SIZE_8BIT,v,1,100)==HAL_OK?IF_OK:IF_ERR; }

/* ── 单寄存器写 ── */
static int _wr(i2c_if_t *b, uint8_t d, uint8_t r, uint8_t v)
{ if (!b->handle) return IF_ERR; return HAL_I2C_Mem_Write(b->handle,d<<1,r,I2C_MEMADD_SIZE_8BIT,&v,1,100)==HAL_OK?IF_OK:IF_ERR; }

/* ── 批量读 ── */
static int _rdm(i2c_if_t *b, uint8_t d, uint8_t r, uint8_t *buf, uint16_t l)
{ if (!b->handle) return IF_ERR; return HAL_I2C_Mem_Read(b->handle,d<<1,r,I2C_MEMADD_SIZE_8BIT,buf,l,100)==HAL_OK?IF_OK:IF_ERR; }

/* ── 原始数据写（无寄存器地址）── */
static int _wrm(i2c_if_t *b, uint8_t d, const uint8_t *buf, uint16_t l)
{ if (!b->handle || !buf || !l) return IF_ERR; return HAL_I2C_Master_Transmit(b->handle,d<<1,(uint8_t *)buf,l,100)==HAL_OK?IF_OK:IF_ERR; }

/* ── 命令字节 (Send Byte 协议: START→addr+W→cmd→STOP, 无数据) ── */
static int _cmd(i2c_if_t *b, uint8_t d, uint8_t c)
{ if (!b->handle) return IF_ERR; return HAL_I2C_Master_Transmit(b->handle,d<<1,&c,1,100)==HAL_OK?IF_OK:IF_ERR; }

/* ── 设备在线检测 ── */
static int _rdy(i2c_if_t *b, uint8_t d)
{ if (!b->handle) return IF_ERR; return HAL_I2C_IsDeviceReady(b->handle,d<<1,3,10)==HAL_OK?IF_OK:IF_ERR; }

/*
 * ── 工厂实例 ───────────────────────────────────────────────────
 * I2C1, PB8=SCL, PB9=SDA, 100KHz, CubeMX 生成 MX_I2C1_Init()
 * 后续新增 I2C2/I2C3 只需仿照声明 i2c2_if/i2c3_if
 */
i2c_if_t i2c1_if = {
  .handle     = &hi2c1,
  .read_reg   = _rd,
  .write_reg  = _wr,
  .read_bytes = _rdm,
  .write_bytes = _wrm,
  .write_cmd  = _cmd,
  .is_ready   = _rdy,
};

i2c_if_t i2c2_if = {               /* B2: I2C2 (PF0/1); V2 未配置, handle=NULL 禁用 */
  .handle     = NULL,              /* B2 阶段改为 &hi2c2 (需 CubeMX 使能 I2C2) */
  .read_reg   = _rd,
  .write_reg  = _wr,
  .read_bytes = _rdm,
  .write_bytes = _wrm,
  .write_cmd  = _cmd,
  .is_ready   = _rdy,
};

i2c_if_t i2c3_if = {               /* PH7=SCL, PH8=SDA */
  .handle     = &hi2c3,
  .read_reg   = _rd,
  .write_reg  = _wr,
  .read_bytes = _rdm,
  .write_bytes = _wrm,
  .write_cmd  = _cmd,
  .is_ready   = _rdy,
};

i2c_if_t i2c4_if = {               /* PF14=SCL, PF15=SDA (V2 引出排针) */
  .handle     = &hi2c4,            /* B2 板: I2C4 切到 PH4/5, 本实例不变 */
  .read_reg   = _rd,
  .write_reg  = _wr,
  .read_bytes = _rdm,
  .write_bytes = _wrm,
  .write_cmd  = _cmd,
  .is_ready   = _rdy,
};
