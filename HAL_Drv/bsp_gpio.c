/*
 * ===================================================================
 * 板级 GPIO 映射实现 — 双引脚表 (阿波罗V2 测试 ↔ B2 正式)
 * ===================================================================
 *
 * 引脚表由 GPIO_USE_B2 宏切换, 两张表语义一一对应,
 * B2 板到货后只需把宏翻成 1 重新编译, 调用方零改动。
 * ===================================================================
 */
#include "bsp_gpio.h"

/* ── 引脚描述 ── */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
} gpio_pin_t;

#if GPIO_USE_B2
/* ── B2 正式引脚表 (原理图 20260811 版) ── */
static const gpio_pin_t _pins[GPIO_FUNC_COUNT] = {
    [GPIO_FUNC_PWR_4V0_EN] = { GPIOC, GPIO_PIN_6  },  /* MP8759 EN */
    [GPIO_FUNC_PWR_3V3_EN] = { GPIOC, GPIO_PIN_7  },  /* RT8070 EN */
    [GPIO_FUNC_PWR_DDR_EN] = { GPIOG, GPIO_PIN_11 },  /* TPS62868×3 EN */
    [GPIO_FUNC_HEAT_0]     = { GPIOH, GPIO_PIN_8  },
    [GPIO_FUNC_HEAT_1]     = { GPIOH, GPIO_PIN_9  },
    [GPIO_FUNC_HEAT_2]     = { GPIOH, GPIO_PIN_10 },
};
#else
/* ── V2 测试引脚表 (.ioc 已配或空闲) ── */
static const gpio_pin_t _pins[GPIO_FUNC_COUNT] = {
    [GPIO_FUNC_PWR_4V0_EN] = { GPIOF, GPIO_PIN_2  },
    [GPIO_FUNC_PWR_3V3_EN] = { GPIOC, GPIO_PIN_7  },
    [GPIO_FUNC_PWR_DDR_EN] = { GPIOG, GPIO_PIN_1  },
    [GPIO_FUNC_HEAT_0]     = { GPIOH, GPIO_PIN_9  },
    [GPIO_FUNC_HEAT_1]     = { GPIOH, GPIO_PIN_10 },
    [GPIO_FUNC_HEAT_2]     = { GPIOH, GPIO_PIN_11 },
};
#endif

void bsp_gpio_init(void)
{
    GPIO_InitTypeDef g;

    for (int i = 0; i < GPIO_FUNC_COUNT; i++) {
        /* 引脚表已按语义保证去重, 重复初始化无副作用 */
        g.Pin   = _pins[i].pin;
        g.Mode  = GPIO_MODE_OUTPUT_PP;
        g.Pull  = GPIO_NOPULL;
        g.Speed = GPIO_SPEED_FREQ_LOW;   /* 电源/加热开关, 无需高速 */
        HAL_GPIO_Init(_pins[i].port, &g);
        HAL_GPIO_WritePin(_pins[i].port, _pins[i].pin, GPIO_PIN_RESET);
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
