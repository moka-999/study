# 测试仪名称更新与 V2 安装记录（2026-10-09）

## 修改范围

用户确认测试仪信息为以下三字段；命令仍为 `0x0101 + {}` 查询、`0x0102` 响应，不增加协议字段：

```json
{
  "testerName": "JMCT-4000",
  "testerVersion": "D00",
  "testBoard": "RK3588_test"
}
```

- V2 与独立 B1 的 `pc_tester_info_load()` 沿用 W25Q256 唯一运行时数据源。有效 FRU 中的旧默认名称 `LPDDR4/X_200B` 一次性迁移为 `JMCT-4000`，保存后重新读取才允许响应；之后查询只读。
- 迁移仅改 `tester_name`，保留 `product_name`、硬件版本、测试板、序列号、MAC、IP、站点及保留字段；定制的其它名称不强制覆盖。内部产品型号仍为 `LPDDR4/X_200B`。
- 复用现有事务锁、CRC 和双副本，只覆盖较旧 FRU 副本。FRU 结构、版本号和分区不变；初始化路径使用新测试仪名称。
- 同步本地 Markdown、Word 示例与相关测试夹具。旧 Word 已按原修改时间保存为 `doc/history/pc_tcp/pc_tcp协议_20261009_120958.docx`，SHA-256 为 `8345a79dde4ed4e778b43d5bcee09701b29451fa1347d679c36e0a495fe276a1`。

## 验证

| 检查 | 结果与边界 |
|---|---|
| 实际 C / QEMU | V2、B1 的信息加载与 `flash_layout.c` 均通过；RAM 模拟介质验证旧名迁移、整个 FRU 其余字节保留、重复读取不写、256 个写入中断点恢复、自定义名、读写/锁失败及空白初始化；不是实板断电试验 |
| 既有合同 | V2 PC 24 项、布局/版本/FRU、分层 19 项及纯接口头检查通过；B1 工程检查、旧 FRU 模型与 Debug 构建通过 |
| V2 发布构建 | Release 0.1.29，App 148516 字节，1482 处地址修正；A2 逐字节比对及反向还原 A1 通过 |
| Word | 仅替换一个名称文本节点，其它正文 XML 不变。标准渲染器缺少 soffice，使用本机 WPS COM 后台只读导出及 PDFium 渲染，19 页逐页检查无截断/重叠 |
| V2 实板 | 正式网络 OTA、0903、30 秒健康确认、重复信息查询、重连查询及 TCP 心跳通过；未执行 RK 算法 |

复跑新增离线检查：

```powershell
python -B -X utf8 tests/verify_pc_tester_info.py
python -B -X utf8 tests/verify_pc_tester_info.py --app-root ../b1
```

## 已安装 V2

- 原版本 `0.1.28 / 0314B38A`，新版本 `0.1.29 / AE91A07F`，最终 `confirmed=true`。
- 包：`build/ota-app-power-sim-0.1.29-release-a1/test.ota`，154560 字节；SHA-256：`7cf01eaa59c43b836ce853df0a3f36a06206aa4430e6a5079254f6cda4da771c`。
- 保留完整 OTA、正式任务、真实 RK 链路及 V2 电源/测量模拟；未打开旧隔离网关联调开关。
- 升级前已确认用户空闲及模拟电源 OFF；本轮不发上/下电、算法或取消命令。16:25 实测三次连续 `0x0101` 和一次重连查询均返回上述新信息；模拟电源仍 OFF、循环计数 0。
- 证据：`build/jmct4000_20261009/board_before.json`、`ota_upgrade_0.1.29.json`、`board_after.json`；所有本轮 TCP 连接已关闭，没有使用 ST-Link 或串口。
- 本次未读取整片 Flash 备份，依靠既有 OTA 旧槽/日志保护及 FRU 双副本迁移；未做实板断电、RK 算法或 B1 实板验收。
- B1 仅同步源码与离线验证，未烧录；既有首烧包未重打。未提交/推送，也未同步本次名称到飞书。
