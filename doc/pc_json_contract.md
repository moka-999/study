# PC–MCU JSON业务合同

本页记录PC–MCU的JSON结构，字段定义与固件接入状态分开说明。固定18字节头、连接规则和命令目录见
[PC–MCU TCP协议框架](pc_tcp_protocol_spec.md)，实现进度见`driver_tasks.md`，历史决策见`adr/0001-protocol-contract.md`。

当前固件使用双方独立发送Seq和ServiceID=0。V2已接外部已启动模式：power/bootMode同时null、reboot=false，真实FUNC算法由RK执行，0x0006受理、0x0105返回一个整体最终结果。完整B1训练电压/启动合同仍受门禁，不能把参数解析通过当作测试成功。

现行[固件升级合同](pc_ota_protocol.md)及[PC协议4.26至4.28](pc_tcp_protocol_spec.md#426-固件升级请求0x0901)复用0901/0902/0903。2026-09-24起，MCU动作使用`mcu_begin/mcu_write/mcu_finish/mcu_query/mcu_cancel`，不再接受旧无前缀动作。V2/B1的MCU下载使用最多3000字节原始块和Base64，完整帧≤4096；普通/隔离构建未启用完整OTA时回notSupported。五个对应`rk_*`动作仅保留入口，返回0006 failed/notSupported，不访问Flash或操作RK，不定义RK结构体/包体。0903当前仍只表示所查询MCU包已健康运行，不恢复Seq回显。本轮离线验证与旧固件迁移说明见[修改记录](firmware_upgrade_reservation_20260924.md)；[2026-09-16实测](ota_network_validation_20260916.md)仅证明当时旧动作，不代表新动作已上板。

## 1. 算法下发与芯片测试结果

PC工程师草案中的命令映射：

| 方向 | CommandID | JSON用途 |
|---|---:|---|
| PC → MCU | `CMD_START_TEST = 0x0103` | 一次提交算法大类/名称、计划项index、训练电压档位及重启/启动模式 |
| MCU → PC | `CMD_TEST_RESULT_REPORT = 0x0105` | 上报该算法的多项芯片测试结果 |

`ServiceID`固定填0，不参与分发。PC和MCU在每条连接中分别维护自己的发送Seq，每发送一个应用帧本端加1；收到对方报文不改变本端发送计数，不回显对方Seq，与MCU–RK的u64 Seq无关。两个方向允许数值相同且可以同时发送。Seq不用于匹配业务请求；首值1、重连归1、u32回绕及发送失败规则见协议框架第2节。新开始测试按4.7节串行提交，不靠算法名称或Seq区分并发请求。

## 心跳

- PC主动发送`CMD_CLIENT_HEART_BEAT(0x0002)`，JSON固定为`{}`。
- MCU返回`CMD_SERVER_HEART_BEAT(0x0001)`，JSON固定为`{}`，使用MCU自己的下一发送Seq，不回显请求Seq。
- 心跳只判断连接存活，不携带版本、电源、温度或板卡信息。

### START_TEST包体

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

字段类型、必填要求、算法分类、冷重启判定和响应流程以[协议框架4.7节](pc_tcp_protocol_spec.md#47-开始测试0x0103)为准。
`power`是训练期间相对标称值的有符号档位，目标mV＝标称mV＋档位×20；0为标称值，正数升压、负数降压，不对当前值累加。标称基准及安全校验见4.7节。
与上一份成功生效且仍有效的配置相比，只要内存类型或任一目标电压改变，就必须受控冷重启，不能由`reboot=false`取消；
Boot改变或配置状态未知/失效也需重新建立启动状态。`reboot`控制是否主动重启，重启统一采用受控断电方式；`bootMode`表示启动模式，不单列重启类型。
算法以`algorithmType/algorithmName`标识，大类为`functional/slt/parametric`；保留`algorithmIndex`作为PC当前计划中的算法项编号，同名算法可出现多次并用不同index区分。编号由PC提供，MCU在开始测试执行响应和最终结果中原样带回，不自增、不当作RK算法ID。现有0x0506/0x0509保留，不改变各自包体。

上述完整B1请求严格校验全部六个根字段及power四字段，不接受缺失、重复、未知键或错误类型；timeoutMs为新增可选根字段。
三路档位复用TPS编码范围检查，但不代表已批准安全训练范围。参数非法回0006
`failed/invalidArgument`；合法请求在OTA未健康时回`otaNotReady`，否则回`notReady`。
根对象的index唯一且为合法u32整数时，失败响应原样携带，即使其它字段非法；不合法时省略。
此完整B1路径不写DAC、不切换电源、不投递RK。安全训练白名单、成功配置比较和完整冷重启仍未接通。

V2外部已启动模式使用`{"algorithmType":"functional","algorithmName":"mats","algorithmIndex":1,"power":null,"reboot":false,"bootMode":null}`。这两个null明确表示不设置电压/Boot，不是标称档位0；只接受功能算法。timeoutMs省略或0表示无限等待，非零1..86400000毫秒。详细响应与保护规则见协议4.7节及[操作记录](rk_real_test_20260928.md)。

### TEST_RESULT_REPORT包体

统一结果包见[协议框架4.9节](pc_tcp_protocol_spec.md#49-上报测试结果0x0105)。
`functional/parametric/slt`三类共用外层结果结构，`resultDesc`是对象，其子字段按`algorithmType`区分，不再使用统一说明字符串。

新结果包原样带回请求的`algorithmType/algorithmName/algorithmIndex`。index用于PC对齐当前计划中的算法项，收到约定最终结果后才推进，不能把启动accepted/completed或局部结果当成整项完成；跨计划/重连不能仅凭index认领残留结果。
`algorithmType/algorithmName/siteId`为字符串，`algorithmIndex/mirrorIndex/testStatus`为整数，`testResults`为对象数组。
`siteId`标识B1板，`mirrorIndex`标识B2，即测试机；PC结合二者定位测试机及其B1板。`algorithmIndex`是测试计划项编号，三者不能混用。RK的die不能直接代替B1/B2标识，其与实际DDR对象的对应关系仍待确认。
FUNC（`functional`）的`resultDesc`包含`firstErrorAddress`和`errorType`：首错地址用`0x`加16位十六进制字符串，通过或无地址时为`null`；错误类型当前未提供，填`null`，不冻结未来枚举或非空类型。PC按`testStatus`判定结论，不能仅凭两个字段为`null`认定PASS。Parametric与SLT的子字段暂不定义，不把FUNC字段当作三类通用字段。JSON字段顺序不影响解析。
`physicalAddr`为同一B1内的LPDDR芯片标识，类型保持字符串；一个site最多两颗LPDDR，通过不同标识区分。该字段不是首错地址，PASS和FAIL均保留芯片标识；示例`"1"`不冻结编号规则。
RK首错地址映射到`resultDesc.firstErrorAddress`，不能填入`physicalAddr`；当前RK以地址0表示PASS，应转换为`firstErrorAddress: null`。RK die与芯片标识的映射仍需双方确认；RK尚未提供错误类型，`resultDesc.errorType`填`null`。

顶层`testTimeMs`位于`algorithmIndex`之后，表示本次算法整体耗时（ms），FUNC直接取RK结果`0x9002`的uint64原值。输出十进制JSON整数，PC按64位无符号整数读取；0是合法耗时，不以PC收发间隔或MCU等待时间替代。其他测试类型未提供耗时时为null；0006受理/失败不携带该字段。
本次文档示例用`testStatus: 1`演示成功，不冻结正式状态码约定。

`measurementData`独立于0x0602，包含`Vdd1/Vdd2/Vddq/Idd1/Idd2/Iddq`分别加`Ave/Peak/Min`后缀的18个字段，不含`state`；这些字段对所有算法类型通用，不是IDD或Parametric专用。
Vdd字段为实测电压mV，Idd字段为实测电流mA，均用JSON数字；Ave是有效样本算术平均值，Peak/Min是采样最大/最小值，不保证捕获瞬态尖峰。
无有效数据的字段填JSON `null`；完全没有测量数据时整个`measurementData`为`null`。执行或采样异常须由`testStatus/resultDesc`明确区分，不冒充芯片测试FAIL；无效样本不得当成0参与统计。
新测试校验通过并正式受理后清零，电压稳定且进入算法有效测量阶段才采样，测试结束后冻结；PC查询不清零，被拒绝的新请求不得影响原统计。
共轨两颗DDR的电流为合计，不作为单颗电流重复填报；具体对象映射另行约定。

**实现边界（2026-09-28）**：V2外部已启动模式使用新构造器输出algorithmType/physicalAddr/measurementData及resultDesc对象，旧字符串构造器仅保留旧隔离测试兼容。用户确认每算法一个整体结果；当前testStatus=1表示整体PASS、2表示FAIL，firstErrorAddress依RK原值转换；siteId读W25站点标签，未配置为空，mirrorIndex=0未分配、physicalAddr为空不猜映射，measurementData=null。本轮不修改独立B1工程；不新增SLT或IDD上报，采样生命周期仍待真实接口，不把收到开始请求当成测量开始。
`siteId/mirrorIndex`的业务含义已于2026-09-24确认，编号分配规则及来源未冻结；
`siteId`是否允许空值、编号及`testStatus`的业务值域、状态码含义、结果数量以及
请求与结果关联方式仍待PC/测试业务评审。

JSON使用严格语法，最后一个字段或数组元素后不得保留逗号；尾随非空白数据和非法UTF-8
导致MCU关闭当前TCP连接。重复键通常也断连，但0103在语法合法时将其作为业务参数错误，
返回0006 `invalidArgument`，不执行请求。完整PDU不得超过4096字节（包含18字节头部）。

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
  "testerName": "LPDDR4/X_200B",
  "testerVersion": "D00",
  "testBoard": "RK3588_test"
}
```

| JSON字段 | FRU来源 | 当前持久化值 | 含义 |
|---|---|---|---|
| `testerName` | `tester_name` | `"LPDDR4/X_200B"` | 测试仪名称 |
| `testerVersion` | `hardware_revision` | `"D00"` | 测试仪硬件版本 |
| `testBoard` | `test_board` | `"RK3588_test"` | 测试板名称 |

线上JSON不包含`//`注释；上表承担字段说明。三个字段均为字符串。
`tester_name`和`product_name`当前均持久化为`LPDDR4/X_200B`；响应中
`testerName`从`tester_name`读取。每次收到0x0101请求都重新读取FRU，不缓存上次响应，
也不会把编译期默认值写回已有有效v2记录。

## 3. 电源采样查询与统计

0x0601请求仍为`{}`，0x0602的新格式见[协议框架4.20节](pc_tcp_protocol_spec.md#420-电源采样结果响应0x0602)。
外层保留`sampleAgeMs/samplePowerState/validMask`和最新`voltageMv/currentMa/powerMw`；新增`statistics`对象，含`state`及电压/电流的Avg/Peak/Min六个数组。
所有数组顺序均为VDD1、VDD2、VDDQ；单位固定mV、mA、mW。`validMask`只说明最新快照组有效性，不说明统计完整性；无效快照组省略。
尚未开始统计时`statistics: null`；统计数组中没有有效样本的元素为JSON `null`。`state`取`running/complete/timeout/cancelled`，表示统计阶段，不是测试PASS/FAIL。
不上传统计年龄、样本数量或窗口时长；PC查询只读已有快照和统计，不触发器件即时采样，也不重置统计。

**实现边界（2026-09-24）**：本次只更新协议。当前0602代码仍仅返回原有最新快照，尚未组帧`statistics`；现有统计后端并不等于已接通PC响应或正式IDD窗口，后续接入及验证单独进行。
