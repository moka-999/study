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

已有编号沿用 `CommandEnum1.cs`；本框架新增0x0006通用应答和0x0509 DDR配置入口，0x0901–0x0903保留编号并改用通用固件升级枚举名称，供双方同步。

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
| 0x0901 | CMD_FIRMWARE_UPGRADE_REQUEST | PC→MCU：固件升级操作请求 |
| 0x0902 | CMD_FIRMWARE_UPGRADE_RESP | MCU→PC：升级操作响应及状态 |
| 0x0903 | CMD_FIRMWARE_UPGRADE_COMPLETE | MCU→PC：目标固件已健康运行 |

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
| 0x0901 固件升级操作 | 0x0902；带MCU目标包查询且健康确认完成时为0x0903 | 0x0006 failed；RK预留动作回notSupported |

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

### 4.26 固件升级请求（0x0901）

沿用现有连接、帧头和JSON，action前缀区分目标：`mcu_*`升级MCU，`rk_*`预留升级RK。现有MCU流程由PC发送一个`.ota`文件，MCU自行选择安装区；以下五种操作共用0x0901，每次等待响应后再发送下一次请求。新固件不再接受旧无前缀动作。

B1升级包板型固定`b1-h743`，禁止使用V2包或稳定镜像；B1目前仅完成构建/离线验证，尚未真板验收，见[同步记录](../../ota_sync_20260916.md)。

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

| 字段 | 类型 | 说明 |
|---|---|---|
| action | string | mcu_begin开始、mcu_write写块、mcu_finish提交并重启、mcu_query查询、mcu_cancel取消 |
| packageId | string | 包头CRC32，8位大写十六进制，不带0x；非Seq，同一文件保持不变 |
| packageSize | integer | `.ota`总字节数，116..2097152；mcu_begin必填 |
| offset | integer | 文件原始字节偏移，从0开始；mcu_write必填 |
| data | string | 独立块的标准Base64，无换行、有必要填充；解码后1..3000字节，且不超过mcu_begin返回的blockSize |

PC从文件偏移36..39读取小端u32包头CRC，并转为packageId；从偏移44/46/48读取三个小端u16组成目标版本major.minor.patch。文件数据原样编码传输，不随网络帧头转换字节序。示例ID、长度和版本仅演示格式；T1RBMQ==只有4字节，不能代替完整升级文件。

每个操作只发送示例列出的字段。每块原始数据不超过mcu_begin返回的blockSize（当前3000，旧版可能为1024），末块按剩余长度发送、不补零。使用紧凑JSON，Base64字符不额外转义；发送前检查UTF-8正文加18字节头≤4096字节，3000字节块的最大正常整帧为4090字节。升级前停止测试、使RK/DDR安全下电，并单独协调B2维护，避免因B1保活中断而复位或断电；MCU仍独立检查升级条件。

### 4.27 固件升级响应（0x0902）

mcu_begin成功返回块上限和起始偏移；重复相同mcu_begin不会清空已收进度：

```json
{"action":"mcu_begin","packageId":"5A1B2C3D","status":"completed","state":"receiving","blockSize":3000,"nextOffset":0}
```

mcu_write成功后返回本块offset和下一块nextOffset；PC按action、packageId及offset匹配，不按Seq匹配：

```json
{"action":"mcu_write","packageId":"5A1B2C3D","status":"completed","state":"receiving","offset":0,"nextOffset":4}
```

nextOffset按已写入并读回验证的原始字节计数。仅允许上一成功块以完全相同内容重发；其他错位或不同内容返回失败，不得自行跳过未确认数据。

mcu_finish受理返回以下内容，随后MCU自动重启；accepted不是升级成功：

```json
{"action":"mcu_finish","packageId":"5A1B2C3D","status":"accepted","state":"ready"}
```

一般状态查询响应示例：

```json
{"action":"mcu_query","status":"completed","state":"confirmed","activePackageId":"11223344","firmwareVersion":"0.1.2","confirmed":true}
```

mcu_query请求含packageId时响应也带回。activePackageId、firmwareVersion、confirmed表示实际运行包、固件版本及本次启动健康确认；旧镜像无有效包标识时activePackageId为null。下载中另带download对象，字段为packageId、receivedBytes、totalBytes；没有下载时省略。

state为receiving下载中、ready已提交、commitUnknown提交结果不确定、trial启动待确认、confirmed当前固件健康、cancelled取消或failed未提交下载失败。mcu_cancel成功回0902、status=completed、state=cancelled；已提交后禁止取消。

业务失败返回0x0006，requestCommandId=2305，并带已合法解析的action/packageId；mcu_write可带offset和仍有效的nextOffset。例如：

```json
{"requestCommandId":2305,"status":"failed","action":"mcu_write","packageId":"5A1B2C3D","offset":1024,"nextOffset":0,"errorCode":"offsetMismatch"}
```

errorCode沿用invalidArgument、invalidDirection、notSupported、invalidState、busy、notReady；OTA补充offsetMismatch偏移错误、dataMismatch重复块内容不同、packageInvalid包结构错误、targetMismatch板型不符、versionRejected版本不允许、crcMismatch校验失败、flashError存储错误、commitUnknown提交不确定。commitUnknown须等待复位后查询，不能按普通失败立即重传。

### 4.28 固件升级完成（0x0903）

本节仅定义MCU的完成判定，RK完成上报暂未定义。

PC重连后发送mcu_query及目标packageId。只有该包正在运行、已通过本次启动30秒健康确认且没有活动升级，MCU才直接返回0x0903；否则返回0x0902当前状态，不主动推送完成通知。

```json
{"action":"mcu_query","packageId":"5A1B2C3D","status":"completed","state":"confirmed","firmwareVersion":"0.1.3","confirmed":true}
```

PC核对packageId和目标固件版本后显示成功。可重复查询，不需要额外ACK；firmwareVersion不是测试仪硬件版本testerVersion。若返回旧版/稳定版则显示“未运行目标版本”，仅凭断连或超时不能认定回滚。

PC操作规则：

1. 普通请求等待5秒，mcu_finish等待30秒；同一连接最多重发3次，业务字段不变，Seq照常递增。
2. 未提交时断连/复位或30秒无下载活动，下载取消，重连从0开始；mcu_query、心跳和重复mcu_begin不延长下载超时。
3. mcu_finish后断连，即使没收到受理响应，也先查询结果，禁止盲目重新下载。ready和commitUnknown后禁止新下载及取消。
4. 每2秒重连/查询；安装期间B无网络服务。PC等待600秒仍无结果时显示“结果未确认”，不自动切电源或强制复位。
5. 仅维护升级，普通路径拒绝同版本和降版；B、稳定版及板卡信息不由PC升级命令修改。CRC用于检测损坏，不作为身份认证。

<!-- 内部接入状态留在Markdown，生成对外Word时不包含此段。 -->
2026-09-24：本地文档采用新action并预留RK入口；新前缀不是旧网络验收结果，当前固件实现与验证见任务清单。

历史记录2026-09-17：V2/B1单块上限更新为3000字节，完整帧仍≤4096；V2已通过真实TCP升级至0.1.6及健康确认，B1只完成构建/离线验证。未启用OTA的普通镜像回notSupported。详细合同见[OTA协议](pc_ota_protocol.md)，验收边界见[本次记录](../../../../test/doc/ota_chunk_3000_validation_20260917.md)。线上文档本轮未修改。

## 5. 业务命令补充格式

PC工程师完善其他命令时，每条命令按以下内容补充即可：

1. 命令名称、编号和发送方向。
2. 请求JSON及字段说明：名称、类型、单位、取值范围。
3. 响应JSON及对应命令编号；结果上报与请求的关联方式。
4. 成功/失败状态、错误码，以及必要的超时和重试规则。

心跳周期、测试结果上报方式等连接级规则集中补充，避免在每条命令中重复。
