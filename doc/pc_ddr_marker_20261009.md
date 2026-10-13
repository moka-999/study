# 0105 DDR标志映射修改记录

## 用户确认的字段定义

| 字段 | 当前定义 |
|---|---|
| siteId | B1板标识，继续读取W25 FRU站点标签；未配置为空 |
| mirrorIndex | DDR标志，直接使用RK TLV 0x3001的uint8值0..255，不加减偏移；0也是有效值，不再表示B2/测试机编号 |
| physicalAddr | 保留字符串，具体含义暂不定义；固定输出空字符串`""` |

例如RK的0x3001 Value为`00`，PC结果包含`"mirrorIndex":0,"physicalAddr":""`；Value为`01`时mirrorIndex为1。MCU不根据首错地址推算或修正这个标志，也不把一个算法整体结果拆成两颗芯片的独立结果。

## 代码与协议修改

- V2与独立B1的`pc_test_service_poll()`不再把mirrorIndex固定传0，改传已经解析的`result.result.die`；复用既有组包函数，不新增字段或层级。
- 同步PC命令/组包接口、RK结果结构的中文注释。RK解码、Seq窗口、进度、最终结果聚合及PASS/FAIL逻辑不变。
- 更新两工程当前PC/RK Markdown、V2 JSON合同与需求基线、两工程AGENTS；带日期的历史记录仍保留当时定义，本记录取代其中旧的B2/physicalAddr含义。
- 对外Word仍为`doc/pc_tcp协议.docx`，只改0105示例中的physicalAddr及两个字段说明。原文件按最后修改时间归档为`doc/history/pc_tcp/pc_tcp协议_20260928_174309.docx`，SHA-256已核对。

## 验证

| 检查 | 结果 |
|---|---|
| 新回归先运行旧代码 | 非0 die仍输出0，断言失败，复现旧行为 |
| V2正式、V2电源模拟、B1真实路径C/QEMU | 全部通过；检查0、1、9、255原样输出，0/1/255分别覆盖PASS/FAIL，physicalAddr为空 |
| V2/B1开始测试与JSON组包C/QEMU | 通过；既有请求、异常恢复、结果格式不变 |
| V2/B1正式Debug构建 | 通过 |
| V2 style-test、B1 check | 通过 |
| Word内容与版面 | 25个JSON对象严格解析通过；非目标正文XML不变；WPS后台只读导出19页，逐页总览并放大检查变化的第8、9页，无截断或重叠 |

内置render_docx.py因运行时未提供LibreOffice失败，采用已有WPS后台导出脚本及捆绑PDFium完成渲染，不接管用户桌面。核对稿、渲染图片及归档校验记录在`build/pc_ddr_marker_20261009/`。

## 源码修改阶段边界

- 未连接TCP、串口或ST-Link，未下发算法、改电源或烧录。
- 板上最近一次已确认版本仍是0.1.27；本次字段修改仅在源码，不宣称实板生效。
- 未提交、推送或同步飞书；未重打首烧/OTA交付包。
- 测量仍遵守各构建配置：V2可使用既有电源模拟，B1走实际采样；共轨测量不能因mirrorIndex而变成单颗测量。

## 随后授权安装V2 0.1.28

用户要求同步飞书并烧录，确认RK空闲、网络助手已断开。首次TCP连接超时，用户检查并恢复网络后，重新查询0.1.27为confirmed、无在途busy、模拟电源OFF。没有发送0502或改电源。

- Release 0.1.28保留ENABLE_OTA_BOOT=ON、ENABLE_V2_POWER_SIMULATION=ON、ENABLE_RK_GATEWAY_TEST=OFF，即正式任务、真实RK与电源/测量模拟。
- App为148460字节，1481处地址修正；A2逐字节和反向A1验证通过。OTA包154500字节，包ID为0314B38A，SHA-256为81df594f0b94baf19dea59ddbb957996139e52a576d8420344b72422c10ffe5c。
- 按3000字节分块经正式TCP OTA安装，0903及30秒健康确认通过；随后从W25查询0.1.28/confirmed、目标包ID一致，0503为OFF、0602 validMask=0/statistics=null、心跳正常。
- TCP连接已关闭，无Established连接；未使用ST-Link或串口、未下发RK算法、未更新B1实板、未提交或推送。网络OTA未另读整片备份，沿用旧槽与启动日志保护，未改FRU/稳定包。
- 飞书首次读取遇到用户授权过期；用户于2026-10-09重新授权后，按lark-doc局部更新[在线协议](https://tcn47lisvzcv.feishu.cn/docx/GWdYdgSArojCm3xPAPzcJxbxnqh)，版本166→169。仅改mirrorIndex、physicalAddr两项说明及0105示例的physicalAddr空值；回读完整示例和字段表，忽略新代码块ID后与三处预期替换逐字一致，JSON严格解析通过。未重写整篇文档，未检查浏览器视觉排版，未再次连接MCU或烧录。

本次证据位于build/pc_ddr_marker_20261009/：board_before.json、ota_upgrade_0.1.28.json、board_after.json；云端同步前后原文及核对结果为feishu_before.json、feishu_after.json、feishu_sync_verification.json。真实RK的0105字段仍待操作者下发算法复验，离线映射回归不能替代该实测。
