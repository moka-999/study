# PXE 多服务器回应与 H1C 单轮传输记录

2026-11-12，本轮仅被动抓包、检查本机配置及源码；未修改固件、Tftpd64、上位机或远端电脑，未自动烧录、开关电。RK 重试由用户操作。

## 已确认原因

用户故障日志显示 EDK H1C 取得 `192.168.1.201`，随后向 `.137` 查询 `BOOTAA64.efi`，收到 TFTP 错误并归还网络。本机 Tftpd64 在 `.200`，文件存在且大小为 1607208 字节。

15:20 的同一 DHCP 事务 `e1c4adb8` 捕获到以下回应，客户端 chaddr 均为 `06:A8:9B:51:0F:1E`：

| 回应时间 | 源地址 | 提供的客户端 IP | siaddr / 启动文件 |
|---|---|---|---|
| 15:20:17.621 | 192.168.1.200 | 192.168.1.201 | 192.168.1.200 / BOOTAA64.efi |
| 15:20:17.696 | 192.168.1.137 | 192.168.1.201 | 192.168.1.137 / BOOTAA64.efi |
| 15:20:19.614 | 192.168.1.1 | 192.168.1.136 | 0.0.0.0 / 空 |

`.137` 的源 MAC 为 `B4:65:DC:BE:6C:C5`；Option 54 和 66 均指向 `.137`，租期为 `FFFFFFFF`，Option 12 为 `DESKTOP-PC`。本轮客户端在 15:20:21.502 通过 REQUEST 的 Option 54 选择 `.200`，收到 `.200` ACK 后成功下载。

这证明网段中存在两套为同一 RK 提供启动文件的服务，而不是本机配置将启动服务器写成 `.137`。Option 50 只请求客户端地址，不能限定引导服务器。原故障轮未捕获完整报文，不能还原当时各回应的到达顺序。

这些选项与本地上位机 `D:/pro/pcsoft/ZhuoyiBinCore/PXEServer/DHCPServer.cs` 的行为高度一致：地址选项来自 BindAddress，Option 12 来自 `Dns.GetHostName()`。`BusinessLogic/TigerClawApp.cs` 初始化调用 `startPXEServer()`，配置加载成功后立即启动，不需要先运行测试。可优先检查 `.137` 电脑是否打开上位机，但尚未确认该电脑的实际进程。

## 处理方向

- 当前使用 `.200` Tftpd64 时，停止 `.137` 上为该板提供服务的测试 DHCP/PXE，或将测试 PC 与板子放入隔离测试网。不要因本记录去关闭公司 `.1` 的正常 DHCP。
- 若必须让多个上位机共享同一网段，应明确本轮启动服务器，再由 MCU–EDK 会话传递并筛选 DHCP/PXE 信息；当前未实现该限制，不把 `.200` 写死为正式地址。
- 本机现有静态绑定及 `PoolSize=0` 可保持。所核对的 Tftpd64 4.74 源码中，静态绑定绕过动态地址 Ping 检查，不能把 MCU 保留 IP 与 `DHCP Ping=1` 当作此次故障原因。

## 本轮实测边界

- NBP 请求 15:20:21.530，完成 15:20:31.668，约 10.14 秒；1607208 字节、1095 个数据块、TFTP 记录零重传。此时间不含启动及应用初始化，零 TFTP 重传也不代表串口没有 RPC 重试。
- Connect/Status XML 上传成功；15:22:15.967 收到 TestResult XML，3042 字节；15:22:16.448 收到 HTML，33962 字节。两文件落盘长度与 TFTP 日志一致。
- 用户故障日志已显示 H1C 版本和失败后归还完成；本轮未记录 MCU 的速率确认行，未核对实际烧录 ELF，也未验证 PC TCP 连接贯穿全程。因此不将本次传输写成 4M 线路、PC 常连或连续运行的完整验收。
- 故障日志的 `attempt=1 op=3` 是 SN_POLL 首次等待 4 秒超时后的重试；空队列也应回复 SN_EMPTY。后续继续工作说明发生恢复，但原因仍需本轮 UART 计数和完整日志，不与 `.137` 的文件不存在混为一项。
- 当前 EDK `Mtftp4Impl.c` 未对错误码做 NTOHS，线上错误 1 会打印为 256；获取大小失败仍打印初值 0，不表示本机文件为空。`test result is in PC report` 是通用提示，不能证明失败轮产生过测试报告。

## 用户追加的 PC 常连反馈

同日用户提供 H1C、session `53479399` 日志：本轮仍选择 `.137`，NBP 获取失败后执行 RELEASE，打印网络归还完成并进入 UiApp。用户确认网络助手原连接没有断开，PC 心跳仍有回复。

这支持同 IP 共存模式在本次“启动至 PXE 失败退出”路径下保持 MCU TCP 服务；不是成功下载、MemTest86 完整运行和报告上传期间 PC 连接连续性的验收，也未取得本轮抓包或 UART 速率确认。EDK 共用的“Network returned / PC may reconnect”提示不表示共存模式必须断开或重新连接；此模式下 MCU 始终持有 IP 和 TCP8888，RELEASE 结束 EDK 网络会话。

## 证据

- [DHCP 原始包及解析 JSON](D:/pro/test/build/mcu_pxe/handoff/pxe_diagnosis_20261112/dhcp_capture_20261112_151910.json)
- [Wireshark 可读 PCAP](D:/pro/test/build/mcu_pxe/handoff/pxe_diagnosis_20261112/rk_dhcp.pcap)
- [TFTP 日志快照](D:/pro/test/build/mcu_pxe/handoff/pxe_diagnosis_20261112/tftp_snapshot.log)

首次 PktMon 未捕获报文，不用其空文件作网络结论；上述证据来自 Npcap，仅记录指定 RK chaddr 的 DHCP/PXE。抓包已结束，PktMon 临时过滤器已清理。
