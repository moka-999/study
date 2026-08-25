# PC–MCU JSON业务合同

本页只记录双方已经给出的JSON结构。固定18字节头、TCP流式组帧和通用
JSON安全规则见`adr/0001-protocol-contract.md`。

## 1. 算法下发与芯片测试结果

PC工程师草案中的命令映射：

| 方向 | CommandID | JSON用途 |
|---|---:|---|
| PC → MCU | `CMD_START_TEST = 0x0103` | 下发算法名称与编号 |
| MCU → PC | `CMD_TEST_RESULT_REPORT = 0x0105` | 上报该算法的多项芯片测试结果 |

`ServiceID`尚未确认。MCU响应必须原样回显PC请求的Seq；该u32 Seq只属于PC-MCU协议，与MCU-RK的u64 Seq无关。

## 心跳

- PC主动发送`CMD_CLIENT_HEART_BEAT(0x0002)`，JSON固定为`{}`。
- MCU返回`CMD_SERVER_HEART_BEAT(0x0001)`，JSON固定为`{}`，并原样回显请求Seq。
- 心跳只判断连接存活，不携带版本、电源、温度或板卡信息。

### START_TEST包体

```json
{
  "algorithmName": "algorithmName",
  "algorithmIndex": 1
}
```

### TEST_RESULT_REPORT包体

```json
{
  "algorithmName": "algorithmName",
  "algorithmIndex": 1,
  "testResults": [
    {"siteId": "", "mirrorIndex": 1, "testStatus": 1},
    {"siteId": "", "mirrorIndex": 1, "testStatus": 1},
    {"siteId": "", "mirrorIndex": 1, "testStatus": 1}
  ]
}
```

当前只冻结结构和JSON类型：`algorithmName/siteId`为字符串，
`algorithmIndex/mirrorIndex/testStatus`为整数，`testResults`为对象数组。
`siteId`是否允许空值、三个整数的业务值域、状态码含义、结果数量以及
ServiceID仍待PC/测试业务评审。

JSON使用严格语法，最后一个字段或数组元素后不得保留逗号；重复键、尾随
非空白数据和非法UTF-8均会导致MCU关闭当前TCP连接。完整PDU不得超过4096
字节（包含18字节头部）。
