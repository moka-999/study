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

/* 初始化及写入返回0成功、-1失败；每个地址/数据字节都要求ACK。 */
int pcf8574_init(void);
/* 读返回0..255或-1；引脚电平不等于内部输出锁存器。 */
int pcf8574_read_byte(void);
/* P1/P5始终写1保持输入；成功时刷新软件锁存，失败后须完整重写再按位操作。 */
int pcf8574_write_byte(uint8_t data);
/* bit=0..7，state=0/1；禁止把P1/P5输入改成输出低。 */
int pcf8574_write_bit(uint8_t bit, uint8_t state);
/* 返回0/1；参数、总线错误返回-1，不伪装成高电平。 */
int pcf8574_read_bit(uint8_t bit);

#endif
