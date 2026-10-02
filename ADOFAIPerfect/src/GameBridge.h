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
    void MainThreadInitTask();   // 仅在游戏主线程调用！
    void SetMainThreadPoster(void (*fn)()); // RenderHook 注入投递函数
    void* GetControllerInstance(); // 已发现的 scrController 实例（未发现返回 nullptr）

    bool Ready();
}
