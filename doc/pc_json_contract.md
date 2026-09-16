# PC–MCU JSON业务合同

本页记录PC–MCU的JSON结构，字段定义与固件接入状态分开说明。固定18字节头、连接规则和命令目录见
[PC–MCU TCP协议框架](pc_tcp_protocol_spec.md)，实现进度见`driver_tasks.md`，历史决策见`adr/0001-protocol-contract.md`。

当前固件已使用双方独立发送Seq和ServiceID=0。2026-09-14新增的开始测试字段仅更新协议文档，尚未接入固件；不得把新格式示例视为已有网络功能。

2026-09-16的[OTA合同](pc_ota_protocol.md)及[PC协议4.26至4.28](pc_tcp_protocol_spec.md#426-mcu固件升级请求0x0901)已接入启用`ENABLE_OTA_BOOT`的V2，并通过真实TCP升级，见[验证记录](ota_network_validation_20260916.md)。0901用action区分五种操作，0902为响应，0903仅表示所查询目标包已健康运行。失败沿用0006，传输使用1024字节原始块和Base64；没有新增或恢复Seq回显。普通/隔离构建未启用完整OTA时回notSupported；B1尚未移植。

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

当前V2/B1仍只解析`algorithmName/algorithmIndex`，合法请求无同步成功响应。
新包虽能通过旧字段校验，新增大类、训练档位、完整启动流程、带index的执行响应及新结果格式仍未接通。

### TEST_RESULT_REPORT包体

```json
{
  "algorithmType": "parametric",
  "algorithmName": "idd_0",
  "algorithmIndex": 1,
  "testResults": [
    {"siteId": "", "mirrorIndex": 1, "resultDesc": "", "testStatus": 1},
    {"siteId": "", "mirrorIndex": 1, "resultDesc": "", "testStatus": 1},
    {"siteId": "", "mirrorIndex": 1, "resultDesc": "", "testStatus": 1}
  ]
}
```

新结果包原样带回请求的`algorithmType/algorithmName/algorithmIndex`。index用于PC对齐当前计划中的算法项，收到约定最终结果后才推进，不能把启动accepted/completed或局部结果当成整项完成；跨计划/重连不能仅凭index认领残留结果。固件构造器尚未增加大类。
`algorithmType/algorithmName/siteId/resultDesc`为字符串，`algorithmIndex/mirrorIndex/testStatus`为整数，`testResults`为对象数组。
`resultDesc`为2026-09-08新增的每项结果描述，使用UTF-8；无描述时输出空字符串`""`，不省略字段或使用`null`，不替代`testStatus`状态码。
`siteId/mirrorIndex`具体业务含义先保留，不绑定物理位置或软件镜像；
`siteId`是否允许空值、`mirrorIndex/testStatus`的业务值域、状态码含义、结果数量以及
请求与结果关联方式仍待PC/测试业务评审。

JSON使用严格语法，最后一个字段或数组元素后不得保留逗号；重复键、尾随
非空白数据和非法UTF-8均会导致MCU关闭当前TCP连接。完整PDU不得超过4096
字节（包含18字节头部）。

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
