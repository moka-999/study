# B1 TCP 网络调试控制台

B1 的 MCU 是 TCP 服务端。8888 接一个正式测试客户端，8889 可同时接两个调试客户端。两个网络助手可开在同一台电脑，也可以分开。调试连接独立于 PC 的测试整轮。

## 连接与命令

网络助手选 **TCP Client、文本收发**，连接 `MCU当前IP:8889`。例如使用本轮 .201 固件时填 `192.168.1.201:8889`，.202 固件则填 `.202:8889`。B2 改 IP 后使用新地址重连。命令区分大小写，以 CR、LF 或 CRLF 结束，不用18字节头或 JSON。

连接后默认 MCU 来源，收到：

```text
[net console] connected; source=mcu; type status or log rk
```

| 命令 | 行为 |
|---|---|
| `status` | 一次状态快照，只回请求端；包含网络调试连接和累计 droppedBytes |
| `test status` | 向请求端显示整轮状态、当前算法和阶段。FUNC 的 `progress` 是最近一次收到的有效百分比，尚无进度时显示 `--`。Shmoo 另列当前坐标和已上报点数/总点数。 |
| `check <时序名>` | 在 MCU 模式输入如 `check CASReadLatency`。RK 已上电且 SPL 协议串口可用时即可查询，支持等待启动心跳、自动回读及 FUNC/PARA 运行期间。不同参数可连续提交，同一在途参数共用结果。请求端收到 `queued`，MCU 日志显示实际通道读数，发出后 500 ms 无有效应答显示 `unavailable=timeout`。 |
| `check all` | 通过同一入口查询全部 15 项普通时序。MCU 日志逐项显示结果，末行 `check all received=N/15` 汇总有效应答项数。重复提交共用进行中的这轮查询。 |
| `set tWTR 20` | RK 上电、SPL 协议就绪且测试空闲时，写入一项普通时序并查询 CH0～CH3。值用十进制 u32，参数名沿用 `check`。请求端看到 `queued` 后，在 MCU 日志查看读回值或失败原因。沿用这次上电继续跑算法时，应按实际读回的寄存器值理解测试条件。 |
| `set freq 1066` | 将下一次手动 `start rk` 的频率保存在 MCU RAM，也接受 `Freq`。`-1` 表示使用 DDRbin 默认频率；RK 发 `0x35` 时 MCU 回复。一次手动启动消费这个设置，PC 测试使用自身的配置。 |
| `log mcu` | 选择 MCU 日志；RK 模式下须在行首完整输入并换行 |
| `log rk` | 选择 RK USART2 日志并申请 RK 输入控制权 |
| `start rk LPDDR4` / `start rk LPDDR4X` | 从 fspiM1 冷启动，按所选芯片设置 VDD1/VDD2/VDDQ。LPDDR4 为 1800/1100/1100 mV，LPDDR4X 为 1800/1100/600 mV。原命令 `start rk` 默认 LPDDR4X。 |
| `func <算法名>` | 手动运行一项已有 FUNC，例如 `func gtest-rand` 或 `func marchc-`。先启动 RK，待 SPL 心跳就绪和启动时序回读结束后发送。原请求端显示运行耗时、两颗 DDR 的 PASS/FAIL 及失败地址；输入 `test status` 可看进度。每次只执行一项，收到结果后可接着发下一项。 |
| `end rk` | 测完后关闭 RK/DDR，若算法仍在运行则返回忙。 |
| `end rk force` | 人工中止当前算法、Shmoo 或手动启动，确认 RK/DDR 下电并完成清理后向请求端回复 `power OFF`。若已开启 `0106` 整轮，则保留该轮。人工中止不向 8888 上报结果。 |
| `net rk` / `net mcu` | 复用已有手动网络交接入口，与正式 010A 业务入口保持独立 |
| `led ...` | 沿用现有空闲验灯命令，使用完毕发送 `led all off` |

例如调试地址为 `.205` 时，两个窗口都选 TCP Client，连接 `192.168.1.205:8889`。窗口 A 保持默认看 MCU 收发的 TLV，窗口 B 用文本发送 `log rk` 并带换行看 DDRbin 启动日志。两者可在 RK 上电前接好。

手动恢复 DDRbin 默认频率时，先发 `set freq -1` 并等到 `next manual start`。RK 已上电就发 `end rk force`，收到 `power OFF` 后再发 `start rk`。原本就是 OFF 时直接发 `start rk`。在 `log mcu` 查看 `0x35` 的 `801C[8]=FFFFFFFFFFFFFFFF`，在 `log rk` 查看 DDRbin 打印的实际频率。

RK 模式除退出命令外，输入全部交给 RK，包括 `status`、方向键和 Ctrl+C。执行 MCU 命令前先 `log mcu`。网络助手适合文本命令；交互 UEFI 菜单需支持 ANSI/VT100 控制序列的 **Raw TCP** 终端，不是 Telnet 协议。

物理串口和两个网络窗口各自选源、保存输入半行，可同时观察 RK。先进入 RK 模式的一端拥有键盘输入权，其余端提示 `read-only`。持有者退出或断线后，观察端依次发 `log mcu`、`log rk` 可取得输入权。重连默认看 MCU。已交给电源服务的请求继续执行，完成提示只交给仍在线的原请求端。

## 日志和资源边界

- 沿用现有时间戳、PC/RK 命令、TLV、结果、电源阶段和 SLT 日志。心跳/电流查询仍只累计计数，不逐包刷屏。PC `recvFailed` 另打印即时 lwIP errno 和对端地址。
- 网络初始化完成且客户端连接后才有网络日志；不回放启动前或断线期间的记录。电脑负责保存文件，MCU 不写 Flash。
- 每个网络连接有独立的 4096 字节发送环，含每条 2 字节长度。普通记录最多 256 字节，为控制回复预留 258 字节。原始 RK 输出按 128 字节块处理。某一端队列满时，只在该端整条丢弃。`status` 的 `connected` 是当前调试连接数（0～2），`droppedBytes` 是合计丢弃字节数，`slotDrops` 按两个连接槽分别累计，重连继续计数。
- RK 日志先到 USART2。该口使用循环 DMA 和独立 4096 字节接收环。真正断流时先交付已收前缀，再通知 DDRbin 解析器丢弃跨断点的半行。复测时对比两次 `status`：`[USART2-RKlog] err` 为累计硬件错误，`HAL=0x8` 表示最近一次为接收溢出；`rxFull` 为软件接收区满次数。`[net console] droppedBytes` 单独统计 8889 发送环丢弃的字节。
- 每个窗口的 TCP 未确认数据最多 1024 字节。复用约 20 ms 一次的非阻塞 lwIP 回调。某窗口有数据要发且持续 3 秒无进展时，只断开该窗口。正常空闲可保持连接。
- 每个窗口有独立的 512 字节接收环，复制后立即释放 pbuf。输入溢出时关闭对应会话并清空半行。两个窗口均占用时不接入第三个窗口，释放名额后可重新连接。
- PCB池耗尽时延后监听；调试PCB优先级最低，避免为日志服务回收正式PC连接。发送/接收环、来源上下文为静态内存，不新增 FreeRTOS 任务栈。TCP自身仍使用网络堆，真实余量及负载须实板确认。
- 默认同 IP 共存模式将8888/8889及相关 ICMP 错误均保留给 MCU，并禁止 EDK 占用这两个 TCP 源端口。旧 `B1_SERIAL_NET_KEEP_TCP=0` 独占交接撤 IP 时会失去网络调试，应使用串口归还网络。
- 这是可信调试网内的纯文本控制接口，没有认证或加密；端口由 `Core/Inc/board_network_config.h` 的 `B1_DEBUG_TCP_PORT` 统一定义。

## 代码归属

| 文件 | 职责 |
|---|---|
| `App/tasks/console/net_console.c/.h` | 独立 TCP 生命周期、有限队列、背压与连接代数；raw API 只在 lwIP 线程调用 |
| `Core/Src/log_console.c` | 串口/网络复用命令解析和日志分发；唯一默认任务执行解析，不在网络回调操作电源 |
| `App/diagnostics/` | PC/RK 日志内容，继续通过统一日志出口输出 |
| `App/protocol/serial_net/net_mux.c` | 同 IP 的两条 MCU TCP 服务分流 |

## 离线验证与实板待办

首次网络日志产物：[.201 ELF](../build/net_console_20261116/b1-ip201.elf)、[.202 ELF](../build/net_console_20261116/b1-ip202.elf)，同目录有BIN和MD5/SHA256清单。两份BIN均为417168字节；这批历史文件不含0104。后续包含网络日志与0104停止的新产物见[停止说明](test_stop_20261116.md)。网络日志相对此前零地址FAIL版，链接静态数据增加6432字节，RTOS堆和UART DMA保留区大小不变。

11 月 16 日单窗口版本的实际 C/QEMU 回归覆盖 lwIP 握手与关闭、连接上限、PCB 耗尽、接收池归还、发送背压、控制回复余量、8888 并存、重连和 IP/链路变化。双窗口版本新增同端口并发、各窗口选源和定向状态回复、RK 输入权移交、慢端与重连隔离用例。

11 月 26 日主线 .205 Debug 构建的 `.bss` 从 89,708 增至 92,908 字节，静态 RAM 净增 3,200 字节。lwIP 离线回归给两个窗口各发 11,520 字节不同数据，其中一路延迟 ACK 200 ms，两边逐字节一致。CPU 占用及对 SLT 下载耗时的影响需在实板上测量。

复跑入口（均在 `D:/pro/test`，不访问硬件）：

```powershell
python -B -X utf8 tests/verify_net_console_c.py --app-root D:/pro/b1
python -B -X utf8 tests/verify_uart_idd_c.py --app-root D:/pro/b1 --compiler <arm-none-eabi-gcc.exe> --log-default 0
python -B -X utf8 tests/verify_uart_idd_c.py --app-root D:/pro/b1 --compiler <arm-none-eabi-gcc.exe> --log-default 1
python -B -X utf8 experiments/mcu_pxe/handoff/tests/run_net_mux.py
```

实板先让上位机保持8888连接，再用网络助手连8889查询status、观察FUNC/PARA日志；随后测试SLT下载/报告上传期间两条连接、UART错误/丢失计数、RTOS堆余量及下载耗时。再验证调试端断线重连、慢接收和串口并用。此次未烧录，未修改V2/手动TLV分支、EDK、电源时序或服务器。
