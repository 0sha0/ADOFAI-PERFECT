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
    constexpr int kMaxLanes = 16;   // 最大轨数（4K/5K/6K/10K/16K(PAD)/CATCH/8K/OSU 共用数组上限）
    constexpr int kModeN    = 8;
    constexpr int kModePad  = 4;   // 16K（PAD）：只支持内置皮肤
    constexpr int kModeCatch = 5;  // CATCH：只支持内置 Dylamo 皮肤
    constexpr int kMode8K   = 6;   // 8K：标准 8 键（ASDF | JKL;）
    constexpr int kModeOsu  = 7;   // OSU（戳泡泡）：osu! standard 玩法，只支持内置皮肤
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
        if (cols == 2) return lane < 1 ? 0 : 1;    // OSU：左键(0) | 右键(1)
        if (cols == 4) return lane < 2 ? 0 : 1;    // D F | J K
        if (cols == 5) return lane < 3 ? 0 : 1;    // S D F | J K
        if (cols == 6) return lane < 3 ? 0 : 1;    // S D F | J K L
        if (cols == 16) return (lane % 4) < 2 ? 0 : 1;  // PAD 4x4：左二列 | 右二列
        if (cols == 8)  return lane < 4 ? 0 : 1;        // CATCH 8 吸附区：左 4 | 右 4
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
        else if (cols == 16)
        {
            // PAD 4x4：index = row*4+col；左手=左二列(col0,1)，右手=右二列(col2,3)。
            // 每只手由内列到外列、每列由上到下（=jubeat 的左右手分区）。
            if (hand == 0) { out[n++] = 1; out[n++] = 5; out[n++] = 9;  out[n++] = 13;
                             out[n++] = 0; out[n++] = 4; out[n++] = 8;  out[n++] = 12; }
            else           { out[n++] = 2; out[n++] = 6; out[n++] = 10; out[n++] = 14;
                             out[n++] = 3; out[n++] = 7; out[n++] = 11; out[n++] = 15; }
        }
        else if (cols == 8)
        {
            if (hand == 0) { out[n++] = 3; out[n++] = 2; out[n++] = 1; out[n++] = 0; }
            else           { out[n++] = 4; out[n++] = 5; out[n++] = 6; out[n++] = 7; }
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
        int    holdLen;  // holdLength（-1 = 普通砖；>= 0 = 长按块，0 是最短长条）
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

    // ---------------- 8K 分键引擎（Chart8K.cpp，独立算法）----------------
    //   标准 8 键 ASDF | JKL;，左右手各 4 键。写谱按 osu!mania 8K 的通行手感：
    //   · 连打用 4 键一组的内外波浪（左右手逐音交替），中央为轴；
    //   · 大回转落最外侧 A / ; 做重音；
    //   · 楼梯沿 8 键单方向扫动，跨过中央自然换手。
    struct State8K
    {
        int    prevLane = -1;
        int    lastHand = 0;
        int    lastOfHand[2] = { -1, -1 };
        double holdUntil[8] = { -1e9, -1e9, -1e9, -1e9, -1e9, -1e9, -1e9, -1e9 };
        double prevTime = -1e9;
        int    use[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        int    cycPos = 0, cycDir = 1, cycLeft = 0;
        int    walkLane = 3, walkDir = 1, walkSteps = 0;
        int    groove = 0, grooveLane = 3;
    };
    int NextLane8K(State8K& st, int dir, double turn, double t, double hold);
    const char* ModeDesc8K();
    struct PadEv;   // 前置声明（完整定义见下方 16K PAD 引擎）

    // ---------------- OSU（戳泡泡）= osu! standard 手法引擎（ChartOsu.cpp）----------------
    //   把冰火砖的转向 / 幅度映射成 512x384 判定场上的落点，生成 Jump / Stream 等
    //   osu! 标准玩法的手法（1/2 拍大跳 = Jump，1/4 拍密集小间距 = Stream）。
    //   玩家用鼠标瞄准 + 点击（左/右键 或 Z/X）；判定窗按 OD 换算（研究自 osu! 官方）：
    //     GREAT(300) = 80-6*OD · OK(100) = 140-8*OD · MEH(50) = 200-10*OD · MISS = 超窗
    //   判定结果同步回冰与火原生判定（与 CATCH 同一套打歌引擎）。
    //   style: 0=经典 1=跳(Jump) 2=串(Stream) 3=交互(Alt) 4=双押(Doubles) 5=技术(Tech)
    void GenerateOsu(const PadEv* ev, int n, int style, int level,
                     float* outX, float* outY, int* outKey, int* outHoldIdx);
    const char* ModeDescOsu();
    void DrawOsuOverlay();            // 渲染线程：OSU 满屏覆盖层（泡泡 + 判定环 + 光标 + HUD）
    void DrawOsuSettingsPage();       // 渲染线程：主窗口里的 OSU 设置页
    void DrawSettings8KPage();        // 渲染线程：8K 设置页（Chart8K.cpp）
    //   osu! 判定场（512x384，4:3）在游戏客户区里的居中矩形（像素）
    static inline void OsuFieldRect(float gw, float gh, float* x0, float* y0, float* w, float* h)
    {
        // osu! 原版坐标单位是 game pixel：1 game px = 640x480 窗口下的 1 个屏幕像素，
        // 分辨率更高时 game px 的视觉大小不变（osu! wiki: Playfield）。
        //   → 缩放系数 s = 客户区高度 / 480；判定场 512x384 game px，水平居中，
        //     垂直比窗口中心低 8 game px。
        float s = gh / 480.f;
        if (gw / 640.f < s) s = gw / 640.f;     // 比 4:3 更窄的窗口：按宽度兜底
        const float fw = 512.f * s, fh = 384.f * s;
        *w = fw; *h = fh;
        *x0 = (gw - fw) * 0.5f;
        *y0 = (gh - fh) * 0.5f + 8.f * s;
    }
    // OSU 运行时（渲染线程写入判定 / 主判定管线读取）
    struct OsuHit { double t; float x; float y; float hold; uint8_t state; };  // state 0 live 1 hit 2 miss
    int  OsuNotes(OsuHit* out, int cap, double minT = -1e18);
    int  OsuTick(double clock, float mx, float my, bool clickEdge, bool clickDown);
    float OsuCursorX(); float OsuCursorY();
    int  OsuCombo(); int OsuMaxCombo();
    float OsuLastDx(); float OsuLastDy();  // 最近一次判定的落点偏差（判定场内像素，用于快慢方向）
    int  OsuMissCount();
    void OsuReset();
    int  OsuPreemptMs(); void OsuPreemptSet(int ms);
    int  OsuOd(); void OsuOdSet(int od);

    // ---------------- 16K（PAD）分砖引擎（Chart16K.cpp，独立算法）----------------
    //   PAD 是 4x4 面板（不是下落式）：index = row*4+col，row0=最上行。
    //   这套引擎按 jubeat/PAD 的写谱手感独立实现（邻居移动 / 双手分区 / 押取聚簇），
    //   与 4K/5K/6K/10K 的分键引擎完全不共用代码。
    struct PadEv
    {
        double  t    = 0.0;   // 音符时刻（song 秒）
        double  dt   = 0.0;   // 与上一音间隔（秒）
        double  turn = 0.0;   // 本音回转角（度）
        int     dir  = 1;     // +1 CW / -1 CCW
        float   hold = 0.f;   // 长按（秒，仅首音）
        uint8_t taps = 1;     // 同刻点击数（>=2 → 押）
    };
    //   style（16K 专属经典手法，与轨道模式互不相干）：
    //     0=经典(邻接移动) 1=绕环(外圈回旋) 2=阶梯(行·列·对角折返)
    //     3=十字(图形谱) 4=交互(双板 trill) 5=冰火拆手序(火左冰右严格交替 + 押按拆手)
    //   outLanes[0..n-1] 主键（0..15）；chordOut[i] 为该音的追加键（押）
    void GeneratePad16K(const PadEv* ev, int n, int style, int level,
                        int* outLanes, std::vector<std::vector<int>>* chordOut);
    const char* ModeDesc16K();

    // ---------------- CATCH（无轨道下落雨）引擎（ChartCatch.cpp，独立算法）----------------
    //   CATCH 不是键位下落式：满屏"雨"沿无轨横向坐标下落，底部"接盘"按对应位置的键滑过去接。
    //   算法把冰火砖的转向/幅度映射成连续横向坐标 x∈[0,1]（长阶梯 / 回摆 / 聚簇），
    //   同时给出一个吸附区间 zone（0..7）供键位 / 自动 / 宏使用。与 4K/5K/6K/10K/16K 完全不共用。
    //   style（CATCH 专用手法，取自 osu!catch 移动语汇）：
    //     0=经典 1=走(Walk) 2=冲(Dash) 3=超冲(Hyper) 4=边冲(Edge) 5=阶梯(Stair)
    void GenerateCatch(const PadEv* ev, int n, int style, int level,
                       int* outZone, float* outX, int* outHoldIdx);
    const char* ModeDescCatch();
    void DrawCatchOverlay();          // 渲染线程：CATCH 满屏覆盖层（雨 + 接盘 + HUD）
    void DrawCatchSettingsPage();     // 渲染线程：主窗口里的 CATCH 设置页

    // ---- CATCH v2：一键自动接（不用按键）+ 鼠标接盘 ----
    //   旧玩法：按任意键把接盘吸到"下一滴雨"，键位分区与雨的实际落点无关。
    //   新玩法：接盘横坐标 = 鼠标 X；雨点落到判定线的瞬间，若接盘覆盖它的落点
    //   就自动接住（无需任何按键，"一键自动打谱子"）；没覆盖就判 MISS。
    //   落点越靠接盘中心判定越高（正中 MARV → 边缘 GOOD）。
    //   MISS 不做任何"自动返回 / 重开"：跟 PERFECT / GOOD 一样照常显示，
    //   打完就继续（是否致死完全交给游戏原关自身判定 / 玩家的不死开关）。
    //   返回本帧新判为 MISS 的音符数。
    int    CatchTick(double clock, float plateX, float plateHalfW);
    void   CatchSetPlateX(float x);   // 写入接盘横坐标 [0,1]
    float  CatchPlateX();             // 接盘横坐标 [0,1]
    float  CatchFxX();                // 最近一次判定的横坐标（打击特效/反馈落点）
    int    CatchMissCount();          // 本关累计漏音数
    void   CatchReset();              // 清空漏音计数 / 快慢方向（换谱 / 重开）
    int    CatchPlateWMil();          // 接盘宽度（千分比，60..400）
    void   CatchPlateWSet(int mil);
    //   CATCH 渲染取数（线程安全拷贝；返回写入条数）
    struct CatchRender { double t; float x; float hold; uint8_t state; };  // state 0 live 1 hit 2 miss
    int  CatchNotes(CatchRender* out, int cap, double minT = -1e18);  // 只拷贝 t>=minT 的音（长谱省拷贝）

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
    const char* SkinDefaultRoot();               // <DLL目录>\skin（外部 MSP 皮肤存放目录）
    // ---- 内置皮肤目录解析（无硬编码盘符 / 用户名 / 绝对路径）----
    const char* ModuleDirPath();                 // 本 DLL 所在目录
    // 解析 DLL 近邻资源目录：<DLL>\<leaf> → 上溯 ..\ → ..\..\ → ..\..\.. → <当前工作目录>\<leaf>
    //   probeFile 非空时要求目录内存在该文件（大小写不敏感）。true 时 out = 绝对路径。
    bool ResolveSidecarDir(const char* leaf, const char* probeFile, char* out, int n);
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
    // ---- 每模式皮肤（PAD / CATCH / 4K / 5K / 6K / 10K 互不通用）----
    //   · 未设置过覆盖的模式沿用全局皮肤；设置后用该模式自己的皮肤
    bool        SkinModeOverrideSet(int mi);
    const char* SkinActiveForMode(int mi);
    bool        SkinBuiltinModeForMode(int mi);
    bool        SkinSetActiveFullForMode(int mi, const char* dir);
    void        SkinSetBuiltinModeForMode(int mi, bool on);
    // 选文件框请用 ShellAsync::PickFile + TakeFile（渲染线程不能进模态循环），
    // 取到路径后再调 SkinImportMsp。

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

    //   全局设置（配置档案整包保存：辅助读谱 / 录制 / 小窗 / 特效 / 宏）
    //   ReadSetting* which: 0=on 1=ahead 2=layout 3=anchor 4=posX 5=posY 6=scale
    //                       7=density 8=opacity 9=offsetMS 10=grid 11=dial 12=rhythm
    //                       13=marks 14=hint 15=angle 16=windows 17=adapt
    int         GlobalSettingCount();
    const char* GlobalSettingKey(int id);
    int         GlobalSettingGet(int id);
    void        GlobalSettingSet(int id, int v);
    int         ReadSettingGet(int which);
    void        ReadSettingSet(int which, int v);

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

    // 最近一次判定的现场（供 CATCH 等独立渲染层画判定提示）
    int    JudgeLastKind();     // -1 无 / 0 MARV / 1 PERF / 2 GOOD / 3 MISS
    double JudgeLastTime();     // 该次判定的歌曲时钟（秒）
    double JudgeLastOffMs();    // 偏差（毫秒，正=按早）
    int    JudgeLastLane();
}
