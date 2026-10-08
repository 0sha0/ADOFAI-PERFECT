#pragma once
// ============================================================
// Lang.h — 多语言（i18n）
//   支持：简体中文 / 繁體中文 / English / 日本語 / Русский
//   后续更新接口：
//     · I18N::Register(zhCN, zhTW, en, ja, ru) 随时追加字符串，返回新 ID
//     · I18N::SetLang / GetLang / Tr(id) —— 新增语言只需扩展 Lang 枚举与表
//   语言选择持久化到 DLL 同目录 adofai_perfect.cfg（重启后保留）
// ============================================================
namespace I18N
{
    enum Lang { ZH_CN = 0, ZH_TW, EN, JA, RU, LANG_N };
    enum Str : int
    {
        TAB_FEATURES = 0, TAB_STATUS, TAB_READ, TAB_4K, TAB_5K, TAB_6K, TAB_10K,
        TAB_KEYVIEWER, TAB_SETTINGS, TAB_MOD, TAB_LUA,
        FEAT_NODEATH, FEAT_AUTOCOMBO, FEAT_UNLOAD, BTN_RESET,
        LBL_SPEED, LBL_OFFSET, LBL_STYLE, LBL_KEYS, LBL_HITFX, LBL_JUDGE, LBL_MINION,
        LBL_POS, LBL_SIZE, LBL_FINETUNE, LBL_OPACITY,
        RD_TITLE, RD_DESC, RD_LAYOUT, RD_L_SIDE, RD_L_BOTTOM, RD_L_TOP, RD_AHEAD,
        RD_DENSITY, RD_GRID, RD_DIAL, RD_RHYTHM, RD_MARKS, RD_HINT, RD_CALIB, RD_NODATA,
        KV_ON, KV_VERT, KV_SHOWKPS, KV_SHOWCOUNT, KV_SHOWTOTAL, KV_ADD, KV_DEFAULT,
        KV_CAPTURE, KV_DEL, KV_DESC,
        ST_TITLE, ST_LANG, ST_OPENCFG, ST_ABOUT, ST_ABOUTTXT, ST_LANGHINT,
        RD_INPLAY_HINT,
        ST_LEVEL, ST_STATE, ST_PROGRESS, ST_ACC, ST_DEATHS_CK, ST_CHECKPOINT,
        ST_CONNECTING, ST_NOCONTROLLER, ST_HINT_BG, ST_INGAME, ST_UNKNOWN, ST_FOOTER,
        LBL_BOARD, LBL_EDIT, LBL_OK, LBL_CANCEL, LBL_RESTORE, LBL_PRESSKEY, LBL_APPLY,
        LBL_KEYDUP, LBL_PRESSNEW,
        RD_SCALE,
        KV_KEYSEDIT, KV_INFO,
        LBL_POS_LT, LBL_POS_RT, LBL_POS_LB, LBL_POS_RB, KV_TOTAL,
        KV_RAIN, KV_WRAP, KV_PERROW, KV_RESETSTATS, KV_PRESET, KV_MAX, KV_AVG,
        LBL_JUDGEW, LBL_JW0, LBL_JW1, LBL_JW2, LBL_BIAS, LBL_CALIB,
        LBL_FAST, LBL_SLOW, LBL_DRAG, KV_BG,
        LBL_UPHIDE, LBL_DNHIDE, LBL_AUTOOFF,
        LUA_TITLE, LUA_AUTORUN, LUA_EMPTY, LUA_CONSOLE,
        MOD_TITLE, MOD_HINT, MOD_ENABLED, MOD_EMPTY, MOD_ABI_HINT,
        BTN_REFRESH, BTN_OPEN_DIR, BTN_RUNALL, BTN_STOPALL, BTN_CLEAR, BTN_CLEAR2,
        BTN_RUN, BTN_STOP, BTN_RELOAD, BTN_EXEC,
        LBL_STOPPED, LBL_RUNNING, LBL_LOADFAIL,
        MOD_OPEN, MOD_LOADING, MOD_UMM_HINT, MOD_RUNTIME_MISSING,
        // ---- 皮肤页 / 设置页（游戏目录 / 配置档案） / 读谱页分区标题 ----
        TAB_SKIN,
        ST_GAMEDIR, ST_GAMEDIR_DESC, ST_BROWSE, ST_APPLY, ST_OPEN, ST_GAMEDIR_OK, ST_GAMEDIR_BAD,
        ST_CFG_TITLE, ST_CFG_DESC, ST_CFG_NAME, ST_CFG_SAVE, ST_CFG_LOAD, ST_CFG_DEL,
        ST_CFG_OPEN, ST_CFG_SAVED, ST_CFG_LOADED, ST_CFG_FAIL, ST_CFG_SELECT,
        ST_SKIN_TITLE, ST_SKIN_DESC, ST_SKIN_CURRENT, ST_SKIN_APPLY, ST_SKIN_OPEN,
        ST_SKIN_RESCAN, ST_SKIN_NONE, ST_SKIN_APPLIED, ST_SKIN_INUSE, ST_SKIN_DIR,
        ST_SKIN_IMPORT, ST_SKIN_IMPORTED, ST_SKIN_IMPORT_FAIL,
        ST_SKIN_BUILTIN, ST_SKIN_BUILTIN_DESC, ST_SKIN_MODE,
        RD_SEC_LAYOUT, RD_SEC_VIEW, RD_SEC_TUNE, RD_SEC_SIGNAL, RD_LIVE,
        MOD_INSTALL, MOD_INSTALL_OK, MOD_INSTALL_FAIL, MOD_OVERWRITE, MOD_HOMEPAGE,
        MOD_REPO, MOD_REQ_MISSING,
        RD_ANGLE, RD_WINDOWS, RD_ADAPT,
        TAB_MACRO,
        LBL_AUTOPLAY, LBL_MACRO, LBL_RECORD,
        MACRO_TITLE, MACRO_DESC, MACRO_ACC, MACRO_HUMAN, MACRO_CALIB, MACRO_CALIBHINT, MACRO_MODES,
        REC_TITLE, REC_DESC, REC_ON, REC_AUTO, REC_DIR, REC_OPEN, REC_FPS, REC_MBPS, REC_STATS, REC_HINT,
        // ---- 关于页 ----
        ABOUT_VER, ABOUT_OPEN, ABOUT_GITHUB, ABOUT_HINT, ABOUT_MADE,
        // ---- 录制页（独立 PAGE） / 冰与火宏模式 ----
        TAB_RECORD, MACRO_FIRE, MACRO_FIRE_HINT,
        REC_START, REC_PAUSE, REC_RESUME, REC_PAUSED_HINT, REC_PAGEDESC,
        BUILTIN_N
    };

    void        SetLang(Lang l);
    Lang        GetLang();
    const char* Tr(int id);                  // 当前语言（越界返回 ""）
    const char* TrL(int id, Lang l);         // 指定语言
    int         Register(const char* zhCN, const char* zhTW, const char* en,
                         const char* ja, const char* ru);   // 追加字符串，返回新 ID
    const char* LangName(Lang l);            // 语言自称（用于下拉框）
    void        Load();                      // 读取 adofai_perfect.cfg
    void        Save();                      // 写回

    // ------------------------------------------------------------
    // Prefs — adofai_perfect.cfg 的通用键值读写（lang=… / game_dir=… / skin_dir=…）
    //   与 I18N 共用同一文件；旧版纯数字文件（只有语言编号）自动兼容。
    // ------------------------------------------------------------
    namespace Prefs
    {
        void        Load();
        void        Save();
        int         GetInt(const char* key, int def);
        void        SetInt(const char* key, int value);
        void        GetStr(const char* key, char* out, int n, const char* def);
        void        SetStr(const char* key, const char* value);
        const char* Dir();      // DLL 所在目录（带末尾反斜杠）
        const char* File();     // adofai_perfect.cfg 完整路径
    }
}
