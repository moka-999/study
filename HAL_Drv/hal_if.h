/*
 * ===================================================================
 * 外设抽象接口 — 面向对象风格 (Interface + Implementation)
 * ===================================================================
 *
 * 设计目标:
 *   上层应用只依赖本文件的接口类型, 不直接调用 HAL 库函数。
 *   换 MCU 或 HAL 版本时, 只需修改 *_hw.c 实现文件, 应用层零改动。
 *
 * 使用方式:
 *   #include "hal_if.h"
 *   extern i2c_if_t i2c1_if;
 *   i2c1_if.read_reg(&i2c1_if, 0x50, 0x00, &val);
 *
 * 层次关系:
 *   App/tasks/   →   hal_if.h (接口)   →   *_hw.c (HAL 实现)
 *   BSP/         →   直接操作 GPIO/I2C 时序, 不走 HAL
 * ===================================================================
 */
#ifndef __HAL_IF_H
#define __HAL_IF_H

#include <stdint.h>

/* ── 通用返回码 ── */
#define IF_OK       0
#define IF_ERR     -1
#define IF_TIMEOUT -2

/*
 * ── I2C 总线接口 ────────────────────────────────────────────────
 * 封装 HAL_I2C_Mem_Read/Write, 屏蔽 7/10 位地址差异
 * handle: HAL I2C_HandleTypeDef* (或软件 I2C 自定义句柄)
 */
typedef struct i2c_if {
    void   *handle;
    int   (*read_reg) (struct i2c_if *bus, uint8_t dev, uint8_t reg, uint8_t *val);
    int   (*write_reg)(struct i2c_if *bus, uint8_t dev, uint8_t reg, uint8_t  val);
    int   (*read_bytes)(struct i2c_if *bus, uint8_t dev, uint8_t reg,
                        uint8_t *buf, uint16_t len);
    int   (*is_ready)  (struct i2c_if *bus, uint8_t dev);
} i2c_if_t;

/*
 * ── SPI 总线接口 ────────────────────────────────────────────────
 * 全双工传输 + 半双工发送, NSS 由上层 GPIO 控制
 */
typedef struct spi_if {
    void   *handle;
    int   (*transfer)(struct spi_if *bus, uint8_t *tx, uint8_t *rx, uint16_t len);
    int   (*send)    (struct spi_if *bus, uint8_t *data, uint16_t len);
} spi_if_t;

/*
 * ── UART 串口接口 ───────────────────────────────────────────────
 * 中断驱动发送(环形缓冲区) + 空闲中断接收(IDLE line detect)
 */
typedef struct uart_if {
    void   *handle;
    int   (*send)    (struct uart_if *port, uint8_t *data, uint16_t len);
    int   (*start_rx)(struct uart_if *port, uint8_t *buf, uint16_t size);
} uart_if_t;

/* 中断 TX drain: 挂到 HAL 的 TxISR 钩子, 由 HAL_UART_IRQHandler 的 TXE 分支自动调用 */
struct __UART_HandleTypeDef;   /* 前向声明, 避免 hal_if.h 依赖 usart.h */
void uart_tx_drain(struct __UART_HandleTypeDef *huart);
void uart_tx_isr_attach(void); /* 在 start_rx 前调用, 绑定 huart1/2.TxISR */

/*
 * ── PWM 接口 ────────────────────────────────────────────────────
 * 0~100% 占空比, 底层硬件自动分频/计算
 */
typedef struct pwm_if {
    void   *handle;
    void  (*set_duty)(struct pwm_if *pwm, uint8_t percent);
} pwm_if_t;

/*
 * ── Flash 接口 ────────────────────────────────────────────────────
 * W25Q256 NOR Flash, QSPI 四线模式
 */
typedef struct flash_if {
    void   *handle;
    int   (*init)(struct flash_if *f);
    int   (*read)(struct flash_if *f, uint32_t addr, uint8_t *buf, uint32_t len);
    int   (*write)(struct flash_if *f, uint32_t addr, const uint8_t *buf, uint32_t len);
    int   (*erase)(struct flash_if *f, uint32_t addr, uint32_t len);
    void  (*info)(struct flash_if *f, uint32_t *total_kb, uint32_t *sector, uint32_t *page);
} flash_if_t;

#endif
