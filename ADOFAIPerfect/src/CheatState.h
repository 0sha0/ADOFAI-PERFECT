#pragma once
// ============================================================
// CheatState.h — 全局开关与状态快照
// NoDeath / AutoCombo 由渲染线程写入、其他线程读取，用 atomic。
// ============================================================
#include <atomic>

namespace CheatState
{
    // ---- 用户开关 ----
    extern std::atomic<bool> NoDeath;        // 不死模式
    extern std::atomic<bool> AutoCombo;      // 自动连击（游戏内置 Auto）
    extern std::atomic<bool> MenuVisible;    // ImGui 菜单显隐
    extern std::atomic<bool> ExitRequested;  // 请求卸载模块
    // 游戏开始退出（收到 WM_CLOSE/WM_DESTROY/WM_ENDSESSION 或窗口已销毁）：
    // 之后一律不再调用 Mono / 不再绘制 / 不再读游戏内存，避免与 Unity 关停流程死锁。
    extern std::atomic<bool> GameQuitting;
    // 最近一次 Present 的 tick（GetTickCount）；0 = 还没出过帧。
    // 主线程据此判断游戏是否仍在正常出帧（退出/设备丢失后不再调用 Mono）。
    extern std::atomic<unsigned long> LastPresentMs;

    // ---- 状态快照（GameBridge 内部维护，经 GetStatusSnapshot 线程安全读取）----
    struct Status
    {
        bool  bridgeReady = false;        // mono 桥接是否就绪
        bool  controllerAlive = false;    // scrController.instance 存在
        bool  gameworld = false;          // 正在游戏关卡内
        bool  paused = false;             // 游戏内暂停（scrController._paused）
        bool  noFailApplied = false;      // 不死模式已生效
        bool  autoApplied = false;        // 自动连击已生效
        int   state = -1;                 // scrController.States 枚举值
        int   floorIndex = 0;             // 当前地块序号（1 起）
        float percentComplete = 0.f;      // 完成度 0-1
        float percentAcc = 0.f;           // 精准度（Acc）
        float percentXAcc = 0.f;          // 严格精准度（XAcc）
        int   deaths = 0;                 // 本关累计死亡/重开次数
        int   checkpoints = 0;            // 使用的检查点次数
        char  levelName[160] = { 0 };     // 关卡名
        char  stateName[32] = { 0 };      // 状态名

        // ---- 命中统计（scrMarginTracker，HitMargin 枚举 12 项）----
        //   0 TooEarly 1 VeryEarly 2 EarlyPerfect 3 Perfect 4 LatePerfect
        //   5 VeryLate 6 TooLate 7 Multipress 8 FailMiss 9 FailOverload
        //  10 Auto 11 OverPress
        int   hitCounts[12] = { 0 };      // 各类判定次数
        int   hitTotal = 0;               // 已判定总数（hitMargins.Count）
        int   combo = 0;                  // 当前连击（尾部连续有效判定）
        int   maxCombo = 0;               // 历史最大连击

        // ---- 判定窗口参数（逆向自 scrMisc.GetAdjustedAngleBoundaryInDeg）----
        //   冰与火判定按“星球与目标角的夹角”，窗口在度数上为
        //   Pure 30° / Perfect 45° / Counted 60°（低 BPM 时按 时间→角度 放大）：
        //     windowDeg = max(baseDeg * marginScale, t_sec * pitch * 3 * bpm * speed)
        int    difficulty = 1;            // GCS.difficulty（0 宽松 / 1 普通 / 2 严格）
        float  speedTrial = 1.f;          // GCS.currentSpeedTrial（难度试炼倍率）
        double marginScale = 1.0;         // currfloor.nextfloor.marginScale（默认 1）
    };
}
