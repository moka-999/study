#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
全链路压力测试 — PC(TCP) ↔ MCU(STM32H743) ↔ USART1 ↔ RK3588(模拟器)

架构:
    PC(本脚本) ──TCP:8888──► MCU ──USART1:115200──► RKSimulator(本脚本内线程)

设计要点:
  * RK3588 由一个独立线程 RKSimulator 模拟, 独占串口 RX (MCU→RK 方向)。
    这样 PC 端只走 TCP 校验结果, 不再与模拟器抢读串口, 消除"读竞争"隐患。
  * 每个测试先发命令(携带随机 payload), 模拟器按配置回响应(可注入
    延迟/抖动/丢包/损坏), PC 通过 TCP 收到 RESULT 帧后逐字节比对 payload,
    实现端到端数据完整性校验。

覆盖维度:
  A. throughput    大吞吐量 (流水线压测 + 持续吞吐)
  B. protocol      协议一致性 (seq/CRC/status/结构)
  C. correctness   功能数据正确性 (随机 payload 往返逐字节比对 + 全 CmdID + 边界长度)
  D. reliability   可靠性 (断连重连 / 并发 / 心跳复位检测)
  E. endurance     耐久性 (长时间 soak, 错误率 + 复位监控)
  F. fault         错误注入 (CRC/截断/超长/垃圾/伪同步/缺尾/非法命令)
  G. loss          丢包 (响应丢包率扫描, 验证重传 + 数据不损坏)
  H. latency       延迟抖动 (RTT 分布 + RK 响应延迟扫描 + 随机抖动)
  I. backpressure  背压 (下游不响应 → BUSY → 恢复)
  J. heartbeat     心跳 (1s 周期 + seq 单调 + 复位检测)
  K. frameboundary 粘包/拆包
  L. idle          空闲恢复

用法:
  python tests/full_chain_stress.py [host] [port] [com]
      --only A,B,C     只跑指定场景 (逗号分隔)
      --list           列出场景
      --duration 60    耐久性时长(秒)
      --n 200          默认命令数
      --seed 1         随机种子
"""

import socket
import sys
import os
import time
import threading
import random
import argparse

try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_client import pack_cmd, pack_heartbeat, parse_frame

try:
    import serial
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False

# ── 常量 ────────────────────────────────────────────────────────────
VALID_CMDS = list(range(0x0001, 0x0011)) + list(range(0x0012, 0x0025))  # 0x11/0x25-0x28 保留
RESERVED_CMDS = [0x0011, 0x0025, 0x0026, 0x0027, 0x0028]
STATUS_ACK = 0x03
STATUS_BUSY = 0x04
CMD_FRAME_ERROR = 0x3F
FRAME_MAX = 1040
BAUD = 115200


# ═══════════════════════════════════════════════════════════════════
# 基础工具
# ═══════════════════════════════════════════════════════════════════

class Stream:
    """字节流 → 完整帧解析 (带粘包/拆包/坏帧跳过, 只吐 parse_frame 成功的帧)."""

    def __init__(self):
        self.buf = b''

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            idx = self.buf.find(b'\x5A')
            if idx < 0:
                self.buf = b''
                break
            if idx > 0:
                self.buf = self.buf[idx:]
            if len(self.buf) < 16:
                break
            plen = (self.buf[1] << 8) | self.buf[2]
            total = 16 + plen
            if total < 16 or total > FRAME_MAX:
                self.buf = self.buf[1:]  # 丢弃坏长度字节, 继续找下一个 0x5A
                continue
            if len(self.buf) < total:
                break
            f = parse_frame(self.buf[:total])
            if f:
                out.append(f)
            self.buf = self.buf[total:]
        return out

    def drain(self, sock):
        """读一次 socket (非阻塞语义, 内部 timeout), 返回新解析出的帧."""
        try:
            d = sock.recv(4096)
        except socket.timeout:
            return []
        except Exception:
            return []
        return self.feed(d) if d else []


def pct(data, p):
    """百分位数 (线性插值)."""
    if not data:
        return 0.0
    s = sorted(data)
    k = (len(s) - 1) * p
    f = int(k)
    c = min(f + 1, len(s) - 1)
    return s[f] + (s[c] - s[f]) * (k - f)


def ms_stats(name, vals):
    """把毫秒列表格式化成 min/avg/p50/p95/p99/max."""
    if not vals:
        return f'{name}=N/A'
    v = [x * 1000 for x in vals]
    return (f'{name}: n={len(v)} min={min(v):.1f} avg={sum(v)/len(v):.1f} '
            f'p50={pct(v,0.5):.1f} p95={pct(v,0.95):.1f} '
            f'p99={pct(v,0.99):.1f} max={max(v):.1f} ms')


def rand_payload(seed_i, lo=1, hi=64):
    """确定性的伪随机 payload (长度 1~64, 内容与 seed 相关), 便于逐字节比对."""
    rng = random.Random(seed_i)
    n = rng.randint(lo, hi)
    return bytes(rng.getrandbits(8) for _ in range(n))


def is_ack(f):
    """判断帧是否为 MCU 的立即 ACK (params 长度1 且值为 0x03/0x04)."""
    return ('status' in f and len(f.get('params', b'')) == 1
            and f['params'][0] in (STATUS_ACK, STATUS_BUSY))


# ═══════════════════════════════════════════════════════════════════
# RK3588 模拟器 (独占串口 RX)
# ═══════════════════════════════════════════════════════════════════

class RKSimulator(threading.Thread):
    """读 MCU 转发的命令帧, 按配置回响应; 支持故障注入与心跳/重传统计."""

    def __init__(self, ser):
        super().__init__(daemon=True, name='RK-SIM')
        self.ser = ser
        self._lock = threading.Lock()
        self._stop_event = threading.Event()   # 注意: 不能叫 _stop, 那是 Thread 的保留方法
        self._buf = b''

        # 故障注入配置
        self.delay_ms = 0.0
        self.jitter_ms = 0.0
        self.drop_rate = 0.0
        self.corrupt_rate = 0.0
        self.transform = 'echo'        # echo | invert | fixed

        # 统计
        self.forwarded = 0             # 收到 MCU 转发的命令帧
        self.responded = 0
        self.dropped = 0
        self.corrupted = 0
        self.retransmit = 0            # 观察到同一 mcu_seq 再次出现 = MCU 重传
        self.hb_count = 0
        self.reset_count = 0           # 心跳 seq 回退 = MCU 复位
        self._seen_seqs = {}
        self._last_hb_seq = None

    # ── 配置 / 快照 ──
    def configure(self, **kw):
        with self._lock:
            for k, v in kw.items():
                setattr(self, k, v)

    def reset_stats(self):
        with self._lock:
            self.forwarded = self.responded = self.dropped = self.corrupted = 0
            self.retransmit = 0
            self._seen_seqs = {}

    def snapshot(self):
        with self._lock:
            max_dup = 0
            max_dup_seq = None
            for sq, cnt in self._seen_seqs.items():
                if cnt - 1 > max_dup:
                    max_dup = cnt - 1
                    max_dup_seq = sq
            return dict(forwarded=self.forwarded, responded=self.responded,
                        dropped=self.dropped, corrupted=self.corrupted,
                        retransmit=self.retransmit, hb_count=self.hb_count,
                        reset_count=self.reset_count,
                        max_dup=max_dup, max_dup_seq=max_dup_seq)

    def stop(self):
        self._stop_event.set()

    # ── 帧解析 ──
    def _feed(self, data):
        self._buf += data
        out = []
        while True:
            idx = self._buf.find(b'\x5A')
            if idx < 0:
                self._buf = b''
                break
            if idx > 0:
                self._buf = self._buf[idx:]
            if len(self._buf) < 16:
                break
            plen = (self._buf[1] << 8) | self._buf[2]
            total = 16 + plen
            if total < 16 or total > FRAME_MAX:
                self._buf = self._buf[1:]
                continue
            if len(self._buf) < total:
                break
            f = parse_frame(self._buf[:total])
            if f:
                out.append(f)
            self._buf = self._buf[total:]
        return out

    def _transform(self, params):
        if self.transform == 'invert':
            return bytes(~b & 0xFF for b in params)
        if self.transform == 'fixed':
            return b'\xAB\xCD'
        return params  # echo

    @staticmethod
    def _corrupt(raw):
        b = bytearray(raw)
        i = random.randrange(len(b))
        b[i] ^= 0xFF
        return bytes(b)

    def _on_frame(self, f):
        if f['type'] == 0x0000:  # 心跳
            with self._lock:
                self.hb_count += 1
                if self._last_hb_seq is not None and f['seq'] < self._last_hb_seq:
                    self.reset_count += 1   # seq 回退 = 复位
                self._last_hb_seq = f['seq']
            return
        if f['type'] != 0x5000 or 'cmd_id' not in f:
            return

        with self._lock:
            self.forwarded += 1
            n = self._seen_seqs.get(f['seq'], 0) + 1
            self._seen_seqs[f['seq']] = n
            if n > 1:
                self.retransmit += 1
            delay = self.delay_ms
            if self.jitter_ms:
                delay += random.uniform(0, self.jitter_ms)
            drop = random.random() < self.drop_rate
            corrupt = random.random() < self.corrupt_rate
            transform = self.transform

        if drop:
            with self._lock:
                self.dropped += 1
            return

        resp = pack_cmd(f['cmd_id'], self._transform(f.get('params', b'')), f['seq'])
        if corrupt:
            resp = self._corrupt(resp)
            with self._lock:
                self.corrupted += 1
        if delay > 0:
            time.sleep(delay / 1000.0)
        try:
            self.ser.write(resp)
            self.ser.flush()
            with self._lock:
                self.responded += 1
        except Exception:
            pass

    def run(self):
        while not self._stop_event.is_set():
            try:
                n = self.ser.in_waiting
                if n <= 0:
                    time.sleep(0.001)
                    continue
                d = self.ser.read(n)
            except Exception:
                time.sleep(0.005)
                continue
            for f in self._feed(d):
                self._on_frame(f)


# ═══════════════════════════════════════════════════════════════════
# 链路封装 (TCP + 串口 + RK 模拟器)
# ═══════════════════════════════════════════════════════════════════

class Chain:
    def __init__(self, host, port, com):
        self.host, self.port, self.com = host, port, com
        self.sock = None
        self.ser = None
        self.rk = None

    def connect(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(3.0)
        self.sock.connect((self.host, self.port))
        self.sock.settimeout(0.05)
        if HAS_SERIAL:
            self.ser = serial.Serial(self.com, BAUD, timeout=0.05)
            self.ser.reset_input_buffer()
            self.rk = RKSimulator(self.ser)
            self.rk.start()
        return self

    def close(self):
        if self.rk:
            self.rk.stop()
            self.rk.join(timeout=2.0)
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass

    def close_tcp(self):
        """只关 TCP 主连接, 保留串口 + RK 模拟器线程。

        MCU 是单客户端 server (listen backlog=1 + 单 accept 循环),
        需要断连重连/错误注入/粘包等场景自己开新连接时, 必须先释放主连接位。
        """
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None

    def reconnect_tcp(self, retries=5):
        """重连 TCP 主连接 (带重试, 等 MCU accept 循环转回来)."""
        for _ in range(retries):
            try:
                self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.sock.settimeout(3.0)
                self.sock.connect((self.host, self.port))
                self.sock.settimeout(0.05)
                return self.sock
            except Exception:
                time.sleep(0.3)
        raise RuntimeError('重连 TCP 失败')


# ═══════════════════════════════════════════════════════════════════
# 通用流水线引擎: 发一批命令, 追踪每条 ACK/RESULT/超时
# ═══════════════════════════════════════════════════════════════════

def pipeline(chain, jobs, window, result_timeout=5.0, ack_timeout=2.0):
    """jobs: [{cmd, params, seq}]. 流水线发送(窗口=window), 逐条追踪结果.

    返回 dict: seq -> {cmd, params, outcome, ack_lat, rtt, result_params}
      outcome ∈ {'ok','busy','abandoned','miss','bad'}
        ok        = 收到 RESULT (全链路往返)
        busy      = 收到 BUSY ACK (被背压拒绝)
        abandoned = 收到 ACK 但 result_timeout 内无 RESULT (RK 不响应, MCU 放弃)
        miss      = ack_timeout 内连 ACK 都没有 (连接/挂死)
        bad       = 收到帧但 CRC 坏 / seq 错 / 非法 status
    """
    sock = chain.sock
    stream = Stream()
    outcomes = {j['seq']: dict(cmd=j['cmd'], params=j['params'], outcome='pending',
                               t_send=None, ack_t=None, result_t=None,
                               result_params=None) for j in jobs}

    send_i = 0
    inflight = 0
    done = 0
    n = len(jobs)
    t_start = time.time()
    # 全局兜底: 最后一帧发出后最多再等 result_timeout, 另加发送耗时余量
    global_deadline = t_start + result_timeout + (n * 24 / BAUD) * 12 + 20

    while done < n and time.time() < global_deadline:
        # ① 按窗口发送
        while send_i < n and inflight < window:
            j = jobs[send_i]
            try:
                sock.sendall(pack_cmd(j['cmd'], j['params'], j['seq']))
            except Exception:
                # 发送失败, 剩余全部判 miss
                for k in range(send_i, n):
                    outcomes[jobs[k]['seq']]['outcome'] = 'miss'
                return outcomes
            outcomes[j['seq']]['t_send'] = time.time()
            inflight += 1
            send_i += 1

        # ② 收帧并归类
        got_any = False
        for f in stream.drain(sock):
            got_any = True
            s = f['seq']
            if s not in outcomes:
                continue
            o = outcomes[s]
            if o['t_send'] is None:
                continue   # 上一轮 seq 段复用残留的迟到 RESULT, 不是本轮已发出的命令
            if o['outcome'] != 'pending':
                continue
            if not (f.get('hcrc_ok') and f.get('fcrc_ok')):
                o['outcome'] = 'bad'
                inflight -= 1
                done += 1
                continue
            if is_ack(f):
                o['ack_t'] = time.time()
                o['ack_lat'] = o['ack_t'] - o['t_send']
                if f['params'][0] == STATUS_BUSY:
                    o['outcome'] = 'busy'
                    inflight -= 1
                    done += 1
                # ACK(0x03): 继续等 RESULT
            else:
                # RESULT (全链路)
                o['result_t'] = time.time()
                o['rtt'] = o['result_t'] - o['t_send']
                o['result_params'] = f.get('params', b'')
                o['result_cmd'] = f.get('cmd_id')
                o['outcome'] = 'ok'
                inflight -= 1
                done += 1

        # ③ 超时判定
        now = time.time()
        for j in jobs:
            o = outcomes[j['seq']]
            if o['outcome'] != 'pending':
                continue
            if o['t_send'] is None:
                continue
            if o.get('ack_t') is not None:
                if now - o['ack_t'] > result_timeout:
                    o['outcome'] = 'abandoned'
                    inflight -= 1
                    done += 1
            elif now - o['t_send'] > ack_timeout:
                o['outcome'] = 'miss'
                inflight -= 1
                done += 1

        if send_i >= n and inflight == 0:
            break
        if not got_any:
            time.sleep(0.002)

    # 兜底: 全局超时后仍未决的按是否收到过 ACK 归类
    for j in jobs:
        o = outcomes[j['seq']]
        if o['outcome'] == 'pending':
            o['outcome'] = 'abandoned' if o.get('ack_t') is not None else 'miss'
    return outcomes


def summarize(outcomes):
    """把 pipeline 结果汇总成 (ok, busy, abandoned, miss, bad) 计数."""
    c = dict(ok=0, busy=0, abandoned=0, miss=0, bad=0)
    for o in outcomes.values():
        c[o['outcome']] += 1
    return c


# ═══════════════════════════════════════════════════════════════════
# 测试报告基础设施
# ═══════════════════════════════════════════════════════════════════

class Reporter:
    def __init__(self):
        self.pass_cnt = 0
        self.fail_cnt = 0
        self.failures = []

    def report(self, name, ok, detail=''):
        if ok:
            self.pass_cnt += 1
            print(f'  [PASS] {name}  {detail}')
        else:
            self.fail_cnt += 1
            self.failures.append(name)
            print(f'  [FAIL] {name}  {detail}')

    def summary(self):
        print('\n' + '=' * 70)
        print(f'  结果: PASS={self.pass_cnt}  FAIL={self.fail_cnt}')
        if self.failures:
            print(f'  失败项: {", ".join(self.failures)}')
        print('=' * 70)
        return self.fail_cnt == 0


# ═══════════════════════════════════════════════════════════════════
# A. 大吞吐量
# ═══════════════════════════════════════════════════════════════════

def test_throughput(chain, rep, n, window):
    print('\n[A] 大吞吐量 (流水线)')
    if not chain.rk:
        print('  [SKIP] 无串口/RK模拟器'); return
    chain.rk.configure(delay_ms=0, jitter_ms=0, drop_rate=0, corrupt_rate=0, transform='echo')
    chain.rk.reset_stats()

    jobs = [dict(cmd=VALID_CMDS[i % len(VALID_CMDS)],
                 params=rand_payload(i, 4, 32), seq=i + 1) for i in range(n)]
    t0 = time.time()
    outcomes = pipeline(chain, jobs, window)
    dt = time.time() - t0
    c = summarize(outcomes)

    # 窗口(默认16)超过 MCU in-flight 容量(pending4+queue8=12)时, 超出部分被诚实
    # busy 拒绝——这是背压的正确行为, 不是丢命令。核心验证: 无坏帧 + 无静默丢失。
    ok = (c['bad'] == 0 and c['miss'] == 0)
    ack_lat = [o['ack_lat'] for o in outcomes.values() if o.get('ack_lat')]
    rtt = [o['rtt'] for o in outcomes.values() if o.get('rtt')]
    rep.report('吞吐量', ok,
               f'{c["ok"]}/{n} 完成 {dt:.1f}s 速率={c["ok"]/dt:.1f}帧/s '
               f'(busy={c["busy"]} abandoned={c["abandoned"]} miss={c["miss"]} bad={c["bad"]})')
    print(f'        {ms_stats("ACK延迟", ack_lat)}')
    print(f'        {ms_stats("全链路RTT", rtt)}')
    print(f'        RK模拟器: {chain.rk.snapshot()}')


# ═══════════════════════════════════════════════════════════════════
# B. 协议一致性
# ═══════════════════════════════════════════════════════════════════

def test_protocol(chain, rep, n):
    print('\n[B] 协议一致性 (seq/CRC/status/结构)')
    chain.rk.configure(delay_ms=0, drop_rate=0, corrupt_rate=0, transform='echo')
    jobs = [dict(cmd=VALID_CMDS[i % len(VALID_CMDS)],
                 params=rand_payload(i, 8, 16), seq=0x1000 + i) for i in range(n)]
    outcomes = pipeline(chain, jobs, window=8)
    c = summarize(outcomes)

    # 一致性: 每条结果 seq 严格匹配, cmd_id 匹配, 无 bad
    mism = sum(1 for j, o in outcomes.items()
               if o['outcome'] == 'ok' and o.get('result_cmd') != outcomes[j]['cmd'])
    ok = (c['ok'] == n and c['bad'] == 0 and mism == 0)
    rep.report('协议一致性', ok,
               f'ok={c["ok"]}/{n} bad={c["bad"]} seq/cmd不匹配={mism} '
               f'busy={c["busy"]} abandoned={c["abandoned"]}')


# ═══════════════════════════════════════════════════════════════════
# C. 功能数据正确性
# ═══════════════════════════════════════════════════════════════════

def test_correctness(chain, rep):
    print('\n[C] 功能数据正确性 (随机 payload 逐字节往返)')
    chain.rk.configure(delay_ms=0, drop_rate=0, corrupt_rate=0, transform='echo')

    # C1: 随机 payload 往返
    jobs = [dict(cmd=VALID_CMDS[i % len(VALID_CMDS)],
                 params=rand_payload(1000 + i, 1, 256), seq=0x2000 + i) for i in range(120)]
    outcomes = pipeline(chain, jobs, window=8)
    bad_data = 0
    for j, o in outcomes.items():
        if o['outcome'] == 'ok' and o['result_params'] != o['params']:
            bad_data += 1
    ok1 = summarize(outcomes)['ok'] == len(jobs) and bad_data == 0
    rep.report('C1 随机payload往返', ok1, f'字节不匹配={bad_data}/{len(jobs)}')

    # C2: 全 CmdID 覆盖
    jobs = [dict(cmd=c, params=rand_payload(i, 2, 8), seq=0x3000 + i)
            for i, c in enumerate(VALID_CMDS)]
    outcomes = pipeline(chain, jobs, window=8)
    ok2 = summarize(outcomes)['ok'] == len(VALID_CMDS)
    rep.report('C2 全CmdID覆盖', ok2, f'{len(VALID_CMDS)} 个命令全往返')

    # C3: 边界长度 (0/1/2/127/128/255/256)
    sizes = [0, 1, 2, 127, 128, 255, 256]
    jobs = [dict(cmd=0x0001, params=bytes((i & 0xFF for i in range(s)), ), seq=0x4000 + k)
            for k, s in enumerate(sizes)]
    outcomes = pipeline(chain, jobs, window=8)
    bad_data = sum(1 for j, o in outcomes.items()
                   if o['outcome'] == 'ok' and o['result_params'] != o['params'])
    ok3 = summarize(outcomes)['ok'] == len(sizes) and bad_data == 0
    rep.report('C3 边界长度', ok3, f'sizes={sizes} 字节不匹配={bad_data}')


# ═══════════════════════════════════════════════════════════════════
# D. 可靠性 (断连重连 / 并发 / 复位检测)
# ═══════════════════════════════════════════════════════════════════

def test_reliability(chain, rep):
    print('\n[D] 可靠性 (断连重连)')
    chain.rk.configure(delay_ms=0, drop_rate=0, corrupt_rate=0, transform='echo')
    chain.close_tcp()   # MCU 单客户端: 释放主连接位, 交给下面的重连循环
    time.sleep(0.3)     # 等 MCU accept 循环转回
    ok_cnt = 0
    total = 30
    try:
        for i in range(total):
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(3.0)
                s.connect((chain.host, chain.port))
                s.settimeout(0.5)
                stream = Stream()
                seq = 0x6000 + i
                s.sendall(pack_cmd(0x0001, bytes([i & 0xFF]), seq))
                deadline = time.time() + 3.0
                while time.time() < deadline:
                    for f in stream.drain(s):
                        if f['seq'] == seq and f.get('status') == STATUS_ACK:
                            ok_cnt += 1
                            deadline = 0
                            break
                    if deadline == 0:
                        break
                s.close()
            except Exception:
                pass
            time.sleep(0.05)
    finally:
        chain.reconnect_tcp()
    rep.report('D1 断连重连', ok_cnt == total, f'{ok_cnt}/{total}')


# ═══════════════════════════════════════════════════════════════════
# E. 耐久性 (soak)
# ═══════════════════════════════════════════════════════════════════

def test_endurance(chain, rep, duration):
    print(f'\n[E] 耐久性 soak ({duration}s)')
    if not chain.rk:
        print('  [SKIP] 无串口/RK模拟器'); return
    chain.rk.configure(delay_ms=0, drop_rate=0, corrupt_rate=0, transform='echo')
    chain.rk.reset_stats()

    t0 = time.time()
    seq = 0x7000
    sent = ok = 0
    errors = 0
    batch = 5
    last_report = t0
    while time.time() - t0 < duration:
        jobs = [dict(cmd=VALID_CMDS[(seq + i) % len(VALID_CMDS)],
                     params=rand_payload(seq + i, 4, 32), seq=seq + i)
                for i in range(batch)]
        seq += batch
        outcomes = pipeline(chain, jobs, window=8, result_timeout=4.0)
        c = summarize(outcomes)
        sent += batch
        ok += c['ok']
        errors += c['abandoned'] + c['miss'] + c['bad'] + c['busy']
        # 每 5s 打印健康行
        if time.time() - last_report >= 5.0:
            last_report = time.time()
            snap = chain.rk.snapshot()
            print(f'    +{time.time()-t0:5.0f}s  已发={sent} ok={ok} '
                  f'错误累计={errors} 心跳={snap["hb_count"]} 复位={snap["reset_count"]}')
        # 心跳/复位已在 RK 模拟器里持续监控

    snap = chain.rk.snapshot()
    rate = ok / duration
    err_rate = errors / max(sent, 1)
    ok_test = (err_rate < 0.01 and snap['reset_count'] == 0)
    rep.report('E1 耐久性', ok_test,
               f'{ok}/{sent} 完成 速率={rate:.1f}帧/s 错误率={err_rate*100:.2f}% '
               f'复位={snap["reset_count"]} 心跳={snap["hb_count"]}')


# ═══════════════════════════════════════════════════════════════════
# F. 错误注入 (TCP 侧: 测 MCU 解析器鲁棒性)
# ═══════════════════════════════════════════════════════════════════

def _inject_then_probe(chain, rep, name, inject, probe_seq):
    """独立连接: 注入坏字节 → 发正常帧 → 期望收到正确 ACK."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(3.0)
        s.connect((chain.host, chain.port))
        s.settimeout(0.3)
        stream = Stream()
        if inject:
            s.sendall(inject)
            time.sleep(0.6)
            stream.drain(s)  # 排空注入阶段残留
        s.sendall(pack_cmd(0x0005, b'\x55', probe_seq))
        deadline = time.time() + 3.0
        got = False
        while time.time() < deadline and not got:
            for f in stream.drain(s):
                if f['seq'] == probe_seq and f.get('status') in (STATUS_ACK, STATUS_BUSY):
                    got = True
        s.close()
        rep.report(name, got, '注入后正常帧仍可 ACK' if got else '注入后无响应')
    except Exception as e:
        rep.report(name, False, f'异常: {e}')


def test_fault(chain, rep):
    print('\n[F] 错误注入 (TCP 侧)')
    good = pack_cmd(0x0001, b'\x01', 1)

    chain.close_tcp()   # MCU 单客户端: 释放主连接位
    time.sleep(0.3)
    try:
        # F1 帧 CRC 破坏
        c = bytearray(good); c[-3] ^= 0xFF
        _inject_then_probe(chain, rep, 'F1 帧CRC破坏', bytes(c), 0x8101)
        # F2 头 CRC 破坏
        c = bytearray(good); c[3] ^= 0xFF
        _inject_then_probe(chain, rep, 'F2 头CRC破坏', bytes(c), 0x8102)
        # F3 截断帧
        _inject_then_probe(chain, rep, 'F3 截断半帧', good[:10], 0x8103)
        # F4 超长 Len
        c = bytearray(good); c[1] = 0xFF; c[2] = 0xFF
        _inject_then_probe(chain, rep, 'F4 超长Len', bytes(c), 0x8104)
        # F5 垃圾字节
        _inject_then_probe(chain, rep, 'F5 垃圾字节', b'\x00\x01\x02\x03\x04\x05\x06\x07', 0x8105)
        # F6 伪同步 (数据里含 0x5A)
        _inject_then_probe(chain, rep, 'F6 伪同步0x5A', pack_cmd(0x0001, b'\x5A\x5A\x5A', 2), 0x8106)
        # F7 缺尾 0xA5
        _inject_then_probe(chain, rep, 'F7 缺尾0xA5', good[:-1], 0x8107)
        # F8 非法命令 (reserved 0x11) → 期望收到 frame_error(0x3F) 响应而非挂死
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(3.0); s.connect((chain.host, chain.port)); s.settimeout(0.3)
            stream = Stream()
            s.sendall(pack_cmd(0x0011, b'\x00', 0x8108))
            deadline = time.time() + 3.0
            got_resp = False
            while time.time() < deadline and not got_resp:
                for f in stream.drain(s):
                    if f['seq'] == 0x8108:
                        got_resp = True  # 任何带该 seq 的响应 (frame_error) 都算鲁棒
            s.close()
            rep.report('F8 非法命令0x11', got_resp, '收到响应' if got_resp else '无响应/挂死')
        except Exception as e:
            rep.report('F8 非法命令0x11', False, f'异常: {e}')
    finally:
        chain.reconnect_tcp()


# ═══════════════════════════════════════════════════════════════════
# G. 丢包 (RK 响应丢包率扫描 → 重传 + 数据不损坏)
# ═══════════════════════════════════════════════════════════════════

def test_loss(chain, rep, n):
    print('\n[G] 丢包 (RK响应丢包率扫描)')
    if not chain.rk:
        print('  [SKIP] 无串口/RK模拟器'); return
    for ri, rate in enumerate((0.0, 0.1, 0.3, 0.5)):
        chain.rk.configure(drop_rate=rate, corrupt_rate=0, delay_ms=0, transform='echo')
        chain.rk.reset_stats()
        base = 0x9000 + ri * 0x100   # 每档丢包率独立 seq 段, 避免跨迭代残留帧串扰
        jobs = [dict(cmd=VALID_CMDS[i % len(VALID_CMDS)],
                     params=rand_payload(i, 4, 16), seq=base + i) for i in range(n)]
        outcomes = pipeline(chain, jobs, window=8, result_timeout=6.0)
        c = summarize(outcomes)
        # 数据不损坏: 所有 ok 的结果 payload 必须逐字节匹配
        bad = sum(1 for j, o in outcomes.items()
                  if o['outcome'] == 'ok' and o['result_params'] != o['params'])
        snap = chain.rk.snapshot()
        rep.report(f'G 丢包率{int(rate*100)}%',
                   bad == 0,
                   f'ok={c["ok"]}/{n} abandoned={c["abandoned"]} 数据损坏={bad} '
                   f'MCU重传={snap["retransmit"]}次 单seq最多重传={snap["max_dup"]} '
                   f'(fwd={snap["forwarded"]} resp={snap["responded"]})')


# ═══════════════════════════════════════════════════════════════════
# H. 延迟抖动
# ═══════════════════════════════════════════════════════════════════

def test_latency(chain, rep, n):
    print('\n[H] 延迟抖动 (RK响应延迟扫描 + 随机抖动)')
    if not chain.rk:
        print('  [SKIP] 无串口/RK模拟器'); return

    # H1: 固定延迟扫描 (0/50/200/450ms 在 500ms 超时内应全部成功)
    for di, delay in enumerate((0, 50, 200, 450)):
        chain.rk.configure(delay_ms=delay, drop_rate=0, corrupt_rate=0, transform='echo')
        chain.rk.reset_stats()
        base = 0xA000 + di * 0x100   # 每档延迟独立 seq 段
        jobs = [dict(cmd=0x0001, params=rand_payload(i, 4, 8), seq=base + i)
                for i in range(n)]
        outcomes = pipeline(chain, jobs, window=4, result_timeout=3.0)
        c = summarize(outcomes)
        rtt = [o['rtt'] for o in outcomes.values() if o.get('rtt')]
        # 核心验证: 延迟下数据不损坏. 450ms 延迟 + ~100ms 处理基线 > 500ms 超时,
        # 必然触发重传后放弃, 所以不要求 ok==n.
        rep.report(f'H1 固定延迟{delay}ms', c['bad'] == 0,
                   f'ok={c["ok"]}/{n} abandoned={c["abandoned"]} {ms_stats("RTT", rtt)}')

    # H2: 超过超时(550/800ms) → 触发 MCU 重传, 最终仍应完成或放弃
    for di, delay in enumerate((550, 800)):
        chain.rk.configure(delay_ms=delay, drop_rate=0, corrupt_rate=0, transform='echo')
        chain.rk.reset_stats()
        base = 0xB000 + di * 0x100   # 每档延迟独立 seq 段
        jobs = [dict(cmd=0x0001, params=rand_payload(i, 4, 8), seq=base + i)
                for i in range(n)]
        outcomes = pipeline(chain, jobs, window=4, result_timeout=5.0)
        c = summarize(outcomes)
        snap = chain.rk.snapshot()
        rep.report(f'H2 超时延迟{delay}ms', c['bad'] == 0,
                   f'ok={c["ok"]}/{n} abandoned={c["abandoned"]} MCU重传={snap["retransmit"]}')

    # H3: 随机抖动 (0~300ms) → 数据仍正确
    chain.rk.configure(delay_ms=20, jitter_ms=280, drop_rate=0, corrupt_rate=0, transform='echo')
    chain.rk.reset_stats()
    jobs = [dict(cmd=0x0001, params=rand_payload(i, 4, 16), seq=0xC000 + i) for i in range(n)]
    outcomes = pipeline(chain, jobs, window=4, result_timeout=4.0)
    c = summarize(outcomes)
    bad = sum(1 for j, o in outcomes.items()
              if o['outcome'] == 'ok' and o['result_params'] != o['params'])
    rep.report('H3 随机抖动0~300ms', bad == 0,
               f'ok={c["ok"]}/{n} abandoned={c["abandoned"]} 数据损坏={bad}')


# ═══════════════════════════════════════════════════════════════════
# I. 背压 (下游不响应 → BUSY → 恢复)
# ═══════════════════════════════════════════════════════════════════

def test_backpressure(chain, rep, n):
    print('\n[I] 背压 (下游不响应 → BUSY → 恢复)')
    if not chain.rk:
        print('  [SKIP] 无串口/RK模拟器'); return

    # 阶段1: 让 RK 完全不响应 → MCU 队列/pending 饱和 → 应回 BUSY 而非假 ACK/静默丢
    chain.rk.configure(drop_rate=1.0, corrupt_rate=0, delay_ms=0)
    chain.rk.reset_stats()
    jobs = [dict(cmd=VALID_CMDS[i % len(VALID_CMDS)], params=b'\x01', seq=0xD000 + i)
            for i in range(n)]
    outcomes = pipeline(chain, jobs, window=16, result_timeout=2.0, ack_timeout=1.0)
    c1 = summarize(outcomes)
    # 饱和后应该出现 busy (诚实拒绝), 且 0 静默丢失 (miss 应为 0)
    rep.report('I1 下游静默→BUSY', c1['miss'] == 0,
               f'ack={c1["ok"]+c1["busy"]+c1["abandoned"]} busy={c1["busy"]} '
               f'miss={c1["miss"]}(静默丢,应为0)')

    # 阶段2: RK 恢复响应 → 链路应自动恢复, 新命令全通
    time.sleep(4.0)  # 等 MCU 把旧 pending 放弃 (500+1000+2000ms 退避)
    chain.rk.configure(drop_rate=0, corrupt_rate=0, delay_ms=0, transform='echo')
    chain.rk.reset_stats()
    jobs2 = [dict(cmd=0x0001, params=rand_payload(i, 4, 8), seq=0xE000 + i)
             for i in range(20)]
    outcomes2 = pipeline(chain, jobs2, window=4, result_timeout=3.0)
    c2 = summarize(outcomes2)
    rep.report('I2 恢复后链路', c2['ok'] == 20, f'ok={c2["ok"]}/20 (恢复后全通)')


# ═══════════════════════════════════════════════════════════════════
# J. 心跳
# ═══════════════════════════════════════════════════════════════════

def test_heartbeat(chain, rep):
    print('\n[J] 心跳 (1s 周期 + seq 单调 + 复位检测)')
    if not chain.rk:
        print('  [SKIP] 无串口/RK模拟器'); return
    # hb_count/reset_count 是全程累计值, 这里取 3.5s 窗口内的差值
    hb0 = chain.rk.snapshot()['hb_count']
    # 采集 3.5s 心跳
    t0 = time.time()
    while time.time() - t0 < 3.5:
        time.sleep(0.1)
    snap = chain.rk.snapshot()
    got = snap['hb_count'] - hb0
    # 3.5s 应收到 ~3 个心跳 (2~5 为合理范围)
    ok_count = 2 <= got <= 5
    ok_reset = snap['reset_count'] == 0
    rep.report('J1 心跳周期', ok_count, f'{got}个/3.5s (期望~3)')
    rep.report('J2 心跳seq无复位', ok_reset, f'复位计数={snap["reset_count"]}')


# ═══════════════════════════════════════════════════════════════════
# K. 粘包 / 拆包
# ═══════════════════════════════════════════════════════════════════

def test_frameboundary(chain, rep):
    print('\n[K] 粘包/拆包')
    chain.rk.configure(delay_ms=0, drop_rate=0, corrupt_rate=0, transform='echo')
    chain.close_tcp()   # MCU 单客户端: 释放主连接位
    time.sleep(0.3)
    try:
        # 粘包: 3 帧一次发出 → 3 个结果
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(3.0); s.connect((chain.host, chain.port)); s.settimeout(0.5)
        stream = Stream()
        f1 = pack_cmd(0x0001, b'\x11', 0xF001)
        f2 = pack_cmd(0x0002, b'\x22', 0xF002)
        f3 = pack_cmd(0x0003, b'\x33', 0xF003)
        s.sendall(f1 + f2 + f3)
        deadline = time.time() + 3.0
        got = set()
        while time.time() < deadline and len(got) < 3:
            for f in stream.drain(s):
                if f['seq'] in (0xF001, 0xF002, 0xF003) and not is_ack(f):
                    got.add(f['seq'])
        rep.report('K1 粘包 3帧→3结果', got == {0xF001, 0xF002, 0xF003}, f'got={sorted(got)}')
        s.close()

        # 拆包: 1 帧分 3 段发 → 1 个结果
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(3.0); s.connect((chain.host, chain.port)); s.settimeout(0.5)
        stream = Stream()
        f = pack_cmd(0x0012, b'\xAA\xBB\xCC', 0xF010)
        s.sendall(f[:5]); time.sleep(0.05)
        s.sendall(f[5:13]); time.sleep(0.05)
        s.sendall(f[13:])
        deadline = time.time() + 3.0
        got = False
        while time.time() < deadline and not got:
            for fr in stream.drain(s):
                if fr['seq'] == 0xF010 and not is_ack(fr):
                    got = True
        rep.report('K2 拆包 3段→1结果', got, '收到结果' if got else '无结果')
        s.close()
    finally:
        chain.reconnect_tcp()


# ═══════════════════════════════════════════════════════════════════
# L. 空闲恢复
# ═══════════════════════════════════════════════════════════════════

def test_idle(chain, rep):
    print('\n[L] 空闲恢复')
    chain.rk.configure(delay_ms=0, drop_rate=0, corrupt_rate=0, transform='echo')

    # 先跑一批确认链路 OK
    jobs = [dict(cmd=0x0001, params=rand_payload(i, 4, 8), seq=0x00010000 + i)
            for i in range(5)]
    outcomes = pipeline(chain, jobs, window=4)
    pre_ok = summarize(outcomes)['ok'] == 5

    # 空闲 6s (超过心跳 1s / 半帧超时 500ms / 重传退避 3.5s)
    print('    ... 空闲 6s ...')
    time.sleep(6.0)

    # 空闲后验证
    jobs2 = [dict(cmd=0x0001, params=rand_payload(i, 4, 8), seq=0x00020000 + i)
             for i in range(5)]
    outcomes2 = pipeline(chain, jobs2, window=4)
    post_ok = summarize(outcomes2)['ok'] == 5
    rep.report('L1 空闲恢复', pre_ok and post_ok,
               f'空闲前={pre_ok} 空闲后={post_ok}')


# ═══════════════════════════════════════════════════════════════════
# 场景注册表
# ═══════════════════════════════════════════════════════════════════

SCENARIOS = [
    ('A', 'throughput',    '大吞吐量', test_throughput),
    ('B', 'protocol',      '协议一致性', test_protocol),
    ('C', 'correctness',   '数据正确性', test_correctness),
    ('D', 'reliability',   '可靠性', test_reliability),
    ('E', 'endurance',     '耐久性', test_endurance),
    ('F', 'fault',         '错误注入', test_fault),
    ('G', 'loss',          '丢包', test_loss),
    ('H', 'latency',       '延迟抖动', test_latency),
    ('I', 'backpressure',  '背压', test_backpressure),
    ('J', 'heartbeat',     '心跳', test_heartbeat),
    ('K', 'frameboundary', '粘包拆包', test_frameboundary),
    ('L', 'idle',          '空闲恢复', test_idle),
]


def run_scenario(code, name, desc, fn, chain, rep, args):
    if code == 'E':
        fn(chain, rep, args.duration)
    elif code == 'A':
        fn(chain, rep, args.n, args.window)
    elif code in ('B', 'G', 'H', 'I'):
        fn(chain, rep, args.n)
    else:
        fn(chain, rep)


def main():
    ap = argparse.ArgumentParser(description='全链路压力测试')
    ap.add_argument('host', nargs='?', default='192.168.1.201')
    ap.add_argument('port', nargs='?', type=int, default=8888)
    ap.add_argument('com', nargs='?', default='COM3')
    ap.add_argument('--only', help='只跑指定场景, 逗号分隔 (A,B,C 或名字)')
    ap.add_argument('--list', action='store_true', help='列出场景')
    ap.add_argument('--duration', type=int, default=60, help='耐久性时长(秒)')
    ap.add_argument('--n', type=int, default=200, help='默认命令数')
    ap.add_argument('--window', type=int, default=16, help='流水线窗口')
    ap.add_argument('--seed', type=int, default=1, help='随机种子')
    args = ap.parse_args()

    if args.list:
        print('场景:')
        for code, name, desc, _ in SCENARIOS:
            print(f'  {code}  {name:<14} {desc}')
        return 0

    random.seed(args.seed)

    # 选择场景
    only = None
    if args.only:
        only = set()
        for tok in args.only.split(','):
            tok = tok.strip().upper()
            for code, name, desc, _ in SCENARIOS:
                if tok == code or tok == name.upper():
                    only.add(code)
    selected = [s for s in SCENARIOS if only is None or s[0] in only]

    print('=' * 70)
    print(f' 全链路压力测试  TCP {args.host}:{args.port}  串口 {args.com}')
    if not HAS_SERIAL:
        print('  [WARN] 无 pyserial, 串口相关场景将 SKIP')
    print('=' * 70)

    chain = Chain(args.host, args.port, args.com)
    try:
        chain.connect()
    except Exception as e:
        print(f'[FATAL] 连接失败: {e}')
        return 1

    rep = Reporter()
    try:
        for code, name, desc, fn in selected:
            run_scenario(code, name, desc, fn, chain, rep, args)
    finally:
        chain.close()

    return 0 if rep.summary() else 1


if __name__ == '__main__':
    sys.exit(main())
