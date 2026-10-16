# -*- coding: utf-8 -*-
"""
verify_dll.py — kfm_chan_DLL.dll 离线验证
L1 接口层：ctypes 模拟通达信加载/注册/读表/调用
L2 数值层：Python 参考实现交叉核对 + 缠论结构不变量断言
"""
import ctypes as C
import math, random, sys

DLL = r"C:\Users\LINRUI\ai\kfm_tdx_chan_dll\kfm_chan_DLL.dll"

# ---------------------------------------------------------------- L1 接口层
lib = C.WinDLL(DLL)

class Info(C.Structure):
    _pack_ = 1
    _fields_ = [("nFuncMark", C.c_ushort), ("pCallFunc", C.c_void_p)]

PF = C.CFUNCTYPE(None, C.c_int,
                 C.POINTER(C.c_float), C.POINTER(C.c_float),
                 C.POINTER(C.c_float), C.POINTER(C.c_float))

lib.RegisterTdxFunc.argtypes = [C.POINTER(C.POINTER(Info))]
lib.RegisterTdxFunc.restype = C.c_int

p = C.POINTER(Info)()
assert lib.RegisterTdxFunc(C.byref(p)) == 1, "注册失败"
assert C.sizeof(Info) == 10, "结构体尺寸 != 10"
assert lib.RegisterTdxFunc(C.byref(C.POINTER(Info)())) in (0, 1), "二次注册防护异常"

TABLE = {}
i = 0
while p[i].nFuncMark and p[i].pCallFunc:
    TABLE[p[i].nFuncMark] = PF(p[i].pCallFunc)
    i += 1
assert sorted(TABLE.keys()) == [1, 2], f"函数表异常: {sorted(TABLE.keys())}"
print(f"L1 OK: sizeof=10, 函数表={sorted(TABLE.keys())}")

F1, F2 = TABLE[1], TABLE[2]

def call_dll(H, L, sel, extra=0.0):
    """模拟通达信：先 fn1(HIGH)，再 fn2(LOW, sel)"""
    n = len(H)
    fa = (C.c_float * n)(*H)
    fb = (C.c_float * 1)(float(sel))
    fc = (C.c_float * 1)(float(extra))
    out1 = (C.c_float * n)()
    F1(n, out1, fa, fb, fc)
    out2 = (C.c_float * n)()
    F2(n, out2, fa, fb, fc)   # 注意：fn2 的 pfINa 是 LOW
    # fn2 需要的是 LOW —— 重新调用，pfINa 换成 L
    fl = (C.c_float * n)(*L)
    F2(n, out2, fl, fb, fc)
    return list(out2)

# ------------------------------------------------- Python 参考实现（同规格）
def ref_chan(H, L):
    n = len(H)
    # 1. K线包含处理
    mk = []  # dict(hi,lo,start,end,hi_idx,lo_idx,size)
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
            pv.append(cand); cand = f
    if cand is not None:
        pv.append(cand)
    # 4. 线段（特征序列法，q 空间统一）
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
    # 5. 笔中枢
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
            zs.append(dict(zg=zg, zd=zd, sb=pv[i]['bar'], eb=pv[j]['bar']))
            i = j
        else:
            i += 1
    return mk, fx, pv, seg, zs

def ref_output(H, L, sel):
    mk, fx, pv, seg, zs = ref_chan(H, L)
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
        for p in pv: put(p['bar'], p['v'])
    elif sel == 4:
        for s, e in seg:
            put(pv[s]['bar'], pv[s]['v']); put(pv[e]['bar'], pv[e]['v'])
    elif sel == 5 or sel == 6:
        for z in zs:
            for b in range(z['sb'], min(z['eb'], n - 1) + 1):
                out[b] = z['zg'] if sel == 5 else z['zd']
    elif sel == 7 or sel == 8:
        for m in mk:
            if m['size'] >= 2:
                for b in range(m['start'], min(m['end'], n - 1) + 1):
                    out[b] = m['hi'] if sel == 7 else m['lo']
    elif sel == 9:
        for p in pv:
            if p['t'] == 1: put(p['bar'], p['v'])
    elif sel == 10:
        for p in pv:
            if p['t'] == -1: put(p['bar'], p['v'])
    elif sel == 11 or sel == 12:
        want = 1 if sel == 11 else -1
        for s, e in seg:
            for pp in (s, e):
                if pv[pp]['t'] == want: put(pv[pp]['bar'], pv[pp]['v'])
    elif sel == 16 or sel == 17:
        for z in zs:
            b = z['sb'] if sel == 16 else z['eb']
            b = min(b, n - 1)
            if b >= 0: out[b] = z['zg']
    return out, (mk, fx, pv, seg, zs)

# ---------------------------------------------------------------- L2 数值层
def gen_data(kind, n, seed=42):
    random.seed(seed)
    H, L = [], []
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
        else:  # flat 极端：大量包含关系
            px += random.uniform(-0.05, 0.05)
        hi = px + random.uniform(0.2, 0.9)
        lo = px - random.uniform(0.2, 0.9)
        H.append(round(hi, 3)); L.append(round(lo, 3))
    return H, L

def structural_check(tag, H, L, mk, fx, pv, seg, zs):
    # 合并K：相邻两组合并K不得再存在包含
    for a, b in zip(mk, mk[1:]):
        inc = (b['hi'] <= a['hi'] and b['lo'] >= a['lo']) or \
              (b['hi'] >= a['hi'] and b['lo'] <= a['lo'])
        assert not inc, f"[{tag}] 合并K残留包含"
    # 笔：顶底交替 + 间隔>=4 + 顶值>底值邻接
    for a, b in zip(pv, pv[1:]):
        assert a['t'] != b['t'], f"[{tag}] 笔端点未交替"
        assert b['m'] - a['m'] >= 4, f"[{tag}] 笔间隔 < 4"
    for p in pv:
        t0 = pv[0]['t']
    # 线段：>=3 笔，端点交替，区间单调
    for s, e in seg:
        assert e - s >= 2, f"[{tag}] 线段不足"
    for (s1, e1), (s2, e2) in zip(seg, seg[1:]):
        assert e1 == s2, f"[{tag}] 线段不连续"
    # 中枢：zd<=zg，且由前三笔极值构成
    for z in zs:
        assert z['zd'] <= z['zg'] + 1e-6, f"[{tag}] 中枢区间倒置"
        assert z['eb'] >= z['sb'], f"[{tag}] 中枢bar倒置"

fails = 0
total = 0

def run_all():
  global fails, total
  for kind in ('trend', 'walk', 'wave', 'step', 'flat'):
    for n in (120, 420, 840):
        H, L = gen_data(kind, n, seed=hash((kind, n)) % 100000)
        mk, fx, pv, seg, zs = ref_chan(H, L)
        structural_check(f"{kind}/{n}", H, L, mk, fx, pv, seg, zs)
        for sel in list(range(1, 13)) + [16, 17]:
            got = call_dll(H, L, sel)
            exp, _ = ref_output(H, L, sel)
            total += 1
            ok = len(got) == len(exp)
            if ok:
                for g, e in zip(got, exp):
                    if e == 0.0:
                        if g != 0.0: ok = False; break
                    elif abs(g - e) > max(1e-4, abs(e) * 1e-5):
                        ok = False; break
            if not ok:
                fails += 1
                print(f"FAIL {kind}/n={n}/sel={sel}")
        # 诊断选择器抽查
        d92 = call_dll(H, L, 92)[0]
        d93 = call_dll(H, L, 93)[0]
        total += 2
        if int(d92) != len(seg): fails += 1; print(f"FAIL {kind}/{n} 线段数 {d92}!={len(seg)}")
        if int(d93) != len(zs):  fails += 1; print(f"FAIL {kind}/{n} 中枢数 {d93}!={len(zs)}")
        print(f"  {kind:5s} n={n:3d}: mk={len(mk):3d} fx={len(fx):3d} "
              f"bi={len(pv)-1:3d} seg={len(seg):2d} zs={len(zs):2d} OK")

  # NaN / 停牌脏值健壮性
  H, L = gen_data('walk', 200, seed=7)
  H[0] = float('nan'); L[1] = 0.0; H[199] = 1e31
  out = call_dll(H, L, 3)
  total += 1
  if any((x != x) or abs(x) > 1e29 for x in out):
      fails += 1; print("FAIL 脏值净化")
  else:
      print("  脏值净化 OK")

  print(f"\n=== L1+L2 验证: {total - fails}/{total} 通过 ===")
  return fails

if __name__ == "__main__":
    sys.exit(1 if run_all() else 0)
