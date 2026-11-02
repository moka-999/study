# 采样 JSON 小数输出修正（2026-11-02）

## 现象与原因

用户B1日志显示心跳、信息、电源控制成功，0601查询后工具报JSON解析错误（第71或72列）。未抓取本次板上原始0602，因此不把离线输出认作现场原文。

两板使用newlib-nano，但此前未显式链接浮点printf/scanf。cJSON整数走`%d`，小数走`%g`并用`%lg`回读检查精度；缺少浮点支持会生成空数字。实际C离线测试改用nano后复现`"Idd1Ave":,`及`"currentAvgMa":[,,null]`，与工具报错类别一致。

V2模拟器先产生整数mA再乘1000存µA，瞬时电流不触发小数分支；B1真实µA换算mA会出现小数。V2平均值仍可能出现小数，不能据过去通信成功认为V2不受影响。

## 修改与验证

- B1和V2顶层CMake显式链接`_printf_float`、`_scanf_float`，不改变协议字段或测量值类型。
- `tests/verify_pc_start_c.py`改为生产同款nano库和浮点选项；`test_pc_start.c`增加采样统计序列化再解析，校验0.5005mA和null。
- 修复前测试失败，原输出保存在`build/json_float_fix_20261102/before.log`；修复后两板实际C回归通过，包括PAC转换到0602完整JSON。
- B1 201、202以及V2模拟配置构建、Boot/App地址检查通过；链接map确认浮点函数已进入最终镜像。B1 BIN为379020字节，V2为387780字节。
- 本轮没有连接或烧录开发板。仍需用户重烧后验证连续0601及实际电流读数；离线通过不代表器件精度验收。

## 烧录产物

- B1 `.201`：`D:/pro/b1/build/standalone/Debug/b1_192.168.1.201.elf`
- B1 `.202`：`D:/pro/b1/build/standalone/Debug-ip202/b1_192.168.1.202.elf`
- V2：`D:/pro/test/build/standalone/Debug/test.elf`（电源/测量模拟、MATS第一颗假FAIL配置）
