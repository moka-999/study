# 已退役的RK测试脚本

这里的`.py.txt`是旧协议脚本的可恢复文本归档，不是现行可运行测试。
它们依赖旧CRC覆盖范围、命令大端、单TLV回包或500ms重传，不能用于2026-09-23后的协议。

现行入口：

- `../verify_rk_protocol_c.py`：QEMU运行目标V2/B1的实际C编解码和事务代码，不连板。
- `../rk_gateway_network_test.py`：真实V2 TCP＋COM串口，PC/RK由脚本模拟；仅限默认关闭的临时联调构建。必须先备份、结束后恢复正式镜像。

正式PC帧和业务测试继续使用`verify_pc_protocol.py`与`pc_business_network_test.py`。
