# H1A：EDK自动PXE、测试与IP归还

## 目标与触发

沿用已验证的完整B1 MCU、SerialNet v2、现有SPL 0x003E及115200协议口。在当前MCU执行`net rk`、按原电源流程启动RK并完成HELLO授权后，EDK自动完成PXE启动；不再手选UEFI PXEv4或执行归还命令。正式PC 010A/010B、跨连接整轮归属和新增runId不在这次修改范围。

`SltAutoBoot.c`独立放置自动编排，在Arm平台`PlatformBootManagerAfterConsole`完成控制台和Shell注册后调用。只接受一个ACTIVE授权控制器，只选同一个网卡完整设备路径下的IPv4 DHCP启动项；不按显示名称猜选，不选其他MAC、IPv6、HTTP或非活动启动项。

## 自动流程

1. 输出`SLT-AUTO-20261111 session=... IP=...`，等待3秒ESC调试窗口。
2. 无ESC则连接授权网卡，刷新启动项并同步执行一次PXEv4。
3. MemTest86按TFTP的`mt86.cfg`自动测试和保存报告。应用退出后控制权回到自动入口；EFI返回值只表示启动/退出状态，不作为DDR PASS/FAIL。
4. 调用现有`ReturnNetwork`拆卸网卡、停止收发并发送RELEASE。NOT_READY最多等待30秒，其他错误提示手动重试；不重跑测试。
5. 成功输出`Network returned to MCU. PC may reconnect to the MCU address.`，保持完整EDK菜单。MCU恢复原IP与8888后，PC可按0101/0102确认。RK仍在EDK，下一轮沿用当前已验证的下电再启动步骤。

未获MCU授权时保留原EDK启动方式。3秒内按ESC会保留RK网络授权并进入菜单，仍可手动PXE和`NetReturn.efi return`。启动项不存在、PXE或应用返回错误时也尝试有序归还；应用卡死且未返回时不会猜测结束或抢回IP。报告缺失属于任务异常，不能由自动归还成功推导测试PASS。

## 配套MemTest86配置

TFTP的`mt86.cfg`启用`AUTOMODE=1`、`AUTOREPORT=1`、`AUTOPROMPTFAIL=0`、`EXITMODE=2`。保留当前测试项0/1/3、单轮、串口显示和报告路径；这是现有验证配置，不是完整产线测试范围。原配置备份保存在本次构建记录中。官方参数依据：https://www.memtest86.com/tech_configuring-memtest.html。

自动模式与EXITMODE组合仍须本版实板验证，尤其确认FAIL时无人工提示、上传失败不伪造PASS、应用返回后能够拆卸网卡。

## 验证记录与边界

11月11日手动H1已有本轮TFTP实证：RK MAC为06:A8:9B:51:0F:1E、IP为192.168.1.201；BOOTAA64.efi 1607208字节/212秒，unifont.bin 2355408字节/306秒。10:44:27收到最终TestResult.xml，测试项0/1/3 PASS、耗时138秒；10:45:43收到33962字节HTML报告。用户随后反馈手动归还及MCU通信恢复成功。以上属于手动版实测，不代表本次自动版已经烧录或验收。

H1A实际C离线合同覆盖无授权、协议版本/状态不符、多个控制器、ESC、授权MAC和IPv4筛选、单次执行、应用退出错误、无PXE项、已连接网卡、归还忙/失败/超时及二次调用不重复执行。现有SNP驱动合同一并回归通过；自动入口源码同步幂等检查通过。ARM QEMU链接仍有工具库wchar_t/enum ABI提示，未将离线模拟当作真实AArch64运行。

11月11日完整AArch64 DEBUG/GCC11.5构建与SPI NOR打包通过。最终BL33为983040字节，版本标记`B1-SERIAL-NET-20261111-H1A`；已解压两个固件卷，核验SerialNet、NetReturn、FvSimpleFileSystem及BdsDxe自动入口标记，NetReturn与独立EFI逐字节一致。原厂双层解包、载荷/介质检查及Windows侧最终IMG散列一致。本次没有连接板卡、烧录或改变波特率。

- [SPI NOR升级整包](../../../build/mcu_pxe/handoff/wsl_20261111_114940/update_spinor_rk_for_edk2_h1.img)：6484554字节，SHA256 `ed00ad2083d935229396aa7c8ebca7bde428d9b801d44ef682753047b649c9c9`。
- [打包校验](../../../build/mcu_pxe/handoff/wsl_20261111_114940/verification.json)。使用RKDevTool“升级固件”，沿用当前RK前级及现有11月11日MCU固件。
- [当前自动配置及原配置备份](../../../build/mcu_pxe/handoff/auto_20261111/)：已更新现用TFTP根目录的`mt86.cfg`；未改DHCP绑定、字体、服务开关或重启服务。
- 自动编排源码：[SltAutoBoot.c](edk2/SltAutoBoot.c)；本机同步位置为`D:/pro/edk2/edk2/ArmPkg/Library/PlatformBootManagerLib/SltAutoBoot.c`。

安装本包后，RK保持OFF、PC断开且不自动重连，按原步骤`net rk`→`start rk`→`log rk`。出现3秒ESC提示后不按键，预期自动下载、测试、上传并归还；确认归还日志后再连接MCU的8888检查0101/0102。保留原手动版升级整包用于对照，不把本次构建通过标成自动流程实板通过。

## 后续下载提速

本轮两个大文件共3962616字节，30秒需要有效132087字节/秒；8N1串口仅载荷的理想下限约1.321 Mbaud，尚未计入以太网、UART封装、轮询及TFTP往返。2～3 Mbaud可以作为后续实验档位，不代表只改波特率就能达到30秒。当前串口显示模式仍请求2.36MB字体；省略字体或跨启动缓存需要单独验证，不在本次自动编排中删除字体。
