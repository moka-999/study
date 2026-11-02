/* 测试仪信息查询，不从此路径写入Flash。 */
#ifndef PC_TESTER_INFO_H
#define PC_TESTER_INFO_H
#include "flash_layout.h"
/* fru为非NULL输出；0有效，-1读取/记录错误。BOARD_W25_PRESENT=0时
 * 使用板级宏常量；=1时只读W25，初始化与迁移由Boot完成。 */
int pc_tester_info_load(fru_info_t *fru);
#endif
