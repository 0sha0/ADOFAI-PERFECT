#pragma once
// ============================================================
// UiKit.h - 自绘控件库（Etherium Menu 风格，全量重写版）
//
//   设计语言（逐条对齐 Etherium Menu / DX11 示例）：
//     · 窗口圆角 12，窗口底 (12,12,16,216)；侧栏 / 顶栏 (12,12,16,148)
//     · 分组面板（Child）圆角 6，底 (11,12,13,127)，描边 (52,52,52) 1px
//     · 强调色默认 (149,173,181)，共 6 套预设
//     · 侧栏 Tab 150x35：图标(左 21px) + 文字，选中底 (34,33,34,205) 圆角 6
//     · 子 Tab：选中底 (34,34,38,225) 圆角 6 + 2px 强调下划线
//     · 开关 = 32x16 胶囊：槽 (58,58,58,70)、旋钮 (58,58,58)，开启向强调色插值
//     · 复选项整行可点：标签在左，胶囊在最右
//     · 行 / 卡片：底 (24,25,27)，悬停 (34,35,37)，选中 (14,15,17) + 强调描边，圆角 3~6
//   全部控件自绘；原生控件（Slider / InputText / Combo 弹层）只做配色注入，
//   保证与自绘控件视觉统一。
// ============================================================
#include "imgui.h"
#include <initializer_list>

namespace UiKit
{
    // ---------------- 主题 ----------------
    struct Theme
    {
        ImU32 winBg;      // 窗口底
        ImU32 sideBg;     // 侧栏底
        ImU32 topBg;      // 顶栏底
        ImU32 tabBg;      // 侧栏选中 Tab 底
        ImU32 sideSel;    // 兼容旧名（= tabBg）
        ImU32 subTabBg;   // 子 Tab 选中底
        ImU32 cardBg;     // 分组面板底
        ImU32 headBg;     // 标题条底
        ImU32 rowBg;      // 行 / 卡片行底
        ImU32 rowBgHover; // 行 hover
        ImU32 rowBgSel;   // 行选中
        ImU32 frame;      // 输入框 / 滑条槽
        ImU32 frameHover;
        ImU32 line;       // 描边 / 分隔线
        ImU32 text;       // 主文字
        ImU32 textDim;
        ImU32 textFaint;
        ImU32 textOn;
        ImU32 accent;
        ImU32 accentDim;
        ImU32 accentLine;
        ImU32 good;
        ImU32 warn;
        ImU32 bad;
        ImU32 track;      // 开关槽
        ImU32 knob;       // 开关旋钮
        ImU32 popBg;      // 弹层底
    };
    Theme& Th();

    int         PaletteCount();
    const char* PaletteName(int i);
    void        SetPalette(int i, bool persist = true);
    int         Palette();

    void ApplyGlobalStyle();   // 每帧幂等：圆角 / 间距 / 配色 / 半透明

    // ---------------- 颜色工具 ----------------
    ImU32  A(ImU32 c, float mul);
    ImU32  Mix(ImU32 a, ImU32 b, float t);
    ImVec4 V4(ImU32 c);

    // ---------------- 文本 ----------------
    void Text(const char* fmt, ...);
    void TextCol(ImU32 c, const char* fmt, ...);
    void TextDim(const char* fmt, ...);
    void TextFaint(const char* fmt, ...);
    void TextWrap(const char* fmt, ...);
    void TextWrapCol(ImU32 c, const char* fmt, ...);
    void TextBig(float size, ImU32 c, const char* text);
    float TextBigW(float size, const char* text);
    void Space(float h = 8.f);
    void Separator(float padY = 5.f);
    void Section(const char* text);
    void LabelRow(const char* label, float colX);
    float LabelCol(std::initializer_list<const char*> labels, float minW = 90.f);

    // ---------------- 面板 ----------------
    void BeginCard(const char* id);                       // 分组面板（自适应高度 + 圆角描边）
    void EndCard();
    bool CardTitle(const char* title, bool* toggle);      // 面板标题（右侧可选胶囊）
    void CardHint(const char* text);

    // ---------------- 导航 ----------------
    bool Tab(const char* id, int icon, const char* label, bool selected, ImVec2 size);
    bool NavItem(const char* id, int icon, const char* label, bool selected);
    bool SubTab(const char* id, const char* label, bool selected, ImVec2 size = ImVec2(0.f, 30.f));

    // ---------------- 控件 ----------------
    bool Toggle(const char* id, bool* v, float w = 32.f, float h = 16.f);
    bool CheckRow(const char* id, const char* label, bool* v, const char* sub = nullptr);
    bool Checkbox(const char* id, bool* v, const char* label = nullptr);
    bool SliderInt(const char* id, int* v, int lo, int hi, const char* fmt = "%d", float width = -1.f);
    bool SliderFloat(const char* id, float* v, float lo, float hi, const char* fmt = "%.2f", float width = -1.f);
    bool SliderIntRow(const char* id, const char* label, int* v, int lo, int hi, const char* fmt = "%d");
    bool Combo(const char* id, int* v, const char* const* items, int n, float width = -1.f);
    bool ComboRow(const char* id, const char* label, int* v, const char* const* items, int n);
    bool Button(const char* id, const char* label, ImVec2 size = ImVec2(0.f, 28.f), int variant = 0);
    bool IconButton(const char* id, int icon, ImVec2 size = ImVec2(22.f, 22.f), int variant = 0);
    bool InputText(const char* id, char* buf, size_t n, const char* hint = nullptr,
                   float width = -1.f, bool enterReturns = false);
    bool CardAction(const char* id, const char* title, const char* sub, float h = 46.f,
                    bool selected = false, bool enabled = true, float rightW = 0.f);
    bool CardActionToggle(const char* id, const char* title, const char* sub, bool* v, float h = 48.f);
    bool KeyBox(const char* id, const char* label, bool active, ImVec2 size);
    void ProgressBar(const ImVec2& size, float t, ImU32 col);
    void Badge(const char* text, ImU32 col);
    void Bullet(ImU32 col, float r = 3.5f);
    void Icon(ImDrawList* dl, int icon, ImVec2 c, float r, ImU32 col);

    // 图标
    enum IconId
    {
        IC_HOME = 0, IC_LIVE, IC_EYE, IC_TRACK, IC_MORE, IC_GEAR, IC_INFO,
        IC_MACRO, IC_REC, IC_SKIN, IC_PAD, IC_CATCH, IC_KEY, IC_CHEVRON_R,
        IC_CHEVRON_L, IC_CHEVRON_D, IC_MINUS, IC_CHECK, IC_SYNC, IC_FOLDER, IC_MOD
    };

    // 按钮风格
    enum { BTN_NORMAL = 0, BTN_PRIMARY, BTN_GHOST, BTN_DANGER, BTN_ICON };

    // 布局常量（与 Etherium Menu 一致）
    const float kSideW   = 175.f;  // 侧栏宽
    const float kTopH    = 60.f;   // 顶栏高
    const float kTabW    = 150.f;  // 侧栏 Tab 宽
    const float kTabH    = 35.f;   // 侧栏 Tab 高
    const float kTabGap  = 12.f;   // 侧栏 Tab 间距
    const float kTabX    = 12.f;   // 侧栏 Tab 左边距
    const float kTabY    = 72.f;   // 侧栏首个 Tab 顶边距
    const float kPanelY  = 59.f;   // 内容面板顶边距（= 25 + 34）
    const float kRowH    = 24.f;   // 原生控件行高
    const float kPadX    = 16.f;   // 分组面板内边距
    const float kPadY    = 14.f;
}