# TCP异常输入恢复（2026-09-29）

同日后续：已同步独立B1正式TCP，实际B1源码通过坏输入恢复、PC业务及RK结果离线回归和两路UART构建。未在B1硬件验收，详细范围见[B1同步记录](../../b1/docs/real_test_sync_20260929.md)；下文“未同步B1”描述修复和V2烧录当时状态。

## 本次问题与处理

用户报告发送裸文本 `TEXT` 后，后面的正常帧不再响应。旧状态机把它当作非法Length并进入ERROR，TCP任务随后关闭连接；这不是RK算法等待。新增实际C回归先在旧代码复现该拒绝，再完成以下修正。

| 输入情况 | 修正后的行为 |
|---|---|
| 裸 `TEXT`、非法Length/Version/Flag/ServiceID | 在HEADER中逐字节丢弃不可能成立的前缀；保留可能的好帧前缀，不清空整次recv的数据 |
| `TEXT` 结束后暂时没有新数据 | 明确垃圾已经全部丢弃，不保留假半帧；连接可等待后续正常帧 |
| 固定头和完整长度有效，UTF-8/JSON语法、根类型、重复键等非法 | 不执行业务，返回 `0x0006`、`status=failed`、`errorCode=invalidArgument`；继续接收下一帧 |
| 正常帧、任意位置半包、粘包 | 原状态表、命令表、独立发送Seq和响应规则不变 |
| 本次恢复已丢弃4096字节后仍需再丢弃 | 返回恢复超限，由TCP任务关闭当前连接；重新连接清空状态 |
| 尚未收齐的候选头/正文，字节间空闲超过500ms | 保留原有断连保护；没有半帧的正常空闲连接不会因此断开 |

恢复只验证帧头前缀。完整头通过后，按Length收齐正文再严格解析JSON及业务字段；不搜索正文中嵌入的二进制帧，更不会绕过测试忙、OTA、电源或RK门禁。未知的合法命令仍返回 `notSupported`。

协议没有独立帧起始标记，容错不等于能够无歧义修复任意损坏数据。尤其是错误Length仍在18..4096范围且固定字段正确时，MCU无法知道真实边界；后续字节可能被当成该正文，不保证找回。不得为了“找回”而从正文中猜测并执行控制命令。半帧超时后PC需要重新连接，不能自动重发已受理的算法。

## 修改范围

- `App/protocol/pc/pc_state_machine.c/.h`：复用四态函数表，增加前缀校验、有界滑动和垃圾计数；仍固定4KiB缓冲，不动态分配。
- `App/protocol/pc/pc_dispatch.c/.h`：完整坏JSON走既有0006失败组帧；保留0103重复键时提取唯一合法index的行为。
- `App/tasks/pc/tcp_task.c`：只更新处理合同注释；socket生命周期、发送、半帧超时和异步结果流程不变。
- 更新实际C回归、Python合同模型和板载Mock断言，避免继续沿用“一次坏输入就必须断连”的旧预期。
- 本文件记录修复及验证，不把修改历史写进源代码注释。协议Markdown与AGENTS同步当前规则；Word、飞书和独立B1工程本轮未同步。

## 修复阶段离线验证

- 实际C/QEMU：旧代码先在裸TEXT用例失败，修正后通过。覆盖垃圾＋双心跳的49种两段切分、五种坏头、前缀重叠、四类命令的七种坏JSON、空正文、正文内嵌完整帧、4096字节帧、半帧保持、垃圾上限及重新初始化。电源、Flash、LED和OTA硬件入口在该回归中一旦被调用就判失败。
- PC合同模型24项、代码分层/事件检查19项通过；后者不是C运行。
- PC/RK业务实际C在普通、V2电源模拟、本源码B1条件门禁三种配置下通过；没有同步或验证独立B1工程。
- V2普通Debug、板载Mock镜像构建通过。Mock镜像只编译，未上板运行。
- `0.1.22` 电源模拟Release双槽构建通过：App 147996字节、1480处地址修正，A1转A2及反向逐字节一致；OTA包154032字节。
- 构建产物：`build/ota-app-power-sim-0.1.22-release-a1/test.ota`。保持电源/Boot/测量模拟、真实RK通信、2秒RK心跳和无simulation响应字段。

上述代码修复阶段没有连接开发板、串口或ST-Link，也未烧录、提交或推送；当时板上仍为0.1.21。后续经用户授权安装与真实TCP验收见下一节，不能把离线结果直接当作真板结果。

离线复跑：

```powershell
python -B -X utf8 tests/verify_pc_start_c.py --compiler "C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc.exe"
python -B -X utf8 tests/verify_pc_protocol.py
python -B -X utf8 tests/verify_code_boundaries.py
python -B -X utf8 tests/verify_pc_rk_service.py
python -B -X utf8 tests/verify_pc_rk_service.py --power-simulation
```

## 0.1.22安装与实板TCP验收（2026-09-29）

用户要求烧录并自行重启MCU。Windows仍列出网络助手原连接，但新的只读查询实际正常，因此不能仅凭该旧连接记录断言MCU卡死或仍被占用。10:58:53读取W25确认0.1.21、包`60BAEF0B`、健康confirmed；预留`rk_query`回notSupported而非busy，MCU无在途标记；0503为虚拟OFF，心跳正常。没有停止用户进程或清理在途记录。

10:59:12开始复用现有网络OTA安装，不重新构建、不直接覆盖B/App。154032字节按3000字节块完整上传，提交READY后由Boot安装，收到0903且通过30秒健康确认；11:00:31–11:00:35完成验收。

- 安装版本：`0.1.22`，packageId：`5FFDA7D5`。
- 包SHA256：`7FEC689A86BD0BF94EBA22CA7FECE3A0D6FB0902945DF85CCB9B13409220D5E1`。
- 保留0.1.21旧构建产物；本轮没有另读整片备份，正常OTA依靠既有旧槽和日志保护。
- 没有连接ST-Link或串口，不发0103/RK算法、不设置电源或LED；本轮也没有修改固件逻辑。

真实TCP共9组检查、38次响应通过：

| 检查 | 实测结果 |
|---|---|
| 裸TEXT后等待1秒再发心跳 | 同一连接返回0001，不因已经丢弃的文本留下假半帧 |
| TEXT夹两帧心跳粘包 | 两帧及后续心跳全部响应，MCU发送Seq连续 |
| 8种完整坏正文 | 文本、语法错误、数组根、尾随字符、重复键、非法UTF-8、空正文、嵌入二进制心跳均只回0006 invalidArgument；下一心跳正常 |
| 5种坏头后粘连好帧 | Length过小/超限、Version、Flag、ServiceID错误后，正常心跳均被找回 |
| 边界 | 4096字节合法PDU分段接收正常；丢弃4096字节垃圾后仍能处理下一好帧 |
| 保护断连 | 4097字节垃圾、残缺候选头超过500ms分别实际断连；不是以接收超时冒充断连；随后重连心跳正常 |
| 最终只读核对 | W25为0.1.22/confirmed；0602为validMask=0/statistics=null，测试仪与LED查询、心跳正常 |

本轮所有脚本连接已退出，末次检查只有本轮TIME_WAIT/PID=0记录和用户网络助手原有连接；没有结束或接管该用户进程。电源/测量仍为V2模拟配置，本次不代表真实器件或RK算法闭环验收。未同步独立B1、Word/飞书，也未提交/推送。

证据：`build/pc-recovery-query-20260929.json`、`build/pc-recovery-before-20260929.json`、`build/pc-recovery-upgrade-20260929.json`、`build/pc-recovery-after-20260929.json`；后者保存异常输入原始HEX、逐帧响应及检查结果。复核脚本为`build/pc_recovery_board_check_20260929.py`，复跑after需关闭其他客户端、确认没有在途测试，并使用新的报告文件名。
