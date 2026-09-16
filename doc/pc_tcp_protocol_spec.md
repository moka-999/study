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
| 0x0901 | CMD_MCU_OTA_REQUEST | PC→MCU：固件升级操作请求 |
| 0x0902 | CMD_MCU_OTA_RESP | MCU→PC：升级操作响应及状态 |
| 0x0903 | CMD_MCU_OTA_COMPLETE | MCU→PC：目标固件已健康运行 |

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
| 0x0901 OTA操作 | 0x0902；带目标包查询且健康确认完成时为0x0903 | 0x0006 failed |

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
  `invalidState`、`busy`、`notReady`、`notConfigured`、`executionFailed`。
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

上述字段均必填，`power`必须完整携带内存类型和三路档位，不能省略字段以继承上次值。
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
开始测试的0x0006在成功解析`algorithmIndex`后也原样带回该字段；编号缺失或非法时不伪造编号。
PC以对应index的约定最终结果推进计划，不能因`accepted`或启动`completed`就切换下一项；
SLT由PC/RK报告通路判断该项是否完成。
PC串行提交开始测试，尚有活动测试时不覆盖配置，新开始请求返回`busy`，不得借Seq关联并行测试。

0x0506和0x0509先保留为独立调试/兼容入口，正常新测试流程不要求PC拆包调用。
0x0509仍是原有类型/运行期拉偏语义，不替代本命令的训练电压，也不修改其现有包体。

**固件接入状态（2026-09-14）**：以上为本次更新的协议格式。V2/B1当前0x0103仍只解析
`algorithmName/algorithmIndex`，合法请求无同步成功响应；保留编号后，新包可以通过旧字段校验，
但旧固件不校验/执行算法大类、训练档位和重启要求。带index的执行响应、完整启动流程及新结果格式尚未接入，
不能把接受JSON视为已支持新业务，须待实现后联调。

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
      "resultDesc": "",
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


`testResults`包含各测试对象的结果；`siteId`和`mirrorIndex`先保留现有字段及类型，具体业务含义和编号规则后续统一规定，不在本框架中绑定物理测试位或软件镜像。`resultDesc`为每项结果的UTF-8字符串描述，无描述时填空字符串`""`，不省略字段或使用`null`。`testStatus`为测试状态码，具体取值由业务定义；描述不替代状态码。

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
成功返回`completed`；本命令不隐含立即重启。

### 4.16 查询RK引导模式（0x0507）

请求固定为`{}`。查询最近一次成功写入的配置；从未成功设置时返回`notConfigured`。
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
- 三个steps：相对该类型标称电压的有符号整数档位，`0`表示标称值，每档20mV；
- 字段名和字符串大小写敏感，四个字段必须全部存在且不能附带其他字段。

| 内存类型 | VDD1 steps | VDD2 steps | VDDQ steps |
|---|---:|---:|---:|
| LPDDR4 | -50..77 | -35..28 | -35..28 |
| LPDDR4X | -50..77 | -35..28 | -10..53 |

电源`off`时只接受三路steps全0以选择下一次上电类型；电源`on`时校验当前类型
和档位后执行三轨调压。PC必须在RK完成DDR初始化后下发非零档位；MCU侧
DDR-ready强制门待双方握手冻结。范围校验、事务互斥、三轨写序和故障回滚已实现。
上述边界只是TPS可精确编码范围，不代表DDR安全范围，禁止自动遍历极端档位。

### 4.19 查询电源采样结果（0x0601）

请求包体固定为`{}`。MCU只读取监测任务最近发布的缓存快照，不在TCP任务中即时访问
PAC1934、AD7124或TPS。

### 4.20 电源采样结果响应（0x0602）

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

三个数组顺序固定为VDD1、VDD2、VDDQ。`validMask` bit0表示`voltageMv`有效，
bit1表示`currentMa`和`powerMw`有效；无效组整体省略。`sampleAgeMs`表示快照年龄，
`samplePowerState`为采样时状态。电压已应用B1前端1.01换算，真实标定仍待B1真板。

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

### 4.26 MCU固件升级请求（0x0901）

沿用现有连接、帧头和JSON，PC发送一个`.ota`文件；MCU自行选择安装区。以下五种操作共用0x0901，包体中的action区分操作，每次等待响应后再发送下一次OTA请求。

开始下载：

```json
{"action":"begin","packageId":"5A1B2C3D","packageSize":139452}
```

发送数据块：

```json
{"action":"write","packageId":"5A1B2C3D","offset":0,"data":"T1RBMQ=="}
```

完整文件发送后申请安装并重启：

```json
{"action":"finish","packageId":"5A1B2C3D"}
```

查询目标包的升级结果；查询一般状态时省略packageId：

```json
{"action":"query","packageId":"5A1B2C3D"}
```

取消尚未提交的下载：

```json
{"action":"cancel","packageId":"5A1B2C3D"}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| action | string | begin开始、write写块、finish提交并重启、query查询、cancel取消 |
| packageId | string | 包头CRC32，8位大写十六进制，不带0x；非Seq，同一文件保持不变 |
| packageSize | integer | `.ota`总字节数，116..2097152；begin必填 |
| offset | integer | 文件原始字节偏移，从0开始；write必填 |
| data | string | 独立块的标准Base64，无换行、有必要填充；解码后1..1024字节 |

PC从文件偏移36..39读取小端u32包头CRC，并转为packageId；从偏移44/46/48读取三个小端u16组成目标版本major.minor.patch。文件数据原样编码传输，不随网络帧头转换字节序。示例ID、长度和版本仅演示格式；T1RBMQ==只有4字节，不能代替完整升级文件。

每个操作只发送示例列出的字段。PC通常每块发送1024字节，末块发送剩余字节，完整帧仍≤4096字节。升级前停止测试、使RK/DDR安全下电，并单独协调B2维护，避免因B1保活中断而复位或断电；MCU仍独立检查升级条件。

### 4.27 MCU固件升级响应（0x0902）

begin成功返回块上限和起始偏移；重复相同begin不会清空已收进度：

```json
{"action":"begin","packageId":"5A1B2C3D","status":"completed","state":"receiving","blockSize":1024,"nextOffset":0}
```

write成功后返回本块offset和下一块nextOffset；PC按action、packageId及offset匹配，不按Seq匹配：

```json
{"action":"write","packageId":"5A1B2C3D","status":"completed","state":"receiving","offset":0,"nextOffset":4}
```

nextOffset按已写入并读回验证的原始字节计数。仅允许上一成功块以完全相同内容重发；其他错位或不同内容返回失败，不得自行跳过未确认数据。

finish受理返回以下内容，随后MCU自动重启；accepted不是升级成功：

```json
{"action":"finish","packageId":"5A1B2C3D","status":"accepted","state":"ready"}
```

一般状态查询响应示例：

```json
{"action":"query","status":"completed","state":"confirmed","activePackageId":"11223344","firmwareVersion":"0.1.2","confirmed":true}
```

query请求含packageId时响应也带回。activePackageId、firmwareVersion、confirmed表示实际运行包、固件版本及本次启动健康确认；旧镜像无有效包标识时activePackageId为null。下载中另带download对象，字段为packageId、receivedBytes、totalBytes；没有下载时省略。

state为receiving下载中、ready已提交、commitUnknown提交结果不确定、trial启动待确认、confirmed当前固件健康、cancelled取消或failed未提交下载失败。cancel成功回0902、status=completed、state=cancelled；已提交后禁止取消。

业务失败返回0x0006，requestCommandId=2305，并带已合法解析的action/packageId；write可带offset和仍有效的nextOffset。例如：

```json
{"requestCommandId":2305,"status":"failed","action":"write","packageId":"5A1B2C3D","offset":1024,"nextOffset":0,"errorCode":"offsetMismatch"}
```

errorCode沿用invalidArgument、invalidDirection、notSupported、invalidState、busy、notReady；OTA补充offsetMismatch偏移错误、dataMismatch重复块内容不同、packageInvalid包结构错误、targetMismatch板型不符、versionRejected版本不允许、crcMismatch校验失败、flashError存储错误、commitUnknown提交不确定。commitUnknown须等待复位后查询，不能按普通失败立即重传。

### 4.28 MCU固件升级完成（0x0903）

PC重连后发送query及目标packageId。只有该包正在运行、已通过本次启动30秒健康确认且没有活动升级，MCU才直接返回0x0903；否则返回0x0902当前状态，不主动推送完成通知。

```json
{"action":"query","packageId":"5A1B2C3D","status":"completed","state":"confirmed","firmwareVersion":"0.1.3","confirmed":true}
```

PC核对packageId和目标固件版本后显示成功。可重复查询，不需要额外ACK；firmwareVersion不是测试仪硬件版本testerVersion。若返回旧版/稳定版则显示“未运行目标版本”，仅凭断连或超时不能认定回滚。

PC操作规则：

1. 普通请求等待5秒，finish等待30秒；同一连接最多重发3次，业务字段不变，Seq照常递增。
2. 未提交时断连/复位或30秒无下载活动，下载取消，重连从0开始；query、心跳和重复begin不延长下载超时。
3. finish后断连，即使没收到受理响应，也先查询结果，禁止盲目重新下载。ready和commitUnknown后禁止新下载及取消。
4. 每2秒重连/查询；安装期间B无网络服务。PC等待600秒仍无结果时显示“结果未确认”，不自动切电源或强制复位。
5. 仅维护升级，普通路径拒绝同版本和降版；B、稳定版及板卡信息不由PC升级命令修改。CRC用于检测损坏，不作为身份认证。

<!-- 内部接入状态留在Markdown，生成对外Word时不包含此段。 -->
2026-09-16：启用完整OTA的V2固件已接入0901/0902/0903，并通过0.1.3→0.1.4真实TCP升级；未启用OTA的普通镜像回notSupported。详细合同见[OTA协议设计](pc_ota_protocol.md)，验收边界见[实测记录](ota_network_validation_20260916.md)。在线文档只读核对版本29，未修改。

## 5. 业务命令补充格式

PC工程师完善其他命令时，每条命令按以下内容补充即可：

1. 命令名称、编号和发送方向。
2. 请求JSON及字段说明：名称、类型、单位、取值范围。
3. 响应JSON及对应命令编号；结果上报与请求的关联方式。
4. 成功/失败状态、错误码，以及必要的超时和重试规则。

心跳周期、测试结果上报方式等连接级规则集中补充，避免在每条命令中重复。
