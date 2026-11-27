/* 010E扫描计划及010F逐点上报；只保存三根轴，无逐点结果缓存。 */
#ifndef PC_SHMOO_H
#define PC_SHMOO_H

#include "pc_power_service.h"
#include "pc_test_message.h"
#include "rk_link.h"

enum { PC_SHMOO_MIN_AXES = 2, PC_SHMOO_MAX_AXES = 3 };

typedef struct {
    uint64_t min, max, step, current;
    uint8_t field; /* 三个电源轨之后为PC时序字段表序号，最后一项是Freq。 */
} pc_shmoo_axis_t;

typedef struct {
    const rk_command_descriptor_t *algorithm; /* 请求JSON释放后目录项仍有效。 */
    ddr_memory_type_t memory_type; /* 本次冷启动的颗粒类型；旧计划缺省LPDDR4X。 */
    uint8_t axis_count;
    pc_shmoo_axis_t axes[PC_SHMOO_MAX_AXES]; /* 固定顺序为x、y、z。 */
} pc_shmoo_plan_t;

/* 解析010E完整正文，检查当前固件支持的轴、TPS范围和原生步进；
 * 成功返回NULL并定位第一个点，失败返回0006错误名且plan保持原值。 */
const char *pc_shmoo_parse(const cJSON *root, pc_shmoo_plan_t *plan);

/* 在调用方提供的基准电源和时序上覆盖当前点，不访问板级硬件。 */
void pc_shmoo_apply(const pc_shmoo_plan_t *plan, pc_power_target_t *target,
                    pc_timing_parameters_t *timing);

/* x轴变化最快，最后一个点仍可组包；advance在末点保持原位。 */
int pc_shmoo_is_last(const pc_shmoo_plan_t *plan);
void pc_shmoo_advance(pc_shmoo_plan_t *plan);

/* 创建当前点的010F正文，failed_mask的位0/1标记对应镜像失败。
 * 每颗都带坐标；最后一点写completed，调用方持有并释放成功返回的对象。 */
cJSON *pc_shmoo_result_create(const pc_shmoo_plan_t *plan, uint8_t failed_mask);

#endif
