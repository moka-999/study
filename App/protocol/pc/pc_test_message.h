/* PC电源与测试业务JSON：目标电压/启动请求的纯校验与芯片测试结果组包。 */
#ifndef PC_TEST_MESSAGE_H
#define PC_TEST_MESSAGE_H

#include "cJSON.h"
#include "pc_command.h"
#include "mcp4725.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PC_TEST_JSON_OK = 0,                 /* 业务字段校验通过。 */
    PC_TEST_JSON_ERROR_ARGUMENT,         /* root/view等调用参数非法。 */
    PC_TEST_JSON_ERROR_MISSING_FIELD,    /* 缺少协议要求的必填字段。 */
    PC_TEST_JSON_ERROR_FIELD_TYPE,       /* 字段类型错误或数字不是整数。 */
    PC_TEST_JSON_ERROR_INTEGER_RANGE,    /* 整数超出目标C类型范围。 */
    PC_TEST_JSON_ERROR_FIELDS,           /* 字段重复、未知、缺失或枚举值不支持。 */
    PC_TEST_JSON_ERROR_POWER_RANGE,      /* 三轨目标不能用TPS寄存器准确表示。 */
} pc_test_json_result_t;

#define PC_TIMING_PARAMETER_COUNT 15U
/* PC负责参数单位、器件适用性及安全范围；MCU只检查十进制整数能否无损编码。
 * present按下方命令映射的序号置位；空串/缺省不置位，frequency缺省为-1。
 */
typedef struct {
    int64_t frequency;
    uint32_t values[PC_TIMING_PARAMETER_COUNT];
    uint16_t present;
} pc_timing_parameters_t;
/* 0..14映射0x25、0x26、0x28..0x34；越界返回0，不使用未实现的0x27。 */
uint16_t pc_timing_command(unsigned index);

/*
 * 字符串指针直接引用cJSON树，调用者必须在使用完view后再cJSON_Delete(root)。
 * algorithmIndex为PC测试计划项编号，不是RK算法编号；不自行递增或重排。
 */
typedef struct {
    const char *algorithm_type;  /* functional、slt或parametric；借用root。 */
    const char *algorithm_name;  /* 借用root内字符串，不能单独释放。 */
    uint32_t algorithm_index;    /* PC计划项编号，有限的0..UINT32_MAX整数。 */
    const char *memory_type;     /* LPDDR4或LPDDR4X；借用power中的字符串。 */
    int16_t power_steps[3];      /* 从字符串mV换算的内部档位，依次每档10/5/5mV；不是PC字段。 */
    uint16_t target_mv[3];       /* 三路目标mV；可编码不等于DDR安全或已经生效。 */
    uint8_t reboot;              /* JSON布尔值，false=0、true=1。 */
    const char *boot_mode;       /* 已支持的Boot模式名称；不是PXE开关。 */
    uint8_t external_ready;      /* 1=power/bootMode均为null，保持RK现有供电和启动状态。 */
    uint32_t timeout_ms;         /* 最终结果等待毫秒数；0=不限制，非零最多86400000。 */
    const cJSON *timing_parameters; /* 成功解析后借用必填对象；空对象表示默认时序，不代表已应用。 */
    pc_timing_parameters_t timing; /* 无借用指针的数值副本，发送时不换单位、不取整。 */
} pc_test_start_view_t;

/** 将已定义的PC启动介质名称转为DAC业务枚举；0成功，-1无效，失败不改输出。 */
int pc_boot_mode_parse(const char *name, boot_mode_t *mode);

/* MCU准备上报给PC的一条芯片测试结果；本结构不拥有字符串，组包时复制。 */
typedef struct {
    const char *site_id;       /* 辅助函数接受空串；业务有效性尚待冻结。 */
    uint32_t mirror_index;     /* 镜像索引；当前每镜像一颗DDR，RK原值不作偏移。 */
    int32_t test_status;       /* PC测试结论：0=FAIL、1=PASS；不能直接填RK的FAIL位。 */
    const char *result_desc;   /* 结果描述，UTF-8字符串；NULL或空串均上报""。 */
} pc_chip_test_result_t;

/**
 * @brief 单独读取唯一且合法的计划项编号，供其他字段错误时关联失败响应。
 * @param[in] root 非NULL JSON对象；仅检查algorithmIndex本身。
 * @param[out] index 非NULL；成功写入u32编号，失败保持原值。
 * @return PC_TEST_JSON_OK成功；缺失、重复、非有限数或非u32整数返回错误。
 */
pc_test_json_result_t pc_test_start_read_index(const cJSON *root, uint32_t *index);

/**
 * @brief 0103.power与0509共用的四字段校验；不访问硬件、不修改电源状态。
 * @param[in] root 仅含memoryType/vdd1/vdd2/vddq；电压是非空十进制数字串，单位mV。
 * @param[out] memory_type 借用root中的LPDDR4或LPDDR4X字符串；root释放后失效。
 * @param[out] steps 三项内部有符号档位，分别为(目标mV-该类型标称mV)/(10/5/5mV)。
 * @param[out] target_mv 三项绝对目标，依次为VDD1/VDD2/VDDQ，单位mV。
 * @return PC_TEST_JSON_OK成功；字段、格式、越界或不能精确编码时返回错误。
 * @note 输出均须非NULL，失败全部保持原值。拒绝旧Steps键、数值型电压、符号、
 *       空白、小数、指数和单位，不做取整；"0"不是保持现状，而是越界电压。
 *       完整PC正文先经pc_json_parse_object拒绝内嵌NUL；可编码不代表DDR安全。
 */
pc_test_json_result_t pc_power_parse(const cJSON *root, const char **memory_type,
                                      int16_t steps[3], uint16_t target_mv[3]);

/**
 * @brief 校验START_TEST七个必填字段及可选timeoutMs，不操作硬件。
 * @param[in] root 非NULL JSON对象；网络正文须先经pc_json_parse_object拒绝内嵌NUL。
 * @param[out] view 非NULL输出视图，失败不修改。
 * @return PC_TEST_JSON_OK成功；其他值区分参数、缺字段、类型及整数范围错误。
 * @note 拒绝重复、缺失、额外字段，数字必须有限且可无损转换，reboot必须为布尔。
 *       power和bootMode同时为null且reboot=false时，输出external_ready=1，
 *       memory_type/boot_mode为NULL且目标电压为0；0不表示要求输出0V。
 *       timeoutMs省略或为0表示不限制结果等待，非零整数范围为1..86400000毫秒。
 *       电压对象严格要求memoryType和字符串mV的vdd1/vdd2/vddq，复用pc_power_parse。
 *       TPS可编码不等于DDR安全范围已经批准。
 *       algorithmName只校验字符串类型，具体算法支持情况由命令表决定。
 *       TimingParameters必须为对象，默认时序传{}；也可只含已定义字段中的几项，值须为
 *       字符串，空字符串仍保留且不下发该项。Freq为int64，其余15项为uint32十进制整数。
 *       只检查格式和编码位宽，不判断DDR适用范围；旧tRC/ODT/tRAS/tRFC/BurstLength拒绝。
 *       CASReadLatency与CASWriteLatency分别表示读、写延迟，可单独提供；不接受CASLatency。
 */
pc_test_json_result_t pc_test_start_parse(const cJSON *root,
                                          pc_test_start_view_t *view);

/** 010A专用解析：严格匹配slt/memtest86，index为null，power/bootMode非null。
 * 复用三轨字符串mV、Boot与reboot校验；不接受timeoutMs，不检查RK软件状态。
 * TimingParameters必须为对象，默认时序传{}；省略或显式填null均拒绝。
 * view仍借用root，algorithm_index置0但不得在线上回填0（SLT必须回null）。
 */
pc_test_json_result_t pc_slt_start_parse(const cJSON *root, pc_test_start_view_t *view);

/** 完整校验0107的errorchip数组后返回镜像失败位图；失败保持*failed_mask不变。
 * mirror_count由当前板级灯映射提供，范围1..32；physicalAddr须为字符串。
 * 同一镜像可列多颗芯片，重复项幂等；空数组输出0，未知镜像整包拒绝。
 * 不另建链表/复制芯片清单，遍历上限由4096字节完整PDU限制。
 */
pc_test_json_result_t pc_round_end_parse(const cJSON *root, uint8_t mirror_count,
                                         uint32_t *failed_mask);

/* 010B只借用身份字段；结论由PC提供，不转发给RK或再次上传0105。 */
typedef struct {
    const char *site_id;
    const char *algorithm_name;
} pc_slt_result_view_t;

/** 校验010B完整结构：slt、null index、两个0/1镜像结果、null保留字段。
 * 返回OK后才可按site/name和活动SLT归属匹配；失败不修改view或任何业务状态。
 * 耗时仅验证非负整数/null，不存储或经double重新转发PC给出的耗时。
 */
pc_test_json_result_t pc_slt_result_parse(const cJSON *root, pc_slt_result_view_t *view);

/**
 * @brief 创建算法结果JSON对象，并复制各字符串到对象树。
 * @param[in] algorithm_name 非NULL、NUL结尾的算法名称。
 * @param[in] algorithm_index 算法索引。
 * @param[in] results 含result_count项的数组；仅在项数为0时可为NULL。
 * @param[in] result_count 结果项数；每项site_id须为非NULL的NUL结尾字符串。
 * @return 新cJSON对象，调用方用cJSON_Delete释放；参数或分配失败返回NULL。
 * @note 仅供隔离测试适配器使用，调用方保证编号/状态合法；不检查最终序列化PDU容量。
 *       每项均输出字符串resultDesc；result_desc为NULL时使用空串，否则须为
 *       NUL结尾的UTF-8字符串，并在调用期间保持有效。
 */
cJSON *pc_test_result_create(const char *algorithm_name,
                             uint32_t algorithm_index,
                             const pc_chip_test_result_t *results,
                             size_t result_count);

/* 一颗DDR的FUNC结论；数组位置0/1对应mirrorIndex，不含共用板卡信息与测量值。 */
#define PC_FUNCTIONAL_DIE_COUNT 2U
typedef struct {
    uint64_t error_address;  /* 失败时原样输出16位十六进制地址，包括0；通过时输出null。 */
    const char *error_type;  /* 借用非NULL字符串；RK未提供类型时为空，不能根据地址猜类型。 */
    uint8_t failed;          /* 0=PASS、1=FAIL；组包转换为PC testStatus的1/0。 */
} pc_functional_die_result_t;

/**
 * @brief 为FUNC/PARA创建共享结果正文，两条RK结果按mirrorIndex=0、1排列。
 * @param[in] algorithm_type 仅允许functional或parametric，不决定PC帧头命令号。
 * @param[in] algorithm_name 非NULL、NUL结尾的算法名称，复制到结果中。
 * @param[in] algorithm_index PC计划项编号，原样回带。
 * @param[in] site_id 非NULL板卡标志字符串，内容由业务层加载。
 * @param[in] elapsed_ms RK上报的本次算法整体耗时，单位ms；0也是有效值，不用MCU等待时间替代。
 * @param[in] dies 非NULL且恰有两项；failed只能为0/1，字符串组包时复制。
 * @return 新cJSON对象，由调用方cJSON_Delete；参数或分配失败返回NULL。
 * @note siteId、testTimeMs及measurementData均在顶层；测量初始为null，由调用方追加共用统计。
 *       physicalAddr固定为空。PASS的地址为null且错误类型为空；FAIL地址0仍保留为十六进制字符串。
 *       testTimeMs按uint64十进制JSON整数无损输出，不经过double换算。
 */
/* parametric成功结果固定两颗testStatus=1、resultDesc=null；尚未接入电流判限。 */
cJSON *pc_algorithm_result_create(const char *algorithm_type, const char *algorithm_name,
    uint32_t algorithm_index, const char *site_id, uint64_t elapsed_ms,
    const pc_functional_die_result_t dies[PC_FUNCTIONAL_DIE_COUNT]);

/* 固定functional的兼容入口，其余输入、所有权及失败返回值与上面相同。 */
cJSON *pc_functional_result_create(const char *algorithm_name,
                                    uint32_t algorithm_index,
                                    const char *site_id,
                                    uint64_t elapsed_ms,
                                    const pc_functional_die_result_t dies[PC_FUNCTIONAL_DIE_COUNT]);

/* 仅借用统计快照，不依赖采样任务或器件；组包不会推进窗口或增加样本。
 * statistics用于0602，measurementData位于0105/0109顶层，表示共轨而非单颗DDR电流。
 * 全部无样本时measurementData为null；部分无样本的单项为null。
 * 返回1成功、0参数/分配失败，失败时调用方删除整个body。
 */
struct idd_stats;
int pc_test_add_statistics(cJSON *body, const struct idd_stats *stats);
int pc_test_add_measurement(cJSON *body, const struct idd_stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* PC_TEST_MESSAGE_H */
