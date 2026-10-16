# 通达信 (TDX) 自定义 DLL 编写注意事项

> 版本：v1.1 ｜ 最后更新：2026-10-12
> 适用：通达信金融终端 64 位主程序（`TdxW.exe`，`pei-x86-64`）
> 依据：本项目 `JMA_DLL`（V1）与 `JMA_DLL_V2`（9 号槽）从零到跑通的全过程实测记录
> 声明：本文为技术研究文档，不构成任何投资建议。

---

## 目录

- [零、先看这十二条（血泪浓缩版）](#零先看这十二条血泪浓缩版)
- [一、开发原理：通达信是怎么调用你的 DLL 的](#一开发原理通达信是怎么调用你的-dll-的)
- [二、完整接口契约（实测确证）](#二完整接口契约实测确证)
- [三、编译与部署](#三编译与部署)
- [四、公式侧调用语法](#四公式侧调用语法)
- [五、十四个坑与解决方案（按踩坑顺序）](#五十四个坑与解决方案按踩坑顺序)
- [六、DLL 内部健壮性设计](#六dll-内部健壮性设计)
- [七、诊断工具与方法论](#七诊断工具与方法论)
- [八、标准作业流程 SOP](#八标准作业流程-sop)
- [九、最小可运行模板](#九最小可运行模板)
- [十、排错速查表](#十排错速查表)

---

## 零、先看这十二条（血泪浓缩版）

如果只看十二条，看这十二条：

| # | 铁律 | 违反后果 |
|---|---|---|
| 1 | **只导出 `RegisterTdxFunc`，绝不导出 `RegisterTdxFunc2`** | ★ 双注册 → 通达信读表崩溃 → **进个股视图即闪退**（本项目最严重的坑） |
| 2 | 结构体必须 `#pragma pack(1)`，`{ unsigned short; void* }`，**sizeof = 10** | 布局错位 → 读到垃圾指针 → 崩溃 |
| 3 | 函数签名固定 5 参数、`__cdecl`、返回 `void` | 调用约定不符 → 栈失衡 → 崩溃 |
| 4 | DLL 位数必须与主程序一致（本机 64 位） | 绑定失败："只能加载 64 位 DLL" |
| 5 | DLL 只能放 `T0002\dlls\` 目录 | 绑定失败："只能加载该目录的 DLL" |
| 6 | 公式里 `TDXDLLx` 的 **x = 绑定槽位号**（1~10），不是功能号 | 不报错但**无输出** |
| 7 | 画图必须用**冒号 `:`**，不是 `:=` | 变量算出来了，但**主图没有线** |
| 8 | 递归滤波器必须做 **NaN/Inf 净化** | 一个 NaN 永久污染 `prev`，之后全为 NaN |
| 9 | 不要在 DLL 里自定义 `DllMain` | 加载时序风险，可能被主程序特殊标志影响 |
| 10 | 输出值必须落在**价格坐标区间内** | 线被画到坐标轴外 → 看起来"没有图" |
| 11 | **"能画出来" ≠ "画得对"** | ★ 接口层全通过，算法仍可能完全违背原意（见坑 #13） |
| 12 | **递归滤波器稳定性必须扫满参数区间** | 默认参数恰好稳定 → 换个 phase/length 就指数发散（见坑 #14，V2 实证） |

---

## 一、开发原理：通达信是怎么调用你的 DLL 的

### 1.1 整体链路

```
┌─────────────────────────────────────────────────────────────┐
│  ① 用户在「公式管理器 → DLL 绑定」里选择 T0002\dlls\xxx.dll   │
│     通达信把它记录到 T0002\dlls\dlls.ini : bandN=xxx.dll      │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  ② 通达信 LoadLibrary 加载 DLL                               │
│     └─ GetProcAddress("RegisterTdxFunc")  ← ★ 唯一注册入口    │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  ③ 调用 RegisterTdxFunc(&pFun)，pFun 初值为 NULL              │
│     └─ DLL 把 pFun 指向自己的函数表 g_CalcFuncSets            │
│     └─ 返回 TRUE 表示注册成功                                 │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  ④ 通达信读函数表（按 sizeof(entry)=10 步进）                 │
│     └─ 直到遇到 {0, NULL} 终止                                │
│     └─ ★ 崩溃高发点：表被写坏 / 重复注册                      │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  ⑤ 用户公式里写 TDXDLL10(2, CLOSE, 50, 0)                     │
│     └─ 通达信按槽位 10 找到 DLL，按功能号 2 找到函数指针       │
│     └─ 调用 Fn(DataLen, pfOUT, pfINa, pfINb, pfINc)          │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  ⑥ DLL 把结果填入 pfOUT[0..DataLen-1]，返回                    │
│     └─ 通达信按公式里的冒号/等号决定是否画线                    │
└─────────────────────────────────────────────────────────────┘
```

### 1.2 关键认知

**认知 1：注册是"一次性握手"，不是"每根 K 线调用一次"。**

`RegisterTdxFunc` 在 DLL 加载时只调一次。它只负责**交出函数表地址**。如果你在这里被调了两次（因为导出了两个入口），表就会被写两次，通达信的内部结构随之损坏。

**认知 2：真正的计算函数签名是"定长 5 参数"。**

通达信不认识可变参数。它固定按 `(int, float*, float*, float*, float*)` 压栈调用。所以：

```c
void __cdecl FnName(int DataLen, float *pfOUT,
                    float *pfINa, float *pfINb, float *pfINc);
```

- `DataLen`：本次要算多少个点。日线图实测约 **420**，更长周期 **840**。
- `pfOUT`：**出参**，长度 = `DataLen`，由你填。
- `pfINa`：公式里第 1 个参数（通常是 `CLOSE` 等序列）。
- `pfINb` / `pfINc`：公式里第 2、3 个参数（常为常数）。

**认知 3：通达信不校验你的算法，也不给你报错。**

它只关心两件事：**表对不对**、**内存别越界**。算法错了、输出全 0、线没画出来——它一概沉默。所以日志探针、独立验证脚本极其重要。

**认知 4：函数表用 `{0, NULL}` 作为终止标记。**

```c
PluginTCalcFuncInfo g_CalcFuncSets[] = {
    { 1, (pPluginFUNC)&TDX_Func1 },
    { 2, (pPluginFUNC)&TDX_Func2 },
    { 0, NULL }                    /* ← 必须有，否则通达信会一直读下去 */
};
```

漏掉这一行，通达信会越界读内存，大概率崩溃。

---

## 二、完整接口契约（实测确证）

以下全部数据来自本项目**探针 DLL 的实测日志**，不是文档抄录。

### 2.1 结构体

```c
#pragma pack(push, 1)          /* ★ 必须 1 字节对齐 */

typedef void (__cdecl *pPluginFUNC)(int DataLen,
                                    float *pfOUT,
                                    float *pfINa,
                                    float *pfINb,
                                    float *pfINc);

typedef struct tagPluginTCalcFuncInfo {
    unsigned short nFuncMark;  /* 功能号 1~65535 */
    pPluginFUNC    pCallFunc;  /* 函数指针 */
} PluginTCalcFuncInfo;

#pragma pack(pop)
```

| 项目 | 实测值 | 说明 |
|---|---|---|
| `sizeof(PluginTCalcFuncInfo)` | **10** | 2（ushort）+ 8（64位指针），无填充 |
| 函数表步进 | **10 字节** | 遍历时按 10 递增 |
| 终止标记 | `{0, NULL}` | 功能号 0 即终止 |

> ⚠️ 如果忘了 `#pragma pack(1)`，编译器会按 8 字节对齐，
> `sizeof` 变成 16，指针字段偏移从 2 变成 8 → **表布局完全错位 → 崩溃**。

### 2.2 注册入口

```c
__declspec(dllexport) BOOL __cdecl RegisterTdxFunc(PluginTCalcFuncInfo **pFun)
{
    if (pFun == NULL)  return FALSE;
    if (*pFun != NULL) return FALSE;   /* 调用方自带了表，尊重它 */
    *pFun = g_CalcFuncSets;
    return TRUE;
}
```

| 项目 | 实测值 |
|---|---|
| 函数名 | `RegisterTdxFunc` |
| 调用约定 | `__cdecl` |
| 返回类型 | `BOOL`（`int`），TRUE = 成功 |
| 参数 | `PluginTCalcFuncInfo **pFun`（二级指针） |
| 初始值 | `*pFun == NULL` |
| 导出方式 | `.def` 文件显式导出最可靠 |

### 2.3 探针实测日志（原始证据）

```
=== PROBE3 loaded ===
=== RegisterTdxFunc (PROBE3) ===
    sizeof(entry)=10  table=00007FF8AD804020
  >> PF_SHIFT L=420
  >> PF_COPY  L=420  A[0]=97.1000  A[last]=68.1100  B=10.0000  C=2.0000
  >> PF_MA5   L=420
  >> PF_COPY  L=420  A[0]=97.1000  A[last]=68.1100  B=0.0000  C=0.0000
  >> PF_COPY  L=420  A[0]=97.1000  A[last]=68.1100  B=0.0000  C=0.0000
```

**从这五行日志读出的全部事实**：

1. `sizeof(entry)=10` —— 结构体布局正确。
2. `L=420` —— 日线图传 420 根 K 线。
3. `A[0]=97.1000, A[last]=68.1100` —— `pfINa` 就是价格序列，且**顺序是"最老 → 最新"**（`A[last]` 是最新价）。
4. `B=10.0000, C=2.0000` —— 公式 `(1, CLOSE, 10, 2)` 里的 10 和 2 原样传给了 `pfINb`/`pfINc`。
5. `B=0.0000, C=0.0000` —— 公式 `(1, CLOSE, 0, 0)` 里传 0 就是 0，不会给默认值。**默认值要在 DLL 里自己兜**。

> 💡 `A[last]` 是最新价这一点非常重要：**所有递归滤波器的初始化和输出对齐都依赖它**。
> `pfOUT[i]` 必须对应 `pfINa[i]`，下标方向不能反。

---

## 三、编译与部署

### 3.1 编译命令（MinGW-w64 64位）

```bash
gcc -O2 -shared -o JMA_DLL.dll JMA_DLL.c JMA_DLL.def -lm
```

| 参数 | 作用 | 备注 |
|---|---|---|
| `-O2` | 优化 | 数值计算建议开 |
| `-shared` | 生成 DLL | |
| `-o` | 输出名 | |
| `-lm` | 链接数学库 | 用了 `sqrt`/`fabs` 等必须加 |
| `-static-libgcc` | **不要加** | 本项目实测加上后反而出问题，去掉后正常 |

### 3.2 `.def` 文件（推荐，最可靠）

```
LIBRARY JMA_DLL
EXPORTS
    RegisterTdxFunc
```

> ⚠️ **绝对不能出现 `RegisterTdxFunc2`**。这是本项目的头号坑，详见 [坑 #1](#坑-1-双注册入口导致闪退★-最严重)。

对 MinGW，`.def` 比 `__declspec(dllexport)` 更可靠——它能精确控制导出名，避免 name mangling。

### 3.3 导出表自检（每次编译后必做）

```bash
objdump -p JMA_DLL.dll | grep -A 6 "Export Address Table --"
```

正确输出：

```
Export Address Table -- Ordinal Base 1
	          Ordinal  Address  Type
	[   0] +base[   1] 00002a50 Export RVA

[Ordinal/Name Pointer] Table -- Ordinal Base 1
	          Ordinal   Hint Name
	[   0] +base[   1]  0000 RegisterTdxFunc
```

**导出数量必须是 1**。如果看到 2 个或更多，立刻停下来排查。

### 3.4 部署路径与位数

| 项目 | 要求 |
|---|---|
| 路径 | `<通达信安装目录>\T0002\dlls\` —— **只能是这里** |
| 位数 | 与主程序一致。本机 `TdxW.exe` 是 `pei-x86-64` → **必须 64 位 DLL** |
| 依赖 | 若有依赖 dll，必须**一起放同目录** |
| 绑定记录 | `T0002\dlls\dlls.ini` 里 `bandN=xxx.dll` |

检查主程序位数：

```bash
file TdxW.exe        # 或
objdump -f TdxW.exe | grep architecture
```

### 3.5 部署时的文件占用问题

**症状**：`Device or resource busy` / "文件正在被使用"

**原因**：
1. 通达信正在运行，`LoadLibrary` 后 DLL 被锁定。
2. ★ 更隐蔽：`TdxW.exe` 被强杀后**会自动重启**（PID 每次不同），你以为关掉了其实没有。

**解决方案**（PowerShell，带重试最稳）：

```powershell
$src = "路径\JMA_DLL.dll"
$dst = "D:\new_tdx_mock\T0002\dlls\JMA_DLL.dll"

# 关闭所有相关进程（TdxW 会重启，所以放在循环里）
Get-Process -Name TdxW,tdxcef,TdxSP -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

# 带重试的复制
$ok = $false
for ($i = 1; $i -le 10; $i++) {
    try { Copy-Item $src $dst -Force -ErrorAction Stop; $ok = $true; break }
    catch { Start-Sleep -Seconds 2 }
}
if ($ok) { "部署成功" } else { "部署失败：文件被占用" }
```

> ⚠️ **Git Bash 陷阱**：在 Git Bash 里执行 `taskkill /F /IM TdxW.exe`，
> `/F` 会被 MSYS 路径转换机制破坏，报 `无效参数/选项 - 'F:/'`。
> **必须改用 PowerShell 工具执行进程操作**。

---

## 四、公式侧调用语法

### 4.1 基本语法

```
TDXDLL<槽位>(功能号, 参数1, 参数2, 参数3)
```

- **`<槽位>`**：1~10，必须与 `dlls.ini` 里的 `bandN` 一致。
  本机实测 `band10=JMA_DLL.dll` → 公式必须写 `TDXDLL10`。
- **`功能号`**：即 `nFuncMark`，对应你在 `g_CalcFuncSets` 里登记的数字。

### 4.2 ★ 冒号 vs 等号（最高频的"没图"原因）

| 写法 | 行为 | 用途 |
|---|---|---|
| `A1:TDXDLL10(2, CLOSE, 50, 0);` | **计算 + 画线** | 要看到的线 |
| `A1:=TDXDLL10(2, CLOSE, 50, 0);` | **只计算，不画线** | 中间变量 |

```pascal
{ 正确：中间变量用 :=，最终输出用 : }
JMAV:=TDXDLL10(2, CLOSE, 50, 0);      { 只算 }
JMA_LINE: JMAV, COLORRED, LINETHICK2;  { 画出来 }

{ 错误：全部用 := ，主图一片空白 }
JMAV:=TDXDLL10(2, CLOSE, 50, 0);
```

### 4.3 参数传递规则

```
TDXDLL10(2, CLOSE, 50, 0)
          │   │     │   └─ pfINc[0] = 0
          │   │     └───── pfINb[0] = 50
          │   └─────────── pfINa = CLOSE 序列指针
          └─────────────── 功能号 = 2
```

**关键点**：
- `pfINa` 传序列（如 `CLOSE`）→ 是一整段数组。
- `pfINb`/`pfINc` 传常数 → 实测**只有 `[0]` 有意义**，其余是填充值。
  → 所以 DLL 里读参数要用 `pfINb[0]`，别去遍历。
- 传 `0` 就是 `0`，不会有默认值。**默认值必须在 DLL 里兜**。

### 4.4 槽位号写错的后果

```
dlls.ini:  band10=JMA_DLL.dll
公式写:    TDXDLL1(2, CLOSE, 50, 0);     ← 错！
结果:      不报错、无输出、无提示
```

通达信找不到槽位 1 的 DLL，**静默失败**。这是最难查的一类问题。

---

## 五、十四个坑与解决方案（按踩坑顺序）

### 坑 #1：双注册入口导致闪退（★ 最严重）

**症状**
- 通达信能正常启动
- 绑定也成功
- **一进入个股 K 线视图就闪退**
- 提示"我正在使用 dll，注意风险"

**根因**

早期版本同时导出了两个注册入口：

```
EXPORTS
    RegisterTdxFunc
    RegisterTdxFunc2    ← 致命
```

通达信会**连续调用两次**，每次传入 `*pFun == NULL`，我们两次都返回 TRUE 并写入同一张表：

```
RegisterTdxFunc  → 写表 → 返回 TRUE
RegisterTdxFunc2 → 再写表 → 返回 TRUE
      ↓
通达信内部结构被损坏
      ↓
读函数表时崩溃（连一次指标函数都没调到）
```

**定位方法**

写一个**探针 DLL**，在注册函数和每个指标函数里写日志文件：

```c
static void logf(const char *fmt, ...) {
    FILE *f = fopen("D:\\new_tdx_mock\\T0002\\dlls\\probe_log.txt", "a");
    if (f) { /* vfprintf... */ fclose(f); }
}

__declspec(dllexport) BOOL __cdecl RegisterTdxFunc(...) {
    logf("=== RegisterTdxFunc called ===\n");
    ...
}
void __cdecl PROBE_Func1(...) {
    logf("  >> Func1 CALLED L=%d\n", DataLen);
    ...
}
```

日志显示 **`RegisterTdxFunc` 和 `RegisterTdxFunc2` 都被调用了，但任何指标函数都没被调用** → 证明崩溃点就在"读表"阶段，且与双注册直接相关。

**修复**

1. `.def` 里删掉 `RegisterTdxFunc2`。
2. 源码里也不要定义/导出它。
3. 加重复注册防护：

```c
static int g_bRegistered = 0;

__declspec(dllexport) BOOL __cdecl RegisterTdxFunc(PluginTCalcFuncInfo **pFun)
{
    if (pFun == NULL)    return FALSE;
    if (g_bRegistered)   return FALSE;   /* ★ 拒绝第二次 */
    if (*pFun != NULL)   return FALSE;
    *pFun = g_CalcFuncSets;
    g_bRegistered = 1;
    return TRUE;
}
```

**验证**

```bash
objdump -p JMA_DLL.dll | grep -c "RegisterTdxFunc2"   # 必须是 0
```

修复后探针日志变为：

```
=== RegisterTdxFunc (PROBE3) ===
    sizeof(entry)=10  table=00007FF8AD804020
  >> PF_SHIFT L=420          ← 函数被正常调用了！
  >> PF_COPY  L=420  A[0]=97.1000 ...
```

**不崩溃了。** ✅

---

### 坑 #2：结构体对齐错误

**症状**：崩溃，或读到 `0xCCCCCCCC` / `0xFFFFFFFF` 之类的垃圾指针。

**根因**：忘了 `#pragma pack(1)`，`sizeof` 从 10 变 16，指针偏移从 2 变 8。

**修复**：

```c
#pragma pack(push, 1)
typedef struct tagPluginTCalcFuncInfo {
    unsigned short nFuncMark;
    pPluginFUNC    pCallFunc;
} PluginTCalcFuncInfo;
#pragma pack(pop)
```

**验证**：注册函数里断言

```c
if (sizeof(PluginTCalcFuncInfo) != 10) return FALSE;  /* 兜底拒绝 */
```

---

### 坑 #3：调用约定不匹配

**症状**：崩溃，或参数读到垃圾值。

**根因**：函数没写 `__cdecl`，编译器按默认约定（64 位下通常是 `__fastcall`）生成代码。

**修复**：函数指针类型和实现都必须是 `__cdecl`：

```c
typedef void (__cdecl *pPluginFUNC)(int, float*, float*, float*, float*);

void __cdecl TDX_MyFunc(int DataLen, float *pfOUT,
                        float *pfINa, float *pfINb, float *pfINc) { ... }
```

> 64 位 Windows 实际只有一种调用约定，但**显式写 `__cdecl` 能避免 32 位兼容问题和编译器警告**，是必须的规范动作。

---

### 坑 #4：导出名被 name mangling

**症状**：绑定失败，通达信找不到 `RegisterTdxFunc`。

**根因**：C++ 编译或没加 `.def` 时，符号名被修饰成 `?RegisterTdxFunc@@YAHPEAPEAU...`。

**修复**：
1. 源文件用 `.c` 后缀（C 编译）。
2. 用 `.def` 文件显式导出。
3. 若必须用 C++，加 `extern "C"`。

```c
extern "C" __declspec(dllexport) BOOL __cdecl RegisterTdxFunc(...)
```

---

### 坑 #5：位数不匹配

**症状**：

```
绑定失败!只能加载 d:\new_tdx_mock\T0002\dlls目录的DLL.，
若DLL有依赖文件,则需要放在一起.64位主程序只能绑定64位DLL，导入不正确
```

**修复**：

```bash
# 确认主程序位数
objdump -f TdxW.exe | grep architecture       # 应看到 x86-64

# 确认 DLL 位数
objdump -f JMA_DLL.dll | grep architecture    # 必须一致

# 用 64 位 MinGW 编译
/d/mingw64/bin/gcc -O2 -shared -o JMA_DLL.dll ...
```

**注意**：必须是 **64 位 MinGW**（`x86_64-w64-mingw32`），32 位工具链编不出 64 位 DLL。

---

### 坑 #6：DLL 放错目录

**症状**：绑定列表里根本看不到你的 DLL。

**修复**：只能放 `<通达信>\T0002\dlls\`。**没有例外**。

---

### 坑 #7：槽位号与公式不匹配

**症状**：**不报错，但完全没有输出**。

**根因**：`dlls.ini` 里绑定在 `band10`，公式里写了 `TDXDLL1`。

**修复**：

```ini
; T0002\dlls\dlls.ini
[BAND]
band10=JMA_DLL.dll
```

→ 公式里必须写 `TDXDLL10(...)`。

**检查方法**：每次改公式前，先 `cat dlls.ini` 确认槽位。

---

### 坑 #8：用 `:=` 却指望它画线

**症状**：数值算出来了（编辑器里能看到），但**主图上没有线**。

**修复**：见 [4.2 冒号 vs 等号](#42--冒号-vs-等号最高频的没图原因)。

---

### 坑 #9：输出值超出坐标区间

**症状**：**没有图**，但 DLL 其实工作正常。

**根因**：探针版输出固定值 `100.0`，而当时股价区间是 `68.11 ~ 97.10`。
均线值 100 落在坐标轴范围外，通达信把线裁掉了。

**这是本项目最"冤"的一个坑**——探针为了让数值好辨认故意输出常数 100，结果反而导致"没图"，一度让人以为接口还有问题。

**诊断方法**（文字输出法，最有效）：

```pascal
A1:TDXDLL10(2, CLOSE, 50, 0);
DRAWTEXT(ISLASTBAR, CLOSE, STRCAT('V=', CON2STR(A1,2)));
```

- **有数字** → DLL 正常，问题在坐标范围。
- **无数字** → DLL 层问题。

**修复**：输出必须与 `pfINa` 同量级。均线类输出应落在价格区间内。

---

### 坑 #10：NaN / Inf 污染递归状态

**症状**：通达信传进来的数据**头部/尾部含未初始化值**（NaN / 1e30 之类），
一个 NaN 进入递归滤波器后，`prev` 被永久污染，**之后所有输出都是 NaN**。

压测实测：KAMA / JMA / VEL 在含 NaN 输入下，**25 个输出点里有 15 个是 NaN**。

**根因**：递归滤波器是 `y[n] = f(y[n-1], x[n])`，NaN 会沿着 `y` 一直传下去。

**修复**：入口和出口双向净化。

```c
#define BIGF 1.0e30f

static int is_valid_price(float x) {
    if (!(x == x))  return 0;      /* NaN 判定：NaN != NaN */
    if (x >  BIGF)  return 0;
    if (x < -BIGF)  return 0;
    return 1;
}

/* 入口净化：开头脏值 → 0；中间/尾部 → 前向填充最近有效值 */
static int sanitize_input(const float *in, int n, float *buf) { ... }

/* 出口净化：任何非有限值 → 0 */
static void sanitize_output(float *out, int n) {
    for (int i = 0; i < n; i++)
        if (!is_valid_price(out[i])) out[i] = 0.0f;
}
```

> ⚠️ 注意：本项目**最终证实的闪退根因是坑 #1（双注册），不是 NaN**。
> NaN 净化是重要的健壮性加固，但它**不能解决闪退**。
> 这是两个独立问题，排查时不要混淆。

---

### 坑 #11：Git Bash 的路径转换破坏命令参数

**症状**：

```
taskkill /F /IM TdxW.exe
→ 错误: 无效参数/选项 - 'F:/'
```

**根因**：MSYS2/Git Bash 会把 `/F` 这类以 `/` 开头的参数当成 Unix 路径，
自动转换成 `F:/`（盘符路径）。

**修复**：
- 改用 **PowerShell** 执行进程操作。
- 或加 `MSYS_NO_PATHCONV=1` 前缀。
- 或用 `//F`（双斜杠转义）。

**推荐**：进程/服务操作一律走 PowerShell 工具。

---

### 坑 #12：`TdxW.exe` 被强杀后自动重启

**症状**：明明 kill 了，复制 DLL 还是报"文件被占用"。

**根因**：通达信有守护机制或用户配置了自动重启，`tasklist` 里能看到**新的 PID**。

**修复**：把"关闭 + 复制"放进重试循环。

```powershell
for ($i = 1; $i -le 10; $i++) {
    Get-Process -Name TdxW,tdxcef -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 1
    try { Copy-Item $src $dst -Force -ErrorAction Stop; break }
    catch { Start-Sleep -Seconds 2 }
}
```

### 坑 #13：算法与原意不符，但"图上看着没问题"（★ 最容易漏）

**症状**：绑定成功、图能画出来、曲线形态也像那么回事，
但**用数值一验就发现完全不是那个指标**。

**本项目实证**（`check_fidelity.py` 跑出 **4/8 失败**）：

| 指标 | 官方原意 | 实际实现 | 数值证据 |
|---|---|---|---|
| JMA | `has no overshoot or undershoot`（p.8 核心卖点） | 三处主动外推叠加 | 阶跃后**超出目标 +12.81** |
| JMA | benchmark 4：平滑度 | 参数结构失配 | 降噪仅 29%，不如同周期 EMA |
| RSX | `neutral value of 50`（中性值 50） | 把 RSI 的 50 映射成了 0 | 50/50 交替输入 → **输出 -0.53**（应为 50） |
| VEL | `NO ADDITIONAL LAG`、零交叉与原始动量重合（p.32） | 用普通 EMA 平滑 | 零交叉**平均错位 +7.4 根** |

**根因**：所有前期验证都聚焦**接口层**（能否加载 / 注册 / 调用 / 出线），
全部通过，于是产生了"已经正确"的错觉。真正的问题在**算法层** ——
输出值的分布、语义、尺度是否符合数学定义。

**修复思路（JMA 为例）**：
```
旧：ZLEMA de-lag + 相位补偿 + GAP-SMART 追赶   ← 三处外推叠加，必过冲
新：EMA 级联底座（一阶低通，数学上不可能过冲）
    + 慢偏差跟踪（补偿量恒 ≤ 当前偏差 → 是"追"不是"外推"）
    + 单调性夹逼（输出钳制在 base 与 price 之间，最后防线）
→ 过冲 0.0000
```

**必须做的检验**（缺一不可）：

| 检验类型 | 构造方法 | 验证目标 |
|---|---|---|
| **阶跃响应** | 输入从 10 跳变到 20 | 是否过冲 / 下冲 |
| **噪声响应** | 周期 ≥5 根的现实噪声 | 平滑度（★ 周期 3 根已超 Nyquist，无效） |
| **中性输入** | 50/50 交替、纯正弦 | 中性值是否正确（RSI 类应为 50） |
| **单边趋势** | 连续等幅上涨 | 是否触及上下界（应接近 100/-100） |
| **零交叉对齐** | 正弦输入，比对原始序列 | 滞后根数是否 ≈ 0 |

**方法论铁律**：
> 1. **对着原始规格文档逐条构造断言**，而不是只看图有没有画出来。
> 2. **测试脚本自己也要先过一遍**——用超因果滤波器能力的频段
>    （如周期 3 根的 `sin(i*2.1)`）测平滑度是**无效测试**，
>    纯 EMA 也会纹丝不动（260→260）。
> 3. **指标语义必须锚定官方原文**，特别是数值范围与中性值。

> 详见本项目 `指标原意符合性审计报告.md` 与 `dll_src/check_fidelity.py`。

---

### 坑 #14：递归结构凭记忆默写，反馈项写错导致发散（V2 实证）

**症状**（`JMA_DLL_V2` 首版，接口层全部正常）：

- DLL 编译、导出、注册、调用全通
- 默认参数（len=7, phase=0）下与 Python 参考实现误差仅 2e-5，**看起来一切正常**
- 但阶跃响应测试当场现形：
  - phase=-100 输出爆炸到 **8.97e29**
  - phase=0 阶跃后过冲 +29.35（10 点阶跃过冲到 49）

**根因**：JMA 三段式结构凭记忆默写，`e2` 的反馈项写错了对象：

```c
/* 错误（首版）：e2 反馈 jma_prev */
e2  = (e0 + pp*e1 - jma_prev) * (1-a)^2 + a^2 * jma_prev;
jma = jma_prev + e2;
/*   → 代入消元后 jma 递归系数 = 2*alpha，
 *     alpha>0.5（负 phase、小 length 时）即 |2α|>1，指数发散 */

/* 正确：e2 反馈 e2 自身历史值（多个公开实现交叉核对确认） */
e2  = (e0 + pp*e1 - jma_prev) * (1-a)^2 + a^2 * e2_prev;
jma = jma_prev + e2;
/*   → e2 递归系数 a^2<1 恒稳定，jma 闭环系数 1-(1-a)^2<1 恒稳定 */
```

**为什么默认参数下没暴露**：len=7/phase=0 时 alpha=0.4355，2α=0.871<1 恰好稳定，
随机游走输入上与参考实现几乎重合。**只有阶跃 + 负 phase 才把 alpha 推过 0.5 的不稳定边界。**
"单个参数点验证通过"不等于"参数全区间稳定"。

**修复与验证**：

1. 修正反馈项；同时确认标准近似中 `alpha = beta`（power=1），
   phaseRatio 只进驱动项、不进 alpha。
2. 阶跃/噪声/滞后/NaN 全套数值断言重跑：**21/21 通过**。
3. 修正后实测：phase=-100 过冲 0.002（0.02%）、全相位无振铃、
   100 万点约 7 ms。

**方法论铁律（追加）**：
> 4. **递归滤波器的稳定性断言必须扫满参数区间**（phase∈{-100,0,+100} ×
>    length∈{最小,典型,最大}），只在默认参数下对比参考实现会漏掉
>    条件稳定（conditional stability）问题。
> 5. 公式默写后先与**两个以上独立公开来源**逐符号核对，再进 DLL。
>    一行反馈项写错，接口层测试发现不了，画图也未必看得出。

> 详见本项目 `dll_src_v2/JMA_DLL_V2.c` 头部注释与 `dll_src_v2/verify_jma_v2.py`。

---

## 六、DLL 内部健壮性设计

编写指标 DLL 时，通达信**不会帮你做任何校验**。所有防御必须在 DLL 内部完成。

### 6.1 入口参数防御（每个函数都要）

```c
void __cdecl TDX_MyFunc(int DataLen, float *pfOUT,
                        float *pfINa, float *pfINb, float *pfINc)
{
    float work[MAX_WORK];

    /* 1. 指针空检查 */
    if (!pfOUT || !pfINa) return;

    /* 2. 长度检查 */
    if (DataLen <= 0) return;
    if (DataLen > MAX_WORK) DataLen = MAX_WORK;   /* ★ 防栈溢出 */

    /* 3. 输入净化 */
    if (sanitize_input(pfINa, DataLen, work) == 0) {
        for (int i = 0; i < DataLen; i++) pfOUT[i] = 0.0f;
        return;
    }

    /* 4. 参数兜底（pfINb/pfINc 可能是 NULL 或脏值）*/
    period = (int)param_or(pfINb, 10.0f);

    /* 5. 核心计算 */
    calc_impl(work, DataLen, pfOUT, period);

    /* 6. 输出净化 */
    sanitize_output(pfOUT, DataLen);
}
```

### 6.2 参数兜底函数

```c
static float param_or(float *pf, float def) {
    float v;
    if (pf == NULL) return def;      /* NULL → 默认 */
    v = pf[0];
    if (!is_valid_price(v)) return def;  /* 脏值 → 默认 */
    return v;
}
```

### 6.3 不要自定义 DllMain

**做法**：完全交给 MinGW 默认实现。

**原因**：通达信加载第三方 DLL 时可能带特殊标志（`LOAD_WITH_ALTERED_SEARCH_PATH` 等），
自定义 `DllMain` 存在时序风险；默认实现最保守，最不容易出问题。

### 6.4 避免静态链接 CRT

`-static-libgcc` 在本项目实测中反而引发问题，去掉后正常。

**原因推测**：静态链接的 CRT 初始化代码与通达信主程序的运行时有交互风险。

**建议**：让 DLL 链接系统 `msvcrt.dll`，与主程序的 MSVC 运行时（`mfc100.dll`/`MSVCR100.dll`）安全共存。

### 6.5 栈占用要克制

```c
#define MAX_WORK 16384
float work[MAX_WORK];    /* 16384 * 4 = 64KB */
```

64KB 在**默认 1MB 栈**下安全。但如果函数里开了好几个这样的数组，或多层递归，就要小心。

**建议**：`MAX_WORK` 取 16384（远超任何实际 K 线长度 420/840），且每个函数只开一个工作缓冲。

---

## 七、诊断工具与方法论

### 7.1 三层验证体系

| 层级 | 工具 | 验证什么 |
|---|---|---|
| **L1 接口层** | `verify_production_dll.py` | 加载、导出、注册、表解析 |
| **L2 数值层** | `cross_check.py` | DLL 与 Python 参考实现是否一致 |
| **L3 业务层** | 通达信实际画面 | 线是否画出来、是否符合预期 |

**L1 脚本核心逻辑**（用 ctypes 模拟通达信加载流程）：

```python
import ctypes as C

lib = C.WinDLL(DLL_PATH)                      # ① 加载

class Info(C.Structure):                      # ② 结构体
    _pack_ = 1
    _fields_ = [("nFuncMark", C.c_ushort), ("pCallFunc", C.c_void_p)]

PF = C.CFUNCTYPE(None, C.c_int,               # ③ 函数签名
                 C.POINTER(C.c_float), C.POINTER(C.c_float),
                 C.POINTER(C.c_float), C.POINTER(C.c_float))

lib.RegisterTdxFunc.argtypes = [C.POINTER(C.POINTER(Info))]
lib.RegisterTdxFunc.restype  = C.c_int

p = C.POINTER(Info)()
assert lib.RegisterTdxFunc(C.byref(p)) == 1   # ④ 注册

TABLE = {}                                    # ⑤ 遍历表
i = 0
while p[i].nFuncMark and p[i].pCallFunc:
    TABLE[p[i].nFuncMark] = PF(p[i].pCallFunc)
    i += 1
```

这个脚本能在**不启动通达信**的情况下验证 90% 的接口问题。

### 7.2 日志探针法

在关键时刻写文件日志——这是排查闪退的**唯一有效手段**，因为通达信崩溃时什么都不留。

```c
static void logf(const char *fmt, ...) {
    FILE *f = fopen("D:\\...\\dlls\\probe_log.txt", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}
```

**探针策略**：
1. 注册入口 → 记录"被调用"
2. 每个指标函数入口 → 记录 `DataLen` 和参数值
3. 通过"日志最后一行"判断崩溃点

### 7.3 二分定位法

崩溃排查的通用思路：

```
① 先做"最小 DLL"：只有 RegisterTdxFunc + 一个空函数
   → 能跑？说明接口层没问题，问题在算法层
   → 不能跑？说明接口层有问题（重点查双注册、对齐、约定）

② 接口层 OK 后，逐个加函数
   → 加到哪个崩，就是哪个函数的问题

③ 函数内部再做二分：注释掉一半逻辑，看是否还崩
```

---

## 八、标准作业流程 SOP

```
┌────────────────────────────────────────────────────────────┐
│ 步骤 1  环境勘察                                            │
│   □ 确认通达信安装路径                                      │
│   □ objdump -f TdxW.exe  确认主程序位数                     │
│   □ 确认 D:\mingw64 位数与主程序匹配                        │
├────────────────────────────────────────────────────────────┤
│ 步骤 2  写最小可运行 DLL                                    │
│   □ 结构体 #pragma pack(1)，sizeof 断言 == 10               │
│   □ 只导出 RegisterTdxFunc（.def 控制）                     │
│   □ 建函数表，末尾 {0, NULL}                                │
│   □ 先实现一个"输出 = 输入"的恒等函数，用于打通链路          │
├────────────────────────────────────────────────────────────┤
│ 步骤 3  编译 + 静态自检                                     │
│   □ gcc -O2 -shared -o xxx.dll xxx.c xxx.def -lm           │
│   □ objdump -p xxx.dll | grep "Export Address Table"        │
│     → 导出数量必须是 1                                      │
│   □ grep -c "RegisterTdxFunc2" → 必须是 0                   │
├────────────────────────────────────────────────────────────┤
│ 步骤 4  离线验证（不启动通达信）                             │
│   □ 运行 verify_production_dll.py                           │
│   □ 确认：加载 OK / 注册返回 TRUE / 表解析出 N 个功能号      │
│   □ 确认：逐个调用，输出无 NaN                              │
├────────────────────────────────────────────────────────────┤
│ 步骤 5  部署                                                 │
│   □ 完全关闭通达信（注意自动重启）                           │
│   □ 复制到 T0002\dlls\（带重试）                            │
│   □ md5 校验部署结果                                        │
├────────────────────────────────────────────────────────────┤
│ 步骤 6  绑定                                                 │
│   □ 公式管理器 → DLL 绑定 → 选择 DLL                        │
│   □ cat dlls.ini 确认 bandN 槽位号 ★                       │
├────────────────────────────────────────────────────────────┤
│ 步骤 7  最小公式验证                                         │
│   □ A1:TDXDLL10(1, CLOSE, 0, 0);   ← 注意冒号               │
│   □ 确认出线且不崩                                          │
│   □ 若没线 → 用 DRAWTEXT 文字诊断法                          │
├────────────────────────────────────────────────────────────┤
│ 步骤 8  完整功能验证                                         │
│   □ 逐个测试每个功能号                                       │
│   □ 用 DRAWTEXT 打印数值，核对量级是否在价格区间内            │
├────────────────────────────────────────────────────────────┤
│ 步骤 9  数值一致性                                          │
│   □ 若同时有 Python 实现，跑 cross_check.py                 │
│   □ 差异应在 float32 精度范围内（1e-6 ~ 1e-4）              │
└────────────────────────────────────────────────────────────┘
```

---

## 九、最小可运行模板

一个能编译、能绑定、能出线的最小 DLL：

**`mini_dll.c`**

```c
#include <windows.h>

/* ---- 结构体：必须 pack(1)，sizeof 必须为 10 ---- */
#pragma pack(push, 1)
typedef void (__cdecl *pPluginFUNC)(int DataLen, float *pfOUT,
                                    float *pfINa, float *pfINb, float *pfINc);
typedef struct tagPluginTCalcFuncInfo {
    unsigned short nFuncMark;
    pPluginFUNC    pCallFunc;
} PluginTCalcFuncInfo;
#pragma pack(pop)

/* ---- 功能1：输出 = 输入（打通链路用）---- */
void __cdecl Mini_Copy(int DataLen, float *pfOUT,
                       float *pfINa, float *pfINb, float *pfINc)
{
    int i;
    if (!pfOUT || !pfINa || DataLen <= 0) return;
    for (i = 0; i < DataLen; i++) pfOUT[i] = pfINa[i];
}

/* ---- 功能2：简单均线（参数从 pfINb[0] 取）---- */
void __cdecl Mini_MA(int DataLen, float *pfOUT,
                     float *pfINa, float *pfINb, float *pfINc)
{
    int i, j, period;
    float sum;
    if (!pfOUT || !pfINa || DataLen <= 0) return;

    period = (pfINb != NULL && pfINb[0] > 0) ? (int)pfINb[0] : 5;
    if (period > DataLen) period = DataLen;

    for (i = 0; i < DataLen; i++) {
        if (i < period - 1) { pfOUT[i] = 0.0f; continue; }
        sum = 0.0f;
        for (j = 0; j < period; j++) {
            float v = pfINa[i - j];
            if (v != v) v = 0.0f;          /* NaN 净化 */
            sum += v;
        }
        pfOUT[i] = sum / period;
    }
}

/* ---- 函数表：末尾必须 {0, NULL} ---- */
PluginTCalcFuncInfo g_CalcFuncSets[] = {
    { 1, (pPluginFUNC)&Mini_Copy },
    { 2, (pPluginFUNC)&Mini_MA   },
    { 0, NULL }
};

/* ---- 注册入口：只导出这一个 ---- */
static int g_bRegistered = 0;

__declspec(dllexport) BOOL __cdecl RegisterTdxFunc(PluginTCalcFuncInfo **pFun)
{
    if (pFun == NULL)  return FALSE;
    if (g_bRegistered) return FALSE;
    if (*pFun != NULL) return FALSE;
    if (sizeof(PluginTCalcFuncInfo) != 10) return FALSE;   /* 布局兜底 */
    *pFun = g_CalcFuncSets;
    g_bRegistered = 1;
    return TRUE;
}
```

**`mini_dll.def`**

```
LIBRARY MINI_DLL
EXPORTS
    RegisterTdxFunc
```

**编译**

```bash
gcc -O2 -shared -o MINI_DLL.dll mini_dll.c mini_dll.def -lm
```

**对应公式**

```pascal
{ 功能1：恒等，用于确认接口通了 }
A1:TDXDLL10(1, CLOSE, 0, 0);

{ 功能2：MA10 }
A2:TDXDLL10(2, CLOSE, 10, 0);
```

---

## 十、排错速查表

### 10.1 按症状查

| 症状 | 可能原因 | 排查动作 |
|---|---|---|
| 绑定列表里看不到 DLL | 放错目录 / 位数不符 | 确认在 `T0002\dlls\`；`objdump -f` 对比位数 |
| 绑定失败"导入不正确" | 位数不符 / 缺依赖 | 用 64 位工具链重编；依赖 dll 放同目录 |
| **进个股视图闪退** | ★ 双注册入口 | `grep -c RegisterTdxFunc2` 必须为 0 |
| 启动后立刻关闭 | DllMain / CRT 问题 | 删掉自定义 `DllMain`；去掉 `-static-libgcc` |
| 公式不报错但无输出 | ★ 槽位号写错 | `cat dlls.ini` 核对 `bandN` |
| 不报错但无线 | ★ 用了 `:=` | 改成 `:` |
| 有数值但无线 | 输出超出坐标区间 | 用 `DRAWTEXT` 打印数值核对 |
| 输出全是 0 | 输入净化误判 / 参数为 0 | 检查 `sanitize_input` 返回值、参数兜底 |
| 输出全是 NaN | 递归状态被污染 | 加 `sanitize_input` / `sanitize_output` |
| 线剧烈抖动 | 算法参数不当 | 检查自适应逻辑、加上限约束 |
| 复制 DLL 失败"被占用" | 通达信在运行（会重启） | 用 PowerShell 循环关+复制 |
| `taskkill` 报 `/F` 无效 | Git Bash 路径转换 | 改用 PowerShell |

### 10.2 按"崩溃时机"查

| 崩溃时机 | 大概率原因 |
|---|---|
| 通达信启动时就崩 | `DllMain` 问题 / CRT 链接问题 / 导出表损坏 |
| 绑定瞬间崩 | 注册函数签名错 / 结构体布局错 |
| **进个股视图才崩** | ★ **双注册** / 读表越界 / 函数指针错 |
| 切换到某周期才崩 | `DataLen` 变化（420→840）导致缓冲区溢出 |
| 某些股票才崩 | 输入含 NaN/特殊值 → 未净化 |
| 跑一段时间才崩 | 内存越界写 / 栈溢出 |

### 10.3 三条黄金排查命令

```bash
# ① 导出表检查（必须只有 RegisterTdxFunc）
objdump -p XXX.dll | grep -A 6 "Export Address Table --"

# ② 危险符号检查（必须为 0）
objdump -p XXX.dll | grep -c "RegisterTdxFunc2"

# ③ 位数检查（必须与 TdxW.exe 一致）
objdump -f XXX.dll | grep architecture
objdump -f TdxW.exe | grep architecture
```

---

## 附录：本项目实测结论速查

| 项目 | 实测值 |
|---|---|
| 通达信安装路径 | `D:\new_tdx_mock` |
| 主程序 | `TdxW.exe`（`pei-x86-64`，64 位） |
| DLL 目录 | `D:\new_tdx_mock\T0002\dlls\` |
| 绑定配置文件 | `T0002\dlls\dlls.ini` |
| 当前绑定槽位 | `band10=JMA_DLL.dll`、`band9=JMA_DLL_V2.dll`（V2，2026-10-12 部署） |
| 公式调用前缀 | `TDXDLL10`（V1）/ `TDXDLL9`（V2） |
| `sizeof(PluginTCalcFuncInfo)` | **10** |
| `DataLen`（日线） | **420**（更长周期 840） |
| `pfINa` | 价格序列，`A[0]` 最老，`A[last]` 最新 |
| `pfINb` / `pfINc` | 公式第 2/3 参数，**只有 `[0]` 有意义** |
| 生产版 DLL md5 | `7682a7cf1b54a3e6233d5e320aa5f6a3`（50274 字节，V1） |
| V2 DLL md5 | `ccf8676e9afeb73f698a82b744c993cb`（含功能号4 KAMA，2026-10-12 二版） |
| V2 功能号 | 1=恒等诊断 2=JMA(LENGTH,PHASE) 3=JMA方向(±1/0) 4=KAMA(ER窗口，默认10) |
| V2 离线验证 | `dll_src_v2/verify_jma_v2.py` **28/28 通过**（含 KAMA 对照实验：干净阶跃 KAMA 快 3 根 vs JMA 5 根；慢趋势+重噪声 KAMA 滞后 26 根 vs JMA 2 根） |
| 编译命令 | `gcc -O2 -shared -o JMA_DLL.dll JMA_DLL.c JMA_DLL.def -lm`（V2 同构换名） |

---

**免责声明**

本文档为通达信 DLL 插件开发的技术研究记录，所述算法实现基于公开文献与开源近似，
非任何商业软件的原始闭源算法。文中所有技术内容仅供学习与研究使用，
**不构成任何投资建议**。市场有风险，决策需谨慎。
