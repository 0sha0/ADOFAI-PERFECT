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
    int         RecOnGet();     void RecOnSet(int v);
    int         RecAutoGet();   void RecAutoSet(int v);
    int         RecFpsGet();    void RecFpsSet(int v);
    int         RecMbpsGet();   void RecMbpsSet(int v);
    const char* RecDirGet();    void RecDirSet(const char* dir);
    void        RecCfgApply();
    void        RecStatsGet(int* in, int* written, int* dropped, const char** file);
    int         RecPausedGet(); void RecPausedSet(int v);

    // ---- 冰与火宏（原生关卡）：宏直接代打游戏本体判定线 ----
    //   逆向链：scrController.PlayerControl_Update → scrPlayer.Simulated_PlayerControl_Update
    //   → Hit(bool) → scrPlanet.SwitchChosen → scrMisc.GetHitMargin(cachedAngle, targetExitAngle)
    bool        FireMacroEnabled(); void FireMacroSet(int v);
    double      MacroTimingOffsetMs(double winMs);   // 复用宏打歌拟人抖动模型（毫秒）
}
