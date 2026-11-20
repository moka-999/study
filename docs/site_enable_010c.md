# B1 站点与 DUT 使能（010C）

PC 在正式 TCP 8888 连接上发送 `010C`，每次提交完整配置。开关可以在 FUNC、PARA、SLT 或启动准备期间使用。所有应答均为 `0006`，其中 `requestCommandId` 为 `268`。

## 请求

下例开启测试仪 2 的站点 3，允许镜像 0 的 DUT 灯显示，屏蔽镜像 1 的 DUT 灯。

```json
{"testerId":2,"siteId":3,"siteEnabled":true,"dutEnabled":[{"mirrorIndex":0,"dut":[true]},{"mirrorIndex":1,"dut":[false]}]}
```

关闭同一站点可发送下面这份配置。

```json
{"testerId":2,"siteId":3,"siteEnabled":false,"dutEnabled":[]}
```

- `testerId` 和 `siteId` 均为 JSON 整数，当前物理范围分别是 1～21 和 1～8。B2 已分配时须两项均匹配。尚未分配的单板按当前 TCP 连接寻址，只检查编号范围。010C 不分配编号或 IP。
- `siteEnabled` 是布尔值。四个顶层字段齐全后，关站直接忽略 `dutEnabled` 的内容。开站要求镜像 0、1 各出现一次，按 `mirrorIndex` 匹配，顺序任意。当前每个镜像的 `dut` 都是只有一个布尔值的数组，下标 0 对应该镜像的第一颗 DUT。
- 目标、格式或开站配置有误时返回 `failed/invalidArgument`，原配置继续生效。

## 业务和灯

| 操作 | 立即生效的行为 | PC 可继续的条件 |
|---|---|---|
| 已开站，只改 DUT | 切换对应灯的可见性。运行、采样以及两颗的结果上报照常进行。恢复显示时使用最新灯态和当前闪烁相位。 | 收到 `completed` 即生效，可继续原整轮。 |
| 关站 | 结束当前整轮，拦住新业务，全灭三组结果灯和 MCU LED7。停止 FUNC、IDD 或 SLT，丢弃尚未交付的结果，沿既有流程下电和清理串口。 | `accepted` 表示已开始。电源状态为 OFF、网络及 RK 任务均清理完毕后回 `completed`。已经稳定关站时重复关闭直接回 `completed`。 |
| 重新开站 | 恢复 LED7，应用 DUT 掩码。结果灯从空闲开始，RK 保持 OFF。 | 等开站 `completed`，发 `0106` 建立新整轮，再由带电源和启动配置的 `0103/0108/010A` 启动 RK。 |

关站和正在下电期间维持 8888 连接，只处理 `0002` 心跳和 `010C`。其他合法请求（含 `0101`、`0601`）回 `failed/siteDisabled`。串口及 8889 调试口可继续看日志和 `status`，电源、手动网络交接及验灯命令受站点开关约束。

若 PC 关站后立即开站，第二条也会收到 `accepted`。MCU 先完成同一次下电，再按最新完整配置恢复，每条已受理的 `010C` 都按接收顺序返回终态。下电中被后续不同配置覆盖的开站请求回 `failed/cancelled`。最多保留 16 条待发送的开关终态，超出容量的请求回 `failed/busy`。下电或清理失败会保持闭站，PC 可重发 `010C` 重试。

开关保存在 RAM。PC 断开不撤销已受理的配置，旧连接的应答不发给新连接。MCU 复位后默认开站、两个 DUT 使能，RK 仍按原流程保持 OFF。站点开启时，独立 eMCP 灯继续沿用原来的整轮显示规则。

## 离线验证

在同级 `test` 工程运行站点专项，使用实际 B1 分发、解析、停机和应答代码。

```powershell
python -B -X utf8 tests/verify_pc_rk_service.py --app-root ../b1 --b1-guard --power-sequence --round --site
```

已对关站过程中再开站、下电失败重试、断线、目标不匹配、DUT 屏蔽期间保留两颗结果进行回归。结果灯驱动另以模拟 GPIO 核对 PG0～PG6 和闪烁相位。实板验收需观察实际灯珠、电源轨，以及开站后下一条测试命令的正常启动。
