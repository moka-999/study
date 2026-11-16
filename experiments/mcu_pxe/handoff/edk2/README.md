# H1C 本地完整 EDK2：同 IP 保留 TCP

当前源码为 H1C：首次授权后优先协商协议口 4,000,000 baud，DDR/SPL及首次握手保持115200。只支持2M的旧MCU明确拒绝4M后，EDK再协商2M；旧MCU两档都明确拒绝时保持115200。通信超时不能当作拒绝自动降档。授权网卡的 DHCP DISCOVER 继续加入本轮 IP 的 Option 50，保留完整固件和原 MAC 固定绑定。配对MCU的网络共享方式及验证状态见 [本次说明](D:/pro/b1/docs/net_coexist_4m_20261112.md)。以下 H1A 描述为历史流程，不能代替当前源码和实板验收结论；B1已接正式010A启动和RELEASE自动收尾，010B不再作为结束门禁。

当前本地EDK主工程为`D:/pro/edk2`；本目录保留其修改同步源、构建/打包入口和离线测试。MCU正式源已接入`D:/pro/b1`默认构建，直接使用B1的Ctrl+Shift+B，不再依赖临时MCU覆盖镜像。`handoff/common`、`handoff/mcu`和旧D4目录仅作历史来源；共用协议源码以B1的`App/protocol/serial_net`为准。

默认共存模式下，MCU一直持有运行IP、物理MAC及TCP 8888连接；对外ARP由MCU应答。EDK仍使用虚拟MAC进行DHCP，MCU负责MAC/ARP转换及报文分流。RELEASE结束RK分流，不撤销MCU地址、不关闭PC连接。正式010A的MCU服务在RELEASE最终OK发完后自动下电、恢复115200及解析器并释放占用，010B回notSupported，下一项可在同一0106内继续；详见[正式SLT合同](../../../../doc/pc_slt_auto_integration_20261112.md)。旧独占交接模式保留为对照，不是默认操作。EDK自动PXE及应用返回后的释放已经接入，RELEASE不判断报告上传成功或测试PASS。

4M的时钟来自现有GPLL的分数输出：GPLL为1.5GHz时使用16/375产生64MHz，UART除数1、16倍采样得到4M；2M对应8/375。不修改共享PLL、SPL或调试UART。B1当前UART4内核时钟100MHz、16倍采样对应BRR=25。离线测试覆盖两档协商、旧MCU、应答丢失、时钟读回与发送排空；4M线路误码和实际吞吐需配对固件实板验证。

历史H1A自动入口见[2026-11-11接入记录](D:/pro/test/experiments/mcu_pxe/handoff/edk_auto_slt_20261111.md)。H1C保留控制台就绪后3秒ESC窗口、仅选择授权网卡IPv4 DHCP PXE、执行一次、应用返回后RELEASE并停在菜单的行为。未获授权仍走原EDK启动；ESC保留手动PXE/NetReturn。旧记录中全程115200、撤IP及PC重连的步骤仅适用于当时版本，当前改速和共存规则以上文为准。

本目录是新协议源码；旧 `experiments/mcu_pxe/edk2` 和 `common` 仍为 D4 来源，不混用。
本轮沿用已存在的 ATF/BL33 引导，不新增 SPL 命令或修改 SPL。EDK不处理PC的010A/010B，也不判断MemTest86测试结果；010A由MCU的PC测试服务受理，最终灯态由PC的0107.failChips给出。

## 地址使用

- SerialNet V2 HELLO 必须返回 21 字节：MAC 6、link 1、MTU 2 小端、IPv4 4、掩码 4、网关 4（地址字段为网络序）。没有授权时等待最多约 60 秒，不安装可联网的 SNP。明确 NOT_READY 使用新序号再查；通信超时保持故障。
- 使用共用 `ip_guard.c` 限制出站源 MAC、IPv4 源地址和 ARP sender。初始 DHCP 允许合法的源 0 请求；收到的 DHCP 非零 yiaddr 必须等于授权 IP，Proxy Offer 的零地址允许。普通入站包的 PC 源地址不受该限制。首版只支持无 VLAN 的 IPv4/ARP，拒绝 IPv4 分片。
- DHCP DISCOVER 用 Option 50 申请当前 MCU IP；服务器仍须按自身策略发放正确租约。Tftpd64 当前的 RK MAC 固定绑定继续保留，过滤器不会替服务器生成租约。

## 显式归还

完整固件加入 EDK2 自带的只读 `FvSimpleFileSystemDxe`，把固件卷内应用暴露给 Shell，不依赖 eMMC 文件系统或 USB。
进入 UEFI Shell 后执行 `connect -r`、`map -r`，查看列出的 `fsN:`，用 `ls fsN:\` 找到 `NetReturn.efi`。
`fsN:\NetReturn.efi` 只显示授权 IP/session；确认 MemTest86 已退出后执行 `fsN:\NetReturn.efi return`。
`N` 以实际映射为准，不预设为 0。也可以把构建所得的同名应用放在已有 FAT 文件系统中运行。

自动入口和手动工具都复用同一个归还接口。驱动先进入不可重新启动的 RETURNING 状态，禁止新收发，再断开该 NIC 上的 UEFI 网络驱动并检查 SNP 已停止；成功后清本地队列并发送独立 RELEASE。MCU在独占模式排空以太帧并恢复IP后才应答OK；共存模式保留IP/TCP，只停止RK分流并等待其已提交的tcpip回调结束，不要求PC流量排空。
NOT_READY 在 30 秒窗口内以新序号查询；应答丢失重试原序号。失败保持禁发，可再次显式执行 return。成功后保留 RELEASED 状态，不能通过 SNP Start/Reset 再次联网；重新测试需要新的配对启动。
SNP Stop/Shutdown、DHCP RELEASE、断线和超时都不代表 SLT 已结束或地址已归还。

## 本地构建

当前产物入口是 [wsl-latest.json](D:/pro/test/build/mcu_pxe/handoff/wsl-latest.json)及其指向目录内的`verification.json`；清单记录实际Loader、BL33和整包散列。更换前级后须重新打包。本文及旧记录中的固定日期包是历史证据，不能据同名IMG判断它与当前源码或前级相同。

日常在WSL中执行`cd /mnt/d/pro/edk2`、`bash make-h1-wsl.sh`，即可编译完整H1并生成三种升级IMG。该入口依赖同级`D:/pro/test`的实验脚本与已配置的本机工具：内包由Linux afptool生成，外包经WSL调用Windows RKImageMaker 2.23，不访问服务器或板卡。输出在`D:/pro/test/build/mcu_pxe/handoff/wsl_日期_时间/`；完整流程及工具边界见[打包记录](D:/pro/test/experiments/mcu_pxe/handoff/packaging_20261110.md#本机wsl编译和打包)。

分别生成时，在同一目录选择一个命令。每个入口都会先编译H1，再只打包选定类型：

| 命令 | 升级整包 |
|---|---|
| `bash make-emmc-d4.sh` | 普通D4前级＋H1，eMMC |
| `bash make-emmc-rk.sh` | RK新前级＋H1，eMMC |
| `bash make-spinor-rk.sh` | RK新前级＋H1，SPI NOR |
| `bash make-h1-wsl.sh` | 上述三份全部生成 |

RK换前级时，把新完整Loader放入`D:/pro/edk2/loader/rk/`，将旧bin移出或移到其子目录，当前目录只留一份`.bin`，文件名任意。两个RK入口共用它，无需改脚本或SHA256。脚本在编译前检查文件唯一性、LDR头与CRC；实际路径和SHA256自动写入输出报告。D4入口仍用原历史基线。源码覆盖同步不覆盖该目录的bin。

以下为源码覆盖同步及仅构建BL33的分步入口。直接修改`D:/pro/edk2`源码后可直接编译，不必每次先运行覆盖同步脚本。

1. Windows 执行 `python experiments/mcu_pxe/handoff/edk2/sync_edk2.py`。仅同步 `D:/pro/edk2`，覆盖前保存在 `build/mcu_pxe/handoff/edk2-before-*`，同时记录前后 SHA256；不修改其他来源目录。
2. WSL 需安装主机 `gcc-11/g++-11/make`、Python 3、`gcc-11-aarch64-linux-gnu`/binutils 及平台构建所需工具。执行 `bash /mnt/d/pro/test/experiments/mcu_pxe/handoff/edk2/build_local.sh`；首次从其他 GCC 版本切换时在命令前加 `H1_CLEAN=1`，清除不兼容的旧 LTO 对象。脚本固定 GCC 11，不改变系统默认编译器。
3. 输出包括完整 BL33 `Build/RK3588/DEBUG_GCC5/FV/BL33_AP_UEFI.Fv` 和 `Build/RK3588/DEBUG_GCC5/AARCH64/NetReturn.efi`。DSC/FDF 保留整个固件，仅替换原 SerialNet 并加入该应用。
4. Windows 可运行 `python experiments/mcu_pxe/handoff/edk2/verify_firmware.py <BL33路径> <NetReturn路径>`，展开最终压缩固件卷，检查三个模块的 GUID/UI、AArch64 格式、H1 版本标记及独立应用和固件内应用完全一致。

编译与 QEMU 离线通过均不代表真板上电、PXE 全程下载、MemTest86 报告上传或地址归还已经验收；仅构建入口`build_local.sh`不打包，所有构建入口均不执行烧录。

## 2026-11-12实板证据边界

- 15:20轮次记录NBP下载约10.14秒，XML及HTML已落盘；下载耗时与启动到测试界面的总耗时分开记录，也不据报告落盘判断DDR PASS/FAIL。
- session `53479399`记录失败退出后PC原连接未断，并继续收到心跳回复；这证明该次失败退出路径保持连接，不代表该次PXE测试成功。
- 尚缺配对MCU的`UART data baud=4000000 confirmed`日志，不能只凭10.14秒下载判定4M全面验收。连续多轮、UART错误/丢包增量、正常完成及带PC查询时的连接保持和吞吐仍需分别确认。

## 历史构建记录

2026-11-10 本机完整 DEBUG/GCC5 构建成功，交付 BL33 为 983040 字节、NetReturn 为 20480 字节。首次完整构建日志及最终固件卷核验分别位于 `build/mcu_pxe/handoff/edk2-build.log` 和 `edk2-image-check.json`，配对产物在其 `artifacts` 子目录。旧 Pccts/VFR/Python 和链接器仍有告警，不能把构建成功描述为零告警。同日已在WSL重编并封装FIT及三种升级整包，实际固件卷、FIT组件、厂商双层解包及CRC/MD5检查通过，见各次输出目录的`verification.json`。

## 本机构建兼容处理

- 原快照带有 GCC 11 主机编译产生的 `.o/.d`，会引用本机不存在的系统头。脚本先通过 BaseTools 自带 clean 清理主机产物，不删除固件组件。
- 本机默认 GCC 15 与旧 Pccts/USB 的接口声明产生新诊断；最终构建固定本地 GCC 11.5（原主机工具来自 GCC 11、原 AArch64 交叉链为 Arm GCC 10.3，不能视为完全同版）。主机 C 工具明确使用 `-std=gnu11`，保留既有 `-Werror`，不修改 USB 等模块规避新诊断。
- GCC 15 找到 GenFfs、GenSec 内存分配失败路径的同一错误：`fclose` 后把文件指针作为 `%s` 参数。`BaseTools-GenFfs.patch` 和 `BaseTools-GenSec.patch` 各仅将参数改为输入文件名；同步前备份原文件，只修本地工程，不覆盖来源快照或服务器。
- GCC 15 尝试构建时曾对 LZMA 单个对象保留 `dangling-pointer` 告警继续诊断；最终 GCC 11 构建已移除此临时选项，不改变 LZMA 算法或全局告警策略。
- Rockchip 少量旧模块包含宿主 `stdlib/string/malloc/stdio` 头。`Rockchip-libc.patch` 将实际使用的整数除法、清零、拷贝等价替换为 UEFI 的 `DivU64x32/ZeroMem/CopyMem`，补直接库依赖并删除未用头；不引入 glibc，不改变 PWM/PCI/显示/Flash 的硬件写入流程。
