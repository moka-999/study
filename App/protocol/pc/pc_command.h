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
 * 一轮测试以0106开始、0107结束；轮内串行执行FUNC、PARA或SLT。
 * FUNC的mirrorIndex取RK 0x3001，表示镜像0/1；当前每镜像一颗，physicalAddr为空。
 * SLT由EDK RELEASE触发执行收尾；PC解析服务器报告并在0107汇总三类结果。
 */
#define PC_COMMAND_START_TEST          0x0103U
#define PC_COMMAND_STOP_TEST           0x0104U /* {}；中止当前执行并关RK，保留0106整轮。 */
#define PC_COMMAND_TEST_RESULT_REPORT  0x0105U
#define PC_COMMAND_ROUND_BEGIN         0x0106U /* PC→MCU，{}；只建立整轮上下文。 */
#define PC_COMMAND_ROUND_END           0x0107U /* PC→MCU，失败芯片数组；汇总灯并废弃未交付结果。 */
#define PC_COMMAND_PARA_START          0x0108U /* parametric/idd_*，开始包结构与FUNC一致。 */
#define PC_COMMAND_PARA_RESULT         0x0109U /* MCU→PC，返回RK结果及全程统计；反向请求拒绝。 */
#define PC_COMMAND_SLT_START           0x010AU /* slt/memtest86，按参数准备电源与Boot。 */
#define PC_COMMAND_SLT_RESULT          0x010BU /* 历史SLT结果编号保留，当前返回notSupported。 */
#define PC_COMMAND_SITE_ENABLE         0x010CU /* PC提交本站及各镜像DUT的完整使能配置。 */

/* RK及内存电源业务：非阻塞编排；初版直接关电，不等待RK关机应答。 */
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

/* 历史升级编号保留，禁止复用；当前不提供升级服务，均返回notSupported。 */
#define PC_COMMAND_FIRMWARE_UPGRADE_REQUEST   0x0901U
#define PC_COMMAND_FIRMWARE_UPGRADE_RESP      0x0902U
#define PC_COMMAND_FIRMWARE_UPGRADE_COMPLETE  0x0903U

#endif /* PC_COMMAND_H */
