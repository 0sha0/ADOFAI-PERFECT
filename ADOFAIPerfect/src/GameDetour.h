#pragma once
// ============================================================
// GameDetour.h — 用 Detours 挂钩游戏 JIT 编译后的原生方法
//
// scrPlayer.DieByHitbox(string) 是游戏中唯一 hitbox 死亡入口
// （内部调用 Die(hitbox:true)，而 hitbox 死亡不走 noFail 存活分支）。
// 不死模式开启时直接吞掉这次死亡：星球不爆炸、音乐不停、继续游玩。
//
// 另含 Application.Quit 退出钩子（逆向证据：Assembly-CSharp.dll 中
// TryApplicationQuit / DoQuitAction / QuitButton / forceQuit 均汇入
// UnityEngine.Application.Quit）。在 Unity 拆卸之前先置 GameQuitting
// 并收尾录制 —— 消除"点退出游戏崩溃/卡死"与录制文件无 moov 尾部。
//
// 注意：本项目 Unity 6 协作式 Mono 上 mono_compile_method 只能由托管
// 主线程调用（非托管线程调用会崩溃），故所有 Attach/Detach 都必须在
// 游戏主线程执行。
// ============================================================
namespace GameDetour
{
    bool Attach();   // 需在 GameBridge::Init 之后调用
    void Detach();

    // ---- Application.Quit 退出钩子（仅托管主线程调用） ----
    void TickQuitHookOnMainThread();   // 安装 / 按请求卸载；幂等，可反复调用
    bool QuitHookSettled();            // 任意线程：已装好或确认不可用
    bool QuitHookDetachRequested();    // 任意线程：是否已被请求摘钩
    void RequestQuitHookDetach();      // 任意线程：请求摘钩（卸载流程）
    bool QuitHookDetached();           // 任意线程：钩子已摘除 / 从未安装

    // ---- 冰与火宏（原生关卡）输入钩子（仅托管主线程调用） ----
    //   逆向链：scrController.PlayerControl_Update（States.PlayerControl 每帧）
    //     → scrPlayer.Simulated_PlayerControl_Update(targetTick)
    //     → HitAutoFloors/UpdateHoldKeys → scrPlayer.Hit(bool)
    //     → scrPlanet.SwitchChosen → scrMisc.GetHitMargin(cachedAngle, targetExitAngle, ...)
    //   代打 = 在宏选定的时机调用 Hit(false)：判定仍走游戏本体角度判定，成绩真实。
    void TickInputHookOnMainThread();  // 安装 / 按请求卸载；幂等，可反复调用
    bool InputHookSettled();           // 任意线程：已装好或确认不可用
    bool InputHookDetachRequested();   // 任意线程：是否已被请求摘钩
    void RequestInputHookDetach();     // 任意线程：请求摘钩（卸载流程）
    bool InputHookDetached();          // 任意线程：钩子已摘除 / 从未安装
}
