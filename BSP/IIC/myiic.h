/**
 ********************************************************************
 * @file    myiic.h
 * @brief   软件模拟 I2C 驱动（IO 口模拟 I2C 时序）
 * @note    SCL=PH4, SDA=PH5, 速率约 250KHz
 * @source  正点原子 ALIENTEK
 ********************************************************************
 */

#ifndef __MYIIC_H
#define __MYIIC_H

#include "main.h"

/* ── I2C 引脚定义 ── */
#define IIC_SCL_GPIO_PORT               GPIOH
#define IIC_SCL_GPIO_PIN                GPIO_PIN_4
#define IIC_SCL_GPIO_CLK_ENABLE()       __HAL_RCC_GPIOH_CLK_ENABLE()

#define IIC_SDA_GPIO_PORT               GPIOH
#define IIC_SDA_GPIO_PIN                GPIO_PIN_5
#define IIC_SDA_GPIO_CLK_ENABLE()       __HAL_RCC_GPIOH_CLK_ENABLE()

/* ── IO 操作宏 ── */
#define IIC_SCL(x)      HAL_GPIO_WritePin(IIC_SCL_GPIO_PORT, IIC_SCL_GPIO_PIN, \
                          (x) ? GPIO_PIN_SET : GPIO_PIN_RESET)

#define IIC_SDA(x)      HAL_GPIO_WritePin(IIC_SDA_GPIO_PORT, IIC_SDA_GPIO_PIN, \
                          (x) ? GPIO_PIN_SET : GPIO_PIN_RESET)

#define IIC_READ_SDA    HAL_GPIO_ReadPin(IIC_SDA_GPIO_PORT, IIC_SDA_GPIO_PIN)

/* ── I2C 基本操作 ── */
void    iic_init(void);                 /* 初始化 I2C IO 口       */
void    iic_start(void);                /* 发送起始信号           */
void    iic_stop(void);                 /* 发送停止信号           */
void    iic_ack(void);                  /* 发送 ACK 应答          */
void    iic_nack(void);                 /* 发送 NACK 非应答       */
uint8_t iic_wait_ack(void);             /* 等待从机 ACK，返回0成功 */
void    iic_send_byte(uint8_t data);    /* 发送一个字节           */
uint8_t iic_read_byte(uint8_t ack);     /* 读取一个字节           */

#endif

