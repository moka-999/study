/*
 * 编译期分层检查：只给PC协议目录和HAL接口目录，不提供STM32、RTOS或
 * cJSON头搜索路径。若纯帧状态机/通用接口重新泄漏实现依赖，编译会失败。
 * 本文件仅做语法检查，不链接进固件，也不运行硬件。
 */
#include "pc_state_machine.h"
#include "uart_router.h"
#include "board_interfaces.h"
#include "../../App/protocol/b2/b2_state_machine.h"
#include "../../App/tasks/b2/b2_task.h"
#include "../../App/tasks/b2/b2_service.h"
#include "../../App/protocol/common/crc.h"
#include "../../App/protocol/rk/idd_session.h"
#include "b2_spi.h"
#include "network_config.h"

_Static_assert(PC_PROTOCOL_HEADER_SIZE == 18U, "PC header contract changed");
_Static_assert(PC_PROTOCOL_MAX_PDU == 4096U, "PC PDU limit changed");
_Static_assert(UART_RX_BUF_SIZE >= 1040U, "RK receive capacity changed");

void verify_portable_types(pc_sm_t *parser, uart_if_t *uart)
{
    (void)parser;
    (void)uart;
}
