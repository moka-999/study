# B1 MCU 引脚映射 (STM32H743IIT6)

> 来源: MYZR-RK3588-ZUOYI_SCH V2.2, 24页
> MCU 在 MB360 子板 p21-p22

## ETH RMII (不变)
| STM32 引脚 | ETH 信号 | 说明 |
|-----------|---------|------|
| PA1 | ETH_REF_CLK | 50MHz 参考时钟 |
| PA2 | ETH_MDIO | 管理数据 |
| PA7 | ETH_CRS_DV | 载波检测 |
| PC1 | ETH_MDC | 管理时钟 |
| PC4 | ETH_RXD0 | 接收数据 |
| PC5 | ETH_RXD1 | 接收数据 |
| PB11 | ETH_TX_EN | 发送使能 |
| PG13 | ETH_TXD0 | 发送数据 |
| PG14 | ETH_TXD1 | 发送数据 |
| PA0 | ETH_nRST | PHY 复位 (GPIO) |

## UART ↔ RK3588
| UART | STM32 引脚 | 说明 |
|------|-----------|------|
| USART1 TX/RX | PA9/PA10 | → RS0102 电平转换 → RK3588 主通道 |
| USART2 TX/RX | PD5/PD6 | → RS0102 电平转换 → RK3588 DEBUG |

## I2C
| 总线 | 引脚 | 设备 |
|------|------|------|
| I2C1 | PB6/PB7 | TPS62868 U4025 (VDD1) |
| I2C2 | PH4/PH5 | TPS62868 U4026 (VDD2) |
| I2C4 | PF14/PF15 | TPS62868 U4027 (VDDQ) + PAC1934T |

## QSPI Flash (W25Q256, 不变)
| 引脚 | 信号 |
|------|------|
| PF10 | CS |
| PG6 | CLK |
| PF8/PF9 | IO0/IO1 (SIO) |
| PF6/PF7 | IO2/IO3 (QIO) |
| PE2 | IO2 |

## SPI
| SPI | 引脚 | 连接 |
|-----|------|------|
| SPI2 | PB13(SCK)/PB14(MISO)/PB15(MOSI)/PB12(CS) | DIMM |
| SPI4 | PE10-14 | AD7124 |

## 电源控制 GPIO
| GPIO | 控制 | 说明 |
|------|------|------|
| PC6 | MP8759 EN | B1 上电时序第1步 |
| PC7 | RT8070 EN | 第2步 |
| PG11 | TPS62868 ×3 EN | 第3步 (I2C 配置电压后拉高) |

## 上电时序: PC6 HIGH → I2C配 TPS62868 → PG11 HIGH

## LED
7路双色 (14 GPIO), 具体引脚见 p22
