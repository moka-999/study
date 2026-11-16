# H1 后级 FIT 与用户提供的 Loader

## 本机WSL编译和打包

2026-11-10已验证本机`Ubuntu-26.04`构建和打包，无需服务器登录。当前推荐入口：

```bash
cd /mnt/d/pro/edk2
bash make-h1-wsl.sh
```

分别生成时，在同一WSL目录选择一个入口，每个脚本都会先编译H1，再只生成对应整包：

| 入口 | 选定类型 |
|---|---|
| `bash make-emmc-d4.sh` | 普通D4前级、eMMC |
| `bash make-emmc-rk.sh` | RK新前级、eMMC |
| `bash make-spinor-rk.sh` | RK新前级、SPI NOR |

也可在Windows PowerShell执行`wsl -d Ubuntu-26.04 -- bash /mnt/d/pro/edk2/make-h1-wsl.sh`。入口使用同级test工程的`build_pack_wsl.sh`，先构建完整DEBUG/GCC5固件，再运行`package_wsl.py`，不会执行源码覆盖同步或访问板卡。只重打包当前已构建BL33时，可执行`python3 /mnt/d/pro/test/experiments/mcu_pxe/handoff/package_wsl.py`。

仅重打指定整包时，在上述Python命令后加`--variant emmc-d4`、`--variant emmc-rk`或`--variant spinor-rk`；缺省为`all`。只加载所选包需要的Loader，并在校验报告记录选择，避免无关前级文件缺失阻断单包生成。

RK的新完整Loader放入`D:/pro/edk2/loader/rk/`，当前目录只保留一份`.bin`（大小写均可），名称任意。旧版本移出或放到子目录；脚本不递归搜索、不按日期猜选。两个RK入口使用同一个输入，无需改Python、SHA256或`.sh`。零份、多份、裸SPL或CRC错误在编译前停止；校验通过后显示路径、记录SHA256并使用同一份内容生成内外Loader。输出的`verification.json.loader_inputs`保存路径、大小和散列，结束时复核输入未改变。源码同步只更新该目录的说明，不覆盖bin，也不回退Downloads旧路径。

该入口已于同日通过完整构建和三包回读，输出为`build/mcu_pxe/handoff/wsl_20261110_164028/`。`tests/test_loader_input.py`的5项离线测试覆盖任意名称/中文路径、非递归旧版备份、替换内容后记录新SHA、缺失/多份/坏头/坏CRC及D4独立选择。人工修改样本仅在测试临时目录使用，不作为交付前级；本次正式产物仍采用原RK输入，未烧录。

本机依赖与范围：

- 固件使用WSL的GCC 11.5、AArch64 GCC 11.5及EDK BaseTools；保持原完整H1组件。
- 内包使用已从服务器只读复制的Linux afptool 1.62，路径`build/mcu_pxe/handoff/host-tools/afptool`，SHA256为`7a10b8ea538e90d6740f141b33a5d8a9883333916fdfc1fc192b4fbfc3ed3216`。它是静态链接的i386程序，本机WSL已实际运行；日常打包不再访问服务器。
- 外包通过WSL互操作调用`E:/MYZR-RK3588/发布包-烧录UEFI/02-烧录工具/RKDevTool_Release_v3.15/bin/RKImageMaker.exe` 2.23。因此流程从WSL发起，但仍依赖Windows工具，不能直接视为纯Linux服务器脚本。
- RK前级由上述投放目录选择，不再固定历史SHA256；D4前级、分区参数及D4参考FIT保留下文基线。仍需完整Rockchip Loader，打包工具不把裸SPL自动转换成Loader；新前级的实际板卡/EDK启动兼容性由联调确认。

输出为`build/mcu_pxe/handoff/wsl_日期_时间/`，各次独立保留；`wsl-latest.json`记录最近一次所选包全部校验通过的目录及`variant`，单包成功不表示同次也生成了另外两包。

| 文件 | 用途 |
|---|---|
| `update_emmc_normal_d4_h1.img` | 普通D4前级＋本次H1，eMMC升级整包 |
| `update_emmc_rk_for_edk2_h1.img` | RK自动进入EDK前级＋本次H1，eMMC升级整包 |
| `update_spinor_rk_for_edk2_h1.img` | RK自动进入EDK前级＋本次H1，32MiB SPI NOR升级整包 |
| `uboot_h1.img` | 仅FIT后级，不是完整升级包 |
| `verification.json` / `SHA256SUMS` | 校验报告与三个整包的SHA256 |

本次通过目录为`wsl_20261110_161840`。完整H1编译成功；校验包括固件卷实际模块与NetReturn、六个FIT组件独立提取、八项错误FIT输入、三个包的CRC/MD5及双层厂商解包、故意替换介质标志后重新计算MD5的拒绝检查。输入内容保持不变。初次校验发现afptool把uboot解到`Image/uboot.img`，修正检查路径后重新打包三份全部通过；原日志保留在`wsl-build-pack-20261110.log`，成功打包日志为`wsl-package-20261110.log`。未烧录，H1实板启动与IP交接仍待验。

## 早期打包过程与来源

2026-11-10。用户提供 `D:/Backup/Downloads/rk3588_spl_for_edk2.bin`，并明确反馈“RK 已确认自动进入”：该版本上电后自行进入 EDK，不需要 MCU 发送 SPL 跳转指令。此项作为 RK 的接入约定；本轮只做文件识别与打包，不修改输入，不访问串口、USB 下载器或板卡。最初尝试本机打包，随后按用户“服务器上的不可以么”的要求，使用服务器既有工具和本次新建的独立目录；不修改服务器原工程或原工具。

## Loader 识别

该文件是带 `LDR ` 头的完整 Rockchip Loader 容器，不是裸 SPL，也不是包含 EDK 的完整升级镜像。

- 大小：1141184 字节。
- SHA256：`066a2b3f03c78f06f17dd5a22884bf0f0994b5e0e785b62e798a08a192da9091`。
- 容器 CRC32 正确；六个组件范围连续且位于文件边界内：UsbHead、ddr、usbplug、FlashHead、FlashData、FlashBoot。
- FlashBoot 解码后含 `U-Boot SPL 2017.09-ga667db7f57-241224 #zhuoyi (Sep 21 2026 - 14:33:46)`，与旧 FUNC 整包该组件逐字节相同。
- DDR/FlashData 与旧 FUNC 包存在实际变化。首条 AArch64 指令跳到组件相对偏移 `0x2F004`，保存现场后调用 `0x391A0`；该调用若返回，再恢复现场并跳回原 `+4` 路径。不能将此静态入口改动等同于已经验证进入 EDK，也不能凭残留调试字符串认定它仍停在测试程序。

完整组件与对照证据见 [Loader 识别](../../../build/mcu_pxe/handoff/loader-inspection-20261110.json)、[静态对照](../../../build/mcu_pxe/handoff/loader-comparison-20261110.json)。REA 接口未接受 Windows 本机路径，本次证据来自本地容器解析、公开格式校验、字节比较及有限 objdump，不属于 REA 引擎结果。

## 后级 FIT 的打包边界

H1 的 `BL33_AP_UEFI.Fv` 已完整编译。后级 FIT 以历史通过 D4 启动的 `b1_serial_net_D4_network_20261103.img` 为结构基线，保留其 ATF、OP-TEE、DTB、加载地址及两个 2 MiB 副本，只替换 EDK BL33 和相应 SHA256 校验值。

新旧 BL33 均为 983040 字节且未压缩，因此可在解析确认的固定范围内等长替换，不移动组件、不改变 FDT 布局。原 FIT 含签名配置占位，但没有实际签名值；本轮不生成签名，不代表支持已开启签名强制校验的启动链。FIT 中的原时间戳和 reservation 等元数据保留为来源内容，不把原时间戳当成本轮打包时间。

产物约定：`build/mcu_pxe/handoff/package_20261110/uboot_h1_reference.img`，它只包含 FIT 后级，不包含 Loader、不属于 Rockchip `update.img` 整包。所提供的 Loader 无需重新封装或拼接到该 FIT。

RK 已确认该 Loader 自动进入 EDK；新 Loader 与本次继承的 ATF/TEE 完整启动 H1 的实际运行仍待实板日志，离线格式和散列检查不替代该验收。历史本板曾使用 `uboot` 的 LBA `0x4000`、4 MiB 分区，但本轮没有读取当前分区表，不能把历史地址当作通用烧录地址。

本轮未改电源时序、未烧录 MCU/RK、未执行上电/复位、未启动 DHCP/TFTP 服务，也未接入自动 `010A/010B` 流程。

## 两套 RKDevTool 启动升级整包

用户随后明确要求各生成一个供 RKDevTool“升级固件”页使用的整包，并确认“普通 SPL”就是之前能进入 D4 的前级。

| 输出文件 | Loader 来源 | H1 后级 |
|---|---|---|
| `update_normal_d4_loader_h1.img` | `build/mcu_pxe/recovery_loader_d4_20261104/b1_zhuoyi_20260819_loader.bin`，502208 字节，SHA256 `16efb11d4ed14c700749cdc1e95f03a71477aeff89378446f625016c9237d2b4` | 同一份 `uboot_h1_reference.img` |
| `update_rk_for_edk2_loader_h1.img` | 用户本次提供的 `rk3588_spl_for_edk2.bin`，1141184 字节，SHA256 见上文 | 同一份 `uboot_h1_reference.img` |

目录为 `build/mcu_pxe/handoff/update_20261110/`。两份均为 RKFW 外层＋RKAF 文件包，外层 Loader 与内层 bootloader 分别使用对应的同一份输入。它们不是把两份相同 FIT 改名为不同 SPL 版本。

分区参数取自此前实际 B1 源整包 `C:/Users/Administrator/Desktop/update-marchcx-mats-delta-complete.img`，本轮重新计算其全文件 SHA256，仍为 `3d0d85d4f71c659210dbf23e64065e7d43c4d8a761bcb43c54a807cdfd9ef891`。参数正文 539 字节，全部原样保留，包括 `uboot` 的 4 MiB/LBA `0x4000` 和原有其他 GPT 分区定义。

新文件清单仅保留 package-file 自身、parameter、bootloader、uboot，以及原清单的 `backup RESERVED` 标记；不加入 misc/boot/rootfs/oem/userdata 的旧 Linux 镜像。产物必须解包回读核对，不能仅以工具退出码判断成功。工具散列、命令、各层内容校验和输入不变检查保存在产物目录。

本机 AFPTool 2.2 的 `-pack` 在短 ASCII 路径和旧模板下也以 `0xC0000409` 退出，失败记录保留；不据此修改原 GPT。服务器实际工具位于 `/disk1/lilihong/tmp_sd_update/tools/`：Linux afptool 1.62、rkImageMaker 1.63，工具身份见 [服务器工具记录](../../../build/mcu_pxe/handoff/server-pack-tools-20261110.json)。仅向本次新目录 `/disk1/lilihong/codex-h1-update-20261110-6ce2981b729a/` 复制工具和已核对输入；原工具内容已在结束后复核不变。

最终成功流程是：服务器 afptool 1.62 生成两份 RKAF 内包，下载原样回读，再由本机 RKImageMaker 2.23 封装 RKFW。服务器旧 rkImageMaker 1.63 报 `Get image version failed`，其失败候选未作为交付文件。两套成品均经本机原厂 RKImageMaker 和 AFPTool 双层解包回读，并验证外层 MD5、内层 CRC、PARM CRC/原文、内外 Loader、同一 H1 FIT 以及输入不变，见 [整包校验](../../../build/mcu_pxe/handoff/update_20261110/verification.json)。坏 MD5 和错误 Loader 输入亦被检查器拒绝。

第二份独立解析检查也已通过，见 [独立验包报告](../../../build/mcu_pxe/handoff/update_20261110/independent-verification.json)。目录实际为四项有效载荷，加一项零长度的 `backup RESERVED`；后者没有文件数据。构建入口为 `package_updates_server.py`（服务器只生成内包）和 `finish_server_updates.py`（本机封装与回读），服务器密码只从 `SERIAL_NET_SSH_PASSWORD` 读取，不写入脚本或交付记录。

| 文件 | 字节数 | SHA256 |
|---|---:|---|
| [普通 Loader 整包](../../../build/mcu_pxe/handoff/update_20261110/update_normal_d4_loader_h1.img) | 5206602 | `f7060ca40a2f7fe090cd5219972d51921da27b55c971ed89b02d600478550012` |
| [RK 新 Loader 整包](../../../build/mcu_pxe/handoff/update_20261110/update_rk_for_edk2_loader_h1.img) | 6484554 | `22f73920ca19a8e06c2c5eac78983f3f80bec061abfe69844b3f2fbed28b9baa` |

这两份是启动升级整包，不能当作运行中的 MCU 固件；串口同 IP 验证仍需配套 H1 `b1_handoff.elf`。本轮未在 RKDevTool 中执行升级，不承诺升级动作会保留盘上其他分区数据，也未验证这两套 H1 的实板启动和 IP 交接。

## RKDevTool 的 SPI NOR 包

用户随后要求RK侧W25版本，并明确使用RKDevTool经USB写SPI NOR。新增 [update_spinor_rk_for_edk2_h1.img](../../../build/mcu_pxe/handoff/spinor_20261110/update_spinor_rk_for_edk2_h1.img)，6484554字节，SHA256 `02c90f28612d20e4622d2f1109b732a35938c2f59b80693c479d59ec51979330`。

前级使用本次RK提供的自动进入EDK Loader，后级仍为同一H1 FIT。针对U4007的32 MiB容量，参数仅保留机器字段和`0x00002000@0x00004000(uboot)`，不继承eMMC的Linux分区表。该位置参考只读获取的SPL MTD实现：优先读取`uboot`分区，缺省偏移为0x4000扇区；本包FIT范围为NOR的8至12 MiB。服务器当前源码不是所提供Loader的确切源码证明；实际FlashBoot中可见SPI NOR、SFC和W25Q256JW信息，但实板引脚、冷启动及eMMC共存行为仍待验证。

构建入口为`package_spinor.py`。服务器仅在新建的`codex-h1-spinor-20261110-*`目录生成RKAF，原工程和工具未修改；Windows RKImageMaker 2.23使用`-storage:SPINOR`生成最终RKFW。双层厂商解包和独立解析均通过；本轮未连接板卡或烧录。使用步骤与完整校验见[烧录说明](../../../build/mcu_pxe/handoff/spinor_20261110/烧录说明.md)、[校验报告](../../../build/mcu_pxe/handoff/spinor_20261110/verification.json)及[独立校验](../../../build/mcu_pxe/handoff/spinor_20261110/independent-verification.json)。MCU启动模式改选`fspiM1`，继续配套H1 MCU。

校验时修正了共用包头解析器的存储标志偏移：RKImageMaker 2.23写在头部偏移98的末4字节，而不是偏移53。相同Loader/内包分别封装的eMMC值为2、SPINOR值为512；错误介质包被拒绝。此前eMMC成品内容没有改变，只校正其JSON报告中的该解析字段。
