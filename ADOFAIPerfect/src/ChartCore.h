#pragma once
// ============================================================
// ChartCore.h — 下坠辅助 / 读谱 / 按键反馈 的共享核心（内部头）
//
//   文件分工：
//     ChartCore.cpp   共享基础设施：mono 字段解析、谱面提取、歌曲时钟、
//                     判定、皮肤、下坠谱面渲染（DrawPlayfield）、通用设置页
//     Chart4K.cpp     4K 分键引擎（DFJK）
//     Chart5K.cpp     5K 分键引擎（SDFJK）
//     Chart6K.cpp     6K 分键引擎（SDFJKL）
//     Chart10K.cpp    10K 分键引擎（ASDFG+HJKL;）
//     SightRead.cpp   辅助读谱（无轨）
//     KeyViewer.cpp   按键反馈
//   四套引擎彼此独立：各自的状态机 / 指法语汇 / 连打循环，互不共用代码。
// ============================================================
#include <atomic>
#include <initializer_list>
#include <cstdint>
#include <vector>
#include "imgui.h"
#include "Log.h"

namespace Chart4K
{
    // ---------------- 模式与共享常量 ----------------
    constexpr int kMaxLanes = 10;   // 最大轨数（4K/5K/6K/10K 共用数组上限）
    constexpr int kModeN    = 4;
    extern const int kLanesOf[kModeN];

    // 分键手感约束（各引擎共同遵守，实现各自独立）
    constexpr double kFastDt      = 0.45;   // 连打阈值（<=0.45s 视为连打）
    constexpr double kRepeatGuard = 0.60;   // 同键保护窗
    constexpr double kSameHandDt  = 1.00;   // 慢速允许同手的时间窗
    constexpr double kLaneFloor   = 0.075;  // 同键物理下限（人手极限）
    constexpr double kGapPhrase   = 0.70;   // 超过该间隔算乐句边界
    constexpr double kAccent4K    = 4.40;   // 4K 大回转阈值（>=252°）
    constexpr double kMidTurn6K   = 2.90;   // 6K 中回转阈值（>=166°）

    // 手别 / 手内键序（由内到外），各键位模式独立的键盘语汇
    static inline int HandOf(int lane, int cols)
    {
        if (cols == 4) return lane < 2 ? 0 : 1;    // D F | J K
        if (cols == 5) return lane < 3 ? 0 : 1;    // S D F | J K
        if (cols == 6) return lane < 3 ? 0 : 1;    // S D F | J K L
        return lane < 5 ? 0 : 1;                   // A S D F G | H J K L ;
    }
    static int HandLanes(int hand, int cols, int* out)
    {
        int n = 0;
        if (cols == 4)
        {
            if (hand == 0) { out[n++] = 1; out[n++] = 0; }
            else           { out[n++] = 2; out[n++] = 3; }
        }
        else if (cols == 5)
        {
            if (hand == 0) { out[n++] = 2; out[n++] = 1; out[n++] = 0; }
            else           { out[n++] = 3; out[n++] = 4; }
        }
        else if (cols == 6)
        {
            if (hand == 0) { out[n++] = 2; out[n++] = 1; out[n++] = 0; }
            else           { out[n++] = 3; out[n++] = 4; out[n++] = 5; }
        }
        else
        {
            if (hand == 0) { out[n++] = 4; out[n++] = 3; out[n++] = 2; out[n++] = 1; out[n++] = 0; }
            else           { out[n++] = 5; out[n++] = 6; out[n++] = 7; out[n++] = 8; out[n++] = 9; }
        }
        return n;
    }

    // ---------------- 地砖原始数据（POD，反编译确认字段）----------------
    // 地砖原始数据（POD）
    struct RawFloor
    {
        double t;        // entryTime（song 秒）
        double tp;       // entryTimePitchAdj（真实秒）
        double ang;      // angleLength（弧度）
        int    holdLen;  // holdLength
        int    taps;     // tapsNeeded
        bool   ccw;
        bool   midSpin;  // 中旋砖（游戏自动打击，读谱需标记）
        bool   fake;     // isFake 装饰砖
        bool   autoPlay; // auto 自动砖
        bool   valid;    // 需要玩家打击的块
    };

    // ---------------- 分键引擎状态与入口 ----------------
    //   每套引擎自己的状态机（互不共享），实现在各自的 .cpp
    struct State4K
    {
        int    prevLane = -1;
        int    lastHand = 0;
        int    lastOfHand[2] = { -1, -1 };
        double holdUntil[4] = { -1e9, -1e9, -1e9, -1e9 };
        double prevTime = -1e9;
        int    streamFinger = 0;
    };
    int NextLane4K(State4K& st, int dir, double turn, double t, double hold);
    struct State5K
    {
        int    prevLane = -1;
        int    lastHand = 0;
        int    lastOfHand[2] = { -1, -1 };
        double holdUntil[5] = { -1e9, -1e9, -1e9, -1e9, -1e9 };
        double prevTime = -1e9;
        int    use[5] = { 0, 0, 0, 0, 0 };
        int    walkLane = 2, walkDir = 1, walkSteps = 0;
        int    groove = 0, grooveLane = 2;
    };
    int NextLane5K(State5K& st, int dir, double turn, double t, double hold);
    struct State6K
    {
        int    prevLane = -1;
        int    lastHand = 0;
        int    lastOfHand[2] = { -1, -1 };
        double holdUntil[6] = { -1e9, -1e9, -1e9, -1e9, -1e9, -1e9 };
        double prevTime = -1e9;
        int    streamPos = -1;
        int    streamCycle = 0;
        int    startHand = 0;
    };
    int NextLane6K(State6K& st, int dir, double turn, double t, double hold);
    struct State10K
    {
        int    prevLane = -1;
        int    lastHand = 0;
        int    lastOfHand[2] = { -1, -1 };
        double holdUntil[10] = { -1e9, -1e9, -1e9, -1e9, -1e9, -1e9, -1e9, -1e9, -1e9, -1e9 };
        double prevTime = -1e9;
        int    use[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        int    cycPos = 0, cycDir = 1, cycLeft = 0;
        int    walkLane = 4, walkDir = 1, walkSteps = 0;
        int    groove = 0, grooveLane = 4;
    };
    int NextLane10K(State10K& st, int dir, double turn, double t, double hold);

    // 各模式自述（设置页用；四模式各自提供）
    const char* ModeDesc4K();
    const char* ModeDesc5K();
    const char* ModeDesc6K();
    const char* ModeDesc10K();
    const char* ModeDesc(int mi);          // ChartCore.cpp 分发

    // ---------------- 共享时钟 / 状态（ChartCore.cpp）----------------
    extern bool g_clockSongUnits;                  // 时钟单位：true=song / false=dsp
    extern std::atomic<double> s_songTime;         // 游戏歌曲时钟（桥接采样）
    extern std::atomic<double> s_clockWall;        // 采样时的 QPC 秒
    extern std::atomic<double> s_clockRate;        // 实测 song 秒 / 墙秒
    extern std::atomic<double> s_clockMovedWall;   // 最近一次时钟前进的墙钟
    extern std::atomic<bool>   s_songStarted;      // hasSongStarted（自定义关卡可能不置位）
    extern std::atomic<bool>   s_anchored;         // 已看到本关开始（时钟重锚）
    extern std::atomic<float>  s_bpm;              // 当前 BPM

    double WallNow();
    double RenderClockOff(int offMs);              // 渲染时钟 = 采样 + QPC 外推 + 偏移

    // ---------------- 辅助读谱（SightRead.cpp）----------------
    void BuildReadTiles(const RawFloor* fl, int n);  // 地砖 → 读谱快照
    void ClearReadTiles();

    // ---------------- 共享 UI 小工具（ChartCore.cpp）----------------
    void BeginCard4K(const char* id, float height);
    void EndCard4K();
    bool MiniToggle(const char* id, bool* v);
    bool TitleToggleRow(const char* id, float titleSize, const char* title, bool* v);
    void DrawTextColored(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text);
    void DrawTextShadow(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text);
    void DrawKeyBinder(const char* pid, int modeIndex);   // 4K 系列键位设置
    float LabelCol(std::initializer_list<const char*> labels, float minW = 74.f);  // 按语言算标签列宽
    void DrawModePage(int mi);                            // 通用模式设置页
    void GetModeKeys(int mi, int* out, int* n);          // 取某模式当前键位（KeyViewer 预设）

    // ---------------- 皮肤管理（ChartCore.cpp；皮肤页） ----------------
    const char* SkinDefaultRoot();               // <DLL目录>\skin
    void        SkinRescan();                    // 重新扫描可用皮肤
    int         SkinCount();
    const char* SkinPathAt(int i);
    const char* SkinName(int i);                 // 目录名
    const char* SkinActive();                    // 当前生效目录
    bool        SkinSetActiveFull(const char* dir);  // 校验+持久化+重载贴图
    bool        SkinBuiltinMode();               // true=内置皮肤（固定版式，推荐）
    void        SkinSetBuiltinMode(bool on);     // 切换 内置 / 外部 MSP（持久化+重载）
    void        SkinOpenFolder();                // 打开 <DLL目录>\skin
    const char* SkinTitle(int i);                // 皮肤标题（MSP meta）
    const char* SkinCreator(int i);              // 皮肤作者
    bool        SkinImportMsp(const wchar_t* mspPath, char* msg, int n);  // 导入 .msp 皮肤包
    bool        SkinImportMspDialog(char* msg, int n);                    // 选文件并导入

    // ---------------- 配置档案访问器（设置页） ----------------
    //   mode which: 0=en 1=speed 2=offset 3=style 4=judge 5=uphide 6=dnhide 7=autooff
    int   ModeLaneCount(int mi);
    int   ModeSettingGet(int mi, int which);
    void  ModeSettingSet(int mi, int which, int v);
    int   ModeKeyGet(int mi, int slot);
    void  ModeKeySet(int mi, int slot, int vk);
    //   KeyViewer which: 0=on 1=anchor 2=posX 3=posY 4=scale 5=opacity 6=vert 7=rain
    //                    8=wrap 9=perRow 10=showKps 11=showCount 12=showTotal 13=bg
    int   KVSettingGet(int which);
    void  KVSettingSet(int which, int v);
    int   KVKeyCount();
    int   KVKeyGet(int i);
    void  KVKeySet(int i, int vk);
    void  KVKeySetCount(int n);

    // ---------------- 脚本 / Mod 访问器（ScriptApi 转发，Lua 的 game.* 与原生 Mod 共用）
    //   mi 越界自动归为无效；读取值在未激活时返回 0 / "" / 1.0。
    int         ApiModeIndex();                      // -1 .. kModeN-1
    bool        ApiModeEnabled(int mi);
    void        ApiSetModeEnabled(int mi, bool on);  // 四模式互斥
    int         ApiSpeed(int mi);      void ApiSetSpeed(int mi, int v);
    int         ApiOffset(int mi);     void ApiSetOffset(int mi, int v);
    int         ApiStyle(int mi);      void ApiSetStyle(int mi, int v);
    int         ApiStyleCount();
    const char* ApiStyleName(int i);
    const char* ApiModeName(int mi);
    int         ApiLevel();
    double      ApiSongTime();
    double      ApiBpm();
    bool        ApiPlaying();
    int         ApiJudgeCombo();
    float       ApiJudgeAcc();
    int         ApiJudgeCount(int kind);             // 0 MARV 1 PERF 2 GOOD 3 MISS
    const char* ApiLevelName();
}
