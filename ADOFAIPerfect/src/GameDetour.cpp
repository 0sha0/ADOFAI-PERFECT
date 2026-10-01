#include "GameDetour.h"
#include "GameBridge.h"
#include "MonoApi.h"
#include "CheatState.h"
#include "Log.h"

#include <windows.h>
#include <detours/detours.h>

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
}
