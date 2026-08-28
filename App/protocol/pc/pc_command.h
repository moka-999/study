/* PC-MCU已确认的命令ID；未冻结的命令不要提前加入本文件。 */
#ifndef PC_COMMAND_H
#define PC_COMMAND_H

/*
 * 心跳方向固定：PC(Client)发送0x0002，MCU(Server)回复0x0001。
 * 两端JSON包体都必须是空对象{}；MCU响应沿用请求ServiceID并回显相同PC Seq。
 * PC若反向发送0x0001，属于命令方向错误，MCU会关闭当前TCP连接。
 */
#define PC_COMMAND_SERVER_HEARTBEAT   0x0001U
#define PC_COMMAND_CLIENT_HEARTBEAT   0x0002U

/*
 * 测试仪信息查询：PC发送0x0101和空JSON对象{}，MCU使用0x0102返回信息。
 * 这是普通请求/响应，不在TCP连接建立时主动推送；响应沿用请求ServiceID，
 * 并原样回显PC请求Seq。
 */
#define PC_COMMAND_QUERY_TESTER_INFO  0x0101U
#define PC_COMMAND_TESTER_INFO_RESP   0x0102U

/*
 * 第一组测试业务命令：0x0103由PC下发算法，0x0105由MCU上报测试结果。
 * 当前只冻结JSON外形；ServiceID、site/mirror/status值域和请求响应关系仍待评审。
 */
#define PC_COMMAND_START_TEST          0x0103U
#define PC_COMMAND_TEST_RESULT_REPORT  0x0105U

#endif /* PC_COMMAND_H */
