# PC–MCU TCP通信协议框架

本文作为PC与MCU双方完善通信协议的基础，规定连接方式、报文头和命令编号，提供常用JSON示例。各业务的包体字段、返回值和错误码按文末格式补充。

## 1. 通信方式

- PC为TCP Client，B1 MCU为TCP Server，服务端口为8888；每块B1一次只服务一个PC连接。
- PC主动连接MCU；连接后由PC发送查询或控制命令，MCU返回响应或测试结果。
- 包体使用UTF-8 JSON对象；无参数时发送 `{}`。
- PC–MCU与MCU–RK是两套独立协议，PC侧不处理RK的串口帧或序号。
- 温控由PC直接与B2通信，B1不执行或转发温控命令。

## 2. 报文格式

多字节整数统一使用网络字节序（大端），固定头部18字节。

```text
[Length:4][Version:2][Flag:2][ServiceID:2]
[CommandID:2][Seq:4][Reserved:2][JSON Body:N]
```

| 字段 | 字节数 | 说明 |
|---|---|---|
| Length | 4 | 整包长度，包含18字节头和JSON包体，最大4096字节 |
| Version | 2 | 协议版本，初版为1 |
| Flag | 2 | 包体格式，JSON使用8 |
| ServiceID | 2 | 初版不使用，固定填0；保留字段宽度，不参与命令分发 |
| CommandID | 2 | 命令编号，见下表 |
| Seq | 4 | 发送流水号；PC和MCU分别递增，不回显请求Seq，与RK序号无关 |
| Reserved | 2 | 保留字段，发送时填0 |
| JSON Body | N | UTF-8编码的JSON对象 |

Length按实际发送的UTF-8字节数计算：`Length = 18 + JSON字节数`，包体最多4078字节。JSON不带注释、尾随逗号或字符串结束符 `\0`。

接收端按Length累计和拆分数据，支持半包、粘包和连续多包；不能把一次TCP接收当成一个完整报文。

每条TCP连接中，PC和MCU分别维护自己的发送Seq，每发送一个应用协议帧，本端Seq加1；接收对方报文不改变本端发送计数。心跳、应答和结果上报共用本端的发送计数，不回显或借用对方Seq。两个方向允许出现相同数值，双方可以同时发送，不要求轮流发包；TCP底层分段和重传不产生新Seq。

每次建立新TCP连接后，双方首个发送帧的Seq为1；完整PDU成功提交发送后才推进计数。
连接断开后丢弃旧计数，新连接重新从1开始。Seq按u32自然递增，`0xFFFFFFFF`之后
回绕到0，接收端不得把0视为无效值。发送失败时当前连接关闭，不在新连接复用旧计数。

Seq只表示本端发送顺序，不作为请求与应答的匹配依据。控制应答通过业务内容说明对应命令；同类命令并发、多次执行同一算法及分批结果的关联方式，由业务包体或串行执行规则明确，不能仅凭Seq或算法编号判断。

## 3. 命令编号

已有编号沿用 `CommandEnum1.cs`；本框架新增0x0006通用应答和0x0509 DDR配置入口，0901–0903枚举名称统一为固件升级，供双方同步枚举。

| 编号 | 枚举名称 | 用途 |
|---|---|---|
| 0x0000 | None | 未指定命令 |
| 0x0001 | CMD_SERVER_HEART_BEAT | MCU→PC：心跳响应 |
| 0x0002 | CMD_CLIENT_HEART_BEAT | PC→MCU：心跳请求 |
| 0x0003 | CMD_LOGIN_REQ | PC→MCU：登录请求 |
| 0x0004 | CMD_LOGIN_RESP | MCU→PC：登录响应 |
| 0x0005 | CMD_LOGOUT | PC→MCU：登出 |
| 0x0006 | CMD_COMMAND_RESP | MCU→PC：通用命令执行结果（成功、受理或失败） |
| 0x0101 | CMD_QUERY_TESTER_INFO | PC→MCU：查询测试仪信息 |
| 0x0102 | CMD_TESTER_INFO_RESP | MCU→PC：测试仪信息响应 |
| 0x0103 | CMD_START_TEST | PC→MCU：开始测试 |
| 0x0104 | CMD_STOP_TEST | PC→MCU：停止测试 |
| 0x0105 | CMD_TEST_RESULT_REPORT | MCU→PC：测试结果上报 |
| 0x0501 | CMD_POWER_ON | PC→MCU：启动RK及内存电源 |
| 0x0502 | CMD_POWER_OFF | PC→MCU：关闭RK及内存电源 |
| 0x0503 | CMD_QUERY_POWER_STATUS | PC→MCU：查询电源状态 |
| 0x0504 | CMD_POWER_STATUS_RESP | MCU→PC：电源状态响应 |
| 0x0505 | CMD_RESET_RK | PC→MCU：RK断电重启 |
| 0x0506 | CMD_SET_BOOT_MODE | PC→MCU：设置RK引导模式 |
| 0x0507 | CMD_QUERY_BOOT_MODE | PC→MCU：查询引导模式 |
| 0x0508 | CMD_BOOT_MODE_RESP | MCU→PC：引导模式响应 |
| 0x0509 | CMD_SET_DDR_CONFIG | PC→MCU：配置LPDDR类型及三轨目标电压 |
| 0x0601 | CMD_QUERY_POWER_MONITOR | PC→MCU：查询电压、电流等采样结果 |
| 0x0602 | CMD_POWER_MONITOR_RESP | MCU→PC：电源采样结果响应 |
| 0x0701 | CMD_SET_RESULT_INDICATOR | PC→MCU：设置结果灯状态 |
| 0x0702 | CMD_QUERY_INDICATOR_STATUS | PC→MCU：查询结果灯状态 |
| 0x0703 | CMD_INDICATOR_STATUS_RESP | MCU→PC：结果灯状态响应 |
| 0x0801 | CMD_QUERY_B2_HEARTBEAT_STATUS | PC→MCU：查询B1–B2保活状态 |
| 0x0802 | CMD_B2_HEARTBEAT_STATUS_RESP | MCU→PC：B1–B2保活状态响应 |
| 0x0901 | CMD_FIRMWARE_UPGRADE_REQUEST | PC→MCU：固件升级操作请求 |
| 0x0902 | CMD_FIRMWARE_UPGRADE_RESP | MCU→PC：升级操作响应及状态 |
| 0x0903 | CMD_FIRMWARE_UPGRADE_COMPLETE | MCU→PC：目标固件已健康运行 |

原枚举中的0x0201～0x0204属于PC–B2温控接口，不列入本B1协议。新增命令按业务分组追加，不改变已有编号。

没有专用响应的控制命令统一使用0x0006，区分已接受、已完成和失败；已接受不表示硬件操作完成，快速操作可直接返回已完成或失败。已有查询命令成功时仍使用各自的专用响应，命令不支持、参数错误、设备忙或执行失败等业务错误使用通用应答。非法头前缀有界丢弃，不回应其中的命令；恢复后可继续处理正常帧。

通用应答的JSON字段和错误码按第5节补充，至少说明对应命令及执行阶段；头部Seq始终使用MCU自己的发送计数，不回显请求值。



### 3.1 请求与响应对应关系

| PC请求 | 成功时MCU响应 | 业务失败时MCU响应 |
|---|---|---|
| 0x0002 心跳 | 0x0001 + `{}` | 完整帧的JSON/参数错误回0x0006 failed |
| 0x0101 查询测试仪 | 0x0102 测试仪信息 | 0x0006 failed |
| 0x0103 开始测试 | 0x0006 accepted；完整启动准备另回completed；最终结果0x0105，适用条件见4.7 | 0x0006 failed |
| 0x0501 上电 | 0x0006 accepted→completed；已ON直接completed | 0x0006 failed |
| 0x0502 下电 | 0x0006 accepted/completed | 0x0006 failed |
| 0x0503 查询电源 | 0x0504 电源状态 | 0x0006 failed |
| 0x0505 重启RK | 0x0006 accepted→completed（本轮RK启动心跳确认后完成） | 0x0006 failed |
| 0x0506 设置Boot模式 | 0x0006 completed | 0x0006 failed |
| 0x0507 查询Boot模式 | 0x0508 Boot模式信息 | 0x0006 failed |
| 0x0509 设置DDR配置 | 0x0006 completed | 0x0006 failed |
| 0x0601 查询电源采样 | 0x0602 最新采样及统计结果 | 0x0006 failed |
| 0x0701 设置结果灯 | 0x0006 completed | 0x0006 failed |
| 0x0702 查询结果灯 | 0x0703 结果灯状态 | 0x0006 failed |
| 0x0901 OTA操作 | 0x0902；带目标包查询且健康确认完成时为0x0903 | 0x0006 failed |

0x0006不是“错误命令”。它是通用命令执行结果，必须结合JSON中的`status`判断：
`accepted`表示非阻塞操作已受理，`completed`表示操作已完成，`failed`才表示失败。
固定头和完整长度有效、但UTF-8或JSON非法时，回0x0006 `failed/invalidArgument`，不执行该请求并保持连接。无有效头的垃圾字节不回应。


## 4. 指令与JSON包体

### 4.1 心跳（0x0001 / 0x0002）

PC发送0x0002，MCU返回0x0001；双方包体均为 `{}`，各自使用本端递增的Seq，不回显对方Seq。

### 4.2 登录请求（0x0003）

### 4.3 登录响应（0x0004）

### 4.4 登出（0x0005）

### 4.5 通用命令执行结果（0x0006）

失败应答格式：

```json
{
  "requestCommandId": 1283,
  "status": "failed",
  "errorCode": "invalidArgument"
}
```

- `requestCommandId`：触发本执行结果的PC请求命令号，使用十进制无符号整数；
- `status`：失败为`failed`；非阻塞请求已受理为`accepted`；同步控制完成为`completed`；
- `errorCode`：当前固定`invalidArgument`、`invalidDirection`、`notSupported`、
  `invalidState`、`busy`、`notReady`、`notConfigured`、`executionFailed`、`timeout`、`txFailed`；
  `otaNotReady`表示当前固件尚未完成OTA启动健康确认，暂不允许相关控制。
  成功应答不含`errorCode`。

同步控制成功示例：

```json
{
  "requestCommandId": 1289,
  "status": "completed"
}
```

完整坏JSON、参数错误、未支持命令或本地执行失败使用0x0006，保持TCP连接。
非法Length、Version、Flag或ServiceID会触发有界重新找头；累计丢弃超过4096字节仍未恢复时断连。
尚未收齐的半帧若超过500ms没有新字节，仍会断连；已知长度的正文不会被拆成其他命令执行。
PC应始终发送正确Length；连接失步后需重新连接，不能依赖容错保证找回所有被错误Length吞入的后续数据。

### 4.6 查询测试仪信息及响应（0x0101 / 0x0102）

请求命令0x0101，包体：

```json
{}
```

响应命令0x0102，包体示例：

```json
{
  "testerName": "JMCT-4000",
  "testerVersion": "D00",
  "testBoard": "RK3588_test"
}
```

字段分别表示测试仪名称、测试仪硬件版本、测试板名称。连接建立时不主动推送，由PC查询后返回。当前仅规定基本JSON格式，后续所需的固件版本等信息可在本响应JSON中追加，不必另增查询命令；新增字段不改变已有字段含义，接收端应允许扩展字段，整包仍不得超过4096字节。

### 4.7 开始测试（0x0103）

#### 外部已启动的功能测试

RK已由操作者启动并进入对应算法服务时，使用下列包体。MCU不改电压、不选启动介质、不重启RK；B1额外要求MCU电源状态为ON，真实RK负责执行算法。

```json
{"algorithmType":"functional","algorithmName":"mats","algorithmIndex":1,"power":null,"reboot":false,"bootMode":null}
```

| 字段/响应 | 规则 |
|---|---|
| `power` / `bootMode` | 必须同时为null，表示保持外部状态；不是要求0V或标称电压 |
| `reboot` | 必须false；本模式不能夹带重启要求 |
| `algorithmName` | 精确匹配RK命令表1..20，例如mats、gtest-xor；未知算法/SLT/IDD不下发 |
| `timeoutMs` | 可选整数，省略或0表示不限算法等待；非零1..86400000毫秒 |
| `0x0006 accepted` | 请求已进入MCU的RK队列，不证明RK已收到/开始，不表示PASS |
| `0x0105` | 一个算法的一个整体最终结果；PC以algorithmIndex关联并推进计划 |
| 通信失败 | 0x0006 failed，含txFailed、timeout等；不是芯片FAIL |

本模式不另发启动completed。算法在途时重复开始、变更电源/Boot/电压及升级写操作返回busy，心跳/查询仍可用。断网不停止RK，也不把旧结果发给重连的新会话；旧算法结束前仍busy。默认无限等待时，若RK没有回最终结果，MCU不会自行解除busy或重发。

每算法上报一个包含两颗DDR的最终结果包，testStatus逐颗为0=FAIL、1=PASS；同一siteId和共轨测量放顶层，颗粒编号为mirrorIndex=0/1。完整字段与无效数据规则见4.9节。

#### 完整B1训练电压/启动流程

三类测试共用0x0103，一次提交算法、训练电压和启动要求，不另拆开始测试命令。
请求包体示例：

```json
{
  "algorithmType": "parametric",
  "algorithmName": "idd_0",
  "algorithmIndex": 1,
  "power": {
    "memoryType": "LPDDR4X",
    "vdd1": "1800",
    "vdd2": "1100",
    "vddq": "600"
  },
  "reboot": true,
  "bootMode": "emmc"
}
```

| 字段 | JSON类型 | 含义 |
|---|---|---|
| `algorithmType` | string | 测试大类：`functional`功能测试、`slt`系统级测试、`parametric`参数测试 |
| `algorithmName` | string | 具体算法名称，例如`idd_0` |
| `algorithmIndex` | integer | PC当前测试计划中算法项的编号，沿用u32类型；由PC提供，MCU保留并在响应/结果中原样带回 |
| `power.memoryType` | string | `LPDDR4`或`LPDDR4X` |
| `power.vdd1` | string | VDD1绝对目标电压，单位mV，例如`"1800"`，须符合10mV步进 |
| `power.vdd2` | string | VDD2绝对目标电压，单位mV，例如`"1100"`，须符合5mV步进 |
| `power.vddq` | string | VDDQ绝对目标电压，单位mV，例如LPDDR4X的`"600"`，须符合5mV步进 |
| `reboot` | boolean | PC是否主动要求重新启动；必须是`true`或`false` |
| `bootMode` | string | 本次启动模式，沿用0x0506的`emmc`、`fspiM0`等取值，见4.15 |

上述六个根字段均必填，可另带timeoutMs；本完整B1流程中`power`必须完整携带内存类型和三路电压，不能省略字段以继承上次值。
根对象及`power`均不接受重复键或未定义字段；`algorithmIndex`范围为0..4294967295。
字符串及字段名区分大小写；电压必须是非空ASCII十进制数字串，不能用JSON数字、符号、空格、小数、指数或单位后缀代替。
算法只用“大类＋名称”标识，同一大类内名称必须唯一并按PC/RK共同约定的算法表匹配；
不再单列小类。`algorithmIndex`标识计划中的算法项，不是算法固定ID或RK命令号：
同一算法在计划中出现多次时使用不同index，PC用它对齐结果并推进计划，MCU不自行递增或重排。
MCU到RK所需的内部算法编号由业务映射，不改变RK协议；协议头Seq仍是独立发送流水号，不回显请求Seq。

电压必须满足`valMv = nominalMv + stepMv * n`，`n`为整数；VDD1/VDD2/VDDQ的stepMv分别为10/5/5。
标称基准按VDD1/VDD2/VDDQ顺序：LPDDR4为1800/1100/1100mV，LPDDR4X为1800/1100/600mV。
PC直接发送绝对mV，不发送`n`或steps。例如LPDDR4X高一档填写`"1810"/"1105"/"605"`，重复发送不会累加。
MCU严格检查原生步进及可编码范围，不自动取整；`"1801"`不是合法VDD1值，`"0"`也不表示“保持当前电压”。
电压含义不随ADC读数或校准结果漂移。旧`vdd1Steps/vdd2Steps/vddqSteps`字段不再接受，PC须与新固件配套。
PC负责确认具体DDR的安全电压范围、三轨组合及板级限制；MCU校验字符串、内存类型、TPS可编码范围和原生步进，不另设训练安全白名单。合法可写不代表DDR安全，不能把TPS全范围直接作为PC可选范围。
发生重启时统一受控断电再上电，不使用RK软件复位、不复位B1 MCU，因此不再单列重启类型。

执行规则：

1. `power`表示本次DDR初始化和训练期间的电压。MCU先校验完整请求，非法参数在操作硬件前拒绝；PC必须先完成安全范围和三轨组合检查。
2. MCU先将三路电压字符串解析为mV，再将`memoryType+三路目标mV`与上一份成功生效且当前仍有效的配置比较，不比较JSON文本或仅比较上次请求。任一项不同必须冷重启，即使PC发`reboot=false`；两端判断不一致以MCU为准。三路电压相同但内存类型改变，也不能省略重启。
3. `bootMode`改变、配置记录未知或已失效，也必须重新建立启动状态。只有RK已就绪、配置仍有效且电源和Boot均未改变时，`reboot=false`才可复用当前启动；`reboot=true`则主动重新启动。已OFF时完成受控上电，无须为了“重启”再做一次无效关电。
4. 冷重启流程为：直接关RK/DDR→按配置等待→在OFF设置Boot→重新上电并写入三轨训练目标→等待本轮有效RK普通串口心跳→交接测试。不发送关机TLV、不等待安全下电应答；PC/RK须提前保证可直接切电，尤其不能仍在写存储。不能改为先在标称值训练、再把本次`power`写进去。
5. 仅完成I2C写入或电源状态到`on`，不能记为训练成功。调压/启动失败或MCU复位后，不得沿用成功记录；运行期拉偏改变实际供电后，也不能按旧记录跳过重新建立目标的过程。

B1目前按“TPS配置及稳定能赶在RK训练前完成”的开发预设推进，时间预算与电气波形仍需实板验收。
本轮有效普通串口心跳作为RK初始化完成的项目约定；RK应在可接收算法后再发心跳，PC的TCP心跳不参与此判断。等待启动心跳超时返回`failed/timeout`，不自动重启或重发测试，PC应先查询状态；该等待不等于算法运行超时。
`bootMode`选择启动模式，不等于PXE开关；SLT的MemTest86配置和报告上传走PC/RK既定通路，B1不代管温控或报告文件。

接入后的响应约定：受理返回0x0006 `accepted`，`requestCommandId=259`；
完成启动准备且算法进入MCU发送队列后返回0x0006 `completed`，不保证RK已执行，更不表示测试PASS。
准备或启动失败返回0x0006 `failed`，不继续执行算法；由MCU转报的测试结果仍使用0x0105。
开始测试的0x0006只要根对象中`algorithmIndex`唯一且为合法u32整数，就原样带回该字段，
即使其他字段无效也保留该编号；编号缺失、重复、类型错误或越界时不伪造编号。
PC以对应index的约定最终结果推进计划，不能因`accepted`或启动`completed`就切换下一项；
SLT由PC/RK报告通路判断该项是否完成。
PC串行提交开始测试，尚有活动测试时不覆盖配置，新开始请求返回`busy`，不得借Seq关联并行测试。

0x0506和0x0509先保留为独立调试/兼容入口，正常新测试流程不要求PC拆包调用。
0x0509仍是类型/运行期拉偏语义，不替代本命令的训练电压；电压字段同样改为字符串mV。

**完整B1流程接入状态（2026-10-13）**：正式代码已装载初版可调时序并绑定本轮RK心跳确认，保持MCU启动后电源默认OFF；尚未进行B1实板验收。
参数非法返回`invalidArgument`，OTA健康未确认返回`otaNotReady`，已有控制/测试在途返回`busy`；驱动失败和阶段超时分别报错，不作为芯片FAIL。异常构建缺配置或适配器时仍保留`notConfigured/notReady`防御检查，不代表当前B1配置为空。
当前只接功能算法1..20；以上parametric示例仅展示统一结构，parametric/slt仍回`notSupported`。外部已启动模式及V2模拟路径不变；V2未启用真实电源编排，不能把其联调响应作为B1电气验收。

### 4.8 停止测试（0x0104）

### 4.9 上报测试结果（0x0105）

每个算法上报一个结果包。顶层`siteId`标识本次B1，`measurementData`为两颗共轨的共享测量；`testResults`固定包含两颗DDR，按`mirrorIndex=0、1`排序。PC收到当前`algorithmIndex`的完整结果后再推进计划；跨计划或重连不能只凭index认领结果。

#### 4.9.1 统一结果结构

以下为功能测试示例：第一颗FAIL，第二颗PASS；编号、地址、耗时和测量值仅作示意。

```json
{
  "siteId": "1",
  "algorithmType": "functional",
  "algorithmName": "mats",
  "algorithmIndex": 1,
  "testTimeMs": 42000,
  "testResults": [
    {
      "mirrorIndex": 0,
      "testStatus": 0,
      "resultDesc": {"firstErrorAddress": "0x0000000012345678", "errorType": "SAF"},
      "physicalAddr": ""
    },
    {
      "mirrorIndex": 1,
      "testStatus": 1,
      "resultDesc": {"firstErrorAddress": null, "errorType": ""},
      "physicalAddr": ""
    }
  ],
  "measurementData": {
    "Vdd1Ave": 1800, "Vdd1Peak": 1810, "Vdd1Min": 1790,
    "Vdd2Ave": 1100, "Vdd2Peak": 1110, "Vdd2Min": 1090,
    "VddqAve": 600, "VddqPeak": 610, "VddqMin": 590,
    "Idd1Ave": 300, "Idd1Peak": 330, "Idd1Min": 270,
    "Idd2Ave": 140, "Idd2Peak": 155, "Idd2Min": 125,
    "IddqAve": 85, "IddqPeak": 95, "IddqMin": 70
  }
}
```

#### 4.9.2 字段及测量规则

| 位置/字段 | 类型 | 含义 |
|---|---|---|
| `siteId` | string | 本次B1板标识，未配置为空字符串；两颗共用 |
| `algorithmType` | string | 测试大类，与请求一致；目前接入functional，parametric/slt结果细节后续约定 |
| `algorithmName` | string | 算法名称，与请求一致 |
| `algorithmIndex` | int | PC测试计划项编号，与请求一致 |
| `testTimeMs` | uint64/null | 整体算法耗时ms；FUNC取RK原值，不含PC等待；未提供时null |
| `testResults` | array | 固定两项，依次为第一颗和第二颗DDR |
| ↳ `mirrorIndex` | int | 0=第一颗、1=第二颗；RK失败明细按0x3001匹配 |
| ↳ `testStatus` | int | 本颗结论：0=FAIL，1=PASS |
| ↳ `resultDesc` | object | 本颗结果说明，子字段由algorithmType决定 |
| ↳ `resultDesc.firstErrorAddress` | string/null | FUNC失败首错地址：0x加16位十六进制；FAIL地址0保留完整零地址，PASS为null |
| ↳ `resultDesc.errorType` | string | FUNC错误类型，如SAF；PASS或RK未提供类型时为空字符串 |
| ↳ `physicalAddr` | string | 保留，固定为空字符串 |
| `measurementData` | object/null | 顶层共享统计，不重复放到两颗结果里；字段为数字或null，不含state |

`Vdd1/Vdd2/Vddq`为三轨电压mV，`Idd1/Idd2/Iddq`为三轨电流mA；各加`Ave/Peak/Min`后缀，共18项，分别为有效样本平均值、样点最大值和最小值。共轨电流为两颗合计，不代表单颗电流；采样峰值不保证捕获瞬态尖峰。无效字段为null，完全无数据时整个measurementData为null。

新测试投递算法前清零，结束后冻结；启动准备等待不计入统计，查询不清零。采样异常不作为芯片FAIL。三类可复用外层结构；Parametric和SLT的resultDesc子字段暂不定义。

### 4.10 启动RK及内存电源（0x0501）

请求固定为`{}`。B1在OFF且无在途冲突时，返回0x0006 `accepted`；
真实上电状态到ON后另回`completed`，也可用0x0503查询；已经ON直接`completed`。
使用已保存的训练目标，未单独设置时为标称，不沿用运行期拉偏。完成不表示RK训练就绪。
启动过程中驱动错误或阶段超时返回`failed`，PC应查询当前状态，不自动重发上电。

V2显式电源模拟沿用原行为：上电请求回`accepted`后用0503查询虚拟状态，不追加真实路径的异步`completed`；不能以此验收真实电源。

### 4.11 关闭RK及内存电源（0x0502）

请求固定为`{}`。OFF直接返回`completed`。ON且无在途冲突时先回`accepted`，
直接调用下电驱动，到OFF另回`completed`；不发送RK关机命令或等待RK允许掉电。
PC/RK须先保证可直接切电；算法/维护在途仍回`busy`，不能用本命令取消正在执行的算法。驱动错误或阶段超时回`failed`；超时不表示硬件动作被撤销，应查询状态。均使用0x0006。

### 4.12 查询电源状态（0x0503）

请求包体固定为：

```json
{}
```

### 4.13 电源状态响应（0x0504）

最小响应示例：

```json
{
  "powerState": "off"
}
```

`powerState`只表示MCU电源状态机的逻辑状态，不代表ADC已经确认实际电压。固定取值：

- `off`：系统电源关闭；
- `starting`：PC6/PC7上电及TPS初始化过程中；
- `on`：状态机已完成所选目标电压写入，不代表RK训练成功；
- `stopping`：PC6/PC7反向下电过程中；
- `fault`：初始化或调压失败，状态机已执行故障关断。

等待RK启动心跳时电源可能已经为`on`。
因此不能用本状态代替操作的最终0x0006或RK-ready；实测结果由0x0601/0x0602提供。

### 4.14 RK断电重启（0x0505）

请求固定为`{}`。B1电源ON且无在途冲突时，先回`accepted`，执行直接关电→
断电等待→保留Boot和训练目标重新上电→本轮有效RK普通串口心跳，成功另回`completed`。
PC/RK须先保证可直接切电；不是RK软件重启，也不复位B1 MCU。OFF时回`invalidState`；
测试/维护在途回`busy`。失败回`failed`，不自动重试；
等待RK-ready超时不强制切电源，PC应查询状态并处理，不把超时视为算法FAIL。

### 4.15 设置RK引导模式（0x0506）

请求示例：

```json
{"bootMode":"emmc"}
```

`bootMode`固定为`usbMaskrom`、`sdCard`、`emmc`、`fspiM0`、`fspiM1`、
`fspiM2`或`emmcSd`。仅RK电源OFF时通过MCP4725 Fast Write设置，不写EEPROM，
成功返回`completed`；本命令不隐含立即重启。实际I2C写入失败返回`executionFailed`并使
原Boot配置缓存失效；参数错误、设备忙或状态拒绝发生在写入前，不清除原配置。

### 4.16 查询RK引导模式（0x0507）

请求固定为`{}`。查询最近一次仍有效的成功写入配置；从未成功设置，或之后一次实际I2C
写入失败导致配置失效时，返回`notConfigured`。重新成功设置后恢复查询。
该值不等同于确认RK实际从对应介质启动。

### 4.17 引导模式响应（0x0508）

```json
{"bootMode":"emmc","targetMv":600}
```

`targetMv`是原理图电阻表对应的目标DAC电压，不是实测VOUT。

### 4.18 配置LPDDR类型及三轨目标电压（0x0509）

请求固定为四个字段：

```json
{
  "memoryType": "LPDDR4X",
  "vdd1": "1800",
  "vdd2": "1100",
  "vddq": "600"
}
```

- `memoryType`：固定为`LPDDR4`或`LPDDR4X`；
- `vdd1/vdd2/vddq`：绝对电压mV的十进制字符串，校验规则与0103的power相同，不自动取整；
- 字段名和字符串大小写敏感，四个字段必须全部存在且不能附带其他字段。

| 电源轨 | 可编码电压（mV） | 原生步进（mV） | LPDDR4标称（mV） | LPDDR4X标称（mV） |
|---|---:|---:|---:|---:|
| VDD1 | 800..3350 | 10 | 1800 | 1800 |
| VDD2 | 400..1675 | 5 | 1100 | 1100 |
| VDDQ | 400..1675 | 5 | 1100 | 600 |

各轨均有256档：VDD1为800～3350mV，VDD2/VDDQ为400～1675mV。

电源`off`时只接受所选类型的三路标称电压以选择下一次上电类型；电源`on`时校验当前类型
和电压后执行三轨调压。PC必须在RK完成DDR初始化后下发偏离标称的电压；MCU侧
DDR-ready强制门待双方握手冻结。范围校验、事务互斥、三轨写序和故障回滚已实现。
上述边界只是TPS可精确编码范围，不代表DDR安全范围，禁止自动遍历极端档位。

### 4.19 查询电源采样结果（0x0601）

请求包体固定为`{}`。MCU只读取监测任务最近发布的缓存快照，不在TCP任务中即时访问
PAC1934、AD7124或TPS；同时返回已有统计，不启动、清零或重置统计。

### 4.20 电源采样结果响应（0x0602）

```json
{
  "sampleAgeMs": 37,
  "samplePowerState": "on",
  "validMask": 3,
  "voltageMv": [1801, 1099, 601],
  "currentMa": [312, 145, 88],
  "powerMw": [562, 159, 53],
  "statistics": {
    "state": "running",
    "voltageAvgMv": [1800, 1100, 600],
    "voltagePeakMv": [1810, 1110, 610],
    "voltageMinMv": [1790, 1090, 590],
    "currentAvgMa": [300, 140, 85],
    "currentPeakMa": [330, 155, 95],
    "currentMinMa": [270, 125, 70]
  }
}
```

| 字段名 | 类型 | 含义 |
|---|---|---|
| sampleAgeMs | integer | 最新快照年龄，单位ms；过期快照不能视为即时测量 |
| samplePowerState | string | 采样时的电源状态 |
| validMask | integer | 仅表示快照有效性：bit0为voltageMv，bit1为currentMa和powerMw；无效组整体省略，不表示统计是否完整 |
| voltageMv | number[3] | 最新电压，单位mV |
| currentMa | number[3] | 最新电流，单位mA |
| powerMw | number[3] | 最新功率，单位mW |
| statistics | object / null | 本次测量阶段累计结果；尚未开始时为null |
| statistics.state | string | 统计状态，见下表 |
| statistics.voltageAvgMv | (number / null)[3] | 平均电压，单位mV |
| statistics.voltagePeakMv | (number / null)[3] | 采样最大电压，单位mV |
| statistics.voltageMinMv | (number / null)[3] | 采样最小电压，单位mV |
| statistics.currentAvgMa | (number / null)[3] | 平均电流，单位mA |
| statistics.currentPeakMa | (number / null)[3] | 采样最大电流，单位mA |
| statistics.currentMinMa | (number / null)[3] | 采样最小电流，单位mA |

| statistics.state | 含义 |
|---|---|
| running | 统计中，数值仍会变化 |
| complete | 统计正常完成，不代表芯片PASS |
| timeout | 窗口结束但有效样本不足，可能保留部分统计 |
| cancelled | 取消、下电或调压等导致中断，可能保留部分统计 |

- 所有数组顺序固定为VDD1、VDD2、VDDQ；使用JSON数字，无有效样本的统计元素为JSON `null`，不能用0代替。
- MCU已完成电流/功率换算，PC直接使用mA/mW，不再乘除电阻系数。
- 不上传统计年龄、样本数量或窗口时长；样本数量与有效性由MCU内部维护。清零、有效测量阶段及冻结规则见4.9.2节。

<!-- 以下为内部换算记录，不同步到Word。 -->
电压已应用B1前端1.01换算，真实标定仍待B1真板。
B1 2134版三路分流电阻按20mΩ换算；V2保留10mΩ验证基线。

### 4.21 设置结果灯状态（0x0701）

```json
{
  "target": "lpddr0",
  "state": "pass"
}
```

`target`仅为`lpddr0`、`lpddr1`、`emcp`；`state`仅为`off`、`pass`、`fail`、
`testNormal`、`testHot`。设置完成返回0x0006 `completed`。协议不暴露物理颜色、
GPIO或LED编号。

### 4.22 查询结果灯状态（0x0702）

请求固定为`{}`；查询业务状态，不查询闪烁周期中的瞬时亮灭电平。

### 4.23 结果灯状态响应（0x0703）

```json
{
  "lpddr0": "off",
  "lpddr1": "pass",
  "emcp": "testHot"
}
```

三个值与0x0701的`state`取值一致。PC反向发送0x0602或0x0703时，MCU返回
0x0006 `invalidDirection`。
### 4.24 查询B1–B2保活状态（0x0801）

### 4.25 B1–B2保活状态响应（0x0802）

### 4.26 固件升级请求（0x0901）

沿用现有连接、帧头和JSON。PC发送一个`.ota`文件，由MCU选择安装区；以下五种`mcu_*`操作共用0x0901，每次等待响应后再发送下一次请求。新固件不接受旧无前缀动作。

RK仅预留`rk_begin/rk_write/rk_finish/rk_query/rk_cancel`，当前统一返回0x0006 `failed/notSupported`并带回action，不下载、不擦写、不重启。RK文件和包体格式先不定义；以下字段、示例及完成条件仅适用于MCU。

开始下载：

```json
{"action":"mcu_begin","packageId":"5A1B2C3D","packageSize":139452}
```

发送数据块：

```json
{"action":"mcu_write","packageId":"5A1B2C3D","offset":0,"data":"T1RBMQ=="}
```

完整文件发送后申请安装并重启：

```json
{"action":"mcu_finish","packageId":"5A1B2C3D"}
```

查询目标包的升级结果；查询一般状态时省略packageId：

```json
{"action":"mcu_query","packageId":"5A1B2C3D"}
```

取消尚未提交的下载：

```json
{"action":"mcu_cancel","packageId":"5A1B2C3D"}
```

| 字段名 | 类型 | 含义 |
|---|---|---|
| action | string | mcu_begin开始、mcu_write写块、mcu_finish提交并重启、mcu_query查询、mcu_cancel取消 |
| packageId | string | 包头CRC32，8位大写十六进制，不带0x；非Seq，同一文件保持不变。除mcu_query可省略外，其余动作必填 |
| packageSize | integer | `.ota`总字节数，116..2097152（2MiB）；仅mcu_begin必填 |
| offset | integer | 文件原始字节偏移，从0开始；mcu_write必填 |
| data | string | mcu_write必填；独立块的标准Base64，无换行、有必要填充，解码后1..3000字节 |

PC从文件偏移36..39读取小端u32包头CRC作为packageId；从偏移44/46/48读取三个小端u16组成目标版本major.minor.patch。文件数据原样编码传输，不随网络帧头转换字节序。示例ID、长度和版本仅演示格式；T1RBMQ==只有4字节，不能代替完整升级文件。

- 每个操作只发送示例列出的字段；mcu_query的一般状态查询可省略packageId。
- 每块原始数据不超过mcu_begin返回的blockSize（当前3000，旧版可能为1024），末块按剩余长度发送、不补零。
- 使用紧凑JSON，Base64字符不额外转义；UTF-8正文加18字节头须≤4096字节，3000字节块的最大正常整帧为4090字节。
- 升级前停止测试、使RK/DDR安全下电，并单独协调B2维护，避免因B1保活中断而复位或断电；MCU仍独立检查升级条件。

### 4.27 固件升级响应（0x0902）

mcu_begin成功：

```json
{"action":"mcu_begin","packageId":"5A1B2C3D","status":"completed","state":"receiving","blockSize":3000,"nextOffset":0}
```

mcu_write成功：

```json
{"action":"mcu_write","packageId":"5A1B2C3D","status":"completed","state":"receiving","offset":0,"nextOffset":4}
```

mcu_finish受理，随后MCU自动重启；accepted不是升级成功：

```json
{"action":"mcu_finish","packageId":"5A1B2C3D","status":"accepted","state":"ready"}
```

一般状态查询响应示例：

```json
{"action":"mcu_query","status":"completed","state":"confirmed","activePackageId":"11223344","firmwareVersion":"0.1.2","confirmed":true}
```

| 字段名 | 类型 | 含义 |
|---|---|---|
| action | string | 对应请求动作 |
| packageId | string | 对应请求包；mcu_query请求带该字段时响应也带回，不代表实际运行包 |
| status | string | completed为本次操作完成；accepted仅为安装已受理 |
| state | string | 当前升级/启动状态，见下表 |
| blockSize | integer | mcu_begin返回的单块原始字节上限 |
| offset | integer | mcu_write确认的本块原始字节偏移 |
| nextOffset | integer | 已连续写入并读回验证的原始字节数，也是下一块起点 |
| activePackageId | string / null | mcu_query返回的实际运行包标识；旧镜像无有效标识时为null |
| firmwareVersion | string | 实际运行固件版本，格式major.minor.patch |
| confirmed | boolean | 实际运行固件是否通过本次启动健康确认 |
| download | object | 仅mcu_query且state=receiving时返回的下载进度；其他状态省略，不是固件文件内容 |
| download.packageId | string | 正在下载的包标识 |
| download.receivedBytes | integer | 已接收的原始字节数 |
| download.totalBytes | integer | 下载文件总字节数 |

| state | 含义 |
|---|---|
| receiving | 下载中，按nextOffset继续 |
| ready | 已提交，等待自动重启安装 |
| commitUnknown | 提交结果不确定，等待复位后查询，不能重新下载 |
| trial | 本次启动尚待健康确认 |
| confirmed | 当前固件健康，不单独表示目标包升级成功 |
| cancelled | 下载已取消 |
| failed | 未提交下载失败 |

- PC按action、packageId匹配响应，mcu_write还须匹配offset；不按Seq匹配。
- 重复相同mcu_begin不清空已收进度。仅上一成功块允许以完全相同内容重发；其他错位或不同内容返回失败，不得跳过未确认数据。
- mcu_cancel成功回0x0902、status=completed、state=cancelled；已提交后禁止取消。

业务失败返回0x0006，例如：

```json
{"requestCommandId":2305,"status":"failed","action":"mcu_write","packageId":"5A1B2C3D","offset":1024,"nextOffset":0,"errorCode":"offsetMismatch"}
```

| 字段名 | 类型 | 含义 |
|---|---|---|
| requestCommandId | integer | 固定2305，即0x0901 |
| status | string | 固定failed |
| action / packageId | string | 仅带回已合法解析的字段 |
| offset | integer | mcu_write可带回本块偏移 |
| nextOffset | integer | mcu_write可带回仍有效的下一块偏移 |
| errorCode | string | 错误码，见下表 |

| errorCode | 含义 |
|---|---|
| invalidArgument / invalidDirection / notSupported / invalidState / busy / notReady | 沿用通用参数、方向、支持能力、状态、忙碌及未就绪错误 |
| offsetMismatch | 偏移错误 |
| dataMismatch | 重复块内容不同 |
| packageInvalid | 包结构错误 |
| targetMismatch | 板型不符 |
| versionRejected | 版本不允许 |
| crcMismatch | 校验失败 |
| flashError | 存储错误 |
| commitUnknown | 提交结果不确定，等待复位后查询，不能按普通失败立即重传下载 |

### 4.28 固件升级完成（0x0903）

本节仅定义MCU的完成判定，RK完成上报暂未定义。

PC重连后发送mcu_query及目标packageId。只有该包正在运行、已通过本次启动30秒健康确认且没有活动升级，MCU才直接返回0x0903；否则返回0x0902当前状态，不主动推送完成通知。

```json
{"action":"mcu_query","packageId":"5A1B2C3D","status":"completed","state":"confirmed","firmwareVersion":"0.1.3","confirmed":true}
```

| 字段名 | 类型 | 含义 |
|---|---|---|
| action | string | 固定mcu_query |
| packageId | string | 请求查询且已健康运行的目标包标识 |
| status | string | 固定completed |
| state | string | 固定confirmed |
| firmwareVersion | string | 实际运行固件版本，须与目标文件一致；不是硬件版本testerVersion |
| confirmed | boolean | 固定true，已通过本次启动健康确认 |

PC核对packageId和目标固件版本后显示成功。可重复查询，无需额外ACK；若返回旧版/稳定版则显示“未运行目标版本”，仅凭断连或超时不能认定回滚。

| 操作 | PC规则 |
|---|---|
| 等待与重发 | 普通请求等待5秒，mcu_finish等待30秒；同一连接最多重发3次，业务字段不变，Seq照常递增 |
| 未提交下载中断 | 断连/复位或30秒无下载活动即取消，重连从0开始；mcu_query、心跳和重复mcu_begin不延长超时 |
| mcu_finish后断连 | 即使未收到受理响应，也先查询结果，禁止盲目重新下载；ready和commitUnknown后禁止新下载及取消 |
| 重连与查询 | 每2秒重连/查询，安装期间B无网络服务；等待600秒仍无结果显示“结果未确认”，不自动切电源或强制复位 |
| 升级边界 | 仅维护升级，普通路径拒绝同版本和降版；不修改B、稳定版及板卡信息。CRC用于检测损坏，不作为身份认证 |

<!-- 内部接入状态留在Markdown，生成对外Word时不包含此段。 -->
2026-09-24：本地文档采用新action并预留RK入口；新前缀不是旧网络验收结果，当前固件实现与验证见任务清单。

历史记录2026-09-17：V2/B1单块上限更新为3000字节，完整帧仍≤4096；V2已通过真实TCP升级至0.1.6及健康确认，B1只完成构建/离线验证。未启用OTA的普通镜像回notSupported。详细合同见[OTA协议](pc_ota_protocol.md)，验收边界见[本次记录](ota_chunk_3000_validation_20260917.md)。线上文档本轮未修改。

## 5. 业务命令补充格式

PC工程师完善其他命令时，每条命令按以下内容补充即可：

1. 命令名称、编号和发送方向。
2. 请求JSON及字段说明：名称、类型、单位、取值范围。
3. 响应JSON及对应命令编号；结果上报与请求的关联方式。
4. 成功/失败状态、错误码，以及必要的超时和重试规则。

心跳周期、测试结果上报方式等连接级规则集中补充，避免在每条命令中重复。
