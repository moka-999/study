#ifndef RK_DDR_CAPACITY_H
#define RK_DDR_CAPACITY_H

#include <stdbool.h>
#include <stdint.h>

#define RK_DDR_MIRROR_COUNT 2U

typedef struct {
    const char *memory_type; /* DDRbin 类型行的静态字符串；无类型行或该镜像无通道记录时为 NULL。 */
    uint64_t size_mib;       /* 该镜像已识别通道的容量之和，单位 MiB；两路均无记录时为 0。 */
} rk_ddr_chip_t;

/* 整轮容量为已识别通道之和，单位 MiB，0 表示未知。上轮总量只在成功的 0107 更新。 */
typedef struct {
    rk_ddr_chip_t chips[RK_DDR_MIRROR_COUNT];
    uint32_t current_mib;
    uint32_t last_completed_mib;
    bool round_active;
    bool has_completed;
} rk_ddr_capacity_t;

/* 默认任务逐行提取 DDRbin 日志里出现的 CH0～CH3 容量。length 为字节数，0 表示空闲，负数表示丢块。
 * powered 从上电第一阶段到 ON 均为真。掉电作废同一次启动的缓存。 */
void rk_ddr_capacity_observe(bool powered, const uint8_t *bytes, int length);

/* TCP 任务从 0106 开始记录一次。算法确实沿用已上电的 RK 时才采用其缓存。 */
void rk_ddr_capacity_round_begin(void);
void rk_ddr_capacity_use_running_boot(void);
/* 首项需要冷启动时，丢弃旧启动的识别值，等待实际下次上电。 */
void rk_ddr_capacity_expect_cold_boot(void);
/* 收齐或读到表尾且有通道即交付；expired 为真时也交付部分或空表。组包重试仍读取同一份。 */
bool rk_ddr_capacity_take_chip_info(bool expired, rk_ddr_chip_t chips[RK_DDR_MIRROR_COUNT]);

/* completed 为真表示 0107 已受理；断线或关站传假，只丢弃未完成轮。 */
void rk_ddr_capacity_round_end(bool completed);
rk_ddr_capacity_t rk_ddr_capacity_snapshot(void);

#endif
