# 全部命令接入核查（2026-09-28）

## 1. 范围与结论

核查对象为当前 V2 工作区的 PC 命令表、处理函数、正式任务、RK 协议和驱动调用，并对照 `doc/pc_tcp_protocol_spec.md`。另外只读检查独立 B1 工程的差异。本轮没有修改固件、发送网络/串口命令、连接 ST-Link、复位、烧录或推送；未重新渲染 Word、同步飞书。

结论：**不是“所有命令都已完成”，也不是“都只是模拟”。** 心跳、W25 信息查询、手动结果灯和 MCU OTA 已有真实执行路径；FUNC 已接入正式 V2 服务，但真实 RK 最终结果闭环仍待验收；电源、安全重启、完整测试编排和采样统计还存在明确缺口。

文档枚举共 **31 个编号**，包含请求、响应和 None，不能按 31 项独立业务计算完成率。V2 头文件定义 24 个编号；接收分发表有 23 项，其中 14 项请求处理、9 项反向响应拒收。`0105` 是异步输出，不能因为不在接收表中就认为未实现。

本表中的状态：

- **已接入**：能够追到正式业务/驱动调用，不表示本轮已在真实器件上验收。
- **部分接入**：已有执行或数据通路，但功能/字段不完整。
- **门禁保留**：入口存在，目前按条件拒绝，不能宣称硬件动作已完成。
- **仅预留**：文档有编号，尚无业务处理。

以下均假定帧头和 JSON 合法。非法帧/JSON 可触发断连；业务参数错误通常返回 `0006/failed`。测试忙碌、OTA 健康确认等公共门禁可能先于具体命令返回。

## 2. PC–MCU 全编号表

| 编号 | 功能/方向 | 当前实际行为 | 接入判断 |
|---|---|---|---|
| `0000` | None | 无业务；发送合法 JSON 时走未支持命令分支 | 占位值，不是待实现功能 |
| `0001` | MCU→PC 心跳响应 | 收到 `0002 + {}` 后返回 `{}`；PC 反向发送返回 `invalidDirection` | 已接入 |
| `0002` | PC→MCU 心跳请求 | 校验空对象，立即回 `0001`；不依赖 RK | 已接入 |
| `0003` | 登录请求 | 未定义处理器，回 `0006/failed/notSupported` | 仅预留 |
| `0004` | 登录响应 | 不会主动生成；PC 发送也走 `notSupported` | 仅预留 |
| `0005` | 登出 | 未实现登出会话逻辑，回 `notSupported` | 仅预留 |
| `0006` | MCU→PC 通用执行结果 | 统一承载 `accepted/completed/failed`，带 `requestCommandId`；不是只用于错误 | 已接入 |
| `0101` | 查询测试仪信息 | 每次经 FRU 加载接口读取 W25Q256；成功回 `0102`，加载失败回 `executionFailed` | 已接入 |
| `0102` | MCU→PC 测试仪信息 | 返回 `testerName/testerVersion/testBoard`；不是完整 FRU、IP 或固件版本清单 | 已接入 |
| `0103` | 开始测试 | 正式 V2 支持外部已启动的 `functional`：`power:null, bootMode:null, reboot:false`；查算法表、排队，回 `0006/accepted`。完整电源/Boot 请求仍拒绝 | 部分接入 |
| `0104` | 停止测试 | 无处理器、无 RK 取消命令，回 `notSupported`；断开 TCP 也不会取消算法 | 仅预留 |
| `0105` | MCU→PC 测试结果 | 正式 V2 已接四 TLV→结果队列→JSON→TCP；只实现 FUNC 整体结果，测量数据为 `null` | 部分接入；真实最终结果待验收 |
| `0501` | 启动 RK/DDR 电源 | 已调用监测层和电源状态机。配置有效时受理后回 `accepted`，已 ON 回 `completed`；正式 V2 默认未配置时序，回 `notConfigured` | 已接底层，但正常上电门禁未放行 |
| `0502` | 安全下电 | 逻辑 OFF 回 `completed`；ON 回 `notReady`。当前处理器没有启动安全关机/反序下电流程 | 门禁保留 |
| `0503` | 查询电源状态 | 回 `0504`，读取 MCU 电源状态机；不读真实电压，不查询 RK 是否正在跑算法 | 已接入 |
| `0504` | MCU→PC 电源状态 | `off/starting/on/stopping/fault`；外部独立供电的 RK 在运行时，V2 仍可能显示逻辑 `off` | 已接入；只能解释为逻辑状态 |
| `0505` | RK 安全断电重启 | ON 回 `notReady`、OFF 回 `invalidState`，故障/过渡态也拒绝；没有实际完整循环执行分支 | 门禁保留 |
| `0506` | 设置 Boot 模式 | OFF 且满足公共门禁时调用 MCP4725 Fast Write；真正写成功才回 `completed`，I2C 失败回 `executionFailed`，不写 EEPROM | 已接驱动，真实 DAC/启动待验收 |
| `0507` | 查询 Boot 模式 | 回最后一次有效成功写入的配置；未设置或实际写失败后缓存失效，回 `notConfigured` | 已接入；查询的是缓存 |
| `0508` | MCU→PC Boot 配置 | 返回 `bootMode/targetMv`；不是 DAC 实测电压，也不是 RK 实际启动介质反馈 | 已接入 |
| `0509` | DDR 类型/20mV 拉偏 | OFF+三路全零：仅选择类型，可不访问 TPS 即成功；ON+类型不变：调用三轨调压。OFF+非零被拒绝 | 已接驱动；不是完整训练电压编排 |
| `0601` | 查询电源采样 | 只复制监测任务的最近快照，回 `0602`；没有快照回 `executionFailed`，不会因查询而直接采样 | 部分接入 |
| `0602` | MCU→PC 采样结果 | 已有 `sampleAgeMs/samplePowerState/validMask`，有效时附三路电压/电流/功率数组；**未输出文档中的 `statistics`** | 部分接入；存在文档/代码差异 |
| `0701` | 设置结果灯 | 查目标/状态表，调用正式结果灯 GPIO 层；成功回 `0006/completed` | 已接入实际 GPIO |
| `0702` | 查询结果灯 | 回 `0703`，读取三组灯的已设置业务状态 | 已接入 |
| `0703` | MCU→PC 结果灯状态 | 返回 `lpddr0/lpddr1/emcp`；不是检测灯珠是否真的发光 | 已接入 |
| `0801` | 查询 B1–B2 保活 | 没有 PC 处理器，回 `notSupported`；B2 保活业务合同仍未冻结 | 仅预留 |
| `0802` | MCU→PC B2 保活状态 | 没有状态聚合和响应生成 | 仅预留 |
| `0901` | 固件升级请求 | MCU 五个动作已接 OTA 后台，RK 五个动作仅预留；见下表 | MCU 已接入，RK 未接入 |
| `0902` | MCU→PC 升级状态/响应 | 对下载、写块、提交、取消及普通查询生成响应；失败通常改回 `0006` | 已接入 |
| `0903` | MCU→PC 目标固件健康确认 | PC 用 `mcu_query` 指定目标包，确实运行该包且健康确认后才返回；**不是上电主动推送** | 已接入 |

登录、登出、停止及 B2 保活在当前协议正文中仍是空标题，属于预留而非已冻结完整合同。`0201–0204` 属于 PC–B2 温控，不应接入 B1 代理转发。

## 3. 当前 V2 发硬件命令会发生什么

### 3.1 结果灯可以真正控制，但不是开发板两颗调试灯

正式 V2 的 `ENABLE_V2_RESULT_LED` 已启用，`0701` 控制六路替代 GPIO：

| 对象 | FAIL 通道 | PASS 通道 |
|---|---|---|
| `lpddr0` | PD7，P2-23 | PD3，P2-24 |
| `lpddr1` | PD2，P2-25 | PC12，P2-26 |
| `emcp` | PC11，P2-27 | PC10，P2-28 |

支持 `off/pass/fail/testNormal/testHot`；测试闪烁亮、灭各 500ms。PB0/PB1 仍是 V2 调试指示，不能以它们没变判断 `0701` 失败。上述排针不能同时连接占用同网的摄像头、LCD、SD 扩展。

当前自动测试服务未调用 `result_led_set()`：**PC 手动设置灯已经接入，“开始/结束测试后 MCU 自动跟随切灯”尚未接入。** 不应从手动灯命令通过推断自动联动也已完成。

### 3.2 采样查询不是没有硬件就必定回失败

正式 V2 启动使用 `pwr_ctrl_init(NULL)`，默认 OFF、无批准时序；监测任务仅在 ON 状态访问采集器件。因此有快照后，`0601` 可以正常回 `0602`，但 `validMask=0`、不附电压/电流/功率数组，表示没有有效测量，不是实测 0V/0mA。

V2 的正式 AD7124 绑定还明确为 NULL：PB12 与板载 PCF8574 中断共网，在独立 ADC 片选接线确认前不能启用。即使临时接了模块，也不能直接宣称已经接入正式采样。

`0506` 则会在状态允许时真正尝试 DAC 通信，失败才返回 `executionFailed`。`0509` 的 OFF+零档位只改软件类型，成功不代表 TPS 电压发生变化。

### 3.3 测试在途的互斥

正式 V2 正在等待算法结果，或有限超时后 RK 状态未知时：

| 操作 | 当前行为 |
|---|---|
| 心跳、信息查询、电源状态、采样、Boot 查询、灯查询 | 不因算法在途而统一拒绝，仍执行各自校验 |
| 手动设置结果灯 | 允许 |
| 新 `0103` | `busy` |
| `0501/0502/0505/0506/0509` | `busy`，不能改变测试环境 |
| `0901 mcu_query` | 允许只读查询 |
| 其他 `0901` 动作 | 公共门禁先回 `busy`，包括保留的 `rk_*` |

没有独立“测试是否忙碌”的查询编号。不能用重复发送 `0103` 当正常状态查询：空闲时它会真的再启动算法。

## 4. RK 侧命令与最终结果

以下是正式 V2 `algorithmName` 查表支持的全部 20 项。表项存在证明 MCU 能编码和分发，**不证明每个真实 RK 算法都已逐项跑完验收**。

| RK CmdID | algorithmName | RK CmdID | algorithmName |
|---:|---|---:|---|
| 1 | `gtest-stuck-address` | 11 | `gtest-checkerboard` |
| 2 | `gtest-rand` | 12 | `gtest-blockseq` |
| 3 | `gtest-xor` | 13 | `gtest-walkbits0` |
| 4 | `gtest-sub` | 14 | `gtest-walkbits1` |
| 5 | `gtest-mul` | 15 | `gtest-bitspread` |
| 6 | `gtest-div` | 16 | `gtest-bitflip` |
| 7 | `gtest-or` | 17 | `mats` |
| 8 | `gtest-and` | 18 | `marchx` |
| 9 | `gtest-seqinc` | 19 | `marchc-` |
| 10 | `gtest-solidbits` | 20 | `marchc+` |

- MCU 发送 `0x5000` 命令容器，内部 CmdID 为小端 u16；PC 的命令编号/Seq 不直接透传为 RK 编号/Seq。
- RK 心跳为 `0x0000` 空 Value；心跳不能表示算法完成、DDR-ready 或安全掉电确认。
- 最终结果同帧必须包含各一次 `0x5001` 命令、`0x9001` 首错地址、`0x3001` die、`0x9002` 耗时。帧校验、宽度、RK Seq 和命令匹配后，整体交付。
- FUNC 地址为 0 → `testStatus=1`，`firstErrorAddress=null`；非 0 → `testStatus=2`，保留 64 位十六进制地址。`errorType` 目前为 null。
- 当前 `siteId` 取 W25 FRU；未配置则为空。`mirrorIndex=0`、`physicalAddr=""`，不拿 RK die 猜测 B2 编号或芯片位置。die/耗时已解码，但当前 PC 结果未独立上报它们。
- `measurementData` 当前固定 null。PARAMETRIC/IDD 21–36、SLT 编排、取消、RK-ready、安全下电都没有可用正式业务闭环。
- 默认算法等待不限时；断开 TCP 不取消在途任务。结果属于原连接，原连接已断开时不会补发给新客户端；迟到结果仍可用于回收内部状态。

2026-09-28 真实联调已观察到 Cmd1 下发、RK schedule/run 和 setting 进度；尚未取得“真实 RK 最终四 TLV + TCP 0105”的完整验收证据。桥接日志曾有截断，不能把未观察到结果直接判定为 RK 未发送或 MCU 解析错误。参见[本次现场记录](pc_func_disconnect_20260928.md)。

## 5. `0901` 内部动作全表

| action | 实际接入 | 成功响应/限制 |
|---|---|---|
| `mcu_begin` | 检查健康、OFF、互斥、版本记录和下载状态，建立下载会话 | `0902/completed`，提供 `blockSize/nextOffset` |
| `mcu_write` | Base64 解码、顺序/重复块检查、W25 分页写入和读回比较 | `0902/completed`，返回下一偏移 |
| `mcu_finish` | 完整包/目标/版本/CRC 校验，提交启动记录，交给 B 安装 | `0902/accepted` 不等于升级完成 |
| `mcu_query` | 每次读取持久化版本并与启动记录交叉核对，同时报告下载状态 | 通常 `0902`；指定包且已健康运行才 `0903` |
| `mcu_cancel` | 取消尚未提交的下载，释放维护态 | `0902/completed`；提交后不可取消 |
| `rk_begin` | 仅识别保留名称 | 空闲时 `0006/notSupported`，不访问 RK/Flash |
| `rk_write` | 仅识别保留名称 | 同上 |
| `rk_finish` | 仅识别保留名称 | 同上 |
| `rk_query` | 仅识别保留名称 | 同上，不冒充 MCU 查询 |
| `rk_cancel` | 仅识别保留名称 | 同上 |

完整 OTA 镜像才开放 MCU 动作；普通非 OTA 镜像拒绝。当前 0.1.17 正式构建缓存确认 `ENABLE_OTA_BOOT=ON`、`ENABLE_RK_GATEWAY_TEST=OFF`。

单次原始块不超过 3000 字节，Base64 不超过 4000 字符，含头整帧不超过 4096 字节。下载 30 秒无有效进度会退出接收态；提交前断线不会自动续传原会话。RK 的两个 8MiB 区域只是空间预留，不意味着 SPL/EDK 下载或串口升级已实现。

## 6. 必须区分的缺口

| 项目 | 已有内容 | 真正缺少的部分 | 性质 |
|---|---|---|---|
| `0602 statistics` | 最新采样快照、内部 `idd_stats` 计算模块 | JSON 序列化、业务启动/停止统计的调用 | 明确软件接入缺口 |
| `0105 measurementData` | FUNC 首错地址结果转换 | 按本次算法开始清零、有效阶段累计、结束冻结并上报统计；当前构造器固定 null | 开发板 null 如实，但整机功能未完成 |
| 自动结果灯 | `0701` 手动驱动、闪烁任务 | 测试生命周期与灯状态联动 | 未接业务，非灯驱动缺失 |
| 完整 `0103` | 全参数解析、训练目标底层接口 | 安全训练范围、配置比较、必要冷重启、Boot/电源/RK-ready/算法交接编排 | 依赖软件编排及 RK/硬件合同 |
| `0502/0505/0104` | 前两者的拒绝门禁、后者的文档编号 | RK 安全下电/取消能力及完成确认 | 需与 RK 联合明确，不可擅自强关替代 |
| B2 保活、编号/IP | SPI 板级基础、文档预留 | 双方冻结后的载荷、状态维护、查询、地址通知合同 | 依赖 B2；不是温控转发 |
| 反向 `0105` 拒收 | 异步 `0105` 输出已接入 | 接收表未显式登记拒收，现回 `notSupported`，与其他响应编号的 `invalidDirection` 不一致 | 小型一致性问题，不影响正常上报方向 |

检索正式源码发现 `monitor_idd_start()` 和 `monitor_idd_get()` 目前只有声明/定义，没有 PC/RK 业务调用。因此不能把“内部算法已经会算平均值”写成“PC 查询和测试结果已经能拿到平均值”。现有约 2 秒后台采样得到的是样本最大值，也不是 DDR 瞬态真实峰值。

`0101` 在有效 FRU 记录上只加载；空白、损坏或支持的旧格式仍可能走既有初始化/迁移并回读。不能把它宣传成对所有 Flash 状态都绝不写入的物理只读操作。

## 7. 独立 B1 工程差异

本轮只读检查 `D:/pro/b1`，没有修改或构建 B1：

- `App/protocol/pc/pc_dispatch.c` 的 `handle_start_test()` 仍只有参数校验，合法请求返回 `notReady/otaNotReady`，不投递 RK。
- B1 未集成 V2 新增的 `pc_test_service.c`；其结果构造器仍为较早字段集合，不能把 V2 最新 FUNC 接入结论套用到 B1。
- B1 `pc_ota.c` 与 V2 当前文件逐字节相同；这只证明该协议文件同步，不代表 B1 的整套硬件/OTA 已实测。
- B1 的 UART、SPI、LED、20mΩ 分流参数和电源门禁必须保留。后续迁移应接 B1 真实供电流程，不能直接照搬 V2 的外部已启动旁路。
- 正式 B1 IP 由 B2 告知的方案仍待通知合同；当前本地地址是测试设置，不通过本次核查新增 SPI/IP 命令。

## 8. 本轮验证与证据

复用现有脚本，不增加测试框架。编译器首次在受限环境启动被拒后，经工具授权执行本机 GNU/QEMU；全部验证仅为本机离线运行。

| 检查 | 本轮结果 | 能证明什么 |
|---|---|---|
| `tests/verify_pc_protocol.py` | 24 PASS | 固定头、JSON、半包/粘包等模型/源合同；不是网络实测 |
| `tests/verify_pc_start_c.py` | PASS | 实际 C 参数校验、分发拒绝和 PAC 换算到 0602；硬件为替身 |
| `tests/verify_pc_rk_service.py` | PASS | 实际 C PC 分发、队列、FUNC PASS/64位首错地址、busy/断线归属/独立 Seq |
| `tests/verify_rk_protocol_c.py` | PASS | 实际 C RK 帧/TLV/事务匹配及心跳调度 |
| `tests/verify_pc_ota_contract.py` | 25 个示例、39 个反例通过 | OTA JSON/长度设计合同；最大数据示例 PDU 4090 字节 |
| `tests/verify_ota_app.py` | 普通镜像拒绝 + 6 场景 PASS | 使用已有 V2 0.1.17 包运行实际 C OTA 后台，Flash/复位由测试替身提供 |
| `tests/verify_result_led.py` | PASS | 5 种状态、500ms、隔离波形镜像的模型/源合同 |

已有真机记录单独引用，不算本轮重测：

- [V2 结果灯 GPIO 与 TCP 记录](20260908_pc_led_gpio.md)：42 次 TCP 交互及追加 22 次 GPIO 验证；确认 GPIO 输出，非灯珠光学验收。
- [OTA 3000 字节分块实测](../ota_chunk_3000_validation_20260917.md)：历史 V2 网络升级与异常路径；不代表 B1 真板通过。
- [真实 RK 接入说明](../rk_real_test_20260928.md)及[现场记录](pc_func_disconnect_20260928.md)：说明 0.1.17 正式接入与尚未完成的真实最终结果验收。

主要源码入口：

- [PC 编号](../../App/protocol/pc/pc_command.h)、[PC 分发表/硬件业务](../../App/protocol/pc/pc_dispatch.c)、[TCP 任务](../../App/tasks/pc/tcp_task.c)。
- [正式测试服务](../../App/protocol/pc/pc_test_service.c)、[测试 JSON](../../App/protocol/pc/pc_test_message.c)、[RK 算法表](../../App/tasks/rk/rk_link.c)、[RK 任务](../../App/tasks/rk/rk_task.c)。
- [监测/Boot/统计入口](../../App/tasks/common/monitor_task.c)、[电源状态机](../../HAL_Drv/pwr_ctrl.c)、[结果灯](../../Core/Src/result_led.c)。
- [OTA 动作分发](../../App/protocol/pc/pc_ota.c)、[OTA 后台](../../App/ota/ota_app.c)。

建议推进顺序：先取得一次真实 RK 最终结果→0105 的证据；再补统计字段和生命周期接入；随后按 B1 边界迁移正式服务。安全下电、取消、完整上电及 B2 接口继续等待相应合同与硬件验收，不为了“全部已接入”而放开拒绝门禁。
