/* PC测试仪信息的W25Q256持久化入口。 */
#ifndef PC_TESTER_INFO_H
#define PC_TESTER_INFO_H

#include "flash_layout.h"

/*
 * 每次调用都从W25Q256读取FRU，不缓存信息。首次使用或记录损坏时
 * 写入当前默认信息的A/B双副本；旧160/192字节v1保留已有身份和网络配置，
 * 只补空白字段后迁移。有效但未知schema/size返回错误，不擦写旧记录。
 * 最新有效v2仅将旧默认名称LPDDR4/X_200B一次性保存为JMCT-4000后回读，
 * 其余字段及定制名称不改；之后查询只读。若最新记录仍是旧格式，
 * 则先迁移该旧记录，不能因另一槽存在较旧v2而忽略最新身份。
 */
int pc_tester_info_load(fru_info_t *fru);

#endif /* PC_TESTER_INFO_H */
