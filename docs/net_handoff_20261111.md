# MCU / EDK 同 IP 交接与串口提速

当前默认方案已于2026-11-12更新为[MCU TCP常连与4M共存](net_coexist_4m_20261112.md)。本文保留11月11日独占IP/2M方案及实板证据；下述断开PC和撤IP操作仅适用于旧独占模式。

2026-11-11：已接入本工程的默认构建，保留完整 Boot、FUNC、IDD、B2、采样、电源和调试串口。只执行 `tools/dev.ps1 build` 或 Ctrl+Shift+B 即可生成 `build/standalone/Debug/b1.elf`、`b1.bin`，不烧录。

## 启动阶段与速率

| 阶段 | 交互 | 协议口速率 |
|---|---|---:|
| DDR / SPL | RK `0x35` 查询启动频率，MCU 用 `5001 + 801C/int64` 应答 | 115200 |
| SPL | MCU `5000[2]=3E00` 通知继续后级引导，等本地 TC，不等 RK ACK | 115200 |
| EDK 首次握手 | 普通 HELLO 获取本轮 MAC / IP / 掩码 / 网关授权 | 115200 |
| EDK 数据阶段 | HELLO 附 u32LE 速率，低速应答发完后切换，再用高速普通 HELLO 确认 | 2000000 |
| RK 明确下电 | 排空并归还网络，恢复启动速率后放行下一轮 | 115200 |

不用修改 DDR/SPL 波特率。USART1 本地控制台和 USART2 RK 日志口仍为原速率。RK UART1 使用现有 GPLL 的分数分频，不修改共享 PLL 或 UART2。速率常量集中在 `App/protocol/serial_net/serial_net.h`，EDK 同步脚本读取同一份文件。

新 EDK 配旧 MCU，速率请求明确返回 INVALID 时继续使用 115200；新 MCU 配旧 EDK，不收到提速请求就保持 115200。低速改速应答丢失时 MCU 在 1 秒内未收到高速 HELLO 则退回低速，允许同序号重试。高速确认后的异常不猜测对端状态，应明确下电后重开一轮；不会靠超时抢回 IP。

## 给测试人员的当前流程

1. 安装配对的 MCU 固件和 [EDK SPI NOR 升级整包](D:/pro/test/build/mcu_pxe/handoff/wsl_20261111_144357/update_spinor_rk_for_edk2_h1.img)。EDK 版本标记为 `B1-SERIAL-NET-20261111-H1B`。此前配对已由用户日志验证 2M 握手、下载及报告上传；下面的 MCU 接收唤醒优化另待安装复测。
2. 正常 FUNC、IDD、采样和电源联调沿用原 PC 命令。需要进行串口网卡 SLT 时，先完成已有测试、让 RK 下电，并断开 PC 与 MCU 的 TCP 连接。
3. MCU 控制台执行 `net rk`，核对 `ip=` / `rkMAC=`。Tftpd64 继续保留该 MAC 到当前 IP 的绑定；PC 内置 DHCP 后续也必须只给受控测试板发放可用地址。
4. 执行 `start rk`。它沿用既有 LPDDR4X 标称电压及 fspiM1 上电流程。支持 `0x3E` 的 SPL 会被通知继续引导；自动进入 EDK 的前级也兼容。
5. MCU 日志应出现 `UART data baud=2000000 confirmed`，EDK 出现相同确认及 `DHCP requested IP=...`。只看到 `trial` 不算改速完成。
6. EDK 保留 3 秒 ESC 手动窗口，随后选择授权网卡的 PXEv4，执行 MemTest86。应用返回后自动归还 IP，停留完整菜单。PC 可再连接 MCU 的原 IP:8888 验证 `0101/0102`；然后 `end rk`，下一轮恢复普通 SPL/FUNC 流程。

**正式 `010A/010B` 与 IP 交接的自动联动尚未接入。** 当前 EDK 自动 PXE/归还可供测试，但交接仍从 `net rk` 触发；不能把 PC 旧 SLT 命令已经回 completed 等同于这条交接流程已自动完成。测试人员应分别验证普通 PC 业务与上述 SLT 路径。

## DHCP 与源码归属

EDK 只在授权虚拟网卡的首次 DHCP DISCOVER 中添加 Option 50，内容为本轮实际 MCU IPv4；Build 接口替换同号选项，不重复追加。标准 DHCP REQUEST 处理保持不变，普通网卡没有授权接口时不添加。Option 50 是申请地址，服务器仍决定是否分配；已有地址保护会拒绝不等于授权 IP 的非零 DHCP 地址。

Tftpd64 现有固定绑定保留，不需要在服务器里额外添加 Option 50。以后 MCU 的实际 IP 改变，Tftpd64 的固定绑定也须相应更新。PC 软件未修改；Tftpd64 的分块协商设置见下节。

| 目录 | 职责 |
|---|---|
| `App/protocol/serial_net/` | COBS、CRC32、会话流水号及 IPv4/ARP/DHCP 地址检查 |
| `App/tasks/net_handoff/` | TCP/RK 任务协作、IP 撤销/归还、以太网队列、速率切换 |
| `HAL_Drv/uart_hw.c` / `uart_router.c` | TX/TC 空闲证明、RK 口改速、RX DMA 重启 |
| `D:/pro/edk2/edk2-platforms/Silicon/Rockchip/Drivers/SerialNetSnpDxe/` | 完整 EDK 的虚拟网卡与提速握手 |
| `D:/pro/edk2/edk2/NetworkPkg/UefiPxeBcDxe/HandoffDhcp4.c` | 本轮授权 IP 的 DHCP Option 50 |
| `D:/pro/edk2/edk2/ArmPkg/Library/PlatformBootManagerLib/SltAutoBoot.c` | 自动 PXE、应用返回后归还及手动菜单 |

H1B 接入时通过 B1 常规构建/板级检查、实际 C 的交接/改速故障路径、原 PC/RK FUNC/PARA/电源/整轮、B2、UART 回归，以及 EDK 实际 C 和完整 AArch64 构建。SPI NOR 包已通过固件卷检查及厂商工具双层解包校验。11 月 11 日关闭字库下载后的单轮主程序下载为 23.066 秒、1 块重传，XML/HTML 上传成功；这不包含上电和应用初始化，也不代表重复运行稳定性已验收。

## 下载开销优化（2026-11-11）

**最新固件为下节“上传大帧接收修复”版本。** 下述吞吐优化镜像保留作历史基线，其 1056 字节 RX 缓冲不适用于已协商的大分块上传。

MCU 在 EDK 数据期改为等待 UART RX 队列，ISR 入队可以立即唤醒 RK 任务，数据仍由原来的唯一消费者取走。空闲等待上限为 1ms（另有调度延迟），继续推进改速回退、电源及健康检查。SPL、等待 TX/TC、流复位和故障状态仍按节拍等待，避免未消费的旧 RX 块导致忙等。未改变串口波特率、SerialNet 格式、PC/RK TLV 或电源流程。

本机 `D:/pro/test/build/mcu_pxe/pxe-server/tftpd32.ini` 的 `PXECompatibility` 从 1 改为 0，`Negociate=1` 保留。Tftpd64 的 PXE 兼容分支会忽略 `blksize`，原 EDK 已按 IPv4 MTU 请求 1468 字节，却实际退回 512 字节；详见 [Tftpd64 源码](https://github.com/PJO2/tftpd64/blob/master/src/_services/tftpd_thread.c)。无需改 EDK 或 SPL 镜像。

当前 Tftpd64 进程未重启。复测前重启软件，在 Settings → TFTP 核对 **Option negotiation 勾选、PXE Compatibility 不勾选**；若程序保存旧设置覆盖了 INI，以界面取消并保存后重启。下一轮须看到 `OACK: <blksize=1468,...>`。1607208 字节主程序应约 1095 个 DATA 块，旧版为 3140 块。保留原 DHCP/IP/MAC 绑定和标准 ACK，不启用 anticipation window。

优化版完整 Boot＋App：[ELF](D:/pro/test/build/mcu_pxe/handoff/throughput_20261111/b1.elf)、[BIN](D:/pro/test/build/mcu_pxe/handoff/throughput_20261111/b1.bin)、[验证记录](D:/pro/test/build/mcu_pxe/handoff/throughput_20261111/verification.json)。BIN 410560 字节，CRC32 `3DF02080`，本地 IP 为 `.201`。原镜像和配置备份在 `D:/pro/test/build/mcu_pxe/handoff/throughput-before-20261111-172135/`。

本轮 B1 构建/check、实际 C 的接收等待/断流/恢复、控制及故障期等待、交接/改速、FUNC/IDD/PC 整轮通过，V2 共用 UART 测试通过。EDK 新增实际 C 的 1514 字节以太帧（1468 字节 TFTP DATA）、应答丢失重放和接收缓冲重试用例通过，未重编 EDK。2M/8N1 下，无额外空轮询和重传的串行线耗时模型约从 10.77 秒降至 9.01 秒；这是模型，不是预计总耗时或实测。新设置下的下载时间、分块协商和持续负载仍待实板记录，本轮未烧录。

## 上传大帧接收修复（2026-11-11）

17:33 本机 TFTP 日志确认 `blksize=1468` 已生效，主程序 1607208 字节、1095 块，17:33:48.628 至 17:34:03.861 共 15.233 秒，零重传。随后 `Connect.xml` 上传协商 1024 字节，服务端收到 0 块；EDK 连续三次 `op=2 seq=7453 tx=1094/1094 rx=0`。用户现场 `status` 为 UART4 `err=3 HAL=0x8 drop=4`。HAL 的 `0x8` 为 ORE 接收溢出，不能把这轮记为报告上传成功；安装的 ELF 尚未独立回读核验。

旧 RX DMA 容量为 1056 字节。NORMAL DMA 满块后，HAL 关闭接收 DMA 请求，回调复制/入队，再重新启动；UART4 未启用硬件 FIFO。1094 字节连续帧在中途触发此路径，2M/8N1 每字节 5µs，日志与停收窗口丢字节高度吻合。之前只补充了 EDK 接收大帧测试，遗漏了 MCU 接收方向的容量约束。

本次把 B1 RX 块统一扩为 1600 字节，并在交接模块编译时要求 `UART_RX_BUF_SIZE > SN_WIRE_MAX`（1540 字节）。严格大于避免最大合法帧恰好在帧尾触发满块 TC。SerialNet 保留请求/应答停止等待，在对端等待应答的帧间窗口重启接收；不宣称 NORMAL DMA 支持无限连续字节流。EDK、SPL、TFTP 配置、电源时序和测试协议不变。

新增真实 C 路由＋SerialNet 编解码用例覆盖 512/1024/1468 字节上传分块：旧 1056 容量用例失败，1600 修复后通过；B1 两种日志默认模式、断流恢复、IDD、交接/改速、V2 共用 UART 回归，以及 B1 build/check 通过。新编译 RX 回调栈 1632 字节，MSP 4096 字节；默认任务 8KiB、RK 32KiB。三路队列新增 6528 字节 RTOS 堆，按现场 26600 字节余量估算同状态约余 20072 字节，实际余量和栈高水位待复测。

新版完整 [ELF](D:/pro/test/build/mcu_pxe/handoff/upload_rx_fix_20261111/b1.elf)、[BIN](D:/pro/test/build/mcu_pxe/handoff/upload_rx_fix_20261111/b1.bin)、[验证与故障日志](D:/pro/test/build/mcu_pxe/handoff/upload_rx_fix_20261111/verification.json)。BIN 410560 字节，CRC32 `510EF204`。旧文件备份在 `D:/pro/test/build/mcu_pxe/handoff/upload-rx-before-20261111-173828/`。本轮未烧录；复测检查 Connect.xml、Status.xml、最终 XML/HTML 上传及归还流程，UART4 ORE/drop 不应继续增加。

启动期间两个 `TFTP error code 256` 对应尝试读取不存在的 `-mt86.cfg`、`4GB-mt86.cfg`；随后通用 `mt86.cfg` 下载成功，故本轮不能将这些配置探测提示当作串口事务超时的原因。

### 修复后实板结果（17:45 轮次）

用户反馈从启动到测试界面约35秒。同期本机TFTP日志显示主程序17:45:26.417至17:45:41.564下载完成，共15.147秒、1095块、零重传；随后1024字节分块的Connect.xml（1745字节）、4份Status.xml、TestResult.xml（3042字节）及HTML（33962字节）均上传完成、零重传，已核对落盘文件大小。先前上传第一块卡住的问题在此轮未复现，单轮下载与报告传输通过。

35秒为用户估算的启动至界面总耗时，不能当作下载耗时；其余阶段还包括前级/EDK启动、DHCP和MemTest86硬件初始化，尚未逐段计时。报告传输成功不代表DDR判定已复核；本轮UART4错误计数、自动归还后MCU重连及连续多轮稳定性仍待记录。证据：[运行记录](D:/pro/test/build/mcu_pxe/handoff/upload_rx_fix_20261111/runtime-success-1745.json)、[TFTP日志](D:/pro/test/build/mcu_pxe/handoff/upload_rx_fix_20261111/tftp-success-1745.log)。
