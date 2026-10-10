#include "Menu.h"
#include "CheatState.h"
#include "GameBridge.h"
#include "Chart4K.h"
#include "ChartCore.h"
#include "GameDir.h"
#include "Lang.h"
#include "Log.h"
#include "RenderHook.h"
#include "StreamMode.h"
#include "UiKit.h"
#include "ModManager.h"
#include "ModLoader.h"
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
// Menu.cpp — 主界面（NEVERLOSE 风格）
//   · 左侧边栏 7 项 + 右侧卡片内容区（不再使用顶部 TAB）
//   · 所有控件由 UiKit 自绘（Toggle / Slider / Combo / CardAction …）
//   · 可折叠成一颗可拖动的小圆点
// ============================================================
namespace Menu
{
    using namespace UiKit;

    // ---- 本地状态 ----
    static bool   s_collapsed = false;
    static ImVec2 s_mainSize(900.f, 560.f);
    static ImVec2 s_mainPos(60.f, 60.f);
    static ImVec2 s_dotPos(60.f, 60.f);

    static const char* kAppVersion  = "1.3";
    static const char* kAppVideoUrl = "https://www.bilibili.com/video/BV115aQ6iEqJ/";
    static const char* kAppRepoUrl  = "https://github.com/0sha0/ADOFAI-PERFECT";
    static const int   kLogoResId   = 101;

    // ---- 侧边栏页 ----
    enum { PG_BASIC = 0, PG_STATUS, PG_READ, PG_TRACK, PG_MISC, PG_MODMGR, PG_SET, PG_ABOUT, PG_N };
    // ---- 「一般轨道」子页 ----
    enum { TR_4K = 0, TR_5K, TR_6K, TR_10K, TR_16K, TR_CATCH, TR_8K, TR_OSU, TR_N };
    // ---- 「其他」子页 ----
    enum { MI_MACRO = 0, MI_RECORD, MI_SKIN, MI_KV, MI_LIVE, MI_N };
    // ---- 「设置」子页 ----
    enum { SE_MAIN = 0, SE_KV, SE_N };

    static int  s_page  = PG_BASIC;
    static int  s_subTR = -1;          // -1 = 显示「一般轨道」入口页
    static int  s_subMI = -1;          // -1 = 显示「其他」入口页
    static int  s_subSE = SE_MAIN;
    // ---- 前置声明（各页实现按主题分组排在后面）----
    static void DrawBasicPage();
    static void DrawStatusPage();
    static void DrawReadPage();
    static void DrawTrackHub();
    static void DrawTrackSub();
    static void DrawMiscHub();
    static void DrawMiscSub();
    static void DrawSettingsPage();
    static void DrawSkinPage();
    static void DrawAboutPage();
    static void DrawSidebar();
    static void DrawPageBody();
    static void DrawMain();
    static void DrawDot();

    // ============================================================
    //  新界面文案（运行时注册，5 语言；只在首次绘制时注册一次）
    // ============================================================
    enum NSId
    {
        NS_BASIC, NS_STATUS, NS_READ, NS_TRACK, NS_MISC, NS_MODMGR, NS_SET, NS_ABOUT,
        NS_FEATURES, NS_HUB_TRACK, NS_HUB_MISC, NS_GLOBAL,
        NS_TRACK_DESC, NS_MISC_DESC, NS_BACK, NS_OPEN,
        NS_CORE_DESC, NS_LIVE, NS_LIVE_DESC,
        NS_UI_STYLE, NS_UI_STYLE_DESC, NS_PROFILE, NS_PROFILE_DESC,
        NS_SAVE, NS_LOAD, NS_DELETE, NS_OPEN_DIR, NS_NAME_HINT,
        NS_KV_TITLE, NS_KEYBIND, NS_KEYBIND_DESC,
        NS_COMBO, NS_ACC, NS_BPM, NS_NOTES, NS_MAXCOMBO, NS_DEATHS, NS_CP,
        NS_MARV, NS_PERF, NS_GOOD, NS_MISS,
        NS_MODE_ON, NS_MODE_DESC, NS_UNLOAD_HINT, NS_VERSION,
        NS_NOTREADY, NS_PLAYING, NS_PAUSED, NS_IDLE, NS_LV,
        NS_NODEATH_DESC, NS_AUTOCOMBO_DESC, NS_COLLAPSE, NS_SELECT, NS_N
    };
    static int NSI(int i)
    {
        static int  t[NS_N];
        static bool init = false;
        if (!init)
        {
            init = true;
            for (int k = 0; k < NS_N; k++) t[k] = -1;
            t[NS_BASIC]   = I18N::Register("基础功能", "基礎功能", "General", "基本機能", "Основное");
            t[NS_STATUS]  = I18N::Register("实时状态", "即時狀態", "Live Status", "リアルタイム状態", "Состояние");
            t[NS_READ]    = I18N::Register("辅助读谱", "輔助讀譜", "Sightread", "譜面補助", "Чтение");
            t[NS_TRACK]   = I18N::Register("一般轨道", "一般軌道", "Tracks", "通常トラック", "Дорожки");
            t[NS_MISC]    = I18N::Register("其他", "其他", "More", "その他", "Прочее");
            t[NS_MODMGR]  = I18N::Register("MOD 管理器", "MOD 管理器", "Mod Manager", "MOD マネージャー", "Mod Manager");
            t[NS_SET]     = I18N::Register("设置", "設定", "Settings", "設定", "Настройки");
            t[NS_ABOUT]   = I18N::Register("关于", "關於", "About", "このアプリ", "О программе");
            t[NS_FEATURES]= I18N::Register("功能", "功能", "FEATURES", "機能", "ФУНКЦИИ");
            t[NS_HUB_TRACK] = I18N::Register("选择轨道模式", "選擇軌道模式", "Choose a track mode", "トラックモードを選択", "Выберите режим");
            t[NS_HUB_MISC]  = I18N::Register("更多功能", "更多功能", "More tools", "その他の機能", "Дополнительно");
            t[NS_GLOBAL]  = I18N::Register("全局", "全域", "GLOBAL", "グローバル", "ОБЩЕЕ");
            t[NS_TRACK_DESC] = I18N::Register("把冰与火谱面转成对应模式的下坠谱；点进去可调整流速、延迟、风格与键位。",
                                              "把冰與火譜面轉成對應模式的下墜譜；點進去可調整流速、延遲、風格與鍵位。",
                                              "Convert an ADOFAI chart into the selected falling-note mode. Tune speed, offset, style and keys inside.",
                                              "ADOFAI譜面を選択モードの落下譜へ変換。中で速度・オフセット・スタイル・キーを調整できます。",
                                              "Преобразует чарт ADOFAI в падающий режим. Внутри — скорость, смещение, стиль и клавиши.");
            t[NS_MISC_DESC] = I18N::Register("宏打歌 / 画面录制 / 皮肤管理。",
                                             "巨集打歌 / 畫面錄影 / 面板管理。",
                                             "Auto-play macro, screen recording and skin management.",
                                             "マクロ演奏・画面録画・スキン管理。",
                                             "Макрос, запись экрана и скины.");
            t[NS_BACK]    = I18N::Register("返回", "返回", "Back", "戻る", "Назад");
            t[NS_OPEN]    = I18N::Register("打开", "開啟", "Open", "開く", "Открыть");
            t[NS_CORE_DESC] = I18N::Register("游戏本体的两个总开关；立即生效，重开游戏后仍保留。",
                                             "遊戲本體的兩個總開關；立即生效，重開遊戲後仍保留。",
                                             "Two master switches applied directly to the game. Effective immediately.",
                                             "ゲーム本体の2つのスイッチ。即時反映されます。",
                                             "Два основных переключателя игры. Действуют сразу.");
            t[NS_LIVE]    = I18N::Register("关卡信息", "關卡資訊", "Level info", "レベル情報", "Уровень");
            t[NS_LIVE_DESC] = I18N::Register("读取游戏内部状态（只读，不会修改存档）。",
                                             "讀取遊戲內部狀態（唯讀，不會修改存檔）。",
                                             "Reads the game state (read-only).",
                                             "ゲーム内部状態を読み取ります（読み取り専用）。",
                                             "Читает состояние игры (только чтение).");
            t[NS_UI_STYLE] = I18N::Register("界面风格", "介面風格", "UI style", "UIスタイル", "Стиль интерфейса");
            t[NS_UI_STYLE_DESC] = I18N::Register("切换强调色；立即生效并记住。",
                                                 "切換強調色；立即生效並記住。",
                                                 "Accent colour. Applied instantly and remembered.",
                                                 "アクセントカラー。即時反映され保存されます。",
                                                 "Акцентный цвет. Применяется сразу и сохраняется.");
            t[NS_PROFILE] = I18N::Register("配置档案", "設定檔", "Profiles", "プロファイル", "Профили");
            t[NS_PROFILE_DESC] = I18N::Register("把键位、皮肤记忆、各模式参数、读谱与录制设置整包保存/读取。",
                                                "把鍵位、面板記憶、各模式參數、讀譜與錄影設定整包儲存/讀取。",
                                                "Save/load keys, skin memory, per-mode options, sightread and recorder settings as one bundle.",
                                                "キー・スキン・各モード設定・譜面読み・録画設定をまとめて保存/読込。",
                                                "Сохраняет и загружает клавиши, скины, режимы, чтение и запись одним файлом.");
            t[NS_SAVE]    = I18N::Register("保存", "儲存", "Save", "保存", "Сохранить");
            t[NS_LOAD]    = I18N::Register("读取", "讀取", "Load", "読込", "Загрузить");
            t[NS_DELETE]  = I18N::Register("删除", "刪除", "Delete", "削除", "Удалить");
            t[NS_OPEN_DIR]= I18N::Register("打开目录", "開啟目錄", "Open folder", "フォルダを開く", "Открыть папку");
            t[NS_NAME_HINT] = I18N::Register("档案名称", "設定檔名稱", "Profile name", "プロファイル名", "Имя профиля");
            t[NS_KV_TITLE]= I18N::Register("按键反馈", "按鍵回饋", "Key feedback", "キー表示", "Клавиши");
            t[NS_KEYBIND] = I18N::Register("键位设置", "鍵位設定", "Key bindings", "キー設定", "Раскладка");
            t[NS_KEYBIND_DESC] = I18N::Register("每个模式独立键位；支持字母、数字、符号键（如 ; ' [ 空格 Enter 等）。",
                                                "每個模式獨立鍵位；支援字母、數字、符號鍵（如 ; ' [ 空格 Enter 等）。",
                                                "Per-mode key bindings. Letters, digits and symbol keys ( ; ' [ Space Enter ... ) are supported.",
                                                "モードごとのキー設定。英数字・記号キー（; ' [ Space Enter など）に対応。",
                                                "Раскладка для каждого режима. Поддерживаются буквы, цифры и символы ( ; ' [ Space Enter ).");
            t[NS_COMBO]   = I18N::Register("连击", "連擊", "Combo", "コンボ", "Комбо");
            t[NS_ACC]     = I18N::Register("精准度", "精準度", "Accuracy", "精度", "Точность");
            t[NS_BPM]     = I18N::Register("BPM", "BPM", "BPM", "BPM", "BPM");
            t[NS_NOTES]   = I18N::Register("音符", "音符", "Notes", "ノーツ", "Ноты");
            t[NS_MAXCOMBO]= I18N::Register("最大连击", "最大連擊", "Max combo", "最大コンボ", "Макс. комбо");
            t[NS_DEATHS]  = I18N::Register("死亡", "死亡", "Deaths", "ミス", "Смерти");
            t[NS_CP]      = I18N::Register("检查点", "檢查點", "Checkpoints", "チェックポイント", "Чекпоинты");
            t[NS_MARV]    = I18N::Register("完美", "完美", "MARV", "MARV", "MARV");
            t[NS_PERF]    = I18N::Register("优秀", "優秀", "PERF", "PERF", "PERF");
            t[NS_GOOD]    = I18N::Register("良好", "良好", "GOOD", "GOOD", "GOOD");
            t[NS_MISS]    = I18N::Register("失误", "失誤", "MISS", "MISS", "MISS");
            t[NS_MODE_ON] = I18N::Register("启用", "啟用", "Enable", "有効", "Включить");
            t[NS_MODE_DESC] = I18N::Register("开启后进入关卡即显示该模式的下坠谱面（模式互斥）。",
                                             "開啟後進入關卡即顯示該模式的下墜譜面（模式互斥）。",
                                             "Shows this mode's falling chart once you enter a level (modes are exclusive).",
                                             "レベル進入時にこのモードの落下譜を表示（排他）。",
                                             "Показывает чарт этого режима при входе в уровень (режимы исключают друг друга).");
            t[NS_NODEATH_DESC] = I18N::Register("失误不再中断关卡：地球闪红但连击保留、自动补拍继续。",
                                                "失誤不再中斷關卡：地球閃紅但連擊保留、自動補拍繼續。",
                                                "Mistakes no longer break the run: the planet flashes but the combo survives.",
                                                "ミスでも中断されず、コンボが維持されます。",
                                                "Ошибки не прерывают уровень: комбо сохраняется.");
            t[NS_AUTOCOMBO_DESC] = I18N::Register("由游戏自身的 Auto 接管，自动完成演奏（不影响判定统计）。",
                                                  "由遊戲自身的 Auto 接管，自動完成演奏（不影響判定統計）。",
                                                  "The game's own Auto mode plays the chart for you.",
                                                  "ゲーム内蔵のAutoが自動演奏します。",
                                                  "Встроенный Auto игры играет чарт за вас.");            t[NS_UNLOAD_HINT] = I18N::Register("卸载模块并恢复游戏原始状态。", "卸載模組並還原遊戲原始狀態。",
                                               "Unload the module and restore the game.", "モジュールを解除してゲームを元に戻します。",
                                               "Выгрузить модуль и вернуть игру в исходное состояние.");
            t[NS_SELECT] = I18N::Register("选择", "選擇", "Select", "選択", "Выбор");
            t[NS_COLLAPSE] = I18N::Register("折叠", "摺疊", "Collapse", "折りたたむ", "Свернуть");
            t[NS_VERSION] = I18N::Register("版本", "版本", "Version", "バージョン", "Версия");
            t[NS_NOTREADY]= I18N::Register("桥接尚未就绪，请先进入一次游戏关卡。", "橋接尚未就緒，請先進入一次遊戲關卡。",
                                           "Bridge not ready yet - enter a level once.", "ブリッジ未準備 - 一度レベルに入ってください。",
                                           "Мост не готов — войдите в уровень.");
            t[NS_PLAYING] = I18N::Register("游戏进行中", "遊戲進行中", "Playing", "プレイ中", "Игра");
            t[NS_PAUSED]  = I18N::Register("已暂停", "已暫停", "Paused", "一時停止", "Пауза");
            t[NS_IDLE]    = I18N::Register("未开始", "未開始", "Idle", "待機", "Ожидание");
            t[NS_LV]      = I18N::Register("难度", "難度", "Level", "レベル", "Сложность");
        }
        if (i < 0 || i >= NS_N) return -1;
        return t[i];
    }
    static const char* TT(int i) { return I18N::Tr(NSI(i)); }

    // 进入某页（统一入口，负责记住滚动位置）
    static void GoPage(int pg, int subTR = -1, int subMI = -1, int subSE = SE_MAIN)
    {
        s_page  = pg;
        s_subTR = subTR;
        s_subMI = subMI;
        s_subSE = subSE;
    }

    // 启动页（调试钩子 ADOFAI_PERFECT_PAGE=0..14，保持与旧版一致）
    static void S_PageFromEnv()
    {
        static bool s_once = false;
        if (s_once) return;   // 只在启动时读一次，否则会覆盖用户点击
        s_once = true;
        char v[16] = { 0 };
        if (GetEnvironmentVariableA("ADOFAI_PERFECT_PAGE", v, sizeof(v)) <= 0) return;
        const int p = atoi(v);
        switch (p)
        {
        case 0:  GoPage(PG_BASIC); break;
        case 1:  GoPage(PG_STATUS); break;
        case 2:  GoPage(PG_READ); break;
        case 3:  GoPage(PG_TRACK, TR_4K); break;
        case 4:  GoPage(PG_TRACK, TR_5K); break;
        case 5:  GoPage(PG_TRACK, TR_6K); break;
        case 6:  GoPage(PG_TRACK, TR_10K); break;
        case 13: GoPage(PG_TRACK, TR_16K); break;
        case 14: GoPage(PG_TRACK, TR_CATCH); break;
        case 7:  GoPage(PG_MISC, -1, MI_KV); break;
        case 8:  GoPage(PG_MISC, -1, MI_SKIN); break;
        case 9:  GoPage(PG_SET); break;
        case 10: GoPage(PG_MISC, -1, MI_MACRO); break;
        case 11: GoPage(PG_ABOUT); break;
        case 12: GoPage(PG_MISC, -1, MI_RECORD); break;
        case 16: GoPage(PG_MISC, -1, MI_LIVE); break;      // 直播模式（独立页）
        case 17: GoPage(PG_TRACK, TR_8K); break;           // 8K
        case 18: GoPage(PG_TRACK, TR_OSU); break;          // OSU（戳泡泡）
        case 15: GoPage(PG_MODMGR); break;
        default: break;
        }
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
        static const char* kKn[15] = { "en", "speed", "offset", "style", "judge", "uphide", "dnhide",
                                       "autooff", "autoplay", "macro", "pseudo2", "hardresist", "innerroll",
                                       "catchkill", "catchplate" };
        for (int mi = 0; mi < Chart4K::kModeN; mi++)
        {
            for (int w = 0; w < 15; w++)
                fprintf(f, "mode%d.%s=%d\n", mi, kKn[w], Chart4K::ModeSettingGet(mi, w));
            int n = Chart4K::ModeLaneCount(mi);
            for (int sl = 0; sl < n; sl++)
                fprintf(f, "mode%d.key%d=%d\n", mi, sl, Chart4K::ModeKeyGet(mi, sl));
        }
        for (int w = 0; w < 15; w++)
            fprintf(f, "kv.s%d=%d\n", w, Chart4K::KVSettingGet(w));
        int kn = Chart4K::KVKeyCount();
        fprintf(f, "kv.n=%d\n", kn);
        for (int i = 0; i < kn; i++)
            fprintf(f, "kv.k%d=%d\n", i, Chart4K::KVKeyGet(i));
        // 全量全局设置（辅助读谱 / 录制 / 小窗 / 特效 / 宏），rec.dir 一并写入便于整套搬迁
        for (int gid = 0; gid < Chart4K::GlobalSettingCount(); gid++)
            fprintf(f, "g.%s=%d\n", Chart4K::GlobalSettingKey(gid), Chart4K::GlobalSettingGet(gid));
        // 直播模式（防采集）开关一并写进档案，便于整套搬迁
        fprintf(f, "stream.on=%d\n",    StreamMode::SettingGet(0));
        fprintf(f, "stream.menu=%d\n",  StreamMode::SettingGet(1));
        fprintf(f, "stream.track=%d\n", StreamMode::SettingGet(2));
        fprintf(f, "stream.read=%d\n",  StreamMode::SettingGet(3));
        fprintf(f, "stream.kv=%d\n",    StreamMode::SettingGet(4));
        fprintf(f, "rec.dir=%s\n", Chart4K::RecDirGet());
        ModManager::ProfileWrite(f);
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

        int enMode[Chart4K::kModeN] = { 0, 0, 0, 0, 0 };
        static const char* kKn[15] = { "en", "speed", "offset", "style", "judge", "uphide", "dnhide",
                                       "autooff", "autoplay", "macro", "pseudo2", "hardresist", "innerroll",
                                       "catchkill", "catchplate" };
        for (int mi = 0; mi < Chart4K::kModeN; mi++)
        {
            int n = Chart4K::ModeLaneCount(mi);
            for (int sl = 0; sl < n; sl++)
            {
                char key[32];
                snprintf(key, sizeof(key), "mode%d.key%d", mi, sl);
                int vk = gi(key, 0);
                if (vk > 0) Chart4K::ModeKeySet(mi, sl, vk);
            }
            for (int w = 0; w < 15; w++)
            {
                char key[32];
                snprintf(key, sizeof(key), "mode%d.%s", mi, kKn[w]);
                int v = gi(key, -1000);
                if (v == -1000) continue;
                if (w == 0) enMode[mi] = v;
                else Chart4K::ModeSettingSet(mi, w, v);
            }
        }
        for (int w = 0; w < 15; w++)
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
        for (int mi = 0; mi < Chart4K::kModeN; mi++)
            if (enMode[mi]) Chart4K::ModeSettingSet(mi, 0, 1);
        // 全量全局设置（辅助读谱 / 录制 / 小窗 / 特效 / 宏）
        {
            char key[64];
            for (int gid = 0; gid < Chart4K::GlobalSettingCount(); gid++)
            {
                snprintf(key, sizeof(key), "g.%s", Chart4K::GlobalSettingKey(gid));
                int v = gi(key, -1000000);
                if (v != -1000000) Chart4K::GlobalSettingSet(gid, v);
            }
            std::string dir = gs("rec.dir");
            if (!dir.empty()) Chart4K::RecDirSet(dir.c_str());
            // 直播模式（防采集）
            static const char* kStreamKeys[5] = { "stream.on", "stream.menu", "stream.track",
                                                  "stream.read", "stream.kv" };
            for (int i = 0; i < 5; i++)
            {
                int v = gi(kStreamKeys[i], -1000000);
                if (v != -1000000) StreamMode::SettingSet(i, v);
            }
        }
        // MOD 管理器：mod.dir / mod.state.<Id>
        for (auto& p : kv) ModManager::ProfileReadKey(p.first, p.second);
        ModManager::OnProfileApplied();   // 保存 prefs + 重扫 + 把开关同步进 Params.xml
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

        int snapSet[Chart4K::kModeN][13] = {};
        int snapKey[Chart4K::kModeN][16] = {};
        int laneN[Chart4K::kModeN] = {};
        for (int mi = 0; mi < Chart4K::kModeN; mi++)
        {
            laneN[mi] = Chart4K::ModeLaneCount(mi);
            for (int w = 0; w < 13; w++) snapSet[mi][w] = Chart4K::ModeSettingGet(mi, w);
            for (int s = 0; s < laneN[mi] && s < 16; s++) snapKey[mi][s] = Chart4K::ModeKeyGet(mi, s);
        }
        int kvN = Chart4K::KVKeyCount();
        int kvSet[15] = {};
        int kvKey[32] = {};
        for (int w = 0; w < 15; w++) kvSet[w] = Chart4K::KVSettingGet(w);
        for (int i = 0; i < kvN && i < 32; i++) kvKey[i] = Chart4K::KVKeyGet(i);

        std::string file = ProfileDir() + "\\__selftest.cfg";
        bool writeOk = ProfileWrite(file);
        Log::Printf("[selftest] profile write=%d file='%s'", writeOk ? 1 : 0, file.c_str());

        int mutated = 0;
        for (int mi = 0; mi < Chart4K::kModeN; mi++)
        {
            Chart4K::ModeSettingSet(mi, 2, snapSet[mi][2] + 9); mutated++;
            Chart4K::ModeSettingSet(mi, 11, snapSet[mi][11] == 900 ? 800 : 900); mutated++;   // 硬抗 BPM
            Chart4K::ModeSettingSet(mi, 12, snapSet[mi][12] ? 0 : 1); mutated++;               // 轮指方向
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
        for (int mi = 0; mi < Chart4K::kModeN; mi++)
        {
            for (int w = 0; w < 13; w++)
                if (Chart4K::ModeSettingGet(mi, w) != snapSet[mi][w])
                {
                    bad++;
                    Log::Printf("[selftest] MISMATCH mode%d.set%d = %d want %d",
                                mi, w, Chart4K::ModeSettingGet(mi, w), snapSet[mi][w]);
                }
            for (int s = 0; s < laneN[mi] && s < 16; s++)
                if (Chart4K::ModeKeyGet(mi, s) != snapKey[mi][s])
                {
                    bad++;
                    Log::Printf("[selftest] MISMATCH mode%d.key%d = %d want %d",
                                mi, s, Chart4K::ModeKeyGet(mi, s), snapKey[mi][s]);
                }
        }
        for (int w = 0; w < 15; w++)
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

    // ============================================================
    //  通用小部件
    // ============================================================
    // 内容区页头：副标题 + 右上角「返回」
    //   （大标题统一画在顶栏，标题与内容读同一份状态，永不脱节）
    // ============================================================
    static bool PageHeader(const char* title, const char* sub, bool showBack)
    {
        (void)title;
        bool back = false;
        ImVec2 p = ImGui::GetCursorScreenPos();
        float avail = ImGui::GetContentRegionAvail().x;
        const float h = 26.f;

        if (sub && sub[0])
            ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 2.f, p.y + 6.f), Th().textFaint, sub);

        if (showBack)
        {
            ImGui::SetCursorScreenPos(ImVec2(p.x + avail - 74.f, p.y));
            back = SubTab("##hdr_back", TT(NS_BACK), false, ImVec2(74.f, h));
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 4.f));
        return back;
    }
    // 一行「标签 + 控件」：标签固定在 colX，控件占满剩余宽度
    static bool LabeledToggle(const char* id, const char* label, bool* v, float colX,
                              float rightX = -1.f, bool rightHalf = false)
    {
        LabelRow(label, colX);
        if (rightHalf && rightX < 0.f)
        {
            float avail = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail * 0.5f);
        }
        else if (rightX > 0.f)
        {
            ImGui::SetCursorPosX(rightX);
        }
        return Toggle(id, v, 46.f, 23.f);
    }

    // ============================================================
    //  基础功能
    // ============================================================
    static void DrawBasicPage()
    {
        PageHeader(TT(NS_BASIC), nullptr, false);

        // ---- 不死模式 ----
        {
            bool v = CheatState::NoDeath.load(std::memory_order_relaxed);
            if (CardActionToggle("##ca_nodeath", I18N::Tr(I18N::FEAT_NODEATH),
                                 TT(NS_NODEATH_DESC), &v, 66.f))
            {
                CheatState::NoDeath.store(v, std::memory_order_relaxed);
                Log::Printf("[UI] no-death -> %d", (int)v);
            }
        }
        Space(9.f);
        // ---- 自动连打 ----
        {
            bool v = CheatState::AutoCombo.load(std::memory_order_relaxed);
            if (CardActionToggle("##ca_autocombo", I18N::Tr(I18N::FEAT_AUTOCOMBO),
                                 TT(NS_AUTOCOMBO_DESC), &v, 66.f))
            {
                CheatState::AutoCombo.store(v, std::memory_order_relaxed);
                Log::Printf("[UI] auto-combo -> %d", (int)v);
            }
        }

        Space(14.f);
        BeginCard("##basic_hint");
        CardHint(TT(NS_CORE_DESC));
        EndCard();

        // ---- 卸载 ----
        Space(14.f);
        if (Button("##btn_unload", I18N::Tr(I18N::FEAT_UNLOAD), ImVec2(-1.f, 34.f), BTN_DANGER))
            CheatState::ExitRequested.store(true, std::memory_order_relaxed);
        Space(4.f);
        TextFaint("%s", TT(NS_UNLOAD_HINT));
    }

    // ============================================================
    //  实时状态
    // ============================================================
    static void StatTile(const char* id, const char* label, const char* value, ImU32 col, float w)
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = 58.f;
        ImGui::Dummy(ImVec2(w, h));
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Th().cardBg, 9.f);
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), Th().line, 9.f);
        dl->AddText(ImVec2(p.x + 11.f, p.y + 8.f), Th().textFaint, label);
        ImFont* f = ImGui::GetFont();
        dl->AddText(f, 21.f, ImVec2(p.x + 11.f, p.y + 25.f), col, value);
    }

    static void DrawStatusPage()
    {
        CheatState::Status st;
        GameBridge::GetStatusSnapshot(&st);

        PageHeader(TT(NS_STATUS), TT(NS_LIVE_DESC), false);

        if (!st.bridgeReady || !st.controllerAlive)
        {
            BeginCard("##st_wait");
            TextBig(16.f, Th().warn, TT(NS_NOTREADY));
            Space(4.f);
            CardHint(I18N::Tr(I18N::ST_HINT_BG));
            EndCard();
            return;
        }

        // ---- Hero：关卡名 + 状态 ----
        BeginCard("##st_hero");
        {
            const char* nm = st.levelName[0] ? st.levelName : I18N::Tr(I18N::ST_UNKNOWN);
            TextBig(20.f, Th().text, nm);
            Space(2.f);
            bool playing = st.gameworld && !st.paused;
            const char* stn = playing ? TT(NS_PLAYING) : (st.paused ? TT(NS_PAUSED) : TT(NS_IDLE));
            ImU32 col = playing ? Th().good : (st.paused ? Th().warn : Th().textDim);
            Bullet(col);
            ImGui::SameLine();
            TextCol(col, "%s", stn);
            if (st.stateName[0])
            {
                ImGui::SameLine();
                TextFaint("  -  %s", st.stateName);
            }
        }
        EndCard();
        Space(9.f);

        // ---- 进度 ----
        BeginCard("##st_prog");
        {
            char pct[32];
            snprintf(pct, sizeof(pct), "%.1f%%", st.percentComplete * 100.f);
            TextDim("%s: %d    %s", I18N::Tr(I18N::ST_PROGRESS), st.floorIndex, pct);
            ProgressBar(ImVec2(-1.f, 10.f), st.percentComplete, Th().accent);
        }
        EndCard();
        Space(9.f);

        // ---- 数值磁贴：Combo / Acc / BPM / Notes ----
        {
            float gap = 8.f;
            float w = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
            char b[64];

            snprintf(b, sizeof(b), "%d", st.combo);
            StatTile("##t1", TT(NS_COMBO), b, Th().accent, w);
            ImGui::SameLine(0.f, gap);
            snprintf(b, sizeof(b), "%.2f%%", st.percentAcc * 100.f);
            StatTile("##t2", TT(NS_ACC), b, Th().good, w);
            Space(8.f);

            float bpm = (float)Chart4K::ApiBpm();
            if (bpm > 1.f) snprintf(b, sizeof(b), "%.1f", bpm); else snprintf(b, sizeof(b), "--");
            StatTile("##t3", TT(NS_BPM), b, Th().text, w);
            ImGui::SameLine(0.f, gap);
            snprintf(b, sizeof(b), "%d", st.hitTotal);
            StatTile("##t4", TT(NS_NOTES), b, Th().text, w);
        }
        Space(9.f);

        // ---- 判定统计 ----
        BeginCard("##st_judge");
        {
            Section(TT(NS_ACC));
            struct Row { const char* nm; int v; ImU32 c; };
            // HitMargin: 3=Perfect 4=LatePerfect → 计为「优秀」
            const int marv = st.hitCounts[3] + st.hitCounts[4];
            const int perf = st.hitCounts[2] + st.hitCounts[5];
            const int good = st.hitCounts[1] + st.hitCounts[6];
            const int miss = st.hitCounts[0] + st.hitCounts[8] + st.hitCounts[9];
            Row rows[4] = {
                { TT(NS_MARV), marv, Th().accent },
                { TT(NS_PERF), perf, Th().good },
                { TT(NS_GOOD), good, Th().warn },
                { TT(NS_MISS), miss, Th().bad },
            };
            int tot = marv + perf + good + miss;
            if (tot < 1) tot = 1;
            for (int i = 0; i < 4; i++)
            {
                float avail = ImGui::GetContentRegionAvail().x;
                TextDim("%s", rows[i].nm);
                char vb[48];
                snprintf(vb, sizeof(vb), "%d", rows[i].v);
                float vw = ImGui::CalcTextSize(vb).x;
                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - vw - 60.f);
                TextCol(rows[i].c, "%s", vb);
                ImGui::SameLine();
                ProgressBar(ImVec2(56.f, 8.f), (float)rows[i].v / (float)tot, rows[i].c);
                ImGui::SameLine();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 9.f);
            }
            Space(4.f);
            TextFaint("XAcc %.2f%%   -   %s %d   -   %s %d",
                      st.percentXAcc * 100.f, TT(NS_MAXCOMBO), st.maxCombo, TT(NS_DEATHS), st.deaths);
            TextFaint("%s %d   -   %s %d", TT(NS_CP), st.checkpoints, TT(NS_LV),
                      st.difficulty + 0);
        }
        EndCard();
        Space(9.f);

        BeginCard("##st_foot");
        CardHint(I18N::Tr(I18N::ST_FOOTER));
        EndCard();
    }

    // ============================================================
    //  一般轨道（入口页 = 6 个 CardAction）
    // ============================================================
    struct TrackItem { int sub; const char* title; const char* keys; };

    static void DrawTrackHub()
    {
        PageHeader(TT(NS_TRACK), TT(NS_HUB_TRACK), false);
        BeginCard("##hub_track_hint");
        CardHint(TT(NS_TRACK_DESC));
        EndCard();
        Space(9.f);

        static const TrackItem kItems[TR_N] = {
            { TR_4K,    "4K",       "D  F  J  K" },
            { TR_5K,    "5K",       "S  D  F  J  K" },
            { TR_6K,    "6K",       "S  D  F  J  K  L" },
            { TR_10K,   "10K",      "A S D F G  /  H J K L ;" },
            { TR_16K,   "16K PAD",  "4 x 4 grid pad" },
            { TR_CATCH, "CATCH",    "rain - no lanes" },
            { TR_8K,    "8K",       "A S D F  /  J K L ;" },
            { TR_OSU,   "OSU",      "osu! standard - click the circles" },
        };
        for (int i = 0; i < TR_N; i++)
        {
            char id[32];
            snprintf(id, sizeof(id), "##hub_tr%d", i);
            if (CardAction(id, kItems[i].title, kItems[i].keys, 54.f, false, true))
                GoPage(PG_TRACK, kItems[i].sub);
            Space(7.f);
        }
    }

    static void DrawTrackSub()
    {
        switch (s_subTR)
        {
        case TR_4K:    Chart4K::DrawSettingsPage();      break;
        case TR_5K:    Chart4K::DrawSettings5KPage();    break;
        case TR_6K:    Chart4K::DrawSettings6KPage();    break;
        case TR_10K:   Chart4K::DrawSettings10KPage();   break;
        case TR_16K:   Chart4K::DrawModePage(4);         break;
        case TR_CATCH: Chart4K::DrawCatchSettingsPage(); break;
        case TR_8K:    Chart4K::DrawSettings8KPage();    break;
        case TR_OSU:   Chart4K::DrawOsuSettingsPage();   break;
        default: break;
        }
    }

    // ============================================================
    //  直播模式（防采集覆盖层）—— 独立 Card，放在「其他」页
    //   勾选的元素：本机屏幕照常显示，直播推送软件（OBS / 直播姬 / 录像机）看不到。
    // ============================================================
    static void DrawLiveCard()
    {
        bool on = StreamMode::Enabled();
        BeginCard("##live_card");
        if (CardTitle(I18N::Tr(I18N::LIVE_TITLE), &on))
            StreamMode::SetEnabled(on);
        Space(2.f);
        CardHint(I18N::Tr(I18N::LIVE_DESC));
        Space(6.f);

        {
            const struct { const char* id; int sid; int e; } kRows[4] = {
                { "##live_e0", I18N::LIVE_HIDE_MENU,  StreamMode::EL_MENU  },
                { "##live_e1", I18N::LIVE_HIDE_TRACK, StreamMode::EL_TRACK },
                { "##live_e2", I18N::LIVE_HIDE_READ,  StreamMode::EL_READ  },
                { "##live_e3", I18N::LIVE_HIDE_KV,    StreamMode::EL_KV    },
            };
            for (int i = 0; i < 4; i++)
            {
                const StreamMode::Elem e = (StreamMode::Elem)kRows[i].e;
                bool hv = StreamMode::Hide(e);
                if (CheckRow(kRows[i].id, I18N::Tr(kRows[i].sid), &hv))
                    StreamMode::SetHide(e, hv);
            }
        }
        Space(5.f);
        {
            const bool ok = StreamMode::OverlayReady();
            char sb[160];
            snprintf(sb, sizeof(sb), "%s: %s", I18N::Tr(I18N::LIVE_STATUS), StreamMode::StatusText());
            TextWrapCol(StreamMode::Enabled() && !ok ? Th().warn : Th().textFaint, "%s", sb);
        }
        if (StreamMode::Enabled() && !StreamMode::OverlayReady())
            CardHint(I18N::Tr(I18N::LIVE_UNAVAIL));
        else
            CardHint(I18N::Tr(I18N::LIVE_HINT));
        EndCard();
    }

    // 直播模式独立页：一张自带总开关 + 隐藏项 + 覆盖层状态的 Card
    static void DrawLivePage()
    {
        PageHeader(TT(NS_MISC), I18N::Tr(I18N::TAB_LIVE), false);
        BeginCard("##live_hint");
        CardHint(I18N::Tr(I18N::LIVE_DESC));
        EndCard();
        Space(9.f);
        DrawLiveCard();
    }

    // ============================================================
    //  其他（入口页 = 5 个 CardAction：宏模式 / 录制 / 皮肤 / 按键反馈 / 直播模式）
    // ============================================================
    static void DrawMiscHub()
    {
        PageHeader(TT(NS_MISC), TT(NS_HUB_MISC), false);
        BeginCard("##hub_misc_hint");
        CardHint(TT(NS_MISC_DESC));
        EndCard();
        Space(9.f);

        if (CardAction("##hub_mi0", I18N::Tr(I18N::TAB_MACRO), "auto-play macro", 54.f))
            GoPage(PG_MISC, -1, MI_MACRO);
        Space(7.f);
        if (CardAction("##hub_mi1", I18N::Tr(I18N::TAB_RECORD), "screen recorder", 54.f))
            GoPage(PG_MISC, -1, MI_RECORD);
        Space(7.f);
        if (CardAction("##hub_mi2", I18N::Tr(I18N::TAB_SKIN), "skin / .msp", 54.f))
            GoPage(PG_MISC, -1, MI_SKIN);
        Space(7.f);
        if (CardAction("##hub_mi3", TT(NS_KV_TITLE), "KeyViewer overlay", 54.f))
            GoPage(PG_MISC, -1, MI_KV);
        Space(7.f);
        if (CardAction("##hub_mi4", I18N::Tr(I18N::TAB_LIVE), "capture-proof overlay", 54.f))
            GoPage(PG_MISC, -1, MI_LIVE);
    }

    static void DrawMiscSub()
    {
        switch (s_subMI)
        {
        case MI_MACRO:  Chart4K::DrawMacroPage();             break;
        case MI_RECORD: Chart4K::DrawRecordPage();            break;
        case MI_SKIN:   DrawSkinPage();                       break;
        case MI_KV:     Chart4K::DrawKeyViewerSettingsPage(); break;
        case MI_LIVE:   DrawLivePage();                       break;
        default: break;
        }
    }

    // ============================================================
    //  设置（全局配置）
    // ============================================================
    static char s_gameDirBuf[MAX_PATH * 2] = { 0 };
    static bool s_gameDirInit = false;
    static void s_gameDirInitOnce()
    {
        if (s_gameDirInit) return;
        s_gameDirInit = true;
        snprintf(s_gameDirBuf, sizeof(s_gameDirBuf), "%s", GameDir::Get());
    }

    static void DrawSettingsPage()
    {
        s_gameDirInitOnce();
        PageHeader(TT(NS_SET), nullptr, false);

        // ---- 界面风格 ----
        BeginCard("##set_style");
        CardTitle(TT(NS_UI_STYLE), nullptr);
        {
            const char* names[16];
            int n = PaletteCount();
            if (n > 16) n = 16;
            for (int i = 0; i < n; i++) names[i] = PaletteName(i);
            int cur = Palette();
            if (Combo("##set_palette", &cur, names, n))
            {
                SetPalette(cur, true);
                Log::Printf("[UI] palette -> %d", cur);
            }
            Space(2.f);
            CardHint(TT(NS_UI_STYLE_DESC));
        }
        EndCard();
        Space(9.f);

        // ---- 语言 ----
        BeginCard("##set_lang");
        CardTitle(I18N::Tr(I18N::ST_LANG), nullptr);
        {
            const char* names[8];
            for (int i = 0; i < I18N::LANG_N; i++) names[i] = I18N::LangName((I18N::Lang)i);
            int cur = (int)I18N::GetLang();
            if (Combo("##set_langc", &cur, names, I18N::LANG_N))
            {
                I18N::SetLang((I18N::Lang)cur);
                Log::Printf("[UI] language -> %d", cur);
            }
            Space(2.f);
            CardHint(I18N::Tr(I18N::ST_LANGHINT));
        }
        EndCard();
        Space(9.f);

        // ---- 游戏目录 ----
        BeginCard("##set_gamedir");
        CardTitle(I18N::Tr(I18N::ST_GAMEDIR), nullptr);
        {
            CardHint(I18N::Tr(I18N::ST_GAMEDIR_DESC));
            Space(4.f);
            InputText("##gamedir", s_gameDirBuf, sizeof(s_gameDirBuf));
            {
                char pick[MAX_PATH * 2] = { 0 };
                if (ShellAsync::TakeFolder(pick, sizeof(pick)))
                    snprintf(s_gameDirBuf, sizeof(s_gameDirBuf), "%s", pick);
            }
            Space(5.f);
            const float sp = ImGui::GetStyle().ItemSpacing.x;
            float w = (ImGui::GetContentRegionAvail().x - sp * 2.f) / 3.f;
            if (Button("##gamedir_browse", I18N::Tr(I18N::ST_BROWSE), ImVec2(w, 27.f)))
                ShellAsync::PickFolder(I18N::Tr(I18N::ST_GAMEDIR));
            ImGui::SameLine();
            if (Button("##gamedir_apply", I18N::Tr(I18N::ST_APPLY), ImVec2(w, 27.f), BTN_PRIMARY))
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
            if (Button("##gamedir_open", I18N::Tr(I18N::ST_OPEN), ImVec2(w, 27.f)))
                GameDir::OpenFolder();
        }
        EndCard();
        Space(9.f);

        // ---- 配置档案 ----
        BeginCard("##set_profile");
        CardTitle(TT(NS_PROFILE), nullptr);
        {
            CardHint(TT(NS_PROFILE_DESC));
            Space(4.f);
            static char s_profileName[64] = "default";
            InputText("##confname", s_profileName, sizeof(s_profileName), TT(NS_NAME_HINT));
            Space(5.f);
            std::string dir  = ProfileDir();
            std::string path = dir + "\\" + (s_profileName[0] ? s_profileName : "default") + ".cfg";
            const float sp = ImGui::GetStyle().ItemSpacing.x;
            float bw = (ImGui::GetContentRegionAvail().x - sp) * 0.5f;
            if (Button("##cfg_save", TT(NS_SAVE), ImVec2(bw, 27.f), BTN_PRIMARY))
                ProfileMsg = ProfileWrite(path) ? I18N::Tr(I18N::ST_CFG_SAVED) : I18N::Tr(I18N::ST_CFG_FAIL);
            ImGui::SameLine();
            if (Button("##cfg_load", TT(NS_LOAD), ImVec2(bw, 27.f)))
                ProfileMsg = ProfileRead(path) ? I18N::Tr(I18N::ST_CFG_LOADED) : I18N::Tr(I18N::ST_CFG_FAIL);
            Space(5.f);
            if (Button("##cfg_del", TT(NS_DELETE), ImVec2(bw, 27.f)))
            {
                DeleteFileA(path.c_str());
                ProfileMsg = I18N::Tr(I18N::ST_CFG_FAIL);
            }
            ImGui::SameLine();
            if (Button("##cfg_open", TT(NS_OPEN_DIR), ImVec2(bw, 27.f)))
                ShellAsync::Open(dir.c_str());

            // 已有档案（点击即选中）
            Space(7.f);
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
                    char idb[300];
                    snprintf(idb, sizeof(idb), "##pf_%s", nm.c_str());
                    bool sel = (nm == s_profileName);
                    if (Button(idb, nm.c_str(), ImVec2(-1.f, 24.f), sel ? BTN_PRIMARY : BTN_GHOST))
                        snprintf(s_profileName, sizeof(s_profileName), "%s", nm.c_str());
                    Space(4.f);
                    cnt++;
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
            if (cnt == 0)
                TextFaint("%s", I18N::Tr(I18N::ST_CFG_SELECT));

            if (!ProfileMsg.empty())
            {
                Space(4.f);
                TextCol(Th().good, "%s", ProfileMsg.c_str());
            }
        }
        EndCard();
        Space(9.f);

        // ---- 键位说明 ----
        BeginCard("##set_keys");
        CardTitle(TT(NS_KEYBIND), nullptr);
        CardHint(TT(NS_KEYBIND_DESC));
        EndCard();

    }
// ============================================================
    //  皮肤列表行（自绘；点击 = 应用）
    // ============================================================
    template <typename F>
    static void RowSkin(const char* label, const char* sub, bool inUse, const char* id, F onClick)
    {
        const float h = 48.f;
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        if (w < 80.f) w = 80.f;

        ImGui::PushID(id);
        bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h));
        bool hov     = ImGui::IsItemHovered();
        ImGui::PopID();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (inUse)     dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), A(Th().accent, 0.15f), 8.f);
        else if (hov)  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 14), 8.f);
        if (inUse)
            dl->AddRectFilled(ImVec2(p.x + 1.f, p.y + 8.f), ImVec2(p.x + 4.f, p.y + h - 8.f),
                              Th().accent, 1.5f);

        dl->PushClipRect(ImVec2(p.x + 12.f, p.y), ImVec2(p.x + w - 80.f, p.y + h), true);
        dl->AddText(ImVec2(p.x + 12.f, p.y + 8.f), inUse ? Th().accent : Th().text,
                    label ? label : "");
        if (sub && sub[0])
            dl->AddText(ImVec2(p.x + 12.f, p.y + 26.f), Th().textFaint, sub);
        dl->PopClipRect();

        {
            const char* tag = inUse ? I18N::Tr(I18N::ST_SKIN_INUSE) : I18N::Tr(I18N::ST_SKIN_APPLY);
            ImVec2 ts = ImGui::CalcTextSize(tag);
            ImU32  tc = inUse ? Th().accent : (hov ? Th().text : Th().textDim);
            dl->AddText(ImVec2(p.x + w - ts.x - 16.f, p.y + (h - ts.y) * 0.5f), tc, tag);
        }
        if (clicked && !inUse) onClick();
    }

    // ============================================================


    // ============================================================
    //  皮肤页（按模式独立）
    // ============================================================
    static void DrawSkinPage()
    {
        static int s_skinSelMode = 0;

        PageHeader(I18N::Tr(I18N::ST_SKIN_TITLE), nullptr, false);

        // ---- 模式选择 ----
        BeginCard("##skin_modesel");
        {
            CardHint(I18N::Tr(I18N::ST_SKIN_CURRENT));
            Space(4.f);
            const float sp = ImGui::GetStyle().ItemSpacing.x;
            float bw = (ImGui::GetContentRegionAvail().x - sp * (Chart4K::kModeN - 1)) / (float)Chart4K::kModeN;
            if (bw < 40.f) bw = 40.f;
            for (int mi = 0; mi < Chart4K::kModeN; mi++)
            {
                if (mi) ImGui::SameLine();
                char id[32];
                snprintf(id, sizeof(id), "##skm%d", mi);
                if (Button(id, Chart4K::ApiModeName(mi), ImVec2(bw, 26.f),
                           s_skinSelMode == mi ? BTN_PRIMARY : BTN_NORMAL))
                    s_skinSelMode = mi;
            }
            Space(4.f);
            if (s_skinSelMode == Chart4K::kModeCatch)
                TextWrapCol(Th().textDim, "%s", I18N::Tr(I18N::LBL_CATCH_HINT));
            else if (s_skinSelMode == Chart4K::kModePad)
                TextWrapCol(Th().warn, "%s", I18N::Tr(I18N::LBL_16K_BUILTIN_ONLY));
            else
                CardHint(I18N::Tr(I18N::ST_SKIN_DESC));
        }
        EndCard();
        Space(9.f);

        // ---- 列表 ----
        const int selMode = s_skinSelMode;
        const int n = Chart4K::SkinCount();
        const char* active = Chart4K::SkinActiveForMode(selMode);

        BeginCard("##skin_list");
        {
            CardTitle(I18N::Tr(I18N::ST_SKIN_MODE), nullptr);
            if (selMode == Chart4K::kModeCatch)
            {
                TextCol(Th().good, "Malody Dylamo  (built-in)");
            }
            else if (selMode == Chart4K::kModePad)
            {
                const bool on = Chart4K::SkinBuiltinModeForMode(selMode);
                RowSkin(I18N::Tr(I18N::ST_SKIN_BUILTIN), Chart4K::SkinName(0), on, "##skrow_pad",
                        [&] { Chart4K::SkinSetBuiltinModeForMode(selMode, true);
                              ProfileMsg = I18N::Tr(I18N::ST_SKIN_APPLIED); });
            }
            else
            {
                // 内置皮肤（固定版式）
                {
                    const bool on = Chart4K::SkinBuiltinModeForMode(selMode);
                    RowSkin(I18N::Tr(I18N::ST_SKIN_BUILTIN), Chart4K::SkinName(0), on, "##skrow_bi",
                            [&] { Chart4K::SkinSetBuiltinModeForMode(selMode, true);
                                  ProfileMsg = I18N::Tr(I18N::ST_SKIN_APPLIED); });
                }
                if (n <= 0)
                    TextCol(Th().warn, "%s", I18N::Tr(I18N::ST_SKIN_NONE));
                for (int i = 0; i < n; i++)
                {
                    const char* path = Chart4K::SkinPathAt(i);
                    const char* title = Chart4K::SkinTitle(i);
                    const char* cr = Chart4K::SkinCreator(i);
                    bool inUse = !Chart4K::SkinBuiltinModeForMode(selMode) && active && path &&
                                 _stricmp(active, path) == 0;
                    char sub[512];
                    if (cr && cr[0]) snprintf(sub, sizeof(sub), "%s  -  %s", Chart4K::SkinName(i), cr);
                    else             snprintf(sub, sizeof(sub), "%s", path ? path : "");
                    char id[32];
                    snprintf(id, sizeof(id), "##skrow%d", i);
                    RowSkin((title && title[0]) ? title : Chart4K::SkinName(i), sub, inUse, id, [&] {
                        if (Chart4K::SkinSetActiveFullForMode(selMode, path))
                            ProfileMsg = I18N::Tr(I18N::ST_SKIN_APPLIED);
                    });
                }
            }
        }
        EndCard();
        Space(9.f);

        // ---- 当前 / 操作 ----
        BeginCard("##skin_act");
        {
            CardTitle(I18N::Tr(I18N::ST_SKIN_CURRENT), nullptr);
            const char* act = Chart4K::SkinActiveForMode(selMode);
            TextWrap("%s", (act && act[0]) ? act : "-");
            Space(5.f);
            if (Button("##skin_import", I18N::Tr(I18N::ST_SKIN_IMPORT), ImVec2(-1.f, 28.f)))
                ShellAsync::PickFile(L"Malody Skin (*.msp)\0*.msp\0All files\0*.*\0",
                                     L"Import Malody skin (.msp)");
            {
                char msp[MAX_PATH * 2] = { 0 };
                if (ShellAsync::TakeFile(msp, sizeof(msp)))
                {
                    char msg[320] = { 0 };
                    wchar_t wmsp[MAX_PATH * 2] = { 0 };
                    MultiByteToWideChar(CP_UTF8, 0, msp, -1, wmsp, MAX_PATH * 2);
                    if (Chart4K::SkinImportMsp(wmsp, msg, sizeof(msg)))
                        SkinMsg = std::string(I18N::Tr(I18N::ST_SKIN_IMPORTED)) + msg;
                    else
                        SkinMsg = std::string(I18N::Tr(I18N::ST_SKIN_IMPORT_FAIL)) + ": " +
                                  (msg[0] ? msg : "failed");
                }
            }
            Space(5.f);
            const float sp = ImGui::GetStyle().ItemSpacing.x;
            float w = (ImGui::GetContentRegionAvail().x - sp) * 0.5f;
            if (Button("##skin_open", I18N::Tr(I18N::ST_SKIN_OPEN), ImVec2(w, 27.f)))
                Chart4K::SkinOpenFolder();
            ImGui::SameLine();
            if (Button("##skin_rescan", I18N::Tr(I18N::ST_SKIN_RESCAN), ImVec2(w, 27.f)))
                Chart4K::SkinRescan();
            if (!SkinMsg.empty())
            {
                Space(4.f);
                TextWrapCol(Th().good, "%s", SkinMsg.c_str());
            }
        }
        EndCard();
    }

    // ============================================================
    //  关于
    // ============================================================
    static void DrawAboutPage()
    {
        static void* s_logo = nullptr;
        static int   s_logoFrame = 0;
        if (!s_logo && (s_logoFrame++ % 30) == 0)
            s_logo = RenderHook_LoadEmbeddedPng(kLogoResId, "adofaiperfect-logo");

        PageHeader(TT(NS_ABOUT), nullptr, false);

        BeginCard("##about_head");
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
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(logo, logo));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRect(p0, ImVec2(p0.x + logo, p0.y + logo), Th().line, 18.f, 0, 2.f);
                dl->AddCircle(ImVec2(p0.x + logo * 0.5f, p0.y + logo * 0.5f), logo * 0.30f,
                              A(Th().accent, 0.75f), 48, 3.f);
            }
            Space(6.f);
            {
                const char* name = "ADOFAI-PERFECT";
                float tw = ImGui::CalcTextSize(name).x;
                ImGui::SetCursorPosX(x0 + (avail - tw) * 0.5f);
                TextBig(26.f, Th().text, name);
            }
            Space(2.f);
            {
                char ver[96];
                snprintf(ver, sizeof(ver), "%s %s", TT(NS_VERSION), kAppVersion);
                float tw = ImGui::CalcTextSize(ver).x;
                ImGui::SetCursorPosX(x0 + (avail - tw) * 0.5f);
                TextCol(Th().accent, "%s", ver);
            }
            Space(4.f);
            {
                const char* tag = I18N::Tr(I18N::ST_ABOUTTXT);
                float tw = ImGui::CalcTextSize(tag).x;
                if (tw <= avail)
                {
                    ImGui::SetCursorPosX(x0 + (avail - tw) * 0.5f);
                    TextDim("%s", tag);
                }
                else
                {
                    TextWrap("%s", tag);
                }
            }
        }
        EndCard();
        Space(9.f);

        BeginCard("##about_link");
        {
            CardHint(I18N::Tr(I18N::ABOUT_HINT));
            Space(5.f);
            const float sp = ImGui::GetStyle().ItemSpacing.x;
            float bw = (ImGui::GetContentRegionAvail().x - sp) * 0.5f;
            if (Button("##about_video", I18N::Tr(I18N::ABOUT_OPEN), ImVec2(bw, 28.f), BTN_PRIMARY))
                ShellAsync::Open(kAppVideoUrl);
            ImGui::SameLine();
            if (Button("##about_gh", I18N::Tr(I18N::ABOUT_GITHUB), ImVec2(bw, 28.f)))
                ShellAsync::Open(kAppRepoUrl);
            Space(5.f);
            if (Button("##about_cfg", I18N::Tr(I18N::ST_OPENCFG), ImVec2(-1.f, 27.f)))
                ShellAsync::Open(I18N::Prefs::Dir());
            Space(6.f);
            TextFaint("%s", I18N::Tr(I18N::ABOUT_MADE));
        }
        EndCard();
    }
    // ============================================================
    //  辅助读谱
    // ============================================================
    static void DrawReadPage()
    {
        PageHeader(TT(NS_READ), TT(NS_LIVE_DESC), false);
        Chart4K::DrawReadSettingsPage();
    }

    // ============================================================
    //  侧边栏（Etherium Tab：图标 + 文字，选中 34,33,34 圆角底）
    // ============================================================
    struct NavDef { int pg; int icon; int ns; };
    static const NavDef kNav[PG_N] = {
        { PG_BASIC,  IC_HOME,  NS_BASIC  },
        { PG_STATUS, IC_LIVE,  NS_STATUS },
        { PG_READ,   IC_EYE,   NS_READ   },
        { PG_TRACK,  IC_TRACK, NS_TRACK  },
        { PG_MISC,   IC_MORE,  NS_MISC   },
        { PG_MODMGR, IC_MOD,   NS_MODMGR },
        { PG_SET,    IC_GEAR,  NS_SET    },
        { PG_ABOUT,  IC_INFO,  NS_ABOUT  },
    };

    static void SidebarGo(int pg)
    {
        if (pg == PG_TRACK) s_subTR = -1;
        if (pg == PG_MISC)  s_subMI = -1;
        s_page = pg;
    }

    // 鼠标数据是否可信：ImGui 在 Unity 主线程调用 NewFrame 时，光标偶尔会脱离窗口，
    // MousePos/MouseDelta 会出现 -FLT_MAX 之类的野值；此时绝不能用于拖动 / 缩放。
    static bool MouseSane()
    {
        ImGuiIO& io = ImGui::GetIO();
        const ImVec2 mp = io.MousePos, md = io.MouseDelta;
        const float lim = 3000.f;
        if (!(mp.x > -lim && mp.x < lim && mp.y > -lim && mp.y < lim)) return false;
        if (!(md.x > -900.f && md.x < 900.f && md.y > -900.f && md.y < 900.f)) return false;
        if (io.DisplaySize.x < 100.f || io.DisplaySize.y < 100.f) return false;
        return true;
    }

    // 把窗口钳制在可视区域内（至少留 90x40 在屏内），避免拖出屏幕后「界面消失」
    static void ClampMainPos()
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.DisplaySize.x < 100.f || io.DisplaySize.y < 100.f) return;
        const float minX = -s_mainSize.x + 90.f;
        const float maxX = io.DisplaySize.x - 90.f;
        const float minY = 0.f;
        const float maxY = io.DisplaySize.y - 40.f;
        if (s_mainPos.x < minX) s_mainPos.x = minX;
        if (s_mainPos.x > maxX) s_mainPos.x = maxX;
        if (s_mainPos.y < minY) s_mainPos.y = minY;
        if (s_mainPos.y > maxY) s_mainPos.y = maxY;
    }

    static void DrawSidebar()
    {
        // 拖动把手：仅覆盖侧栏顶部品牌区（不与 Tab 重叠）
        ImGui::SetCursorPos(ImVec2(0.f, 0.f));
        ImGui::InvisibleButton("##sidedrag", ImVec2(kSideW, kTopH));
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && MouseSane())
        {
            s_mainPos = ImVec2(s_mainPos.x + ImGui::GetIO().MouseDelta.x,
                               s_mainPos.y + ImGui::GetIO().MouseDelta.y);
            ClampMainPos();
        }

        for (int i = 0; i < PG_N; i++)
        {
            ImGui::SetCursorPos(ImVec2(kTabX, kTabY + (float)i * (kTabH + kTabGap)));
            char id[24];
            snprintf(id, sizeof(id), "##nav%d", i);
            if (Tab(id, kNav[i].icon, TT(kNav[i].ns), s_page == kNav[i].pg,
                    ImVec2(kTabW, kTabH)))
                SidebarGo(kNav[i].pg);
        }
    }
    // ============================================================
    //  内容区分发
    // ============================================================
    static const char* TrackTitle(int sub)
    {
        switch (sub)
        {
        case TR_4K:    return I18N::Tr(I18N::TAB_4K);
        case TR_5K:    return I18N::Tr(I18N::TAB_5K);
        case TR_6K:    return I18N::Tr(I18N::TAB_6K);
        case TR_10K:   return I18N::Tr(I18N::TAB_10K);
        case TR_16K:   return I18N::Tr(I18N::TAB_16K);
        case TR_CATCH: return I18N::Tr(I18N::TAB_CATCH);
        case TR_8K:    return I18N::Tr(I18N::TAB_8K);
        case TR_OSU:   return I18N::Tr(I18N::TAB_OSU);
        default:       return "";
        }
    }
    static const char* MiscTitle(int sub)
    {
        switch (sub)
        {
        case MI_MACRO:  return I18N::Tr(I18N::TAB_MACRO);
        case MI_RECORD: return I18N::Tr(I18N::TAB_RECORD);
        case MI_SKIN:   return I18N::Tr(I18N::TAB_SKIN);
        case MI_KV:     return TT(NS_KV_TITLE);
        case MI_LIVE:   return I18N::Tr(I18N::TAB_LIVE);
        default:        return "";
        }
    }

    // 当前页面标题（顶栏用）——与页面体读同一份状态，永不脱节
    static const char* PageTitleText()
    {
        if (s_page >= 0 && s_page < PG_N) return TT(kNav[s_page].ns);
        return "";
    }
    static float s_subX = 0.f;   // 子标签起始 x（由顶栏标题宽度决定）
    static int   s_trScrolled = -999;   // 轨道子标签：上次已滚动的选中项
    static int   s_miScrolled = -999;   // 其它子标签：上次已滚动的选中项

    // ----------------------------------------------
    // 子导航（Etherium SubTab 行：位于内容面板上方的 y=12 处）
    // ----------------------------------------------
    static void DrawTrackTabs()
    {
        const char* kL[TR_N + 1] = {
            TT(NS_SELECT), I18N::Tr(I18N::TAB_4K), I18N::Tr(I18N::TAB_5K),
            I18N::Tr(I18N::TAB_6K), I18N::Tr(I18N::TAB_10K),
            I18N::Tr(I18N::TAB_16K), I18N::Tr(I18N::TAB_CATCH),
            I18N::Tr(I18N::TAB_8K), I18N::Tr(I18N::TAB_OSU)
        };
        const float avail = ImGui::GetWindowSize().x - s_subX - 8.f;
        if (avail < 70.f) return;
        ImGui::SetCursorPos(ImVec2(s_subX, 8.f));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 7.f);
        ImGui::BeginChild("##trtabs", ImVec2(avail, kTabH + 9.f), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar |
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
        for (int i = 0; i < TR_N + 1; i++)
        {
            if (i) ImGui::SameLine(0.f, 9.f);
            char id[24];
            snprintf(id, sizeof(id), "##trtab%d", i);
            float bw = ImGui::CalcTextSize(kL[i]).x + 24.f;
            if (bw < 74.f) bw = 74.f;
            if (SubTab(id, kL[i], s_subTR == (i - 1), ImVec2(bw, kTabH)))
                s_subTR = i - 1;
            if (i == s_subTR + 1 && s_trScrolled != s_subTR)
            {
                ImGui::SetScrollHereX(0.5f);
                s_trScrolled = s_subTR;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    static void DrawMiscTabs()
    {
        const char* kL[MI_N + 1] = {
            TT(NS_SELECT), I18N::Tr(I18N::TAB_MACRO), I18N::Tr(I18N::TAB_RECORD),
            I18N::Tr(I18N::TAB_SKIN), TT(NS_KV_TITLE), I18N::Tr(I18N::TAB_LIVE)
        };
        const float avail = ImGui::GetWindowSize().x - s_subX - 8.f;
        if (avail < 70.f) return;
        ImGui::SetCursorPos(ImVec2(s_subX, 8.f));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 7.f);
        ImGui::BeginChild("##mitabs", ImVec2(avail, kTabH + 9.f), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar |
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
        for (int i = 0; i < MI_N + 1; i++)
        {
            if (i) ImGui::SameLine(0.f, 10.f);
            char id[24];
            snprintf(id, sizeof(id), "##mitab%d", i);
            float bw = ImGui::CalcTextSize(kL[i]).x + 24.f;
            if (bw < 100.f) bw = 100.f;
            if (SubTab(id, kL[i], s_subMI == (i - 1), ImVec2(bw, kTabH)))
                s_subMI = i - 1;
            if (i == s_subMI + 1 && s_miScrolled != s_subMI)
            {
                ImGui::SetScrollHereX(0.5f);
                s_miScrolled = s_subMI;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    // 页面体（在内容面板子窗口内绘制）
    static void DrawPageBody()
    {
        switch (s_page)
        {
        case PG_BASIC:
            DrawBasicPage();
            break;
        case PG_STATUS:
            DrawStatusPage();
            break;
        case PG_READ:
            DrawReadPage();
            break;
        case PG_TRACK:
            if (s_subTR < 0) DrawTrackHub();
            else             DrawTrackSub();
            break;
        case PG_MISC:
            if (s_subMI < 0) DrawMiscHub();
            else             DrawMiscSub();
            break;
        case PG_MODMGR:
            ModManager::DrawPage();
            break;
        case PG_SET:
            DrawSettingsPage();
            break;
        case PG_ABOUT:
            DrawAboutPage();
            break;
        default:
            break;
        }
    }

    // ============================================================
    //  主窗口（Etherium：左 175px 导航 + 60px 顶栏 + 圆角内容面板）
    // ============================================================
    static void DrawMain()
    {
        ImGui::SetNextWindowPos(s_mainPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(s_mainSize, ImGuiCond_Always);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoScrollWithMouse |
                                 ImGuiWindowFlags_NoSavedSettings;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.f);
        ImGui::Begin("##adofai_main", nullptr, flags);
        StreamMode::MarkWindow("##adofai_main", StreamMode::EL_MENU);   // 直播模式：整块菜单（含卡片子窗口）

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wpos  = ImGui::GetWindowPos();
        ImVec2 wsize = ImGui::GetWindowSize();
        ImVec2 wmax(wpos.x + wsize.x, wpos.y + wsize.y);
        const float W = wsize.x, H = wsize.y;


        // ---- 侧栏 / 顶栏底（Etherium 同款双层色块）----
        dl->AddRectFilled(wpos, ImVec2(wpos.x + kSideW, wmax.y), Th().sideBg, 12.f,
                          ImDrawFlags_RoundCornersLeft);
        dl->AddRectFilled(ImVec2(wpos.x + kSideW, wpos.y), ImVec2(wmax.x, wpos.y + kTopH),
                          Th().topBg, 12.f, ImDrawFlags_RoundCornersTopRight);

        // ---- 品牌（Bold 22px）----
        {
            ImFont* f = ImGui::GetFont();
            dl->AddText(f, 21.f, ImVec2(wpos.x + 24.f, wpos.y + 15.f), Th().text, "ADOFAI");
            dl->AddText(f, 21.f, ImVec2(wpos.x + 24.f, wpos.y + 35.f), Th().accent, "PERFECT");
            ImVec2 c(wpos.x + kSideW - 26.f, wpos.y + 30.f);
            dl->AddQuadFilled(ImVec2(c.x, c.y - 9.f), ImVec2(c.x + 9.f, c.y),
                              ImVec2(c.x, c.y + 9.f), ImVec2(c.x - 9.f, c.y), A(Th().accent, 0.28f));
            dl->AddQuad(ImVec2(c.x, c.y - 9.f), ImVec2(c.x + 9.f, c.y),
                        ImVec2(c.x, c.y + 9.f), ImVec2(c.x - 9.f, c.y), Th().accent, 1.6f);
        }

        // ---- 顶栏：当前页标题 + 面包屑 + 折叠 ----
        {
            ImFont* f = ImGui::GetFont();
            const char* pt = PageTitleText();
            const float tw = f->CalcTextSizeA(17.f, FLT_MAX, 0.f, pt).x;
            dl->AddText(f, 17.f, ImVec2(wpos.x + kSideW + 18.f, wpos.y + 21.f), Th().text, pt);
            s_subX = kSideW + 18.f + tw + 22.f;
            ImGui::SetCursorPos(ImVec2(W - 38.f, 20.f));
            if (IconButton("##collapse", IC_MINUS, ImVec2(22.f, 22.f), BTN_GHOST))
                s_collapsed = true;
        }

        // ---- 侧栏导航 ----
        DrawSidebar();


        // ---- 子导航行（内容面板之上）----
        if (s_page == PG_TRACK)      DrawTrackTabs();
        else if (s_page == PG_MISC)  DrawMiscTabs();

        // ---- 内容面板 ----
        {
            ImGui::SetCursorPos(ImVec2(kSideW, kPanelY));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPadX, kPadY));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 9.f));
            ImGui::BeginChild("##content", ImVec2(W - kSideW - 12.f, H - kPanelY - 12.f),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, 0);
            DrawPageBody();
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
        }

        // ---- 右下角缩放柄 ----
        {
            const float gs = 16.f;
            ImGui::SetCursorPos(ImVec2(W - gs - 5.f, H - gs - 5.f));
            ImGui::InvisibleButton("##wndresize", ImVec2(gs, gs));
            bool hov = ImGui::IsItemHovered() || ImGui::IsItemActive();
            if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
            if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && MouseSane())
            {
                s_mainSize.x += ImGui::GetIO().MouseDelta.x;
                s_mainSize.y += ImGui::GetIO().MouseDelta.y;
                if (s_mainSize.x < 720.f) s_mainSize.x = 720.f;
                if (s_mainSize.y < 460.f) s_mainSize.y = 460.f;
                if (ImGui::GetIO().DisplaySize.x > 200.f && s_mainSize.x > ImGui::GetIO().DisplaySize.x)
                    s_mainSize.x = ImGui::GetIO().DisplaySize.x;
                if (ImGui::GetIO().DisplaySize.y > 200.f && s_mainSize.y > ImGui::GetIO().DisplaySize.y)
                    s_mainSize.y = ImGui::GetIO().DisplaySize.y;
            }
            ImVec2 gp(wmax.x - gs - 5.f, wmax.y - gs - 5.f);
            for (int i = 0; i < 3; i++)
            {
                const float o = 4.f + i * 4.f;
                dl->AddLine(ImVec2(gp.x + gs - o, gp.y + gs),
                            ImVec2(gp.x + gs, gp.y + gs - o),
                            IM_COL32(230, 235, 245, hov ? 170 : 70), 1.4f);
            }
        }

        ImGui::End();
        ImGui::PopStyleVar(2);

        // ---- MOD 管理器模态层（删除确认 / MOD 设置）----
        //   必须在主窗口 End() 之后调用：它是独立的顶层窗口（自带标题栏 + X）。
        ModManager::DrawModal();
    }    // ============================================================
    //  折叠后的小圆点
    // ============================================================
    static void DrawDot()
    {
        ImGui::SetNextWindowPos(s_dotPos, ImGuiCond_Always);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoSavedSettings;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##adofai_dot", nullptr, flags);
        StreamMode::MarkWindow("##adofai_dot", StreamMode::EL_MENU);    // 直播模式：收起态圆点

        ImVec2 wpos = ImGui::GetWindowPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float t = (float)ImGui::GetTime();

        ImGui::InvisibleButton("##dotbtn", ImVec2(34.f, 34.f));
        bool hovered = ImGui::IsItemHovered();
        bool active  = ImGui::IsItemActive();

        if (active && ImGui::IsMouseDragging(0) && MouseSane())
            s_dotPos = ImVec2(s_dotPos.x + ImGui::GetIO().MouseDelta.x,
                              s_dotPos.y + ImGui::GetIO().MouseDelta.y);

        ImVec2 c(wpos.x + 17.f, wpos.y + 17.f);
        float pulse = 0.5f + 0.5f * sinf(t * 2.6f);
        dl->AddCircle(c, 15.f + pulse * 2.5f, A(Th().accent, hovered ? 0.85f : 0.45f), 0, 2.f);
        dl->AddCircleFilled(c, 11.f, A(Th().accent, 0.92f));
        dl->AddCircleFilled(c, 4.5f, IM_COL32(12, 10, 18, 235));

        static ImVec2 s_pressPos(0.f, 0.f);
        if (ImGui::IsItemActivated())
            s_pressPos = ImGui::GetIO().MousePos;
        if (ImGui::IsItemDeactivated())
        {
            ImVec2 mp = ImGui::GetIO().MousePos;
            if (fabsf(mp.x - s_pressPos.x) + fabsf(mp.y - s_pressPos.y) < 6.f)
            {
                s_collapsed = false;
                s_mainPos = ImVec2(s_dotPos.x - 30.f, s_dotPos.y - 17.f);
            }
        }

        ImGui::End();
        ImGui::PopStyleVar();
    }

    // ============================================================
    //  入口
    // ============================================================
    void Draw()
    {
        static bool s_langLoaded = false;
        if (!s_langLoaded)
        {
            I18N::Load();
            s_langLoaded = true;
        }
        ApplyGlobalStyle();   // 每帧幂等：圆角 / 间距 / 半透明（UiKit 统一风格）
        S_PageFromEnv();      // 调试钩子：只读一次环境变量

        Chart4K::DrawPlayfield();        // 4K/5K/6K/10K/16K/CATCH 谱面（独立透明窗口）
        Chart4K::DrawReadOverlay();      // 辅助读谱（无轨）独立窗口
        Chart4K::DrawKeyViewerOverlay(); // KeyViewer 按键反馈叠加层

        // 自研 UMM 兼容加载器：每帧（节流）驱动一次。即使菜单隐藏也要跑，
        // 这样“MOD 加载 / 状态轮询”始终在后台推进。
        ModLoader::Tick();

        // 直播模式水印：画在游戏画面上（观众可见），即使菜单隐藏也保留
        StreamMode::DrawWatermark();

        if (!CheatState::MenuVisible.load(std::memory_order_relaxed))
            return;

        RunSelfTestOnce();
        if (s_collapsed) DrawDot();
        else             DrawMain();
    }
}
