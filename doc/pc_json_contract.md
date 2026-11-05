# PC–MCU JSON业务合同

2026-11-05：用户确认16项时序字段及无应答发送。PC判定数值，MCU只校验十进制格式/位宽；Freq在启动早期回复0x35，普通时序在SPL后逐项发送，之后才投递算法。空串规则、重启/SLT边界及验证见[接入记录](rk_timing_frequency_20261105.md)。下方带日期的旧记录不覆盖本条。

2026-10-26：0108/0109已接入PARA，包体复用FUNC字段，类别固定`parametric`、名称按双方定义匹配；RK只下发21～36编号，进入/退出和时序寄存器仅准备编解码。FUNC/PARA/SLT的`TimingParameters`统一为必填对象，默认用`{}`，缺失或null回invalidArgument，子字段仍可部分提供字符串。当前统计为全程统计，不是IDD有效窗口或判限。详见[本轮接入与验证](para_integration_20261026.md)；以下带日期的历史状态不覆盖本条。

2026-10-20：按飞书809版及本轮确认，0103/010A增加可选`TimingParameters`对象，11项均可单独提供字符串；0103允许null，010A提供时必须是对象。MCU仅校验、保存本次已受理请求的RAM副本，尚不向RK下发，也不将受理解释为时序已应用。单位、有效取值及RK映射待商量；SLT仍严格匹配`slt/memtest86`。见[时序字段接入记录](pc_timing_parameters_20261020.md)。下方旧日期描述不覆盖本轮结论。

2026-10-14协议及代码以[PC协议](pc_tcp_protocol_spec.md)和[实现记录§10](pc_test_round_protocol_20261014.md#10-整轮slt及070x删除实现2026-10-14未烧录)为准：0106空对象建轮，之后执行FUNC 0103/0105或SLT 010A/010B，最后0107提交errorchip数组。镜像0/1/2映射两组LPDDR/eMCP；SLT固定slt/memtest86、index=null，只做电源/Boot准备，完成由PC回送。0108/0109 PARA预留；070x已从V2/B1源码删除，合法旧请求回notSupported。B2加热/灯态获取尚无报文合同，默认常温仅为显示初值。两板代码已构建/离线验证，V2已安装0.1.35并通过网络检查，B1未烧录；Word/飞书停留在此前621版，待补齐固定名称及灯映射等后续细化。

0103和0509统一使用`vdd1/vdd2/vddq`字符串，直接表示绝对电压mV，例如`"1800"/"1100"/"600"`。必须是非空ASCII十进制数字串，并满足`valMv=nominalMv+stepMv*n`：VDD1步进10mV、VDD2/VDDQ步进5mV，n为整数。越界或非整档直接拒绝，不取整；旧steps字段不再接受。详见[电压字段修改记录](pc_voltage_mv_20261012.md)。

本页记录PC–MCU的JSON结构，字段定义与固件接入状态分开说明。固定18字节头、连接规则和命令目录见
[PC–MCU TCP协议框架](pc_tcp_protocol_spec.md)，实现进度见`driver_tasks.md`，历史决策见`adr/0001-protocol-contract.md`。

当前固件使用双方独立发送Seq和ServiceID=0。V2/B1均已接外部已启动模式：power/bootMode同时null、reboot=false，真实FUNC算法由RK执行，0x0006受理、0x0105返回一个整体最终结果，B1还要求真实电源状态ON。B1完整训练启动已配置初值并接入驱动与本轮RK心跳确认，尚待实板验收；不能把参数解析、受理或启动完成当作测试PASS。

现行[固件升级合同](pc_ota_protocol.md)及[PC协议4.29至4.31](pc_tcp_protocol_spec.md#429-固件升级请求0x0901)复用0901/0902/0903。2026-09-24起，MCU动作使用`mcu_begin/mcu_write/mcu_finish/mcu_query/mcu_cancel`，不再接受旧无前缀动作。V2/B1的MCU下载使用最多3000字节原始块和Base64，完整帧≤4096；普通/隔离构建未启用完整OTA时回notSupported。五个对应`rk_*`动作仅保留入口，返回0006 failed/notSupported，不访问Flash或操作RK，不定义RK结构体/包体。0903当前仍只表示所查询MCU包已健康运行，不恢复Seq回显。本轮离线验证与旧固件迁移说明见[修改记录](firmware_upgrade_reservation_20260924.md)；[2026-09-16实测](ota_network_validation_20260916.md)仅证明当时旧动作，不代表新动作已上板。

## 1. 算法下发与芯片测试结果

现有固件的算法命令映射：

| 方向 | CommandID | JSON用途 |
|---|---:|---|
| PC → MCU | `PC_COMMAND_START_TEST = 0x0103` | 一次提交FUNC名称、计划项index、训练目标mV及重启/启动模式 |
| MCU → PC | `PC_COMMAND_TEST_RESULT_REPORT = 0x0105` | 上报该FUNC算法的双颗粒测试结果 |
| PC → MCU | `PC_COMMAND_PARA_START = 0x0108` | 同格式提交PARA名称、计划index、训练目标及重启/启动模式 |
| MCU → PC | `PC_COMMAND_PARA_RESULT = 0x0109` | 上报该PARA算法的双颗粒结果及共享统计 |

`ServiceID`固定填0，不参与分发。PC和MCU在每条连接中分别维护自己的发送Seq，每发送一个应用帧本端加1；收到对方报文不改变本端发送计数，不回显对方Seq，与MCU–RK的u64 Seq无关。两个方向允许数值相同且可以同时发送。Seq不用于匹配业务请求；首值1、重连归1、u32回绕及发送失败规则见协议框架第2节。新开始测试按4.7节串行提交，不靠算法名称或Seq区分并发请求。

## 心跳

- PC主动发送`CMD_CLIENT_HEART_BEAT(0x0002)`，JSON固定为`{}`。
- MCU返回`CMD_SERVER_HEART_BEAT(0x0001)`，JSON固定为`{}`，使用MCU自己的下一发送Seq，不回显请求Seq。
- 心跳只判断连接存活，不携带版本、电源、温度或板卡信息。

### START_TEST包体

```json
{
  "algorithmType": "functional",
  "algorithmName": "mats",
  "algorithmIndex": 1,
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

字段类型、必填要求、算法分类、冷重启判定和响应流程以[协议框架4.7节](pc_tcp_protocol_spec.md#47-func开始测试0x0103)为准。
`power`为训练期间的绝对电压mV字符串，重复发送不累加；不是运行期拉偏。步进为10/5/5mV，旧steps不再接受。PC确认DDR安全范围与三轨组合，MCU严格校验格式、内存类型、TPS可编码范围和原生步进，不取整或钳位。
与上一份成功生效且仍有效的配置相比，只要内存类型或任一目标电压改变，就必须受控冷重启，不能由`reboot=false`取消；
Boot改变或配置状态未知/失效也需重新建立启动状态。`reboot`控制是否主动重启，重启统一采用受控断电方式；`bootMode`表示启动模式，不单列重启类型。
FUNC以`algorithmType/algorithmName`标识，0103固定为functional；PARA的0108固定为parametric，结果用0109；SLT另用010A/010B且algorithmIndex=null。FUNC/PARA的`algorithmIndex`为PC当前计划中的算法项编号，同名算法可出现多次并用不同index区分。编号由PC提供，MCU在开始测试执行响应和最终结果中原样带回，不自增、不当作RK算法ID。现有0x0506/0x0509保留，不改变各自包体。

上述完整B1请求严格校验七个根字段及power四字段，不接受缺失、重复、未知键或错误类型；timeoutMs仅FUNC/PARA可选。TimingParameters对象必填，使用默认时序传{}；内层只接受新定义的16项字符串（见接入记录）。Freq为int64，其他15项为u32十进制；允许部分项和空字符串，不继承上一请求。合法请求受理后复制并按阶段发送，失败请求不覆盖原副本。
三路电压复用TPS编码范围检查，但不代表已批准安全训练范围。参数非法回0006
`failed/invalidArgument`；OTA未健康时回`otaNotReady`，在途冲突回`busy`。
根对象的index唯一且为合法u32整数时，失败响应原样携带，即使其它字段非法；不合法时省略。
B1完整路径已接DAC、上/下电、训练目标写入和本轮RK心跳等待；必要时直接断电重启，不等RK关机应答。PC/RK负责保证可直接切电。受理回accepted，启动准备及算法入队后回completed，FUNC最终结果用0105、PARA用0109；SLT只做启动准备，由PC回送总结果。电气验收见[初版配置](b1_power_business_20261013.md)，PARA边界见[接入记录](para_integration_20261026.md)。

V2外部已启动模式使用`{"algorithmType":"functional","algorithmName":"mats","algorithmIndex":1,"TimingParameters":{},"power":null,"reboot":false,"bootMode":null}`。这两个null明确表示不设置电压/Boot，不是标称档位0；PARA同样允许该模式，但必须用0108及parametric/IDD名称。timeoutMs省略或0表示无限等待，非零1..86400000毫秒。详细响应与保护规则见协议4.7/4.12节及[操作记录](rk_real_test_20260928.md)。

### TEST_RESULT_REPORT包体

正式FUNC结果结构见[协议框架4.9节](pc_tcp_protocol_spec.md#49-func结果上报0x0105)。每算法一个0105包，根字段为`siteId/algorithmType/algorithmName/algorithmIndex/testTimeMs/testResults/measurementData`。

- 顶层siteId从本次受理时读取的W25 FRU站点标签获取，未配置为空；不在每颗里重复。
- testResults固定两项，mirrorIndex依次为0/1；每项含testStatus、resultDesc和保留空字符串physicalAddr。testStatus为0=FAIL、1=PASS。
- FUNC失败条目按RK每组9001+3001的die归位，未列出的颗粒为PASS，前提是完整3/5/7 TLV最终包已通过一致性、算法和Seq窗口校验。不能把缺包/解析错误当成全PASS。
- RK完整结果的非零失败地址输出0x加16位十六进制字符串；零地址明细表示PASS，不计入失败数组，PASS的firstErrorAddress为null。errorType为字符串，真实RK未提供时为空，PASS也为空。整体FAIL与明细是否含非零地址矛盾时整包拒绝，见[2026-11-05核查](rk_protocol_review_20261105.md)。
- V2显式MATS假FAIL仅改第一颗PC视图：原PASS时使用演示地址0x0000000012345678，原FAIL时保留原地址，类型SAF；第二颗仍为真实结果。B1禁止该模拟，RK原结果不被改写。
- testTimeMs为RK9002原始uint64毫秒，用整数十进制输出，不通过double转换；算法名与计划index沿用请求。Seq仍是独立发送流水号，不改业务关联。
- 顶层measurementData为两颗共轨共享的18项Vdd/Idd Ave/Peak/Min，单位mV/mA，不含state；无效单项null，完全无样本整个对象null。有效样本平均值和样点极值沿用原统计，不代表瞬态峰值或正式IDD判限。
- 统计在投递算法前清零、结束冻结，启动准备等待不计入统计，查询不清零；V2电源模拟配置给出模拟统计，B1来自真实采样。

**实现边界（2026-10-13）**：V2/B1均已接入双颗粒原子解析和正式JSON组帧；3/5/7包通过校验后才交付一次0105。B1完整冷启动及心跳就绪已接初版配置，SLT/IDD结果仍未实现；硬件时序、供电和RK侧发送时点待实板验收。双颗粒实现证据见[修改记录](rk_dual_die_result_20261010.md)，最新对外说明以协议框架及固定名Word为准。

JSON使用严格语法，最后一个字段或数组元素后不得保留逗号。已收齐完整PDU时，非法UTF-8、JSON语法、尾随非空白数据或不合法的业务参数返回0006 `invalidArgument`，不执行请求，并继续接收；坏头和半帧按协议框架的有界恢复规则处理。完整PDU不得超过4096字节（包含18字节头部）。

## 2. 测试仪信息

测试仪信息采用普通请求/响应，不在TCP连接建立后主动推送：

当前仅规定基本格式；固件版本等后续需要的信息可追加到0x0102响应JSON，不必新增命令。保留现有三个字段的含义，接收端允许扩展字段，完整PDU仍不超过4096字节。固件/FRU信息的运行时来源仍为W25Q256；本次不增加JSON字段或Flash结构。

| 方向 | CommandID | JSON用途 |
|---|---:|---|
| PC → MCU | `CMD_QUERY_TESTER_INFO = 0x0101` | 查询测试仪信息，包体固定为`{}` |
| MCU → PC | `CMD_TESTER_INFO_RESP = 0x0102` | 返回测试仪信息 |

响应的`ServiceID`固定为0，`Seq`使用MCU自己的发送流水号。MCU从
W25Q256 FRU v2读取三个响应字段：

```json
{
  "testerName": "JMCT-4000",
  "testerVersion": "D00",
  "testBoard": "RK3588_test"
}
```

| JSON字段 | FRU来源 | 当前持久化值 | 含义 |
|---|---|---|---|
| `testerName` | `tester_name` | `"JMCT-4000"` | 测试仪名称 |
| `testerVersion` | `hardware_revision` | `"D00"` | 测试仪硬件版本 |
| `testBoard` | `test_board` | `"RK3588_test"` | 测试板名称 |

线上JSON不包含`//`注释；上表承担字段说明。三个字段均为字符串。
`tester_name`确认为`JMCT-4000`，`product_name`仍保留`LPDDR4/X_200B`；响应中
`testerName`从`tester_name`读取。每次收到0x0101请求都重新读取FRU，不缓存上次响应，
仅旧默认名称`LPDDR4/X_200B`会一次性保存为新名称并回读，之后只读；
序列号、MAC、IP、站点等字段及定制名称保持不变，不强制覆盖其它有效v2信息。

## 3. 电源采样查询与统计

0x0601请求仍为`{}`，0x0602的新格式见[协议框架4.20节](pc_tcp_protocol_spec.md#420-电源采样结果响应0x0602)。
外层保留`sampleAgeMs/samplePowerState/validMask`和最新`voltageMv/currentMa/powerMw`；新增`statistics`对象，含`state`及电压/电流的Avg/Peak/Min六个数组。
所有数组顺序均为VDD1、VDD2、VDDQ；单位固定mV、mA、mW。`validMask`只说明最新快照组有效性，不说明统计完整性；无效快照组省略。
尚未开始统计时`statistics: null`；统计数组中没有有效样本的元素为JSON `null`。`state`取`running/complete/timeout/cancelled`，表示统计阶段，不是测试PASS/FAIL。
不上传统计年龄、样本数量或窗口时长；PC查询只读已有快照和统计，不触发器件即时采样，也不重置统计。

**实现边界（2026-10-13）**：0602已接最近快照及statistics，测试中可反复查询，不因算法在途回busy；B1使用真实采样，V2显式模拟构建使用模型数据。统计接入不等于正式IDD状态/测量窗口或真实硬件验收。
