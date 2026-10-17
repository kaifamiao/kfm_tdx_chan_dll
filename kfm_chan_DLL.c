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
 *   功能号 3  KFM_SetC : pfINa = CLOSE 序列 -> 存入（买卖点 MACD 背驰用），回显输出
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
 *   18  ZDS   中枢起点标记（起点bar输出ZD）
 *   19  ZDE   中枢终点标记（终点bar输出ZD）
 *   20  B1    一买   21  B2 二买   22  B3 三买
 *   23  S1    一卖   24  S2 二卖   25  S3 三卖
 *        （买卖点需公式先调功能号3存入 CLOSE：CC:=TDXDLL9(3,CLOSE,0,0)）
 *        信号只标在已确认端点上，出现后永不漂移；
 *        pfINc[0]=1 时标在"确认bar"（信号出现时的新K线，不补历史）
 *   26  PB1   预警一买   27  PB2 预警二买   28  PB3 预警三买
 *   29  PS1   预警一卖   30  PS2 预警二卖   31  PS3 预警三卖
 *        （预警层：分型密封（右侧2根合并K成型）即判定，
 *          比确认层早约2-4根K线出现；密封后分型永不改写，
 *          故预警信号同样固定不漂移。口径：预1买=底分型相对前底
 *          分型创新低+MACD面积缩小+DIF抬高；预2买=预1买后不创新低
 *          的首个底分型；预3买=中枢形成后突破ZG、回抽底分型守住ZG）
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

/* 笔端点（顶底交替）；cfm = 确认bar（使本端点成立的那个反向分型所在bar），
   末尾未确认端点 cfm = -1 */
typedef struct { int type; int midx; float price; int bar; int cfm; } Pivot;
static Pivot g_pv[MAXP];
static int   g_npv = 0;

/* 线段（端点在 g_pv 中的下标区间） */
typedef struct { int s, e; } Seg;
static Seg g_seg[MAXS];
static int g_nseg = 0;

/* 中枢 */
typedef struct { float zg, zd; int start_bar, end_bar; int sp, ep; } ZS;
static ZS  g_zs[MAXS];
static int g_nzs = 0;

/* 买卖点（kind: 1=一买 2=二买 3=三买 4=一卖 5=二卖 6=三卖）
   bar = 端点所在bar；cfm = 端点被确认的bar（实时锚定用） */
typedef struct { int bar; float price; int kind; int cfm; } BSP;
static BSP g_bsp[MAXP];
static int g_nbsp = 0;

/* CLOSE 序列与 MACD 构件 */
static float g_C[MAXN];
static int   g_Clen = 0;
static float g_dif[MAXN], g_dea[MAXN], g_macd[MAXN];
static float g_negpre[MAXN + 1], g_pospre[MAXN + 1];

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
            g_pv[g_npv].cfm   = b;   /* 确认bar = 触发确认的反向分型所在bar */
            g_npv++;
            ct = t; cm = m; cv = v; cb = b;
        }
        /* 间隔不够：忽略该分型，候选端点保持不变 */
    }
    /* 末尾候选端点作为未确认端点照常输出（笔/线段实时延伸用），
       但不参与买卖点信号（防漂移） */
    if (ct != 0 && g_npv < MAXP) {
        g_pv[g_npv].type  = ct;
        g_pv[g_npv].midx  = cm;
        g_pv[g_npv].price = cv;
        g_pv[g_npv].bar   = cb;
        g_pv[g_npv].cfm   = -1;
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
            g_zs[g_nzs].sp = i;                 /* 起始笔（端点下标） */
            g_zs[g_nzs].ep = j;                 /* 结束端点下标：离开笔为 j->j+1 */
            g_nzs++;
            i = j;
        } else {
            i++;
        }
    }
}

/* ---------------- 第 6 步：买卖点（MACD 背驰 + 中枢位置） ---------------- */

/* MACD(12,26,9)，基于 CLOSE；同时维护正/负柱面积前缀和 */
static void compute_macd(int n)
{
    int i;
    float e12, e26, dea;
    const float a12 = 2.0f / 13.0f, a26 = 2.0f / 27.0f, a9 = 2.0f / 10.0f;
    e12 = e26 = g_C[0];
    dea = 0.0f;
    g_negpre[0] = g_pospre[0] = 0.0f;
    for (i = 0; i < n; i++) {
        float dif, m;
        e12 += (g_C[i] - e12) * a12;
        e26 += (g_C[i] - e26) * a26;
        dif  = e12 - e26;
        dea += (dif - dea) * a9;
        m = 2.0f * (dif - dea);
        g_dif[i] = dif;
        g_dea[i] = dea;
        g_macd[i] = m;
        g_negpre[i + 1] = g_negpre[i] + (m < 0 ? -m : 0.0f);
        g_pospre[i + 1] = g_pospre[i] + (m > 0 ?  m : 0.0f);
    }
}

/* 笔 i（P[i]->P[i+1]）的力度统计：负/正柱面积、DIFF 区间极值 */
static void leg_stats(int i, float *neg_area, float *pos_area,
                      float *dif_min, float *dif_max)
{
    int b1 = g_pv[i].bar, b2 = g_pv[i + 1].bar, b;
    float mn = 1e30f, mx = -1e30f;
    if (b2 < b1) { int t = b1; b1 = b2; b2 = t; }
    *neg_area = g_negpre[b2 + 1] - g_negpre[b1 + 1];
    *pos_area = g_pospre[b2 + 1] - g_pospre[b1 + 1];
    for (b = b1 + 1; b <= b2; b++) {
        if (g_dif[b] < mn) mn = g_dif[b];
        if (g_dif[b] > mx) mx = g_dif[b];
    }
    *dif_min = mn;
    *dif_max = mx;
}

static void add_point(int pivot_idx, int kind)
{
    if (g_nbsp >= MAXP) return;
    g_bsp[g_nbsp].bar   = g_pv[pivot_idx].bar;
    g_bsp[g_nbsp].price = g_pv[pivot_idx].price;
    g_bsp[g_nbsp].kind  = kind;
    g_bsp[g_nbsp].cfm   = g_pv[pivot_idx].cfm;
    g_nbsp++;
}

static void build_points(int n)
{
    int i, z;
    int last_b1 = -1, b2_done = 1;
    int last_s1 = -1, s2_done = 1;
    g_nbsp = 0;
    if (g_Clen != n) return;                /* 无 CLOSE 则跳过买卖点与预警 */
    compute_macd(n);                        /* 预警层也依赖 MACD，先算 */
    if (g_npv < 4) return;

    /* —— 1买/2买/1卖/2卖：逐笔端点扫描（仅限已确认端点，排除末尾未确认端点） —— */
    for (i = 2; i < g_npv - 1; i++) {
        if (g_pv[i].type == -1) {                    /* 底端点 */
            int is_b1 = 0;
            if (i >= 3) {
                int zi = -1;
                for (z = g_nzs - 1; z >= 0; z--)     /* 最近已结束中枢 */
                    if (g_zs[z].ep < i) { zi = z; break; }
                if (zi >= 0 &&
                    g_pv[i].price < g_zs[zi].zd &&          /* 跌破中枢下沿 */
                    g_pv[i].price < g_pv[i - 2].price) {    /* 相对前底创新低 */
                    float na1, pa1, mn1, mx1, na0, pa0, mn0, mx0;
                    leg_stats(i - 1, &na1, &pa1, &mn1, &mx1);   /* 当前下跌笔 */
                    leg_stats(i - 3, &na0, &pa0, &mn0, &mx0);   /* 前一下跌笔 */
                    if (na1 < na0 && mn1 > mn0)                 /* 面积减小 + DIF抬高 */
                        is_b1 = 1;
                }
            }
            if (is_b1) {
                add_point(i, 1);
                last_b1 = i; b2_done = 0;
            } else if (last_b1 >= 0 && !b2_done) {
                if (g_pv[i].price > g_pv[last_b1].price) {      /* 不破一买低点 */
                    add_point(i, 2);
                    b2_done = 1;
                } else {
                    last_b1 = -1;                               /* 一买失效 */
                }
            }
        } else {                                     /* 顶端点（镜像） */
            int is_s1 = 0;
            if (i >= 3) {
                int zi = -1;
                for (z = g_nzs - 1; z >= 0; z--)
                    if (g_zs[z].ep < i) { zi = z; break; }
                if (zi >= 0 &&
                    g_pv[i].price > g_zs[zi].zg &&          /* 升破中枢上沿 */
                    g_pv[i].price > g_pv[i - 2].price) {    /* 相对前顶创新高 */
                    float na1, pa1, mn1, mx1, na0, pa0, mn0, mx0;
                    leg_stats(i - 1, &na1, &pa1, &mn1, &mx1);   /* 当前上升笔 */
                    leg_stats(i - 3, &na0, &pa0, &mn0, &mx0);   /* 前一上升笔 */
                    if (pa1 < pa0 && mx1 < mx0)                 /* 面积减小 + DIF压低 */
                        is_s1 = 1;
                }
            }
            if (is_s1) {
                add_point(i, 4);
                last_s1 = i; s2_done = 0;
            } else if (last_s1 >= 0 && !s2_done) {
                if (g_pv[i].price < g_pv[last_s1].price) {      /* 不过一卖高点 */
                    add_point(i, 5);
                    s2_done = 1;
                } else {
                    last_s1 = -1;
                }
            }
        }
    }

    /* —— 3买/3卖：逐中枢扫描（回抽端点须已确认：m < g_npv-1） —— */
    for (z = 0; z < g_nzs; z++) {
        int k, m;
        float zg = g_zs[z].zg, zd = g_zs[z].zd;
        /* 3买：中枢结束后向上离开（顶> ZG），第一次回抽笔底仍 > ZG */
        for (k = g_zs[z].ep + 1; k < g_npv; k++)
            if (g_pv[k].type == 1 && g_pv[k].price > zg) break;
        if (k < g_npv) {
            m = k + 1;
            if (m < g_npv - 1 && g_pv[m].type == -1 && g_pv[m].price > zg)
                add_point(m, 3);
        }
        /* 3卖：向下离开（底< ZD），第一次反弹笔顶仍 < ZD */
        for (k = g_zs[z].ep + 1; k < g_npv; k++)
            if (g_pv[k].type == -1 && g_pv[k].price < zd) break;
        if (k < g_npv) {
            m = k + 1;
            if (m < g_npv - 1 && g_pv[m].type == 1 && g_pv[m].price < zd)
                add_point(m, 6);
        }
    }
}

/* ---------------- 第 7 步：预警信号（分型级，密封分型永不改写） ---------------- */
/* 密封定义：分型右侧已出现第 2 根合并K（midx <= nmk-3），
   此后该分型的值与位置在任何后续数据下都不再改变 */

static void bar_stats(int b1, int b2, float *neg_area, float *pos_area,
                      float *dif_min, float *dif_max)
{
    int b;
    float mn = 1e30f, mx = -1e30f;
    if (b2 < b1) { int t = b1; b1 = b2; b2 = t; }
    *neg_area = g_negpre[b2 + 1] - g_negpre[b1 + 1];
    *pos_area = g_pospre[b2 + 1] - g_pospre[b1 + 1];
    for (b = b1 + 1; b <= b2; b++) {
        if (g_dif[b] < mn) mn = g_dif[b];
        if (g_dif[b] > mx) mx = g_dif[b];
    }
    *dif_min = mn;
    *dif_max = mx;
}

static void add_alert(int fx_idx, int kind)
{
    int m;
    if (g_nbsp >= MAXP) return;
    g_bsp[g_nbsp].bar   = g_fx[fx_idx].bar;
    g_bsp[g_nbsp].price = g_fx[fx_idx].val;
    g_bsp[g_nbsp].kind  = kind;                 /* 7=预1买 8=预2买 9=预3买 10=预1卖 11=预2卖 12=预3卖 */
    m = g_fx[fx_idx].midx;
    /* 预警确认bar = 使分型密封的那根合并K的起始原始bar（信号最早可知时刻） */
    g_bsp[g_nbsp].cfm   = g_mk[m + 2].start;
    g_nbsp++;
}

static void build_alerts(int n)
{
    int j, z;
    int last_pb1 = -1, pb2_done = 1;
    int last_ps1 = -1, ps2_done = 1;
    if (g_Clen != n || g_nfx < 4) return;    /* 依赖 build_points 已算好 MACD */

    /* —— 预1买/预2买/预1卖/预2卖：逐密封分型扫描 —— */
    for (j = 1; j < g_nfx; j++) {
        if (g_fx[j].midx > g_nmk - 3) break;     /* 未密封（midx 随 j 递增） */
        if (g_fx[j].type == -1) {                /* 底分型 */
            int is_pb1 = 0;
            if (j >= 3) {
                float na1, pa1, mn1, mx1, na0, pa0, mn0, mx0;
                bar_stats(g_fx[j-1].bar, g_fx[j].bar,   &na1, &pa1, &mn1, &mx1);
                bar_stats(g_fx[j-3].bar, g_fx[j-2].bar, &na0, &pa0, &mn0, &mx0);
                if (g_fx[j].val < g_fx[j-2].val &&      /* 相对前底分型创新低 */
                    na1 < na0 && mn1 > mn0)             /* 面积减小 + DIF抬高 */
                    is_pb1 = 1;
            }
            if (is_pb1) {
                add_alert(j, 7);
                last_pb1 = j; pb2_done = 0;
            } else if (last_pb1 >= 0 && !pb2_done) {
                if (g_fx[j].val > g_fx[last_pb1].val) {
                    add_alert(j, 8);
                    pb2_done = 1;
                } else {
                    last_pb1 = -1;
                }
            }
        } else {                                 /* 顶分型（镜像） */
            int is_ps1 = 0;
            if (j >= 3) {
                float na1, pa1, mn1, mx1, na0, pa0, mn0, mx0;
                bar_stats(g_fx[j-1].bar, g_fx[j].bar,   &na1, &pa1, &mn1, &mx1);
                bar_stats(g_fx[j-3].bar, g_fx[j-2].bar, &na0, &pa0, &mn0, &mx0);
                if (g_fx[j].val > g_fx[j-2].val &&      /* 相对前顶分型创新高 */
                    pa1 < pa0 && mx1 < mx0)
                    is_ps1 = 1;
            }
            if (is_ps1) {
                add_alert(j, 10);
                last_ps1 = j; ps2_done = 0;
            } else if (last_ps1 >= 0 && !ps2_done) {
                if (g_fx[j].val < g_fx[last_ps1].val) {
                    add_alert(j, 11);
                    ps2_done = 1;
                } else {
                    last_ps1 = -1;
                }
            }
        }
    }

    /* —— 预3买/预3卖：中枢形成（第4个端点已确认）后，突破+回抽守住 —— */
    for (z = 0; z < g_nzs; z++) {
        int fbar, jj, k;
        float zg = g_zs[z].zg, zd = g_zs[z].zd;
        if (g_zs[z].sp + 3 > g_npv - 2) continue;      /* 形成端点未确认 */
        fbar = g_pv[g_zs[z].sp + 3].bar;               /* 中枢形成bar（固定） */
        k = -1;
        for (jj = 0; jj < g_nfx; jj++) {               /* 预3买 */
            if (g_fx[jj].midx > g_nmk - 3) break;
            if (g_fx[jj].bar <= fbar) continue;
            if (k < 0) {
                if (g_fx[jj].type == 1 && g_fx[jj].val > zg) k = jj;
            } else if (g_fx[jj].type == -1) {
                if (g_fx[jj].val > zg) add_alert(jj, 9);
                break;                               /* 首个回抽底分型即判定 */
            }
        }
        k = -1;
        for (jj = 0; jj < g_nfx; jj++) {               /* 预3卖（镜像） */
            if (g_fx[jj].midx > g_nmk - 3) break;
            if (g_fx[jj].bar <= fbar) continue;
            if (k < 0) {
                if (g_fx[jj].type == -1 && g_fx[jj].val < zd) k = jj;
            } else if (g_fx[jj].type == 1) {
                if (g_fx[jj].val < zd) add_alert(jj, 12);
                break;
            }
        }
    }
}
static void compute_all(int n)
{
    build_merged(n);
    build_fractals();
    build_pivots();
    build_segments();
    build_zhongshu();
    build_points(n);
    build_alerts(n);    /* 预警层：依赖 MACD（build_points 内已算）与分型/中枢 */
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

/* ---------------- 功能号 3：存 CLOSE（供 MACD 背驰判断） ---------------- */
void __cdecl KFM_SetC(int DataLen, float *pfOUT,
                      float *pfINa, float *pfINb, float *pfINc)
{
    int i;
    (void)pfINb; (void)pfINc;
    if (!pfOUT || !pfINa || DataLen <= 0) return;
    if (DataLen > MAXN) DataLen = MAXN;
    for (i = 0; i < DataLen; i++) g_C[i] = pfINa[i];
    sanitize(g_C, DataLen);
    g_Clen = DataLen;
    for (i = 0; i < DataLen; i++) pfOUT[i] = g_C[i];
}

/* ---------------- 功能号 2：存 LOW + 计算 + 选择输出 ---------------- */
void __cdecl KFM_Out(int DataLen, float *pfOUT,
                     float *pfINa, float *pfINb, float *pfINc)
{
    int i, sel, debug, anchor;
    (void)debug;
    if (!pfOUT || !pfINa || DataLen <= 0) return;
    if (DataLen > MAXN) DataLen = MAXN;

    sel    = (pfINb && pfINb[0] == pfINb[0]) ? (int)(pfINb[0] + 0.5f) : 0;
    debug  = (pfINc && pfINc[0] == 777.0f) ? 1 : 0;
    /* 买卖点锚定：0=端点bar（默认，价格位置准确）；1=确认bar（信号出现时的新K线，不补课） */
    anchor = (pfINc && pfINc[0] == 1.0f) ? 1 : 0;

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
            for (b = g_zs[z].start_bar; b <= eb; b++) {
                /* 共享边界 bar：先写者（前一个中枢）保留，
                   防止旧版公式读连续序列画边时跨中枢粘连 */
                if (pfOUT[b] == 0.0f)
                    pfOUT[b] = (sel == 5) ? g_zs[z].zg : g_zs[z].zd;
            }
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
    case 16:  /* ZGS 中枢起点标记（起点bar输出ZG，供第二版矩形画法） */
    case 17:  /* ZGE 中枢终点标记（终点bar输出ZG） */
    case 18:  /* ZDS 中枢起点标记（起点bar输出ZD） */
    case 19: {/* ZDE 中枢终点标记（终点bar输出ZD） */
        int z;
        for (z = 0; z < g_nzs; z++) {
            int b;
            if (sel == 16 || sel == 18) {
                b = g_zs[z].start_bar;
                /* 与上一中枢终点同bar时右移1格，保证起/终标记严格交替 */
                if (z > 0 && b == g_zs[z-1].end_bar && b < g_zs[z].end_bar)
                    b++;
            } else {
                b = g_zs[z].end_bar;
            }
            if (b >= DataLen) b = DataLen - 1;
            if (b >= 0)
                pfOUT[b] = (sel == 16 || sel == 17) ? g_zs[z].zg : g_zs[z].zd;
        }
        break;
    }
    case 20:  /* B1 一买 */
    case 21:  /* B2 二买 */
    case 22:  /* B3 三买 */
    case 23:  /* S1 一卖 */
    case 24:  /* S2 二卖 */
    case 25:  /* S3 三卖 */
    case 26:  /* PB1 预警一买 */
    case 27:  /* PB2 预警二买 */
    case 28:  /* PB3 预警三买 */
    case 29:  /* PS1 预警一卖 */
    case 30:  /* PS2 预警二卖 */
    case 31: {/* PS3 预警三卖 */
        int want = sel - 19;    /* 20-25 -> kind 1..6（确认层），26-31 -> kind 7..12（预警层） */
        int q;
        for (q = 0; q < g_nbsp; q++) {
            if (g_bsp[q].kind != want) continue;
            /* 锚定：1 且确认bar有效时标在确认bar，否则标在端点bar */
            i = (anchor && g_bsp[q].cfm >= 0) ? g_bsp[q].cfm : g_bsp[q].bar;
            if (i >= 0 && i < DataLen)
                pfOUT[i] = g_bsp[q].price;
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
    { 3, (pPluginFUNC)&KFM_SetC },
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
