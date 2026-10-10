#include "GameDetour.h"
#include "GameBridge.h"
#include "Chart4K.h"
#include "MonoApi.h"
#include "CheatState.h"
#include "Log.h"
#include "GameRecorder.h"

#include <windows.h>
#include <detours/detours.h>
#include <chrono>

// Mono JIT 后的原生调用约定 = MS x64：rcx=this, rdx=string
typedef void(__fastcall* DieByHitbox_t)(void* self, void* failMessage);

namespace GameDetour
{
    using namespace MonoApi;

    static DieByHitbox_t g_origDieByHitbox = nullptr;
    static void* g_target = nullptr;
    static bool g_attached = false;

    static void HK_DieByHitbox(void* self, void* failMessage)
    {
        if (CheatState::NoDeath.load(std::memory_order_relaxed))
            return; // 不死模式：吞掉 hitbox 死亡
        g_origDieByHitbox(self, failMessage);
    }

    bool Attach()
    {
        if (g_attached)
            return true;
        if (!MonoApi::Ready())
            return false;

        MonoImage* img = mono_image_loaded("Assembly-CSharp");
        MonoClass* clsPlayer = img ? mono_class_from_name(img, "", "scrPlayer") : nullptr;
        MonoMethod* m = clsPlayer ? mono_class_get_method_from_name(clsPlayer, "DieByHitbox", 1) : nullptr;
        if (!m)
        {
            Log::Printf("[Detour] scrPlayer.DieByHitbox not found (img=%p cls=%p)",
                        (void*)img, (void*)clsPlayer);
            return false;
        }

        // 强制 JIT，取得稳定原生入口
        g_target = mono_compile_method(m);
        if (!g_target)
        {
            Log::Printf("[Detour] mono_compile_method failed");
            return false;
        }
        g_origDieByHitbox = (DieByHitbox_t)g_target;
        Log::Printf("[Detour] scrPlayer.DieByHitbox native = %p", g_target);

        LONG rv = DetourTransactionBegin();
        if (rv != NO_ERROR)
        {
            Log::Printf("[Detour] TransactionBegin failed %ld", rv);
            return false;
        }
        DetourUpdateThread(GetCurrentThread());
        rv = DetourAttach((PVOID*)&g_origDieByHitbox, (PVOID)HK_DieByHitbox);
        if (rv != NO_ERROR)
        {
            DetourTransactionAbort();
            Log::Printf("[Detour] DetourAttach failed %ld", rv);
            return false;
        }
        rv = DetourTransactionCommit();
        if (rv != NO_ERROR)
        {
            Log::Printf("[Detour] Commit failed %ld", rv);
            g_origDieByHitbox = (DieByHitbox_t)g_target;
            return false;
        }

        g_attached = true;
        Log::Printf("[Detour] DieByHitbox hooked (no-death guard active)");
        return true;
    }

    void Detach()
    {
        if (!g_attached)
            return;
        LONG rv = DetourTransactionBegin();
        if (rv == NO_ERROR)
        {
            DetourUpdateThread(GetCurrentThread());
            DetourDetach((PVOID*)&g_origDieByHitbox, (PVOID)HK_DieByHitbox);
            DetourTransactionCommit();
        }
        g_attached = false;
        g_origDieByHitbox = (DieByHitbox_t)g_target;
        Log::Printf("[Detour] DieByHitbox unhooked");
    }

    // ============================================================
    // Application.Quit 退出钩子
    //
    // 逆向证据（Assembly-CSharp.dll 字符串表）：
    //   TryApplicationQuit / DoQuitAction / QuitButton / SaveAndQuit / forceQuit
    //   —— 游戏内"退出游戏"按钮最终调用 UnityEngine.Application.Quit。
    // 钩子命中时（Unity 拆卸开始之前）：
    //   1) GameQuitting=true —— 桥接线程/渲染线程立即停手，不再在
    //      Mono 运行时拆卸过程中调用 mono API（此前会 AV/卡死）；
    //   2) GameRecorder::Stop() —— 有界等待(≤3s)+detach，写出 MP4 moov 尾部；
    //      否则 Unity 直接结束进程，编码线程来不及 Finalize，文件不可播。
    // ============================================================
    typedef void(__fastcall* Quit0_t)();
    typedef void(__fastcall* Quit1_t)(int32_t);

    static Quit0_t g_origQuit0 = nullptr;
    static Quit1_t g_origQuit1 = nullptr;
    static void*   g_quitTarget0 = nullptr;
    static void*   g_quitTarget1 = nullptr;
    static bool    g_quitAttached = false;

    static std::atomic<bool> g_quitSettled{ false };
    static std::atomic<bool> g_quitDetachReq{ false };
    static std::atomic<bool> g_quitDetachedFlag{ true };

    static void OnGameQuit()
    {
        if (!CheatState::GameQuitting.exchange(true, std::memory_order_relaxed))
        {
            Log::Printf("[Detour] Application.Quit intercepted - tool dormant, finalizing recording");
            GameRecorder::Stop();
        }
    }

    static void HK_Quit0()
    {
        OnGameQuit();
        if (g_origQuit0) g_origQuit0();
    }

    static void HK_Quit1(int32_t exitCode)
    {
        OnGameQuit();
        if (g_origQuit1) g_origQuit1(exitCode);
    }

    static void AttachQuit_Hooked()
    {
        if (!MonoApi::Ready())
            return;                                  // 元数据未就绪：下次 Tick 再试

        MonoImage* img = mono_image_loaded("UnityEngine.CoreModule");
        MonoClass* cls = img ? mono_class_from_name(img, "UnityEngine", "Application") : nullptr;
        MonoMethod* m0 = cls ? mono_class_get_method_from_name(cls, "Quit", 0) : nullptr;
        MonoMethod* m1 = cls ? mono_class_get_method_from_name(cls, "Quit", 1) : nullptr;
        if (!cls || (!m0 && !m1))
        {
            Log::Printf("[Detour] Application.Quit not found (img=%p cls=%p m0=%p m1=%p) - quit hook off",
                        (void*)img, (void*)cls, (void*)m0, (void*)m1);
            g_quitSettled.store(true, std::memory_order_relaxed);   // 放弃，避免刷日志
            return;
        }

        // 强制 JIT 取原生入口（本函数只在托管主线程执行 —— 关键前提）
        g_quitTarget0 = m0 ? mono_compile_method(m0) : nullptr;
        g_quitTarget1 = m1 ? mono_compile_method(m1) : nullptr;
        if (!g_quitTarget0 && !g_quitTarget1)
        {
            Log::Printf("[Detour] mono_compile_method(Application.Quit) failed");
            g_quitSettled.store(true, std::memory_order_relaxed);
            return;
        }

        LONG rv = DetourTransactionBegin();
        if (rv != NO_ERROR)
        {
            Log::Printf("[Detour] quit-hook TransactionBegin failed %ld", rv);
            return;                                  // 下次再试
        }
        DetourUpdateThread(GetCurrentThread());
        LONG a0 = NO_ERROR, a1 = NO_ERROR;
        if (g_quitTarget0)
        {
            g_origQuit0 = (Quit0_t)g_quitTarget0;
            a0 = DetourAttach((PVOID*)&g_origQuit0, (PVOID)HK_Quit0);
        }
        if (g_quitTarget1)
        {
            g_origQuit1 = (Quit1_t)g_quitTarget1;
            a1 = DetourAttach((PVOID*)&g_origQuit1, (PVOID)HK_Quit1);
        }
        rv = DetourTransactionCommit();
        if (rv != NO_ERROR || (a0 != NO_ERROR && a1 != NO_ERROR))
        {
            g_origQuit0 = (Quit0_t)g_quitTarget0;
            g_origQuit1 = (Quit1_t)g_quitTarget1;
            Log::Printf("[Detour] quit-hook attach failed rv=%ld a0=%ld a1=%ld", rv, a0, a1);
            return;                                  // 下次再试
        }

        g_quitAttached = true;
        g_quitDetachedFlag.store(false, std::memory_order_relaxed);
        g_quitSettled.store(true, std::memory_order_relaxed);
        Log::Printf("[Detour] Application.Quit hooked (Quit()=%p Quit(int)=%p)",
                    g_quitTarget0, g_quitTarget1);
    }

    static void DetachQuit_Hooked()
    {
        if (g_quitAttached)
        {
            LONG rv = DetourTransactionBegin();
            if (rv == NO_ERROR)
            {
                DetourUpdateThread(GetCurrentThread());
                if (g_origQuit0) DetourDetach((PVOID*)&g_origQuit0, (PVOID)HK_Quit0);
                if (g_origQuit1) DetourDetach((PVOID*)&g_origQuit1, (PVOID)HK_Quit1);
                DetourTransactionCommit();
            }
            g_origQuit0 = (Quit0_t)g_quitTarget0;
            g_origQuit1 = (Quit1_t)g_quitTarget1;
            g_quitAttached = false;
            Log::Printf("[Detour] Application.Quit unhooked");
        }
        g_quitDetachedFlag.store(true, std::memory_order_relaxed);
        g_quitSettled.store(true, std::memory_order_relaxed);
    }

    void TickQuitHookOnMainThread()
    {
        if (g_quitDetachReq.load(std::memory_order_relaxed))
        {
            DetachQuit_Hooked();
            return;
        }
        if (!g_quitSettled.load(std::memory_order_relaxed))
            AttachQuit_Hooked();
    }

    bool QuitHookSettled()         { return g_quitSettled.load(std::memory_order_relaxed); }
    bool QuitHookDetachRequested() { return g_quitDetachReq.load(std::memory_order_relaxed); }
    void RequestQuitHookDetach()   { g_quitDetachReq.store(true, std::memory_order_relaxed); }
    bool QuitHookDetached()        { return g_quitDetachedFlag.load(std::memory_order_relaxed); }

    // ============================================================
    // 冰与火宏（原生关卡）：判定现场代打
    //
    // 逆向证据（Assembly-CSharp 反编译，scrController.cs / scrPlayer.cs / scrPlanet.cs）：
    //   scrController.Update → (StateEngine) PlayerControl_Update
    //     if (!AsyncInputManager.isActive) foreach (scrPlayer p) p.Simulated_PlayerControl_Update()
    //   scrPlayer.Simulated_PlayerControl_Update(ulong? targetTick)
    //     → HitAutoFloors → ValidInputWasTriggered/CountValidKeysPressed → keyTimes
    //     → UpdateHoldKeys → scrPlayer.Hit(bool isAuto)
    //   scrPlayer.Hit() → chosenPlanet.cachedAngle = angle
    //     → scrPlanet.SwitchChosen() → scrMisc.GetHitMargin(cachedAngle, targetExitAngle,
    //        isCW, bpm*speed, pitch, marginScale) → player.marginTracker.AddHit(...)
    //   scrPlanet.Update_RefreshAngles(): angle = snappedLastAngle
    //     + (songposition_minusi - player.lastHit) / crotchet * π * speed * (±1)
    //   → 1ms 误差 = 180*speed/(crotchet*1000) 度角误差，与 GetAdjustedAngleBoundaryInDeg 同源。
    //
    // 因此：本宏 = 在"拟人毫秒偏移"对应的角度上直接调用 Hit(false)，
    //   判定/连击/准确率全部由游戏本体计算（真实成绩，非 100%，也不注入系统输入）。
    //   长条（holdLength > -1）由"长条结束时刻再按一次"完成（与游戏自带 auto 的
    //   OttoHoldHit 完全一致：判定期望跨过 targetExitAngle 时再次 Hit）。
    // ============================================================
    typedef bool(__fastcall* FireHit_t)(void* self, int32_t isAuto);
    typedef void(__fastcall* SimUpdate_t)(void* self, void* a, void* b);
    typedef void(__fastcall* CtrlUpdate_t)(void* self);

    static FireHit_t   g_fireHit = nullptr;
    static SimUpdate_t g_origSimUpdate = nullptr;
    static void*       g_simUpdateTarget = nullptr;
    static CtrlUpdate_t g_origCtrlUpdate = nullptr;
    static void*       g_ctrlUpdateTarget = nullptr;
    static bool        g_inputAttached = false;
    static std::atomic<bool> g_inputSettled{ false };
    static std::atomic<bool> g_inputDetachReq{ false };
    static std::atomic<bool> g_inputDetachedFlag{ true };
    static std::atomic<int>  g_inputTries{ 0 };   // 目标解析/挂钩失败次数（有界重试用）

    struct FireRt
    {
        int    floorSeq = -1;
        int    tries = 0;
        int    logged = 0;
        double targetDeg = 0.0;
        double lastCallMs = 0.0;
        int    lastEnabled = 0;
    };
    static FireRt g_fireRt;

    static double FireNowMs()
    {
        using clock = std::chrono::steady_clock;
        static const clock::time_point t0 = clock::now();
        return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    }

    // 每帧（游戏主线程、scrPlayer 判定入口之前）跑一次：读判定现场 → 决定是否代打
    static void FireMacroFrame(void* self)
    {
        GameBridge::FireCtx c;
        if (!GameBridge::ReadFireCtx(&c) || !c.ok || c.player != self)
            return;
        if (!c.gameworld || c.paused || !c.inControl || !c.hasNext ||
            c.floorIndex < 0 || c.crotchet <= 0.0)
        {
            g_fireRt.floorSeq = -1;   // 不在打歌中：复位（下次进入重新抽偏移）
            return;
        }

        double nowMs = FireNowMs();
        double dt = (g_fireRt.lastCallMs > 0.0) ? (nowMs - g_fireRt.lastCallMs) : 16.0;
        g_fireRt.lastCallMs = nowMs;
        if (dt < 0.5) dt = 0.5;
        if (dt > 100.0) dt = 100.0;

        const double degPerMs = 180.0 * c.speed / (c.crotchet * 1000.0);
        if (c.floorIndex != g_fireRt.floorSeq)
        {
            g_fireRt.floorSeq = c.floorIndex;
            g_fireRt.tries = 0;
            g_fireRt.targetDeg = Chart4K::MacroTimingOffsetMs(90.0) * degPerMs;
        }
        if (g_fireRt.tries >= 4 || !g_fireHit)
            return;

        // 触发阈值：目标角误差 - 半帧量化补偿（命中落在 [本帧, 下帧) 之间，平均晚 dt/2）
        const double thr = g_fireRt.targetDeg - 0.5 * dt * degPerMs;
        if (c.errDeg >= thr)
        {
            g_fireRt.tries++;
            const bool okHit = g_fireHit(self, 0);
            if (g_fireRt.logged < 16)
            {
                g_fireRt.logged++;
                Log::Printf("[fire] hit floor=%d err=%.2fdeg target=%.2fdeg ok=%d try=%d",
                            c.floorIndex, c.errDeg, g_fireRt.targetDeg, okHit ? 1 : 0,
                            g_fireRt.tries);
            }
        }
    }

    // ============================================================
    // CATCH 专用背景打歌引擎（独立算法：与"冰与火宏"不共用状态 / 参数 / 随机源）
    //
    //   CATCH 覆盖层每判一个音就把"归一化目标偏差"按歌曲时刻发布出来
    //   （Chart4K::CatchPlayTargetFrac）。这里按同一个原生入口打背景那首原关：
    //     · 归一化单位 = 背景"计数窗(Counted)"半宽（60° × marginScale）；
    //       MARV 0.20 / PERFECT 0.42 / GOOD 0.88 / MISS 1.60（符号 = 慢/快）；
    //       → 背景分别吃到 Perfect / Perfect / VeryEarly·VeryLate / TooEarly·TooLate，
    //         即 CATCH 判什么，冰与火本体就判什么（含"快/慢"方向）。
    //   自带拟人抖动（每砖抽一次，±5ms），不读宏模式的精准度 / 拟人度设置。
    // ============================================================
    struct CatchPlayRt
    {
        int      floorSeq   = -1;
        int      tries      = 0;
        int      logged     = 0;
        double   targetDeg  = 0.0;
        double   jitMs      = 0.0;     // 本砖的拟人抖动（每砖抽一次）
        double   lastCallMs = 0.0;
        unsigned jit        = 0x9E3779B9u;
    };
    static CatchPlayRt g_catchRt;

    static void CatchPlayFrame(void* self)
    {
        GameBridge::FireCtx c;
        if (!GameBridge::ReadFireCtx(&c) || !c.ok || c.player != self)
            return;
        if (!c.gameworld || c.paused || !c.inControl || !c.hasNext ||
            c.floorIndex < 0 || c.crotchet <= 0.0)
        {
            g_catchRt.floorSeq = -1;      // 不在打歌中：复位
            return;
        }

        double nowMs = FireNowMs();
        double dt = (g_catchRt.lastCallMs > 0.0) ? (nowMs - g_catchRt.lastCallMs) : 16.0;
        g_catchRt.lastCallMs = nowMs;
        if (dt < 0.5)   dt = 0.5;
        if (dt > 100.0) dt = 100.0;
        const double degPerMs = 180.0 * c.speed / (c.crotchet * 1000.0);

        if (c.floorIndex != g_catchRt.floorSeq)
        {
            g_catchRt.floorSeq = c.floorIndex;
            g_catchRt.tries = 0;
            g_catchRt.jit = g_catchRt.jit * 1664525u + 1013904223u;   // 每砖一次拟人抖动
            // 固定 ±5ms → 随宏页「抖动幅度」缩放（0% = 绝对零抖动，高 BPM 不出窗）
            g_catchRt.jitMs = ((double)(g_catchRt.jit >> 8) / 16777216.0 * 2.0 - 1.0) * 5.0
                              * ((double)Chart4K::MacroJitterGet() / 100.0);
        }

        // CATCH 的判定偏差（按当前歌曲时刻取最近一条）；没拿到就按 0（当成完美接住）
        double tgtFrac = 0.0;
        const bool hasTgt = Chart4K::CatchPlayTargetFrac(Chart4K::ApiSongTime(), &tgtFrac);
        const double scale = (double)Chart4K::CatchPlayAccGet() / 100.0;   // 打歌精准度
        // 背景原关的"计数窗"半宽（度）：60° × marginScale（marginScale 取自原生砖块）
        const double marginScale = (c.marginScale > 0.01 && c.marginScale < 100.0) ? c.marginScale : 1.0;
        const double cntDeg = 60.0 * marginScale;
        g_catchRt.targetDeg = (hasTgt ? tgtFrac * scale * cntDeg : 0.0) + g_catchRt.jitMs * degPerMs;

        if (g_catchRt.tries >= 4 || !g_fireHit)
            return;

        const double thr = g_catchRt.targetDeg - 0.5 * dt * degPerMs;
        if (c.errDeg >= thr)
        {
            g_catchRt.tries++;
            const bool okHit = g_fireHit(self, 0);
            if (g_catchRt.logged < 24)
            {
                g_catchRt.logged++;
                Log::Printf("[catch-hit] floor=%d err=%.2fdeg target=%+.3f*%0.0fdeg(%.1fdeg) got=%d hit=%d",
                            c.floorIndex, c.errDeg, (hasTgt ? tgtFrac * scale : 0.0), cntDeg,
                            g_catchRt.targetDeg, (int)hasTgt, okHit ? 1 : 0);
            }
        }
    }

    static void HK_SimPlayerControlUpdate(void* self, void* a, void* b)
    {
        if (!CheatState::GameQuitting.load(std::memory_order_relaxed))
            GameBridge::TickCheatsFast();
        if (!CheatState::GameQuitting.load(std::memory_order_relaxed))
        {
            if      (Chart4K::FireMacroEnabled())        FireMacroFrame(self);   // 冰与火宏（用户自己的）
            else if (Chart4K::CatchAssistEnabled())      CatchPlayFrame(self);   // CATCH 专用打歌引擎
        }
        if (g_origSimUpdate)
            g_origSimUpdate(self, a, b);
    }

    // scrController.Update：任何状态下每帧都会跑。
    // 宏模式用它跳过 "Press to start" 的真实输入等待（写 levelWasSkipped），
    // 否则纯宏（人不按键）永远停在 States.Start，代打无从触发。
    static void HK_ControllerUpdate(void* self)
    {
        // 每帧驱动"开关重应用"：关卡 Awake 会重置 noFail / RDC.auto，
        // 这里保证在 ~100ms 内把它们改回用户选择（修复"不死/自动连打有时失效"）。
        if (!CheatState::GameQuitting.load(std::memory_order_relaxed))
            GameBridge::TickCheatsFast();
        if (!CheatState::GameQuitting.load(std::memory_order_relaxed) &&
            (Chart4K::FireMacroEnabled() || Chart4K::CatchAssistEnabled()))
        {
            GameBridge::FireCtx c;
            if (GameBridge::ReadFireCtx(&c) && c.ok && c.gameworld && !c.paused && c.state == 1)
                GameBridge::SetLevelWasSkipped(true);
        }
        if (g_origCtrlUpdate)
            g_origCtrlUpdate(self);
    }

    // 名称匹配、不限参数个数：不同游戏版本 / Mono 后端上同一方法的签名可能是
    // ()、 (ulong?)、 (bool,bool) 等，逐个 arity 试，避免"换个版本就找不到目标"。
    static MonoMethod* FindMethodAnyArity(MonoClass* cls, const char* name)
    {
        if (!cls || !name)
            return nullptr;
        for (int n = 0; n <= 4; n++)
        {
            MonoMethod* m = mono_class_get_method_from_name(cls, name, n);
            if (m)
                return m;
        }
        return nullptr;
    }

    static void AttachInput_Hooked()
    {
        if (!MonoApi::Ready())
            return;                                  // 元数据未就绪：下次 Tick 再试

        // 这里曾经是"宏在某些机器上不生效"的主因：旧实现在第一次找不到目标时就
        // 永久放弃（g_inputSettled=true）。但注入时机偏早 / 机器偏快时
        // Assembly-CSharp 可能还没加载完，于是钩子再也装不上、冰与火宏静默失效。
        // 现在改成有界重试：程序集/类还没就绪就一直等；确认程序集在了却仍找不到
        // 方法、或 Detours 反复失败，才判定不可用。
        MonoImage* img = mono_image_loaded("Assembly-CSharp");
        if (!img)
        {
            static DWORD s_lastLog = 0;
            if (GetTickCount() - s_lastLog > 5000)
            {
                s_lastLog = GetTickCount();
                Log::Printf("[Detour] Assembly-CSharp not loaded yet - retrying");
            }
            return;                                  // 不放弃：下次 Tick 再试
        }
        MonoClass* clsPlayer = mono_class_from_name(img, "", "scrPlayer");
        if (!clsPlayer)
            return;                                  // 类还未就绪：继续等

        MonoMethod* mUpdate = FindMethodAnyArity(clsPlayer, "Simulated_PlayerControl_Update");
        if (!mUpdate)
        {
            // 退路：scrPlayer.Update 也是每帧跑在同一个实例上，宏逻辑与它无关，
            // 只需要"每帧、在主线程、判定之前"拿到同一时机即可。
            mUpdate = FindMethodAnyArity(clsPlayer, "Update");
            if (mUpdate)
                Log::Printf("[Detour] Simulated_PlayerControl_Update missing - hooking scrPlayer.Update instead");
        }
        MonoMethod* mHit    = FindMethodAnyArity(clsPlayer, "Hit");
        MonoClass* clsCtrl  = mono_class_from_name(img, "", "scrController");
        MonoMethod* mCtrlUpdate = clsCtrl ? FindMethodAnyArity(clsCtrl, "Update") : nullptr;
        if (!mUpdate || !mHit)
        {
            if (++g_inputTries > 20)
            {
                Log::Printf("[Detour] fire hook targets missing after %d tries (upd=%p hit=%p) - fire macro off",
                            (int)g_inputTries.load(), (void*)mUpdate, (void*)mHit);
                g_inputSettled.store(true, std::memory_order_relaxed);   // 确认不可用
            }
            return;
        }

        g_fireHit = (FireHit_t)mono_compile_method(mHit);
        g_simUpdateTarget = mono_compile_method(mUpdate);
        g_ctrlUpdateTarget = mCtrlUpdate ? mono_compile_method(mCtrlUpdate) : nullptr;
        if (!g_fireHit || !g_simUpdateTarget)
        {
            if (++g_inputTries > 20)
            {
                Log::Printf("[Detour] mono_compile_method(fire) failed (hit=%p upd=%p) - fire macro off",
                            (void*)g_fireHit, g_simUpdateTarget);
                g_inputSettled.store(true, std::memory_order_relaxed);
            }
            return;
        }

        // 事务 1（宏必需）：scrPlayer 判定入口。
        LONG rv = DetourTransactionBegin();
        if (rv != NO_ERROR)
        {
            Log::Printf("[Detour] fire TransactionBegin failed %ld", rv);
            return;                                  // 瞬时失败：下次再试
        }
        DetourUpdateThread(GetCurrentThread());
        g_origSimUpdate = (SimUpdate_t)g_simUpdateTarget;
        rv = DetourAttach((PVOID*)&g_origSimUpdate, (PVOID)HK_SimPlayerControlUpdate);
        if (rv == NO_ERROR)
            rv = DetourTransactionCommit();
        else
            DetourTransactionAbort();
        if (rv != NO_ERROR)
        {
            g_origSimUpdate = (SimUpdate_t)g_simUpdateTarget;
            if (++g_inputTries > 20)
            {
                Log::Printf("[Detour] fire DetourAttach/Commit failed %ld after %d tries - fire macro off",
                            rv, (int)g_inputTries.load());
                g_inputSettled.store(true, std::memory_order_relaxed);
            }
            return;                                  // 否则下次再试
        }

        // 事务 2（可选，失败不影响宏）：scrController.Update → 跳过 Press to start。
        //   与宏分开挂：以前放在同一事务里，ctrl 挂不上会把整个事务回滚，
        //   连 macros 必需的 scrPlayer 钩子一起丢，宏直接失效。
        if (g_ctrlUpdateTarget)
        {
            LONG rv2 = DetourTransactionBegin();
            if (rv2 == NO_ERROR)
            {
                DetourUpdateThread(GetCurrentThread());
                g_origCtrlUpdate = (CtrlUpdate_t)g_ctrlUpdateTarget;
                rv2 = DetourAttach((PVOID*)&g_origCtrlUpdate, (PVOID)HK_ControllerUpdate);
                if (rv2 == NO_ERROR)
                    rv2 = DetourTransactionCommit();
                else
                    DetourTransactionAbort();
            }
            if (rv2 != NO_ERROR)
            {
                g_origCtrlUpdate = nullptr;
                g_ctrlUpdateTarget = nullptr;        // 摘钩时不再尝试 detach
                Log::Printf("[Detour] ctrl Update hook failed %ld (press-to-start skip off, macro kept)", rv2);
            }
        }

        g_inputTries.store(0, std::memory_order_relaxed);
        g_inputAttached = true;
        g_inputDetachedFlag.store(false, std::memory_order_relaxed);
        g_inputSettled.store(true, std::memory_order_relaxed);
        Log::Printf("[Detour] fire macro hooked: scrPlayer entry=%p Hit=%p", g_simUpdateTarget, (void*)g_fireHit);
        if (g_ctrlUpdateTarget)
            Log::Printf("[Detour] ctrl Update hooked: scrController.Update=%p (press-to-start auto-skip)",
                        g_ctrlUpdateTarget);
    }

    static void DetachInput_Hooked()
    {
        if (g_inputAttached)
        {
            LONG rv = DetourTransactionBegin();
            if (rv == NO_ERROR)
            {
                DetourUpdateThread(GetCurrentThread());
                DetourDetach((PVOID*)&g_origSimUpdate, (PVOID)HK_SimPlayerControlUpdate);
                if (g_ctrlUpdateTarget)
                    DetourDetach((PVOID*)&g_origCtrlUpdate, (PVOID)HK_ControllerUpdate);
                DetourTransactionCommit();
            }
            g_origSimUpdate = (SimUpdate_t)g_simUpdateTarget;
            g_origCtrlUpdate = (CtrlUpdate_t)g_ctrlUpdateTarget;
            g_inputAttached = false;
            Log::Printf("[Detour] fire macro unhooked");
        }
        g_inputDetachedFlag.store(true, std::memory_order_relaxed);
        g_inputSettled.store(true, std::memory_order_relaxed);
    }

    void TickInputHookOnMainThread()
    {
        if (g_inputDetachReq.load(std::memory_order_relaxed))
        {
            DetachInput_Hooked();
            return;
        }
        if (!g_inputSettled.load(std::memory_order_relaxed))
            AttachInput_Hooked();
    }

    bool InputHookSettled()        { return g_inputSettled.load(std::memory_order_relaxed); }
    bool InputHookActive()         { return g_inputAttached; }
    bool InputHookDetachRequested() { return g_inputDetachReq.load(std::memory_order_relaxed); }
    void RequestInputHookDetach()  { g_inputDetachReq.store(true, std::memory_order_relaxed); }
    bool InputHookDetached()       { return g_inputDetachedFlag.load(std::memory_order_relaxed); }
}
