#include "pc_shmoo.h"
#include "board_rk_config.h"
#include <string.h>

static const char *const s_axes[PC_SHMOO_MAX_AXES] = {"x", "y", "z"};
static const char *const s_rails[DDR_RAIL_COUNT] = {"vdd1", "vdd2", "vddq"};

enum { SHMOO_FIELD_FREQ = DDR_RAIL_COUNT + PC_TIMING_PARAMETER_COUNT };

/* 仅对协议示例中的电压首字母兼容大写；时序字段沿用0103的大小写和映射。 */
static int axis_field(const char *name)
{
    if (!name) return -1;
    if (name[0] == 'v' || name[0] == 'V') {
        for (int rail = 0; rail < DDR_RAIL_COUNT; ++rail) {
            if (strcmp(name + 1, s_rails[rail] + 1) == 0) return rail;
        }
    }
    int timing = pc_timing_field_index(name);
    return timing < 0 ? -1 : DDR_RAIL_COUNT + timing;
}

/* cJSON以double存储JSON数值；限制在可连续精确表示整数的范围内，接受700.0。 */
static int read_axis_value(const cJSON *object, const char *key, uint64_t *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item)) return 0;
    const double number = item->valuedouble;
    if (!(number >= 0.0 && number <= 9007199254740991.0)) return 0;
    *value = (uint64_t)number;
    return (double)*value == number;
}

static int rail_range_valid(const pc_shmoo_axis_t *axis)
{
    const int vdd1 = axis->field == DDR_RAIL_VDD1;
    const unsigned min_mv = vdd1 ? DDR_VDD1_WRITABLE_MIN_MV : DDR_VDDX_WRITABLE_MIN_MV;
    const unsigned max_mv = vdd1 ? DDR_VDD1_WRITABLE_MAX_MV : DDR_VDDX_WRITABLE_MAX_MV;
    const unsigned quantum = vdd1 ? DDR_VDD1_STEP_MV : DDR_VDDX_STEP_MV;
    if (axis->min < min_mv || axis->max > max_mv || (axis->min - min_mv) % quantum) return 0;
    /* max是上限。只有生成第二个坐标时，step才会施加到TPS。 */
    return axis->step > axis->max - axis->min || axis->step % quantum == 0U;
}

static const char *parse_axis(const cJSON *object, pc_shmoo_axis_t *axis)
{
    static const char *const fields[] = {"type", "min", "max", "step"};
    if (!pc_test_has_exact_fields(object, fields, sizeof(fields) / sizeof(fields[0])))
        return "invalidArgument";
    int field = axis_field(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, "type")));
    if (field < 0 || !read_axis_value(object, "min", &axis->min) ||
        !read_axis_value(object, "max", &axis->max) || !read_axis_value(object, "step", &axis->step) ||
        axis->min > axis->max || axis->step == 0U) return "invalidArgument";

    axis->field = (uint8_t)field;
    axis->current = axis->min;
    if (field < DDR_RAIL_COUNT) return rail_range_valid(axis) ? NULL : "invalidArgument";
    if (field == SHMOO_FIELD_FREQ) return B1_RK_FREQ_ENABLE ? NULL : "notSupported";
    if (axis->max > UINT32_MAX) return "invalidArgument";
    return B1_RK_TIMING_ENABLE ? NULL : "notSupported";
}

const char *pc_shmoo_parse(const cJSON *root, pc_shmoo_plan_t *plan)
{
    static const char *const fields[] = {"algorithmType", "algorithmName", "shmoo", "memoryType"};
    const cJSON *memory = cJSON_GetObjectItemCaseSensitive(root, "memoryType");
    size_t field_count = sizeof(fields) / sizeof(fields[0]);
    if (!plan || !pc_test_has_exact_fields(root, fields, memory ? field_count : field_count - 1U))
        return "invalidArgument";
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "algorithmType"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "algorithmName"));
    if (!type || !name) return "invalidArgument";
    const rk_command_descriptor_t *algorithm = rk_command_find_by_name(name);
    if (strcmp(type, "functional") != 0 || !algorithm ||
        algorithm->category != RK_COMMAND_CATEGORY_FUNCTIONAL) return "notSupported";

    const cJSON *axes = cJSON_GetObjectItemCaseSensitive(root, "shmoo");
    pc_shmoo_plan_t parsed = {.algorithm = algorithm, .memory_type = DDR_MEMORY_LPDDR4X};
    if (memory) {
        const char *memory_name = cJSON_GetStringValue(memory);
        if (!memory_name) return "invalidArgument";
        if (strcmp(memory_name, "LPDDR4") == 0) parsed.memory_type = DDR_MEMORY_LPDDR4;
        else if (strcmp(memory_name, "LPDDR4X") != 0) return "invalidArgument";
    }
    parsed.axis_count = cJSON_GetObjectItemCaseSensitive(axes, "z") ? PC_SHMOO_MAX_AXES : PC_SHMOO_MIN_AXES;
    if (!pc_test_has_exact_fields(axes, s_axes, parsed.axis_count)) return "invalidArgument";
    uint32_t used = 0U;
    for (unsigned i = 0U; i < parsed.axis_count; ++i) {
        pc_shmoo_axis_t *axis = &parsed.axes[i];
        const char *error = parse_axis(cJSON_GetObjectItemCaseSensitive(axes, s_axes[i]), axis);
        if (error) return error;
        uint32_t bit = 1UL << axis->field;
        if (used & bit) return "invalidArgument";
        used |= bit;
    }
    *plan = parsed;
    return NULL;
}

void pc_shmoo_apply(const pc_shmoo_plan_t *plan, pc_power_target_t *target,
                    pc_timing_parameters_t *timing)
{
    for (unsigned i = 0U; i < plan->axis_count; ++i) {
        const pc_shmoo_axis_t *axis = &plan->axes[i];
        if (axis->field < DDR_RAIL_COUNT) {
            target->mv[axis->field] = (uint16_t)axis->current;
        } else if (axis->field == SHMOO_FIELD_FREQ) {
            timing->frequency = (int64_t)axis->current;
        } else {
            unsigned index = axis->field - DDR_RAIL_COUNT;
            timing->present |= (uint16_t)(1U << index);
            timing->values[index] = (uint32_t)axis->current;
        }
    }
}

/* 找第一根还能再走一步的轴。用剩余跨度比较，既不计算总点数，也不需要乘法。 */
static unsigned next_axis(const pc_shmoo_plan_t *plan)
{
    for (unsigned i = 0U; i < plan->axis_count; ++i) {
        const pc_shmoo_axis_t *axis = &plan->axes[i];
        if (axis->step <= axis->max - axis->current) return i;
    }
    return plan->axis_count;
}

int pc_shmoo_is_last(const pc_shmoo_plan_t *plan)
{
    return next_axis(plan) == plan->axis_count;
}

void pc_shmoo_advance(pc_shmoo_plan_t *plan)
{
    unsigned next = next_axis(plan);
    if (next == plan->axis_count) return;
    plan->axes[next].current += plan->axes[next].step;
    for (unsigned i = 0U; i < next; ++i) plan->axes[i].current = plan->axes[i].min;
}

cJSON *pc_shmoo_result_create(const pc_shmoo_plan_t *plan, uint8_t failed_mask)
{
    cJSON *body = cJSON_CreateObject();
    if (!body || !cJSON_AddStringToObject(body, "algorithmType", "functional") ||
        !cJSON_AddStringToObject(body, "algorithmName", plan->algorithm->name) ||
        !cJSON_AddStringToObject(body, "shmooprogress", pc_shmoo_is_last(plan) ? "completed" : "running"))
        goto fail;
    cJSON *results = cJSON_AddArrayToObject(body, "testResults");
    if (!results) goto fail;
    for (unsigned die = 0U; die < PC_FUNCTIONAL_DIE_COUNT; ++die) {
        cJSON *entry = cJSON_CreateObject();
        if (!cJSON_AddItemToArray(results, entry)) {
            cJSON_Delete(entry);
            goto fail;
        }
        if (!cJSON_AddNumberToObject(entry, "mirrorIndex", die)) goto fail;
        cJSON *point = cJSON_AddObjectToObject(entry, "shmoopoint");
        if (!point) goto fail;
        for (unsigned i = 0U; i < plan->axis_count; ++i) {
            if (!pc_test_add_uint64(point, s_axes[i], plan->axes[i].current)) goto fail;
        }
        if (!cJSON_AddNumberToObject(entry, "testStatus", (failed_mask & (1U << die)) ? 0 : 1))
            goto fail;
    }
    return body;
fail:
    cJSON_Delete(body);
    return NULL;
}
