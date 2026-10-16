/*
 * ============================================================================
 * kfm_chan_DLL.c  —  缠论分析 DLL（通达信 9 号槽，公式+DLL 模式）
 * ============================================================================
 *
 * 功能：K线包含处理 / 分型 / 笔 / 线段 / 笔中枢，供通达信主图画线标注。
 *
 * 接口契约（依据《通达信DLL编写注意事项.MD》实测结论）：
 *   - 结构体 #pragma pack(1)，sizeof(PluginTCalcFuncInfo) == 10
 *   - 只导出 RegisterTdxFunc（.def 控制），拒绝二次注册
 *   - 计算函数签名：void __cdecl Fn(int, float*, float*, float*, float*)
 *   - 输入序列方向：A[0] 最老，A[last] 最新
 *
 * 两次调用模式（通达信一次只能传 1 个序列 + 2 个常数）：
 *   功能号 1  KFM_SetH : pfINa = HIGH 序列 -> 存入 DLL 静态缓冲，回显输出
 *   功能号 2  KFM_Out  : pfINa = LOW  序列 -> 存入后完成全部缠论计算，
 *                        pfINb[0] = 输出选择器 selector，pfINc[0]=777 时写调试日志
 *
 * selector 一览（0 值 = 该 bar 无输出，公式侧转 DRAWNULL / 作条件）：
 *    1  FXG   顶分型值（在顶分型最高 bar 输出分型高）
 *    2  FXD   底分型值
 *    3  BI    全部笔端点值（POLYLINE 画笔）
 *    4  XD    全部线段端点值（POLYLINE 画线段）
 *    5  ZG    中枢上沿（中枢区间内的 bar）
 *    6  ZD    中枢下沿
 *    7  MKH   被合并K线组的高（组内所有 bar，仅 size>=2 的组）
 *    8  MKL   被合并K线组的低
 *    9  BIG   笔顶值（仅顶笔端点）
 *   10  BID   笔底值
 *   11  XDG   线段顶值
 *   12  XDD   线段底值
 *   16  ZGS   中枢起点标记（起点bar输出ZG，供第二版矩形画法）
 *   17  ZGE   中枢终点标记（终点bar输出ZG）
 *   90  诊断：常量 DataLen      91 诊断：常量 笔数
 *   92  诊断：常量 线段数       93 诊断：常量 中枢数
 *   94  诊断：常量 合并K线组数  95 诊断：常量 分型数
 *
 * 算法要点：
 *   - K线包含处理：方向动态判定（首次包含默认向上），向上合并取高高/向下取低低
 *   - 分型：合并K序列上严格三K分型
 *   - 笔：严格笔，顶底交替，合并K间隔 >= 4（含端点至少 5 根）；同类分型取更极端者
 *   - 线段：特征序列包含处理 + 分型；第一种情况（无缺口）直接确认；
 *           第二种情况（有缺口）待反向特征序列分型确认，期间新高/新低则取消重扫
 *   - 中枢：笔中枢，前三笔重叠区 [max(d),min(g)]，后续笔相交则延伸，不相交则终结
 *   - 末尾未确认的笔/线段照常输出（指标惯例，实时延伸）
 *
 * 编译（MinGW-w64 64位）：
 *   D:\mingw64\bin\gcc.exe -O2 -shared -o kfm_chan_DLL.dll kfm_chan_DLL.c kfm_chan_DLL.def -lm
 *
 * 部署：C:\new_tdx64\T0002\dlls\kfm_chan_DLL.dll ，dlls.ini 绑定 band9
 * ============================================================================
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

/* ---------------- 接口结构体（pack(1)，sizeof 必须为 10） ---------------- */
#pragma pack(push, 1)
typedef void (__cdecl *pPluginFUNC)(int DataLen, float *pfOUT,
                                    float *pfINa, float *pfINb, float *pfINc);
typedef struct tagPluginTCalcFuncInfo {
    unsigned short nFuncMark;
    pPluginFUNC    pCallFunc;
} PluginTCalcFuncInfo;
#pragma pack(pop)

/* ---------------- 常量与缓冲 ---------------- */
#define MAXN 16384              /* 最大K线数（远超实测 420/840） */
#define MAXP (MAXN / 2 + 16)    /* 最大分型/端点数 */
#define MAXS (MAXP / 3 + 16)    /* 最大线段/中枢数 */
#define BIGF 1.0e30f

static float g_H[MAXN], g_L[MAXN];
static int   g_Hlen = 0;

/* 合并K线 */
typedef struct { float hi, lo; int start, end, hi_idx, lo_idx, size; } MK;
static MK  g_mk[MAXN];
static int g_nmk = 0;

/* 分型 */
typedef struct { int type; int midx; float val; int bar; } FX; /* type +1顶 -1底 */
static FX  g_fx[MAXP];
static int g_nfx = 0;

/* 笔端点（顶底交替） */
typedef struct { int type; int midx; float price; int bar; } Pivot;
static Pivot g_pv[MAXP];
static int   g_npv = 0;

/* 线段（端点在 g_pv 中的下标区间） */
typedef struct { int s, e; } Seg;
static Seg g_seg[MAXS];
static int g_nseg = 0;

/* 中枢 */
typedef struct { float zg, zd; int start_bar, end_bar; } ZS;
static ZS  g_zs[MAXS];
static int g_nzs = 0;

/* ---------------- 调试日志（默认静默，selector 带 777 才写） ---------------- */
static void klog(const char *fmt, ...)
{
    FILE *f = fopen("C:\\new_tdx64\\T0002\\dlls\\kfm_chan_log.txt", "a");
    va_list ap;
    if (!f) return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

/* ---------------- 数据净化 ---------------- */
static int validf(float x)
{
    if (!(x == x))   return 0;   /* NaN */
    if (x >  BIGF || x < -BIGF) return 0;
    if (x == 0.0f)   return 0;   /* 停牌/脏值 */
    return 1;
}

/* 头部脏值用首个有效值回填，中间/尾部前向填充 */
static void sanitize(float *a, int n)
{
    int i;
    float first = 0.0f, last;
    for (i = 0; i < n; i++) if (validf(a[i])) { first = a[i]; break; }
    if (i >= n) return;                      /* 全脏，保持 0，后续会判失败 */
    for (i = 0; i < n && !validf(a[i]); i++) a[i] = first;
    last = first;
    for (; i < n; i++) {
        if (validf(a[i])) last = a[i];
        else              a[i] = last;
    }
}

/* ---------------- 第 1 步：K线包含处理 ---------------- */
static void build_merged(int n)
{
    int i, dir = 0;
    g_nmk = 0;
    for (i = 0; i < n; i++) {
        float h = g_H[i], l = g_L[i];
        MK *c;
        if (g_nmk == 0) {
            g_mk[0].hi = h; g_mk[0].lo = l;
            g_mk[0].start = g_mk[0].end = i;
            g_mk[0].hi_idx = g_mk[0].lo_idx = i;
            g_mk[0].size = 1;
            g_nmk = 1;
            continue;
        }
        c = &g_mk[g_nmk - 1];
        if (h > c->hi && l > c->lo) {           /* 向上，无包含 */
            dir = 1;
            c = &g_mk[g_nmk++];
            c->hi = h; c->lo = l;
            c->start = c->end = i;
            c->hi_idx = c->lo_idx = i;
            c->size = 1;
        } else if (h < c->hi && l < c->lo) {    /* 向下，无包含 */
            dir = -1;
            c = &g_mk[g_nmk++];
            c->hi = h; c->lo = l;
            c->start = c->end = i;
            c->hi_idx = c->lo_idx = i;
            c->size = 1;
        } else {                                /* 包含关系，按方向合并 */
            if (dir >= 0) {                     /* 向上处理：取高高 */
                if (h > c->hi) { c->hi = h; c->hi_idx = i; }
                if (l > c->lo) { c->lo = l; c->lo_idx = i; }
            } else {                            /* 向下处理：取低低 */
                if (h < c->hi) { c->hi = h; c->hi_idx = i; }
                if (l < c->lo) { c->lo = l; c->lo_idx = i; }
            }
            c->end = i;
            c->size++;
        }
    }
}

/* ---------------- 第 2 步：分型 ---------------- */
static void build_fractals(void)
{
    int i;
    g_nfx = 0;
    for (i = 1; i < g_nmk - 1; i++) {
        if (g_mk[i].hi > g_mk[i-1].hi && g_mk[i].hi > g_mk[i+1].hi &&
            g_nfx < MAXP) {
            g_fx[g_nfx].type = 1;
            g_fx[g_nfx].midx = i;
            g_fx[g_nfx].val  = g_mk[i].hi;
            g_fx[g_nfx].bar  = g_mk[i].hi_idx;
            g_nfx++;
        } else if (g_mk[i].lo < g_mk[i-1].lo && g_mk[i].lo < g_mk[i+1].lo &&
                   g_nfx < MAXP) {
            g_fx[g_nfx].type = -1;
            g_fx[g_nfx].midx = i;
            g_fx[g_nfx].val  = g_mk[i].lo;
            g_fx[g_nfx].bar  = g_mk[i].lo_idx;
            g_nfx++;
        }
    }
}

/* ---------------- 第 3 步：笔（严格笔，合并K间隔 >= 4） ---------------- */
#define BI_GAP 4

static void build_pivots(void)
{
    int i;
    int ct = 0, cm = 0, cb = 0;   /* 候选端点：类型/合并K下标/原始bar */
    float cv = 0.0f;
    g_npv = 0;
    for (i = 0; i < g_nfx; i++) {
        int t = g_fx[i].type, m = g_fx[i].midx, b = g_fx[i].bar;
        float v = g_fx[i].val;
        if (ct == 0) { ct = t; cm = m; cv = v; cb = b; continue; }
        if (t == ct) {                              /* 同类分型：取更极端 */
            if ((t == 1 && v >= cv) || (t == -1 && v <= cv)) {
                cm = m; cv = v; cb = b;
            }
            continue;
        }
        /* 异类分型：间隔够则确认一笔 */
        if (m - cm >= BI_GAP && g_npv < MAXP) {
            g_pv[g_npv].type  = ct;
            g_pv[g_npv].midx  = cm;
            g_pv[g_npv].price = cv;
            g_pv[g_npv].bar   = cb;
            g_npv++;
            ct = t; cm = m; cv = v; cb = b;
        }
        /* 间隔不够：忽略该分型，候选端点保持不变 */
    }
    /* 末尾候选端点作为未确认端点照常输出 */
    if (ct != 0 && g_npv < MAXP) {
        g_pv[g_npv].type  = ct;
        g_pv[g_npv].midx  = cm;
        g_pv[g_npv].price = cv;
        g_pv[g_npv].bar   = cb;
        g_npv++;
    }
}

/* ---------------- 第 4 步：线段（特征序列分型法） ---------------- */

static float qprice(int idx, int dir) { return dir * g_pv[idx].price; }

/* 特征序列包含处理的临时缓冲（文件级静态，避免大栈帧） */
static float ph_h[MAXP], ph_l[MAXP];
static int   ph_k0[MAXP];
static int   ph_n;
static float uh_[MAXP], ul_[MAXP];
static int   u_n_;

/*
 * 第二种情况确认：r 为候选段端点（q 空间中的顶点），
 * 扫描其后的反向特征序列（q 空间上向元素），
 * 返回 1=确认，0=被取消（出现 q 新高），-1=数据不足暂未确认
 */
static int confirm_case2(int r, int dir, int np)
{
    int m, ufdir = 0;
    float qr = qprice(r, dir);
    u_n_ = 0;
    for (m = 0; ; m++) {
        int c = r + 1 + 2 * m;   /* q 底 */
        int d = c + 1;           /* q 顶 */
        float uh, ul;
        if (d >= np) return -1;
        if (qprice(d, dir) > qr) return 0;      /* q 新高，候选被取消 */
        uh = qprice(d, dir);
        ul = qprice(c, dir);
        if (u_n_ == 0) { uh_[0] = uh; ul_[0] = ul; u_n_ = 1; continue; }
        if (uh > uh_[u_n_-1] && ul > ul_[u_n_-1]) {
            uh_[u_n_] = uh; ul_[u_n_] = ul; u_n_++; ufdir = 1;
        } else if (uh < uh_[u_n_-1] && ul < ul_[u_n_-1]) {
            uh_[u_n_] = uh; ul_[u_n_] = ul; u_n_++; ufdir = -1;
        } else {
            if (ufdir >= 0) { if (uh > uh_[u_n_-1]) uh_[u_n_-1] = uh;
                              if (ul > ul_[u_n_-1]) ul_[u_n_-1] = ul; }
            else              { if (uh < uh_[u_n_-1]) uh_[u_n_-1] = uh;
                              if (ul < ul_[u_n_-1]) ul_[u_n_-1] = ul; }
        }
        if (u_n_ >= 3) {                        /* q 顶分型 -> 确认 */
            int j1 = u_n_-3, j2 = u_n_-2, j3 = u_n_-1;
            if (uh_[j2] > uh_[j1] && uh_[j2] > uh_[j3] &&
                ul_[j2] > ul_[j1] && ul_[j2] > ul_[j3])
                return 1;
        }
    }
}

/*
 * 求从端点 s 出发、方向 dir（+1 q上 / -1 q下）的线段终点端点下标，
 * 找不到（未确认）返回 -1。
 * q 空间统一处理：q(i) = dir * price(i)，线段在 q 空间恒为"向上"。
 */
static int find_seg_end(int s, int dir)
{
    int np = g_npv;
    static int invalid[MAXP];   /* 被取消过的候选端点，防止死循环 */
    int ninv = 0;

    for (;;) {                  /* 取消后从头重扫 */
        int k, fdir = 0;
        ph_n = 0;
        for (k = 0; ; k++) {
            int a = s + 1 + 2 * k;   /* q 顶（特征元素起点） */
            int b = a + 1;           /* q 底 */
            float eh, el;
            if (b >= np) return -1;  /* 数据耗尽，未确认 */
            eh = qprice(a, dir);
            el = qprice(b, dir);
            /* 特征序列包含处理 */
            if (ph_n == 0) {
                ph_h[0] = eh; ph_l[0] = el; ph_k0[0] = k; ph_n = 1;
                continue;
            }
            if (eh > ph_h[ph_n-1] && el > ph_l[ph_n-1]) {
                ph_h[ph_n] = eh; ph_l[ph_n] = el; ph_k0[ph_n] = k;
                ph_n++; fdir = 1;
            } else if (eh < ph_h[ph_n-1] && el < ph_l[ph_n-1]) {
                ph_h[ph_n] = eh; ph_l[ph_n] = el; ph_k0[ph_n] = k;
                ph_n++; fdir = -1;
            } else {
                if (fdir >= 0) { if (eh > ph_h[ph_n-1]) ph_h[ph_n-1] = eh;
                                 if (el > ph_l[ph_n-1]) ph_l[ph_n-1] = el; }
                else           { if (eh < ph_h[ph_n-1]) ph_h[ph_n-1] = eh;
                                 if (el < ph_l[ph_n-1]) ph_l[ph_n-1] = el; }
            }
            /* q 底分型判定（线段顶） */
            if (ph_n >= 3) {
                int i1 = ph_n-3, i2 = ph_n-2, i3 = ph_n-1;
                if (ph_l[i2] < ph_l[i1] && ph_l[i2] < ph_l[i3] &&
                    ph_h[i2] < ph_h[i1] && ph_h[i2] < ph_h[i3]) {
                    int r = s + 1 + 2 * ph_k0[i1];
                    int ii, skip;
                    if (r - s < 3) continue;    /* 线段至少 3 笔 */
                    if (ph_h[i2] >= ph_l[i1]) return r;  /* 无缺口：第一种情况 */
                    /* 有缺口：第二种情况 */
                    skip = 0;
                    for (ii = 0; ii < ninv; ii++)
                        if (invalid[ii] == r) { skip = 1; break; }
                    if (skip) continue;
                    {
                        int conf = confirm_case2(r, dir, np);
                        if (conf == 1)  return r;
                        if (conf == 0) {            /* 被取消，记入黑名单重扫 */
                            if (ninv < MAXP) invalid[ninv++] = r;
                            break;                  /* 跳出内层 for，外层 for(;;) 重扫 */
                        }
                        return -1;                  /* 数据不足，暂未确认 */
                    }
                }
            }
        }
        /* 只有 conf==0 的 break 会到这里 -> 重扫；return 的已直接返回 */
    }
}

static void build_segments(void)
{
    int s, dir;
    g_nseg = 0;
    if (g_npv < 2) return;
    if (g_npv < 4) {                       /* 不足成段，整体作未确认段画出 */
        g_seg[0].s = 0; g_seg[0].e = g_npv - 1; g_nseg = 1;
        return;
    }
    s = 0;
    dir = (g_pv[1].price > g_pv[0].price) ? 1 : -1;
    while (s < g_npv - 1 && g_nseg < MAXS) {
        int e = find_seg_end(s, dir);
        if (e < 0) break;
        g_seg[g_nseg].s = s;
        g_seg[g_nseg].e = e;
        g_nseg++;
        s = e;
        dir = -dir;
    }
    /* 末尾未确认段照常画出（实时延伸） */
    if (s < g_npv - 1 && g_nseg < MAXS) {
        g_seg[g_nseg].s = s;
        g_seg[g_nseg].e = g_npv - 1;
        g_nseg++;
    }
}

/* ---------------- 第 5 步：笔中枢 ---------------- */
static void build_zhongshu(void)
{
    int nbis = g_npv - 1;
    int i;
    g_nzs = 0;
    if (nbis < 3) return;
    i = 0;
    while (i + 2 < nbis && g_nzs < MAXS) {
        float h1 = g_pv[i].price   > g_pv[i+1].price ? g_pv[i].price   : g_pv[i+1].price;
        float l1 = g_pv[i].price   < g_pv[i+1].price ? g_pv[i].price   : g_pv[i+1].price;
        float h2 = g_pv[i+1].price > g_pv[i+2].price ? g_pv[i+1].price : g_pv[i+2].price;
        float l2 = g_pv[i+1].price < g_pv[i+2].price ? g_pv[i+1].price : g_pv[i+2].price;
        float h3 = g_pv[i+2].price > g_pv[i+3].price ? g_pv[i+2].price : g_pv[i+3].price;
        float l3 = g_pv[i+2].price < g_pv[i+3].price ? g_pv[i+2].price : g_pv[i+3].price;
        float zg = h1 < h2 ? h1 : h2; if (h3 < zg) zg = h3;
        float zd = l1 > l2 ? l1 : l2; if (l3 > zd) zd = l3;
        if (zd <= zg) {                 /* 前三笔有重叠，中枢成立 */
            int j = i + 3;
            while (j < nbis) {          /* 与 [zd,zg] 相交则延伸 */
                float hj = g_pv[j].price > g_pv[j+1].price ? g_pv[j].price : g_pv[j+1].price;
                float lj = g_pv[j].price < g_pv[j+1].price ? g_pv[j].price : g_pv[j+1].price;
                if (lj <= zg && hj >= zd) j++;
                else break;
            }
            g_zs[g_nzs].zg = zg;
            g_zs[g_nzs].zd = zd;
            g_zs[g_nzs].start_bar = g_pv[i].bar;
            g_zs[g_nzs].end_bar   = g_pv[j].bar;
            g_nzs++;
            i = j;
        } else {
            i++;
        }
    }
}

/* ---------------- 全量计算 ---------------- */
static void compute_all(int n)
{
    build_merged(n);
    build_fractals();
    build_pivots();
    build_segments();
    build_zhongshu();
}

/* ---------------- 功能号 1：存 HIGH ---------------- */
void __cdecl KFM_SetH(int DataLen, float *pfOUT,
                      float *pfINa, float *pfINb, float *pfINc)
{
    int i;
    (void)pfINb; (void)pfINc;
    if (!pfOUT || !pfINa || DataLen <= 0) return;
    if (DataLen > MAXN) DataLen = MAXN;
    for (i = 0; i < DataLen; i++) g_H[i] = pfINa[i];
    sanitize(g_H, DataLen);
    g_Hlen = DataLen;
    for (i = 0; i < DataLen; i++) pfOUT[i] = g_H[i];   /* 回显，兼作恒等诊断 */
}

/* ---------------- 功能号 2：存 LOW + 计算 + 选择输出 ---------------- */
void __cdecl KFM_Out(int DataLen, float *pfOUT,
                     float *pfINa, float *pfINb, float *pfINc)
{
    int i, sel, debug;
    (void)debug;
    if (!pfOUT || !pfINa || DataLen <= 0) return;
    if (DataLen > MAXN) DataLen = MAXN;

    sel   = (pfINb && pfINb[0] == pfINb[0]) ? (int)(pfINb[0] + 0.5f) : 0;
    debug = (pfINc && pfINc[0] == 777.0f) ? 1 : 0;

    for (i = 0; i < DataLen; i++) pfOUT[i] = 0.0f;

    /* HIGH 必须由功能号 1 先存入且长度一致 */
    if (g_Hlen != DataLen) return;

    for (i = 0; i < DataLen; i++) g_L[i] = pfINa[i];
    sanitize(g_L, DataLen);
    if (!validf(g_L[0])) return;          /* 输入全脏 */

    compute_all(DataLen);

    if (debug) {
        klog("KFM_Out L=%d sel=%d mk=%d fx=%d pv=%d seg=%d zs=%d\n",
             DataLen, sel, g_nmk, g_nfx, g_npv, g_nseg, g_nzs);
    }

    switch (sel) {
    case 1:   /* FXG 顶分型 */
        for (i = 0; i < g_nfx; i++)
            if (g_fx[i].type == 1 && g_fx[i].bar < DataLen)
                pfOUT[g_fx[i].bar] = g_fx[i].val;
        break;
    case 2:   /* FXD 底分型 */
        for (i = 0; i < g_nfx; i++)
            if (g_fx[i].type == -1 && g_fx[i].bar < DataLen)
                pfOUT[g_fx[i].bar] = g_fx[i].val;
        break;
    case 3:   /* BI 全部笔端点 */
        for (i = 0; i < g_npv; i++)
            if (g_pv[i].bar < DataLen) pfOUT[g_pv[i].bar] = g_pv[i].price;
        break;
    case 4:   /* XD 全部线段端点 */
        for (i = 0; i < g_nseg; i++) {
            int s = g_seg[i].s, e = g_seg[i].e;
            if (g_pv[s].bar < DataLen) pfOUT[g_pv[s].bar] = g_pv[s].price;
            if (g_pv[e].bar < DataLen) pfOUT[g_pv[e].bar] = g_pv[e].price;
        }
        break;
    case 5:   /* ZG 中枢上沿 */
    case 6: { /* ZD 中枢下沿 */
        int z;
        for (z = 0; z < g_nzs; z++) {
            int b;
            int eb = g_zs[z].end_bar;
            if (eb >= DataLen) eb = DataLen - 1;
            for (b = g_zs[z].start_bar; b <= eb; b++)
                pfOUT[b] = (sel == 5) ? g_zs[z].zg : g_zs[z].zd;
        }
        break;
    }
    case 7:   /* MKH 被合并K线组高 */
    case 8: { /* MKL 被合并K线组低 */
        int m;
        for (m = 0; m < g_nmk; m++) {
            int b;
            if (g_mk[m].size < 2) continue;
            for (b = g_mk[m].start; b <= g_mk[m].end && b < DataLen; b++)
                pfOUT[b] = (sel == 7) ? g_mk[m].hi : g_mk[m].lo;
        }
        break;
    }
    case 9:   /* BIG 笔顶 */
        for (i = 0; i < g_npv; i++)
            if (g_pv[i].type == 1 && g_pv[i].bar < DataLen)
                pfOUT[g_pv[i].bar] = g_pv[i].price;
        break;
    case 10:  /* BID 笔底 */
        for (i = 0; i < g_npv; i++)
            if (g_pv[i].type == -1 && g_pv[i].bar < DataLen)
                pfOUT[g_pv[i].bar] = g_pv[i].price;
        break;
    case 11:  /* XDG 线段顶 */
    case 12: {/* XDD 线段底 */
        for (i = 0; i < g_nseg; i++) {
            int pts[2];
            pts[0] = g_seg[i].s; pts[1] = g_seg[i].e;
            {
                int k;
                for (k = 0; k < 2; k++) {
                    int p = pts[k];
                    int want = (sel == 11) ? 1 : -1;
                    if (g_pv[p].type == want && g_pv[p].bar < DataLen)
                        pfOUT[g_pv[p].bar] = g_pv[p].price;
                }
            }
        }
        break;
    }
    case 16:  /* ZGS 中枢起点标记（起点bar输出ZG） */
    case 17: {/* ZGE 中枢终点标记（终点bar输出ZG） */
        int z;
        for (z = 0; z < g_nzs; z++) {
            int b = (sel == 16) ? g_zs[z].start_bar : g_zs[z].end_bar;
            if (b >= DataLen) b = DataLen - 1;
            if (b >= 0) pfOUT[b] = g_zs[z].zg;
        }
        break;
    }
    case 90: for (i = 0; i < DataLen; i++) pfOUT[i] = (float)DataLen; break;
    case 91: for (i = 0; i < DataLen; i++) pfOUT[i] = (float)(g_npv - 1); break;
    case 92: for (i = 0; i < DataLen; i++) pfOUT[i] = (float)g_nseg; break;
    case 93: for (i = 0; i < DataLen; i++) pfOUT[i] = (float)g_nzs; break;
    case 94: { int m, cnt = 0;
               for (m = 0; m < g_nmk; m++) { if (g_mk[m].size >= 2) cnt++; }
               for (i = 0; i < DataLen; i++) { pfOUT[i] = (float)cnt; }
               break; }
    case 95: for (i = 0; i < DataLen; i++) pfOUT[i] = (float)g_nfx; break;
    default: break;
    }
}

/* ---------------- 函数表与注册入口 ---------------- */
PluginTCalcFuncInfo g_CalcFuncSets[] = {
    { 1, (pPluginFUNC)&KFM_SetH },
    { 2, (pPluginFUNC)&KFM_Out  },
    { 0, NULL }
};

static int g_bRegistered = 0;

__declspec(dllexport) BOOL __cdecl RegisterTdxFunc(PluginTCalcFuncInfo **pFun)
{
    if (pFun == NULL)    return FALSE;
    if (g_bRegistered)   return FALSE;   /* 拒绝二次注册（防双注册闪退） */
    if (*pFun != NULL)   return FALSE;
    if (sizeof(PluginTCalcFuncInfo) != 10) return FALSE;
    *pFun = g_CalcFuncSets;
    g_bRegistered = 1;
    return TRUE;
}
