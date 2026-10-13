/*
 * B1板级总线实例的唯一声明入口。
 * 接口类型在hal_if.h；本头只列出实际提供的B1实例，不沿用V2板级映射。
 * SPI4供AD7124使用；SPI2是B2保活从机，不在器件主机接口中导出。
 * UART4固定RK协议，USART2只接收RK日志，USART1为可切换日志的本地控制台。
 */
#ifndef BOARD_INTERFACES_H
#define BOARD_INTERFACES_H

#include "hal_if.h"

extern i2c_if_t i2c1_if;
extern i2c_if_t i2c2_if;
extern i2c_if_t i2c4_if; /* PH4/PH5软件开漏GPIO，不是STM32硬件I2C4。 */
extern spi_if_t spi4_if;
extern uart_if_t uart4_if;
extern uart_if_t uart2_if;
extern uart_if_t uart1_if;

#endif /* BOARD_INTERFACES_H */
