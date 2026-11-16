# D4 前级 Loader 恢复记录

2026-11-04。用户要求从已有记录找回曾正常进入 EDK2 的前级。已找到SPL版本标记一致的源整包，原样提取Loader并配齐D4；用户随后反馈D4菜单、SNP过滤Success及IPv4/PXE项，确认已进入D4。文件提取并非本机代为烧录，也未取得eMMC完整回读。

## 证据

1. 成功进入 D3 的用户日志：`C:/Users/Administrator/.codex/attachments/9d33df1e-6c6d-4bff-af93-0955213bb756/已粘贴的文本.txt`。出现 `U-Boot SPL 2017.09-241224-dirty #zhuoyi (Aug 19 2026 - 14:28:44)`，MMC1 从 `0x4000` 加载 FIT、进入 ATF/EDK2，随后 UART1 HELLO 成功。
2. `E:/MYZR-RK3588/发布包-烧录UEFI/02-烧录工具/RKDevTool_Release_v3.15/Log/Log2026-11-03.txt`：17:11 重写 eMMC IDB 和 FUNC 整包；18:02、18:05 只写 D4 到 `0x4000`。11 月 4 日日志 09:11 再次只记录 D4 到该地址。用户报告已擦除 eMMC，但这些记录不证明擦除的覆盖范围，不据此否定用户操作。
3. FUNC 整包 `build/mcu_pxe/artifacts/uart1.spl.value.test.img` 的 Loader CRC 正确；解码 FlashData 内能找到 `GICv3_Demo()`、`Enable rockchip debugger`、`my_count=0x%lx`，与当前冷启动日志一致。D4 FIT 不包含这个前级；仅替换 uboot 分区不会替换已有 IDB。这是当前优先恢复前级的依据，不能仅凭日志确定实际启动介质。
4. 桌面 `update-marchcx-mats-delta-complete.img` 的 Loader 内包含与第 1 项相同的 SPL 版本和 `Chenduanhai Trying fit image at 0x%lx sector`。该包中没有上述 FUNC 演示标记。未保留此前板上 IDB 的原始回读，因此这里只确认版本标记一致，不能声称与原先板上内容逐字节一致。
5. 几份名为 `rk3588_spl_loader_v1.18.113*.bin` 的文件含 2023 年 `#lxh` SPL，不能因文件名/复制时间相近就视作本板配套版本；本次未选用。

## 提取与交付

目录：`D:/pro/test/build/mcu_pxe/recovery_loader_d4_20261104/`。

| 项目 | 值 |
|---|---|
| 源整包 | `C:/Users/Administrator/Desktop/update-marchcx-mats-delta-complete.img` |
| 源整包大小 | 999610954 字节 |
| 源整包 SHA256 | `3d0d85d4f71c659210dbf23e64065e7d43c4d8a761bcb43c54a807cdfd9ef891` |
| RKFW 内 Loader 起始偏移 | `0x66`，102 字节 |
| Loader 长度 | 502208 字节 |
| 提取文件 | `b1_zhuoyi_20260819_loader.bin` |
| Loader SHA256 | `16efb11d4ed14c700749cdc1e95f03a71477aeff89378446f625016c9237d2b4` |
| Loader CRC | 提取前后均通过；原始字节不变 |
| 配对 FIT | `b1_serial_net_D4_network_20261103.img`，4194304 字节 |
| FIT SHA256 | `48202c6616521aac267de44e12fa259704597eb8192d4c5fe9b4013f6c488ae8` |
| FIT 写入范围 | 本板已核对的 eMMC uboot：LBA `0x4000`，`0x2000` 个 512 字节扇区 |

已重新核验复制后的 FIT 散列、FDT 标记和两个 2MiB 槽完全相同。操作说明在交付目录的 `恢复步骤.md`，详细组件标记在 `verification.json`。分析脚本及多包对照在 `build/mcu_pxe/inspect_loader_20261104.py`、`loader_inspection_20261104.json`；生成脚本在 `prepare_recovery_20261104.py`。

## 写入方式与验收

RKDevTool 自带手册第 3 页区分两种操作：`LoaderToDDR` 临时下载到 RAM；`Loader` 生成 IDBlock 并写设备存储。应在“下载镜像”页选 Loader 写入上面的 `.bin`，并写 uboot 的 D4；不能把 `.bin` 作为普通文件按任意地址写入，也不能把 D4 FIT 送入“升级固件”页。

冷启动先核对匹配的 SPL、`0x4000` FIT 加载和 D4 版本，再核对 HELLO/过滤成功。若仍运行 FUNC 演示，继续检查实际启动来源及 Boot 电压；11 月 3 日 11:41 有 NOR 写入记录，不在未核实时自动擦除 NOR。当前不重编固件、不接 ST-Link/串口、不改电源、不清理 eMMC/NOR，也未提交或推送本轮文件。

PXE后续：11月4日TFTP已恢复，本机普通DHCP仍关闭；MAC白名单代理配合现有网络地址服务器，RK已请求下载。网络与下载证据见[D4网络记录](network_d4_20261103.md#2026-11-04-实板网络证据)。恢复EDK2不代表MemTest86已运行。

格式核对依据为 Rockchip 官方 [RKBoot.h](https://github.com/rockchip-linux/rkdeveloptool/blob/master/RKBoot.h)、[CRC 实现](https://github.com/rockchip-linux/rkdeveloptool/blob/master/crc.cpp)及 [Loader/IDB 处理](https://github.com/rockchip-linux/rkdeveloptool/blob/master/main.cpp)。本轮 REA 无法接受 Windows 本机路径，实际验证使用本地脚本按官方格式完成，没有将 REA 工具失败当作固件异常。
