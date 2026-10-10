// ============================================================
// UiKit.cpp - 自绘控件库实现（Etherium Menu 风格，全量重写版）
// ============================================================
#include "UiKit.h"
#include "Lang.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>
#include <utility>

namespace UiKit
{
namespace
{
    Theme g_th{};
    int   g_palette = 0;

    // ------------------------------------------------------------------
    //  幽灵点击过滤
    //    ImGui 在 ActiveId 残留 / 输入被重放 / 从未按下过左键的情况下，
    //    也可能让 InvisibleButton 报出 pressed（表现为“没点也触发”）。
    //    这里要求：本次“按下”的落点确实落在这个控件的矩形里，才算点击。
    // ------------------------------------------------------------------
    bool ClickLanded(const ImVec2& p, const ImVec2& sz)
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.MouseClickedTime[0] <= 0.f) return false;   // 从未按下过左键
        const ImVec2 c = io.MouseClickedPos[0];
        return c.x >= p.x - 3.f && c.x <= p.x + sz.x + 3.f &&
               c.y >= p.y - 3.f && c.y <= p.y + sz.y + 3.f;
    }
    bool InvClick(const char* id, const ImVec2& size)      // = InvisibleButton + 过滤
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::InvisibleButton(id, size);
        if (clicked) clicked = ClickLanded(p, size);
        return clicked;
    }

    struct PaletteDef { const char* name; ImU32 accent; };
    // 「界面风格」：只换强调色，灰黑基底与 Etherium 保持一致
    const PaletteDef kPalettes[] = {
        { "灰蓝 Slate",  IM_COL32(149, 173, 181, 255) },   // = Etherium 默认
        { "橄榄 Olive",  IM_COL32(172, 187, 120, 255) },
        { "青苔 Mint",   IM_COL32( 92, 200, 168, 255) },
        { "深海 Azure",  IM_COL32( 96, 160, 226, 255) },
        { "暖粉 Rose",   IM_COL32(226, 124, 170, 255) },
        { "琥珀 Amber",  IM_COL32(226, 174, 104, 255) },
    };
    const int kPaletteN = (int)(sizeof(kPalettes) / sizeof(kPalettes[0]));

    void BuildTheme(ImU32 accent)
    {
        g_th.accent     = accent;
        g_th.accentDim  = A(accent, 0.22f);
        g_th.accentLine = A(accent, 0.62f);

        g_th.winBg       = IM_COL32( 12,  12,  16, 216);   // Etherium WindowBg
        g_th.sideBg      = IM_COL32( 12,  12,  16, 148);   // Etherium 侧栏
        g_th.topBg       = IM_COL32( 12,  12,  16, 148);   // Etherium 顶栏
        g_th.tabBg       = IM_COL32( 34,  33,  34, 205);   // Etherium 选中 Tab
        g_th.sideSel     = g_th.tabBg;
        g_th.subTabBg    = IM_COL32( 34,  34,  38, 225);   // Etherium 子 Tab
        g_th.cardBg      = IM_COL32( 11,  12,  13, 127);   // Etherium ChildBg
        g_th.headBg      = IM_COL32( 17,  17,  19, 190);
        g_th.rowBg       = IM_COL32( 24,  25,  27, 255);   // Etherium config_selector
        g_th.rowBgHover  = IM_COL32( 34,  35,  37, 255);
        g_th.rowBgSel    = IM_COL32( 14,  15,  17, 255);
        g_th.frame       = IM_COL32( 31,  32,  35, 255);
        g_th.frameHover  = IM_COL32( 44,  45,  46, 255);
        g_th.line        = IM_COL32( 52,  52,  52, 255);   // Etherium Border
        g_th.text        = IM_COL32(255, 255, 255, 255);
        g_th.textDim     = IM_COL32(175, 175, 175, 255);
        g_th.textFaint   = IM_COL32(128, 128, 128, 255);   // Etherium text_inactive
        g_th.textOn      = IM_COL32(255, 255, 255, 255);
        g_th.good        = IM_COL32(150, 205, 155, 255);
        g_th.warn        = IM_COL32(226, 178, 112, 255);
        g_th.bad         = IM_COL32(216, 104, 104, 255);
        g_th.track       = IM_COL32( 58,  58,  58,  70);   // Etherium checkbox 槽
        g_th.knob        = IM_COL32( 58,  58,  58, 255);   // Etherium checkbox 圆点
        g_th.popBg       = IM_COL32( 11,  12,  13, 250);
    }

    struct ScopedId
    {
        explicit ScopedId(const char* id) { ImGui::PushID(id ? id : "##ui"); }
        ~ScopedId() { ImGui::PopID(); }
    };

    inline float FontH() { return ImGui::GetFontSize(); }
    inline float RegionW() { return ImGui::GetContentRegionAvail().x; }
    inline float ComboH() { return FontH() + 10.f; }

    // 控件缓动动画（自维护表，避免依赖 imgui_internal）
    float Anim(ImGuiID id, float target, float speed)
    {
        static std::vector<std::pair<ImGuiID, float>> s_anims;
        float cur = target;
        bool found = false;
        for (auto& kv : s_anims)
        {
            if (kv.first == id)
            {
                kv.second += (target - kv.second) * (1.f - expf(-ImGui::GetIO().DeltaTime * speed));
                cur = kv.second;
                if (fabsf(target - cur) < 0.002f) { cur = target; kv.second = target; }
                found = true;
                break;
            }
        }
        if (!found) s_anims.emplace_back(id, target);
        return cur;
    }

    // Etherium 开关 / 复选框胶囊：32x16，圆点直径 = h-6
    void DrawCapsule(ImDrawList* dl, ImVec2 p, float w, float h, float t, bool hover)
    {
        ImU32 track = Mix(g_th.track, A(g_th.accent, 0.30f), t);
        ImU32 knobC = Mix(g_th.knob, g_th.accent, t);
        if (hover)
        {
            track = Mix(track, IM_COL32(255, 255, 255, 255), 0.06f);
            knobC = Mix(knobC, IM_COL32(255, 255, 255, 255), 0.16f);
        }
        if (track & IM_COL32_A_MASK)
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), track, h * 0.5f);
        const float kd = h - 6.f;
        const float kx = p.x + 3.f + t * (w - 6.f - kd);
        dl->AddCircleFilled(ImVec2(kx + kd * 0.5f, p.y + h * 0.5f), kd * 0.5f, knobC);
    }

    // 一行「标签 + 右侧控件」的定位辅助
    inline void RowLabel(ImDrawList* dl, ImVec2 p, const char* label, float rowH, bool on, bool hover)
    {
        if (!label || !label[0]) return;
        ImU32 c = on ? g_th.text : (hover ? g_th.textDim : g_th.textFaint);
        dl->AddText(ImVec2(p.x, p.y + (rowH - FontH()) * 0.5f), c, label);
    }
} // namespace

Theme& Th() { return g_th; }
int PaletteCount() { return kPaletteN; }
const char* PaletteName(int i) { return (i >= 0 && i < kPaletteN) ? kPalettes[i].name : ""; }
int Palette() { return g_palette; }

void SetPalette(int i, bool persist)
{
    if (i < 0 || i >= kPaletteN) i = 0;
    g_palette = i;
    BuildTheme(kPalettes[i].accent);
    if (persist)
    {
        I18N::Prefs::SetInt("ui.palette", i);
        I18N::Prefs::Save();
    }
}

ImU32 A(ImU32 c, float mul)
{
    int a = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * mul);
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return (c & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
}

ImU32 Mix(ImU32 a, ImU32 b, float t)
{
    if (t <= 0.f) return a;
    if (t >= 1.f) return b;
    const int r = (int)(((a >> IM_COL32_R_SHIFT) & 0xFF) + (((b >> IM_COL32_R_SHIFT) & 0xFF) - ((a >> IM_COL32_R_SHIFT) & 0xFF)) * t);
    const int g = (int)(((a >> IM_COL32_G_SHIFT) & 0xFF) + (((b >> IM_COL32_G_SHIFT) & 0xFF) - ((a >> IM_COL32_G_SHIFT) & 0xFF)) * t);
    const int bl = (int)(((a >> IM_COL32_B_SHIFT) & 0xFF) + (((b >> IM_COL32_B_SHIFT) & 0xFF) - ((a >> IM_COL32_B_SHIFT) & 0xFF)) * t);
    const int al = (int)(((a >> IM_COL32_A_SHIFT) & 0xFF) + (((b >> IM_COL32_A_SHIFT) & 0xFF) - ((a >> IM_COL32_A_SHIFT) & 0xFF)) * t);
    return IM_COL32(r, g, bl, al);
}

ImVec4 V4(ImU32 c)
{
    return ImVec4(((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.f, ((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.f,
                  ((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.f, ((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.f);
}

// ============================================================
//  全局样式（每帧幂等）
// ============================================================
void ApplyGlobalStyle()
{
    if (g_th.accent == 0)
    {
        int p = I18N::Prefs::GetInt("ui.palette", 0);
        if (p < 0 || p >= kPaletteN) p = 0;
        g_palette = p;
        BuildTheme(kPalettes[p].accent);
    }

    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 12.f;      // Etherium
    s.ChildRounding     = 6.f;
    s.FrameRounding     = 4.f;
    s.PopupRounding     = 8.f;
    s.ScrollbarRounding = 6.f;
    s.GrabRounding      = 4.f;
    s.TabRounding       = 6.f;
    s.WindowBorderSize  = 0.f;
    s.ChildBorderSize   = 1.f;
    s.FrameBorderSize   = 1.f;
    s.WindowPadding     = ImVec2(0.f, 0.f);
    s.ChildBorderSize   = 1.f;
    s.FramePadding      = ImVec2(10.f, 4.f);
    s.ItemSpacing       = ImVec2(8.f, 6.f);
    s.ItemInnerSpacing  = ImVec2(8.f, 5.f);
    s.ScrollbarSize     = 10.f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]        = V4(g_th.winBg);
    c[ImGuiCol_ChildBg]         = V4(g_th.cardBg);
    c[ImGuiCol_PopupBg]         = V4(g_th.popBg);
    c[ImGuiCol_Border]          = V4(g_th.line);
    c[ImGuiCol_BorderShadow]    = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Text]            = V4(g_th.text);
    c[ImGuiCol_TextDisabled]    = V4(g_th.textFaint);
    c[ImGuiCol_FrameBg]         = V4(g_th.frame);
    c[ImGuiCol_FrameBgHovered]  = V4(g_th.frameHover);
    c[ImGuiCol_FrameBgActive]   = V4(g_th.frameHover);
    c[ImGuiCol_TitleBg]         = V4(g_th.winBg);
    c[ImGuiCol_TitleBgActive]   = V4(g_th.winBg);
    c[ImGuiCol_MenuBarBg]       = V4(g_th.winBg);
    c[ImGuiCol_ScrollbarBg]     = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]   = V4(A(g_th.line, 0.85f));
    c[ImGuiCol_ScrollbarGrabHovered] = V4(g_th.textFaint);
    c[ImGuiCol_ScrollbarGrabActive]  = V4(g_th.accent);
    c[ImGuiCol_CheckMark]       = V4(g_th.accent);
    c[ImGuiCol_SliderGrab]      = V4(g_th.accent);
    c[ImGuiCol_SliderGrabActive]= V4(Mix(g_th.accent, IM_COL32(255,255,255,255), 0.25f));
    c[ImGuiCol_Button]          = V4(g_th.rowBg);
    c[ImGuiCol_ButtonHovered]   = V4(g_th.rowBgHover);
    c[ImGuiCol_ButtonActive]    = V4(A(g_th.accent, 0.45f));
    c[ImGuiCol_Header]          = V4(g_th.rowBgHover);
    c[ImGuiCol_HeaderHovered]   = V4(A(g_th.accent, 0.40f));
    c[ImGuiCol_HeaderActive]    = V4(A(g_th.accent, 0.55f));
    c[ImGuiCol_Separator]       = V4(A(g_th.line, 0.75f));
    c[ImGuiCol_SeparatorHovered]= V4(g_th.accent);
    c[ImGuiCol_SeparatorActive] = V4(g_th.accent);
    c[ImGuiCol_ResizeGrip]      = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TextSelectedBg]  = V4(A(g_th.accent, 0.35f));
    c[ImGuiCol_NavHighlight]    = V4(g_th.accent);
}

// ============================================================
//  文本
// ============================================================
void Text(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    ImGui::TextV(fmt, ap);
    va_end(ap);
}
void TextCol(ImU32 c, const char* fmt, ...)
{
    char b[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(p, c, b);
    ImGui::Dummy(ImGui::CalcTextSize(b));
}
void TextDim(const char* fmt, ...)
{
    char b[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(p, g_th.textDim, b);
    ImGui::Dummy(ImGui::CalcTextSize(b));
}
void TextFaint(const char* fmt, ...)
{
    char b[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(p, g_th.textFaint, b);
    ImGui::Dummy(ImGui::CalcTextSize(b));
}
void TextWrap(const char* fmt, ...)
{
    char b[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextUnformatted(b);
    ImGui::PopTextWrapPos();
}
void TextWrapCol(ImU32 c, const char* fmt, ...)
{
    char b[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    ImGui::PushStyleColor(ImGuiCol_Text, V4(c));
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextUnformatted(b);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}
void TextBig(float size, ImU32 c, const char* text)
{
    if (!text) return;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), size, p, c, text);
    const float w = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.f, text).x;
    ImGui::Dummy(ImVec2(w, size));
}
float TextBigW(float size, const char* text)
{
    if (!text) return 0.f;
    return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.f, text).x;
}
void Space(float h) { if (h > 0.f) ImGui::Dummy(ImVec2(0.f, h)); }

void Separator(float padY)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = RegionW();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + padY), ImVec2(p.x + w, p.y + padY),
                                        A(g_th.line, 0.7f), 1.f);
    ImGui::Dummy(ImVec2(w, padY * 2.f + 1.f));
}

void Section(const char* text)
{
    if (text && text[0])
        ImGui::GetWindowDrawList()->AddText(ImGui::GetCursorScreenPos(), g_th.textFaint, text);
    ImGui::Dummy(ImVec2(RegionW(), FontH() + 2.f));
}

void LabelRow(const char* label, float colX)
{
    const char* t = label ? label : "";
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 ts = ImGui::CalcTextSize(t);
    // 用真实 item 占位，保证行布局与 ImGui 光标推进正确
    ImGui::GetWindowDrawList()->AddText(p, g_th.textDim, t);
    ImGui::Dummy(ts);
    ImGui::SameLine();
    if (colX > 0.f && colX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(colX);
}

float LabelCol(std::initializer_list<const char*> labels, float minW)
{
    float w = minW;
    for (const char* s : labels)
    {
        if (!s) continue;
        float t = ImGui::CalcTextSize(s).x + 14.f;
        if (t > w) w = t;
    }
    return w;
}

// ============================================================
//  分组面板（Etherium child：圆角 + 半透明底 + 描边）
// ============================================================
void BeginCard(const char* id)
{
    ImGui::PushID(id ? id : "##card");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPadX, kPadY));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 9.f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, V4(g_th.cardBg));
    ImGui::BeginChild("##c", ImVec2(0.f, 0.f),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding |
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
}
void EndCard()
{
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::PopID();
}
bool CardTitle(const char* title, bool* toggle)
{
    ImGui::PushID(title ? title : "##ct");
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = RegionW();
    if (w < 40.f) w = 40.f;
    const float h = 22.f;
    bool clicked = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (title && title[0])
    {
        ImFont* f = ImGui::GetFont();
        dl->AddText(f, 17.f, ImVec2(p.x, p.y + (h - 17.f) * 0.5f), g_th.text, title);
    }
    if (toggle)
    {
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - 32.f, p.y + (h - 16.f) * 0.5f));
        clicked = Toggle("##ctt", toggle);
    }
    else
    {
        ImGui::Dummy(ImVec2(w, h));
    }
    dl->AddLine(ImVec2(p.x, p.y + h + 5.f), ImVec2(p.x + w, p.y + h + 5.f), A(g_th.line, 0.55f), 1.f);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 11.f));
    ImGui::PopID();
    return clicked;
}
void CardHint(const char* text)
{
    if (!text || !text[0]) return;
    TextWrapCol(g_th.textFaint, text);
}
// ============================================================
//  导航（侧栏 Tab / 子 Tab）
// ============================================================
bool Tab(const char* id, int icon, const char* label, bool selected, ImVec2 size)
{
    ScopedId sid(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool clicked = InvClick("##tab", size);
    bool hovered = ImGui::IsItemHovered();
    bool held    = ImGui::IsItemActive();

    const float t = Anim(ImGui::GetID("##tabbg"), selected ? 1.f : 0.f, 12.f);
    ImU32 bg = 0;
    if (t > 0.004f)                       bg = A(g_th.tabBg, t);
    else if (held)                        bg = A(g_th.tabBg, 0.75f);
    else if (hovered)                     bg = A(g_th.tabBg, 0.45f);
    if (bg & IM_COL32_A_MASK)
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, 6.f);

    // Etherium：图标 21x21 在左 7px，文字在 x+35
    ImU32 ic = selected ? IM_COL32(255, 255, 255, 255)
                        : (hovered ? g_th.textDim : IM_COL32(136, 136, 137, 255));
    Icon(dl, icon, ImVec2(p.x + 17.5f, p.y + size.y * 0.5f), 8.5f, ic);
    dl->AddText(ImVec2(p.x + 35.f, p.y + (size.y - FontH()) * 0.5f), ic, label ? label : "");
    return clicked;
}

bool NavItem(const char* id, int icon, const char* label, bool selected)
{
    return Tab(id, icon, label, selected, ImVec2(RegionW(), kTabH));
}

bool SubTab(const char* id, const char* label, bool selected, ImVec2 size)
{
    ScopedId sid(id);
    const char* t = label ? label : "";
    ImVec2 ts = ImGui::CalcTextSize(t);
    if (size.x <= 0.f) size.x = ts.x + 30.f;
    if (size.y <= 0.f) size.y = 30.f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool clicked = InvClick("##subtab", size);
    bool hovered = ImGui::IsItemHovered();
    bool held    = ImGui::IsItemActive();

    float a = Anim(ImGui::GetID("##subbg"), selected ? 1.f : 0.f, 12.f);
    ImU32 bg = 0;
    if (a > 0.004f)       bg = A(g_th.subTabBg, a);
    else if (held)        bg = A(g_th.subTabBg, 0.75f);
    else if (hovered)     bg = A(g_th.subTabBg, 0.45f);
    if (bg & IM_COL32_A_MASK)
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, 6.f);
    if (selected)
        dl->AddRectFilled(ImVec2(p.x + 7.f, p.y + size.y - 3.f),
                          ImVec2(p.x + size.x - 7.f, p.y + size.y - 1.f), g_th.accent, 1.f);

    ImU32 tc = selected ? IM_COL32(255, 255, 255, 255)
                        : (hovered ? g_th.textDim : IM_COL32(138, 138, 140, 255));
    dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f - 1.f), tc, t);
    return clicked;
}

// ============================================================
//  控件
// ============================================================
bool Toggle(const char* id, bool* v, float w, float h)
{
    ScopedId sid(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool clicked = InvClick("##tg", ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    if (clicked) *v = !*v;
    float t = Anim(ImGui::GetID("##tganim"), *v ? 1.f : 0.f, 16.f);
    DrawCapsule(dl, p, w, h, t, hovered);
    return clicked;
}

bool CheckRow(const char* id, const char* label, bool* v, const char* sub)
{
    ScopedId sid(id);
    float w = RegionW();
    if (w < 60.f) w = 60.f;
    const float cw = 32.f, ch = 16.f;
    const bool two = (sub && sub[0]);
    const float rowH = two ? 38.f : 24.f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool clicked = InvClick("##ck", ImVec2(w, rowH));
    bool hovered = ImGui::IsItemHovered();
    if (clicked) *v = !*v;

    float t = Anim(ImGui::GetID("##ckanim"), *v ? 1.f : 0.f, 16.f);
    ImU32 tc = *v ? g_th.text : (hovered ? g_th.textDim : g_th.textFaint);
    if (label && label[0])
    {
        if (two)
        {
            dl->AddText(ImVec2(p.x, p.y + 3.f), tc, label);
            dl->AddText(ImVec2(p.x, p.y + 4.f + FontH()), g_th.textFaint, sub);
        }
        else
        {
            dl->AddText(ImVec2(p.x, p.y + (rowH - FontH()) * 0.5f), tc, label);
        }
    }
    else if (two)
    {
        dl->AddText(ImVec2(p.x, p.y + (rowH - FontH()) * 0.5f), g_th.textFaint, sub);
    }
    DrawCapsule(dl, ImVec2(p.x + w - cw, p.y + (rowH - ch) * 0.5f), cw, ch, t, hovered);
    return clicked;
}

bool Checkbox(const char* id, bool* v, const char* label) { return CheckRow(id, label, v, nullptr); }

bool SliderInt(const char* id, int* v, int lo, int hi, const char* fmt, float width)
{
    ScopedId sid(id);
    if (width <= 0.f) width = RegionW();
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, V4(g_th.frame));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 4.f);
    bool ch = ImGui::SliderInt("##sl", v, lo, hi, fmt ? fmt : "%d");
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    return ch;
}

bool SliderFloat(const char* id, float* v, float lo, float hi, const char* fmt, float width)
{
    ScopedId sid(id);
    if (width <= 0.f) width = RegionW();
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, V4(g_th.frame));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 4.f);
    bool ch = ImGui::SliderFloat("##sl", v, lo, hi, fmt ? fmt : "%.2f");
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    return ch;
}

// 标签在左、滑条在右的一整行
bool SliderIntRow(const char* id, const char* label, int* v, int lo, int hi, const char* fmt)
{
    ImGui::PushID(id ? id : "##slr");
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = RegionW();
    if (w < 120.f) w = 120.f;
    const float sw = (w * 0.55f < 160.f) ? 160.f : w * 0.55f;
    const float sh = FontH() + 10.f;
    ImGui::Dummy(ImVec2(w, sh));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (label && label[0])
        dl->AddText(ImVec2(p.x, p.y + (sh - FontH()) * 0.5f), g_th.textDim, label);
    bool ch = false;
    if (sw < w)
    {
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - sw, p.y));
        ch = SliderInt("##v", v, lo, hi, fmt, sw);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + sh + ImGui::GetStyle().ItemSpacing.y));
    ImGui::PopID();
    return ch;
}

bool Combo(const char* id, int* v, const char* const* items, int n, float width)
{
    ScopedId sid(id);
    if (n <= 0 || !items) return false;
    if (*v < 0) *v = 0;
    if (*v >= n) *v = n - 1;
    if (width <= 0.f) width = RegionW();

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.f, 4.f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.f, 6.f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, V4(g_th.frame));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, V4(g_th.frameHover));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, V4(g_th.frame));
    ImGui::PushStyleColor(ImGuiCol_Border, V4(A(g_th.line, 0.9f)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, V4(IM_COL32(11, 12, 13, 252)));
    ImGui::PushStyleColor(ImGuiCol_Header, V4(g_th.rowBgHover));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, V4(A(g_th.accent, 0.35f)));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, V4(A(g_th.accent, 0.5f)));

    ImGui::SetNextItemWidth(width);
    bool changed = false;
    if (ImGui::BeginCombo("##cb", items[*v]))
    {
        for (int i = 0; i < n; i++)
        {
            const bool sel = (i == *v);
            if (ImGui::Selectable(items[i], sel)) { *v = i; changed = true; }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PopStyleColor(8);
    ImGui::PopStyleVar(5);
    return changed;
}

bool ComboRow(const char* id, const char* label, int* v, const char* const* items, int n)
{
    ImGui::PushID(id ? id : "##cbr");
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = RegionW();
    if (w < 120.f) w = 120.f;
    const float cw = (w * 0.45f < 150.f) ? 150.f : w * 0.45f;
    const float sh = ComboH();
    ImGui::Dummy(ImVec2(w, sh));
    if (label && label[0])
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x, p.y + (sh - FontH()) * 0.5f),
                                            g_th.textDim, label);
    bool ch = false;
    if (cw < w)
    {
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - cw, p.y));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.f, (sh - FontH()) * 0.5f - 1.f));
        ch = Combo("##v", v, items, n, cw);
        ImGui::PopStyleVar();
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + sh + ImGui::GetStyle().ItemSpacing.y));
    ImGui::PopID();
    return ch;
}

bool Button(const char* id, const char* label, ImVec2 size, int variant)
{
    ScopedId sid(id);
    float w = (size.x < 0.f) ? RegionW() : size.x;
    float h = (size.y <= 0.f) ? 28.f : size.y;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool clicked = InvClick("##btn", ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    bool held    = ImGui::IsItemActive();

    ImU32 bg;
    ImU32 tc = g_th.text;
    ImU32 bd = 0;
    switch (variant)
    {
    case BTN_PRIMARY:
        bg = A(g_th.accent, held ? 0.55f : (hovered ? 0.42f : 0.28f));
        bd = A(g_th.accent, 0.75f);
        break;
    case BTN_DANGER:
        bg = IM_COL32(held ? 150 : (hovered ? 138 : 116), held ? 58 : (hovered ? 52 : 44),
                      held ? 62 : (hovered ? 56 : 48), 235);
        bd = IM_COL32(190, 90, 96, 200);
        break;
    case BTN_GHOST:
        bg = held ? IM_COL32(255, 255, 255, 26) : (hovered ? IM_COL32(255, 255, 255, 16) : 0);
        bd = A(g_th.line, 0.75f);
        break;
    default:
        bg = held ? g_th.rowBgHover : (hovered ? g_th.rowBgHover : g_th.rowBg);
        bd = A(g_th.line, 0.9f);
        break;
    }
    if (bg & IM_COL32_A_MASK)
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, 4.f);
    if (bd & IM_COL32_A_MASK)
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), bd, 4.f);

    const char* t = label ? label : "";
    ImVec2 ts = ImGui::CalcTextSize(t);
    dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), tc, t);
    return clicked;
}

bool IconButton(const char* id, int icon, ImVec2 size, int variant)
{
    ScopedId sid(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool clicked = InvClick("##icobtn", size);
    bool hovered = ImGui::IsItemHovered();
    bool held    = ImGui::IsItemActive();
    ImU32 bg = (variant == BTN_PRIMARY) ? A(g_th.accent, held ? 0.5f : (hovered ? 0.34f : 0.22f))
                                        : (held ? IM_COL32(255, 255, 255, 32)
                                                : (hovered ? IM_COL32(255, 255, 255, 18) : 0));
    if (bg & IM_COL32_A_MASK)
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, 5.f);
    Icon(dl, icon, ImVec2(p.x + size.x * 0.5f, p.y + size.y * 0.5f), size.y * 0.34f,
         hovered ? g_th.text : g_th.textDim);
    return clicked;
}

bool InputText(const char* id, char* buf, size_t n, const char* hint, float width, bool enterReturns)
{
    ScopedId sid(id);
    if (width <= 0.f) width = RegionW();
    const float h = FontH() + 12.f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGui::PushStyleColor(ImGuiCol_FrameBg, V4(g_th.frame));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, V4(g_th.frameHover));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, V4(g_th.frame));
    ImGui::PushStyleColor(ImGuiCol_Border, V4(A(g_th.line, 0.9f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.f, (h - FontH()) * 0.5f - 1.f));
    ImGui::SetCursorScreenPos(p);
    ImGui::SetNextItemWidth(width);
    const ImGuiInputTextFlags f = enterReturns ? ImGuiInputTextFlags_EnterReturnsTrue : 0;
    bool changed = false;
    if (hint && hint[0]) changed = ImGui::InputTextWithHint("##it", hint, buf, n, f);
    else                 changed = ImGui::InputText("##it", buf, n, f);
    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();
    if (hovered || active)
        dl->AddRect(p, ImVec2(p.x + width, p.y + h), active ? g_th.accent : A(g_th.line, 0.95f), 4.f);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
    return changed;
}

bool KeyBox(const char* id, const char* label, bool active, ImVec2 size)
{
    ScopedId sid(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool clicked = InvClick("##kb", size);
    bool hovered = ImGui::IsItemHovered();
    ImU32 bg = active ? A(g_th.accent, 0.30f) : (hovered ? g_th.rowBgHover : g_th.rowBg);
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, 4.f);
    dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y),
                active ? g_th.accent : A(g_th.line, 0.9f), 4.f);
    const char* t = label ? label : "";
    ImVec2 ts = ImGui::CalcTextSize(t);
    dl->PushClipRect(p, ImVec2(p.x + size.x, p.y + size.y), true);
    dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f),
                active ? g_th.text : g_th.textDim, t);
    dl->PopClipRect();
    return clicked;
}

bool CardAction(const char* id, const char* title, const char* sub, float h,
                bool selected, bool enabled, float rightW)
{
    ScopedId sid(id);
    float w = RegionW();
    if (w < 60.f) w = 60.f;
    float clickW = w - rightW;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool clicked = InvClick("##ca", ImVec2(clickW, h));
    bool hovered = ImGui::IsItemHovered() && enabled;
    bool held    = ImGui::IsItemActive() && enabled;
    if (!enabled) clicked = false;

    ImU32 bg = selected ? g_th.rowBgSel : g_th.rowBg;
    if (hovered || held) bg = selected ? Mix(g_th.rowBgSel, g_th.rowBgHover, 0.8f) : g_th.rowBgHover;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, 6.f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), selected ? g_th.accentLine : A(g_th.line, 0.85f), 6.f);
    if (selected)
        dl->AddRectFilled(ImVec2(p.x + 1.f, p.y + 8.f), ImVec2(p.x + 3.f, p.y + h - 8.f),
                          g_th.accent, 1.f);

    const char* t = title ? title : "";
    const char* s = sub ? sub : "";
    ImFont* f = ImGui::GetFont();
    if (!enabled)
    {
        f->CalcTextSizeA(17.f, FLT_MAX, 0.f, t);
    }
    const float th = 17.f;
    const float sh = FontH();
    if (s[0])
    {
        const float total = th + 3.f + sh;
        const float y0 = p.y + (h - total) * 0.5f;
        dl->AddText(f, th, ImVec2(p.x + 15.f, y0), enabled ? g_th.text : g_th.textDim, t);
        dl->AddText(ImVec2(p.x + 15.f, y0 + th + 3.f), g_th.textFaint, s);
    }
    else
    {
        dl->AddText(f, th, ImVec2(p.x + 15.f, p.y + (h - th) * 0.5f),
                    enabled ? g_th.text : g_th.textDim, t);
    }
    const float cxp = p.x + w - 18.f;
    Icon(dl, IC_CHEVRON_R, ImVec2(cxp, p.y + h * 0.5f), 5.5f,
         hovered ? g_th.text : g_th.textFaint);
    return clicked;
}

bool CardActionToggle(const char* id, const char* title, const char* sub, bool* v, float h)
{
    ScopedId sid(id);
    const float cw = 32.f, chh = 16.f;
    float w = RegionW();
    if (w < 60.f) w = 60.f;
    (void)h;

    const float rowH = 62.f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool clicked = InvClick("##cat", ImVec2(w, rowH));
    bool hovered = ImGui::IsItemHovered();
    if (clicked) *v = !*v;

    bool on = *v;
    ImU32 bg = on ? Mix(g_th.rowBg, A(g_th.accent, 0.30f), 0.30f)
                  : (hovered ? g_th.rowBgHover : g_th.rowBg);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH), bg, 6.f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + rowH), on ? g_th.accentLine : A(g_th.line, 0.85f), 6.f);

    const float tw = w - 15.f - cw - 18.f;
    ImFont* f = ImGui::GetFont();
    const float th = 17.f;
    const float sy = (title && title[0] && sub && sub[0]) ? (rowH - th - 3.f - FontH()) * 0.5f
                                                          : (rowH - th) * 0.5f;
    if (title && title[0])
        dl->AddText(f, th, ImVec2(p.x + 15.f, p.y + sy), g_th.text, title);
    if (sub && sub[0])
    {
        ImVec2 ss = ImGui::CalcTextSize(sub);
        if (ss.x > tw)
            dl->AddText(ImVec2(p.x + 15.f, p.y + sy + th + 3.f), g_th.textFaint, sub);
        else
            dl->AddText(ImVec2(p.x + 15.f, p.y + sy + th + 3.f), g_th.textFaint, sub);
    }
    float t = Anim(ImGui::GetID("##catanim"), on ? 1.f : 0.f, 16.f);
    DrawCapsule(dl, ImVec2(p.x + w - cw - 15.f, p.y + (rowH - chh) * 0.5f), cw, chh, t, hovered);
    return clicked;
}

void ProgressBar(const ImVec2& size, float t, ImU32 col)
{
    float w = (size.x < 0.f) ? RegionW() : size.x;
    float h = (size.y <= 0.f) ? 8.f : size.y;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(w, h));
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 18), h * 0.5f);
    if (t > 0.001f)
        dl->AddRectFilled(p, ImVec2(p.x + w * t, p.y + h), col, h * 0.5f);
}

void Badge(const char* text, ImU32 col)
{
    if (!text) text = "";
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ts.x + 16.f, h = ts.y + 6.f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), A(col, 0.20f), h * 0.5f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), A(col, 0.55f), h * 0.5f);
    dl->AddText(ImVec2(p.x + 8.f, p.y + 3.f), col, text);
    ImGui::Dummy(ImVec2(w, h));
}

void Bullet(ImU32 col, float r)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = FontH();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + h * 0.5f), r, col);
    ImGui::Dummy(ImVec2(r * 2.f, h));
}
void Icon(ImDrawList* dl, int icon, ImVec2 c, float r, ImU32 col)
{
    const float T = 1.7f;
    switch (icon)
    {
    case IC_HOME:
        dl->AddLine(ImVec2(c.x - r, c.y - r * 0.10f), ImVec2(c.x, c.y - r * 0.92f), col, T);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.92f), ImVec2(c.x + r, c.y - r * 0.10f), col, T);
        dl->AddRect(ImVec2(c.x - r * 0.68f, c.y - r * 0.12f),
                    ImVec2(c.x + r * 0.68f, c.y + r * 0.92f), col, 1.5f, 0, T);
        break;
    case IC_LIVE:
        dl->AddRectFilled(ImVec2(c.x - r * 0.80f, c.y + r * 0.05f), ImVec2(c.x - r * 0.45f, c.y + r * 0.85f), col, 1.f);
        dl->AddRectFilled(ImVec2(c.x - r * 0.20f, c.y - r * 0.85f), ImVec2(c.x + r * 0.15f, c.y + r * 0.85f), col, 1.f);
        dl->AddRectFilled(ImVec2(c.x + r * 0.40f, c.y - r * 0.35f), ImVec2(c.x + r * 0.75f, c.y + r * 0.85f), col, 1.f);
        break;
    case IC_EYE:
        dl->PathArcTo(ImVec2(c.x, c.y + r * 0.35f), r * 1.05f, -2.55f, -0.59f, 24);
        dl->PathStroke(col, 0, T);
        dl->PathArcTo(ImVec2(c.x, c.y - r * 0.35f), r * 1.05f, 0.59f, 2.55f, 24);
        dl->PathStroke(col, 0, T);
        dl->AddCircleFilled(c, r * 0.34f, col);
        break;
    case IC_TRACK:
        for (int i = 0; i < 4; i++)
        {
            float x = c.x + (-0.90f + 0.60f * i) * r;
            dl->AddLine(ImVec2(x, c.y - r * 0.90f), ImVec2(x, c.y + r * 0.90f), A(col, 0.55f), T * 0.8f);
        }
        dl->AddRectFilled(ImVec2(c.x - r * 0.98f, c.y - r * 0.55f), ImVec2(c.x - r * 0.70f, c.y - r * 0.15f), col, 1.f);
        dl->AddRectFilled(ImVec2(c.x - r * 0.38f, c.y + r * 0.10f), ImVec2(c.x - r * 0.10f, c.y + r * 0.50f), col, 1.f);
        dl->AddRectFilled(ImVec2(c.x + r * 0.22f, c.y - r * 0.30f), ImVec2(c.x + r * 0.50f, c.y + r * 0.10f), col, 1.f);
        break;
    case IC_MORE:
        for (int i = -1; i <= 1; i++)
            dl->AddCircleFilled(ImVec2(c.x + i * r * 0.62f, c.y), r * 0.20f, col);
        break;
    case IC_GEAR:
    {
        dl->AddCircle(c, r * 0.62f, col, 20, T);
        dl->AddCircleFilled(c, r * 0.20f, col);
        for (int i = 0; i < 8; i++)
        {
            float a = (float)i * 3.14159265358979f / 4.f;
            float ca = cosf(a), sa = sinf(a);
            dl->AddLine(ImVec2(c.x + ca * r * 0.62f, c.y + sa * r * 0.62f),
                        ImVec2(c.x + ca * r * 0.98f, c.y + sa * r * 0.98f), col, T);
        }
        break;
    }
    case IC_INFO:
        dl->AddCircle(c, r * 0.94f, col, 26, T);
        dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.45f), r * 0.15f, col);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.10f), ImVec2(c.x, c.y + r * 0.52f), col, T);
        break;
    case IC_MACRO:
        dl->AddRect(ImVec2(c.x - r * 0.95f, c.y - r * 0.72f), ImVec2(c.x + r * 0.95f, c.y + r * 0.72f), col, 2.5f, 0, T);
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.24f, c.y - r * 0.42f),
                              ImVec2(c.x - r * 0.24f, c.y + r * 0.42f),
                              ImVec2(c.x + r * 0.46f, c.y), col);
        break;
    case IC_REC:
        dl->AddRect(ImVec2(c.x - r * 0.95f, c.y - r * 0.72f), ImVec2(c.x + r * 0.95f, c.y + r * 0.72f), col, 2.5f, 0, T);
        dl->AddCircleFilled(c, r * 0.34f, col);
        break;
    case IC_SKIN:
    {
        dl->PathArcTo(ImVec2(c.x, c.y + r * 0.15f), r * 0.88f, 0.55f, 2.59f, 22);
        dl->PathLineTo(ImVec2(c.x - r * 0.62f, c.y - r * 0.62f));
        dl->PathLineTo(ImVec2(c.x, c.y - r * 0.98f));
        dl->PathLineTo(ImVec2(c.x + r * 0.62f, c.y - r * 0.62f));
        dl->PathStroke(col, ImDrawFlags_Closed, T);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.28f, c.y - r * 0.05f), r * 0.17f, col);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.16f, c.y + r * 0.12f), r * 0.17f, col);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.05f, c.y + r * 0.45f), r * 0.17f, col);
        break;
    }
    case IC_PAD:
        dl->AddRect(ImVec2(c.x - r * 0.95f, c.y - r * 0.95f), ImVec2(c.x - r * 0.12f, c.y - r * 0.12f), col, 2.f, 0, T);
        dl->AddRect(ImVec2(c.x + r * 0.12f, c.y - r * 0.95f), ImVec2(c.x + r * 0.95f, c.y - r * 0.12f), col, 2.f, 0, T);
        dl->AddRect(ImVec2(c.x - r * 0.95f, c.y + r * 0.12f), ImVec2(c.x - r * 0.12f, c.y + r * 0.95f), col, 2.f, 0, T);
        dl->AddRect(ImVec2(c.x + r * 0.12f, c.y + r * 0.12f), ImVec2(c.x + r * 0.95f, c.y + r * 0.95f), col, 2.f, 0, T);
        break;
    case IC_CATCH:
        dl->AddLine(ImVec2(c.x - r * 0.95f, c.y + r * 0.70f), ImVec2(c.x + r * 0.95f, c.y + r * 0.70f), col, T + 0.4f);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.42f, c.y - r * 0.30f), r * 0.22f, col);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.30f, c.y - r * 0.72f), r * 0.22f, col);
        dl->AddTriangleFilled(ImVec2(c.x + r * 0.62f, c.y - r * 0.12f),
                              ImVec2(c.x + r * 0.40f, c.y - r * 0.55f),
                              ImVec2(c.x + r * 0.84f, c.y - r * 0.55f), col);
        break;
    case IC_KEY:
        dl->AddRect(ImVec2(c.x - r * 0.92f, c.y - r * 0.92f), ImVec2(c.x + r * 0.92f, c.y + r * 0.92f), col, 2.5f, 0, T);
        dl->AddLine(ImVec2(c.x - r * 0.45f, c.y), ImVec2(c.x + r * 0.45f, c.y), col, T);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.45f), ImVec2(c.x, c.y + r * 0.45f), col, T);
        break;
    case IC_CHEVRON_R:
        dl->AddLine(ImVec2(c.x - r * 0.4f, c.y - r * 0.7f), ImVec2(c.x + r * 0.3f, c.y), col, T + 0.3f);
        dl->AddLine(ImVec2(c.x + r * 0.3f, c.y), ImVec2(c.x - r * 0.4f, c.y + r * 0.7f), col, T + 0.3f);
        break;
    case IC_CHEVRON_L:
        dl->AddLine(ImVec2(c.x + r * 0.4f, c.y - r * 0.7f), ImVec2(c.x - r * 0.3f, c.y), col, T + 0.3f);
        dl->AddLine(ImVec2(c.x - r * 0.3f, c.y), ImVec2(c.x + r * 0.4f, c.y + r * 0.7f), col, T + 0.3f);
        break;
    case IC_CHEVRON_D:
        dl->AddLine(ImVec2(c.x - r * 0.7f, c.y - r * 0.35f), ImVec2(c.x, c.y + r * 0.35f), col, T + 0.3f);
        dl->AddLine(ImVec2(c.x, c.y + r * 0.35f), ImVec2(c.x + r * 0.7f, c.y - r * 0.35f), col, T + 0.3f);
        break;
    case IC_MINUS:
        dl->AddLine(ImVec2(c.x - r * 0.75f, c.y), ImVec2(c.x + r * 0.75f, c.y), col, T + 0.5f);
        break;
    case IC_CHECK:
        dl->AddLine(ImVec2(c.x - r * 0.7f, c.y + r * 0.05f), ImVec2(c.x - r * 0.15f, c.y + r * 0.6f), col, T + 0.4f);
        dl->AddLine(ImVec2(c.x - r * 0.15f, c.y + r * 0.6f), ImVec2(c.x + r * 0.75f, c.y - r * 0.6f), col, T + 0.4f);
        break;
    case IC_SYNC:
        dl->PathArcTo(c, r * 0.78f, 1.1f, 5.1f, 26);
        dl->PathStroke(col, 0, T);
        dl->AddTriangleFilled(ImVec2(c.x + r * 0.10f, c.y - r * 0.98f),
                              ImVec2(c.x + r * 0.72f, c.y - r * 0.62f),
                              ImVec2(c.x + r * 0.36f, c.y - r * 0.18f), col);
        break;
    case IC_FOLDER:
        dl->AddLine(ImVec2(c.x - r * 0.92f, c.y + r * 0.62f), ImVec2(c.x - r * 0.92f, c.y - r * 0.55f), col, T);
        dl->AddLine(ImVec2(c.x - r * 0.92f, c.y - r * 0.55f), ImVec2(c.x - r * 0.20f, c.y - r * 0.55f), col, T);
        dl->AddLine(ImVec2(c.x - r * 0.20f, c.y - r * 0.55f), ImVec2(c.x + r * 0.02f, c.y - r * 0.24f), col, T);
        dl->AddLine(ImVec2(c.x + r * 0.02f, c.y - r * 0.24f), ImVec2(c.x + r * 0.92f, c.y - r * 0.24f), col, T);
        dl->AddLine(ImVec2(c.x + r * 0.92f, c.y - r * 0.24f), ImVec2(c.x + r * 0.92f, c.y + r * 0.62f), col, T);
        dl->AddLine(ImVec2(c.x + r * 0.92f, c.y + r * 0.62f), ImVec2(c.x - r * 0.92f, c.y + r * 0.62f), col, T);
        break;
    case IC_MOD:
    {
        // 拼图块：主体 + 上凸 + 右凸
        dl->AddRect(ImVec2(c.x - r * 0.85f, c.y - r * 0.52f),
                    ImVec2(c.x + r * 0.60f, c.y + r * 0.85f), col, 2.f, 0, T);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.12f, c.y - r * 0.52f), r * 0.30f, col);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.60f, c.y + r * 0.16f), r * 0.30f, col);
        break;
    }
    default:
        dl->AddCircleFilled(c, r * 0.5f, col);
        break;
    }
}


} // namespace UiKit
