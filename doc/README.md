# MCU 项目文档入口

本页只作导航。当前进度只维护在[任务清单](driver_tasks.md#current-status)，不再把同一轮修改重复追加到多个入口。带日期的记录说明当时的范围与验证，不能用旧记录里的“尚未实现”覆盖后续代码。

## 按需要阅读

| 要做什么 | 入口 |
|---|---|
| 接手项目、确认工程约束 | [AGENTS](../AGENTS.md)、[当前进度与待办](driver_tasks.md#current-status) |
| 确认职责、编号、IP和运行边界 | [需求基线](requirements_baseline.md)；术语见[CONTEXT](../CONTEXT.md) |
| 修改PC协议或业务 | [PC协议Markdown](pc_tcp_protocol_spec.md)、[PC协议Word](pc_tcp协议.docx)、[内部JSON合同](pc_json_contract.md) |
| 修改RK协议 | [现行RK协议](protocol_spec.md)、[双颗粒结果解析](rk_dual_die_result_20261010.md)、[进度双TLV](rk_progress_dual_tlv_20261009.md) |
| 修改B1–B2 SPI或动态IP | [SPI协议Markdown](b1_b2_spi_protocol.md)、[SPI协议Word](b1_b2_spi协议.docx)、[接入与调用链](b2_spi_integration_20261020.md) |
| 用第二块V2模拟B2 | [裸机主机的接线、串口命令与构建](../tools/b2_spi_master/README.md) |
| 单块V2双SPI回环验证 | [同级独立工程、Flash下载、4线接法、板载灯及协议日志](../../b2_spi_loopback/README.md) |
| 调试B1电源、Boot、采样 | [B1业务初值](b1_power_business_20261013.md)、[电源硬件依据](b1_power_control_review.md)、[引脚表](b1_mcu_pinout.md) |
| 修改OTA或Flash布局 | [OTA设计](ota_design.md)、[升级协议](pc_ota_protocol.md)、[Flash布局](flash_layout.md)、[完整Boot工程](../../bootloader/README.md) |
| 日常下载、空白板首烧 | [日常下载](ota_daily_download.md)、[空白板首烧](../../bootloader/README.md#空白板首次部署v2--b1)、[独立网络工具](ota_standalone_tools_20261015.md) |
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
- 已部署完整OTA的板卡按日常下载入口操作，禁止直接覆盖旧最小`boot.elf`或普通App ELF。

## 历史资料与恢复

2026-10-20将旧DQ透传方案、过期暂停交接、AD7124分段提取稿和旧原理图渲染图片移出常用目录；精简前的三个入口文档也已保留原文快照。文件清单、SHA256及恢复方法见[清理归档](history/cleanup_20261020/README.md)。

原始PDF、当前协议、Word历史版本、器件测试证据和烧录备份未删除。`.rgignore`让默认全文搜索跳过历史及临时产物；需要追溯时显式指定路径或使用`rg --no-ignore`，不必每次整库读取。
