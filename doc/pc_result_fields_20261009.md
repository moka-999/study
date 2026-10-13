# 0105结果字段修正与V2升级（2026-10-09）

## 已确认合同

| 字段 | 输出 |
| --- | --- |
| testStatus | 整数0=FAIL、1=PASS；RK 0x1017仍是0=PASS、1=FAIL，MCU显式转换 |
| mirrorIndex | RK 0x3001原值，0=第一颗DDR、1=第二颗；不加1；其它原值不擅自重映射 |
| resultDesc.errorType | 字符串；本次V2强制MATS FAIL填`SAF`，通过或真实RK未提供类型时填空串 |
| physicalAddr | 保持空字符串，含义保留 |

不新增JSON字段；index、耗时、首错地址和18项测量统计沿用原来源。MATS无真实首错地址时仍使用演示地址`0x0000000012345678`。真实RK必须先完整结束，模拟FAIL不绕过结果收齐条件。若结果未被MCU接收，不会仅凭打印的PASS生成0105。

修改位置：`pc_test_message.c/.h`统一组包及入参检查；`pc_test_service.c`仅在显式MATS联调配置填SAF；旧隔离测试适配器的状态映射同步0/1，但未启用该任务。调用者、实际C测试、当前PC/RK协议Markdown与本地Word同步修正，修改说明不混入源代码历史注释。

## 验证

- 先改预期FAIL=0，确认旧实现回归失败；实现后通过。
- 实际C/QEMU：20算法×4组结果，共80组，核对0/1、DDR两颗编号、SAF仅作用MATS、原始结果不改及所有生命周期门禁。
- 普通V2、电源模拟及本源码B1条件门禁回归通过；不代表独立B1已同步。
- JSON组包边界、64位地址/耗时、errorType空串与非空串、NULL入参、地址与PASS/FAIL独立关系通过。
- PC协议24项、分层19项及公共头文件独立编译通过。
- Release App148604字节、1484项地址修正，A2逐字节匹配及反向A1核对通过；OTA包154656字节。

## 实板升级

用户确认RK空闲、PC断开，允许必要时仅重启MCU清除busy。旧MATS现场已保存，见[未上报定位记录](mats_result_missing_20261009.md)。初次只读rk_query仍回busy，随后一次SWD软件复位，未直接写内部Flash。旧App健康确认、电源模拟OFF及查询恢复后，经TCP正式OTA安装：

| 项目 | 结果 |
| --- | --- |
| 旧版本 / 包ID | 0.1.30 / 5520E4C7 |
| 新版本 / 包ID | 0.1.31 / 7452B5BE |
| OTA SHA-256 | cd496750787e28d347143adf644d3c2bd41090309cd6ca6fb70dd065a1f6f4e3 |
| 开始升级 | 2026-10-09 18:05:24（本机时间） |
| 验收 | 完整包提交、重复finish幂等、Boot安装、0903、30秒健康确认、心跳与重连查询通过 |
| 测试仪信息 | JMCT-4000 / D00 / RK3588_test |
| 电源 | 前后均为内存模拟OFF，未发送电源控制命令 |
| 任务配置 | OTA ON、电源模拟 ON、MATS FAIL ON；真实RK，旧网关隔离任务OFF |

原始报告位于`build/mats_result_20261009/`：`board_before_fields_upgrade.json`（记录busy）、`swd_reset_fields.log`、`board_after_reset.json`、`ota_upgrade_0.1.31.json`、`board_after_upgrade.json`。TCP与ST-Link均已释放。

未运行真实MATS，未修改100ms串口半帧超时；此版不能作为“结果漏收已修复”的证明。独立B1、飞书及远程仓库本次未同步，未新做整片Flash备份；网络升级沿用旧槽和启动日志保护。每次MATS强制FAIL仅供PC联调，重启不会关闭，应在联调完成后安装不带该开关的更高版本。

## 2026-10-10文档收尾

恢复昨天暂停的工作后，将已经准备好的0105字段修改稿替换到固定文件名`doc/pc_tcp协议.docx`。替换前按原稿修改时间归档到[pc_tcp协议_20261009_161948.docx](history/pc_tcp/pc_tcp协议_20261009_161948.docx)，并核对旧稿与候选稿SHA-256，未覆盖其它正文内容。

- 24个JSON示例严格解析通过；本次仅修改0105的状态、DDR编号及错误类型说明。
- 本机标准渲染入口因缺少LibreOffice未完成，改用WPS后台只读导出PDF，再以pypdfium2渲染；实际检查全部19页，无本次修改引起的截断或表格溢出。
- 与修改前的渲染结果逐页对照，仅第8、9页改变，其余17页像素一致；未把局部修正宣称为全协议内容审计。
- 正式Word SHA-256：`9a70a6b44d025c9fec4b5df6430a3d770210e5dcb600bda657a173b0b2a3cede`。
- 证据位于`build/mats_result_20261009/word_update.json`及`render_check.json`。本次文档收尾未连接硬件、烧录或下发算法。
