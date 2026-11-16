# B1 MCU 转发网络到 RK EDK2：备用方案

**当前入口（2026-11-16）：完整B1默认保留MCU IP与TCP8888/8889，EDK通过串口网卡共享同一IP，授权后优先4M并兼容2M。** MCU正式源码在 `D:/pro/b1`，EDK实际工程在 `D:/pro/edk2`；正常构建直接包含本功能，不再以临时复制工程作为日常入口。[H1C操作入口](handoff/README.md)保留手动`net rk`/`start rk`、SPL `0x003E`继续引导及自动进入EDK前级的兼容。[正式SLT](../../doc/pc_slt_auto_integration_20261112.md)由010A启动，RELEASE最终OK实际发完后自动下电、恢复协议并释放占用，不等010B；010B回notSupported，PC最终以0107汇总灯态。B1另支持[0104停止](../../../b1/docs/test_stop_20261116.md)和[8889网络日志](../../../b1/docs/network_debug_console_20261116.md)。[地址绑定工具](same_ip_handoff.md)供DHCP配置登记。[实板记录](handoff/pxe_server_conflict_20261112.md)分别记录NBP与报告传输、失败退出时PC常连，不能合并为4M及成功测试全程常连已验收。以下D4独立IP、替换RK任务及115200性能记录均为历史，不是当前默认实现。

2026-11-03。目标是在不增加 RK 网口的情况下，通过 MCU 网口下载 ARM64 MemTest86。**RK 只启动到 EDK2，再运行 MemTest86，不进入 Linux。**

2026-11-04用户确认：本方案保留作备用，后续主方案优先通过电路调整给RK提供硬件网络通路；具体连接方式尚未定。当前115200串口实测约7.5kB/s，主程序下载214秒、字体314秒，加配置等约9分钟，暂不继续推进串口方案的正式010A接入或提速。该时间是下载/启动开销，不是内存算法执行耗时。

本目录版本管理包含自有源码、EDK同步补丁、构建/打包脚本及离线测试；完整EDK工程、Loader和编译产物仍在本地，不能把源码同步当作完整镜像备份。配对MCU/RK固件、匹配Loader、配置及实测证据保存在`build/mcu_pxe`；另有本地备用归档`archive/serial_net_D4_backup_20261104.zip`，包含SHA256清单并逐项校验，不加入Git。归档不含商业MemTest86程序或完整工具链；商业程序来源及散列已记录，重新构建仍需现有B1/EDK2工程环境。

## 1. 启动与通信

```text
RK BootROM
  └─ eMMC 上的兼容 Loader / SPL → DDR 初始化 → ATF → EDK2
       └─ 串口虚拟网卡 → UEFI PXEv4 → BOOTAA64.efi（MemTest86）

PC DHCP/TFTP服务器 ─ 网线 ─ MCU以太网
                             ├─ MCU自己的IP：原TCP 8888 / 验板工具
                             └─ 另一MAC的以太网帧
                                  ↕ UART4（115200/8N1）
                                RK UART1_M0（GPIO2_B6 RX / GPIO2_B7 TX）
                                  ↕ SerialNetSnpDxe
                                EDK2 IPv4 / DHCP / TFTP
```

eMMC 存前级启动程序和 EDK2；MemTest86 由服务器下载到 RAM。SPL 能读 eMMC 即可加载 EDK2，不要求 EDK2 自己提供 eMMC 文件系统，也不需要 Linux 分区。

中转不借用 MCU 的 TCP 8888 连接，不做 NAT。RK 虚拟网卡有独立 MAC/IP：MCU 的 UID MAC 首字节为 `02`，RK 侧改为 `06`，其余五字节相同。MCU IP 不变；RK 通过 DHCP 获取另一个地址。

## 2. 文件与已完成检查

源文件位置：`D:/pro/test/experiments/mcu_pxe`。

| 路径 | 用途 |
|---|---|
| `common/serial_net.c/.h` | 两端共用的帧编码、CRC、字节流解析及请求去重 |
| `mcu/serial_net_mcu.c/.h` | 独立RTOS任务、网口收包分流、tcpip线程发包 |
| `edk2/SerialNetSnpDxe.c/.inf` | RK UART1驱动、UEFI SNP网卡接口，供原网络协议栈使用 |
| `build_mcu.py` | 复制当前B1到build内，应用集成补丁并构建；不改正式B1 |
| `build_edk2.py` | 编译服务器独立目录构建，不改原B1 EDK2工程 |
| `check_artifacts.py` | MCU Boot/App地址及ELF/BIN一致性、FIT/BL33校验 |
| `pxe_proxy.py` | 首板MAC白名单临时PXE启动信息代理，不分配IP；地址仍由现有DHCP提供 |
| `tests/run.py` | 实际C代码的QEMU离线测试，不访问板卡 |

产物在 `D:/pro/test/build/mcu_pxe/artifacts`：

| 文件 | 含义 |
|---|---|
| `b1_serial_net.elf` / `.bin` / `.manifest.json` | MCU实验Boot＋App，供ST-Link写入；Boot `0x08000000`，App `0x08020000` |
| `b1_serial_net_bl33.bin` | RK EDK2原始BL33，960KiB，RAM加载/入口 `0x00200000` |
| `b1_serial_net_reference_fit.img` | 参考FIT，含EDK2及沿用的ATF/TEE，两个相同2MiB槽；不含SPL |
| `SHA256.json` / `verification.json` | 产物散列及离线校验结果 |

当前验证：MCU/EDK2已交叉构建。帧0～1514字节、CRC、溢出恢复、序号回绕、重复请求通过；实际SNP源码在模拟UART中验证了丢应答不重复发包/出队、小缓冲保留接收包、过滤及连续超时停止新事务。

**D3实板UART1握手已通过，DDR探测8192MiB；D4补齐SNP组播过滤。** 三组实际C回归、交叉构建及镜像校验通过，MCU D4已安装回读。11月4日恢复前级后，RK进入D4，已通过MCU完成主程序、字体及配置下载，识别7916MB RAM。MemTest86单核Test 0/1/3全部PASS、错误0、耗时266秒；Connect/Status、最终XML及HTML报告均已上传。源MAC保留、TCP警告定位、多板和正式SLT接入仍待处理。匹配Loader见[恢复记录](loader_recovery_20261104.md)；两端使用`D4_network_20261103`配对文件。MAC白名单代理只补启动信息，现有网络DHCP分配地址，详见[D4网络记录](network_d4_20261103.md)。[UART1核对记录](uart1_fix_20261103.md)仅保留历史。正式B1/V2源码未改。

离线SNP测试替换UART/计时/锁环境，不模拟RK实际时钟、引脚和线路；MCU网口分流及RTOS并发仍需实板验收。D1备份在`build/mcu_pxe/history/before_uart_diag_20261103_154551`，D2备份在`build/mcu_pxe/history/before_uart1_fix_20261103`，用于匹配旧日志/符号。

## 3. 实验对现有功能的影响

- UART4独占用于网络中转，替换原RK任务，不发送算法TLV或RK心跳。0103/0108/010A在副作用发生前返回`notSupported`。
- TCP心跳、信息查询、050x电源/Boot、060x采样、灯、B2任务和双向调试口沿用B1源码。默认电源仍OFF，实际电源初值、AVDD换算、看门狗开关沿用构建时的B1配置。
- RK日志仍走UART2_M0 ↔ MCU USART2；PC通过MCU USART1输入`log rk`进入RK控制台。UART1不打印调试文本。
- 中转只使用RAM，不缓存MemTest86到W25。原精简Boot的FRU维护保持不变。
- 没有改变PC协议、正式SLT业务或飞书文档；本实验不是正式SLT完成上报实现。

## 4. 第一次实板验证顺序

1. **确认 eMMC 启动链。** 用户目前不知道其中内容，不按空白介质处理。接好RK USB烧录口，先识别模式、读取分区和现有Loader信息并备份将要覆盖的范围。采用已能启动这块B1的DDR/SPL/ATF链，把本次BL33接入对应FIT。`0x00200000`是RAM地址，不能当作eMMC写入偏移。
2. 本轮B1的RKDevTool已读到GPT `uboot`：起点LBA `0x00004000`、大小`0x00002000`个512字节扇区（4MiB）；SPL日志确认从该分区成功加载FIT。后续本板实验FIT仅替换该范围，保留已有兼容Loader/SPL。此地址只适用于本轮已核对的分区表；参考FIT不是Rockchip `update.img`或完整eMMC镜像。
3. RK空闲、PC验板工具断开后，备份MCU内部Flash，按现有直接烧录流程写`b1_serial_net.elf`。新日志含`[serial-net] EXPERIMENT; algorithms disabled`。恢复时烧回原正式B1完整ELF。
4. 通过原验板工具设置已经核对的内存类型/电压与`bootMode=emmc`，再上电。启动EDK2不依赖原算法服务心跳；使用050x单步流程，不下发010A。
5. 本轮使用配对D4：`log rk`观察`B1-SERIAL-NET-20261103-D4`、`B1-DDR`和`SerialNet: UART1_M0 115200, SNP MAC=...`，MCU应显示`diag=4`。先确认HELLO成功、SNP过滤设置成功，再启动PXE；若仍超时，按下表核对串口证据。
6. 独立网段可配置自己的DHCP；公司共享网段继续使用现有DHCP，只补PXE启动信息，不开普通地址池。本轮PC为`192.168.1.200/24`、MCU`.201`，代理只响应首板MAC。实际RK租约以抓包为准，不能预设或抢占`.203`。
7. 服务器TFTP根目录使用用户现有MemTest86 Site PXE包，DHCP Boot File填写 **`BOOTAA64.efi`**。不是`BOOTX64.efi`，也不是旧Debian启动文件。`mt86.cfg`使用串口控制台配置`CONSOLEONLY=1`。首次先确认菜单出现，再人工启动测试。
8. EDK2菜单选UEFI PXEv4，分别记录：DHCP租约、TFTP完整下载、MemTest86界面、测试结束及报告上传。报告上传仍需TFTP服务器允许写入，并单独验证，不能由下载成功推定。

首轮保持115200，当前ARM64程序约1.6MB，仅程序字节传输的理论下限约140秒；串口封装、请求往返、字体等附加文件会增加时间。先验证完整功能，再评估提高波特率及多板下载压力。

### 握手诊断

D2增加MCU串口计数和RK配置回读；D3纠正RK UART1引脚、时钟及复位配置。帧格式、波特率、算法门禁和电源流程不变，MCU仍显示diag=2。配置未生效时停止握手，每次超时打印投递和接收字节数。MCU发送入队失败会计数，仍不自行重发，由RK重复请求取得缓存应答。

| 日志 | 含义 |
|---|---|
| RK `UART1_M0 diag=3 mux/clk/gate/rst/ie/lcr/div/usr` | UART1初始化回读：mux的15:8位AA、clk低2位2、gate及rst的位2/13为0、ie位6为1、lcr=03、div=13；其他位不必相同 |
| RK `attempt=... tx=N/总数 rx=N bad=N other=N` | 本次超时前写入UART FIFO的字节、读取的字节、坏帧、无法匹配本事务的合法帧；写入FIFO不代表已到MCU |
| MCU `diag=2 ... ethRx/ethTx` | 以太网帧数；旧版`rx/tx`也是此含义，不能据0断定UART没收到字节 |
| MCU `serial-net-uart rxBytes/frames/hello` | 任务实际取到的UART字节、完整合法帧及HELLO请求数；含重复请求 |
| MCU `txQueued/txBytes/txFail` | 应答进入UART TX环的帧数/字节数，以及组帧或发送入队失败数；不是物理发送完成计数 |
| MCU `off/partial` | 电源OFF时收到而未处理的完整帧数；当前解析器内未收完的数据字节数 |

D2日志及热读显示MCU `rxBytes=0`、DMA剩余1056字节不变，与RK使用错误端口相符。下一轮先看D3配置及HELLO，再切`log mcu`等待5秒取得`serial-net-uart`；仍未收到时再核查U4008线路，不将Debug已通视为协议线路通过。

## 5. 串口合同与异常处理

不复用RK算法TLV，避免相同UART中的两个消费者。只在安装配对的实验固件时启用。

```text
COBS编码(Version:u8, Op:u8, Status:u8, Reserved:u8,
         Session:u32LE, Sequence:u32LE, Length:u16LE, Payload, CRC32:u32LE) + 00
```

Version=1、Reserved=0、Session非0；Payload最大1514字节，含14字节以太网头、不含FCS/VLAN。CRC32使用IEEE多项式，初值/末尾异或均为`FFFFFFFF`，覆盖CRC之前的全部字段。

| Op | 请求 | 应答 |
|---|---|---|
| 1 HELLO | 空，新Session建立会话 | MAC 6字节＋Link 1字节＋MTU u16LE |
| 2 SEND | 完整以太网帧 | 空，发送提交状态 |
| 3 POLL | 空 | 一帧，或EMPTY |
| 4 RESET | 空 | 清接收队列、启用中转 |
| 5 STOP | 空 | 清队列、停止中转 |

应答Op为请求Op或`0x80`；Session/Sequence回显。状态0=OK、1=EMPTY、2=INVALID、3=NOT_READY、4=IO_ERROR。

- RK一次仅发一个请求，4秒内无应答重试相同序号/内容，最多三次。MCU缓存最近一次应答；相同请求重放缓存，不重复网口发送或领取下一帧。
- 当前会话必须使用连续序号（允许u32回绕）；同序号不同内容拒绝。新Session必须先HELLO。此缓存不提供跨MCU复位的执行保证。
- RK三次超时后停止新事务，需重启RK/重新加载驱动握手；不隐瞒通信故障继续推进。
- MCU只转IPv4/ARP：目的为RK虚拟MAC的包不交给MCU lwIP；广播同时保留给MCU。其他MAC的单播不转给RK。
- MCU接收队列8帧，满则丢弃并计数。线上的DHCP/TFTP正常重传由其原协议处理，不伪造下载完成。
- 从机网口发送统一排到tcpip线程；回调尚未结束时不得复用发送缓冲。RK掉电后清会话和收包队列。

## 6. 重建

在`D:/pro/test`执行（Python需可访问本机工具链，EDK2远程构建需paramiko）：

```powershell
python -B experiments/mcu_pxe/build_mcu.py
python -B experiments/mcu_pxe/build_edk2.py
python -B experiments/mcu_pxe/tests/run.py
python -B experiments/mcu_pxe/check_artifacts.py
```

EDK2脚本默认从`/disk1/lilihong/uefi-b1-20261026`复制到`/disk1/lilihong/uefi-b1-serial-net-20261103`，SSH验证已保存主机密钥并提示密码，不保存密码。MCU四个集成点的补丁与源码散列另存于`build/mcu_pxe`；原B1源码改变导致锚点不匹配时停止构建，需人工核对。

## 7. 依据

- 现有B1 EDK2：`E:/MYZR-RK3588/uefi/b1-edk2-20261026/README-B1.txt`；保留其DDR容量识别和安全内存保留，不承诺测试全部物理内存。
- B1的20260920-2134原理图第6/14页：UART4 ↔ U4008 ↔ RK UART1_M0；USART2 ↔ U4013 ↔ RK UART2_M0调试。`UART_RX_M0`是网络名，其中M0不表示UART0。
- [UEFI SNP/PXE接口](https://uefi.org/specs/UEFI/2.10/24_Network_Protocols_SNP_PXE_BIS.html)。
- [Rockchip U-Boot UART1初始化](https://github.com/rockchip-linux/u-boot/blob/next-dev/arch/arm/mach-rockchip/rk3588/rk3588.c)及[Linux RK3588时钟定义](https://github.com/torvalds/linux/blob/master/drivers/clk/rockchip/clk-rk3588.c)；主CRU及IOC偏移同时核对本地RK3588 TRM Part1。
- 用户现有MemTest86 Site PXE包`readme.txt`及[厂商PXE说明](https://www.memtest86.com/tech_pxe-client-config.html)。商业包仍留在用户目录，不纳入源码仓库。
