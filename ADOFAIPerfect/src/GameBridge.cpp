#include "GameBridge.h"
#include "MonoApi.h"
#include "CheatState.h"
#include "Chart4K.h"
#include "Log.h"

#include <windows.h>
#include <cstring>
#include <cstdio>

// ============================================================
// 关键设计（由崩溃转储分析得出）：
//   Unity 6 的 Mono 采用协作式运行时（Coop GC）。任何 mono API
//   （哪怕是 mono_field_static_get_value 这种"纯"读取）在非托管
//   线程上调用都会触发 mono_thread_info 检查 → mono 主动抛出
//   e0000001 致命异常带崩游戏（已由 game_crash.dmp 栈回溯确认）。
//
//   因此本模块只在 Init 阶段（worker 线程，可容忍）使用 mono 元数据
//   API 解析"字段偏移 + vtable"，之后运行时路径 0 次 mono API 调用：
//     静态字段 = *(T*)((char*)mono_vtable_get_static_field_data(vt) + field->offset)
//     实例字段 = *(T*)((char*)obj + field->offset)
//     数组元素 = *(T**)((char*)arr + 0x20 + i*8)      // MonoArray.vector
//     字符串   = 长度 *(int*)((char*)s+0x10)，字符 (char*)s + g_strCharOff
//   （Unity 的 Boehm GC 不移动对象，上述纯内存访问跨线程安全。）
//   仍保留 SEH 兜底。
// ============================================================
namespace GameBridge
{
    using namespace MonoApi;

    static MonoImage* g_img = nullptr;
    // MonoString 字符数据偏移（主线程 Init 时用 mono_string_chars 实测，不靠猜）
    static int g_strCharOff = 0x14;

    static MonoClass* g_clsController = nullptr;      // scrController
    static MonoClass* g_clsRDConstants = nullptr;     // RDConstants
    static MonoClass* g_clsPlayerMgr = nullptr;       // scrPlayerManager
    static MonoClass* g_clsMistakes = nullptr;        // scrMistakesManager
    static MonoClass* g_clsPlayer = nullptr;          // scrPlayer
    static MonoClass* g_clsGCS = nullptr;             // GCS
    static MonoClass* g_clsTracker = nullptr;         // scrMarginTracker
    static bool g_vtTrackerOK = false;                // tracker 类无需 vtable，仅标记 init 过

    static MonoVTable* g_vtController = nullptr;
    static MonoVTable* g_vtRDConstants = nullptr;
    static MonoVTable* g_vtPlayerMgr = nullptr;
    static MonoVTable* g_vtMistakes = nullptr;
    static MonoVTable* g_vtGCS = nullptr;

    // 缓存的字段偏移（运行时只用偏移，不再触碰 mono API）
    // 缓存的字段偏移（运行时只用偏移，不再触碰 mono API）
    // 引用型静态字段用 StaticRefSlot（field->offset 对它们不可靠，需要发现）
    struct StaticRefSlot
    {
        uint32_t offset = 0;
        bool valid = false;
    };
    static StaticRefSlot g_ctrlInst;
    static StaticRefSlot g_rdData;
    static StaticRefSlot g_mgrInst;
    static StaticRefSlot g_trackerArr;
    static uint32_t g_offNoFail = 0;
    static uint32_t g_offGameworld = 0;
    static uint32_t g_offCurrentSeqID = 0;
    static uint32_t g_offLevelName = 0;
    static uint32_t g_offCurrentState = 0;
    static uint32_t g_offPaused = 0;             // scrController._paused（private，Mono 可读）
    static uint32_t g_offDeaths = 0;             // static（值类型，metadata 偏移可用）
    static uint32_t g_offCheckpoints = 0;        // static
    static uint32_t g_offAuto = 0;
    static uint32_t g_offUseNoFail = 0;          // static (GCS)
    static uint32_t g_offMgrPlayers = 0;
    static uint32_t g_offPlayerID = 0;
    static uint32_t g_offTrackerComplete = 0;
    static uint32_t g_offTrackerAcc = 0;
    static uint32_t g_offTrackerXAcc = 0;
    static uint32_t g_offTrackerHits = 0;      // scrMarginTracker.hitMargins (List<HitMargin>)
    static uint32_t g_offTrackerCounts = 0;    // scrMarginTracker.hitMarginsCount (int[])
    static int      g_maxCombo = 0;            // 本关历史最大连击（桥接侧统计）

    // 主线程开关应用（mono API 只能在游戏主线程调用）
    static MonoClassField* g_fieldNoFail = nullptr;   // scrController.noFail
    static MonoClassField* g_fieldInstance = nullptr; // scrController._instance
    static MonoClassField* g_fieldAuto = nullptr;     // RDConstants.auto
    static MonoClassField* g_fieldUseNoFail = nullptr; // GCS.useNoFail
    static MonoMethod*     g_mtdGetData = nullptr;    // RDConstants.get_data
    static bool     g_cheatAppliedNoFail = false;
    static bool     g_cheatAppliedAuto = false;

    static bool g_ready = false;
    static bool g_offsetsReady = false;
    static bool g_monoDisabled = false;
    static bool g_appliedNoFail = false;
    static bool g_appliedAuto = false;

    static SRWLOCK g_statusLock = SRWLOCK_INIT;
    static CheatState::Status g_status{};

    static const char* kStateNames[] = {
        "None", "Start", "Countdown", "Checkpoint",
        "PlayerControl", "Fail", "Fail2", "Won"
    };

    bool Ready() { return g_ready && !g_monoDisabled; }

    // ---------------- 纯内存访问原语 ----------------
    // 静态字段所在数据区（vtable->data）；类未初始化时为 NULL
    static void* StaticData(MonoVTable* vt)
    {
        if (!vt || !mono_vtable_get_static_field_data)
            return nullptr;
        return mono_vtable_get_static_field_data(vt); // 纯指针读取
    }

    template <typename T>
    static T ReadStatic(MonoVTable* vt, uint32_t offset, T defVal)
    {
        void* data = StaticData(vt);
        if (!data)
            return defVal;
        return *(T*)((char*)data + offset);
    }

    template <typename T>
    static void WriteStatic(MonoVTable* vt, uint32_t offset, T v)
    {
        void* data = StaticData(vt);
        if (!data)
            return;
        *(T*)((char*)data + offset) = v;
    }

    // ---- 引用型静态字段偏移发现 ----
    // Unity mono 对引用静态字段的 field->offset 不可靠（可能为 0）。
    // 改为在静态数据区头部扫描指针，用"对象的类指针/类名 == 期望"验证。
    // 只做指针读取 + 类指针比较（纯内存访问），SEH 包裹防垃圾指针。
    static MonoVTable* s_discoverVt = nullptr;
    static MonoClass* s_discoverCls = nullptr;
    static const char* s_discoverName = nullptr;
    static StaticRefSlot s_discoverResult{};

    static bool ClassMatches(MonoClass* c)
    {
        if (!c)
            return false;
        if (s_discoverCls && c == s_discoverCls)
            return true;
        if (s_discoverName && mono_class_get_name && strcmp(mono_class_get_name(c), s_discoverName) == 0)
            return true;
        return false;
    }

    static void DiscoverScan()
    {
        void* data = StaticData(s_discoverVt);
        if (!data)
            return;
        for (uint32_t off = 0; off <= 0x100; off += 8)
        {
            void* obj = *(void**)((char*)data + off);
            if (!obj)
                continue;
            MonoClass* c = mono_object_get_class((MonoObject*)obj);
            if (ClassMatches(c))
            {
                s_discoverResult.offset = off;
                s_discoverResult.valid = true;
                return;
            }
        }
    }

    static StaticRefSlot DiscoverStaticRef(MonoVTable* vt, MonoClass* expectedClass, const char* expectedName = nullptr)
    {
        StaticRefSlot r;
        if (!vt || (!expectedClass && !expectedName) || !mono_object_get_class)
            return r;
        s_discoverVt = vt;
        s_discoverCls = expectedClass;
        s_discoverName = expectedName;
        s_discoverResult = r;
        __try
        {
            DiscoverScan();
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                      ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
        }
        return s_discoverResult;
    }

    template <typename T>
    static T ReadInst(void* obj, uint32_t offset, T defVal)
    {
        if (!obj || !offset)
            return defVal;
        return *(T*)((char*)obj + offset);
    }

    template <typename T>
    static void WriteInst(void* obj, uint32_t offset, T v)
    {
        if (!obj || !offset)
            return;
        *(T*)((char*)obj + offset) = v;
    }

    static void* ArrayElement(void* arr, int index)
    {
        // MonoArray: MonoObject(0x10) + bounds(0x8) + max_length(0x8) → vector @ 0x20
        if (!arr || index < 0)
            return nullptr;
        __try
        {
            int len = *(int*)((char*)arr + 0x18);
            if (index >= len)
                return nullptr;
            return *(void**)((char*)arr + 0x20 + (size_t)index * sizeof(void*));
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                      ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return nullptr;
        }
    }

    // MonoString: MonoObject(0x10) + length(0x4) + pad(0x4) → chars @ 0x20
    static void CopyMonoString(void* str, char* out, size_t outCap)
    {
        out[0] = '\0';
        if (!str)
            return;
        int len = *(int*)((char*)str + 0x10);
        if (len <= 0 || len > 8192)
            return;
        const unsigned short* p = (const unsigned short*)((char*)str + g_strCharOff);
        if (len <= 0)
            return;
        size_t o = 0;
        for (int i = 0; i < len && o + 4 < outCap; i++)
        {
            unsigned c = p[i];
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < len)
            {
                unsigned lo = p[i + 1];
                if (lo >= 0xDC00 && lo < 0xE000)
                {
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                    i++;
                }
            }
            if (c < 0x80) out[o++] = (char)c;
            else if (c < 0x800)
            {
                out[o++] = (char)(0xC0 | (c >> 6));
                out[o++] = (char)(0x80 | (c & 0x3F));
            }
            else if (c < 0x10000)
            {
                out[o++] = (char)(0xE0 | (c >> 12));
                out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (c & 0x3F));
            }
            else
            {
                out[o++] = (char)(0xF0 | (c >> 18));
                out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
                out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (c & 0x3F));
            }
        }
        out[o] = '\0';
    }

    // 辅助：解析字段并缓存偏移
    static uint32_t FieldOffset(MonoClass* cls, const char* name)
    {
        if (!cls)
            return 0;
        MonoClassField* f = mono_class_get_field_from_name(cls, name);
        if (!f)
            return 0;
        return mono_field_get_offset(f);
    }

    // ---------------- 解析（幂等） ----------------
    // allowVtableCreate:
    //   false —— 桥接线程（非托管）：只为"偏移已非零（类已初始化）"的类
    //            取 vtable（mono_class_vtable 仅返回缓存，绝不触发 GC 分配）。
    //   true  —— 游戏主线程（mono 已托管）：可为尚未使用的类创建 vtable，
    //            同时完成静态字段布局，使桥接在标题画面即就绪。
    // MonoString 布局探测：Unity Mono 的 chars 只要 2 字节对齐（通常 0x14），
    // 之前硬编码 0x20 会读错位 → 关卡名乱码。这里在主线程（mono 已托管）
    // 用 mono_string_chars 实测一次，之后运行时只用偏移裸读。
    static void ProbeStringLayout(MonoDomain* dom)
    {
        static bool done = false;
        if (done || !dom || !mono_string_new || !mono_string_chars)
            return;
        MonoString* probe = mono_string_new(dom, "PROBE");
        gunichar2* chars = probe ? mono_string_chars(probe) : nullptr;
        if (!probe || !chars)
            return;
        done = true;
        uintptr_t off = (uintptr_t)((char*)chars - (char*)probe);
        if (off > 0 && off < 0x40)
            g_strCharOff = (int)off;
        int lenAt0x10 = -1;
        __try
        {
            lenAt0x10 = *(int*)((char*)probe + 0x10);
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                      ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            lenAt0x10 = -1;
        }
        Log::Printf("[Bridge] MonoString layout: chars@0x%X len@0x10=%d (expect 5)",
                    g_strCharOff, lenAt0x10);
    }

    static bool InitCore(bool allowVtableCreate)
    {
        if (!MonoApi::Ready())
            return false;

        if (!g_img)
        {
            g_img = mono_image_loaded("Assembly-CSharp");
            if (!g_img)
                return false;
            Log::Printf("[Bridge] Assembly-CSharp image = %p", (void*)g_img);
        }

        if (!g_clsController)
        {
            g_clsController = mono_class_from_name(g_img, "", "scrController");
            g_clsRDConstants = mono_class_from_name(g_img, "", "RDConstants");
            g_clsPlayerMgr = mono_class_from_name(g_img, "", "scrPlayerManager");
            g_clsMistakes = mono_class_from_name(g_img, "", "scrMistakesManager");
            g_clsPlayer = mono_class_from_name(g_img, "", "scrPlayer");
            g_clsGCS = mono_class_from_name(g_img, "", "GCS");
            g_clsTracker = mono_class_from_name(g_img, "", "scrMarginTracker");
            if (!g_clsController || !g_clsRDConstants || !g_clsPlayerMgr || !g_clsMistakes || !g_clsPlayer)
                return false;
            Log::Printf("[Bridge] classes resolved");
        }

        MonoDomain* dom = mono_domain_get ? mono_domain_get() : nullptr;
        if (!dom)
            dom = mono_get_root_domain();
        if (!dom)
            return false;

        if (allowVtableCreate)
            ProbeStringLayout(dom);

        if (allowVtableCreate)
        {
            // 主线程（mono 托管线程）上这是完全合法的操作：
            // mono_class_init 完成字段布局（含静态字段偏移），不运行 cctor。
            if (g_clsController && !g_vtController)
            {
                mono_class_init(g_clsController);
                g_vtController = mono_class_vtable(dom, g_clsController);
            }
            if (g_clsRDConstants && !g_vtRDConstants)
            {
                int rv = mono_class_init(g_clsRDConstants);
                g_vtRDConstants = mono_class_vtable(dom, g_clsRDConstants);
                Log::Printf("[Bridge] mono_class_init(RDConstants) rv=%d vt=%p", rv, (void*)g_vtRDConstants);

                // 诊断：枚举全部字段与偏移（一次性）
                if (mono_class_get_fields && mono_field_get_name && mono_field_get_offset)
                {
                    void* iter = nullptr;
                    MonoClassField* f;
                    char line[512];
                    int n = 0;
                    line[0] = 0;
                    while ((f = (MonoClassField*)mono_class_get_fields(g_clsRDConstants, &iter)) != nullptr)
                    {
                        const char* fn = mono_field_get_name(f);
                        uint32_t fo = mono_field_get_offset(f);
                        int printed = _snprintf_s(line + strlen(line), sizeof(line) - strlen(line), _TRUNCATE,
                                                  "%s=%x ", fn ? fn : "?", fo);
                        if (printed < 0 || ++n >= 12)
                            break;
                    }
                    Log::Printf("[Bridge] RDConstants fields: %s", line);
                }
            }
            if (g_clsPlayerMgr && !g_vtPlayerMgr)
            {
                mono_class_init(g_clsPlayerMgr);
                g_vtPlayerMgr = mono_class_vtable(dom, g_clsPlayerMgr);
            }
            if (g_clsMistakes && !g_vtMistakes)
            {
                mono_class_init(g_clsMistakes);
                g_vtMistakes = mono_class_vtable(dom, g_clsMistakes);
            }
            if (g_clsGCS && !g_vtGCS)
            {
                mono_class_init(g_clsGCS);
                g_vtGCS = mono_class_vtable(dom, g_clsGCS);
            }
            if (g_clsTracker && !g_vtTrackerOK)
            {
                mono_class_init(g_clsTracker);
                g_vtTrackerOK = true;
            }
        }

        if (!g_ctrlInst.valid)
        {
            uint32_t off = FieldOffset(g_clsController, "_instance");
            if (off)
                g_ctrlInst = { off, true };
            else if (g_vtController)
                g_ctrlInst = DiscoverStaticRef(g_vtController, g_clsController);
        }
        if (!g_offNoFail)
            g_offNoFail = FieldOffset(g_clsController, "noFail");
        if (!g_offGameworld)
            g_offGameworld = FieldOffset(g_clsController, "gameworld");
        if (!g_offCurrentSeqID)
            g_offCurrentSeqID = FieldOffset(g_clsController, "currentSeqID");
        if (!g_offLevelName)
            g_offLevelName = FieldOffset(g_clsController, "levelName");
        if (!g_offCurrentState)
            g_offCurrentState = FieldOffset(g_clsController, "currentState");
        if (!g_offPaused)
            g_offPaused = FieldOffset(g_clsController, "_paused");
        if (!g_offDeaths)
            g_offDeaths = FieldOffset(g_clsController, "deaths");
        if (!g_offCheckpoints)
            g_offCheckpoints = FieldOffset(g_clsController, "checkpointsUsed");
        if (!g_rdData.valid)
        {
            uint32_t off = FieldOffset(g_clsRDConstants, "internalData");
            if (off)
                g_rdData = { off, true };
            else if (g_vtRDConstants)
                g_rdData = DiscoverStaticRef(g_vtRDConstants, g_clsRDConstants);
        }
        if (!g_offAuto)
            g_offAuto = FieldOffset(g_clsRDConstants, "auto");
        if (!g_offUseNoFail && g_clsGCS)
            g_offUseNoFail = FieldOffset(g_clsGCS, "useNoFail");
        if (!g_mgrInst.valid)
        {
            uint32_t off = FieldOffset(g_clsPlayerMgr, "_instance");
            if (off)
                g_mgrInst = { off, true };
            else if (g_vtPlayerMgr)
                g_mgrInst = DiscoverStaticRef(g_vtPlayerMgr, g_clsPlayerMgr);
        }
        if (!g_offMgrPlayers)
            g_offMgrPlayers = FieldOffset(g_clsPlayerMgr, "<players>k__BackingField");
        if (!g_trackerArr.valid)
        {
            uint32_t off = FieldOffset(g_clsMistakes, "marginTrackers");
            if (off)
                g_trackerArr = { off, true };
            else if (g_vtMistakes)
                // marginTrackers 是数组（元素类 scrMarginTracker），
                // 按数组类名匹配
                g_trackerArr = DiscoverStaticRef(g_vtMistakes, nullptr, "scrMarginTracker[]");
        }
        if (!g_offPlayerID && g_clsPlayer)
            g_offPlayerID = FieldOffset(g_clsPlayer, "playerID");
        if (!g_offTrackerComplete && g_clsTracker)
            g_offTrackerComplete = FieldOffset(g_clsTracker, "<percentComplete>k__BackingField");
        if (!g_offTrackerAcc && g_clsTracker)
            g_offTrackerAcc = FieldOffset(g_clsTracker, "<percentAcc>k__BackingField");
        if (!g_offTrackerXAcc && g_clsTracker)
            g_offTrackerXAcc = FieldOffset(g_clsTracker, "<percentXAcc>k__BackingField");
        if (!g_offTrackerHits && g_clsTracker)
            g_offTrackerHits = FieldOffset(g_clsTracker, "hitMargins");
        if (!g_offTrackerCounts && g_clsTracker)
            g_offTrackerCounts = FieldOffset(g_clsTracker, "hitMarginsCount");

        if (g_vtController && g_vtRDConstants && g_ctrlInst.valid && g_offNoFail &&
            g_rdData.valid && g_offAuto)
        {
            if (!g_ready)
            {
                g_offsetsReady = true;
                g_ready = true;
                Log::Printf("[Bridge] GameBridge ready (raw-offset mode)");
            }
            return true;
        }

        // 诊断：每秒一次说明缺什么
        static DWORD s_lastDiag = 0;
        DWORD now = GetTickCount();
        if (now - s_lastDiag > 1000)
        {
            s_lastDiag = now;
            Log::Printf("[Bridge] init pending: vtCtrl=%p vtRdc=%p ctrlInst=%d rdData=%d offNoFail=%x offAuto=%x",
                        (void*)g_vtController, (void*)g_vtRDConstants,
                        (int)g_ctrlInst.valid, (int)g_rdData.valid, g_offNoFail, g_offAuto);
        }
        return false;
    }

    bool Init()
    {
        // 非托管线程绝不能调用 mono API；真正的解析由
        // QueueMainThreadInit() 投递到游戏主线程完成。
        return g_ready;
    }

    // ---------------- 主线程初始化任务 ----------------
    static void (*g_postToMainThread)() = nullptr;
    static volatile long g_taskFlags = 0;
    static const long kTaskInit = 1;
    static const long kTaskCheats = 2;

    static void PostTask(long bit)
    {
        if (!g_postToMainThread)
            return;
        if ((InterlockedOr(&g_taskFlags, bit) & bit) == 0)
            g_postToMainThread();
    }

    void QueueMainThreadInit() { PostTask(kTaskInit); }
    void QueueCheatApply() { PostTask(kTaskCheats); }

    static void ApplyCheatsMainThread();   // 定义见下（开关应用，仅主线程）

    // 主线程用 scrController.get_instance() 刷新的实例（自动重新 Find 已销毁的
    // _instance）。静态槽里的对象被 Unity 销毁后 `_instance == null` 为真，
    // 但槽位仍留着失效指针 —— 直接读会拿到垃圾状态（自定义/额外关卡切换时常见）。
    static std::atomic<void*> g_ctrlFresh{ nullptr };
    // 主线程 get_instance 是否已至少成功发布过一次：发布后以它为准（含 null），
    // 避免在切自定义/额外关卡时继续读静态槽里已销毁的旧 scrController
    static std::atomic<bool>  g_ctrlFreshPub{ false };

    static void RefreshControllerInstanceMainThread()
    {
        if (!g_clsController || !mono_runtime_invoke)
            return;
        static MonoMethod* m = nullptr;
        if (!m)
            m = mono_class_get_method_from_name(g_clsController, "get_instance", 0);
        if (!m)
            return;
        MonoObject* exc = nullptr;
        MonoObject* o = mono_runtime_invoke(m, nullptr, nullptr, &exc);
        if (!exc)
        {
            g_ctrlFresh.store((void*)o, std::memory_order_release);
            g_ctrlFreshPub.store(true, std::memory_order_release);
        }
    }

    void MainThreadInitTask()
    {
        long flags = InterlockedExchange(&g_taskFlags, 0);
        static DWORD s_lastInitLog = 0;
        if ((flags & kTaskInit) && GetTickCount() - s_lastInitLog > 2000)
        {
            s_lastInitLog = GetTickCount();
            Log::Printf("[Bridge] main-thread init task running");
        }
        InitCore(true);                 // 元数据解析（主线程 = mono 托管线程）
        RefreshControllerInstanceMainThread();
        Chart4K::MainThreadResolve();   // 4K 辅助：主线程安全创建 vtable / 解析偏移
        if ((flags & kTaskCheats) || CheatState::NoDeath.load(std::memory_order_relaxed) ||
            CheatState::AutoCombo.load(std::memory_order_relaxed))
            ApplyCheatsMainThread();
    }

    // 由 RenderHook 注入投递函数（PostMessage 到游戏窗口）
    void SetMainThreadPoster(void (*fn)())
    {
        g_postToMainThread = fn;
    }

    // ---------------- 应用开关（仅游戏主线程；mono API 安全） ----------------
    // 说明（为什么不在桥接线程写字段）：
    //   Unity 的 Mono 是协作式运行时，非托管线程调用 mono API 会触发
    //   致命异常；而"用缓存的字段偏移裸写"在偏移不可靠时（例如
    //   RDConstants.auto 这类字段）会写坏托管堆 → 之后随机 AV。
    //   这里全部改用主线程 + mono_field_set_value，偏移由运行时给出，
    //   彻底消除猜测。
    static void* CtrlInstanceMainThread()
    {
        if (!g_clsController || !g_vtController)
            return nullptr;
        if (!g_fieldInstance)
            g_fieldInstance = mono_class_get_field_from_name(g_clsController, "_instance");
        if (!g_fieldInstance)
            return nullptr;
        void* obj = nullptr;
        mono_field_static_get_value(g_vtController, g_fieldInstance, &obj);
        return obj;
    }

    static void ApplyCheatsMainThread()
    {
        if (!MonoApi::Ready() || !g_clsController)
            return;

        bool wantNoFail = CheatState::NoDeath.load(std::memory_order_relaxed);
        bool wantAuto = CheatState::AutoCombo.load(std::memory_order_relaxed);

        // ---- 不死模式：scrController.noFail ----
        if (!g_fieldNoFail)
            g_fieldNoFail = mono_class_get_field_from_name(g_clsController, "noFail");
        void* ctrl = CtrlInstanceMainThread();
        if (ctrl && g_fieldNoFail)
        {
            bool cur = false;
            mono_field_get_value((MonoObject*)ctrl, g_fieldNoFail, &cur);
            bool target = wantNoFail;
            if (!wantNoFail && g_cheatAppliedNoFail)
            {
                // 关闭时恢复游戏自身的原值（GCS.useNoFail）
                target = false;
                if (g_clsGCS && g_vtGCS)
                {
                    if (!g_fieldUseNoFail)
                        g_fieldUseNoFail = mono_class_get_field_from_name(g_clsGCS, "useNoFail");
                    if (g_fieldUseNoFail)
                        mono_field_static_get_value(g_vtGCS, g_fieldUseNoFail, &target);
                }
            }
            if (cur != target)
            {
                mono_field_set_value((MonoObject*)ctrl, g_fieldNoFail, &target);
                Log::Printf("[Bridge] noFail <- %d (main thread)", (int)target);
            }
            g_cheatAppliedNoFail = wantNoFail;
        }
        else if (!wantNoFail)
        {
            g_cheatAppliedNoFail = false;
        }

        // ---- 自动连击：RDConstants.data.auto（RDC.auto）----
        if (g_clsRDConstants && mono_runtime_invoke)
        {
            if (!g_mtdGetData)
                g_mtdGetData = mono_class_get_method_from_name(g_clsRDConstants, "get_data", 0);
            if (!g_fieldAuto)
                g_fieldAuto = mono_class_get_field_from_name(g_clsRDConstants, "auto");
            if (g_mtdGetData && g_fieldAuto)
            {
                MonoObject* exc = nullptr;
                MonoObject* data = mono_runtime_invoke(g_mtdGetData, nullptr, nullptr, &exc);
                if (data && !exc)
                {
                    bool cur = false;
                    mono_field_get_value(data, g_fieldAuto, &cur);
                    if (cur != wantAuto)
                    {
                        bool v = wantAuto;
                        mono_field_set_value(data, g_fieldAuto, &v);
                        Log::Printf("[Bridge] RDC.auto <- %d (main thread)", (int)v);
                    }
                    g_cheatAppliedAuto = wantAuto;
                }
            }
        }
        else if (!wantAuto)
        {
            g_cheatAppliedAuto = false;
        }

        AcquireSRWLockExclusive(&g_statusLock);
        g_status.noFailApplied = g_cheatAppliedNoFail;
        g_status.autoApplied = g_cheatAppliedAuto;
        ReleaseSRWLockExclusive(&g_statusLock);
    }

    // ---------------- 桥接线程 Tick（纯指针读，绝不调用 mono API） ----------------
    static void TickInner()
    {
        AcquireSRWLockExclusive(&g_statusLock);
        g_status.bridgeReady = true;
        g_status.noFailApplied = g_cheatAppliedNoFail;
        g_status.autoApplied = g_cheatAppliedAuto;
        ReleaseSRWLockExclusive(&g_statusLock);
    }

    // ---------------- 读取状态用于 UI（纯内存读） ----------------
    // 全部指针解引用都在锁外完成，只有最后一次 POD 拷贝在锁内：
    // 一旦中途 AV（读到已销毁对象），SEH 会直接跳到 handler，函数余下的
    // ReleaseSRWLockExclusive 不会执行 —— 而 SRW 锁只能由持锁线程释放，
    // 届时桥接线程与渲染线程将永久死锁（表现为"谱面偶尔再也读不出来"）。
    static void PublishStatus(const CheatState::Status& st)
    {
        AcquireSRWLockExclusive(&g_statusLock);
        g_status = st;
        ReleaseSRWLockExclusive(&g_statusLock);
    }

    static void ReadStatusInner()
    {
        CheatState::Status st{};
        {
            AcquireSRWLockShared(&g_statusLock);
            st.bridgeReady = g_status.bridgeReady;
            st.noFailApplied = g_status.noFailApplied;
            st.autoApplied = g_status.autoApplied;
            ReleaseSRWLockShared(&g_statusLock);
        }
        void* ctrl = g_ctrlInst.valid
                         ? ReadStatic<void*>(g_vtController, g_ctrlInst.offset, nullptr)
                         : nullptr;
        {
            void* fresh = g_ctrlFresh.load(std::memory_order_acquire);
            if (g_ctrlFreshPub.load(std::memory_order_acquire))
                ctrl = fresh;   // 主线程 get_instance 为唯一权威（含 null）
            else if (fresh && fresh != ctrl)
                ctrl = fresh;   // 主线程 get_instance 已重新 Find（旧实例被销毁）
        }
        st.controllerAlive = (ctrl != nullptr);

        if (!ctrl)
        {
            st.gameworld = false;
            st.state = -1;
            _snprintf_s(st.stateName, _TRUNCATE, "Menu");
            PublishStatus(st);
            return;
        }

        st.gameworld = ReadInst<bool>(ctrl, g_offGameworld, false);
        st.state = ReadInst<int>(ctrl, g_offCurrentState, -1);
        st.paused = g_offPaused ? ReadInst<bool>(ctrl, g_offPaused, false) : false;
        st.floorIndex = ReadInst<int>(ctrl, g_offCurrentSeqID, -1) + 1;
        st.deaths = ReadStatic<int>(g_vtController, g_offDeaths, 0);
        st.checkpoints = ReadStatic<int>(g_vtController, g_offCheckpoints, 0);

        if (st.state >= 0 && st.state < (int)(sizeof(kStateNames) / sizeof(kStateNames[0])))
            _snprintf_s(st.stateName, _TRUNCATE, "%s", kStateNames[st.state]);
        else
            _snprintf_s(st.stateName, _TRUNCATE, "?");

        void* levelNameStr = ReadInst<void*>(ctrl, g_offLevelName, nullptr);
        CopyMonoString(levelNameStr, st.levelName, sizeof(st.levelName));

        // 精准度：scrPlayerManager._instance.<players>[0] → marginTrackers[playerID]
        void* mgr = g_mgrInst.valid
                        ? ReadStatic<void*>(g_vtPlayerMgr, g_mgrInst.offset, nullptr)
                        : nullptr;
        void* playersArr = mgr ? ReadInst<void*>(mgr, g_offMgrPlayers, nullptr) : nullptr;
        void* player = ArrayElement(playersArr, 0);
        if (player)
        {
            int pid = ReadInst<int>(player, g_offPlayerID, 0);
            void* trackersArr = g_trackerArr.valid
                                    ? ReadStatic<void*>(g_vtMistakes, g_trackerArr.offset, nullptr)
                                    : nullptr;
            void* tracker = ArrayElement(trackersArr, pid >= 0 ? pid : 0);
            if (tracker)
            {
                st.percentComplete = ReadInst<float>(tracker, g_offTrackerComplete, 0.f);
                st.percentAcc = ReadInst<float>(tracker, g_offTrackerAcc, 0.f);
                st.percentXAcc = ReadInst<float>(tracker, g_offTrackerXAcc, 0.f);

                // 逐类判定次数：hitMarginsCount 是 int[]，元素在 0x20 + i*4
                void* countsArr = g_offTrackerCounts
                                      ? ReadInst<void*>(tracker, g_offTrackerCounts, nullptr)
                                      : nullptr;
                if (countsArr)
                    for (int i = 0; i < 12; i++)
                        st.hitCounts[i] = *(int*)((char*)countsArr + 0x20 + (size_t)i * 4);

                // 连击：从 hitMargins(List<HitMargin>) 尾部数连续有效判定
                void* hitsList = g_offTrackerHits
                                     ? ReadInst<void*>(tracker, g_offTrackerHits, nullptr)
                                     : nullptr;
                int combo = 0;
                if (hitsList)
                {
                    void* items = *(void**)((char*)hitsList + 0x10);
                    int size = *(int*)((char*)hitsList + 0x18);
                    st.hitTotal = size > 0 ? size : 0;
                    if (items && size > 0)
                    {
                        for (int i = size - 1; i >= 0 && i > size - 8192; i--)
                        {
                            int m = *(int*)((char*)items + 0x20 + (size_t)i * 4);
                            bool valid = (m >= 1 && m <= 5) || m == 10; // VeryEarly..VeryLate / Auto
                            if (!valid)
                                break;
                            combo++;
                        }
                    }
                }
                else
                {
                    st.hitTotal = 0;
                }
                st.combo = combo;
                if (combo == 0 && st.hitTotal == 0)
                    g_maxCombo = 0;             // 新关卡 / 重开
                if (combo > g_maxCombo)
                    g_maxCombo = combo;
                st.maxCombo = g_maxCombo;
            }
        }
        PublishStatus(st);
    }

    // ---------------- SEH 包装 ----------------
    // AV 视为"读到了刚被销毁的对象"（切场景 / 自定义关卡加载瞬间很常见）：
    // 清掉可能失效的实例缓存并暂停桥接 2 秒后自动重试，而不是一次 AV 就永久停摆
    static DWORD g_monoDisableUntil = 0;
    static void NoteBridgeFault(const char* where)
    {
        g_monoDisabled = true;
        g_monoDisableUntil = GetTickCount() + 2000;
        g_ctrlFresh.store(nullptr, std::memory_order_release);
        Log::Printf("[Bridge] AV in %s — bridge paused 2s", where);
    }

    static void TickSafe()
    {
        __try
        {
            TickInner();
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                      ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            NoteBridgeFault("Tick");
        }
    }

    static void ReadStatusSafe()
    {
        __try
        {
            ReadStatusInner();
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                      ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            NoteBridgeFault("ReadStatus");
        }
    }

    // ---------------- 循环线程 ----------------
    static DWORD WINAPI BridgeLoop(LPVOID)
    {
        int tickCount = 0;
        while (!CheatState::ExitRequested.load(std::memory_order_relaxed))
        {
            if (g_monoDisabled && g_monoDisableUntil && GetTickCount() > g_monoDisableUntil)
            {
                g_monoDisabled = false;
                g_monoDisableUntil = 0;
                Log::Printf("[Bridge] bridge resumed");
            }
            if (!g_ready)
            {
                // 元数据解析全部投递到游戏主线程（本线程禁止调用 mono API）
                if ((tickCount % 5) == 0)
                    QueueMainThreadInit();
            }
            else
            {
                TickSafe();
                // 4K 辅助：歌曲时钟 + 谱面提取（纯内存读取，独立于 mono 禁用状态）
                Chart4K::Tick();
                // 就绪后继续补齐 gameplay 引用（playerMgr / marginTrackers
                // 需要游戏实际创建这些单例后才能发现）
                if (!g_mgrInst.valid || !g_trackerArr.valid || !g_offTrackerAcc || !g_offMgrPlayers)
                {
                    static int s_slow = 0;
                    if ((s_slow++ % 60) == 0) // ~0.5s 一次
                        QueueMainThreadInit();
                }
                if (!g_monoDisabled && (tickCount % 10) == 0)
                    ReadStatusSafe();
            }
            // 开关应用（主线程执行；开启期间周期性重投，因为关卡加载会重置）
            if ((CheatState::NoDeath.load(std::memory_order_relaxed) ||
                 CheatState::AutoCombo.load(std::memory_order_relaxed)) && (tickCount % 30) == 0)
                QueueCheatApply();
            tickCount++;
            Sleep(g_ready ? 7 : 100);
        }
        return 0;
    }

    void StartLoop()
    {
        HANDLE h = CreateThread(nullptr, 0, BridgeLoop, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }

    void* GetControllerInstance()
    {
        if (g_ctrlFreshPub.load(std::memory_order_acquire))
            return g_ctrlFresh.load(std::memory_order_acquire);
        if (!g_ctrlInst.valid)
            return nullptr;
        return ReadStatic<void*>(g_vtController, g_ctrlInst.offset, nullptr);
    }

    void GetStatusSnapshot(CheatState::Status* out)
    {
        AcquireSRWLockShared(&g_statusLock);
        *out = g_status;
        ReleaseSRWLockShared(&g_statusLock);
    }

    // ---------------- 卸载时恢复 ----------------
    void Restore()
    {
        CheatState::NoDeath.store(false);
        CheatState::AutoCombo.store(false);
        // 卸载路径不再直接改游戏内存：noFail 由下次关卡加载重置，
        // RDC.auto 在每次加载关卡时被游戏自身清为 false。
        g_cheatAppliedNoFail = false;
        g_cheatAppliedAuto = false;
        Log::Printf("[Bridge] cheat flags cleared");
    }
}
