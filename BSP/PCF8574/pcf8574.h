/**
 ********************************************************************
 * @file    pcf8574.h
 * @brief   PCF8574 I2C 8位IO扩展芯片驱动
 * @note    阿波罗V2底板用 PCF8574 扩展 IO，
 *          P7 控制 YT8512C 以太网 PHY 硬件复位
 * @source  正点原子 ALIENTEK
 ********************************************************************
 */

#ifndef __PCF8574_H
#define __PCF8574_H

#include "main.h"
#include "myiic.h"

/* ── 引脚定义 ── */
#define PCF8574_GPIO_PORT              GPIOB
#define PCF8574_GPIO_PIN               GPIO_PIN_12        /* INT 中断输入         */
#define PCF8574_GPIO_CLK_ENABLE()      __HAL_RCC_GPIOB_CLK_ENABLE()

#define PCF8574_ADDR                   0x40               /* I2C 器件地址(含最低位) */

#define PCF8574_INT  HAL_GPIO_ReadPin(PCF8574_GPIO_PORT, PCF8574_GPIO_PIN)  /* 读 INT 脚 */

/* ── PCF8574 8路 IO 功能分配 ── */
#define BEEP_IO         0       /* 蜂鸣器                P0 */
#define AP_INT_IO       1       /* AP3216C 中断         P1 */
#define DCMI_PWDN_IO    2       /* DCMI 电源控制        P2 */
#define USB_PWR_IO      3       /* USB 电源控制         P3 */
#define EX_IO           4       /* 扩展 IO，自定义      P4 */
#define MPU_INT_IO      5       /* SH3001 中断          P5 */
#define RS485_RE_IO     6       /* RS485 收发使能       P6 */
#define ETH_RESET_IO    7       /* 以太网 PHY 硬件复位   P7 */

    /* ── API ── */
uint8_t pcf8574_init(void);                                 /* 初始化，返回0成功 */
uint8_t pcf8574_read_byte(void);                            /* 读取8位IO值       */
void    pcf8574_write_byte(uint8_t data);                   /* 写入8位IO值       */
void    pcf8574_write_bit(uint8_t bit, uint8_t sta);        /* 写某一位(0/1)     */
uint8_t pcf8574_read_bit(uint8_t bit);                      /* 读某一位          */

#endif
