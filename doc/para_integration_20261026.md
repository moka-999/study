# PARA 编号下发与协议准备

日期：2026-10-26。B1/V2源码及本地协议已更新，飞书已同步；V2已安装0.1.39，B1未烧录。本轮未提交或推送仓库。

## 本轮确认

- PC 用 `0108` 开始 PARA，包体沿用 FUNC 七项：`algorithmType/algorithmName/algorithmIndex/TimingParameters/power/reboot/bootMode`；类别固定 `parametric`，`timeoutMs`可选。FUNC/PARA/SLT均必须携带`TimingParameters`对象，默认时序用`{}`，缺失或null拒绝；12项子字段可部分提供字符串，不继承上一请求。
- 与飞书确认的字段一致：`CASReadLatency`和`CASWriteLatency`分别为读、写延迟，可只填一项或同时填不同字符串；旧`CASLatency`、重复键、大小写错误和非字符串回`invalidArgument`。两板解析、RAM保存及三类开始请求回归已通过；未定义RK寄存器下发方式，不将保存解释为已应用时序。
- 16 个名称按 RK 文档大小写匹配，编号 21～36：`idd_0`、`idd_2P`、`idd_2PS`、`idd_2N`、`idd_2NS`、`idd_3P`、`idd_3PS`、`idd_3N`、`idd_3NS`、`idd_4R`、`idd_4W`、`idd_5`、`idd_5AB`、`idd_5PB`、`idd_6`、`idd_6ET`。计划 index 与 RK 算法编号各自独立。
- 本轮只按 FUNC 方式下发算法编号。RK 下行仍为单个 `5000`、Value 两字节小端算法编号，总帧长 22 字节；不自动附加进入/退出或寄存器地址。
- 结果返回 `0109`，类别为 `parametric`，其余容器沿用双颗粒结果：顶层共享 `siteId/testTimeMs/measurementData`，两条 `testResults`。颗粒 PASS/FAIL 来自 RK 完整最终包，不由共轨电流推断。

## 业务流程与代码入口

`0106 {}` 建立本连接整轮 → `0108` 校验类别、名称及参数 → `0006 accepted` → 必要时完成电源/Boot 准备 → 向 RK 投递编号 → 等待有效最终包 → `0109` → 下一算法或 `0107` 收尾。

`0006.requestCommandId=264`，沿用请求的合法 `algorithmIndex`；需要异步启动准备时，准备完成仍用 `0006 completed`。受理和准备完成均不代表测试通过。外部已启动模式仍用 `power:null/reboot:false/bootMode:null`，B1 另要求真实电源 ON。

| 文件 | 职责 |
|---|---|
| `pc_dispatch.c` | 0103/0108 共用开始测试入口，传入实际请求命令号 |
| `pc_test_service.c` | 通道表关联请求、结果、类别；管理电源准备、在途、连接归属及最终交付 |
| `pc_test_message.c` | FUNC/PARA 共用双颗粒 JSON 容器，类别由通道提供 |
| `rk_link.c` | 算法目录增加类别和 21～36 名称；FUNC 与 PARA 不交叉认领 |
| `frame.c/.h` | 保留帧校验，准备动作及 32 位寄存器地址 TLV |

互斥、断线、提前0107、超时/恢复沿用已有规则：不自动重发；未结束的真实 RK 不因 PC 收尾或断线就释放占用；旧连接结果不交付给新连接。默认算法超时仍为0，即无限等待。

## 已准备但尚不发送的 TLV

| 字段 | Type | 长度 | Value | 接入边界 |
|---|---:|---:|---|---|
| `param_action`（tag26） | `0x101A` | 1 | bool，1进入、0退出 | 已有独立 `tlv_pack_idd_action()` 组帧接口，正式任务尚不调用 |
| `param_32bit_addr`（tag24） | `0x7018` | 4 | u32小端寄存器地址 | 通用编解码已校验宽度；地址不隐含读写命令 |

例如地址 `0xFEDCBA98` 的 Value 是 `98 BA DC FE`。Type/Length 仍为大端；新增 bool 不接受2等非布尔值。两项都不是最终结果字段，不能混入当前3/5/7结果包。PC JSON 未新增 action 或地址字段；`TimingParameters` 仍只解析并复制到 RAM，不直接修改 RK 寄存器。

## 验证与待办

- V2/B1 普通固件构建通过；实际 C 离线覆盖 16 个 IDD 映射、类别混用拒绝、264响应关联、0109结果、忙碌、冷启动、断线与提前收尾。
- 时序对象必填回归覆盖三类缺失/null拒绝且无电源或队列副作用、{}及部分字符串合法、空对象替换旧RAM参数和内存分配失败保护；B1实际源码也已复跑。当前20组FUNC、16组PARA示例及有效网络脚本均补TimingParameters:{}，完整帧长度同步增加22字节；历史原始抓包不改写。
- 原 FUNC、V2 电源模拟/MATS 第一颗假 FAIL、B1 真实电源守卫回归通过。B1 同步不含 V2 假 FAIL。
- RK 实际 C 回归覆盖 IDD 编号事务、3/5/7结果、动作0/1、寄存器地址小端与错误宽度、CRC及重复结果；两工程通过。
- 本地 PC Word/Markdown 更新；[16项JSON及网络助手HEX](examples/para_start_packets_20261026.md)逐包核对总长度与JSON。离线通过不代表已与真实 RK IDD 联调。
- 读/写延迟分别保存的回归覆盖只填一项、两项不同、12项全空、旧名/重复键/非字符串拒绝，以及JSON释放后的RAM副本、部分参数不继承和提交失败保留原配置；V2模拟与B1真实电源路径均通过。
- Word使用WPS只读导出22页并检查分页及字段表；两处JSON及字段表已与Markdown一致。飞书保留读/写两个字段，删除“固件解析待同步”的过期提示，更新后读回核对。

## V2安装与TCP验证

V2已通过日常SWD流程从0.1.38/A2升级至0.1.39/A1，W25写入回读、App镜像核对及30秒健康确认通过；电源/测量模拟、MATS第一颗假FAIL保持开启。B1仅构建及离线验证。

实板TCP验证覆盖FUNC、PARA、SLT各11项，共33项。未发送0106建立整轮：合法的空对象、读延迟、写延迟、两项不同和全空字符串对象均通过参数解析，再由整轮门禁返回invalidState；旧CASLatency、数字/null值、错误大小写、缺失或null对象返回invalidArgument。此验证没有投递RK算法，不代表IDD实际执行通过。心跳、W25测试仪信息及0602采样响应正常，验证前后电源均为OFF，连接已关闭。

证据：`build/cas_latency_20261026/network_after.jsonl`及`network_result.json`；安装目录`build/ota-dev/20261026-174936-447910/`。原始日志保存在本机build目录，不替代上面的验证边界。

仍需 RK 确定进入/保持/退出的应答和时点、寄存器读写操作及对应时序字段。当前采样为算法投递至结果领取的全程统计，尚无 IDD 稳定窗口、测试条件与限值判定；共轨测量仍不能分开两颗电流。本轮不修改既有零地址结果语义，该项应与 RK 最新结果合同单独核对。
