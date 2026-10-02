#include "Menu.h"
#include "CheatState.h"
#include "GameBridge.h"
#include "Chart4K.h"
#include "Log.h"

#include <windows.h>
#include "imgui.h"
#include <cmath>
#include <cfloat>
#include <cstdio>

// ============================================================
// Menu.cpp — ImGui 界面（自定义风格）
//   · 半透明正方形小窗，双页：功能 / 实时状态
//   · 可折叠成一颗可拖动的小圆点，点击展开
// ============================================================
namespace Menu
{
    // ---- 本地状态 ----
    static bool  s_collapsed = false;      // 是否缩成小圆点
    static ImVec2 s_mainSize(448, 430);    // 主窗口尺寸（可拖右下角调整，控件随宽度自适应）
    static ImVec2 s_mainPos(60, 60);       // 主窗口位置（折叠/展开间保持）
    static ImVec2 s_dotPos(60, 60);        // 小圆点位置
    static float s_anim[2] = { 0.f, 0.f }; // 开关动画 0..1
    static int   s_page = 0;               // 0=功能 1=状态

    // ---- 调色板 ----
    static const ImVec4 kAccent    = ImVec4(0.24f, 0.85f, 0.72f, 1.f);  // 青绿
    static const ImVec4 kAccent2   = ImVec4(1.00f, 0.48f, 0.60f, 1.f);  // 暖粉
    static const ImVec4 kTextDim   = ImVec4(0.62f, 0.65f, 0.72f, 1.f);
    static const ImVec4 kCardBg    = ImVec4(1.f, 1.f, 1.f, 0.045f);
    static const ImVec4 kCardLine  = ImVec4(1.f, 1.f, 1.f, 0.10f);

    // ---------- 工具 ----------
    static ImU32 Col(const ImVec4& c, float alphaMul = 1.f)
    {
        return IM_COL32((int)(c.x * 255), (int)(c.y * 255), (int)(c.z * 255),
                        (int)(c.w * 255 * alphaMul));
    }

    // ---------- 开关（药丸形，带动画） ----------
    static bool ToggleSwitch(const char* id, bool* v, float* anim, float width = 54.f, float height = 28.f)
    {
        float target = *v ? 1.f : 0.f;
        float dt = ImGui::GetIO().DeltaTime;
        *anim += (target - *anim) * (1.f - expf(-dt * 14.f));
        if (fabsf(target - *anim) < 0.002f)
            *anim = target;

        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        bool clicked = ImGui::InvisibleButton(id, ImVec2(width, height));
        // 防止同一物理点击在多帧中被重复上报（Present 多次触发的场景）
        {
            static DWORD s_lastTick = 0;
            DWORD now = GetTickCount();
            if (clicked && now - s_lastTick < 200)
                clicked = false;
            if (clicked)
            {
                s_lastTick = now;
                *v = !*v; // ★ 翻转状态
            }
        }

        ImU32 bg = *v ? Col(kAccent, 0.92f) : IM_COL32(0, 0, 0, 110);
        if (ImGui::IsItemHovered())
            bg = *v ? Col(kAccent, 1.f) : IM_COL32(255, 255, 255, 40);
        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), bg, height * 0.5f);
        // 内圈微光
        if (*v)
            dl->AddRectFilled(ImVec2(p.x + 2, p.y + 2), ImVec2(p.x + width - 2, p.y + height - 2),
                              IM_COL32(255, 255, 255, 18), height * 0.5f - 2);

        float knobD = height - 8.f;
        float kx = p.x + 4.f + *anim * (width - 8.f - knobD);
        dl->AddCircleFilled(ImVec2(kx + knobD * 0.5f, p.y + height * 0.5f), knobD * 0.5f,
                            IM_COL32(250, 250, 252, 255));
        return clicked;
    }

    // ---------- 圆角卡片 ----------
    static void BeginCard(const char* id, float height)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kCardBg);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
        ImGui::BeginChild(id, ImVec2(-1, height), ImGuiChildFlags_None);
    }
    static void EndCard()
    {
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    // ---------- 主窗口 ----------
    static void DrawMain()
    {
        const CheatState::Status st = [] {
            CheatState::Status s;
            GameBridge::GetStatusSnapshot(&s);
            return s;
        }();

        // 页内容超出窗口高度时可上下滚动（鼠标滚轮 / 右侧滚动条）。
        // 4K 页三张卡片总高 > 窗口可视区，指定内容高度让 ImGui 产生滚动范围。
        const float k4kPageH = 92.f + 8.f + 244.f + 8.f + 152.f + 44.f;   // 4K 页三张卡片
        const float k6kPageH = 92.f + 8.f + 118.f + 8.f + 132.f + 44.f;   // 6K 页三张卡片
        const float pageScrollH = (s_page == 2) ? (88.f + k4kPageH)
                                : (s_page == 3) ? (88.f + k6kPageH) : 400.f;
        // 宽度 0 = 由内容自适应（不会出现横向滚动条）；高度显式给值以产生纵向滚动范围
        ImGui::SetNextWindowContentSize(ImVec2(0.f, pageScrollH));
        ImGui::SetNextWindowPos(s_mainPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(s_mainSize, ImGuiCond_Always);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
        ImGui::Begin("##adofai_main", nullptr, flags);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wpos = ImGui::GetWindowPos();
        ImVec2 wsize = ImGui::GetWindowSize();

        // ============ 标题栏（自绘，可拖动窗口） ============
        {
            const float hdrH = 38.f;
            dl->AddRectFilled(ImVec2(wpos.x + 1, wpos.y + 1),
                              ImVec2(wpos.x + wsize.x - 1, wpos.y + hdrH),
                              IM_COL32(21, 22, 26, 255), 11.f, ImDrawFlags_RoundCornersTop);
            ImGui::SetCursorScreenPos(ImVec2(wpos.x + 8.f, wpos.y + 8.f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            ImGui::InvisibleButton("##hdrdrag", ImVec2(wsize.x - 76.f, hdrH - 12.f));
            ImGui::PopStyleVar();
            if (ImGui::IsItemActive())
                s_mainPos = ImVec2(s_mainPos.x + ImGui::GetIO().MouseDelta.x,
                                   s_mainPos.y + ImGui::GetIO().MouseDelta.y);

            // 标题：ADOFAI-PERFECT
            ImGui::SetCursorScreenPos(ImVec2(wpos.x + 14, wpos.y + 11));
            ImGui::PushStyleColor(ImGuiCol_Text, Col(kAccent));
            ImGui::TextUnformatted("\xe2\x97\x88"); // ◈
            ImGui::PopStyleColor();
            ImGui::SetCursorScreenPos(ImVec2(wpos.x + 30, wpos.y + 12));
            ImGui::PushFont(nullptr, 14.f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.95f, 0.98f, 1.f));
            ImGui::TextUnformatted("ADOFAI-PERFECT");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, Col(kTextDim, 0.9f));
            ImGui::TextUnformatted("  \xc2\xb7  4K / 6K ASSIST");
            ImGui::PopStyleColor();
            ImGui::PopFont();

            // 折叠按钮（–）
            ImGui::SetCursorScreenPos(ImVec2(wpos.x + wsize.x - 34, wpos.y + 8));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.14f));
            if (ImGui::Button("##min", ImVec2(26, 22)))
                s_collapsed = true;
            ImGui::PopStyleColor(3);
            {
                ImVec2 bp = ImGui::GetItemRectMin();
                dl->AddLine(ImVec2(bp.x + 6, bp.y + 11), ImVec2(bp.x + 20, bp.y + 11),
                            IM_COL32(200, 205, 215, 220), 2.f);
            }

            dl->AddLine(ImVec2(wpos.x + 1, wpos.y + hdrH),
                        ImVec2(wpos.x + wsize.x - 1, wpos.y + hdrH),
                        IM_COL32(255, 255, 255, 22));
        }

        // ============ 模式 TAB（深色标签栏） ============
        {
            const char* names[4] = {
                "\xe5\x8a\x9f\xe8\x83\xbd",                        // 功能
                "\xe5\xae\x9e\xe6\x97\xb6\xe7\x8a\xb6\xe6\x80\x81", // 实时状态
                "4K\xe8\xbe\x85\xe5\x8a\xa9",                      // 4K辅助
                "6K\xe6\xa8\xa1\xe5\xbc\x8f"                       // 6K模式
            };
            // 页签条底：整幅深色（随窗口宽度自适应），让 TAB 看起来像设计好的分段控件
            dl->AddRectFilled(ImVec2(wpos.x + 1.f, wpos.y + 38.f),
                              ImVec2(wpos.x + wsize.x - 1.f, wpos.y + 86.f),
                              IM_COL32(16, 17, 20, 255));
            dl->AddLine(ImVec2(wpos.x + 1.f, wpos.y + 86.f),
                        ImVec2(wpos.x + wsize.x - 1.f, wpos.y + 86.f),
                        IM_COL32(255, 255, 255, 18));
            ImGui::SetCursorScreenPos(ImVec2(wpos.x + 10.f, wpos.y + 46.f));
            ImGui::PushStyleColor(ImGuiCol_Tab,                 ImVec4(0.11f, 0.11f, 0.13f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_TabHovered,          ImVec4(0.26f, 0.59f, 0.98f, 0.45f));
            ImGui::PushStyleColor(ImGuiCol_TabSelected,         ImVec4(0.16f, 0.29f, 0.48f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, ImVec4(0.26f, 0.59f, 0.98f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_TabDimmed,           ImVec4(0.11f, 0.11f, 0.13f, 0.75f));
            ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected,   ImVec4(0.14f, 0.22f, 0.36f, 1.f));
            ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 6.f);
            if (ImGui::BeginTabBar("##modetabs", ImGuiTabBarFlags_None))
            {
                for (int i = 0; i < 4; i++)
                {
                    if (ImGui::BeginTabItem(names[i]))
                    {
                        s_page = i;
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(6);
        }

        // 页面几何（全部绝对坐标，杜绝光标残留导致的错位）
        // 滚动：页内容整体上移 -scrollY；标题栏 / 页签 / 底部按钮固定不动
        const float scrollY = ImGui::GetScrollY();
        const float pageX = wpos.x + 12.f;
        const float pageY = wpos.y + 88.f - scrollY;             // 标题栏34 + 边距 + 页签32 + 间隙
        const float pageW = wsize.x - 24.f;
        const float contentH = wsize.y - 88.f - 52.f;            // 底部留按钮/提示区

        // 可滚动内容区裁剪（内容滚出可视区时不覆盖标题栏 / 页签 / 底部按钮）
        ImGui::PushClipRect(ImVec2(wpos.x + 1.f, wpos.y + 88.f),
                            ImVec2(wpos.x + wsize.x - 1.f,
                                   wpos.y + wsize.y - ((s_page == 2 || s_page == 3) ? 8.f : 46.f)), true);

        // ============ 页：功能 ============
        if (s_page == 0)
        {
            const float cardH = (contentH - 10.f) * 0.5f;

            auto FeatureCard = [&](int idx, const char* id, const char* title,
                                   std::atomic<bool>* value, float* anim,
                                   const ImVec4& accent, const char* swId) {
                const float cy = pageY + idx * (cardH + 10.f);

                // 卡片背景（主窗口 drawlist 直绘，位置绝对可控）
                dl->AddRectFilled(ImVec2(pageX, cy), ImVec2(pageX + pageW, cy + cardH),
                                  Col(kCardBg), 12.f);
                dl->AddRect(ImVec2(pageX, cy), ImVec2(pageX + pageW, cy + cardH),
                            Col(kCardLine), 12.f);

                ImFont* font = ImGui::GetFont();

                // ---- 大号艺术字标题（阴影 + 渐变双层）----
                {
                    float ts = 30.f;
                    ImVec2 tsize = font->CalcTextSizeA(ts, FLT_MAX, 0.f, title);
                    ImVec2 tp(pageX + 22.f, cy + (cardH - tsize.y) * 0.5f);
                    bool on = value->load(std::memory_order_relaxed);
                    // 阴影
                    dl->AddText(font, ts, ImVec2(tp.x + 2, tp.y + 2), IM_COL32(0, 0, 0, 170), title);
                    // 主体（开启时亮色，关闭时灰）
                    ImU32 mainCol = on ? Col(accent, 1.f) : IM_COL32(178, 182, 195, 235);
                    dl->AddText(font, ts, tp, mainCol, title);
                    // 顶部高光（艺术字效果：上半段再画一遍浅色）
                    dl->PushClipRect(ImVec2(tp.x, tp.y), ImVec2(tp.x + tsize.x, tp.y + tsize.y * 0.55f), true);
                    dl->AddText(font, ts, tp, IM_COL32(255, 255, 255, on ? 110 : 60), title);
                    dl->PopClipRect();
                }

                // ---- 开关（右侧垂直居中）----
                bool v = value->load(std::memory_order_relaxed);
                const float swW = 64.f, swH = 34.f;
                ImVec2 swPos(pageX + pageW - 22.f - swW, cy + (cardH - swH) * 0.5f);
                ImGui::SetCursorScreenPos(swPos);
                if (ToggleSwitch(swId, &v, anim, swW, swH))
                {
                    value->store(v, std::memory_order_relaxed);
                    Log::Printf("[UI] toggle %s -> %d", swId, (int)v);
                }
            };

            FeatureCard(0, "##card1", "\xe4\xb8\x8d\xe6\xad\xbb\xe6\xa8\xa1\xe5\xbc\x8f",
                        &CheatState::NoDeath, &s_anim[0], kAccent, "##sw1");
            FeatureCard(1, "##card2", "\xe8\x87\xaa\xe5\x8a\xa8\xe8\xbf\x9e\xe5\x87\xbb",
                        &CheatState::AutoCombo, &s_anim[1], kAccent2, "##sw2");

            ImGui::PopClipRect();
            ImGui::SetCursorScreenPos(ImVec2(pageX, wpos.y + wsize.y - 46.f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.20f, 0.55f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.20f, 0.25f, 0.75f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.45f, 0.13f, 0.16f, 0.85f));
            if (ImGui::Button("\xe5\x8d\xb8\xe8\xbd\xbd\xe6\xa8\xa1\xe5\x9d\x97\xe5\xb9\xb6\xe8\xbf\x98\xe5\x8e\x9f", ImVec2(pageW, 30)))
                CheatState::ExitRequested.store(true, std::memory_order_relaxed);
            ImGui::PopStyleColor(3);
        }
        // ============ 页：4K 辅助 ============
        else if (s_page == 2)
        {
            ImGui::SetCursorScreenPos(ImVec2(pageX, pageY));
            Chart4K::DrawSettingsPage();
            ImGui::PopClipRect();
        }
        // ============ 页：6K 模式 ============
        else if (s_page == 3)
        {
            ImGui::SetCursorScreenPos(ImVec2(pageX, pageY));
            Chart4K::DrawSettings6KPage();
            ImGui::PopClipRect();
        }
        // ============ 页：实时状态 ============
        else
        {
            ImGui::SetCursorScreenPos(ImVec2(pageX, pageY));
            BeginCard("##cardS", contentH);
            {
                if (!st.bridgeReady)
                {
                    ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f),
                                       "\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5\xe6\xb8\xb8\xe6\x88\x8f...");
                }
                else if (!st.controllerAlive)
                {
                    ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f),
                                       "\xe6\x9c\xaa\xe6\xa3\x80\xe6\xb5\x8b\xe5\x88\xb0\xe5\x85\xb3\xe5\x8d\xa1\xef\xbc\x8c\xe8\xaf\xb7\xe8\xbf\x9b\xe5\x85\xa5\xe4\xbb\xbb\xe6\x84\x8f\xe5\x85\xb3\xe5\x8d\xa1");
                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_Text, Col(kTextDim));
                    ImGui::TextWrapped("\xe6\x8f\x90\xe7\xa4\xba\xef\xbc\x9a\xe5\xbc\x80\xe5\x85\xb3\xe5\xb7\xb2\xe5\x9c\xa8\xe5\x90\x8e\xe5\x8f\xb0\xe7\x94\x9f\xe6\x95\x88\xef\xbc\x8c\xe8\xbf\x9b\xe5\x85\xa5\xe5\x85\xb3\xe5\x8d\xa1\xe5\x90\x8e\xe8\x87\xaa\xe5\x8a\xa8\xe5\xba\x94\xe7\x94\xa8\xe3\x80\x82");
                    ImGui::PopStyleColor();
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, Col(kTextDim));
                    ImGui::TextUnformatted("\xe5\x85\xb3\xe5\x8d\xa1"); // 关卡
                    ImGui::PopStyleColor();
                    ImGui::TextWrapped("%s", st.levelName[0] ? st.levelName : "(\xe6\x9c\xaa\xe7\x9f\xa5)");

                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_Text, Col(kTextDim));
                    ImGui::TextUnformatted("\xe7\x8a\xb6\xe6\x80\x81"); // 状态
                    ImGui::PopStyleColor();
                    ImGui::TextUnformatted(st.stateName);
                    ImGui::SameLine();
                    if (st.gameworld)
                        ImGui::TextColored(kAccent, "\xc2\xb7 \xe6\xb8\xb8\xe6\x88\x8f\xe4\xb8\xad");

                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_Text, Col(kTextDim));
                    ImGui::TextUnformatted("\xe8\xbf\x9b\xe5\xba\xa6"); // 进度
                    ImGui::PopStyleColor();
                    char pct[32];
                    snprintf(pct, sizeof(pct), "%.1f%%", st.percentComplete * 100.f);
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Col(kAccent, 0.55f));
                    ImGui::ProgressBar(st.percentComplete, ImVec2(-1, 8), "");
                    ImGui::PopStyleColor();
                    ImGui::Text("\xe7\xac\xac %d \xe5\x9d\x97  \xc2\xb7  %s", st.floorIndex, pct);

                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, Col(kTextDim));
                    ImGui::TextUnformatted("\xe7\xb2\xbe\xe5\x87\x86\xe5\xba\xa6"); // 精准度
                    ImGui::PopStyleColor();
                    ImGui::PushFont(nullptr, 26.f);
                    ImGui::TextColored(ImVec4(kAccent.x, kAccent.y, kAccent.z, 1.f), "%.2f%%", st.percentAcc * 100.f);
                    ImGui::PopFont();
                    ImGui::Text("XAcc:  %.2f%%", st.percentXAcc * 100.f);
                    ImGui::Text("\xe6\xad\xbb\xe4\xba\xa1: %d      \xe6\xa3\x80\xe6\x9f\xa5\xe7\x82\xb9: %d", st.deaths, st.checkpoints);
                }
            }
            EndCard();

            ImGui::PopClipRect();
            ImGui::SetCursorPos(ImVec2(12.f, wsize.y - 34.f));
            ImGui::PushStyleColor(ImGuiCol_Text, Col(kTextDim, 0.8f));
            ImGui::TextDisabled("Insert \xe6\x98\xbe\xe9\x9a\x90  \xc2\xb7  End \xe5\x8d\xb8\xe8\xbd\xbd  \xc2\xb7  \xe5\x8f\xaf\xe6\x8b\x96\xe5\x8a\xa8\xe6\xa0\x87\xe9\xa2\x98\xe6\xa0\x8f");
            ImGui::PopStyleColor();
        }

        // ============ 右下角缩放柄（拖动 = 改窗口尺寸，控件随宽度自适应） ============
        {
            const float gs = 16.f;
            ImVec2 gp(wpos.x + wsize.x - gs - 3.f, wpos.y + wsize.y - gs - 3.f);
            ImGui::SetCursorScreenPos(gp);
            ImGui::InvisibleButton("##wndresize", ImVec2(gs, gs));
            bool hov = ImGui::IsItemHovered() || ImGui::IsItemActive();
            if (hov)
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
            if (ImGui::IsItemActive())
            {
                s_mainSize.x += ImGui::GetIO().MouseDelta.x;
                s_mainSize.y += ImGui::GetIO().MouseDelta.y;
                if (s_mainSize.x < 380.f)  s_mainSize.x = 380.f;
                if (s_mainSize.x > 1000.f) s_mainSize.x = 1000.f;
                if (s_mainSize.y < 340.f)  s_mainSize.y = 340.f;
                if (s_mainSize.y > 900.f)  s_mainSize.y = 900.f;
            }
            for (int i = 0; i < 3; i++)
            {
                float o = 3.f + i * 4.5f;
                dl->AddLine(ImVec2(gp.x + gs - o, gp.y + gs - 2.f),
                            ImVec2(gp.x + gs - 2.f, gp.y + gs - o),
                            IM_COL32(230, 235, 245, hov ? 170 : 80), 1.4f);
            }
        }
        ImGui::End();

        ImGui::PopStyleVar(); // WindowBorderSize
    }
    static void DrawDot()
    {
        ImGui::SetNextWindowPos(s_dotPos, ImGuiCond_Always);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##adofai_dot", nullptr, flags);

        ImVec2 wpos = ImGui::GetWindowPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float t = (float)ImGui::GetTime();

        // 可点击区域（略大于圆）
        ImGui::InvisibleButton("##dotbtn", ImVec2(34, 34));
        bool hovered = ImGui::IsItemHovered();
        bool active = ImGui::IsItemActive();

        // 拖动
        if (active && ImGui::IsMouseDragging(0))
            s_dotPos = ImVec2(s_dotPos.x + ImGui::GetIO().MouseDelta.x,
                              s_dotPos.y + ImGui::GetIO().MouseDelta.y);

        // 中心
        ImVec2 c(wpos.x + 17, wpos.y + 17);
        float pulse = 0.5f + 0.5f * sinf(t * 2.6f);

        // 外圈呼吸
        dl->AddCircle(c, 15.f + pulse * 2.5f, Col(kAccent, hovered ? 0.85f : 0.45f), 0, 2.f);
        // 主体
        dl->AddCircleFilled(c, 11.f, Col(kAccent, 0.92f));
        dl->AddCircleFilled(c, 4.5f, IM_COL32(12, 14, 22, 235));

        // 点击（未拖动）展开
        static ImVec2 pressPos(0, 0);
        if (ImGui::IsItemActivated())
            pressPos = ImGui::GetIO().MousePos;
        if (ImGui::IsItemDeactivated())
        {
            ImVec2 mp = ImGui::GetIO().MousePos;
            float d = fabsf(mp.x - pressPos.x) + fabsf(mp.y - pressPos.y);
            if (d < 6.f)
            {
                s_collapsed = false;
                s_mainPos = ImVec2(s_dotPos.x - 30.f, s_dotPos.y - 17.f);
            }
        }

        ImGui::End();
        ImGui::PopStyleVar();
    }

    void Draw()
    {
        Chart4K::DrawPlayfield(); // 4K 谱面独立窗口（不受菜单显隐影响）
        if (!CheatState::MenuVisible.load(std::memory_order_relaxed))
            return;
        if (s_collapsed)
            DrawDot();
        else
            DrawMain();
    }
}
