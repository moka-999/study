# B1 三串口分工与日志切换

2026-10-10 用户确认 UART4 跑 RK 协议，USART2 接收 RK 调试日志，USART1 输出可选日志；同时支持编译默认值和运行时切换。本记录只描述 B1，V2 的 USART1 仍用于 RK 协议。

## 1. 接线与用途

| MCU 接口 | 引脚 | 对端 / 用途 |
|---|---|---|
| UART4 | PA12 TX、PA11 RX | 经 U4008/RS0102 接 RK UART1_M0，专用 TLV 协议；按20260920-2134原图第6/14页纠正旧记载 |
| USART2 | PD5 TX、PD6 RX | 经 U4013/RS0102 接 RK UART2_M0_DEBUG；双向调试转发，输入只在RK模式发送 |
| USART1 | PA9 TX、PA10 RX | 接外部串口助手，输出 MCU 日志或转发 RK 日志，MCU模式接收本地命令，RK模式转发输入 |

三路沿用 **115200、8 数据位、1 停止位、无校验、无流控**。RK 调试端实际波特率尚未实测，不宣称已经匹配；若不同，应核对 RK 配置并同步调整日志输入/出口速率，不能仅依赖加大缓冲。

## 2. 操作

串口助手选**文本发送**，附加 CR、LF 或 CRLF。只在 USART1 发送，不是网络 JSON，也不是 RK TLV：

| 文本命令 | 输出选择 | 确认文字 |
|---|---|---|
| `log mcu` | MCU 自身日志 | `[log] source=mcu` |
| `log rk` | USART2 收到的 RK 原始日志 | `[log] source=rk` |
| `status` | MCU模式下输出一次板级状态 | `[log] status queued`；RK模式原样转发给RK |
| `start rk` | MCU模式下以LPDDR4X标称1800/1100/600mV、fspiM1冷启动 | 先`[rk power] start rk queued`；完成后`power ON`，失败显示错误名 |
| `end rk` | MCU模式下正常下电 | 先`[rk power] end rk queued`；完成后`power OFF`，失败显示错误名 |
| `led 0 pass` | 单组验灯：组0显示PASS，其余两组灭 | `[led] states=1,0,0; ...` |
| `led all off` | 关闭三组结果灯 | `[led] states=0,0,0; ...`；不改变PG6 |
| `led 0 00` / `01` / `10` / `11` | 固定组0两路电平，其他组灭；左位PG0、右位PG1，0低1高 | `states`分别为0/1/2/5；11双路常亮，不闪烁 |

两种来源互斥；RK输出里的 `log mcu` 等文字只显示，不解释为命令。

- MCU模式：只处理上表本地命令；其他命令、二进制和超长行返回用法提示。
- 本地电源命令：当前为RK模式时先发送`log mcu`并换行，再发送`start rk`或`end rk`并换行。无需PC连接；唯一TCP任务需正常运行。`start rk`覆盖此前保存的训练目标，已上电时也先正常下电，再按现有OFF等待、Boot设置和轨间时序冷启动；不修改原时序，不建立FUNC/PARA/SLT测试占用。它不等RK心跳、不下发算法，`power ON`只确认本地电源动作完成，RK启动结果须看启动日志。需要查看时另发`log rk`。
- `end rk`沿用正常下电，已经OFF时直接完成；测试或电源操作在途时两条命令均返回`busy`，不会取消在途测试。命令只保留一条，失败不自动重试，切换日志来源不取消已受理动作；完成回复遇发送拥塞保留，完整入队后才接受下一条电源命令。
- 空闲验灯：`led 0|1|2|all off|pass|fail|normal|hot`，选一组时其余两组熄灭。
  `pass`亮PASS通道，`fail`亮FAIL通道，`normal`两通道同时闪烁，`hot`仅FAIL通道闪烁，亮/灭各500ms。
  只控制显示，不启动算法、修改轮次或控制加热；正式PC/B2显示事件仍可覆盖，验灯时保持业务空闲，结束发送`led all off`。
- 电平验灯：`led 0|1|2|all 00|01|10|11`。左位为该组FAIL引脚，右位为PASS引脚；
  组0为PG0/PG1，组1为PG2/PG3，组2为PG4/PG5。0输出低、1输出高，11两路持续高电平；
  `status`的状态5仅表示此次调试双路常亮，不新增TCP测试状态。
- RK模式：输入原样转发到USART2，包括回车、退格、Tab、方向键序列及Ctrl+C；`status`也交给RK。串口助手须发送实际控制字节，例如Ctrl+C为HEX `03`。
- 退出RK模式：在行首输入完整`log mcu`加CR/LF/CRLF。该保留行不交给RK，尾随LF也不泄漏；命令区分大小写，不带前后空格。
- 行首与`log mcu`相同的前缀暂存，失配时一起发出；因此输入开头的`l`等字符可能延迟显示。前缀内退格可编辑，Ctrl+C取消暂存前缀并发送中断字符。普通RK命令不受MCU本地15字符行长限制。
- USART2发送拥塞时缓存一块，暂缓取新输入，RK日志仍继续处理；接收断流会警告并丢弃到下个换行，不自动重发命令。已发到RK的前半行无法撤回，必要时由操作者发送Ctrl+C再重输。
- 调试转发不是文件传输协议，不保证持续大流量无损；不能通过此通道发送保留的`log mcu`命令给RK。

确认文字之前可能还有已经排队的旧来源字节，确认之后只加入新来源。出口拥塞时先等待确认文字入队，期间日志直接丢弃；连续大量切换命令只保留最新的待发确认。日志切换不会清空UART4接收、重启RK、操作电源或写入Flash；电源操作仅由明确的`start rk`/`end rk`命令触发。

## 3. 编译默认值与 MCU 日志入口

在 `D:/pro/b1` 执行：

```powershell
.\tools\dev.ps1 build -LogSource MCU
.\tools\dev.ps1 build -LogSource RK
```

对应 CMake `B1_LOG_DEFAULT_SOURCE=MCU|RK`，编译到 C 的值分别为 0/1。首次默认 MCU；不传参数时保留该构建目录的缓存。运行时命令只改变本次运行，重启恢复所烧固件的默认值。

当前Boot＋App合并产物为`build/standalone/Debug/b1.elf`，地址别名为同目录`b1_192.168.1.201.elf`；只构建不会烧录，旧双槽验证仅为第5节的历史记录。

MCU 自有日志使用 `log_console_write(text, length)`，普通任务中的 `printf` / stdout / stderr 也经同一出口。运行期启动提示、电源阶段及限速状态报告使用此入口；不要在普通任务里直接调用 `HAL_UART_Transmit(&huart1, ...)` 绕过来源过滤。ISR 中不打印，错误处置不能依赖日志输出成功。

## 4. 本轮改动与限制

- 固定逻辑槽：0=UART4 协议，1=USART2 RK 日志，2=USART1 本地控制台；拒绝旧 `B1_RK_UART=2` 构建和 B1 首烧参数。
- 三路独立 NORMAL RX DMA：DMA1 Stream0/1/2；IDLE/TC 入队，忽略 HT，DMA 位于 AXI SRAM，D-Cache 仍关闭。USART2为TX_RX，三路分别使用独立TX环；同步`b1.ioc`配置。
- 复用默认任务处理控制台和日志，每轮各消费最多一个接收块；没有新增任务。协议 RX 队列保持 8 块，两路日志/控制台各 2 块，不占用协议队列。
- USART1 非阻塞 TX，每次临界区最多入队 128 字节；不等串口排空。拥塞允许丢日志，调试器可看 `s_dropped_bytes`（TX空间/取锁失败的丢弃尝试字节）；RX 丢块继续通过各路 `uart_rx_diagnostics_t` 观察。
- 未选中来源不缓存、不补发；日志链路是诊断出口，不保证无损。RTOS 和 DMA 启动之前的 RK 日志不能捕获，网络初始化期间的缓冲也可能溢出。需要完整冷启动记录时仍应直接采集 RK 调试口。
- 未增加PC JSON字段，未修改RK帧/Seq/心跳、上下电门禁或V2固件；RK调试输入直接经USART2转发，不参与UART4解析。

## 5. 原日志切换验证（2026-10-10历史）

已通过：B1 `tools/dev.ps1 check`；MCU/RK 两种默认模式的普通 ARM 构建；实际 `log_console.c` + `uart_router.c` 的 C/QEMU 回归（两种默认值）；B1 RK 协议和 V2 原 UART/IDD 回归。额外负向检查确认 CMake 拒绝旧 `B1_RK_UART=2` 及非法默认来源 `BOTH`；最终普通构建缓存恢复为 MCU/UART4。

日志回归覆盖命令分片、CR/LF、非法命令、NUL、超长行、断流恢复、RK 文字不作为命令、日志队列溢出不消费协议帧、TX满后确认优先、MCU/RK互斥、stdout/stderr、ISR拒绝及默认值恢复。代码在正式 C 中执行，队列/HAL/TX 是测试桩，不是真机串口验收。

B1 OTA Release 双槽验证通过：145132 字节、1468 处地址修正，A1→A2 逐字节一致且可逆。报告：`build/ota-app-0.1.0-release-a1/relocation_report.json`，0.1.0 仅用于离线检查，未对已有设备降级安装。

复跑日志测试使用 V2 仓库共用工具：`D:/pro/test/tests/verify_uart_idd_c.py --app-root D:/pro/b1 --compiler <arm-none-eabi-gcc.exe> --log-default 0`，将末尾改为 1 验证 RK 默认。

2026-10-27新增早期启动阶段和`status`只读诊断，当前用法、字段及验证见[首板诊断记录](board_diagnostics_20261027.md)。调度前只有专用诊断入口可短时轮询发送；在创建内核对象前永久关闭，不与运行期发送环混用。

**待真板验证**：三路实际波特率/电气接线、DMA连续接收、日志高负载下 UART4 和 TCP 并发、任务堆栈/FreeRTOS堆余量及完整启动表现。本轮未连接串口/TCP/ST-Link、未烧录、未提交或推送。

2026-11-02双向输入改动、验证与待验收项见[接入记录](../../test/doc/b1_debug_bridge_20261102.md)。

## 6. 本地电源命令（2026-11-05）

本次新增`start rk`/`end rk`，复用现有电源服务；不修改电源驱动、轨间延时、RK协议或V2运行代码。实际C/QEMU回归已覆盖两种日志默认值、分片/换行/断流、重复输入、发送拥塞、模式隔离，以及标称训练目标、fspiM1、ON后冷重启、正常下电、PC互斥、断线继续和I/O失败/超时。PC整轮、原PC业务、B1完整`check`、Debug构建及Boot＋App文件布局检查通过；合并BIN为386660字节，CRC32为`34EC6C89`。新ELF位于`D:/pro/test/output/b1-console-power-20261105/b1.elf`；安装及真实启动由用户在设备空闲时验证，本轮未连接或烧录设备。

复跑：V2共用脚本`tests/verify_uart_idd_c.py --app-root D:/pro/b1 --compiler <arm-none-eabi-gcc.exe> --log-default 0`（另测1），以及`tests/verify_pc_rk_service.py --app-root D:/pro/b1 --power-sequence`。两项分别运行正式串口解析/单槽交接和正式PC电源服务，硬件、调度器端口为测试桩。

## 7. 在 `log mcu` 看 PC 和 RK 测试协议

USART1 输入 `log mcu` 并换行。时间为 MCU 上电后的毫秒数，RX/TX 均从 MCU 看。`log rk` 继续显示 USART2 的 RK 调试台。

| 串口标签 | 读法 |
|---|---|
| `[PC]`、`[PC RX]` | `conn` 区分每次 TCP 连接。收齐 PC 帧时记录命令、PC Seq 和总长度。测试受理后另列算法、index、启动方式、Freq、普通时序项数和训练目标电压（VDD1/VDD2/VDDQ，mV）。提前 0107 或断线废弃待交付结果时记录原因。 |
| `[PC TX]` | 0006 显示原请求号及实际状态正文。0105/0109 显示 index、MCU 自身 Seq、总长度及 `tcpQueued`。该值是本地 TCP 已接收的字节数。短写后继续发送，最终失败保留已提交的字节数和 lwIP errno。 |
| `[RK TX]`、`[RK RX]` | 每帧一行。RK Seq 后依次显示 `Type[Value字节数]=十六进制原始Value`，保留顺序和小端字节。发送记录的 `uart=queued` 表示已进入本地协议 UART 发送队列。可选 0x35 查询及回复按实际发生记录，首次有效 SPL 心跳显示 `freqQuery=0/1`，无绑定时为 `unbound`。 |
| `[RESULT]` | 首次领取 RK 合法最终结果时显示算法、PC index、RK 耗时及两颗判定，随后可用 `[PC TX]` 的 0105/0109 查交付。显式零地址和未列出的颗粒均显示 PASS，RK RX 行保留两种线帧的区别。 |

以 `gtest-mul` 为例，`5000[2]=0500` 是发给 RK 的算法号，`9001[8]=3800200000000000 3001[1]=00` 对应 die0 首错地址 `0x0000000000200038`。设置 tRCD 为 38 时可见 `5000[2]=2B00 701B[4]=26000000`。频率为 2133 且 RK 请求时可见 `5001[2]=3500 801C[8]=5508000000000000`。线上采用独立的 PC 和 RK Seq。

PC 心跳、0601/0602 电流轮询和普通 RK 心跳只累计次数。`status` 的 `[traffic]` 显示各自的 RX、TX、错误数和最近时间。进度打印首帧及首次跨过 25%、50%、75%、100% 的帧，同类拒绝最多每秒显示一次。`[console] droppedBytes` 累计所选日志因出口拥塞或控制回复优先被丢弃的字节。每条记录整行入队，出口满即丢弃。过长的扩展帧以 `...` 标明省略，标准 3/5/7 TLV 结果可完整显示。

维护入口在 B1 的 `App/diagnostics/`。`pc_trace.c` 负责 PC 收发、颗粒判定和原有 `[power]` 阶段行的显示，`rk_trace.c` 负责 RK 帧、进度和拒绝原因，`protocol_trace.c` 负责时间戳、单行输出和计数。串口模式选择由 `Core/Src/log_console.c` 管理。
