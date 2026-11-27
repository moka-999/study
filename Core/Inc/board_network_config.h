#ifndef BOARD_NETWORK_CONFIG_H
#define BOARD_NETWORK_CONFIG_H

/* B1上电时的本地调试IPv4，四段均为十进制，末段1～254。
 * 保存后按Ctrl+Shift+B生成完整ELF/BIN。本地使用/24，网关取同网段的.1。
 * B2成功配置testerId和槽位后，按产线规则切换运行地址。 */
#ifndef B1_TEST_IP
#define B1_TEST_IP "192.168.1.205"
#endif

/* B2 通过 SPI 配置 testerId 后使用的 B1 产线网段。填写四段十进制地址，
 * /24 的末段固定填 0，运行时替换为 10 × testerId + siteId。 */
#ifndef B1_PRODUCTION_SUBNET
#define B1_PRODUCTION_SUBNET "192.168.1.0"
#endif

/* PC 010C 的两个 ID 始终必填且为 uint16。每个开关独立生效：
 * 0 允许任意 uint16；1 检查该编号的物理范围，B2 已分配时还须与本机一致。 */
#ifndef B1_010C_CHECK_TESTER_ID
#define B1_010C_CHECK_TESTER_ID 0
#endif
#ifndef B1_010C_CHECK_SITE_ID
#define B1_010C_CHECK_SITE_ID 0
#endif
#if (B1_010C_CHECK_TESTER_ID != 0 && B1_010C_CHECK_TESTER_ID != 1) || \
    (B1_010C_CHECK_SITE_ID != 0 && B1_010C_CHECK_SITE_ID != 1)
#error "B1 010C ID 校验开关仅允许 0 或 1"
#endif

/* PC监听与串口网卡分流共用端口；EDK不得占用此TCP源端口。 */
#define B1_PC_TCP_PORT 8888U
/* 独立纯文本调试控制台，与正式PC连接各自接手一个客户端。 */

#define B1_DEBUG_TCP_PORT 8889U

/* 1：MCU保留IP/TCP，EDK经以太帧分流联网；0：沿用独占IP交接。 */
#ifndef B1_SERIAL_NET_KEEP_TCP
#define B1_SERIAL_NET_KEEP_TCP 1
#endif
#if B1_SERIAL_NET_KEEP_TCP != 0 && B1_SERIAL_NET_KEEP_TCP != 1
#error "B1_SERIAL_NET_KEEP_TCP must be 0 or 1"
#endif

#endif
