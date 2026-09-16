# PC MCU OTA 协议设计

日期：2026-09-16。本文已接入启用`ENABLE_OTA_BOOT`的V2固件，并完成0.1.3→0.1.4真实TCP升级；普通/隔离镜像未启用完整Boot合同则返回notSupported。实测范围与未验收项见[网络验证记录](ota_network_validation_20260916.md)。PC 对接摘要见 [PC 协议 4.26 至 4.28](pc_tcp_protocol_spec.md#426-mcu固件升级请求0x0901)；Boot 安装与回滚见 [OTA 设计](ota_design.md)。

本次只设计本颗 STM32 的 App 升级。沿用 TCP 8888、单客户端、18 字节大端头、JSON、4096 字节完整帧上限及双方独立发送 Seq。PC 只发送一个 `.ota` 文件；App 下载到 W25Q256，B 安装非活动槽。PC 不传槽号、擦写地址或强制降级开关，不在线更新 B、稳定版或 FRU。

## 1 命令及操作

已于 2026-09-16 只读核对[在线 PC 协议](https://tcn47lisvzcv.feishu.cn/docx/GWdYdgSArojCm3xPAPzcJxbxnqh)版本 29：编号、枚举名及 0901 成功回 0902、失败回 0006 已存在，三个 OTA 正文章节仅有“保留”。本设计保留编号，为其补充以下业务语义；没有修改在线文档。

| CommandID | 枚举名 | 方向及含义 |
|---|---|---|
| 0x0901 | CMD_MCU_OTA_REQUEST | PC→MCU，action 选择 begin、write、finish、query、cancel |
| 0x0902 | CMD_MCU_OTA_RESP | MCU→PC，操作成功或安装已受理、当前状态 |
| 0x0903 | CMD_MCU_OTA_COMPLETE | MCU→PC，查询的目标包已运行且本次启动已健康确认 |
| 0x0006 | CMD_COMMAND_RESP | MCU→PC，业务失败；requestCommandId 固定十进制 2305 |

PC 不发送 0902/0903，反向请求回 0006/invalidDirection。未知 action 回 notSupported。合法帧内字段错误回 0006，不断开连接；帧或 JSON 语法错误沿用原协议断连规则。

0903 只作为带 packageId 的 query 的响应，不依赖一次主动推送。一次请求只生成一个响应，不能先发 0902 再发 0903。没有 packageId 的普通查询永远回 0902。

## 2 包标识与传输字段

| 字段 | 类型与范围 | 精确定义 |
|---|---|---|
| action | 字符串，五个小写固定值 | 本次操作；不使用 Seq 代替 |
| packageId | 8 个大写十六进制字符 | 升级包头 CRC32 的数值编码，不带 0x；例如 5A1B2C3D |
| packageSize | JSON 整数，116..2097152 | `.ota` 文件总字节数，包含包头、版本、修正表和一个镜像；下限仅为接收边界，不代表 116 字节就是合法包 |
| offset | JSON 整数，0..packageSize-1 | Base64 解码后的文件字节偏移；从文件首字节算，不是 Flash 地址 |
| data | Base64 字符串，最长 1368 字符 | 解码后 1..1024 字节；通常每块 1024，末块按实际长度 |
| nextOffset | JSON 整数，0..packageSize | MCU 已连续写入并读回验证的字节数，也是下一块起点 |
| firmwareVersion | 字符串 major.minor.patch | 固件版本，不是 testerVersion 硬件版本；每段为 0..65535 的十进制整数 |

请求只接收对应操作列出的字段；缺失、重复键、错误类型、未知字段、非整数数值均拒绝。JSON 中的 bool 不当作整数。响应接收端允许新增字段。

packageId 读取方法：包格式 v1 的文件偏移 36..39 为小端 u32，转换成固定 8 位大写十六进制字符串；该数应等于文件前 36 字节的 IEEE CRC32。PC 从偏移 44、46、48 的三个小端 u16 读取目标固件版本，并保留用于最终比对。文件内部小端不随 TCP 大端帧头转换；data 解码后必须原样写入。

packageId 标识发布文件，不是随机会话号或历史升级次数。同一文件重传、重连仍使用同一值；不同板卡可安装同一包。它不是密码或数字签名，不保证抗碰撞。以用户批准的版本号＋CRC 初版为边界，最终仍核对实际版本、完整包各段 CRC、板型和镜像结构，不把 packageId 单独当作安装授权。

复用启动记录中现有的 package_header_crc32，可以在复位后查询当前镜像的包标识，不为一次传输新增持久化流水。没有有效包标识的旧镜像返回 activePackageId=null，不能用全零冒充未知值；00000000 仍是合法 CRC 编码。

Base64 使用 [RFC 4648 §4](https://www.rfc-editor.org/rfc/rfc4648.html#section-4) 标准字母表、必要的等号填充、不换行；拒绝空白、非法字符、错误填充和非规范编码。每块独立编码，不把整文件 Base64 后任意截字符串。解码后长度检查与完整 PDU ≤4096 两个限制同时成立，不另外增加每块 CRC。

## 3 请求与响应

以下 5A1B2C3D、0.1.3 是格式示例，不是可直接安装的真实发布包。T1RBMQ== 仅演示文件头四字节 OTA1，后续仍须发送文件其余内容。

### 3.1 开始下载

0901 请求：

```json
{"action":"begin","packageId":"5A1B2C3D","packageSize":139452}
```

0902 成功：

```json
{"action":"begin","packageId":"5A1B2C3D","status":"completed","state":"receiving","blockSize":1024,"nextOffset":0}
```

只有当前 App 健康确认、测试空闲、RK/DDR 已安全 OFF、Flash 可用且没有其他升级操作时才受理。不得代替尚未完成的 RK 安全下电握手。PC 先单独协调 B2 维护，再调用 begin；B1 不控制或代理温控。

同一连接正在 receiving 时，相同 packageId 和 packageSize 的重复 begin 返回现有 nextOffset，不清进度、不擦写；不同包回 busy，相同标识但长度不同回 invalidArgument。试运行或 READY 阶段拒绝新 begin。必须先查询旧结果，不能借 begin 清除已提交的请求。

### 3.2 写数据块

0901 请求与 0902 成功：

```json
{"action":"write","packageId":"5A1B2C3D","offset":0,"data":"T1RBMQ=="}
```

```json
{"action":"write","packageId":"5A1B2C3D","status":"completed","state":"receiving","offset":0,"nextOffset":4}
```

先完整验证 JSON、Base64、包标识、长度及偏移，再调用存储层。必须满足 bytes ≤ packageSize-offset，按减法校验防止无符号加法溢出；不要求 256/1024 字节对齐。第一次收到完整 116 字节前缀后尽早核对标识、声明长度、板型和版本，finish 仍重新完整校验。

只有写入和读回都通过才推进确认偏移。正常请求要求 offset==nextOffset。仅允许立即前一个完整成功块重复：offset、解码后长度以及 W25 中的内容完全相同，返回同一 offset/nextOffset，不重复擦写。更早的块或跳偏移回 offsetMismatch；上一个块同位置内容不同回 dataMismatch，保留有效进度。错误应答带 nextOffset 供查询，不授权 PC 不经核对地跳块。

PC 对同一板一次只挂起一个 OTA 请求。匹配响应使用 action/packageId，write 还必须匹配 offset；重传使用新的发送 Seq，但业务字段不变。心跳不能被当作块确认。即使重复响应晚到，也不能多推进一次文件偏移。

发生部分写入、擦除、读回失败，终止未提交会话并释放维护占用，回 flashError/state=failed；不得凭部分增长的计数继续原会话。重试必须重新 begin，从文件首字节按需擦写。

### 3.3 完成并申请安装

0901 请求与 0902 受理：

```json
{"action":"finish","packageId":"5A1B2C3D"}
```

```json
{"action":"finish","packageId":"5A1B2C3D","status":"accepted","state":"ready"}
```

要求完整接收、包中总长度与 begin 一致、包头 CRC 与 packageId 一致，并通过板型、版本、元数据/修正表/双槽镜像 CRC、向量与范围检查。普通升级版本必须高于当前已确认版本，比较三个数字而不是字符串。校验失败不提交 READY，不复位，不擦当前 App。

数据未收齐的 finish 回 invalidState，并带仍有效的 nextOffset，允许继续下载；完整包的格式、板型、版本或 CRC 校验失败则结束该未提交会话并释放维护占用，修正文件后重新 begin。字段/偏移等可纠正的请求错误不清除已验证进度。

READY 落盘并验证成功后才返回 accepted。初版不拆出另一个“激活”命令：提交后尝试发送受理结果并在 5 秒内执行 MCU 软件复位，发送等待有界。send 成功不代表 PC 应用已经收到；应答丢失、发送失败或连接断开均不得撤销 READY，也不得无限等 PC 确认而不复位。

READY 后相同 finish 幂等返回 accepted，不重复提交日志；cancel、begin、write 拒绝。若日志提交返回错误，但可能已写完提交标记，回 commitUnknown/state=commitUnknown（能发送时），保持维护锁并在同样的复位期限内交 B 重读；不能把它当作“可安全重新下载”的普通失败。

### 3.4 查询与最终确认

普通查询：

```json
{"action":"query"}
```

0902 示例，说明当前固件健康，并不说明某次新升级成功：

```json
{"action":"query","status":"completed","state":"confirmed","activePackageId":"11223344","firmwareVersion":"0.1.2","confirmed":true}
```

PC 提交安装后或应答丢失后，重连发送带目标包标识的查询：

```json
{"action":"query","packageId":"5A1B2C3D"}
```

未满足完成条件时仍回 0902，若请求含 packageId 则原样带回。state 来自当前阶段；activePackageId/firmwareVersion 始终表示实际正在运行的镜像，而不是下载目标。正在下载时附加 download={packageId,receivedBytes,totalBytes}；没有下载上下文时省略该对象。firmwareVersion 与 activePackageId 无法可靠加载时回 flashError，不用编译期版本顶替。

满足目标标识匹配、实际槽与启动记录一致、本次启动健康确认、没有活动升级或已锁存健康故障时，直接返回 0903：

```json
{"action":"query","packageId":"5A1B2C3D","status":"completed","state":"confirmed","firmwareVersion":"0.1.3","confirmed":true}
```

PC 还须核对版本与所选文件一致，然后才显示升级成功。该响应可重复查询，不额外定义 ACK，不写新的 Flash 标志。仅表示所查询的发布包当前已健康运行，不证明它刚刚由哪次连接安装。

如果回到旧版或稳定版，0902 返回实际版本/实际包标识，绝不为原目标包返回 0903。PC 显示“未运行目标版本”，没有可靠证据时不把未提交、安装失败或回滚混成同一种原因。初版不新增持久化升级历史；现有 stage 在健康确认后清零，不能据此伪造一个可永久查询的 rolledBack 状态。

### 3.5 取消未提交下载

0901 请求与 0902 成功：

```json
{"action":"cancel","packageId":"5A1B2C3D"}
```

```json
{"action":"cancel","packageId":"5A1B2C3D","status":"completed","state":"cancelled"}
```

仅对 receiving 的匹配包生效，释放 RAM 上下文与维护占用；下载区残片不作为有效安装包，不要求立即再擦除。相同连接、下一次 begin 前，重复取消最后已取消的包仍返回 completed。无对应会话或 ID 不符回 invalidState；READY/提交不确定后拒绝取消。

## 4 状态与错误

| state | 含义与 PC 操作 |
|---|---|
| receiving | 正在下载，按 nextOffset 继续 |
| ready | 包已提交，等待自动复位安装；停止发文件 |
| commitUnknown | 提交结果不确定，等 MCU 复位后查询，禁止重新下载 |
| trial | 本次启动尚未健康确认，继续查询；不等于安装已失败 |
| confirmed | 当前 App 已健康确认；只有目标 ID 匹配的 0903 才是所选包成功 |
| cancelled | 当前连接内下载已取消，可重新 begin |
| failed | 未提交下载失败，问题消除后从头开始；不表示 Boot 安装失败 |

状态优先级：有效下载/提交上下文优先于旧 App 的 confirmed；否则由本次启动健康确认决定 trial/confirmed。B 的 INSTALLING/MAINTENANCE 阶段没有网络服务，不虚构这些阶段的在线进度。cancelled/failed 在下一次 begin、断连或复位后可丢失，下一连接只查询实际启动状态。

0006 失败示例：

```json
{"requestCommandId":2305,"status":"failed","action":"write","packageId":"5A1B2C3D","offset":1024,"nextOffset":0,"errorCode":"offsetMismatch"}
```

| errorCode | 含义与处理 |
|---|---|
| invalidArgument | 字段/数值/Base64 不合法，先修正请求 |
| invalidDirection / notSupported | 方向或 action/协议能力不支持，不继续发送文件 |
| invalidState | 无匹配下载、数据未收齐或当前阶段不允许操作，查询状态 |
| busy / notReady | 测试、电源、其他下载或健康条件不满足，不自动强制停止业务 |
| offsetMismatch / dataMismatch | 偏移不连续或重复块内容不同，保留有效接收进度 |
| packageInvalid | 包格式、长度、修正表/向量/兼容性不合法，换正确发布包 |
| targetMismatch | 包板型与本机不符，不允许继续 |
| versionRejected | 普通升级同版本或降版被拒绝；先查询是否已运行目标 |
| crcMismatch | 完整性校验失败，确认文件后重新下载 |
| flashError | 存储读取/擦写/读回失败；先定位硬件或存储异常 |
| commitUnknown | 已尝试提交、不能确定是否生效，只能等待复位后查询 |

只有已校验合法的 action/packageId/offset 才带回错误响应；无效字段不得伪造。nextOffset 仅在会话仍可继续且包匹配时携带。错误文本不代替稳定 errorCode；PC 应忽略额外的未知响应字段。

## 5 超时与断线

| 项目 | 初版设计值 |
|---|---|
| begin/write/query/cancel 等待响应 | 5 秒 |
| finish 完整校验及提交等待响应 | 30 秒 |
| 同一连接请求重发 | 最多 3 次，串行重发原业务字段；Seq 正常递增 |
| receiving 无下载有效活动 | 30 秒后取消未提交会话；新块和核实一致的末块重发刷新，查询/心跳/重复 begin 不续期 |
| 重启后 PC 重新连接/查询 | 每 2 秒尝试，成功连接后每 2 秒查询目标 |
| PC 自动等待展示窗口 | 600 秒，超时标记“结果未确认”；不是自动强制复位或判定回滚的依据 |

以上为软件合同默认值，不冒充真板最坏耗时已验收。MCU 的存储等待和发送必须有界，2 MiB 最大包及失联情况下需测量验证；超出预算要明确调整合同，不通过无进展喂狗掩盖卡死。已有TCP任务500ms半帧无字节超时继续适用；5秒指完整请求后的响应等待，不允许PC发送半帧后停顿5秒。

1. 未提交时 TCP 断开、语法失步、会话超时或 MCU 复位：取消下载；重新连接先 query，再 begin 从 0 开始。初版不做跨连接或掉电续传。
2. finish 超时但连接还在：可串行重试同一 finish，不能转发新 begin；已提交则返回同一受理结果。
3. finish 后断线，即使没收到 accepted：重连后只查询。若实际仍为旧版本且已 confirmed、无 ready/commitUnknown，再决定是否重试下载；不凭 TCP 断线推断提交失败。
4. READY 后掉电：B 依据持久化启动记录决定安装/恢复；PC 不重写下载区。所有自动恢复属于 B，不使用 PC 强制降级命令。

## 6 接入位置

保持 `TCP socket → PC帧状态机 → PC命令表 → pc_ota动作表 → ota_app → Flash/启动记录`。TCP只通知连接结束和周期轮询，不解析action/Base64，不安装镜像。提交后由独立看门狗任务在4秒后复位（加500ms调度周期仍小于5秒），不依赖发送完成或PC收到了应答。

| 模块 | 复用能力 | 本轮接入内容 |
|---|---|---|
| App/ota/ota_app.c | 下载、读回、OFF维护门、健康确认 | 包ID绑定、错误分类、进度查询、上一块幂等、30秒超时、提交后复位 |
| App/protocol/pc/pc_dispatch.c、pc_ota.c | 命令表、严格JSON、cJSON与固定头 | 0901及五动作查表、0902/0903响应、Base64严格解码、反向命令拒绝 |
| App/tasks/pc/tcp_task.c | 单连接与独立Seq、发送超时 | 通过dispatch转达断线和轮询，不增加业务解析 |
| App/ota/boot_record.c | 包标识与实际镜像描述、pending/epoch、3+3恢复 | 查询时在存储锁内一致读取，不新增历史字段；核对实际运行槽 |
| firmware_version_load | W25运行版本信息 | OTA查询每次从W25读取，并与启动记录版本/槽交叉核对；读取失败不能输出默认版本 |

特别注意：当前 `ota_app_is_confirmed()` 把“有下载会话”也视为不可接收新业务，不能直接把其返回 0 映射成“当前镜像不健康”。查询需分别表达启动健康与下载占用。现有响应缓冲为 384 字节，本设计的最大组合须检查可容纳，不能按接收上限4096误认为发送缓冲也是4096。

本轮不改变W25布局、不移植B1。包头早期检查复用B/App共享校验器，只读前116字节；finish仍重新完整验证，不能以早检成功代替镜像/修正表CRC。

## 7 验收清单

设计检查脚本 `tests/verify_pc_ota_contract.py` 仅校验文档 JSON、字段边界、Base64、18字节组帧及缓冲预算，不代表固件实现、C状态机、网络或真板通过。

设计阶段结果：24个OTA JSON示例及29个字段反例通过；1024字节数据块的最大检查帧为1454字节，最大组合查询响应270字节，分别低于4096/384字节。Word已在设计阶段归档，经WPS后台导出检查；本轮接口外形不变，不另改Word或飞书。实际C及网络证据见[网络验证记录](ota_network_validation_20260916.md)，不以文档测试代替固件测试。

接入后必须测试：半包/粘包、严格JSON/重复键、各方向、最大块/空块/超长、负数/小数/bool/溢出偏移、末块非对齐、重复begin不清进度、最后块重发同内容/不同内容、乱序旧块、写入部分失败、断线与无活动取消、坏CRC/板型/版本、finish应答丢失、提交成功但返回错误、提交后拒绝取消与新begin、重连查询不重复安装、旧版/稳定版不能冒报0903、W25读取失败、30秒确认前后以及心跳/旧业务回归。

本轮已通过A2→A1安装（SWD提交下载包）及A1→A2安装（真实TCP传包）。最大包时延、真实掉电/3+3恢复及B1/B2维护配合仍需实板验收。软测通过不更新其真板状态。
