# STM32H743 Bootloader（B区）

本目录只负责B区。PC通信、固件下载和测试业务留在App工程。
默认`boot.bin`已接入真实QSPI/内部Flash、可靠启动日志、安装及回退逻辑。
V2完成真实TCP升级和镜像回读；B1完成适配/构建/离线验证，未进行B1实板测试。

```text
boot/       复位入口、B区链接脚本和OTA安装核心
cmake/      原GNU Arm工具链配置
tools/      独立构建、产物检查和单App升级包工具
tests/      仅OTA核心的离线C故障注入，不是可烧录固件
doc/        OTA状态和实验记录
```

## 构建

```powershell
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 build
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 build -Configuration Release
# B1专用B：固定b1-h743板型，复位先关PC6/PC7/PG11：
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 build -Configuration Release -Board b1
# 仅固定跳A2的实验B，不是自动切槽：
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 build -ProbeSlot A2
# 执行C安装核心，Flash用RAM模拟；可选真实A1构建作为安装输入：
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 ota-test -AppBuild ../test/build/ota-probe-debug-a1
# 只打包已有验证产物，输出仍留在App构建目录的test.ota：
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 ota-pack -AppBuild ../test/build/ota-probe-debug-a1
```

默认产物：`build/Debug/boot.elf` 与 `boot.bin`，烧录地址0x08000000。
构建命令不连接烧录器；`-ProbeSlot A2`产物在`build/Debug-A2/`，只用于配套启动实验。

App单镜像转换在`../test`内运行`tools/dev.ps1 ota-probe`；
独立链接A2只作比对答案，实板候选为A1实验目录的`a2_from_a1.bin`。
代码更新后的App不自动继承旧基线的真板通过结论。

普通构建的`boot.bin`已链接`libota_core.a`。`ota-test`沿用原GNU Arm，
使用本机已有QEMU（默认`D:\qemu\qemu-system-arm.exe`，可用`-Qemu`覆盖）；
测试镜像`build/Debug-ota-test/ota_core_test.elf`只适用于MPS2仿真，严禁烧录到STM32。
包格式和模块安全边界见[升级包v1](doc/ota_package_v1.md)。

当前进度见[Boot任务](doc/driver_tasks.md)，硬件证据见[启动实验](doc/ota_relocation_probe.md)。

B1产物位于`build/Release-b1/`；B始终用复位HSI，QSPI使用两板共同的PB2/PB6、PF6–PF9。
打B1包用`ota-pack -Board b1 -AppBuild ../b1/build/ota-app-0.1.1-release-a1`。
首次部署使用下述入口，备份并创建匹配本板的初始日志/恢复包；版本记录由B同步，空白日志仍会停留维护。
V2固定槽探针不适用于B1，构建会拒绝该组合。详见[B1同步记录](../b1/docs/ota_sync_20260916.md)。

## 空白板首次部署（V2 / B1）

构建与烧写分离：开发者构建一次，整个输出目录交给同事；同事不需要源码或重新编译。
首烧工具是SWD离线维护工具，不接TCP/RK，不修改业务协议、选项字节或Bank Swap。

```powershell
# V2：在D:\pro\test执行。省略-PowerSimulation则为非模拟配置。
powershell -ExecutionPolicy Bypass -File tools/first_flash.ps1 -Action build -Version 0.1.23 -PowerSimulation

# B1：在D:\pro\b1执行，按实际RK接线选择-RkUart 4或2，不允许电源模拟。
powershell -ExecutionPolicy Bypass -File tools/first_flash.ps1 -Action build -Version 0.1.23 -RkUart 4
```

产物在各App的`build/first-flash/<板型>-<版本>-<配置>-<时间>/`，同时生成同名ZIP，方便直接转交同事解压。包括完整B、开启OTA的A1、RAM维护助手、单App的`.ota`、初始记录、恢复包、SHA256清单及烧写脚本。A2首次留空。恢复源默认采用这次A1包；这不等于该版本已经完成实板稳定性验收。

把整个目录复制到同事电脑，例如`D:\mcu_first\v2`，先核对标签、板型和包内`README.txt`。文件路径不要含空格或引号。

```powershell
# 在交付目录内执行；这里只读文件，不接烧录器。
powershell -ExecutionPolicy Bypass -File .\flash.ps1 -Action verify

# 确认空板、RK/温控空闲和允许复位后，替换实际ST-Link序列号；B1改-Board b1。
powershell -ExecutionPolicy Bypass -File .\flash.ps1 -Board apollo-v2 -Serial <24位ST-Link序列号> -ConfirmIdle YES -ConfirmBlank YES
```

同事电脑需要Python 3.10+、GNU Arm工具（GDB/objdump/objcopy）、ST-Link GDB Server、STM32CubeProgrammer及ST-Link驱动；烧写无需CMake/Ninja。脚本优先PATH与STM32Cube标准安装目录，也可用`-Python/-Compiler/-Server/-Programmer`指定实际exe。使用Programmer查看序列号时，查看后先断开它，再运行脚本，避免两个工具同时占用ST-Link。

### 实际执行顺序及限制

1. 核验板型、包中全部文件的SHA256、镜像/修正表CRC、ELF装载范围和bin一致性。散列是误拷贝检测，不是签名认证；PCB板型须由操作者确认，不能从MCU芯片ID自动识别。
2. 连接指定ST-Link，核对目标芯片标识、2MiB容量、RDP0及无Bank Swap；只读取，不自动改保护位。备份内部2MiB及W25前`0x410000`字节，W25双遍读取必须一致。
3. 内部Flash必须全FF，W25的版本区`0x0000–0x1FFF`、启动日志`0x4000–0x5FFF`和稳定区`0x210000–0x40FFFF`必须全FF。不满足就停止，保留备份，**没有force或自动全片擦除**。出厂自带例程的V2也可能被拒绝，需另行确认旧程序/数据的迁移或清空方案。
4. 写B到`0x08000000`、A1到`0x08020000`，回读完整内部Flash，确认A2及其余空间仍空。写W25稳定包并回读相关范围，FRU、下载区及其他非目标字节必须保持原样。
5. 最后由既有C接口编码、提交并回读启动记录；不把236字节RAM结构直接当作Flash记录烧写。复位后B同步W25固件版本，App持续健康30秒后清pending。180秒内未确认、异常槽或额外复位均报失败，不自动重刷。结束释放本工具启动的服务器，不终止别人的调试器。

每板独立备份和`report.json`存于交付目录`records/<时间>-<ST-Link序列号>/`。中断或失败后不要重复首烧、手工伪造确认或擦掉日志；保留备份供恢复分析。成功后同步版本走正常OTA，不再用首烧覆盖。

FRU不从开发者板卡复制；现有App仍在首次信息查询时初始化/迁移FRU，正常响应读W25持久化记录。首烧不修改网络策略，当前IP仍是`192.168.1.201`测试配置，多板同网需避免地址冲突；B1正式IP来源仍为待冻结接口的B2通知。

V2当前`board_mac_address()`仍返回固定`00:80:E1:00:00:00`，所以同一广播域暂时只接一块这种V2；要并行联调还需单独完善唯一MAC与IP配置，不能只改IP。此限制不影响离线逐块首烧，本轮未修改网络代码。

### 验证边界

2026-09-30：V2模拟/非模拟、B1 UART4/UART2的Release App双槽修正及完整B/RAM助手构建通过；包字段/CRC/散列检查、26组双板型Python成功/失败编排、日常OTA兼容回归通过。原C安装核心144项、启动日志及两板启动管理QEMU回归通过；两份交付ZIP在独立临时目录解压后，包内PowerShell校验入口通过，无源码依赖。新增统一首烧流程尚未在空白V2/B1实板执行；旧V2人工部署通过不替代新入口验收。离线复跑在V2目录执行`python -B -X utf8 tests/test_first_flash.py`，日常OTA兼容回归用`tests/test_ota_dev.py`。本轮没有连接烧录器或改变现有板卡固件。
