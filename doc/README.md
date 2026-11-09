# MCU 项目文档入口

本页只作导航。当前进度只维护在[任务清单](driver_tasks.md#current-status)，不再把同一轮修改重复追加到多个入口。带日期的记录说明当时的范围与验证，不能用旧记录里的“尚未实现”覆盖后续代码。

## 按需要阅读

| 要做什么 | 入口 |
|---|---|
| 接手项目、确认工程约束 | [AGENTS](../AGENTS.md)、[当前进度与待办](driver_tasks.md#current-status) |
| 确认职责、编号、IP和运行边界 | [需求基线](requirements_baseline.md)；术语见[CONTEXT](../CONTEXT.md) |
| 修改PC协议或业务 | [PC协议Markdown](pc_tcp_protocol_spec.md)、[PC协议Word](pc_tcp协议.docx)、[内部JSON合同](pc_json_contract.md) |
| 修改RK协议 | [现行RK协议](protocol_spec.md)、[时序与启动频率接入](rk_timing_frequency_20261105.md)、[11月5日服务器核查与修改边界](rk_protocol_review_20261105.md)、[双颗粒结果解析](rk_dual_die_result_20261010.md)、[进度双TLV](rk_progress_dual_tlv_20261009.md) |
| 联调PARA与IDD编号 | [IDD正式接入与5s逐点采集](idd_session_prepare_20261105.md)、[接入范围及待办](para_integration_20261026.md)、[16项JSON及网络助手HEX](examples/para_start_packets_20261026.md) |
| 修改B1–B2 SPI或动态IP | [SPI协议Markdown](b1_b2_spi_protocol.md)、[SPI协议Word](b1_b2_spi协议.docx)、[接入与调用链](b2_spi_integration_20261020.md) |
| 用第二块V2模拟B2 | [裸机主机的接线、串口命令与构建](../tools/b2_spi_master/README.md) |
| 单块V2双SPI回环验证 | [同级独立工程、Flash下载、4线接法、板载灯及协议日志](../../b2_spi_loopback/README.md) |
| 调试B1电源、Boot、采样 | [B1业务初值](b1_power_business_20261013.md)、[电源硬件依据](b1_power_control_review.md)、[电压/电流独立采集](sampling_schedule_20261026.md)、[引脚表](b1_mcu_pinout.md) |
| 图形窗口循环验板 | [三轨电压清单、Boot上下电循环及CSV记录](../tools/b1_debug/README.md)；复用050x/060x，不增加协议字段 |
| 修改版本/FRU及外置Flash布局 | [Flash布局](flash_layout.md)、[直接烧录](direct_flash.md) |
| 日常下载、空白板首烧及工厂交付 | [B1/V2直接烧录](direct_flash.md)；两板各自产出Boot＋App合并文件，一次下载 |
| 查驱动修复和测试依据 | [驱动审查记录](driver_review_and_changes.md)、[近期修复](audit_fixes_20261015.md)、[测试报告目录](test_reports/) |

RK Word `mcu_rk协议.docx`保留供历史查阅；其中早期四TLV结果示例不能替代现行3/5/7 TLV合同。需要对外发送前应先按现行Markdown核对，不能只凭文件名认作已同步。

## 文档维护规则

- `driver_tasks.md`只保留当前状态、下一步和验收边界；详细修改写入已有主题记录，不追加长篇历史到入口。
- 对外协议只写帧、字段、请求/响应及双方必须遵守的规则。内部任务分工、时序推导、调试与故障记录留在内部文档。
- 最新Word保持固定名。编辑前按旧文件修改时间保存到对应历史目录；同名加序号，不覆盖归档。正在编辑的Word先保存、关闭，再替换。
  - PC：`history/pc_tcp/pc_tcp协议_yyyyMMdd_HHmmss.docx`
  - RK：`history/rk/mcu_rk协议_yyyyMMdd_HHmmss.docx`
  - SPI：`history/b1_b2_spi/b1_b2_spi协议_yyyyMMdd_HHmmss.docx`
- 代码行为、协议约定与硬件实测是不同依据。发现不一致先定位差异，不把构建/Mock通过写成真板通过。
- 2026-10-28恢复精简Boot＋单App；Boot=0x08000000、App=0x08020000，MCU升级仍停用。见[修改记录](minimal_boot_20261028.md)。旧OTA记录只作历史证据，不能套用其旧地址/命令。PC协议Word和飞书尚未同步本次090x停用说明，以本轮Markdown与源码为准。

## 历史资料与恢复

2026-10-20将旧DQ透传方案、过期暂停交接、AD7124分段提取稿和旧原理图渲染图片移出常用目录；精简前的三个入口文档也已保留原文快照。文件清单、SHA256及恢复方法见[清理归档](history/cleanup_20261020/README.md)。

原始PDF、当前协议、Word历史版本、器件测试证据和烧录备份未删除。`.rgignore`让默认全文搜索跳过历史及临时产物；需要追溯时显式指定路径或使用`rg --no-ignore`，不必每次整库读取。
