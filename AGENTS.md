# Bootloader 工程接手说明

> 2026-09-16：V2完整B安装A1的0.1.3、TCP升级A2的0.1.4及健康/全镜像回读通过，FRU和稳定区不变。B1专用B及App已适配，只有构建/离线验证；实板3+3、掉电/ECC仍待验收。证据见[网络记录](../test/doc/ota_network_validation_20260916.md)和[B1同步](../b1/docs/ota_sync_20260916.md)。

本工作树位于 `D:\pro\bootloader`，分支为 `bootloader`，只维护 STM32H743 的 B 区。
App-V2 位于 `../test`（分支 `app-v2`），App-B1 位于 `../b1`（分支 `app-b1`）。
不要把 App、FreeRTOS、LwIP、PC/RK协议、DDR测试业务或 App 自检复制回本目录。

## 构建与入口

- 原 GNU Arm / CMake / Ninja 工具链不变。
- B1用`tools/dev.ps1 build -Configuration Release -Board b1`；B可信板型固定b1-h743，复位入口在RAM初始化前关闭PC6/PC7/PG11。V2包与B1包不能混刷；QSPI同为PB2/PB6、PF6–PF9，B使用HSI不依赖HSE。
- `powershell -ExecutionPolicy Bypass -File tools/dev.ps1 build`：默认正式C启动/日志/安装B，仅构建不烧录。
- `powershell -ExecutionPolicy Bypass -File tools/dev.ps1 build -ProbeSlot A2`：固定A2跳转实验B。
- 产物在 `build/Debug/` 或 `build/Debug-A2/`，名称为 `boot.elf`、`boot.bin`。
- `boot/startup.s`和`boot/boot_main.c`为正式入口，`boot/STM32H743_ota.ld`将B限制在0x08000000的128KiB区域；`boot.s`仅用于显式固定槽探针。
- App的地址修正验证在 `../test/tests/verify_ota_relocation.py`；在App工程运行 `tools/dev.ps1 ota-probe`。
- `tools/dev.ps1 ota-test`：在已安装QEMU中运行实际C安装核心，Flash用RAM模拟。
- `tools/dev.ps1 ota-pack -AppBuild <A1实验构建目录>`：生成一个App的`test.ota`；包格式见`doc/ota_package_v1.md`。
- 两App的`tools/first_flash.ps1`固定各自板型；`deploy`用于VS Code非默认空白板首烧，先确认版本/24位探针序列号/空闲/全空，再构建并只烧本次包，失败不选旧产物。`build`仍可生成独立交付ZIP，包内`flash.ps1`不需源码。备份后核对全空，B/A1/W25恢复包回读通过才提交初始日志，无force，不替代日常OTA。V2可显式模拟及MATS假FAIL，B1禁止且固定UART4；新增首烧编排和四线SWD仍待实板验收，见README。
- 两App的日常ST-Link快捷键共用`../test/tools/ota_dev.py`，从W25已确认版本自动patch+1；只用于已部署完整OTA的板。按板型构建RAM助手，捕获复位入口后一次加载，`board_ota.py --reuse-agent`接续各步而不重复复位/load。保留旧首烧/维护入口默认行为；修改的是主机脚本，不是B固件，新编排仍待真板验收。详见`../test/doc/ota_daily_download.md`。

## 状态与约束

当前默认boot.bin链接安装、持久化日志、健康状态及真实QSPI/内部Flash端口；App持续健康30秒后确认。
3+3及稳定恢复已有实际C离线验证，不能据此声称同Bank写入、真实掉电/ECC及恢复均已实板验收。
`mark_installing`必须由可靠日志实现，禁止接一个假成功回调后就开启真板擦写。
既有入口检查不能当作完整固件安全校验。详细证据与待办见[任务清单](doc/driver_tasks.md)。

- 自有代码、汇编和脚本注释统一使用中文，说明用途、参数/返回值及关键约束；第三方版权/许可保留原文。
- B安装器后续必须校验板型、长度、地址、版本、CRC及修正表；不靠扫描“像地址”的数据重定位。
- W25Q256信息记录是App向PC返回固件/FRU信息的唯一运行时来源；B写入接管前不能删除App兼容初始化。
- 保留版本/FRU断电保护；不改芯片保护、选项字节或Bank Swap，不新增第二个B。
- 硬件测试前备份原内部Flash；诊断地址从对应ELF获取。离线模型、槽位启动、完整OTA验收分别记录。
- 未明确授权不得烧录、提交或推送。清理/暂存只处理本任务内容，保留其他工作树及未提交修改。
