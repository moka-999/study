# W25Q256 空间与记录格式

- 状态：版本/FRU及MCU OTA沿用现有布局；2026-09-24新增两块各8MiB的RK固件预留区
- 容量：W25Q256JV，256 Mbit = 32 MiB
- 擦除单位：4 KiB 扇区；写入单位：最多 256 B 页
- 本页规定地址分配及现有版本/FRU记录；MCU安装和回滚见[OTA设计](ota_design.md)
- RK仅分配地址和容量，不定义结构体、包头、版本记录或下载/安装过程

## 1. 空间分配

| 起始地址 | 结束地址 | 大小 | 用途 | 当前状态 |
|---:|---:|---:|---|---|
| 0x00000000 | 0x0000FFFF | 64 KiB | 关键元数据 | 已冻结 |
| 0x00010000 | 0x0020FFFF | 2 MiB | MCU OTA下载包 | 已使用，地址不变 |
| 0x00210000 | 0x0040FFFF | 2 MiB | MCU稳定版包 | 仅授权维护/量产写入 |
| 0x00410000 | 0x00C0FFFF | 8 MiB | RK固件空间A | 仅预留，不写入 |
| 0x00C10000 | 0x0140FFFF | 8 MiB | RK固件空间B | 仅预留，不写入 |
| 0x01410000 | 0x01FEFFFF | 11.875 MiB | 日志、标定或其他数据 | 尚未分配 |
| 0x01FF0000 | 0x01FFFFFF | 64 KiB | 开发/产测自检区 | 已冻结，可破坏 |

MCU下载区和稳定区各2MiB，实际App镜像上限为896KiB；外部两个存放区不是内部A1/A2。RK每区大小为0x00800000字节（8×1024×1024），从原业务预留区划出，不移动已有数据、不主动擦除或初始化。A/B仅是两个独立空间的名称，不代表已实现RK双槽切换/回滚，也不先固定SPL与EDK的分配。余下11.875MiB仍未分配。

末尾 64 KiB 是唯一允许自检任务反复擦写的区域。独立自检镜像临时把版本/FRU四个副本映射到前16 KiB，使用0x01FFD000保存复位标记，并用最后8 KiB做原始读写；正式构建仍使用0x00000000起始的元数据地址。不得再通过“容量减若干扇区”暗示测试地址。

## 2. 元数据区

| 地址 | 大小 | 内容 |
|---:|---:|---|
| 0x00000000 | 4 KiB | 固件版本副本 A |
| 0x00001000 | 4 KiB | 固件版本副本 B |
| 0x00002000 | 4 KiB | FRU 副本 A |
| 0x00003000 | 4 KiB | FRU 副本 B |
| 0x00004000 | 8 KiB | MCU启动管理日志，两独立扇区 |
| 0x00006000 | 40 KiB | 后续元数据扩展，当前不得占用 |

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

固件版本载荷固定76字节，只包括：

- `schema_version`和`struct_size`；
- `major.minor.patch`语义版本；
- 裸镜像长度和IEEE CRC-32；
- UTC构建时间；
- 构建类型和目标板。

不保存构建编号、Git提交号、dirty标志或其他扩展身份。运行固件内的
`firmware_version_current()`提供编译期身份。镜像长度和CRC存在自引用问题，
因此内建结构中保持0；链接完成后，构建系统生成：

- build_debug/test.bin：裸 App 镜像；
- build_debug/test.manifest.json：可读构建清单；
- build_debug/test.version.bin：与C结构完全一致的76字节版本载荷。

以后生产工具或 OTA 流程可以把 test.version.bin 交给 firmware_version_save()，不需要修改记录格式。
项目规定通过 tools/dev.ps1 build/rebuild 构建；脚本每次先快速刷新 CMake，
确保 UTC 时间和 Git 状态属于本次镜像，而不是首次配置目录的旧值。

## 5. FRU v2

FRU固定224字节，定义以下字段：

- 产品名称；
- 板卡序列号；
- 硬件版本；
- 生产日期（建议YYYY-MM-DD）；
- 6字节MAC（全0表示未配置）；
- 网络模式（未配置/静态/DHCP）；
- 配置 IPv4、子网掩码、网关和 TCP 服务端口；
- 最长15字节站点标签，例如`B1-03`；
- 测试仪名称`tester_name`；
- 测试板名称`test_board`；
- 22字节保留区。

字符串是带NUL结尾的UTF-8/ASCII定长数组。v2只在v1的
`site_label`后追加两个32字节字段，不改变旧字段偏移。以后扩展优先
使用保留区，无法兼容时提升`schema_version`。

PC首次查询测试仪信息时，若W25Q256中没有有效v2 FRU，固件会写入
A/B两个副本。一旦存在有效v2，每次0x0101请求只重新从W25Q256读取，
不缓存上次信息，也不用编译期常量覆盖。空白或旧格式初始化值为：

| FRU字段 | 当前值 | PC查询映射 |
|---|---|---|
| `tester_name` | `LPDDR4/X_200B` | `testerName` |
| `product_name` | `LPDDR4/X_200B` | 产品名称，与当前测试仪名称一致 |
| `hardware_revision` | `D00` | `testerVersion` |
| `test_board` | `RK3588_test` | `testBoard` |

初始写入后会重新从W25Q256读取并校验，TCP响应始终使用实际持久化数据。
旧v1记录因`schema_version/struct_size`不符合v2而不会被误读。首次查询
会显式读取160字节v1，保留序列号、MAC、网络配置、生产日期和站点
标签，再补齐上述四个值并写成v2 A/B副本。迁移首次写入优先覆盖
较旧v1副本，使最新v1在新v2校验通过前仍可回退。

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
