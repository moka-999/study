# H1：MCU / EDK同IP通路与正式SLT接入

**当前入口（2026-11-12 H1C）：已纳入 `D:/pro/b1` 默认主工程和 `D:/pro/edk2`，MCU保留IP/TCP8888，EDK共享同IP；授权后优先4M、明确拒绝时兼容2M。** SPL/首次HELLO仍为115200，DHCP Option 50请求本轮地址。日常 MCU 构建直接使用 B1 的 Ctrl+Shift+B；编解码及分流在 B1 的 `App/protocol/serial_net`，会话协调在 `App/tasks/net_handoff`。本目录 `common/mcu` 保留接入时的来源，后续维护以 B1 为准；EDK 同步和离线测试读取 B1 的共用协议文件。`build_handoff_mcu.py` 对已接入 B1 只作复制归档，不再注入旧覆盖层。操作见[共存与4M说明](D:/pro/b1/docs/net_coexist_4m_20261112.md)，实板反馈见[单轮下载及失败退出常连证据](pxe_server_conflict_20261112.md)。正式010A/010B自动接入已完成本地代码与离线验证，尚未实板验收，合同及完整PC JSON见[正式SLT接入](../../../doc/pc_slt_auto_integration_20261112.md)。

正式入口由同一0106中的PC测试服务受理010A，沿PC目标电压冷启动、按查询回复Freq、SPL心跳后发送普通时序，completed后复用底层进入EDK/PXE。按11月13日合同，最终RELEASE OK实际发完后自动完成OFF、115200和解析器恢复，再允许下一项FUNC/PARA/SLT；不等待010B，不发送010B completed，合法010B回notSupported。PC收齐并解析报告后用0107.failChips汇总灯态，RELEASE不表示上传成功或PASS。它与串口net rk/start rk调试入口分开；独占宏0仅支持手动。11月16日另接B1的0104主动停止与8889网络日志；完整自动流程仍待实板验收，不沿用下方历史“先断开PC”的步骤，也不把旧下载记录算作正式自动流程验收。

以下为2026-11-10/11的H1/H1A独占交接历史。其版本、固定产物及待办仅指当时；当前固件从B1默认构建取得，EDK升级包以`build/mcu_pxe/handoff/wsl-latest.json`及所指目录的`verification.json`为准。

2026-11-11后续已完成[H1A EDK自动测试入口](edk_auto_slt_20261111.md)及SPI NOR升级整包，沿用本页的MCU授权/启动方式，获得授权后自动PXE、应用返回后自动归还。用户已反馈本页手动链路闭环成功；H1A自动版仍待单独实板验收。

2026-11-11。用户转述 RK 已实测：在 SPL 期间发送 `5000[2]=3E00`（命令 `0x003E`、无参数）会继续后级引导。MCU 验证覆盖层已接入此步骤，并保留自动进入 EDK 的前级兼容性。这里按新确认解释为继续引导，不根据旧文档的 `reboot` 名称额外操作电源。板上仍须有能够加载 ATF / EDK2 的前级和后级镜像；本轮未连接板卡或烧录，EDK 协议与镜像不变。

## 本轮实现

- **完整 MCU 保留。** `build_handoff_mcu.py` 从当前 `D:/pro/b1` 生成独立复制工程；保留 Boot、原 RK 任务及 FUNC/IDD/SLT 等代码，不替换为旧 D4 的精简网桥任务，不修改正式 B1/V2、电源时序或旧 D4 文件。
- **一个 UART 消费者。** `net rk` 后仍由原 RK 任务消费串口。先复用正式 SPL 心跳、频率查询与本轮 Seq 窗口；业务和 UART 空闲、本轮 SPL 普通心跳确认后，发送一次 `0x003E`，TX 环及硬件 TC 均空闲后切到 SerialNet v2。未约定跳转 ACK，不等待算法结果；EDK HELLO 才是进入后级的证据。
- **兼容自动前级。** 若先收到合法 EDK HELLO，直接停止 TLV 并处理交接，不再补发 `0x003E`。无有效心跳、发送入队失败或 TC 未完成都不靠延时放行；发送失败不自动重试，保留 MCU IP，明确掉电恢复后再试。开始跳转或识别 EDK 后，额外电源配置只允许明确下电。
- **明确所有权。** MCU 保留实际 IP/掩码/网关。无已接受 PC 连接、RK 真正 ON、原请求和串口 TX 均空闲后，先关闭监听、等待 ETH TX 空闲，再由 tcpip 回调撤销 MCU IPv4；PHY、网口及桥接继续运行。
- **EDK 授权后联网。** V2 HELLO 返回本轮 session 对应的 MAC、link、MTU、实际 IP/掩码/网关；未许可返回 NOT_READY。EDK 只在拿到合法许可后安装可联网 SNP，MAC 与 MCU 分开、IP 相同。
- **归还可重试。** 明确 RELEASE 后立即停新转发，等待 tcpip 已排队发送及 ETH DMA 全部完成，恢复 MCU IP/ARP 和监听后才回复成功。保持 EDK_IDLE 串口会话，允许重放归还应答；确定 RK 掉电后才回普通 TLV。
- **错误不会变成抢占。** 超时、断线、SNP STOP/RESET、DHCP RELEASE 均不自动归还。错误 DHCP 地址与出站源 IP 被拒绝；运行阶段不能被 B2 改 IP 或接受新算法。已归还且确实 OFF 后，可保留现有 PC 连接回普通模式。

## 首次实板操作顺序

以下是安装配对固件后的验证步骤，不是本轮已经执行的操作。

1. 使用本次[完整 MCU ELF](../../../build/mcu_pxe/handoff/spl_jump_20261111/artifacts/b1_handoff.elf) / [BIN](../../../build/mcu_pxe/handoff/spl_jump_20261111/artifacts/b1_handoff.bin) 和对应的 [H1 启动升级整包](packaging_20261110.md)。可使用自动进入 EDK 的前级，或 RK 已确认支持 `0x003E` 的 SPL。更换前级须重新打包；新增 MCU 命令不会修改已装的 SPL 或补齐缺少的后级。单独 BL33 不是整片 eMMC 镜像，不能直接按 Flash 偏移烧写。旧 D4 与 H1 的串口版本不兼容。
2. RK 保持 OFF。MCU 日志模式输入 `net rk` 加回车。它只进入准备状态，不上电、不立刻让出 IP。日志给出当前 `ip=` 和 `rkMAC=`，供地址绑定核对。
3. 在地址服务中登记 `RK MAC → 本次 MCU IP`，并固定本轮映射。沿用[连接时更新工具](../same_ip_handoff.md)，不能把候选配置文件保存成功当作 DHCP 已加载。实际启用 DHCP 是独立步骤，本轮没有执行；旧 PXE 代理不能与 Tftpd64 DHCP 同占 UDP 67。
4. 沿用现有 PC 连接与电源命令：eMMC包使用`0506 {"bootMode":"emmc"}`；新增的[SPI NOR包](../../../build/mcu_pxe/handoff/spinor_20261110/烧录说明.md)使用`0506 {"bootMode":"fspiM1"}`。按已经确认的板卡类型与目标电压发 `0509`（标称 LPDDR4X 示例为 1800/1100/600 mV）；发 `0501 {}` 并等待其 `0006 completed`。不发 `0106/010A`，不使用等待 SPL 心跳的 `0505`。本地 `start rk` 默认 fspiM1，不代替这组 eMMC 步骤。
5. **断开 PC 到 MCU 的 TCP 连接。** 使用测试 SPL 时，`log mcu` 可看到本轮 `SPL ready`、`[RK TX] ... 5000[2]=3E00 uart=queued`，随后 `[handoff] SPL cmd=003E tx=complete; waiting EDK HELLO`；自动前级可直接进入 HELLO，不出现 `003E`。EDK 的 HELLO 等待窗口为约 60 秒；PC 仍连接时 MCU 不让出地址。通过 `log rk` 查看 SPL / ATF 是否确实进入 `B1-SERIAL-NET-20261110-H1` EDK，以及 HELLO 获得的授权 IP。
6. 在 EDK Shell / PXE 中取得该 IP 并访问 PC。首轮只验证 DHCP、ARP、同 IP 网络访问，不必启动完整 MemTest86。PC 侧抓包应看到同一 IP 对应 RK MAC，此时 MCU 的 TCP 8888 不提供服务。错误地址应被拒绝，不能用另一租约继续测试。
7. 确认网络测试应用已退出。在 UEFI Shell 执行 `connect -r`、`map -r`，查看各 `fsN:`，找到只读固件卷内的 `NetReturn.efi`。`fsN:\NetReturn.efi` 仅显示授权；显式执行 `fsN:\NetReturn.efi return` 才拆卸网卡并归还。该入口不依赖 USB 盘或 EDK 的 eMMC 文件系统。
8. 看到归还成功后，PC 重新连接原 IP，发送 `0101 {}` 验证仍收到 `0102`。此时 RK 仍停在 EDK，FUNC/IDD/新整轮保持 busy。通过原 `0502` 或已允许的本地 `end rk` 确认掉电后，才恢复普通 SPL 测试模式。

MCU 的 `net mcu` 仅允许确认 OFF 时取消/回收，不能在 RK 仍持有地址时强制抢回。归还失败时先查看返回错误；不以等待时长推定 RK 已释放。已有 `end rk` 属于真实下电，须由操作者确认当前可断电，工具不会自动调用。

## 线格式与会话

V2 仍为 COBS、零分隔、CRC32：

```text
Version:u8=2, Op:u8, Status:u8, Reserved:u8=0,
Session:u32LE, Sequence:u32LE, Length:u16LE, Payload, CRC32:u32LE
```

| Op | 含义 | 与 IP 所有权的关系 |
|---|---|---|
| 1 HELLO | 空请求；许可成功返回 21 字节 | MCU 撤销 IP 之后才可成功 |
| 2 SEND / 3 POLL | 以太网帧发送 / 领取 | 必须处于本 session 已授权状态 |
| 4 RESET / 5 STOP | SNP 网卡生命周期 | 不归还 IP、不释放测试占用 |
| 6 RELEASE | 空请求，明确归还 | 排空并恢复 MCU 监听后才返回 OK |

HELLO Payload：MAC 6 字节、link 1 字节、MTU 2 字节小端、IPv4/掩码/网关各 4 字节网络序。session 为 RK 本次启动的非零编号，MCU 已接纳后不允许另一 session 替换。

重复相同事务回缓存，不重复发包/取包；NOT_READY 是已确认状态，后续查询使用新序号。数据事务完全丢失时，显式空 RELEASE 可在同 session 内有限前向跳号，避免为了补序号重放未知 SEND；旧序号和异 session 仍拒绝。归还 ACK 全丢时保留原 RELEASE 事务，人工再次执行可取缓存。

首版只支持无 VLAN 的 IPv4/ARP，拒绝 IPv4 分片；DHCP 初始化源 0 有专用校验。协议过滤不替代受控的 DHCP 配置，也不证明 MemTest86 独立 DHCP 的整个流程已经通过。

## 源码、构建与验证

本机WSL已完成编译和三种升级整包校验，无需SSH。当前入口为`D:/pro/edk2/make-h1-wsl.sh`；在WSL执行：

```bash
cd /mnt/d/pro/edk2
bash make-h1-wsl.sh
```

分别生成的入口也在`D:/pro/edk2`：`bash make-emmc-d4.sh`、`bash make-emmc-rk.sh`、`bash make-spinor-rk.sh`，分别对应普通D4前级eMMC、RK新前级eMMC、RK新前级SPI NOR。每个脚本都会先编译H1，再只生成所选整包。

RK更新前级时，只需替换`D:/pro/edk2/loader/rk/`里的bin：当前目录保留一份，名字任意；旧版本可移到子目录。无需修改脚本或SHA256，构建前会检查LDR头、CRC及文件唯一性，并记录实际输入散列。源码同步不覆盖用户放入的bin；D4入口保留原历史前级。

脚本编译完整H1，再生成普通D4 Loader的eMMC包、RK新Loader的eMMC包、RK新Loader的SPI NOR包。输出到`D:/pro/test/build/mcu_pxe/handoff/wsl_日期_时间/`，最新成功目录记录在`wsl-latest.json`。Linux afptool生成内包，外层通过WSL互操作调用本机Windows RKImageMaker 2.23；不是完全原生Linux打包。构建和打包只产生文件，不访问板卡。输入、工具依赖及已通过的校验见[本机WSL打包](packaging_20261110.md#本机wsl编译和打包)。

2026-11-10已将完整H1源码、两份前级和本机预编译产物同步到服务器`/disk1/lilihong/uefi-b1-20261110`，36180个文件逐项SHA256核对通过。服务器构建入口为`build-h1.sh`；当前原交叉工具链路径权限不足，尚未在服务器重编。见[同步记录](server_sync_20261110.md)，旧服务器工程未修改。

| 入口 | 内容 |
|---|---|
| `common/serial_net.c/.h` | V2 帧与去重，单 session、归还跳号例外 |
| `common/ip_guard.c/.h` | 两端共用 IPv4/ARP/DHCP 地址检查 |
| `mcu/handoff_state.c/.h` | 地址所有权状态及门禁 |
| `mcu/handoff_mcu.c/.h` | UART、tcpip、转发和任务上下文接入 |
| [`../build_handoff_mcu.py`](../build_handoff_mcu.py) | 只复制当前 B1 到本仓 build 子目录，输出可审阅的接入 patch 与来源 SHA256 |
| [`edk2/README.md`](edk2/README.md) | 完整 EDK 同步、构建及手动归还入口 |

```powershell
python -B experiments/mcu_pxe/handoff/tests/run_contract.py
python -B experiments/mcu_pxe/handoff/tests/test_mcu_overlay.py
python -B experiments/mcu_pxe/handoff/edk2/run_tests.py
python -B experiments/mcu_pxe/build_handoff_mcu.py --out build/mcu_pxe/handoff/spl_jump_20261111/b1-snapshot
```

本次 MCU 产物目录为 `build/mcu_pxe/handoff/spl_jump_20261111/artifacts/`，旧 `handoff/artifacts/` 内的 11 月 10 日固件没有 `0x003E` 步骤。完整 B1 的 Boot 位于 0x08000000、App 位于 0x08020000；不是单独 App 文件。当前复制板级配置为测试 IP `.201`、无 W25、IWDG 关闭，均沿用原 B1。正式 B2 地址配置不改变；交接使用实际 netif 地址，不使用写死的 `.201`。

11月11日完整构建与6项覆盖层测试通过；实际C测试同时链接复制工程的 `rk_task.c`、`rk_link.c`、组帧与状态机，覆盖 RK 给定金样、动态 Seq、旧/未来/坏 CRC 心跳拒绝、启动频率、业务/UART门禁、TC前不得消费HELLO、失败不重发、自动EDK兼容及原IP归还流程。ELF 3951704字节，BIN 409668字节，CRC32 `273C5CF2`；Boot＋App范围及ELF/BIN字节一致，构建无warning/error。校验与SHA256见[本次报告](../../../build/mcu_pxe/handoff/spl_jump_20261111/artifacts/verification.json)。以下保留11月10日初版配对产物记录。

2026-11-10 已完成本机完整构建及产物核对：

| 产物 | 大小 | 核对内容 |
|---|---:|---|
| `b1_handoff.elf` / `b1_handoff.bin` | ELF 3929148 / BIN 408628 字节 | Boot＋App、ELF/BIN 内容、CRC32 `D2A54F48`；6 项 MCU 测试通过 |
| `BL33_AP_UEFI.Fv` | 983040 字节 | 完整 AArch64 DEBUG；实际解压核验 SerialNetSnpDxe、FvSimpleFileSystem、NetReturn 与 H1 标记 |
| `NetReturn.efi` | 20480 字节 | AArch64，和固件卷内应用逐字节一致 |
| `update_normal_d4_loader_h1.img` | 5206602 字节 | 使用此前成功进入 D4 的前级，RKFW/RKAF 整包 |
| `update_rk_for_edk2_loader_h1.img` | 6484554 字节 | 使用本次 `rk3588_spl_for_edk2.bin` 前级，RKFW/RKAF 整包 |

配对来源、检查日志和 SHA256 见 [verification.json](../../../build/mcu_pxe/handoff/artifacts/verification.json)。MCU 构建无新 warning/error；EDK 保留旧主机工具、VFR、Python 和链接器告警，不称为零告警构建。原服务器交叉工具链为 Arm GCC 10.3，本次本地使用 GCC 11.5，兼容补丁和备份见 EDK 说明。后级 FIT 及两套启动升级整包已生成，IMG 位于 `build/mcu_pxe/handoff/update_20261110/`，见 [打包记录](packaging_20261110.md)和 [整包校验](../../../build/mcu_pxe/handoff/update_20261110/verification.json)。两包沿用原 B1 全部 GPT 定义，仅带启动载荷，不带 Linux 镜像；本轮未安装镜像或启用网络服务。

历史实际C检查涵盖未授权拒绝、关闭监听/撤IP顺序、UART TC、异session、DMA与callback未排空、旧数据包延迟到达、显式归还丢包重试和OFF恢复。它不代表当前共存模式或正式010A/010B自动编排的实板验收；当前接入合同与验证状态见页首入口。
