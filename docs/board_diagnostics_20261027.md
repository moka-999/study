# B1 首板串口诊断（2026-10-27）

## 使用

只用于B1；V2的USART1仍用于RK协议，本次不改变V2。

- 本地调试口：USART1，PA9为MCU TX、PA10为MCU RX，115200/8N1，无流控。使用3.3V TTL转串口，TX/RX交叉、GND共地；不是RS232电平。
- 先打开串口助手再上电，文本模式发送命令，附加CR、LF或CRLF。
- 默认MCU日志；log mcu选择MCU，log rk选择RK原始日志，status在MCU模式下输出一份状态。RK模式下status只提示先切回MCU，不混入诊断报告。
- 启动完成后自动报告一次，之后不周期刷屏；电源流程原有[power]阶段日志保留。高频采样每次读取不打印。
- 只编译：在B1工程执行 tools/dev.ps1 build -LogSource MCU。当前产物 D:/pro/b1/build/standalone/Debug/b1.elf，从0x08000000启动，无MCU Boot/OTA。

## 可以看到什么

| 标记 | 内容与判读 |
|---|---|
| [boot] | USART1建立后依次记录其他UART/DMA、电源配置、I2C、SPI、QSPI、W25、看门狗/LED、MAC/PHY复位、内核创建和lwIP阶段 |
| FAILED at W25 device init | W25初始化返回错误并进入Error_Handler；不会继续启动业务。最后阶段也保存在s_boot_stage供SWD观察 |
| [slot] | GPIO原始码、加1的候选site、B2已生效site；assignedSite=0表示未收到B2正式配置，不冒充已经分配 |
| [net] | 当前IPv4/MAC、netif的up/link缓存；link=1也不等于PC已连接 |
| [power] state=... PC6=... PC7=... | 电源状态机及输出锁存；不是4V/3.3V实测，更不是DDR已训练成功 |
| `[RK DDR] currentMiB`、`lastCompletedMiB` | 当前整轮及最近一次成功 0107 结束的整轮所识别的 DDR 总容量，单位 MiB。当前轮只在 0106 至 0107 之间显示。 |
| [monitor] | AD/PAC最近发布的就绪状态、AD ID及错误；默认OFF时ready=0正常，不能据此判定芯片损坏 |
| [sample] | 快照年龄和有效位：1电压、2电流、4TPS；无效组只显示unavailable，不打印旧值或假零值 |
| `[VDD1/2/Q mV]`、`[PAC VBUS 1/2/Q mV]` | 依次显示 AD7124 输入电压和 PAC1934 总线电压，每行按 VDD1、VDD2、VDDQ 排列，单位 mV。两行取自同一份监测快照，两颗器件各按自身采样周期更新，适合在负载稳定时比较。 |
| `[IDD1/2/Q uA]` | PAC1934 最近一次有效采样的三路电流，与上方 PAC 电压共用有效位。PAC 采样无效时，两行均显示 `unavailable`。 |
| [TPS status 1/2/Q] | 监测任务已读取的三个原始状态字节；查询不再读取/清除器件状态 |
| [UART4-RK]等 | 每路 UART 的累计错误、最近 HAL 错误位、丢块、启动 RX 失败及待恢复状态。rxFull 统计接收队列或字节环满；USART2 的 HAL=0x8 表示硬件接收溢出 |
| [B2 SPI] | CS事务、收到请求、硬件错误、队列丢弃、合法/非法帧及在线状态；未接B2时0事务/离线正常 |
| [RK history] | 历史拒绝次数、最近拒绝原因和tick，不能当成当前busy；原因0无、1无在途、2Seq、3算法、4进度、5结果、6TLV |
| `freeHeap`、`minFreeHeap`、`largestFreeBlock` | 单位为字节，依次为 FreeRTOS 当前空闲堆、本次开机以来的最低余量、查询时最大的连续空闲块（含分配器块头）。 |
| `[stack minFreeBytes]` | 各任务创建以来按栈填充值测得的最小剩余字节数。`tcp` 是 8888 业务任务，`rk` 是 RK 协议任务，`defaultTask` 处理调试窗口和状态快照。数值越小，表示该任务在最忙时的栈余量越少。 |

ADlastError保留历史非零ERROR；PAClastError会被后续操作覆盖，0无、1参数、2IO、3ID、4配置、5数据、6量程。应结合ready、有效位和实际测量分析；不同模块在报告生成时分别读取，不宣称全板同一瞬间的原子快照。

## RK 内存容量

MCU 在 USART2 接收 DDRbin 启动日志时提取容量。串口和 TCP 8889 都可以在 MCU 模式发送 `status` 查看。

- 成功的 `0106` 建立整轮。MCU 在原 TCP 8888 连接上主动上报一包 `010D`。PC 可立即下发首项 FUNC、PARA 或 SLT。MCU 受理请求并准备启动，完整 `010D` 进入 TCP 发送缓冲且正常启动条件满足后才投递算法或放行 EDK。PC 无需应答。
- DDRbin 的 `LPDDR4,` / `LPDDR4X,` 行给出本次启动的类型。镜像 0 取 CH0 + CH1，镜像 1 取 CH2 + CH3。每条 `Size=…MB` 已包含该通道全部 Rank，单位按 MiB 处理。同一通道重复出现只计一次。首项冷启动使用新日志，沿用已运行的 RK 才复用同一次启动的缓存。
- 四路容量和类型读齐，或已读到至少一路容量并遇到 DDRbin 表尾（`Manufacturer ID:` 或单独一行 `out`）时，即可上报。冷启动从开始供电（`waitOn`）起计时，复用已运行的 RK 从首项受理起计时。`B1_RK_CHIP_INFO_TIMEOUT_MS` 默认 10000 ms。USART2 中途断流时丢弃被截断的行，保留同次启动中已完整识别的通道与类型。
- 到期按已读通道求和。例如只识别 CH0=1024 MiB 和 CH2=512 MiB 时，两颗分别报 1024 和 512。镜像两路均无记录时报 0，类型行缺失或该镜像无通道记录时类型为空串。每轮只上报一次。通过 TCP 8889 或调试串口的 `log mcu` 查看 `[PC TX] ... cmd=010D`，成功入队时该行带有实际发送的 JSON 正文，可直接和 PC 显示比较。
- `0107` 成功后，`status` 显示最近结束轮各已识别通道的容量之和。没有有效容量时显示 `unknown`，尚无已结束轮时显示 `none`。断线或关站丢弃未完成轮。

四个通道各 1024 MiB 时的 `010D` 正文如下，`MemorySize` 为无符号 64 位十进制 JSON 整数。

```json
{"chipinfo":[{"mirrorIndex":0,"MemoryType":"LPDDR4X","MemorySize":2048},{"mirrorIndex":1,"MemoryType":"LPDDR4X","MemorySize":2048}]}
```

## 代码边界

- Core/Src/board_diagnostics.c集中组织诊断。main把USART初始化移到安全GPIO/电源OFF之后，先建立日志口，再初始化其他外设；未增加上电、Flash写入或探测命令。
- 调度前按编译默认来源短时输出；每次HAL发送超时20ms，只在非ISR且中断未屏蔽时调用。在创建内核对象前永久关闭，后续全部经过原日志路由。
- 若时钟/USART1尚未建立，或内核创建/运行期进入Error_Handler，不能保证串口打印失败文字；应结合最后阶段与SWD。HardFault也没有新增不安全的printf。
- log_console.c只解释本地命令和管理输出，查询通过一位请求交给默认任务。monitor_task.c只额外发布少量已有器件状态，getter不取总线锁、不访问器件。
- 报告使用1536字节静态缓冲、每10ms最多96字节非阻塞入队，出口忙最多保留2秒；切换来源取消剩余报告。仍是诊断出口，拥塞/接线问题可丢日志。
- DDRbin 识别复用 B1 默认任务的 USART2 接收路径，TCP 任务负责生成、发送 `010D`。

## 验证

- B1 MCU默认完整ARM构建及0x08000000向量/容量检查通过，BIN为253652字节。
- 实际board_diagnostics.c在QEMU执行：MCU/RK两种编译默认、早期错误提示、ISR/中断屏蔽拒绝、永久关闭早期输出、只报告一次、有效/无效测量、分块限速、拥塞重试/2秒丢弃、切换取消、尚无快照均通过。
- 实际日志模块和三路UART路由：两种默认、status分片/合并/确认优先/来源隔离及原日志回归通过。
- 实际监测任务：OFF不初始化、快速采样/窗口/缓存原回归与只读诊断不增加IO通过。
- B1现有板级、PC、电源、独立启动检查通过。代码已编译，尚未烧录到B1；时钟、串口电气、器件应答、精度和现场时序仍需实板验证。

复跑脚本在同级V2仓库：tests/verify_b1_diagnostics_c.py、tests/verify_uart_idd_c.py、tests/verify_monitor_boot.py，传入 --app-root D:/pro/b1 --compiler <arm-none-eabi-gcc.exe>；UART脚本分别加 --log-default 0/1。本次未连接串口、网络或ST-Link，未推送。
