/* PC测试仪信息的W25Q256持久化入口。 */
#ifndef PC_TESTER_INFO_H
#define PC_TESTER_INFO_H

#include "flash_layout.h"

/**
 * @brief 从W25Q256加载测试仪FRU，必要时初始化、迁移v1或更新旧默认名称。
 * @param[out] fru 非NULL输出；仅返回0时可用于响应，失败可能已部分更新。
 * @return 0已重新读到有效持久化记录；-1参数、访问、迁移或保存失败。
 * @note 有效v2仅将旧默认名称LPDDR4/X_200B一次性保存为JMCT-4000，
 *       其余字段及定制名称不改；之后查询只读。空白/损坏记录生成默认值，
 *       已知160/192字节v1迁移保留原身份与网络字段，只补空字段，再保存
 *       A/B副本并回读；因此查询可能擦写。有效未知格式直接失败，不覆盖。
 *       同步、不可重入；当前由单TCP任务访问，新增访问者须共用Flash事务锁。
 */
int pc_tester_info_load(fru_info_t *fru);

#endif /* PC_TESTER_INFO_H */
