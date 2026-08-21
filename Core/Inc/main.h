#ifndef B1_PINCHECK_MAIN_H
#define B1_PINCHECK_MAIN_H

#include "stm32h7xx_hal.h"

/* Ethernet RMII + PHY reset */
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

/* W25Q256 QSPI */
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

/* I2C power, monitor and boot buses */
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

/* B1 master to B2 MCU, and AD7124 */
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

/* Local console and two RK candidates */
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

/* Power enables default OFF */
#define B1_PWR_4V0_EN_PORT     GPIOC
#define B1_PWR_4V0_EN_PIN      GPIO_PIN_6
#define B1_PWR_3V3_EN_PORT     GPIOC
#define B1_PWR_3V3_EN_PIN      GPIO_PIN_7
#define B1_PWR_DDR_EN_PORT     GPIOG
#define B1_PWR_DDR_EN_PIN      GPIO_PIN_11

/* Three bicolor result LEDs plus MCU-controlled status LED */
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
