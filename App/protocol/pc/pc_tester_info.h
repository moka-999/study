/* PC测试仪信息的W25Q256持久化入口。 */
#ifndef PC_TESTER_INFO_H
#define PC_TESTER_INFO_H

#include "flash_layout.h"

/*
 * 每次调用都从W25Q256读取FRU，不缓存信息。首次使用或记录损坏时
 * 写入当前默认信息的A/B双副本；旧v1保留序列号、MAC和网络配置后迁移。
 * 一旦存在有效v2，查询路径只读取，绝不用编译期默认值覆盖。
 */
int pc_tester_info_load(fru_info_t *fru);

#endif /* PC_TESTER_INFO_H */
