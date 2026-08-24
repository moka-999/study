/*
 * ===================================================================
 * 板级 GPIO 映射实现 — 阿波罗V2 验证 ↔ B1 正式
 * ===================================================================
 *
 * 阿波罗V2扩展排针恰好引出了B1电源控制所需的PC6、PC7、PG11，
 * 因此两种板型使用完全相同的逻辑引脚。仍保留GPIO_USE_B1分支，
 * 用于阻止B1误启“跳过TPS配置”的波形测试镜像。
 * ===================================================================
 */
#include "bsp_gpio.h"

/* ── 引脚描述 ── */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
} gpio_pin_t;

#if GPIO_USE_B1
/* ── B1 正式引脚表 (原理图 20260811 版) ── */
static const gpio_pin_t _pins[GPIO_FUNC_COUNT] = {
    [GPIO_FUNC_PWR_4V0_EN] = { GPIOC, GPIO_PIN_6  },  /* MP8759 EN */
    [GPIO_FUNC_PWR_3V3_EN] = { GPIOC, GPIO_PIN_7  },  /* RT8070 EN */
    [GPIO_FUNC_PWR_DDR_EN] = { GPIOG, GPIO_PIN_11 },  /* TPS62868×3 EN */
};
#else
/* ── 阿波罗V2验证引脚表（底板P2-31/P2-32/P3-31） ── */
static const gpio_pin_t _pins[GPIO_FUNC_COUNT] = {
    [GPIO_FUNC_PWR_4V0_EN] = { GPIOC, GPIO_PIN_6  },
    [GPIO_FUNC_PWR_3V3_EN] = { GPIOC, GPIO_PIN_7  },
    [GPIO_FUNC_PWR_DDR_EN] = { GPIOG, GPIO_PIN_11 },
};
#endif

void bsp_gpio_init(void)
{
    GPIO_InitTypeDef g;

    for (int i = 0; i < GPIO_FUNC_COUNT; i++) {
        /*
         * 阿波罗V2的PC6在CubeMX历史配置中先被TIM8初始化；这里位于
         * MX_TIM8_Init()之后，按电源控制职责重新配置为GPIO输出。
         * 先把输出数据寄存器预置为低，再切换引脚模式，可避免原复用
         * 功能退出到推挽输出的瞬间出现不可控高电平毛刺。
         */
        g.Pin   = _pins[i].pin;
        g.Mode  = GPIO_MODE_OUTPUT_PP;
        g.Pull  = GPIO_NOPULL;
        g.Speed = GPIO_SPEED_FREQ_LOW;   /* 电源开关，无需高速 */
        HAL_GPIO_WritePin(_pins[i].port, _pins[i].pin, GPIO_PIN_RESET);
        HAL_GPIO_Init(_pins[i].port, &g);
    }
}

void bsp_gpio_set(gpio_func_t func, uint8_t level)
{
    if (func >= GPIO_FUNC_COUNT) return;
    HAL_GPIO_WritePin(_pins[func].port, _pins[func].pin,
                      level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

uint8_t bsp_gpio_get(gpio_func_t func)
{
    if (func >= GPIO_FUNC_COUNT) return 0;
    return HAL_GPIO_ReadPin(_pins[func].port, _pins[func].pin)
           == GPIO_PIN_SET ? 1 : 0;
}
