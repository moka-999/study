# B1 MCU 固件

STM32H743IIT6/LQFP176 独立工程。工具链保持 STM32Cube CMake + Ninja + GNU Arm，25MHz HSE、400MHz内核，App位于0x08020000，带耦合复位安全关电的最小Boot位于0x08000000。

## 当前整合状态

V2通用软件已整合到B1，并保留B1板级映射。当前是**已编译的软件集成基线，不是真板验收完成**。

- PC：TCP:8888单客户端server，18字节头+JSON，4KiB上限；独立帧状态查表、命令查表、cJSON校验、本地心跳和W25测试仪信息查询。
- RK：独立TLV解析、完整错误传播、u64序号、单在途及心跳；UART4固定协议、不自动重发，USART2只接RK日志，USART1为可选来源的本地日志出口。
- W25Q256：32MiB布局、76字节版本、224字节FRU v2、A/B和CRC；兼容96字节版本只读转换和160/192字节旧FRU，保留旧身份/网络字段并保护有效未知格式。
- 电源/采样：按当前BRD由PC6统一控制4V及三颗TPS，PC7控制3.3V；PG11物理未接EN，但固件按防御性总闸时序驱动，有连接时提供额外关断、无连接时不影响PC6主路径。TPS上电后支持三轨独立原生步进（VDD1 10mV，VDD2/VDDQ 5mV）。仍不自动上电、设DAC、扫压或读采样器件。
- B1电源初值在`Core/Inc/board_power_config.h`：PC7先开→10ms→PC6开→5ms后写TPS，反序下电间隔10ms；关电停留500ms、Boot稳定10ms、切换上限2s、启动心跳上限60s。main装载但仍OFF。PC确认DDR安全范围，MCU校验TPS范围/步进；完整启动等本轮普通心跳，下电直接关轨，不等RK应答。初值不是实板验收，见[初版业务记录](../test/doc/b1_power_business_20261013.md)。
- 总线：UART TX锁/TXE修复，NORMAL RX DMA/IDLE/HT过滤，硬件与软件I2C入口保护，接口声明集中化。
- 复位：任何MCU复位都会由Boot立即拉低PG11、PC6、PC7，使RK/DDR进入OFF；之后必须由PC重新执行上电流程。

PC双方发送Seq独立递增、ServiceID固定0的合同已接入：每条连接首发Seq=1，发送成功后递增，u32回绕允许0。0x0503/0x0504提供只读电源逻辑状态。完整0103已接参数校验、必要冷启动及本轮RK心跳等待，按accepted→启动completed→0105上报；外部已启动模式为accepted→0105。两颗结果共用顶层siteId与measurementData，mirrorIndex固定0/1。最新接口见[PC协议](docs/protocol/pc/pc_tcp_protocol_spec.md)；真实电气及时序仍待首板验收。

20260920-2134版B1按三路20mΩ换算电流/功率，0602沿用原有字段；V2仍为10mΩ测试配置。Boot写入失败会使查询缓存失效。电源时序、TPS输出选装、RK启动及真实测量仍待实板验收，详见[适配记录](docs/schematic_adaptation_20260921.md)。

## 构建与检查

```powershell
.\tools\dev.ps1 check
.\tools\dev.ps1 build
```

产物为`build_debug/boot.elf`、`b1.elf`、`b1.bin`、`b1.manifest.json`和76字节`b1.version.bin`。烧录和运行测试需另外明确执行，本次整合未烧录或推送。

UART4固定连接RK UART0_M0运行协议，`B1_RK_UART`只允许4；USART2连接RK UART2_M0_DEBUG接收日志，USART1连接串口助手。三路RX在RTOS运行后启动；RK未上电时UART4不主动发命令或心跳，日志接收不修改电源门禁。

USART1、USART2当前均为115200/8N1、无流控（RK调试端实际波特率待真板核对）。串口助手使用文本发送`log mcu`或`log rk`并附CR、LF或CRLF；默认MCU，只切换本次运行，重启恢复构建默认值。编译时可用：

```powershell
.\tools\dev.ps1 build -LogSource MCU
.\tools\dev.ps1 build -LogSource RK
```

不传`-LogSource`沿用该构建目录的CMake缓存，首次为MCU；对应`B1_LOG_DEFAULT_SOURCE=MCU|RK`。运行时两种来源互斥，切换命令不会发给RK，不写Flash或新增TCP字段。用法、离线验证及实板限制见[串口日志切换](docs/uart_log_console_20261010.md)。

## 必须保留的B1差异

- PHY复位是PG12，不是RK复位；MAC由STM32 UID生成，不使用V2固定MAC。
- SPI2是B2主机的Mode 0从机，仅保活；AD7124-4使用SPI4/PE11-PE14、Mode 3、约390.625kHz。
- PH4/PH5保持软件开漏I2C，不是STM32硬件I2C4 AF。
- D-Cache关闭；FreeRTOS堆与UART DMA在AXI SRAM，ETH缓冲及预留LwIP堆在D2 SRAM。
- 三组PG0-PG5仅显示测试结果：FAIL/PASS通道常亮分别表示失败/通过，双色同步闪烁表示常温测试、FAIL通道闪烁表示高温测试。保留现有通道映射，实际颜色及测试位置到板后确认。
- PG6表示B1 MCU正常上电：初始化期间灭，默认任务完成初始化后常亮，不随TCP/RK通信闪烁，也不表示RK/DDR已经上电。
- PB3～PB5为GPIO输入、无内部上下拉，使用B1外部4.7kΩ上拉和B2贴装接地编码；`board_slot_code()`返回PB5:PB4:PB3的原始0～7槽位码。启动时通过MCU日志输出一次，111也是有效槽位；不改IP、FRU或PC siteId。B2在位检测使用独立的SLOT_ONLINE#。
- B1不控制、代理或转发温控，PC直接管理B2；不复制V2自检、波形任务或旧TLV GPIO重启代码。

`b1.ioc`保存引脚/时钟与RX DMA配置；重新生成时仍须恢复App偏移、链接段、B1正式任务创建和板级适配，不能用V2生成代码覆盖。

## 文档入口

- [正常上电灯、槽位读取与真板验收](docs/board_status_slot_20261010.md)

- [新版原理图软件适配及验证](docs/schematic_adaptation_20260921.md)

- [OTA同步、B1专用Boot和验证边界](docs/ota_sync_20260916.md)

- [整合清单、初版BRD MCU核对、验证与待办](docs/integration_20260831.md)
- [PC协议目标框架](docs/protocol/pc/pc_tcp_protocol_spec.md)
- [RK当前实现基线与差异](docs/protocol/rk/implementation_baseline.md)

仍使用默认IPv4 `192.168.1.201/24`，不会因MAC唯一就自动得到不同IP；8板并网前必须落实唯一IP配置。现工程PHY型号标记仍为YT8512H，与先前确认的YT8512C需核对BOM/芯片ID；本次保留现有PHY实现，不以编译通过代替型号和硬件确认。
