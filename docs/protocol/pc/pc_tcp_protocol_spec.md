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
| 0x0006 | CMD_COMMAND_RESP | MCU→PC：通用命令应答 |
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

## 4. 指令与JSON包体

### 4.1 心跳（0x0001 / 0x0002）

PC发送0x0002，MCU返回0x0001；双方包体均为 `{}`，各自使用本端递增的Seq，不回显对方Seq。

### 4.2 登录请求（0x0003）

### 4.3 登录响应（0x0004）

### 4.4 登出（0x0005）

### 4.5 通用命令应答（0x0006）

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

### 4.7 下发测试算法（0x0103）

请求命令0x0103，包体示例：

```json
{
  "algorithmName": "algorithmName",
  "algorithmIndex": 1
}
```

`algorithmName`为算法名称，`algorithmIndex`为算法编号。

### 4.8 停止测试（0x0104）

### 4.9 上报测试结果（0x0105）

命令0x0105，包体示例：

```json
{
  "algorithmName": "algorithmName",
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

`testResults`包含各测试对象的结果；`siteId`和`mirrorIndex`先保留现有字段及类型，具体业务含义和编号规则后续统一规定，不在本框架中绑定物理测试位或软件镜像。`testStatus`为测试状态码，具体取值由业务定义。

### 4.10 启动RK及内存电源（0x0501）

执行RK及内存的上电流程；DDR按已配置的LPDDR类型恢复标称电压后初始化，不沿用上一个电源周期的拉偏值。

### 4.11 关闭RK及内存电源（0x0502）

正常下电先取得RK安全掉电确认，再关闭RK及内存电源；未取得确认不得当作正常下电成功。

### 4.12 查询电源状态（0x0503）

### 4.13 电源状态响应（0x0504）

### 4.14 RK安全断电重启（0x0505）

通过B1的电源控制流程执行RK安全下电、再上电；不是RK软件重启，也不复位B1 MCU。B2异常复位不属于本命令。

### 4.15 设置RK引导模式（0x0506）

设置下一次RK启动所使用的引导模式，不隐含立即重启。

### 4.16 查询RK引导模式（0x0507）

查询B1已设置的引导配置，不等同于确认RK当前实际从该介质启动。

### 4.17 引导模式响应（0x0508）

### 4.18 配置LPDDR类型及三轨目标电压（0x0509）

用于选择LPDDR4/LPDDR4X及配置VDD1、VDD2、VDDQ目标电压；类型在下电状态配置，拉偏在内存初始化完成后执行。电压以mV表示，测试档位按20mV步进；设置目标值不等于实测电压，实际采样由0x0601/0x0602查询。每次上电仍先恢复所选类型的标称值，超出器件规格的测试电压存在损坏风险。

### 4.19 查询电源采样结果（0x0601）

### 4.20 电源采样结果响应（0x0602）

### 4.21 设置结果灯状态（0x0701）

### 4.22 查询结果灯状态（0x0702）

### 4.23 结果灯状态响应（0x0703）

### 4.24 查询B1–B2保活状态（0x0801）

### 4.25 B1–B2保活状态响应（0x0802）

## 5. 业务命令补充格式

PC工程师完善其他命令时，每条命令按以下内容补充即可：

1. 命令名称、编号和发送方向。
2. 请求JSON及字段说明：名称、类型、单位、取值范围。
3. 响应JSON及对应命令编号；结果上报与请求的关联方式。
4. 成功/失败状态、错误码，以及必要的超时和重试规则。

心跳周期、测试结果上报方式等连接级规则集中补充，避免在每条命令中重复。
