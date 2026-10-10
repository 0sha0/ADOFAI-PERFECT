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
    //   在 512x384 判定场里做"跟随冰火旋转角的反射游走"：
    //     heading 按本砖转向旋转（与星球同步），步长按手法 / 转角大小给。
    //   style: 0=经典 1=跳 2=串 3=交互 4=双押 5=技术
    void GenerateOsu(const PadEv* ev, int n, int style, int level,
                     float* outX, float* outY, int* outKey, int* outHoldIdx)
    {
        if (!outX || !outY || n <= 0) return;
        if (style < 0 || style > 5) style = 0;
        const float R    = OsuRadius();
        const float minX = R + 8.f, maxX = kOsuFieldW - R - 8.f;
        const float minY = R + 8.f, maxY = kOsuFieldH - R - 8.f;
        float x = kOsuFieldW * 0.5f, y = kOsuFieldH * 0.5f;
        float heading = -1.5707963f;                 // 朝上起步

        uint32_t rng = 0x9E3779B9u ^ ((uint32_t)style * 2654435761u)
                     ^ ((uint32_t)level * 40503u + 12345u);
        auto rnd01 = [&rng]() { rng = rng * 1664525u + 1013904223u; return (float)(rng >> 8) / 16777216.0f; };

        // 步长（osu!px）：串 = 略大于一个圆（密集但不重叠）；跳 = 1/2 屏级大甩动
        const float kStreamStep = 2.30f * R;         // ≈ 84 px
        const float kJumpStep   = 4.20f * R;         // ≈ 153 px
        int   altPhase = 0;
        int   pairLeft = 0;

        for (int i = 0; i < n; i++)
        {
            const PadEv& e = ev[i];
            const float tu  = (float)e.turn;                     // 度
            const bool  fast = (e.dt > 0.0) && (e.dt <= 0.26);   // 1/4 拍级（>3.8 音/秒）
            const float dirSign = (e.dir >= 0) ? 1.f : -1.f;

            // 1) heading：跟随冰火旋转（转角越大转得越多）
            float rot = (tu / 180.f) * 1.15f;
            if (rot > 2.6f) rot = 2.6f;
            heading += dirSign * rot;
            if (heading > 6.2831853f)  heading -= 6.2831853f;
            if (heading < -6.2831853f) heading += 6.2831853f;

            // 2) 步长：按手法 / 快慢 / 转角
            float step = kStreamStep;
            switch (style)
            {
            case 1:  step = kJumpStep * (0.92f + 0.16f * rnd01()); break;                       // 跳
            case 2:  step = kStreamStep * (0.92f + 0.12f * rnd01()); break;                     // 串
            case 3:  step = (altPhase ^= 1) ? kStreamStep : kJumpStep * (0.95f + 0.15f * rnd01());// 交互
            case 4:                                                                             // 双押
                if (pairLeft > 0) { step = R * 0.85f; pairLeft--; }
                else              { step = kJumpStep * (0.95f + 0.15f * rnd01()); pairLeft = 1; }
                break;
            case 5:                                                                             // 技术
                step = kStreamStep + (kJumpStep - kStreamStep) * std::min(1.f, tu / 180.f);
                step *= (0.85f + 0.30f * rnd01());
                break;
            default:                                                                            // 经典
                if (fast && tu < 70.f) step = kStreamStep * (0.90f + 0.14f * rnd01());
                else                   step = kStreamStep + (kJumpStep - kStreamStep) * std::min(1.f, tu / 180.f);
                break;
            }
            if (step < 0.85f * R) step = 0.85f * R;      // 避免与上一个圆重叠
            if (e.hold > 0.f) step = 0.f;                // 长按：原地（泡泡叠成"按住"）

            // 3) 走位 + 边界反射（圆始终完整留在判定场内）
            x += std::cos(heading) * step;
            y += std::sin(heading) * step;
            if (x < minX) { x = minX + (minX - x); heading = 3.1415926f - heading; }
            if (x > maxX) { x = maxX - (x - maxX); heading = 3.1415926f - heading; }
            if (y < minY) { y = minY + (minY - y); heading = -heading; }
            if (y > maxY) { y = maxY - (y - maxY); heading = -heading; }
            if (x < minX) x = minX; if (x > maxX) x = maxX;
            if (y < minY) y = minY; if (y > maxY) y = maxY;

            outX[i] = x / kOsuFieldW;
            outY[i] = y / kOsuFieldH;
            if (outKey) outKey[i] = (i & 1);             // 供统计：0=左键 1=右键（偶数左、奇数右）
            if (outHoldIdx) outHoldIdx[i] = (e.hold > 0.f) ? 1 : 0;
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

        // ---- 背景：判定场外压暗，判定场略压暗（保留游戏画面）----
        dl->AddRectFilled(ImVec2(0.f, 0.f), ImVec2(gw, gh), IM_COL32(4, 6, 12, 150));
        dl->AddRectFilled(ImVec2(fx0, fy0), ImVec2(fx0 + fw, fy0 + fh), IM_COL32(8, 10, 18, 96), 10.f * u);
        dl->AddRect(ImVec2(fx0, fy0), ImVec2(fx0 + fw, fy0 + fh), IM_COL32(90, 110, 150, 70), 10.f * u, 0, 1.4f * u);

        // ---- 泡泡（HitCircle + ApproachCircle + 连击数字）----
        int liveIdx = -1;
        for (int i = 0; i < nTot; i++) if (s_snap[i].state == 0) { liveIdx = i; break; }
        const int baseCombo = OsuCombo() + 1;
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
            // 判定环：从 4x 半径收到 1x（缩到重合时点击）
            if (dt > -0.28)
            {
                float t01 = 1.f - (float)(dt / preempt);
                if (t01 < 0.f) t01 = 0.f;
                if (t01 > 1.f) t01 = 1.f;
                const float ar = R * (4.f - 3.f * t01);
                const int   aa = (int)(225 * std::min(1.f, (float)((preempt - dt) / (preempt * 0.35f))));
                dl->AddCircle(ImVec2(cx, cy), ar, IM_COL32(255, 255, 255, aa), 72, R * 0.11f);
            }
        }

        // ---- 光标（osu! 默认：白圈 + 实心点 + 拖尾）----
        {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const bool menuOpen = CheatState::MenuVisible.load(std::memory_order_relaxed);
            if (!menuOpen && mp.x >= 0.f && mp.x <= gw && mp.y >= 0.f && mp.y <= gh)
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
