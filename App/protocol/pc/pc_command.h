/* PC-MCU已确认的命令ID；未冻结的命令不要提前加入本文件。 */
#ifndef PC_COMMAND_H
#define PC_COMMAND_H

/*
 * 心跳方向固定：PC(Client)发送0x0002，MCU(Server)回复0x0001。
 * 两端JSON包体都必须是空对象{}；MCU响应使用本连接自己的发送Seq。
 * PC若反向发送0x0001，MCU返回0x0006/invalidDirection，不因方向错误断连。
 */
#define PC_COMMAND_SERVER_HEARTBEAT   0x0001U
#define PC_COMMAND_CLIENT_HEARTBEAT   0x0002U
#define PC_COMMAND_COMMAND_RESP       0x0006U

/*
 * 测试仪信息查询：PC发送0x0101和空JSON对象{}，MCU使用0x0102返回信息。
 * 这是普通请求/响应，不在TCP连接建立时主动推送；响应ServiceID固定0，
 * 使用MCU本连接自己的发送Seq。
 */
#define PC_COMMAND_QUERY_TESTER_INFO  0x0101U
#define PC_COMMAND_TESTER_INFO_RESP   0x0102U

/*
 * 第一组测试业务命令：0x0103由PC下发算法，0x0105由MCU上报测试结果。
 * 当前只冻结JSON外形；ServiceID、site/mirror/status值域和请求响应关系仍待评审。
 */
#define PC_COMMAND_START_TEST          0x0103U
#define PC_COMMAND_TEST_RESULT_REPORT  0x0105U

/* RK及内存电源业务：上电走状态机；下电/复位受RK安全关机确认门控。 */
#define PC_COMMAND_POWER_ON            0x0501U
#define PC_COMMAND_POWER_OFF           0x0502U
#define PC_COMMAND_QUERY_POWER_STATUS  0x0503U
#define PC_COMMAND_POWER_STATUS_RESP   0x0504U
#define PC_COMMAND_RESET_RK            0x0505U
#define PC_COMMAND_SET_BOOT_MODE       0x0506U
#define PC_COMMAND_QUERY_BOOT_MODE     0x0507U
#define PC_COMMAND_BOOT_MODE_RESP      0x0508U
#define PC_COMMAND_SET_DDR_CONFIG      0x0509U

/* 三轨实际采样查询：响应数组顺序固定为VDD1、VDD2、VDDQ。 */
#define PC_COMMAND_QUERY_POWER_MONITOR 0x0601U
#define PC_COMMAND_POWER_MONITOR_RESP  0x0602U

/* 三组双色结果灯使用业务对象和状态，不暴露物理颜色或GPIO编号。 */
#define PC_COMMAND_SET_RESULT_INDICATOR    0x0701U
#define PC_COMMAND_QUERY_INDICATOR_STATUS  0x0702U
#define PC_COMMAND_INDICATOR_STATUS_RESP   0x0703U

/* OTA请求/动作应答/健康完成查询。文件块放0901的Base64 data，不新增传输端口。 */
#define PC_COMMAND_MCU_OTA_REQUEST         0x0901U
#define PC_COMMAND_MCU_OTA_RESP            0x0902U
#define PC_COMMAND_MCU_OTA_COMPLETE        0x0903U

#endif /* PC_COMMAND_H */
