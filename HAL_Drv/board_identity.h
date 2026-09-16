#ifndef BOARD_IDENTITY_H
#define BOARD_IDENTITY_H

#include <stdint.h>

/**
 * @brief 对96位UID散列生成本地管理的单播MAC。
 * @param[in] uid 非NULL、至少3个uint32_t；每字按低字节到高字节参与散列。
 * @param[out] mac 非NULL、至少6字节；任一参数为NULL则保持输出原值。
 * @note 确定性计算但不保证无碰撞，不表示物理槽位；不写网络接口或Flash。
 */
void board_mac_from_uid(const uint32_t uid[3], uint8_t mac[6]);

/**
 * @brief 获取本V2工程当前固定的默认MAC。
 * @return 静态只读6字节地址，不可释放或修改，整个运行期有效。
 * @note 当前未调用UID散列，也不读取FRU；不能把它当成逐板唯一地址。
 */
const uint8_t *board_mac_address(void);

#endif
