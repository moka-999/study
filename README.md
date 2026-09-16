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
首次部署必须备份并创建匹配本板的初始日志/版本/稳定包；空白日志会停留维护。
V2固定槽探针不适用于B1，构建会拒绝该组合。详见[B1同步记录](../b1/docs/ota_sync_20260916.md)。
