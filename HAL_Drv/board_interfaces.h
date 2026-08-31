/*
 * Apollo V2外设接口实例的唯一声明入口。
 * hal_if.h只定义接口类型；本文件列出实际实例，*_hw.c负责HAL绑定。
 * 普通调用方不得重新绑定实例；Mock可暂时替换，但须保存/恢复且隔离运行。
 * B1为独立工程，移植时按其引脚/外设配置提供实例，不直接复制V2绑定。
 */
#ifndef BOARD_INTERFACES_H
#define BOARD_INTERFACES_H

#include "hal_if.h"

extern i2c_if_t i2c1_if;
extern i2c_if_t i2c2_if; /* V2占位，handle=NULL；调用返回IF_ERR。 */
extern i2c_if_t i2c3_if;
extern i2c_if_t i2c4_if;
extern spi_if_t spi2_if; /* V2的AD7124开发总线，不是B1的SPI2保活从机。 */
extern uart_if_t uart1_if;
extern uart_if_t uart2_if;

#endif /* BOARD_INTERFACES_H */
