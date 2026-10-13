# B1 正常上电指示与槽位读取（2026-10-10）

## 本次修改

| 项目 | 实现及边界 |
|---|---|
| PG6 / LED7 | 高有效。GPIO初始化先灭；默认任务执行完串口、日志和LwIP初始化后点亮并保持，不增加任务或闪烁逻辑。只表示MCU完成启动初始化，不证明网络连通、RK/DDR上电、实测电压正常或OTA健康确认。 |
| PB3 / ID0 | 输入、无内部上下拉；R7296 4.7kΩ上拉至3V3_MCU，槽位码最低位。 |
| PB4 / ID1 | 输入、无内部上下拉；R7297 4.7kΩ上拉，槽位码中间位。 |
| PB5 / ID2 | 输入、无内部上下拉；R7298 4.7kΩ上拉，槽位码最高位。 |
| 读取接口 | `board_slot_code()`返回`uint8_t`原始0～7，GPIOB IDR一次取样；每次调用直接读引脚，不加1、不写Flash。只能在GPIO初始化后调用。 |
| 日志 | 默认任务启动时输出一次`B1 slot code: N (PB5..PB3)`；复用USART1 MCU日志出口，默认来源为RK时该MCU日志被过滤，不周期刷屏。 |
| PG0～PG5 | 保留原有FAIL/PASS通道、500ms亮灭逻辑。图纸料号L-C295JFLBCT的手册为琥珀/蓝，但实际灯色及测试位置均等板到后确认。 |

B2在对应位贴0Ω接地电阻表示0、不贴表示1，B1不能把PB3～PB5设成推挽输出高。
位序为`PB5 PB4 PB3 = ID2 ID1 ID0`：000是码0，001是码1，111是码7。
未插B2也可能读111，不能据此判断在位；B2使用独立的`SLOT_ONLINE#`。

本次不将硬件码自动映射到PC siteId，不改FRU、MAC、IP、SPI载荷或TCP字段。
生产IP仍由B2通知；原始0～7码不能直接替代产线规则里的1～8编号。
PG6不属于结果灯驱动；PC设置或清除测试结果灯不改变PG6。

## 变更位置

- `Core/Inc/main.h`、`Core/Src/main.c`：引脚定义及安全输入配置。
- `Core/Inc/board_identity.h`、`Core/Src/board_identity.c`：槽位读取接口及中文合同。
- `Core/Src/freertos.c`：在既有默认任务初始化完成点点亮PG6并读取槽位日志。
- `b1.ioc`：同步输入及无内部上下拉配置；保留PA13/PA14的SWD，PG6初值仍为低。
- `tests/verify_project.py`：检查引脚、CubeMX配置、PG6点亮顺序及结果灯隔离。
- `tests/test_board_identity.c`、`tests/verify_board_identity.py`：执行真实C读取代码，GPIOB仅以离线内存替代；接入`tools/dev.ps1 check`。

## 依据与验证

依据B1 `MYZR-RK3588-ZUOYI_SCH_20260920-2134.pdf`第19、21、24页，B2 `JMCT-B2-20260917.pdf`第7、12～19页，以及`../../test/output/hardware_review/B2_原理图问题表_20260920_含槽位贴装.xlsx`。

- 修改前原有B1检查通过；新增C测试先因缺少槽位接口失败，补齐后通过。
- `powershell -ExecutionPolicy Bypass -File tools/dev.ps1 check`：49/49引脚、现有安全/协议/元数据检查及32种GPIO输入组合和再次读取通过。
- `powershell -ExecutionPolicy Bypass -File tools/dev.ps1 build`：B1 Debug编译通过。仅软件验证，无烧录、真板或网络操作，不代表硬件验收。

到板后检查：八槽编码是否按贴装表不同、PB3～PB5实测电平、PG6先灭后常亮，以及三颗双色灯的实际颜色/对象顺序。
如果B2八槽接地电阻全部装上，所有B1都会读取000；软件不会自动猜测或修正贴装错误。
