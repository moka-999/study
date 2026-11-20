#ifndef BOARD_NETWORK_CONFIG_H
#define BOARD_NETWORK_CONFIG_H

/* B1上电时的本地调试IPv4，四段均为十进制，末段1～254。
 * 保存后按Ctrl+Shift+B生成完整ELF/BIN。本地使用/24，网关取同网段的.1。
 * B2成功配置testerId和槽位后，按产线规则切换运行地址。 */
#ifndef B1_TEST_IP
#define B1_TEST_IP "192.168.1.205"
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
