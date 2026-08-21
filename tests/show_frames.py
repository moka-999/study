#!/usr/bin/env python3
"""生成带详细解析的 TLV v2.0 帧示例"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from tests.verify_protocol_v2 import pack_heartbeat, pack_cmd, crc16_modbus, crc16_modbus_continue, unpack_frame

def cmd_name(cmd):
    m={0x0001:'gtest_start',0x0010:'gtest_end',0x0012:'marchx',0x0013:'marchc-',
       0x0014:'marchc+',0x0015:'idd_start',0x0024:'idd_end',
       0x003E:'reboot',0x003F:'frame_error'}
    if cmd in m: return m[cmd]
    if 0x0001<=cmd<=0x0010: return 'gtest_%02d' % (cmd-0x0001)
    if 0x0015<=cmd<=0x0024: return 'idd_%02d' % (cmd-0x0015)
    return '0x%04X' % cmd

def ok(v): return "OK" if v else "FAIL"

def show(name, data, notes=None):
    plen=(data[1]<<8)|data[2]
    seq=int.from_bytes(data[5:13],'big')
    tlv=data[13:13+plen]
    t_type=(tlv[0]<<8)|tlv[1] if len(tlv)>=4 else 0
    t_len=(tlv[2]<<8)|tlv[3] if len(tlv)>=4 else 0
    val=tlv[4:4+t_len] if len(tlv)>=4+t_len else b''
    hok = crc16_modbus(data[:3]) == (data[3]|(data[4]<<8))
    fcrca = crc16_modbus_continue(crc16_modbus(data[1:3]), data[13:13+plen])
    fok = fcrca == (data[13+plen]|(data[14+plen]<<8))

    print('='*80)
    print('  %s' % name)
    print('='*80)

    # Layout
    print()
    print('  [%s] [%s] [%s] [%s] [%s] [%s] [%s]' % (
        data[0:1].hex(), data[1:3].hex(), data[3:5].hex(),
        data[5:13].hex(), tlv.hex(), data[13+plen:15+plen].hex(),
        data[15+plen:16+plen].hex()))
    print('   Sync    Len(BE)  HCRC(LE)  Seq(8B BE)       TLV(%dB)        FCRC(LE)    Footer' % plen)
    print('   0x5A    仅TLV    CRC16(    不参与CRC        Type+Len+Value   CRC16(      0xA5')
    print('           不含Seq   Sync+Len)                  见下方展开       Len+TLV)')
    print()

    # Verification
    print('  【结构验证】')
    print('    Len      = %d  (TLV 字节数, 不含 8B Seq)' % plen)
    print('    Seq      = %d (0x%016X)' % (seq, seq))
    print('    HCRC     = 0x%04X  CRC16(Sync+Len 共3B)  %s' % (data[3]|(data[4]<<8), ok(hok)))
    print('    FCRC     = 0x%04X  CRC16(Len2B + TLV%dB)  %s' % (data[13+plen]|(data[14+plen]<<8), plen, ok(fok)))
    print('    总帧长   = 1+2+2+8+%d+2+1 = %dB  (= 16 + Len)' % (plen, len(data)))
    print()

    # TLV decode
    print('  【TLV 解析】')
    tnames = {0x0000:'心跳 (Heartbeat)', 0x5000:'命令容器 (Command)'}
    print('    Type    = 0x%04X  ->  %s' % (t_type, tnames.get(t_type, '未知/预留')))
    print('    TLV_Len = %d  (Value 字节数)' % t_len)

    if t_type == 0x5000 and t_len >= 2:
        c = (val[0]<<8) | val[1]
        p = val[2:]
        print('    Value   = [CmdID:2B][Params:%dB]' % (t_len-2))
        print('      CmdID  = 0x%04X  ->  %s' % (c, cmd_name(c)))
        if p:
            print('      Params = %s  (%dB)' % (p.hex(), len(p)))
        else:
            print('      Params = (空)')
        # Decode known params
        if c == 0x0001 and len(p) >= 4:
            addr = (p[1]<<24)|(p[2]<<16)|(p[3]<<8)
            mode_s = '启动' if p[0] == 1 else '停止'
            print('        -> mode=%s, 起始地址=0x%08X' % (mode_s, addr))
        elif c == 0x003E and len(p) >= 1:
            rst_s = '长复位6s' if p[0] == 1 else '短复位500ms'
            print('        -> %s' % rst_s)
        elif len(p) == 1 and p[0] in (1, 2, 3, 4):
            st_s = {1:'PASS', 2:'FAIL', 3:'ACK', 4:'BUSY'}[p[0]]
            print('        -> status=%s' % st_s)
    elif t_type == 0x0000:
        print('    Value   = (空, 心跳无需 Value)')

    print()
    if notes:
        print('  【说明】')
        for n in notes:
            print('    -> %s' % n)
        print()

    print('  full hex: %s' % data.hex())
    print('  C array:  {%s}' % (', '.join('0x%02X' % b for b in data)))
    print()


if __name__ == '__main__':
    show('例1: 心跳帧 (MCU -> RK3588, 每1000ms一次, seq=0)',
         pack_heartbeat(0),
         ['Len=4 恒定: Type(2B)+TLV_Len(2B), Value 空',
          'FrameCRC=0xF1DB 对所有心跳帧恒定 (不含 Seq)',
          'RK3588 检测: 连续3个周期 Seq 未递增 -> 判定断开'])

    show('例2: 心跳帧 (seq=99)',
         pack_heartbeat(99),
         ['HeaderCRC/FCRC 不变, MCU 可用常量模板只改 Seq 字节',
          '节省 CRC 重算开销, 只需写 5:13 这 8 个字节'])

    show('例3: 命令帧 -- gtest_start 启动内存测试',
         pack_cmd(0x0001, bytes([0x01,0x00,0x00,0x80]), 100),
         ['PC -> TCP -> MCU: MCU 收到后立即回 ACK(seq=100) 给 PC',
          'MCU -> USART1 -> RK3588: 用 rk_seq_counter 重打包, 加入 pending',
          'RK3588 解析: CmdID=0x0001 启动 gtest, 起始地址 0x80000000',
          '超时 500ms 无响应 -> 重传最多 3 次 (指数退避)'])

    show('例4: 命令帧 -- marchx 测试 (无参数)',
         pack_cmd(0x0012, b'', 101),
         ['TLV_Len=2: Value 仅含 2 字节 CmdID, 无额外参数',
          '总帧长 22B, 理论最短命令帧',
          'March X 算法参数在 RK3588 侧预定义'])

    show('例5: 命令帧 -- 重启 RK3588 (MCU 本地处理)',
         pack_cmd(0x003E, bytes([0x01]), 102),
         ['MCU 不转发 RK: 直接 GPIO 拉低 NPOR -> 延时 6s -> 拉高',
          '执行完成后 MCU 直接回 ACK 给 PC',
          'params[0]=0x01 长复位(6s), 0x00=短复位(500ms)'])

    show('例6: 结果帧 -- 测试 PASS (RK -> MCU -> PC, 响应同 CmdID)',
         pack_cmd(0x0001, bytes([0x01]), 100),
         ['RK -> USART1 -> MCU: 状态机解析 frame.seq=100',
          'MCU _pending_take(seq=100) -> 取回 pc_seq=100',
          'MCU -> TCP -> PC: 用 pc_seq=100 打包, PC 匹配原始请求',
           'params[0]=0x01 -> 测试通过'])

    show('例7: 结果帧 -- 测试 FAIL (响应同 CmdID)',
         pack_cmd(0x0001, bytes([0x02]), 200),
         ['params[0]=0x02 -> FAIL',
           '更详细错误参数格式待三方协议冻结'])

    show('例8: 满载帧 -- 最大参数 1018B (Len=1024)',
         pack_cmd(0x0024, bytes([i & 0xFF for i in range(1018)]), 255),
         ['Len=1024 (最大值), 总帧长=1040B (TLV_FRAME_MAX)',
          'CmdID=0x0024 (idd_end), 传输 1018B IDD 测试结果',
          '超过此限: C 侧 tlv_pack_cmd() 返回 0 拒绝打包'])
