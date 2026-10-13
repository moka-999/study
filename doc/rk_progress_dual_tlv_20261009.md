# RK双TLV进度修正与Seq验证（2026-10-09）

## 修正范围

实际串口日志的进度帧为`0x5001＋0x3016`，此前解码器把其中的算法号当作残缺四详情，导致整条进度报告被拒绝。本次只修正报告分类与在途算法匹配，复用现有字段表、结果结构及任务缓存；不增加协议字段或任务。

| 项目 | 当前规则 |
|---|---|
| 进度帧 | 0x5001为2字节小端算法号，0x3016为1字节0..100进度；各一次、顺序不限，完整帧27字节 |
| 校验 | 双CRC、字段宽度/值域、Seq窗口及在途算法号均通过才缓存 |
| 错误报告 | 单独3016、重复字段、混入残缺详情或算法不匹配，不更新当前进度 |
| 生命周期 | 进度100不代表完成，不解除busy，不生成PC JSON新字段 |
| 最终结果 | 四详情加独立FAIL，或五TLV合并帧，继续沿用原有收齐后一次交付逻辑 |

同步V2/B1的`App/protocol/rk/frame.c/.h`和`App/tasks/rk/rk_task.c`；测试集中在V2的`tests/test_rk_protocol.c`，通过`--app-root`编译B1实际源码，不复制V2测试任务到B1。修改的自有注释用中文；历史与验证记录留在本文件。

## Seq不需要改代码

用户补充：RK收到新MCU Seq就保存，由RK自己的定时器触发发送。因此可能落后1～2个Seq，连续两三条上行也可能同Seq。现有窗口已兼容，正式Seq代码保持不变。

窗口下界为本次测试命令Seq，上界为测试期间最近成功进入UART发送队列的MCU Seq。例如下界100、上界105时，103、104及连续三帧104均允许；99或106不在窗口。不是严格回显、不是必须等于最新Seq，也没有固定20个序号的差值限制。实际实现用u64无符号差支持回绕。

相同Seq不代表重复业务：算法进度按每帧校验；最终详情/FAIL相同重复幂等，内容矛盾仍保持等待。最终两部分可同Seq或不同Seq，先后不限。若RK给同算法的旧结果重新标上当前窗口Seq，仍无法区分执行实例，不宣称完整防重放。

## 验证结果

全部为本机离线验证，无真实串口或TCP连接。

| 检查 | 结果与边界 |
|---|---|
| V2/B1实际C＋QEMU | 双TLV黄金帧、换序、0..100、单字段拒绝、重复/残缺字段、算法匹配、越界Seq、最终结果与超时等回归通过 |
| RK定时发送场景 | 落后1～2个Seq、连续三帧同Seq且进度变化、两结果包不同Seq、u64回绕通过；未修改Seq实现 |
| PC实际业务C＋QEMU | V2普通/V2电源模拟/B1三配置通过；PC JSON与结果路由未改 |
| 历史日志连续回放 | 两组字节流分别识别61/5个双TLV进度，各接纳5个对应MATS进度；最终PASS耗时分别5639/5645ms，均单次交付 |
| 构建与工程检查 | V2/B1 Debug构建通过；V2分层19项/纯接口头与B1工程check通过 |

日志回放输入为`build/mats-log-review-20261008/capture.h`与`success_capture.h`，临时回放入口为`build/rk-progress-replay-20261009.c`。无时间戳的连续字节回放不能还原现场字节间延迟，不能据此断言旧现场漏结果的根因已解决。正式黄金帧及事务用例已保存在`tests/test_rk_protocol.c`。

复跑主要入口（在V2根目录）：

```powershell
python -B -X utf8 tests/verify_rk_protocol_c.py
python -B -X utf8 tests/verify_rk_protocol_c.py --app-root ../b1
python -B -X utf8 tests/verify_pc_rk_service.py
python -B -X utf8 tests/verify_pc_rk_service.py --power-simulation
python -B -X utf8 tests/verify_pc_rk_service.py --app-root ../b1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev.ps1 build
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev.ps1 style-test
# B1的build/check入口须在D:/pro/b1运行。
```

## 离线阶段边界

- V2字节间超时100ms、B1 50ms保持各自配置；心跳仍为1秒，算法默认无限等待。
- 保留B1实际电源ON、UART路由及真实采样门禁；没有把V2模拟带入B1。
- 上述离线修改阶段未烧录、查询板上版本、接入ST-Link、打开串口/TCP、下发算法、提交或推送；未生成新的OTA/首烧交付包，也未更新Word/飞书。
- 用户随后授权V2烧录，安装结果见下节。真实RK双TLV进度及0105最终上报仍须现场复验。

## V2网络安装（2026-10-09）

用户确认RK空闲、网络助手已断开后，通过既有正式网络OTA流程升级，未接入ST-Link或串口。

- 预检W25为`0.1.26/confirmed/0D5DCFAB`；与上次安装包及构建配置匹配，确定保留真实RK、V2电源/测量模拟。只读`rk_query`返回notSupported而非busy，仅说明MCU未保留在途测试标记，不是读取RK执行状态。
- 0503原虚拟电源为ON、powerCycleCount=1。使用既有0502接口把内存模拟状态置OFF后复查通过，未切断真实RK电源、操作DAC/TPS或发送算法。
- 用`tools/dev.ps1 ota-build -Version 0.1.27 -Configuration Release -PowerSimulation`构建：App为148460字节、1481处地址修正，A1→A2逐字节一致且反向校验通过；升级包为154500字节。
- 包ID为`B8B364A8`，SHA-256为`ec866948dcbdbd2d9347093177804d880331d44e0c033da558301359ff1722b7`。沿用`tests/pc_ota_network_test.py --upgrade`，协商3000字节分块，提交后由B安装。
- 安装后的0903目标包完成响应、30秒健康确认与心跳通过；再次从W25查询为`0.1.27/confirmed/B8B364A8`。0503为OFF、powerCycleCount=0，0602为validMask=0、statistics=null，符合重启后模拟电源默认OFF；TCP连接已关闭。
- 版本包含双TLV进度修复及算法匹配；Seq窗口、1秒心跳、V2 100ms字节间门限未改。保留正式TCP/RK任务和原电源模拟配置。
- 网络升级不另读整片备份，依靠既有旧运行槽及启动日志保护；未更新Bootloader、FRU、稳定包或B1实板，未提交/推送或同步Word/飞书。未执行真实算法，不能把安装与基本查询通过写成真实RK进度/0105闭环已验收。

安装证据：`build/ota-preflight-20261009.json`、`build/ota-power-preflight-20261009.json`、`build/ota-upgrade-0.1.27-20261009.json`、`build/ota-postflight-0.1.27-20261009.json`；升级包及两槽产物在`build/ota-app-power-sim-0.1.27-release-a1/`及对应A2目录。
