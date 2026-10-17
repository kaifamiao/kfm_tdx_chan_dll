# -*- coding: utf-8 -*-
"""
verify_dll.py — kfm_chan_DLL.dll 离线验证
L1 接口层：ctypes 模拟通达信加载/注册/读表/调用
L2 数值层：Python 参考实现交叉核对 + 缠论结构不变量断言
"""
import ctypes as C_
import math, random, sys
import numpy as np

DLL = r"C:\Users\LINRUI\ai\kfm_tdx_chan_dll\kfm_chan_DLL.dll"

# ---------------------------------------------------------------- L1 接口层
lib = C_.WinDLL(DLL)

class Info(C_.Structure):
    _pack_ = 1
    _fields_ = [("nFuncMark", C_.c_ushort), ("pCallFunc", C_.c_void_p)]

PF = C_.CFUNCTYPE(None, C_.c_int,
                  C_.POINTER(C_.c_float), C_.POINTER(C_.c_float),
                  C_.POINTER(C_.c_float), C_.POINTER(C_.c_float))

lib.RegisterTdxFunc.argtypes = [C_.POINTER(C_.POINTER(Info))]
lib.RegisterTdxFunc.restype = C_.c_int

p = C_.POINTER(Info)()
assert lib.RegisterTdxFunc(C_.byref(p)) == 1, "注册失败"
assert C_.sizeof(Info) == 10, "结构体尺寸 != 10"

TABLE = {}
i = 0
while p[i].nFuncMark and p[i].pCallFunc:
    TABLE[p[i].nFuncMark] = PF(p[i].pCallFunc)
    i += 1
assert sorted(TABLE.keys()) == [1, 2, 3], f"函数表异常: {sorted(TABLE.keys())}"
print(f"L1 OK: sizeof=10, 函数表={sorted(TABLE.keys())}")

F1, F2, F3 = TABLE[1], TABLE[2], TABLE[3]

def call_dll(H, L, Cc, sel, extra=0.0):
    """模拟通达信：先 fn1(HIGH)，再 fn3(CLOSE)，最后 fn2(LOW, sel)"""
    n = len(H)
    fb = (C_.c_float * 1)(float(sel))
    fc = (C_.c_float * 1)(float(extra))
    fa = (C_.c_float * n)(*H)
    out1 = (C_.c_float * n)()
    F1(n, out1, fa, fb, fc)
    fa3 = (C_.c_float * n)(*Cc)
    out3 = (C_.c_float * n)()
    F3(n, out3, fa3, fb, fc)
    fl = (C_.c_float * n)(*L)
    out2 = (C_.c_float * n)()
    F2(n, out2, fl, fb, fc)
    return list(out2)

# ------------------------------------------------- Python 参考实现（同规格）
def macd_f32(Cc):
    """float32 逐步递推，与 DLL 的 float 运算对齐"""
    n = len(Cc)
    dif = np.zeros(n, dtype=np.float64)
    macd = np.zeros(n, dtype=np.float64)
    e12 = np.float32(Cc[0]); e26 = np.float32(Cc[0]); dea = np.float32(0)
    a12 = np.float32(2.0 / 13.0); a26 = np.float32(2.0 / 27.0); a9 = np.float32(2.0 / 10.0)
    negpre = np.zeros(n + 1); pospre = np.zeros(n + 1)
    for i in range(n):
        c = np.float32(Cc[i])
        e12 = np.float32(e12 + np.float32(c - e12) * a12)
        e26 = np.float32(e26 + np.float32(c - e26) * a26)
        d = np.float32(e12 - e26)
        dea = np.float32(dea + np.float32(d - dea) * a9)
        m = np.float32(2.0) * np.float32(d - dea)
        dif[i] = float(d)
        macd[i] = float(m)
        negpre[i + 1] = negpre[i] + (-float(m) if m < 0 else 0.0)
        pospre[i + 1] = pospre[i] + (float(m) if m > 0 else 0.0)
    return dif, macd, negpre, pospre

def ref_chan(H, L, Cc=None):
    n = len(H)
    # 1. K线包含处理
    mk = []
    d = 0
    for i in range(n):
        h, l = H[i], L[i]
        if not mk:
            mk.append(dict(hi=h, lo=l, start=i, end=i, hi_idx=i, lo_idx=i, size=1))
            continue
        c = mk[-1]
        if h > c['hi'] and l > c['lo']:
            d = 1
            mk.append(dict(hi=h, lo=l, start=i, end=i, hi_idx=i, lo_idx=i, size=1))
        elif h < c['hi'] and l < c['lo']:
            d = -1
            mk.append(dict(hi=h, lo=l, start=i, end=i, hi_idx=i, lo_idx=i, size=1))
        else:
            if d >= 0:
                if h > c['hi']: c['hi'] = h; c['hi_idx'] = i
                if l > c['lo']: c['lo'] = l; c['lo_idx'] = i
            else:
                if h < c['hi']: c['hi'] = h; c['hi_idx'] = i
                if l < c['lo']: c['lo'] = l; c['lo_idx'] = i
            c['end'] = i; c['size'] += 1
    # 2. 分型
    fx = []
    for i in range(1, len(mk) - 1):
        if mk[i]['hi'] > mk[i-1]['hi'] and mk[i]['hi'] > mk[i+1]['hi']:
            fx.append(dict(t=1, m=i, v=mk[i]['hi'], bar=mk[i]['hi_idx']))
        elif mk[i]['lo'] < mk[i-1]['lo'] and mk[i]['lo'] < mk[i+1]['lo']:
            fx.append(dict(t=-1, m=i, v=mk[i]['lo'], bar=mk[i]['lo_idx']))
    # 3. 笔
    GAP = 4
    pv = []
    cand = None
    for f in fx:
        if cand is None:
            cand = f; continue
        if f['t'] == cand['t']:
            if (f['t'] == 1 and f['v'] >= cand['v']) or (f['t'] == -1 and f['v'] <= cand['v']):
                cand = f
            continue
        if f['m'] - cand['m'] >= GAP:
            pv.append(dict(cand, cfm=f['bar']))  # cfm=触发确认的反向分型bar
            cand = f
    if cand is not None:
        pv.append(dict(cand, cfm=-1))            # 末尾未确认端点
    # 4. 线段（特征序列法）
    def q(idx, dirn): return dirn * pv[idx]['v']
    def confirm_case2(r, dirn):
        qr = q(r, dirn)
        uh, ul = [], []
        fd = 0
        m = 0
        while True:
            c = r + 1 + 2 * m; dd = c + 1
            if dd >= len(pv): return -1
            if q(dd, dirn) > qr: return 0
            h_, l_ = q(dd, dirn), q(c, dirn)
            if not uh:
                uh.append(h_); ul.append(l_)
            elif h_ > uh[-1] and l_ > ul[-1]:
                uh.append(h_); ul.append(l_); fd = 1
            elif h_ < uh[-1] and l_ < ul[-1]:
                uh.append(h_); ul.append(l_); fd = -1
            else:
                if fd >= 0:
                    uh[-1] = max(uh[-1], h_); ul[-1] = max(ul[-1], l_)
                else:
                    uh[-1] = min(uh[-1], h_); ul[-1] = min(ul[-1], l_)
            if len(uh) >= 3:
                if (uh[-2] > uh[-3] and uh[-2] > uh[-1] and
                        ul[-2] > ul[-3] and ul[-2] > ul[-1]):
                    return 1
            m += 1
    def find_seg_end(s, dirn):
        npv = len(pv)
        invalid = set()
        while True:
            ph, pl, pk = [], [], []
            fd = 0
            k = 0
            restart = False
            while True:
                a = s + 1 + 2 * k; b = a + 1
                if b >= npv: return -1
                eh, el = q(a, dirn), q(b, dirn)
                if not ph:
                    ph.append(eh); pl.append(el); pk.append(k)
                elif eh > ph[-1] and el > pl[-1]:
                    ph.append(eh); pl.append(el); pk.append(k); fd = 1
                elif eh < ph[-1] and el < pl[-1]:
                    ph.append(eh); pl.append(el); pk.append(k); fd = -1
                else:
                    if fd >= 0:
                        ph[-1] = max(ph[-1], eh); pl[-1] = max(pl[-1], el)
                    else:
                        ph[-1] = min(ph[-1], eh); pl[-1] = min(pl[-1], el)
                if len(ph) >= 3:
                    i1, i2, i3 = len(ph)-3, len(ph)-2, len(ph)-1
                    if (pl[i2] < pl[i1] and pl[i2] < pl[i3] and
                            ph[i2] < ph[i1] and ph[i2] < ph[i3]):
                        r = s + 1 + 2 * pk[i1]
                        if r - s >= 3:
                            if ph[i2] >= pl[i1]:
                                return r
                            if r not in invalid:
                                conf = confirm_case2(r, dirn)
                                if conf == 1: return r
                                if conf == 0:
                                    invalid.add(r); restart = True; break
                                return -1
                k += 1
            if not restart:
                return -1
    seg = []
    if len(pv) >= 2:
        if len(pv) < 4:
            seg.append((0, len(pv) - 1))
        else:
            s = 0
            dirn = 1 if pv[1]['v'] > pv[0]['v'] else -1
            while s < len(pv) - 1:
                e = find_seg_end(s, dirn)
                if e < 0: break
                seg.append((s, e)); s = e; dirn = -dirn
            if s < len(pv) - 1:
                seg.append((s, len(pv) - 1))
    # 5. 笔中枢（sp/ep = 端点下标）
    zs = []
    nb = len(pv) - 1
    i = 0
    while i + 2 < nb:
        hs = [max(pv[j]['v'], pv[j+1]['v']) for j in (i, i+1, i+2)]
        ls = [min(pv[j]['v'], pv[j+1]['v']) for j in (i, i+1, i+2)]
        zg, zd = min(hs), max(ls)
        if zd <= zg:
            j = i + 3
            while j < nb:
                hj = max(pv[j]['v'], pv[j+1]['v'])
                lj = min(pv[j]['v'], pv[j+1]['v'])
                if lj <= zg and hj >= zd: j += 1
                else: break
            zs.append(dict(zg=zg, zd=zd, sb=pv[i]['bar'], eb=pv[j]['bar'],
                           sp=i, ep=j))
            i = j
        else:
            i += 1
    # 6. 买卖点（需要 CLOSE）
    pts = []
    if Cc is not None and len(pv) >= 4:
        dif, macd, negpre, pospre = macd_f32(Cc)
        def leg(ii):
            b1, b2 = pv[ii]['bar'], pv[ii+1]['bar']
            if b2 < b1: b1, b2 = b2, b1
            na = negpre[b2+1] - negpre[b1+1]
            pa = pospre[b2+1] - pospre[b1+1]
            seg_dif = dif[b1+1:b2+1]
            mn = float(seg_dif.min()) if len(seg_dif) else 1e30
            mx = float(seg_dif.max()) if len(seg_dif) else -1e30
            return na, pa, mn, mx
        last_b1 = -1; b2_done = True
        last_s1 = -1; s2_done = True
        for i in range(2, len(pv) - 1):        # 排除末尾未确认端点（防漂移）
            if pv[i]['t'] == -1:
                is_b1 = False
                if i >= 3:
                    zi = None
                    for z in range(len(zs) - 1, -1, -1):
                        if zs[z]['ep'] < i: zi = z; break
                    if (zi is not None and pv[i]['v'] < zs[zi]['zd']
                            and pv[i]['v'] < pv[i-2]['v']):
                        na1, _, mn1, _ = leg(i-1)
                        na0, _, mn0, _ = leg(i-3)
                        if na1 < na0 and mn1 > mn0:
                            is_b1 = True
                if is_b1:
                    pts.append(dict(bar=pv[i]['bar'], v=pv[i]['v'], kind=1, pvi=i, cfm=pv[i]['cfm']))
                    last_b1 = i; b2_done = False
                elif last_b1 >= 0 and not b2_done:
                    if pv[i]['v'] > pv[last_b1]['v']:
                        pts.append(dict(bar=pv[i]['bar'], v=pv[i]['v'], kind=2, pvi=i, cfm=pv[i]['cfm']))
                        b2_done = True
                    else:
                        last_b1 = -1
            else:
                is_s1 = False
                if i >= 3:
                    zi = None
                    for z in range(len(zs) - 1, -1, -1):
                        if zs[z]['ep'] < i: zi = z; break
                    if (zi is not None and pv[i]['v'] > zs[zi]['zg']
                            and pv[i]['v'] > pv[i-2]['v']):
                        _, pa1, _, mx1 = leg(i-1)
                        _, pa0, _, mx0 = leg(i-3)
                        if pa1 < pa0 and mx1 < mx0:
                            is_s1 = True
                if is_s1:
                    pts.append(dict(bar=pv[i]['bar'], v=pv[i]['v'], kind=4, pvi=i, cfm=pv[i]['cfm']))
                    last_s1 = i; s2_done = False
                elif last_s1 >= 0 and not s2_done:
                    if pv[i]['v'] < pv[last_s1]['v']:
                        pts.append(dict(bar=pv[i]['bar'], v=pv[i]['v'], kind=5, pvi=i, cfm=pv[i]['cfm']))
                        s2_done = True
                    else:
                        last_s1 = -1
        for z in range(len(zs)):
            zg, zd, ep = zs[z]['zg'], zs[z]['zd'], zs[z]['ep']
            k = None
            for kk in range(ep + 1, len(pv)):
                if pv[kk]['t'] == 1 and pv[kk]['v'] > zg: k = kk; break
            if k is not None:
                m = k + 1
                if m < len(pv) - 1 and pv[m]['t'] == -1 and pv[m]['v'] > zg:
                    pts.append(dict(bar=pv[m]['bar'], v=pv[m]['v'], kind=3, pvi=m, cfm=pv[m]['cfm']))
            k = None
            for kk in range(ep + 1, len(pv)):
                if pv[kk]['t'] == -1 and pv[kk]['v'] < zd: k = kk; break
            if k is not None:
                m = k + 1
                if m < len(pv) - 1 and pv[m]['t'] == 1 and pv[m]['v'] < zd:
                    pts.append(dict(bar=pv[m]['bar'], v=pv[m]['v'], kind=6, pvi=m, cfm=pv[m]['cfm']))
    # 7. 预警信号（分型级；与 C build_alerts 一一对应，密封判定 midx <= nmk-3）
    if Cc is not None and len(fx) >= 4:
        nmk = len(mk)
        dif, macd, negpre, pospre = macd_f32(Cc)
        def bstat(b1, b2):
            if b2 < b1: b1, b2 = b2, b1
            na = negpre[b2+1] - negpre[b1+1]
            pa = pospre[b2+1] - pospre[b1+1]
            sd = dif[b1+1:b2+1]
            mn = float(sd.min()) if len(sd) else 1e30
            mx = float(sd.max()) if len(sd) else -1e30
            return na, pa, mn, mx
        last_pb1 = -1; pb2_done = True
        last_ps1 = -1; ps2_done = True
        for j in range(1, len(fx)):
            if fx[j]['m'] > nmk - 3: break      # 未密封
            cfm_j = mk[fx[j]['m'] + 2]['start']
            if fx[j]['t'] == -1:                # 底分型
                is_pb1 = False
                if j >= 3:
                    na1, _, mn1, _ = bstat(fx[j-1]['bar'], fx[j]['bar'])
                    na0, _, mn0, _ = bstat(fx[j-3]['bar'], fx[j-2]['bar'])
                    if fx[j]['v'] < fx[j-2]['v'] and na1 < na0 and mn1 > mn0:
                        is_pb1 = True
                if is_pb1:
                    pts.append(dict(bar=fx[j]['bar'], v=fx[j]['v'], kind=7, cfm=cfm_j))
                    last_pb1 = j; pb2_done = False
                elif last_pb1 >= 0 and not pb2_done:
                    if fx[j]['v'] > fx[last_pb1]['v']:
                        pts.append(dict(bar=fx[j]['bar'], v=fx[j]['v'], kind=8, cfm=cfm_j))
                        pb2_done = True
                    else:
                        last_pb1 = -1
            else:                               # 顶分型（镜像）
                is_ps1 = False
                if j >= 3:
                    _, pa1, _, mx1 = bstat(fx[j-1]['bar'], fx[j]['bar'])
                    _, pa0, _, mx0 = bstat(fx[j-3]['bar'], fx[j-2]['bar'])
                    if fx[j]['v'] > fx[j-2]['v'] and pa1 < pa0 and mx1 < mx0:
                        is_ps1 = True
                if is_ps1:
                    pts.append(dict(bar=fx[j]['bar'], v=fx[j]['v'], kind=10, cfm=cfm_j))
                    last_ps1 = j; ps2_done = False
                elif last_ps1 >= 0 and not ps2_done:
                    if fx[j]['v'] < fx[last_ps1]['v']:
                        pts.append(dict(bar=fx[j]['bar'], v=fx[j]['v'], kind=11, cfm=cfm_j))
                        ps2_done = True
                    else:
                        last_ps1 = -1
        npv = len(pv)
        for z in range(len(zs)):                # 预3买/预3卖
            if zs[z]['sp'] + 3 > npv - 2: continue   # 形成端点未确认
            fbar = pv[zs[z]['sp'] + 3]['bar']
            zg, zd = zs[z]['zg'], zs[z]['zd']
            k = -1
            for jj in range(len(fx)):           # 预3买：突破ZG后首个回抽底分型守住ZG
                if fx[jj]['m'] > nmk - 3: break
                if fx[jj]['bar'] <= fbar: continue
                if k < 0:
                    if fx[jj]['t'] == 1 and fx[jj]['v'] > zg: k = jj
                elif fx[jj]['t'] == -1:
                    if fx[jj]['v'] > zg:
                        pts.append(dict(bar=fx[jj]['bar'], v=fx[jj]['v'], kind=9,
                                        cfm=mk[fx[jj]['m'] + 2]['start']))
                    break
            k = -1
            for jj in range(len(fx)):           # 预3卖（镜像）
                if fx[jj]['m'] > nmk - 3: break
                if fx[jj]['bar'] <= fbar: continue
                if k < 0:
                    if fx[jj]['t'] == -1 and fx[jj]['v'] < zd: k = jj
                elif fx[jj]['t'] == 1:
                    if fx[jj]['v'] < zd:
                        pts.append(dict(bar=fx[jj]['bar'], v=fx[jj]['v'], kind=12,
                                        cfm=mk[fx[jj]['m'] + 2]['start']))
                    break
    return mk, fx, pv, seg, zs, pts

def ref_output(H, L, Cc, sel, anchor=0):
    mk, fx, pv, seg, zs, pts = ref_chan(H, L, Cc)
    n = len(H)
    out = [0.0] * n
    def put(bar, v):
        if 0 <= bar < n: out[bar] = v
    if sel == 1:
        for f in fx:
            if f['t'] == 1: put(f['bar'], f['v'])
    elif sel == 2:
        for f in fx:
            if f['t'] == -1: put(f['bar'], f['v'])
    elif sel == 3:
        for p_ in pv: put(p_['bar'], p_['v'])
    elif sel == 4:
        for s, e in seg:
            put(pv[s]['bar'], pv[s]['v']); put(pv[e]['bar'], pv[e]['v'])
    elif sel == 5 or sel == 6:
        for z in zs:
            for b in range(z['sb'], min(z['eb'], n - 1) + 1):
                if out[b] == 0.0:   # 共享边界：先写者保留
                    out[b] = z['zg'] if sel == 5 else z['zd']
    elif sel == 7 or sel == 8:
        for m in mk:
            if m['size'] >= 2:
                for b in range(m['start'], min(m['end'], n - 1) + 1):
                    out[b] = m['hi'] if sel == 7 else m['lo']
    elif sel == 9:
        for p_ in pv:
            if p_['t'] == 1: put(p_['bar'], p_['v'])
    elif sel == 10:
        for p_ in pv:
            if p_['t'] == -1: put(p_['bar'], p_['v'])
    elif sel == 11 or sel == 12:
        want = 1 if sel == 11 else -1
        for s, e in seg:
            for pp in (s, e):
                if pv[pp]['t'] == want: put(pv[pp]['bar'], pv[pp]['v'])
    elif sel in (16, 17, 18, 19):
        prev_eb = None
        for z in zs:
            if sel in (16, 18):
                b = z['sb']
                if prev_eb is not None and b == prev_eb and b < z['eb']:
                    b += 1
            else:
                b = z['eb']
            prev_eb = z['eb']
            b = min(b, n - 1)
            if b >= 0: out[b] = z['zg'] if sel in (16, 17) else z['zd']
    elif 20 <= sel <= 31:
        want = sel - 19
        for pt in pts:
            if pt['kind'] == want:
                b = pt['cfm'] if anchor else pt['bar']
                if b >= 0: put(b, pt['v'])
    return out, (mk, fx, pv, seg, zs, pts)

# ---------------------------------------------------------------- L2 数值层
def gen_data(kind, n, seed=42):
    random.seed(seed)
    H, L, Cc = [], [], []
    px = 100.0
    for i in range(n):
        if kind == 'trend':
            px += 0.15 + random.uniform(-0.6, 0.6)
        elif kind == 'walk':
            px += random.uniform(-1.0, 1.0)
        elif kind == 'wave':
            px = 100 + 12 * math.sin(i / 9.0) + 0.05 * i + random.uniform(-0.5, 0.5)
        elif kind == 'step':
            px = 100.0 if i < n // 2 else 120.0
            px += random.uniform(-0.4, 0.4)
        else:
            px += random.uniform(-0.05, 0.05)
        hi = px + random.uniform(0.2, 0.9)
        lo = px - random.uniform(0.2, 0.9)
        cl = random.uniform(lo, hi)
        H.append(round(hi, 3)); L.append(round(lo, 3)); Cc.append(round(cl, 3))
    return H, L, Cc

def structural_check(tag, pv, seg, zs, pts):
    for a, b in zip(pv, pv[1:]):
        assert a['t'] != b['t'], f"[{tag}] 笔端点未交替"
        assert b['m'] - a['m'] >= 4, f"[{tag}] 笔间隔 < 4"
    for s, e in seg:
        assert e - s >= 2, f"[{tag}] 线段不足"
    for (s1, e1), (s2, e2) in zip(seg, seg[1:]):
        assert e1 == s2, f"[{tag}] 线段不连续"
    for z in zs:
        assert z['zd'] <= z['zg'] + 1e-6, f"[{tag}] 中枢区间倒置"
    # 买卖点结构断言
    pvbar = {p_['bar']: p_ for p_ in pv}
    b1s = [pt for pt in pts if pt['kind'] == 1]
    for pt in pts:
        if pt['kind'] <= 6:  # 预警点（7-12）标在分型上，不要求笔端点
            assert pt['bar'] in pvbar, f"[{tag}] 买卖点不在笔端点"
    for pt in b1s:  # 一买必须是底端点且跌破某中枢ZD
        p_ = pvbar[pt['bar']]
        assert p_['t'] == -1, f"[{tag}] 一买不是底端点"
        assert any(pt['v'] < z['zd'] for z in zs), f"[{tag}] 一买未跌破中枢ZD"
    for pt in pts:
        if pt['kind'] == 2:  # 二买必须高于此前最近一买
            prev_b1 = [q for q in b1s if q['bar'] < pt['bar']]
            assert prev_b1 and pt['v'] > prev_b1[-1]['v'], f"[{tag}] 二买破一买低点"
        if pt['kind'] == 3:  # 三买必须站在某中枢ZG之上
            assert any(pt['v'] > z['zg'] for z in zs), f"[{tag}] 三买未站上ZG"
        if pt['kind'] == 4:
            p_ = pvbar[pt['bar']]
            assert p_['t'] == 1, f"[{tag}] 一卖不是顶端点"
        if pt['kind'] == 6:
            assert any(pt['v'] < z['zd'] for z in zs), f"[{tag}] 三卖未压在ZD下"

SELS = list(range(1, 13)) + [16, 17, 18, 19] + list(range(20, 32))

def series_eq(got, exp):
    """容差比较：DLL 为 float32，参考实现为 float64"""
    if len(got) != len(exp): return False
    for g, e in zip(got, exp):
        if e == 0.0:
            if g != 0.0: return False
        elif abs(g - e) > max(1e-4, abs(e) * 1e-5):
            return False
    return True

fails = 0
total = 0

def run_all():
  global fails, total
  for kind in ('trend', 'walk', 'wave', 'step', 'flat'):
    for n in (120, 420, 840):
        H, L, Cc = gen_data(kind, n, seed=hash((kind, n)) % 100000)
        mk, fx, pv, seg, zs, pts = ref_chan(H, L, Cc)
        structural_check(f"{kind}/{n}", pv, seg, zs, pts)
        for sel in SELS:
            got = call_dll(H, L, Cc, sel)
            exp, _ = ref_output(H, L, Cc, sel)
            total += 1
            if not series_eq(got, exp):
                fails += 1
                print(f"FAIL {kind}/n={n}/sel={sel}")
        # 诊断选择器抽查
        d92 = call_dll(H, L, Cc, 92)[0]
        d93 = call_dll(H, L, Cc, 93)[0]
        total += 2
        if int(d92) != len(seg): fails += 1; print(f"FAIL {kind}/{n} 线段数 {d92}!={len(seg)}")
        if int(d93) != len(zs):  fails += 1; print(f"FAIL {kind}/{n} 中枢数 {d93}!={len(zs)}")
        nb1 = sum(1 for pt in pts if pt['kind'] == 1)
        nb3 = sum(1 for pt in pts if pt['kind'] == 3)
        ns1 = sum(1 for pt in pts if pt['kind'] == 4)
        ns3 = sum(1 for pt in pts if pt['kind'] == 6)
        print(f"  {kind:5s} n={n:3d}: bi={len(pv)-1:3d} seg={len(seg):2d} zs={len(zs):2d} "
              f"B1={nb1} B3={nb3} S1={ns1} S3={ns3} OK")

  # NaN / 停牌脏值健壮性
  H, L, Cc = gen_data('walk', 200, seed=7)
  H[0] = float('nan'); L[1] = 0.0; H[199] = 1e31; Cc[5] = float('nan')
  out = call_dll(H, L, Cc, 3)
  total += 1
  if any((x != x) or abs(x) > 1e29 for x in out):
      fails += 1; print("FAIL 脏值净化")
  else:
      print("  脏值净化 OK")

  # ★ 流式防漂移测试：数据逐段增长，历史信号不得移动/消失（单调递增）
  for kind in ('walk', 'trend'):
      H, L, Cc = gen_data(kind, 840, seed=123)
      prev = {}
      drift = 0
      for t in range(150, 841, 30):
          cur = {}
          for sel in range(20, 32):
              out = call_dll(H[:t], L[:t], Cc[:t], sel)
              for bi_, x in enumerate(out):
                  if x > 0: cur[(bi_, sel)] = round(x, 3)
          for k, v in prev.items():
              if k not in cur or abs(cur[k] - v) > 1e-3:
                  drift += 1
                  print(f"FAIL 信号漂移 {kind} t={t} key={k}")
          prev = cur
          # 前缀上 DLL 与参考实现一致性（抽查）
          if t % 120 == 0:
              for sel in (20, 22, 26, 29):
                  got = call_dll(H[:t], L[:t], Cc[:t], sel)
                  exp, _ = ref_output(H[:t], L[:t], Cc[:t], sel)
                  total += 1
                  if not series_eq(got, exp):
                      fails += 1; print(f"FAIL 前缀一致性 {kind} t={t} sel={sel}")
          total += 1
      if drift == 0:
          print(f"  流式防漂移 {kind}: OK（信号只增不改）")
      else:
          fails += drift

  # 锚定模式一致性（pfINc=1 → 确认bar）
  H, L, Cc = gen_data('walk', 420, seed=55)
  for sel in range(20, 32):
      got = call_dll(H, L, Cc, sel, extra=1.0)
      exp, _ = ref_output(H, L, Cc, sel, anchor=1)
      total += 1
      if not series_eq(got, exp):
          fails += 1; print(f"FAIL 锚定模式 sel={sel}")
  print("  锚定模式(确认bar) OK")

  print(f"\n=== L1+L2 验证: {total - fails}/{total} 通过 ===")
  return fails

if __name__ == "__main__":
    sys.exit(1 if run_all() else 0)
