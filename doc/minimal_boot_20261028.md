# 精简Boot恢复与未焊W25首板适配

日期：2026-10-28。范围为V2与同级独立B1工程，依据用户确认恢复Boot；本轮只修改代码、内部文档、构建及离线验证，未连接ST-Link、串口或TCP，未烧录、提交或推送。

## 1. 最终行为

- 内部Flash首128KiB为Boot（0x08000000）；单App从0x08020000开始，容量1920KiB。无OTA、A1/A2、回滚、稳定版或固件搬运。
- Boot自带启动汇编、最小QSPI访问及FRU准备；不初始化业务外设，不启动RTOS。B1在RAM初始化前关闭PC6/PC7/PG11，App仍保留自身早期关断。
- `BOARD_W25_PRESENT=1`时Boot初始化W25、首次写入/迁移FRU并回读，再检查App向量和跳转。正常App只读FRU，信息查询不再写入；写/擦回调为空，底层写使能拒绝。
- 当前B1未焊W25，默认`BOARD_W25_PRESENT=0`，Boot及App均跳过器件。0102用宏定义返回JMCT-4000、D00、RK3588_test，未知序列号及未分配站点留空。V2仍默认有W25。
- W25版本区、旧包/启动记录和RK两块8MiB空间均不访问。Boot写权限仅限FRU两个扇区；隔离Flash自检编译时Boot也使用末尾64KiB内的映射。
- Ctrl+Shift+B仍自动编译并下载一个合并文件；工厂GUI可下载完整ELF或BIN（BIN地址0x08000000）。新脚本拒绝单独Boot、旧直启App、旧槽位文件及清单不符的镜像。

## 2. 代码入口

| 位置（两板各自包含） | 用途 |
|---|---|
| `Core/Inc/board_storage_config.h` | 是否装W25、测试仪默认信息、隔离自检写权限 |
| `boot/startup.S`、`boot_main.c` | 最早期关断、启动流程、装配分支、诊断和跳转 |
| `boot/boot_port.c` | 复位HSI 64MHz下的QSPI、FRU写范围、跳转前清理 |
| `boot/boot_fru.c` | 当前FRU读取、旧名称/格式迁移、首次初始化与回读 |
| `boot/boot_vectors.h` | App栈范围、8字节对齐、Thumb入口及App边界检查 |
| `boot/minimal_boot.cmake`、`minimal_boot.ld` | 独立构建Boot；V2/B1配置均随工程编译 |
| `tools/embed_boot.py`、主链接脚本 | 将Boot作为固定地址段并入最终ELF/BIN |
| `Core/Src/system_stm32h7xx.c` | App VTOR使用实际`g_pfnVectors`，避免重新指回Boot |
| `App/protocol/pc/pc_tester_info.c` | 有W25只读，未焊配置使用常量；不增加PC字段 |
| `BSP/Flash/flash_bsp.c`、`HAL_Drv/qspi_hw.c` | 正式App禁止写擦，未焊配置拒绝器件访问 |
| `tools/direct_flash.py` | 合并镜像检查，保留先备份、写入、回读、最后复位的顺序 |

无需同级旧`D:/pro/bootloader`参与构建。该旧工程未修改。

## 3. FRU与故障处理

有W25时保留既有双扇区/CRC/generation规则。空白或损坏介质初始化默认信息；已知160/192字节v1转换为v2，保留已有身份和网络字段；有效未知格式及访问失败不覆盖。当前有效v2不重复写入，旧默认测试仪名称只迁移一次。写较旧或无效副本，回读后才能继续。

Boot不校验完整App CRC或签名，仅检查启动向量；电脑端清单及下载回读用于确认本次写入。它不具备断电安装恢复或固件回滚能力。

Boot错误停在本阶段，B1保持早期关断；`boot_diag`仅在Boot阶段有意义，进入App后该RAM会被重新使用：

| 下标 | 含义 |
|---|---|
| 0 | 0x424F4F54标识 |
| 1 | 1进入、2 FRU有效、3跳转；E1器件错误、E2 FRU错误、E3 App向量错误 |
| 2 | 是否装W25（0/1） |
| 3 | 读取到的JEDEC ID；未焊分支为0 |

B1补焊后将本工程头文件中的默认值改为1，重新编译和烧录完整文件。不能只替换App，也不能把未焊配置的常量响应当作FRU已写入证明。

## 4. 已完成验证

| 检查 | 结果与边界 |
|---|---|
| 两板完整构建 | 通过；V2保留电源模拟/MATS第一颗假FAIL，B1为无W25真实业务配置 |
| 最终镜像布局与清单 | 两板通过；合并Boot字节与独立Boot完全一致，App向量0x08020000；ELF/BIN范围、板型及CRC一致 |
| App中断表及Boot跳转反汇编 | 已核对两板SystemInit使用0x08020000；换MSP后仅执行屏障、开中断及bx，不再使用C栈 |
| B1无W25 | 实际C查询测试通过，无Flash锁/读写；默认Boot符号中不含器件初始化、FRU准备或NOR读取 |
| 有W25 FRU | V2及B1实际C在QEMU各覆盖1280个写入中断点；已知迁移、未改变免写、未知格式/IO保护及只读查询通过；仅模拟介质，不等价于电气断电实验 |
| 补充Boot配置 | B1补焊W25配置、V2隔离Flash自检配置均独立编译通过；未烧录 |
| 下载工具 | 11项离线检查通过：板型/容量、合并向量、备份顺序、写入/回读失败不启动等；探针调用均为替身 |
| PC与B1入口 | PC协议24项检查通过；B1 49项引脚及启动/板级静态合同通过 |

本轮最终合并BIN：V2 382260字节（CRC32 56E3D046），B1 373444字节（CRC32 86712E4B）；均为本地构建0.1.0，不代表安装版本。后续重新构建以同次`.manifest.json`为准。

复跑主要入口：

```powershell
python tests/verify_pc_tester_info.py
python tests/verify_pc_tester_info.py --app-root ../b1
python tests/verify_pc_tester_info.py --app-root ../b1 --without-w25
python -m unittest discover -s tests -p test_direct_flash.py
python tests/verify_pc_protocol.py
python ../b1/tests/verify_project.py
python tools/direct_flash.py --image build/standalone/Debug/test.bin --target apollo-v2-h743 --check-only
```

## 5. 仍待实板验收

1. V2实际下载后Boot跳转、复位、看门狗、网络及只读FRU查询。
2. 无W25 B1首次启动、MCU日志/正常上电灯、0102宏定义响应及耦合复位关断。
3. B1补焊后核对器件ID、首次写入和再次启动免写、真实断电保护；同时核对QSPI电气与装配。

本轮未改PC/RK/B2线上字段，未修改Word或飞书；用法见[一键烧录](direct_flash.md)。
