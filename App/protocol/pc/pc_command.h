/* PC-MCU已确认的命令ID；未冻结的命令不要提前加入本文件。 */
#ifndef PC_COMMAND_H
#define PC_COMMAND_H

/* PC作为Client主动探测，MCU作为Server使用相同Seq响应。JSON包体均为{}。 */
#define PC_COMMAND_SERVER_HEARTBEAT   0x0001U
#define PC_COMMAND_CLIENT_HEARTBEAT   0x0002U

/* 第一组测试业务命令；ServiceID和字段值域仍待双方冻结。 */
#define PC_COMMAND_START_TEST          0x0103U
#define PC_COMMAND_TEST_RESULT_REPORT  0x0105U

#endif /* PC_COMMAND_H */
