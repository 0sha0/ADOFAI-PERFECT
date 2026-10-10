#pragma once
// ============================================================
// GameBridge.h — 通过 Mono API 直接读写游戏内部状态
//
// 游戏机制（由对 Assembly-CSharp.dll 的反编译确认，Unity 6000.3.10f1）：
//   1) 不死模式：
//      scrController 实例字段 `noFail` —— 游戏自身的无失败开关。
//      Awake() 中 noFail = GCS.useNoFail（官方关卡强制关闭），
//      但 Die() 中只要 noFail==true，失误判定走"存活"分支：
//      地球闪红、combo 不断、自动补一拍继续游戏。
//      ⇒ 我们每帧强写 noFail=true，绕过官方关卡的开关限制；
//        关闭时恢复为 GCS.useNoFail 的原值。
//   2) 自动连击：
//      静态单例 RDConstants.data 的实例字段 `auto`（RDC.auto 属性即它）。
//      游戏在每次关卡加载时 Awake 里 RDC.auto = false 强制关闭，
//      自定义关卡靠秘籍码开启 ⇒ 我们每帧强写 true 实现官方关卡自动连击。
//   3) 精准度：
//      scrController.playerOne.marginTracker 的 percentAcc / percentXAcc
//      （AddHit 时实时刷新），死亡次数 deaths、检查点 checkpointsUsed。
//
// 线程模型：
//   所有 Mono 调用在自有 worker 线程上执行（mono_thread_attach 一次），
//   绝不调用 mono_runtime_invoke（纯字段/数组数据访问，Boehm GC 不移动
//   对象，因此跨线程读取安全），并用 SEH 兜底。
// ============================================================
#include "CheatState.h"

namespace GameBridge
{
    bool Init();        // 解析 Assembly-CSharp 中的类/字段（可重试；不安全路径，仅保守创建）
    void StartLoop();   // 启动 Tick/ReadStatus 循环线程
    void GetStatusSnapshot(CheatState::Status* out); // 线程安全取快照（渲染线程调用）
    void Restore();     // 卸载前恢复游戏状态（worker 线程调用）

    // 主线程初始化：由渲染钩子通过窗口消息投递到游戏主线程执行。
    // 主线程是 mono 正式托管的线程，在这里调用 mono_class_vtable 可以
    // 安全地为尚未使用的类创建 vtable（并完成静态字段布局），
    // 让桥接在标题画面就立即就绪，无需等玩家进入关卡。
    void QueueMainThreadInit();
    void QueueCheatApply();      // 请求主线程把 noFail / RDC.auto 应用到当前值
    // CATCH 漏音即死：请求主线程重开本关（scrController.Restart(false)）。
    // 只在"主线程任务"里真正调用，任意线程可安全请求。
    void RequestLevelRestart();
    // 游戏主线程（每帧钩子）调用：节流后重新把 noFail / RDC.auto 应用到当前值。
    // 关卡 Awake 会把 noFail / RDC.auto 重置为默认，靠它把"进关卡 / 重开一关"
    // 之后的重置窗口压到 ~100ms 以内（此前只能等 1s 一次的主线程任务）。
    void TickCheatsFast();
    void MainThreadInitTask();   // 仅在游戏主线程调用！
    void SetMainThreadPoster(void (*fn)()); // RenderHook 注入投递函数
    // 通用主线程任务（在主线程、mono 托管线程上执行；ctx 由调用方负责生命周期）
    void QueueMainThreadWork(void (*fn)(void*), void* ctx);
    void* GetControllerInstance(); // 已发现的 scrController 实例（未发现返回 nullptr）

    // 实时判定角度（7ms 快循环采样，渲染线程无锁读取）：
    //   逆向依据 scrPlanet.SwitchChosen → scrMisc.GetHitMargin(cachedAngle, targetExitAngle,
    //   isCW, bpm*speed, pitch, marginScale)：判定值 = (angle - targetExitAngle) * (isCW?1:-1)，
    //   单位为弧度；换算成度后负=早（星球还没转到目标角），正=晚（已经越过）。
    struct PlanetLive
    {
        bool   ok = false;      // 读到有效星球对象
        double errDeg = 0.0;    // 判定角误差（度，负早正晚）
        double speed = 1.0;     // PlanetarySystem.speed（关卡速度倍率）
    };
    void GetPlanetLive(PlanetLive* out);

    // ---- 冰与火宏模式：每帧判定现场（游戏主线程输入钩子调用） ----
    //   全部为"缓存偏移 + 纯指针读取"，任意线程可调用（内部 SEH 兜底）。
    //   字段来源（Assembly-CSharp 反编译确认）：
    //     scrController.state/currentState → States.PlayerControl(4)
    //     scrPlanet.currfloor → scrFloor.seqID / holdLength / holdCompletion
    //     scrPlanet.<targetExitAngle> / angle → err = (angle-target)*(isCW?1:-1)
    struct FireCtx
    {
        bool   ok = false;
        bool   inControl = false;      // 正在 PlayerControl（可打歌）
        bool   gameworld = false;
        bool   paused = false;
          bool   hasNext = false;        // currfloor.nextfloor != null（最后一砖不再代打）
          void*  player = nullptr;       // scrPlayer 实例（= playerManager.players[0]）
          int    state = -1;             // scrController.currentState（1=Start 2=Countdown 4=PlayerControl）
          int    floorIndex = -1;        // scrFloor.seqID（-1 = 读不到）
        int    holdLength = -1;        // scrFloor.holdLength（> -1 = 长条块）
        double holdCompletion = 0.0;   // scrFloor.holdCompletion 0..1
        double errDeg = 0.0;           // 判定角误差（度，随时间增大）
        double marginScale = 1.0;      // nextfloor.marginScale（判定窗倍率）
        double speed = 1.0;            // PlanetarySystem.speed
        double crotchet = 0.0;         // scrConductor.crotchetAtStart（秒/拍；BPM 折算）
    };
    bool ReadFireCtx(FireCtx* out);
    // 跳过"按任意键开始"等待（写 scrController.levelWasSkipped；仅托管主线程调用）
    bool SetLevelWasSkipped(bool on);

    bool Ready();
}
