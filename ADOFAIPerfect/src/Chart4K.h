#pragma once
// ============================================================
// Chart4K.h — 4K 辅助页
//
// 把冰与火的无轨谱面实时转换为 Malody 式 4K（DFJK）下落谱：
//   · 谱面：scrLevelMaker._instance.listFloors，scrFloor.entryTime =
//     每块绝对命中时刻（BPM/变速已由游戏折算），angleLength+isCCW = 转向角
//   · 同步：scrConductor._instance.dspTimeSong（游戏歌曲时钟，纯字段读取）
//   · 分键（手感，参考 osu!mania 官方 mapping guide）：
//       - 逆时针 → 左手 (D/F)，顺时针 → 右手 (J/K)
//       - 同方向连打在两手两键内交替（roll），换向时按角度大小落键
//       - 直行块（|角|≈0）= 同键连击（jack，对应游戏中重复音）
//       - 重复节奏型（motif）复用同一键位
//   · 皮肤：加载 Malody V 皮肤目录（rurudokey 小人 / notex 音符 /
//     judgercolor 判定线 / combo 数字），DFJK 按下小人同步敲击
//   · 连击数 + 精准度（游戏真实 Acc）+ Malody 式难度 Lv 自动计算
// ============================================================
#include <atomic>

namespace Chart4K
{
    void Tick();              // 桥接线程：歌曲时钟 / 换谱检测 / 谱面提取 / 难度计算
    void DrawPlayfield();     // 渲染线程：绘制下坠谱面（独立透明窗口）
    void DrawSettingsPage();  // 渲染线程：主窗口里的 4K辅助 设置页
    void DrawSettings6KPage();// 渲染线程：主窗口里的 6K模式 设置页
    void DrawSettings5KPage();// 渲染线程：主窗口里的 5K模式 设置页
    void DrawSettings10KPage();// 渲染线程：主窗口里的 10K模式 设置页
    void DrawReadOverlay();    // 渲染线程：辅助读谱（无轨）叠加层（独立于 4K/5K/6K/10K）
    void DrawReadSettingsPage();// 渲染线程：主窗口里的 辅助读谱 设置页
    void DrawKeyViewerOverlay();     // 渲染线程：KeyViewer 游戏内叠加层（打歌时自动显示）
    void DrawKeyViewerSettingsPage();// 渲染线程：主窗口里的 KeyViewer 设置页
    void DrawMacroPage();            // 渲染线程：主窗口里的 宏模式 页（宏打歌 + 自动录制）
    void DrawRecordPage();           // 渲染线程：主窗口里的 录制 页（小窗录制：开始/暂停/继续）
    void MainThreadResolve(); // 仅游戏主线程调用：安全创建所需类的 vtable

    bool HasChart();                       // 是否已成功解析出谱面
    int  NoteCount();                      // 当前谱面音符数
    void GetDiag(char* buf, int n);         // 诊断文本（供设置页显示）

    // ---- 宏 / 录制 配置（设置页档案 + 宏页共用；rec_dir 持久化在 adofai_perfect.cfg）----
    int         MacroAccGet();  void MacroAccSet(int v);     // 目标精准度 90..100
    int         MacroHumanGet(); void MacroHumanSet(int v);  // 拟人程度 0..100
    int         MacroJitterGet(); void MacroJitterSet(int v); // 抖动幅度 0..100（总乘数；0=绝对零抖动，高 BPM 保底档）
    int         RecOnGet();     void RecOnSet(int v);
    int         RecAutoGet();   void RecAutoSet(int v);
    int         RecFpsGet();    void RecFpsSet(int v);
    int         RecMbpsGet();   void RecMbpsSet(int v);
    const char* RecDirGet();    void RecDirSet(const char* dir);
    void        RecCfgApply();
    void        RecStatsGet(int* in, int* written, int* dropped, const char** file);
    const char* RecLastError();                      // 最近一次录制失败原因（无则 ""）
    int         RecPausedGet(); void RecPausedSet(int v);

    // ---- 冰与火宏（原生关卡）：宏直接代打游戏本体判定线 ----
    //   逆向链：scrController.PlayerControl_Update → scrPlayer.Simulated_PlayerControl_Update
    //   → Hit(bool) → scrPlanet.SwitchChosen → scrMisc.GetHitMargin(cachedAngle, targetExitAngle)
    bool        FireMacroEnabled(); void FireMacroSet(int v);
    double      MacroTimingOffsetMs(double winMs);   // 复用宏打歌拟人抖动模型（毫秒）

    // ---- CATCH 玩法：玩家用鼠标操纵接盘完成 CATCH 判定；背景那首原关交给 CATCH 专用
    //      打歌引擎演奏。不再替用户强开不死：失误照原生判定，MISS 会让原关真的失败/重开。----
    bool        CatchAssistEnabled();
    // 任意覆盖层辅助模式（4K/5K/6K/10K/16K/CATCH）启用中：桥接层据此把游戏原关
    // 交给内置 Auto 演奏（玩家要打的是覆盖层，不可能同时按键打原关）。
    bool        AnyAssistEnabled();

    // ---- CATCH 专用背景打歌引擎（独立于"冰与火宏"，算法思路相同但不共用状态）----
    //   CATCH 覆盖层把每次判定换算成"目标偏差（归一化）"并按歌曲时刻发布；打歌引擎
    //   （GameDetour::CatchPlayFrame）按当前时刻取最近一条，换算成角度直接决定背景
    //   原关的判定档（判定/连击/准确率仍由游戏本体计算，是真实成绩）：
    //     归一化单位 = 背景原关"计数窗(Counted)"半宽（60° × marginScale）：
    //       0.20 → 正中 MARV     → 背景 Perfect
    //       0.42 → CATCH PERFECT → 背景 Perfect
    //       0.88 → CATCH GOOD    → 背景 VeryEarly/VeryLate（"GOOD"档，且能看出快/慢）
    //       1.60 → CATCH MISS    → 背景 TooEarly/TooLate（断连击 = MISS）
    //   符号 = 快慢：正 = 慢(SLOW)、负 = 快(FAST)。
    double      ApiSongTime();                       // 当前歌曲时钟（秒）
    bool        CatchPlayTargetFrac(double songTime, double* outFrac);
    int         CatchLastDir();                      // 最近一次判定方向：+1=慢 / -1=快 / 0=无
    int         CatchPlayAccGet();                   // 打歌精准度 0..100（把 CATCH 偏差带到背景的比例）
    void        CatchPlayAccSet(int v);
}
