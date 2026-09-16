/*
 * STM32H743最小跳转入口：B位于0x08000000，占用上限128 KiB。
 *
 * 默认启动A1；实验构建可指定固定的A1/A2入口，不读取W25Q256或执行安装。
 * 当前沿用原入口的粗略栈范围及复位地址范围检查，用于排除空白/明显越界；
 * 尚未逐块校验RAM有效范围、栈对齐、App的Thumb位及镜像CRC，不能替代正式
 * OTA安装器的安全检查。本次整理注释不改变这段既有机器指令的行为。
 *
 * 入口从芯片复位进入，没有初始化外设；App自行设置VTOR及运行环境。
 */
    .syntax unified
    .cpu    cortex-m7
    .thumb

/* 默认保留原A1入口；实验入口只允许两个已经约定的App起始地址。 */
#if defined(OTA_BOOT_APP_BASE)
#if OTA_BOOT_APP_BASE != 0x08020000 && OTA_BOOT_APP_BASE != 0x08100000
#error "OTA startup probe only supports A1 or A2"
#endif
    .equ    APP_BASE,    OTA_BOOT_APP_BASE
    .equ    APP_END,     (OTA_BOOT_APP_BASE + 0x000E0000)
#else
    .equ    APP_BASE,    0x08020000
    .equ    APP_END,     0x08200000
#endif
    .equ    RAM_LO,      0x20000000
    .equ    RAM_HI,      0x24080000

/* B自身只提供初始栈顶和复位入口；尚无完整异常向量表。 */
    .section .isr_vector,"a",%progbits
    .word   _estack
    .word   Reset_Handler

    .section .text
    .thumb_func
    .global Reset_Handler
Reset_Handler:
    ldr     r0, =APP_BASE          /* r0指向待启动App的向量表。 */
    ldr     r1, [r0]               /* 第一个32位字是App初始主栈指针。 */
    ldr     r2, =RAM_LO
    cmp     r1, r2
    blo     .Lfail                 /* 小于粗略RAM下界，拒绝启动。 */
    ldr     r2, =RAM_HI
    cmp     r1, r2
    bhs     .Lfail                 /* 超出粗略RAM上界，拒绝启动。 */

    ldr     r2, [r0, #4]           /* 第二个32位字是App复位入口。 */
    bic     r2, r2, #1             /* 暂去掉Thumb标志位，仅用于范围比较。 */
    ldr     r3, =APP_BASE
    cmp     r2, r3
    blo     .Lfail                 /* 入口地址不能落在App区域之前。 */
    ldr     r3, =APP_END
    cmp     r2, r3
    bhs     .Lfail                 /* 区域末地址不包含在可执行范围内。 */

    msr     msp, r1                /* 切换为App初始栈，不再使用B的栈。 */
    ldr     r2, [r0, #4]           /* 重新读取原始入口，保留其Thumb标志。 */
    bx      r2                     /* 控制权交给App，正常情况下不返回。 */

.Lfail:
    b       .Lfail                 /* 拒绝跳转后停留；恢复策略尚待实现。 */

    .ltorg
    .end
