// ============================================================
// ChartCatch.cpp — CATCH（无轨道下落雨）独立引擎 + 渲染
//
//   CATCH 是"无轨"玩法：满屏"雨"沿连续横向坐标 x∈[0,1] 下落，底部接盘是
//   一个连续滑动的板，**横坐标由鼠标左右移动控制**（没有键位分区 / 没有轨道 /
//   不绑定具体键）。雨点落到判定线的瞬间，只要接盘盖住它的落点就**自动接住**
//   （"一键自动接"，无需任何按键）；正中 MARVELOUS、靠边 GOOD。没盖住就是 MISS，
//   MISS 只是照常显示一条判定（和 PERFECT / GOOD 一样），不会自动返回 / 重开本关。
//   谱面算法把冰火砖的转向 / 幅度映射成连续横向坐标：持续同向旋转渲染成
//   "长阶梯"（该连的不散），大回转才是折返大跳。
//   本文件与 4K/5K/6K/10K/16K 引擎完全不共用代码，皮肤也独立。
//
//   皮肤：内置 Malody 原皮 Dylamo（note / plate / line / hit / judge / numbers）。
//   搜索顺序：<DLL>\CatchSkin → <DLL>\skin\Dylamo → <GameDir>\skin\Dylamo；
//   绝不硬编码盘符/绝对路径；找不到则退回程序化圆点 / 矩形，绝不借用轨道皮肤。
// ============================================================
#include "Chart4K.h"
#include "ChartCore.h"
#include "CheatState.h"
#include "GameBridge.h"
#include "GameDir.h"
#include "RenderHook.h"
#include "StreamMode.h"
#include "Lang.h"
#include "Log.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <algorithm>

void* RenderHook_LoadTexture(const char* path);   // RenderHook.cpp（D3D 纹理上传）

namespace Chart4K
{

// ---------------- CATCH 谱面生成（独立算法，多种手法） ----------------
//   把冰火砖的转向 / 幅度映射成"无轨横向坐标 x∈[0,1]"。手法取自 osu!catch 的移动语汇
//   （walk / dash / hyperdash / edge dash / stair —— 见 osu! wiki: Gameplay/Walk, Dash,
//   Hyperdash, Edge_dash），与 4K 的"叠/技/乱/切"（键位手法）没有任何关系：
//     · 0 经典 Classic：跟随旋转角（连续同向旋转 → 长阶梯；大回转 → 折返大跳）；
//     · 1 走   Walk   ：相邻吸附区之间的小步走位，几乎不需要跨区；
//     · 2 冲   Dash   ：保持"冲刺距离"，每 1~2 音换向，接盘要来回跨区；
//     · 3 超冲 Hyper  ：大跨度滑行，同向连走 2~4 音再回摆（半屏扫描）；
//     · 4 边冲 Edge   ：落点贴左右边缘来回拉满（edge dash / pixel dash 极限接法）；
//     · 5 阶梯 Stair  ：单调同向小步，只在边界折返 —— 视觉上是一条连续长阶梯。
//   长按（hold）一律原地不动（雨拉成竖线）；边界反弹，接盘不用瞬移。
void GenerateCatch(const PadEv* ev, int n, int style, int level,
                   int* outZone, float* outX, int* outHoldIdx)
{
    if (!outZone || !outX || n <= 0) return;
    const int Z = 8;
    if (style < 0 || style > 5) style = 0;
    float x = 0.5f;
    int   dir = 1;
    int   stairLeft = 0;      // 经典：当前同向阶梯剩余步数
    int   streak = 0;         // 走 / 冲 / 超冲：当前同向剩余步数
    uint32_t rng = 0x9E3779B9u ^ (uint32_t)((unsigned)style * 2654435761u)
                 ^ (uint32_t)((unsigned)level * 40503u + 12345u);
    auto rnd01 = [&rng]() { rng = rng * 1664525u + 1013904223u; return (float)(rng >> 8) / 16777216.0f; };
    const float kStair = 0.115f;   // 长阶梯步长（≈ 1 个吸附区 / 音）

    for (int i = 0; i < n; i++)
    {
        const PadEv& e  = ev[i];
        const float  tu = (float)e.turn;                          // 度
        const bool   fast = (e.dt > 0.0) && (e.dt <= (double)kFastDt);
        float step = 0.0f;

        if (e.hold > 0.f)
        {
            step = 0.0f;                                          // 长按：原地
            stairLeft = 0;
            streak = 0;
        }
        else if (style == 1)                                      // 走 Walk
        {
            if (tu >= 8.f && e.dir != 0) dir = e.dir;
            step = 0.045f * (float)dir;                           // 相邻区之间的小步
        }
        else if (style == 2)                                      // 冲 Dash
        {
            if (streak <= 0)
            {
                if (tu >= 8.f && e.dir != 0) dir = e.dir;
                else                         dir = -dir;
                streak = 1 + (int)(rnd01() * 2.f);
            }
            step = (0.150f + 0.05f * rnd01()) * (float)dir;        // 保持冲刺距离
            streak--;
        }
        else if (style == 3)                                      // 超冲 Hyper
        {
            if (streak <= 0)
            {
                if (tu >= 8.f && e.dir != 0) dir = e.dir;
                else                         dir = -dir;
                streak = 2 + (int)(rnd01() * 3.f);
            }
            step = (0.300f + 0.08f * rnd01()) * (float)dir;        // 大跨度滑行
            streak--;
        }
        else if (style == 4)                                      // 边冲 Edge
        {
            const float tgt = (dir > 0) ? 0.06f : 0.94f;           // 目标：左右边缘
            step = (tgt - x) * 0.55f;
            if (fabsf(step) < 0.004f) dir = -dir;                  // 到边了换向
            streak = 0;
        }
        else if (style == 5)                                      // 阶梯 Stair
        {
            if (tu >= 8.f && e.dir != 0 && rnd01() < 0.30f) dir = e.dir;
            step = 0.085f * (float)dir;                            // 单调同向小步
            streak = 0;
        }
        else if (fast)                                             // 0 经典
        {
            if (stairLeft <= 0)
            {
                stairLeft = 5 + ((int)(tu / 45.0f) % 7);          // 5..11 步
                if (tu > 6.0f && e.dir != 0) dir = e.dir;         // 跟随旋转方向
            }
            step = kStair * (float)dir;
            stairLeft--;
        }
        else                                                       // 0 经典：慢速块按转角分档
        {
            float mag;
            if      (tu < 8.f)   mag = 0.05f;
            else if (tu < 60.f)  mag = 0.11f;
            else if (tu < 150.f) mag = 0.19f;
            else if (tu < 250.f) mag = 0.27f;
            else                 mag = 0.36f;                     // 折返大跳
            if (tu >= 150.f)          dir = (e.dir >= 0) ? 1 : -1;
            else if (tu >= 8.f)       dir = (e.dir >= 0) ? 1 : -1;
            step = mag * (float)dir;
        }

        x += step;
        if (x < 0.03f) { x = 0.03f + (0.03f - x); dir = 1; }      // 边界反弹
        if (x > 0.97f) { x = 0.97f - (x - 0.97f); dir = -1; }
        if (x < 0.02f) x = 0.02f;
        if (x > 0.98f) x = 0.98f;

        outX[i] = x;
        int z = (int)(x * (float)Z);
        if (z < 0) z = 0;
        if (z > Z - 1) z = Z - 1;
        outZone[i] = z;
        if (outHoldIdx) outHoldIdx[i] = (e.hold > 0.f) ? 1 : 0;
    }
    Log::Printf("[CATCH] gen: style=%d level=%d notes=%d", style, level, n);
}

const char* ModeDescCatch()
{
    return "CATCH（落物）：一键自动接 —— 无需按键，用鼠标左右移动底部接盘；"
           "雨点落到判定线的瞬间被接盘盖住即自动接住（正中 MARVELOUS / 靠边 GOOD）。"
           "没盖住判 MISS，只显示判定、不会自动重开；可在「手法」里换 走 / 冲 / 超冲 / 边冲 / 阶梯。";
}

// ---------------- 内置皮肤（Malody 原皮 Dylamo） ----------------
namespace
{
// 一张带 alpha 包围盒裁剪信息的贴图（Malody 贴图常带大量透明留白）
struct CatchTex
{
    void* tex = nullptr;
    float u0 = 0.f, v0 = 0.f, u1 = 1.f, v1 = 1.f;
    bool  ok() const { return tex != nullptr; }
};
struct CatchSkin
{
    CatchTex note, plate, line;
    CatchTex hit[24], judge[4], num[12];
    bool  loaded = false;
    bool  tried  = false;
    char  dir[MAX_PATH * 2] = { 0 };
};
static CatchSkin s_ct;

static bool FileExistsA(const char* p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool TryCatchDir(const char* d, char* out, size_t n)
{
    if (!d || !d[0]) return false;
    char probe[MAX_PATH * 2 + 64];
    snprintf(probe, sizeof(probe), "%s\\Dylamo-note.png", d);
    if (!FileExistsA(probe)) return false;
    snprintf(out, n, "%s", d);
    return true;
}
static const char* CatchSkinDir()
{
    if (s_ct.dir[0]) return s_ct.dir;
    char d[MAX_PATH * 2] = { 0 };
    // 1) 内置 CATCH 皮肤：DLL（Injector.exe）近邻 或 当前工作目录下的 CatchSkin 文件夹。
    //    绝不硬编码盘符 / 用户名 / 绝对路径 —— 换台电脑把 CatchSkin 放到 DLL 旁边即可。
    if (ResolveSidecarDir("CatchSkin", nullptr, d, (int)sizeof(d)))
    {
        if (TryCatchDir(d, s_ct.dir, sizeof(s_ct.dir))) return s_ct.dir;
        char sub[MAX_PATH * 2];
        snprintf(sub, sizeof(sub), "%s\\Dylamo", d);
        if (TryCatchDir(sub, s_ct.dir, sizeof(s_ct.dir))) return s_ct.dir;
    }
    // 2) 旧发布包布局：DLL 近邻 skin\Dylamo
    if (ResolveSidecarDir("skin", nullptr, d, (int)sizeof(d)))
    {
        char sub[MAX_PATH * 2];
        snprintf(sub, sizeof(sub), "%s\\Dylamo", d);
        if (TryCatchDir(sub, s_ct.dir, sizeof(s_ct.dir))) return s_ct.dir;
    }
    // 3) 游戏目录（MalodyV）下的皮肤
    const char* gd = GameDir::Get();
    if (gd && gd[0])
    {
        snprintf(d, sizeof(d), "%s\\skin\\Dylamo", gd);
        if (TryCatchDir(d, s_ct.dir, sizeof(s_ct.dir))) return s_ct.dir;
        snprintf(d, sizeof(d), "%s\\Dylamo", gd);
        if (TryCatchDir(d, s_ct.dir, sizeof(s_ct.dir))) return s_ct.dir;
        snprintf(d, sizeof(d), "%s\\CatchSkin", gd);
        if (TryCatchDir(d, s_ct.dir, sizeof(s_ct.dir))) return s_ct.dir;
    }
    snprintf(d, sizeof(d), "%sskin\\Dylamo", I18N::Prefs::Dir());
    if (TryCatchDir(d, s_ct.dir, sizeof(s_ct.dir))) return s_ct.dir;
    return s_ct.dir;
}
static CatchTex CTex(const char* file)
{
    CatchTex t;
    const char* d = CatchSkinDir();
    if (!d || !d[0]) return t;
    char p[MAX_PATH * 2 + 96];
    snprintf(p, sizeof(p), "%s\\%s", d, file);
    int w = 0, h = 0, bx = 0, by = 0, bw = 0, bh = 0;
    if (RenderHook_ImageInfo(p, &w, &h, &bx, &by, &bw, &bh) && w > 0 && h > 0 && bw > 0 && bh > 0)
    {
        t.u0 = (float)bx / (float)w;
        t.v0 = (float)by / (float)h;
        t.u1 = (float)(bx + bw) / (float)w;
        t.v1 = (float)(by + bh) / (float)h;
    }
    t.tex = RenderHook_LoadTexture(p);
    return t;
}
static void LoadCatchSkinOnce()
{
    // 不放弃：找不到时每 3s 再试一次（皮肤文件夹晚到也能自动加载）。
    static DWORD s_nextTry = 0;
    if (s_ct.loaded) return;
    const DWORD tn = GetTickCount();
    if (tn < s_nextTry) return;
    s_nextTry = tn + 3000;
    const char* d = CatchSkinDir();
    if (!d[0])
    {
        if (!s_ct.tried)
        {
            s_ct.tried = true;
            Log::Printf("[CATCH] Dylamo skin not found; procedural fallback (retry every 3s)");
        }
        return;
    }
    s_ct.note  = CTex("Dylamo-note.png");
    s_ct.plate = CTex("Dylamo-plate.png");
    s_ct.line  = CTex("Dylamo-line.png");
    for (int i = 0; i < 24; i++) { char f[32]; snprintf(f, sizeof(f), "Dylamo-hit-%d.png", i);   s_ct.hit[i] = CTex(f); }
    for (int i = 0; i < 4;  i++) { char f[32]; snprintf(f, sizeof(f), "Dylamo-judge-%d.png", i); s_ct.judge[i] = CTex(f); }
    for (int i = 0; i < 12; i++) { char f[32]; snprintf(f, sizeof(f), "numbers_%d.png", i);      s_ct.num[i] = CTex(f); }
    s_ct.loaded = true;
    Log::Printf("[CATCH] skin dir='%s' note=%p plate=%p line=%p", d, s_ct.note.tex, s_ct.plate.tex, s_ct.line.tex);
}
static inline ImTextureRef CTRef(void* t) { return ImTextureRef((ImTextureID)(uintptr_t)t); }
} // namespace

// ============================================================
// 渲染：满屏无轨道下落雨 + 底部接盘 + HUD
//   · 只在关卡进行中绘制（菜单 / 暂停 / 结算画面不覆盖 → 不再挡金币任务界面）
//   · 雨：x∈[0,1] 连续坐标映射屏宽；长按拉成竖条（该连的不散）
//   · 接盘：按住 8 个吸附键之一 → 接盘平滑滑到对应区间中心
// ============================================================
static void DrawCTex(ImDrawList* dl, const CatchTex& t, ImVec2 p0, ImVec2 p1, ImU32 col)
{
    if (!t.tex) return;
    if (t.u0 == 0.f && t.v0 == 0.f && t.u1 == 1.f && t.v1 == 1.f)
    { dl->AddImage(CTRef(t.tex), p0, p1, ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), col); return; }
    dl->AddImage(CTRef(t.tex), p0, p1, ImVec2(t.u0, t.v0), ImVec2(t.u1, t.v1), col);
}

// 用 Dylamo numbers_*.png 画数字（缺失时回退到普通文本）
static void DrawCatchNumber(ImDrawList* dl, float x, float y, float hgt, int value, ImU32 col)
{
    if (value < 0) value = 0;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    const int n = (int)strlen(buf);
    bool all = (n > 0 && n <= 12);
    for (int i = 0; i < n && all; i++)
        if (!s_ct.num[buf[i] - '0'].tex) all = false;
    if (!all)
    {
        ImGui::PushFont(nullptr, hgt);
        DrawTextShadow(dl, ImVec2(x, y), col, buf);
        ImGui::PopFont();
        return;
    }
    float wsum = 0.f;
    float wd[16];
    for (int i = 0; i < n; i++)
    {
        const CatchTex& t = s_ct.num[buf[i] - '0'];
        const float asp = (t.u1 - t.u0) > 0.f && (t.v1 - t.v0) > 0.f
                              ? ((t.u1 - t.u0) / (t.v1 - t.v0))
                              : 1.f;
        wd[i] = hgt * asp;
        wsum += wd[i];
    }
    float cx = x;
    for (int i = 0; i < n; i++)
    {
        const CatchTex& t = s_ct.num[buf[i] - '0'];
        DrawCTex(dl, t, ImVec2(cx, y), ImVec2(cx + wd[i], y + hgt), col);
        cx += wd[i];
    }
}

void DrawCatchOverlay()
{
    CheatState::Status st;
    GameBridge::GetStatusSnapshot(&st);
    const bool inPlay = st.bridgeReady && st.controllerAlive && st.gameworld && !st.paused;

    // 小窗（游戏原生画面）与其它模式共用同一组参数
    const bool mini = (GlobalSettingGet(23) != 0) && inPlay;
    RenderHook_SetCaptureWanted(mini);
    if (!inPlay || !HasChart())
        return;

    LoadCatchSkinOnce();

    float gw = 1280.f, gh = 720.f;
    RenderHook::GetGameWindowSize(&gw, &gh);
    static float s_szW = 0.f, s_szH = 0.f;
    if (gw < 200.f || gh < 200.f)
    {
        if (s_szW < 200.f || s_szH < 200.f) return;
        gw = s_szW; gh = s_szH;
    }
    else { s_szW = gw; s_szH = gh; }

    const float u = gh / 1080.f;
    const int   speed = std::max(1, ModeSettingGet(5, 1));
    const int   offMs = ModeSettingGet(5, 2);
    const float fallTime = 4.6f / (float)speed;
    const double clock = RenderClockOff(offMs);

    const float cx = gw * 0.5f;
    const float judgeY = gh * 0.82f;
    const float pxPerSec = judgeY / fallTime;
    const float rainW = std::max(24.f * u, gw * 0.030f);

    // ---- 雨快照（提前取，供自动/宏驱动接盘）----
    static std::vector<CatchRender> s_snap;
    if ((int)s_snap.size() < 4096) s_snap.resize(4096);
    const int nTot = CatchNotes(s_snap.data(), (int)s_snap.size(), clock - 0.5);

    // ---- 接盘：横坐标由主判定管线（DrawPlayfield → CatchTick）按鼠标 X 写入 ----
    //   CATCH v2「一键自动接」：不再有"按键吸盘"——雨点落到判定线的瞬间，只要
    //   接盘盖住它的落点就自动接住（正中 MARV / 靠边 GOOD）；没盖住判 MISS。
    const float s_catX = CatchPlateX();

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(gw, gh), ImGuiCond_Always);
    const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing |
                                ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##catch_overlay", nullptr, fl);
    StreamMode::MarkWindow("##catch_overlay", StreamMode::EL_TRACK);   // 直播模式：CATCH 覆盖层
    ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ---- 背景：轻压暗（保留游戏画面），底部接盘带 ----
    dl->AddRectFilled(ImVec2(0.f, 0.f), ImVec2(gw, gh), IM_COL32(6, 9, 14, 120));
    const float bandTop = judgeY - 46.f * u;
    dl->AddRectFilled(ImVec2(0.f, bandTop), ImVec2(gw, gh), IM_COL32(10, 14, 20, 170));
    // 无轨：不画任何分区刻度（CATCH 没有轨道/键位分区）
    // 判定线
    if (s_ct.line.tex)
    {
        const float lh = 10.f * u;
        for (float x = 0.f; x < gw; x += 96.f * u)
            DrawCTex(dl, s_ct.line, ImVec2(x, judgeY - lh * 0.5f),
                     ImVec2(x + 96.f * u, judgeY + lh * 0.5f), IM_COL32(120, 230, 255, 90));
    }
    dl->AddLine(ImVec2(0.f, judgeY), ImVec2(gw, judgeY), IM_COL32(150, 235, 255, 150), 1.6f * u);

    // ---- 接盘落点柔光（跟着接盘走，无轨、无分区）----
    {
        const float zw = gw * 0.10f;
        const float zx = s_catX * gw;
        dl->AddRectFilled(ImVec2(zx - zw, bandTop), ImVec2(zx + zw, gh),
                          IM_COL32(90, 220, 255, 26), 4.f * u);
    }

    // ---- 雨 ----
    const int hitN = (s_ct.hit[0].tex) ? 24 : 0;
    for (int i = 0; i < nTot; i++)
    {
        const CatchRender& nr = s_snap[i];
        const double dtc = nr.t - clock;
        if (dtc > (double)fallTime + 0.05) break;      // 时间有序：后面的更远
        if (dtc < -0.30) continue;
        if (nr.state != 0) continue;                   // 已命中/已漏：不再画活雨
        const float nx = nr.x * gw;
        const float ny = judgeY - (float)dtc * pxPerSec;
        const ImU32 col = IM_COL32(255, 255, 255, 235);
        if (s_ct.note.tex)
        {
            const float hw = rainW, hh = rainW;
            DrawCTex(dl, s_ct.note, ImVec2(nx - hw * 0.5f, ny - hh * 0.5f),
                     ImVec2(nx + hw * 0.5f, ny + hh * 0.5f), col);
        }
        else
        {
            dl->AddCircleFilled(ImVec2(nx, ny), rainW * 0.42f, IM_COL32(255, 120, 140, 240), 24);
            dl->AddCircle(ImVec2(nx, ny), rainW * 0.42f, IM_COL32(255, 240, 240, 230), 24, 2.f * u);
        }
        // 长按：向上拉一条长条（该连的不散）
        if (nr.hold > 0.f)
        {
            const float hh2 = (float)nr.hold * pxPerSec;
            dl->AddRectFilled(ImVec2(nx - rainW * 0.22f, ny - hh2),
                              ImVec2(nx + rainW * 0.22f, ny),
                              IM_COL32(255, 210, 220, 150), rainW * 0.22f);
        }
    }
    (void)hitN;

    // ---- 接盘 ----
    const float catX = s_catX * gw;
    const float plateW = std::max(120.f * u, gw * 0.16f);
    const float plateH = plateW * 0.42f;
    {
        const float px0 = catX - plateW * 0.5f, px1 = catX + plateW * 0.5f;
        const float py0 = judgeY - plateH * 0.42f, py1 = judgeY + plateH * 0.58f;
        if (s_ct.plate.tex)
            DrawCTex(dl, s_ct.plate, ImVec2(px0, py0), ImVec2(px1, py1), IM_COL32(255, 255, 255, 255));
        else
        {
            dl->AddRectFilled(ImVec2(px0, py0), ImVec2(px1, py1), IM_COL32(40, 210, 230, 235), plateH * 0.4f);
            dl->AddRect(ImVec2(px0, py0), ImVec2(px1, py1), IM_COL32(200, 250, 255, 255), plateH * 0.4f, 0, 2.f * u);
        }
    }

    // ---- 打击特效 / 判定提示 ----
    const int    jk = JudgeLastKind();
    const double jt = JudgeLastTime();
    const double jage = clock - jt;
    if (inPlay && jk >= 0 && jage >= 0.0 && jage < 0.34)
    {
        const float kk = (float)(jage / 0.34);
        const float aa = 1.f - kk * kk;
        const int   alpha = (int)(255.f * aa);
        const float jx = CatchFxX() * gw;   // 无轨：打击特效落在"被接/漏掉的那滴雨"的横向落点
        const int   fr = std::min(23, (int)(kk * 24.f));
        if (hitN > 0 && s_ct.hit[fr].tex)
        {
            const float hs = plateW * (0.85f + 0.5f * kk);
            DrawCTex(dl, s_ct.hit[fr], ImVec2(jx - hs * 0.5f, judgeY - hs * 0.5f),
                     ImVec2(jx + hs * 0.5f, judgeY + hs * 0.5f), IM_COL32(255, 255, 255, alpha));
        }
        if (s_ct.judge[jk].tex && jage < 0.30)
        {
            const float jw = 210.f * u * (1.06f - 0.20f * kk);
            const float jh = jw * 0.48f;
            DrawCTex(dl, s_ct.judge[jk], ImVec2(cx - jw * 0.5f, judgeY - jh - 96.f * u),
                     ImVec2(cx + jw * 0.5f, judgeY - 96.f * u), IM_COL32(255, 255, 255, alpha));
        }
        // 快 / 慢（GOOD 与 MISS 时显示）：符号与发布给背景打歌引擎的方向严格一致
        if ((jk == 2 || jk == 3) && jage < 0.34)
        {
            const int   dir  = CatchLastDir();
            const char* dtx  = I18N::Tr(dir > 0 ? I18N::LBL_CATCH_SLOW : I18N::LBL_CATCH_FAST);
            const ImU32 dcol = (dir > 0) ? IM_COL32(255, 186, 116, alpha) : IM_COL32(132, 224, 255, alpha);
            ImGui::PushFont(nullptr, 26.f * u);
            const ImVec2 dsz = ImGui::CalcTextSize(dtx);
            DrawTextShadow(dl, ImVec2(cx - dsz.x * 0.5f, judgeY - 62.f * u), dcol, dtx);
            ImGui::PopFont();
        }
    }

    // ---- HUD：左上信息 / 右上准确率 / 中央连击 ----
    {
        const int combo = ApiJudgeCombo();
        const float acc = ApiJudgeAcc();
        ImGui::PushFont(nullptr, 20.f * u);
        char lb[160];
        snprintf(lb, sizeof(lb), "CATCH   LV %d   BPM %.0f", ApiLevel(), ApiBpm());
        DrawTextShadow(dl, ImVec2(28.f * u, 24.f * u), IM_COL32(255, 255, 255, 235), lb);
        ImGui::PopFont();
        ImGui::PushFont(nullptr, 26.f * u);
        char ab[32];
        snprintf(ab, sizeof(ab), "%.2f%%", acc * 100.0f);
        const ImVec2 asz = ImGui::CalcTextSize(ab);
        DrawTextShadow(dl, ImVec2(gw - asz.x - 30.f * u, 26.f * u), IM_COL32(150, 232, 255, 245), ab);
        ImGui::PopFont();
        if (combo >= 2)
        {
            const float nh = 46.f * u;
            const int   nd = combo;
            char        nb[16];
            snprintf(nb, sizeof(nb), "%d", nd);
            const int   nn = (int)strlen(nb);
            bool all = (nn > 0 && nn <= 12);
            for (int i = 0; i < nn && all; i++)
                if (!s_ct.num[nb[i] - '0'].tex) all = false;
            if (all) DrawCatchNumber(dl, cx - 26.f * u * nn, 60.f * u, nh, combo, IM_COL32(255, 255, 255, 240));
            else
            {
                ImGui::PushFont(nullptr, 40.f * u);
                const ImVec2 csz = ImGui::CalcTextSize(nb);
                DrawTextShadow(dl, ImVec2(cx - csz.x * 0.5f, 60.f * u), IM_COL32(255, 255, 255, 245), nb);
                ImGui::PopFont();
            }
        }
    }

    // ---- 左下角小窗（游戏原生画面）----
    if (mini)
    {
        int mw = 0, mh = 0;
        void* cap = RenderHook_GetCaptureTex(&mw, &mh);
        if (cap && mw > 0 && mh > 0)
        {
            float mx = (float)GlobalSettingGet(24) * u;
            float my = (float)GlobalSettingGet(25) * u;
            float sw = (float)GlobalSettingGet(26) * u;
            float sh = (float)GlobalSettingGet(27) * u;
            if (sw < 60.f * u) sw = 60.f * u;
            if (sh < 34.f * u) sh = 34.f * u;
            ImVec2 p0(mx, my), p1(mx + sw, my + sh);
            dl->AddRectFilled(ImVec2(p0.x - 3.f * u, p0.y - 3.f * u),
                              ImVec2(p1.x + 3.f * u, p1.y + 3.f * u), IM_COL32(0, 0, 0, 165), 7.f * u);
            dl->AddImage(ImTextureRef((ImTextureID)(uintptr_t)cap), p0, p1,
                         ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), IM_COL32(255, 255, 255, 255));
            dl->AddRect(p0, p1, IM_COL32(255, 219, 168, 230), 5.f * u, 0, 2.f * u);
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void DrawCatchSettingsPage()
{
    DrawModePage(5);
}

} // namespace Chart4K
