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

已有编号沿用 `CommandEnum1.cs`，保留原枚举名称；本框架新增0x0006通用应答和0x0509 DDR配置入口，供双方同步枚举。

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
| 0x0505 | CMD_RESET_RK | PC→MCU：RK安全断电重启 |
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

原枚举中的0x0201～0x0204属于PC–B2温控接口，不列入本B1协议。新增命令按业务分组追加，不改变已有编号。

没有专用响应的控制命令统一使用0x0006，区分已接受、已完成和失败；已接受不表示硬件操作完成，快速操作可直接返回已完成或失败。已有查询命令成功时仍使用各自的专用响应，命令不支持、参数错误、设备忙或执行失败等业务错误使用通用应答。非法长度、版本或Flag直接关闭连接，不作为业务错误应答。

通用应答的JSON字段和错误码按第5节补充，至少说明对应命令及执行阶段；头部Seq始终使用MCU自己的发送计数，不回显请求值。



### 3.1 请求与响应对应关系

| PC请求 | 成功时MCU响应 | 业务失败时MCU响应 |
|---|---|---|
| 0x0002 心跳 | 0x0001 + `{}` | 0x0006 failed；帧/JSON语法失步则断连 |
| 0x0101 查询测试仪 | 0x0102 测试仪信息 | 0x0006 failed |
| 0x0103 开始测试 | 接入后0x0006 accepted/completed；测试结果0x0105，当前状态见4.7 | 0x0006 failed |
| 0x0501 上电 | 0x0006 accepted；已ON为completed | 0x0006 failed |
| 0x0502 下电 | 0x0006 accepted/completed | 0x0006 failed |
| 0x0503 查询电源 | 0x0504 电源状态 | 0x0006 failed |
| 0x0505 重启RK | 0x0006 accepted/completed（RK安全确认接入后） | 0x0006 failed |
| 0x0506 设置Boot模式 | 0x0006 completed | 0x0006 failed |
| 0x0507 查询Boot模式 | 0x0508 Boot模式信息 | 0x0006 failed |
| 0x0509 设置DDR配置 | 0x0006 completed | 0x0006 failed |
| 0x0601 查询电源采样 | 0x0602 采样快照 | 0x0006 failed |
| 0x0701 设置结果灯 | 0x0006 completed | 0x0006 failed |
| 0x0702 查询结果灯 | 0x0703 结果灯状态 | 0x0006 failed |

0x0006不是“错误命令”。它是通用命令执行结果，必须结合JSON中的`status`判断：
`accepted`表示非阻塞操作已受理，`completed`表示操作已完成，`failed`才表示失败。
若固定头、UTF-8或JSON语法本身非法，MCU直接关闭连接，不发送0x0006。


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
  `invalidState`、`busy`、`notReady`、`notConfigured`、`executionFailed`；
  `otaNotReady`表示当前固件尚未完成OTA启动健康确认，暂不允许相关控制。
  成功应答不含`errorCode`。

同步控制成功示例：

```json
{
  "requestCommandId": 1289,
  "status": "completed"
}
```

已通过固定头和严格JSON语法校验后的参数错误、未支持命令或本地执行失败使用0x0006，
保持TCP连接。非法Length、Version、Flag、ServiceID、UTF-8或JSON语法仍属于协议失步并断连。

### 4.6 查询测试仪信息及响应（0x0101 / 0x0102）

请求命令0x0101，包体：

```json
{}
```

响应命令0x0102，包体示例：

```json
{
  "testerName": "LPDDR4/X_200B",
  "testerVersion": "D00",
  "testBoard": "RK3588_test"
}
```

字段分别表示测试仪名称、测试仪硬件版本、测试板名称。连接建立时不主动推送，由PC查询后返回。当前仅规定基本JSON格式，后续所需的固件版本等信息可在本响应JSON中追加，不必另增查询命令；新增字段不改变已有字段含义，接收端应允许扩展字段，整包仍不得超过4096字节。

### 4.7 开始测试（0x0103）

三类测试共用0x0103，一次提交算法、训练电压和启动要求，不另拆开始测试命令。
请求包体示例：

```json
{
  "algorithmType": "parametric",
  "algorithmName": "idd_0",
  "algorithmIndex": 1,
  "power": {
    "memoryType": "LPDDR4X",
    "vdd1Steps": 0,
    "vdd2Steps": 0,
    "vddqSteps": 0
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
| `power.vdd1Steps` | integer | VDD1相对标称电压的有符号档位，每档20mV |
| `power.vdd2Steps` | integer | VDD2相对标称电压的有符号档位，每档20mV |
| `power.vddqSteps` | integer | VDDQ相对标称电压的有符号档位，每档20mV |
| `reboot` | boolean | PC是否主动要求重新启动；必须是`true`或`false` |
| `bootMode` | string | 本次启动模式，沿用0x0506的`emmc`、`fspiM0`等取值，见4.15 |

上述六个根字段均必填，`power`必须完整携带内存类型和三路档位，不能省略字段以继承上次值。
根对象及`power`均不接受重复键或额外字段；`algorithmIndex`范围为0..4294967295。
字符串及字段名区分大小写；档位必须是整数，不能用字符串、小数或布尔值代替。
算法只用“大类＋名称”标识，同一大类内名称必须唯一并按PC/RK共同约定的算法表匹配；
不再单列小类。`algorithmIndex`标识计划中的算法项，不是算法固定ID或RK命令号：
同一算法在计划中出现多次时使用不同index，PC用它对齐结果并推进计划，MCU不自行递增或重排。
MCU到RK所需的内部算法编号由业务映射，不改变RK协议；协议头Seq仍是独立发送流水号，不回显请求Seq。

训练目标计算为`targetMv = nominalMv + steps * 20`。
标称基准按VDD1/VDD2/VDDQ顺序：LPDDR4为1800/1100/1100mV，LPDDR4X为1800/1100/600mV。
`0`表示标称值，`1`表示标称值＋20mV，`-1`表示标称值－20mV；这是相对固定标称值的目标档位，
不是对当前电压再加减，重复发送`1`不会累加。`0`也不表示“保持当前电压”。
标称基准不随ADC读数或校准结果漂移；测量/输出校准属于驱动，不能改变线上档位含义。
PC和MCU均须校验换算后的安全电压范围、三轨组合及板级限制，不能用TPS寄存器全范围代替。
发生重启时统一受控断电再上电，不使用RK软件复位、不复位B1 MCU，因此不再单列重启类型。

执行规则：

1. `power`表示本次DDR初始化和训练期间的电压。MCU先校验完整请求；安全范围或时序尚未配置时拒绝启动，不先开电再检查。
2. MCU先把三路档位换算为目标mV，再将`memoryType+三路目标mV`与上一份成功生效且当前仍有效的配置比较，不比较JSON文本或仅比较上次请求。任一项不同必须冷重启，即使PC发`reboot=false`；两端判断不一致以MCU为准。三路档位相同但内存类型改变，也不能省略重启。
3. `bootMode`改变、配置记录未知或已失效，也必须重新建立启动状态。只有RK已就绪、配置仍有效且电源和Boot均未改变时，`reboot=false`才可复用当前启动；`reboot=true`则主动重新启动。已OFF时完成受控上电，无须为了“重启”再做一次无效关电。
4. 冷重启流程为：取得RK安全下电确认→关RK/DDR并满足放电窗口→在OFF设置Boot→重新上电并在训练前写入、稳定三轨目标→确认RK就绪→交接测试。不能改为先在标称值训练、再把本次`power`写进去。
5. 仅完成I2C写入或电源状态到`on`，不能记为训练成功。调压/启动失败或MCU复位后，不得沿用成功记录；运行期拉偏改变实际供电后，也不能按旧记录跳过重新建立目标的过程。

B1目前按“TPS配置及稳定能赶在RK训练前完成”的开发预设推进，时间预算与电气波形仍需实板验收。
`bootMode`选择启动模式，不等于PXE开关；SLT的MemTest86配置和报告上传走PC/RK既定通路，B1不代管温控或报告文件。

接入后的响应约定：受理返回0x0006 `accepted`，`requestCommandId=259`；
完成启动准备并确认测试启动交接后返回0x0006 `completed`，只表示本次启动命令完成，不表示测试PASS。
准备或启动失败返回0x0006 `failed`，不继续执行算法；由MCU转报的测试结果仍使用0x0105。
开始测试的0x0006只要根对象中`algorithmIndex`唯一且为合法u32整数，就原样带回该字段，
即使其他字段无效也保留该编号；编号缺失、重复、类型错误或越界时不伪造编号。
PC以对应index的约定最终结果推进计划，不能因`accepted`或启动`completed`就切换下一项；
SLT由PC/RK报告通路判断该项是否完成。
PC串行提交开始测试，尚有活动测试时不覆盖配置，新开始请求返回`busy`，不得借Seq关联并行测试。

0x0506和0x0509先保留为独立调试/兼容入口，正常新测试流程不要求PC拆包调用。
0x0509仍是原有类型/运行期拉偏语义，不替代本命令的训练电压，也不修改其现有包体。

**固件接入状态（2026-09-21）**：V2/B1已严格解析本节全部字段，缺失、额外、重复或值非法时
返回0x0006 `failed/invalidArgument`。三路档位复用TPS可表示范围检查，不等于训练安全范围已获批准。
完整合法请求在OTA启动健康未确认时返回`failed/otaNotReady`，否则返回`failed/notReady`；
参数校验先于OTA门。响应按上述规则携带合法且唯一的`algorithmIndex`。
当前未接RK测试执行，不修改电源/Boot配置，不发送RK命令，也不产生受理或启动完成响应。
完整启动流程、安全训练白名单及新结果格式仍未接入，不能把参数解析通过当作测试已开始。

### 4.8 停止测试（0x0104）

### 4.9 上报测试结果（0x0105）

命令0x0105，包体示例：

```json
{
  "algorithmType": "parametric",
  "algorithmName": "idd_0",
  "algorithmIndex": 1,
  "testResults": [
    {
      "siteId": "",
      "mirrorIndex": 1,
      "testStatus": 1
    }
  ]
}
```

`algorithmType/algorithmName/algorithmIndex`均沿用对应开始测试请求；名称标识具体算法，
index标识PC当前计划中的算法项，MCU必须原样返回。PC收到该项约定的最终结果后推进计划，
不按名称猜测index，也不能把局部结果当作整项完成。
index不是跨计划/重连的全局唯一ID；当前按4.7节串行执行，残留结果必须按会话/任务边界处理。
这是新结果格式，现有固件构造器仍为名称/编号格式，尚未增加大类。


`testResults`包含各测试对象的结果；`siteId`和`mirrorIndex`先保留现有字段及类型，具体业务含义和编号规则后续统一规定，不在本框架中绑定物理测试位或软件镜像。`testStatus`为测试状态码，具体取值由业务定义。

### 4.10 启动RK及内存电源（0x0501）

请求固定为`{}`。OFF状态且时序已配置时启动非阻塞上电，返回0x0006
`accepted`，PC随后用0x0503轮询；已经ON返回`completed`。当前B1未写入批准时序，
因此安全返回`notConfigured`。DDR按已选类型恢复标称电压，不沿用上一周期拉偏。

### 4.11 关闭RK及内存电源（0x0502）

请求固定为`{}`。OFF状态幂等返回`completed`；ON状态在取得RK安全关机确认前返回
`notReady`，不得直接关轨。安全确认接入后受理反序下电并由0x0503报告进度。

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

0x0501已接入状态机但在批准时序写入前返回`notConfigured`；0x0502仍受RK安全关机确认
门控。状态字段不等同于实测电压，实测结果由0x0601/0x0602提供。

### 4.14 RK安全断电重启（0x0505）

请求固定为`{}`。目标流程是RK安全下电确认→反序关电→最小掉电窗口→重新上电；
不是RK软件重启，也不复位B1 MCU。当前ON状态返回`notReady`，等第1项RK安全确认
接入后开放；OFF状态返回`invalidState`。

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
  "vdd1Steps": 0,
  "vdd2Steps": 0,
  "vddqSteps": 0
}
```

- `memoryType`：固定为`LPDDR4`或`LPDDR4X`；
- `vdd1Steps`、`vdd2Steps`、`vddqSteps`：相对该类型标称电压的有符号整数档位，
  `0`表示标称值，每档20mV，不表示“保持当前电压”；
- 字段名和字符串大小写敏感，四个字段必须全部存在且不能附带其他字段。

可写档位如下，边界来自TPS寄存器可精确编码范围，不代表DDR安全工作范围：

| 内存类型 | VDD1 steps | VDD2 steps | VDDQ steps |
|---|---:|---:|---:|
| LPDDR4 | -50..77 | -35..28 | -35..28 |
| LPDDR4X | -50..77 | -35..28 | -10..53 |

状态规则：

- 电源`off`时只接受三路steps全0，用于选择下一次上电的内存类型，不访问I²C；
- 电源`on`时只能配置当前已选择的类型，三轨按安全顺序同步写入，完成后返回
  0x0006 `completed`；
- `starting`、`stopping`、类型不一致或`off`时带非零steps返回`invalidState`；
- 字段类型、非整数或档位越界返回`invalidArgument`；真实TPS/I²C失败返回
  `executionFailed`并进入电源故障关断。

每次重新上电仍先恢复所选类型的标称值，不沿用上一次拉偏。设置值只是TPS目标，
不等于实测电压；实测由0x0601/0x0602查询。PC必须在RK完成DDR初始化后下发
非零档位；MCU–RK的DDR-ready强制门待双方业务握手冻结。
极端档位可能永久损坏被测器件，禁止自动遍历全范围。

### 4.19 查询电源采样结果（0x0601）

请求包体固定为：

```json
{}
```

本命令只读取监测任务最近发布的缓存快照，不在TCP任务中即时访问PAC1934、
AD7124或TPS。

### 4.20 电源采样结果响应（0x0602）

完整有效示例：

```json
{
  "sampleAgeMs": 37,
  "samplePowerState": "on",
  "validMask": 3,
  "voltageMv": [1801, 1099, 601],
  "currentMa": [312, 145, 88],
  "powerMw": [562, 159, 53]
}
```

- 三个数组顺序固定为`VDD1`、`VDD2`、`VDDQ`；
- `sampleAgeMs`是响应时刻减去采样时刻的无符号毫秒差，可跨MCU tick回绕；
- `samplePowerState`是产生该快照时的电源状态，不是另一次实时读取；
- `validMask` bit0=三路`voltageMv`有效，bit1=三路`currentMa`和`powerMw`
  有效；
- 未置位的测量组在JSON中整体省略，不能用全零数组表示“无效”。部分有效仍正常
  返回0x0602；启动后尚无任何快照时返回0x0006 `executionFailed`。

`voltageMv`来自AD7124并已按B1的100R+10k前端换算；`currentMa`和`powerMw`
来自PAC1934，B1 2134版三路按20mΩ换算；V2保留10mΩ验证基线。PC直接使用mA/mW结果，
不再乘除电阻系数，线上字段不变。真实零点、增益和电阻误差仍需B1真板标定。TPS STATUS属于读清事件，
首版不把它作为“当前健康状态”发送。

### 4.21 设置结果灯状态（0x0701）

```json
{
  "target": "lpddr0",
  "state": "pass"
}
```

| 字段 | 允许值 | 说明 |
|---|---|---|
| target | `lpddr0` / `lpddr1` / `emcp` | 三组测试对象；不表示GPIO或LED编号 |
| state | `off` | 两通道关闭 |
| state | `pass` | PASS通道常亮 |
| state | `fail` | FAIL通道常亮 |
| state | `testNormal` | 两通道500ms亮/500ms灭同步闪烁 |
| state | `testHot` | FAIL/警示通道500ms亮/500ms灭闪烁 |

请求必须恰好包含`target`和`state`。设置完成返回0x0006 `completed`；参数错误
返回`invalidArgument`，GPIO业务驱动不可用返回`executionFailed`。线协议不绑定颜色，
当前B1板PASS为蓝色、FAIL/警示为琥珀色，物理顺序只由板级映射决定。

### 4.22 查询结果灯状态（0x0702）

请求固定为`{}`。查询的是三组业务状态，不是闪烁周期内瞬时亮灭电平。

### 4.23 结果灯状态响应（0x0703）

```json
{
  "lpddr0": "off",
  "lpddr1": "pass",
  "emcp": "testHot"
}
```

三个值与0x0701的`state`取值完全相同。PC不得向MCU发送0x0602或0x0703；
反向发送响应命令返回0x0006 `invalidDirection`。

### 4.24 查询B1–B2保活状态（0x0801）

### 4.25 B1–B2保活状态响应（0x0802）

### 4.26 MCU固件升级请求（0x0901）

PC发送一个`.ota`文件，按begin/write/finish/query/cancel动作传输；每个write的data为最多3000字节原始内容的Base64字符串，且不超过begin返回的blockSize（旧版可能为1024）。使用紧凑JSON，Base64字符不额外转义，发送前检查18字节头加UTF-8正文≤4096字节；末块不补零。MCU须已健康确认且电源OFF。完整字段及示例见[OTA协议](pc_ota_protocol.md)。B1与V2包不能混用。

### 4.27 MCU固件升级响应（0x0902）

操作成功及进度查询返回0902；字段/状态/校验失败返回0006并携带requestCommandId=2305。finish的accepted仅表示已提交安装，不表示升级完成。

### 4.28 MCU固件升级完成（0x0903）

重启重连后PC携带目标packageId查询，仅当目标镜像已运行并完成30秒任务健康确认才返回0903；不依赖主动推送，不复用请求Seq。

## 5. 业务命令补充格式

PC工程师完善其他命令时，每条命令按以下内容补充即可：

1. 命令名称、编号和发送方向。
2. 请求JSON及字段说明：名称、类型、单位、取值范围。
3. 响应JSON及对应命令编号；结果上报与请求的关联方式。
4. 成功/失败状态、错误码，以及必要的超时和重试规则。

心跳周期、测试结果上报方式等连接级规则集中补充，避免在每条命令中重复。
