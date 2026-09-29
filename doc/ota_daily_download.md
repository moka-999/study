# V2改代码后的编译与下载

适用：`D:\pro\test` 的 App-V2、已部署完整OTA Bootloader的阿波罗V2开发板。不适用于空片首次烧录或B1实板。Bootloader工程须保留在同级 `D:\pro\bootloader`。

> **2026-09-23实测提示**：SWD入口已完成内部Flash和W25双读备份，但本次连接/复位期间启动日志变为pending=1，因此请求生成器安全拒绝，未写升级包；不能把它标为“一键SWD安装已验收”。本轮改用现有TCP入口安装并健康确认成功。暂优先使用下文网络升级；不要删除pending检查或手改确认标志。SWD复位与健康确认衔接留待专门修复，证据见[RK联调记录](rk_protocol_migration_20260923.md)。

## 最常用：ST-Link下载，不需要网线

1. 保存修改。停止测试；若连接了B2，应另行协调停止温控。本工具不控制B2。开发板上电后先等待约40秒，让旧App完成健康确认。
2. 结束其他调试会话，关闭正在占用ST-Link的下载工具。在VS Code打开 `D:\pro\test` 文件夹。
3. 按 **Ctrl+Shift+B**，运行 **“⚡ OTA 安全下载（ST-Link，会复位）”**。
4. 输入新版本，例如 `0.1.9`；下次再改代码用 `0.1.10`。版本必须大于板上版本，不是改完代码还反复填同一版本。无需手改C代码里的版本宏。
5. ST-Link序列号默认是当前开发板所用的 `54FF6D067182505239431087`；更换烧录器后填实际序列号。确认已停止测试时选择 `YES`，否则选择 `NO` 取消。
6. 等待终端显示安装及30秒健康确认 `PASS`。备份与校验需要时间，中途不要断电或拔烧录器。退出时释放本工具自己启动的连接，不关闭你的其他调试进程。

自动流程：构建A1/A2槽限App并逐字节验证地址修正 → 生成一个 `.ota` → 检查V2电源输出为低 → 备份内部Flash 2MiB和W25前 `0x410000` 字节 → 把包写入W25下载区并完整回读 → 通过现有C接口提交升级请求 → 复位由B安装到另一个A槽 → 核对实际镜像、非目标Flash区域及健康确认。

**不需要修改链接地址、手写启动标志或自行更新CRC，也不需要每次重烧B。** Boot同步本次实际运行槽的固件信息到W25；App的信息查询继续从W25读取。FRU和稳定包不属于本入口的写入目标。

## 另外两个入口

在“终端 → 运行任务”中选择：

| 入口 | 用途 | 连接要求 |
|---|---|---|
| 🔨 OTA 编译打包（不下载） | 只检查代码和生成一个 `test.ota` | 不连接烧录器/开发板 |
| 🌐 OTA 网络升级（会复位） | 使用现有0901/0902/0903协议下载、安装、确认 | PC能访问板卡IP及8888端口；不用ST-Link |

网络升级前关闭网络调试助手、PC软件等现有连接；MCU只服务一个客户端。输入板卡IP，默认 `192.168.1.201`。App须正常运行并已健康确认、电源状态为OFF。网络方式不另外读取整片备份，依靠完整OTA的旧槽/日志保护；保留之前的本机首部署备份。

一次只运行一种下载方式，不要同时启动网络升级和SWD操作。旧的扩展“Download/Run”按钮不等于这里的任务，不能用它直接覆盖App。F5断点调试也不是本次入口；若后续只附加调试，应采用下载成功后打印的**实际槽ELF**，不要自动load一个固定地址的旧ELF。

## 命令行用法

在 `D:\pro\test` 终端执行，版本仅为示例：

```powershell
# 只生成升级包，不访问板卡。
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev.ps1 ota-build -Version 0.1.9

# ST-Link下载：操作者确认空闲且允许复位后，才加ConfirmIdle YES。
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev.ps1 ota-swd -Version 0.1.9 -Serial 54FF6D067182505239431087 -ConfirmIdle YES

# 网络升级，不占用ST-Link。
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev.ps1 ota-net -Version 0.1.9 -BoardIP 192.168.1.201 -ConfirmIdle YES
```

默认Debug，可加 `-Configuration Release`。构建目录为 `build/ota-app-<版本>-debug-a1/` 或 `-release-a1/`，其中 `test.ota` 是交给PC的唯一升级文件；另一个A2构建用于验证/调试符号，不需要发送两个App。每次两槽共用同一构建时间。

### 旧固件首次迁移到带目标前缀的协议

现行0901使用`mcu_begin/mcu_write/mcu_finish/mcu_query/mcu_cancel`。只有确认板上仍是旧无前缀动作的固件时，网络入口额外加`-LegacyOtaActions`；这不是跳过版本/空闲/CRC检查。该参数不适用于构建或SWD入口。

```powershell
# 仅供旧协议板卡首次迁移；版本必须高于实际板上版本。
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev.ps1 ota-net -Version 0.1.17 -BoardIP 192.168.1.201 -ConfirmIdle YES -LegacyOtaActions
```

此选项上传阶段使用旧动作；提交后重新连接先用`mcu_query`，仅显式旧版模式且收到notSupported时才回退只读`query`，以查询尚未重启的旧App或回滚结果。不自动回退或重发写入/提交。迁移成功后去掉该选项，后续使用默认新动作。Python直接入口对应`--legacy-ota-actions`；本次仅离线验证，没有据此烧录开发板。

每轮下载备份、GDB命令及网络报告保存在 `build/ota-dev/<时间戳>/`，不覆盖历史目录。现有SWD助手不支持路径含空格/引号；当前工程路径可用。`.ota` 是升级包，不要交给普通烧录器当内部Flash裸bin。

## 遇到拒绝/失败

- **版本不递增**：换更高版本重新构建，不关闭版本保护。
- **pending未清零/未确认**：旧App尚未健康，先等待；仍失败就查看日志，不要连续复位重试，以免触发启动失败回滚。
- **ST-Link占用/61234端口占用**：结束对应调试会话，再运行任务；脚本不会替你杀其他程序。
- **电源输出非OFF**：按正常业务流程停止测试/下电；脚本不会擅自拉低控制脚。
- **包回读失败**：不提交升级请求；保留原运行槽和证据。提交结果不确定时不自动重发，由B按日志裁决。
- **安装/确认超时**：可能已回滚，不等于升级成功。查看该次日志；如果报告目标连接无法恢复，关闭调试器后手动按RESET，不要直接覆盖A区或清启动日志。

## 本次修改和验证（2026-09-22）

替换旧的直接覆盖App任务，新增统一构建/下载入口；复用现有地址修正、打包、SWD备份/C日志和网络升级工具，没有增加跳过校验的调试模式。Boot的SWD工具仅增加可由调用方传参的入口，未改Boot/App固件逻辑。

已执行：脚本双槽保护检查、9种正常/故障编排回归、未确认空闲时拒绝执行、Python/VS Code配置语法检查、RAM助手构建，以及使用既有板卡备份的升级前检查。V2 OTA Debug/Release均完成A1/A2逐字节及反向核验、打包：Debug App为243928字节、包251528字节、1871处地址修正；Release App为143696字节、包149248字节、1359处地址修正。**本次没有连接ST-Link、烧录或复位开发板；新的一键SWD编排仍待首次实板运行验收。**

离线复跑：`python -B -X utf8 tests/test_ota_dev.py`。模拟中的安装PASS只属于脚本验证，不代表开发板已升级到0.1.9。
