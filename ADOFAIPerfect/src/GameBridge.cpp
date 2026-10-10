#include "GameBridge.h"
#include "MonoApi.h"
#include "CheatState.h"
#include "Chart4K.h"
#include "Log.h"
#include "GameDetour.h"

#include <windows.h>
#include <cstring>
#include <cstdio>
#include <vector>
#include <mutex>

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
    static MonoClass* g_clsPlanetSys = nullptr;       // PlanetarySystem
    static MonoClass* g_clsPlanet = nullptr;          // scrPlanet
    static MonoClass* g_clsFloorBr = nullptr;         // scrFloor（marginScale / nextfloor）
    static MonoClass* g_clsConductor = nullptr;       // scrConductor（crotchetAtStart）
    static bool g_vtTrackerOK = false;                // tracker 类无需 vtable，仅标记 init 过

    static MonoVTable* g_vtController = nullptr;
    static MonoVTable* g_vtRDConstants = nullptr;
    static MonoVTable* g_vtPlayerMgr = nullptr;
    static MonoVTable* g_vtMistakes = nullptr;
    static MonoVTable* g_vtGCS = nullptr;
    static MonoVTable* g_vtConductor = nullptr;

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
    static StaticRefSlot g_csInst;                     // scrConductor._instance
    static uint32_t g_offNoFail = 0;
    static uint32_t g_offLevelSkipped = 0;   // scrController.levelWasSkipped
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
    // 实时判定角度（scrPlanet / PlanetarySystem / scrFloor，全部逆向确认的字段名）
    static uint32_t g_offPlayerPS = 0;         // scrPlayer.planetarySystem
    static uint32_t g_offPSChosen = 0;         // PlanetarySystem.chosenPlanet
    static uint32_t g_offPSSpeed = 0;          // PlanetarySystem.speed (double)
    static uint32_t g_offPSCw = 0;             // PlanetarySystem.isCW (bool)
    static uint32_t g_offPAngle = 0;           // scrPlanet.angle (double)
    static uint32_t g_offPTarget = 0;          // scrPlanet.<targetExitAngle>k__BackingField (double)
    static uint32_t g_offPlanetFloor = 0;      // scrPlanet.currfloor
    static uint32_t g_offFloorNext = 0;        // scrFloor.nextfloor
    static uint32_t g_offFloorMargin = 0;      // scrFloor.marginScale (double)
    static uint32_t g_offGcsDiff = 0;          // GCS.difficulty (static int)
    static uint32_t g_offGcsTrial = 0;         // GCS.currentSpeedTrial (static float)
    static uint32_t g_offFloorSeq = 0;         // scrFloor.seqID (int)
    static uint32_t g_offFloorHoldLen = 0;     // scrFloor.holdLength (int)
    static uint32_t g_offFloorHoldComp = 0;    // scrFloor.holdCompletion (float)
    static uint32_t g_offCsCrotchet = 0;       // scrConductor.crotchetAtStart (double)
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
            g_clsPlanetSys = mono_class_from_name(g_img, "", "PlanetarySystem");
            g_clsPlanet = mono_class_from_name(g_img, "", "scrPlanet");
            g_clsFloorBr = mono_class_from_name(g_img, "", "scrFloor");
            g_clsConductor = mono_class_from_name(g_img, "", "scrConductor");
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
            if (g_clsConductor && !g_vtConductor)
            {
                mono_class_init(g_clsConductor);
                g_vtConductor = mono_class_vtable(dom, g_clsConductor);
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
        if (!g_offLevelSkipped)
            g_offLevelSkipped = FieldOffset(g_clsController, "levelWasSkipped");
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
        if (!g_offPlayerPS && g_clsPlayer)
            g_offPlayerPS = FieldOffset(g_clsPlayer, "planetarySystem");
        if (!g_offPSChosen && g_clsPlanetSys)
        {
            g_offPSChosen = FieldOffset(g_clsPlanetSys, "chosenPlanet");
            g_offPSSpeed = FieldOffset(g_clsPlanetSys, "speed");
            g_offPSCw = FieldOffset(g_clsPlanetSys, "isCW");
        }
        if (!g_offPAngle && g_clsPlanet)
        {
            g_offPAngle = FieldOffset(g_clsPlanet, "angle");
            g_offPTarget = FieldOffset(g_clsPlanet, "<targetExitAngle>k__BackingField");
            g_offPlanetFloor = FieldOffset(g_clsPlanet, "currfloor");
        }
        if (!g_offFloorNext && g_clsFloorBr)
        {
            g_offFloorNext = FieldOffset(g_clsFloorBr, "nextfloor");
            g_offFloorMargin = FieldOffset(g_clsFloorBr, "marginScale");
        }
        if (!g_offFloorSeq && g_clsFloorBr)
        {
            g_offFloorSeq = FieldOffset(g_clsFloorBr, "seqID");
            g_offFloorHoldLen = FieldOffset(g_clsFloorBr, "holdLength");
            g_offFloorHoldComp = FieldOffset(g_clsFloorBr, "holdCompletion");
        }
        if (g_clsConductor && !g_offCsCrotchet)
            g_offCsCrotchet = FieldOffset(g_clsConductor, "crotchetAtStart");
        if (!g_csInst.valid)
        {
            uint32_t off = FieldOffset(g_clsConductor, "_instance");
            if (off)
                g_csInst = { off, true };
            else if (g_vtConductor)
                g_csInst = DiscoverStaticRef(g_vtConductor, g_clsConductor);
        }
        if (!g_offGcsDiff && g_clsGCS)
        {
            g_offGcsDiff = FieldOffset(g_clsGCS, "difficulty");
            g_offGcsTrial = FieldOffset(g_clsGCS, "currentSpeedTrial");
        }

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
        if (CheatState::GameQuitting.load(std::memory_order_relaxed))
            return;                       // 游戏退出中：不再打扰主线程
        if ((InterlockedOr(&g_taskFlags, bit) & bit) == 0)
            g_postToMainThread();
    }

    void QueueMainThreadInit() { PostTask(kTaskInit); }
    void QueueCheatApply() { PostTask(kTaskCheats); }

    // ---- CATCH 漏音即死：请求重开本关 ----
    static std::atomic<bool> g_restartReq{ false };
    void RequestLevelRestart()
    {
        if (CheatState::GameQuitting.load(std::memory_order_relaxed)) return;
        g_restartReq.store(true, std::memory_order_relaxed);
        PostTask(kTaskCheats);   // 由主线程任务真正调用 scrController.Restart
    }

    // ---- 通用主线程任务队列 ----
    struct MainWork { void (*fn)(void*); void* ctx; };
    static std::mutex g_workMx;
    static std::vector<MainWork> g_work;
    static const long kTaskWork = 4;

    void QueueMainThreadWork(void (*fn)(void*), void* ctx)
    {
        if (!fn) return;
        {
            std::lock_guard<std::mutex> lk(g_workMx);
            g_work.push_back(MainWork{ fn, ctx });
        }
        PostTask(kTaskWork);
    }

    static void ApplyCheatsMainThread();   // 定义见下（开关应用，仅主线程）
    static void RestartLevelMainThread();  // 定义见下（CATCH 漏音即死：重开本关）

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

    static thread_local int t_initDepth = 0;

    // ---- 测试钩子：ADOFAI_PERFECT_QUITTEST=<秒> ----
    //   到点后用游戏自己的 UnityEngine.Application.Quit() 退出，
    //   与点击游戏里的"退出游戏"按钮走完全相同的路径（仅自动化测试用）。
    static void QuitTestTick()
    {
        static int   s_delay = -2;
        static DWORD s_t0 = 0;
        static bool  s_fired = false;
        if (s_fired) return;
        if (s_delay == -2)
        {
            char v[16] = { 0 };
            s_delay = (GetEnvironmentVariableA("ADOFAI_PERFECT_QUITTEST", v, sizeof(v)) > 0) ? atoi(v) : -1;
            s_t0 = GetTickCount();
        }
        if (s_delay <= 0 || GetTickCount() - s_t0 < (DWORD)s_delay * 1000u)
            return;
        s_fired = true;
        MonoImage* img = mono_image_loaded("UnityEngine.CoreModule");
        MonoClass* cls = img ? mono_class_from_name(img, "UnityEngine", "Application") : nullptr;
        MonoMethod* m = cls ? mono_class_get_method_from_name(cls, "Quit", 0) : nullptr;
        if (!m) { Log::Printf("[quit] Application.Quit not found"); return; }
        Log::Printf("[quit] calling Application.Quit()");
        MonoObject* exc = nullptr;
        mono_runtime_invoke(m, nullptr, nullptr, &exc);
        Log::Printf("[quit] Application.Quit() returned exc=%p", (void*)exc);
    }

    // 测试钩子：ADOFAI_PERFECT_TESTLEVEL=<自定义关卡绝对路径> + 延迟秒数后，
    // 由主线程调用 scrController.LoadCustomLevel(path) 自动进入关卡
    // （用于自动化验证冰与火宏/4K 谱面提取；普通运行零开销）
    static std::atomic<bool> g_levelTestDone{ false };
    static void* CtrlInstanceMainThread();   // 定义见下
    // 主菜单直载关卡：复刻 scrController.LoadCustomLevel 的静态赋值 + scrLoader 切场景。
    // 逆向依据（decomp/scrController.cs:2067）：LoadCustomLevel →
    //   GCS.sceneToLoad="scnGame"; GCS.customLevelPaths=new string[1]{path};
    //   GCS.loadCustomFromBundle=fromBundle; StartLoadingScene() →
    //   ADOBase.loader.LoadSceneWithTransition(wipeDirection)。
    // 主菜单（scnLevelSelect）里没有 scrController 实例，故走这条等价路径。
    static bool LoadLevelViaLoaderMainThread(const char* path)
    {
        if (!MonoApi::Ready() || !mono_array_new || !mono_string_new || !mono_runtime_invoke ||
            !mono_image_loaded || !mono_class_from_name || !mono_class_vtable ||
            !mono_field_static_set_value || !mono_array_addr_with_size || !mono_domain_get ||
            !mono_class_get_field_from_name || !mono_class_get_method_from_name)
            return false;
        MonoImage* img = mono_image_loaded("Assembly-CSharp");
        MonoClass* clsGCS = img ? mono_class_from_name(img, "", "GCS") : nullptr;
        MonoClass* clsLoader = img ? mono_class_from_name(img, "", "scrLoader") : nullptr;
        MonoImage* imgCore = mono_image_loaded("mscorlib");
        MonoClass* clsString = imgCore ? mono_class_from_name(imgCore, "System", "String") : nullptr;
        if (!clsGCS || !clsLoader || !clsString)
            return false;
        MonoDomain* dom = mono_domain_get();
        MonoVTable* vtGCS = dom ? mono_class_vtable(dom, clsGCS) : nullptr;
        if (!vtGCS)
            return false;
        MonoClassField* fScene  = mono_class_get_field_from_name(clsGCS, "sceneToLoad");
        MonoClassField* fPaths  = mono_class_get_field_from_name(clsGCS, "customLevelPaths");
        MonoClassField* fBundle = mono_class_get_field_from_name(clsGCS, "loadCustomFromBundle");
        if (!fScene || !fPaths || !fBundle)
            return false;
        MonoString* scene = mono_string_new(dom, "scnGame");
        MonoArray*  paths = mono_array_new(dom, clsString, 1);
        MonoString* level = mono_string_new(dom, path);
        if (!scene || !paths || !level)
            return false;
        *(MonoString**)mono_array_addr_with_size((MonoObject*)paths, (int)sizeof(void*), 0) = level;
        bool fromBundle = false;
        mono_field_static_set_value(vtGCS, fScene, &scene);
        mono_field_static_set_value(vtGCS, fPaths, &paths);
        mono_field_static_set_value(vtGCS, fBundle, &fromBundle);

        MonoMethod* mInst = mono_class_get_method_from_name(clsLoader, "get_instance", 0);
        MonoObject* exc = nullptr;
        MonoObject* loader = mInst ? mono_runtime_invoke(mInst, nullptr, nullptr, &exc) : nullptr;
        if (!loader || exc)
        {
            Log::Printf("[test] level auto-load: scrLoader.instance null (%p exc=%p)", (void*)loader, (void*)exc);
            return false;
        }
        MonoMethod* mLoad = mono_class_get_method_from_name(clsLoader, "LoadSceneWithTransition", 2);
        if (!mLoad)
        {
            Log::Printf("[test] level auto-load: LoadSceneWithTransition not found");
            return false;
        }
        int32_t wipe = 1;   // WipeDirection.StartsFromRight（LoadCustomLevel 的默认值）
        void* args[2] = { &wipe, nullptr };
        exc = nullptr;
        mono_runtime_invoke(mLoad, loader, args, &exc);
        Log::Printf("[test] level auto-load via loader: '%s' exc=%p", path, (void*)exc);
        return !exc;
    }
    static void LevelTestTick()
    {
        static int   s_state = 0;      // 0=初始化 1=等待 2=完成
        static DWORD s_t0 = 0;
        static int   s_delay = 20;
        static char  s_path[MAX_PATH * 2] = { 0 };
        if (s_state >= 2) return;
        if (s_state == 0)
        {
            s_state = 2;
            if (GetEnvironmentVariableA("ADOFAI_PERFECT_TESTLEVEL", s_path, sizeof(s_path)) <= 0 || !s_path[0])
            {
                g_levelTestDone.store(true, std::memory_order_relaxed);
                return;
            }
            char dv[16] = { 0 };
            if (GetEnvironmentVariableA("ADOFAI_PERFECT_TESTDELAY", dv, sizeof(dv)) > 0)
                s_delay = atoi(dv);
            if (s_delay < 2) s_delay = 2;
            s_t0 = GetTickCount();
            s_state = 1;
            Log::Printf("[test] level auto-load armed: '%s' in %ds", s_path, s_delay);
            return;
        }
        if (GetTickCount() - s_t0 < (DWORD)s_delay * 1000u) return;
        if (!g_clsController || !mono_string_new || !mono_runtime_invoke)
        {
            s_state = 2;
            g_levelTestDone.store(true, std::memory_order_relaxed);
            Log::Printf("[test] level auto-load: runtime not ready");
            return;
        }
        void* ctrl = CtrlInstanceMainThread();
        if (!ctrl)
        {
            if (LoadLevelViaLoaderMainThread(s_path))
            {
                s_state = 2;
                g_levelTestDone.store(true, std::memory_order_relaxed);
            }
            else
            {
                // 开场 logo / 运行时未就绪：等下一轮（BridgeLoop 每 2s 投递一次任务）
                static DWORD s_lastWaitLog = 0;
                if (GetTickCount() - s_t0 > 180000u)
                {
                    s_state = 2;
                    g_levelTestDone.store(true, std::memory_order_relaxed);
                    Log::Printf("[test] level auto-load: controller/loader path never ready");
                }
                else if (GetTickCount() - s_lastWaitLog > 10000u)
                {
                    s_lastWaitLog = GetTickCount();
                    Log::Printf("[test] level auto-load: waiting for controller/loader ...");
                }
            }
            return;
        }
        s_state = 2;
        g_levelTestDone.store(true, std::memory_order_relaxed);
        MonoMethod* m = mono_class_get_method_from_name(g_clsController, "LoadCustomLevel", 3);
        if (!m)
        {
            Log::Printf("[test] level auto-load: LoadCustomLevel not found (m=null)");
            return;
        }
        MonoDomain* dom = mono_domain_get ? mono_domain_get() : nullptr;
        MonoString* sp = dom ? mono_string_new(dom, s_path) : nullptr;
        int32_t fromBundle = 0;
        void* args[3] = { sp, nullptr, &fromBundle };
        MonoObject* exc = nullptr;
        mono_runtime_invoke(m, ctrl, args, &exc);
        Log::Printf("[test] level auto-load invoked: '%s' exc=%p", s_path, (void*)exc);
    }
    struct InitDepthScope { InitDepthScope() { ++t_initDepth; } ~InitDepthScope() { --t_initDepth; } };

    // 真正的主线程工作（含 C++ 对象，不能直接放进 __try）
    static void MainThreadInitTaskInner()
    {
        InitDepthScope depthScope;
        long flags = InterlockedExchange(&g_taskFlags, 0);
        static DWORD s_lastInitLog = 0;
        if ((flags & kTaskInit) && GetTickCount() - s_lastInitLog > 60000)
        {
            s_lastInitLog = GetTickCount();
            Log::Printf("[Bridge] main-thread init task running tid=%lu depth=%d",
                        (unsigned long)GetCurrentThreadId(), t_initDepth);
        }
        InitCore(true);                 // 元数据解析（主线程 = mono 托管线程）
        QuitTestTick();
        LevelTestTick();
        // 退出钩子：mono_compile_method 必须在托管主线程调用（协作式 Mono 约束），
        // 幂等；装好后此调用立即返回。
        GameDetour::TickQuitHookOnMainThread();
        // 冰与火宏：scrPlayer 判定入口钩子（同样受 mono_compile_method 主线程约束）
        GameDetour::TickInputHookOnMainThread();
        RefreshControllerInstanceMainThread();
        Chart4K::MainThreadResolve();   // 4K 辅助：主线程安全创建 vtable / 解析偏移
        // 只要队列里有工作就跑（不再依赖 kTaskWork 标志）：
        //   某些 MOD 的 Load/OnToggle 会弹出 Win32 模态对话框（如 Creplay 的
        //   System.Windows.Forms.MessageBox）。模态循环仍会 DispatchMessage 本线程所有窗口的消息，
        //   于是 kMsgBridgeInit -> MainThreadInitTask 会被重入；此时若坚持看 kTaskWork 标志，
        //   排在后面的 MOD 就永远加载不到。无条件排空队列后，后续 MOD 在重入帧里继续加载。
        // 逐条"取出即执行"（而不是一次性 swap 到局部数组）：
        //   Creplay 这类 MOD 会在 Load/OnToggle 里弹 Win32 模态对话框，模态循环仍会
        //   DispatchMessage 本线程的消息 -> kMsgBridgeInit -> 本函数被重入（depth=2）。
        //   任务保留在共享队列里，重入帧才能继续把后面的 MOD 加载完（官方 UMM 在这里会整体卡死）。
        for (;;)
        {
            MainWork job;
            {
                std::lock_guard<std::mutex> lk(g_workMx);
                if (g_work.empty()) break;
                job = g_work.front();
                g_work.erase(g_work.begin());
            }
            if (job.fn) job.fn(job.ctx);
        }
        if ((flags & kTaskCheats) || CheatState::NoDeath.load(std::memory_order_relaxed) ||
            CheatState::AutoCombo.load(std::memory_order_relaxed))
            ApplyCheatsMainThread();
    }

    void MainThreadInitTask()
    {
        // 卸载路径的摘钩请求优先处理：即使游戏已进入退出流程也必须执行，
        // 否则 FreeLibrary 后残留的 detour 会让下一次 Application.Quit 跳到已卸载内存。
        if (GameDetour::QuitHookDetachRequested())
        {
            GameDetour::TickQuitHookOnMainThread();   // 只做 DetourDetach，不调用 mono
            return;
        }
        if (GameDetour::InputHookDetachRequested())
        {
            GameDetour::TickInputHookOnMainThread();  // 只做 DetourDetach，不调用 mono
            return;
        }
        if (CheatState::GameQuitting.load(std::memory_order_relaxed))
            return;                       // 退出流程里不再调用 Mono（否则可能与运行时锁死锁）
        // 游戏已经不出帧（退出中 / 设备丢失 / 长时间挂起）：本帧不执行 Mono 工作。
        // 这是"点退出游戏卡死"的第二道保险 —— 关停中的 Mono 运行时一旦被外部调用就可能死锁。
        {
            const unsigned long last = CheatState::LastPresentMs.load(std::memory_order_relaxed);
            if (last != 0 && (GetTickCount() - last) > 4000ul)
                return;
        }
        __try
        {
            MainThreadInitTaskInner();
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                      ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            // 一次瞬时 AV（读到刚被销毁的托管对象很常见）不应让整个工具永久失效 ——
            // 此前这里直接置 GameQuitting，表现为"某次进关卡后不死/自动连打再也回不来"。
            // 现在只计数限流，连续大量 AV 才判定环境不可用。
            static volatile long s_avCount = 0;
            long n = InterlockedIncrement(&s_avCount);
            if (n <= 5 || (n % 200) == 0)
                Log::Printf("[Bridge] AV in MainThreadInitTask (#%ld)", n);
            if (n > 400)
            {
                Log::Printf("[Bridge] too many AVs -> tool dormant");
                CheatState::GameQuitting.store(true, std::memory_order_relaxed);
            }
        }
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
        void* obj = nullptr;
        // 优先使用主线程 get_instance() 发布的实例：
        // 切换关卡时静态槽 _instance 可能仍指向已销毁的旧 scrController，
        // 直接写它的 noFail 等于写空气（表现为"不死模式有时候不生效"）。
        if (g_ctrlFreshPub.load(std::memory_order_acquire))
        {
            obj = g_ctrlFresh.load(std::memory_order_acquire);
            if (!obj) return nullptr;      // 游戏明确表示当前没有实例
        }
        if (!obj && g_fieldInstance)
            mono_field_static_get_value(g_vtController, g_fieldInstance, &obj);
        if (!obj)
        {
            // _instance 只有被 instance 属性访问过才有值；直接调 getter（内部 FindAnyObjectByType）
            static MonoMethod* mInst = nullptr;
            if (!mInst)
                mInst = mono_class_get_method_from_name(g_clsController, "get_instance", 0);
            if (mInst)
            {
                MonoObject* exc = nullptr;
                MonoObject* o = mono_runtime_invoke(mInst, nullptr, nullptr, &exc);
                if (!exc && o)
                    obj = o;
            }
        }
        return obj;
    }

    static void ApplyCheatsMainThread()
    {
        if (!MonoApi::Ready() || !g_clsController)
            return;

        bool wantNoFail = CheatState::NoDeath.load(std::memory_order_relaxed);
        bool wantAuto = CheatState::AutoCombo.load(std::memory_order_relaxed);

        // 冰与火宏 / CATCH 打歌引擎：宏要自己按键打歌 ⇒ 必须抑制游戏内置自动演奏
        // （RDC.auto）；否则 Auto 会满判，CATCH 判什么背景就体现不出来。
        // 注意：**不再**替用户强开不死 —— 正常打歌即可，宏的拟人失误照原生判定处理，
        // （CATCH 判 MISS 时背景原关也确实会 TooEarly/TooLate → 死亡/重开）。
        if (Chart4K::FireMacroEnabled() || Chart4K::CatchAssistEnabled())
            wantAuto = false;

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

        RestartLevelMainThread();   // CATCH 漏音即死：处理"重开本关"请求

        AcquireSRWLockExclusive(&g_statusLock);
        g_status.noFailApplied = g_cheatAppliedNoFail;
        g_status.autoApplied = g_cheatAppliedAuto;
        ReleaseSRWLockExclusive(&g_statusLock);
    }

    // CATCH 漏音即死：在主线程调用 scrController.Restart(false) 重开本关。
    // 只在主线程任务里执行（mono_runtime_invoke 必须托管主线程）。
    static void RestartLevelMainThread()
    {
        if (!g_restartReq.exchange(false, std::memory_order_relaxed)) return;
        if (!MonoApi::Ready() || !g_clsController || !mono_runtime_invoke)
        {
            Log::Printf("[CATCH] restart skipped (mono not ready)");
            return;
        }
        static MonoMethod* mRestart = nullptr;
        if (!mRestart)
            mRestart = mono_class_get_method_from_name(g_clsController, "Restart", 1);
        void* ctrl = CtrlInstanceMainThread();
        if (!ctrl || !mRestart)
        {
            Log::Printf("[CATCH] restart unavailable (ctrl=%p m=%p)", ctrl, (void*)mRestart);
            return;
        }
        bool fromBeginning = false;
        void* args[1] = { &fromBeginning };
        MonoObject* exc = nullptr;
        mono_runtime_invoke(mRestart, ctrl, args, &exc);
        Log::Printf("[CATCH] Restart(false) invoked exc=%p", (void*)exc);
    }

    // 游戏主线程每帧钩子调用（节流）：把开关重应用间隔从 ~1s 压到 ~100ms。
    // 为什么需要：scrController.Awake 每关都把 noFail 重置为 GCS.useNoFail、
    // 把 RDC.auto 重置为 false；只在 1s 一次的主线程任务里应用，会在"进关卡 /
    // 死亡重开"的头一段时间里失效（不死被打死、自动连打前几拍不触发）。
    void TickCheatsFast()
    {
        static DWORD s_last = 0;
        DWORD now = GetTickCount();
        if (now - s_last < 100) return;
        s_last = now;
        if (CheatState::GameQuitting.load(std::memory_order_relaxed)) return;
        if (!CheatState::NoDeath.load(std::memory_order_relaxed) &&
            !CheatState::AutoCombo.load(std::memory_order_relaxed) &&
            !Chart4K::FireMacroEnabled() &&
            !Chart4K::AnyAssistEnabled())
            return;
        ApplyCheatsMainThread();
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

        // ---- 判定窗口参数（慢循环）：GCS 难度 / 试炼倍率 / 下一块 marginScale ----
        //   scrMisc.GetAdjustedAngleBoundaryInDeg 依赖这三个值 + bpm*speed + pitch；
        //   marginScale 取自 currfloor.nextfloor.marginScale（scrPlanet.SwitchChosen 同源）。
        if (g_vtGCS && g_offGcsDiff)
            st.difficulty = ReadStatic<int>(g_vtGCS, g_offGcsDiff, 1);
        if (g_vtGCS && g_offGcsTrial)
            st.speedTrial = ReadStatic<float>(g_vtGCS, g_offGcsTrial, 1.f);
        if (player && g_offPlayerPS && g_offPSChosen && g_offPlanetFloor &&
            g_offFloorNext && g_offFloorMargin)
        {
            void* ps = ReadInst<void*>(player, g_offPlayerPS, nullptr);
            void* planet = ps ? ReadInst<void*>(ps, g_offPSChosen, nullptr) : nullptr;
            if (planet)
            {
                void* cf = ReadInst<void*>(planet, g_offPlanetFloor, nullptr);
                void* nf = cf ? ReadInst<void*>(cf, g_offFloorNext, nullptr) : nullptr;
                if (nf)
                    st.marginScale = ReadInst<double>(nf, g_offFloorMargin, 1.0);
            }
        }
        PublishStatus(st);
    }

    // ============================================================
    // 实时判定角度快循环（7ms）：player → planetarySystem → chosenPlanet
    //   scrPlanet.angle / <targetExitAngle>k__BackingField 均为 double（弧度），
    //   PlanetarySystem.speed / isCW 为速度倍率与旋转方向（均逆向确认的字段）。
    //   误差采用 scrMisc.GetHitMargin 的约定：
    //     err = (angle - targetExitAngle) * (isCW ? 1 : -1)
    //   换算成度后：负 = 早（星球还没转到目标角），正 = 晚（已经越过）。
    // ============================================================
    static std::atomic<bool>   s_plOk{ false };
    static std::atomic<double> s_plErrDeg{ 0.0 };
    static std::atomic<double> s_plSpeed{ 1.0 };
    static DWORD               s_plFaultUntil = 0;

    static void ReadPlanetFastInner()
    {
        if (!g_offPlayerPS || !g_offPAngle)
        {
            s_plOk.store(false, std::memory_order_relaxed);
            return;
        }
        void* mgr = g_mgrInst.valid
                        ? ReadStatic<void*>(g_vtPlayerMgr, g_mgrInst.offset, nullptr)
                        : nullptr;
        void* playersArr = mgr ? ReadInst<void*>(mgr, g_offMgrPlayers, nullptr) : nullptr;
        void* player = ArrayElement(playersArr, 0);
        void* ps = player ? ReadInst<void*>(player, g_offPlayerPS, nullptr) : nullptr;
        void* planet = (ps && g_offPSChosen) ? ReadInst<void*>(ps, g_offPSChosen, nullptr) : nullptr;
        if (!planet)
        {
            s_plOk.store(false, std::memory_order_relaxed);
            return;
        }
        double angle  = ReadInst<double>(planet, g_offPAngle, 0.0);
        double target = g_offPTarget ? ReadInst<double>(planet, g_offPTarget, 0.0) : angle;
        bool   cw     = (ps && g_offPSCw) ? ReadInst<bool>(ps, g_offPSCw, true) : true;
        double speed  = (ps && g_offPSSpeed) ? ReadInst<double>(ps, g_offPSSpeed, 1.0) : 1.0;
        double err = (angle - target) * (cw ? 1.0 : -1.0);
        const double kPi = 3.14159265358979323846;
        static bool s_plLogged = false;
        if (!s_plLogged)
        {
            s_plLogged = true;
            Log::Printf("[Bridge] planet live: angle=%.3f target=%.3f errDeg=%.1f cw=%d speed=%.2f",
                        angle, target, err * 180.0 / kPi, (int)cw, speed);
        }
        s_plErrDeg.store(err * 180.0 / kPi, std::memory_order_relaxed);
        s_plSpeed.store((speed > 0.02 && speed < 100.0) ? speed : 1.0, std::memory_order_relaxed);
        s_plOk.store(true, std::memory_order_relaxed);
    }

    static void ReadPlanetFastSafe()
    {
        DWORD now = GetTickCount();
        if (s_plFaultUntil && now < s_plFaultUntil)
            return;
        __try
        {
            ReadPlanetFastInner();
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                      ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            s_plFaultUntil = now + 500;   // 切场景瞬间对象销毁：短退避后自动重试
            s_plOk.store(false, std::memory_order_relaxed);
        }
    }

    void GetPlanetLive(PlanetLive* out)
    {
        if (!out)
            return;
        out->ok     = s_plOk.load(std::memory_order_relaxed);
        out->errDeg = s_plErrDeg.load(std::memory_order_relaxed);
        out->speed  = s_plSpeed.load(std::memory_order_relaxed);
    }

    // ============================================================
    // 冰与火宏模式：判定现场同步读取（游戏主线程输入钩子每帧调用）
    //   读取链 = ReadPlanetFastInner + scrController(gameworld/paused/state)
    //            + scrPlanet.currfloor(seqID / holdLength / holdCompletion)
    //   与慢循环不同，这里每次都读最新值；调用点就是游戏本帧判定输入的位置。
    // ============================================================
    static void ReadFireCtxInner(FireCtx* out)
    {
        out->ok = false;
        if (!g_offPlayerPS || !g_offPAngle)
            return;

        void* mgr = g_mgrInst.valid
                        ? ReadStatic<void*>(g_vtPlayerMgr, g_mgrInst.offset, nullptr)
                        : nullptr;
        void* playersArr = mgr ? ReadInst<void*>(mgr, g_offMgrPlayers, nullptr) : nullptr;
        void* player = ArrayElement(playersArr, 0);
        out->player = player;
        void* ps = player ? ReadInst<void*>(player, g_offPlayerPS, nullptr) : nullptr;
        void* planet = (ps && g_offPSChosen) ? ReadInst<void*>(ps, g_offPSChosen, nullptr) : nullptr;

        void* ctrl = g_ctrlFreshPub.load(std::memory_order_acquire)
                         ? g_ctrlFresh.load(std::memory_order_acquire)
                         : (g_ctrlInst.valid
                                ? ReadStatic<void*>(g_vtController, g_ctrlInst.offset, nullptr)
                                : nullptr);
        if (ctrl)
        {
            if (g_offGameworld)    out->gameworld = ReadInst<bool>(ctrl, g_offGameworld, false);
            if (g_offPaused)       out->paused    = ReadInst<bool>(ctrl, g_offPaused, false);
            if (g_offCurrentState)
            {
                out->state     = ReadInst<int>(ctrl, g_offCurrentState, -1);
                out->inControl = (out->state == 4);      // States.PlayerControl
            }
        }
        else if (g_offCurrentState)
        {
            out->inControl = false;
        }

        double speed = (ps && g_offPSSpeed) ? ReadInst<double>(ps, g_offPSSpeed, 1.0) : 1.0;
        out->speed = (speed > 0.02 && speed < 100.0) ? speed : 1.0;

        // 每秒 180°（π/拍，与 scrPlanet.Update_RefreshAngles 的角度公式同源）：
        //   angle = snappedLastAngle + (songpos - player.lastHit)/crotchet * π * speed * dir
        // 由此 1ms 对应的角度 = 180*speed/(crotchet*1000) 度。
        if (g_csInst.valid && g_offCsCrotchet)
        {
            void* cs = ReadStatic<void*>(g_vtConductor, g_csInst.offset, nullptr);
            double crotchet = cs ? ReadInst<double>(cs, g_offCsCrotchet, 0.0) : 0.0;
            if (crotchet > 0.0005 && crotchet < 60.0)
                out->crotchet = crotchet;
        }

        if (!planet)
            return;
        double angle  = ReadInst<double>(planet, g_offPAngle, 0.0);
        double target = g_offPTarget ? ReadInst<double>(planet, g_offPTarget, angle) : angle;
        bool   cw     = (ps && g_offPSCw) ? ReadInst<bool>(ps, g_offPSCw, true) : true;
        const double kPi = 3.14159265358979323846;
        out->errDeg = (angle - target) * (cw ? 1.0 : -1.0) * 180.0 / kPi;

        void* floor = g_offPlanetFloor ? ReadInst<void*>(planet, g_offPlanetFloor, nullptr) : nullptr;
        if (floor)
        {
            if (g_offFloorSeq)      out->floorIndex = ReadInst<int>(floor, g_offFloorSeq, -1);
            if (g_offFloorHoldLen)  out->holdLength = ReadInst<int>(floor, g_offFloorHoldLen, -1);
            if (g_offFloorHoldComp) out->holdCompletion = (double)ReadInst<float>(floor, g_offFloorHoldComp, 0.f);
            void* nf = g_offFloorNext ? ReadInst<void*>(floor, g_offFloorNext, nullptr) : nullptr;
            out->hasNext = (nf != nullptr);
            if (nf && g_offFloorMargin)
            {
                double ms = ReadInst<double>(nf, g_offFloorMargin, 1.0);
                out->marginScale = (ms > 0.01 && ms < 100.0) ? ms : 1.0;
            }
        }
        out->ok = true;
    }

    bool ReadFireCtx(FireCtx* out)
    {
        if (!out)
            return false;
        *out = FireCtx{};
        DWORD now = GetTickCount();
        if (s_plFaultUntil && now < s_plFaultUntil)
            return false;
        __try
        {
            ReadFireCtxInner(out);
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                      ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            out->ok = false;
        }
        return out->ok;
    }

    // 「Press to start」等待输入：宏模式下自动置 levelWasSkipped（游戏 debug 的
    // BeatLevel 也只置这一个标记 + 结算；这里只置标记，不结算关卡）。
    // 逆向依据：scrController.WaitForStartCo() 的
    //   while (!levelWasSkipped && (!AnyValidInputWasTriggered() || isCutscene)) yield return null;
    // 置位后协程立刻进入 ShowGetReady → conductor.Start() → Start_Rewind()。
    bool SetLevelWasSkipped(bool on)
    {
        if (!g_offLevelSkipped)
            return false;
        __try
        {
            void* ctrl = g_ctrlFreshPub.load(std::memory_order_acquire)
                             ? g_ctrlFresh.load(std::memory_order_acquire)
                             : (g_ctrlInst.valid
                                    ? ReadStatic<void*>(g_vtController, g_ctrlInst.offset, nullptr)
                                    : nullptr);
            if (!ctrl)
                return false;
            *reinterpret_cast<bool*>(reinterpret_cast<char*>(ctrl) + g_offLevelSkipped) = on;
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                      ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
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
            if (CheatState::GameQuitting.load(std::memory_order_relaxed))
            {
                // 游戏退出中：不再读游戏状态 / 不再投递主线程任务，安静待着
                Sleep(50);
                continue;
            }
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
                // 实时判定角度（读谱辅助的角度判定条，7ms 刷新）
                if (!g_monoDisabled)
                    ReadPlanetFastSafe();
                // 就绪后继续补齐 gameplay 引用（playerMgr / marginTrackers
                // 需要游戏实际创建这些单例后才能发现）
                if (!g_mgrInst.valid || !g_trackerArr.valid || !g_offTrackerAcc || !g_offMgrPlayers)
                {
                    static int s_slow = 0;
                    if ((s_slow++ % 60) == 0) // ~0.5s 一次
                        QueueMainThreadInit();
                }
                // 退出钩子尚未装好时周期性重投（幂等，装好后零开销）
                if (!GameDetour::QuitHookSettled() && (tickCount % 140) == 0)
                    QueueMainThreadInit();
                if (!g_monoDisabled && (tickCount % 10) == 0)
                    ReadStatusSafe();
            }
            // 开关应用（主线程执行；开启期间周期性重投，因为关卡加载会重置）
            if ((CheatState::NoDeath.load(std::memory_order_relaxed) ||
                 CheatState::AutoCombo.load(std::memory_order_relaxed)) && (tickCount % 30) == 0)
                QueueCheatApply();
            // 冰与火宏模式：输入钩子的安装/摘除只能在游戏主线程（mono_compile_method 约束）
            if (!GameDetour::InputHookSettled() && (tickCount % 20) == 0)
                QueueMainThreadInit();
            // 测试钩子：待自动载入关卡时周期投递主线程任务
            if (!g_levelTestDone.load(std::memory_order_relaxed) && (tickCount % 60) == 0)
                QueueMainThreadInit();
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
