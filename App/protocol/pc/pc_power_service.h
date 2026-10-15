/* 正式电源业务编排：复用监测层事务锁和板级驱动，不创建任务、不定义RK线协议。 */
#ifndef PC_POWER_SERVICE_H
#define PC_POWER_SERVICE_H

#include "ddr_power.h"
#include "mcp4725.h"

typedef enum {
    PC_POWER_ON = 0, PC_POWER_OFF, PC_POWER_RESTART,
    PC_POWER_PREPARE_TEST, /* FUNC：电源/Boot完成后还须本轮RK普通心跳。 */
    PC_POWER_PREPARE_SLT   /* SLT：每次冷启动并准备电源/Boot，不检查RK软件状态。 */
} pc_power_action_t;

typedef enum {
    PC_POWER_IDLE = 0, PC_POWER_REQUEST_OFF, PC_POWER_WAIT_OFF,
    PC_POWER_OFF_HOLD, PC_POWER_BOOT_SETTLE, PC_POWER_WAIT_ON,
    PC_POWER_WAIT_RK_READY, PC_POWER_DONE
} pc_power_phase_t;

typedef struct {
    ddr_memory_type_t memory_type;
    uint16_t mv[DDR_RAIL_COUNT]; /* VDD1/VDD2/VDDQ训练目标mV，不是运行期拉偏。 */
    boot_mode_t boot_mode;
} pc_power_target_t;

/* 均为板级显式配置，不从PC JSON接收；配置成功不等于时序已经硬件验收。
 * 两个timeout须为1..INT32_MAX ms；两个等待可为0，最大INT32_MAX ms。
 * DDR安全训练范围由PC确认；MCU仍检查TPS可写范围和原生步进，不夹紧或取整。
 */
typedef struct {
    uint32_t transition_timeout_ms;
    uint32_t ready_timeout_ms;
    uint32_t off_hold_ms;
    uint32_t boot_settle_ms;
} pc_power_config_t;

/* 接口仅由TCP任务调用，必须立即返回，不能在回调中等待串口或反调本模块。
 * begin(token)：上电前清除旧心跳确认；0受理，非0失败，不向断电RK发命令。
 * poll(token)：0等待、1本轮心跳确认、-1令牌失效；B1绑定RK链路的就绪窗口。
 * RK须在初始化完成后发送普通心跳；进度/结果/调试文字不能替代它。
 * 下电不经过此适配器，直接请求板级关电，不代表RK已完成写盘。
 */
typedef struct {
    int (*begin)(uint32_t token);
    int (*poll)(uint32_t token);
} pc_power_rk_adapter_t;

/* 内部调试快照，不增加PC字段；失败时保留出错阶段、时刻及RK令牌。 */
typedef struct {
    pc_power_action_t action;
    pc_power_phase_t phase;
    pc_power_phase_t failed_phase;
    uint32_t phase_since_ms;
    uint32_t rk_token;
    const char *error; /* 静态错误名；NULL表示未失败，并非已经完成。 */
} pc_power_diagnostic_t;

/** 仅启动期或唯一TCP任务在空闲且电源OFF时调用；复制配置/函数指针。
 * 返回0成功、-1拒绝；config=NULL清除配置。另须配置pwr_ctrl的真实轨间时序。
 * 适配器可为NULL；单独0501/0502不依赖RK就绪，0505/完整0103必须绑定。
 */
int pc_power_service_configure(const pc_power_config_t *config,
                               const pc_power_rk_adapter_t *adapter);
/** 唯一TCP任务调用。NULL=已受理（含立即完成），否则静态业务错误名。
 * PREPARE_TEST/PREPARE_SLT必须给target；其它动作target必须NULL，受理时复制target。
 * reboot须为0/1：FUNC为1强制冷启动，为0仍会在配置变化或失效时冷启动；
 * SLT无论reboot值、配置是否相同，每次都冷启动，让RK重新走服务器下载流程。
 * 预检查不访问GPIO/I2C；实际驱动在poll推进。完成结果未消费前仍为busy。
 * SLT的DONE仅表示本地准备完成；其成功缓存不能当成FUNC初始化就绪。
 */
const char *pc_power_service_start(pc_power_action_t action,
                                  const pc_power_target_t *target, uint8_t reboot);
/** 周期推进一次，不睡眠；真实DAC/I2C仍使用已有有界同步驱动。无连接也须调用。 */
void pc_power_service_poll(void);
/** 仅动作匹配且DONE时返回1并消费结果；*error=NULL成功，否则业务失败。 */
int pc_power_service_take(pc_power_action_t action, const char **error);
int pc_power_service_busy(void);
/** 0506/0509设置成功或硬件可能已变时调用；写入前明确拒绝须保留原标记。 */
void pc_power_service_invalidate(void);
/** 只在唯一TCP任务/暂停后的调试器读取，不能用该快照代替实测电压。 */
const pc_power_diagnostic_t *pc_power_service_diagnostic(void);

#endif
