# B1 TCP 网络调试控制台

2026-11-16：正式 B1 工程新增同一运行 IP 的 TCP 8889。MCU 是 Server，网络助手/终端是 Client。8888 仍用于正式 PC 测试，每个服务各接手一个客户端；调试连接不参与 PC 整轮的建立、结束或归属。

## 连接与命令

网络助手选 **TCP Client、文本收发**，连接 `MCU当前IP:8889`。例如使用本轮 .201 固件时填 `192.168.1.201:8889`，.202 固件则填 `.202:8889`。B2 改 IP 后使用新地址重连。命令区分大小写，以 CR、LF 或 CRLF 结束，不用18字节头或 JSON。

连接后默认 MCU 来源，收到：

```text
[net console] connected; source=mcu; type status or log rk
```

| 命令 | 行为 |
|---|---|
| `status` | 一次状态快照，只回请求端；包含网络调试连接和累计 droppedBytes |
| `log mcu` | 选择 MCU 日志；RK 模式下须在行首完整输入并换行 |
| `log rk` | 选择 RK USART2 日志并申请 RK 输入控制权 |
| `start rk` / `end rk` | 复用现有电源请求单槽及正式忙门禁，不绕开测试占用 |
| `net rk` / `net mcu` | 复用已有手动网络交接入口，与正式 010A 业务入口保持独立 |
| `led ...` | 沿用现有空闲验灯命令，使用完毕发送 `led all off` |

RK 模式除退出命令外，输入全部交给 RK，包括 `status`、方向键和 Ctrl+C。执行 MCU 命令前先 `log mcu`。网络助手适合文本命令；交互 UEFI 菜单需支持 ANSI/VT100 控制序列的 **Raw TCP** 终端，不是 Telnet 协议。

串口和网络各自选源、各自保存半行，可以一端看 MCU、一端看 RK，也可同时观察 RK。先进入 RK 模式的一端拥有输入权，另一端提示 `read-only` 并只观察；输入权释放后，观察端重新 `log mcu`、`log rk` 申请。网络断线丢弃旧输入和未送出的旧回复，释放该端 RK 输入权；不会因此结束正式测试或关电。已经交给电源服务的请求仍按原流程完成，不向重连客户端投递旧回复。

## 日志和资源边界

- 沿用现有时间戳、PC/RK 命令、TLV、结果、电源阶段和 SLT 日志。心跳/电流查询仍只累计计数，不逐包刷屏。PC `recvFailed` 另打印即时 lwIP errno 和对端地址。
- 网络初始化完成且客户端连接后才有网络日志；不回放启动前或断线期间的记录。电脑负责保存文件，MCU 不写 Flash。
- 发送环 4096 字节，含每条2字节长度；普通记录最多256字节，拥塞整条丢弃并累计 droppedBytes。为控制回复预留258字节。原始 RK 输出按128字节块处理，拥塞可能丢块，不能用于可靠文件传输。
- TCP 未确认数据最多1024字节，约20ms投递一次非阻塞 lwIP 回调；有数据待发但持续3s无进展时，只断开8889。正常空闲不按此超时断开。
- 接收环512字节，立即复制并释放 pbuf；输入超限直接断开8889并清空旧会话，避免把残缺字节拼成控制命令。
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

实际 C/QEMU 已覆盖：真实 lwIP SYN/ACK/FIN/RST、第二连接拒绝、PCB耗尽、接收池立即归还、1024字节发送上限、整条丢弃、控制回复余量、慢接收/输入洪泛、8888并存、重连旧数据隔离、IP/链路变化；控制台测试覆盖两种串口默认来源、两端半行隔离、状态定向回复、RK输入权及电源单槽。原PC整轮、RK/IDD、SLT独占/共存、分流和B1 check通过。均为离线执行，不代表网卡/串口实际吞吐验收。

复跑入口（均在 `D:/pro/test`，不访问硬件）：

```powershell
python -B -X utf8 tests/verify_net_console_c.py --app-root D:/pro/b1
python -B -X utf8 tests/verify_uart_idd_c.py --app-root D:/pro/b1 --compiler <arm-none-eabi-gcc.exe> --log-default 0
python -B -X utf8 tests/verify_uart_idd_c.py --app-root D:/pro/b1 --compiler <arm-none-eabi-gcc.exe> --log-default 1
python -B -X utf8 experiments/mcu_pxe/handoff/tests/run_net_mux.py
```

实板先让上位机保持8888连接，再用网络助手连8889查询status、观察FUNC/PARA日志；随后测试SLT下载/报告上传期间两条连接、UART错误/丢失计数、RTOS堆余量及下载耗时。再验证调试端断线重连、慢接收和串口并用。此次未烧录，未修改V2/手动TLV分支、EDK、电源时序或服务器。
