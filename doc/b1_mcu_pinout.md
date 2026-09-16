> **已按20260811原理图修订（2026-08-21）**：最终仍以BOM、最新版原理图和真板测量为准。

# B1 MCU 引脚映射 (STM32H743IIT6)

> 来源: MYZR-RK3588-ZUOYI_SCH V2.2, 24页
> MCU 在 MB360 子板 p21-p22
> 电源章节于2026-09-14按0831版完整原理图复核；其它映射仍按各自验证范围理解，详见[电源控制记录](b1_power_control_review.md)。

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
| PG12 | ETH_nRST | PHY复位；PA0是WK-UP，不能用于PHY复位 |

## UART ↔ RK3588
| UART | STM32 引脚 | 说明 |
|------|-----------|------|
| UART4 TX/RX | PA12/PA11 | → RS0102 U4008 → RK3588 UART0_M0/BMC console候选 |
| USART2 TX/RX | PD5/PD6 | → RS0102 U4013 → RK3588 UART2_M0_DEBUG候选 |
| USART1 TX/RX | PA9/PA10 | 板载本地串口；不是最终RK主链路 |

## I2C
| 总线 | 引脚 | 设备 |
|------|------|------|
| I2C1 | PB7(SDA)/PB8(SCL) | TPS62868 U4025 (VDD1) |
| I2C2 | PF0(SDA)/PF1(SCL) | TPS62868 U4026 (VDD2) + MCP4725 |
| 逻辑I2C4 | PH4(SCL)/PH5(SDA)，软件开漏GPIO（非STM32 I2C4 AF引脚对） | TPS62868 U4027 (VDDQ) + PAC1934T |

## QSPI Flash（W25Q256，当前固件/Apollo映射）
| 引脚 | 信号 |
|------|------|
| PB6 | NCS |
| PB2 | CLK |
| PF8/PF9 | IO0/IO1 |
| PF7/PF6 | IO2/IO3 |

> 最终B1必须对照最新版原理图复核；当前CubeMX生成代码使用上述映射。

## SPI
| SPI | 引脚 | 连接 |
|-----|------|------|
| SPI2 | PB13(SCK输入)/PB14(MISO输出)/PB15(MOSI输入)/PB12(NSS输入) | B1从机↔B2共享总线主机；B2以每槽独立CS选择8块B1，Mode 0、8位MSB、硬件NSS低有效 |
| SPI4 | PE12(SCK)/PE13(MISO)/PE14(MOSI)/PE11(NSS) | AD7124 |

## 电源控制 GPIO
| GPIO | 控制 | 说明 |
|------|------|------|
| PC6 | MP8759 EN | VCC4V0_SYS，同时给RK806和三颗TPS供电；不是TPS独立电源 |
| PC7 | RT8070 EN | VCC3V3_SYS，不同于RK806的VCC_3V3_S3；不能单独阻止RK开始训练 |
| PG11 | TPS62868 ×3 EN预留 | 0831图R4073/R4081/R4086为NC；EN经10kΩ上拉至4V，实际随PC6启动 |

## 上电时序

MCU由独立3V3_MCU供电，可先准备总线/Boot和训练目标，再开启PC6。TPS与RK806共享4V，应验证在RK806自动上电并释放RK复位前完成调压的时间余量；不能用PG11或PC7虚构独立的“RK放行”动作。PC6/PC7顺序、真实DDR建立/掉电时序及失败撤销时间均待B1实测，见[完整核对与验收条件](b1_power_control_review.md)。

## LED
PG0–PG5控制三颗红绿双色结果灯，PG6控制一颗单色状态灯；另一颗单色灯由`CPU_GPIO_PWR`控制。结果灯具体物理顺序待确认。
