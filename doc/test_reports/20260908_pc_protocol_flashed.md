# PC模拟上位机 实板协议收发记录

目标：`192.168.1.201:8888`。时间：2026-09-08T15:41:33.585104+08:00。

测试对象为本次重新编译并经SWD烧录、逐字节校验和复位的正式固件（App 220156字节，CRC32=0xD14BB7FC）；Bootloader也已烧录校验。固定头18字节，Version=1、Flag=8、ServiceID=0；表中命令和Seq属于帧头，JSON仅为包体。

覆盖心跳、查询、业务错误、半包/粘包及畸形帧断连。结果灯初始查询失败，跳过3次设置、设置后查询、3次恢复及恢复后查询，共8次交互；因此本轮未验证结果灯设置/恢复。未执行有效上电、DAC改写或DDR调压。测试仪查询走固件既有FRU入口；该入口在Flash空白/旧格式时可能初始化/迁移记录，未据网络响应推断是否发生擦写。

记录数：34；符合本脚本预期：33；差异：1。无响应窗口不等于业务完成；查询类未固定动态值的记录只核对帧结构和Seq。

## 验证结论与留存证据

- 通用响应已实际返回`requestCommandId`，上次旧镜像的`commandId`差异已消失。
- 唯一不符项：`0x0702 {}`返回`0x0006 {"requestCommandId":1794,"status":"failed","errorCode":"executionFailed"}`。`Core/Src/main.c`仅在`ENABLE_PC_PROTOCOL_NET_TEST`开启时初始化结果灯板级映射，本次正式构建该选项为OFF；`result_led_get_all()`因映射未初始化返回HAL_ERROR。保留正式镜像原有配置，未擅自扩展引脚映射。
- 合法`0x0103`算法请求在1.5秒内无响应，随后心跳正常。正式分发目前仅解析算法请求，尚未接通RK执行及结果上报；反向发送`0x0105`被拒绝，不能用它验证MCU主动上报。
- 已先烧录隔离Mock实际运行正式C组包器，`resultDesc`空串、中文和NULL默认值及固定头组帧检查通过。首次Mock为145 PASS、4 FAIL，定位为旧测试合法帧的ServiceID=0x0102；只将该输入及相应期望改为0，重新构建、烧录、校验后为**150 PASS、0 FAIL**。新增的第150项来自修正后不再提前终止的头部完成状态检查。
- Mock验证结束后烧录正式App并复位，以下TCP记录均来自正式App；最终电源状态OFF。未验证真实RK算法、器件测量或灯光物理波形。PC离线合同24/24、边界模型19/19及纯接口头GCC检查也通过。

串口原始输出：

```text
=== TPS/PAC/AD/MCP driver mock test ===
RESULT: 150 PASS, 0 FAIL
=== driver mock test complete ===
```

ST-LINK：`54FF6D067182505239431087`；设备ID：`0x450`，Rev V，内部Flash 2MiB。构建目标：`apollo-v2-h743` / Debug；构建时间UTC：`2026-09-08T07:41:05Z`。

| 留存文件（仓库内） | 用途 / SHA256 |
|---|---|
| `tmp/pc_protocol_flashed_20260908/before_flash.bin` | 烧录前2MiB备份；`51DB27D8A25B2B96C4C04E8D181F075B101AD42881F7A9E92295F9C8CEAD1523` |
| `tmp/pc_protocol_flashed_20260908/boot.elf` | 已烧录Boot；`4033792660013E56C6F7D2D893DF06288F4E836D26BB1045D3C1BEFEF6ED487C` |
| `tmp/pc_protocol_flashed_20260908/official.elf` | 最终实际烧录ELF；`7E91C6A3443F8939D6E21DCFFEDE7077E6C80AEA853052F2A1FBC213737ED6E8` |
| `tmp/pc_protocol_flashed_20260908/official.bin` | 同构建App，220156字节，CRC32=0xD14BB7FC；`82D2B9E27A1B4AC85966F24CC90966A12720507D82FFEC59BD5AD2A8FF8C979E` |
| `tmp/pc_protocol_flashed_20260908/mock_fixed.elf` | 通过的Mock；`B122161ABEBA3CD53FD02EB3429C78CC94DDD921D3947A2404778A7D4C8E00C1` |

同目录留存`backup.log`、`flash_boot.log`、`flash_mock_fixed.log`、`mock_fixed_serial.log`、`flash_official.log`、`reset_official.log`、`official.manifest.json`及`wire.jsonl`。烧录日志均确认`Download verified successfully`。下方JSON是原始TCP包体；非法UTF-8显示的替换字符仅用于阅读，原始字节以十六进制为准。

## 1. 心跳

连接 1；PC命令 `0x0002` / Seq `100`；接收等待 0ms；符合预期。

PC发送包体：

```json
{}
```

MCU响应命令 `0x0001` / Seq `1`：

```json
{}
```

<details><summary>原始帧十六进制</summary>

`TX 0000001400010008000000020000006400007b7d`

`RX 0000001400010008000000010000000100007b7d`

</details>

## 2. 查询电源状态

连接 1；PC命令 `0x0503` / Seq `101`；接收等待 0ms；符合预期。

PC发送包体：

```json
{}
```

MCU响应命令 `0x0504` / Seq `2`：

```json
{"powerState":"off"}
```

<details><summary>原始帧十六进制</summary>

`TX 0000001400010008000005030000006500007b7d`

`RX 0000002600010008000005040000000200007b22706f7765725374617465223a226f6666227d`

</details>

## 3. 查询监测快照

连接 1；PC命令 `0x0601` / Seq `102`；接收等待 0ms；符合预期。

PC发送包体：

```json
{}
```

MCU响应命令 `0x0602` / Seq `3`：

```json
{"sampleAgeMs":436,"samplePowerState":"off","validMask":0}
```

<details><summary>原始帧十六进制</summary>

`TX 0000001400010008000006010000006600007b7d`

`RX 0000004c00010008000006020000000300007b2273616d706c654167654d73223a3433362c2273616d706c65506f7765725374617465223a226f6666222c2276616c69644d61736b223a307d`

</details>

## 4. 查询Boot配置

连接 1；PC命令 `0x0507` / Seq `103`；接收等待 0ms；符合预期。

PC发送包体：

```json
{}
```

MCU响应命令 `0x0006` / Seq `4`：

```json
{"requestCommandId":1287,"status":"failed","errorCode":"notConfigured"}
```

<details><summary>原始帧十六进制</summary>

`TX 0000001400010008000005070000006700007b7d`

`RX 0000005900010008000000060000000400007b2272657175657374436f6d6d616e644964223a313238372c22737461747573223a226661696c6564222c226572726f72436f6465223a226e6f74436f6e66696775726564227d`

</details>

## 5. 读取测试前结果灯状态

连接 1；PC命令 `0x0702` / Seq `104`；接收等待 0ms；**与预期不符。**

PC发送包体：

```json
{}
```

MCU响应命令 `0x0006` / Seq `5`：

```json
{"requestCommandId":1794,"status":"failed","errorCode":"executionFailed"}
```

<details><summary>原始帧十六进制</summary>

`TX 0000001400010008000007020000006800007b7d`

`RX 0000005b00010008000000060000000500007b2272657175657374436f6d6d616e644964223a313739342c22737461747573223a226661696c6564222c226572726f72436f6465223a22657865637574696f6e4661696c6564227d`

</details>

## 6. 查询测试仪信息（固件FRU加载入口）

连接 1；PC命令 `0x0101` / Seq `105`；接收等待 0ms；符合预期。

PC发送包体：

```json
{}
```

MCU响应命令 `0x0102` / Seq `6`：

```json
{"testerName":"LPDDR4/X_200B","testerVersion":"D00","testBoard":"RK3588_test"}
```

<details><summary>原始帧十六进制</summary>

`TX 0000001400010008000001010000006900007b7d`

`RX 0000006000010008000001020000000600007b227465737465724e616d65223a224c50444452342f585f32303042222c2274657374657256657273696f6e223a22443030222c2274657374426f617264223a22524b333538385f74657374227d`

</details>

## 7. 心跳拒绝额外字段

连接 1；PC命令 `0x0002` / Seq `106`；接收等待 0ms；符合预期。

PC发送包体：

```json
{"extra":1}
```

MCU响应命令 `0x0006` / Seq `7`：

```json
{"requestCommandId":2,"status":"failed","errorCode":"invalidArgument"}
```

<details><summary>原始帧十六进制</summary>

`TX 0000001d00010008000000020000006a00007b226578747261223a317d`

`RX 0000005800010008000000060000000700007b2272657175657374436f6d6d616e644964223a322c22737461747573223a226661696c6564222c226572726f72436f6465223a22696e76616c6964417267756d656e74227d`

</details>

## 8. 上电命令参数校验，不执行上电

连接 1；PC命令 `0x0501` / Seq `107`；接收等待 0ms；符合预期。

PC发送包体：

```json
{"extra":1}
```

MCU响应命令 `0x0006` / Seq `8`：

```json
{"requestCommandId":1281,"status":"failed","errorCode":"invalidArgument"}
```

<details><summary>原始帧十六进制</summary>

`TX 0000001d00010008000005010000006b00007b226578747261223a317d`

`RX 0000005b00010008000000060000000800007b2272657175657374436f6d6d616e644964223a313238312c22737461747573223a226661696c6564222c226572726f72436f6465223a22696e76616c6964417267756d656e74227d`

</details>

## 9. 已OFF时下电的幂等应答

连接 1；PC命令 `0x0502` / Seq `108`；接收等待 0ms；符合预期。

PC发送包体：

```json
{}
```

MCU响应命令 `0x0006` / Seq `9`：

```json
{"requestCommandId":1282,"status":"completed"}
```

<details><summary>原始帧十六进制</summary>

`TX 0000001400010008000005020000006c00007b7d`

`RX 0000004000010008000000060000000900007b2272657175657374436f6d6d616e644964223a313238322c22737461747573223a22636f6d706c65746564227d`

</details>

## 10. 已OFF时拒绝重启RK

连接 1；PC命令 `0x0505` / Seq `109`；接收等待 0ms；符合预期。

PC发送包体：

```json
{}
```

MCU响应命令 `0x0006` / Seq `10`：

```json
{"requestCommandId":1285,"status":"failed","errorCode":"invalidState"}
```

<details><summary>原始帧十六进制</summary>

`TX 0000001400010008000005050000006d00007b7d`

`RX 0000005800010008000000060000000a00007b2272657175657374436f6d6d616e644964223a313238352c22737461747573223a226661696c6564222c226572726f72436f6465223a22696e76616c69645374617465227d`

</details>

## 11. Boot模式非法枚举

连接 1；PC命令 `0x0506` / Seq `110`；接收等待 16ms；符合预期。

PC发送包体：

```json
{"bootMode":"invalid"}
```

MCU响应命令 `0x0006` / Seq `11`：

```json
{"requestCommandId":1286,"status":"failed","errorCode":"invalidArgument"}
```

<details><summary>原始帧十六进制</summary>

`TX 0000002800010008000005060000006e00007b22626f6f744d6f6465223a22696e76616c6964227d`

`RX 0000005b00010008000000060000000b00007b2272657175657374436f6d6d616e644964223a313238362c22737461747573223a226661696c6564222c226572726f72436f6465223a22696e76616c6964417267756d656e74227d`

</details>

## 12. DDR档位类型校验，不写电压

连接 1；PC命令 `0x0509` / Seq `111`；接收等待 0ms；符合预期。

PC发送包体：

```json
{"memoryType":"LPDDR4X","vdd1Steps":"invalid","vdd2Steps":0,"vddqSteps":0}
```

MCU响应命令 `0x0006` / Seq `12`：

```json
{"requestCommandId":1289,"status":"failed","errorCode":"invalidArgument"}
```

<details><summary>原始帧十六进制</summary>

`TX 0000005c00010008000005090000006f00007b226d656d6f727954797065223a224c504444523458222c22766464315374657073223a22696e76616c6964222c22766464325374657073223a302c22766464715374657073223a307d`

`RX 0000005b00010008000000060000000c00007b2272657175657374436f6d6d616e644964223a313238392c22737461747573223a226661696c6564222c226572726f72436f6465223a22696e76616c6964417267756d656e74227d`

</details>

## 13. 未知命令

连接 1；PC命令 `0x7FFF` / Seq `112`；接收等待 0ms；符合预期。

PC发送包体：

```json
{}
```

MCU响应命令 `0x0006` / Seq `13`：

```json
{"requestCommandId":32767,"status":"failed","errorCode":"notSupported"}
```

<details><summary>原始帧十六进制</summary>

`TX 000000140001000800007fff0000007000007b7d`

`RX 0000005900010008000000060000000d00007b2272657175657374436f6d6d616e644964223a33323736372c22737461747573223a226661696c6564222c226572726f72436f6465223a226e6f74537570706f72746564227d`

</details>

## 14. 反向发送MCU心跳命令

连接 1；PC命令 `0x0001` / Seq `113`；接收等待 0ms；符合预期。

PC发送包体：

```json
{}
```

MCU响应命令 `0x0006` / Seq `14`：

```json
{"requestCommandId":1,"status":"failed","errorCode":"invalidDirection"}
```

<details><summary>原始帧十六进制</summary>

`TX 0000001400010008000000010000007100007b7d`

`RX 0000005900010008000000060000000e00007b2272657175657374436f6d6d616e644964223a312c22737461747573223a226661696c6564222c226572726f72436f6465223a22696e76616c6964446972656374696f6e227d`

</details>

## 15. 开始测试缺少索引

连接 1；PC命令 `0x0103` / Seq `114`；接收等待 0ms；符合预期。

PC发送包体：

```json
{"algorithmName":"algorithmName"}
```

MCU响应命令 `0x0006` / Seq `15`：

```json
{"requestCommandId":259,"status":"failed","errorCode":"invalidArgument"}
```

<details><summary>原始帧十六进制</summary>

`TX 0000003300010008000001030000007200007b22616c676f726974686d4e616d65223a22616c676f726974686d4e616d65227d`

`RX 0000005a00010008000000060000000f00007b2272657175657374436f6d6d616e644964223a3235392c22737461747573223a226661696c6564222c226572726f72436f6465223a22696e76616c6964417267756d656e74227d`

</details>

## 16. 合法开始测试：观察1.5秒

连接 1；PC命令 `0x0103` / Seq `115`；接收等待 1500ms；符合预期。

PC发送包体：

```json
{"algorithmName":"algorithmName","algorithmIndex":1}
```

MCU行为：观察窗口内未收到响应。

<details><summary>原始帧十六进制</summary>

`TX 0000004600010008000001030000007300007b22616c676f726974686d4e616d65223a22616c676f726974686d4e616d65222c22616c676f726974686d496e646578223a317d`

</details>

## 17. 开始测试后连接仍可心跳

连接 1；PC命令 `0x0002` / Seq `116`；接收等待 0ms；符合预期。

PC发送包体：

```json
{}
```

MCU响应命令 `0x0001` / Seq `16`：

```json
{}
```

<details><summary>原始帧十六进制</summary>

`TX 0000001400010008000000020000007400007b7d`

`RX 0000001400010008000000010000001000007b7d`

</details>

## 18. 反向发送0x0105含resultDesc（不是MCU结果上报）

连接 1；PC命令 `0x0105` / Seq `117`；接收等待 0ms；符合预期。

PC发送包体：

```json
{"algorithmName":"algorithmName","algorithmIndex":1,"testResults":[{"siteId":"","mirrorIndex":1,"resultDesc":"","testStatus":1}]}
```

MCU响应命令 `0x0006` / Seq `17`：

```json
{"requestCommandId":261,"status":"failed","errorCode":"notSupported"}
```

<details><summary>原始帧十六进制</summary>

`TX 0000009300010008000001050000007500007b22616c676f726974686d4e616d65223a22616c676f726974686d4e616d65222c22616c676f726974686d496e646578223a312c2274657374526573756c7473223a5b7b22736974654964223a22222c226d6972726f72496e646578223a312c22726573756c7444657363223a22222c2274657374537461747573223a317d5d7d`

`RX 0000005700010008000000060000001100007b2272657175657374436f6d6d616e644964223a3236312c22737461747573223a226661696c6564222c226572726f72436f6465223a226e6f74537570706f72746564227d`

</details>

## 19. 结果灯拒绝多余字段

连接 1；PC命令 `0x0701` / Seq `118`；接收等待 0ms；符合预期。

PC发送包体：

```json
{"target":"lpddr0","state":"pass","extra":1}
```

MCU响应命令 `0x0006` / Seq `18`：

```json
{"requestCommandId":1793,"status":"failed","errorCode":"invalidArgument"}
```

<details><summary>原始帧十六进制</summary>

`TX 0000003e00010008000007010000007600007b22746172676574223a226c7064647230222c227374617465223a2270617373222c226578747261223a317d`

`RX 0000005b00010008000000060000001200007b2272657175657374436f6d6d616e644964223a313739332c22737461747573223a226661696c6564222c226572726f72436f6465223a22696e76616c6964417267756d656e74227d`

</details>

## 20. 分片心跳验证半包

连接 1；PC命令 `0x0002` / Seq `119`；接收等待 94ms；符合预期。

PC发送包体：

```json
{}
```

MCU响应命令 `0x0001` / Seq `19`：

```json
{}
```

<details><summary>原始帧十六进制</summary>

`TX 0000001400010008000000020000007700007b7d`

`RX 0000001400010008000000010000001300007b7d`

</details>

## 21. 粘包第1帧：心跳

连接 1；PC命令 `0x0002` / Seq `120`；接收等待 0ms；符合预期。

PC发送包体：

```json
{}
```

MCU响应命令 `0x0001` / Seq `20`：

```json
{}
```

<details><summary>原始帧十六进制</summary>

`TX 0000001400010008000000020000007800007b7d`

`RX 0000001400010008000000010000001400007b7d`

</details>

## 22. 粘包第2帧：电源查询

连接 1；PC命令 `0x0503` / Seq `121`；接收等待 31ms；符合预期。

PC发送包体：

```json
{}
```

MCU响应命令 `0x0504` / Seq `21`：

```json
{"powerState":"off"}
```

<details><summary>原始帧十六进制</summary>

`TX 0000001400010008000005030000007900007b7d`

`RX 0000002600010008000005040000001500007b22706f7765725374617465223a226f6666227d`

</details>

## 23. 非法JSON尾随逗号

连接 2；PC命令 `None` / Seq `None`；接收等待 0ms；符合预期。

PC发送包体：

```json
{"a":1,}
```

MCU行为：关闭TCP连接，无JSON响应。

<details><summary>原始帧十六进制</summary>

`TX 0000001a00010008000000020000038400007b2261223a312c7d`

</details>

## 24. 重复JSON键

连接 3；PC命令 `None` / Seq `None`；接收等待 0ms；符合预期。

PC发送包体：

```json
{"a":1,"a":2}
```

MCU行为：关闭TCP连接，无JSON响应。

<details><summary>原始帧十六进制</summary>

`TX 0000001f00010008000000020000038500007b2261223a312c2261223a327d`

</details>

## 25. 非法UTF-8

连接 4；PC命令 `None` / Seq `None`；接收等待 0ms；符合预期。

PC发送包体：

```json
{"a":"�"}
```

MCU行为：关闭TCP连接，无JSON响应。

<details><summary>原始帧十六进制</summary>

`TX 0000001b00010008000000020000038600007b2261223a22ff227d`

</details>

## 26. JSON内嵌NUL转义

连接 5；PC命令 `None` / Seq `None`；接收等待 0ms；符合预期。

PC发送包体：

```json
{"a":"\u0000"}
```

MCU行为：关闭TCP连接，无JSON响应。

<details><summary>原始帧十六进制</summary>

`TX 0000002000010008000000020000038700007b2261223a225c7530303030227d`

</details>

## 27. 非零ServiceID

连接 6；PC命令 `None` / Seq `None`；接收等待 0ms；符合预期。

PC发送包体：

```json
{}
```

MCU行为：关闭TCP连接，无JSON响应。

<details><summary>原始帧十六进制</summary>

`TX 0000001400010008000100020000038800007b7d`

</details>

## 28. Length小于18

连接 7；PC命令 `None` / Seq `None`；接收等待 0ms；符合预期。

PC发送包体：

未发送完整JSON包体；见原始帧。

MCU行为：关闭TCP连接，无JSON响应。

<details><summary>原始帧十六进制</summary>

`TX 00000011`

</details>

## 29. Length超过4096

连接 8；PC命令 `None` / Seq `None`；接收等待 0ms；符合预期。

PC发送包体：

未发送完整JSON包体；见原始帧。

MCU行为：关闭TCP连接，无JSON响应。

<details><summary>原始帧十六进制</summary>

`TX 00001001`

</details>

## 30. Version不是1

连接 9；PC命令 `None` / Seq `None`；接收等待 0ms；符合预期。

PC发送包体：

未发送完整JSON包体；见原始帧。

MCU行为：关闭TCP连接，无JSON响应。

<details><summary>原始帧十六进制</summary>

`TX 000000140002`

</details>

## 31. Flag不是8

连接 10；PC命令 `None` / Seq `None`；接收等待 0ms；符合预期。

PC发送包体：

未发送完整JSON包体；见原始帧。

MCU行为：关闭TCP连接，无JSON响应。

<details><summary>原始帧十六进制</summary>

`TX 0000001400010007`

</details>

## 32. 截断包体触发半帧超时

连接 11；PC命令 `None` / Seq `None`；接收等待 593ms；符合预期。

PC发送包体：

```json
{
```

MCU行为：关闭TCP连接，无JSON响应。

<details><summary>原始帧十六进制</summary>

`TX 0000001400010008000000020000038d00007b`

</details>

## 33. 异常帧后重连恢复，Seq重新从1开始

连接 12；PC命令 `0x0002` / Seq `100`；接收等待 0ms；符合预期。

PC发送包体：

```json
{}
```

MCU响应命令 `0x0001` / Seq `1`：

```json
{}
```

<details><summary>原始帧十六进制</summary>

`TX 0000001400010008000000020000006400007b7d`

`RX 0000001400010008000000010000000100007b7d`

</details>

## 34. 最终电源状态

连接 12；PC命令 `0x0503` / Seq `101`；接收等待 0ms；符合预期。

PC发送包体：

```json
{}
```

MCU响应命令 `0x0504` / Seq `2`：

```json
{"powerState":"off"}
```

<details><summary>原始帧十六进制</summary>

`TX 0000001400010008000005030000006500007b7d`

`RX 0000002600010008000005040000000200007b22706f7765725374617465223a226f6666227d`

</details>
