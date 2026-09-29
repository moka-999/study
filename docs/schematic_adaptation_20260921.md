# B1 新原理图软件适配与协议接入

依据：`MYZR-RK3588-ZUOYI_SCH_20260920-2134.pdf`，SHA-256 `EA886788A1C356EB571AE9243F9E5A201090DC1D87420C2A0E5C449B03FA7D1C`。本次不改原理图、BOM、对外硬件问题表，不烧录或擦写板卡。

## 2026-09-22驱动增量入口

AD/NOR/MCP/PHY与MAC链路修复已同步，B1的SPI4/PE11、20mΩ、1.01分压及UART4/USART2选择保持不变；两板实际C离线回归、B1 Debug和两种串口OTA双槽构建通过。V2开发板验证不代表B1真板验收。具体问题、修改与验证统一见[驱动核查与修改记录](../../test/doc/driver_review_and_changes.md)，本页不重复变更历史。

## 已修改的内容

1. P22的R7268/R7269/R7270为20mΩ。B1生产PAC句柄显式使用20000µΩ，电流及功率换算自动沿用到0602的`currentMa/powerMw`，不在协议层重复补偿；电压换算不受该参数影响。V2测试板保留10mΩ，通用驱动仍可显式选择其它阻值。
2. TPS注释跟随新版U4025 56.2kΩ、U4026 86.6kΩ；料号、三路地址、倍率及20mV步进不变。没有改I2C/UART/PHY/LED/AD引脚，也未复制V2板级初始化。
3. 0103校验完整的`algorithmType/algorithmName/algorithmIndex/power/reboot/bootMode`；缺失、重复、未知字段、错误类型及不可编码电压拒绝。合法index在0006失败应答中原样携带，缺失或非法时不伪造；合法请求仍明确`failed/notReady`，不写DAC、不切电、不发送RK命令。OTA未健康时合法请求为`otaNotReady`。TPS可编码范围不是已批准的DDR安全训练范围。
4. Boot DAC真正开始写入即使旧缓存失效，成功后才恢复。实际写失败时0506返回`executionFailed`，随后0507返回`notConfigured`；参数/忙/状态拒绝未访问器件，保留旧缓存。查询仍是最近成功目标，不是模拟电压或RK实际启动介质回读。

## 保留的执行门禁与待办

- 用户接受共同4V上电后及时完成调压的开发预设，暂不增加RK初始化前等待；这不等于批准具体时序、取消RK-ready或完成实板验收。默认OFF及未配置时序的`notConfigured`保留。
- H04：TPS输出仍为NC选装，不能因为I2C可写就称DDR已由TPS供电；硬件需确认最终选路，禁止并联稳压输出。
- H13：DAC在RK掉电时是否允许输出、首次上电EEPROM恢复值和RK锁存窗口仍待确认。本次不自动写EEPROM、不擅自全时清零。
- 0103未实现安全训练白名单、成功配置比较、自动冷重启、RK就绪和算法交接；0502/0505仍不绕过RK安全下电确认。没有活动测试，自然也没有伪造的0105结果。
- B1的20mΩ实物阻值、真实电压/电流、Boot VOUT及上下电时序需实板验收。

## 验证与复跑

以下均为构建或离线验证，不接板卡，不等同于网络联调或电气验收：

- V2/B1普通Debug构建通过；V2及B1 UART4/UART2的OTA开启、槽限A1 Release构建通过。使用新`build/schematic-2134-ota*`目录，未覆盖旧实板镜像。
- B1引脚、生产默认OFF、驱动/协议/FRU合同检查通过；V2 PC协议24项、RK模型96项、边界19项与纯接口头检查通过，电源/LPDDR/Boot模型检查通过。
- 实际PAC C换算13项通过，20mΩ电流/功率为同原始码10mΩ的一半；实际dispatch/cJSON回归验证该系数进入0602，电压不变。
- 两板实际0103 C解析/分发回归通过，覆盖缺失/重复/未知键、非法数字与枚举、index及独立Seq、OTA拒绝和无硬件副作用。两板实际monitor/MCP C回归通过，覆盖写失败失效、写前拒绝保留及重试恢复。
- V2固定Word局部修改并归档旧文件，内容/结构检查通过；缺少bundled渲染器且WPS后台导出失败，本轮分页视觉未验证。未修改飞书。
- 用户确认服务器未开，要求本轮仅本地提交，待后续明确通知再推送GitHub/Gitea；未同步远端不代表修改丢失。

```powershell
.\tools\dev.ps1 check
.\tools\dev.ps1 build
```

实际C回归复用同级bootloader工程的QEMU启动/链接文件，不把Python模型测试写成硬件通过：

```powershell
$taskCompiler = 'C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc.exe'
python -B -X utf8 ../test/tests/verify_monitor_boot.py --app-root . --compiler $taskCompiler
python -B -X utf8 tests/verify_pc_start_c.py --compiler $taskCompiler
New-Item -ItemType Directory -Path build/pac-rsense-test -Force | Out-Null
& $taskCompiler -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16 -std=c11 -O1 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -IBSP/PAC1934 -IHAL_Drv tests/test_pac1934_rsense.c BSP/PAC1934/pac1934.c ../bootloader/tests/qemu_start.s -nostartfiles --specs=nosys.specs -T../bootloader/tests/qemu_test.ld '-Wl,--gc-sections' -o build/pac-rsense-test/test.elf
& D:/qemu/qemu-system-arm.exe -M mps2-an500 -display none -monitor none -serial none -no-reboot -semihosting-config enable=on,target=native -kernel build/pac-rsense-test/test.elf
```

## 同日增量：AD7124软件缺陷修正

V2/B1共用AD驱动已同步完整快照/错误状态检查、校准IO与超时恢复、FILTER高位参数及中文API合同。B1仅改`BSP/AD7124/ad7124.c/.h`，保留SPI4绑定、1.01分压、20mΩ、默认OFF及其它板级差异。

B1实际AD/采样层C故障注入、Debug、UART4/UART2 OTA槽限Release及完整check通过；没有B1实板验证。复跑：`python -B -X utf8 ../test/tests/verify_ad7124_c.py --app-root . --compiler <原GNU编译器路径>`。范围、手册依据、开发板只读查询及未验收项见[AD7124修正记录](../../test/doc/test_reports/ad7124_driver_20260921.md)。本增量未提交/推送。

## 同日增量：UART恢复及IDD统计基础

B1已同步UART断流/恢复/诊断、RK打包输入边界及三轨统计模块，并接入现有监测任务：下电、尝试调压或OTA维护取消旧窗口。保留UART4/USART2选择、SPI4、20mΩ、默认OFF和现有安全门禁。统计仍使用约2秒采样节奏，未增加PC/RK JSON、测试条件、限值或PASS/FAIL逻辑。

两板实际C路由/帧/统计回归和真实monitor任务循环验证通过；B1 Debug、UART4/UART2 OTA槽限Release及完整check通过。本增量未烧录、未提交/推送；复跑入口和真板待验收边界见[UART与IDD统计记录](../../test/doc/test_reports/uart_idd_foundation_20260921.md)。

## 同日增量：PAC1934底层与最短等待修正

B1已同步CH4关闭位0x10、28位功率对齐、单极性ACT/LAT核对、故障不交付及下一周期重新初始化；增加底层µA值但不改PC currentMa或IDD统计口径。保留B1的20mΩ、软件I2C、AD SPI4/1.01分压及所有电源门禁。等待复用现有TIM1/HAL时基，不新增定时器或中断；任务休眠后核对已过时间，保证正常时基下至少请求的毫秒数，不承诺精确唤醒。

B1实际PAC/等待/组合层ARM C故障测试及AD、UART/IDD、monitor、PC C回归通过；Debug、双UART OTA槽限Release和check通过。按用户要求，IDD业务窗口、RK配合、约2秒采样节奏和判限仍待B1。不烧录、不提交/推送；实物PAC精度与时序未验收。复跑与手册依据见[PAC1934修正记录](../../test/doc/test_reports/pac1934_driver_20260921.md)。
