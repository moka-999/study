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
{ return HAL_I2C_Mem_Read(b->handle,d<<1,r,I2C_MEMADD_SIZE_8BIT,v,1,100)==HAL_OK?IF_OK:IF_ERR; }

/* ── 单寄存器写 ── */
static int _wr(i2c_if_t *b, uint8_t d, uint8_t r, uint8_t v)
{ return HAL_I2C_Mem_Write(b->handle,d<<1,r,I2C_MEMADD_SIZE_8BIT,&v,1,100)==HAL_OK?IF_OK:IF_ERR; }

/* ── 批量读 ── */
static int _rdm(i2c_if_t *b, uint8_t d, uint8_t r, uint8_t *buf, uint16_t l)
{ return HAL_I2C_Mem_Read(b->handle,d<<1,r,I2C_MEMADD_SIZE_8BIT,buf,l,100)==HAL_OK?IF_OK:IF_ERR; }

/* ── 设备在线检测 ── */
static int _rdy(i2c_if_t *b, uint8_t d)
{ return HAL_I2C_IsDeviceReady(b->handle,d<<1,3,10)==HAL_OK?IF_OK:IF_ERR; }

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
  .is_ready   = _rdy,
};

i2c_if_t i2c3_if = {               /* PH7=SCL, PH8=SDA */
  .handle     = &hi2c3,
  .read_reg   = _rd,
  .write_reg  = _wr,
  .read_bytes = _rdm,
  .is_ready   = _rdy,
};
