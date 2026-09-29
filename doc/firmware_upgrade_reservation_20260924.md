# 固件升级目标与RK空间预留（2026-09-24）

## 本次修改

按用户要求仅预留RK存储和升级入口，不设计RK结构体、包头、版本记录或串口安装流程。

| 内容 | 结果 |
|---|---|
| RK固件空间A | W25Q256 `0x00410000–0x00C0FFFF`，8 MiB |
| RK固件空间B | W25Q256 `0x00C10000–0x0140FFFF`，8 MiB |
| 余下未分配空间 | `0x01410000–0x01FEFFFF`，11.875 MiB |
| 原有数据 | MCU下载/稳定、版本、FRU、启动日志和末尾自检区地址不变 |
| 通用命令 | 0901/0902/0903数值不变，枚举为`CMD_FIRMWARE_UPGRADE_REQUEST/RESP/COMPLETE`；C宏对应`PC_COMMAND_FIRMWARE_UPGRADE_*` |
| MCU动作 | `mcu_begin/mcu_write/mcu_finish/mcu_query/mcu_cancel`，请求及响应同名 |
| RK动作 | 预留`rk_begin/rk_write/rk_finish/rk_query/rk_cancel`，当前一律0006 `failed/notSupported`并携带action |

A/B仅为两个独立存放空间，不代表RK双槽回滚，也未冻结SPL/EDK如何分配。分区宏和断言已同步V2/B1；没有执行擦除、初始化或搬移数据。Boot仅使用既有MCU区域，不需修改安装算法或包格式。

RK保留请求在MCU下载后台轮询之前被拒绝，不解析MCU包字段、不刷新/取消MCU会话、不触发复位、不访问Flash或RK串口。重复键、非字符串action仍遵守现有参数/JSON检查。正常任务自己的定时轮询不因RK预留入口停用；本次保证的是RK请求不触发MCU升级动作，而非禁止系统既有超时保护。

## 兼容与文档

新固件拒绝旧无前缀动作。Python客户端默认发送新动作；只为迁移已部署旧固件保留显式`--legacy-ota-actions`，日常PowerShell入口对应`-LegacyOtaActions`且仅允许`ota-net`。提交后先查询`mcu_query`，只有显式迁移且收到notSupported才只读回退`query`。写入/提交失败不自动换动作重发。操作见[日常下载说明](ota_daily_download.md)。

MCU包仍为原有`.ota`格式、2MiB下载区、896KiB App镜像上限。原始分块最多3000字节，Base64最多4000字符；紧凑`mcu_write`在最长合法偏移下完整PDU为4090字节，按u32最大偏移保守预算为4093字节。仍须检查实际UTF-8编码长度≤4096，额外空白也占容量。

同步了V2/B1的PC协议Markdown、V2 Flash布局、OTA设计和固定名称`doc/pc_tcp协议.docx`；Word原稿按修改时间归档为`doc/history/pc_tcp/pc_tcp协议_20260924_113446.docx`，SHA-256见[归档表](history/pc_tcp/README.md)。本轮Word修改保留上轮0105内容，不表示新结果格式已在固件实现；B1的0105章节未借此次升级同步改动。飞书未修改。

## 验证及复跑

| 检查 | 结果与边界 |
|---|---|
| V2/B1分区连续性、容量及对齐 | 通过；既有MCU和自检地址锁定，布局头独立ARM C11编译通过 |
| 两板实际OTA C/QEMU | 各7组通过；覆盖新MCU五动作、旧动作拒绝、非OTA镜像拒绝、RK在空闲/下载/待复位状态的无副作用边界 |
| 两板正式PC分发C/QEMU | 通过 |
| Python真实客户端离线编排 | 新动作升级及旧上传→旧App查询→新App确认通过；未使用真实socket连接板卡 |
| 文档OTA合同 | 25个JSON及39个反例通过，4090/4093字节预算通过 |
| 既有PC/代码边界检查 | 24项PC、19项边界通过 |
| 普通Debug | V2 FLASH占用230496 B；B1 222364 B，均通过 |
| 0.1.16 OTA Release | V2镜像144784 B、1394处修正；B1 UART4/USART2均140148 B、1381处修正；各A1/A2构建及正反向逐字节修正一致 |
| 构建配置 | OTA版槽限896KiB，OTA_BOOT/RELOCATION开启，V2临时RK联调门关闭；0.1.16仅离线候选，未安装 |
| Word内容与排版 | 25个正文JSON可解析；0105不变；默认渲染器缺LibreOffice后使用WPS后台只读导出16页，全部页面检查通过 |

```powershell
python -B -X utf8 tests/test_version.py
python -B -X utf8 tests/test_ota_dev.py
python -B -X utf8 tests/verify_pc_ota_contract.py
python -B -X utf8 tests/verify_pc_protocol.py
python -B -X utf8 tests/verify_code_boundaries.py

# 现有包仅作离线安装测试输入，不代表本轮构建/烧录版本。
$otaReviewCompiler = 'C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc.exe'
python -B -X utf8 tests/verify_ota_app.py --package build/ota-app-0.1.15-release-a1/test.ota --compiler $otaReviewCompiler
python -B -X utf8 tests/verify_ota_app.py --app-root ../b1 --target b1-h743 --package ../b1/build/ota-app-0.1.8-release-a1/b1.ota --compiler $otaReviewCompiler
python -B -X utf8 tests/verify_pc_start_c.py --compiler $otaReviewCompiler
python -B -X utf8 tests/verify_pc_start_c.py --app-root ../b1 --compiler $otaReviewCompiler
```

实际C/QEMU报告保存在两工程的`build/ota-app-test/`和`build/pc-start-test/`。地址修正报告在各工程`build/ota-app-0.1.16-release-a1/relocation_report.json`，B1第二UART配置在同名带`-uart2`目录；报告明确为离线构建，不是新OTA安装包。

本轮没有连接ST-Link、网络联调、烧录、读取或改写板上Flash，也没有提交或推送。真板新动作及旧版本首次迁移仍待后续授权实测；RK升级仍不具备可执行能力。
