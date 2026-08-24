# W25Q256 空间与记录格式

- 状态：v1 基础布局已实现，产品字段仍可通过提升 schema 版本扩展
- 容量：W25Q256JV，256 Mbit = 32 MiB
- 擦除单位：4 KiB 扇区；写入单位：最多 256 B 页
- 当前范围：版本信息、FRU、构建清单和自检隔离
- 明确不包含：OTA 下载、镜像切换、内部 Flash 搬移和回滚

## 1. 空间分配

| 起始地址 | 结束地址 | 大小 | 用途 | 当前状态 |
|---:|---:|---:|---|---|
| 0x00000000 | 0x0000FFFF | 64 KiB | 关键元数据 | 已冻结 |
| 0x00010000 | 0x0020FFFF | 2 MiB | OTA 镜像槽 A | 仅预留 |
| 0x00210000 | 0x0040FFFF | 2 MiB | OTA 镜像槽 B | 仅预留 |
| 0x00410000 | 0x01FEFFFF | 27.875 MiB | 日志、标定或其他数据 | 仅保留，未分配 |
| 0x01FF0000 | 0x01FFFFFF | 64 KiB | 开发/产测自检区 | 已冻结，可破坏 |

两个 OTA 槽各为 2 MiB，大于当前 STM32 内部 Flash App 最大 1920 KiB。槽位只保留地址，当前代码不会写入，也没有升级状态机。未确定用途的 27.875 MiB 保持整块空白，避免日志、采样和标定需求尚未明确时提前切碎空间。

末尾 64 KiB 是唯一允许自检任务反复擦写的区域。独立自检镜像临时把版本/FRU四个副本映射到前16 KiB，使用0x01FFD000保存复位标记，并用最后8 KiB做原始读写；正式构建仍使用0x00000000起始的元数据地址。不得再通过“容量减若干扇区”暗示测试地址。

## 2. 元数据区

| 地址 | 大小 | 内容 |
|---:|---:|---|
| 0x00000000 | 4 KiB | 固件版本副本 A |
| 0x00001000 | 4 KiB | 固件版本副本 B |
| 0x00002000 | 4 KiB | FRU 副本 A |
| 0x00003000 | 4 KiB | FRU 副本 B |
| 0x00004000 | 48 KiB | 后续元数据扩展，当前不得占用 |

版本和 FRU 各使用两个物理扇区，不共享擦除单元。更新时选择较旧或无效的副本写入，读取校验通过后才把它视为新版本；原有效副本不会在同一次更新中被擦除。

## 3. 公共记录头

记录头固定 32 字节，STM32 和离线工具统一按小端存储。

| 偏移 | 大小 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | magic | 内存字节为 ASCII B1RC |
| 4 | 2 | format_version | 当前为 1 |
| 6 | 2 | record_type | 1=固件版本，2=FRU |
| 8 | 4 | generation | 每次成功更新加 1，支持 UINT32_MAX→0 回绕 |
| 12 | 4 | payload_length | 当前类型的固定载荷长度 |
| 16 | 4 | payload_crc32 | IEEE CRC-32 |
| 20 | 4 | header_crc32 | 覆盖头部偏移 0～19 |
| 24 | 8 | reserved | 写 0xFF，后续扩展 |

读取顺序为：校验头格式和头 CRC → 读取载荷 → 校验载荷 CRC → 比较 generation。若新副本在擦写中断电，其头或载荷 CRC 无效，系统继续使用旧副本。

## 4. 固件版本 v1

固件版本载荷固定 96 字节，包括：

- major.minor.patch 语义版本；
- 构建编号、UTC 构建时间；
- 12 位 Git 提交号和工作区 dirty 标志；
- 构建类型、目标板；
- 裸镜像长度和 IEEE CRC-32。

运行固件内的 firmware_version_current() 提供编译期身份。镜像长度和 CRC 存在自引用问题，因此内建结构中保持 0 且不置 IMAGE_VALID；链接完成后，构建系统生成：

- build_debug/test.bin：裸 App 镜像；
- build_debug/test.manifest.json：可读构建清单；
- build_debug/test.version.bin：与 C 结构完全一致的 96 字节版本载荷。

以后生产工具或 OTA 流程可以把 test.version.bin 交给 firmware_version_save()，不需要修改记录格式。
项目规定通过 tools/dev.ps1 build/rebuild 构建；脚本每次先快速刷新 CMake，
确保 UTC 时间和 Git 状态属于本次镜像，而不是首次配置目录的旧值。

## 5. FRU v1

FRU 固定 192 字节，当前定义通用基础字段：

- 产品名称；
- 产品料号；
- 板卡序列号；
- 硬件版本；
- 生产日期（建议 YYYY-MM-DD）；
- 可选 6 字节 MAC 及有效标志；
- 网络模式（未配置/静态/DHCP）；
- 配置 IPv4、子网掩码、网关和 TCP 服务端口；
- 最长 15 字节站点标签，例如 B1-03；
- 22 字节保留区。

字符串是带 NUL 结尾的 UTF-8/ASCII 定长数组。字段尚未经过产品/生产最终评审，因此现阶段只冻结偏移，不自动写入默认内容；扩展优先使用保留区，无法兼容时提升 schema_version，不得悄悄改变 v1 偏移。

FRU中的地址表示“期望的持久化配置”，不能冒充运行时地址。后续PC本地管理响应应至少同时返回：lwIP当前IPv4、服务端口、实际MAC、FRU序列号和site_label。这样静态地址、DHCP租约或板卡更换时，PC都能定位到同一逻辑设备。

## 6. 当前验证边界

- ARM 全量构建已通过，结构尺寸由 C11 _Static_assert 固定。
- 离线测试验证 32 MiB 分区连续无重叠、CRC 向量、版本载荷尺寸、generation 回绕。
- FRU 更新按每个可能的字节中断点模拟掉电，均恢复为旧完整记录或新完整记录。
- 2026-08-24 在阿波罗 V2 板载 W25Q256 实测通过：JEDEC `EF 40 19`、QE=1、32 MiB容量、擦除态、参数边界、跨页写、扇区RMW、0x01FFE000四字节寻址及24位别名不变。
- 版本和FRU均在隔离槽完成A/B generation选择、主动破坏最新副本CRC后回退旧副本；FRU实测包含`192.168.1.201:8888`网络定位字段。
- 自检写入标记后执行`NVIC_SystemReset()`，第二轮启动成功读到标记，证明数据跨MCU复位保持；SWD最终诊断字为`0x600D600D`、测试位图为`0x1F`。
- 本次只验证受控软件复位保持，不等价于生产条件下任意时刻断电的电气测试，也不构成Flash寿命结论。

复现命令：

```powershell
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 flash-self-test-build
python -X utf8 tests/capture_flash_self_test.py --port COM3 --timeout 120
```

烧录`build_flash_self_test/boot.elf`和`test.elf`。本次Apollo V2的启动选择使普通复位进入STM32系统BootROM，因此实测由ST-Link执行`-g 0x08000000`进入内部Flash；第一轮软件复位后再次执行同一命令完成第二轮。这是开发板启动配置问题，不是W25Q256测试步骤的一部分。

规范源文件是 BSP/Flash/flash_layout.h；本文用于解释，不得与头文件另立一套数值。
