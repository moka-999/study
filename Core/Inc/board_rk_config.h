#ifndef BOARD_RK_CONFIG_H
#define BOARD_RK_CONFIG_H

/* PC测试请求仍完整校验；0跳过15项普通时序设置，1按本轮参数发送。
 * 只控制FUNC/PARA/SLT的自动设置，不改变算法命令或电源时序。 */
#ifndef B1_RK_TIMING_ENABLE
#define B1_RK_TIMING_ENABLE 0
#endif

/* 0：RK查询0x35时回复int64的-1，使用RK默认频率；1：回复本轮Freq。
 * 两种配置均只在RK查询后回复；缺省Freq仍为-1，不主动发送频率。 */
#ifndef B1_RK_FREQ_ENABLE
#define B1_RK_FREQ_ENABLE 1
#endif

#if (B1_RK_TIMING_ENABLE != 0 && B1_RK_TIMING_ENABLE != 1) || \
    (B1_RK_FREQ_ENABLE != 0 && B1_RK_FREQ_ENABLE != 1)
#error "RK timing and frequency switches must be 0 or 1"
#endif

#endif
