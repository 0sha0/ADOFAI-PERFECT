#include "GameBridge.h"
#include "MonoApi.h"
#include "CheatState.h"
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
//     字符串   = 长度 *(int*)((char*)s+0x10)，字符 (char*)s+0x20
//   （Unity 的 Boehm GC 不移动对象，上述纯内存访问跨线程安全。）
//   仍保留 SEH 兜底。
// ============================================================
namespace GameBridge
{
    using namespace MonoApi;

    static MonoImage* g_img = nullptr;

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
    static uint32_t g_offDeaths = 0;             // static（值类型，metadata 偏移可用）
    static uint32_t g_offCheckpoints = 0;        // static
    static uint32_t g_offAuto = 0;
    static uint32_t g_offUseNoFail = 0;          // static (GCS)
    static uint32_t g_offMgrPlayers = 0;
    static uint32_t g_offPlayerID = 0;
    static uint32_t g_offTrackerComplete = 0;
    static uint32_t g_offTrackerAcc = 0;
    static uint32_t g_offTrackerXAcc = 0;

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
        if (!arr)
            return nullptr;
        return *(void**)((char*)arr + 0x20 + (size_t)index * sizeof(void*));
    }

    // MonoString: MonoObject(0x10) + length(0x4) + pad(0x4) → chars @ 0x20
    static void CopyMonoString(void* str, char* out, size_t outCap)
    {
        out[0] = '\0';
        if (!str)
            return;
        int len = *(int*)((char*)str + 0x10);
        const unsigned short* p = (const unsigned short*)((char*)str + 0x20);
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
        return InitCore(false);
    }

    // ---------------- 主线程初始化任务 ----------------
    static void (*g_postToMainThread)() = nullptr;
    static volatile long g_taskQueued = 0;

    void QueueMainThreadInit()
    {
        if (!g_postToMainThread)
            return;
        if (InterlockedCompareExchange(&g_taskQueued, 1, 0) == 0)
            g_postToMainThread();
    }

    void MainThreadInitTask()
    {
        InterlockedExchange(&g_taskQueued, 0);
        Log::Printf("[Bridge] main-thread init task running");
        InitCore(true);
    }

    // 由 RenderHook 注入投递函数（PostMessage 到游戏窗口）
    void SetMainThreadPoster(void (*fn)())
    {
        g_postToMainThread = fn;
    }

    // ---------------- 应用开关（纯内存写） ----------------
    static void TickInner()
    {
        const bool noDeath = CheatState::NoDeath.load(std::memory_order_relaxed);
        const bool autoCombo = CheatState::AutoCombo.load(std::memory_order_relaxed);

        AcquireSRWLockExclusive(&g_statusLock);
        g_status.bridgeReady = true;
        g_status.noFailApplied = false;
        g_status.autoApplied = false;
        ReleaseSRWLockExclusive(&g_statusLock);

        // ---- 不死模式：强写 scrController.noFail ----
        void* ctrl = g_ctrlInst.valid
                         ? ReadStatic<void*>(g_vtController, g_ctrlInst.offset, nullptr)
                         : nullptr;
        if (ctrl)
        {
            if (noDeath)
            {
                WriteInst<bool>(ctrl, g_offNoFail, true);
                g_appliedNoFail = true;
                AcquireSRWLockExclusive(&g_statusLock);
                g_status.noFailApplied = true;
                ReleaseSRWLockExclusive(&g_statusLock);
            }
            else if (g_appliedNoFail)
            {
                // 关闭时恢复游戏原值（GCS.useNoFail，读不到则视为 false）
                WriteInst<bool>(ctrl, g_offNoFail, ReadStatic<bool>(g_vtGCS, g_offUseNoFail, false));
                g_appliedNoFail = false;
            }
        }
        else if (g_appliedNoFail && !noDeath)
        {
            g_appliedNoFail = false;
        }

        // ---- 自动连击：强写 RDConstants.data.auto（RDC.auto）----
        void* rdData = g_rdData.valid
                           ? ReadStatic<void*>(g_vtRDConstants, g_rdData.offset, nullptr)
                           : nullptr;
        if (rdData)
        {
            if (autoCombo)
            {
                WriteInst<bool>(rdData, g_offAuto, true);
                g_appliedAuto = true;
                AcquireSRWLockExclusive(&g_statusLock);
                g_status.autoApplied = true;
                ReleaseSRWLockExclusive(&g_statusLock);
            }
            else if (g_appliedAuto)
            {
                WriteInst<bool>(rdData, g_offAuto, false);
                g_appliedAuto = false;
            }
        }
        else if (g_appliedAuto && !autoCombo)
        {
            g_appliedAuto = false;
        }
    }

    // ---------------- 读取状态用于 UI（纯内存读） ----------------
    static void ReadStatusInner()
    {
        AcquireSRWLockExclusive(&g_statusLock);
        CheatState::Status& st = g_status;
        void* ctrl = g_ctrlInst.valid
                         ? ReadStatic<void*>(g_vtController, g_ctrlInst.offset, nullptr)
                         : nullptr;
        st.controllerAlive = (ctrl != nullptr);

        if (!ctrl)
        {
            st.gameworld = false;
            st.state = -1;
            _snprintf_s(st.stateName, _TRUNCATE, "Menu");
            ReleaseSRWLockExclusive(&g_statusLock);
            return;
        }

        st.gameworld = ReadInst<bool>(ctrl, g_offGameworld, false);
        st.state = ReadInst<int>(ctrl, g_offCurrentState, -1);
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
            }
        }
        ReleaseSRWLockExclusive(&g_statusLock);
    }

    // ---------------- SEH 包装 ----------------
    static void TickSafe()
    {
        __try
        {
            TickInner();
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                      ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            g_monoDisabled = true;
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
            g_monoDisabled = true;
        }
    }

    // ---------------- 循环线程 ----------------
    static DWORD WINAPI BridgeLoop(LPVOID)
    {
        int tickCount = 0;
        while (!CheatState::ExitRequested.load(std::memory_order_relaxed))
        {
            if (!g_ready)
            {
                // 先在桥接线程保守重试；同时请求主线程任务（可安全创建
                // vtable 并完成静态布局，让桥接在标题画面即就绪）
                Init();
                QueueMainThreadInit();
            }
            else
            {
                if (!g_monoDisabled)
                {
                    TickSafe();
                    if (g_monoDisabled)
                        Log::Printf("[Bridge] AV in Tick — bridge disabled");
                }
                // 就绪后继续补齐 gameplay 引用（playerMgr / marginTrackers
                // 需要游戏实际创建这些单例后才能发现）
                if (!g_mgrInst.valid || !g_trackerArr.valid || !g_offTrackerAcc || !g_offMgrPlayers)
                {
                    static int s_slow = 0;
                    if ((s_slow++ % 60) == 0) // ~0.5s 一次
                    {
                        Init();
                        QueueMainThreadInit();
                    }
                }
                if (!g_monoDisabled && (tickCount % 10) == 0)
                {
                    ReadStatusSafe();
                    if (g_monoDisabled)
                        Log::Printf("[Bridge] AV in ReadStatus — bridge disabled");
                }
            }
            tickCount++;
            Sleep(g_ready ? 7 : 200);
        }
        return 0;
    }

    void StartLoop()
    {
        HANDLE h = CreateThread(nullptr, 0, BridgeLoop, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
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
        if (!g_ready || g_monoDisabled)
            return;
        CheatState::NoDeath.store(false);
        CheatState::AutoCombo.store(false);
        TickSafe(); // 开关已清零 → 走恢复分支
        g_appliedNoFail = false;
        g_appliedAuto = false;
        Log::Printf("[Bridge] restored game state");
    }
}
