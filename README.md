# B1 MCU 固件

STM32H743IIT6/LQFP176 独立工程。工具链保持 STM32Cube CMake + Ninja + GNU Arm，25MHz HSE、400MHz内核，App位于0x08020000，带耦合复位安全关电的最小Boot位于0x08000000。

## 当前整合状态

V2通用软件已整合到B1，并保留B1板级映射。当前是**已编译的软件集成基线，不是真板验收完成**。

- PC：TCP:8888单客户端server，18字节头+JSON，4KiB上限；独立帧状态查表、命令查表、cJSON校验、本地心跳和W25测试仪信息查询。
- RK：独立TLV解析、完整错误传播、u64序号、pending/重传/心跳；UART4/USART2只选一路，USART1仍是本地控制台。
- W25Q256：32MiB布局、76字节版本、224字节FRU v2、A/B和CRC；兼容96字节版本只读转换和160/192字节旧FRU，保留旧身份/网络字段并保护有效未知格式。
- 电源/采样：按当前BRD由PC6统一控制4V及三颗TPS，PC7控制3.3V；PG11物理未接EN，但固件按防御性总闸时序驱动，有连接时提供额外关断、无连接时不影响PC6主路径。TPS上电后仍支持三轨独立20mV步进。仍不自动上电、设DAC、扫压或读采样器件。
- 总线：UART TX锁/TXE修复，NORMAL RX DMA/IDLE/HT过滤，硬件与软件I2C入口保护，接口声明集中化。
- 复位：任何MCU复位都会由Boot立即拉低PG11、PC6、PC7，使RK/DDR进入OFF；之后必须由PC重新执行上电流程。

PC双方发送Seq独立递增、ServiceID固定0的合同已接入：每条连接首发Seq=1，发送成功后递增，u32回绕允许0。0x0503/0x0504已提供只读电源逻辑状态查询；START_TEST目前仍仅校验JSON，没有投递RK或回传最终结果。

## 构建与检查

```powershell
.\tools\dev.ps1 check
.\tools\dev.ps1 build
```

产物为`build_debug/boot.elf`、`b1.elf`、`b1.bin`、`b1.manifest.json`和76字节`b1.version.bin`。烧录和运行测试需另外明确执行，本次整合未烧录或推送。

CMake选项`B1_RK_UART=4`暂选UART4，设为2可切换USART2；这不是已确认的RK实机串口。默认只注册slot0的一路。RX在RTOS运行后启动，RK未上电时不主动发命令或心跳。

## 必须保留的B1差异

- PHY复位是PG12，不是RK复位；MAC由STM32 UID生成，不使用V2固定MAC。
- SPI2是B2主机的Mode 0从机，仅保活；AD7124-4使用SPI4/PE11-PE14、Mode 3、约390.625kHz。
- PH4/PH5保持软件开漏I2C，不是STM32硬件I2C4 AF。
- D-Cache关闭；FreeRTOS堆与UART DMA在AXI SRAM，ETH缓冲及预留LwIP堆在D2 SRAM。
- 三组PG0-PG5仅显示测试结果：琥珀常亮FAIL、蓝色常亮PASS、双色同步闪烁表示常温测试、琥珀闪烁表示高温测试。单色PG6语义未定，不能借用为通信指示灯。
- B1不控制、代理或转发温控，PC直接管理B2；不复制V2自检、波形任务或旧TLV GPIO重启代码。

`b1.ioc`保存引脚/时钟与RX DMA配置；重新生成时仍须恢复App偏移、链接段、B1正式任务创建和板级适配，不能用V2生成代码覆盖。

## 文档入口

- [OTA同步、B1专用Boot和验证边界](docs/ota_sync_20260916.md)

- [整合清单、初版BRD MCU核对、验证与待办](docs/integration_20260831.md)
- [PC协议目标框架](docs/protocol/pc/pc_tcp_protocol_spec.md)
- [RK当前实现基线与差异](docs/protocol/rk/implementation_baseline.md)

仍使用默认IPv4 `192.168.1.201/24`，不会因MAC唯一就自动得到不同IP；8板并网前必须落实唯一IP配置。现工程PHY型号标记仍为YT8512H，与先前确认的YT8512C需核对BOM/芯片ID；本次保留现有PHY实现，不以编译通过代替型号和硬件确认。
