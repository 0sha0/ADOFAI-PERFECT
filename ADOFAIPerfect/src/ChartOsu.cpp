// ============================================================
// ChartOsu.cpp — OSU「戳泡泡」= osu! standard 手法引擎 + 内置皮肤覆盖层
//   玩法（与 PC 版 osu! 一致）：
//     · 冰火砖 → 512x384 判定场上的 HitCircle（泡泡）；
//     · 每个泡泡有一个由外向里收缩的 ApproachCircle，缩到与外圈重合时点击；
//     · 鼠标移动瞄准 + 左键 / 右键 / Z / X 点击；
//     · 判定窗按 OD 换算（osu! 官方数值，见 ModeDescOsu）；
//     · 判定结果同步回冰与火原生判定（Great/Ok/Meh → Perfect，Miss → 断连击）。
//   手法（osu! wiki: Gameplay/Jump、Gameplay/Stream）：
//     Jump   = 1/2 拍大间距（快速甩动、单点）；Stream = 1/4 拍密集小间距（同向连打）。
//   内置皮肤：完全按 osu! 默认皮肤版式程序化绘制（不读取任何外部 .msp / 皮肤目录）——
//     白圈 + 连击色填充 + 中央数字 + 收缩判定环 + 光标拖尾，配色取 osu! 默认连击色。
// ============================================================
#include "Chart4K.h"
#include "ChartCore.h"
#include "CheatState.h"
#include "GameBridge.h"
#include "RenderHook.h"
#include "StreamMode.h"
#include "Log.h"
#include "Lang.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <windows.h>
#include <cmath>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstdint>

namespace Chart4K
{
    // ---------------- osu! 默认皮肤常量（研究自 ppy/osu DefaultLegacySkin / osu-resources）----------------
    //   判定场 512x384（osu!px）；命中圈半径 = 54.4 - 4.48*CS（CS4 ≈ 36.5px）。
    static const float kOsuFieldW = 512.f, kOsuFieldH = 384.f;
    static const float kOsuCS     = 4.0f;
    static inline float OsuRadius() { return 54.4f - 4.48f * kOsuCS; }
    // osu! 默认连击色（4 色循环，取自 DefaultLegacySkin）：#FFC000 / #00CA00 / #127CFF / #F21839
    static const ImU32 kOsuCombo[4] = {
        IM_COL32(255, 192, 0, 255), IM_COL32(0, 202, 0, 255),
        IM_COL32(18, 124, 255, 255), IM_COL32(242, 24, 57, 255)
    };

    // ---------------- 手法生成 ----------------
    //   按 osu! 的写谱习惯在 512x384 判定场里铺泡泡（不是"随手乱走"）：
    //     · 以「图形组」为单位落点：直线串 / 折返 / 三角梯 / 蛇形方块 / 方块 / 五角星；
    //       组内相邻两注的间距由 unit 决定 —— 小 unit = stream（等距连打），
    //       大 unit = jump（大跨甩动），和真实谱面的读谱手感一致；
    //     · 组与组之间换位（换位距离按快慢给：快音近、慢音远）；
    //     · 组内顺序用最近邻重排，从上一组末点接进去时总走最短的一条路，不会来回横跳；
    //     · 每个落点都留场边距、彼此不重叠；长按（hold）在原地叠一注。
    //   style: 0=经典 1=跳 2=串 3=交互 4=双押 5=技术
    void GenerateOsu(const PadEv* ev, int n, int style, int level,
                     float* outX, float* outY, int* outKey, int* outHoldIdx)
    {
        if (!outX || !outY || n <= 0) return;
        if (style < 0 || style > 5) style = 0;
        const float R  = OsuRadius();                 // CS4 ≈ 36.5 osu!px
        const float mg = R + 12.f;                    // 场边留白（圆完整落在场内）
        const float minX = mg, maxX = kOsuFieldW - mg;
        const float minY = mg, maxY = kOsuFieldH - mg;

        uint32_t rng = 0x9E3779B9u ^ ((uint32_t)style * 2654435761u)
                     ^ ((uint32_t)level * 40503u + 12345u);
        auto rnd01 = [&rng]() { rng = rng * 1664525u + 1013904223u; return (float)(rng >> 8) / 16777216.0f; };
        auto rndR  = [&](float a, float b) { return a + (b - a) * rnd01(); };

        // 图形模板（局部坐标，相邻点间距 ≈ 1 格）
        struct Pat { int n; float x[8]; float y[8]; };
        static const Pat kPat[8] = {
            { 6, { 0,1,2,3,4,5 },                          { 0,0,0,0,0,0 } },            // 0 直线串
            { 6, { 0,1,2,3,4,5 },                          { 0,1,0,1,0,1 } },            // 1 折返
            { 5, { 0,1,0.5f,1.5f,1.f },                    { 0,0,0.87f,0.87f,1.74f } },  // 2 三角梯
            { 8, { 0,1,1,0,0,1,1,0 },                      { 0,0,1,1,2,2,3,3 } },        // 3 蛇形方块
            { 6, { 0,1,2,2,1,0 },                          { 0,0,0,1,1,1 } },            // 4 方块回路
            { 5, { 0,0.951f,0.588f,-0.588f,-0.951f },      { 1,0.309f,-0.809f,-0.809f,0.309f } }, // 5 五角星（大跳）
            { 3, { 0,1,2 },                                { 0,0.9f,0 } },               // 6 浅折
            { 5, { 0,1,0,1,0 },                            { 0,0,1,1,2 } },              // 7 竖折返
        };

        float px = kOsuFieldW * 0.5f, py = kOsuFieldH * 0.55f;   // 上一注落点
        float gx[8], gy[8];
        int   gn = 0, gi = 0;

        for (int i = 0; i < n; i++)
        {
            const PadEv& e = ev[i];
            if (e.hold > 0.f)                     // 长按：原地叠一注（泡泡按住）
            {
                outX[i] = px / kOsuFieldW;
                outY[i] = py / kOsuFieldH;
                if (outKey) outKey[i] = (i & 1);
                if (outHoldIdx) outHoldIdx[i] = 1;
                continue;
            }

            if (gi >= gn)                          // 该铺新的一组了
            {
                const bool fast = (e.dt > 0.0) && (e.dt <= 0.26);   // 1/4 拍级（>3.8 音/秒）
                int p;
                switch (style)
                {
                case 1:  p = (rnd01() < 0.55f) ? 5 : 4; break;                     // 跳：星形 / 方块大跨
                case 2:  p = (rnd01() < 0.62f) ? 0 : 1; break;                     // 串：直线 / 折返
                case 3:  p = (rnd01() < 0.60f) ? 1 : 7; break;                     // 交互：折返
                case 4:  p = (rnd01() < 0.50f) ? 4 : 3; break;                     // 双押：方块 / 蛇形
                case 5:  p = (int)(2.f + rnd01() * 6.f); break;                    // 技术：三角/蛇形/方块/星/浅折/竖折
                default: p = fast ? ((rnd01() < 0.5f) ? 0 : 1)                     // 经典：快→串，慢→混
                                  : (int)(rnd01() * 8.f); break;
                }
                if (p < 0 || p > 7) p = 0;
                const Pat& pat = kPat[p];
                gn = pat.n; gi = 0;
                if (gn > 8) gn = 8;

                // 组内间距（osu!px）：串 ≈ 1.4R 等距；跳 ≈ 3.4R 大甩
                float unit;
                switch (style)
                {
                case 1:  unit = R * rndR(2.9f, 3.9f); break;
                case 2:  unit = R * rndR(1.55f, 1.95f); break;
                case 3:  unit = R * rndR(1.85f, 2.40f); break;
                case 4:  unit = R * rndR(1.65f, 2.15f); break;
                case 5:  unit = R * rndR(1.60f, 3.30f); break;
                default: unit = R * (fast ? rndR(1.55f, 2.00f) : rndR(2.00f, 3.40f)); break;
                }

                // 旋转 + 缩放（超场就整体缩小）
                const float rot = rndR(0.f, 6.2831853f);
                const float cs = cosf(rot), sn = sinf(rot);
                float lx[8], ly[8];
                float mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f;
                for (int k = 0; k < gn; k++)
                {
                    const float ux = pat.x[k] * unit, uy = pat.y[k] * unit;
                    lx[k] = ux * cs - uy * sn;
                    ly[k] = ux * sn + uy * cs;
                    if (lx[k] < mnx) mnx = lx[k];
                    if (lx[k] > mxx) mxx = lx[k];
                    if (ly[k] < mny) mny = ly[k];
                    if (ly[k] > mxy) mxy = ly[k];
                }
                float fit = 1.f;
                const float bw = mxx - mnx, bh = mxy - mny;
                if (bw > (maxX - minX)) fit = (maxX - minX) / bw;
                if (bh > 1.f && bh * fit > (maxY - minY)) fit = std::min(fit, (maxY - minY) / bh);
                const float hw = bw * fit * 0.5f, hh = bh * fit * 0.5f;
                const float lcx = (mnx + mxx) * 0.5f, lcy = (mny + mxy) * 0.5f;

                // 摆位：与上一注保持一个"换位距离"（快音近、慢音远），并夹进合法范围
                const float jump = R * (fast ? rndR(2.0f, 2.8f) : rndR(2.5f, 4.3f));
                const float ja   = rndR(0.f, 6.2831853f);
                float ox = px + cosf(ja) * jump;
                float oy = py + sinf(ja) * jump;
                ox = std::max(minX + hw, std::min(maxX - hw, ox));
                oy = std::max(minY + hh, std::min(maxY - hh, oy));
                for (int k = 0; k < gn; k++)
                {
                    gx[k] = ox + (lx[k] - lcx) * fit;
                    gy[k] = oy + (ly[k] - lcy) * fit;
                }

                // 组内最近邻重排：从上一注出发，每一步都走到最近的一个落点
                for (int a = 0; a < gn; a++)
                {
                    int best = a; float bd = 1e18f;
                    for (int b2 = a; b2 < gn; b2++)
                    {
                        const float dx = gx[b2] - px, dy = gy[b2] - py;
                        const float d = dx * dx + dy * dy;
                        if (d < bd) { bd = d; best = b2; }
                    }
                    if (best != a) { std::swap(gx[a], gx[best]); std::swap(gy[a], gy[best]); }
                }
            }

            px = gx[gi]; py = gy[gi]; gi++;        // 取组内下一个落点
            outX[i] = px / kOsuFieldW;
            outY[i] = py / kOsuFieldH;
            if (outKey) outKey[i] = (i & 1);
            if (outHoldIdx) outHoldIdx[i] = 0;
        }
        Log::Printf("[OSU] gen: style=%d level=%d notes=%d", style, level, n);
    }
    const char* ModeDescOsu()
    {
        return "OSU（戳泡泡）：鼠标瞄准 + 左/右键（或 Z / X）点击泡泡，"
               "判定环缩到外圈重合时命中。判定窗按 OD 换算（300 = 80-6*OD · 100 = 140-8*OD · "
               "50 = 200-10*OD · 超窗 MISS），结果同步回冰与火原生判定。"
               "手法：经典 / 跳(Jump) / 串(Stream) / 交互 / 双押 / 技术；只用内置 osu! 皮肤。";
    }

    void DrawOsuSettingsPage() { DrawModePage(kModeOsu); }

    // ============================================================
    //  内置皮肤覆盖层（程序化绘制，osu! 默认皮肤版式）
    // ============================================================
    // 判定文字（osu! 默认皮肤用 hit300/hit100/hit50/hit0 图；这里用同配色文字）
    static const char* OsuJudgeText(int kind)
    {
        switch (kind)
        {
        case 0: return "300";
        case 1: return "100";
        case 2: return "50";
        default: return "X";
        }
    }
    static ImU32 OsuJudgeColor(int kind)
    {
        switch (kind)
        {
        case 0: return IM_COL32(102, 204, 255, 255);   // 300 淡蓝
        case 1: return IM_COL32(136, 179, 0, 255);     // 100 绿
        case 2: return IM_COL32(255, 204, 34, 255);    // 50 黄
        default: return IM_COL32(237, 28, 36, 255);    // miss 红
        }
    }

    void DrawOsuOverlay()
    {
        CheatState::Status st;
        GameBridge::GetStatusSnapshot(&st);
        const bool inPlay = st.bridgeReady && st.controllerAlive && st.gameworld && !st.paused;

        const bool mini = (GlobalSettingGet(23) != 0) && inPlay;
        RenderHook_SetCaptureWanted(mini);
        if (!inPlay || !HasChart())
            return;

        float gw = 1280.f, gh = 720.f;
        RenderHook::GetGameWindowSize(&gw, &gh);
        static float s_szW = 0.f, s_szH = 0.f;
        if (gw < 200.f || gh < 200.f)
        {
            if (s_szW < 200.f || s_szH < 200.f) return;
            gw = s_szW; gh = s_szH;
        }
        else { s_szW = gw; s_szH = gh; }

        float fx0, fy0, fw, fh;
        OsuFieldRect(gw, gh, &fx0, &fy0, &fw, &fh);
        const float sx = fw / kOsuFieldW;      // 判定场 → 屏幕像素缩放
        const float sy = fh / kOsuFieldH;
        const float u  = gh / 1080.f;
        const float R  = OsuRadius() * sx;
        const double clock = RenderClockOff(ModeSettingGet(kModeOsu, 2));
        const int   preemptMs = OsuPreemptMs();
        const double preempt  = preemptMs / 1000.0;

        static std::vector<OsuHit> s_snap;
        if ((int)s_snap.size() < 4096) s_snap.resize(4096);
        const int nTot = OsuNotes(s_snap.data(), (int)s_snap.size(), clock - preempt - 0.5);

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
        ImGui::Begin("##osu_overlay", nullptr, fl);
        StreamMode::MarkWindow("##osu_overlay", StreamMode::EL_TRACK);   // 直播模式：OSU 覆盖层
        ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // ---- 背景：判定场外按「背景压暗」轻压暗，判定场几乎透出游戏画面（原版 osu! 的 dim 观感）----
        const int dimA = OsuDimGet() * 255 / 100;
        dl->AddRectFilled(ImVec2(0.f, 0.f), ImVec2(gw, gh), IM_COL32(4, 6, 12, dimA));
        dl->AddRectFilled(ImVec2(fx0, fy0), ImVec2(fx0 + fw, fy0 + fh), IM_COL32(8, 10, 18, (int)(dimA * 0.36f)), 10.f * u);
        dl->AddRect(ImVec2(fx0, fy0), ImVec2(fx0 + fw, fy0 + fh), IM_COL32(90, 110, 150, 70), 10.f * u, 0, 1.4f * u);

        // ---- 泡泡（HitCircle + ApproachCircle + 连击数字）----
        int liveIdx = -1;
        for (int i = 0; i < nTot; i++) if (s_snap[i].state == 0) { liveIdx = i; break; }
        const int baseCombo = OsuCombo() + 1;

        // ---- 引导点（Follow Points）：与 osu! 默认皮肤一致 ----
        //   · 不画连线，只有一串**白色小圆点**（osu! 默认 followpoint 就是白点）；
        //   · 上一个泡泡命中后，圆点从它出发、沿直线**逐个亮起**滑向下一个泡泡；
        //   · MISS / 长按叠泡泡 / 间隔过大 不引导；画在泡泡之下（先画）。
        {
            const float spacing = std::max(R * 1.00f, 22.f * u);
            for (int i = 0; i + 1 < nTot; i++)
            {
                const OsuHit& a = s_snap[i];
                const OsuHit& b = s_snap[i + 1];
                if (a.state != 1) continue;                    // 只有已命中的泡泡才铺引导点
                if (b.state != 0) continue;                    // 下一个已命中 / MISS：不再铺
                if (a.hold > 0.f || b.hold > 0.f) continue;    // 长按叠泡泡：无引导
                const double gap = b.t - a.t;
                if (gap <= 0.0 || gap > 2.0) continue;
                if (b.t - clock > preempt + 0.05) break;       // b 还没进窗：后面更远（时间有序）
                const float ax = fx0 + a.x * fw, ay = fy0 + a.y * fh;
                const float dx = (fx0 + b.x * fw) - ax, dy = (fy0 + b.y * fh) - ay;
                const float dist = sqrtf(dx * dx + dy * dy);
                if (dist < spacing) continue;
                int nf = (int)(dist / spacing);
                if (nf < 1) continue;
                if (nf > 24) nf = 24;
                float prog = (float)((clock - a.t) / gap);
                if (prog < 0.f) prog = 0.f;
                if (prog > 1.f) prog = 1.f;
                const float reach = prog * (float)(nf + 1);    // 已经"流到"第几个槽位
                for (int k = 1; k <= nf; k++)
                {
                    if ((float)k > reach) break;               // 还没轮到 → 不画（逐个亮起）
                    const float fade = std::min(1.f, reach - (float)k);
                    const float fr = (float)k / (float)(nf + 1);
                    const float qx = ax + dx * fr, qy = ay + dy * fr;
                    const float rr = R * 0.18f;
                    dl->AddCircleFilled(ImVec2(qx, qy), rr, IM_COL32(255, 255, 255, (int)(140 * fade)), 20);
                    dl->AddCircleFilled(ImVec2(qx, qy), rr * 0.55f, IM_COL32(255, 255, 255, (int)(205 * fade)), 16);
                }
            }
        }

        int runCombo = 0;
        for (int i = 0; i < nTot; i++)
        {
            const OsuHit& nh = s_snap[i];
            const float cx = fx0 + nh.x * fw;
            const float cy = fy0 + nh.y * fh;
            const double dt = nh.t - clock;
            if (dt > preempt + 0.05) continue;
            if (nh.state == 1)      runCombo++;
            const int comboNum = baseCombo + (i - liveIdx >= 0 ? (i - liveIdx) : 0);
            const ImU32 col = kOsuCombo[(unsigned)(comboNum - 1) & 3u];
            const float alpha = 1.f;
            if (nh.state == 2)      // MISS：整圈变红并淡出
            {
                dl->AddCircle(ImVec2(cx, cy), R, IM_COL32(237, 28, 36, 200), 64, R * 0.22f);
                continue;
            }
            if (nh.state == 1)      // 已命中：只留一圈淡出的白环
            {
                const float a = 1.f - std::min(1.f, (float)(clock - nh.t) / 0.22f);
                dl->AddCircle(ImVec2(cx, cy), R * (1.f + 0.25f * (1.f - a)),
                              IM_COL32(255, 255, 255, (int)(200 * a)), 64, R * 0.20f * a + 1.f);
                continue;
            }
            // 未命中：连击色外圈 + 白内圈 + 中央数字
            dl->AddCircleFilled(ImVec2(cx, cy), R * 1.08f, IM_COL32(0, 0, 0, 110), 64);
            dl->AddCircleFilled(ImVec2(cx, cy), R, col, 64);
            dl->AddCircleFilled(ImVec2(cx, cy), R * 0.72f, IM_COL32(255, 255, 255, 26), 64);
            dl->AddCircle(ImVec2(cx, cy), R * 0.86f, IM_COL32(255, 255, 255, (int)(235 * alpha)), 64, R * 0.14f);
            dl->AddCircle(ImVec2(cx, cy), R, IM_COL32(255, 255, 255, (int)(150 * alpha)), 64, R * 0.06f);
            {
                char nb[8];
                snprintf(nb, sizeof(nb), "%d", comboNum);
                const float fsz = R * 1.05f;
                const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fsz, FLT_MAX, 0.f, nb);
                dl->AddText(ImGui::GetFont(), fsz, ImVec2(cx - ts.x * 0.5f + 1.f, cy - ts.y * 0.5f + 1.f),
                            IM_COL32(0, 0, 0, 150), nb);
                dl->AddText(ImGui::GetFont(), fsz, ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f),
                            IM_COL32(255, 255, 255, 245), nb);
            }
            // 判定环：从 3x 半径收到 1x（缩到与圈重合时点击；osu! 默认皮肤的比例）
            if (dt > -0.28)
            {
                float t01 = 1.f - (float)(dt / preempt);
                if (t01 < 0.f) t01 = 0.f;
                if (t01 > 1.f) t01 = 1.f;
                const float ar = R * (3.f - 2.f * t01);
                const int   aa = (int)(225 * std::min(1.f, (float)((preempt - dt) / (preempt * 0.35f))));
                dl->AddCircle(ImVec2(cx, cy), ar, IM_COL32(255, 255, 255, aa), 72, R * 0.075f);
            }
        }

        // ---- 光标（osu! 默认：白圈 + 实心点 + 拖尾）----
        //   取**系统光标**（与判定用的坐标同源）—— 用 ImGui 的 MousePos 会在游戏
        //   RawInput / 覆盖层抢消息时停在原地，出现"画的光标和实际判定点不一致"。
        {
            float scx = -1.f, scy = -1.f;
            const ImVec2 mp = RenderHook::GameCursorClientPos(&scx, &scy) ? ImVec2(scx, scy)
                                                                          : ImGui::GetIO().MousePos;
            if (mp.x >= 0.f && mp.x <= gw && mp.y >= 0.f && mp.y <= gh)
            {
                const float cr = 18.f * u;
                static ImVec2 s_trail[10] = {};
                static int    s_th = 0;
                static int    s_tn = 0;
                static double s_lastPush = 0.0;
                const double now = ImGui::GetTime();
                if (now - s_lastPush > 0.012)
                {
                    s_lastPush = now;
                    s_trail[s_th] = mp; s_th = (s_th + 1) % 10; if (s_tn < 10) s_tn++;
                }
                for (int i = 0; i < s_tn; i++)
                {
                    const int idx = (s_th - 1 - i + 20) % 10;
                    const float a = 0.30f * (1.f - (float)i / 10.f);
                    dl->AddCircleFilled(s_trail[idx], cr * (0.85f - 0.04f * i),
                                        IM_COL32(160, 220, 255, (int)(120 * a)), 24);
                }
                dl->AddCircleFilled(mp, cr * 0.42f, IM_COL32(255, 255, 255, 235), 24);
                dl->AddCircle(mp, cr, IM_COL32(255, 255, 255, 220), 32, 2.4f * u);
            }
        }

        // ---- 判定文字 + 连击 ----
        {
            const int kind = JudgeLastKind();
            const double jt = JudgeLastTime();
            if (kind >= 0 && kind < 4 && jt > -50.0)
            {
                const double age = clock - jt;
                if (age >= -0.05 && age < 0.85)
                {
                    const float a = (float)std::max(0.0, 1.0 - age / 0.85);
                    const char* jt2 = OsuJudgeText(kind);
                    const float fsz = 44.f * u * (1.0f + 0.14f * a);
                    const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fsz, FLT_MAX, 0.f, jt2);
                    const ImVec2 p(fx0 + fw * 0.5f - ts.x * 0.5f, fy0 + fh * 0.30f);
                    dl->AddText(ImGui::GetFont(), fsz, ImVec2(p.x + 2.f, p.y + 2.f), IM_COL32(0, 0, 0, (int)(170 * a)), jt2);
                    ImU32 jc = OsuJudgeColor(kind);
                    jc = (jc & 0x00FFFFFFu) | ((ImU32)(255 * a) << 24);
                    dl->AddText(ImGui::GetFont(), fsz, p, jc, jt2);
                }
            }
            const int combo = OsuCombo();
            if (combo > 2)
            {
                char cb[24];
                snprintf(cb, sizeof(cb), "%dx", combo);
                const float fsz = 30.f * u;
                const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fsz, FLT_MAX, 0.f, cb);
                const ImVec2 p(fx0 + fw - ts.x - 26.f * u, fy0 + fh - ts.y - 22.f * u);
                dl->AddText(ImGui::GetFont(), fsz, ImVec2(p.x + 1.5f, p.y + 1.5f), IM_COL32(0, 0, 0, 160), cb);
                dl->AddText(ImGui::GetFont(), fsz, p, IM_COL32(255, 255, 255, 235), cb);
            }
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }
}
