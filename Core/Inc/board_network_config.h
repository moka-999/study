#ifndef BOARD_NETWORK_CONFIG_H
#define BOARD_NETWORK_CONFIG_H

/* B1上电时的本地调试IPv4，四段均为十进制，末段1～254。
 * 保存后按Ctrl+Shift+B生成完整ELF/BIN。本地使用/24，网关取同网段的.1。
 * B2成功配置testerId和槽位后，按产线规则切换运行地址。 */
#ifndef B1_TEST_IP
#define B1_TEST_IP "192.168.1.201"
#endif

#endif
