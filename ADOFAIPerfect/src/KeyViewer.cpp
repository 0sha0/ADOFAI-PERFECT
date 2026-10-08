// ============================================================
// KeyViewer.cpp — 按键反馈（KeyViewer Page）
//   参照 ADOFAI 官方 KeyViewer mod（modlist-org/KeyViewer）的呈现方式实现：
//     · 键帽：圆角底板 + 描边 + 顶部高光；按下 0.9→1.0 缩放、按下/松开配色互换
//     · 每键计数（可关）；总击键、实时 KPS、MAX、AVG
//     · 按键雨：按下时键帽虚影向上飘散（官方 Key Rain 的简化）
//     · 任意键位（最多 24 个，自动换行），可从 4K/5K/6K/10K 当前键位一键预设
//   只读键盘（GetAsyncKeyState），不做任何鼠标自动化。
// ============================================================
#include "ChartCore.h"
#include "CheatState.h"
#include "GameBridge.h"
#include "RenderHook.h"
#include "Lang.h"
#include "imgui.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace Chart4K
{
    static const int kVKMax    = 24;
    static const int kVKGhosts = 96;
    static const int kVKDef[4] = { 'D', 'F', 'J', 'K' };

    // ---------------- 用户设置 ----------------
    static std::atomic<bool> s_kvOn{ true };
    static std::atomic<int>  s_kvAnchor{ 2 };       // 0 左上 / 1 右上 / 2 左下 / 3 右下
    static std::atomic<int>  s_kvPosX{ 0 }, s_kvPosY{ -240 };  // 默认在小窗上方
    static std::atomic<int>  s_kvScale{ 100 };
    static std::atomic<int>  s_kvOpacity{ 228 };
    static std::atomic<bool> s_kvVert{ false };
    static std::atomic<bool> s_kvRain{ true };
    static std::atomic<bool> s_kvWrap{ true };
    static std::atomic<int>  s_kvPerRow{ 8 };
    static std::atomic<bool> s_kvShowKps{ true }, s_kvShowCount{ true }, s_kvShowTotalKps{ true };
    static std::atomic<bool> s_kvBg{ true };      // 面板背景（官方 KeyViewer 风格）

    // ---------------- 生效键位 / 编辑草稿（应用后才写入） ----------------
    static int  s_kvLive[kVKMax] = { 'D', 'F', 'J', 'K' };
    static int  s_kvLiveN = 4;
    static int  s_kvDraft[kVKMax] = { 0 };
    static int  s_kvDraftN = 0;
    static bool s_kvEditing = false;
    static int  s_kvCap = -1;
    static int  s_kvLocked[64];
    static int  s_kvLockedN = 0;

    // ---------------- 实时统计（渲染线程） ----------------
    static bool  s_down[kVKMax]     = { false };
    static int   s_count[kVKMax]    = { 0 };
    static float s_press01[kVKMax]  = { 0.f };     // 0..1 按下动画
    static float s_flash[kVKMax]    = { 0.f };
    static float s_keyTimes[kVKMax][64];
    static int   s_keyHead[kVKMax]  = { 0 };
    static float s_rect[kVKMax][4]  = { {0,0,0,0} };  // 上一帧键帽位置（按键雨发射点）
    static int   s_total  = 0;
    static float s_maxKps = 0.f, s_avgSum = 0.f;
    static int   s_avgN   = 0;
    static float s_sampleAcc = 0.f;
    static int   s_lastFrame = -1;

    // ---------------- 按键雨 ----------------
    struct Ghost
    {
        float x, y, vy, life, maxLife;
        float w, h;
        int   vk;
        ImU32 col;
    };
    static Ghost s_ghost[kVKGhosts];
    static int   s_ghostNext = 0;
    static float s_lastU = 1.f;

    // ---------------- 小工具 ----------------
    static ImU32 Lighten(ImU32 c, float k)
    {
        int r = (int)((c >> IM_COL32_R_SHIFT) & 0xFF);
        int g = (int)((c >> IM_COL32_G_SHIFT) & 0xFF);
        int b = (int)((c >> IM_COL32_B_SHIFT) & 0xFF);
        r = (int)(r + (255 - r) * k);
        g = (int)(g + (255 - g) * k);
        b = (int)(b + (255 - b) * k);
        return IM_COL32(r, g, b, (c >> IM_COL32_A_SHIFT) & 0xFF);
    }

    static ImU32 KeyAccent(int i)
    {
        static const ImU32 cols[6] = {
            IM_COL32(72, 214, 186, 255),   // 青绿
            IM_COL32(240, 128, 158, 255),  // 暖粉
            IM_COL32(240, 184, 96, 255),   // 琥珀
            IM_COL32(112, 168, 246, 255),  // 天蓝
            IM_COL32(176, 142, 240, 255),  // 紫
            IM_COL32(128, 216, 148, 255),  // 绿
        };
        return cols[i % 6];
    }

    static void KVKeyName(int vk, char* out, size_t n)
    {
        if (vk >= 'A' && vk <= 'Z') { snprintf(out, n, "%c", (char)vk); return; }
        if (vk >= '0' && vk <= '9') { snprintf(out, n, "%c", (char)vk); return; }
        switch (vk)
        {
        case VK_SPACE:    snprintf(out, n, "SP");    break;
        case VK_LEFT:     snprintf(out, n, "<");     break;
        case VK_UP:       snprintf(out, n, "^");     break;
        case VK_DOWN:     snprintf(out, n, "v");     break;
        case VK_RIGHT:    snprintf(out, n, ">");     break;
        case VK_TAB:      snprintf(out, n, "TB");    break;
        case VK_RETURN:   snprintf(out, n, "EN");    break;
        case VK_BACK:     snprintf(out, n, "BS");    break;
        case VK_DELETE:   snprintf(out, n, "DE");    break;
        case VK_LSHIFT:   snprintf(out, n, "LSH");   break;
        case VK_RSHIFT:   snprintf(out, n, "RSH");   break;
        case VK_LCONTROL: snprintf(out, n, "LCT");   break;
        case VK_RCONTROL: snprintf(out, n, "RCT");   break;
        case VK_LMENU:    snprintf(out, n, "LAL");   break;
        case VK_RMENU:    snprintf(out, n, "RAL");   break;
        default:
            if (vk == 0xBA) snprintf(out, n, ";");
            else if (vk == 0xBB) snprintf(out, n, "=");
            else if (vk == 0xBC) snprintf(out, n, ",");
            else if (vk == 0xBD) snprintf(out, n, "-");
            else if (vk == 0xBE) snprintf(out, n, ".");
            else if (vk == 0xBF) snprintf(out, n, "/");
            else if (vk == 0xC0) snprintf(out, n, "`");
            else if (vk == 0xDB) snprintf(out, n, "[");
            else if (vk == 0xDC) snprintf(out, n, "\\");
            else if (vk == 0xDD) snprintf(out, n, "]");
            else if (vk == 0xDE) snprintf(out, n, "'");
            else snprintf(out, n, "%c", (vk >= 32 && vk < 127) ? (char)vk : '?');
            break;
        }
    }

    static const int kKVCapVKs[] = {
        'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
        '0','1','2','3','4','5','6','7','8','9',
        VK_SPACE, VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN, VK_TAB, VK_RETURN, VK_BACK, VK_DELETE,
        VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU,
        0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0, 0xDB, 0xDC, 0xDD, 0xDE,
    };
    static void KVLockCapture()
    {
        s_kvLockedN = 0;
        for (int k : kKVCapVKs)
            if ((GetAsyncKeyState(k) & 0x8000) && s_kvLockedN < 64)
                s_kvLocked[s_kvLockedN++] = k;
    }
    static bool KVLocked(int vk)
    {
        for (int i = 0; i < s_kvLockedN; i++)
            if (s_kvLocked[i] == vk) return true;
        return false;
    }
    static int KVPollNewKey()
    {
        for (int k : kKVCapVKs)
            if ((GetAsyncKeyState(k) & 0x8000) && !KVLocked(k))
                return k;
        return 0;
    }

    static float KVKpsOfKey(int i, float now)
    {
        int cnt = 0;
        for (int k = 0; k < 64; k++)
            if (s_keyTimes[i][k] > now - 1.0f && s_keyTimes[i][k] <= now + 0.001f)
                cnt++;
        return (float)cnt;
    }
    static float KVKpsTotal(float now)
    {
        float s = 0.f;
        for (int i = 0; i < s_kvLiveN; i++)
            s += KVKpsOfKey(i, now);
        return s;
    }

    static void KVSpawnGhost(int i, ImU32 col)
    {
        if (s_rect[i][2] <= 0.f)
            return;
        Ghost& g = s_ghost[s_ghostNext];
        s_ghostNext = (s_ghostNext + 1) % kVKGhosts;
        g.w = s_rect[i][2];
        g.h = s_rect[i][3];
        g.x = s_rect[i][0] + g.w * 0.5f;
        g.y = s_rect[i][1] + g.h * 0.5f;
        g.vy = -420.f * s_lastU;
        g.maxLife = g.life = 0.55f;
        g.vk = s_kvLive[i];
        g.col = col;
    }

    // 每帧只采集一次（叠加层与设置页预览共用）
    static void TickKeyViewer()
    {
        const int frame = ImGui::GetFrameCount();
        if (s_lastFrame == frame)
            return;
        s_lastFrame = frame;

        const float now = (float)ImGui::GetTime();
        const float dt  = ImGui::GetIO().DeltaTime;
        for (int i = 0; i < s_kvLiveN; i++)
        {
            const bool down = (GetAsyncKeyState(s_kvLive[i]) & 0x8000) != 0;
            if (down && !s_down[i])
            {
                s_count[i]++;
                s_total++;
                s_flash[i] = 0.30f;
                s_keyTimes[i][s_keyHead[i]] = now;
                s_keyHead[i] = (s_keyHead[i] + 1) % 64;
                if (s_kvRain.load(std::memory_order_relaxed))
                    KVSpawnGhost(i, KeyAccent(i));
            }
            s_down[i] = down;
            float target = down ? 1.f : 0.f;
            float rate = down ? 18.f : 11.f;
            s_press01[i] += (target - s_press01[i]) * (1.f - expf(-dt * rate));
            if (fabsf(target - s_press01[i]) < 0.003f) s_press01[i] = target;
            s_flash[i] = (s_flash[i] > dt) ? (s_flash[i] - dt) : 0.f;
        }

        // KPS 采样（MAX / AVG 用，0.1s 一次）
        s_sampleAcc += dt;
        if (s_sampleAcc >= 0.1f)
        {
            s_sampleAcc -= 0.1f;
            float kps = KVKpsTotal(now);
            if (kps > s_maxKps) s_maxKps = kps;
            if (kps > 0.f) { s_avgSum += kps; s_avgN++; }
        }
    }

    // ============================================================
    // 游戏内叠加层（打歌时自动显示，不受菜单显隐影响）
    // ============================================================
    void DrawKeyViewerOverlay()
    {
        if (!s_kvOn.load(std::memory_order_relaxed))
            return;
        CheatState::Status st;
        GameBridge::GetStatusSnapshot(&st);
        if (!(st.gameworld && !st.paused))
            return;

        float gw = 1280.f, gh = 720.f;
        RenderHook::GetGameWindowSize(&gw, &gh);
        if (gw < 200.f || gh < 200.f)
            return;

        TickKeyViewer();
        const int n = s_kvLiveN;
        if (n <= 0)
            return;

        const float u    = (gh / 1080.f) * ((float)s_kvScale.load(std::memory_order_relaxed) / 100.f);
        s_lastU = u;
        const float op   = (float)s_kvOpacity.load(std::memory_order_relaxed) / 255.f;
        const bool  vert = s_kvVert.load(std::memory_order_relaxed);
        const bool  wrap = s_kvWrap.load(std::memory_order_relaxed);
        const bool  showKps   = s_kvShowKps.load(std::memory_order_relaxed);
        const bool  showCount = s_kvShowCount.load(std::memory_order_relaxed);
        const bool  showTot   = s_kvShowTotalKps.load(std::memory_order_relaxed);
        const bool  showBg    = s_kvBg.load(std::memory_order_relaxed);
        int perRow = s_kvPerRow.load(std::memory_order_relaxed);
        if (perRow < 3)  perRow = 3;
        if (perRow > 16) perRow = 16;

        // 紧凑键帽：54u 方块 + 8u 间距；每键计数画进键帽内部，不再额外占一行高度
        const float bw = 54.f * u, bh = 54.f * u, gap = 8.f * u;
        const float pad = showBg ? 12.f * u : 0.f;
        const int   cols = vert ? 1 : (wrap ? (n < perRow ? n : perRow) : n);
        const int   rows = vert ? n : (wrap ? ((n + perRow - 1) / perRow) : 1);
        const float kpsH = (showKps || showTot) ? (22.f * u) : 0.f;
        const float totalW = cols * bw + (cols - 1) * gap;
        const float totalH = rows * bh + (rows - 1) * gap + kpsH;

        int anchor = s_kvAnchor.load(std::memory_order_relaxed);
        if (anchor < 0 || anchor > 3) anchor = 2;
        const float margin = 24.f * u;
        float px = (anchor & 1) ? (gw - totalW - margin) : margin;
        float py = (anchor >= 2) ? (gh - totalH - 40.f * u) : (68.f * u);
        px += (float)s_kvPosX.load(std::memory_order_relaxed) * u;
        py += (float)s_kvPosY.load(std::memory_order_relaxed) * u;
        if (px + totalW + pad > gw - 4.f) px = gw - totalW - pad - 4.f;
        if (py + totalH + pad > gh - 4.f) py = gh - totalH - pad - 4.f;
        if (px - pad < 4.f) px = pad + 4.f;
        if (py - pad < 4.f) py = pad + 4.f;

        const bool interactive = CheatState::MenuVisible.load(std::memory_order_relaxed);
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(gw, gh), ImGuiCond_Always);
        ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                              ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (!interactive)
            fl |= ImGuiWindowFlags_NoInputs;   // 打歌时不抢鼠标，只有菜单打开时可拖动
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##keyviewer_overlay", nullptr, fl);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImFont* font = ImGui::GetFont();
        auto A = [op](ImU32 c, float mul) {
            int a = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * op * mul);
            if (a < 0) a = 0; if (a > 255) a = 255;
            return (c & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
        };

        // ---- 背景面板（让整组按键看起来是一块紧凑的整体） ----
        if (showBg)
        {
            dl->AddRectFilled(ImVec2(px - pad, py - pad), ImVec2(px + totalW + pad, py + totalH + pad),
                              A(IM_COL32(10, 12, 16, 255), 0.62f), 12.f * u);
            dl->AddRect(ImVec2(px - pad, py - pad), ImVec2(px + totalW + pad, py + totalH + pad),
                        A(IM_COL32(255, 255, 255, 255), 0.10f), 12.f * u, 0, 1.4f * u);
        }

        // ---- 菜单开启时：拖动面板即可改位置（打歌中不受影响） ----
        if (interactive)
        {
            ImGui::SetCursorScreenPos(ImVec2(px - pad, py - pad));
            ImGui::InvisibleButton("##kv_drag", ImVec2(totalW + pad * 2.f, totalH + pad * 2.f));
            if (ImGui::IsItemHovered())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            if (ImGui::IsItemActive())
            {
                int nx = s_kvPosX.load(std::memory_order_relaxed) + (int)lroundf(ImGui::GetIO().MouseDelta.x / u);
                int ny = s_kvPosY.load(std::memory_order_relaxed) + (int)lroundf(ImGui::GetIO().MouseDelta.y / u);
                if (nx < -300) nx = -300; if (nx > 300) nx = 300;
                if (ny < -300) ny = -300; if (ny > 300) ny = 300;
                s_kvPosX.store(nx, std::memory_order_relaxed);
                s_kvPosY.store(ny, std::memory_order_relaxed);
            }
        }

        // ---- 按键雨（在键帽之后面） ----
        {
            float dt = ImGui::GetIO().DeltaTime;
            for (int gi = 0; gi < kVKGhosts; gi++)
            {
                Ghost& g = s_ghost[gi];
                if (g.life <= 0.f)
                    continue;
                g.life -= dt;
                g.y += g.vy * dt;
                if (g.life <= 0.f)
                    continue;
                float k = g.life / g.maxLife;               // 1 -> 0
                float a = k * k * 0.55f;
                float w = g.w * (0.72f + 0.28f * k);
                float h = g.h * (0.72f + 0.28f * k);
                ImVec2 a0(g.x - w * 0.5f, g.y - h * 0.5f), a1(g.x + w * 0.5f, g.y + h * 0.5f);
                dl->AddRectFilled(a0, a1, A(g.col, a * 0.5f), 9.f * u);
                dl->AddRect(a0, a1, A(g.col, a), 9.f * u, 0, 1.4f * u);
                char nm[8];
                KVKeyName(g.vk, nm, sizeof(nm));
                float fs = 20.f * u;
                ImVec2 ts = font->CalcTextSizeA(fs, 1e9f, 0.f, nm);
                dl->AddText(font, fs, ImVec2(g.x - ts.x * 0.5f, g.y - ts.y * 0.5f),
                            A(Lighten(g.col, 0.35f), k * 0.9f), nm);
            }
        }

        // ---- KPS / MAX / AVG / 总击键（面板顶部一行） ----
        if (showKps || showTot)
        {
            float kps = KVKpsTotal((float)ImGui::GetTime());
            float avg = (s_avgN > 0) ? (s_avgSum / (float)s_avgN) : 0.f;
            char buf[160];
            int w = 0;
            buf[0] = 0;
            if (showKps)
                w += snprintf(buf + w, sizeof(buf) - w, "KPS %.1f   %s %.0f   %s %.1f",
                              kps, I18N::Tr(I18N::KV_MAX), s_maxKps, I18N::Tr(I18N::KV_AVG), avg);
            if (showTot)
                snprintf(buf + w, sizeof(buf) - w, "%s%s %d", w ? "   |   " : "",
                         I18N::Tr(I18N::KV_TOTAL), s_total);
            float fs = 17.f * u;
            float tw = font->CalcTextSizeA(fs, 1e9f, 0.f, buf).x;
            float ty = py + 2.f * u;
            ImVec2 tp((anchor & 1) ? (px + totalW - tw) : px, ty);
            if (tp.y < 4.f) tp.y = 4.f;
            dl->AddText(font, fs, ImVec2(tp.x + 1.5f * u, tp.y + 1.5f * u), A(IM_COL32(0, 0, 0, 255), 0.8f), buf);
            dl->AddText(font, fs, tp, A(IM_COL32(232, 238, 250, 255), 1.f), buf);
        }

        for (int i = 0; i < n; i++)
        {
            const int col = vert ? 0 : (i % (wrap ? perRow : n));
            const int row = vert ? i : (i / (wrap ? perRow : n));
            float bx = px + col * (bw + gap);
            float by = py + kpsH + row * (bh + gap);

            const float t = s_press01[i];
            const float sc = 0.90f + 0.10f * t;
            const float cx = bx + bw * 0.5f, cy = by + bh * 0.5f;
            const float hw = bw * 0.5f * sc, hh = bh * 0.5f * sc;
            ImVec2 a0(cx - hw, cy - hh), a1(cx + hw, cy + hh);
            ImU32 acc = KeyAccent(i);

            // 外发光（按下）
            if (t > 0.02f)
            {
                float glow = (1.f - t) * 0.5f + 0.5f;
                dl->AddRectFilled(ImVec2(a0.x - 6.f * u, a0.y - 6.f * u),
                                  ImVec2(a1.x + 6.f * u, a1.y + 6.f * u),
                                  A(acc, 0.30f * t * glow), 15.f * u);
            }
            // 阴影
            dl->AddRectFilled(ImVec2(a0.x, a0.y + 3.f * u), ImVec2(a1.x, a1.y + 3.f * u),
                              A(IM_COL32(0, 0, 0, 255), 0.35f), 10.f * u);
            // 底板（上浅下深渐变）
            ImU32 bgTop = (t > 0.01f) ? A(Lighten(acc, 0.45f - 0.35f * t), 0.96f) : A(IM_COL32(255, 255, 255, 255), 0.15f);
            ImU32 bgBot = (t > 0.01f) ? A(acc, 0.96f) : A(IM_COL32(255, 255, 255, 255), 0.05f);
            dl->AddRectFilled(a0, a1, bgBot, 10.f * u);
            dl->AddRectFilled(a0, ImVec2(a1.x, a0.y + (a1.y - a0.y) * 0.55f), bgTop,
                              10.f * u, ImDrawFlags_RoundCornersTop);
            // 顶部高光
            dl->AddLine(ImVec2(a0.x + 8.f * u, a0.y + 3.f * u), ImVec2(a1.x - 8.f * u, a0.y + 3.f * u),
                        A(IM_COL32(255, 255, 255, 255), 0.18f + 0.45f * t), 1.6f * u);
            // 描边
            dl->AddRect(a0, a1, (t > 0.01f) ? A(IM_COL32(255, 255, 255, 255), 0.85f * t + 0.15f)
                                            : A(IM_COL32(255, 255, 255, 255), 0.22f),
                        10.f * u, 0, (1.4f + 0.8f * t) * u);

            // 键名（有计数时上移一点，计数画在键帽内下方）
            char nm[8];
            KVKeyName(s_kvLive[i], nm, sizeof(nm));
            float fs = (showCount ? 21.f : 24.f) + 3.f * t;
            float ny = showCount ? (cy - 7.f * u) : cy;
            ImVec2 ts = font->CalcTextSizeA(fs * u, 1e9f, 0.f, nm);
            dl->AddText(font, fs * u, ImVec2(cx - ts.x * 0.5f, ny - ts.y * 0.5f),
                        (t > 0.45f) ? IM_COL32(16, 22, 22, 255) : A(IM_COL32(240, 244, 252, 255), 1.f), nm);

            // 每键计数：画在键帽内部下方（紧凑，不额外占行）
            if (showCount)
            {
                char cb[16];
                snprintf(cb, sizeof(cb), "%d", s_count[i]);
                float cs = 12.f * u;
                ImVec2 cts = font->CalcTextSizeA(cs, 1e9f, 0.f, cb);
                dl->AddText(font, cs, ImVec2(cx - cts.x * 0.5f, cy + 11.f * u - cts.y * 0.5f),
                            (t > 0.45f) ? IM_COL32(20, 26, 26, 210) : A(IM_COL32(176, 186, 206, 255), 0.85f), cb);
            }

            s_rect[i][0] = bx; s_rect[i][1] = by; s_rect[i][2] = bw; s_rect[i][3] = bh;
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    // ============================================================
    // 主窗口：KeyViewer 设置页
    // ============================================================
    static void KVSetDraft(const int* keys, int n)
    {
        if (n > kVKMax) n = kVKMax;
        for (int i = 0; i < n; i++) s_kvDraft[i] = keys[i];
        s_kvDraftN = n;
        s_kvEditing = true;
        s_kvCap = -1;
    }

    // ---------------- 配置档案访问器（ChartCore.h 声明；设置页保存/加载用） ----------------
    int KVSettingGet(int which)
    {
        switch (which)
        {
        case 0:  return s_kvOn.load(std::memory_order_relaxed) ? 1 : 0;
        case 1:  return s_kvAnchor.load(std::memory_order_relaxed);
        case 2:  return s_kvPosX.load(std::memory_order_relaxed);
        case 3:  return s_kvPosY.load(std::memory_order_relaxed);
        case 4:  return s_kvScale.load(std::memory_order_relaxed);
        case 5:  return s_kvOpacity.load(std::memory_order_relaxed);
        case 6:  return s_kvVert.load(std::memory_order_relaxed) ? 1 : 0;
        case 7:  return s_kvRain.load(std::memory_order_relaxed) ? 1 : 0;
        case 8:  return s_kvWrap.load(std::memory_order_relaxed) ? 1 : 0;
        case 9:  return s_kvPerRow.load(std::memory_order_relaxed);
        case 10: return s_kvShowKps.load(std::memory_order_relaxed) ? 1 : 0;
        case 11: return s_kvShowCount.load(std::memory_order_relaxed) ? 1 : 0;
        case 12: return s_kvShowTotalKps.load(std::memory_order_relaxed) ? 1 : 0;
        case 13: return s_kvBg.load(std::memory_order_relaxed) ? 1 : 0;
        default: return 0;
        }
    }
    void KVSettingSet(int which, int v)
    {
        switch (which)
        {
        case 0:  s_kvOn.store(v != 0, std::memory_order_relaxed); break;
        case 1:  if (v < 0) v = 0; if (v > 3) v = 3; s_kvAnchor.store(v, std::memory_order_relaxed); break;
        case 2:  s_kvPosX.store(v, std::memory_order_relaxed); break;
        case 3:  s_kvPosY.store(v, std::memory_order_relaxed); break;
        case 4:  if (v < 50) v = 50; if (v > 200) v = 200; s_kvScale.store(v, std::memory_order_relaxed); break;
        case 5:  if (v < 40) v = 40; if (v > 255) v = 255; s_kvOpacity.store(v, std::memory_order_relaxed); break;
        case 6:  s_kvVert.store(v != 0, std::memory_order_relaxed); break;
        case 7:  s_kvRain.store(v != 0, std::memory_order_relaxed); break;
        case 8:  s_kvWrap.store(v != 0, std::memory_order_relaxed); break;
        case 9:  if (v < 3) v = 3; if (v > 12) v = 12; s_kvPerRow.store(v, std::memory_order_relaxed); break;
        case 10: s_kvShowKps.store(v != 0, std::memory_order_relaxed); break;
        case 11: s_kvShowCount.store(v != 0, std::memory_order_relaxed); break;
        case 12: s_kvShowTotalKps.store(v != 0, std::memory_order_relaxed); break;
        case 13: s_kvBg.store(v != 0, std::memory_order_relaxed); break;
        default: break;
        }
    }
    int KVKeyCount() { return s_kvLiveN; }
    int KVKeyGet(int i) { return (i >= 0 && i < s_kvLiveN) ? s_kvLive[i] : 0; }
    void KVKeySet(int i, int vk)
    {
        if (i < 0 || i >= kVKMax || vk <= 0) return;
        s_kvLive[i] = vk;
        s_kvDraft[i] = vk;
        if (i >= s_kvLiveN) s_kvLiveN = i + 1;
        if (s_kvDraftN < s_kvLiveN) s_kvDraftN = s_kvLiveN;
        s_kvEditing = false;
        s_kvCap = -1;
    }
    void KVKeySetCount(int n)
    {
        if (n < 1) n = 1;
        if (n > kVKMax) n = kVKMax;
        s_kvLiveN = n;
        s_kvDraftN = n;
        s_kvEditing = false;
        s_kvCap = -1;
    }

    // ---- 卡片高度（页面滚动范围由 Menu.cpp 通过 KeyViewerPageHeight() 取用） ----
    static constexpr float kKVCardMain = 112.f;
    static constexpr float kKVCardLook = 312.f;
    static constexpr float kKVCardStat = 116.f;
    static float KVKeyCardHeight()
    {
        const int n = s_kvEditing ? s_kvDraftN : s_kvLiveN;
        int rows = (n + 4) / 5;
        if (rows < 1) rows = 1;
        float h = 30.f;                                     // 键位行
        h += s_kvEditing ? (rows * 30.f + 8.f + 30.f + 30.f) : 20.f;
        h += 34.f;                                          // 预设行
        return h + 16.f;
    }

    void DrawKeyViewerSettingsPage()
    {
        TickKeyViewer();

        // ---- 卡片 1：开关 + 说明 ----
        BeginCard4K("##cardKV_main", kKVCardMain);
        {
            bool en = s_kvOn.load(std::memory_order_relaxed);
            if (TitleToggleRow("##kv_en", 19.f, I18N::Tr(I18N::KV_ON), &en))
            {
                s_kvOn.store(en, std::memory_order_relaxed);
                Log::Printf("[UI] keyviewer %s", en ? "on" : "off");
            }
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 166, 182, 255));
            ImGui::TextWrapped("%s", I18N::Tr(I18N::KV_DESC));
            ImGui::PopStyleColor();
        }
        EndCard4K();
        ImGui::Spacing();

        // ---- 卡片 2：键位编辑（任意数量，最多 24；点槽位后按新键） ----
        const float colW = LabelCol({ I18N::Tr(I18N::LBL_KEYS), I18N::Tr(I18N::KV_PRESET) });
        BeginCard4K("##cardKV_keys", KVKeyCardHeight());
        {
            const float sp = ImGui::GetStyle().ItemSpacing.x;
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f, 4.f));
            char nm[8];

            if (!s_kvEditing)
            {
                char cur[192];
                int c = 0;
                cur[0] = 0;
                for (int i = 0; i < s_kvLiveN; i++)
                {
                    KVKeyName(s_kvLive[i], nm, sizeof(nm));
                    c += snprintf(cur + c, sizeof(cur) - c, "%s%s", i ? " " : "", nm);
                }
                ImGui::TextUnformatted(I18N::Tr(I18N::LBL_KEYS));
                ImGui::SameLine();
                ImGui::SetCursorPosX(colW);
                ImGui::TextDisabled("%s", cur[0] ? cur : "-");
                ImGui::SameLine();
                {
                    float bx = ImGui::GetCursorPosX();
                    if (bx < colW + 150.f) bx = colW + 150.f;
                    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 96.f);
                }
                if (ImGui::Button(I18N::Tr(I18N::KV_KEYSEDIT), ImVec2(96, 26)))
                {
                    for (int i = 0; i < s_kvLiveN; i++)
                        s_kvDraft[i] = s_kvLive[i];
                    s_kvDraftN = s_kvLiveN;
                    s_kvEditing = true;
                    s_kvCap = -1;
                }
            }
            else
            {
                ImGui::TextUnformatted(I18N::Tr(I18N::LBL_KEYS));
                ImGui::SameLine();
                ImGui::SetCursorPosX(colW);
                char lbl[40];
                int perRow = 5;
                {
                    float avail = ImGui::GetContentRegionAvail().x;
                    perRow = (int)((avail + 4.f) / 78.f);
                    if (perRow < 3) perRow = 3;
                }
                for (int i = 0; i < s_kvDraftN; i++)
                {
                    KVKeyName(s_kvDraft[i], nm, sizeof(nm));
                    if (s_kvCap == i) snprintf(lbl, sizeof(lbl), "[%s]##kvs%d", nm, i);
                    else              snprintf(lbl, sizeof(lbl), "%s##kvs%d", nm, i);
                    if (s_kvCap == i)
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.26f, 0.59f, 0.98f, 0.90f));
                    else
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 1.f, 1.f, 0.10f));
                    if (ImGui::Button(lbl, ImVec2(46, 26)))
                    {
                        s_kvCap = (s_kvCap == i) ? -1 : i;
                        if (s_kvCap == i) KVLockCapture();
                    }
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    snprintf(lbl, sizeof(lbl), "x##kvdel%d", i);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 1.f, 1.f, 0.05f));
                    if (ImGui::Button(lbl, ImVec2(24, 26)))
                    {
                        for (int j = i; j + 1 < s_kvDraftN; j++)
                            s_kvDraft[j] = s_kvDraft[j + 1];
                        s_kvDraftN--;
                        s_kvCap = -1;
                        ImGui::PopStyleColor();
                        break;
                    }
                    ImGui::PopStyleColor();
                    if ((i + 1) % perRow != 0 && i + 1 < s_kvDraftN)
                        ImGui::SameLine();
                }

                if (s_kvCap >= 0)
                {
                    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
                    {
                        if (s_kvCap == s_kvDraftN - 1 && s_kvDraft[s_kvCap] == 0)
                            s_kvDraftN--;
                        s_kvCap = -1;
                    }
                    else
                    {
                        int got = KVPollNewKey();
                        if (got)
                        {
                            bool dup = false;
                            for (int j = 0; j < s_kvDraftN; j++)
                                if (j != s_kvCap && s_kvDraft[j] == got) dup = true;
                            if (!dup) { s_kvDraft[s_kvCap] = got; s_kvCap = -1; }
                        }
                    }
                }

                ImGui::SetCursorPosX(colW);
                if (s_kvDraftN < kVKMax && ImGui::Button(I18N::Tr(I18N::KV_ADD), ImVec2(92, 26)))
                {
                    if (s_kvCap >= 0 && s_kvCap < s_kvDraftN)
                    {
                        s_kvCap = -1;
                    }
                    else
                    {
                        s_kvDraft[s_kvDraftN] = 0;
                        s_kvCap = s_kvDraftN;
                        s_kvDraftN++;
                        KVLockCapture();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button(I18N::Tr(I18N::LBL_RESTORE), ImVec2(96, 26)))
                {
                    s_kvDraftN = 4;
                    for (int i = 0; i < 4; i++) s_kvDraft[i] = kVKDef[i];
                    s_kvCap = -1;
                }

                bool dupAny = false;
                for (int i = 0; i < s_kvDraftN && !dupAny; i++)
                    for (int j = i + 1; j < s_kvDraftN; j++)
                        if (s_kvDraft[i] == s_kvDraft[j]) dupAny = true;

                ImGui::SetCursorPosX(colW);
                if (dupAny)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.20f, 0.55f));
                if (ImGui::Button(I18N::Tr(I18N::LBL_APPLY), ImVec2(84, 26)) && !dupAny)
                {
                    s_kvLiveN = s_kvDraftN;
                    for (int i = 0; i < s_kvLiveN; i++)
                    {
                        s_kvLive[i] = s_kvDraft[i];
                        s_count[i] = 0;
                        s_down[i] = false;
                        s_press01[i] = 0.f;
                        for (int k = 0; k < 64; k++) s_keyTimes[i][k] = 0.f;
                    }
                    s_total = 0; s_maxKps = 0.f; s_avgSum = 0.f; s_avgN = 0;
                    s_kvEditing = false;
                    s_kvCap = -1;
                    Log::Printf("[UI] keyviewer keys -> %d", s_kvLiveN);
                }
                if (dupAny)
                    ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::Button(I18N::Tr(I18N::LBL_CANCEL), ImVec2(84, 26)))
                {
                    s_kvEditing = false;
                    s_kvCap = -1;
                }
                ImGui::SameLine();
                if (dupAny)
                    ImGui::TextDisabled("%s", I18N::Tr(I18N::LBL_KEYDUP));
                else if (s_kvCap >= 0)
                    ImGui::TextDisabled("%s", I18N::Tr(I18N::LBL_PRESSNEW));
                else
                    ImGui::TextDisabled("%s", I18N::Tr(I18N::LBL_PRESSKEY));
            }

            // 快速预设
            ImGui::Spacing();
            ImGui::TextUnformatted(I18N::Tr(I18N::KV_PRESET));
            ImGui::SameLine();
            ImGui::SetCursorPosX(colW);
            for (int mi = 0; mi < kModeN; mi++)
            {
                char pid[16];
                snprintf(pid, sizeof(pid), "%dK##kvpre%d", kLanesOf[mi], mi);
                if (ImGui::Button(pid, ImVec2(52, 24)))
                {
                    int tmp[kVKMax];
                    int tn = 0;
                    GetModeKeys(mi, tmp, &tn);
                    KVSetDraft(tmp, tn);
                }
                ImGui::SameLine();
            }
            if (ImGui::Button("DFJK##kvpredef", ImVec2(62, 24)))
                KVSetDraft(kVKDef, 4);
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 150.f);
            if (ImGui::Button(I18N::Tr(I18N::KV_RESETSTATS), ImVec2(150, 24)))
            {
                s_total = 0; s_maxKps = 0.f; s_avgSum = 0.f; s_avgN = 0;
                for (int i = 0; i < kVKMax; i++)
                {
                    s_count[i] = 0;
                    for (int k = 0; k < 64; k++) s_keyTimes[i][k] = 0.f;
                }
            }
            ImGui::PopStyleVar();
        }
        EndCard4K();
        ImGui::Spacing();

        // ---- 卡片 3：外观 / 行为 ----
        const float lookW = LabelCol({ I18N::Tr(I18N::LBL_POS), I18N::Tr(I18N::LBL_FINETUNE),
                                       I18N::Tr(I18N::RD_SCALE), I18N::Tr(I18N::LBL_OPACITY),
                                       I18N::Tr(I18N::KV_PERROW) });
        BeginCard4K("##cardKV_look", kKVCardLook);
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 6.f));
            int anchor = s_kvAnchor.load(std::memory_order_relaxed);
            char posItems[192] = { 0 };
            {
                int off = 0;
                for (int i = 0; i < 4; i++)
                {
                    const char* t = I18N::Tr(i == 0 ? I18N::LBL_POS_LT : i == 1 ? I18N::LBL_POS_RT
                                                  : i == 2 ? I18N::LBL_POS_LB : I18N::LBL_POS_RB);
                    off += snprintf(posItems + off, sizeof(posItems) - off, "%s", t);
                    posItems[++off] = 0;
                }
            }
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_POS));
            ImGui::SameLine();
            ImGui::SetCursorPosX(lookW);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::Combo("##kv_anchor", &anchor, posItems))
                s_kvAnchor.store(anchor, std::memory_order_relaxed);

            int vx = s_kvPosX.load(std::memory_order_relaxed);
            int vy = s_kvPosY.load(std::memory_order_relaxed);
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_FINETUNE));
            ImGui::SameLine();
            ImGui::SetCursorPosX(lookW);
            float halfW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (halfW < 70.f) halfW = 70.f;
            ImGui::SetNextItemWidth(halfW);
            if (ImGui::SliderInt("##kv_px", &vx, -300, 300, "X %d"))
                s_kvPosX.store(vx, std::memory_order_relaxed);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(halfW);
            if (ImGui::SliderInt("##kv_py", &vy, -300, 300, "Y %d"))
                s_kvPosY.store(vy, std::memory_order_relaxed);

            int sc = s_kvScale.load(std::memory_order_relaxed);
            ImGui::TextUnformatted(I18N::Tr(I18N::RD_SCALE));
            ImGui::SameLine();
            ImGui::SetCursorPosX(lookW);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::SliderInt("##kv_scale", &sc, 60, 180, "%d %%"))
                s_kvScale.store(sc, std::memory_order_relaxed);

            int opa = s_kvOpacity.load(std::memory_order_relaxed);
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_OPACITY));
            ImGui::SameLine();
            ImGui::SetCursorPosX(lookW);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::SliderInt("##kv_opac", &opa, 60, 255))
                s_kvOpacity.store(opa, std::memory_order_relaxed);

            int pr = s_kvPerRow.load(std::memory_order_relaxed);
            ImGui::TextUnformatted(I18N::Tr(I18N::KV_PERROW));
            ImGui::SameLine();
            ImGui::SetCursorPosX(lookW);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::SliderInt("##kv_perrow", &pr, 3, 16))
                s_kvPerRow.store(pr, std::memory_order_relaxed);

            bool vt = s_kvVert.load(std::memory_order_relaxed);
            if (MiniToggle("##kv_vert", &vt)) s_kvVert.store(vt, std::memory_order_relaxed);
            ImGui::SameLine(); ImGui::TextUnformatted(I18N::Tr(I18N::KV_VERT));
            ImGui::SameLine(); ImGui::SetCursorPosX(lookW + 92.f);
            bool wr = s_kvWrap.load(std::memory_order_relaxed);
            if (MiniToggle("##kv_wrap", &wr)) s_kvWrap.store(wr, std::memory_order_relaxed);
            ImGui::SameLine(); ImGui::TextUnformatted(I18N::Tr(I18N::KV_WRAP));

            bool rn = s_kvRain.load(std::memory_order_relaxed);
            if (MiniToggle("##kv_rain", &rn)) s_kvRain.store(rn, std::memory_order_relaxed);
            ImGui::SameLine(); ImGui::TextUnformatted(I18N::Tr(I18N::KV_RAIN));
            ImGui::SameLine(); ImGui::SetCursorPosX(lookW + 92.f);
            bool bgv = s_kvBg.load(std::memory_order_relaxed);
            if (MiniToggle("##kv_bg", &bgv)) s_kvBg.store(bgv, std::memory_order_relaxed);
            ImGui::SameLine(); ImGui::TextUnformatted(I18N::Tr(I18N::KV_BG));

            bool sk = s_kvShowKps.load(std::memory_order_relaxed);
            if (MiniToggle("##kv_kps", &sk)) s_kvShowKps.store(sk, std::memory_order_relaxed);
            ImGui::SameLine(); ImGui::TextUnformatted(I18N::Tr(I18N::KV_SHOWKPS));
            ImGui::SameLine(); ImGui::SetCursorPosX(lookW + 92.f);
            bool sc2 = s_kvShowCount.load(std::memory_order_relaxed);
            if (MiniToggle("##kv_cnt", &sc2)) s_kvShowCount.store(sc2, std::memory_order_relaxed);
            ImGui::SameLine(); ImGui::TextUnformatted(I18N::Tr(I18N::KV_SHOWCOUNT));

            bool st2 = s_kvShowTotalKps.load(std::memory_order_relaxed);
            if (MiniToggle("##kv_tot", &st2)) s_kvShowTotalKps.store(st2, std::memory_order_relaxed);
            ImGui::SameLine(); ImGui::TextUnformatted(I18N::Tr(I18N::KV_SHOWTOTAL));

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 158, 174, 255));
            ImGui::TextWrapped("%s", I18N::Tr(I18N::LBL_DRAG));
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }
        EndCard4K();
        ImGui::Spacing();

        // ---- 卡片 4：实时状态（菜单内预览） ----
        BeginCard4K("##cardKV_stat", kKVCardStat);
        {
            const float now = (float)ImGui::GetTime();
            float kps = KVKpsTotal(now);
            float avg = (s_avgN > 0) ? (s_avgSum / (float)s_avgN) : 0.f;
            ImGui::Text("KPS %.1f    %s %.0f    %s %.1f    %s %d",
                        kps, I18N::Tr(I18N::KV_MAX), s_maxKps, I18N::Tr(I18N::KV_AVG), avg,
                        I18N::Tr(I18N::KV_TOTAL), s_total);

            char line[256];
            int c = 0;
            line[0] = 0;
            for (int i = 0; i < s_kvLiveN; i++)
            {
                char nm[8];
                KVKeyName(s_kvLive[i], nm, sizeof(nm));
                c += snprintf(line + c, sizeof(line) - c, "%s%s%s%s",
                              i ? "  " : "", s_down[i] ? "[" : "", nm, s_down[i] ? "]" : "");
                if (c > (int)sizeof(line) - 16) break;
            }
            ImGui::TextDisabled("%s", line[0] ? line : "-");
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 158, 174, 255));
            ImGui::TextWrapped("%s", I18N::Tr(I18N::KV_INFO));
            ImGui::PopStyleColor();
        }
        EndCard4K();
    }

}
