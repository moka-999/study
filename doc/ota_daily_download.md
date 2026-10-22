# V2 / B1 改代码后的一键编译与下载

适用：`D:\pro\test` 的 App-V2、`D:\pro\b1` 的 App-B1，板上必须已经首烧对应板型的完整OTA Bootloader。开发电脑保留同级 `test`、`b1`、`bootloader` 工程；B1脚本复用test中的编排，不复制另一套擦写逻辑。B1入口已适配，实际首板验证仍待完成。

空白板在“终端 → 运行任务”选择 **🆕 V2/B1 空白板 Boot+App 首烧（ST-Link）**：填首版版本、实际探针序列号并确认后，自动编译完整B+A1及初始化W25恢复包/日志。对应`tools/first_flash.ps1 -Action deploy`，不需手选文件，也不会自动擦除非空板。首烧任务独立于Ctrl+Shift+B；V2沿用电源模拟/MATS第一颗假FAIL，B1真实。只构建交付给同事仍用`-Action build`，包内`flash.ps1`不需源码。详见[统一首烧与B1接线说明](../../bootloader/README.md#空白板首次部署v2--b1)，不要用日常入口初始化空日志。

> **验收边界（2026-10-16）**：旧入口在2026-09-23曾因连接/复位使日志变为pending=1而被安全拒绝，见[当时记录](rk_protocol_migration_20260923.md)。本次调整为捕获复位入口后仅加载一次RAM助手，备份、写入及提交之间不再复位/重新load，仍保留pending门禁。新编排已通过离线故障检查和两板构建，**尚未实板验证**；不能把历史TCP升级成功当作本入口通过。

## 独立网络工具（V2 / B1，2026-10-15）

不依赖正式上位机，也不要求操作人员电脑安装编译器。解压对应板型交付包，
双击`start_v2_ota.cmd`或`start_b1_ota.cmd`，选择查询/升级、填写实际IP、选择`.ota`。
需要Python 3.10+；两板入口独立，发送前拒绝另一板型文件，公共传输实现只有一份。

V2的`ota-build`现在同时输出`build/v2-ota-tool-<版本>-<时间>.zip`；
B1在自己的工程执行`tools/dev.ps1 ota-build -Version <新版本> -Configuration Release`。
不需要更改MCU程序、Boot分区或线上协议；`ota-net`验收脚本保留，SWD入口按下节使用。

升级前必须人工停止算法/温控、断开正式PC客户端，并确认逻辑电源OFF及当前固件健康。
新工具不自动关电、不清busy、不重复提交finish；提交后只读重连验证目标包和健康状态。
finish前断线不跨连接续传。说明与本轮验证见[双板独立工具](ota_standalone_tools_20261015.md)。

## 最常用：ST-Link下载，不需要网线

1. 保存修改；停止RK测试、结束本轮业务并使电源OFF，断开PC/网络助手。若接B2，另行停止温控。本工具不控制B2，也无法替操作者判断外部RK是否空闲。上电后先等待约40秒，让旧App完成健康确认。
2. 接好ST-Link/SWD，只接一只烧录器，结束其他调试会话。VS Code打开要烧录的工程文件夹。
3. 按 **Ctrl+Shift+B**：V2执行“⚡ V2 一键编译烧录”，B1执行“⚡ B1 一键编译烧录”。**没有版本、序列号或文件选择弹窗；按下即确认上述准备完成并允许暂停/复位MCU。**
4. 等待安装及30秒健康确认 `PASS`。备份与构建需要时间，中途不要断电或拔烧录器。结束后释放本工具自己的连接，不关闭其他调试程序。

| 打开的工程 | 固定板型 | 快捷键构建配置 |
|---|---|---|
| `D:\pro\test` | `apollo-v2-h743` | Debug；保留V2电源/测量模拟和MATS第一颗假FAIL |
| `D:\pro\b1` | `b1-h743` | Debug；B1真实驱动，禁止V2模拟和假FAIL |

脚本自动选择唯一ST-Link，从W25启动记录读取已确认版本，将patch加1后编译，例如0.1.36→0.1.37。无需填写 `.ota` 路径、改链接地址或修改C版本宏；多探针时拒绝猜测，需用命令行指定`-Serial`。patch达到65535时要求人工指定更高minor版本。

自动流程：检查PC6/PC7/PG11为输出低 → 备份内部Flash 2MiB → 核对B区板型 → 暂停在复位入口，加载一次RAM助手 → W25前 `0x410000` 字节双读备份 → 核对已确认版本/当前槽CRC → 构建A1/A2并验证地址修正 → 生成一个 `.ota` → 写W25下载区并完整回读 → C接口提交升级请求 → 复位由B安装到另一个A槽 → 核对实际镜像、非目标Flash区域及健康确认。

**B1的MCU复位会关闭RK/DDR电源**，升级后由PC正常流程重新上电。脚本遇到输出非OFF会拒绝，不自动下电、不修改启动日志来清除busy/pending；复位本身仍会丢失运行时业务状态，必须先停测。

**不需要修改链接地址、手写启动标志或自行更新CRC，也不需要每次重烧B。** Boot同步本次实际运行槽的固件信息到W25；App的信息查询继续从W25读取。FRU和稳定包不属于本入口的写入目标。

## 其他入口

在“终端 → 运行任务”中选择；B1也有独立首烧和编译打包，网络下载另用`start_b1_ota.cmd`：

| 入口 | 用途 | 连接要求 |
|---|---|---|
| 🆕 V2/B1 空白板 Boot+App 首烧 | 本板完整B+A1和W25初始化，仅首次部署 | ST-Link；确认全空及空闲，不需网线 |
| 🔨 OTA 编译打包（不下载） | 只检查代码和生成一个 `test.ota` | 不连接烧录器/开发板 |
| 🌐 OTA 网络升级（会复位） | 使用现有0901/0902/0903协议下载、安装、确认 | PC能访问板卡IP及8888端口；不用ST-Link |

网络升级前关闭网络调试助手、PC软件等现有连接；MCU只服务一个客户端。输入板卡IP，默认 `192.168.1.201`。App须正常运行并已健康确认、电源状态为OFF。网络方式不另外读取整片备份，依靠完整OTA的旧槽/日志保护；保留之前的本机首部署备份。

一次只运行一种下载方式，不要同时启动网络升级和SWD操作。旧的扩展“Download/Run”按钮不等于这里的任务，不能用它直接覆盖App。F5断点调试也不是本次入口；若后续只附加调试，应采用下载成功后打印的**实际槽ELF**，不要自动load一个固定地址的旧ELF。

## 命令行用法

在 `D:\pro\test` 终端执行，版本仅为示例：

```powershell
# 只生成升级包，不访问板卡。
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev.ps1 ota-build -Version 0.1.9

# V2：等价于当前快捷键，版本及唯一烧录器自动选择。
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev.ps1 ota-swd -ConfirmIdle YES -PowerSimulation -MatsFailTest

# B1：在D:\pro\b1终端执行；真实驱动，不传V2模拟选项。
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev.ps1 ota-swd -Configuration Debug -ConfirmIdle YES

# 网络升级，不占用ST-Link。
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev.ps1 ota-net -Version 0.1.9 -BoardIP 192.168.1.201 -ConfirmIdle YES
```

两板快捷键都是Debug；命令行V2默认Debug、B1默认Release，可显式指定 `-Configuration`。SWD可加`-Version <新版本>`或`-Serial <24位序列号>`覆盖自动值，但仍检查版本必须更高。构建目录为各工程 `build/ota-app-<版本>-debug-a1/` 或 `-release-a1/`；V2模拟追加`-power-sim`和`-mats-fail`目录标识。`test.ota`/`b1.ota`为各自唯一升级文件，A2构建仅用于验证/调试符号，不需要发送两个App。每次两槽共用同一构建时间。

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
- **ST-Link枚举失败**：查看本轮`probes.log`中的原始错误及退出码；失败时尚未启动目标调试连接，不会继续复位或写入。`0x8004FFFD`曾由遗漏Programmer路径导致，现已修复，不能仅凭这个退出码认定USB或板卡损坏。
- **GDB报`Argument to arithmetic operation not a number or boolean`**：2026-10-16复位入口检查曾直接对指针类型`$pc`做按位与，现已转换为32位无符号整数。判断是否复位、写入或清理成功仍以本轮`init-agent.log`和`release-probe.log`为准，不应把所有GDB错误归为同一原因。
- **没有唯一ST-Link/板型不符/空白板**：只接一只匹配板卡的烧录器；不要用V2入口刷B1，空白板用`first_flash`。不会为通过检查而重写B。
- **电源输出非OFF**：按正常业务流程停止测试/下电；脚本不会擅自拉低控制脚。
- **包回读失败**：不提交升级请求；保留原运行槽和证据。提交结果不确定时不自动重发，由B按日志裁决。
- **安装/确认超时**：可能已回滚，不等于升级成功。查看该次日志；如果报告目标连接无法恢复，关闭调试器后手动按RESET，不要直接覆盖A区或清启动日志。

## V2日常SWD正常路径实测（2026-10-21）

用户授权后，先从独立双SPI镜像恢复匹配W25日志的完整V2 0.1.37/A1，再通过上述日常SWD入口安装最新0.1.38/A2，保留电源模拟/MATS第一颗假FAIL。备份、RAM复位捕获、助手复用、W25写入/回读、C日志提交、B安装和30秒健康确认全部完成；没有再出现10月16日的枚举路径或GDB指针表达式错误。

本次App268108字节、修正2065项、OTA包276484字节，A1↔A2逐字节核验通过；安装后的实际App与产物一致，内部B/原A1/保留区未变。TCP查询返回`firmwareVersion=0.1.38`、`activePackageId=79C8CF12`、`confirmed=true`及逻辑电源OFF；测试仪信息、心跳、采样查询正常。ST-Link与TCP连接已释放。

升级证据：`build/ota-dev/20261021-162349-324600/`；完整恢复及基线核对见[B2接入记录第9节](b2_spi_integration_20261020.md#9-v2恢复与0138安装2026-10-21)。这仅验收该V2的正常下载路径，不代表B1、空白板首烧、所有断电恢复或回滚故障场景均已实测。下面历史段落中的“尚未实板”保留为各自记录日期的状态。

## GDB复位入口检查修复（2026-10-16）

用户的`build/ota-dev/20261016-163712-936998/`已成功枚举探针、备份内部Flash共2097152字节，随后在`init-agent.gdb`的复位入口比较处报类型错误。原表达式`$pc & 0xFFFFFFFE`未将GDB地址值转换为整数；在本机Arm GDB中以指针值复现了同样报错。GDB允许将PC表示为[代码指针](https://www.sourceware.org/gdb/download/onlinedocs/gdb.html/Predefined-Target-Types.html)，不能假设所有寄存器表达式都按整数处理。

共用日常脚本改为`(unsigned int)$pc`后再屏蔽Thumb最低位，仍严格比较实际B向量，不删除复位捕获检查，不更改App/B固件或升级协议。新增实际GDB离线检查：只把PC和向量读取替换为常量，复用生产表达式，验证两种指针类型的10种情况；正确入口接受，App/RAM/零地址拒绝。测试已通过，内存中换回旧表达式会被此测试捕获；36个既有双板编排及CLI/PowerShell检查也通过。之前的流程测试使用测试桩代替GDB，未覆盖表达式类型，这是此前遗漏的验证边界。

故障现场尚未加载RAM助手、读取W25备份或写入升级包。备份中的B复位向量为`0x08000299`，失败后清理连接观察到PC为`0x08000298`；清理日志显示恢复调试寄存器并执行复位、detach。该用户尝试确实复位了MCU，但不能由日志认定App已完成健康确认。本轮修复只读本机日志并运行离线GDB，没有再次连接、暂停、复位或烧录MCU；完整SWD安装仍待实板验收。

## ST-Link枚举失败修复（2026-10-16）

用户两次下载在`--debuggers`阶段退出，十进制`2147811325`对应`0x8004FFFD`。只读复现得到原始错误：`Couldn't locate STM32CubeProgrammer ... use -cp <path>`。Windows已正常识别ST-Link，原因是脚本只在正式连接时传入Programmer位置，枚举时遗漏了相同参数；CMake警告不是本次失败原因。两个失败目录尚未生成备份，流程没有进入MCU连接、复位或擦写阶段。

共用`tools/ota_dev.py`已为枚举补传Programmer的`bin`路径，并在检查退出码前保存`probes.log`，失败时显示原始原因；V2/B1入口同时生效，不改固件、协议、分区或安全门禁。只读实测枚举返回0并识别唯一探针；双板正常/故障编排现共36个场景通过，覆盖枚举失败、无探针、多探针及显式序列号跳过枚举，原有CLI/PowerShell检查也通过。

本次仅枚举USB探针，未建立MCU调试连接、暂停、复位、烧录或推送。完整SWD安装及健康确认仍待实板验收；下次确认空闲、OFF及PC断开后可沿用Ctrl+Shift+B重试。

## 一键入口调整和验证（2026-10-16）

- V2/B1各自默认快捷键；共用`tools/ota_dev.py`，自动板型、探针、版本和文件路径。Boot侧只改主机脚本的助手复用和只读日志选择，不改App/B固件、PC协议、FRU布局或稳定包。
- 本轮针对旧的pending问题减少重复复位：RAM助手使用GDB `restore ... binary`载入RAM，后续只接续助手。ST官方[UM2576](https://www.st.com/resource/en/user_manual/um2576-stm32cubeide-stlink-gdb-server-stmicroelectronics.pdf)说明GDB `load`会间接调用Programmer并复位，不能只靠服务器`--attach`避免。复位未捕获在B向量入口时拒绝继续。
- 暂停期间冻结IWDG，退出时恢复原调试寄存器；所有提交只尝试一次。启动B的指令结果不确定时不重复复位，不以退出连接后的状态猜测安装成功。
- 离线覆盖两板各自A1/A2正常安装、12类故障（共28个编排场景），另验自动版本、唯一探针、CLI门禁、配置隔离、日志CRC、板型/当前镜像CRC及非目标槽保护；实际PowerShell分支的空/指定序列号传参检查、首烧回归和独立网络工具10组回归均通过。网络工具只使用本机回环，不访问开发板。
- 两板RAM助手及Debug双槽App构建、A1↔A2逐字节核对和打包通过：V2模拟/MATS镜像260892字节、包269180字节；B1镜像254228字节、包262192字节。候选版本0.1.37，不表示已经烧入板卡。
- **未连接ST-Link或开发板、未烧录、未提交/推送。** 后续先在备份后验收V2完整SWD流程，再在B1首烧后验收对应入口。复位捕获、助手接续、W25回读和健康确认均需真板验证，不能用离线PASS替代。

离线复跑：`python -B -X utf8 tests/test_ota_dev.py`。

## 历史修改和验证（2026-09-22）

替换旧的直接覆盖App任务，新增统一构建/下载入口；复用现有地址修正、打包、SWD备份/C日志和网络升级工具，没有增加跳过校验的调试模式。Boot的SWD工具仅增加可由调用方传参的入口，未改Boot/App固件逻辑。

已执行：脚本双槽保护检查、9种正常/故障编排回归、未确认空闲时拒绝执行、Python/VS Code配置语法检查、RAM助手构建，以及使用既有板卡备份的升级前检查。V2 OTA Debug/Release均完成A1/A2逐字节及反向核验、打包：Debug App为243928字节、包251528字节、1871处地址修正；Release App为143696字节、包149248字节、1359处地址修正。**本次没有连接ST-Link、烧录或复位开发板；新的一键SWD编排仍待首次实板运行验收。**

离线复跑：`python -B -X utf8 tests/test_ota_dev.py`。模拟中的安装PASS只属于脚本验证，不代表开发板已升级到0.1.9。
