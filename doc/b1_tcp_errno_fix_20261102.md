# B1 TCP空闲断线与串口现场诊断

日期：2026-11-02。只读现场检查获用户授权，不复位、不暂停CPU、不烧录。

## 现场证据

- B1上电后ST-Link参考电压3.25～3.27V，芯片可识别；先前0.76V由用户确认当时未供电。
- 两份RAM快照中HAL/RTOS计数均增长，四项受监督任务持续更新，看门狗未标记故障。
- USART1的命令缓冲分别保留log mcu/log rk，来源状态随之变为MCU/RK；发送环head=tail，从1003推进到1042。证明这些命令到达并被处理、发送环已消耗，不能证明外部转换器完整收到。
- PHY已识别（地址0），netif flags=0x0F，当前有链路；ping两次通过，ARP的02:2A:DF:95:2D:04与B1一致。开机1.5秒的link=0快照不能代表持续掉线。
- TCP诊断最后断线原因为0xBEEF00E6（recv错误），随后等待新连接；一次只读查询脚本尝试连接超时，未发送控制命令。
- RAM原始文件：build/b1_live_20261102/ram_powered.bin、ram_powered2.bin。所有诊断进程均已退出，无探针或TCP连接持续占用。

## 确认的软件缺陷与修复

两板lwIP的arch/cc.h启用LWIP_PROVIDE_ERRNO，socket写全局errno；TCP任务却包含C库errno.h，实际ELF中通过newlib的__errno读取另一份错误码。正常20ms接收等待返回EWOULDBLOCK时，TCP任务可能读到0或无关值并主动断开。这解释了快速连续查询能成功、稍后再次查询或控制时发现连接已断的现象；不据此断言所有物理断链及连接超时都由它引起。

仅将两板App/tasks/pc/tcp_task.c的错误码头文件改为lwip/errno.h，与既有socket实现一致。未更改帧、JSON、上下电行为或接收超时，不靠持续心跳掩盖错误。

新增tests/verify_tcp_errno_link.py检查实际构建对象：TCP任务和sockets均引用errno，禁止TCP引用__errno。旧B1对象检查失败，修复后B1的201/202及V2三个构建全部通过，Boot/App镜像检查通过。该检查不替代空闲保活和反复连接的实板验收。

## 交付与待验收

- B1 .201：D:/pro/b1/build/standalone/Debug/b1_192.168.1.201.elf，SHA256=1E8D020A6BE25597658D879464EB2D750D85340A3F4DECADD20EC1B468EE73F3。
- B1 .202：D:/pro/b1/build/standalone/Debug-ip202/b1_192.168.1.202.elf。
- V2：D:/pro/test/build/standalone/Debug/test.elf，保留电源模拟及MATS假FAIL配置。
- 本轮仅构建，未安装、提交或推送。安装后先连上并空闲10秒，再查询0503；重复查询和断开重连，不需先操作真实电源。随后验收上下电和串口回显。
- RJ45变压器是否位于插座内部尚未核实，不能仅凭外观或ping成功确认电气合规。
