#!/usr/bin/env python3
"""TLV v2.0 收发序列 — 带完整 hex"""
import sys; sys.path.insert(0,'d:/pro/test')
from tests.verify_protocol_v2 import *

mcu_seq=0; rk_seq=2000; pc_seq=0x1000

def hr(title):
    print('\n  ── %s ──' % title)

def pkt(d, label, data, note=''):
    plen=(data[1]<<8)|data[2]; seq=int.from_bytes(data[5:13],'big')
    tlv=data[13:13+plen]
    t_type=(tlv[0]<<8)|tlv[1] if len(tlv)>=4 else 0
    t_len=(tlv[2]<<8)|tlv[3] if len(tlv)>=4 else 0
    val=tlv[4:4+t_len] if len(tlv)>=4+t_len else b''
    c=(val[0]<<8)|val[1] if t_type==0x5000 and t_len>=2 else 0
    # decode
    detail=''
    if t_type==0x0000: detail='Type=HB, TLV_Len=0'
    elif t_type==0x5000:
        detail='CmdID=0x%04X' % c
        if c==0x0001 and len(val)>=6: detail+=' [gtest_start] addr=0x%08X' % ((val[3]<<24)|(val[4]<<16)|(val[5]<<8))
        elif c==0x0012: detail+=' [marchx]'
        elif c==0x003E: detail+=' [reboot]'
        elif c==0x003F: detail+=' [frame_error]'
        is_ack = (len(val)>2 and val[2]==0x03 and c != 0x003F)
        if is_ack: detail+=' ACK=0x03'
    if note: detail+=(' | ' if detail else '') + note
    print('  %-7s %-25s | %s  seq=%-5d Len=%-4d' % (d, label, data.hex(), seq, plen))
    if detail: print('  %7s  %s' % ('', detail))

print('='*95)
print('  TLV v2.0 收发序列 | ACK=0x03 | 双seq: MCU-RK / PC-MCU')
print('='*95)

# ═══ Phase 1 ═══
hr('Phase 1: 心跳握手')
pkt('MCU→RK','[心跳]', pack_heartbeat(mcu_seq), 'MCU启动,首个心跳'); mcu_seq+=1
pkt('RK→MCU','[心跳]', pack_heartbeat(rk_seq), 'RK存活应答'); rk_seq+=1
pkt('MCU→RK','[心跳]', pack_heartbeat(mcu_seq)); mcu_seq+=1
pkt('RK→MCU','[心跳]', pack_heartbeat(rk_seq)); rk_seq+=1

# ═══ Phase 2 ═══
hr('Phase 2: PC发起 gtest_start (LPDDR4内存测试)')
pkt('PC→MCU','[命令] gtest_start', pack_cmd(0x0001, b'\x01\x00\x00\x80', pc_seq))
pkt('MCU→PC','[ACK]  gtest_start', pack_cmd(0x0001, b'\x03', pc_seq), 'echo pc_seq=0x%04X'%pc_seq)
gtest_rk_seq=mcu_seq
pkt('MCU→RK','[转发] gtest_start', pack_cmd(0x0001, b'\x01\x00\x00\x80', gtest_rk_seq), 'rk_seq=%d, 加入pending'%gtest_rk_seq); mcu_seq+=1

# ═══ Phase 3 ═══
hr('Phase 3: RK测试完成, 返回结果')
pkt('RK→MCU','[结果] gtest_result', pack_cmd(0x0001, b'\x01', gtest_rk_seq), '响应同CmdID, echo rk_seq')
pkt('MCU→PC','[上报] gtest_result', pack_cmd(0x0001, b'\x01', pc_seq), 'MCU换回 pc_seq=0x%04X'%pc_seq)
pc_seq+=1

# ═══ Phase 4 ═══
hr('Phase 4: marchx 无参命令 (最短命令帧 22B)')
pkt('PC→MCU','[命令] marchx', pack_cmd(0x0012, b'', pc_seq))
pkt('MCU→PC','[ACK]  marchx', pack_cmd(0x0012, b'\x03', pc_seq))
march_rk_seq=mcu_seq
pkt('MCU→RK','[转发] marchx', pack_cmd(0x0012, b'', march_rk_seq)); mcu_seq+=1
pkt('RK→MCU','[结果] marchx_result', pack_cmd(0x0012, b'\x01', march_rk_seq))
pkt('MCU→PC','[上报] marchx_result', pack_cmd(0x0012, b'\x01', pc_seq))
pc_seq+=1

# ═══ Phase 5 ═══
hr('Phase 5: 超时重传 (500ms 无响应)')
pkt('PC→MCU','[命令] gtest_03', pack_cmd(0x0003, b'\x01\x10\x00\x00', pc_seq))
pkt('MCU→PC','[ACK]  gtest_03', pack_cmd(0x0003, b'\x03', pc_seq))
retry_rk_seq=mcu_seq
pkt('MCU→RK','[转发] gtest_03', pack_cmd(0x0003, b'\x01\x10\x00\x00', retry_rk_seq)); mcu_seq+=1
print('  %-7s %-25s | %s' % ('  ...', '500ms 超时, RK无响应...', '(重传)'))
pkt('MCU→RK','[重传] gtest_03', pack_cmd(0x0003, b'\x01\x10\x00\x00', retry_rk_seq), '同一rk_seq, 重试1/3')
pkt('RK→MCU','[结果] gtest_03_result', pack_cmd(0x0003, b'\x01', retry_rk_seq))
pkt('MCU→PC','[上报] gtest_03_result', pack_cmd(0x0003, b'\x01', pc_seq))
pc_seq+=1

# ═══ Phase 6 ═══
hr('Phase 6: 重启RK3588 (MCU本地执行, 不转发)')
pkt('PC→MCU','[命令] reboot', pack_cmd(0x003E, b'\x01', pc_seq))
print('  %-7s %-25s | %s' % ('  ●', 'GPIO PG12 LOW→6s→HIGH', '本地复位, 不转发RK3588'))
pkt('MCU→PC','[ACK]  reboot', pack_cmd(0x003E, b'\x03', pc_seq))
pc_seq+=1

# ═══ Phase 7 ═══
hr('Phase 7: 未知CmdID → frame_error')
pkt('PC→MCU','[命令] 0x00FF', pack_cmd(0x00FF, b'\x42', pc_seq), 'CmdID未注册')
pkt('MCU→PC','[错误] frame_error', pack_cmd(0x003F, b'\xFF\x00\x42', pc_seq))

print()
print('='*95)
print('  完成 | MCU-RK seq=%d | PC-MCU seq范围 0x1000~0x%04X | ACK status=0x03' % (mcu_seq, pc_seq))
print('='*95)
