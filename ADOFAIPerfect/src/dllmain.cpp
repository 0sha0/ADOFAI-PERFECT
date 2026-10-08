// ============================================================
// ADOFAIPerfect.dll — A Dance of Fire and Ice (Steam, Unity 6000.3.10f1 Mono)
// ImGui 辅助工具：不死模式 / 自动连击（官方关卡 & 自定义关卡）
//
// 注入后流程：
//   1. 等待 mono-2.0-bdwgc.dll 加载 → 解析 Mono 导出
//   2. 解析 Assembly-CSharp 中的 scrController / RDConstants 等
//   3. Detours 挂钩 scrPlayer.DieByHitbox（hitbox 死亡兜底）
//   4. 挂钩 DXGI Present（覆盖层）后进入每帧循环：
//        GameBridge::Tick()     强写 noFail / auto 开关
//        GameBridge::ReadStatus() 节流读取精准度等信息
//        ImGui 菜单渲染
//   5. 卸载：还原游戏状态 → 摘钩 → FreeLibraryAndExitThread
// ============================================================
#include "CheatState.h"
#include "Log.h"
#include "MonoApi.h"
#include "GameBridge.h"
#include "GameDetour.h"
#include "RenderHook.h"

#include <windows.h>
#include <dbghelp.h>
#include <atomic>
#include <cstdio>

#pragma comment(lib, "dbghelp.lib")

// ---------------- 崩溃转储（分析用） ----------------
static LONG WINAPI CrashDumpFilter(EXCEPTION_POINTERS* ep)
{
    static LONG s_count = 0;
    if (InterlockedCompareExchange(&s_count, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    char path[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH))
    {
        strncat_s(path, "\\ADOFvec\\game_crash.dmp", _TRUNCATE);
        HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION mei = { GetCurrentThreadId(), ep, FALSE };
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                              MiniDumpNormal, &mei, nullptr, nullptr);
            CloseHandle(f);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

std::atomic<bool> CheatState::NoDeath{ false };
std::atomic<bool> CheatState::AutoCombo{ false };
std::atomic<bool> CheatState::MenuVisible{ true };
std::atomic<bool> CheatState::ExitRequested{ false };
std::atomic<bool> CheatState::GameQuitting{ false };
std::atomic<unsigned long> CheatState::LastPresentMs{ 0 };

static HMODULE g_hSelf = nullptr;

// ---------------- 卸载流程（worker 线程） ----------------
static void CleanupAndUnload()
{
    Log::Printf("[Main] exit requested, cleaning up...");

    // 1) 让渲染钩子停止工作（它检查 ExitRequested）
    CheatState::ExitRequested.store(true, std::memory_order_relaxed);
    Sleep(250);

    // 2) 还原游戏状态（恢复 noFail / RDC.auto 原值）
    GameBridge::Restore();

    // 2.5) 摘除 Application.Quit 钩子：trampoline 位于本模块内，FreeLibrary
    //      之前必须还原，否则游戏下一次退出会跳进已卸载的内存。
    //      摘钩只能在托管主线程执行（mono 约束），故投递主线程任务并等待；
    //      MainThreadInitTask 里有专门的高速通道处理该请求。
    GameDetour::RequestQuitHookDetach();
    for (int i = 0; i < 40 && !GameDetour::QuitHookDetached(); i++)
    {
        GameBridge::QueueMainThreadInit();
        Sleep(50);
    }
    if (!GameDetour::QuitHookDetached())
        Log::Printf("[Main] quit-hook detach timed out - game thread not pumping");

    // 2.6) 摘除冰与火宏的 scrPlayer 判定钩子（trampoline 同样位于本模块内）
    GameDetour::RequestInputHookDetach();
    for (int i = 0; i < 40 && !GameDetour::InputHookDetached(); i++)
    {
        GameBridge::QueueMainThreadInit();
        Sleep(50);
    }
    if (!GameDetour::InputHookDetached())
        Log::Printf("[Main] input-hook detach timed out - game thread not pumping");

    // 3) 渲染侧完整清理：恢复 WndProc、ImGui 关闭、摘 DXGI 钩
    RenderHook::Shutdown();

    // 4) 移除崩溃转储过滤器（它指向本模块代码）
    SetUnhandledExceptionFilter(nullptr);

    // 5) 等待所有线程离开本模块代码
    Sleep(400);

    // 6) 自卸载
    Log::Printf("[Main] unloading self");
    HMODULE h = g_hSelf;
    FreeLibraryAndExitThread(h, 0);
}

// ---------------- worker 线程 ----------------
static DWORD WINAPI WorkerMain(LPVOID)
{
    Log::Init();
    SetUnhandledExceptionFilter(CrashDumpFilter);
    Log::Printf("[Main] injected, module=%p", (void*)g_hSelf);

    // ADOF_PASSIVE=1：什么都不做（隔离注入本身的影响）
    {
        char envBuf[8] = {};
        if (GetEnvironmentVariableA("ADOF_PASSIVE", envBuf, 8) > 0 && envBuf[0] == '1')
        {
            Log::Printf("[Main] ADOF_PASSIVE=1 — passive mode");
            while (!CheatState::ExitRequested.load(std::memory_order_relaxed))
                Sleep(200);
            return 0;
        }
    }

    // 等待 mono 运行时
    for (int i = 0; i < 600 && !GetModuleHandleW(L"mono-2.0-bdwgc.dll"); i++)
        Sleep(200);
    if (!GetModuleHandleW(L"mono-2.0-bdwgc.dll"))
    {
        Log::Printf("[Main] FATAL: mono runtime not found after 120s");
        return 0;
    }

    if (!MonoApi::Init())
    {
        Log::Printf("[Main] FATAL: MonoApi init failed");
        return 0;
    }

    // 解析游戏类（非阻塞）：实际解析投递到游戏主线程执行
    // （mono API 只有主线程可安全调用），BridgeLoop 会持续重投
    // 直到就绪；期间覆盖层照常工作。
    GameBridge::QueueMainThreadInit();

    // 注：不再对 scrPlayer.DieByHitbox 做 JIT detour ——
    // 本 Unity 6 协作式 Mono 上，mono_compile_method 在非托管线程会
    // 崩溃（见崩溃转储分析）。noFail 字段已覆盖官方关卡的全部死亡
    // 类型（失误/超载/保持失败），hitbox 死亡仅存在于自定义关卡的
    // 特殊地块，正常游玩不受影响。
    //
    // 退出安全：改为挂钩 UnityEngine.Application.Quit（游戏内"退出游戏"
    // 按钮的调用点，逆向证据见 GameDetour.h）。钩子由主线程任务安装
    // （mono_compile_method 仅托管主线程安全），命中后立即 GameQuitting
    // + 录制收尾，杜绝退出期崩溃与 MP4 无 moov 尾部。

    // 渲染钩子（等 dxgi.dll）
    {
        char envBuf[8] = {};
        if (GetEnvironmentVariableA("ADOF_NOHOOK", envBuf, 8) > 0 && envBuf[0] == '1')
            Log::Printf("[Main] ADOF_NOHOOK=1 — render hooks disabled");
        else
            for (int i = 0; i < 600 && !RenderHook::Install(); i++)
                Sleep(200);
    }

    // 开关应用 + 状态读取循环（独立线程，避免在渲染线程做 mono 调用）
    {
        char envBuf[8] = {};
        if (!(GetEnvironmentVariableA("ADOF_NOBRIDGE", envBuf, 8) > 0 && envBuf[0] == '1'))
            GameBridge::StartLoop();
        else
            Log::Printf("[Main] ADOF_NOBRIDGE=1 — bridge loop disabled");
    }

    Log::Printf("[Main] ready — press Insert for menu");

    // 等待卸载请求（FrameTick 由渲染钩子驱动）
    while (!CheatState::ExitRequested.load(std::memory_order_relaxed))
        Sleep(100);

    CleanupAndUnload();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_hSelf = hModule;
        DisableThreadLibraryCalls(hModule);
        if (!CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr))
            return FALSE;
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
