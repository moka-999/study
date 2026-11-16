# MCU TCP常连、同IP串口网卡与4M验证

2026-11-12：B1默认改为保留MCU的IP和TCP连接，EDK经串口网卡使用同一个IPv4。
当前实现已纳入 `D:/pro/b1` 默认主工程，EDK 实际源码位于 `D:/pro/edk2`；正常构建直接包含本功能，不再依赖临时复制工程。实际C离线验证、B1完整Boot＋App构建和完整EDK/SPI NOR打包已通过。
同日用户日志确认EDK运行H1C；15:20单轮NBP约10.14秒、TFTP零重传，随后XML/HTML上传落盘。另一轮session 53479399在PXE失败退出时，用户确认网络助手原连接未断且心跳回复。尚缺本轮MCU的4M确认日志、成功测试全程PC连接和连续稳定性验收，见[分项证据](../../test/experiments/mcu_pxe/handoff/pxe_server_conflict_20261112.md)。
11月11日的2M独占交接实测主程序下载15.147秒，不能作为本版4M或TCP共存的实测结论。

**正式SLT自动接入（本地代码及离线验证通过）：** 同一0106内按PC任意顺序串行FUNC/PARA/SLT，支持重复算法，PC010A按本轮电源/Boot/时序完成冷启动后自动进入EDK；010B等待EDK最终RELEASE OK发完、OFF及115200/解析器恢复后才回completed，再继续本轮下一项。入口与串口调试分开，不改PC字段/命令及既有电源时序，详见[合同、完整JSON及待验证项](../../test/doc/pc_slt_auto_integration_20261112.md)。正式自动编排的PC业务、RK与运输实际C回归、B1 check/build通过，日志见接入合同；以下既有实板记录不替代本次正式自动流程验收。

## 本地主工程入口

- B1：`D:/pro/b1` 的顶层CMake直接编入串口网卡、帧分流与会话模块；`B1_SERIAL_NET_KEEP_TCP`默认1。Ctrl+Shift+B构建完整Boot＋App，输出 `build/standalone/Debug/b1.elf` 和 `b1.bin`，不自动烧录。
- EDK：`D:/pro/edk2` 实际源与当前配套源一致，沿用现有本地WSL入口。当前前级以 `loader/rk` 的唯一bin为准，最新产物入口见 `D:/pro/test/build/mcu_pxe/handoff/wsl-latest.json`，不回退覆盖新Loader。
- 本次本地整合已运行B1 `check`、默认 `build`、MCU两模式实际C会话测试、帧分流及EDK三组实际C离线回归；均通过。EDK QEMU链接器仍有既有enum/wchar ABI告警；这些合同测试不替代AArch64固件或硬件验收，当前EDK代码与既有完整构建一致。
- 本次B1 BIN为412008字节、CRC32 `49D09818`；构建时间元数据会随日常构建变化。日志在 `D:/pro/b1/build/mainline_h1c_20261112/`，未访问远端、未推送或烧录。

## 网络怎么分流

MCU持续持有物理网卡MAC和运行IP，对外ARP始终由MCU应答。EDK仍看到自己的虚拟MAC，DHCP的chaddr和既有06开头固定绑定保持原样。
MCU只转换以太网源/目的MAC及ARP硬件地址，不改IP、TCP、UDP正文或其校验和，不增加第二个IP。

| 流量 | 共存模式处理 |
|---|---|
| 发给本机TCP 8888的包 | 交原lwIP/TCP任务，保留socket、发送Seq和半包状态 |
| EDK的其余TCP及UDP | 走串口网卡；TFTP动态传输端口也覆盖，不只转发UDP 69 |
| ARP请求 | MCU应答；EDK自己的地址探测、宣告及应答不再发到局域网 |
| 远端ARP应答 | 原包给MCU，改写副本给EDK，供它解析服务器MAC |
| ICMP echo请求 | MCU应答；错误报文按引用的TCP源端口或UDP交相应协议栈 |
| 旧ARP缓存仍指向06虚拟MAC的TCP 8888 | 收到后改成MCU目的MAC，仍交原TCP连接 |

纯帧分流在`App/protocol/serial_net/net_mux.c/.h`，会话授权、队列和改速仍在`App/tasks/net_handoff`。
共存时准备阶段不撤IP、不清ARP缓存、不关闭监听。RELEASE停止RK转发并等待其已提交的tcpip回调结束；已交ETH驱动的pbuf由驱动持有引用，不要求PC持续流量也排空。
独占模式仍保留原来的撤IP、DMA排空、恢复IP流程。

当前边界：无VLAN、IPv4；EDK不支持IP分片，MCU自身TCP分片仍交lwIP。TCP 8888保留给MCU，EDK不能用它作TCP源端口，UDP 8888不受此限制。
当前MCU没有UDP网络业务；以后增加MCU DHCP/DNS等客户端或服务时须补充分流归属，不能直接把UDP仍全部交EDK。
会话期间新FUNC/PARA/SLT、重启、改压和B2改IP仍遵守busy门禁；当前SLT所有者的010B走正式收尾，查询、心跳和0601缓存采样可以持续使用。网络共存不等于允许并行执行其他DDR测试。0107不提前取消或释放SLT；重复010B、断线归属及同轮继续规则见正式接入合同。

## 配置及兼容

- `Core/Inc/board_network_config.h`：`B1_SERIAL_NET_KEEP_TCP=1`为本版默认；改0重编只选择旧手动独占IP交接，正式010A回notSupported。`B1_PC_TCP_PORT`同时用于监听和分流，避免端口定义不一致。
- `App/protocol/serial_net/serial_net.h`：`SN_DATA_BAUD=4000000`，`SN_LEGACY_DATA_BAUD=2000000`，启动速率115200。
- `0x35`、`0x3E`和首次HELLO均为115200。授权后的低速改速回复必须发完至TC，EDK等待20ms后切换，再用高速HELLO确认。只见trial不算成功。
- MCU可接受旧EDK的2M请求；新EDK遇到旧MCU明确拒绝4M才尝试2M，两档都拒绝才保持115200。静默超时不是拒绝，不据此盲目切换波特率。
- MCU试切后1秒未收到合法高速HELLO会恢复115200；已经确认后的通信故障不自动换速，需要明确下电重开一轮。协议口以外的两条日志串口不变。

MCU UART4时钟100MHz、16倍采样、BRR=25可以精确得到4M。RK使用现有1.5GHz GPLL的16/375分数输出得到64MHz，再16倍采样、除数1得到4M，不修改共享PLL或SPL。
电平转换器22M额定值不能代表整条PCB线路和驱动已通过4M验收；MCU TX仍逐字节TXE中断，CPU负载、误码率及真实吞吐必须实测。

DHCP仍需服务器批准本轮地址。现有Tftpd64的06 MAC→当前MCU IP固定绑定可保留：其[静态绑定路径](https://github.com/PJO2/tftpd64/blob/master/src/_services/bootpd.c)直接选地址，动态池则可能先做地址占用探测。MCU在线时，带ARP/ping占用检查的普通动态池可能拒绝这个已使用IP；不能靠屏蔽MCU ARP来绕过。当前PC源码的空闲地址检查基于本地租约表，PC软件这轮未修改。
Option50不负责选择PXE服务器，先前偶尔选到`.137`的问题并未由本次解决。Tftpd64/mt86.cfg这轮均未改。

## 手动传输通路验证

1. 由操作者在RK空闲、可复位时安装本版配对MCU ELF和EDK SPI NOR升级IMG。安装会复位/关RK；“连接保持”指固件运行中的EDK启动与退出，不指MCU烧录或复位。
2. PC网络助手连接MCU当前IP:8888，发0101确认0102，保持这条连接。可按正常周期继续0002心跳或0601查询，首轮不要用无间隔洪泛作为基准。
3. RK处于OFF且没有在途测试时，MCU控制台执行`net rk`、`start rk`。当前调试IP仍为192.168.1.201；正式地址仍来自既有B2配置流程，本功能读取实际运行地址。
4. MCU日志应出现`phase=shared`和`UART data baud=4000000 confirmed`；EDK也应确认4M，并从预期的`.200`取得BOOTAA64.efi。SPL日志仍走115200。
5. 下载和运行期间，在同一PC连接继续验证0101/0102、0002/0001及0601/0602；不应出现由网络交接引起的closed/reconnected或Seq重新起算。
6. 记录TFTP开始/完成时间、实际blksize及重传数，确认XML和HTML都落盘。记录启动到界面时间时与NBP下载时间分开，不能混为一个指标。
7. EDK自动结束后MCU进入`phase=edkIdle`，同一PC连接仍可查询。`end rk`后确认115200恢复，再验证下一轮普通FUNC流程。
8. 对比开始前和结束后的`status`：UART4 err/drop/restartFail不得新增，堆余量及PC连接应稳定。至少重复数轮，再比较仅下载与带普通心跳/采样查询的耗时。

EDK完整菜单、自动PXE及应用返回后的RELEASE保留。以上net rk/start rk仅用于手动调试通路；正式010A/010B由独立PC业务入口编排，不自动执行串口命令。本轮自动接入已通过独立离线验证，仍待实板验收，步骤见[正式SLT接入](../../test/doc/pc_slt_auto_integration_20261112.md)。

## 离线验证及产物

- `tests/test_mcu_overlay.py`：实际B1源同时测试独占和共存，保持connected=1的授权/停止/复位/归还/OFF，以及4M和旧2M协商。
- `tests/run_net_mux.py`：真实C/QEMU测试TCP8888、动态TFTP、1468/1024分块、DHCP、ARP、ICMP、截断及分片；1000组交错帧只证明分流逻辑，不是性能压力测试。
- 原PC FUNC/PARA/SLT/整轮/电源服务合同，UART大帧接收/日志/IDD回归，B1 build/check通过。
- EDK实际C的SNP/自动PXE/DHCP合同、WSL完整AArch64构建、47个FFS及厂商内外包校验通过；BaseTools仍有原有构建告警。

配对B1和验证记录在`D:/pro/test/build/mcu_pxe/handoff/coexist_4m_20261112/`。
EDK SPI NOR包为`D:/pro/test/build/mcu_pxe/handoff/wsl_20261112_092959/update_spinor_rk_for_edk2_h1.img`，版本标记`B1-SERIAL-NET-20261112-H1C`，SHA256为`0750c24a0b8b1eb2328e4b707096b5d3c7afa93e8d5e9d6b659ac0406a4d6679`。
上述09:29配对包保留当时Loader，属于历史交付；后续本地更换前级时，以当前构建目录的verification.json为准，不覆盖已有新前级。2026-11-12本地主工程整合只更新入口说明、注释及验证，不推送仓库、不自动烧录、不操作电源或服务器；实板状态按上述分项证据记录。
