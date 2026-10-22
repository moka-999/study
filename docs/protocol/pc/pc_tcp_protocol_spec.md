# PC–MCU TCP通信协议框架

本文规定PC与MCU之间的报文、命令、JSON字段和应答规则。示例中的编号、测量值和版本仅供格式参考。

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

- Length=18+UTF-8包体字节数；接收须支持半包、粘包和连续多包。
- JSON为对象，不带注释、尾随逗号、重复键或结尾NUL；`null`不是字符串`"null"`。
- 每条连接双方发送Seq各自从1开始，完整帧成功提交后加1；u32回绕到0，重连重新从1开始。接收不改变本端计数，允许双向同号。
- Seq只作流水号，不回显、不作业务匹配；关联字段及串行规则见各命令。
- 完整帧的坏JSON回0006 failed/invalidArgument；无法恢复的非法头或半帧空闲超过500ms可断连。正常连接空闲不按半帧超时处理。

## 3. 命令编号

0103/0105用于FUNC开始/结果；0106/0107用于整轮开始/结束及镜像结果灯汇总；0108/0109预留PARA；010A/010B用于整次SLT开始/结果回送。双方按下表使用命令编号。

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
| 0x0103 | CMD_START_FUNC_TEST | PC→MCU：开始一个FUNC算法项 |
| 0x0104 | CMD_STOP_TEST | 保留，未定义为RK取消或整轮结束 |
| 0x0105 | CMD_FUNC_TEST_RESULT_REPORT | MCU→PC：FUNC算法结果 |
| 0x0106 | CMD_START_TEST_ROUND | PC→MCU：整轮开始，包体为{} |
| 0x0107 | CMD_FINISH_TEST_ROUND | PC→MCU：提交失败芯片清单，结束本轮并更新镜像结果灯 |
| 0x0108 | CMD_START_PARA_TEST | PC→MCU：PARA开始，保留 |
| 0x0109 | CMD_PARA_TEST_RESULT_REPORT | MCU→PC：PARA结果，保留 |
| 0x010A | CMD_START_SLT_TEST | PC→MCU：开始整次SLT测试 |
| 0x010B | CMD_SLT_TEST_RESULT_SUBMIT | PC→MCU：回送SLT汇总结果 |
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
| 0x0801 | CMD_QUERY_B2_HEARTBEAT_STATUS | PC→MCU：查询B1–B2保活状态 |
| 0x0802 | CMD_B2_HEARTBEAT_STATUS_RESP | MCU→PC：B1–B2保活状态响应 |
| 0x0901 | CMD_FIRMWARE_UPGRADE_REQUEST | PC→MCU：固件升级操作请求 |
| 0x0902 | CMD_FIRMWARE_UPGRADE_RESP | MCU→PC：升级操作响应及状态 |
| 0x0903 | CMD_FIRMWARE_UPGRADE_COMPLETE | MCU→PC：目标固件已健康运行 |

温控命令属于PC–B2接口，不在本协议中。无专用响应的控制命令使用0006；业务失败统一使用0006 failed。

### 3.1 请求与响应对应关系

| PC请求 | 成功时MCU响应 | 业务失败时MCU响应 |
|---|---|---|
| 0x0002 心跳 | 0x0001 + `{}` | 完整帧的JSON/参数错误回0x0006 failed |
| 0x0101 查询测试仪 | 0x0102 测试仪信息 | 0x0006 failed |
| 0x0103 FUNC开始 | 0x0006 accepted；完整启动准备另回completed；最终结果0x0105 | 0x0006 failed |
| 0x0106 整轮开始 | 0x0006 completed，本轮已开始 | 0x0006 failed |
| 0x0107 整轮结束 | 0x0006 completed，仅表示收尾及灯状态更新完成 | 0x0006 failed |
| 0x0108 PARA开始 | 保留，成功应答及结果包体后续定义 | 未实现时0x0006 failed/notSupported |
| 0x010A SLT开始 | 0x0006 accepted；电源/Boot准备完成另回completed，MCU不上传SLT最终结果 | 0x0006 failed |
| 0x010B SLT结果回送 | 0x0006 completed，确认接收PC汇总结果 | 0x0006 failed |
| 0x0501 上电 | 0x0006 accepted→completed；已ON直接completed | 0x0006 failed |
| 0x0502 下电 | 0x0006 accepted/completed | 0x0006 failed |
| 0x0503 查询电源 | 0x0504 电源状态 | 0x0006 failed |
| 0x0505 重启RK | 0x0006 accepted→completed（本轮RK启动心跳确认后完成） | 0x0006 failed |
| 0x0506 设置Boot模式 | 0x0006 completed | 0x0006 failed |
| 0x0507 查询Boot模式 | 0x0508 Boot模式信息 | 0x0006 failed |
| 0x0509 设置DDR配置 | 0x0006 completed | 0x0006 failed |
| 0x0601 查询电源采样 | 0x0602 最新采样及统计结果 | 0x0006 failed |
| 0x0901 OTA操作 | 0x0902；带目标包查询且健康确认完成时为0x0903 | 0x0006 failed |

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

| 字段 | 类型 | 含义 |
|---|---|---|
| requestCommandId | integer | 对应PC请求命令号，十进制 |
| status | string | accepted=已受理；completed=操作完成；failed=失败 |
| errorCode | string | 仅失败应答携带，见下表 |
| algorithmIndex | integer/null | FUNC带回合法请求index；SLT填null；其他命令省略 |

同步控制成功示例：

```json
{
  "requestCommandId": 1289,
  "status": "completed"
}
```

| errorCode | 含义 |
|---|---|
| invalidArgument / invalidDirection | 参数或发送方向错误 |
| notSupported / invalidState | 命令未支持或当前状态不允许 |
| busy | 已有测试或互斥操作在途 |
| notReady / notConfigured | 设备未就绪或必要配置缺失 |
| otaNotReady | 固件启动健康确认未完成，暂不接受相关控制 |
| executionFailed / txFailed | 执行或发送失败 |
| timeout | 等待超时，不等于芯片FAIL |

accepted及启动completed均不表示算法完成。FUNC请求index无效时不回填伪造编号；OTA附加字段见4.30。

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

| 字段 | 类型 | 含义 |
|---|---|---|
| testerName | string | 测试仪名称 |
| testerVersion | string | 测试仪硬件版本 |
| testBoard | string | 测试板名称 |

仅查询后返回，连接建立时不主动推送；接收端允许后续扩展字段。

### 4.7 FUNC开始测试（0x0103）

PC→MCU，每次提交一个FUNC算法。

```json
{
  "algorithmType": "functional",
  "algorithmName": "mats",
  "algorithmIndex": 1,
  "TimingParameters": {
    "Freq": "",
    "CASLatency": "",
    "tRC": "",
    "tRCD": "",
    "tRFC": "",
    "tRP": "",
    "tRAS": "",
    "tRRD": "",
    "BurstLength": "",
    "ODT": "",
    "tREFI": ""
  },
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

| 字段 | 类型 | 含义 |
|---|---|---|
| algorithmType | string | 固定functional |
| algorithmName | string | 已支持的FUNC算法名，如mats、gtest-xor；区分大小写 |
| algorithmIndex | integer | PC计划项编号0..4294967295；同一轮不同项不复用，响应及结果原样带回 |
| power | object/null | 对象为训练目标；null表示保持外部供电，条件见下文 |
| power.memoryType | string | LPDDR4或LPDDR4X |
| power.vdd1 / vdd2 / vddq | string | 三轨绝对mV，power为对象时全部必填 |
| reboot | boolean | true要求冷重启；false不能绕过配置变化所需重启 |
| bootMode | string/null | 启动介质，取值见4.21；外部已启动模式为null |
| timeoutMs | integer 可选 | 省略或0不限算法等待；非零1..86400000ms；不是启动等待时限 |
| TimingParameters | object/null 可选 | 时序参数，可只提供下表中的部分项；当前仅解析保存，不应用到RK |

原六个根字段必填，TimingParameters和timeoutMs可省略；不接受未知字段。power对象含类型与三路电压，不以缺省字段继承上次值。

TimingParameters子字段均为可选字符串；可传空对象或空字符串。未填项不补默认值、不继承上一请求。单位、有效取值及RK下发方式待双方确认；accepted/completed不表示时序已生效。

| 子字段 | 类型 | 含义 |
|---|---|---|
| Freq | string | 目标频率 |
| CASLatency | string | CAS读写延迟，读写区分方式待确认 |
| tRC | string | 同一bank相邻Activate的最小间隔 |
| tRCD | string | Activate到Read/Write的最小间隔 |
| tRFC | string | 刷新恢复时间 |
| tRP | string | Precharge到Activate的最小间隔 |
| tRAS | string | Activate到Precharge的最小间隔 |
| tRRD | string | 不同bank的Activate最小间隔 |
| BurstLength | string | 突发长度，取值待RK确认 |
| ODT | string | 终端匹配配置，保留 |
| tREFI | string | 刷新间隔 |

#### 电压与重启规则

| 规则 | 要求 |
|---|---|
| 电压格式 | 非空ASCII十进制字符串mV；不含符号、空格、小数、指数或单位，不用JSON数字 |
| 标称值 | VDD1/VDD2/VDDQ：LPDDR4为1800/1100/1100mV，LPDDR4X为1800/1100/600mV |
| 步进和范围 | valMv=nominalMv+stepMv×n，n为整数；步进10/5/5mV，范围见4.24；不取整、不接收旧Steps字段 |
| 安全范围 | PC确认DDR安全电压及三轨组合；MCU校验器件可编码范围和步进。可写不等于DDR安全 |
| 电压用途 | power用于DDR初始化/训练，不是训练后拉偏；运行期调压另用0509 |
| 必须重新启动 | 类型、电压或Boot改变，或有效启动配置未知/失效时；即使reboot=false也执行，以MCU判断为准 |
| 可复用启动 | 配置仍有效、全部不变且RK就绪时可用reboot=false；已OFF则先上电 |
| 断电约束 | 冷重启直接关RK/DDR电源；PC须先确保RK可断电、不在写盘 |

外部已启动模式：RK已进入算法服务、B1电源状态为ON时，以下请求保持电压与启动状态：

```json
{"algorithmType":"functional","algorithmName":"mats","algorithmIndex":1,"power":null,"reboot":false,"bootMode":null}
```

power与bootMode必须同时null、reboot必须false；不能混用部分null或空字符串。

#### 应答与推进

| 场景 | MCU应答 / PC规则 |
|---|---|
| 完整启动 | 0006 accepted → completed；completed只表示启动准备及算法提交完成 |
| 外部已启动 | 0006 accepted；不另发启动completed |
| 最终结果 | 0105；PC收到对应algorithmIndex的结果后再推进下一项 |
| 准备或通信失败 | 0006 failed，不作为芯片FAIL；超时不自动重发或重启 |
| 测试在途 | 新开始、改电源/Boot/电压、升级写回busy；心跳和查询可继续 |
| 断开连接 | 不取消RK；旧结果不补发到新连接，执行未结束前仍可能busy |

0006的requestCommandId=259。合法index原样带回；默认无限等待不表示会自动解除busy。提前收尾见0107。

### 4.8 停止测试（0x0104）

保留编号，不作为本轮整轮结束命令，也不承诺RK取消能力。整轮收尾使用0107；未支持时返回0006 failed/notSupported。

### 4.9 FUNC结果上报（0x0105）

MCU→PC。每个FUNC算法上报一包，当前为两个镜像各一颗DDR的结果；siteId和共轨measurementData在顶层。镜像类似DIMM分组，可包含多颗芯片，physicalAddr用于区分镜像内芯片。PC按当前algorithmIndex推进计划，跨计划或重连不能仅凭index认领。

#### 4.9.1 统一结果结构

以下为功能测试示例：镜像0的芯片FAIL，镜像1的芯片PASS；编号、地址、耗时和测量值仅作示意。

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
| `siteId` | string | 本次B1板标识，未配置为空字符串；本板所有结果共用 |
| `algorithmType` | string | 本命令固定functional；其他类使用各自结果命令，见4.13及4.15 |
| `algorithmName` | string | 算法名称，与请求一致 |
| `algorithmIndex` | int | PC测试计划项编号，与请求一致 |
| `testTimeMs` | uint64/null | 整体算法耗时ms；FUNC取RK原值，不含PC等待；未提供时null |
| `testResults` | array | 当前两项，按镜像0、1排序，各含一颗DDR结果 |
| ↳ `mirrorIndex` | int | 镜像索引，当前为0或1；不是镜像内芯片编号 |
| ↳ `testStatus` | int | 本颗结论：0=FAIL，1=PASS |
| ↳ `resultDesc` | object | 本颗结果说明，子字段由algorithmType决定 |
| ↳↳ `firstErrorAddress` | string/null | FUNC失败首错地址：0x加16位十六进制；FAIL地址0保留完整零地址，PASS为null |
| ↳↳ `errorType` | string | FUNC错误类型，如SAF；PASS或RK未提供类型时为空字符串 |
| ↳ `physicalAddr` | string | 镜像内物理芯片标识；当前每镜像一颗时填空，不是首错内存地址 |
| `measurementData` | object/null | 顶层共享统计，不重复放到两颗结果里；字段为数字或null，不含state |

| 测量项 | 单位 / 规则 |
|---|---|
| Vdd1 / Vdd2 / Vddq | 三轨电压，mV |
| Idd1 / Idd2 / Iddq | 三轨电流，mA；两颗合计，不代表单颗 |
| Ave / Peak / Min | 各项后缀：平均、采样最大、采样最小，共18项；不保证捕获瞬态尖峰 |
| 无有效数据 | 对应值为null；完全无数据时measurementData=null，不用0代替 |
| 统计区间 | 本次算法投递前清零、结束冻结；不计启动准备，查询不清零 |

采样异常不等于芯片FAIL；0107提前结束后的结果不再上报。SLT格式见4.15，PARA保留。

### 4.10 整轮开始（0x0106）

PC→MCU。开始一轮测试，不携带加热属性或灯状态。

```json
{}
```

- 应答：0006 completed，requestCommandId=262；已有整轮或测试占用时回busy。
- 是否加热和测试期间灯态由B1通过B2通信获取，实际加热仍由PC控制B2。
- 默认常温仅为显示初值，不代表已确认B2未加热、已冷却或可触碰。
- 顺序：0106 → 串行FUNC/SLT/PARA → 0107。FUNC等待0105；SLT由PC回送010B并等待确认；PARA的0108/0109当前仅预留。
- 未先完成0106时，FUNC/SLT开始返回0006 failed/invalidState。
- FUNC的algorithmIndex在本轮内区分算法项；SLT的algorithmIndex固定null。
- 本命令不启动算法或切换电源；灯态不是已达温、已冷却或可触碰的判据。

### 4.11 整轮结束（0x0107）

PC→MCU。提交本轮失败芯片清单并结束本轮；只列失败芯片，正常芯片不列入。示例为镜像0和1各有一颗芯片失败。

```json
{
  "errorchip": [
    {
      "physicalAddr": "",
      "mirrorIndex": 0
    },
    {
      "physicalAddr": "",
      "mirrorIndex": 1
    }
  ]
}
```

| 字段 | 类型 | 含义 |
|---|---|---|
| errorchip | array | 必填，每项对应一颗失败芯片；全部通过时填空数组[] |
| ↳ physicalAddr | string | 镜像内物理芯片标识；当前每镜像一颗时可填空，多颗时填写具体标识 |
| ↳ mirrorIndex | integer | 所属镜像索引，沿用整数；同一镜像多颗失败时可重复该索引，由physicalAddr区分 |

- 应答：0006 completed，requestCommandId=263。
- 每个镜像对应一盏结果灯；清单中出现的镜像置FAIL，其余镜像置PASS。`{"errorchip":[]}`表示全部镜像置PASS。
- 当前映射：0→第一组LPDDR灯、1→第二组LPDDR灯、2→eMCP灯；其他编号整包回invalidArgument。
- PC须提交完整汇总；MCU完整校验清单后统一更新结果灯，报文错误不部分更新。
- 未完成项的后续结果作废，不覆盖汇总灯；不取消正在执行的RK测试，不自动断电或停止B2加热。
- RK仍运行时，新开始、改电源及升级写仍可回busy；SLT实际结束后仍须回送010B。

### 4.12 PARA开始测试（0x0108）

保留：PC→MCU。JSON及执行规则暂不定义，未实现时返回0006 failed/notSupported。

### 4.13 PARA结果上报（0x0109）

保留：MCU→PC。结果JSON、测量及判定规则暂不定义。

### 4.14 SLT开始测试（0x010A）

PC→MCU。启动整次SLT，MCU准备电源和EDK启动介质；具体算法及报告走PC/RK与服务器通路。

```json
{
  "algorithmType": "slt",
  "algorithmName": "memtest86",
  "algorithmIndex": null,
  "TimingParameters": {},
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

| 字段 | 类型 | 含义 |
|---|---|---|
| algorithmType | string | 固定slt |
| algorithmName | string | 固定memtest86，大小写严格匹配；结果回送时一致 |
| algorithmIndex | null | 保留字段，固定JSON null，不逐算法编号 |
| power | object | 沿用4.7的memoryType及三轨字符串mV，非null |
| reboot | boolean | 沿用4.7重启规则，false不能绕过配置变化所需重启 |
| bootMode | string | 实际EDK启动介质；取值见4.21，非空且非null |
| TimingParameters | object 可选 | 子字段同4.7；可只填部分项或空对象，提供时不能为null；当前仅解析保存 |

- 原六个根字段必填，TimingParameters可省略；不带timeoutMs。电压范围、步进和安全责任沿用4.7。
- 应答：0006 accepted → completed，requestCommandId=266、algorithmIndex=null；completed仅表示电源/Boot准备完成，不是SLT测试完成。
- MCU不等待RK心跳、不下发串口算法，也不检查EDK或报告服务是否启动。
- PC确认实际启动、取得SLT报告后发送010B；示例emmc须与实际EDK部署位置一致。

### 4.15 SLT结果回送（0x010B）

PC→MCU。回送整次SLT的汇总结论，当前两个镜像各一颗DDR，不逐算法回送；保留统一结果字段，无内容填空。

```json
{
  "siteId": "1",
  "algorithmType": "slt",
  "algorithmName": "memtest86",
  "algorithmIndex": null,
  "testTimeMs": null,
  "testResults": [
    {"mirrorIndex": 0, "testStatus": 1, "resultDesc": null, "physicalAddr": ""},
    {"mirrorIndex": 1, "testStatus": 1, "resultDesc": null, "physicalAddr": ""}
  ],
  "measurementData": null
}
```

| 字段 | 类型 | 含义 |
|---|---|---|
| siteId | string | B1板标识，未配置为空字符串 |
| algorithmType / algorithmName | string | slt及开始请求中的名称 |
| algorithmIndex | null | 固定JSON null，不用于逐算法配对 |
| testTimeMs | uint64/null | 整次SLT实际耗时ms，未知为null |
| testResults | array | 当前两个镜像各一条DDR汇总结果，按mirrorIndex 0、1排序 |
| ↳ mirrorIndex | integer | 镜像索引，当前为0或1 |
| ↳ testStatus | integer | 对应DDR的SLT汇总结论：0=FAIL，1=PASS |
| ↳ resultDesc | null | 保留，当前填null |
| ↳ physicalAddr | string | 镜像内物理芯片标识；当前每镜像一颗时填空字符串 |
| measurementData | null | 保留，当前填null |

- 应答：0006 completed，requestCommandId=267、algorithmIndex=null；不再回传0105/0109。
- 仅接收当前SLT的结果；格式错误回invalidArgument，无在途SLT、名称不符或已处理回invalidState。
- 保持原TCP连接，siteId与本板配置一致；不支持断线重连后认领旧SLT结果。
- 最终镜像结果灯由0107汇总设置；若已提前结束本轮，010B仅确认SLT结束，不覆盖灯态。

### 4.16 启动RK及内存电源（0x0501）

PC→MCU，请求`{}`。使用已保存的训练目标上电，未设置时用标称值。

- 应答：0006 accepted → completed；已ON直接completed。
- completed不表示RK训练就绪；失败后先查询0503，不自动重发。

### 4.17 关闭RK及内存电源（0x0502）

PC→MCU，请求`{}`，直接关闭RK/DDR电源，不等待RK关机应答。

- 应答：0006 accepted → completed；已OFF直接completed；测试/维护在途回busy。
- PC须先确保RK可断电、不在写盘；此命令不取消算法。失败或超时后查询0503。

### 4.18 查询电源状态（0x0503）

PC→MCU，请求包体如下；成功响应0504。

```json
{}
```

### 4.19 电源状态响应（0x0504）

MCU→PC，响应0503。

```json
{
  "powerState": "off"
}
```

| powerState | 含义 |
|---|---|
| off | 电源已关闭 |
| starting | 上电及配置中 |
| on | 上电及目标配置完成，不表示RK训练成功 |
| stopping | 下电中 |
| fault | 电源故障，已执行故障关断 |

字段类型为string，表示逻辑状态，不是实测电压；实测值查询0601。操作是否完成以0006为准。

### 4.20 RK断电重启（0x0505）

PC→MCU，请求`{}`。在ON状态按原Boot和训练目标断电重启RK，不重启MCU。

- 应答：0006 accepted → completed；completed表示本轮RK初始化就绪。
- PC先确认可断电；OFF回invalidState，测试/维护在途回busy。失败/超时不自动重试，先查询状态。

### 4.21 设置RK引导模式（0x0506）

PC→MCU，仅电源OFF时设置；成功回0006 completed，不自动重启。

```json
{"bootMode":"emmc"}
```

| 字段 | 类型 | 取值 |
|---|---|---|
| bootMode | string | usbMaskrom、sdCard、emmc、fspiM0、fspiM1、fspiM2、emmcSd |

### 4.22 查询RK引导模式（0x0507）

PC→MCU，请求`{}`；成功响应0508。无有效Boot配置时回0006 failed/notConfigured。

### 4.23 引导模式响应（0x0508）

```json
{"bootMode":"emmc","targetMv":600}
```

| 字段 | 类型 | 含义 |
|---|---|---|
| bootMode | string | 最近成功且仍有效的Boot配置，不保证实际启动介质 |
| targetMv | integer | 对应目标DAC电压mV，非实测值 |

### 4.24 配置LPDDR类型及三轨目标电压（0x0509）

PC→MCU，独立调压接口；成功回0006 completed。正常训练目标由0103/010A提交。

```json
{
  "memoryType": "LPDDR4X",
  "vdd1": "1800",
  "vdd2": "1100",
  "vddq": "600"
}
```

四个字段必填，格式与4.7的power一致；不附带其他字段。

| 电源轨 | 可编码电压（mV） | 原生步进（mV） | LPDDR4标称（mV） | LPDDR4X标称（mV） | LPDDR4安全范围（mV） | LPDDR4X安全范围（mV） |
|---|---:|---:|---:|---:|---:|---:|
| VDD1 | 800..3350 | 10 | 1800 | 1800 | 1700..1950 | 1700..1950 |
| VDD2 | 400..1675 | 5 | 1100 | 1100 | 1060..1170 | 1060..1170 |
| VDDQ | 400..1675 | 5 | 1100 | 600 | 1060..1170 | 570..650 |

- OFF：只接受所选类型的三路标称值，用于选择下一次上电类型。
- ON：类型须与当前一致；PC确认DDR已初始化后才可拉偏，在途测试仍受busy限制。
- “可编码电压”是器件可写范围，不是DDR安全范围；PC按安全范围限制选值，并核对被测器件手册及三轨组合要求，不自动遍历极端档位。

### 4.25 查询电源采样结果（0x0601）

PC→MCU，请求`{}`；响应0602。测试期间可轮询，不因算法运行返回busy，也不重置统计。

### 4.26 电源采样结果响应（0x0602）

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

PC反向发送0x0602时，MCU返回0x0006 failed/invalidDirection。

### 4.27 查询B1–B2保活状态（0x0801）

### 4.28 B1–B2保活状态响应（0x0802）

### 4.29 固件升级请求（0x0901）

PC→MCU。一个.ota文件按begin→write→finish发送，每步等待响应；不直接发送bin/axf。以下只定义MCU升级。

rk_begin/rk_write/rk_finish/rk_query/rk_cancel仅预留，返回0006 failed/notSupported并带回action。

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

| 操作规则 | 要求 |
|---|---|
| 包信息 | 文件36..39字节为小端u32包头CRC；44/46/48偏移的三个小端u16为目标版本major/minor/patch |
| 写块 | 不超过begin返回的blockSize；末块不补零，文件字节原样Base64编码 |
| 报文限制 | 紧凑JSON，Base64不额外转义；18字节头加正文≤4096字节 |
| 字段 | 仅使用各动作示例列出的字段；query可省略packageId查询一般状态；旧无前缀动作不接受 |
| 升级前 | 停止测试、使RK/DDR安全下电，并协调B2维护，防止升级中复位或断电 |

T1RBMQ==仅为4字节示例，因此下一偏移为4；不是一个完整升级包。

### 4.30 固件升级响应（0x0902）

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

### 4.31 固件升级完成（0x0903）

MCU→PC，响应携带目标packageId的mcu_query；仅在目标包运行、通过本次30秒健康确认且无活动升级时返回。否则回0902，不主动推送。

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

PC核对packageId和firmwareVersion后才判升级成功；可重复查询，无需额外ACK。返回旧版本表示未运行目标包，断连/超时不能单独认定回滚。

| 操作 | PC规则 |
|---|---|
| 等待与重发 | 普通请求等待5秒，mcu_finish等待30秒；同一连接最多重发3次，业务字段不变，Seq照常递增 |
| 未提交下载中断 | 断连/复位或30秒无下载活动即取消，重连从0开始；mcu_query、心跳和重复mcu_begin不延长超时 |
| mcu_finish后断连 | 即使未收到受理响应，也先查询结果，禁止盲目重新下载；ready和commitUnknown后禁止新下载及取消 |
| 重连与查询 | 每2秒重连/查询，安装期间B无网络服务；等待600秒仍无结果显示“结果未确认”，不自动切电源或强制复位 |
| 升级边界 | 仅维护升级，普通路径拒绝同版本和降版；不修改B、稳定版及板卡信息。CRC用于检测损坏，不作为身份认证 |

## 5. 业务命令补充格式

保留命令后续按“方向与用途、JSON示例、字段表、应答及必要规则”补充；实现流程和调试记录不纳入本协议。
