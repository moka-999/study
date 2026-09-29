# V2连接真实RK：功能测试接入与用法

## 本轮修改

- 正式固件的`pc_test_service`连接PC命令表与RK队列，不再依赖模拟开关；TCP任务只收发完整PDU。
- `power:null, bootMode:null, reboot:false`明确保持外部已启动的RK现状。旧电压对象/Boot请求仍由完整B1流程门控，SLT/IDD本轮不执行。
- 按用户确认，每算法只接收一个四TLV最终结果，不复制成两颗芯片的独立结论。0首错地址表示整体PASS，非0为FAIL；地址完整保留，未确认地址映射前不猜芯片。
- 默认无限等待算法结果，仍处理通信帧超时/心跳。可选`timeoutMs`为0或省略时无限等，非零范围1..86400000毫秒。
- 单在途；等待及有限超时后的未知状态禁止重复开始、改电源/Boot/电压和升级写入。通信失败是0006 failed，不冒充芯片FAIL。迟到结果只解除未知，不再报第二次结果。
- 断线不取消RK；旧测试结束前继续busy，旧结果不发给新连接。没有停止命令，不能靠断线或反复发包停止算法。
- 新/修改注释用中文。独立B1工程本轮不改；V2源码中的B1分支仍拒绝外部启动模式，不能绕过整机供电状态机。

### 0105输出顺序对齐（2026-09-28，同日增量）

- V2正式结果构造器按协议示例输出：`siteId → mirrorIndex → testStatus → resultDesc → physicalAddr → measurementData`。JSON仍按字段名解释，接收端不应依赖对象成员位置。
- 只调整cJSON添加字段的先后；PASS/FAIL、64位首错地址、未分配编号和`measurementData:null`的语义不变，未接入测量统计。
- 在现有PC/RK实际C回归中增加序列化后字段顺序断言，原实现先复现失败，修改后通过；参数/结果构造C回归也通过。仅离线QEMU，不等于板上镜像已更新。
- 本增量未修改独立B1、Word或飞书，未烧录、提交或推送。

## 网络助手怎么发送

全部 20 个 FUNC 算法的 JSON 与完整单行 HEX 见[算法发送包清单](func_algorithm_network_packets.md)，一次只发送一项。该清单现用于0.1.18电源模拟配置：前10项三轨+1档、后10项三轨-1档；普通外部已启动模式仍使用下文power/bootMode均为null的包，不能混淆两种适用范围。

前提：RK已进入同事的算法服务、串口参数/电平兼容，且空闲。V2为USART1 PA9(TX)/PA10(RX)，115200、8N1、无流控。不要让另一串口发送端同时驱动该接收线。

网络助手选择TCP客户端，连接测试地址`192.168.1.201:8888`，勾选HEX发送。不能只发送裸JSON文本；必须携带18字节头。一次只连一个PC客户端。

### 先查心跳（不会启动RK算法）

```text
00 00 00 14 00 01 00 08 00 00 00 02 00 00 00 01 00 00 7B 7D
```

收到0x0001、正文{}表示PC↔MCU通，不能据此断言RK就绪。

### 开始MATS（会实际下发RK命令17）

新连接发送Seq=1，算法计划index=1。以下JSON仅供阅读：

```json
{"algorithmType":"functional","algorithmName":"mats","algorithmIndex":1,"power":null,"reboot":false,"bootMode":null}
```

对应完整134字节HEX如下；若同一连接已经发过心跳，把头部Seq的`00 00 00 01`改为`00 00 00 02`，后续本端递增。

```text
00 00 00 86 00 01 00 08 00 00 01 03 00 00 00 01 00 00
7B 22 61 6C 67 6F 72 69 74 68 6D 54 79 70 65 22 3A 22 66 75 6E 63 74 69 6F 6E 61 6C 22 2C
22 61 6C 67 6F 72 69 74 68 6D 4E 61 6D 65 22 3A 22 6D 61 74 73 22 2C
22 61 6C 67 6F 72 69 74 68 6D 49 6E 64 65 78 22 3A 31 2C
22 70 6F 77 65 72 22 3A 6E 75 6C 6C 2C 22 72 65 62 6F 6F 74 22 3A 66 61 6C 73 65 2C
22 62 6F 6F 74 4D 6F 64 65 22 3A 6E 75 6C 6C 7D
```

先收到0x0006：

```json
{"requestCommandId":259,"algorithmIndex":1,"status":"accepted"}
```

accepted仅表示MCU接受并排队，不能证明RK已经开始。等RK回真实结果后收到0x0105，整体通过示例：

```json
{"algorithmType":"functional","algorithmName":"mats","algorithmIndex":1,"testResults":[{"siteId":"","mirrorIndex":0,"testStatus":1,"resultDesc":{"firstErrorAddress":null,"errorType":null},"physicalAddr":"","measurementData":null}]}
```

| 字段/现象 | 含义 |
|---|---|
| siteId | W25 FRU站点标签；未配置为空，不用编译期默认值冒充编号 |
| mirrorIndex=0 | 本次没有分配B2/测试机编号，不是RK die |
| physicalAddr为空 | 尚无地址到芯片映射；一个结果是算法整体结论 |
| testStatus=1 / 2 | 本轮功能测试PASS / FAIL；依据RK首错地址0 / 非0 |
| firstErrorAddress | PASS为null；FAIL如`0x0000000123456780`，保留64位地址 |
| errorType / measurementData=null | RK未提供错误类型、本次未连接对应测量，不表示0或正常 |
| 只有accepted、一直无0105 | RK可能仍执行，也可能链路/结果格式不匹配；不自动重发、不当作PASS |
| busy | 旧请求尚未结束；断开网络不会清除它 |

更换算法名称或index后必须重新计算JSON字节数和Length，不要只修改上述HEX中的文字。PC与MCU发送Seq各自递增，不回显；以algorithmIndex对应业务。

## 验证记录

| 验证 | 结果及证据 |
|---|---|
| 普通Debug构建 | 通过；不是直接拿此ELF覆盖已部署OTA的开发板 |
| Release双槽地址修正 | A1/A2逐字节及反向核验通过；App 146848字节，1433处修正，OTA包152696字节；`build/ota-app-0.1.17-release-a1/relocation_report.json` |
| 真实C JSON/分发 | 通过；`build/pc-start-test/run.log` |
| 真实C PC业务及B1条件门禁 | 通过；`build/pc-rk-service/run.log`、`build/pc-rk-service-b1-guard/run.log`；底层队列事件/FRU/OTA状态为测试桩 |
| 真实C RK事务 | V2及B1掉电条件分支均通过；20分钟无限等待、tick回绕、有限超时/迟到恢复/背压；`build/rk-protocol-test/run.log`、`build/rk-protocol-test-b1-gate/run.log` |
| PC静态合同/架构边界 | 24项/19项通过，不能代替C运行或硬件验收 |
| 开发板网络OTA | 0.1.15→**0.1.17**，完整文件提交、B安装、30秒健康确认及0903通过；`build/rk-real-upgrade-20260928.json` |
| 更新后实际TCP | 11请求通过：版本/健康、心跳、W25信息、非法/不支持/旧电源模式拒绝；`build/rk-real-network-check-20260928.json` |
| 实际RK算法与最终结果 | **本轮未发起**，留给操作者用网络助手发送；以上构建/离线/拒绝路径不能冒充实机算法通过 |

本次走已有网络OTA，保留旧运行槽供回滚；未连接ST-Link、未改Bootloader、未重做整片备份。网络连接已释放。独立B1工程、Word及飞书本轮未修改，未提交/推送。新外部启动JSON以本记录及Markdown协议4.7为准。

可直接复制[完整MATS HEX](../output/mats_start_0103.hex.txt)到网络助手的HEX发送框。该文件不含说明文字；它会触发真实RK算法17，不要反复点击发送。
