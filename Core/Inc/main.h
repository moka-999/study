#ifndef B1_PINCHECK_MAIN_H
#define B1_PINCHECK_MAIN_H

#include "stm32h7xx_hal.h"

/* 板级HAL句柄统一声明；业务层使用board_interfaces.h中的总线接口。 */
extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;
extern UART_HandleTypeDef huart4;
extern DMA_HandleTypeDef hdma_uart4_rx;
extern DMA_HandleTypeDef hdma_usart2_rx;
extern DMA_HandleTypeDef hdma_usart1_rx;

/* 以太网RMII及PHY复位。 */
#define B1_RMII_REF_CLK_PORT GPIOA
#define B1_RMII_REF_CLK_PIN  GPIO_PIN_1
#define B1_ETH_MDIO_PORT      GPIOA
#define B1_ETH_MDIO_PIN       GPIO_PIN_2
#define B1_RMII_CRS_DV_PORT   GPIOA
#define B1_RMII_CRS_DV_PIN    GPIO_PIN_7
#define B1_ETH_MDC_PORT       GPIOC
#define B1_ETH_MDC_PIN        GPIO_PIN_1
#define B1_RMII_RXD0_PORT     GPIOC
#define B1_RMII_RXD0_PIN      GPIO_PIN_4
#define B1_RMII_RXD1_PORT     GPIOC
#define B1_RMII_RXD1_PIN      GPIO_PIN_5
#define B1_RMII_TX_EN_PORT    GPIOB
#define B1_RMII_TX_EN_PIN     GPIO_PIN_11
#define B1_RMII_TXD0_PORT     GPIOG
#define B1_RMII_TXD0_PIN      GPIO_PIN_13
#define B1_RMII_TXD1_PORT     GPIOG
#define B1_RMII_TXD1_PIN      GPIO_PIN_14
#define B1_ETH_NRST_PORT       GPIOG
#define B1_ETH_NRST_PIN        GPIO_PIN_12

/* W25Q256的QSPI接口。 */
#define B1_QSPI_CLK_PORT       GPIOB
#define B1_QSPI_CLK_PIN        GPIO_PIN_2
#define B1_QSPI_NCS_PORT       GPIOB
#define B1_QSPI_NCS_PIN        GPIO_PIN_6
#define B1_QSPI_IO3_PORT       GPIOF
#define B1_QSPI_IO3_PIN        GPIO_PIN_6
#define B1_QSPI_IO2_PORT       GPIOF
#define B1_QSPI_IO2_PIN        GPIO_PIN_7
#define B1_QSPI_IO0_PORT       GPIOF
#define B1_QSPI_IO0_PIN        GPIO_PIN_8
#define B1_QSPI_IO1_PORT       GPIOF
#define B1_QSPI_IO1_PIN        GPIO_PIN_9

/* 电源、监测及启动模式使用的I2C总线。 */
#define B1_I2C1_SDA_PORT       GPIOB
#define B1_I2C1_SDA_PIN        GPIO_PIN_7
#define B1_I2C1_SCL_PORT       GPIOB
#define B1_I2C1_SCL_PIN        GPIO_PIN_8
#define B1_I2C2_SDA_PORT       GPIOF
#define B1_I2C2_SDA_PIN        GPIO_PIN_0
#define B1_I2C2_SCL_PORT       GPIOF
#define B1_I2C2_SCL_PIN        GPIO_PIN_1
#define B1_I2C4_SCL_PORT       GPIOH
#define B1_I2C4_SCL_PIN        GPIO_PIN_4
#define B1_I2C4_SDA_PORT       GPIOH
#define B1_I2C4_SDA_PIN        GPIO_PIN_5

/* SPI2为B2通信从机；SPI4为AD7124主机。 */
#define B1_SPI2_NCS_PORT       GPIOB
#define B1_SPI2_NCS_PIN        GPIO_PIN_12
#define B1_SPI2_SCK_PORT       GPIOB
#define B1_SPI2_SCK_PIN        GPIO_PIN_13
#define B1_SPI2_MISO_PORT      GPIOB
#define B1_SPI2_MISO_PIN       GPIO_PIN_14
#define B1_SPI2_MOSI_PORT      GPIOB
#define B1_SPI2_MOSI_PIN       GPIO_PIN_15
#define B1_SPI4_NCS_PORT       GPIOE
#define B1_SPI4_NCS_PIN        GPIO_PIN_11
#define B1_SPI4_SCK_PORT       GPIOE
#define B1_SPI4_SCK_PIN        GPIO_PIN_12
#define B1_SPI4_MISO_PORT      GPIOE
#define B1_SPI4_MISO_PIN       GPIO_PIN_13
#define B1_SPI4_MOSI_PORT      GPIOE
#define B1_SPI4_MOSI_PIN       GPIO_PIN_14

/* USART1本地日志出口；UART4对接RK协议；USART2只收RK调试日志。 */
#define B1_USART1_TX_PORT      GPIOA
#define B1_USART1_TX_PIN       GPIO_PIN_9
#define B1_USART1_RX_PORT      GPIOA
#define B1_USART1_RX_PIN       GPIO_PIN_10
#define B1_UART4_RX_PORT       GPIOA
#define B1_UART4_RX_PIN        GPIO_PIN_11
#define B1_UART4_TX_PORT       GPIOA
#define B1_UART4_TX_PIN        GPIO_PIN_12
#define B1_USART2_TX_PORT      GPIOD
#define B1_USART2_TX_PIN       GPIO_PIN_5
#define B1_USART2_RX_PORT      GPIOD
#define B1_USART2_RX_PIN       GPIO_PIN_6

/* PC6/PC7是真实电源使能；PG11到TPS EN为NC，仅按防御性总闸时序驱动。 */
#define B1_PWR_4V0_EN_PORT     GPIOC
#define B1_PWR_4V0_EN_PIN      GPIO_PIN_6
#define B1_PWR_3V3_EN_PORT     GPIOC
#define B1_PWR_3V3_EN_PIN      GPIO_PIN_7
#define B1_PWR_DDR_EN_PORT     GPIOG
#define B1_PWR_DDR_EN_PIN      GPIO_PIN_11

/* B2电阻贴装形成槽位码；PB3为最低位，三脚只作输入，使用板上4.7kΩ上拉。 */
#define B1_SLOT_ID0_PORT        GPIOB
#define B1_SLOT_ID0_PIN         GPIO_PIN_3
#define B1_SLOT_ID1_PORT        GPIOB
#define B1_SLOT_ID1_PIN         GPIO_PIN_4
#define B1_SLOT_ID2_PORT        GPIOB
#define B1_SLOT_ID2_PIN         GPIO_PIN_5

/* 三颗双色结果灯；LED7/PG6为MCU正常上电指示，初始化完成后高电平常亮。 */
#define B1_LED1_PORT           GPIOG
#define B1_LED1_PIN            GPIO_PIN_0
#define B1_LED2_PORT           GPIOG
#define B1_LED2_PIN            GPIO_PIN_1
#define B1_LED3_PORT           GPIOG
#define B1_LED3_PIN            GPIO_PIN_2
#define B1_LED4_PORT           GPIOG
#define B1_LED4_PIN            GPIO_PIN_3
#define B1_LED5_PORT           GPIOG
#define B1_LED5_PIN            GPIO_PIN_4
#define B1_LED6_PORT           GPIOG
#define B1_LED6_PIN            GPIO_PIN_5
#define B1_LED7_PORT           GPIOG
#define B1_LED7_PIN            GPIO_PIN_6

void Error_Handler(void);

#endif
