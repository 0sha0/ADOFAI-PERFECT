#include "Menu.h"
#include "CheatState.h"
#include "GameBridge.h"
#include "Chart4K.h"
#include "ChartCore.h"
#include "GameDir.h"
#include "Lang.h"
#include "Log.h"
#include "RenderHook.h"
#include <cstring>
#include <cstdlib>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include "imgui.h"
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")

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
    // ---- 应用信息（关于页）----
    static const char* kAppVersion  = "1.2";
    static const char* kAppVideoUrl = "https://www.bilibili.com/video/BV115aQ6iEqJ/";
    static const char* kAppRepoUrl  = "https://github.com/0sha0/ADOFAI-PERFECT";
    static const int   kLogoResId   = 101;   // 与 ADOFAIPerfect.rc 的 IDR_ADOFAI_LOGO 一致
    static const int   kPageAbout   = 11;    // 页 id（渲染分支见 DrawMain）
    static const int   kPageRecord  = 12;    // 录制页（小窗录制：开始/暂停/继续）

    // 调试：ADOFAI_PERFECT_PAGE=N 指定启动页（0..12）
    static int S_PageFromEnv()
    {
        char v[16] = { 0 };
        if (GetEnvironmentVariableA("ADOFAI_PERFECT_PAGE", v, sizeof(v)) > 0)
        {
            int p = atoi(v);
            if (p >= 0 && p <= 12) return p;
            return 0;
        }
        return 0;
    }
    static int   s_page = S_PageFromEnv();  // 0=功能 1=状态

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
        (void)height;   // 卡片高度随内容自适应，滚动交给页面外层
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kCardBg);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
        ImGui::BeginChild(id, ImVec2(0.f, 0.f), ImGuiChildFlags_AutoResizeY);
    }
    static void EndCard()
    {
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    // ---------- 文件夹选择框（浏览游戏目录 / 皮肤目录） ----------
    static bool BrowseFolderDlg(const char* title, char* out, int n)
    {
        if (!out || n <= 0) return false;
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool comOk = SUCCEEDED(hr);
        bool ok = false;
        wchar_t wtitle[160] = L"Select folder";
        if (title && title[0])
            MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 160);
        BROWSEINFOW bi = {};
        bi.lpszTitle = wtitle;
        // 新版对话框需要 STA；线程已是其它 COM 模式时退回经典对话框
        bi.ulFlags = BIF_RETURNONLYFSDIRS | (comOk ? (BIF_NEWDIALOGSTYLE | BIF_USENEWUI) : 0);
        LPITEMIDLIST idl = SHBrowseForFolderW(&bi);
        if (idl)
        {
            wchar_t wpath[MAX_PATH * 2] = {};
            if (SHGetPathFromIDListW(idl, wpath))
            {
                WideCharToMultiByte(CP_UTF8, 0, wpath, -1, out, n, nullptr, nullptr);
                ok = true;
            }
            CoTaskMemFree(idl);
        }
        if (comOk) CoUninitialize();
        return ok;
    }

    // ---------- 配置档案（保存 4K/5K/6K/10K + 键位 + KeyViewer + MOD 加载器） ----------
    static std::string ProfileDir()
    {
        std::string d = I18N::Prefs::Dir();
        d += "configs";
        CreateDirectoryA(d.c_str(), nullptr);
        return d;
    }
    static std::string ProfileMsg;
    static std::string SkinMsg;

    static bool ProfileWrite(const std::string& path)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "wb") != 0 || !f)
            return false;
        fprintf(f, "# ADOFAI-PERFECT profile (modes / keys / keyviewer / mods)\n");
        static const char* kKn[10] = { "en", "speed", "offset", "style", "judge", "uphide", "dnhide",
                                       "autooff", "autoplay", "macro" };
        for (int mi = 0; mi < 4; mi++)
        {
            for (int w = 0; w < 10; w++)
                fprintf(f, "mode%d.%s=%d\n", mi, kKn[w], Chart4K::ModeSettingGet(mi, w));
            int n = Chart4K::ModeLaneCount(mi);
            for (int sl = 0; sl < n; sl++)
                fprintf(f, "mode%d.key%d=%d\n", mi, sl, Chart4K::ModeKeyGet(mi, sl));
        }
        for (int w = 0; w < 14; w++)
            fprintf(f, "kv.s%d=%d\n", w, Chart4K::KVSettingGet(w));
        int kn = Chart4K::KVKeyCount();
        fprintf(f, "kv.n=%d\n", kn);
        for (int i = 0; i < kn; i++)
            fprintf(f, "kv.k%d=%d\n", i, Chart4K::KVKeyGet(i));
        // 宏打歌 / 自动录制（rec.dir 也写进档案，便于整套搬迁）
        fprintf(f, "macro.acc=%d\n", Chart4K::MacroAccGet());
        fprintf(f, "macro.human=%d\n", Chart4K::MacroHumanGet());
        fprintf(f, "rec.on=%d\n", Chart4K::RecOnGet());
        fprintf(f, "rec.auto=%d\n", Chart4K::RecAutoGet());
        fprintf(f, "rec.fps=%d\n", Chart4K::RecFpsGet());
        fprintf(f, "rec.mbps=%d\n", Chart4K::RecMbpsGet());
        fprintf(f, "rec.dir=%s\n", Chart4K::RecDirGet());
        fclose(f);
        return true;
    }

    static void Trim(std::string& v)
    {
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
    }

    static bool ProfileRead(const std::string& path)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "rb") != 0 || !f)
            return false;
        std::vector<std::pair<std::string, std::string>> kv;
        char line[1024];
        while (fgets(line, sizeof(line), f))
        {
            std::string ln = line;
            if (ln.empty() || ln[0] == '#') continue;
            size_t eq = ln.find('=');
            if (eq == std::string::npos) continue;
            std::string k = ln.substr(0, eq), v = ln.substr(eq + 1);
            Trim(k); Trim(v);
            if (!k.empty()) kv.emplace_back(k, v);
        }
        fclose(f);
        if (kv.empty()) return false;
        auto gi = [&](const char* key, int def) {
            std::string kk = key;
            for (auto& p : kv) if (p.first == kk) return atoi(p.second.c_str());
            return def;
        };
        auto gs = [&](const char* key) -> std::string {
            std::string kk = key;
            for (auto& p : kv) if (p.first == kk) return p.second;
            return std::string();
        };

        int enMode[4] = { 0, 0, 0, 0 };
        static const char* kKn[10] = { "en", "speed", "offset", "style", "judge", "uphide", "dnhide",
                                       "autooff", "autoplay", "macro" };
        for (int mi = 0; mi < 4; mi++)
        {
            int n = Chart4K::ModeLaneCount(mi);
            for (int sl = 0; sl < n; sl++)
            {
                char key[32];
                snprintf(key, sizeof(key), "mode%d.key%d", mi, sl);
                int vk = gi(key, 0);
                if (vk > 0) Chart4K::ModeKeySet(mi, sl, vk);
            }
            for (int w = 0; w < 10; w++)
            {
                char key[32];
                snprintf(key, sizeof(key), "mode%d.%s", mi, kKn[w]);
                int v = gi(key, -1000);
                if (v == -1000) continue;
                if (w == 0) enMode[mi] = v;
                else Chart4K::ModeSettingSet(mi, w, v);
            }
        }
        for (int w = 0; w < 14; w++)
        {
            char key[16];
            snprintf(key, sizeof(key), "kv.s%d", w);
            int v = gi(key, -1000);
            if (v != -1000) Chart4K::KVSettingSet(w, v);
        }
        int kn = gi("kv.n", 0);
        if (kn > 0)
        {
            Chart4K::KVKeySetCount(kn);
            for (int i = 0; i < kn; i++)
            {
                char key[16];
                snprintf(key, sizeof(key), "kv.k%d", i);
                int vk = gi(key, 0);
                if (vk > 0) Chart4K::KVKeySet(i, vk);
            }
        }
        // 模式互斥开关最后应用，避免中途互相覆盖
        for (int mi = 0; mi < 4; mi++)
            if (enMode[mi]) Chart4K::ModeSettingSet(mi, 0, 1);
        // 宏 / 录制
        {
            int v;
            v = gi("macro.acc", -1);     if (v >= 0) Chart4K::MacroAccSet(v);
            v = gi("macro.human", -1);   if (v >= 0) Chart4K::MacroHumanSet(v);
            v = gi("rec.auto", -1);      if (v >= 0) Chart4K::RecAutoSet(v);
            v = gi("rec.fps", -1);       if (v >= 0) Chart4K::RecFpsSet(v);
            v = gi("rec.mbps", -1);      if (v >= 0) Chart4K::RecMbpsSet(v);
            v = gi("rec.on", -1);        if (v >= 0) Chart4K::RecOnSet(v);
            std::string dir = gs("rec.dir");
            if (!dir.empty()) Chart4K::RecDirSet(dir.c_str());
        }
        return true;
    }

    // ---------- 运行时自检（仅 ADOFAI_PERFECT_SELFTEST=1 时执行一次，用于回归验证，不影响正常使用） ----------
    static void RunSelfTestOnce()
    {
        static bool done = false;
        if (done) return;
        char envBuf[8] = { 0 };
        if (GetEnvironmentVariableA("ADOFAI_PERFECT_SELFTEST", envBuf, sizeof(envBuf)) == 0 || envBuf[0] != '1')
            return;
        done = true;
        Log::Printf("[selftest] ===== begin =====");

        Log::Printf("[selftest] gamedir='%s' valid=%d badvalid=%d",
                    GameDir::Get(),
                    GameDir::Valid(GameDir::Get()) ? 1 : 0,
                    GameDir::Valid("Z:\\__adof_selftest_none__") ? 1 : 0);
        Log::Printf("[selftest] skin root='%s' count=%d active='%s'",
                    Chart4K::SkinDefaultRoot(), Chart4K::SkinCount(), Chart4K::SkinActive());
        for (int i = 0; i < Chart4K::SkinCount() && i < 6; i++)
            Log::Printf("[selftest] skin[%d]='%s'", i, Chart4K::SkinName(i));

        int snapSet[4][8] = {};
        int snapKey[4][10] = {};
        int laneN[4] = {};
        for (int mi = 0; mi < 4; mi++)
        {
            laneN[mi] = Chart4K::ModeLaneCount(mi);
            for (int w = 0; w < 8; w++)  snapSet[mi][w] = Chart4K::ModeSettingGet(mi, w);
            for (int s = 0; s < laneN[mi] && s < 10; s++) snapKey[mi][s] = Chart4K::ModeKeyGet(mi, s);
        }
        int kvN = Chart4K::KVKeyCount();
        int kvSet[14] = {};
        int kvKey[32] = {};
        for (int w = 0; w < 14; w++) kvSet[w] = Chart4K::KVSettingGet(w);
        for (int i = 0; i < kvN && i < 32; i++) kvKey[i] = Chart4K::KVKeyGet(i);

        std::string file = ProfileDir() + "\\__selftest.cfg";
        bool writeOk = ProfileWrite(file);
        Log::Printf("[selftest] profile write=%d file='%s'", writeOk ? 1 : 0, file.c_str());

        int mutated = 0;
        for (int mi = 0; mi < 4; mi++)
        {
            Chart4K::ModeSettingSet(mi, 2, snapSet[mi][2] + 9); mutated++;
            if (snapSet[mi][1] >= 1 && snapSet[mi][1] <= 12)
            {
                Chart4K::ModeSettingSet(mi, 1, snapSet[mi][1] == 1 ? 2 : 1); mutated++;
            }
            if (laneN[mi] > 0 && snapKey[mi][0] > 0)
            {
                Chart4K::ModeKeySet(mi, 0, snapKey[mi][0] == 'H' ? 'G' : 'H'); mutated++;
            }
        }
        Chart4K::KVSettingSet(4, kvSet[4] < 190 ? kvSet[4] + 10 : kvSet[4] - 10); mutated++;
        Chart4K::KVSettingSet(5, kvSet[5] < 235 ? kvSet[5] + 20 : kvSet[5] - 20); mutated++;
        if (kvN > 0 && kvKey[0] > 0)
        {
            Chart4K::KVKeySet(0, kvKey[0] == 'H' ? 'G' : 'H'); mutated++;
        }
        bool readOk = ProfileRead(file);
        Log::Printf("[selftest] profile mutate=%d read=%d", mutated, readOk ? 1 : 0);

        int bad = 0;
        for (int mi = 0; mi < 4; mi++)
        {
            for (int w = 0; w < 8; w++)
                if (Chart4K::ModeSettingGet(mi, w) != snapSet[mi][w])
                {
                    bad++;
                    Log::Printf("[selftest] MISMATCH mode%d.set%d = %d want %d",
                                mi, w, Chart4K::ModeSettingGet(mi, w), snapSet[mi][w]);
                }
            for (int s = 0; s < laneN[mi] && s < 10; s++)
                if (Chart4K::ModeKeyGet(mi, s) != snapKey[mi][s])
                {
                    bad++;
                    Log::Printf("[selftest] MISMATCH mode%d.key%d = %d want %d",
                                mi, s, Chart4K::ModeKeyGet(mi, s), snapKey[mi][s]);
                }
        }
        for (int w = 0; w < 14; w++)
            if (Chart4K::KVSettingGet(w) != kvSet[w])
            {
                bad++;
                Log::Printf("[selftest] MISMATCH kv.s%d = %d want %d",
                            w, Chart4K::KVSettingGet(w), kvSet[w]);
            }
        if (Chart4K::KVKeyCount() != kvN)
        {
            bad++;
            Log::Printf("[selftest] MISMATCH kv.n = %d want %d", Chart4K::KVKeyCount(), kvN);
        }
        for (int i = 0; i < kvN && i < 32; i++)
            if (Chart4K::KVKeyGet(i) != kvKey[i])
            {
                bad++;
                Log::Printf("[selftest] MISMATCH kv.k%d = %d want %d",
                            i, Chart4K::KVKeyGet(i), kvKey[i]);
            }

        DeleteFileA(file.c_str());
        Log::Printf("[selftest] ===== done pass=%d (mismatch=%d) =====",
                    (writeOk && readOk && bad == 0) ? 1 : 0, bad);
    }

    // ---------- 皮肤页 ----------
    static void DrawSkinPage()
    {
        Chart4K::BeginCard4K("##skin_head", 92.f);
        {
            ImGui::PushFont(nullptr, 19.f);
            ImGui::TextUnformatted(I18N::Tr(I18N::ST_SKIN_TITLE));
            ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 166, 182, 255));
            ImGui::TextWrapped("%s", I18N::Tr(I18N::ST_SKIN_DESC));
            ImGui::TextWrapped("%s", I18N::Tr(I18N::ST_SKIN_BUILTIN_DESC));
            ImGui::PopStyleColor();
        }
        Chart4K::EndCard4K();
        ImGui::Spacing();

        float listH = 300.f;
        Chart4K::BeginCard4K("##skin_list", listH);
        {
            const char* active = Chart4K::SkinActive();
            int n = Chart4K::SkinCount();
            if (n <= 0)
                ImGui::TextColored(ImVec4(1.f, 0.72f, 0.35f, 1.f), "%s", I18N::Tr(I18N::ST_SKIN_NONE));
            {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f, 5.f));
                // ---- 内置皮肤（固定版式，推荐）——与外部 MSP 皮肤互斥 ----
                {
                    const bool builtinOn = Chart4K::SkinBuiltinMode();
                    ImGui::PushID(-1);
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, builtinOn ? ImVec4(0.24f, 0.85f, 0.72f, 0.13f)
                                                                      : ImVec4(1.f, 1.f, 1.f, 0.035f));
                    ImGui::BeginChild("##rowb", ImVec2(-1, 46.f), ImGuiChildFlags_None,
                                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                    ImGui::SetCursorPos(ImVec2(10.f, 7.f));
                    ImGui::PushFont(nullptr, 15.f);
                    ImGui::TextUnformatted(I18N::Tr(I18N::ST_SKIN_BUILTIN));
                    ImGui::PopFont();
                    ImGui::SetCursorPos(ImVec2(10.f, 25.f));
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(140, 148, 164, 255));
                    {
                        char sub[128];
                        snprintf(sub, sizeof(sub), "%s: %s", I18N::Tr(I18N::ST_SKIN_MODE),
                                 Chart4K::SkinName(0));
                        ImGui::TextUnformatted(sub);
                    }
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    float bx = ImGui::GetWindowWidth() - 84.f;
                    if (bx < 120.f) bx = 120.f;
                    ImGui::SetCursorPosX(bx);
                    ImGui::SetCursorPosY(10.f);
                    if (builtinOn)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.36f, 0.95f, 0.80f, 1.f));
                        ImGui::TextUnformatted(I18N::Tr(I18N::ST_SKIN_INUSE));
                        ImGui::PopStyleColor();
                    }
                    else if (ImGui::Button(I18N::Tr(I18N::ST_SKIN_APPLY), ImVec2(74.f, 26.f)))
                    {
                        Chart4K::SkinSetBuiltinMode(true);
                        ProfileMsg = I18N::Tr(I18N::ST_SKIN_APPLIED);
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                for (int i = 0; i < n; i++)
                {
                    const char* path = Chart4K::SkinPathAt(i);
                    bool inUse = !Chart4K::SkinBuiltinMode() && active && path &&
                                 _stricmp(active, path) == 0;
                    ImGui::PushID(i);
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, inUse ? ImVec4(0.24f, 0.85f, 0.72f, 0.13f)
                                                                  : ImVec4(1.f, 1.f, 1.f, 0.035f));
                    ImGui::BeginChild("##row", ImVec2(-1, 46.f), ImGuiChildFlags_None,
                                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                    ImGui::SetCursorPos(ImVec2(10.f, 7.f));
                    ImGui::PushFont(nullptr, 15.f);
                    {
                        const char* st = Chart4K::SkinTitle(i);
                        ImGui::TextUnformatted((st && st[0]) ? st : Chart4K::SkinName(i));
                    }
                    ImGui::PopFont();
                    ImGui::SetCursorPos(ImVec2(10.f, 25.f));
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(140, 148, 164, 255));
                    {
                        const char* cr = Chart4K::SkinCreator(i);
                        if (cr && cr[0])
                        {
                            char sub[MAX_PATH * 2 + 96];
                            snprintf(sub, sizeof(sub), "%s  Â·  %s", Chart4K::SkinName(i), cr);
                            ImGui::TextUnformatted(sub);
                        }
                        else
                            ImGui::TextUnformatted(path);
                    }
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    float bx = ImGui::GetWindowWidth() - 84.f;
                    if (bx < 120.f) bx = 120.f;
                    ImGui::SetCursorPosX(bx);
                    ImGui::SetCursorPosY(10.f);
                    if (inUse)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.36f, 0.95f, 0.80f, 1.f));
                        ImGui::TextUnformatted(I18N::Tr(I18N::ST_SKIN_INUSE));
                        ImGui::PopStyleColor();
                    }
                    else if (ImGui::Button(I18N::Tr(I18N::ST_SKIN_APPLY), ImVec2(74.f, 26.f)))
                    {
                        if (Chart4K::SkinSetActiveFull(path))
                            ProfileMsg = I18N::Tr(I18N::ST_SKIN_APPLIED);
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                ImGui::PopStyleVar();
            }
        }
        Chart4K::EndCard4K();
        ImGui::Spacing();

        Chart4K::BeginCard4K("##skin_act", 118.f);
        {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 166, 182, 255));
            ImGui::TextUnformatted(I18N::Tr(I18N::ST_SKIN_CURRENT));
            ImGui::PopStyleColor();
            ImGui::TextWrapped("%s", Chart4K::SkinActive()[0] ? Chart4K::SkinActive() : "-");

            const float sp = ImGui::GetStyle().ItemSpacing.x;
            float w = (ImGui::GetContentRegionAvail().x - sp) * 0.5f;
            if (ImGui::Button(I18N::Tr(I18N::ST_SKIN_IMPORT), ImVec2(-1.f, 28.f)))
            {
                char msg[320] = { 0 };
                if (Chart4K::SkinImportMspDialog(msg, sizeof(msg)))
                    SkinMsg = std::string(I18N::Tr(I18N::ST_SKIN_IMPORTED)) + msg;
                else if (msg[0])
                    SkinMsg = std::string(I18N::Tr(I18N::ST_SKIN_IMPORT_FAIL)) + ": " + msg;
            }
            if (ImGui::Button(I18N::Tr(I18N::ST_SKIN_OPEN), ImVec2(w, 28.f)))
                Chart4K::SkinOpenFolder();
            ImGui::SameLine();
            if (ImGui::Button(I18N::Tr(I18N::ST_SKIN_RESCAN), ImVec2(w, 28.f)))
                Chart4K::SkinRescan();
            if (!SkinMsg.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 210, 170, 255));
                ImGui::TextWrapped("%s", SkinMsg.c_str());
                ImGui::PopStyleColor();
            }
        }
        Chart4K::EndCard4K();
    }

    // ---------- 设置页（游戏目录 / i18n / 配置档案 / 关于） ----------
    static char s_gameDirBuf[MAX_PATH * 2] = { 0 };
    static bool s_gameDirInit = false;

    static void DrawShellSettingsPage()
    {
        if (!s_gameDirInit)
        {
            s_gameDirInit = true;
            snprintf(s_gameDirBuf, sizeof(s_gameDirBuf), "%s", GameDir::Get());
        }

        // ---- 卡片 1：游戏目录 ----
        Chart4K::BeginCard4K("##cardSet_gamedir", 168.f);
        {
            ImGui::PushFont(nullptr, 19.f);
            ImGui::TextUnformatted(I18N::Tr(I18N::ST_GAMEDIR));
            ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 158, 174, 255));
            ImGui::TextWrapped("%s", I18N::Tr(I18N::ST_GAMEDIR_DESC));
            ImGui::PopStyleColor();
            ImGui::Spacing();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 96.f);
            ImGui::InputText("##gamedir", s_gameDirBuf, sizeof(s_gameDirBuf));
            ImGui::SameLine();
            if (ImGui::Button(I18N::Tr(I18N::ST_BROWSE), ImVec2(88.f, 0.f)))
            {
                char pick[MAX_PATH * 2] = { 0 };
                if (BrowseFolderDlg(I18N::Tr(I18N::ST_GAMEDIR), pick, sizeof(pick)))
                    snprintf(s_gameDirBuf, sizeof(s_gameDirBuf), "%s", pick);
            }
            const float sp = ImGui::GetStyle().ItemSpacing.x;
            float w = (ImGui::GetContentRegionAvail().x - sp * 2.f) / 3.f;
            if (ImGui::Button(I18N::Tr(I18N::ST_APPLY), ImVec2(w, 26.f)))
            {
                if (GameDir::Valid(s_gameDirBuf))
                {
                    GameDir::Set(s_gameDirBuf);
                    ProfileMsg = I18N::Tr(I18N::ST_GAMEDIR_OK);
                }
                else
                {
                    ProfileMsg = I18N::Tr(I18N::ST_GAMEDIR_BAD);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(I18N::Tr(I18N::ST_OPEN), ImVec2(w, 26.f)))
                GameDir::OpenFolder();
        }
        Chart4K::EndCard4K();
        ImGui::Spacing();

        // ---- 卡片 2：语言 ----
        Chart4K::BeginCard4K("##cardSet_lang", 92.f);
        {
            ImGui::PushFont(nullptr, 19.f);
            ImGui::TextUnformatted(I18N::Tr(I18N::ST_LANG));
            ImGui::PopFont();
            int lang = (int)I18N::GetLang();
            ImGui::SameLine();
            ImGui::SetCursorPosX(150.f);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::BeginCombo("##set_lang", I18N::LangName((I18N::Lang)lang)))
            {
                for (int i = 0; i < I18N::LANG_N; i++)
                {
                    bool sel = (i == lang);
                    if (ImGui::Selectable(I18N::LangName((I18N::Lang)i), sel) && !sel)
                    {
                        I18N::SetLang((I18N::Lang)i);
                        Log::Printf("[UI] language -> %d", i);
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 158, 174, 255));
            ImGui::TextWrapped("%s", I18N::Tr(I18N::ST_LANGHINT));
            ImGui::PopStyleColor();
        }
        Chart4K::EndCard4K();
        ImGui::Spacing();

        // ---- 卡片 3：配置档案 ----
        Chart4K::BeginCard4K("##cardSet_profile", 322.f);
        {
            ImGui::PushFont(nullptr, 19.f);
            ImGui::TextUnformatted(I18N::Tr(I18N::ST_CFG_TITLE));
            ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 158, 174, 255));
            ImGui::TextWrapped("%s", I18N::Tr(I18N::ST_CFG_DESC));
            ImGui::PopStyleColor();

            static char s_profileName[64] = "default";
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8.f);
            ImGui::InputTextWithHint("##confname", I18N::Tr(I18N::ST_CFG_NAME), s_profileName, sizeof(s_profileName));

            const float sp = ImGui::GetStyle().ItemSpacing.x;
            float bw = (ImGui::GetContentRegionAvail().x - sp) * 0.5f;
            std::string dir = ProfileDir();
            std::string path = dir + "\\" + (s_profileName[0] ? s_profileName : "default") + ".cfg";
            if (ImGui::Button(I18N::Tr(I18N::ST_CFG_SAVE), ImVec2(bw, 27.f)))
                ProfileMsg = ProfileWrite(path) ? I18N::Tr(I18N::ST_CFG_SAVED) : I18N::Tr(I18N::ST_CFG_FAIL);
            ImGui::SameLine();
            if (ImGui::Button(I18N::Tr(I18N::ST_CFG_LOAD), ImVec2(bw, 27.f)))
                ProfileMsg = ProfileRead(path) ? I18N::Tr(I18N::ST_CFG_LOADED) : I18N::Tr(I18N::ST_CFG_FAIL);
            if (ImGui::Button(I18N::Tr(I18N::ST_CFG_DEL), ImVec2(bw, 27.f)))
            {
                DeleteFileA(path.c_str());
                ProfileMsg = I18N::Tr(I18N::ST_CFG_FAIL);
            }
            ImGui::SameLine();
            if (ImGui::Button(I18N::Tr(I18N::ST_CFG_OPEN), ImVec2(bw, 27.f)))
                ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

            // 已有档案列表（点击即选中文件名）
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 0.16f));
            ImGui::BeginChild("##proflist", ImVec2(0.f, 0.f), ImGuiChildFlags_AutoResizeY);
            {
                std::string pat = dir + "\\*.cfg";
                WIN32_FIND_DATAA fd{};
                HANDLE h = FindFirstFileA(pat.c_str(), &fd);
                int cnt = 0;
                if (h != INVALID_HANDLE_VALUE)
                {
                    do
                    {
                        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                        std::string nm = fd.cFileName;
                        size_t dot = nm.find_last_of('.');
                        if (dot != std::string::npos) nm = nm.substr(0, dot);
                        bool sel = (nm == s_profileName);
                        if (ImGui::Selectable(nm.c_str(), sel))
                            snprintf(s_profileName, sizeof(s_profileName), "%s", nm.c_str());
                        cnt++;
                    } while (FindNextFileA(h, &fd));
                    FindClose(h);
                }
                if (cnt == 0)
                {
                    ImGui::Dummy(ImVec2(0.f, 56.f));
                    ImGui::TextDisabled("%s", I18N::Tr(I18N::ST_CFG_SELECT));
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();

            if (!ProfileMsg.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.36f, 0.95f, 0.80f, 1.f));
                ImGui::TextUnformatted(ProfileMsg.c_str());
                ImGui::PopStyleColor();
            }
        }
        Chart4K::EndCard4K();
        ImGui::Spacing();

        // ---- 卡片 4：API（关于 / 版本信息已移到独立的「关于」页）----
        Chart4K::BeginCard4K("##cardSet_api", 120.f);
        {
            ImGui::TextDisabled("I18N::Register(zhCN, zhTW, en, ja, ru) -> id");
            ImGui::TextDisabled("I18N::Tr(id)  |  I18N::SetLang(lang)");
            ImGui::TextDisabled("Prefs::SetStr(\"skin_dir\" | \"game_dir\", value)");
            ImGui::TextDisabled("Builtin strings: %d", (int)I18N::BUILTIN_N);
        }
        Chart4K::EndCard4K();
    }

    // ---------- 关于页（LOGO / 名称 / 版本 / 演示视频） ----------
    static void AboutOpenVideo()
    {
        ShellExecuteA(nullptr, "open", kAppVideoUrl, nullptr, nullptr, SW_SHOWNORMAL);
    }

    // 进入关于页时自动跳转演示视频；3 秒冷却，来回切页不会刷屏
    static void AboutAutoOpenVideo()
    {
        static double s_last = -1e9;
        const double now = ImGui::GetTime();
        if (now - s_last < 3.0)
            return;
        s_last = now;
        Log::Printf("[UI] about: auto open %s", kAppVideoUrl);
        AboutOpenVideo();
    }

    static void DrawAboutPage()
    {
        // 内嵌 LOGO：贴图失败（设备/解码未就绪）时每 30 帧重试一次
        static void* s_logo = nullptr;
        static int   s_logoFrame = 0;
        if (!s_logo && (s_logoFrame++ % 30) == 0)
            s_logo = RenderHook_LoadEmbeddedPng(kLogoResId, "adofaiperfect-logo");

        // ---- 卡片 1：LOGO + 名称 + 版本 ----
        Chart4K::BeginCard4K("##cardAboutHead", 0.f);
        {
            const float avail = ImGui::GetContentRegionAvail().x;
            const float logo = 104.f;
            const float x0 = ImGui::GetCursorPosX();

            ImGui::SetCursorPosX(x0 + (avail - logo) * 0.5f);
            if (s_logo)
            {
                ImGui::Image(ImTextureRef((ImTextureID)(uintptr_t)s_logo), ImVec2(logo, logo));
            }
            else
            {
                // 占位：圆角方块 + 圆环（渲染依赖未就绪时）
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(logo, logo));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRect(p0, ImVec2(p0.x + logo, p0.y + logo),
                            IM_COL32(255, 255, 255, 70), 16.f, 0, 3.f);
                dl->AddCircle(ImVec2(p0.x + logo * 0.5f, p0.y + logo * 0.5f),
                              logo * 0.30f, IM_COL32(255, 255, 255, 80), 48, 3.f);
            }

            // 名称（居中大字）
            {
                const char* name = "ADOFAI-PERFECT";
                ImGui::PushFont(nullptr, 26.f);
                const float tw = ImGui::CalcTextSize(name).x;
                ImGui::SetCursorPosX(x0 + (avail - tw) * 0.5f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.95f, 0.98f, 1.f));
                ImGui::TextUnformatted(name);
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }

            // 版本（居中，强调色）
            {
                char ver[64];
                snprintf(ver, sizeof(ver), "%s %s", I18N::Tr(I18N::ABOUT_VER), kAppVersion);
                const float tw = ImGui::CalcTextSize(ver).x;
                ImGui::SetCursorPosX(x0 + (avail - tw) * 0.5f);
                ImGui::PushStyleColor(ImGuiCol_Text, Col(kAccent));
                ImGui::TextUnformatted(ver);
                ImGui::PopStyleColor();
            }

            // 副标题（居中；过长则换行）
            {
                const char* tag = I18N::Tr(I18N::ST_ABOUTTXT);
                const float tw = ImGui::CalcTextSize(tag).x;
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 158, 174, 255));
                if (tw <= avail)
                {
                    ImGui::SetCursorPosX(x0 + (avail - tw) * 0.5f);
                    ImGui::TextUnformatted(tag);
                }
                else
                {
                    ImGui::TextWrapped("%s", tag);
                }
                ImGui::PopStyleColor();
            }
        }
        Chart4K::EndCard4K();
        ImGui::Spacing();

        // ---- 卡片 2：演示视频 / 仓库 / 配置目录 ----
        Chart4K::BeginCard4K("##cardAboutLink", 0.f);
        {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 158, 174, 255));
            ImGui::TextWrapped("%s", I18N::Tr(I18N::ABOUT_HINT));
            ImGui::PopStyleColor();
            ImGui::Spacing();

            const float sp = ImGui::GetStyle().ItemSpacing.x;
            const float bw = (ImGui::GetContentRegionAvail().x - sp) * 0.5f;
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.26f, 0.59f, 0.98f, 0.32f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.59f, 0.98f, 0.50f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.26f, 0.59f, 0.98f, 0.62f));
            if (ImGui::Button(I18N::Tr(I18N::ABOUT_OPEN), ImVec2(bw, 28.f)))
                AboutOpenVideo();
            ImGui::PopStyleColor(3);
            ImGui::SameLine();
            if (ImGui::Button(I18N::Tr(I18N::ABOUT_GITHUB), ImVec2(bw, 28.f)))
                ShellExecuteA(nullptr, "open", kAppRepoUrl, nullptr, nullptr, SW_SHOWNORMAL);

            if (ImGui::Button(I18N::Tr(I18N::ST_OPENCFG), ImVec2(-1.f, 26.f)))
            {
                std::string dir = I18N::Prefs::Dir();
                ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 128, 144, 255));
            ImGui::TextUnformatted(I18N::Tr(I18N::ABOUT_MADE));
            ImGui::PopStyleColor();
        }
        Chart4K::EndCard4K();
    }

    // ---------- 主窗口 ----------
    static void DrawMain()
    {
        const CheatState::Status st = [] {
            CheatState::Status s;
            GameBridge::GetStatusSnapshot(&s);
            return s;
        }();

        // 页内容自适应：卡片高度随内容，滚动范围由 ImGui 按实际内容自动计算，无需内层滚动条。
        ImGui::SetNextWindowPos(s_mainPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(s_mainSize, ImGuiCond_Always);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
        ImGui::Begin("##adofai_main", nullptr, flags);
        {
        }

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
            ImGui::TextUnformatted("  \xc2\xb7  4K/5K/6K/10K + \xe6\x97\xa0\xe8\xbd\xa8\xe8\xaf\xbb\xe8\xb0\xb1");
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
            // 页 id → 标题（索引与 DrawMain 的渲染分支一致）
            static const int kPageTitle[13] = {
                I18N::TAB_FEATURES, I18N::TAB_STATUS, I18N::TAB_READ,
                I18N::TAB_4K, I18N::TAB_5K, I18N::TAB_6K, I18N::TAB_10K,
                I18N::TAB_KEYVIEWER, I18N::TAB_SKIN, I18N::TAB_SETTINGS,
                I18N::TAB_MACRO, I18N::ST_ABOUT, I18N::TAB_RECORD
            };
            // 页签顺序（索引 = 页签位置，值 = 页 id）：… 皮肤 → 录制 → 宏 → 设置 → 关于
            static const int kTabOrder[13] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 10, 9, 11 };
            const char* names[13];
            for (int i = 0; i < 13; i++)
                names[i] = I18N::Tr(kPageTitle[kTabOrder[i]]);
            // 首次绘制：按页签实际宽度自动加宽窗口（保证全部可见，不再出现滚动箭头）
            static bool s_fitTabs = false;
            if (!s_fitTabs)
            {
                s_fitTabs = true;
                float need = 10.f + 42.f;
                for (int i = 0; i < 13; i++)
                    need += ImGui::CalcTextSize(names[i]).x + 14.f + 4.f + 6.f;   // FramePadding.x*2 + ItemSpacing.x + 余量
                const float maxW = ImGui::GetIO().DisplaySize.x - 40.f;
                if (need > maxW) need = maxW;
                if (need > s_mainSize.x) s_mainSize.x = need;
            }
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
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.f, 6.f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f, 4.f));
            if (ImGui::BeginTabBar("##modetabs", ImGuiTabBarFlags_None))
            {
                static int s_pendingTab = S_PageFromEnv();   // 初始页（首帧一次性选中）
                for (int i = 0; i < 13; i++)
                {
                    if (ImGui::BeginTabItem(names[i], nullptr,
                                            (s_pendingTab == kTabOrder[i]) ? ImGuiTabItemFlags_SetSelected : 0))
                    {
                        s_page = kTabOrder[i];
                        ImGui::EndTabItem();
                    }
                }
                s_pendingTab = -1;
                ImGui::EndTabBar();
            }
            {
                static int s_loggedPage = -1;
                if (s_page != s_loggedPage)
                {
                    s_loggedPage = s_page;
                    Log::Printf("[UI] page -> %d (env=%d)", s_page, S_PageFromEnv());
                }
            }
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(6);
        }

        // 切页/首次进入：回到顶部（放在页签之后，保证不会被页签自身的滚动请求覆盖）
        {
            static int s_lastPage = -1;
            if (s_page != s_lastPage)
            {
                s_lastPage = s_page;
                ImGui::SetScrollY(0.f);
                if (s_page == kPageAbout)
                    AboutAutoOpenVideo();     // 打开「关于」页 → 自动跳转演示视频
            }
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
                                   wpos.y + wsize.y - (s_page >= 2 ? 8.f : 46.f)), true);

        // ============ 页：功能 ============
        if (s_page == 0)
        {
            float cardH = (contentH - 10.f) * 0.5f;
            if (cardH > 96.f) cardH = 96.f;               // 紧凑：卡片不随窗口高度无限拉伸

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
                    float ts = 22.f;
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

            FeatureCard(0, "##card1", I18N::Tr(I18N::FEAT_NODEATH),
                        &CheatState::NoDeath, &s_anim[0], kAccent, "##sw1");
            FeatureCard(1, "##card2", I18N::Tr(I18N::FEAT_AUTOCOMBO),
                        &CheatState::AutoCombo, &s_anim[1], kAccent2, "##sw2");

            ImGui::PopClipRect();
            ImGui::SetCursorScreenPos(ImVec2(pageX, wpos.y + wsize.y - 46.f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.20f, 0.55f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.20f, 0.25f, 0.75f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.45f, 0.13f, 0.16f, 0.85f));
            if (ImGui::Button(I18N::Tr(I18N::FEAT_UNLOAD), ImVec2(pageW, 30)))
                CheatState::ExitRequested.store(true, std::memory_order_relaxed);
            ImGui::PopStyleColor(3);
        }
        // ============ 页：辅助读谱（无轨） ============
        else if (s_page == 2)
        {
            ImGui::SetCursorScreenPos(ImVec2(pageX, pageY));
            Chart4K::DrawReadSettingsPage();
            ImGui::PopClipRect();
        }
        // ============ 页：4K 辅助 ============
        else if (s_page == 3)
        {
            ImGui::SetCursorScreenPos(ImVec2(pageX, pageY));
            Chart4K::DrawSettingsPage();
            ImGui::PopClipRect();
        }
        // ============ 页：5K 模式 ============
        else if (s_page == 4)
        {
            ImGui::SetCursorScreenPos(ImVec2(pageX, pageY));
            Chart4K::DrawSettings5KPage();
            ImGui::PopClipRect();
        }
        // ============ 页：6K 模式 ============
        else if (s_page == 5)
        {
            ImGui::SetCursorScreenPos(ImVec2(pageX, pageY));
            Chart4K::DrawSettings6KPage();
            ImGui::PopClipRect();
        }
        // ============ 页：10K 模式 ============
        else if (s_page == 6)
        {
            ImGui::SetCursorScreenPos(ImVec2(pageX, pageY));
            Chart4K::DrawSettings10KPage();
            ImGui::PopClipRect();
        }
        // ============ 页：KeyViewer ============
        else if (s_page == 7)
        {
            ImGui::SetCursorScreenPos(ImVec2(pageX, pageY));
            Chart4K::DrawKeyViewerSettingsPage();
            ImGui::PopClipRect();
        }
        // ============ 页：皮肤 ============
        else if (s_page == 8)
        {
            ImGui::SetCursorScreenPos(ImVec2(pageX, pageY));
            DrawSkinPage();
            ImGui::PopClipRect();
        }
        // ============ 页：设置 ============
        else if (s_page == 9)
        {
            ImGui::SetCursorScreenPos(ImVec2(pageX, pageY));
            DrawShellSettingsPage();
            ImGui::PopClipRect();
        }
        // ============ 页：宏模式（宏打歌 + 自动录制） ============
        else if (s_page == 10)
        {
            ImGui::SetCursorScreenPos(ImVec2(pageX, pageY));
            Chart4K::DrawMacroPage();
            ImGui::PopClipRect();
        }
        // ============ 页：录制（小窗录制：开始/暂停/继续 + 输出目录） ============
        else if (s_page == kPageRecord)
        {
            ImGui::SetCursorScreenPos(ImVec2(pageX, pageY));
            Chart4K::DrawRecordPage();
            ImGui::PopClipRect();
        }
        // ============ 页：关于 ============
        else if (s_page == kPageAbout)
        {
            ImGui::SetCursorScreenPos(ImVec2(pageX, pageY));
            DrawAboutPage();
            ImGui::PopClipRect();
        }
        // ============ 页：实时状态 ============
        else if (s_page == 1)
        {
            ImGui::SetCursorScreenPos(ImVec2(pageX, pageY));
            BeginCard("##cardS", contentH);
            {
                if (!st.bridgeReady)
                {
                    ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f),
                                       "%s", I18N::Tr(I18N::ST_CONNECTING));
                }
                else if (!st.controllerAlive)
                {
                    ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f),
                                       "%s", I18N::Tr(I18N::ST_NOCONTROLLER));
                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_Text, Col(kTextDim));
                    ImGui::TextWrapped("%s", I18N::Tr(I18N::ST_HINT_BG));
                    ImGui::PopStyleColor();
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, Col(kTextDim));
                    ImGui::TextUnformatted(I18N::Tr(I18N::ST_LEVEL)); // 关卡
                    ImGui::PopStyleColor();
                    ImGui::TextWrapped("%s", st.levelName[0] ? st.levelName : I18N::Tr(I18N::ST_UNKNOWN));

                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_Text, Col(kTextDim));
                    ImGui::TextUnformatted(I18N::Tr(I18N::ST_STATE)); // 状态
                    ImGui::PopStyleColor();
                    ImGui::TextUnformatted(st.stateName);
                    ImGui::SameLine();
                    if (st.gameworld)
                        ImGui::TextColored(kAccent, "%s", I18N::Tr(I18N::ST_INGAME));

                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_Text, Col(kTextDim));
                    ImGui::TextUnformatted(I18N::Tr(I18N::ST_PROGRESS)); // 进度
                    ImGui::PopStyleColor();
                    char pct[32];
                    snprintf(pct, sizeof(pct), "%.1f%%", st.percentComplete * 100.f);
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Col(kAccent, 0.55f));
                    ImGui::ProgressBar(st.percentComplete, ImVec2(-1, 8), "");
                    ImGui::PopStyleColor();
                    ImGui::Text("\xe7\xac\xac %d \xe5\x9d\x97  \xc2\xb7  %s", st.floorIndex, pct);

                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, Col(kTextDim));
                    ImGui::TextUnformatted(I18N::Tr(I18N::ST_ACC)); // 精准度
                    ImGui::PopStyleColor();
                    ImGui::PushFont(nullptr, 26.f);
                    ImGui::TextColored(ImVec4(kAccent.x, kAccent.y, kAccent.z, 1.f), "%.2f%%", st.percentAcc * 100.f);
                    ImGui::PopFont();
                    ImGui::Text("XAcc:  %.2f%%", st.percentXAcc * 100.f);
                    ImGui::Text("%s: %d      %s: %d", I18N::Tr(I18N::ST_DEATHS_CK), st.deaths, I18N::Tr(I18N::ST_CHECKPOINT), st.checkpoints);
                }
            }
            EndCard();

            ImGui::PopClipRect();
            ImGui::SetCursorPos(ImVec2(12.f, wsize.y - 34.f));
            ImGui::PushStyleColor(ImGuiCol_Text, Col(kTextDim, 0.8f));
            ImGui::TextDisabled("%s", I18N::Tr(I18N::ST_FOOTER));
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
        static bool s_langLoaded = false;
        if (!s_langLoaded)
        {
            I18N::Load();
            s_langLoaded = true;
        }
        Chart4K::DrawPlayfield();      // 4K/5K/6K/10K 下坠谱面独立窗口（不受菜单显隐影响）
        Chart4K::DrawReadOverlay();    // 辅助读谱（无轨）独立窗口
        Chart4K::DrawKeyViewerOverlay(); // KeyViewer 按键反馈叠加层
        if (!CheatState::MenuVisible.load(std::memory_order_relaxed))
            return;
        RunSelfTestOnce();
        if (s_collapsed)
            DrawDot();
        else
            DrawMain();
    }
}
