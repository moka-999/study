# TCP单客户端连接准入修复（2026-11-13）

## 现场与原因

ST-LINK仅attach读取，未复位、未下载。保存于本机`../b1/build/tcp_fault_20261113/`，App读回与当时正常B1 .201镜像逐字节匹配。实际存在同一PC `192.168.1.111:59178` 与 `:59180` 两条到MCU `:8888` 的ESTABLISHED连接；第一条被应用接受且接收队列为空，第二条仍在accept队列且积压5个数据块、接收窗口2044。12块RX_POOL已无空闲，驱动RX_ALLOC_ERROR并记录RBU。

TCP应用任务正常阻塞于第一条连接的20ms接收等待，tcpip_thread等待消息，非全机HardFault。第二条不产生应用connected日志；0601/0602日志本就折叠，只计入status。PC一进程可创建多条连接，具体建连来源仍待PC排查。

## 实现

- B1、V2、手动TLV的唯一TCP任务在accept成功后立即关闭listener、置为-1；当前connfd独立保留，不抢占、不改变协议或空闲规则。
- 使用现有lwIP socket close：netconn_drain递归释放accept队列内连接的recvmbox/pbuf并abort其PCB；已取出的连接不受影响。此处不修改第三方协议栈。
- 新SYN在无监听时由TCP拒绝，不分发JSON；已在队列中的额外连接被清理。短暂窗口内完成握手不代表获得业务服务。待握手PCB在失去listener后也不会交付给应用。
- 连接结束后走原disconnected业务清理，再重新创建监听；SO_REUSEADDR保持原设置，旧TIME_WAIT不阻止重新监听。
- B1连接/关闭日志记录peer IPv4和端口；connected附listener=closed。未接受的新SYN由lwIP拒绝，不新增逐包拒绝日志。
- B1 SLT共存模式：已有PC连接也可完成HANDOFF_RESTORED转换，不等待为单客户端策略关闭的listener。独占交接仍须监听重新就绪。

## 验证边界

源码与离线验证不替代实板验收；本轮不自动烧录、不复位，也不改电源时序。需验证：首连接0601/0602持续正常；第二连接被拒绝且不能拖死网口；首连接退出后可重新连接；SLT RELEASE后同一连接可继续下一算法。PC仍应复用唯一连接，MCU不会把发往被拒连接的数据转移到首连接。

## 已执行离线验证与产物

- `python -B -X utf8 tests/verify_tcp_single_client_c.py --app-root ../b1`（另以`.`、`../b1_tlv`运行）：三份实际TCP任务和帧状态机通过。socket/RTOS为桩，覆盖监听关闭顺序、当前客户端不受影响、短发送、空闲、重开和Seq初始化、收发/分发错误及半帧超时；不把桩测试当成真实lwIP接收池回收实测。
- 正常B1 `verify_pc_rk_service.py --b1-guard` 及 `--round`：业务、测量查询、日志、断线归属和测试整轮回归通过。
- `experiments/mcu_pxe/handoff/tests/test_mcu_overlay.py McuOverlayTests.test_actual_mcu_c_contract`：编译当前正常B1实际交接源码，独占与共存通过。共存完整RELEASE/OFF及占用释放在`connected=1, listener=0`下执行；独占仍须listener恢复。
- 正常B1 .201/.202、手动TLV .201/.202、V2 Debug构建及Boot＋App布局检查通过；B1/TLV工程边界检查通过。
- 固定交付目录：`../b1/build/tcp_single_client_20261113/`，正常版`b1-single-client-ip201/202.elf/.bin`，透传版`b1_tlv-single-client-ip201/202.elf/.bin`；`files.json`记录大小和MD5。BIN为完整Boot＋App，起点0x08000000。

本次未提交/推送，未操作目标板。PC仍须避免重复建连；新固件保留首连接，不自动把第二条的业务迁移过去。
