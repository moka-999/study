# B1 / V2 一键烧录

更新：2026-10-29。两板恢复精简Boot＋单App，最终仍是一个ELF/BIN，一次下载。没有OTA、A1/A2、回滚、固件搬运或30秒健康确认。

## 1. 日常使用

VS Code打开对应工程，接好ST-Link和板卡电源，停止RK测试及温控、断开PC/网络助手，再按 **Ctrl+Shift+B**。自动完成编译、地址检查、内部Flash备份、写入、回读校验和复位。B1每次MCU复位都会关闭RK/DDR。

| 工程 | 默认快捷键配置 | 完整烧录文件 |
|---|---|---|
| V2：`D:/pro/test` | 真实RK；电源/测量模拟；MATS第一颗假FAIL；已装W25 | `build/standalone/Debug/test.elf`或`test.bin` |
| B1：`D:/pro/b1` | 真实驱动和结果；首板未焊W25，信息使用常量 | `build/standalone/Debug/b1.elf`或`b1.bin` |

每个工程独立构建，不依赖同级旧Bootloader工程。

```powershell
# 只编译，不连接设备
.\tools\dev.ps1 build

# B1一键下载；V2不带模拟参数则使用实际驱动配置
.\tools\dev.ps1 flash -ConfirmIdle YES

# V2日常联调，与默认快捷键一致
.\tools\dev.ps1 flash -ConfirmIdle YES -PowerSimulation -MatsFailTest
```

可选`-Configuration Release`、`-Version 0.1.40`、`-Serial <探针序列号>`；B1还支持`-LogSource MCU|RK`。版本不自动递增，构建清单的版本/CRC只用于电脑核对文件，不写入W25。

### 1.1 两块B1同时调试

在`D:/pro/b1`分别构建两份完整Boot＋App镜像：

```powershell
.\tools\dev.ps1 build -TestIpLastOctet 201
.\tools\dev.ps1 build -TestIpLastOctet 202
```

| 板卡 | 上电调试地址 | 完整ELF（相对B1工程） |
|---|---|---|
| 第一块B1 | `192.168.1.201:8888` | `build/standalone/Debug/b1_192.168.1.201.elf` |
| 第二块B1 | `192.168.1.202:8888` | `build/standalone/Debug-ip202/b1_192.168.1.202.elf` |

两份ELF分别烧入对应板卡。PC可使用`192.168.1.200`、掩码`255.255.255.0`，与两块板接入同一交换机；PC分别建立到两个地址的TCP连接，每块板仍只接受一个客户端。

`-TestIpLastOctet`允许1～254，端口仍为8888；选地址时避开PC及其他设备。未传参数和默认Ctrl+Shift+B仍使用201，其他地址使用独立构建目录。用脚本下载第二块板时执行`.\tools\dev.ps1 flash -TestIpLastOctet 202 -ConfirmIdle YES`，同样须先确保RK/温控空闲、PC断开。

MAC仍由各自STM32 UID派生。这个参数只修改B2首次配置前的本地地址；B2有效通知后仍按正式规则动态改IP，不写FRU、不增加协议字段。当前两份均为B1真实驱动、`BOARD_W25_PRESENT=0`，不带V2电源模拟或MATS假FAIL。

2026-10-29两份构建、Boot/App装载地址和实际IP指令检查通过，越界参数0/255在PowerShell和CMake入口均被拒绝；未烧录或双板实测。离线证据保存在`build/b1_dual_ip_20261029/verification.json`（相对V2工程）。

## 2. Boot与W25

| 内部Flash范围 | 大小 | 用途 |
|---|---:|---|
| `0x08000000–0x0801FFFF` | 128KiB | 精简Boot |
| `0x08020000–0x081FFFFF` | 1920KiB | 单App |

Boot依次执行：B1提前关闭RK/DDR → 按装配配置准备W25/FRU → 检查App栈与入口 → 跳转。Boot不启动RTOS或业务协议。

装配开关位于两个工程各自的`Core/Inc/board_storage_config.h`：

| `BOARD_W25_PRESENT` | 行为 |
|---|---|
| `0`（当前B1默认） | Boot及App跳过W25；0102从同文件宏定义返回`JMCT-4000 / D00 / RK3588_test`；未知序列号/站点留空 |
| `1`（V2默认） | Boot检查器件，初始化/迁移FRU并回读；有效信息不重复擦写。App只读FRU，查询不写Flash |

B1补焊W25后，把B1默认值改为1并重新构建、下载完整镜像。不要只更新App而遗漏Boot中的配置。已装配置出现器件或FRU错误时停在Boot；它不会偷偷改用宏定义。有效未知FRU格式不覆盖。

FRU仍位于`0x2000/0x3000`两个4KiB扇区。Boot仅写FRU和必要的器件配置；不读写固件版本、升级状态、旧包或RK两块8MiB预留空间。正式App不提供W25写/擦回调；隔离Flash自检仍仅使用末尾64KiB地址。

`0101→0102`的`testerVersion: "D00"`是硬件版本，不是MCU固件版本。恢复Boot没有增加PC字段，0901～0903仍回复`0006 failed/notSupported`。

## 3. 给工厂或同事

交付上述完整ELF或BIN及同次构建清单。STM32CubeProgrammer选择ST-LINK、SWD、950kHz；打开BIN时地址填写 **`0x08000000`**，勾选校验后下载并复位。ELF自身携带地址。不要拿`minimal-boot/boot.bin`单独交付，它只有Boot。

B1 J5：1=GND、2=SWCLK/PA14、3=SWDIO/PA13、4=3V3_MCU参考电压。板卡自行供电；不把烧录器电源输出当作参考输入，也不用RK复位代替NRST。J5四线可用软件复位方式下载，接线方向以实际插座和原图为准。

脚本首次连接复位并暂停MCU；备份、写入、回读期间Hotplug连接已暂停的内核，最后才复位运行。自动备份完整2MiB内部Flash，不修改Option Bytes、不全擦、不访问W25；按镜像覆盖的扇区擦写，末扇区尾部旧数据可能被清除。失败保留备份，不自动重试。GUI下载须另行备份，不具备脚本的自动流程。

## 4. 验证范围

本轮只构建与离线验证，未烧录。Boot只检查App向量，不提供运行期整镜像CRC或签名认证；下载工具验证文件清单和回读结果。当前代码、测试及硬件待办见[修改记录](minimal_boot_20261028.md)和[任务清单](driver_tasks.md#current-status)。旧OTA安装记录不代表这份镜像已经上板。
