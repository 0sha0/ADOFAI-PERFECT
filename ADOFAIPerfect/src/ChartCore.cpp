#include "Chart4K.h"
#include "ChartCore.h"
#include "AdoGen.h"
#include "GameRecorder.h"
#include "CheatState.h"
#include "MonoApi.h"
#include "GameBridge.h"
#include "Lang.h"
#include "Log.h"
#include "RenderHook.h"
#include "SkinMsp.h"
#include "SkinLua.h"

#include "imgui.h"
#include "imgui_internal.h"
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <vector>
#include <mutex>
#include <string>
#include <algorithm>
#include <random>
#include <memory>
#include <initializer_list>

// RenderHook 提供的统一贴图加载（DX11 / D3D12 自适应）
void* RenderHook_LoadTexture(const char* path);

// ============================================================
// Chart4K — 冰与火谱面 → Malody 式 4K（DFJK）下坠谱
//
// 全部数据字段都由 Assembly-CSharp.dll 反编译确认（见 .reverse/）：
//   scrLevelMaker._instance.listFloors : List<scrFloor>
//   scrFloor.entryTime      每块的绝对命中时刻（song 秒；BPM/变速/暂停/长按
//                           已由 CalculateFloorEntryTimes 折算进去）
//   scrFloor.angleLength    该块旋转的绝对角度（弧度 0..2π，长按块额外含
//                           holdLength*2π）—— 见 CalculateSingleFloorAngleLength
//   scrFloor.isCCW          该块方向（true = 逆时针）
//   scrFloor.tapsNeeded     多次点击块；holdLength>0 = 长按块
//   scrConductor._songposition_minusi  当前歌曲位置（与 entryTime 同单位）
//     = (dspTime - dspTimeSong - calibration_i) * song.pitch - addoffset
//   scrConductor.bpm        当前 BPM
//   命中统计来自 scrMarginTracker.percentAcc / hitMargins / hitMarginsCount
//
// 线程模型：mono API 只在游戏主线程（MainThreadResolve）调用；桥接线程
// 只做"缓存偏移 + 纯指针读取"，并用 SEH 兜底。
// ============================================================
namespace Chart4K
{
    using namespace MonoApi;

    // ---------------- 用户设置（按键位模式分组：0=4K 1=5K 2=6K 3=10K） ----------------
    const int kLanesOf[kModeN] = { 4, 5, 6, 10 };   // 定义（声明见 ChartCore.h）
    // 转换风格：经典 = 原每模式引擎；叠/技/乱/切 = 四套独立算法
    enum ConvStyle { kStyleClassic = 0, kStyleJack = 1, kStyleTech = 2, kStyleRandom = 3, kStyleTrill = 4,
                     kStyleAdo = 5 };   // 冰火手法（ADOFAI）：轮指(混F/插J/插DF) / 交互 / 押轮拆手
    static const char* kStyleNames[6] = { "\u7ecf\u5178", "\u53e0", "\u6280", "\u4e71", "\u5207",
                                          "\u51b0\u706b\u624b\u6cd5" };
    const char* ModeDesc(int mi)
    {
        switch (mi)
        {
        case 0:  return ModeDesc4K();
        case 1:  return ModeDesc5K();
        case 2:  return ModeDesc6K();
        default: return ModeDesc10K();
        }
    }
    // 默认键位：4K=DFJK ／ 5K=SDFJK ／ 6K=SDFJKL ／ 10K=ASDFG + HJKL;（菜单可改，确认才生效）
    static const int kVKDefs[kModeN][10] = {
        { 'D', 'F', 'J', 'K', 0, 0, 0, 0, 0, 0 },
        { 'S', 'D', 'F', 'J', 'K', 0, 0, 0, 0, 0 },
        { 'S', 'D', 'F', 'J', 'K', 'L', 0, 0, 0, 0 },
        { 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xBA },   // 0xBA = ';'
    };
    static std::atomic<bool> s_en[kModeN] = { { false }, { false }, { false }, { false } };  // 四模式互斥
    static std::atomic<int>  s_speed[kModeN]    = { { 4 }, { 5 }, { 6 }, { 7 } };   // Malody 式流速
    static std::atomic<int>  s_offsetMS[kModeN] = { { 0 }, { 0 }, { 0 }, { 0 } };   // 额外延迟补偿
    static std::atomic<int>  s_style[kModeN]    = { { 0 }, { 0 }, { 0 }, { 0 } };   // 转换风格（ConvStyle）   // 转换风格（ConvStyle）
    static std::atomic<bool> s_upHide[kModeN]   = { { false }, { false }, { false }, { false } };  // 上隐
    static std::atomic<bool> s_dnHide[kModeN]   = { { false }, { false }, { false }, { false } };  // 下隐
    static std::atomic<bool> s_autoOff[kModeN]  = { { false }, { false }, { false }, { false } };  // 自动调整延迟
    // 自动打歌 / 宏打歌 / 录制（内部虚拟按键与 GPU 回读；绝不注入系统键鼠、绝不改游戏内存）
    static std::atomic<bool> s_autoPlay[kModeN] = { { false }, { false }, { false }, { false } };  // 自动打歌（100% 精准）
    static std::atomic<bool> s_macroPlay[kModeN]= { { false }, { false }, { false }, { false } };  // 宏打歌（拟人化）
    static std::atomic<int>  s_macroAcc{ 98 };     // 宏打歌目标精准度（90..100）
    static std::atomic<int>  s_macroHuman{ 60 };   // 拟人抖动强度（0..100）
    static std::atomic<bool> s_recOn{ false };     // 录制开关
    static std::atomic<bool> s_recAuto{ true };    // 录制跟随游戏自动开始/停止
    static std::atomic<bool> s_recPaused{ false }; // 录制暂停（结束当前分段，恢复时写新文件）
    static std::atomic<int>  s_recFps{ 60 };       // 录制帧率
    static std::atomic<int>  s_recMbps{ 20 };      // 录制码率（Mbps）
    static char s_recDir[MAX_PATH * 2] = { 0 };    // 录制输出目录（默认 <DLL目录>\records）
    static void RecCfgLoadOnce();

    // 冰与火宏模式（原生关卡内按键代打）
    //   逆向证据：scrController.ProcessKeyInputs → scrPlayer.Simulated_PlayerControl_Update
    //   → HitAutoFloors → ValidInputWasTriggered/CountValidKeysPressed → scrPlayer.Hit()
    //   判定用 scrPlanet.cachedAngle 与 targetExitAngle 的角差（同 scrMisc.GetHitMargin）。
    //   引擎在 GameDetour 的输入钩子里每帧跑一次（游戏主线程），只写"本工具的虚拟键"，
    //   不注入系统输入、不改游戏内存。
    static std::atomic<bool> s_macroFire{ false };      // 参与模式：冰与火（原生关卡）
    static std::atomic<bool> s_fireDown{ false };       // 本帧虚拟键按下（GetKeyDown）
    static std::atomic<bool> s_fireHeld{ false };       // 虚拟键保持（GetKey）
    static std::atomic<bool> s_fireUp{ false };         // 本帧虚拟键抬起（GetKeyUp）
    static std::atomic<int>   s_opacity{ 242 };   // 谱面底板不透明度
    static std::atomic<bool>  s_hitFx{ true };    // 打击特效（爆发 + 判定闪条）
    static std::atomic<bool>  s_judgePop{ true }; // 中央判定提示（MARV/PERFECT/GOOD/MISS）
    // 左下角"冰与火之舞"画面缩略图（1080p 基准坐标：x/y/w/h 随分辨率等比缩放）
    static std::atomic<bool>  s_miniOn{ true };
    static std::atomic<int>   s_miniX{ 16 }, s_miniY{ 848 };
    static std::atomic<int>   s_miniW{ 384 }, s_miniH{ 216 };

    // 多押砖（tapsNeeded>1）固定按"自动"处理：平均间隔 >= 75ms 完整展开，
    // 更密的（>13 NPS，人手按不出来）合并为 1 个。

    // 键位（每模式独立，菜单里可改，确认后才生效）
    static std::atomic<int> s_vk[kModeN][10] = {
        { { 'D' }, { 'F' }, { 'J' }, { 'K' } },
        { { 'S' }, { 'D' }, { 'F' }, { 'J' }, { 'K' } },
        { { 'S' }, { 'D' }, { 'F' }, { 'J' }, { 'K' }, { 'L' } },
        { { 'A' }, { 'S' }, { 'D' }, { 'F' }, { 'G' }, { 'H' }, { 'J' }, { 'K' }, { 'L' }, { 0xBA } },
    };
    static int  s_draft[kModeN][10] = {
        { 'D', 'F', 'J', 'K', 0, 0, 0, 0, 0, 0 },
        { 'S', 'D', 'F', 'J', 'K', 0, 0, 0, 0, 0 },
        { 'S', 'D', 'F', 'J', 'K', 'L', 0, 0, 0, 0 },
        { 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xBA },
    };
    static bool s_keyEdit[kModeN] = { false, false, false, false };  // 是否处于编辑（未确认）状态
    static int  s_keyCap[kModeN]  = { -1, -1, -1, -1 };              // 正在等待新键的槽位
    static bool s_keyInit[kModeN] = { false, false, false, false };
    static void KeyName(int vk, char* out, size_t n);   // 定义在下方（键位编辑）

    // 当前启用的键位模式（-1 = 全部关闭）
    static int ActiveModeIndex()
    {
        for (int i = 0; i < kModeN; i++)
            if (s_en[i].load(std::memory_order_relaxed))
                return i;
        return -1;
    }

    // ---------------- mono 元数据 ----------------
    static MonoImage* g_img = nullptr;
    static MonoClass* g_clsFloor = nullptr;
    static MonoClass* g_clsConductor = nullptr;
    static MonoClass* g_clsLevelMaker = nullptr;
    static MonoClass* g_clsController = nullptr;
    static MonoVTable* g_vtFloor = nullptr;
    static MonoVTable* g_vtConductor = nullptr;
    static MonoVTable* g_vtLevelMaker = nullptr;
    static bool s_vtResolved = false;

    static uint32_t oF_entryTime = 0, oF_entryTimePitch = 0, oF_angleLen = 0, oF_isCCW = 0;
    static uint32_t oF_midSpin = 0, oF_isFake = 0, oF_auto = 0, oF_hold = 0;
    static uint32_t oF_next = 0, oF_prev = 0, oF_taps = 0;
    static uint32_t oLM_floors = 0;
    static uint32_t oC_songPos = 0, oC_dspTime = 0, oC_dspTimeSong = 0, oC_bpm = 0;
    static uint32_t oC_songStarted = 0;      // scrConductor.hasSongStarted
    static uint32_t oCtrl_firstFloor = 0;

    static bool  g_offsetsOK = false;
    static void* g_lm = nullptr;      // scrLevelMaker._instance
    static void* g_cond = nullptr;    // scrConductor._instance
    bool  g_clockSongUnits = true;
    static bool  s_metaReady = false;      // 字段偏移已在主线程解析完成
    static uint32_t s_lmInstOff = 0;       // scrLevelMaker._instance 在静态数据区的偏移
    static bool     s_lmInstOK = false;
    static uint32_t s_condInstOff = 0;     // scrConductor._instance 在静态数据区的偏移
    static bool     s_condInstOK = false;
    // 主线程用托管 get_instance() 刷新的实例指针（桥接线程优先使用）。
    // 原因：游戏静态槽 _instance 可能是"已销毁的 Unity 对象"（fake-null），
    // 此时直接读槽会拿到失效指针 → 谱面读取偶发失败；get_instance 会自己重新
    // FindFirstObjectByType 修复。自定义关卡 / 额外关卡 / 编辑器关卡切换时尤其明显。
    static std::atomic<void*> s_lmInstMain{ nullptr };
    static std::atomic<void*> s_condInstMain{ nullptr };
    static std::atomic<void*> s_ctrlInstMain{ nullptr };
    // 主线程 get_instance 是否已成功发布过：发布后一律以它为准（即使为 null），
    // 不再回退读静态槽 —— 槽位可能仍指着已销毁的旧实例（切自定义/额外关卡
    // 后若继续读旧实例，会拿到上一张谱面甚至失效内存）
    static std::atomic<bool>  s_lmInstPub{ false };
    static std::atomic<bool>  s_condInstPub{ false };

    // ---------------- 歌曲 / 谱面状态 ----------------
    std::atomic<double> s_songTime{ 0.0 };
    std::atomic<float>  s_bpm{ 0.f };
    static std::atomic<bool>   s_inLevel{ false };
    std::atomic<bool>   s_songStarted{ false };   // song really started (state is unreliable in custom levels)
    std::atomic<double> s_clockMovedWall{ 0.0 };  // last wall time the song clock was seen moving
    std::atomic<bool>   s_anchored{ false };      // level start seen (clock re-anchored / restarted)
    static std::atomic<int>    s_level{ 0 };
    static std::atomic<int>    s_combo{ 0 };
    static std::atomic<int>    s_maxCombo{ 0 };
    static std::atomic<float>  s_acc{ 0.f };
    static std::atomic<int>    s_noteCount{ 0 };
    static std::atomic<bool>   s_resetKeys{ false };
    static std::atomic<int>    s_keyPressCount[kMaxLanes] = {};

    struct Note
    {
        double time = 0;     // 命中时刻（song 秒）
        int    lane = 0;     // 4K: 0=D 1=F 2=J 3=K ／ 6K: 0=S 1=D 2=F 3=J 4=K 5=L
        float  hold = 0.f;   // >0 = 长按（秒）
        int    dir = 0;      // -1 CCW / +1 CW (debug)
        float  turn = 0.f;   // turn angle (rad, debug)
    };
    static std::mutex s_notesMutex;
    static std::vector<Note> s_notes;
    static void* s_chartKey = nullptr;   // 谱面识别键（List._items 指针）
    static int   s_chartFloorN = 0;      // 谱面识别键（地砖数：防止指针地址复用误判为同谱）
    static int   s_chartSize = 0;
    static std::atomic<double> s_chartEndT{ 0.0 };   // last note time + slack (fallback gate)
    static double s_lastSongT = -1e9;
    static char  s_diag[192] = { 0 };

    // ---------------- 4K 自算成绩（玩家 DFJK 判定） ----------------
    // 判定档：0=Marvelous 1=Perfect 2=Good 3=Miss；acc 权重 1 / 0.9 / 0.5 / 0
    static std::atomic<int>    s_jdCounts[4] = { {0}, {0}, {0}, {0} };
    // Cynosure（cynosure.lua 238-267）金判计数：hitcount1=MV 只统计 |offset|<=20ms 的命中
    static std::atomic<int>    s_jdGold[4] = { {0}, {0}, {0}, {0} };
    static std::atomic<int>    s_jdCombo{ 0 };
    static std::atomic<int>    s_jdMaxCombo{ 0 };
    static std::atomic<int>    s_jdTotal{ 0 };
    static std::atomic<double> s_jdWeight{ 0.0 };
    static std::atomic<int>    s_jdLastKind{ -1 };
    static std::atomic<double> s_jdLastTime{ -100.0 };
    static std::atomic<int>    s_jdLastLane{ 0 };
    static std::atomic<double> s_jdLastOff{ 0.0 };   // 最近一次判定偏差（秒；正=按早，负=按晚）
    static int s_autoCalN = 0;                       // 自动调延迟：命中计数（渲染线程）

    // 最近 32 次命中的偏差环形缓冲（正=按早）：平均偏差 → 一键校准判定中心
    static constexpr int kBiasN = 32;
    static std::mutex s_biasMutex;
    static double     s_biasRing[kBiasN] = {};
    static int        s_biasCount = 0, s_biasHead = 0;
    static void BiasPush(double dt)
    {
        std::lock_guard<std::mutex> lk(s_biasMutex);
        s_biasRing[s_biasHead] = dt;
        s_biasHead = (s_biasHead + 1) % kBiasN;
        if (s_biasCount < kBiasN) s_biasCount++;
    }
    static void BiasClear()
    {
        std::lock_guard<std::mutex> lk(s_biasMutex);
        s_biasCount = 0; s_biasHead = 0;
    }
    static double BiasAvgMs(int* nOut = nullptr)
    {
        std::lock_guard<std::mutex> lk(s_biasMutex);
        if (nOut) *nOut = s_biasCount;
        if (s_biasCount <= 0) return 0.0;
        double sum = 0.0;
        for (int i = 0; i < s_biasCount; i++) sum += s_biasRing[i];
        return sum / (double)s_biasCount * 1000.0;
    }

    // 判定窗（单侧毫秒；对称双侧）：严判 20/50/100 · 标准 30/75/150 · 宽判 45/100/200
    //   参考 ADOFAI 自身 Strict/Normal/Lenient 的角度窗换算（约 ±40/±65/±91ms）
    //   与 Malody 4K 的判定量级，宽判不再把 100ms 的偏差算成 Marvelous。
    static const double kJudgeWinMs[3][3] = {
        { 20.0,  50.0, 100.0 },
        { 30.0,  75.0, 150.0 },
        { 45.0, 100.0, 200.0 },
    };
    static std::atomic<int> s_judgeSet[kModeN] = { { 1 }, { 1 }, { 1 }, { 1 } };
    static inline int JudgeSet()
    {
        int mi = ActiveModeIndex();
        if (mi < 0) mi = 0;
        int js = s_judgeSet[mi].load(std::memory_order_relaxed);
        return (js < 0 || js > 2) ? 1 : js;
    }
    static inline double JudgeWinMs()  { return kJudgeWinMs[JudgeSet()][2]; }
    static inline double JudgeMarvMs() { return kJudgeWinMs[JudgeSet()][0]; }
    static inline double JudgePerfMs() { return kJudgeWinMs[JudgeSet()][1]; }
    static const double kJudgeWeight[4] = { 1.0, 0.9, 0.5, 0.0 };

    // ---------------- 打击特效 / 视觉运行时（仅渲染线程访问） ----------------
    struct HitFx
    {
        int    lane = 0;
        int    kind = 0;       // 0..3 见上
        double t0 = 0.0;       // 生成时刻（ImGui::GetTime）
        float  scale = 1.0f;   // 爆发图额外缩放
    };
    static std::vector<HitFx>   s_fx;               // 爆发特效
    static std::vector<uint8_t> s_consumed;         // 音符是否已被击中（与 s_notes 同尺寸）
    static std::vector<uint8_t> s_missed;           // 音符是否已判定漏掉
    static std::vector<uint8_t> s_tailDone;         // 长按尾端：0=未判 1=已判 2=提前松手(断)
    static float                s_laneFlash[kMaxLanes] = {};  // 判定闪条强度
    static float                s_laneHitBg[kMaxLanes] = {};  // 判定底光强度

    // ---- 自动/宏打歌运行时（仅渲染线程访问）----
    static bool     s_virtualDown[kMaxLanes] = {};   // 内部虚拟按下（不碰系统键鼠）
    static double   s_virtualUntil[kMaxLanes] = {};  // 长按保持到的 song 时刻
    static int      s_autoIdx = 0;                   // 下一个待打音符下标
    static int      s_autoGen = -1;                  // 谱面代次（换谱重置）
    static unsigned s_autoRng = 0x9E3779B9u;         // 拟人抖动 RNG
    static double   s_macroDrift = 0.0;              // 拟人慢漂移（ms）

    static std::atomic<int>    s_chartGen{ 0 };     // 谱面重建计数（渲染线程清运行时状态）

    // 渲染线程时钟：桥接线程每 ~7ms 更新 song 时间，这里用 QPC 线性外推，
    // 避免音符下落出现 7ms 级别的抖动（音画同步的关键）
    std::atomic<double> s_clockWall{ 0.0 };  // 桥接线程采样时钟时的 QPC 秒
    std::atomic<double> s_clockRate{ 1.0 };  // 实测 song 秒 / 墙秒（song.pitch）
    // ---------------- 皮肤贴图 ----------------
    // 皮肤目录解析优先级：
    //   1) 环境变量 ADOFAI_PERFECT_SKIN
    //   2) 运行目录下 skin_dir.txt 首行指定的目录（自定义皮肤）
    //   3) 运行目录（exe/DLL 同级）下的 skin\（发布包自带；含子目录时自动选有效子目录）
    //   4) 兜底：Steam 库中的 MalodyV\skin
    static char s_skinDir[MAX_PATH * 2] = { 0 };
    // 皮肤模式：true=内置皮肤（固定版式，ChartCore 自绘，推荐）；false=外部 .msp 皮肤（数据驱动）
    static std::atomic<bool> s_skinBuiltin{ true };

    static bool DirHasSkin(const char* dir)
    {
        return SkinMsp::LooksLikeSkin(dir);
    }

    static bool PickSkinSubdir(const char* skinRoot, char* out, size_t n)
    {
        char pat[MAX_PATH * 2];
        snprintf(pat, sizeof(pat), "%s\\*", skinRoot);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE)
            return false;
        bool found = false;
        do
        {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.')
            {
                char cand[MAX_PATH * 2];
                snprintf(cand, sizeof(cand), "%s\\%s", skinRoot, fd.cFileName);
                if (DirHasSkin(cand))
                {
                    snprintf(out, n, "%s", cand);
                    found = true;
                    break;
                }
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        return found;
    }

    static const char* BuiltinSkinDir();
    // 皮肤模式只读一次：true=内置皮肤（固定版式，ChartCore 自绘）；false=外部 .msp 皮肤（数据驱动）
    static void SkinModeLoadOnce()
    {
        static bool s_done = false;
        if (s_done) return;
        s_done = true;
        // 一次性迁移：旧版本把「MSP 数据驱动」写成默认，用户看到的是错位/缺元素的 MSP 皮肤。
        // 升级后先回到用户认可的旧版固定版式（内置皮肤）；要外部 .msp 皮肤需在皮肤页手动选择。
        if (I18N::Prefs::GetInt("skin_mode_v2", 0) == 0)
        {
            I18N::Prefs::SetStr("skin_mode", "builtin");
            I18N::Prefs::SetInt("skin_mode_v2", 1);
            I18N::Prefs::Save();
        }
        char mode[32] = { 0 };
        I18N::Prefs::GetStr("skin_mode", mode, sizeof(mode), "builtin");
        s_skinBuiltin.store(!(mode[0] && _stricmp(mode, "msp") == 0), std::memory_order_relaxed);
    }

    static bool TrySkinRoot(const char* steamRoot, char* out, size_t n)
    {
        char skinRoot[MAX_PATH * 2];
        snprintf(skinRoot, sizeof(skinRoot), "%s\\steamapps\\common\\MalodyV\\skin", steamRoot);
        if (DirHasSkin(skinRoot))
        {
            snprintf(out, n, "%s", skinRoot);
            return true;
        }
        return PickSkinSubdir(skinRoot, out, n);
    }

    // 皮肤目录名归一化：绝对路径、相对 <DLL目录>\skin 的目录名（如 "3758056108"）、
    // 相对 DLL 目录的路径都能解析。发布包皮肤位于 <DLL目录>\skin\<名字>；测试脚本与
    // 设置页保存的往往是相对名字，旧逻辑对相对值直接 DirHasSkin 失败后会回落到
    // <DLL目录>\skin 根（内置皮肤 Rurudo），导致"选中的皮肤没生效"。
    static bool SnapSkinPath(const char* v, const char* modPath, char* out, size_t n)
    {
        if (!v || !v[0]) return false;
        if (DirHasSkin(v)) { snprintf(out, n, "%s", v); return true; }
        if (!modPath || !modPath[0]) return false;
        static const char* kFmt[] = {
            "%s\\skin\\%s", "%s\\%s", "%s\\..\\skin\\%s",
            "%s\\..\\..\\skin\\%s", "%s\\..\\..\\..\\skin\\%s",
        };
        for (int i = 0; i < 5; i++)
        {
            char cand[MAX_PATH * 2];
            snprintf(cand, sizeof(cand), kFmt[i], modPath, v);
            if (DirHasSkin(cand)) { snprintf(out, n, "%s", cand); return true; }
        }
        return false;
    }

    static const char* SkinDir()
    {
        SkinModeLoadOnce();
        if (s_skinDir[0])
        {
            // 缓存目录必须与当前模式一致；运行时切回内置皮肤时立即生效（丢弃外部缓存）
            if (!s_skinBuiltin.load(std::memory_order_relaxed))
                return s_skinDir;
            const char* b = BuiltinSkinDir();
            if (b && b[0] && _stricmp(s_skinDir, b) == 0)
                return s_skinDir;
            s_skinDir[0] = 0;
        }

        // 未找到时不每帧全盘重扫：每 300 次调用最多重试一次
        static int s_missN = 0;
        if (s_missN > 0)
        {
            if (++s_missN < 300)
                return s_skinDir;
            s_missN = 1;
        }

        // DLL 所在目录（相对皮肤名解析用；缓存一次）
        static char s_modDir[MAX_PATH * 2] = { 0 };
        if (!s_modDir[0])
        {
            HMODULE hm0 = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)&SkinDir, &hm0) && hm0)
                GetModuleFileNameA(hm0, s_modDir, sizeof(s_modDir));
            if (char* slash = strrchr(s_modDir, '\\')) *slash = 0;
        }
        const char* modPath = s_modDir;

        // 1) 环境变量
        char env[MAX_PATH * 2];
        DWORD en = GetEnvironmentVariableA("ADOFAI_PERFECT_SKIN", env, sizeof(env));
        if (en > 0 && en < sizeof(env))
            SnapSkinPath(env, modPath, s_skinDir, sizeof(s_skinDir));
        if (s_skinDir[0])
            return s_skinDir;

        // 1a) 内置皮肤模式：固定使用发布包自带皮肤（<DLL目录>\skin 根 = Rurudo 4K VI），
        //     完全忽略外部 skin_dir；外部 .msp 模式才使用下方 skin_dir 解析链。
        if (s_skinBuiltin.load(std::memory_order_relaxed))
        {
            const char* b = BuiltinSkinDir();
            if (b && b[0] && DirHasSkin(b))
                snprintf(s_skinDir, sizeof(s_skinDir), "%s", b);
            if (s_skinDir[0])
                return s_skinDir;
        }

        // 1b) 设置页 / 皮肤页保存的 skin_dir（Prefs）
        char prefSaved[MAX_PATH * 2] = { 0 };
        I18N::Prefs::GetStr("skin_dir", prefSaved, sizeof(prefSaved), "");
        if (prefSaved[0])
            SnapSkinPath(prefSaved, modPath, s_skinDir, sizeof(s_skinDir));
        if (s_skinDir[0])
            return s_skinDir;

        // 2) DLL 同目录 skin_dir.txt
        if (modPath[0])
        {
            char cfg[MAX_PATH * 2];
            snprintf(cfg, sizeof(cfg), "%s\\skin_dir.txt", modPath);
            FILE* fp = fopen(cfg, "rb");
            if (fp)
            {
                char line[MAX_PATH * 2] = { 0 };
                if (fgets(line, sizeof(line), fp))
                {
                    size_t n = strlen(line);
                    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' '))
                        line[--n] = 0;
                    if (line[0])
                        SnapSkinPath(line, modPath, s_skinDir, sizeof(s_skinDir));
                }
                fclose(fp);
            }
            if (s_skinDir[0])
                return s_skinDir;
            // 3) 运行目录（exe/DLL 同级）下的 skin\：发布包自带皮肤优先
            //    依次检查 DLL 目录、上一级、上两级、上三级（兼容 bin\Release 开发布局）
            {
                static const char* kUps[] = { "", "\\..", "\\..\\..", "\\..\\..\\.." };
                for (int up = 0; up < 4 && !s_skinDir[0]; up++)
                {
                    char root[MAX_PATH * 2];
                    snprintf(root, sizeof(root), "%s%s\\skin", modPath, kUps[up]);
                    if (DirHasSkin(root))
                        snprintf(s_skinDir, sizeof(s_skinDir), "%s", root);
                    else
                        PickSkinSubdir(root, s_skinDir, sizeof(s_skinDir));
                }
            }
            if (s_skinDir[0])
                return s_skinDir;

            // 3a) 模块目录往上找 steamapps 结构（"...\steamapps\common\MalodyV\skin"）
            char up[MAX_PATH * 2];
            snprintf(up, sizeof(up), "%s\\..\\..\\..\\MalodyV\\skin", modPath);
            if (PickSkinSubdir(up, s_skinDir, sizeof(s_skinDir)))
                return s_skinDir;
        }

        // 3b) 注册表 Steam 安装目录
        char steamRoot[MAX_PATH * 2] = { 0 };
        DWORD sz = sizeof(steamRoot);
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath",
                         RRF_RT_REG_SZ, nullptr, steamRoot, &sz) == ERROR_SUCCESS && steamRoot[0])
        {
            for (char* q = steamRoot; *q; q++)
                if (*q == '/') *q = '\\';
            if (TrySkinRoot(steamRoot, s_skinDir, sizeof(s_skinDir)))
                return s_skinDir;
        }

        // 3c) Steam 库配置文件 libraryfolders.vdf 中登记的其它库（不猜盘符/目录名）
        if (steamRoot[0])
        {
            char vdf[MAX_PATH * 2];
            snprintf(vdf, sizeof(vdf), "%s\\steamapps\\libraryfolders.vdf", steamRoot);
            FILE* fp = fopen(vdf, "rb");
            if (fp)
            {
                char line[1024];
                while (fgets(line, sizeof(line), fp) && !s_skinDir[0])
                {
                    const char* q = strstr(line, "\"path\"");
                    if (!q) continue;
                    q = strchr(q + 6, '"');
                    if (!q) continue;
                    const char* e = strchr(q + 1, '"');
                    if (!e || e - q - 1 <= 2) continue;
                    char lib[MAX_PATH * 2] = { 0 };
                    size_t n = (size_t)(e - q - 1);
                    if (n >= sizeof(lib)) n = sizeof(lib) - 1;
                    memcpy(lib, q + 1, n);
                    for (char* c = lib; *c; c++)
                        if (*c == '/') *c = '\\';
                    TrySkinRoot(lib, s_skinDir, sizeof(s_skinDir));
                }
                fclose(fp);
            }
        }
        if (s_skinDir[0])
            return s_skinDir;

        // 未找到皮肤：保持空串，调用方按无皮肤处理（每 300 次调用自动重试）
        if (!s_skinDir[0])
            s_missN = 1;
        {
            static bool s_loggedMissing = false;
            if (!s_loggedMissing)
            {
                s_loggedMissing = true;
                Log::Printf("[4K] skin not found; put skin folder next to Injector.exe (or set ADOFAI_PERFECT_SKIN)");
            }
        }
        return s_skinDir;
    }
    // ---------------- 皮肤贴图（Malody MSP 角色表 + 内置回退） ----------------
    static const char* BuiltinSkinDir()
    {
        static char s_dir[MAX_PATH * 2] = { 0 };
        if (!s_dir[0])
            SkinMsp::DefaultRoot(s_dir, sizeof(s_dir));
        return s_dir;
    }
    static void* LoadTexAbs(const char* dir, const char* file)
    {
        if (!dir || !dir[0] || !file || !file[0])
            return nullptr;
        char buf[MAX_PATH * 2 + 128];
        snprintf(buf, sizeof(buf), "%s\\%s", dir, file);
        return RenderHook_LoadTexture(buf);
    }
    static void* LoadRoleTex(const char* packDir, const char* file, const char* fallback)
    {
        void* t = LoadTexAbs(packDir, file);
        if (t) return t;
        const char* b = BuiltinSkinDir();
        if (packDir && b && _stricmp(packDir, b) == 0)
            return LoadTexAbs(packDir, fallback);   // 内置目录：角色名缺失时回退同目录的内置文件名
        return LoadTexAbs(b, fallback);
    }
    // 皮肤目录内是否存在该文件（用于判定贴图是否来自本皮肤，而非内置回退）
    static bool FileExistsIn(const char* dir, const char* file)
    {
        if (!dir || !dir[0] || !file || !file[0])
            return false;
        char buf[MAX_PATH * 2 + 128];
        snprintf(buf, sizeof(buf), "%s\\%s", dir, file);
        DWORD a = GetFileAttributesA(buf);
        return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }
    static float PngAspect(const char* path, float dflt)
    {
        FILE* fp = nullptr;
        if (fopen_s(&fp, path, "rb") != 0 || !fp) return dflt;
        unsigned char h[33] = { 0 };
        size_t rd = fread(h, 1, 33, fp);
        fclose(fp);
        if (rd < 33) return dflt;
        if (!(h[0] == 0x89 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G')) return dflt;
        unsigned w = ((unsigned)h[16] << 24) | ((unsigned)h[17] << 16) | ((unsigned)h[18] << 8) | h[19];
        unsigned hh = ((unsigned)h[20] << 24) | ((unsigned)h[21] << 16) | ((unsigned)h[22] << 8) | h[23];
        if (!w || !hh) return dflt;
        float a = (float)w / (float)hh;
        return (a > 0.05f && a < 4.f) ? a : dflt;
    }
    static const char* FallbackNoteName(int lane)
    {
        return (lane == 0 || lane == 3) ? "notex-1.png" : "notex-2.png";
    }
    static const char* FallbackPressName(int lane)
    {
        return (lane == 1 || lane == 2) ? "notepress2.png" : "notepress1.png";
    }

    static void* s_texCharIdle = nullptr;
    static void* s_texCharKey[4] = {};
    static void* s_texCheek = nullptr;
    static void* s_texJudgePop[5] = {};
    static void* s_texNote = nullptr;
    static void* s_texPress = nullptr;
    static void* s_texJudge = nullptr;
    static void* s_texLogo = nullptr;
    static void* s_texAvatar = nullptr;
    static void* s_texCombo[12] = {};
    static void* s_texAcc[12] = {};
    static void* s_texBurst = nullptr;
    static void* s_texHitBg = nullptr;
    static void* s_texLine = nullptr;
    static void* s_texGrid = nullptr;
    static void* s_texJudgeBar[4] = {};
    static void* s_texHold = nullptr;
    static void* s_texPress2 = nullptr;
    // ---- MSP 整屏皮肤运行时（声明需早于 LoadSkin）----
    static char s_lineImgName[96] = { 0 };    // 皮肤判定线贴图名（引擎定位）
    static char s_hitBgImgName[96] = { 0 };   // 皮肤底部打击光贴图名（引擎定位）
    static int  s_modNoteSplit = 0;           // 排序后第一个音符模块位置（之前=音符后层）
    static void* s_texHits[16] = {};
    static void* s_texPnum = nullptr;
    static void* s_texPnumSet[12] = {};
    static int   s_kps[kMaxLanes + 1] = {};      // 每轨 KPS + 合计（右上数字）
    static void* s_texRing = nullptr;
    static void* s_texRingOut = nullptr;
    static void* s_texRingIn = nullptr;
    static int   s_skinTries = 0;
    static bool  s_skinLoaded = false;
    static bool  s_skinReload = false;
    static bool  s_loggedDir = false;
    // MSP 角色扩展
    static void* s_texNoteL[kMaxLanes] = {};
    static void* s_texHoldHeadL[kMaxLanes] = {};
    static void* s_texHoldBodyL[kMaxLanes] = {};
    static void* s_texHoldTailL[kMaxLanes] = {};
    static void* s_texPressL[kMaxLanes] = {};
    static bool  s_pressBuiltin[kMaxLanes] = {};
    static bool  s_noteCustom[kMaxLanes] = {};
    static bool  s_holdCustom[kMaxLanes] = {};
    static bool  s_bgEnabled = false;
    static int   s_hitFxN = 9;
    static int   s_hitFxFps = 33;
    static void* s_judgeAnim[4][64] = {};
    static int   s_judgeAnimN[4] = {};
    static int   s_judgeAnimFps[4] = {};
    static int   s_judgeAnimW[4] = {};                 // judge 结果动画首帧原始宽（px）
    static int   s_judgeAnimH[4] = {};                 // judge 结果动画首帧原始高（px）
    static bool  s_digitFullCombo = false, s_digitFullAcc = false;
    static float s_digitAspectCombo = 0.34f, s_digitAspectAcc = 0.34f;
    static void* s_texBg = nullptr;
    static char  s_skinTitle[128] = { 0 };
    static char  s_skinCover[96] = { 0 };            // Meta.cover：ModuleParamImage.res 的图片兜底
    static char  s_skinCreator[128] = { 0 };
    static bool StrHasI(const char* s, const char* sub)
    {
        if (!s || !sub || !*sub) return false;
        size_t n = strlen(sub);
        for (const char* p = s; *p; p++)
        {
            size_t i = 0;
            while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)sub[i])) i++;
            if (i == n) return true;
        }
        return false;
    }
    static void LoadDigitSet(const char* dir, const char* base, const char* fallbackBase,
                             int start, void* out[12], bool* fullTex, float* aspect)
    {
        char useBase[64] = {};
        if (base && base[0]) snprintf(useBase, sizeof(useBase), "%s", base);
        else snprintf(useBase, sizeof(useBase), "%s", fallbackBase);
        bool custom = (base && base[0]) ? (_stricmp(base, "combo-") != 0 && _stricmp(base, "acc-") != 0) : false;
        *fullTex = custom;
        for (int d = 0; d < 12; d++)
        {
            char f[128];
            snprintf(f, sizeof(f), "%s%d.png", useBase, start + d);
            char fb[64];
            snprintf(fb, sizeof(fb), "%s%d.png", fallbackBase, d);
            out[d] = LoadRoleTex(dir, f, fb);
        }
        if (custom && dir)
        {
            char p[MAX_PATH * 2 + 128];
            snprintf(p, sizeof(p), "%s\\%s%d.png", dir, useBase, start);
            float a = PngAspect(p, 0.34f);
            if (a > 0.05f && a < 4.f) *aspect = a;
        }
    }

    // 返回是否全部就绪；D3D 设备未就绪时下一帧重试
    // ================= MSP 整屏渲染（数据驱动） =================
    // 逆向依据（Malody.Scene.Composer.fky::ApplyBasicParam / ApplyImageSize，见 SkinMsp.h）：
    //   · x/y 单位：0=Percent（x 用父宽、y 用父高；原点左下，y=100 上边缘）
    //               1=Unit（1080p 像素 × 屏幕缩放 u）  2=Px（原始像素 × u）
    //   · pivot：0..8 = 左/中/右 × 上/中/下
    //   · 带 scene NoteX 的模块是"轨道模块"：x 从所在轨左边缘起算（% 按整屏宽）
    //   · ModuleParamImage w/h 单位 wu/hu 同上，0 = 用贴图原始尺寸（保持比例）
    static SkinMsp::RoleMod s_mods[SkinMsp::kModMax];
    static int   s_modN = 0;
    static int   s_modSorted[SkinMsp::kModMax] = {};
    static bool  s_modAlt[SkinMsp::kModMax] = {};      // 「备用图组」中落选的模块（同组只画一张）
    static bool  s_mspFull = false;
    static float s_mspKeyScale = 0.f;                  // Meta.Key.Scale：判定区宽 = Scale×屏宽（0=无 meta 兜底整屏）
    static float s_mspKeyAngle = 0.f;                  // Meta.Key.angle：3D 轨道倾角（度）
    static int   s_mspKeyUse3D = 0;                    // Meta.Key.use3D：1=走透视轨道渲染
    static int   s_mspModeId = -1;                     // Meta.mode：7=Ring（圆形判定皮肤）
    static float s_mspRingDis = 0.f;                   // Meta.Ring.dis：圆环半径
    static bool  s_skinRrd = false;                    // 本皮肤是否 rrdv52 系（Rurudo）: Lua 驱动模块只对该系生效
    static bool  s_mspHasLine = false;                 // msp 皮肤自带判定线模块（judgeline/noteline/noteinex）
    static bool  s_judgeOwnBar[4] = {};                // 皮肤自带判定静态图（judge-N.png，非内置回退）
    static bool  s_judgeOwnAnim[4] = {};               // 皮肤自带判定动画（Judge 模块 images[] frames>1）
    static bool  s_hitBgOwn = false;                   // 皮肤自带判定底光（notehitbg，非内置回退）
    static char  s_skinDirCur[MAX_PATH * 2] = { 0 };
    static char  s_skinScript[96] = { 0 };             // Meta.script（皮肤 Lua 入口）
    static bool  s_skinLuaTried = false;               // 沙箱只尝试装载一次/每次换肤
    static bool  s_skinLuaActive = false;              // 本帧沙箱是否生效
    // 每轨音符贴图的可见区域（1080p 基准；notex-1.png 420x400 可见部分只有顶部 185px）
    static float s_noteNatW[kMaxLanes] = {}, s_noteNatH[kMaxLanes] = {};
    static float s_noteU0[kMaxLanes] = {}, s_noteV0[kMaxLanes] = {};
    static float s_noteU1[kMaxLanes] = { 1,1,1,1,1,1,1,1,1,1 };
    static float s_noteV1[kMaxLanes] = { 1,1,1,1,1,1,1,1,1,1 };

    // MSP 绘制顺序 rank（必须与场景画布一致，见加载处注释）：
    //   0 = layer0/2（世界空间：轨道之下，先画）
    //   1 = layer3（世界空间：音符之上）
    //   2 = layer1（Background 屏幕空间画布，sortOrder 1）
    //   3 = layer4（Foreground 屏幕空间画布，sortOrder 4，最前）
    static int MspDrawRank(int layer)
    {
        switch (layer)
        {
        case 2:  return 0;
        case 3:  return 1;
        case 1:  return 2;
        case 4:  return 3;
        default: return 0;      // layer 0 / 未知：跟随轨道之下
        }
    }
    static void ModAbsLen(int unit, float v, float along, float u, float* out)
    {
        *out = (unit == 0) ? along * v * 0.01f : v * u;
    }
    static void ModAnchor(const SkinMsp::RoleMod& m, float gw, float gh, float u, float* sx, float* sy)
    {
        float ax = 0, ay = 0, dx = 0, dy = 0;
        ModAbsLen(m.xu, m.x, gw, u, &ax);
        ModAbsLen(m.yu, m.y, gh, u, &ay);
        ModAbsLen(m.dxu, m.dx, gw, u, &dx);
        ModAbsLen(m.dyu, m.dy, gh, u, &dy);
        *sx = ax + dx;
        *sy = gh - (ay + dy);
    }
    static void ModPivotF(const SkinMsp::RoleMod& m, float* px, float* py)
    {
        int p = m.pivot;
        if (p < 0 || p > 8) p = 4;
        *px = (float)(p % 3) * 0.5f;     // 0 左 1 中 2 右
        *py = (float)(p / 3) * 0.5f;     // 0 上 1 中 2 下（屏幕 y 向下）
    }
    static void* ModLoadTex(const char* dir, const char* file)
    {
        if (!file || !file[0]) return nullptr;
        char p[MAX_PATH * 2 + 128];
        snprintf(p, sizeof(p), "%s\\%s", dir, file);
        void* t = RenderHook_LoadTexture(p);
        if (t) return t;
        const char* b = BuiltinSkinDir();
        if (b && dir && _stricmp(dir, b) != 0)
        {
            snprintf(p, sizeof(p), "%s\\%s", b, file);
            t = RenderHook_LoadTexture(p);
        }
        return t;
    }
    static bool ModTexInfo(const char* dir, const char* file, int nfo[6])
    {
        if (!file || !file[0]) return false;
        char p[MAX_PATH * 2 + 128];
        snprintf(p, sizeof(p), "%s\\%s", dir, file);
        if (RenderHook_ImageInfo(p, &nfo[0], &nfo[1], &nfo[2], &nfo[3], &nfo[4], &nfo[5]))
            return true;
        const char* b = BuiltinSkinDir();
        if (b && dir && _stricmp(dir, b) != 0)
        {
            snprintf(p, sizeof(p), "%s\\%s", b, file);
            if (RenderHook_ImageInfo(p, &nfo[0], &nfo[1], &nfo[2], &nfo[3], &nfo[4], &nfo[5]))
                return true;
        }
        return false;
    }
    static ImU32 ModColor(const char* s, ImU32 dflt, float aMul)
    {
        if (!s || s[0] != '#') return dflt;
        unsigned r = 255, g = 255, b = 255;
        if (sscanf_s(s, "#%2x%2x%2x", &r, &g, &b) < 3) return dflt;
        if (aMul < 0.f) aMul = 0.f;
        if (aMul > 1.f) aMul = 1.f;
        return IM_COL32(r, g, b, (int)(255.f * aMul));
    }
    static bool ModHasTrig(const SkinMsp::RoleMod& m, int src, int* valOut = nullptr)
    {
        for (int i = 0; i < m.trigN; i++)
            if (m.trig[i].source == src) { if (valOut) *valOut = (int)m.trig[i].val; return true; }
        return false;
    }

    static bool LoadSkin()
    {
        if (s_skinReload)
        {
            s_skinReload = false;
            s_skinLoaded = false;
            s_skinTries = 0;
            s_loggedDir = false;
        }
        if (!s_loggedDir)
        {
            s_loggedDir = true;
            Log::Printf("[skin] dir = %s", SkinDir());
        }
        if (s_skinLoaded)
            return true;
        const char* dir = SkinDir();
        static SkinMsp::Roles roles{};        // ~180KB（含整屏模块表）→ 必须静态
        bool hasRoles = (dir && dir[0]) ? SkinMsp::Load(dir, &roles) : false;
        // 皮肤模式（持久化于 adofai_perfect.cfg 的 skin_mode=msp|builtin）。默认内置皮肤。
        SkinModeLoadOnce();
        snprintf(s_skinDirCur, sizeof(s_skinDirCur), "%s", dir ? dir : "");
        // ---- MSP 整屏模块表（layer/order 升序；同层按文件顺序）----
        s_mspFull = false;
        s_mspKeyScale = 0.f;
        s_skinLuaTried = false;
        s_skinScript[0] = 0;
        s_mspKeyAngle = 0.f;
        s_mspKeyUse3D = 0;
        s_mspModeId = -1;
        s_mspRingDis = 0.f;
        s_skinRrd = false;
        s_modN = 0;
        s_modNoteSplit = 0;
        memset(s_modSorted, 0, sizeof(s_modSorted));
        memset(s_modAlt, 0, sizeof(s_modAlt));
        s_lineImgName[0] = 0;
        s_hitBgImgName[0] = 0;
        s_mspHasLine = false;
        s_hitBgOwn = false;
        // 贴图指针全量复位：避免上一个皮肤（尤其 MSP 皮肤）的贴图在本皮肤缺图时残留，
        // 这是"内置皮肤受其他皮肤影响"的直接根因之一（s_texNote 原先只在为 null 时赋值）。
        s_texNote = nullptr; s_texPress = nullptr; s_texPress2 = nullptr; s_texHold = nullptr;
        s_texBurst = nullptr; s_texJudge = nullptr; s_texLogo = nullptr; s_texAvatar = nullptr;
        s_texHitBg = nullptr; s_texLine = nullptr; s_texGrid = nullptr; s_texBg = nullptr;
        s_texPnum = nullptr; s_texRing = nullptr; s_texRingOut = nullptr; s_texRingIn = nullptr;
        s_texCharIdle = nullptr; s_texCheek = nullptr;
        memset(s_texNoteL, 0, sizeof(s_texNoteL));
        memset(s_texHoldHeadL, 0, sizeof(s_texHoldHeadL));
        memset(s_texHoldBodyL, 0, sizeof(s_texHoldBodyL));
        memset(s_texHoldTailL, 0, sizeof(s_texHoldTailL));
        memset(s_texPressL, 0, sizeof(s_texPressL));
        memset(s_texCharKey, 0, sizeof(s_texCharKey));
        memset(s_texJudgePop, 0, sizeof(s_texJudgePop));
        memset(s_texJudgeBar, 0, sizeof(s_texJudgeBar));
        memset(s_texCombo, 0, sizeof(s_texCombo));
        memset(s_texAcc, 0, sizeof(s_texAcc));
        memset(s_texPnumSet, 0, sizeof(s_texPnumSet));
        memset(s_texHits, 0, sizeof(s_texHits));
        memset(s_judgeAnim, 0, sizeof(s_judgeAnim));
        for (int i = 0; i < 4; i++) { s_judgeAnimN[i] = 0; s_judgeAnimFps[i] = 0; }
        s_digitFullCombo = s_digitFullAcc = false;
        s_digitAspectCombo = s_digitAspectAcc = 0.34f;
        // 内置皮肤隔离：切回内置（或非 MSP）时彻底卸载皮肤 Lua 与模块派生状态，
        // 不留上一个 MSP 皮肤的脚本实例/Clone/Shadow/贴图引用。
        if ((s_skinBuiltin.load(std::memory_order_relaxed) || !hasRoles) && SkinLua::Active())
            SkinLua::Unload();
        for (int i = 0; i < 4; i++)
        {
            s_judgeOwnBar[i] = false;
            s_judgeOwnAnim[i] = false;
            s_judgeAnimW[i] = 0;
            s_judgeAnimH[i] = 0;
        }
        if (!s_skinBuiltin.load(std::memory_order_relaxed) && hasRoles && roles.hasModules && roles.modCount > 0)
        {
            s_modN = roles.modCount > SkinMsp::Roles::kMaxMods ? SkinMsp::Roles::kMaxMods : roles.modCount;
            for (int i = 0; i < s_modN; i++) s_mods[i] = roles.mods[i];
            // ---- 未声明 Alpha（ModuleParamBasic 字段 6 缺省）= 0（不可见）----
            // 逆向依据：protobuf 缺省值即 0，Malody 编辑器只在 Alpha != 0 时写该字段。
            //   · Rurudo rrdcb1..4（连击提示图）：asminfo 无 alpha，rrdv52.lua 只在连击里程碑
            //     DoAlpha 淡入 —— 按 100 画会在屏幕左侧常驻 4 张叠在一起的大图（实测症状）。
            //   · Rurudo offm0..3 / Mango offset* / Kalpa offp/offg/offm：只作为 Shadow 源。
            //   · Phigros hscore/hmod、Kalpa circle/文本组：纯 Lua 载体/脚本按页切换。
            // 脚本驱动过的模块由 Sandbox 的 alpha 动画覆盖，不受影响。
            int alphaHidden = 0;
            for (int i = 0; i < s_modN; i++)
                if (!s_mods[i].alphaSet) { s_mods[i].alpha = 0; alphaHidden++; }
            if (alphaHidden > 0)
                Log::Printf("[skin] MSP alpha-unset -> hidden: %d/%d modules", alphaHidden, s_modN);
            snprintf(s_skinScript, sizeof(s_skinScript), "%s", roles.script);
            // Rurudo 家族识别：m5logo/cyellow 仅 Rurudo rrdv52 皮肤存在。
            // rrdv52.lua 的 DoMoveY 终值表只能套用到该系皮肤；
            // 否则 Phigros 等皮肤的 title/ver/pause 同名模块会被推出屏幕（实测）。
            for (int i = 0; i < s_modN; i++)
                if (!_stricmp(s_mods[i].desc, "m5logo") || !_stricmp(s_mods[i].desc, "cyellow"))
                { s_skinRrd = true; break; }
            Log::Printf("[skin] rrdv52 family=%d", (int)s_skinRrd);
            // 绘制顺序（逆向 level16 场景：画布 renderMode/sortOrder + SkinLayer 映射）：
            //   Below  = WorldSpace canvas order -10 → 在音符/判定线之下
            //   Above  = WorldSpace canvas order  20 → 在音符之上
            //   Background = ScreenSpaceCamera canvas order 1 → 屏幕空间，永远盖在 3D 内容之上
            //   Foreground = ScreenSpaceCamera canvas order 4 → 最前
            //  ⇒ 最终顺序：L2 → 音符 → L3 → L1 → L4（旧的按 layer 升序是错的：
            //     黑板 L2 会盖住 L1 的进度条/立绘）。
            for (int i = 0; i < s_modN; i++) s_modSorted[i] = i;
            for (int i = 1; i < s_modN; i++)
            {
                int v = s_modSorted[i], j = i - 1;
                while (j >= 0)
                {
                    const SkinMsp::RoleMod& a = s_mods[s_modSorted[j]];
                    const SkinMsp::RoleMod& b = s_mods[v];
                    const int ra = MspDrawRank(a.layer), rb = MspDrawRank(b.layer);
                    bool greater = (ra > rb) || (ra == rb && a.order > b.order);
                    if (!greater) break;
                    s_modSorted[j + 1] = s_modSorted[j];
                    j--;
                }
                s_modSorted[j + 1] = v;
            }
            // ---- 「备用图」组：同一描述前缀 + 尾号 1..4、纯图片、无 scene/trigger、同 layer。
            //   皮肤 Lua 常用 Module:Find("rrdcb"..math.random(1,4)) 只显示其中一张
            //   （Rurudo 的 4 张背景人物），若不处理会 4 张全部叠在画面上。
            memset(s_modAlt, 0, sizeof(s_modAlt));
            for (int i = 0; i < s_modN; i++)
            {
                const SkinMsp::RoleMod& a = s_mods[i];
                if (a.usage != 99 || a.type != 5000 || !a.img[0] || a.sceneN > 0 || a.trigN > 0 ||
                    a.lane >= 0 || !a.desc[0])
                    continue;
                // rrdcb1..4 = Rurudo 的「连击提示图」，由 Lua 在 combo 里程碑时抽一张：
                // 不是常显的备用图组，交给 MspLuaDriven 处理（否则会被这里强行只剩一张常显）。
                if (strncmp(a.desc, "rrdcb", 5) == 0) continue;
                const size_t dl = strlen(a.desc);
                if (dl < 2 || dl >= 39) continue;
                const char lastCh = a.desc[dl - 1];
                if (lastCh < '1' || lastCh > '4') continue;     // 从 1 开始才算备用组（offm0..3 不是）
                char pre[40];
                memcpy(pre, a.desc, dl - 1);
                pre[dl - 1] = 0;
                int grp[SkinMsp::kModMax];
                int gn = 0;
                for (int j = 0; j < s_modN && gn < SkinMsp::kModMax; j++)
                {
                    const SkinMsp::RoleMod& b = s_mods[j];
                    if (b.usage != 99 || b.type != 5000 || !b.img[0] || b.sceneN > 0 || b.trigN > 0 ||
                        b.lane >= 0 || b.layer != a.layer)
                        continue;
                    if (strlen(b.desc) != dl || strncmp(b.desc, pre, dl - 1) != 0) continue;
                    // 备用图必须同锚点/同轴心（Rurudo rrdcb1..4 完全叠放）。
                    // 否则 Taiko ship1/ship2、rendabg1/2 这种不同位置的独立图会被误并成一组只剩一张。
                    if (b.x != a.x || b.y != a.y || b.dx != a.dx || b.dy != a.dy ||
                        b.xu != a.xu || b.yu != a.yu || b.dxu != a.dxu || b.dyu != a.dyu ||
                        b.pivot != a.pivot)
                        continue;
                    grp[gn++] = j;
                }
                if (gn < 2) continue;
                bool seen = false;
                for (int t = 0; t < gn; t++) if (s_modAlt[grp[t]]) { seen = true; break; }
                if (seen) continue;
                unsigned h = 2166136261u;
                for (const char* p = pre; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
                const unsigned keep = h % (unsigned)gn;
                for (int t = 0; t < gn; t++) s_modAlt[grp[t]] = ((unsigned)t != keep);
                Log::Printf("[skin] alt group '%s*': %d imgs, keep #%u", pre, gn, keep + 1);
            }
            s_mspFull = true;
            s_mspKeyScale = roles.keyScale;
            s_mspKeyAngle = roles.keyAngle;
            s_mspKeyUse3D = roles.keyUse3D;
            s_mspModeId = roles.modeId;
            s_mspRingDis = roles.ringDis;
            Log::Printf("[skin] MSP keyScale=%.4f keys=%d judgePos=%d", roles.keyScale, roles.keyKeys, roles.keyJudgePos);
            Log::Printf("[skin] MSP mode=%d angle=%.1f use3D=%d ringDis=%.2f script='%s'",
                        roles.modeId, roles.keyAngle, roles.keyUse3D, roles.ringDis, roles.script);
            Log::Printf("[skin] MSP full-screen render: %d modules", s_modN);
            s_modNoteSplit = s_modN;
            for (int k = 0; k < s_modN; k++)
                if (s_mods[s_modSorted[k]].usage == 1) { s_modNoteSplit = k; break; }
            snprintf(s_lineImgName, sizeof(s_lineImgName), "%s", roles.line);
            snprintf(s_hitBgImgName, sizeof(s_hitBgImgName), "%s", roles.hitBg);
        }
        snprintf(s_skinTitle, sizeof(s_skinTitle), "%s", hasRoles ? roles.title : "");
        snprintf(s_skinCover, sizeof(s_skinCover), "%s", hasRoles ? roles.cover : "");
        snprintf(s_skinCreator, sizeof(s_skinCreator), "%s", hasRoles ? roles.creator : "");
        if (hasRoles)
            Log::Printf("[skin] msp title='%s' creator='%s' mode=%d note0='%s' hold0='%s' press0='%s' line='%s' combo='%s' acc='%s' hitfx='%s'x%d",
                        roles.title, roles.creator, roles.modeId, roles.note[0], roles.holdBody[0],
                        roles.press[0], roles.line, roles.comboBase, roles.accBase,
                        roles.hitFxBase, roles.hitFxCount);

        // 内置皮肤严格隔离：内置模式下一律不采用 MSP 角色名（roles.*），全部回退
        // 旧版固定文件名 + 固定参数（数字字型走 DrawDigitsCenter、特效 9 帧 33fps、
        // 不加载 judge 动画、不画 bg.png），保证与旧版 (main) 的渲染一致。
        if (!(!s_skinBuiltin.load(std::memory_order_relaxed) && hasRoles && dir && dir[0]))
            roles = SkinMsp::Roles{};

        for (int i = 0; i < kMaxLanes; i++)
        {
            s_texNoteL[i] = s_texHoldHeadL[i] = s_texHoldBodyL[i] = s_texHoldTailL[i] = nullptr;
            s_texPressL[i] = nullptr;
            s_pressBuiltin[i] = false;
        }
        memset(s_judgeAnim, 0, sizeof(s_judgeAnim));
        for (int i = 0; i < 4; i++) { s_judgeAnimN[i] = 0; s_judgeAnimFps[i] = 0; }

        // 小人（右上常驻 + 打击姿势）
        s_texCharIdle = LoadRoleTex(dir, roles.charImg, "rurudokey.png");
        for (int i = 0; i < 4; i++)
        {
            char fb[32];
            snprintf(fb, sizeof(fb), "rurudokey-%d.png", i + 1);
            s_texCharKey[i] = LoadRoleTex(dir, roles.charPress[i], fb);
        }
        s_texCheek = LoadRoleTex(dir, roles.cheek, "m5judgerrda.png");

        // 中央判定图（scene Judge=0..4）
        {
            const char* jp[5] = { "m5judgerrda.png", "m5judgerrdb.png", "m5judgerrdc.png",
                                  "m5judgerrdd.png", "m5judgerrde.png" };
            for (int i = 0; i < 5; i++)
                s_texJudgePop[i] = LoadRoleTex(dir, roles.judgePop[i], jp[i]);
        }
        // 判定动画（Judge 模块 frames>1，如 Malody Gazer 的 AGbest-*）
        const bool mspActive = !s_skinBuiltin.load(std::memory_order_relaxed) && hasRoles && dir && dir[0];
        for (int i = 0; i < 4; i++)
        {
            int cnt = roles.judgeAnimCount[i];
            if (cnt <= 1) continue;
            const char* base = roles.judgeAnimBase[i];
            if (!base[0]) continue;
            if (cnt > 64) cnt = 64;     // Gazer AGbest-* 等长动画（51 帧）需完整播放
            const int start = roles.judgeAnimStart[i];
            char first[128];
            snprintf(first, sizeof(first), "%s%d.png", base, start);
            // 皮肤自带判定动画：首帧必须在本皮肤目录内（否则视为无，MSP 不回退内置 judge-N）
            s_judgeOwnAnim[i] = mspActive && FileExistsIn(dir, first);
            if (s_judgeOwnAnim[i])
            {
                int nfo[6] = {};
                if (ModTexInfo(dir, first, nfo) && nfo[0] > 0 && nfo[1] > 0)
                {
                    s_judgeAnimW[i] = nfo[0];
                    s_judgeAnimH[i] = nfo[1];
                }
            }
            char fb[64];
            snprintf(fb, sizeof(fb), "judge-%d.png", i);
            for (int f = 0; f < cnt; f++)
            {
                char file[128];
                snprintf(file, sizeof(file), "%s%d.png", base, start + f);
                s_judgeAnim[i][f] = LoadRoleTex(dir, file, fb);
            }
            s_judgeAnimN[i] = cnt;
            int fps = roles.judgeAnimFps[i];
            s_judgeAnimFps[i] = (fps > 0) ? fps : 60;
        }

        // 音符 / 长条 / 按键光（按轨）
        for (int lane = 0; lane < kMaxLanes; lane++)
        {
            // 音符可见区域（贴图常有透明留白：notex-1.png 420x400 可见只有顶部 185px）
            s_noteNatW[lane] = s_noteNatH[lane] = 0.f;
            s_noteU0[lane] = s_noteV0[lane] = 0.f;
            s_noteU1[lane] = s_noteV1[lane] = 1.f;
            {
                const char* nf = roles.note[lane][0] ? roles.note[lane] : FallbackNoteName(lane);
                int nfo[6] = {};
                if (ModTexInfo(dir, nf, nfo) && nfo[0] > 0 && nfo[1] > 0)
                {
                    s_noteNatW[lane] = (float)nfo[4];
                    s_noteNatH[lane] = (float)nfo[5];
                    s_noteU0[lane] = (float)nfo[2] / (float)nfo[0];
                    s_noteV0[lane] = (float)nfo[3] / (float)nfo[1];
                    s_noteU1[lane] = (float)(nfo[2] + nfo[4]) / (float)nfo[0];
                    s_noteV1[lane] = (float)(nfo[3] + nfo[5]) / (float)nfo[1];
                }
            }
            void* nt = LoadRoleTex(dir, roles.note[lane], FallbackNoteName(lane));
            s_texNoteL[lane] = nt;
            if (!s_texNote && nt) s_texNote = nt;
            void* hh = LoadRoleTex(dir, roles.holdHead[lane], FallbackNoteName(lane));
            s_texHoldHeadL[lane] = hh;
            void* hb = LoadRoleTex(dir, roles.holdBody[lane], "lnx6.png");
            s_texHoldBodyL[lane] = hb;
            s_texHoldTailL[lane] = LoadRoleTex(dir, roles.holdTail[lane], nullptr);
            void* pr = LoadRoleTex(dir, roles.press[lane], FallbackPressName(lane));
            s_texPressL[lane] = pr;
            const char* pf = roles.press[lane][0] ? roles.press[lane] : FallbackPressName(lane);
            s_pressBuiltin[lane] = StrHasI(pf, "notepress");
            s_noteCustom[lane] = roles.note[lane][0] != 0;
            s_holdCustom[lane] = roles.holdBody[lane][0] != 0;
        }
        s_texPress = LoadRoleTex(dir, nullptr, "notepress1.png");
        s_texPress2 = LoadRoleTex(dir, nullptr, "notepress2.png");
        s_texHold = LoadRoleTex(dir, nullptr, "lnx6.png");

        // 打击特效帧序列
        s_hitFxN = 9;
        s_hitFxFps = 33;
        {
            int cnt = roles.hitFxCount > 0 ? roles.hitFxCount : 9;
            if (cnt > 16) cnt = 16;
            const char* base = roles.hitFxBase[0] ? roles.hitFxBase : "hits-";
            int start = roles.hitFxBase[0] ? roles.hitFxStart : 1;
            if (cnt <= 0) cnt = 1;
            for (int f = 0; f < cnt; f++)
            {
                char file[128], fb[64];
                snprintf(file, sizeof(file), "%s%d.png", base, start + f);
                snprintf(fb, sizeof(fb), "hits-%d.png", f + 1);
                s_texHits[f] = LoadRoleTex(dir, file, fb);
            }
            s_hitFxN = cnt;
            if (roles.hitFxFps > 0) s_hitFxFps = roles.hitFxFps;
            s_texBurst = s_texHits[0];
        }
        for (int i = s_hitFxN; i < 16; i++) s_texHits[i] = nullptr;

        // 判定闪条
        for (int i = 0; i < 4; i++)
        {
            char fb[32];
            snprintf(fb, sizeof(fb), "judge-%d.png", i);
            s_texJudgeBar[i] = LoadRoleTex(dir, roles.judgeBar[i], fb);
            // 仅当文件确实在本皮肤目录内才算"皮肤自带"（judge-N.png 常来自内置回退）
            s_judgeOwnBar[i] = mspActive && roles.judgeBar[i][0] && FileExistsIn(dir, roles.judgeBar[i]);
        }

        // 数字字型（combo / acc）
        LoadDigitSet(dir, roles.comboBase, "combo-", roles.comboStart, s_texCombo,
                     &s_digitFullCombo, &s_digitAspectCombo);
        LoadDigitSet(dir, roles.accBase, "acc-", roles.accStart, s_texAcc,
                     &s_digitFullAcc, &s_digitAspectAcc);

        // 场景元素
        s_texJudge = LoadRoleTex(dir, roles.judgeColor, "judgercolor.png");
        s_texLogo = LoadRoleTex(dir, roles.logo, "m5logo.png");
        s_texAvatar = LoadRoleTex(dir, roles.avatar, "progressrrd.png");
        s_texHitBg = LoadRoleTex(dir, roles.hitBg, "notehitbg.png");
        // 判定底光同样只认皮肤自带文件（MSP 不回退内置 notehitbg）
        s_hitBgOwn = mspActive && roles.hitBg[0] && FileExistsIn(dir, roles.hitBg);
        s_texLine = LoadRoleTex(dir, roles.line, "noteinex.png");
        // 皮肤自带判定线：roles.line 由皮肤模块解析而来且文件存在于本皮肤（MSP 不回退 noteinex）
        s_mspHasLine = mspActive && roles.line[0] && FileExistsIn(dir, roles.line);
        s_texGrid = LoadRoleTex(dir, roles.grid, "bggrid.png");
        s_texBg = LoadRoleTex(dir, roles.bg, "bg.png");
        s_bgEnabled = roles.bg[0] != 0;

        // 数字字体 / 倒计时圆环（内置）
        for (int i = 0; i <= 9; i++)
        {
            char n[32];
            snprintf(n, sizeof(n), "anton-%d.png", i);
            s_texPnumSet[i] = LoadRoleTex(dir, nullptr, n);
        }
        s_texPnum = s_texPnumSet[0];
        s_texRing = LoadRoleTex(dir, nullptr, "circle.png");
        s_texRingOut = LoadRoleTex(dir, nullptr, "circleout2.png");
        s_texRingIn = LoadRoleTex(dir, nullptr, "circlein2.png");

        if (s_texNote || s_texNoteL[0] || s_texLine || s_texCombo[0] || s_texCharIdle || s_texJudgeBar[0])
        {
            s_skinLoaded = true;
            Log::Printf("[skin] ready: msp=%d note=%d hold=%d press=%d line=%d combo=%d acc=%d hits=%d char=%d bg=%d",
                        hasRoles ? 1 : 0, s_texNote != 0, s_texHoldBodyL[0] != 0, s_texPressL[0] != 0,
                        s_texLine != 0, s_texCombo[0] != 0, s_texAcc[0] != 0, s_texHits[0] != 0,
                        s_texCharIdle != 0, s_texBg != 0);
        }
        else if (++s_skinTries % 60 == 0)
        {
            Log::Printf("[skin] pending: dir='%s' note=%d line=%d combo=%d",
                        dir ? dir : "", s_texNote != 0, s_texLine != 0, s_texCombo[0] != 0);
        }
        return s_skinLoaded;
    }

    // ---------------- 纯内存读取原语 ----------------
    static void* StaticDataPtr(MonoVTable* vt)
    {
        if (!vt || !mono_vtable_get_static_field_data)
            return nullptr;
        return mono_vtable_get_static_field_data(vt);
    }

    static void* StaticObj(MonoVTable* vt, uint32_t off)
    {
        void* data = StaticDataPtr(vt);
        // 注意：off == 0 是合法偏移（静态区第一个字段），不能当无效值
        if (!data)
            return nullptr;
        return *(void**)((char*)data + off);
    }

    static void* ListArrayRaw(void* list, int* count)
    {
        if (!list) { *count = 0; return nullptr; }
        void* items = *(void**)((char*)list + 0x10);   // List<T>._items (MonoArray)
        int size = *(int*)((char*)list + 0x18);        // List<T>._size
        if (size < 0) size = 0;
        if (size > 40000) size = 40000;
        *count = size;
        return items;
    }

    static uint32_t FO(MonoClass* cls, const char* name)
    {
        if (!cls || !mono_class_get_field_from_name)
            return 0;
        MonoClassField* f = mono_class_get_field_from_name(cls, name);
        return f ? mono_field_get_offset(f) : 0;
    }

    // ---------------- 静态单例发现（SEH 逐指针探测） ----------------
    static MonoClass* s_scanCls = nullptr;
    static void* s_scanHit = nullptr;
    static uint32_t s_scanHitOff = 0;

    static void ScanProbeLoop(void* data)
    {
        for (uint32_t off = 0x0; off <= 0x400; off += 8)
        {
            void* obj = *(void**)((char*)data + off);
            if (!obj || ((uintptr_t)obj & 7) != 0 || (uintptr_t)obj < 0x10000)
                continue;
            MonoClass* c = mono_object_get_class((MonoObject*)obj);
            if (c == s_scanCls)
            {
                s_scanHit = obj;
                s_scanHitOff = off;
                return;
            }
        }
    }

    // out : 命中的对象指针；outOff : 命中处相对静态数据区的偏移（供桥接线程纯读）
    static void ScanForInstance(MonoVTable* vt, MonoClass* cls, void** out, uint32_t* outOff)
    {
        *out = nullptr;
        if (outOff) *outOff = 0;
        if (!vt || !cls || !mono_object_get_class)
            return;
        void* data = StaticDataPtr(vt);
        if (!data)
            return;
        s_scanCls = cls;
        s_scanHit = nullptr;
        s_scanHitOff = 0;
        for (uint32_t base = 0x0; base <= 0x400; base += 0x80)
        {
            __try
            {
                ScanProbeLoop((char*)data + base);
            }
            __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                          ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
            {
            }
            if (s_scanHit)
            {
                *out = s_scanHit;
                if (outOff) *outOff = s_scanHitOff;
                return;
            }
        }
    }

    // ---------------- 主线程 vtable / 字段布局 ----------------
    static void SetDiag(const char* text);
    static void MainThreadDiag();
    static void MainThreadRefreshInstances();

    void MainThreadResolve()
    {
        if (!MonoApi::Ready())
            return;
        if (!g_img)
            g_img = mono_image_loaded("Assembly-CSharp");
        if (!g_img)
            return;
        if (!g_clsFloor)
        {
            g_clsFloor = mono_class_from_name(g_img, "", "scrFloor");
            g_clsConductor = mono_class_from_name(g_img, "", "scrConductor");
            g_clsLevelMaker = mono_class_from_name(g_img, "", "scrLevelMaker");
            g_clsController = mono_class_from_name(g_img, "", "scrController");
            if (!g_clsFloor || !g_clsConductor || !g_clsLevelMaker)
            {
                Log::Printf("[4K] class lookup failed floor=%p cond=%p lm=%p",
                            (void*)g_clsFloor, (void*)g_clsConductor, (void*)g_clsLevelMaker);
                return;
            }
            Log::Printf("[4K] classes resolved");
        }
        MonoDomain* dom = mono_domain_get ? mono_domain_get() : mono_get_root_domain();
        if (!dom)
            return;
        // 主线程上 mono_class_init 合法：完成字段布局（偏移才有效）并建 vtable
        if (!g_vtFloor)
        {
            mono_class_init(g_clsFloor);
            g_vtFloor = mono_class_vtable(dom, g_clsFloor);
        }
        if (!g_vtConductor)
        {
            mono_class_init(g_clsConductor);
            g_vtConductor = mono_class_vtable(dom, g_clsConductor);
        }
        if (!g_vtLevelMaker)
        {
            mono_class_init(g_clsLevelMaker);
            g_vtLevelMaker = mono_class_vtable(dom, g_clsLevelMaker);
        }
        if (!s_vtResolved && g_vtFloor && g_vtConductor && g_vtLevelMaker)
        {
            s_vtResolved = true;
            Log::Printf("[4K] vtables ready (floor=%p cond=%p lm=%p)",
                        (void*)g_vtFloor, (void*)g_vtConductor, (void*)g_vtLevelMaker);
        }
        if (!s_vtResolved)
            return;

        // ---- 字段偏移（mono API；仅主线程） ----
        if (!s_metaReady)
        {
            oF_entryTime = FO(g_clsFloor, "entryTime");
            oF_entryTimePitch = FO(g_clsFloor, "entryTimePitchAdj");
            oF_angleLen = FO(g_clsFloor, "angleLength");
            oF_isCCW = FO(g_clsFloor, "isCCW");
            oF_midSpin = FO(g_clsFloor, "midSpin");
            oF_isFake = FO(g_clsFloor, "isFake");
            oF_auto = FO(g_clsFloor, "auto");
            oF_hold = FO(g_clsFloor, "holdLength");
            oF_next = FO(g_clsFloor, "nextfloor");
            oF_prev = FO(g_clsFloor, "prevfloor");
            oF_taps = FO(g_clsFloor, "tapsNeeded");
            oLM_floors = FO(g_clsLevelMaker, "listFloors");
            oC_songPos = FO(g_clsConductor, "_songposition_minusi");
            oC_dspTime = FO(g_clsConductor, "dspTime");
            oC_dspTimeSong = FO(g_clsConductor, "dspTimeSong");
            oC_bpm = FO(g_clsConductor, "bpm");
            oCtrl_firstFloor = FO(g_clsController, "firstFloor");
            if (!oF_entryTime || !oF_angleLen || !oF_isCCW || !oF_midSpin || !oF_isFake ||
                !oF_auto || !oF_next || !oF_prev || !oLM_floors || !oC_bpm || !oCtrl_firstFloor)
            {
                char b[192];
                snprintf(b, sizeof(b),
                         "field offsets pending t=%x a=%x ccw=%x mid=%x fake=%x auto=%x nxt=%x prv=%x lm=%x bpm=%x ff=%x",
                         oF_entryTime, oF_angleLen, oF_isCCW, oF_midSpin, oF_isFake, oF_auto,
                         oF_next, oF_prev, oLM_floors, oC_bpm, oCtrl_firstFloor);
                SetDiag(b);
                return;
            }
            s_metaReady = true;
            g_clockSongUnits = (oC_songPos != 0);
            Log::Printf("[4K] offsets ready (songPos=%x clock=%s hold=%x taps=%x tp=%x lm=%x ff=%x)",
                        oC_songPos, g_clockSongUnits ? "song" : "dsp", oF_hold, oF_taps,
                        oF_entryTimePitch, oLM_floors, oCtrl_firstFloor);
        }

        // ---- 单例静态引用（静态字段偏移不可靠 → 扫描静态数据区匹配类） ----
        if (!s_lmInstOK && g_vtLevelMaker && g_clsLevelMaker)
        {
            void* p = nullptr;
            uint32_t off = 0;
            ScanForInstance(g_vtLevelMaker, g_clsLevelMaker, &p, &off);
            if (p)
            {
                s_lmInstOff = off;
                s_lmInstOK = true;
                Log::Printf("[4K] scrLevelMaker._instance slot @ static+%x", off);
            }
        }
        if (!s_condInstOK && g_vtConductor && g_clsConductor)
        {
            void* p = nullptr;
            uint32_t off = 0;
            ScanForInstance(g_vtConductor, g_clsConductor, &p, &off);
            if (p)
            {
                s_condInstOff = off;
                s_condInstOK = true;
                Log::Printf("[4K] scrConductor._instance slot @ static+%x", off);
            }
        }
        MainThreadRefreshInstances();
        MainThreadDiag();
    }

    // 主线程：调用托管 get_instance() 让游戏自己修复"已销毁实例"，并把最新
    // 指针发布给桥接线程（每 ~500ms 一次；mono 方法句柄缓存）。
    static void MainThreadRefreshInstances()
    {
        static MonoMethod* m_lm = nullptr;
        static MonoMethod* m_cond = nullptr;
        static MonoMethod* m_ctrl = nullptr;
        static DWORD s_lastRef = 0;
        DWORD now = GetTickCount();
        if (s_lastRef && now - s_lastRef < 500)
            return;
        s_lastRef = now;
        if (!mono_runtime_invoke)
            return;
        if (!m_lm && g_clsLevelMaker)
            m_lm = mono_class_get_method_from_name(g_clsLevelMaker, "get_instance", 0);
        if (!m_cond && g_clsConductor)
            m_cond = mono_class_get_method_from_name(g_clsConductor, "get_instance", 0);
        if (!m_ctrl && g_clsController)
            m_ctrl = mono_class_get_method_from_name(g_clsController, "get_instance", 0);
        if (m_lm)
        {
            MonoObject* exc = nullptr;
            MonoObject* o = mono_runtime_invoke(m_lm, nullptr, nullptr, &exc);
            if (!exc)
            {
                s_lmInstMain.store((void*)o, std::memory_order_release);
                s_lmInstPub.store(true, std::memory_order_release);
            }
        }
        if (m_cond)
        {
            MonoObject* exc = nullptr;
            MonoObject* o = mono_runtime_invoke(m_cond, nullptr, nullptr, &exc);
            if (!exc)
            {
                s_condInstMain.store((void*)o, std::memory_order_release);
                s_condInstPub.store(true, std::memory_order_release);
            }
        }
        if (m_ctrl)
        {
            MonoObject* exc = nullptr;
            MonoObject* o = mono_runtime_invoke(m_ctrl, nullptr, nullptr, &exc);
            if (!exc)
                s_ctrlInstMain.store((void*)o, std::memory_order_release);
        }
    }
    static void SetDiag(const char* text)
    {
        strncpy_s(s_diag, sizeof(s_diag), text, _TRUNCATE);
    }

    // 主线程诊断：用托管 getter（mono_runtime_invoke）核对静态槽位是否正确
    static void MainThreadDiag()
    {
        static DWORD s_last = 0;
        DWORD now = GetTickCount();
        static void* s_lastSlot = nullptr;
        static DWORD s_lastFull = 0;
        void* curSlot = (g_vtLevelMaker && s_lmInstOK) ? StaticObj(g_vtLevelMaker, s_lmInstOff) : nullptr;
        bool changed = (curSlot != s_lastSlot);
        if (now - s_last < 1000)
            return;
        if (!changed && now - s_lastFull < 60000)
            return;
        s_lastSlot = curSlot;
        s_lastFull = now;
        s_last = now;
        if (!g_clsLevelMaker || !g_vtLevelMaker || !mono_runtime_invoke)
            return;

        void* data = StaticDataPtr(g_vtLevelMaker);
        void* slot = s_lmInstOK ? StaticObj(g_vtLevelMaker, s_lmInstOff) : nullptr;
        void* viaGetter = nullptr;
        MonoMethod* m = mono_class_get_method_from_name(g_clsLevelMaker, "get_instance", 0);
        if (m)
        {
            MonoObject* exc = nullptr;
            MonoObject* o = mono_runtime_invoke(m, nullptr, nullptr, &exc);
            if (!exc)
                viaGetter = o;
        }
        char buf[400];
        int n = 0;
        n += snprintf(buf + n, sizeof(buf) - n, "[4K] mt: slot=%p getter=%p data=%p base=",
                      slot, viaGetter, data);
        if (data)
        {
            for (int i = 0; i < 8 && n < (int)sizeof(buf) - 24; i++)
                n += snprintf(buf + n, sizeof(buf) - n, "%x:%p ",
                              i * 8, *(void**)((char*)data + i * 8));
        }
        Log::Printf("%s", buf);
    }

    // ---------------- 字段偏移解析（桥接线程；类已在主线程 init） ----------------
    static bool ResolveFields()
    {
        if (g_offsetsOK)
            return true;
        if (!s_metaReady)
        {
            GameBridge::QueueMainThreadInit();
            return false;
        }
        if (!g_clockSongUnits && (!oC_dspTime || !oC_dspTimeSong))
        {
            SetDiag("conductor clock fields missing");
            return false;
        }
        g_offsetsOK = true;
        return true;
    }

    // ---------------- 原始读取（POD + SEH） ----------------
    struct ClockData { double song; float bpm; bool ok; };

    static void ReadClockRaw(void* cond, ClockData* out)
    {
        out->ok = false;
        out->song = 0.0;
        out->bpm = 0.f;
        if (!cond)
            return;
        __try
        {
            out->bpm = *(float*)((char*)cond + oC_bpm);
            if (g_clockSongUnits)
                out->song = *(double*)((char*)cond + oC_songPos);
            else
                out->song = *(double*)((char*)cond + oC_dspTime) -
                            *(double*)((char*)cond + oC_dspTimeSong);
            out->ok = true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                      ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            out->ok = false;
        }
    }

    struct ListHdr { void* items; int size; };

    // 单个引用字段的 SEH 安全读取（POD，无对象展开）
    static void* ReadPtrField(void* obj, uint32_t off)
    {
        void* v = nullptr;
        if (!obj || !off)
            return nullptr;
        __try
        {
            v = *(void**)((char*)obj + off);
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                      ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            v = nullptr;
        }
        return v;
    }

    // SEH-safe bool field read (POD)
    static bool ReadBoolField(void* obj, uint32_t off, bool def)
    {
        bool v = def;
        if (!obj || !off)
            return def;
        __try
        {
            v = *(bool*)((char*)obj + off);
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                      ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            v = def;
        }
        return v;
    }

    static void ReadListHdrRaw(void* list, ListHdr* out)
    {
        out->items = nullptr;
        out->size = 0;
        if (!list || ((uintptr_t)list & 7) != 0 || (uintptr_t)list < 0x10000)
            return;
        __try
        {
            void* items = *(void**)((char*)list + 0x10);
            int size = *(int*)((char*)list + 0x18);
            if (size < 0) size = 0;
            if (size > 40000) size = 40000;
            out->items = items;
            out->size = size;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                      ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
        }
    }


    // 直接从 List<scrFloor>._items 读（MonoArray.vector 在 0x20，元素 8 字节指针）
    static int ReadFloorsRaw(void* items, int count, RawFloor* out, int cap)
    {
        if (!items || !out || count <= 0)
            return 0;
        if (count > cap) count = cap;
        __try
        {
            for (int i = 0; i < count; i++)
            {
                void* f = *(void**)((char*)items + 0x20 + (size_t)i * sizeof(void*));
                RawFloor& r = out[i];
                r.t = 0; r.tp = 0; r.ang = 0; r.holdLen = 0; r.taps = 1;
                r.ccw = false; r.valid = false;
                r.midSpin = false; r.fake = false; r.autoPlay = false;
                if (!f || ((uintptr_t)f & 7) != 0 || (uintptr_t)f < 0x10000)
                    continue;
                r.t = *(double*)((char*)f + oF_entryTime);
                r.tp = oF_entryTimePitch ? *(double*)((char*)f + oF_entryTimePitch) : r.t;
                r.ang = *(double*)((char*)f + oF_angleLen);
                r.holdLen = oF_hold ? *(int*)((char*)f + oF_hold) : 0;
                r.taps = oF_taps ? *(int*)((char*)f + oF_taps) : 1;
                r.ccw = *(bool*)((char*)f + oF_isCCW) != 0;
                bool midSpin = *(bool*)((char*)f + oF_midSpin) != 0;
                bool isFake = *(bool*)((char*)f + oF_isFake) != 0;
                bool isAuto = *(bool*)((char*)f + oF_auto) != 0;
                r.midSpin = midSpin; r.fake = isFake; r.autoPlay = isAuto;
                r.valid = !midSpin && !isFake && !isAuto;
            }
            return count;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                      ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return -1;
        }
    }

    // 退化路径：从 scrController.firstFloor 沿 prevfloor 回溯到链头再正向收集
    static int ReadChainRaw(void* firstFloor, RawFloor* out, int cap)
    {
        if (!firstFloor || !out || cap <= 0)
            return 0;
        __try
        {
            void* head = firstFloor;
            for (int i = 0; i < 40000; i++)
            {
                void* prev = oF_prev ? *(void**)((char*)head + oF_prev) : nullptr;
                if (!prev || prev == head)
                    break;
                head = prev;
            }
            int n = 0;
            void* f = head;
            while (f && n < cap)
            {
                RawFloor& r = out[n];
                r.t = *(double*)((char*)f + oF_entryTime);
                r.tp = oF_entryTimePitch ? *(double*)((char*)f + oF_entryTimePitch) : r.t;
                r.ang = *(double*)((char*)f + oF_angleLen);
                r.holdLen = oF_hold ? *(int*)((char*)f + oF_hold) : 0;
                r.taps = oF_taps ? *(int*)((char*)f + oF_taps) : 1;
                r.ccw = *(bool*)((char*)f + oF_isCCW) != 0;
                r.midSpin  = *(bool*)((char*)f + oF_midSpin) != 0;
                r.fake     = *(bool*)((char*)f + oF_isFake) != 0;
                r.autoPlay = *(bool*)((char*)f + oF_auto) != 0;
                r.valid = !r.midSpin && !r.fake && !r.autoPlay;
                n++;
                f = *(void**)((char*)f + oF_next);
            }
            return n;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                      ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return -1;
        }
    }
    // ============================================================
    // 分键引擎：4K / 5K / 6K / 10K —— 四套彼此独立的算法
    //   转换风格（每个键位模式可选）：经典 / 叠(Jack) / 技(Tech) / 乱(Random) / 切(Trill)
    //
    //   研究依据（osu!wiki：Beatmap/Pattern/osu!mania，2026-10 查证）：
    //     · Jack(叠) ：同一列连续 3+ 音、密度 1/4 起，代表音乐里重复的音；
    //                   minijack = 2 连、longjack = 4+；同列在 1/2 间隔上的高
    //                   浓度出现叫"锚点"。写谱惯例：三音起步、四音封顶、换锚
    //                   换手 —— 4K 的叠必须跨手换锚，绝不能写成两列来回的伪叠。
    //     · Trill(切)：两列交替 3+ 音，分单手块 / 双手块；成块出现、块间换对。
    //     · Stream   ：等间隔连打；roll = 全键盘顺序扫过；burst = 1/4 以上短爆发。
    //     · Anchor   ：1/2 间隔同列高浓度（技术谱基本语汇，与楼梯/小叠组合）。
    //
    //   四套引擎共同遵守的手感约束（代码逐条实现）：
    //     ① 连打禁止单手轮指：同手连续落音在 <=0.15s 间隔下必须换手
    //     ② 同键硬下限 0.075s（人手物理极限）；非叠风格 0.6s 内不做无意义重复
    //     ③ 长按占用期间该键不再落音（含 0.05s 收尾余量）
    //     ④ 乐句化：按自然停顿/时间窗分句（8~18 音），句内图案成套、句间换花样
    //     ⑤ 均匀：乱/技 用"最近最少使用"加权 + 偏差校正（偏离最空键 3 次以上就改键）；
    //        叠/切允许锚点/对子键偏高，但锚点随句轮换，长谱整体均衡
    //     ⑥ 人味：手势化 burst（2~4 音）、句间镜像呼应、少量刻意小叠点缀，
    //        避免纯随机噪声与完美循环造成的机械感
    // ============================================================
    // 手别 / 手内键序（由内到外），各键位模式独立的键盘语汇
    // ------------------------------------------------------------
    // 风格生成框架（叠 / 技 / 乱 / 切）
    //   写法依据（osu!mania Ranking Criteria 术语与限制 + 社区写谱惯例）：
    //   · 叠 Jack/Minijack：同列 2~4 连，双手轮换 x x | y y；1/4 及更快只写小叠，
    //     长叠只允许慢拍；锚点在手内轮换，4K 绝不写成两列单手伪叠
    //   · 切 Trill：两列交替，跨手对子优先（4K F/J、D/K）；长度按等级封顶
    //     （Hard ≤9 连），块间插过渡音换对；高手度才允许单手短交替
    //   · 技 Tech：图案库（全键扫/折返扫/楼梯/之字/锚点/双手括弧）按乐句复用+镜像
    //   · 乱 Random：RC 的 Stream 定义——同拍值、不产生小叠，半结构化行走
    //   · 共通：BPM→拍值自适应、Lv 分级封顶、乐句动机（重复+变奏，去随机感）、
    //     回转角重音对齐拍点、软性均衡（不机械平均）
    // ------------------------------------------------------------
    static int EstimateLevel(const std::vector<Note>& notes, int cols);   // 定义见下文

    struct Plan
    {
        int    level = 10;
        int    tier  = 1;            // 0 简单/普通  1 困难  2 疯狂  3 专家
        double bpm   = 120.0;
        double beat  = 0.5;          // 秒/拍
        double phase = 0.0;          // 拍网格相位（秒）
        double avgDt = 0.30;
    };

    struct Ev
    {
        double t    = 0.0;    // 音符时刻（song 秒）
        double dt   = 0.0;    // 与上一音间隔
        double turn = 0.0;    // 本音回转角（弧度）
        int    dir  = 1;      // -1 = CCW / +1 = CW
        float  hold = 0.0f;   // 长按（仅首音）
        uint8_t taps = 1;     // 同刻点击数（>=2 → 押：双押/三押/四押/五押）
    };

    struct Gen
    {
        int cols = 4, mode = 0, style = 0;
        std::mt19937 rng;
        double lastUse[kMaxLanes];
        double holdUntil[kMaxLanes];
        int    use[kMaxLanes];
        int    lastLane = -1, lastHand = -1;
        double lastT = -1e9;
        int    lastRun = 0;      // 当前连续同键长度（跨块）
        int    handRun = 0;      // 当前连续单手长度（跨块）

        // 规划与乐句状态（去人机味的核心：图案按乐句复用/变奏，而不是逐音抽签）
        Plan   plan;
        int    phrase = 0, phraseNotes = 0, phraseLen = 12;
        int    motif = 0, nextMotif = -1;
        bool   mirror = false, drain = false;
        int    jackHand = 0, anchorLane[2] = { -1, -1 };
        int    pairA = -1, pairB = -1, pairLeft = 0;
        int    momentum = 1, momentumLeft = 0;
        bool   drainNext = false;   // 下一句的降强度标记（由 AssignStyled 按上一句密度设置）
        int    hist[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
        int    histN = 0;
        // 冰火手法（Ado）：当前段手法/手法对/相位（跨调用保持，段内锁死）
        int    adoTech = -1;                  // -1 未定 / 0 轮指 / 1 交互
        int    adoPairA = -1, adoPairB = -1;  // 当前手法对（严格 2 键）
        int    adoPhase = 0;                  // 起手相位（乐句锁定）
        int    adoPairPhrase = -1;            // 上次换对的乐句号

        void Init(int c, int m, int st, unsigned seed)
        {
            cols = c; mode = m; style = st;
            rng.seed(seed ? seed : 1u);
            for (int i = 0; i < kMaxLanes; i++)
            {
                lastUse[i] = -1e9; holdUntil[i] = -1e9; use[i] = 0;
                hist[i & 7] = -1;
            }
            histN = 0;
            adoTech = -1; adoPairA = adoPairB = -1; adoPhase = 0; adoPairPhrase = -1;
        }
        double Rand01() { return (double)(rng() % 1000000u) / 1000000.0; }
        int    Rand(int n) { return n > 0 ? (int)(rng() % (unsigned)n) : 0; }
        bool   Ok(int l, double t) const
        {
            return l >= 0 && l < cols && holdUntil[l] <= t && (t - lastUse[l]) >= kLaneFloor;
        }
        int    Hand(int l) const { return HandOf(l, cols); }
        double Snap(const Ev& e) const { return (plan.beat > 0.001) ? (e.dt / plan.beat) : 0.5; }
        bool   Accent(const Ev& e) const { return e.turn >= kAccent4K; }
        void   Hist(int l)
        {
            if (histN < 8) hist[histN++] = l;
            else { for (int i = 1; i < 8; i++) hist[i - 1] = hist[i]; hist[7] = l; }
        }
        int    HistAgo(int k) const { int i = histN - 1 - k; return (i >= 0 && i < 8) ? hist[i] : -1; }
        void   NewPhrase()
        {
            phrase++;
            phraseNotes = 0;
            phraseLen = 8 + (int)(rng() % 8u);          // 一句 8~15 音
            motif = (int)(rng() % 6u);                  // 动机 0..5（句内复用、句间变奏）
            nextMotif = -1;
            mirror = ((rng() % 100u) < 48u);            // 句间镜像呼应
            jackHand = 1 - jackHand;                    // 锚点换手：不长期吊在同一侧
            momentum = (rng() % 2u) ? 1 : 0;
            momentumLeft = 0;
            anchorLane[0] = anchorLane[1] = -1;
            pairLeft = 0;
            drain = drainNext;                          // 上一句偏密 → 本句降强度（呼吸句）
            drainNext = false;
        }
        void   Commit(int l, double t, float hold)
        {
            lastRun = (l == lastLane && (t - lastT) <= 1.0) ? (lastRun + 1) : 1;
            handRun = (Hand(l) == lastHand && (t - lastT) <= 1.0) ? (handRun + 1) : 1;
            lastUse[l] = t;
            use[l]++;
            if (hold > 0.f) holdUntil[l] = t + (double)hold + 0.05;
            lastLane = l; lastHand = Hand(l); lastT = t;
        }
        int    Nearby(int lane, double t)
        {
            if (lane >= 0 && lane < cols && Ok(lane, t))
                return lane;
            for (int d = 1; d < cols; d++)
            {
                if (lane - d >= 0 && Ok(lane - d, t)) return lane - d;
                if (lane + d < cols && Ok(lane + d, t)) return lane + d;
            }
            return lane;
        }
    };

    // ---- 等级/BPM 自适应上限 ----
    static int JackCap(int tier, double beats)
    {
        if (beats >= 1.40) return (tier >= 3) ? 6 : (tier >= 2) ? 5 : (tier >= 1) ? 3 : 2;
        if (beats >= 0.70) return (tier >= 3) ? 5 : (tier >= 2) ? 4 : (tier >= 1) ? 3 : 2;
        if (beats >= 0.45) return (tier >= 3) ? 4 : (tier >= 2) ? 3 : 2;
        if (beats >= 0.30) return (tier >= 3) ? 4 : (tier >= 2) ? 3 : (tier >= 1) ? 2 : 1;
        if (beats >= 0.21) return (tier >= 3) ? 3 : (tier >= 2) ? 3 : (tier >= 1) ? 2 : 1;
        return (tier >= 3) ? 2 : 1;
    }
    static int TrillCap(int tier, double beats)
    {
        int base = (tier <= 0) ? 4 : (tier == 1) ? 6 : (tier == 2) ? 9 : 12;
        if (beats < 0.45) base -= 2;
        if (beats < 0.28) base -= 2;
        return base < 2 ? 2 : base;
    }
    static int HandCap(int tier)   { return (tier <= 0) ? 4 : (tier == 1) ? 5 : (tier == 2) ? 6 : 8; }
    static int AnchorCap(int tier) { return (tier <= 0) ? 1 : (tier == 1) ? 2 : (tier == 2) ? 3 : 4; }

    // 锚点键：手内中心（食指/中指）优先 + 用量均衡 + 抖动；avoid 用于轮换锚点
    static int PickAnchor(Gen& g, int hand, double t, int avoid)
    {
        int lanes[5];
        int n = HandLanes(hand, g.cols, lanes);
        int best = -1; double bs = 1e18;
        for (int i = 0; i < n; i++)
        {
            int l = lanes[i];
            if (!g.Ok(l, t) || l == avoid) continue;
            double w = (double)i * 0.70 + (double)g.use[l] * 0.90 + g.Rand01() * 1.6;
            if (w < bs) { bs = w; best = l; }
        }
        if (best < 0)
            best = g.Nearby(lanes[0], t);
        return best;
    }

    // 跨手交替对（4K F/J、D/K；6K S/J、F/K...）：
    // 每只手取“用量最少的 3 键”做候选，再略微偏好中心 —— 边缘键不会被饿死
    static void PickCrossPair(Gen& g, double t)
    {
        int lh[5], rh[5];
        int ln = HandLanes(0, g.cols, lh);
        int rn = HandLanes(1, g.cols, rh);
        int lc[5], rc[5], lcn = 0, rcn = 0;
        for (int i = 0; i < ln; i++) if (g.Ok(lh[i], t)) lc[lcn++] = lh[i];
        for (int j = 0; j < rn; j++) if (g.Ok(rh[j], t)) rc[rcn++] = rh[j];
        if (lcn == 0) { lcn = 1; lc[0] = lh[0]; }
        if (rcn == 0) { rcn = 1; rc[0] = rh[0]; }
        for (int i = 0; i < lcn; i++)
            for (int k = i + 1; k < lcn; k++)
                if (g.use[lc[k]] < g.use[lc[i]]) std::swap(lc[i], lc[k]);
        for (int i = 0; i < rcn; i++)
            for (int k = i + 1; k < rcn; k++)
                if (g.use[rc[k]] < g.use[rc[i]]) std::swap(rc[i], rc[k]);
        int lk = std::min(3, lcn), rk = std::min(3, rcn);
        int bestA = lc[0], bestB = rc[0];
        double bs = -1e18;
        for (int i = 0; i < lk; i++)
            for (int j2 = 0; j2 < rk; j2++)
            {
                int a2 = lc[i], b2 = rc[j2];
                double w = 4.0
                         - 0.9 * (double)(std::abs(a2 - (g.cols / 2 - 1)) + std::abs(b2 - g.cols / 2))
                         - (double)(g.use[a2] + g.use[b2]) * 0.12
                         + g.Rand01() * 1.8;
                if (a2 == g.pairA && b2 == g.pairB) w -= 6.0;      // 换对
                if (w > bs) { bs = w; bestA = a2; bestB = b2; }
            }
        g.pairA = bestA; g.pairB = bestB;
        g.pairLeft = 5 + g.Rand(9);
    }

    // 落键：尊重物理限制（同键 >=75ms / 长按占用），推进手别与用量状态
    static int Put(Gen& g, int lane, const Ev& e)
    {
        int l = g.Nearby(lane, e.t);
        g.Commit(l, e.t, e.hold);
        g.Hist(l);
        return l;
    }

    static void MakePlan(Gen& g, const std::vector<Ev>& evs)
    {
        double bpm = s_bpm.load(std::memory_order_relaxed);
        if (!(bpm > 20.0 && bpm < 400.0))
        {
            std::vector<double> d;
            for (size_t i = 1; i < evs.size(); i++)
                if (evs[i].dt > 0.03) d.push_back(evs[i].dt);
            if (!d.empty())
            {
                std::sort(d.begin(), d.end());
                bpm = 60.0 / std::max(0.08, d[d.size() / 2] * 2.0);      // 中位音距按 1/2 拍估
            }
            if (!(bpm > 20.0 && bpm < 400.0)) bpm = 120.0;
        }
        g.plan.bpm  = bpm;
        g.plan.beat = 60.0 / bpm;

        double sum = 0.0; int cnt = 0;
        for (size_t i = 1; i < evs.size(); i++)
            if (evs[i].dt > 0.0) { sum += evs[i].dt; cnt++; }
        g.plan.avgDt = cnt ? (sum / (double)cnt) : 0.30;

        std::vector<Note> tmp;
        tmp.reserve(evs.size());
        for (size_t i = 0; i < evs.size(); i++)
        {
            Note nt; nt.time = evs[i].t; nt.lane = 0; tmp.push_back(nt);
        }
        g.plan.level = EstimateLevel(tmp, g.cols);
        g.plan.tier  = (g.plan.level <= 8) ? 0 : (g.plan.level <= 15) ? 1 : (g.plan.level <= 23) ? 2 : 3;

        // 拍网格相位：让重音（大回转）尽量落在拍点上
        double bestPh = 0.0, bestSc = -1e18;
        for (int k = 0; k < 32; k++)
        {
            double ph = g.plan.beat * (double)k / 32.0;
            double sc = 0.0;
            for (size_t i = 0; i < evs.size(); i++)
            {
                double x = (evs[i].t - ph) / g.plan.beat;
                x -= std::floor(x);
                double dist = ((x < 0.5) ? x : 1.0 - x) * g.plan.beat;
                if (dist < 0.05)
                    sc += 1.0 + std::min(1.5, evs[i].turn / 3.0);
            }
            if (sc > bestSc) { bestSc = sc; bestPh = ph; }
        }
        g.plan.phase = bestPh;
    }

    // ---- 叠（Jack）：跨手换锚 x x | y y，锚点手内轮换，快拍只写小叠 ----
    static void RunJack(Gen& g, const Ev* ev, int len, int* out)
    {
        int pos = 0;
        bool lastStair = false;
        int hand = ((g.motif & 1) ? 1 : 0) ^ (g.mirror ? 1 : 0);
        while (pos < len)
        {
            const Ev& e = ev[pos];
            int cap = JackCap(g.plan.tier, g.Snap(e));
            if (g.drain && cap > 2) cap = 2;
            int left = len - pos;
            if (cap < 2)                                   // 太快：不写叠，跨手交替
            {
                out[pos] = Put(g, PickAnchor(g, hand, e.t, -1), e);
                hand = 1 - hand;
                pos++;
                continue;
            }
            int runLen;
            int r2 = g.Rand(100);
            if (r2 < 55)      runLen = 2;                              // 小叠为主（换手好手感）
            else if (r2 < 85) runLen = std::min(cap, 3);
            else              runLen = std::min(cap, 3 + g.Rand(std::max(1, cap - 3)));
            if (runLen > left) runLen = left;
            if (g.Accent(e) && runLen < cap && runLen < left) runLen++;
            int avoid = (g.lastRun >= cap) ? g.lastLane : g.anchorLane[hand];
            int lane = PickAnchor(g, hand, e.t, avoid);
            g.anchorLane[hand] = lane;
            for (int k = 0; k < runLen; k++)
                out[pos + k] = Put(g, lane, ev[pos + k]);
            pos += runLen;
            int hln[5];
            int hlnN = HandLanes(hand, g.cols, hln);
            bool stair = (g.plan.tier >= 1) && !g.drain && !lastStair && hlnN >= 3 &&
                         (g.Rand(100) < ((g.motif % 3) ? 18 : 8));
            lastStair = stair;
            bool flip = !stair;
            if (flip && pos < len && g.Rand(100) < 80)
            {
                // 过渡音落另一只手后，下一组回到本手：x x | y x x | y ...（换手交互感）
                out[pos] = Put(g, PickAnchor(g, 1 - hand, ev[pos].t, lane), ev[pos]);
                pos++;
                flip = false;
            }
            if (flip) hand = 1 - hand;                     // 跨手换锚（4K 正统写法）
            if (pos + 1 < len && g.Rand(100) < 12)         // 破折：x x y x x
            {
                out[pos] = Put(g, PickAnchor(g, 1 - hand, ev[pos].t, lane), ev[pos]);
                pos++;
            }
        }
    }

    // ---- 切（Trill）：跨手对子交替，块间过渡换对，长度按等级封顶 ----
    static void RunTrill(Gen& g, const Ev* ev, int len, int* out)
    {
        int pos = 0;
        while (pos < len)
        {
            const Ev& e = ev[pos];
            int cap = TrillCap(g.plan.tier, g.Snap(e));
            if (g.drain && cap > 4) cap = 4;
            if (g.pairLeft <= 0 || g.pairA < 0 || g.pairB < 0)
                PickCrossPair(g, e.t);
            int a = g.pairA, b = g.pairB;
            if (!g.Ok(a, e.t) && !g.Ok(b, e.t))
            {
                PickCrossPair(g, e.t);
                a = g.pairA; b = g.pairB;
            }
            int group = 3 + g.Rand(std::max(1, cap - 2));  // 3..cap
            if (group > len - pos) group = len - pos;
            int start = (g.lastLane == a) ? 1 : 0;         // 顺着上一个音起
            for (int k = 0; k < group; k++)
                out[pos + k] = Put(g, ((k + start) & 1) ? b : a, ev[pos + k]);
            pos += group;
            g.pairLeft -= group;
            bool needConn = (g.plan.tier <= 1) || (g.Rand(100) < 55);
            if (pos < len && needConn)                     // 过渡音：换对 + 让手指喘口气
            {
                int other = 1 - HandOf(a, g.cols);
                out[pos] = Put(g, PickAnchor(g, other, ev[pos].t, b), ev[pos]);
                pos++;
                PickCrossPair(g, ev[(pos < len) ? pos : len - 1].t);
            }
            else if (g.pairLeft <= 2)
                PickCrossPair(g, ev[(pos < len) ? pos : len - 1].t);
            if (g.plan.tier >= 2 && !g.drain && g.Rand(100) < 15 && pos + 4 <= len)
            {                                              // 单手短交替（split 的一半）
                int h = 1 - HandOf(a, g.cols);
                int hl[5];
                int hn = HandLanes(h, g.cols, hl);
                if (hn >= 2)
                {
                    int x = hl[0], y = hl[1];
                    for (int k = 0; k < 4 && pos + k < len; k++)
                        out[pos + k] = Put(g, ((k & 1) ? y : x), ev[pos + k]);
                    pos += 4;
                    PickCrossPair(g, ev[(pos < len) ? pos : len - 1].t);
                }
            }
        }
    }

    // ---- 冰火手法（ADOFAI）：独立引擎见 AdoGen.h（轮指/交互/插/押，可离线单测）----
    //   本函数只做适配：Ev(弧度) → AdoGen::Ev(度) → 回填主键与押的追加键。
    static void RunAdo(Gen& g, const Ev* ev, int len, int* out,
                       std::vector<std::vector<int>>* chordOut, size_t off)
    {
        if (len <= 0) return;
        std::vector<AdoGen::Ev> ae((size_t)len);
        for (int i = 0; i < len; i++)
        {
            ae[(size_t)i].t    = ev[i].t;
            ae[(size_t)i].dt   = ev[i].dt;
            ae[(size_t)i].turn = ev[i].turn * (180.0 / 3.14159265358979323846);
            ae[(size_t)i].dir  = ev[i].dir;
            ae[(size_t)i].hold = ev[i].hold;
            ae[(size_t)i].taps = ev[i].taps;
        }
        AdoGen::Cfg cfg;
        cfg.cols = g.cols;
        cfg.tier = g.plan.tier;
        std::vector<AdoGen::Lane> al;
        AdoGen::Stats ast;
        AdoGen::Generate(ae.data(), len, cfg, al, &ast);
        for (int i = 0; i < len; i++)
        {
            out[i] = al[(size_t)i].primary;
            if (chordOut && al[(size_t)i].extraN > 0)
            {
                std::vector<int>& v = (*chordOut)[off + (size_t)i];
                for (int k = 0; k < al[(size_t)i].extraN; k++)
                    v.push_back(al[(size_t)i].extra[k]);
            }
        }
        // 段尾状态回写外层 Gen：段后紧随的慢音接着手别/键位/长按继续，
        // 避免段尾与慢音撞键、或慢音落进段中长按仍占用的键
        for (int i = 0; i < len; i++)
        {
            const double te = ae[(size_t)i].t;
            const AdoGen::Lane& le = al[(size_t)i];
            g.lastUse[le.primary] = te;
            if (ae[(size_t)i].hold > 0.f)
                g.holdUntil[le.primary] = te + (double)ae[(size_t)i].hold + 0.05;
            for (int x = 0; x < le.extraN; x++) g.lastUse[le.extra[x]] = te;
        }
        {
            const AdoGen::Lane& le = al[(size_t)len - 1];
            g.lastLane = le.primary;
            g.lastHand = HandOf(le.primary, g.cols);
            g.lastT    = ae[(size_t)len - 1].t;
            g.Hist(le.primary);
        }
        Log::Printf("[%dK] ado: roll=%d alt=%d chords=%d(%d keys, max %d) ins=%d sw=%d flip=%d",
                    g.cols, ast.rollNotes, ast.altNotes, ast.chords, ast.chordKeys, ast.maxChord,
                    ast.inserts, ast.switches, ast.pairFlips);
    }
    // ---- 技（Tech）：乐句图案库（全键扫/折返/楼梯/之字/锚点/双手括弧） ----
    static void RunTech(Gen& g, const Ev* ev, int len, int* out)
    {
        const int cols = g.cols;
        int pattern = (g.motif + g.phrase) % 6;
        if (g.drain) pattern = 4;                          // 上一句密集 → 舒服的锚点句
        int pos = 0;
        while (pos < len)
        {
            int take = len - pos;
            if (take > 24) take = 24;
            int hand = ((g.motif + pos) & 1) ^ (g.mirror ? 1 : 0);
            if (g.handRun >= HandCap(g.plan.tier) && g.lastLane >= 0)
                hand = 1 - g.Hand(g.lastLane);             // 防单手埋头长串
            if (cols == 4 && g.handRun >= 4 && g.lastLane >= 0)
                hand = 1 - g.Hand(g.lastLane);             // 4K 单手仅 2 键：同手串严控在 4 以内
            switch (pattern)
            {
            case 0:   // 全键扫（边界折返）
            {
                int dir = (g.motif & 2) ? 1 : -1;
                int lane = (dir > 0) ? 0 : cols - 1;
                for (int k = 0; k < take; k++)
                {
                    if (lane < 0 || lane >= cols)
                    {
                        dir = -dir;
                        lane += dir * 2;
                        if (lane < 0) lane = 1;
                        if (lane >= cols) lane = cols - 2;
                    }
                    out[pos + k] = Put(g, lane, ev[pos + k]);
                    lane += dir;
                }
                break;
            }
            case 1:   // 单手滚，每 4~6 音换手
            {
                int blk = (cols == 4) ? 4 : 4 + g.Rand(3);   // 4K 单手只有 2 键：块长 4 防伪 2K
                int h = hand, idx = 0, dir = (g.motif & 1) ? 1 : -1, left = blk;
                int hl[5];
                int hn = HandLanes(h, g.cols, hl);
                if (hn <= 0) hn = 1;
                for (int k = 0; k < take; k++)
                {
                    if (left <= 0)
                    {
                        h = 1 - h;
                        hn = HandLanes(h, g.cols, hl);
                        dir = -dir; left = blk; idx = 0;
                    }
                    if (idx < 0) idx = 0;
                    if (idx >= hn) idx = hn - 1;
                    out[pos + k] = Put(g, hl[idx], ev[pos + k]);
                    idx += dir;
                    if (idx >= hn || idx < 0) dir = -dir;
                    left--;
                }
                break;
            }
            case 2:   // 楼梯：每键两下，全键上/下行
            {
                int dir = (g.motif & 1) ? 1 : -1;
                if (cols == 4)
                {
                    // 4K 单手只有 2 键：改成"折点重复"走位 0-1-2-3-3-2-1-0，
                    // 保留楼梯语汇又避免 2 键同手连打形成伪 2K 串
                    static const int kStair4[8] = { 0, 1, 2, 3, 3, 2, 1, 0 };
                    int ph;
                    if (g.lastLane >= 0)                       // 从上一音的另一只手起头，防跨段串联
                        ph = (g.Hand(g.lastLane) == 0) ? 4 : 0;
                    else
                        ph = (g.motif & 2) ? 4 : 0;
                    for (int k = 0; k < take; k++)
                        out[pos + k] = Put(g, kStair4[(ph + k) & 7], ev[pos + k]);
                }
                else
                {
                    int lane = (dir > 0) ? 0 : cols - 1;
                    for (int k = 0; k < take; k++)
                    {
                        out[pos + k] = Put(g, lane, ev[pos + k]);
                        if ((k & 1) == 1)
                        {
                            lane += dir;
                            if (lane < 0) { lane = 1; dir = 1; }
                            if (lane >= cols) { lane = cols - 2; dir = -1; }
                        }
                    }
                }
                break;
            }
            case 3:   // 之字：外内交替
            {
                int seq[kMaxLanes]; int n = 0;
                for (int i = 0; i < (cols + 1) / 2; i++)
                {
                    seq[n++] = i;
                    if (cols - 1 - i != i) seq[n++] = cols - 1 - i;
                }
                int idx = (g.motif & 1) ? (n - 1) : 0;
                int step = (g.motif & 1) ? -1 : 1;
                for (int k = 0; k < take; k++)
                {
                    out[pos + k] = Put(g, seq[idx], ev[pos + k]);
                    idx += step;
                    if (idx < 0 || idx >= n)
                    {
                        step = -step; idx += step * 2;
                        if (idx < 0) idx = 0;
                        if (idx >= n) idx = n - 1;
                    }
                }
                break;
            }
            case 4:   // 锚点句：强指重复 + 手内邻键走位，每 4 音换手换锚（防单手长串）
            {
                int acap = AnchorCap(g.plan.tier);
                int h = hand;
                int hl[5];
                int hn = HandLanes(h, g.cols, hl);
                if (hn < 1) hn = 1;
                int ai = 0;
                {
                    double bs2 = 1e18;
                    for (int t2 = 0; t2 < hn; t2++)
                    {
                        if (!g.Ok(hl[t2], ev[pos].t)) continue;
                        double w2 = (double)t2 * 1.35 + (double)g.use[hl[t2]] * 0.6 + g.Rand01() * 1.6;
                        if (w2 < bs2) { bs2 = w2; ai = t2; }
                    }
                }
                for (int k = 0; k < take; k++)
                {
                    if (k > 0 && (k % 4) == 0)
                    {
                        h = 1 - h;
                        hn = HandLanes(h, g.cols, hl);
                        if (hn < 1) hn = 1;
                        ai = g.Rand(hn);
                    }
                    int idx;
                    if (acap >= 2 && (k % 3) == 0)
                        idx = ai;
                    else
                    {
                        int off = (hn >= 3) ? (1 + (k % 2)) : 1;
                        idx = ai + ((k & 1) ? off : -off);
                        if (idx < 0 || idx >= hn)
                            idx = ai + ((k & 1) ? -off : off);
                        if (idx < 0) idx = 0;
                        if (idx >= hn) idx = hn - 1;
                    }
                    out[pos + k] = Put(g, hl[idx], ev[pos + k]);
                }
                break;
            }
            default:  // 双手括弧：单手小交替块左右交替（split 的单键版）
            {
                int hl[5], hr[5];
                int hn = HandLanes(0, g.cols, hl);
                int rn = HandLanes(1, g.cols, hr);
                for (int t2 = 0; t2 < hn; t2++)
                    for (int u2 = t2 + 1; u2 < hn; u2++)
                        if (g.use[hl[u2]] < g.use[hl[t2]]) std::swap(hl[t2], hl[u2]);
                for (int t2 = 0; t2 < rn; t2++)
                    for (int u2 = t2 + 1; u2 < rn; u2++)
                        if (g.use[hr[u2]] < g.use[hr[t2]]) std::swap(hr[t2], hr[u2]);
                int a = hl[0], b = (hn > 1) ? hl[1] : hl[0];
                int c = hr[0], d = (rn > 1) ? hr[1] : hr[0];
                int first = (g.lastLane >= 0) ? (1 - g.Hand(g.lastLane)) : hand;   // 先落另一只手
                for (int k = 0; k < take; k++)
                {
                    int block = ((k / 4) + (first ? 1 : 0)) & 1;
                    int sub = k & 1;
                    int lane = block ? (sub ? d : c) : (sub ? b : a);
                    out[pos + k] = Put(g, lane, ev[pos + k]);
                }
                break;
            }
            }
            pos += take;
            if (pos < len && g.Rand(100) < 60)             // 段间过渡音后换图案
            {
                // 过渡音强制落另一只手：隔断"同手 2 键"跨段的伪 2K 串联
                int th = (g.lastLane >= 0) ? (1 - g.Hand(g.lastLane)) : hand;
                out[pos] = Put(g, PickAnchor(g, th, ev[pos].t, -1), ev[pos]);
                pos++;
            }
            pattern = (pattern + 1 + g.Rand(2)) % 6;
            if (g.drain) pattern = 4;
        }
    }

    // ---- 乱（Random）：RC 的 Stream 定义（不写小叠），半结构化行走 ----
    static void RunRandom(Gen& g, const Ev* ev, int len, int* out)
    {
        const int cols = g.cols;
        int momentum = (g.motif & 1) ? 1 : -1;
        int momentLeft = 0;
        for (int pos = 0; pos < len; pos++)
        {
            const Ev& e = ev[pos];
            int cap = JackCap(g.plan.tier, g.Snap(e));
            int best = -1, best2 = -1;
            double bs = -1e18, bs2 = -1e18;
            for (int l = 0; l < cols; l++)
            {
                if (!g.Ok(l, e.t)) continue;
                double sc = -(double)g.use[l] * 1.1;                  // 软均衡
                if (g.lastLane >= 0)
                {
                    int dl = l - g.lastLane;
                    if (l == g.lastLane) sc -= 9.0;                   // 乱不写小叠
                    else sc += 3.0 - 0.9 * std::abs(dl);              // 邻键优先
                }
                if (g.lastLane >= 0 && g.handRun >= HandCap(g.plan.tier) - 1 && g.Hand(l) == g.lastHand)
                    sc -= 7.0;                                        // 防单手漂移
                if (momentLeft > 0 && g.lastLane >= 0)
                {
                    int step = l - g.lastLane;
                    if ((momentum > 0 && step > 0) || (momentum < 0 && step < 0)) sc += 2.2;
                }
                if (l == g.HistAgo(1)) sc -= 5.5;                     // 反 ABA/ABAB 循环
                if (l == g.HistAgo(2) && g.HistAgo(0) == g.HistAgo(1)) sc -= 7.0;
                sc += g.Rand01() * 2.0;                               // 抖动（去机械化）
                if (sc > bs) { bs2 = bs; best2 = best; bs = sc; best = l; }
                else if (sc > bs2) { bs2 = sc; best2 = l; }
            }
            if (best < 0) best = g.Nearby(cols / 2, e.t);
            if (best2 >= 0 && g.plan.tier >= 1 && g.Rand(100) < 8) best = best2;   // 人味偏差
            if (g.Accent(e) && g.plan.tier >= 2 && g.lastRun + 1 <= std::min(cap, 3) &&
                pos > 1 && g.Rand(100) < 22)                          // 重音处偶尔小叠
                best = g.lastLane;
            out[pos] = Put(g, best, e);
            if (momentLeft <= 0)
            {
                int m = ((g.motif + g.phrase) & 1) ? 1 : -1;
                if (g.Rand(100) < 25) m = -m;
                momentum = m;
                momentLeft = 4 + g.Rand(4);
            }
            momentLeft--;
            if (out[pos] == 0 || out[pos] == cols - 1) momentLeft = 0;   // 到边换向
        }
    }

    // ---- 慢音：音乐性优先（大回转=重音可重复；换手呼吸；重复率按风格） ----
    static void FillSlow(Gen& g, const Ev& e, int* out)
    {
        int repPct = (g.style == kStyleJack) ? 45 : (g.style == kStyleTech) ? 26 :
                     (g.style == kStyleRandom) ? 12 : 8;
        int hand = (e.dir < 0) ? 0 : 1;
        if (g.mirror) hand = 1 - hand;
        if (g.lastHand == hand && (e.t - g.lastT) <= kSameHandDt) hand = 1 - hand;
        bool accent = g.Accent(e);
        double beats = (g.lastT > -1e8) ? ((e.t - g.lastT) / g.plan.beat) : 9.0;
        bool rep = g.lastLane >= 0 && (e.t - g.lastT) >= 0.30 && (e.t - g.lastT) <= 1.30 &&
                   g.Rand(100) < (accent ? repPct + 25 : repPct);
        if (rep && g.lastRun + 1 > JackCap(g.plan.tier, beats)) rep = false;
        int lane;
        if (rep)
            lane = g.lastLane;
        else
        {
            int lanes[5];
            int n = HandLanes(hand, g.cols, lanes);
            int depth = accent ? (n - 1) : ((e.turn >= kMidTurn6K) ? 1 : 0);
            if (g.style == kStyleAdo && depth > 1) depth = 1;   // 冰火手法：重音也只做相邻键
            lane = lanes[std::min(depth, n - 1)];
        }
        out[0] = Put(g, lane, e);
    }

    static void FillRun(Gen& g, const Ev* ev, int len, int* out,
                        std::vector<std::vector<int>>* chordOut, size_t off)
    {
        switch (g.style)
        {
        case kStyleJack:  RunJack(g, ev, len, out);  break;
        case kStyleTech:  RunTech(g, ev, len, out);  break;
        case kStyleTrill: RunTrill(g, ev, len, out); break;
        case kStyleAdo:   RunAdo(g, ev, len, out, chordOut, off); break;
        default:          RunRandom(g, ev, len, out); break;
        }
    }

    // 稳定种子：同一谱面 + 模式 + 风格 → 同一输出（不随重开谱面漂移）
    static unsigned SeedFor(const std::vector<Ev>& evs, int mode, int style, int cols)
    {
        unsigned h = 2166136261u;
        auto mix = [&](unsigned v) { h ^= v; h *= 16777619u; };
        mix((unsigned)cols); mix((unsigned)mode); mix((unsigned)style);
        mix((unsigned)evs.size());
        if (!evs.empty())
        {
            mix((unsigned)(evs.front().t * 1000.0));
            mix((unsigned)(evs.back().t * 1000.0));
            mix((unsigned)(evs[evs.size() / 2].t * 1000.0));
        }
        return h ? h : 1u;
    }

    // ---- 分发：乐句切块 → 整段规划（去人机味关键：图案按乐句复用而不是逐音抽签） ----
    static void AssignStyled(Gen& g, const std::vector<Ev>& evs, std::vector<int>& lanes,
                             std::vector<std::vector<int>>* chordOut)
    {
        const int style = g.style;
        lanes.assign(evs.size(), 0);
        if (chordOut) chordOut->assign(evs.size(), std::vector<int>());
        if (evs.empty())
            return;
        MakePlan(g, evs);
        g.NewPhrase();
        size_t i = 0;
        while (i < evs.size())
        {
            const Ev& e = evs[i];
            bool fast = (e.dt > 0.0) && (e.dt <= kFastDt);
            if (!fast)
            {
                if (e.dt > kGapPhrase || g.phraseNotes >= g.phraseLen)
                    g.NewPhrase();
                FillSlow(g, e, &lanes[i]);
                g.phraseNotes++;
                i++;
                continue;
            }
            size_t j = i + 1;
            while (j < evs.size() && evs[j].dt > 0.0 && evs[j].dt <= kFastDt)
                j++;
            size_t k = i;
            while (k < j)
            {
                if (g.phraseNotes >= g.phraseLen)
                {
                    // 上一句偏密 → 本句降强度（真人谱的呼吸句）
                    g.drainNext = (g.phraseNotes >= 12);
                    g.NewPhrase();
                }
                // 冰火手法：整段一次交给引擎（引擎内部按手法段切，长轮指段不会被切碎）
                size_t end = (style == kStyleAdo) ? j : std::min(j, k + 24);
                int part = (int)(end - k);
                int room = g.phraseLen - g.phraseNotes;
                if (style != kStyleAdo && part > room && room > 0)
                    part = room;
                if (part <= 0)
                    part = 1;
                FillRun(g, &evs[k], part, &lanes[k], chordOut, k);
                g.phraseNotes += part;
                k += (size_t)part;
            }
            i = j;
        }
    }

    // ---------------- 难度（Malody 式 Lv） ----------------
    //   Malody 的 Lv 是作者按体感标注的难度星级（社区口径：Lv.20 约 3★、
    //   Lv.26 约 4★、Lv.32 约 5★、Lv.38 约 5.8★、Lv.45 约 6.5★），没有公开
    //   自动公式。旧模型的三个偏高来源已修正：
    //     ① 4.10*ln(n)：800 音就 +27 级，任何长流谱都被推到 40+ → 改为 ±0.7 微调
    //     ② p95(8s) 对均匀流谱饱和（6.9 和 12 NPS 的谱报同一个密度量级）
    //        → 改为峰值密度 pk4 + 持续密度 su 双特征
    //     ③ 分段校准表最高 +5.6 级 → 删除
    //   新模型（体感难度 = 最难段落 + 耐力，凹映射不发散）：
    //     pk4 = 最难 4 秒窗平均行 NPS（97 分位，抗毛刺；和弦按 1 行计）
    //     pk2 = 2 秒窗 97 分位（速度尖峰）   su = 8 秒窗 60 分位（持续/耐力）
    //     eff = 0.62*pk4 + 0.38*su + 0.20*min(3, pk2-pk4) + 长度修正
    //     Lv  = 14.2*ln(eff) - 0.8
    //   校准锚点：It Go 类 160~170BPM 16 分单键流（819 音 / 148s / pk4≈7.2 /
    //   su≈5.8 / 中位音距 0.120s）→ eff≈6.7 → Lv.26（旧公式同谱 36~45）。
    static void RowDensityPercentile(const std::vector<Note>& notes, double win, double q, double* outDensity)
    {
        size_t n = notes.size();
        if (n == 0) { *outDensity = 0.0; return; }
        std::vector<double> rowsT;
        rowsT.reserve(n);
        for (size_t i = 0; i < n; i++)
            if (i == 0 || notes[i].time - notes[i - 1].time > 0.005)
                rowsT.push_back(notes[i].time);
        std::vector<double> v;
        v.reserve(rowsT.size());
        size_t lo = 0;
        for (size_t hi = 0; hi < rowsT.size(); hi++)
        {
            while (rowsT[hi] - rowsT[lo] > win)
                lo++;
            double span = rowsT[hi] - rowsT[lo];
            if (span < win * 0.5)
                span = win * 0.5;
            v.push_back((double)(hi - lo + 1) / span);
        }
        std::sort(v.begin(), v.end());
        size_t idx = (size_t)((double)(v.size() - 1) * q + 0.5);
        if (idx >= v.size())
            idx = v.size() - 1;
        *outDensity = v[idx];
    }

    static int EstimateLevel(const std::vector<Note>& notes, int cols)
    {
        const size_t n = notes.size();
        if (n < 4)
            return 1;
        double pk4 = 0.0, pk2 = 0.0, su = 0.0;
        RowDensityPercentile(notes, 4.0, 0.97, &pk4);
        RowDensityPercentile(notes, 2.0, 0.97, &pk2);
        RowDensityPercentile(notes, 8.0, 0.60, &su);
        double eff = 0.62 * pk4 + 0.38 * su
                   + 0.20 * std::min(3.0, std::max(0.0, pk2 - pk4));
        double lenAdj = 0.30 * std::log2((double)n / 800.0);   // 长度只微调（耐力）
        if (lenAdj < -0.35) lenAdj = -0.35;
        if (lenAdj >  0.70) lenAdj =  0.70;
        eff += lenAdj;
        if (eff < 0.25) eff = 0.25;
        double lv = 14.2 * std::log(eff) - 0.8;
        if (cols == 5)       lv -= 2.85;
        else if (cols == 6)  lv -= 2.49;
        else if (cols == 10) lv -= 3.20;
        if (lv < 1.0)  lv = 1.0;
        if (lv > 45.0) lv = 45.0;
        return (int)std::lround(lv);
    }
    // ---------------- 谱面构建 ----------------
    static void DumpChartTxt();
    static void BuildChart(const RawFloor* fl, int n)
    {
        const int  mi    = ActiveModeIndex();
        const int  midx  = (mi < 0) ? 0 : mi;
        const int  cols  = kLanesOf[midx];
        const int  style = s_style[midx].load(std::memory_order_relaxed);

        // ---- 第一遍：地砖 → 事件表（拆多押砖 / 长按 / 合并人类打不出的密押）----
        //   反编译依据（scrPlayer.UpdateHoldBehavior / scrPlanet.perc 的 holdCompletion）：
        //   ADOFAI 的长按块从 entryTime 一直按到"下一砖 entryTime"，**松手这一刻就是
        //   下一砖的判定拍**（nextTileIsHold 时松手不触发 Hit，必须继续按住 →
        //   链式长按合并成一条）。所以转谱时 t1 不再单独出键，由松手拍完成，
        //   否则长按尾端会多出一个"要同时松手+换键"的伪短按 → 手感错位。
        std::vector<Ev> evs;
        evs.reserve((size_t)n);
        int    statMashed = 0, statHoldCover = 0;
        double prevTe = -1e9;
        int    holdCoverIdx = -1;      // 该下标砖的拍点由上一块长按的松手完成
        int    lastHoldEv = -1;        // 最近一条长按事件（链式合并用）
        for (int i = 1; i < n; i++)
        {
            const RawFloor& r = fl[i];
            bool covered = false;      // true = 该砖的拍点由松手拍覆盖，不再出键
            if (i == holdCoverIdx)
            {
                holdCoverIdx = -1;
                if (r.valid && r.holdLen > 0 && lastHoldEv >= 0)
                {
                    double tEnd = (i + 1 < n) ? (g_clockSongUnits ? fl[i + 1].t : fl[i + 1].tp) : 0.0;
                    if (tEnd > evs[lastHoldEv].t + 0.02)
                    {
                        evs[lastHoldEv].hold = (float)(tEnd - evs[lastHoldEv].t);
                        holdCoverIdx = i + 1;
                        continue;
                    }
                }
                else if (r.valid)
                    covered = true;
            }
            if (!r.valid)
                continue;
            double t0 = g_clockSongUnits ? r.t : r.tp;
            double t1;
            bool hasNext = (i + 1 < n);
            if (hasNext)
                t1 = g_clockSongUnits ? fl[i + 1].t : fl[i + 1].tp;
            else
            {
                // 终点块没有下一格：用上一段的时长估算（只用于排布多押砖）
                double prevDt = t0 - (g_clockSongUnits ? fl[i - 1].t : fl[i - 1].tp);
                if (!(prevDt > 0.01))
                    prevDt = 0.12;
                t1 = t0 + prevDt;
            }
            if (!(t1 > t0))
                continue;
            double turn = r.ang;
            if (r.holdLen > 0)
                turn -= (double)r.holdLen * 2.0 * 3.14159265358979323846;
            if (turn < 0.0)
                turn = 0.0;
            int dir = r.ccw ? -1 : 1;
            int taps = (r.taps > 0) ? r.taps : 1;
            if (taps > 8)
                taps = 8;
            // 多押砖（tapsNeeded >= 2）的处理按"人手能不能分开按"分三档：
            //   间隔 >= 75ms          → 正常逐拍展开（交互/轮指流）
            //   45ms ~ 75ms           → 保留展开（伪双押 flam：快速两连，不同手指）
            //   < 45ms（>22 NPS/键）  → 合并成一个同刻"押"事件（双押/三押/四押/五押），
            //                           由冰火手法引擎按交叉/宽押取键（见 AdoGen.h）
            const int  tapsOrig = taps;
            const double tapGap = (t1 - t0) / (double)std::max(1, tapsOrig);
            if (tapsOrig >= 2 && tapGap < 0.045)
            {
                Ev e;
                e.t    = t0;
                e.dt   = e.t - prevTe;
                e.turn = turn / tapsOrig;
                e.dir  = dir;
                e.hold = (r.holdLen > 0 && hasNext) ? (float)(t1 - t0) : 0.f;
                e.taps = (uint8_t)std::min(tapsOrig, 5);
                prevTe = e.t;
                if (e.hold > 0.f) lastHoldEv = (int)evs.size();
                evs.push_back(e);
                statMashed += (tapsOrig - 1);
                continue;
            }
            {
                int maxTaps = (int)std::floor((t1 - t0) / 0.045);
                if (maxTaps < 1)
                    maxTaps = 1;
                if (taps > maxTaps)
                    taps = maxTaps;
            }
            if (taps != ((r.taps > 0) ? r.taps : 1))
                statMashed++;
            // 终点块没有"下一格"，长按无意义，按普通单键处理
            float hold = (r.holdLen > 0 && hasNext) ? (float)(t1 - t0) : 0.f;
            const int kStart = covered ? 1 : 0;      // 被松手拍覆盖的砖：首拍不再出键
            if (covered)
                statHoldCover++;
            if (kStart >= taps)
                continue;
            if (hold > 0.f)
                lastHoldEv = (int)evs.size();
            for (int k = kStart; k < taps; k++)
            {
                Ev e;
                e.t    = t0 + (t1 - t0) * (double)k / (double)taps;
                e.dt   = e.t - prevTe;
                e.turn = turn / taps;
                e.dir  = dir;
                e.hold = (k == 0) ? hold : 0.f;
                prevTe = e.t;
                evs.push_back(e);
            }
        }

        // ---- 第二遍：分键 ----
        //   经典：4K/5K/6K/10K 四套独立引擎（各自的手型 / 连打语汇）
        //   叠/技/乱/切：四套独立风格算法（同一谱面 + 模式 + 风格 → 稳定输出）
        std::vector<int> lanes(evs.size(), 0);
        std::vector<std::vector<int>> chordExtra;   // 押：每事件的追加键（同刻多键）
        if (style == kStyleClassic)
        {
            State4K  ls4;
            State5K  ls5;
            State6K  ls6;
            State10K ls10;
            for (size_t i = 0; i < evs.size(); i++)
            {
                const Ev& e = evs[i];
                switch (midx)
                {
                case 0:  lanes[i] = NextLane4K(ls4, e.dir, e.turn, e.t, e.hold);    break;
                case 1:  lanes[i] = NextLane5K(ls5, e.dir, e.turn, e.t, e.hold);    break;
                case 2:  lanes[i] = NextLane6K(ls6, e.dir, e.turn, e.t, e.hold);    break;
                default: lanes[i] = NextLane10K(ls10, e.dir, e.turn, e.t, e.hold);  break;
                }
            }
        }
        else
        {
            Gen g;
            g.Init(cols, midx, style, SeedFor(evs, midx, style, cols));
            AssignStyled(g, evs, lanes, &chordExtra);
        }

        // ---- 组装音符 + 手感自检统计 ----
        std::vector<Note> notes;
        notes.reserve(evs.size());
        int statNotes = 0, statStream = 0, statStreamAlt = 0, statJack = 0;
        int statSameHand = 0, statHandRepeat = 0;
        int statPrevHandLane[2] = { -1, -1 };
        int statLane[kMaxLanes] = {};
        int statLaneRun = 0, statLaneRunMax = 0;
        int prevL = -1;
        double prevTn = -1e9;
        for (size_t i = 0; i < evs.size(); i++)
        {
            const Ev& e = evs[i];
            int lane = lanes[i];
            if (lane < 0) lane = 0;
            if (lane >= cols) lane = cols - 1;
            Note nt;
            nt.time = e.t;
            nt.lane = lane;
            nt.hold = e.hold;
            nt.dir  = e.dir;
            nt.turn = (float)e.turn;
            notes.push_back(nt);
            // 押：同刻追加键（双押/三押/四押/五押，来自冰火手法引擎）
            if (i < chordExtra.size() && !chordExtra[i].empty())
            {
                for (int xl : chordExtra[i])
                {
                    if (xl < 0 || xl >= cols || xl == lane) continue;
                    Note nx = nt;
                    nx.lane = xl;
                    nx.hold = 0.f;
                    notes.push_back(nx);
                    statLane[xl]++;
                }
            }

            double dt = nt.time - prevTn;
            bool stream = (dt <= kFastDt && dt > 0.0);
            if (prevL >= 0)
            {
                bool sameLane = (lane == prevL);
                if (sameLane && dt < 0.5)
                    statJack++;
                int hNow = HandOf(lane, cols);
                if (statPrevHandLane[hNow] == lane && dt < kRepeatGuard)
                    statHandRepeat++;
                statPrevHandLane[hNow] = lane;
                int hand = HandOf(lane, cols);
                int prevHand = HandOf(prevL, cols);
                if (stream)
                {
                    statStream++;
                    if (hand != prevHand) statStreamAlt++;
                    else                  statSameHand++;
                }
                if (sameLane) { if (++statLaneRun > statLaneRunMax) statLaneRunMax = statLaneRun; }
                else          statLaneRun = 1;
            }
            statLane[lane]++;
            prevL = lane;
            prevTn = nt.time;
            statNotes++;
        }

        int lv = EstimateLevel(notes, cols);
        s_level.store(lv, std::memory_order_relaxed);

        // 与上一份完全一致就不重建（3 秒周期刷新不应清空连击/按键计数）；
        // 模式或风格切换必须重建
        static int    sigSize = -1;
        static double sigFirst = 0.0, sigLast = 0.0, sigLaneSum = 0.0;
        static int    sigMode = -1, sigStyle = -1, sigCols = -1;
        int modeNow = midx;
        double laneSum = 0.0;
        for (size_t i = 0; i < notes.size(); i++)
            laneSum += notes[i].lane;
        if (!notes.empty() && (int)notes.size() == sigSize &&
            fabs(notes.front().time - sigFirst) < 1e-7 &&
            fabs(notes.back().time - sigLast) < 1e-7 &&
            fabs(laneSum - sigLaneSum) < 1e-9 &&
            modeNow == sigMode && style == sigStyle && cols == sigCols)
            return;
        sigSize  = (int)notes.size();
        sigFirst = notes.empty() ? 0.0 : notes.front().time;
        sigLast  = notes.empty() ? 0.0 : notes.back().time;
        sigLaneSum = laneSum;
        sigMode = modeNow; sigStyle = style; sigCols = cols;

        if (statNotes > 0)
        {
            Log::Printf("[%dK] pattern(%s): notes=%d stream=%d handAlt=%.1f%% sameHand=%d handRepeat=%d jack=%d maxLaneRun=%d mashed=%d",
                        cols, kStyleNames[style], statNotes, statStream,
                        statStream > 0 ? 100.0 * statStreamAlt / statStream : 100.0,
                        statSameHand, statHandRepeat, statJack, statLaneRunMax, statMashed);
            char lb[192];
            int  c = 0;
            for (int i = 0; i < cols; i++)
                c += snprintf(lb + c, sizeof(lb) - c, "%s%d", (i ? "/" : ""), statLane[i]);
            Log::Printf("[%dK] lane use: %s", cols, lb);
            if (style == kStyleAdo && statNotes > 2)
            {
                // 冰火手法纯度自检：高速段里严格 2 键交替（轮指/交互）的占比
                int pure = 0, fast = 0;
                for (size_t i = 2; i < notes.size(); i++)
                {
                    const double dt = notes[i].time - notes[i - 1].time;
                    if (!(dt > 0.0 && dt <= kFastDt)) continue;
                    fast++;
                    if (notes[i].lane != notes[i - 1].lane && notes[i].lane == notes[i - 2].lane)
                        pure++;
                }
                Log::Printf("[%dK] ado purity: %d/%d fast notes in strict 2-key alternation (%.1f%%)",
                            cols, pure, fast, fast ? 100.0 * pure / fast : 0.0);
            }
        }

        {
            std::lock_guard<std::mutex> lk(s_notesMutex);
            s_notes.swap(notes);
            s_chartSize = (int)s_notes.size();
            s_consumed.assign(s_notes.size(), 0);
            s_missed.assign(s_notes.size(), 0);
            s_tailDone.assign(s_notes.size(), 0);
        }
        s_anchored.store(false, std::memory_order_relaxed);   // new chart -> wait for the next real level start
        s_fx.clear();
        for (int i = 0; i < kMaxLanes; i++)
            s_laneFlash[i] = 0.f;
        for (int i = 0; i < 4; i++)
            s_jdCounts[i].store(0, std::memory_order_relaxed);
        for (int i = 0; i < 4; i++)
            s_jdGold[i].store(0, std::memory_order_relaxed);
        s_jdCombo.store(0, std::memory_order_relaxed);
        s_jdTotal.store(0, std::memory_order_relaxed);
        s_jdWeight.store(0.0, std::memory_order_relaxed);
        s_jdLastKind.store(-1, std::memory_order_relaxed);
        s_chartGen.fetch_add(1, std::memory_order_relaxed);
        s_noteCount.store(s_chartSize, std::memory_order_relaxed);
        s_resetKeys.store(true, std::memory_order_relaxed);

        double dur = 0.0;
        double endT = 0.0;
        if (s_chartSize > 0)
        {
            std::lock_guard<std::mutex> lk(s_notesMutex);
            dur = s_notes.back().time - s_notes.front().time;
            endT = s_notes.back().time;
        }
        s_chartEndT.store(endT + 3.0, std::memory_order_relaxed);
        static int s_lastBuilt = -1;
        if (s_chartSize != s_lastBuilt)
        {
            s_lastBuilt = s_chartSize;
            Log::Printf("[%dK] chart built: %d notes, %.1fs, Lv.%d, style=%s (hold-cover=%d, mashed=%d)",
                        cols, s_chartSize, dur, lv, kStyleNames[style], statHoldCover, statMashed);
            DumpChartTxt();
        }
    }

    // 调试用：把转换结果（时刻 / 键位 / 方向 / 转角）导出，便于离线检查手感
    static void DumpChartTxt()
    {
        char path[MAX_PATH];
        const char* base = getenv("LOCALAPPDATA");
        if (!base)
            return;
        snprintf(path, sizeof(path), "%s\\ADOFvec\\4k_chart.txt", base);
        FILE* fp = nullptr;
        if (fopen_s(&fp, path, "w") != 0 || !fp)
            return;
        std::lock_guard<std::mutex> lk(s_notesMutex);
        fprintf(fp, "# notes=%d\n# idx time dt lane hold dir turn chord\n", (int)s_notes.size());
        for (size_t i = 0; i < s_notes.size(); i++)
        {
            double dt = (i == 0) ? 0.0 : s_notes[i].time - s_notes[i - 1].time;
            int grp = 1;
            while (i + (size_t)grp < s_notes.size() &&
                   s_notes[i + (size_t)grp].time == s_notes[i].time) grp++;
            fprintf(fp, "%d %.6f %.6f %d %.3f %d %.4f %d\n",
                    (int)i, s_notes[i].time, dt, s_notes[i].lane,
                    s_notes[i].hold, s_notes[i].dir, s_notes[i].turn, grp);
        }
        fclose(fp);
        Log::Printf("[4K] chart dump -> %s", path);
    }

    // 周期性 / 换谱重新提取（返回是否成功取到谱面）
    static bool ExtractChart(void* ctrl)
    {
        void* items = nullptr;
        int count = 0;
        if (g_lm && oLM_floors)
        {
            void* list = ReadPtrField(g_lm, oLM_floors);
            ListHdr hdr{};
            ReadListHdrRaw(list, &hdr);
            items = hdr.items;
            count = hdr.size;
        }

        static std::vector<RawFloor> raw;
        int got = 0;
        if (items && count > 2)
        {
            raw.assign((size_t)count, RawFloor{});
            got = ReadFloorsRaw(items, count, raw.data(), count);
            s_chartKey = items;
            s_chartFloorN = count;
        }
        else if (ctrl && oCtrl_firstFloor)
        {
            void* first = ReadPtrField(ctrl, oCtrl_firstFloor);
            if (first)
            {
                raw.assign(40000, RawFloor{});
                got = ReadChainRaw(first, raw.data(), (int)raw.size());
                s_chartKey = first;
                s_chartFloorN = got > 0 ? got : 0;
            }
        }

        if (got > 2)
        {
            BuildReadTiles(raw.data(), got);   // 读谱页与 4K/6K 无关，独立成表
            BuildChart(raw.data(), got);
            return true;
        }
        else if (got < 0)
        {
            Log::Printf("[4K] floor read AV — aborted");
            SetDiag("读取地砖时发生访问异常");
        }
        else
        {
            static void* s_lastLogItems = nullptr;
            static int s_lastLogCount = -1;
            if (items != s_lastLogItems || count != s_lastLogCount)
            {
                s_lastLogItems = items;
                s_lastLogCount = count;
                Log::Printf("[4K] extract: lm=%p floors=%p count=%d got=%d ctrl=%p",
                            g_lm, items, count, got, ctrl);
            }
        }
        return false;
    }

    // 时间跳变后的判定重同步（桥接线程调用）：
    //   重开 / 切关 / 跳过前奏时，时钟会突然前后跳 —— 此时把"已经过去"的音符
    //   直接跳过不计 Miss，连击与成绩清零，避免出现"一次漏 100 多个"的假成绩。
    static void ResyncJudge(double clock)
    {
        {
            std::lock_guard<std::mutex> lk(s_notesMutex);
            for (size_t i = 0; i < s_notes.size(); i++)
            {
                bool past = (s_notes[i].time < clock - JudgeWinMs() / 1000.0);
                s_consumed[i] = past ? 1 : 0;
                s_missed[i] = 0;
                s_tailDone[i] = 0;
            }
        }
        s_jdCombo.store(0, std::memory_order_relaxed);
        s_jdTotal.store(0, std::memory_order_relaxed);
        s_jdWeight.store(0.0, std::memory_order_relaxed);
        for (int i = 0; i < 4; i++)
            s_jdCounts[i].store(0, std::memory_order_relaxed);
        for (int i = 0; i < 4; i++)
            s_jdGold[i].store(0, std::memory_order_relaxed);
        s_jdLastKind.store(-1, std::memory_order_relaxed);
        s_chartGen.fetch_add(1, std::memory_order_relaxed);   // 渲染侧清特效/KPS
        Log::Printf("[4K] judge resync @ %.2fs", clock);
    }
    // ---------------- 时钟（渲染线程插值） ----------------
    double WallNow()
    {
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        return (double)c.QuadPart / (double)f.QuadPart;
    }

    // 渲染线程用的歌曲时钟 = 桥接采样值 + QPC 外推 * 实测速率 + 用户偏移
    // （读谱页用自己的偏移，见 RenderClockOff）
    double RenderClockOff(int offMs)
    {
        // 上限 200ms：mono 阻塞/GC 造成采样间隙时，时钟只允许落后 200ms
        // （旧的 50ms 上限会在卡顿时把判定时钟冻结在采样值上 → 判定中心偏移）
        double lead = WallNow() - s_clockWall.load(std::memory_order_relaxed);
        if (lead < 0.0) lead = 0.0;
        if (lead > 0.2) lead = 0.2;
        return s_songTime.load(std::memory_order_relaxed) +
               lead * s_clockRate.load(std::memory_order_relaxed) +
               (double)offMs / 1000.0;
    }
    static double RenderClock()
    {
        int  mi = ActiveModeIndex();
        return RenderClockOff(s_offsetMS[(mi < 0) ? 0 : mi].load(std::memory_order_relaxed));
    }

    // ---------------- 精确按键事件环（桥接线程写 / 渲染线程读） ----------------
    //   GetAsyncKeyState 在渲染线程里一帧只采一次：60fps 时平均 8ms、最坏 17ms
    //   的量化误差，宽判/标准判下会明显表现为"经常按快/按慢"。改为桥接线程
    //   ~7ms 轮询并把事件时间戳换算成歌曲时刻（QPC 外推），判定使用事件自带
    //   的时刻，输入量化误差降到平均 ~3.5ms，和 Malody 的手感一致。
    struct KeyPressEv { int lane; int vk; double songT; double wallT; int kind; };  // kind: 0=按下 1=抬起
    static constexpr unsigned kKeyEvN = 256;
    static KeyPressEv            s_keyEv[kKeyEvN];
    static std::atomic<unsigned> s_keyEvWrite{ 0 };
    static unsigned              s_keyEvRead = 0;                 // 仅渲染线程访问
    static int                   s_capPrevVK[kMaxLanes] = {};
    static bool                  s_capPrevDown[kMaxLanes] = {};
    static double                s_capLastWall = 0.0;      // 桥接线程上一轮采样时刻
    static double                s_capCadence  = 0.0075;   // 实测采样周期 EMA（秒）

    // ---------------- 桥接线程 Tick ----------------
    // ---- 冰火手法管线自检（ADOFAI_PERFECT_GENTEST=1）----
    //   合成 U 级密度事件流（高速轮指 / 交互 / 大回转 / 双押~五押 / 长按压轮），
    //   完整跑一遍分键管线（含押的追加键），导出 %LOCALAPPDATA%\ADOFvec\gentest.txt
    static void GenTestOnce()
    {
        static bool s_done = false;
        if (s_done) return;
        char v[8] = { 0 };
        if (!(GetEnvironmentVariableA("ADOFAI_PERFECT_GENTEST", v, sizeof(v)) > 0 && v[0] == '1'))
            return;
        s_done = true;
        const double kDeg = 3.14159265358979323846 / 180.0;
        std::vector<Ev> evs;
        double t = 0.0;
        auto push = [&](double dt, double turnDeg, uint8_t taps, float hold) {
            Ev e;
            e.t = t; e.dt = evs.empty() ? 0.0 : dt;
            e.turn = turnDeg * kDeg; e.dir = 1; e.hold = hold; e.taps = taps;
            evs.push_back(e); t += dt;
        };
        for (int i = 0; i < 48; i++) push(0.060, 180.0, 1, 0.f);                   // 纯轮指段
        for (int i = 0; i < 3; i++)  push(0.900, 180.0, 1, 0.f);                   // 断句
        for (int i = 0; i < 48; i++) push(0.120, 90.0, 1, 0.f);                    // 纯交互段
        for (int i = 0; i < 96; i++) push(0.058, (i % 12 == 0) ? 270.0 : 120.0, 1, 0.f); // 长轮指（大回转/换手）
        push(0.300, 180.0, 2, 0.f);                                                // 轮指中的双押
        for (int i = 0; i < 20; i++) push(0.058, 120.0, 1, 0.f);                   // 押后继续轮指
        for (int i = 0; i < 8; i++)  push(0.220, 180.0, (uint8_t)(2 + i % 4), 0.f);      // 双押~五押
        for (int i = 0; i < 24; i++) push(0.100, 180.0, 1, (i == 0) ? 1.20f : 0.f);      // 长按 → 押轮
        for (int i = 0; i < 24; i++) push((i & 1) ? 0.072 : 0.063, 180.0, 1, 0.f);       // 迟滞带（不得抖动）
        char path[MAX_PATH];
        const char* base = getenv("LOCALAPPDATA");
        if (!base) return;
        snprintf(path, sizeof(path), "%s\\ADOFvec\\gentest.txt", base);
        FILE* fp = nullptr;
        if (fopen_s(&fp, path, "w") != 0 || !fp) return;
        for (int mi = 0; mi < kModeN; mi++)
        {
            const int cols = kLanesOf[mi];
            Gen g;
            g.Init(cols, mi, kStyleAdo, 12345u);
            std::vector<int> lanes;
            std::vector<std::vector<int>> extra;
            AssignStyled(g, evs, lanes, &extra);
            fprintf(fp, "# mode=%d cols=%d tier=%d\n", mi, cols, g.plan.tier);
            int nChord = 0, nKeys = 0, maxC = 0;
            for (auto& v : extra)
                if (!v.empty())
                {
                    nChord++; nKeys += (int)v.size() + 1;
                    if ((int)v.size() + 1 > maxC) maxC = (int)v.size() + 1;
                }
            fprintf(fp, "# stats: chords=%d keys=%d max=%d\n", nChord, nKeys, maxC);
            for (size_t i = 0; i < evs.size(); i++)
            {
                fprintf(fp, "%d %.4f %d", (int)i, evs[i].t, lanes[i]);
                for (int x : extra[i]) fprintf(fp, " +%d", x);
                fprintf(fp, "\n");
            }
        }
        fclose(fp);
        Log::Printf("[GENTEST] written %s (%d events)", path, (int)evs.size());
    }

    void Tick()
    {
        GenTestOnce();
        if (!MonoApi::Ready())
            return;
        if (!ResolveFields())
            return;

        // 单例引用：每帧从静态数据槽重读（切关卡/退出关卡都会更新）
        // 优先用主线程 get_instance() 发布的实例（可自愈"已销毁对象"），
        // 未发布时退回静态槽位直读
        void* lmMain = s_lmInstMain.load(std::memory_order_acquire);
        void* condMain = s_condInstMain.load(std::memory_order_acquire);
        bool lmPub = s_lmInstPub.load(std::memory_order_acquire);
        bool condPub = s_condInstPub.load(std::memory_order_acquire);
        g_lm = lmPub ? lmMain : (s_lmInstOK ? StaticObj(g_vtLevelMaker, s_lmInstOff) : nullptr);
        g_cond = condPub ? condMain : (s_condInstOK ? StaticObj(g_vtConductor, s_condInstOff) : nullptr);
        {
            // 周期性请主线程刷新实例（mono API 只能主线程；顺带扫描未发现的槽位）
            static DWORD s_lastReq = 0;
            DWORD nowReq = GetTickCount();
            if (nowReq - s_lastReq > 1000)
            {
                s_lastReq = nowReq;
                GameBridge::QueueMainThreadInit();
            }
        }

        ClockData cd{};
        ReadClockRaw(g_cond, &cd);
        s_songStarted.store((g_cond && oC_songStarted)
                                ? ReadBoolField(g_cond, oC_songStarted, false)
                                : false,
                            std::memory_order_relaxed);
        if (cd.ok)
        {
            double prev = s_lastSongT;
            double wall = WallNow();
            static double s_lastWallT = 0.0;
            double dw = wall - s_lastWallT;
            if (s_lastWallT > 0.0 && dw > 0.002 && prev > -1e8)
            {
                double r = (cd.song - prev) / dw;
                // 暂停/冻结时 Δsong≈0，若参与平滑会把外推速率越拉越低 →
                // 恢复后判定时钟"走得慢"→ 全部按早。只在歌曲确实前进时更新。
                if (fabs(cd.song - prev) < 1e-5)
                    r = s_clockRate.load(std::memory_order_relaxed);
                if (r >= 0.0 && r <= 4.0)
                    s_clockRate.store(s_clockRate.load(std::memory_order_relaxed) * 0.9 + r * 0.1,
                                      std::memory_order_relaxed);
            }
            s_lastWallT = wall;
            s_clockWall.store(wall, std::memory_order_relaxed);
            s_songTime.store(cd.song, std::memory_order_relaxed);
            // Does the song clock actually advance? (frozen while paused / not started)
            if (prev > -1e8 && fabs(cd.song - prev) > 0.0008)
                s_clockMovedWall.store(wall, std::memory_order_relaxed);
            // Level-start anchor: StartMusic / Scrub re-anchor dspTimeSong so the song
            // clock jumps to ~0 (fresh start) or to the checkpoint time. An unanchored
            // clock built from the previous level's stale dspTimeSong must never make
            // notes fall before the level really starts, so the clock fallback only
            // engages after a jump, a near-zero clock, or a moving in-range clock.
            {
                double chartEnd = s_chartEndT.load(std::memory_order_relaxed);
                bool jumped = (prev > -1e8 && fabs(cd.song - prev) > 0.35);
                bool nearZero = (cd.song > -0.6 && cd.song < 1.5);
                bool inRange = (chartEnd > 0.0 && cd.song > -0.25 && cd.song < chartEnd);
                bool moving = (wall - s_clockMovedWall.load(std::memory_order_relaxed)) < 0.35;
                if (jumped || nearZero || (inRange && moving))
                    s_anchored.store(true, std::memory_order_relaxed);
            }
            s_bpm.store(cd.bpm, std::memory_order_relaxed);
            // 时间跳变（重开 / 切关 / 跳过前奏）→ 判定重同步
            if (prev > -1e8 && fabs(cd.song - prev) > 0.35)
            {
                s_resetKeys.store(true, std::memory_order_relaxed);
                ResyncJudge(cd.song);
            }
            s_lastSongT = cd.song;
        }

        // ---- 高精度按键采集（带 QPC 校准的歌曲时刻；判定用） ----
        {
            int miCap = ActiveModeIndex();
            if (miCap >= 0)
            {
                const int colsCap = kLanesOf[miCap];
                const int offCap = s_offsetMS[miCap].load(std::memory_order_relaxed);
                const double wallCap = WallNow();
                {
                    const double dCap = wallCap - s_capLastWall;
                    if (dCap > 0.0005 && dCap < 0.25)          // 实测轮询周期（自适应 Sleep 精度）
                        s_capCadence += (dCap - s_capCadence) * 0.05;
                    s_capLastWall = wallCap;
                }
                for (int i = 0; i < colsCap; i++)
                {
                    const int vk = s_vk[miCap][i].load(std::memory_order_relaxed);
                    if (!vk)
                    {
                        s_capPrevVK[i] = 0;
                        s_capPrevDown[i] = false;
                        continue;
                    }
                    if (s_capPrevVK[i] != vk)
                    {
                        s_capPrevVK[i] = vk;      // 键位刚改：只记录状态，不产生事件
                        s_capPrevDown[i] = (GetAsyncKeyState(vk) & 0x8000) != 0;
                        continue;
                    }
                    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
                    if (down != s_capPrevDown[i])
                    {
                        KeyPressEv ev;
                        ev.lane = i;
                        ev.vk = vk;
                        ev.kind = down ? 0 : 1;
                        // GetAsyncKeyState 没有事件时间戳：真实事件时刻平均比检测时刻早半个采样周期
                        ev.songT = RenderClockOff(offCap) - s_capCadence * 0.5;
                        ev.wallT = wallCap;
                        unsigned w = s_keyEvWrite.load(std::memory_order_relaxed);
                        s_keyEv[w % kKeyEvN] = ev;
                        s_keyEvWrite.store(w + 1, std::memory_order_release);
                    }
                    s_capPrevDown[i] = down;
                }
            }
        }

        void* ctrl = GameBridge::GetControllerInstance();
        {
            void* ctrlFresh = s_ctrlInstMain.load(std::memory_order_acquire);
            if (ctrlFresh && ctrlFresh != ctrl)
                ctrl = ctrlFresh;   // 主线程 get_instance 已重新 Find（旧实例被销毁）
        }
        CheatState::Status st;
        GameBridge::GetStatusSnapshot(&st);

        s_inLevel.store(ctrl != nullptr && st.gameworld, std::memory_order_relaxed);
        s_acc.store(st.percentAcc, std::memory_order_relaxed);
        s_combo.store(st.combo, std::memory_order_relaxed);
        s_maxCombo.store(st.maxCombo, std::memory_order_relaxed);

        if (!ctrl)
        {
            s_chartKey = nullptr;
            s_chartFloorN = 0;
            std::lock_guard<std::mutex> lk(s_notesMutex);
            if (!s_notes.empty())
            {
                s_notes.clear();
                s_chartSize = 0;
                s_noteCount.store(0, std::memory_order_relaxed);
            }
            ClearReadTiles();
            return;
        }

        // 谱面识别键：scrLevelMaker.listFloors 的底层数组指针
        void* chartKeyNow = nullptr;
        int   chartCountNow = 0;
        if (g_lm && oLM_floors)
        {
            void* list = ReadPtrField(g_lm, oLM_floors);
            ListHdr hdr{};
            ReadListHdrRaw(list, &hdr);
            chartKeyNow = hdr.items;
            chartCountNow = hdr.size;
        }
        if (!chartKeyNow && ctrl && oCtrl_firstFloor)
            chartKeyNow = ReadPtrField(ctrl, oCtrl_firstFloor);

        static DWORD s_lastExtract = 0;
        static bool  s_lastExtractOK = false;
        static int   s_lastModeSig = -1000;
        int  miNow = ActiveModeIndex();
        int  modeSig = ((miNow < 0) ? 0 : miNow) * 10 +
                       s_style[(miNow < 0) ? 0 : miNow].load(std::memory_order_relaxed);
        if (modeSig != s_lastModeSig)
        {
            s_lastModeSig = modeSig;      // 模式/风格切换：下一帧立即重建谱面
            s_lastExtract = 0;
        }
        DWORD now = GetTickCount();
        DWORD interval = s_lastExtractOK ? 3000 : 150;   // 成功 3s 复检；失败快速重试
        if (chartKeyNow != s_chartKey || chartCountNow != s_chartFloorN ||
            (now - s_lastExtract) > interval)
        {
            s_lastExtract = now;
            s_lastExtractOK = ExtractChart(ctrl);
        }

        // 诊断：每 5 秒打印一次关键引用（定位"读不到谱面"用）
        static DWORD s_lastDiag = 0;
        if (now - s_lastDiag > 5000)
        {
            s_lastDiag = now;
            void* list = (g_lm && oLM_floors) ? ReadPtrField(g_lm, oLM_floors) : nullptr;
            ListHdr hdr{};
            ReadListHdrRaw(list, &hdr);
            void* first = (oCtrl_firstFloor) ? ReadPtrField(ctrl, oCtrl_firstFloor) : nullptr;
            Log::Printf("[4K] diag: ctrl=%p state=%s gw=%d lm=%p cond=%p list=%p floors=%p n=%d firstFloor=%p key=%p notes=%d t=%.2f bpm=%.1f songStart=%d move=%d anch=%d",
                        ctrl, st.stateName, (int)st.gameworld, g_lm, g_cond, list, hdr.items, hdr.size,
                        first, chartKeyNow,
                        s_noteCount.load(std::memory_order_relaxed),
                        s_songTime.load(std::memory_order_relaxed),
                        s_bpm.load(std::memory_order_relaxed),
                        (int)s_songStarted.load(std::memory_order_relaxed),
                        (int)((WallNow() - s_clockMovedWall.load(std::memory_order_relaxed)) < 0.35),
                        (int)s_anchored.load(std::memory_order_relaxed));
        }
    }

    // ---------------- 4K 判定（DFJK，仅渲染线程调用） ----------------

    static void JudgePushFx(int lane, int kind)
    {
        HitFx fx;
        fx.lane = lane;
        fx.kind = kind;
        fx.t0 = ImGui::GetTime();
        fx.scale = (kind == 0) ? 1.12f : (kind == 1 ? 0.98f : 0.84f);
        s_fx.push_back(fx);
        if (s_fx.size() > 64)
            s_fx.erase(s_fx.begin());
        s_laneFlash[lane] = 1.f;
        s_laneHitBg[lane] = (kind <= 1) ? 1.f : 0.55f;
    }

    static void JudgePress(int lane, double clock)
    {
        const double win  = JudgeWinMs()  / 1000.0;
        const double marv = JudgeMarvMs() / 1000.0;
        const double perf = JudgePerfMs() / 1000.0;
        int    best = -1;
        double bestAd = 1e9, bestDt = 0.0;
        {
            std::lock_guard<std::mutex> lk(s_notesMutex);
            for (size_t i = 0; i < s_notes.size(); i++)
            {
                if (s_consumed[i] || s_missed[i])
                    continue;
                const Note& n = s_notes[i];
                if (n.lane != lane)
                    continue;
                double dt = n.time - clock;
                if (dt > win)                // 时间有序：后面的只会更远
                    break;
                double ad = dt < 0 ? -dt : dt;
                if (ad <= win && ad < bestAd)
                {
                    bestAd = ad;
                    best = (int)i;
                    bestDt = dt;
                }
            }
            if (best >= 0)
                s_consumed[best] = 1;
        }
        if (best < 0)
            return;                          // 空打：不扣分（Malody 的 OverPress）
        int kind = (bestAd < marv) ? 0 : (bestAd < perf) ? 1 : 2;
        s_jdCounts[kind].fetch_add(1, std::memory_order_relaxed);
        // 金判（Cynosure hitcount1 = MV）：|offset|<=20ms，与 cynosure.lua 的边界一致
        if (kind <= 2 && bestDt <= 0.020 && bestDt >= -0.020)
            s_jdGold[kind].fetch_add(1, std::memory_order_relaxed);
        s_jdTotal.fetch_add(1, std::memory_order_relaxed);
        s_jdWeight.fetch_add(kJudgeWeight[kind], std::memory_order_relaxed);
        int combo = s_jdCombo.fetch_add(1, std::memory_order_relaxed) + 1;
        int mc = s_jdMaxCombo.load(std::memory_order_relaxed);
        while (combo > mc && !s_jdMaxCombo.compare_exchange_weak(mc, combo)) {}
        s_jdLastKind.store(kind, std::memory_order_relaxed);
        s_jdLastTime.store(clock, std::memory_order_relaxed);
        s_jdLastLane.store(lane, std::memory_order_relaxed);
        s_jdLastOff.store(bestDt, std::memory_order_relaxed);
        SkinLua::PushHit(kind, bestDt * 1000.0);          // 皮肤 Lua：OnHit/HitEvent
        if (bestAd <= 0.09)              // 只统计正常范围内的偏差，避免乱按污染校准
        {
            BiasPush(bestDt);
            // 自动调整延迟：每 16 次命中按平均偏差把判定中心往 0 拉（增益 0.6，避免抖动）
            int miA = ActiveModeIndex();
            if (miA < 0) miA = 0;
            if (s_autoOff[miA].load(std::memory_order_relaxed) && ++s_autoCalN >= 16)
            {
                s_autoCalN = 0;
                int bn = 0;
                double avg = BiasAvgMs(&bn);
                if (bn >= 8 && fabs(avg) > 3.0)
                {
                    int off = s_offsetMS[miA].load(std::memory_order_relaxed) + (int)lround(avg * 0.6);
                    if (off < -400) off = -400;
                    if (off > 400)  off = 400;
                    s_offsetMS[miA].store(off, std::memory_order_relaxed);
                    BiasClear();
                    Log::Printf("[judge] auto offset -> %d ms (avg %.1f ms)", off, avg);
                }
            }
        }
        if (s_hitFx.load(std::memory_order_relaxed))
            JudgePushFx(lane, kind);
    }

    static void JudgeMissScan(double clock)
    {
        int missed = 0;
        int missedLane[16] = {};
        float missedOff[16] = {};
        {
            std::lock_guard<std::mutex> lk(s_notesMutex);
            for (size_t i = 0; i < s_notes.size(); i++)
            {
                if (s_consumed[i] || s_missed[i])
                    continue;
                if (s_notes[i].time < clock - JudgeWinMs() / 1000.0)
                {
                    if (missed < 16)
                    {
                        missedLane[missed] = s_notes[i].lane;
                        missedOff[missed] = (float)((clock - s_notes[i].time) * 1000.0);   // 晚按偏移（ms）
                    }
                    s_missed[i] = 1;
                    missed++;
                }
                else
                    break;
            }
        }
        for (int k = 0; k < missed; k++)
        {
            s_jdCounts[3].fetch_add(1, std::memory_order_relaxed);
            s_jdTotal.fetch_add(1, std::memory_order_relaxed);
            s_jdWeight.fetch_add(0.0, std::memory_order_relaxed);
            s_jdCombo.store(0, std::memory_order_relaxed);
            s_jdLastKind.store(3, std::memory_order_relaxed);
            s_jdLastTime.store(clock, std::memory_order_relaxed);
            SkinLua::PushHit(3, (k < 16) ? missedOff[k] : 0.f);   // 皮肤 Lua：漏键也要走 OnHit/HitEvent
            if (s_hitFx.load(std::memory_order_relaxed))
            {
                HitFx fx;
                fx.lane = (k < 16) ? missedLane[k] : -1;   // 皮肤 Miss 特效按轨播放（Rurudo hit6 等）
                fx.kind = 3;
                fx.t0 = ImGui::GetTime();
                fx.scale = 1.f;
                s_fx.push_back(fx);
                if (s_fx.size() > 64)
                    s_fx.erase(s_fx.begin());
            }
        }
    }

    // ---------------- 长按尾端（松手拍）判定 ----------------
    //   反编译依据：ADOFAI 长按块必须一直按住，松手那一刻 = 下一砖的判定拍
    //   （scrPlayer.UpdateHoldBehavior: 松手且 holdCompletion>=margin → Hit()）。
    //   这里把松手当成一次独立判定：
    //     · 判定窗内松手      → PERFECT（略早）/ 稍早 → GOOD
    //     · 超过判定窗提前松手 → MISS（断连击，长条消失）
    //     · 一直按到尾端不松手 → 自动 PERFECT（Malody 长条手感）
    static void JdTailCount(int kind, double off, double clock, int lane)
    {
        s_jdCounts[kind].fetch_add(1, std::memory_order_relaxed);
        if (kind <= 2 && off <= 0.020 && off >= -0.020)
            s_jdGold[kind].fetch_add(1, std::memory_order_relaxed);
        s_jdTotal.fetch_add(1, std::memory_order_relaxed);
        s_jdWeight.fetch_add(kJudgeWeight[kind], std::memory_order_relaxed);
        int comboNow = 0;
        if (kind == 3)
            s_jdCombo.store(0, std::memory_order_relaxed);
        else
        {
            int combo = s_jdCombo.fetch_add(1, std::memory_order_relaxed) + 1;
            comboNow = combo;
            int mc = s_jdMaxCombo.load(std::memory_order_relaxed);
            while (combo > mc && !s_jdMaxCombo.compare_exchange_weak(mc, combo)) {}
        }
        s_jdLastKind.store(kind, std::memory_order_relaxed);
        s_jdLastTime.store(clock, std::memory_order_relaxed);
        s_jdLastLane.store(lane, std::memory_order_relaxed);
        s_jdLastOff.store(off, std::memory_order_relaxed);
        SkinLua::PushHit(kind, off * 1000.0);             // 皮肤 Lua：OnHit/HitEvent
        if (kind == 3 && s_hitFx.load(std::memory_order_relaxed))
        {
            HitFx fx;
            fx.lane = lane;
            fx.kind = 3;
            fx.t0 = ImGui::GetTime();
            fx.scale = 1.f;
            s_fx.push_back(fx);
            if (s_fx.size() > 64)
                s_fx.erase(s_fx.begin());
        }
    }

    // 松开某键：结算该键上尚未完成的长按尾端
    static void JudgeHoldRelease(int lane, double clock)
    {
        const double win  = JudgeWinMs()  / 1000.0;
        const double perf = JudgePerfMs() / 1000.0;
        std::lock_guard<std::mutex> lk(s_notesMutex);
        for (size_t i = 0; i < s_notes.size(); i++)
        {
            const Note& n = s_notes[i];
            if (n.hold <= 0.f || n.lane != lane)
                continue;
            if (!s_consumed[i] || s_missed[i] || s_tailDone[i])
                continue;
            const double tailT = n.time + (double)n.hold;
            const double dt    = tailT - clock;      // >0 = 提前松手
            if (dt > win)
            {
                s_tailDone[i] = 2;                   // 提前太多：长按拉断 = Miss
                JdTailCount(3, 0.0, clock, lane);
            }
            else
            {
                s_tailDone[i] = 1;
                JdTailCount((dt > perf) ? 2 : 0, dt, clock, lane);
            }
            return;
        }
    }

    // 仍在按住的长按：到尾端即算 PERFECT（不需要玩家刻意松手）
    static void JudgeHoldScan(double clock, const bool* keyDown)
    {
        std::lock_guard<std::mutex> lk(s_notesMutex);
        for (size_t i = 0; i < s_notes.size(); i++)
        {
            const Note& n = s_notes[i];
            if (n.hold <= 0.f || !s_consumed[i] || s_missed[i] || s_tailDone[i])
                continue;
            if (n.lane < 0 || n.lane >= kMaxLanes || !keyDown[n.lane])
                continue;
            const double tailT = n.time + (double)n.hold;
            if (clock < tailT)
                continue;
            s_tailDone[i] = 1;
            JdTailCount(0, 0.0, clock, n.lane);
        }
    }


    // ============================================================
    // 自动打歌 / 宏打歌（内部虚拟按键驱动判定；不发任何系统输入、不碰游戏内存）
    //   · 自动：按下时刻 = 音符时刻（100% MARVELOUS）
    //   · 宏  ：音符时刻 + 拟人抖动（目标精准度 σ + 慢漂移 + 偶发手滑），
    //           长按由虚拟按住交给 JudgeHoldScan 结算
    //   · 派发只依赖"音符自身的 song 时刻"，本帧只把 [clock, clock+lead] 内的
    //     音符发出去，因此不受帧率抖动影响（判定时间戳是精确的）
    // ============================================================
    static double AutoHumanOffsetMs(double winMs, bool macro)
    {
        if (!macro) return 0.0;
        auto urand = []() { s_autoRng = s_autoRng * 1664525u + 1013904223u; return (double)(s_autoRng >> 8) / 16777216.0; };
        const int acc = std::max(90, std::min(100, s_macroAcc.load(std::memory_order_relaxed)));
        const int hum = std::max(0, std::min(100, s_macroHuman.load(std::memory_order_relaxed)));
        const double sigma = (17.0 - 0.14 * (double)(acc - 90)) * (0.30 + 0.70 * (double)hum / 100.0);
        const double u1 = std::max(1e-9, urand()), u2 = urand();
        const double g = std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
        s_macroDrift = s_macroDrift * 0.990 + g * 0.22;                    // 慢漂移（真人节奏感）
        double off = g * sigma + s_macroDrift;
        if (urand() < (double)(100 - acc) * 0.012)                         // 偶发手滑（GOOD 级偏差）
            off += (urand() * 2.0 - 1.0) * winMs * 0.42;
        if (off > winMs * 0.92)  off = winMs * 0.92;
        if (off < -winMs * 0.92) off = -winMs * 0.92;
        return off;
    }

    static void AutoPlayStop()
    {
        for (int i = 0; i < kMaxLanes; i++)
        {
            s_virtualDown[i] = false;
            s_virtualUntil[i] = 0.0;
        }
        s_autoIdx = 0;
        s_macroDrift = 0.0;
    }

    static void AutoPlayTick(double clock, double nowT, double lead)
    {
        const int mi = ActiveModeIndex();
        if (mi < 0) { AutoPlayStop(); return; }
        const bool macro = s_macroPlay[mi].load(std::memory_order_relaxed);
        const bool autoP = s_autoPlay[mi].load(std::memory_order_relaxed);
        if (!macro && !autoP) { AutoPlayStop(); return; }
        const int gen = s_chartGen.load(std::memory_order_relaxed);
        if (gen != s_autoGen) { s_autoGen = gen; AutoPlayStop(); }

        struct Act { int lane; double t; double hold; };
        Act act[24];
        int an = 0;
        {
            std::lock_guard<std::mutex> lk(s_notesMutex);
            const int n = (int)s_notes.size();
            if (s_autoIdx > n) s_autoIdx = 0;
            for (int i = s_autoIdx; i < n && an < 24; i++)
            {
                const Note& nt = s_notes[i];
                if (nt.time > clock + lead) break;
                if (s_missed[i] || s_consumed[i]) { s_autoIdx = i + 1; continue; }
                Act a;
                a.lane = nt.lane;
                a.t = nt.time + AutoHumanOffsetMs(JudgeWinMs(), macro) / 1000.0;
                a.hold = (double)nt.hold;
                act[an++] = a;
                s_autoIdx = i + 1;
            }
        }
        for (int k = 0; k < an; k++)
        {
            JudgePress(act[k].lane, act[k].t);
            s_virtualDown[act[k].lane] = true;
            const double until = act[k].t + act[k].hold + 0.06;
            if (until > s_virtualUntil[act[k].lane]) s_virtualUntil[act[k].lane] = until;
        }
        (void)nowT;
    }

    // 渲染线程手持的判定状态（谱面重建时重置）
    static void ResetJudgeRuntime(int gen)
    {
        s_fx.clear();
        for (int i = 0; i < kMaxLanes; i++)
        {
            s_laneFlash[i] = 0.f;
            s_laneHitBg[i] = 0.f;
        }
        (void)gen;
    }

    // ---------------- 渲染：小工具 ----------------
    static inline float U(float v, float u) { return v * u; }

    void DrawTextColored(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text)
    {
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), pos, col, text);
    }
    // 带阴影的文字（压在游戏自带 UI 上也能看清）
    void DrawTextShadow(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text)
    {
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(pos.x + 1.f, pos.y + 1.f), IM_COL32(0, 0, 0, 170), text);
        { static const char* td = getenv("ADOFAI_PERFECT_TEXTDBG"); if (td && td[0] == '1') { static int tn = 0; if (tn < 120 && text && text[0]) { tn++; Log::Printf("[textdbg] ra=%p pos=%.0f,%.0f txt=%s", _ReturnAddress(), pos.x, pos.y, text); } } }
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), pos, col, text);
    }

    static inline ImTextureRef TRef(void* t)
    {
        return ImTextureRef((ImTextureID)(uintptr_t)t);
    }

    // Malody 数字（combo-*.png / acc-*.png）：字形在 120 高的贴图里的位置
    //   数字  : src(6,27)-(31,71)   %: src(6,31)-(44,71)(50 宽)   .: src(16,64)-(23,72)
    // 字形：UV 归一化 + 源像素宽 sw + 相对数字顶部的基线偏移 dy
    //   数字 : 40x120 贴图，字形 (6,27)-(31,71)      → sw=26 h=45
    //   %    : 50x120 贴图，字形 (6,31)-(43,71)      → sw=38 h=41
    //   .    : 40x120 贴图，字形 (16,64)-(22,71)     → sw=7  h=8
    struct Glyph { int idx; float u0, v0, u1, v1; float sw; float dy; };
    static bool GlyphFor(void** tex, char c, Glyph* g)
    {
        if (c >= '0' && c <= '9')
        {
            int d = c - '0';
            if (!tex[d]) return false;
            *g = { d, 6.f / 40.f, 27.f / 120.f, 32.f / 40.f, 72.f / 120.f, 26.f, 0.f };
            return true;
        }
        if (c == '%')
        {
            if (!tex[10]) return false;
            *g = { 10, 6.f / 50.f, 31.f / 120.f, 44.f / 50.f, 72.f / 120.f, 38.f, 4.f };
            return true;
        }
        if (c == '.')
        {
            if (!tex[11]) return false;
            *g = { 11, 16.f / 40.f, 64.f / 120.f, 23.f / 40.f, 72.f / 120.f, 7.f, 37.f };
            return true;
        }
        return false;
    }

    static float MeasureDigits(void** tex, const char* s, float h)
    {
        float k = h / 45.f;
        float w = 0.f;
        for (const char* p = s; *p; p++)
        {
            Glyph g{};
            if (!GlyphFor(tex, *p, &g))
            {
                if (*p == ' ')
                    w += 10.f * k;
                continue;
            }
            w += g.sw * k + 4.f * k;
        }
        return w;
    }

    static void DrawDigits(ImDrawList* dl, void** tex, ImVec2 pos, const char* s, float h, ImU32 tint)
    {
        float k = h / 45.f;
        float x = pos.x;
        for (const char* p = s; *p; p++)
        {
            Glyph g{};
            if (!GlyphFor(tex, *p, &g))
            {
                if (*p == ' ')
                    x += 10.f * k;
                continue;
            }
            float gw = g.sw * k;
            float y0 = pos.y + g.dy * k;
            float y1 = y0 + (g.v1 - g.v0) * 120.f * k;
            dl->AddImage(TRef(tex[g.idx]), ImVec2(x, y0), ImVec2(x + gw, y1),
                         ImVec2(g.u0, g.v0), ImVec2(g.u1, g.v1), tint);
            x += gw + 4.f * k;
        }
    }

    static void DrawDigitsCenter(ImDrawList* dl, void** tex, ImVec2 center, const char* s,
                                 float h, ImU32 tint)
    {
        float w = MeasureDigits(tex, s, h);
        DrawDigits(dl, tex, ImVec2(center.x - w * 0.5f, center.y), s, h, tint);
    }
    // MSP 皮肤完整字形（每个文件是一个裁好的字形，按宽高比整图缩放）
    static float MeasureFull(const char* s, float h, float aspect)
    {
        float w = 0.f;
        for (const char* p = s; *p; p++)
            w += (*p == ' ') ? h * aspect * 0.4f : h * aspect + h * 0.12f;
        return w;
    }
    static void DrawDigitsFullCenter(ImDrawList* dl, void** tex, ImVec2 center, const char* s,
                                     float h, ImU32 tint, float aspect)
    {
        float x = center.x - MeasureFull(s, h, aspect) * 0.5f;
        for (const char* p = s; *p; p++)
        {
            int idx = -1;
            if (*p >= '0' && *p <= '9') idx = *p - '0';
            else if (*p == '%') idx = 10;
            else if (*p == '.') idx = 11;
            if (idx < 0) { if (*p == ' ') x += h * aspect * 0.4f; continue; }
            if (tex[idx])
                dl->AddImage(TRef(tex[idx]), ImVec2(x, center.y), ImVec2(x + h * aspect, center.y + h),
                             ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), tint);
            x += h * aspect + h * 0.12f;
        }
    }
    // 时间 01:58（Malody 数字 + 手绘冒号）
    static void DrawClock(ImDrawList* dl, void** tex, ImVec2 pos, float h, double seconds)
    {
        if (seconds < 0)
            seconds = 0;
        int total = (int)seconds;
        int mm = total / 60;
        int ss = total % 60;
        char mmss[16];
        snprintf(mmss, sizeof(mmss), "%02d%02d", mm > 99 ? 99 : mm, ss);
        float k = h / 45.f;
        float w = MeasureDigits(tex, "00", h);
        float gap = 14.f * k;
        float x = pos.x;
        DrawDigits(dl, tex, ImVec2(x, pos.y), mmss, h, IM_COL32(255, 255, 255, 255));
        // 冒号
        float colonX = x + w + gap * 0.15f;
        float dot = 6.f * k;
        dl->AddRectFilled(ImVec2(colonX, pos.y + 12.f * k), ImVec2(colonX + dot, pos.y + 12.f * k + dot), IM_COL32(255, 255, 255, 255));
        dl->AddRectFilled(ImVec2(colonX, pos.y + 30.f * k), ImVec2(colonX + dot, pos.y + 30.f * k + dot), IM_COL32(255, 255, 255, 255));
        char ss2[4];
        snprintf(ss2, sizeof(ss2), "%02d", ss);
        DrawDigits(dl, tex, ImVec2(colonX + dot + gap * 0.6f, pos.y), ss2, h, IM_COL32(255, 255, 255, 255));
    }

    // ---- anton 数字字体（anton-0..9.png，76x102 单元，字形 y=4..98）----
    static bool GlyphAnton(void** tex, char c, Glyph* g)
    {
        if (c < '0' || c > '9')
            return false;
        int d = c - '0';
        if (!tex[d])
            return false;
        static const float kX0[10] = { 13, 22, 13, 13, 12, 13, 13, 14, 13, 13 };
        static const float kX1[10] = { 63, 54, 63, 63, 64, 63, 63, 61, 63, 63 };
        const float CW = 76.f, CH = 102.f;
        *g = { d, kX0[d] / CW, 4.f / CH, kX1[d] / CW, 98.f / CH, kX1[d] - kX0[d], 0.f };
        return true;
    }
    static float MeasureAnton(void** tex, const char* s, float h)
    {
        float k = h / 94.f, w = 0.f;
        for (const char* p = s; *p; p++)
        {
            Glyph g{};
            if (GlyphAnton(tex, *p, &g))
                w += g.sw * k + 6.f * k;
        }
        return w;
    }
    static void DrawDigitsAnton(ImDrawList* dl, void** tex, ImVec2 pos, const char* s,
                                float h, ImU32 tint)
    {
        float k = h / 94.f, x = pos.x;
        for (const char* p = s; *p; p++)
        {
            Glyph g{};
            if (!GlyphAnton(tex, *p, &g))
                continue;
            float gw = g.sw * k;
            dl->AddImage(TRef(tex[g.idx]), ImVec2(x, pos.y), ImVec2(x + gw, pos.y + 94.f * k),
                         ImVec2(g.u0, g.v0), ImVec2(g.u1, g.v1), tint);
            x += gw + 6.f * k;
        }
    }
    static void DrawDigitsAntonCenter(ImDrawList* dl, void** tex, ImVec2 center, const char* s,
                                      float h, ImU32 tint)
    {
        float w = MeasureAnton(tex, s, h);
        DrawDigitsAnton(dl, tex, ImVec2(center.x - w * 0.5f, center.y), s, h, tint);
    }
    // 时间 01:58（anton 数字 + 手绘冒号）
    static void DrawClockAnton(ImDrawList* dl, void** tex, ImVec2 pos, float h, double seconds)
    {
        if (seconds < 0)
            seconds = 0;
        int total = (int)seconds;
        int mm = total / 60;
        int ss = total % 60;
        char mmss[16];
        snprintf(mmss, sizeof(mmss), "%02d%02d", mm > 99 ? 99 : mm, ss);
        float k = h / 94.f;
        float w = MeasureAnton(tex, "00", h);
        DrawDigitsAnton(dl, tex, pos, mmss, h, IM_COL32(255, 255, 255, 255));
        float colonX = pos.x + w + 1.f * k;
        float dot = 7.f * k;
        dl->AddRectFilled(ImVec2(colonX, pos.y + 30.f * k),
                          ImVec2(colonX + dot, pos.y + 30.f * k + dot),
                          IM_COL32(255, 255, 255, 255));
        dl->AddRectFilled(ImVec2(colonX, pos.y + 62.f * k),
                          ImVec2(colonX + dot, pos.y + 62.f * k + dot),
                          IM_COL32(255, 255, 255, 255));
        char s2[4];
        snprintf(s2, sizeof(s2), "%02d", ss);
        DrawDigitsAnton(dl, tex, ImVec2(colonX + dot + 2.f * k, pos.y), s2, h,
                        IM_COL32(255, 255, 255, 255));
    }

    struct MspCtx
    {
        const char* dir = nullptr;
        float gw = 0, gh = 0, u = 0;
        float trackL = 0, trackW = 0;      // 轨道矩形（PlayKeyTrackSizes：186u/轨，居中）
        float laneW = 0, judgeY = 0;       // 轨宽 / 判定线（1080p 基准×u）
        float progress = 0.f;              // 关卡进度（进度条模块）
        float score = 0.f;
        int   cols = 4;
        int   combo = 0;
        int   maxCombo = 0;
        int   counts[4] = {};
        int   gold[4] = {};                // 金判（|offset|<=20ms）分档计数（Cynosure MV）
        double acc = 1.0;
        float bpm = 0.f;
        int   level = 0;
        const char* levelName = "";
        double songT = 0, songLen = 0;
        int   judgeKind = -1;
        double judgeAge = 99.0, judgeOff = 0.0;
        int   kps = 0, kpsMax = 0;
        int   nps = 0, npsMax = 0, npsTotal = 0;
        float hp = 1.f;                    // 0..1（判定用血量/生命）
        int   mod = 0;                     // Malody mod 位掩码（本工具无 mod -> 0）
        float trackAngle = 0.f;            // Meta.Mode.Key.angle（3D 轨道倾角，度）
        bool  track3D = false;             // Meta.Mode.Key.use3D
        float trackLen = 0.f;              // 3D 轨道长度（= 预制体 distanceFarEnd 20000u；0 = 非 3D）
        float trackD = 0.f;                // 3D 透视等效相机距离（px，见 Msp3DD）
        float trackScale = 1.f;            // Meta.Mode.Key.scale（轨道缩放；scene TrackScale 用）
        float ringDis = 0.f;               // Meta.Mode.Ring.dis（scene RingDistance 用）
        const bool* keyDown = nullptr;
        const float* laneAct = nullptr;    // 每轨"按下/余韵"强度（0..1，含 0.18s 尾音）
        double lastHitLaneT[kMaxLanes] = {};
        double nowT = 0;
        bool  inPlay = false;
    };

    // 文本占位符替换（{title}/{bpm}/{ver}/{best}...）。未知占位符 → false（该模块不画，
    // 多为 Lua 驱动，例如 Malody 皮肤里用脚本改 Text 的模块）。
    static bool ModSubst(const char* in, const MspCtx& C, char* out, int n)
    {
        int o = 0;
        for (const char* p = in; *p;)
        {
            if (*p == '{')
            {
                const char* e = strchr(p, '}');
                if (!e) return false;
                char key[40];
                int kl = (int)(e - p - 1);
                if (kl <= 0 || kl >= 40) return false;
                memcpy(key, p + 1, kl); key[kl] = 0;
                char rep[160];
                rep[0] = 0;
                if (!_stricmp(key, "title")) snprintf(rep, sizeof(rep), "%s", C.levelName[0] ? C.levelName : "ADOFAI");
                else if (!_stricmp(key, "bpm")) snprintf(rep, sizeof(rep), "%.0f", C.bpm);
                else if (!_stricmp(key, "ver")) snprintf(rep, sizeof(rep), "lv.%d", C.level);
                else if (!_stricmp(key, "level")) snprintf(rep, sizeof(rep), "%d", C.level);
                else if (!_stricmp(key, "player")) snprintf(rep, sizeof(rep), "ADOFAI-PERFECT");
                else if (!_stricmp(key, "combo")) snprintf(rep, sizeof(rep), "%d", C.combo);
                else if (!_stricmp(key, "maxcombo")) snprintf(rep, sizeof(rep), "%d", C.maxCombo);
                else if (!_stricmp(key, "acc")) snprintf(rep, sizeof(rep), "%.2f%%", C.acc * 100.0);
                else if (!_stricmp(key, "best") || !_stricmp(key, "marv")) snprintf(rep, sizeof(rep), "%d", C.counts[0]);
                else if (!_stricmp(key, "cool")) snprintf(rep, sizeof(rep), "%d", C.counts[1]);
                else if (!_stricmp(key, "good")) snprintf(rep, sizeof(rep), "%d", C.counts[2]);
                else if (!_stricmp(key, "miss")) snprintf(rep, sizeof(rep), "%d", C.counts[3]);
                else if (!_stricmp(key, "kcur")) snprintf(rep, sizeof(rep), "%d", C.kps);
                else if (!_stricmp(key, "kmax")) snprintf(rep, sizeof(rep), "%d", C.kpsMax);
                else if (!_stricmp(key, "audio")) snprintf(rep, sizeof(rep), "%02d:%02d", (int)C.songT / 60, (int)C.songT % 60);
                else if (!_stricmp(key, "total") || !_stricmp(key, "length")) snprintf(rep, sizeof(rep), "%02d:%02d", (int)C.songLen / 60, (int)C.songLen % 60);
                else if (!_stricmp(key, "orgtitle")) snprintf(rep, sizeof(rep), "%s", C.levelName[0] ? C.levelName : "ADOFAI");
                else if (!_stricmp(key, "orgart")) snprintf(rep, sizeof(rep), "");
                else if (!_stricmp(key, "creator")) snprintf(rep, sizeof(rep), "%s", s_skinCreator);
                else if (!_stricmp(key, "score")) snprintf(rep, sizeof(rep), "%07d", (int)C.score);
                else if (!_stricmp(key, "hp")) snprintf(rep, sizeof(rep), "%.2f%%", C.hp * 100.f);
                else if (!_stricmp(key, "hp_int")) snprintf(rep, sizeof(rep), "%d", (int)(C.hp * 100.f));
                else if (!_stricmp(key, "progress")) snprintf(rep, sizeof(rep), "%.2f%%", C.progress * 100.f);
                else if (!_stricmp(key, "mod")) snprintf(rep, sizeof(rep), "%d", C.mod);
                else if (!_stricmp(key, "kps")) snprintf(rep, sizeof(rep), "%d", C.kps);
                else if (!_stricmp(key, "kpsmax")) snprintf(rep, sizeof(rep), "%d", C.kpsMax);
                else if (!_stricmp(key, "nps")) snprintf(rep, sizeof(rep), "%d", C.nps);
                else if (!_stricmp(key, "npsmax")) snprintf(rep, sizeof(rep), "%d", C.npsMax);
                else if (!_stricmp(key, "npstotal")) snprintf(rep, sizeof(rep), "%d", C.npsTotal);
                else if (!_stricmp(key, "artist")) snprintf(rep, sizeof(rep), "%s", s_skinCreator);
                else if (!_stricmp(key, "time")) snprintf(rep, sizeof(rep), "%02d:%02d", (int)C.songT / 60, (int)C.songT % 60);
                else return false;
                for (int i = 0; rep[i] && o < n - 1; i++) out[o++] = rep[i];
                p = e + 1;
            }
            else
            {
                if (o >= n - 1) break;
                out[o++] = *p++;
            }
        }
        out[o] = 0;
        return out[0] != 0;
    }

    // ================= MSP 整屏皮肤：模块渲染（数据驱动 · 逆向自 MalodyV GameAssembly） =================
    // 依据（反汇编，非猜测）：
    //  · Malody.Composer.fky::ApplyBasicParam VA 0x595DE0
    //      anchorX = xu==Percent ? x/100 : unit*x/parentW
    //      anchorY = yu==Percent ? y/100 : unit*y/parentH      (Percent 原点左下，1=右/上)
    //      anchoredPos = dxu==Percent ? dx/100*parentW : unit*dx   (y 向上为正)
    //      pivot = fky::CreatePivot(pivot)
    //  · fky::.cctor 逐字节核对 + Vector2 静态量 → CreatePivot 九值：
    //      0=(0,1) 1=(.5,1) 2=(1,1) 3=(0,.5) 4=(.5,.5) 5=(1,.5) 6=(0,0) 7=(.5,0) 8=(1,0)
    //  · fky::ApplyImageSize VA 0x5960B0：w/h 单位同上；0 → 贴图（可见）尺寸并保持比例
    //  · PlayKeyTrackSizes：trackWidth 186u/轨 · trackJudgeHeight 判定线 915u
    //  · WidgetKeyMode.judgeLine / judgeLineY：判定线与轨道贴图由引擎定位
    // 父物体由 SkinLayer 决定。逆向证据：MalodyV level16 场景里 5 个 SkinRuntimeFactory 的挂点
    //   layer=1 Background   → 工厂挂在 RootPlay（全屏父 1920x1080）
    //   layer=4 Above        → 工厂挂在 RootPlay（全屏父）
    //   layer=2 Below        → 工厂挂在 Track 3D/Track（轨道父，整轨宽）
    //   layer=3 PlayFieldAbove → 工厂挂在 Track 3D/Track（轨道父）
    // scene NoteX=n 只表示「属于第 n 轨」（供 Press/Note 触发），不改变父物体。
    static bool MspTrackParent(const SkinMsp::RoleMod& m)
    {
        return (m.layer == 2 || m.layer == 3);
    }
    static float MspParW(const MspCtx& C, const SkinMsp::RoleMod& m)
    {
        if (!MspTrackParent(m)) return C.gw;
        return (C.trackW > 1.f) ? C.trackW : C.gw;
    }
    static float MspParH(const MspCtx& C, const SkinMsp::RoleMod& m)
    {
        if (!MspTrackParent(m)) return C.gh;
        // 3D：轨道父矩形 = 预制体「Track」节点（level16 序列化 sizeDelta.y = 20000 = distanceFarEnd），
        // 百分比高度以轨道长为基准（皮肤用 200% 覆盖整屏，2D 的 judgeY 基准会只画一半 → 实测）。
        if (C.track3D && C.trackLen > 1.f) return C.trackLen;
        // 2D 轨道层父矩形 = 场景 'Track' 节点 rect（Below/Above 以锚点 0..1 拉伸其上）：
        //   sizeDelta.y = 20000（level16/23/26/28 序列化，逆向证据），整个轨道子树再乘 Meta.Key.scale。
        //   像素验证（Mango233 参考截图）：Pas 圈 h=2.1% ⇒ 420u；420 × 0.350313 × k = 137.5px/0.95(贴图可见比)
        //   = 144.7px ⇒ k≈1，与判定圈实测一致。旧实现用 judgeY(=915u) 使黑条只有半屏、圈缩成 20px。
        return 20000.f * C.trackScale * C.u;
    }
    static float MspParX(const MspCtx& C, const SkinMsp::RoleMod& m)
    {
        if (!MspTrackParent(m)) return 0.f;
        return (C.trackW > 1.f) ? C.trackL : 0.f;
    }
    static float MspParY0(const MspCtx& C, const SkinMsp::RoleMod& m)
    {
        if (!MspTrackParent(m)) return C.gh;            // 屏幕父：底边 = 屏幕底（y 向上）
        return (C.judgeY > 1.f) ? C.judgeY : C.gh;      // 轨道父：底边 = 判定线
    }
    // 模块「单位量」标量：Malody 在 SkinModuleBase.Awake（VA 0x18059B3E0）里缓存 *(m+56) = 父矩形高/1080，
    // 该值即 ApplyBasicParam / ApplyImageSize 的 a3 参数（逆向：单位值 = a3×v；百分比 = 父尺寸×v/100）。
    // 屏幕父：gh/1080 = C.u（等价，不变）；2D 轨道父：20000*scale*u/1080（旧实现误用 C.u，
    // 使 CrinoBaka bg2 y=-100u 之类模块偏移差 18.5 倍）。3D 轨道层保持 C.u（透视管线另行标定）。
    static float MspUnit(const MspCtx& C, const SkinMsp::RoleMod& m)
    {
        if (MspTrackParent(m) && !C.track3D) return MspParH(C, m) / 1080.f;
        return C.u;
    }
    // ---- 3D 斜轨道（Meta.Key.use3D + angle）----
    // 依据（逆向 MalodyV level16 场景 WidgetKeyTrack3D 序列化值 + 反汇编，非猜测）：
    //   · rotationAxis=(1,0,0)（level16 410：0xAC=1,0,0）、distancePerDeltaY=10、distanceFarEnd=20000、
    //     distanceMax=80000、distanceScaleByDistanceInView 关键帧 (0,0)(5000,0.5)(20000,1.0)、
    //     trackScale=1、compensationOnScale=1、noteSpeedScale=1（同上对象 0xB8..0x12B）；
    //     trackAngle 由皮肤 Meta.Key.angle 注入（EmoCosine_4K3D = 45）。
    //   · 层级（level16）：Track Anchor(判定线，距父底 250u) → Track(绕 X 轴 angle°、pivot.y=0、
    //     序列化 sizeDelta.y=20000) → Below/Above（皮肤 layer2/3 挂点）。
    //   · 参考相机为透视（field of view 71.45°）→ 平面透视可用平面单应近似：
    //       S(A) = 1/(1 + A·tanθ/D)              横向缩放（判定线处 S=1）
    //       y(A) = judgeY - A·cosθ·S(A)          屏幕 y（向下）
    //       x(x,A) = cx + (x-cx)·S(A)，cx = 屏宽/2（相机主点）
    //     其中 A = 沿轨道距离（单位=判定线处像素），D = 相机到判定线等效距离。
    //   · D 标定：取「far end(20000u) 至少投影到屏幕顶」的最小可行值（覆盖下界，同时给出
    //     该条件下最强透视），并保证屏幕顶处横向缩放 ≥0.28（避免极端针孔导致轨道细如线）。
    static bool Msp3DActive(const MspCtx& C)
    {
        return C.track3D && C.trackAngle > 0.5f && C.trackAngle < 89.5f && C.judgeY > 1.f;
    }
    static float Msp3DTan(const MspCtx& C) { return tanf(C.trackAngle * 3.14159265358979f / 180.f); }
    static float Msp3DCos(const MspCtx& C) { return cosf(C.trackAngle * 3.14159265358979f / 180.f); }
    static float Msp3DD(const MspCtx& C)
    {
        const float tanT = Msp3DTan(C), cosT = Msp3DCos(C);
        if (tanT < 1e-4f) return C.judgeY * 8.f;
        const float farDist = (C.trackLen > 1.f) ? C.trackLen : (20000.f * C.u);
        float D = C.judgeY * 8.f;
        const float ratio = farDist * cosT / C.judgeY;         // far end 到屏幕顶所需
        if (ratio > 1.001f)
        {
            const float d1 = farDist * tanT / (ratio - 1.f);   // 覆盖下界：Δv(far)=judgeY
            if (d1 > 1.f && d1 < D) D = d1;
        }
        {
            const float aTop = C.judgeY / (cosT * 0.28f);      // 屏幕顶处 S=0.28 对应距离
            const float d2 = aTop * tanT / (1.f / 0.28f - 1.f);
            if (d2 > D) D = d2;
        }
        return D;
    }
    static float Msp3DScaleAt(const MspCtx& C, float A)
    {
        const float den = 1.f + A * Msp3DTan(C) / ((C.trackD > 1.f) ? C.trackD : 1.f);
        return (den > 1e-3f) ? (1.f / den) : 1000.f;
    }
    static float Msp3DYAt(const MspCtx& C, float A)
    {
        return C.judgeY - A * Msp3DCos(C) * Msp3DScaleAt(C, A);
    }
    // 距离上限：逆向 Malody WidgetKeyTrack3D.GetNoteDistanceFromDeltaY（VA 0x180312550）——
    //   d = k*dy/(1 - k*dy*q)，分母 <=0 时取无限大，最后 min(d, this+120)；
    //   this+120 = Meta.Mode.Key.distanceMax（level16 序列化 80000），distanceFarEnd = 20000。
    // 旧实现把「地平线前 41%」（D*cos/tan）当地平线截断 → 轨道木板在屏幕中段被硬切
    // （实测 EmoCosine_4K3D 板顶只到 y≈519，上方空洞；用户反馈「斜轨没有全 Y 轴」）。
    static float Msp3DAMax(const MspCtx& C)
    {
        const float farDist = (C.trackLen > 1.f) ? C.trackLen : (20000.f * C.u);
        return farDist * 4.f;                    // distanceMax / distanceFarEnd = 80000/20000
    }
    // 屏幕 y → 沿轨道距离 A（2D 下落轨迹保持原手感，只把绘制搬到轨道平面上）
    static float Msp3DAsolve(const MspCtx& C, float yScreen)
    {
        const float tanT = Msp3DTan(C);
        const float D = (C.trackD > 1.f) ? C.trackD : 1.f;
        const float Y = C.judgeY - yScreen;
        float den = Msp3DCos(C) - Y * tanT / D;
        if (den < 1e-3f) den = 1e-3f;
        float A = Y / den;
        const float amax = Msp3DAMax(C);
        return (A > amax) ? amax : A;
    }
    // 平面坐标（x，A）→ 屏幕
    static void Msp3DPlanePoint(const MspCtx& C, float x, float A, float* ox, float* oy, float* sc)
    {
        const float s = Msp3DScaleAt(C, A);
        *sc = s;
        *ox = C.gw * 0.5f + (x - C.gw * 0.5f) * s;
        *oy = Msp3DYAt(C, A);
    }
    // 平面矩形（x0..x1，A0..A1）→ 屏幕四边形（顺序 LT RT RB LB）
    static void Msp3DPlaneQuad(const MspCtx& C, float x0, float x1, float A0, float A1, ImVec2 out[4])
    {
        const float xs[4] = { x0, x1, x1, x0 };
        const float as[4] = { A0, A0, A1, A1 };
        for (int i = 0; i < 4; i++)
        {
            float ox, oy, sc;
            Msp3DPlanePoint(C, xs[i], as[i], &ox, &oy, &sc);
            out[i] = ImVec2(ox, oy);
        }
    }
    static void Msp3DPoint(const MspCtx& C, float x, float y, float* ox, float* oy, float* sc)
    {
        Msp3DPlanePoint(C, x, Msp3DAsolve(C, y), ox, oy, sc);
    }
    // 轨道层矩形在 3D 下的四角（顺序：LT RT RB LB，屏幕坐标 y 向下）
    static void Msp3DQuad(const MspCtx& C, const SkinMsp::RoleMod& m, ImVec2 p0, ImVec2 p1, ImVec2 pt[4])
    {
        if (!Msp3DActive(C) || !MspTrackParent(m))
        {
            pt[0] = p0; pt[1] = ImVec2(p1.x, p0.y); pt[2] = p1; pt[3] = ImVec2(p0.x, p1.y);
            return;
        }
        float sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3, sc;
        Msp3DPoint(C, p0.x, p0.y, &sx0, &sy0, &sc);
        Msp3DPoint(C, p1.x, p0.y, &sx1, &sy1, &sc);
        Msp3DPoint(C, p1.x, p1.y, &sx2, &sy2, &sc);
        Msp3DPoint(C, p0.x, p1.y, &sx3, &sy3, &sc);
        pt[0] = ImVec2(sx0, sy0); pt[1] = ImVec2(sx1, sy1);
        pt[2] = ImVec2(sx2, sy2); pt[3] = ImVec2(sx3, sy3);
    }
    static void MspAnchorPt(const MspCtx& C, const SkinMsp::RoleMod& m, float* sx, float* sy)
    {
        const float pw = MspParW(C, m), ph = MspParH(C, m);
        float ax = 0, ay = 0, dx = 0, dy = 0;
        const float su = MspUnit(C, m);                 // 单位量标量 = 父矩形高/1080（SkinModuleBase.Awake 逆向）
        ModAbsLen(m.xu, m.x, pw, su, &ax);
        ModAbsLen(m.yu, m.y, ph, su, &ay);
        ModAbsLen(m.dxu, m.dx, pw, su, &dx);
        ModAbsLen(m.dyu, m.dy, ph, su, &dy);
        *sx = MspParX(C, m) + ax + dx;
        *sy = MspParY0(C, m) - ay - dy;
    }
    // ================= 条件求值（逆向自 Malody 皮肤系统，非猜测） =================
    //  来源枚举 SkinModuleScene / SkinModuleTrigger（dump：Malody.SkinFile.Types）
    //  flag：ModuleCondFlag  Equal=0 NotEqual=1 Large=2 Less=3 Occur=4
    //  · scene 必须全部成立；trigger 必须全部成立（trigger 为空 = 常显）
    //  · Judge scene 0..4 = Marv/Best/Cool/Good/Miss（m5judgerrda..e 五张表情图）
    //  · Mod scene 是位掩码（CrinoBaka：Mod!=256 常态 / ==256 慢速 / ==16 冲刺 / ==32 狂暴）
    static bool MspCmp(int flag, double v, double ref)
    {
        switch (flag)
        {
        // 逆向 bec::IsConditionMatch（VA 0x584970 double / 0x584930 int）：
        //  Equal/NotEqual 用 epsilon 比较（double 版 subsd+abs+comisd eps），Large=input>target，
        //  Less=input<target。scene 值常是 float（0.42 这类），必须带容差。
        case 0:  return fabs(v - ref) < 1e-3;
        case 1:  return fabs(v - ref) >= 1e-3;
        case 2:  return v > ref;
        case 3:  return v < ref;
        default: return true;                     // Occur：事件类，在下面单独判定
        }
    }
    // SkinTriggerJudge（逆向自 MalodyV 枚举）：None=0 Best=1 Cool=2 Good=3 Miss=4
    // 本工具 0=Marv 1=Best 2=Good 3=Miss → 依次映射 1..4；尚无判定时 = None(0)（Rurudo 的
    // m5judgerrda=场景0，就是「常态」那张托腮小人）。
    static int MspJudgeScene(const MspCtx& C)
    {
        if (C.judgeKind < 0) return 0;
        if (C.judgeAge > 0.35) return 0;   // 判定表情只保持 0.35s，之后回常态(None=0)
        return (C.judgeKind & 3) + 1;
    }
    static double MspTrigVal(const MspCtx& C, int src)
    {
        switch (src)
        {
        case 1:  return (double)C.score;
        case 2:  return (double)C.combo;
        case 3:  return C.acc * 100.0;
        case 4:  return C.hp * 100.0;
        case 5:  return C.progress * 100.0;
        case 6:  return C.songT;
        case 9: case 10: return C.judgeOff * 1000.0;
        case 11: return (double)C.counts[0];
        case 12: return (double)C.counts[1];
        case 13: return (double)C.counts[2];
        case 14: return (double)C.counts[3];
        case 24: return (double)C.kps;
        case 25: return (double)C.kpsMax;
        case 26: return (double)C.kpsMax;
        case 27: return (double)C.nps;
        case 28: return (double)C.npsMax;
        case 29: return (double)C.npsTotal;
        case 30: case 31: return 0.0;      // TaikoHit/TaikoRemain：非太鼓模式
        case 32: return (double)C.maxCombo;
        case 33: return C.hp * 100.0;      // HP_Int（整数血量）
        case 34: return (double)C.bpm;
        default: return 0.0;
        }
    }
    static bool MspCondScene(const MspCtx& C, const SkinMsp::RoleCond& c)
    {
        switch (c.source)
        {
        // ---- SkinModuleScene 全字段（dump：Malody.Composer.SkinModuleScene）----
        // 1 NoteX：轨道选择器（模块通过 scene 的 N 绑定第 N 轨，不是显隐条件）
        case 1:  return true;
        // 2 NoteY：Catch 模式音符 Y。键模式恒 0（皮肤用 NoteY 的条件按 0 处理）
        case 2:  return MspCmp(c.flag, 0.0, (double)c.val);
        // 3/4/5 Width/Height/Ratio：屏幕尺寸条件（Taiko trackmark2：Width>1920 才显示）
        case 3:  return MspCmp(c.flag, (double)C.gw, (double)c.val);
        case 4:  return MspCmp(c.flag, (double)C.gh, (double)c.val);
        case 5:  return MspCmp(c.flag, (double)(C.gw / ((C.gh > 1.f) ? C.gh : 1.f)), (double)c.val);
        // 6 Platform：SkinModulePlatform None=0 Windows=1 iOS=2 Android=3
        case 6:  return MspCmp(c.flag, 1.0, (double)c.val);
        // 7 NoteType：Key 模式音符类型（Tap/Hold）。音符模块由引擎按音符绘制，不在模块表面板判定
        case 7:  return true;
        // 8 TrackAngle：Meta.Mode.Key.angle（3D 斜轨道皮肤用角度切换模块）
        case 8:  return MspCmp(c.flag, (double)C.trackAngle, (double)c.val);
        // 9 TrackScale：Meta.Mode.Key.scale（轨道缩放）
        case 9:  return MspCmp(c.flag, (double)C.trackScale, (double)c.val);
        // 10 RingDistance：Meta.Mode.Ring.dis（圆形/环形皮肤）
        case 10: return MspCmp(c.flag, (double)C.ringDis, (double)c.val);
        // 11 Arrow：箭头方向。枚举 bbbg.ArrowDirection 经 bec::NoteArrowToInt 映射：
        //   Left(8)→1 · Up(4)→2 · Right(2)→3 · 其余→0(Static)。本工具键谱无方向音符 → 恒 0，
        //   即只显示 Static(0) 的轨道/音符模块，带方向的箭头模块不会同时铺满屏幕。
        case 11: return MspCmp(c.flag, 0.0, (double)c.val);
        // 12 PlayBy：1=Player 2=AutoPlay 3=Replay（dakumi main.lua 用 sc[12=2] 显示 AUTO PLAY）
        case 12: return MspCmp(c.flag, 1.0, (double)c.val);
        case 13: return MspCmp(c.flag, (double)C.mod, (double)c.val);
        case 14: return MspCmp(c.flag, (double)MspJudgeScene(C), (double)c.val);
        // 15 BeatDenom：谱面节拍分母（本工具转换谱为 4 分基准）
        case 15: return MspCmp(c.flag, 4.0, (double)c.val);
        // 16/17 NoteWidth/NoteLen：音符尺寸条件（音符模块专用）
        case 16: case 17: return true;
        default: return true;
        }
    }
    // 轨道上下文：模块若带 scene NoteX=N（皮肤把按键姿势/键光/打击特效按轨拆成多个模块），
    // 则 Press/Note 的"值"在皮肤里恒为 1（Rurudo：rrd1..4 / keylight1..4 / hit2..5 实测），
    // 真正的轨道 = scene 的 N。旧实现直接用 val-1 → 所有模块都跟第 1 轨联动。
    static bool MspCondTrig(const MspCtx& C, const SkinMsp::RoleMod& m, const SkinMsp::RoleCond& c)
    {
        const int lane = (m.lane >= 0) ? m.lane : (int)c.val - 1;
        switch (c.source)
        {
        case 7:            // Press：val = 轨号(1..n)，0 = 任意轨
        {
            if (lane >= 0 && lane < kMaxLanes)
            {
                if (C.laneAct) return C.laneAct[lane] > 0.01f;
                if (C.keyDown) return C.keyDown[lane];
                return false;
            }
            if (C.laneAct) { for (int i = 0; i < C.cols; i++) if (C.laneAct[i] > 0.01f) return true; return false; }
            if (C.keyDown) { for (int i = 0; i < C.cols; i++) if (C.keyDown[i]) return true; }
            return false;
        }
        case 8:            // Judge：1=Best 2=Cool 3=Good 4=Miss（flag 参与比较，NotEqual 照实求值）
            return MspCmp(c.flag, (double)(C.judgeKind + 1), (double)c.val);
        case 9:            // Fast（按下偏快）
            return MspCmp(c.flag, (C.judgeOff > 0.0) ? 1.0 : 0.0, (double)c.val);
        case 10:           // Slow（按下偏慢）
            return MspCmp(c.flag, (C.judgeOff < 0.0) ? 1.0 : 0.0, (double)c.val);
        case 15:           // Note：val = 判定结果(1..4)（Malody 枚举 SkinTriggerJudge 偏移）；
        // 轨道来自 scene NoteX=N。证据：Rurudo hit1 val=1 无 scene（最高判定）、
        // hit2..5 val=2 + NoteX1..4（分轨）、hit6 val=4（Miss）。
        {
            if (C.judgeKind < 0 || C.judgeAge > 0.30) return false;
            if ((C.judgeKind + 1) != (int)c.val) return false;
            if (lane < 0) return true;
            for (size_t i = 0; i < s_fx.size(); i++)
            {
                const HitFx& fx = s_fx[i];
                if (C.nowT - fx.t0 > 0.30) continue;
                if (fx.lane == lane) return true;
            }
            return false;
        }
        default:
            return MspCmp(c.flag, MspTrigVal(C, c.source), (double)c.val);
        }
    }
    static bool MspModVisible(const MspCtx& C, const SkinMsp::RoleMod& m)
    {
        for (int i = 0; i < m.sceneN; i++)
            if (!MspCondScene(C, m.scene[i])) return false;
        for (int i = 0; i < m.trigN; i++)
            if (!MspCondTrig(C, m, m.trig[i])) return false;
        return true;
    }
    struct MspImg
    {
        void* tex = nullptr;
        int nw = 0, nh = 0, bx = 0, by = 0, bw = 0, bh = 0;
        bool ok = false;
    };
    static bool MspImage(const MspCtx& C, const char* file, MspImg* im)
    {
        if (!file || !file[0]) return false;
        im->tex = ModLoadTex(C.dir, file);
        if (!im->tex) return false;
        int nfo[6] = {};
        if (ModTexInfo(C.dir, file, nfo))
        {
            im->nw = nfo[0]; im->nh = nfo[1];
            im->bx = nfo[2]; im->by = nfo[3]; im->bw = nfo[4]; im->bh = nfo[5];
            im->ok = (im->bw > 0 && im->bh > 0);
        }
        return true;
    }
    static void MspSize(const MspCtx& C, const SkinMsp::RoleMod& m, const MspImg* im,
                        bool useVisible, float* ow, float* oh)
    {
        float w = 0, h = 0;
        const float su = MspUnit(C, m);
        ModAbsLen(m.wu, m.w, MspParW(C, m), su, &w);
        ModAbsLen(m.hu, m.h, MspParH(C, m), su, &h);    // 百分比按父矩形；单位量按父矩形高/1080
        if (!im || !im->ok) { *ow = w; *oh = h; return; }
        const float iw = (float)(useVisible ? im->bw : im->nw) * C.u;
        const float ih = (float)(useVisible ? im->bh : im->nh) * C.u;
        if (w <= 0.5f && h <= 0.5f) { w = iw; h = ih; }
        else if (w <= 0.5f) { w = (ih > 0.5f) ? h * (iw / ih) : h; }
        else if (h <= 0.5f) { h = (iw > 0.5f) ? w * (ih / iw) : w; }
        *ow = w; *oh = h;
    }
    static void MspRect(const MspCtx& C, const SkinMsp::RoleMod& m, float w, float h,
                        bool clampInside, ImVec2* p0, ImVec2* p1)
    {
        float sx, sy;
        MspAnchorPt(C, m, &sx, &sy);
        float px, py;
        ModPivotF(m, &px, &py);
        p0->x = sx - px * w; p0->y = sy - py * h;
        p1->x = p0->x + w;    p1->y = p0->y + h;
        if (clampInside && h > 0.5f && h < C.gh * 0.6f)
        {
            if (p0->y < 0.f) { const float d = -p0->y; p0->y += d; p1->y += d; }
            if (p1->y > C.gh) { const float d = p1->y - C.gh; p0->y -= d; p1->y -= d; }
            if (p0->y < 0.f) { const float d = -p0->y; p0->y += d; p1->y += d; }
        }
    }
    static void MspImgQuad(ImDrawList* dl, const MspCtx& C, const SkinMsp::RoleMod& m,
                           const char* file, bool useVisible, bool clampInside, float aMul)
    {
        if (aMul <= 0.004f) return;
        // ModuleParamImage.res（SkinModuleRes：1 歌曲封面 / 2 用户头像 / 3 玩家头像）：
        // 这些贴图由 Malody 运行时注入，皮肤包内往往没有对应 PNG（如 CrinoBaka head、
        // dakumi pause）。本工具用皮肤 Meta.cover 兜底（逆向：Meta field 4 = cover；
        // 实测 CrinoBaka cover=bakahead.jpg、dakumi cover=icon.png，正是头像/封面）。
        if (m.res >= 1 && m.res <= 3 && s_skinCover[0]) file = s_skinCover;
        MspImg im;
        if (!MspImage(C, file, &im)) return;
        float w = 0, h = 0;
        MspSize(C, m, &im, useVisible, &w, &h);
        if (w <= 0.5f || h <= 0.5f) return;
        ImVec2 p0, p1;
        MspRect(C, m, w, h, clampInside, &p0, &p1);
        ImVec2 uv0(0.f, 0.f), uv1(1.f, 1.f);
        if (useVisible && im.ok && im.nw > 0 && im.nh > 0)
        {
            uv0 = ImVec2((float)im.bx / im.nw, (float)im.by / im.nh);
            uv1 = ImVec2((float)(im.bx + im.bw) / im.nw, (float)(im.by + im.bh) / im.nh);
        }
        // ModuleParamImage 8/9 flipx/flipy：交换 UV
        if (m.flipx) { float t = uv0.x; uv0.x = uv1.x; uv1.x = t; }
        if (m.flipy) { float t = uv0.y; uv0.y = uv1.y; uv1.y = t; }
        // ModuleParamImage 13 fill（旧写法 float 比例）：按比例裁切可见段（仅填充类模块 4900 用）
        if (m.type == 4900 && m.fill > 0.001f && m.fill < 0.999f)
            uv1.x = uv0.x + (uv1.x - uv0.x) * m.fill;
        float a = aMul * (m.alpha / 100.f);
        if (a > 1.f) a = 1.f;
        if (a <= 0.004f) return;
        // ModuleParamImage 15 color（#RRGGBB）：Malody 用它给贴图染色。
        // 证据：Cynosure trackbg=#000000 / Mango offset bar=#FFFFFF / Rurudo progresslight=#ffffff；
        // 以及 phi.lua OnHit 的 SetColor（白图 phira-effect-*.png → 黄 252,254,184 / 蓝 185,230,246）。
        // 旧实现只给纯色矩形与文本上色、贴图一律原图 → Phigros 打击特效任何时候都是白的。
        ImU32 col = ModColor(m.color, IM_COL32(255, 255, 255, 255), a);
        { static const char* te = getenv("ADOFAI_PERFECT_MSPTINT"); if (te && te[0]=='1') col = IM_COL32(255,0,255,(int)(a*255.f)); }
        // ModuleBlend.Additive（ModuleParamImage 19=1）：ImGui 无逐绘制加法混合，
        // 用「再叠一遍」近似（Malody 发光类模块 blend 均为 Additive：Taiko hit/gauge、
        // CrinoBaka bg2s/noteline、dakumi hit）。
        const int passes = (m.blend == 1) ? 2 : 1;
        // ModuleParamImage 10 slice（repeated 左/下/右/上，贴图像素）：Unity 9-slice。
        // 逆向依据：fky::ConvertToSlice(Sprite, RepeatedField<int> slice, float scale)。
        // 目标尺寸大于边框时按 9 片绘制，边框不拉伸（Taiko trackbg/press、Gazer 进度条）。
        const bool sliced = (m.sliceN == 4) && (m.slice[0] > 0 || m.slice[1] > 0 || m.slice[2] > 0 || m.slice[3] > 0) &&
                            im.nw > 0 && im.nh > 0 && !(Msp3DActive(C) && MspTrackParent(m)) && (m.rotate % 360) == 0;
        if (sliced)
        {
            const float bp[4] = { (float)m.slice[0], (float)m.slice[1], (float)m.slice[2], (float)m.slice[3] };
            const float bw = p1.x - p0.x, bh = p1.y - p0.y;
            if (bw > 1.f && bh > 1.f)
            {
                const float srcW = useVisible ? (float)(im.bw ? im.bw : im.nw) : (float)im.nw;
                const float srcH = useVisible ? (float)(im.bh ? im.bh : im.nh) : (float)im.nh;
                const float sx0 = useVisible ? (float)im.bx : 0.f, sy0 = useVisible ? (float)im.by : 0.f;
                float sc = w / (srcW > 0.f ? srcW : 1.f);      // 边框按整个贴图→屏幕的缩放
                if (sc <= 0.f) sc = 1.f;
                const float l = bp[0] * sc, r = bp[2] * sc, tp = bp[3] * sc, bt = bp[1] * sc;
                if (l + r < bw && tp + bt < bh)
                {
                    const float xs[4] = { p0.x, p0.x + l, p1.x - r, p1.x };
                    const float ys[4] = { p0.y, p0.y + tp, p1.y - bt, p1.y };
                    const float us[4] = { sx0 / im.nw, (sx0 + bp[0]) / im.nw,
                                          (sx0 + srcW - bp[2]) / im.nw, (sx0 + srcW) / im.nw };
                    const float vs[4] = { sy0 / im.nh, (sy0 + bp[3]) / im.nh,
                                          (sy0 + srcH - bp[1]) / im.nh, (sy0 + srcH) / im.nh };
                    for (int pi = 0; pi < passes; pi++)
                    {
                        const ImU32 cc = (pi == 0) ? col : IM_COL32(255, 255, 255, (int)(a * 140.f));
                        for (int cy = 0; cy < 3; cy++)
                            for (int cx = 0; cx < 3; cx++)
                            {
                                if (xs[cx + 1] - xs[cx] <= 0.01f || ys[cy + 1] - ys[cy] <= 0.01f) continue;
                                dl->AddImage(TRef(im.tex),
                                             ImVec2(xs[cx], ys[cy]), ImVec2(xs[cx + 1], ys[cy + 1]),
                                             ImVec2(us[cx], vs[cy]), ImVec2(us[cx + 1], vs[cy + 1]), cc);
                            }
                    }
                    return;
                }
            }
        }
        // 3D 斜轨道：轨道层模块按透视四角绘制
        if (Msp3DActive(C) && MspTrackParent(m))
        {
            ImVec2 pt[4];
            Msp3DQuad(C, m, p0, p1, pt);
            for (int pi = 0; pi < passes; pi++)
            {
                const ImU32 cc = (pi == 0) ? col : IM_COL32(255, 255, 255, (int)(a * 140.f));
                dl->AddImageQuad(TRef(im.tex), pt[0], pt[1], pt[2], pt[3],
                                 uv0, ImVec2(uv1.x, uv0.y), uv1, ImVec2(uv0.x, uv1.y), cc);
            }
            return;
        }
        if ((m.rotate % 360) != 0)      // ModuleParam.Rotate：Unity Z 旋转（度）→ 屏幕 y 向下时取负角
        {
            const float cx = (p0.x + p1.x) * 0.5f, cy = (p0.y + p1.y) * 0.5f;
            const float rad = (float)(-m.rotate % 360) * 3.14159265358979f / 180.f;
            const float cs = cosf(rad), sn = sinf(rad);
            const float hw = (p1.x - p0.x) * 0.5f, hh = (p1.y - p0.y) * 0.5f;
            const float ox[4] = { -hw, hw, hw, -hw }, oy[4] = { -hh, -hh, hh, hh };
            ImVec2 pt[4];
            for (int i = 0; i < 4; i++)
                pt[i] = ImVec2(cx + ox[i] * cs - oy[i] * sn, cy + ox[i] * sn + oy[i] * cs);
            for (int pi = 0; pi < passes; pi++)
            {
                const ImU32 cc = (pi == 0) ? col : IM_COL32(255, 255, 255, (int)(a * 140.f));
                dl->AddImageQuad(TRef(im.tex), pt[0], pt[1], pt[2], pt[3],
                                 uv0, ImVec2(uv1.x, uv0.y), uv1, ImVec2(uv0.x, uv1.y), cc);
            }
            return;
        }
        for (int pi = 0; pi < passes; pi++)
        {
            const ImU32 cc = (pi == 0) ? col : IM_COL32(255, 255, 255, (int)(a * 140.f));
            dl->AddImage(TRef(im.tex), p0, p1, uv0, uv1, cc);
        }
    }
    // 数字字体：逆向 UIDynamicNumber（VA 0x8D5CF0 字符分发）字符 → 字形索引
    //   '0'..'9' → 0..9 · '%' → 10 · '.' → 11（其余字符无字形，退回系统字体）
    //   文件 = Filebase + (Filestart + index) + .png（ModuleParamNumber: 5=filebase 6=filestart）
    static bool MspGlyphFile(const SkinMsp::RoleMod& m, char ch, char* out, int n)
    {
        int idx = -1;
        if (ch >= '0' && ch <= '9') idx = ch - '0';
        else if (ch == '%') idx = 10;
        else if (ch == '.') idx = 11;
        if (idx < 0) return false;
        snprintf(out, n, "%s%d.png", m.numBase, m.start + idx);
        return true;
    }
    // 逆向 UIDynamicNumber::set_CharValue VA 0x8D5E10：每字符 sizeDelta.x = 字形宽×scale，
    //  下一字符 x 累加 (charPadding×scale + 字形宽×scale)，scale = displayHeight / 字形可见高
    //  ⇒ 字距 pad 必须与字形同 scale；字号 displayHeight = 父矩形高 × Height%（SkinModuleNumber）
    static float MspGlyphPad(const SkinMsp::RoleMod& m, float hgt, const MspImg& g)
    {
        return (g.bh > 0) ? (m.numPad * (hgt / (float)g.bh)) : 0.f;
    }
    static void MspDrawNumber(ImDrawList* dl, const MspCtx& C, const SkinMsp::RoleMod& m,
                              const char* txt, float aMul)
    {
        if (!txt || !txt[0] || aMul <= 0.004f) return;
        char line[64];
        snprintf(line, sizeof(line), "%s", txt);
        const int n = (int)strlen(line);
        if (n <= 0 || n > 48) return;
        float hgt = m.numH * (MspParH(C, m) / 100.f);
        if (hgt < 6.f * C.u) hgt = 6.f * C.u;
        static MspImg gl[48];
        static char   gf[48][96];
        static bool   gok[48];
        bool allOk = true;
        float total = 0.f;
        int   refBy = -1, refBh = 0;            // 字形基线参考（min by）与字体参考高（max 可见高）
        for (int i = 0; i < n; i++)
        {
            gok[i] = MspGlyphFile(m, line[i], gf[i], sizeof(gf[i])) &&
                     MspImage(C, gf[i], &gl[i]) && gl[i].ok && gl[i].bh > 0;
            if (!gok[i]) { allOk = false; break; }
            if (refBy < 0 || gl[i].by < refBy) refBy = gl[i].by;
            if (gl[i].bh > refBh) refBh = gl[i].bh;
        }
        // 逆向 Unity UIDynamicNumber：字符占位 = 字形单元（nw × nh）缩放，
        // 推进 = 单元宽×scale + charPadding×scale。两个常见错误：
        //   ① 按裁剪后宽度推进 → numPad 负值时每字重叠 ~27%；
        //   ② 按每字可见高自适应缩放 → '.' 字形被放大到一个字宽（中间空洞）。
        // 正确做法：字号只由字体基准高决定，各字符保持自身基线。
        const float gscAll = (refBh > 0) ? hgt / (float)refBh : 1.f;
        for (int i = 0; i < n; i++)
            total += (float)gl[i].nw * gscAll + MspGlyphPad(m, hgt, gl[i]);
        float sx, sy;
        MspAnchorPt(C, m, &sx, &sy);
        float px, py;
        ModPivotF(m, &px, &py);
        float a = aMul * (m.alpha / 100.f);
        if (a > 1.f) a = 1.f;
        // [debug] 数字模块绘制跟踪（ADOFAI_PERFECT_NUMDBG=1）
        {
            static const char* s_nd = getenv("ADOFAI_PERFECT_NUMDBG");
            if (s_nd && s_nd[0] == '1' && m.desc[0] && strstr(m.desc, "acc"))
            {
                static int s_nc = 0;
                if (s_nc < 400) { s_nc++; Log::Printf("[numdbg] d=%s x=%.1f y=%.1f hgt=%.1f total=%.1f allOk=%d txt=%s", m.desc, sx, sy, hgt, total, (int)allOk, line); }
            }
        }
        ImU32 col = IM_COL32(255, 255, 255, (int)(a * 255.f));
        { static const char* te = getenv("ADOFAI_PERFECT_MSPTINT"); if (te && te[0]=='1') col = IM_COL32(255,0,255,(int)(a*255.f)); }
        if (!allOk)
        {
            float size = hgt * 0.86f;
            if (size < 8.f * C.u) size = 8.f * C.u;
            ImGui::PushFont(nullptr, size);
            ImVec2 ts = ImGui::CalcTextSize(line);
            ImVec2 p(sx - px * ts.x, sy - py * ts.y);
            DrawTextShadow(dl, p, col, line);
            ImGui::PopFont();
            return;
        }
        float x = sx - px * total;
        float y = sy - py * hgt;      // 不夹取：按 Malody 原始锚点绘制（允许超出屏幕）
        for (int i = 0; i < n; i++)
        {
            const float cw = (float)gl[i].nw * gscAll, ch = (float)gl[i].nh * gscAll;
            const float yc = y - (float)refBy * gscAll;     // 各字符单元顶对齐（保持各自基线）
            dl->AddImage(TRef(gl[i].tex), ImVec2(x, yc), ImVec2(x + cw, yc + ch),
                         ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), col);
            x += cw + MspGlyphPad(m, hgt, gl[i]);
        }
    }
    // ================= 模块内置动画（info.asm Module.animations，field 20） =================
    // 逆向依据：Malody.Composer.AnimateType 枚举
    //   None0 MoveX1 MoveY2 Move3 SizeW4 SizeH5 Size6 ScaleX7 ScaleY8 Scale9 Alpha10 RotateZ11
    // ModuleAnimation 字段：startTime/endTime · fromVal0/toVal0 · fromVal1/toVal1 · repeat ·
    //   repeatType · delay · ease · nid（dump：SkinFile.Types.ModuleAnimation）。
    // 时间单位：皮肤写入的 2.0 → 2 秒；500 → 毫秒（>100 视为毫秒，两款实测皮肤均符合）。
    // repeatType=1 = 往返（ping-pong），0 = 重播；repeat=0 = 无限循环。
    // 动画值语义：Move*/Size* 以 u(1080p 基准像素) 为单位；Scale* 为倍率；Alpha 为 0..100；
    //   RotateZ 为角度。基值 = 模块自身声明值（叠加 Move/Scale，覆盖 Size/Alpha/Rotate）。
    static float MspEase(int ease, float t)
    {
        if (t < 0.f) t = 0.f;
        if (t > 1.f) t = 1.f;
        switch (ease)
        {
        case 0:  return t * t;                                     // In
        case 2:  return 1.f - (1.f - t) * (1.f - t);               // Out
        case 3:  return (t < 0.5f) ? (2.f * t * t) : (1.f - 2.f * (1.f - t) * (1.f - t));
        default: return t;                                         // Linear（ease=1）
        }
    }
    // baseSec < -1e8 时用 C.songT；否则用给定时间基准（Lua Play() 的内置动画从 playT0 起算）
    static void MspApplyAnims(const MspCtx& C, const SkinMsp::RoleMod& m, SkinMsp::RoleMod* d,
                              double baseSec = -1e9)
    {
        const double nowSec = (baseSec < -1e8) ? C.songT : baseSec;
        for (int i = 0; i < m.animN; i++)
        {
            const SkinMsp::RoleAnim& a = m.anims[i];
            if (a.type <= 0 || a.type > 11) continue;
            const float t0 = (a.startMs > 100) ? (float)a.startMs / 1000.f : (float)a.startMs;
            const float t1 = (a.endMs > 100) ? (float)a.endMs / 1000.f : (float)a.endMs;
            const float dly = (a.delay > 100) ? (float)a.delay / 1000.f : (float)a.delay;
            const float dur = t1 - t0;
            if (dur <= 0.0001f) continue;
            float u = (float)(nowSec - t0 - dly) / dur;
            if (u < 0.f) u = 0.f;
            if (a.repeat > 0 && u > (float)a.repeat) u = (float)a.repeat;
            float ph = u - floorf(u);                              // 每周期 0..1
            if (a.repeatType == 1) ph = (ph < 0.5f) ? (ph * 2.f) : (2.f - ph * 2.f);   // 往返
            const float v0 = a.from0 + (a.to0 - a.from0) * MspEase(a.ease, ph);
            switch (a.type)
            {
            case 1:  d->dx += v0 * C.u; break;                     // MoveX
            case 2:  d->dy += v0 * C.u; break;                     // MoveY
            case 3:                                                // Move（x+y）
            {
                const float v1 = a.from1 + (a.to1 - a.from1) * MspEase(a.ease, ph);
                d->dx += v0 * C.u; d->dy += v1 * C.u;
                break;
            }
            case 4:  d->w = v0; d->wu = 1; break;                  // SizeW（u）
            case 5:  d->h = v0; d->hu = 1; break;                  // SizeH（u）
            case 6:  d->w = v0; d->wu = 1; d->h = v0; d->hu = 1; break;   // Size
            case 7:                                                // ScaleX
                if (d->wu == 0) { d->w = (d->w > 0.f) ? d->w * v0 : 100.f * v0; }
                else d->w = d->w * v0;
                break;
            case 8:                                                // ScaleY
                if (d->hu == 0) { d->h = (d->h > 0.f) ? d->h * v0 : 100.f * v0; }
                else d->h = d->h * v0;
                break;
            case 9:                                                // Scale（等比）
                if (d->wu == 0) d->w = (d->w > 0.f) ? d->w * v0 : 100.f * v0; else d->w = d->w * v0;
                if (d->hu == 0) d->h = (d->h > 0.f) ? d->h * v0 : 100.f * v0; else d->h = d->h * v0;
                break;
            case 10: d->alpha = (int)(d->alpha * (v0 / 100.f)); break;      // Alpha（0..100）
            case 11: d->rotate = d->rotate + (int)v0; break;               // RotateZ（度）
            default: break;
            }
        }
    }
    static void MspDrawModule(ImDrawList* dl, const MspCtx& C, const SkinMsp::RoleMod& m, float aMul)
    {
        if (m.alpha <= 0 || aMul <= 0.004f) return;
        // [debug] MSP 模块矩形追踪（ADOFAI_PERFECT_MSPDBG=1：每个模块每次加载只输出 1 条）
        {
            static const char* s_env = getenv("ADOFAI_PERFECT_MSPDBG");
            if (s_env && s_env[0] == '1')
            {
                static char s_seen[SkinMsp::kModMax][40];
                static int  s_seenN = 0;
                static int  s_seenGen = 0;
                if (s_seenGen != s_modN) { s_seenGen = s_modN; s_seenN = 0; }   // 换皮肤后重置
                bool dup = false;
                for (int q = 0; q < s_seenN; q++)
                    if (strncmp(s_seen[q], m.desc[0] ? m.desc : "?", 39) == 0) { dup = true; break; }
                if (!dup && s_seenN < SkinMsp::kModMax)
                {
                    snprintf(s_seen[s_seenN], 40, "%s", m.desc[0] ? m.desc : "?");
                    s_seenN++;
                    float sx, sy, px, py;
                    MspAnchorPt(C, m, &sx, &sy);
                    ModPivotF(m, &px, &py);
                    float w = 0, h = 0;
                    ModAbsLen(m.wu, m.w, MspParW(C, m), MspUnit(C, m), &w);
                    ModAbsLen(m.hu, m.h, MspParH(C, m), MspUnit(C, m), &h);
                    char dbg[160] = { 0 };
                    if (m.type == 5001) ModSubst(m.text, C, dbg, sizeof(dbg));
                    else if (m.type == 5003) snprintf(dbg, sizeof(dbg), "%s", m.numText);
                    Log::Printf("[mspdbg] t=%d d=%s txt=[%s] x=%.1f y=%.1f pv=%d w=%.1f h=%.1f ax=%.2f ay=%.2f a=%d l=%d sd=%d tg=%d hd=%d bx=%d",
                                m.type, m.desc[0] ? m.desc : "-", dbg, sx, sy, m.pivot, w, h, px, py, m.alpha,
                                m.layer, m.sceneN, m.trigN, m.hide, m.blend);
                }
            }
        }
        switch (m.type)
        {
        case 5000:
        case 4600:            // SkinModuleSubType.CustomBar：按贴图渲染（Taiko barline 等）
            MspImgQuad(dl, C, m, m.img, false, false, aMul);
            break;
        case 5002:
        {
            const char* f = m.img;
            char fr[128];
            if (m.base[0] && m.frames > 1 && m.fps > 0)
            {
                // ModuleParamImage 5 repeat：0=无限循环；>0 播放 repeat 遍后定格末帧
                double tt = C.nowT * (double)m.fps;
                if (m.repeat > 0 && tt >= (double)m.frames * (double)m.repeat)
                    tt = (double)m.frames * (double)m.repeat - 1e-6;
                int k = (int)fmod(tt, (double)m.frames);
                if (k < 0) k = 0;
                snprintf(fr, sizeof(fr), "%s%d.png", m.base, m.start + k);
                f = fr;
            }
            MspImgQuad(dl, C, m, f, false, false, aMul);
            break;
        }
        case 5004:
        {
            const float ppw = MspParW(C, m);
            float w = 0, h = 0;
            ModAbsLen(m.wu, m.w, ppw, MspUnit(C, m), &w);
            ModAbsLen(m.hu, m.h, MspParH(C, m), MspUnit(C, m), &h);
            // 亚像素细线（Gazer/Phigros 的判定线只有 0.3~0.5px）：至少 1 物理像素，
            // 等价 Unity 的抗锯齿细线（否则 w<=0.5||h<=0.5 直接被丢弃，判定线完全不可见）
            if (w > 0.f && w < 1.f) w = 1.f;
            if (h > 0.f && h < 1.f) h = 1.f;
            const bool prog = ModHasTrig(m, 5) || (m.desc[0] && strstr(m.desc, "progress"));
            if (prog)
            {
                float pr = C.progress;
                if (pr < 0.f) pr = 0.f;
                if (pr > 1.f) pr = 1.f;
                if (w < 5.f * C.u)
                {
                    float sx, sy;
                    MspAnchorPt(C, m, &sx, &sy);
                    float px, py;
                    ModPivotF(m, &px, &py);
                    const float mx = MspParX(C, m) + pr * ppw;
                    ImVec2 p0(mx - px * w, sy - py * h), p1(p0.x + w, p0.y + h);
                    dl->AddRectFilled(p0, p1,
                                      ModColor(m.color, IM_COL32(255, 255, 255, 255), aMul * (m.alpha / 100.f)));
                    break;
                }
                w *= pr;
            }
            if (w <= 0.5f || h <= 0.5f) break;
            ImVec2 p0, p1;
            MspRect(C, m, w, h, false, &p0, &p1);
            {
                const ImU32 rc = ModColor(m.color, IM_COL32(255, 255, 255, 255), aMul * (m.alpha / 100.f));
                if (Msp3DActive(C) && MspTrackParent(m))
                {
                    ImVec2 pt[4];
                    Msp3DQuad(C, m, p0, p1, pt);
                    dl->AddQuadFilled(pt[0], pt[1], pt[2], pt[3], rc);
                }
                else dl->AddRectFilled(p0, p1, rc);
            }
            break;
        }
        case 4900:            // HP 填充（usage=4）：按血量比例裁切贴图（不拉伸）
        {
            MspImg im;
            if (!MspImage(C, m.img, &im)) break;
            float w = 0, h = 0;
            MspSize(C, m, &im, false, &w, &h);
            if (w <= 0.5f || h <= 0.5f) break;
            // 填充量：usage=4(HP) → 血量；含 Progress 触发且无 HP 触发 → 歌曲进度
            // （Malody 的 4900 是通用填充图，由引擎按绑定值填充）
            float hp = m.fillSet ? m.fillVal : C.hp;
            if (!m.fillSet && !ModHasTrig(m, 4) && ModHasTrig(m, 5)) hp = C.progress;
            if (hp < 0.f) hp = 0.f;
            if (hp > 1.f) hp = 1.f;
            float sx, sy;
            MspAnchorPt(C, m, &sx, &sy);
            float px, py;
            ModPivotF(m, &px, &py);
            const ImVec2 p0(sx - px * w, sy - py * h);
            float a = aMul * (m.alpha / 100.f);
            if (a > 1.f) a = 1.f;
            if (hp > 0.004f)
            {
                // ModuleParamFill：0 FromLeft（贴左边裁切）/ 1 FromRight（贴右边裁切）
                // 裁切四角必须一起走旋转/3D 透视：Malody 的 4900 同样受 ModuleParam.Rotate
                // 影响（证据：Mango 的 hp-p/time-p rot=90。旧实现忽略旋转，两个 1290x20 的
                // 条被水平拉伸成横贯全屏的白线 → shot_mango3.png 实测）。
                const ImU32 cc = IM_COL32(255, 255, 255, (int)(a * 255.f));
                const bool fromRight = (m.fillDir == 1);
                const float rx0 = fromRight ? (p0.x + w * (1.f - hp)) : p0.x;
                const float rx1 = fromRight ? (p0.x + w) : (p0.x + w * hp);
                const float u0 = fromRight ? (1.f - hp) : 0.f;
                const float u1 = fromRight ? 1.f : hp;
                ImVec2 q[4] = { ImVec2(rx0, p0.y), ImVec2(rx1, p0.y), ImVec2(rx1, p0.y + h), ImVec2(rx0, p0.y + h) };
                if (Msp3DActive(C) && MspTrackParent(m))
                {
                    for (int i = 0; i < 4; i++)
                    {
                        float ox, oy, sc;
                        Msp3DPoint(C, q[i].x, q[i].y, &ox, &oy, &sc);
                        q[i] = ImVec2(ox, oy);
                    }
                }
                else if ((m.rotate % 360) != 0)
                {
                    const float rcx = p0.x + w * 0.5f, rcy = p0.y + h * 0.5f;
                    const float rad = (float)(-m.rotate % 360) * 3.14159265358979f / 180.f;
                    const float cs = cosf(rad), sn = sinf(rad);
                    for (int i = 0; i < 4; i++)
                    {
                        const float ox = q[i].x - rcx, oy = q[i].y - rcy;
                        q[i] = ImVec2(rcx + ox * cs - oy * sn, rcy + ox * sn + oy * cs);
                    }
                }
                dl->AddImageQuad(TRef(im.tex), q[0], q[1], q[2], q[3],
                                 ImVec2(u0, 0.f), ImVec2(u1, 0.f), ImVec2(u1, 1.f), ImVec2(u0, 1.f), cc);
            }
            break;
        }
        case 5001:
        {
            char txt[192];
            if (!ModSubst(m.text, C, txt, sizeof(txt))) break;
            // 引擎绑定的连击文本：0 连击时不显示（Malody 行为：无连击 → 不画）
            if (!strcmp(m.text, "{combo}") && C.combo <= 0) break;
            // Lua 驱动的计分模块（Rurudo：best/marv 的原生文本恒为 "0"）→ 用本工具判定计数覆盖
            // Cynosure（cynosure.lua 107-111/238-267）：hitcount1..5 = MV(金判)/PF/GR/GD/MS 实时计数；
            // 初始文本只是标签，Lua 在 OnHit 里改写为数字 —— 不执行 Lua，这里直接用本工具判定还原。
            if (m.desc[0] && !strncmp(m.desc, "hitcount", 8) && m.desc[8] >= '1' && m.desc[8] <= '5' && !m.desc[9])
            {
                const int hc = m.desc[8] - '1';
                int v = 0;
                if (hc == 0)      v = C.gold[0] + C.gold[1] + C.gold[2];              // MV：|offset|<=20ms
                else if (hc == 1) v = C.counts[0] - C.gold[0];                        // PF
                else if (hc == 2) v = C.counts[1] - C.gold[1];                        // GR
                else if (hc == 3) v = C.counts[2] - C.gold[2];                        // GD
                else              v = C.counts[3];                                    // MS
                // 与 cynosure.lua 一致：计数为 0 时保留初始标签（MV/PF/...），首次命中后才变数字
                if (v > 0) snprintf(txt, sizeof(txt), "%d", v);
            }
            else if (m.desc[0] && strstr(m.desc, "marv")) snprintf(txt, sizeof(txt), "%d", C.counts[0]);
            else if (m.desc[0] && strstr(m.desc, "best")) snprintf(txt, sizeof(txt), "%d", C.counts[1]);
            // Rurudo Lua 里由脚本写入的 KPS 文本（模块原生文本是 "0" / "/0"）
            else if (m.desc[0] && !strcmp(m.desc, "currentkps")) snprintf(txt, sizeof(txt), "%d", C.kps);
            else if (m.desc[0] && !strcmp(m.desc, "maxkps")) snprintf(txt, sizeof(txt), "/%d", C.kpsMax);
            // tsize 缺省(0) 的 Text 模块在 Malody 里不可见：皮肤拿它当纯数据载体
            // （Rurudo luaacc：x=50% y=50% text={acc}，Lua 只读取它算结算音频）。
            if (m.size <= 0.001f) break;
            float size = m.size * (C.gh / 100.f);
            if (size < 8.f * C.u) size = 8.f * C.u;
            ImGui::PushFont(nullptr, size);
            ImVec2 ts = ImGui::CalcTextSize(txt);
            float sx, sy;
            MspAnchorPt(C, m, &sx, &sy);
            // Rurudo 信息栏 title/bpm/ver：皮肤数据里三行 x 不同（编辑器拖拽误差），
            // 按 title 的左边缘对齐，避免"左边文字没有对齐"
            // 仅限 Rurudo 系：其他皮肤的 ver/bpm 有自己的位置
            // （Phigros ver 在屏幕右下，被旧 hack 拖到左下与 title 重叠）。
            if (s_skinRrd && (!strcmp(m.desc, "bpm") || !strcmp(m.desc, "ver")))
            {
                for (int ti = 0; ti < s_modN; ti++)
                {
                    const SkinMsp::RoleMod& tm = s_mods[ti];
                    if (strcmp(tm.desc, "title") != 0) continue;
                    float tx2, ty2;
                    MspAnchorPt(C, tm, &tx2, &ty2);
                    if (tx2 > 1.f) sx = tx2;
                    break;
                }
            }
            float px, py;
            ModPivotF(m, &px, &py);
            ImVec2 p(sx - px * ts.x, sy - py * ts.y);
            { ImU32 tc = ModColor(m.color, IM_COL32(255, 255, 255, 255), aMul * (m.alpha / 100.f)); static const char* te2 = getenv("ADOFAI_PERFECT_MSPTINT"); if (te2 && te2[0]=='1') tc = IM_COL32(255,0,255,(int)(255*aMul*(m.alpha/100.f))); DrawTextShadow(dl, p, tc, txt); }
            ImGui::PopFont();
            break;
        }
        case 5003:
        {
            char val[64];
            val[0] = 0;
            // 连击数字：0 连击不显示（Rurudo 的 combok 在 Lua 里也是空串）
            {
                const char* dn = m.desc[0] ? m.desc : m.numBase;
                if (strstr(dn, "combo") && C.combo <= 0) break;
            }
            if (m.numText[0] && !ModSubst(m.numText, C, val, sizeof(val))) val[0] = 0;
            if (!val[0])
            {
                const char* d = m.desc[0] ? m.desc : m.numBase;
                if (strstr(d, "acc")) snprintf(val, sizeof(val), "%.2f%%", C.acc * 100.0);
                else if (strstr(d, "combo")) snprintf(val, sizeof(val), "%d", C.combo);
                else if (strstr(d, "anton"))
                {
                    double rem = (C.songLen > 0.5) ? (C.songLen - C.songT) : C.songT;   // countdown = 剩余时间
                    if (rem < 0) rem = 0;
                    snprintf(val, sizeof(val), "%02d:%02d", (int)rem / 60, (int)rem % 60);
                }
                else if (strstr(d, "kps")) snprintf(val, sizeof(val), "%d", C.kps);
                else if (strstr(d, "score")) snprintf(val, sizeof(val), "%d", (int)C.score);
                else break;
            }
            MspDrawNumber(dl, C, m, val, aMul);
            break;
        }
        default:
            break;
        }
    }
    // 打击特效模块识别：Malody 皮肤里打击特效有两种绑定方式：
    //   1) trigger Note(val=1..4) —— Rurudo/CrinoBaka 等；
    //   2) 皮肤 Lua 在命中时 DoAlpha/DoFrame 驱动，模块表里没有 trigger
    //      —— Phigros effect(phira-effect-*)、Gazer groundhit/airhit、Taiko hiteffect-*
    //      （实测这些模块若当常驻动画画会在画面上一直扇动）。
    // 判据：5002 帧动画 + layer>=2（玩法层）+ 名字含 hit/effect。
    static bool MspHitFxLike(const SkinMsp::RoleMod& m)
    {
        if (m.type != 5002 || m.layer < 2) return false;
        if (ModHasTrig(m, 15)) return true;
        static const char* kPat[] = { "hit", "effect", "hits-" };
        for (size_t p = 0; p < sizeof(kPat) / sizeof(kPat[0]); p++)
            if (StrHasI(m.desc, kPat[p]) || StrHasI(m.base, kPat[p]) || StrHasI(m.img, kPat[p]))
                return true;
        return false;
    }
    // 判定提示：MalodyV 皮肤 Judge 模块的等价呈现（皮肤资源优先，引擎字体仅兜底）
    //  · 皮肤自带判定动画（Judge 模块 images[] frames>1：Gazer AGbest/AGcool/AGgood/AGmiss）：
    //      在 Judge 模块锚点按皮肤 fps 播放一次（帧序列来自皮肤目录，绝不回退内置 judge-N）
    //  · 皮肤自带静态判定图（judge-N.png：Rurudo 系）：
    //      命中瞬间 DoResize(105→73.5)+DoAlpha(100→0) 100ms（rrdv52.lua 638-649）
    //  · 皮肤两者皆无 → 引擎文字兜底（MARVELOUS/PERFECT/GOOD/MISS + FAST/SLOW）
    static void MspDrawJudge(ImDrawList* dl, const MspCtx& C)
    {
        const int kind = s_jdLastKind.load(std::memory_order_relaxed);
        if (kind < 0 || kind > 3) return;
        const double age = RenderClock() - s_jdLastTime.load(std::memory_order_relaxed);
        if (age < 0.0) return;
        // 判定模块选择：同时存在多个（4800 判定条 / 4801 判定反馈）时优先 desc 含 "judge" 的那个
        // （证据：Taiko #32 hitmask 在前、#36 judge 在后，后者才是判定字样位置）
        const SkinMsp::RoleMod* jm = nullptr;
        for (int k = 0; k < s_modN; k++)
        {
            const SkinMsp::RoleMod& m = s_mods[k];
            if (!(m.usage == 7 || m.type == 4800 || m.type == 4801)) continue;
            if (StrHasI(m.desc, "judge")) { jm = &m; break; }
            if (!jm) jm = &m;
        }
        // 位置：皮肤 Judge 模块自身锚点；皮肤没有 Judge 模块（如 Phigros V）→ 与内置皮肤一致
        float jx = C.gw * 0.5f, jy = 566.f * C.u, w = 105.f * C.u, h = 30.f * C.u;
        if (jm)
        {
            MspAnchorPt(C, *jm, &jx, &jy);
            ModAbsLen(jm->wu, jm->w, MspParW(C, *jm), MspUnit(C, *jm), &w);
            ModAbsLen(jm->hu, jm->h, MspParH(C, *jm), MspUnit(C, *jm), &h);
        }
        const float jAlpha = jm ? (jm->alpha / 100.f) : 1.f;
        // ---- ① 皮肤自带判定动画（帧序列）----
        if (s_judgeOwnAnim[kind] && s_judgeAnimN[kind] > 0)
        {
            const int nf = s_judgeAnimN[kind];
            const int fps = (s_judgeAnimFps[kind] > 0) ? s_judgeAnimFps[kind] : 60;
            const double dur = (double)nf / (double)fps;
            if (age < dur)
            {
                int fr = (int)(age * (double)fps);
                if (fr < 0) fr = 0;
                if (fr >= nf) fr = nf - 1;
                void* tex = s_judgeAnim[kind][fr];
                if (tex)
                {
                    const float iw = (s_judgeAnimW[kind] > 0) ? (float)s_judgeAnimW[kind] : 256.f;
                    const float ih = (s_judgeAnimH[kind] > 0) ? (float)s_judgeAnimH[kind] : 128.f;
                    float ww = w, hh = h;
                    if (ww <= 0.5f && hh <= 0.5f) { ww = iw * C.u; hh = ih * C.u; }
                    else if (ww <= 0.5f) ww = hh * (iw / ih);
                    else if (hh <= 0.5f) hh = ww * (ih / iw);
                    dl->AddImage(TRef(tex), ImVec2(jx - ww * 0.5f, jy - hh * 0.5f),
                                 ImVec2(jx + ww * 0.5f, jy + hh * 0.5f),
                                 ImVec2(0.f, 0.f), ImVec2(1.f, 1.f),
                                 IM_COL32(255, 255, 255, (int)(255.f * jAlpha)));
                }
            }
            return;     // 皮肤有自有动画：模块表负责表情/小人，不再叠加引擎文字
        }
        // ---- ② 皮肤自带静态判定图（judge-N.png）----
        if (s_judgeOwnBar[kind] && s_texJudgeBar[kind])
        {
            if (jm && age < 0.10)
            {
                const float t = (float)(age / 0.10);
                const float sc = 1.f - 0.30f * t;             // DoResize 105→73.5
                const float a = 1.f - t;                      // DoAlpha 100→0
                float ww = w, hh = h;
                if (ww <= 0.5f || hh <= 0.5f) { ww = 105.f * C.u; hh = 30.f * C.u; }
                ww *= sc; hh *= sc;
                dl->AddImage(TRef(s_texJudgeBar[kind]), ImVec2(jx - ww * 0.5f, jy - hh * 0.5f),
                             ImVec2(jx + ww * 0.5f, jy + hh * 0.5f),
                             ImVec2(0.f, 0.f), ImVec2(1.f, 1.f),
                             IM_COL32(255, 255, 255, (int)(a * 255.f)));
            }
            return;     // 皮肤有自有判定图：不再叠加引擎文字
        }
        // ---- ③ 引擎文字兜底（皮肤完全没有判定资源）----
        // ---- 判定字样（Malody JudgeType1 的等价呈现：文字 sprite 用引擎字体代替）----
        // 动画对齐内置皮肤（用户认可的样式）：0.55s、1.05→0.735 收缩、颜色随判定档位，
        // 附带 FAST/SLOW ±ms 提示。位置 = 皮肤 Judge 模块锚点（无 Judge 模块用内置相对位）。
        const double textDur = 0.55;
        if (age >= textDur) return;
        static const char* kLabel[4] = { "MARVELOUS", "PERFECT", "GOOD", "MISS" };
        static const ImU32 kCol[4] = { IM_COL32(126, 255, 158, 255), IM_COL32(255, 226, 120, 255),
                                       IM_COL32(255, 172, 92, 255), IM_COL32(255, 92, 92, 255) };
        const float k = (float)(age / textDur);
        const float ease = 1.f - (1.f - k) * (1.f - k);
        const float pop = 1.05f + (0.735f - 1.05f) * ease;
        float a = 1.f - k * k;
        if (a < 0.f) a = 0.f;
        float size = 34.f * C.u * pop;
        if (size < 10.f * C.u) size = 10.f * C.u;
        ImGui::PushFont(nullptr, size);
        ImVec2 ts = ImGui::CalcTextSize(kLabel[kind]);
        ImU32 lc = kCol[kind];
        lc = (lc & 0x00FFFFFF) | ((ImU32)(a * 255.f) << 24);
        DrawTextShadow(dl, ImVec2(jx - ts.x * 0.5f, jy - ts.y * 0.5f), lc, kLabel[kind]);
        ImGui::PopFont();
        if (kind < 3)
        {
            const double dtMs = C.judgeOff * 1000.0;
            if (fabs(dtMs) >= 6.0)
            {
                char hb[48];
                snprintf(hb, sizeof(hb), "%s %dms", I18N::Tr(dtMs > 0 ? I18N::LBL_FAST : I18N::LBL_SLOW),
                         (int)(fabs(dtMs) + 0.5));
                ImGui::PushFont(nullptr, 17.f * C.u);
                ImVec2 hs = ImGui::CalcTextSize(hb);
                DrawTextShadow(dl, ImVec2(jx - hs.x * 0.5f, jy + ts.y * 0.5f + 6.f * C.u),
                               IM_COL32(255, 255, 255, (int)(190.f * a)), hb);
                ImGui::PopFont();
            }
        }
    }
    static void MspDrawHitFx(ImDrawList* dl, const MspCtx& C)
    {
        const double nowT = ImGui::GetTime();
        // ---- 皮肤声明的命中模块（先找，鼠标判定圈常驻绘制需要）----
        //  · Slide(圆形)皮肤（Meta.mode=7，dakumi）：hit/hitlight 由 Lua 克隆到每轨
        //    （main.lua track:Update：hit.X=轨道中心、hitlight.X=轨道中心/Width=轨宽；
        //     track:hit：hit DoScale 0→1＋Alpha 100→0(200ms)，hitlight SetColor+Alpha→0(500ms)）
        //  · particle（Phigros phi.lua OnHit）：命中粒子
        const SkinMsp::RoleMod* hitM = nullptr;
        const SkinMsp::RoleMod* lightM = nullptr;
        int partIdx = -1;
        for (int k = 0; k < s_modN; k++)
        {
            const SkinMsp::RoleMod& m = s_mods[k];
            if (!strcmp(m.desc, "particle")) { partIdx = k; continue; }
            if (s_mspModeId != 7 || m.type != 5000) continue;
            if (!strcmp(m.desc, "hit")) hitM = &m;
            else if (!strcmp(m.desc, "hitlight")) lightM = &m;
        }
        MspImg him{}, lim{};
        const bool hok = hitM && MspImage(C, hitM->img, &him) && him.ok;
        const bool lok = lightM && MspImage(C, lightM->img, &lim) && lim.ok;
        float hw = 0, hh = 0, lw2 = 0, lh2 = 0;
        if (hok) MspSize(C, *hitM, &him, true, &hw, &hh);
        if (lok) MspSize(C, *lightM, &lim, true, &lw2, &lh2);
        // [debug] ADOFAI_PERFECT_MSPDBG=1：命中特效环境（限速 1 条/0.5s）
        {
            static const char* s_env = getenv("ADOFAI_PERFECT_MSPDBG");
            if (s_env && s_env[0] == '1')
            {
                static double s_last = -99.0;
                if (nowT - s_last > 0.5)
                {
                    s_last = nowT;
                    Log::Printf("[mspfx] mode=%d cols=%d fxN=%d hit='%s' ok=%d w=%.1f h=%.1f light='%s' ok=%d lh=%.1f trackL=%.1f laneW=%.1f judgeY=%.1f u=%.2f",
                                s_mspModeId, C.cols, (int)s_fx.size(), hitM ? hitM->desc : "-", (int)hok, hw, hh,
                                lightM ? lightM->desc : "-", (int)lok, lh2, C.trackL, C.laneW, C.judgeY, C.u);
                }
            }
        }
        static const int kDkCol[4][3] = { { 255, 187, 69 }, { 249, 255, 69 },
                                          { 110, 255, 69 }, { 255, 0, 0 } };   // dakumi judge.color
        // 常驻判定圈：皮肤 Lua 把 hit 初始 Alpha 设为 0（只在命中闪现），但完全隐身没法打；
        // 这里以低透明度常显皮肤声明的判定圈（大小完全按 hit 模块声明，绝不安缩）。
        if (hok && hw > 1.f)
        {
            for (int lane = 0; lane < C.cols && lane < kMaxLanes; lane++)
            {
                const float cx = C.trackL + C.laneW * ((float)lane + 0.5f);
                // 圈的大小严格按 hit 模块声明（dakumi hit.png = 500u ≈ 半屏），透明度取
                // 足够看清又不过曝的 96（皮肤 Lua 常态 Alpha=0，此处是本工具的可见化处理）。
                dl->AddImage(TRef(him.tex), ImVec2(cx - hw * 0.5f, C.judgeY - hh * 0.5f),
                             ImVec2(cx + hw * 0.5f, C.judgeY + hh * 0.5f),
                             ImVec2((float)him.bx / him.nw, (float)him.by / him.nh),
                             ImVec2((float)(him.bx + him.bw) / him.nw, (float)(him.by + him.bh) / him.nh),
                             IM_COL32(255, 255, 255, 150));
            }
        }
        // 命中瞬间：逐轨判定圈 + 判定光
        if (hitM || lightM)
        {
            for (size_t i = 0; i < s_fx.size(); i++)
            {
                const HitFx& fx = s_fx[i];
                if (fx.lane < 0 || fx.lane >= C.cols) continue;
                const float age = (float)(nowT - fx.t0);
                if (age < 0.f || age > 0.6f) continue;
                const float cx = C.trackL + C.laneW * ((float)fx.lane + 0.5f);
                if (hok && hw > 1.f && age < 0.20f && fx.kind < 3)
                {
                    const float t = age / 0.20f;
                    const float sc = 0.15f + 0.85f * t;
                    const int a = (int)((1.f - t) * 255.f * (hitM->alpha / 100.f));
                    if (a > 2)
                        dl->AddImage(TRef(him.tex),
                                     ImVec2(cx - hw * 0.5f * sc, C.judgeY - hh * 0.5f * sc),
                                     ImVec2(cx + hw * 0.5f * sc, C.judgeY + hh * 0.5f * sc),
                                     ImVec2((float)him.bx / him.nw, (float)him.by / him.nh),
                                     ImVec2((float)(him.bx + him.bw) / him.nw, (float)(him.by + him.bh) / him.nh),
                                     IM_COL32(255, 255, 255, a));
                }
                if (lok && lh2 > 1.f && age < 0.5f && fx.kind < 3)
                {
                    static const float kDkAl[4] = { 1.f, 1.f, 1.f, 1.f };
                    const float t = age / 0.5f;
                    const int a = (int)((1.f - t) * 255.f * (lightM->alpha / 100.f) * kDkAl[fx.kind]);
                    if (a > 2)
                        dl->AddImage(TRef(lim.tex), ImVec2(cx - C.laneW * 0.5f, C.judgeY - lh2 * 0.5f),
                                     ImVec2(cx + C.laneW * 0.5f, C.judgeY + lh2 * 0.5f),
                                     ImVec2((float)lim.bx / lim.nw, (float)lim.by / lim.nh),
                                     ImVec2((float)(lim.bx + lim.bw) / lim.nw, (float)(lim.by + lim.bh) / lim.nh),
                                     IM_COL32(kDkCol[fx.kind][0], kDkCol[fx.kind][1], kDkCol[fx.kind][2], a));
                }
            }
        }
        if (s_fx.empty()) return;
        for (size_t i = 0; i < s_fx.size(); i++)
        {
            const HitFx& fx = s_fx[i];
            if (fx.lane < 0 || fx.lane >= C.cols) continue;
            const float age = (float)(nowT - fx.t0);
            if (age < 0.f || age > 1.20f) continue;      // 单模块时长按 frames/fps 判定（下方）
            // 命中特效模块匹配：
            //   ① trigger Note val = 判定结果(1..4)（Rurudo hit1 val=1 任意轨 / hit2..5 val=2
            //     分轨 / hit6 val=4 Miss；CrinoBaka hit9 val=4 Miss）；
            //   ② 无 trigger 的 Lua 驱动打击图（Phigros effect / Gazer groundhit /
            //     Taiko hiteffect）—— MspHitFxLike 识别。
            // 优先级：轨道相等 + val 精确 > 轨道相等 > val 精确 > val 不高于判定的最近一级 > 任意兼容项。
            const SkinMsp::RoleMod* pick = nullptr;
            int pickRank = 99;
            const int wantVal = fx.kind + 1;
            for (int k = 0; k < s_modN; k++)
            {
                const SkinMsp::RoleMod& hm = s_mods[s_modSorted[k]];
                if (!MspHitFxLike(hm)) continue;
                int tv = 0;
                const bool hasTrig = ModHasTrig(hm, 15, &tv);
                // 皮肤脚本已在运行且自己定义了 OnHit（Phigros phi.lua / Kalpa kalpa-phigros.lua）
                // → 打击特效完全由脚本的 Shadow 生成（含按判定的 SetColor）；引擎再画一遍会
                // 变成"双份特效"，且引擎副本没有脚本颜色（纯白）→ 跳过无 trigger 的 Lua 驱动项。
                if (!hasTrig && s_skinLuaActive && SkinLua::HasOnHit()) continue;
                // Miss：只播放皮肤显式绑定的 Miss 特效（trig val=4）。
                // Gazer/Phigros 的打击图由 Lua 在命中时播放、Miss 不播（证据：两皮肤 .lua 的 OnHit）。
                if (fx.kind == 3 && !(hasTrig && tv == 4)) continue;
                const bool laneOk = (hm.lane == fx.lane);
                const bool anyLane = (hm.lane < 0);
                if (!laneOk && !anyLane) continue;
                int rank = 8;
                if (hasTrig && tv != 0)
                {
                    if (tv == wantVal)      rank = laneOk ? 0 : 2;
                    else if (tv < wantVal)  rank = laneOk ? 4 : 5;   // 低一档特效（Good 用 Cool 图等）
                    else                    rank = 9;                // 高于本次判定的特效不用
                }
                else rank = laneOk ? 1 : 3;                          // 无 trigger（Lua 驱动）
                if (rank < pickRank) { pickRank = rank; pick = &hm; }
                if (pickRank == 0) break;
            }
            if (!pick) continue;
            const SkinMsp::RoleMod& hm = *pick;
            // [debug] 命中特效命中模块追踪（每条 fx 只记一次）
            {
                static const char* s_env = getenv("ADOFAI_PERFECT_MSPDBG");
                static double s_lastT0 = -1.0;
                if (s_env && s_env[0] == '1' && fx.t0 != s_lastT0)
                {
                    s_lastT0 = fx.t0;
                    Log::Printf("[mspfx] hit lane=%d kind=%d -> '%s' mlane=%d rank=%d frames=%d fps=%d x=%.1f y=%.1f",
                                fx.lane, fx.kind, hm.desc[0] ? hm.desc : "-", hm.lane, pickRank,
                                hm.frames, hm.fps, hm.x, hm.y);
                }
            }
            const int nf = (hm.frames > 1) ? hm.frames : 1;
            const int fps = (hm.fps > 1) ? hm.fps : 45;
            const float dur = (nf > 1) ? ((float)nf / (float)fps) : 0.20f;
            if (age > dur) continue;                     // 按皮肤 fps 播完即止（如 Gazer 51f@100fps=0.51s）
            int k = (int)(age * (float)fps);
            if (k >= nf) k = nf - 1;
            if (k < 0) k = 0;
            char fr[128];
            const char* f = hm.img;
            if (hm.base[0] && nf > 1)
            {
                snprintf(fr, sizeof(fr), "%s%d.png", hm.base, hm.start + k);
                f = fr;
            }
            MspImg im;
            if (!MspImage(C, f, &im) || !im.ok) continue;
            float w = 0, h = 0;
            MspSize(C, hm, &im, true, &w, &h);
            if (w <= 0.5f || h <= 0.5f) continue;
            float sx, sy;
            MspAnchorPt(C, hm, &sx, &sy);
            const float cx = C.trackL + C.laneW * ((float)fx.lane + 0.5f);   // 命中轨中心
            const float cy = sy;                                            // 模块自身锚点（判定线 + dy）
            // 帧序列自带淡出（Malody 皮肤帧动画如此）；透明度只用模块 alpha
            const int alpha = (int)(hm.alpha / 100.f * 255.f);
            // Lua 着色：Phigros phi.lua OnHit 按判定 SetColor（judge 1/6 黄 252,254,184；
            // 2/3 蓝 185,230,246；4 不播放）。带 particle 模块的皮肤视为 Lua 着色皮肤。
            ImU32 col = IM_COL32(255, 255, 255, alpha);
            if (partIdx >= 0 && StrHasI(hm.desc, "effect"))
            {
                if (fx.kind == 0)      col = IM_COL32(252, 254, 184, alpha);
                else if (fx.kind <= 2) col = IM_COL32(185, 230, 246, alpha);
            }
            // 3D 斜轨道：轨道层特效随透视投影（与音符/轨道模块一致）
            float dcx = cx, dcy = cy, dsc = 1.f;
            if (Msp3DActive(C) && MspTrackParent(hm)) Msp3DPoint(C, cx, cy, &dcx, &dcy, &dsc);
            dl->AddImage(TRef(im.tex), ImVec2(dcx - w * 0.5f * dsc, dcy - h * 0.5f * dsc),
                         ImVec2(dcx + w * 0.5f * dsc, dcy + h * 0.5f * dsc),
                         ImVec2((float)im.bx / im.nw, (float)im.by / im.nh),
                         ImVec2((float)(im.bx + im.bw) / im.nw, (float)(im.by + im.bh) / im.nh),
                         col);
            // 命中粒子（phi.lua OnHit）：particle 模块克隆 4 个，400ms 内沿随机方向漂移 20px，
            // 333ms 后 167ms 淡出；颜色随判定。
            if (partIdx >= 0 && fx.kind < 3 && age < 0.5f && s_mods[partIdx].type == 5004)
            {
                const SkinMsp::RoleMod& pm = s_mods[partIdx];
                float pw = 0, phh = 0;
                ModAbsLen(pm.wu, pm.w, MspParW(C, pm), MspUnit(C, pm), &pw);
                ModAbsLen(pm.hu, pm.h, MspParH(C, pm), MspUnit(C, pm), &phh);
                if (pw < 1.2f) pw = 1.6f * C.u;
                if (phh < 1.2f) phh = pw;
                const float mv = (age < 0.4f) ? (age / 0.4f) : 1.f;
                float pa = 255.f;
                if (age > 0.333f) pa = 255.f * (1.f - (age - 0.333f) / 0.167f);
                if (pa > 0.f)
                {
                    const ImU32 pc = (fx.kind == 0) ? IM_COL32(252, 254, 184, (int)pa)
                                                    : IM_COL32(185, 230, 246, (int)pa);
                    for (int q = 0; q < 4; q++)
                    {
                        const float ang = (float)(fx.t0 * 2.3999632 + q * 1.5707963 + fx.lane * 0.7853982);
                        const float dx = 20.f * C.u * sinf(ang) * mv;
                        const float dy = 20.f * C.u * cosf(ang) * mv;
                        dl->AddRectFilled(ImVec2(cx + dx - pw * 0.5f, cy + dy - phh * 0.5f),
                                          ImVec2(cx + dx + pw * 0.5f, cy + dy + phh * 0.5f), pc);
                    }
                }
            }
        }
    }
    // ---- Malody 皮肤 Lua 驱动模块的等价实现（本渲染器不执行 Malody Lua）----
    // 证据：Rurudo rrdv52.lua
    //  · rrdcb1..4「连击提示图」（皮肤设置默认=开）：combo 每满 100 的 3 连击内随机抽 1 张，
    //    DoMoveX 520→0（1.1s）+ DoAlpha 0→100(0.1s) → 100→0(0.5..0.95s)。
    //  · progressrrd 底部小人：DoMoveX 0→screenWidth（整曲随进度移动）。
    //  · currentkps / maxkps：DoMoveY 0→600（随时间上升），文本由 Lua 写入。
    //  · bar1：KPS 直方图（克隆成 slot_count 根竖条，Y=600/slot*(i-1)，宽度 = 该时段最大 KPS）。
    static float s_cbT0 = -1000.f;
    static int   s_cbPick = -1;
    static int   s_cbLastCombo = -1;
    static float  s_kpsSlotMax[180] = {};
    static int    s_kpsSlotN = 0;
    static double s_kpsSlotLen = 0.0;
    static double s_kpsSongLen = -1.0;

    static void MspKpsBars(ImDrawList* dl, const MspCtx& C, const SkinMsp::RoleMod& m)
    {
        const double len = (C.songLen > 1.0) ? C.songLen : 0.0;
        if (len <= 1.0) return;
        if (s_kpsSongLen != len)
        {
            s_kpsSongLen = len;
            int slots = (int)(len + 0.999);
            if (slots < 1) slots = 1;
            if (slots > 180) slots = 180;
            s_kpsSlotN = slots;
            s_kpsSlotLen = len / (double)slots;
            for (int i = 0; i < 180; i++) s_kpsSlotMax[i] = 0.f;
        }
        if (s_kpsSlotN <= 0 || s_kpsSlotLen <= 0.01) return;
        const int cur = (int)(C.songT / s_kpsSlotLen);
        if (cur >= 0 && cur < s_kpsSlotN && (float)C.kps > s_kpsSlotMax[cur])
            s_kpsSlotMax[cur] = (float)C.kps;
        float mx = 1.f;
        for (int i = 0; i < s_kpsSlotN; i++) if (s_kpsSlotMax[i] > mx) mx = s_kpsSlotMax[i];
        float sx, sy;
        MspAnchorPt(C, m, &sx, &sy);
        const float barH = 600.f * C.u / (float)s_kpsSlotN;
        const float maxW = 80.f * C.u;
        const float a = m.alpha / 100.f;
        for (int i = 0; i < s_kpsSlotN; i++)
        {
            if (s_kpsSlotMax[i] <= 0.01f) continue;
            float w = maxW * (s_kpsSlotMax[i] / mx);
            if (w < 2.f * C.u) w = 2.f * C.u;
            const float yb = C.gh - (float)i * barH;      // i=0 从屏幕底开始（Lua：Y=barAHS*(i-1)）
            dl->AddRectFilled(ImVec2(sx, yb - barH + 1.f), ImVec2(sx + w, yb),
                              IM_COL32(255, 255, 255, (int)(a * 235.f)));
        }
    }

    // 返回 0=普通模块；1=本帧不画；2=已按 Lua 覆写坐标（画 out，透明度 aMul）
    static int MspLuaDriven(ImDrawList* dl, const MspCtx& C, const SkinMsp::RoleMod& m,
                            SkinMsp::RoleMod* out, float* aMul)
    {
        *out = m;
        *aMul = 1.f;
        const char* d = m.desc;
        if (!d[0]) return 0;
        if (m.type == 5000 && !strncmp(d, "rrdcb", 5))
        {
            const int idx = (d[5] >= '1' && d[5] <= '4') ? (d[5] - '0') : 0;
            const int trig = 100;                       // 皮肤设置「连击提示图触发值」默认 100
            if (C.combo < trig) s_cbLastCombo = -1;
            else if (C.combo != s_cbLastCombo && (C.combo % trig) <= 3)
            {
                s_cbLastCombo = C.combo;
                s_cbT0 = (float)C.nowT;
                s_cbPick = (C.combo / trig) % 4;        // Lua math.random(1,4)：按里程碑序号取一张
            }
            const float t = (float)C.nowT - s_cbT0;
            if (s_cbPick < 0 || idx <= 0 || (idx - 1) != s_cbPick || t < 0.f || t > 1.10f) return 1;
            float a = 1.f;
            if (t < 0.10f) a = t / 0.10f;
            else if (t > 0.50f) a = 1.f - (t - 0.50f) / 0.45f;
            if (a <= 0.01f) return 1;
            const float e = t / 1.10f;
            const float es = e * e * (3.f - 2.f * e);
            out->x = 520.f * (1.f - es);
            out->xu = 1;
            *aMul = a;
            return 2;
        }
        if (m.type == 5000 && !strcmp(d, "progressrrd"))
        {
            out->x = C.progress * C.gw / (C.u > 0.0001f ? C.u : 1.f);   // Lua DoMoveX 0→screenWidth
            out->xu = 1;
            return 2;
        }
        if (m.type == 5001 && (!strcmp(d, "currentkps") || !strcmp(d, "maxkps")))
        {
            // 同名模块属于 rrdv52.lua（Rurudo）才按 Lua 协议驱动，否则会误伤其他皮肤
            if (!s_skinRrd) return 0;
            out->y = 600.f * C.progress;               // Lua DoMoveY 0→600
            out->yu = 1;
            return 2;
        }
        if (m.type == 5004 && !strcmp(d, "bar1"))
        {
            if (!s_skinRrd) return 0;
            MspKpsBars(dl, C, m);
            return 1;
        }
        if (!s_skinRrd) return 0;   // 以下均为 rrdv52.lua 专属（Rurudo 同名模块）
        // ---- rrdv52.lua 顶部 HUD 入场动画终值（DoMoveY from=240 → to=…；单位 u，负值=向下）----
        //  证据：rrdv52.lua 320-360（cyellow/m5logo/cout/cin/pause→-87、title→-81、bpm→-112.5、
        //  ver→-139.5）、237-243+354-359（countdownbg/countdown→-39）。
        //  不实现则该 HUD 停在屏幕顶端（circle y=100% pivot=Middle ⇒ 半圆出屏，整体偏上）
        {
            static const struct { const char* d; float off; } kRest[] = {
                { "cyellow", 87.f }, { "m5logo", 87.f }, { "cout", 87.f }, { "cin", 87.f },
                { "pause", 87.f }, { "title", 81.f }, { "bpm", 112.5f }, { "ver", 139.5f },
                { "countdownbg", 39.f }, { "countdown", 39.f },
            };
            for (size_t ri = 0; ri < sizeof(kRest) / sizeof(kRest[0]); ri++)
                if (!strcmp(d, kRest[ri].d)) { out->dy = m.dy - kRest[ri].off; out->dyu = 1; return 2; }
        }
        return 0;
    }

    // ---- 5001 文本「数据载体」抑制（实测重影修复）----
    // Malody 皮肤里有一类纯数据文本：Lua 只读取它的 Text（Rurudo luaacc/luabest、
    // Phigros hscore/hmod），真正显示由另一个模块完成（数字模块或 Lua 写入的静态文本）。
    // 我们不执行 Lua，若照画会与显示模块叠在同一位置 → 重影
    // （实测：Rurudo 中央两处 100.00%、Phigros 右上 score/hscore/hmod 三重叠）。
    // 判据（逆向自皮肤数据 + 同锚点堆叠规律）：
    //  ① 纯变量文本（"{acc}"）且同皮肤存在 5003 数字模块画同一变量 → 载体；
    //  ② 与另一 5001 模块锚点/层完全相同（编辑器堆叠）→ 只保留文本最"具体"的一条，
    //     其余视为载体（Phigros score 与 hscore/hmod 就是这一情形）。
    static bool MspTextPureVar(const char* t, char* varOut, int n)
    {
        if (!t || t[0] != '{') return false;
        const char* close = strchr(t, '}');
        if (!close || close[1] != 0) return false;
        const size_t len = (size_t)(close - t);
        if (len < 2 || len > 20) return false;
        if (varOut && n > 0) snprintf(varOut, (size_t)n, "%.*s", (int)(len + 1), t);
        return true;
    }
    static bool MspTextIsCarrier(const SkinMsp::RoleMod& m)
    {
        if (m.type != 5001 || !m.text[0]) return false;
        char var[24] = { 0 };
        const bool pure = MspTextPureVar(m.text, var, sizeof(var));
        if (pure)
        {
            // 变量名（去掉花括号），用于和「显示模块」的描述名比对
            char vname[24] = { 0 };
            snprintf(vname, sizeof(vname), "%s", var + 1);
            size_t vl = strlen(vname);
            if (vl > 0 && vname[vl - 1] == '}') vname[vl - 1] = 0;
            for (int k = 0; k < s_modN; k++)
            {
                const SkinMsp::RoleMod& o = s_mods[k];
                if (&o == &m) continue;
                if (o.type == 5003)
                {
                    if (o.numText[0] && strstr(o.numText, var)) return true;
                    if (o.desc[0] && vname[0] && !_stricmp(o.desc, vname)) return true;
                }
                // 同类皮肤常见写法：显示模块 desc 就是变量名（Rurudo best/marv、Phigros score）
                if (o.type == 5001 && o.desc[0] && vname[0] && !_stricmp(o.desc, vname))
                {
                    char ov[24] = { 0 };
                    if (!MspTextPureVar(o.text, ov, sizeof(ov))) return true;
                }
            }
        }
        // (2) 同一锚点堆叠组：只保留一条用于显示。优先保留“非纯变量”文本（显示用，
        //     如 Phigros 的 score）；纯变量文本（hscore/hmod 这类 Lua 读写载体）抑制。
        int best = -1, bestRank = 99;
        for (int k = 0; k < s_modN; k++)
        {
            const SkinMsp::RoleMod& o = s_mods[k];
            if (o.type != 5001 || !o.text[0]) continue;
            if (o.x != m.x || o.y != m.y || o.dx != m.dx || o.dy != m.dy) continue;
            if (o.xu != m.xu || o.yu != m.yu || o.dxu != m.dxu || o.dyu != m.dyu) continue;
            if (o.pivot != m.pivot || o.layer != m.layer) continue;
            char other[24] = { 0 };
            const bool op = MspTextPureVar(o.text, other, sizeof(other));
            const int rank = op ? 1 : 0;
            if (rank < bestRank) { bestRank = rank; best = k; }
        }
        if (best >= 0 && &s_mods[best] != &m) return true;
        return false;
    }
    // ---- Skin Lua 沙箱接入（Malody MSP 皮肤脚本；逆向见 SkinLua.cpp 头注释）----
    //  脚本 X/Y/Width/Height 单位 = 1080 基准设计像素（= RoleMod.dx 且 dxu=1），
    //  所以 Patch 直接写入 RoleMod 副本即可，和 info.asm 静态值同坐标系。
    static void MspApplyLuaPatch(const SkinLua::Patch& p, SkinMsp::RoleMod* d)
    {
        // 脚本坐标单位（逆向自各皮肤 info.asm 与 .lua 的对照）：
        //   · 模块声明过任一 X 位置字段（x / xu / dx）→ 脚本值按 1080 基准像素（u）解释。
        //     证据：Mango233 time(dx=-295,dxu=1) 的 Lua True_Time.X=-840*scale；
        //           Rurudo rrdcb(x=-25,xu=1) 的 Lua X=520 / DoMoveX 520→0。
        //   · 模块完全没有声明 X（x=xu=dx=0）→ 脚本值按百分比解释。
        //     证据：Phigros effect/particle、Kalpa hit1..5 的 Lua X=11.34+22.68*(hitx-1)，
        //           是 4K 轨道中心百分比；旧实现一律按 u 解释 → 特效落在屏幕最左边 11..79px
        //           （实测症状"左边一直出现白色的打击特效"，且与官方 Malody 位置不符）。
        const bool xPosDeclared = (d->x != 0.f) || (d->xu != 0) || (d->dx != 0.f);
        const bool yPosDeclared = (d->y != 0.f) || (d->yu != 0) || (d->dy != 0.f);
        if (p.x) { d->dx = p.xv; d->dxu = xPosDeclared ? 1 : 0; }
        if (p.y) { d->dy = p.yv; d->dyu = yPosDeclared ? 1 : 0; }
        if (p.color)
        {
            const int r = p.cr < 0 ? 0 : (p.cr > 255 ? 255 : p.cr);
            const int g = p.cg < 0 ? 0 : (p.cg > 255 ? 255 : p.cg);
            const int b = p.cb < 0 ? 0 : (p.cb > 255 ? 255 : p.cb);
            snprintf(d->color, sizeof(d->color), "#%02X%02X%02X", r, g, b);
        }
        if (p.w) { d->w = p.wv; d->wu = 1; }
        if (p.h) { d->h = p.hv; d->hu = 1; }
        if (p.alpha) d->alpha = p.av;
        if (p.rotate) d->rotate = p.rv;
        if (p.fill) { d->fillSet = true; d->fillVal = p.fillv; }
        if (p.text)
        {
            if (d->type == 5003) snprintf(d->numText, sizeof(d->numText), "%s", p.text);
            else snprintf(d->text, sizeof(d->text), "%s", p.text);
        }
    }
    static void EnsureSkinLua(const MspCtx& C)
    {
        if (s_skinLuaTried || !s_mspFull || C.gw <= 1.f || !s_skinScript[0]) return;
        s_skinLuaTried = true;
        SkinLua::Env env{};
        env.windowW = C.gw;
        env.windowH = C.gh;
        env.sceneScale = (s_mspKeyScale > 0.01f) ? s_mspKeyScale : 1.f;
        env.trackAngle = (int)(s_mspKeyAngle + 0.5f);
        env.audioLengthMs = s_chartEndT.load(std::memory_order_relaxed) * 1000.0;
        env.startTimeMs = 0.0;
        env.bpm = s_bpm.load(std::memory_order_relaxed);
        env.level = s_level.load(std::memory_order_relaxed);
        env.judgeName = (JudgeSet() == 0) ? "HARD" : (JudgeSet() == 2) ? "EASY" : "NORMAL";
        env.levelName = C.levelName ? C.levelName : "";
        {
            std::lock_guard<std::mutex> lk(s_notesMutex);
        env.noteCount = (int)s_notes.size();
        }
        env.columns = C.cols;
        const bool ok = SkinLua::Load(s_skinDirCur, s_skinScript, s_mods, s_modN, env);
        Log::Printf("[skin] SkinLua '%s' load=%d err='%s'", s_skinScript, ok ? 1 : 0, SkinLua::LastError());
    }

    static void MspDrawPass(ImDrawList* dl, const MspCtx& C, int band)
    {
        for (int k = 0; k < s_modN; k++)
        {
            const SkinMsp::RoleMod& m = s_mods[s_modSorted[k]];
            // [debug] ADOFAI_PERFECT_MSPDBG=1：逐模块（按索引去重）输出最终锚点/尺寸/层，
            // 供与 info.asm 模块表逐条比对定位错位模块。
            {
                static const char* s_env = getenv("ADOFAI_PERFECT_MSPDBG");
                if (s_env && s_env[0] == '1')
                {
                    static unsigned long long s_seen2[8] = { 0 };
                    static int s_gen2 = 0;
                    if (s_gen2 != s_modN) { s_gen2 = s_modN; memset(s_seen2, 0, sizeof(s_seen2)); }
                    const int idx = s_modSorted[k];
                    if (idx >= 0 && idx < 512 && !(s_seen2[idx >> 6] & (1ull << (idx & 63))))
                    {
                        s_seen2[idx >> 6] |= (1ull << (idx & 63));
                        float sx, sy, px, py;
                        MspAnchorPt(C, m, &sx, &sy);
                        ModPivotF(m, &px, &py);
                        float dw = 0, dh = 0;
                        ModAbsLen(m.wu, m.w, MspParW(C, m), MspUnit(C, m), &dw);
                        ModAbsLen(m.hu, m.h, MspParH(C, m), MspUnit(C, m), &dh);
                        Log::Printf("[mspmod] i=%d u=%d t=%d d=%s img=%s x=%.1f y=%.1f pv=%d w=%.1f h=%.1f L%d ro=%d a=%d sd=%d tg=%d hd=%d alt=%d RAW x=%.1f/xu%d y=%.1f/yu%d w=%.1f/wu%d h=%.1f/hu%d dx=%.1f/dxu%d dy=%.1f/dyu%d",
                                    idx, m.usage, m.type, m.desc[0] ? m.desc : "-", m.img[0] ? m.img : "-",
                                    sx, sy, m.pivot, dw, dh, m.layer, m.rotate, m.alpha,
                                    m.sceneN, m.trigN, m.hide, s_modAlt[idx] ? 1 : 0,
                                    m.x, m.xu, m.y, m.yu, m.w, m.wu, m.h, m.hu, m.dx, m.dxu, m.dy, m.dyu);
                    }
                }
            }
            const bool after = (MspDrawRank(m.layer) >= 1); // rank≥1：音符之上（L3/L1/L4，见 MspDrawRank）
            // 备用图组里落选的（只显示 1 张）；脚本已接管的模块不套用该启发式（脚本用 Alpha/坐标决定显隐）
            if (s_modAlt[s_modSorted[k]] && !(s_skinLuaActive && SkinLua::Touched(s_modSorted[k]))) continue;
            if ((band == 1) != after) continue;
            if (m.usage == 1 || m.usage == 98) continue;    // 音符/脚本值：由引擎按音符绘制
            // Malody 引擎交互控件（Skip/Cover/Catcher/Bar/TouchArea）：位置由引擎运行时改写，
            // 按模块表坐标硬画会在画面中央堆成一坨（Rurudo #87 skip：x=50% y=50%）。
            if (m.usage == 3 || m.usage == 6 || m.usage == 8 || m.usage == 9 || m.usage == 10) continue;
            if (m.usage == 7 || m.type == 4800 || m.type == 4801) continue;   // 判定提示：MspDrawJudge
            if (MspHitFxLike(m)) continue;                                    // 打击特效：MspDrawHitFx
            // Slide(圆形滑条)皮肤：hit/hitlight 只在命中瞬间由皮肤 Lua 播放（dakumi main.lua
            // track:hit，初始 Alpha=0），静态画会在界面留下常驻大图 → 交给 MspDrawHitFx。
            if (s_mspModeId == 7 && (!strcmp(m.desc, "hit") || !strcmp(m.desc, "hitlight"))) continue;
            // particle 模块：命中粒子由 MspDrawHitFx 按 phi.lua OnHit 生成
            if (m.desc[0] && !strcmp(m.desc, "particle")) continue;
            if (m.img[0] && strcmp(m.img, s_lineImgName) == 0) continue;      // 判定线：引擎定位
            if (m.img[0] && strcmp(m.img, s_hitBgImgName) == 0) continue;     // 判定底光：引擎定位
            // 网格：Rurudo desc=wangge + bggrid.png（Lua 里按 3D 场角旋转并放大到 3456²，
            // 2D 叠加只会糊满全屏 → 跳过）
            if (m.type == 5000 && (StrHasI(m.img, "bggrid") || StrHasI(m.desc, "wangge"))) continue;
            if (m.type != 5000 && m.type != 5001 && m.type != 5002 &&
                m.type != 5003 && m.type != 5004 && m.type != 4900 && m.type != 4600) continue;
            if (MspTextIsCarrier(m)) continue;               // 数据载体文本（不画，避免重影）
            SkinMsp::RoleMod mm = m;
            float aMul = 1.f;
            // Lua 沙箱：脚本覆写优先（旧特化路径仅在无脚本或脚本未触及该模块时生效）
            if (s_skinLuaActive)
            {
                SkinLua::Patch lp{};
                SkinLua::Eval(s_modSorted[k], C.songT * 1000.0, &lp);
                if (lp.x || lp.y || lp.w || lp.h || lp.alpha || lp.rotate || lp.fill || lp.text)
                {
                    MspApplyLuaPatch(lp, &mm);
                    MspApplyAnims(C, mm, &mm);
                    MspDrawModule(dl, C, mm, aMul);
                    continue;
                }
            }
            const int lua = MspLuaDriven(dl, C, m, &mm, &aMul);   // Lua 驱动模块（连击图/进度小人/KPS 图）
            if (lua == 1) continue;
            if (lua == 2) { MspApplyAnims(C, mm, &mm); MspDrawModule(dl, C, mm, aMul); continue; }
            if (!MspModVisible(C, m)) continue;             // scene/trigger 条件（含按键姿势、表情、ACC 表情）
            // ModuleParamImage 16 hide：模块默认隐藏（Malody 由 Lua/触发器点亮）。命中特效由
            // MspDrawHitFx 按命中播放，这里只跳过常驻静态绘制（与引擎行为一致；
            // ADOFAI_PERFECT_SHOWHIDDEN=1 可强制显示用于排查）。
            if (m.hide)
            {
                static const char* sh = getenv("ADOFAI_PERFECT_SHOWHIDDEN");
                if (!(sh && sh[0] == '1')) continue;
            }
            // Slide 模式（Meta.mode=7）逐轨展开：Malody 里 line-l/line-r/track_line/track_bg 由
            // 皮肤 Lua 克隆成每轨实例（dakumi main.lua track:Update：modLine.X=x-w/2、
            // modLine2.X=x+w/2、modBg.X=x、modBg.Width=w）。静态坐标会把左右线全部叠在屏幕中央，
            // 这里按本工具轨道逐轨绘制（圆形/滑条皮肤的核心轨道结构）。
            if (s_mspModeId == 7 &&
                (!strcmp(m.desc, "line-l") || !strcmp(m.desc, "line-r") ||
                 !strcmp(m.desc, "track_line") || !strcmp(m.desc, "track_bg")))
            {
                const bool isBg = !strcmp(m.desc, "track_bg");
                const bool atLeft = !strcmp(m.desc, "line-l");
                const bool atRight = !strcmp(m.desc, "line-r");
                float lw = 0;
                ModAbsLen(m.wu, m.w, MspParW(C, m), MspUnit(C, m), &lw);
                if (lw < 1.f) lw = 1.f;
                for (int lane = 0; lane < C.cols && lane < kMaxLanes; lane++)
                {
                    const float lx = C.trackL + C.laneW * (float)lane;
                    const float rx = lx + C.laneW;
                    if (isBg)
                    {
                        MspImg im;
                        if (!MspImage(C, m.img, &im) || !im.ok) continue;
                        const int a = (int)(m.alpha / 100.f * 255.f * 0.75f);
                        if (a <= 2) continue;
                        dl->AddImage(TRef(im.tex), ImVec2(lx, 0.f), ImVec2(rx, C.judgeY),
                                     ImVec2((float)im.bx / im.nw, (float)im.by / im.nh),
                                     ImVec2((float)(im.bx + im.bw) / im.nw, (float)(im.by + im.bh) / im.nh),
                                     IM_COL32(255, 255, 255, a));
                        continue;
                    }
                    for (int side = 0; side < 2; side++)
                    {
                        if (side == 1 && (atLeft || atRight)) continue;   // 单侧线
                        const float x = (side == 0) ? lx : rx;
                        if (m.type == 5000)
                        {
                            MspImg im;
                            if (!MspImage(C, m.img, &im) || !im.ok) continue;
                            const int a = (int)(m.alpha / 100.f * 255.f);
                            if (a <= 2) continue;
                            dl->AddImage(TRef(im.tex), ImVec2(x - lw * 0.5f, 0.f), ImVec2(x + lw * 0.5f, C.judgeY),
                                         ImVec2((float)im.bx / im.nw, (float)im.by / im.nh),
                                         ImVec2((float)(im.bx + im.bw) / im.nw, (float)(im.by + im.bh) / im.nh),
                                         IM_COL32(255, 255, 255, a));
                        }
                        else
                            dl->AddRectFilled(ImVec2(x - lw * 0.5f, 0.f), ImVec2(x + lw * 0.5f, C.judgeY),
                                              ModColor(m.color, IM_COL32(255, 255, 255, 220), m.alpha / 100.f));
                    }
                }
                continue;
            }
            // ModuleAnimation（field 20）：在已声明参数上求值（Lua 驱动路径在 mm 上另行叠加）
            SkinMsp::RoleMod am = mm;
            MspApplyAnims(C, m, &am);
            MspDrawModule(dl, C, am, 1.f);
        }
        // ---- Lua Clone/Shadow 额外实例（Cynosure 的 300 根 KPS 柱、Phigros 粒子等）----
        if (s_skinLuaActive)
        {
            static SkinLua::Extra s_ex[512];
            const int nEx = SkinLua::Extras(s_ex, 512);
            for (int i = 0; i < nEx; i++)
            {
                const int sm = s_ex[i].srcMod;
                if (sm < 0 || sm >= s_modN) continue;
                const SkinMsp::RoleMod& src = s_mods[sm];
                if ((band == 1) != (MspDrawRank(src.layer) >= 1)) continue;
                SkinMsp::RoleMod mm = src;
                MspApplyLuaPatch(s_ex[i].patch, &mm);
                // 帧序列（5002 frames/fps）按"播放经过时间"推进：否则每个 Shadow 都落到绝对
                // 时间对应的随机帧（Phigros 打击特效 30f@60fps 会随机出现在任意帧）。
                MspCtx C2 = C;
                if (s_ex[i].playing)
                {
                    const double age = (C.songT - s_ex[i].playT0 / 1000.0);
                    C2.nowT = (age > 0.0) ? age : 0.0;
                    MspApplyAnims(C, mm, &mm, age);
                }
                MspDrawModule(dl, C2, mm, 1.f);
            }
        }
    }
    // ---------------- 主绘制 ----------------
    // 版面按示例图（Malody V · Rurudo 4K）逐像素实测重建，1080p 基准 u = gh/1080：
    //   轨道外框 744u（金边 11u）· 内侧键区 177u×4 · 音符 168×63u
    //   判定线 y=915u（音符中心落点）· 底光带 88u · 键块 971..1031 · 键条 1031..1051
    //   左上 圆环(100,91) 半径97 · 时间条 (218,16)-(487,61) · 曲名/BPM/难度 x=220（避开圆环与游戏自带文字）
    //   右上 玩家条 y26..59 · 彩虹条 y59..77 · 5 数字 y68 · 头像 gw-98..gw-23
    //   右上小人 345u @ (gw-345,165) 常驻 + 打击时叠加变化图（不可替换整张）
    //   右下托腮小人 279x301u @ (gw-279,781)
    // 调试钩子：ADOFAI_PERFECT_ENABLE=4k|5k|6k|10k 启动时直接启用对应模式
    // （供无人值守截图/回归测试使用；不写配置、不影响手动选择）
    static void ApplyEnableEnvOnce()
    {
        static bool s_done = false;
        if (s_done) return;
        s_done = true;
        char v[16] = { 0 };
        if (GetEnvironmentVariableA("ADOFAI_PERFECT_ENABLE", v, sizeof(v)) <= 0)
            return;
        int mi = -1;
        if (!_stricmp(v, "4k")) mi = 0;
        else if (!_stricmp(v, "5k")) mi = 1;
        else if (!_stricmp(v, "6k")) mi = 2;
        else if (!_stricmp(v, "10k")) mi = 3;
        if (mi >= 0)
        {
            for (int i = 0; i < kModeN; i++)
                s_en[i].store(i == mi, std::memory_order_relaxed);
            Log::Printf("[debug] mode enabled by env: %s", v);

            // 同批测试钩子：ADOFAI_PERFECT_STYLE=0..5 指定该模式转换风格（0 经典 … 5 冰火手法）
            char st[16] = { 0 };
            if (GetEnvironmentVariableA("ADOFAI_PERFECT_STYLE", st, sizeof(st)) > 0)
            {
                const int sv = atoi(st);
                if (sv >= 0 && sv <= 5)
                {
                    s_style[mi].store(sv, std::memory_order_relaxed);
                    Log::Printf("[debug] style set by env: %d (mode %d)", sv, mi);
                }
            }
        }
    }

    void DrawPlayfield()
    {
        ApplyEnableEnvOnce();
        const int  mi = ActiveModeIndex();
        if (mi < 0)
        {
            RenderHook_SetCaptureWanted(false);
            return;
        }
        // 键位布局由模式决定：4K=DFJK / 5K=SDFJK / 6K=SDFJKL / 10K=ASDFG+HJKL;
        const int  cols = kLanesOf[mi];
        int vkBuf[kMaxLanes];
        for (int i = 0; i < kMaxLanes; i++)
            vkBuf[i] = 0;
        for (int i = 0; i < cols; i++)
            vkBuf[i] = s_vk[mi][i].load(std::memory_order_relaxed);
        const int* vkp = vkBuf;

        float gw = 1280.f, gh = 720.f;
        RenderHook::GetGameWindowSize(&gw, &gh);
        if (gw < 200.f || gh < 200.f)
        {
            static DWORD s_lastSzLog = 0;
            if (GetTickCount() - s_lastSzLog > 2000)
            {
                s_lastSzLog = GetTickCount();
                Log::Printf("[4K] draw: bad window size %.0fx%.0f -> skip", gw, gh);
            }
            return;
        }

        if (!s_skinLoaded)
        {
            static int s_frame = 0;
            if ((s_frame++ % 30) == 0)
                LoadSkin();
        }

        const float u = gh / 1080.f;
        const float cx = gw * 0.5f;
        const double nowT = ImGui::GetTime();

        // ---- 时钟（桥接线程 ~7ms 采样 + QPC 外推）→ 音画同步基准 ----
        const double clock = RenderClock();
        const double songT = s_songTime.load(std::memory_order_relaxed);
        {
            // 诊断：覆盖层是否在画 / 小窗捕获是否就绪（30s 一条）
            static DWORD s_lastDrawLog = 0;
            if (GetTickCount() - s_lastDrawLog > 30000)
            {
                s_lastDrawLog = GetTickCount();
                int cw = 0, ch = 0;
                void* capNow = RenderHook_GetCaptureTex(&cw, &ch);
                size_t live = 0, cons = 0, miss = 0;
                double firstDt = 1e9;
                {
                    std::lock_guard<std::mutex> lk(s_notesMutex);
                    for (size_t i = 0; i < s_notes.size(); i++)
                    {
                        if (s_consumed[i]) cons++;
                        else if (s_missed[i]) miss++;
                        else
                        {
                            if (live == 0)
                                firstDt = s_notes[i].time - clock;
                            live++;
                        }
                    }
                }
                Log::Printf("[4K] draw: gw=%.0f gh=%.0f cap=%p %dx%d mini=%d en=%d n=%d live=%d cons=%d miss=%d firstDt=%.2f",
                            gw, gh, capNow, cw, ch,
                            (int)s_miniOn.load(std::memory_order_relaxed),
                            mi,
                            (int)s_notes.size(), (int)live, (int)cons, (int)miss, firstDt);
            }
        }
        const int    speed = std::max(1, s_speed[mi].load(std::memory_order_relaxed));
        const float  fallTime = 4.6f / (float)speed;

        // ---- 换谱 → 重置渲染侧运行时（特效 / 底光 / KPS） ----
        static double s_keyHist[kMaxLanes][64] = {};
        static int    s_keyHistPos[kMaxLanes] = {};
        {
            static int s_fxGen = -1;
            int gen = s_chartGen.load(std::memory_order_relaxed);
            if (gen != s_fxGen)
            {
                s_fxGen = gen;
                ResetJudgeRuntime(gen);
                for (int i = 0; i < kMaxLanes; i++)
                {
                    for (int k = 0; k < 64; k++)
                        s_keyHist[i][k] = 0.0;
                    s_keyHistPos[i] = 0;
                }
            }
        }

        // ---- DFJK 输入 + 判定 ----
        // 只在"真正在关卡里"时判定：切关/暂停/结算画面都不算成绩，
        // 否则会把还没开始的音符整段判成 Miss（连击数看起来就"不同步"了）。
        CheatState::Status st;
        GameBridge::GetStatusSnapshot(&st);
        // Gate for falling notes / judgement.
        // In custom levels (Worlds / Workshop / extra) scrController.currentState can stay
        // None forever even though the level runs normally (official levels report
        // PlayerControl). currentState only mirrors the FSM and is NOT a reliable
        // 'level started' signal, so use: chart loaded + in gameworld + not paused +
        // song really started. state stays as a fallback for old-conductor mode, plus a
        // clock-motion fallback for levels where the game never raises hasSongStarted.
        const bool chartReady = s_noteCount.load(std::memory_order_relaxed) > 0;
        const bool songStarted = s_songStarted.load(std::memory_order_relaxed);
        const bool fsmPlaying = (st.state == 2 || st.state == 3 || st.state == 4);
        const bool clockMoving = (WallNow() - s_clockMovedWall.load(std::memory_order_relaxed)) < 0.35;
        const bool clockInRange = (clock > -0.25) && (clock < s_chartEndT.load(std::memory_order_relaxed));
        const bool clockFallback = s_anchored.load(std::memory_order_relaxed) && clockMoving && clockInRange;
        const bool inPlay = chartReady && st.gameworld && !st.paused &&
                            (songStarted || fsmPlaying || clockFallback);
        static double s_poseUntil[kMaxLanes] = {};
        bool keyDown[kMaxLanes] = {};
        bool keyHit[kMaxLanes] = {};
        for (int i = 0; i < cols; i++)
            keyDown[i] = (GetAsyncKeyState(vkp[i]) & 0x8000) != 0;
        // 判定：消费桥接线程采集的精确按键事件（事件自带按下瞬间的歌曲时刻）
        {
            const unsigned w = s_keyEvWrite.load(std::memory_order_acquire);
            const double wallNow = WallNow();
            while (s_keyEvRead != w)
            {
                const KeyPressEv ev = s_keyEv[s_keyEvRead % kKeyEvN];
                s_keyEvRead++;
                if (ev.wallT < wallNow - 1.0) continue;   // 暂停/切关期间积压的事件丢弃
                if (ev.lane < 0 || ev.lane >= cols) continue;
                if (ev.vk != vkp[ev.lane])     continue;  // 键位已改，丢弃旧事件
                if (!inPlay)                   continue;
                if (ev.kind == 1)              // 抬起：只用于长按尾端判定
                {
                    JudgeHoldRelease(ev.lane, ev.songT);
                    continue;
                }
                keyHit[ev.lane] = true;
                SkinLua::PushInput(1, ev.lane + 1);   // 皮肤 Lua：OnInput/InputEvent
                s_keyPressCount[ev.lane].fetch_add(1, std::memory_order_relaxed);
                s_keyHist[ev.lane][(s_keyHistPos[ev.lane]++) & 63] = nowT;
                JudgePress(ev.lane, ev.songT);
                s_poseUntil[ev.lane] = nowT + 0.18;
            }
        }
        for (int i = 0; i < cols; i++)
            if (keyDown[i] && inPlay)
                s_poseUntil[i] = std::max(s_poseUntil[i], nowT + 0.10);
        // 自动 / 宏打歌：内部虚拟按键（不注入系统键鼠），与真人输入共用同一条判定管线
        //   （一次性环境变量钩子：ADOFAI_PERFECT_AUTOPLAY=1|macro、ADOFAI_PERFECT_RECORD=1、
        //     ADOFAI_PERFECT_RECDIR=<目录>，仅用于自动化测试）
        {
            static bool s_envOnce = false;
            if (!s_envOnce)
            {
                s_envOnce = true;
                const char* ap = getenv("ADOFAI_PERFECT_AUTOPLAY");
                if (ap && ap[0])
                {
                    if (!_stricmp(ap, "macro")) s_macroPlay[mi].store(true, std::memory_order_relaxed);
                    else if (ap[0] == '1')      s_autoPlay[mi].store(true, std::memory_order_relaxed);
                    Log::Printf("[test] autoplay hook '%s' -> auto=%d macro=%d", ap,
                                (int)s_autoPlay[mi].load(std::memory_order_relaxed),
                                (int)s_macroPlay[mi].load(std::memory_order_relaxed));
                }
                const char* rd = getenv("ADOFAI_PERFECT_RECDIR");
                if (rd && rd[0]) { RecDirSet(rd); Log::Printf("[test] rec dir '%s'", rd); }
                const char* rc = getenv("ADOFAI_PERFECT_RECORD");
                if (rc && rc[0] == '1')
                {
                    RecAutoSet(0);
                    RecOnSet(1);
                    Log::Printf("[test] record hook on");
                }
                const char* fm = getenv("ADOFAI_PERFECT_FIRE");
                if (fm && fm[0] == '1')
                {
                    s_macroFire.store(true, std::memory_order_relaxed);   // 测试钩子：不写配置
                    for (int mi2 = 0; mi2 < kModeN; mi2++)
                    {
                        s_macroPlay[mi2].store(false, std::memory_order_relaxed);
                        s_autoPlay[mi2].store(false, std::memory_order_relaxed);
                    }
                    Log::Printf("[test] fire macro hook on");
                }
            }
        }
        if (inPlay)
        {
            static double s_lastNowT = 0.0;
            double frameDt = (s_lastNowT > 0.0) ? (nowT - s_lastNowT) : 0.016;
            s_lastNowT = nowT;
            if (frameDt < 0.0 || frameDt > 0.5) frameDt = 0.016;
            const double lead = std::min(0.12, std::max(0.020, frameDt * 1.5));
            AutoPlayTick(clock, nowT, lead);
            {
                static DWORD s_lastAutoLog = 0;
                const bool autoOn = s_autoPlay[mi].load(std::memory_order_relaxed) ||
                                    s_macroPlay[mi].load(std::memory_order_relaxed);
                if (autoOn && GetTickCount() - s_lastAutoLog > 5000)
                {
                    s_lastAutoLog = GetTickCount();
                    const int total = s_jdTotal.load(std::memory_order_relaxed);
                    const double wsum = s_jdWeight.load(std::memory_order_relaxed);
                    Log::Printf("[auto] idx=%d n=%d combo=%d hits=%d acc=%.2f%% clock=%.2f",
                                s_autoIdx, s_noteCount.load(std::memory_order_relaxed),
                                s_jdCombo.load(std::memory_order_relaxed), total,
                                (total > 0) ? (wsum / (double)total * 100.0) : 100.0, clock);
                }
            }
            for (int i = 0; i < cols; i++)
            {
                if (s_virtualDown[i] && clock > s_virtualUntil[i])
                    s_virtualDown[i] = false;
                if (s_virtualDown[i])
                    keyDown[i] = true;
            }
        }
        else
        {
            for (int i = 0; i < kMaxLanes; i++)
                s_virtualDown[i] = false;
        }
        // 自动录制：画面源 = 游戏原生后缓冲（不含本工具覆盖层）
        {
            static bool s_wasInPlay = false;
            RecCfgLoadOnce();
            const bool recOn = s_recOn.load(std::memory_order_relaxed);
            const bool recPaused = s_recPaused.load(std::memory_order_relaxed);
            RenderHook_SetRecordWanted(recOn && !recPaused);
            if (recOn && !recPaused)
            {
                const bool autoRec = s_recAuto.load(std::memory_order_relaxed);
                const int fps = s_recFps.load(std::memory_order_relaxed);
                const int mbps = s_recMbps.load(std::memory_order_relaxed);
                if (!autoRec)
                {
                    if (!GameRecorder::Active())
                    {
                        Log::Printf("[rec] start (manual, dir='%s')", s_recDir);
                        GameRecorder::Start(s_recDir, fps, mbps);
                    }
                }
                else if (inPlay && !s_wasInPlay)
                {
                    if (!GameRecorder::Active()) GameRecorder::Start(s_recDir, fps, mbps);
                    Log::Printf("[rec] auto start (inPlay)");
                }
                else if (!inPlay && s_wasInPlay)
                {
                    GameRecorder::Stop();
                    Log::Printf("[rec] auto stop");
                }
            }
            else if (GameRecorder::Active())
            {
                Log::Printf("[rec] stop (toggle off)");
                GameRecorder::Stop();
            }
            s_wasInPlay = inPlay;
        }
        if (s_resetKeys.exchange(false, std::memory_order_relaxed))
            for (int i = 0; i < kMaxLanes; i++)
                s_keyPressCount[i].store(0, std::memory_order_relaxed);
        if (inPlay)
        {
            JudgeMissScan(clock);
            JudgeHoldScan(clock, keyDown);
        }

        // KPS：最近 1 秒内每轨按键次数（右上数字）
        {
            int total = 0;
            for (int i = 0; i < cols; i++)
            {
                int c = 0;
                for (int k = 0; k < 64; k++)
                {
                    double ts = s_keyHist[i][k];
                    if (ts > 0.0001 && nowT - ts <= 1.0)
                        c++;
                }
                s_kps[i] = c;
                total += c;
            }
            s_kps[cols] = total;
        }

        // [调试] ADOFAI_PERFECT_FAKEHIT=1：周期性注入合成打击（仅供无输入设备时截图验证 MSP
        // 打击特效/判定提示；不发送任何键鼠事件、不写配置）
        {
            static const char* s_fake = getenv("ADOFAI_PERFECT_FAKEHIT");
            if (s_fake && (s_fake[0] == '1' || s_fake[0] == '2'))
            {
                // =2：持续保持一个 age=0.12s 的打击特效（供截图确认动画帧位）
                static double s_next = 0.0;
                static int s_n = 0;
                if (s_fake[0] == '2')
                {
                    static double s_next2 = 0.0;
                    if (nowT >= s_next2)
                    {
                        s_next2 = nowT + 0.60;
                        // 测试：同步推进判定计数，便于截图核对 hitcount 数字（皮肤 Lua 用）
                        const int fk = (s_n++) % 4;
                        s_jdCounts[fk].fetch_add(1, std::memory_order_relaxed);
                        if (fk <= 2) s_jdGold[fk].fetch_add(1, std::memory_order_relaxed);
                    }
                    HitFx hf; hf.lane = s_n % cols; hf.kind = s_n % 4;
                    hf.t0 = nowT - 0.12; hf.scale = 1.f;
                    s_fx.push_back(hf);
                    if (s_fx.size() > 64) s_fx.erase(s_fx.begin());
                    s_jdLastKind.store(s_n % 4, std::memory_order_relaxed);
                    s_jdLastTime.store(RenderClock() - 0.12, std::memory_order_relaxed);
                    s_jdLastOff.store(0.012, std::memory_order_relaxed);
                    s_jdCombo.store(12, std::memory_order_relaxed);
                }
                else if (nowT >= s_next)
                {
                    s_next = nowT + 0.55;
                    const int lane = s_n % cols;
                    const int kind = s_n % 4;   // 0 Marv / 1 Best / 2 Good / 3 Miss 轮换
                    s_n++;
                    HitFx fx; fx.lane = lane; fx.kind = kind; fx.t0 = nowT; fx.scale = 1.f;
                    s_fx.push_back(fx);
                    if (s_fx.size() > 64) s_fx.erase(s_fx.begin());
                    s_jdLastKind.store(kind, std::memory_order_relaxed);
                    s_jdLastTime.store(RenderClock(), std::memory_order_relaxed);
                    s_jdLastOff.store((kind == 0) ? 0.012 : (kind == 1 ? -0.020 : 0.045), std::memory_order_relaxed);
                    s_jdCounts[kind].fetch_add(1, std::memory_order_relaxed);
                    if (kind <= 1) s_jdGold[kind].fetch_add(1, std::memory_order_relaxed);   // 假命中 12/20ms → 金判
                    if (kind == 3) s_jdCombo.store(0, std::memory_order_relaxed);
                    else
                    {
                        int cb = s_jdCombo.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (cb > s_jdMaxCombo.load(std::memory_order_relaxed))
                            s_jdMaxCombo.store(cb, std::memory_order_relaxed);
                    }
                }
            }
        }

        // ---- 几何（全部 1080p 基准） ----
        // MSP 整屏皮肤：轨道铺满整宽（Malody 键位布局 = 屏宽/键数；Gazer press 模块 x=0/20/40%…
        // 与 Rurudo keylight x=12.5/37.5% 都指向"轨宽 = 屏宽/键数、轨心 = (i+0.5)*轨宽"）
        const bool msp = s_mspFull;
        // MSP 轨道宽 = 场景 'Track Rect' 宽 × Meta.Key.Scale：
        //  · level16/23/26/28 场景 'Track Rect'（'Track Scale' 子节点，锚点 0..1 拉伸，canvas 参考宽 1920）
        //    sizeDelta.x = -240.759521484375 ⇒ 轨道父矩形宽 = canvasW_local - 240.7595（= 1679.24 @1920）。
        //  · 像素验证（Mango233 参考截图）：黑条 588px = 1679.24 × 0.350313 × k（k≈1，0.1% 误差）。
        //  · 旧实现 gw×scale（=1920u×scale）使轨道偏宽 ≈14%，判定圈相对道宽偏小。
        const float trackW0 = gw - 240.7595f * u;
        float mspTrackW = (s_mspKeyScale > 0.01f) ? (trackW0 * s_mspKeyScale) : trackW0;
        if (mspTrackW > gw) mspTrackW = gw;
        if (mspTrackW < 1.f) mspTrackW = gw;
        const float mspLaneW = mspTrackW / (float)cols;
        const float mspTrackL = cx - mspTrackW * 0.5f;
        {
            static float dbgW = -1.f;   // 每皮肤加载只打一次
            if (msp && dbgW != mspTrackW)
            {
                dbgW = mspTrackW;
                Log::Printf("[skin] track geom: gw=%.0f scale=%.4f trackL=%.1f trackW=%.1f laneW=%.1f",
                            gw, s_mspKeyScale, mspTrackL, mspTrackW, mspLaneW);
            }
        }
        const float pfL = msp ? mspTrackL : (cx - 372.f * u);
        const float pfR = msp ? (mspTrackL + mspLaneW * (float)cols) : (cx + 372.f * u);
        const float goldW = msp ? 0.f : 11.f * u;
        const float innerL = pfL + goldW, innerR = pfR - goldW;
        const float innerW = innerR - innerL;
        const float laneW = msp ? mspLaneW : (708.f * u / (float)cols);
        const float lane0 = msp ? mspTrackL : (innerL + 7.f * u);
        const float noteW = msp ? (laneW * 0.94f) : (laneW - 9.f * u);
        const float noteH = std::min(63.f * u, std::max(30.f * u, noteW * 0.42f));
        const float judgeY = 915.f * u;
        const float keyTop = 971.f * u, keyH = 60.f * u;
        const float keyBarTop = 1031.f * u, keyBarH = 20.f * u;
        const float pxPerSec = judgeY / fallTime;
        int opacity = s_opacity.load(std::memory_order_relaxed);
        if (opacity < 0) opacity = 0;
        if (opacity > 255) opacity = 255;
        const bool hitFx = s_hitFx.load(std::memory_order_relaxed);

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(gw, gh), ImGuiCond_Always);
        ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                              ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                              ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_NoFocusOnAppearing |   // 皮肤全屏绘制窗口绝不抢焦点/置顶（否则盖住菜单）
                              ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        char pfId[32];
        snprintf(pfId, sizeof(pfId), "##playfield_%d", mi);
        ImGui::Begin(pfId, nullptr, fl);
        // 皮肤整屏窗口永远压到 ImGui 显示序最底层：菜单/MOD 窗口/KeyViewer 都在其之上，
        // 避免皮肤立绘（L4 大图）盖住本工具界面。依据：imgui.cpp BringWindowToDisplayBack。
        ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // ---- 轨道底板 + 网格 + 金边 ----
        if (!msp && s_texBg && s_bgEnabled)
            dl->AddImage(TRef(s_texBg), ImVec2(pfL, 0.f), ImVec2(pfR, gh),
                         ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), IM_COL32(255, 255, 255, 235));
        if (!msp)
        {
        dl->AddRectFilled(ImVec2(innerL, 0.f), ImVec2(innerR, gh), IM_COL32(9, 9, 8, opacity));
        {
            float cell = innerW / 8.f;
            for (int i = 1; i < 8; i++)
                dl->AddLine(ImVec2(innerL + cell * i, 0.f), ImVec2(innerL + cell * i, gh),
                            IM_COL32(255, 255, 255, 14), 1.f);
            for (float y = cell; y < gh; y += cell)
                dl->AddLine(ImVec2(innerL, y), ImVec2(innerR, y), IM_COL32(255, 255, 255, 12), 1.f);
            for (int i = 1; i < cols; i++)
            {
                float x = lane0 + laneW * i;
                dl->AddLine(ImVec2(x, 0.f), ImVec2(x, keyBarTop + keyBarH),
                            IM_COL32(255, 255, 255, 40), 1.4f * u);
            }
        }
        {
            ImU32 goldHi = IM_COL32(255, 236, 172, 255);
            ImU32 goldLo = IM_COL32(170, 138, 98, 255);
            dl->AddRectFilledMultiColor(ImVec2(pfL, 0.f), ImVec2(pfL + goldW, gh),
                                        goldHi, goldLo, goldLo, goldHi);
            dl->AddRectFilledMultiColor(ImVec2(pfR - goldW, 0.f), ImVec2(pfR, gh),
                                        goldLo, goldHi, goldHi, goldLo);
        }
        }

        // ---- 音符临近程度（键块随之亮起；未开始不做） ----
        float laneNear[kMaxLanes] = {};
        if (inPlay)
        {
            std::lock_guard<std::mutex> lk(s_notesMutex);
            for (size_t i = 0; i < s_notes.size(); i++)
            {
                const Note& n = s_notes[i];
                double dt = n.time - clock;
                if (dt < -0.18)
                    continue;
                if (dt > fallTime)
                    break;
                if (dt < 0.10)
                {
                    float a = 1.f - (float)(dt < 0 ? -dt : dt) / 0.18f;
                    if (a > laneNear[n.lane])
                        laneNear[n.lane] = a;
                }
            }
        }
        for (int i = 0; i < kMaxLanes; i++)
        {
            s_laneFlash[i] *= (float)pow(0.02, (1.0 / 60.0) * 8.0);   // 判定闪条快速衰减
            s_laneHitBg[i] *= (float)pow(0.02, (1.0 / 60.0) * 5.0);
            if (keyHit[i])
            {
                s_laneFlash[i] = std::max(s_laneFlash[i], 0.55f);
                s_laneHitBg[i] = std::max(s_laneHitBg[i], 0.85f);
            }
        }

        // ---- 判定底光（皮肤 notehitbg，按轨切成 cols 段；引擎定位在判定线下方） ----
        if (s_texHitBg && (!msp || s_hitBgOwn))
        {
            int nfo[6] = {};
            const bool hi = (s_hitBgImgName[0] != 0) && ModTexInfo(s_skinDirCur, s_hitBgImgName, nfo);
            const float iw = (hi && nfo[0] > 0) ? (float)nfo[0] : 1680.f;
            const float ihh = (hi && nfo[1] > 0) ? (float)nfo[1] : 200.f;
            const float bx = (hi && nfo[4] > 0) ? (float)nfo[2] : 0.f;
            const float by = (hi && nfo[5] > 0) ? (float)nfo[3] : 4.f;
            const float bw = (hi && nfo[4] > 0) ? (float)nfo[4] : iw;
            const float bhh = (hi && nfo[5] > 0) ? (float)nfo[5] : 192.f;
            float by0 = judgeY, by1 = judgeY + 88.6f * u;
            float segW = laneW;
            for (int i = 0; i < cols; i++)
            {
                float a = 0.20f * laneNear[i] + s_laneHitBg[i];
                if (a <= 0.004f)
                    continue;
                if (a > 1.f) a = 1.f;
                float x0 = lane0 + segW * i;
                float x1 = x0 + segW;
                const float us = bw / (float)cols;
                dl->AddImage(TRef(s_texHitBg), ImVec2(x0, by0), ImVec2(x1, by1),
                             ImVec2((bx + i * us + us * 0.03f) / iw, by / ihh),
                             ImVec2((bx + (i + 1) * us - us * 0.03f) / iw, (by + bhh) / ihh),
                             IM_COL32(255, 255, 255, (int)(a * 235.f)));
            }
        }

        // ---- 上隐 / 下隐（Malody 式）：只控制音符可见度，不影响判定 ----
        const bool upHide = s_upHide[mi].load(std::memory_order_relaxed);
        const bool dnHide = s_dnHide[mi].load(std::memory_order_relaxed);
        const float upDepth = judgeY * 0.42f;      // 高处超过这个距离完全隐藏
        const float dnDepth = judgeY * 0.16f;      // 贴近判定线这一段隐藏

        // ---- MSP 整屏皮肤：模块（layer<=2 在音符后面；layer>=3 在音符前面）----
        float laneAct[kMaxLanes] = {};
        for (int i = 0; i < cols && i < kMaxLanes; i++)
        {
            if (keyDown[i]) laneAct[i] = 1.f;
            else if (s_poseUntil[i] > nowT) laneAct[i] = (float)((s_poseUntil[i] - nowT) / 0.18);
        }
        MspCtx C{};
        if (msp)
        {
            const bool mspOff2 = ([](){ static const char* e = getenv("ADOFAI_PERFECT_MSPOFF"); return e && e[0] == '1'; })();
            C.dir = s_skinDirCur;
            C.gw = gw; C.gh = gh; C.u = u;
            C.trackL = mspTrackL; C.trackW = mspTrackW;
            C.laneW = mspLaneW; C.judgeY = judgeY;
            C.progress = (float)st.percentComplete;
            C.cols = cols;
            C.combo = s_jdCombo.load(std::memory_order_relaxed);
            C.maxCombo = s_jdMaxCombo.load(std::memory_order_relaxed);
            for (int i = 0; i < 4; i++) C.counts[i] = s_jdCounts[i].load(std::memory_order_relaxed);
            for (int i = 0; i < 4; i++) C.gold[i] = s_jdGold[i].load(std::memory_order_relaxed);
            C.score = (float)(C.counts[0] * 101 + C.counts[1] * 100 + C.counts[2] * 70);
            C.trackAngle = s_mspKeyAngle;
            C.track3D = (s_mspKeyUse3D != 0);
            C.trackLen = (C.track3D && C.trackAngle > 0.5f) ? (20000.f * u) : 0.f;   // 预制体 distanceFarEnd
            C.trackD = Msp3DActive(C) ? Msp3DD(C) : 0.f;
            C.trackScale = (s_mspKeyScale > 0.01f) ? s_mspKeyScale : 1.f;
            C.ringDis = s_mspRingDis;
            int jdTotal = s_jdTotal.load(std::memory_order_relaxed);
            double jdWeight = s_jdWeight.load(std::memory_order_relaxed);
            C.acc = (jdTotal > 0) ? (jdWeight / jdTotal) : 1.0;
            C.bpm = s_bpm.load(std::memory_order_relaxed);
            C.level = s_level.load(std::memory_order_relaxed);
            C.levelName = (st.levelName[0] != 0 && strncmp(st.levelName, "scn", 3) != 0) ? st.levelName : "";
            C.songT = songT;
            C.songLen = s_chartEndT.load(std::memory_order_relaxed);
            C.judgeKind = s_jdLastKind.load(std::memory_order_relaxed);
            C.judgeAge = clock - s_jdLastTime.load(std::memory_order_relaxed);
            C.judgeOff = s_jdLastOff.load(std::memory_order_relaxed);
            C.kps = s_kps[cols];
            C.kpsMax = 0;
            for (int i = 0; i < kMaxLanes + 1; i++) if (s_kps[i] > C.kpsMax) C.kpsMax = s_kps[i];
            C.keyDown = keyDown;
            C.laneAct = laneAct;
            C.nowT = nowT;
            C.inPlay = inPlay;
            EnsureSkinLua(C);
            s_skinLuaActive = SkinLua::Active();
            if (s_skinLuaActive)
            {
                SkinLua::Live lv{};
                lv.songMs = C.songT * 1000.0;
                lv.combo = C.combo;
                lv.maxCombo = C.maxCombo;
                for (int i = 0; i < 4; i++) lv.counts[i] = C.counts[i];
                lv.acc = C.acc;
                lv.hp = C.hp;
                lv.progress = C.progress;
                lv.score = C.score;
                lv.kps = C.kps;
                lv.kpsMax = C.kpsMax;
                lv.inPlay = C.inPlay;
                // 无人值守测试钩子（仅 ADOFAI_PERFECT_FAKEHITS=1）：周期性注入判定事件，
                // 用于验证皮肤 Lua 的 OnHit/HitEvent → 连击图/音效链路；正常游玩不生效。
                {
                    static const char* fh = getenv("ADOFAI_PERFECT_FAKEHITS");
                    if (fh && fh[0] == '1')
                    {
                        static double s_fakeNext = 0.0;
                        static int s_fakeK = 0;
                        if (C.songT >= s_fakeNext)
                        {
                            s_fakeNext = C.songT + 0.4;
                            const int kinds[4] = { 0, 1, 2, 3 };
                            const int fk = kinds[s_fakeK & 3];
                            SkinLua::PushHit(fk, (s_fakeK & 1) ? 9.0 : -6.0);
                            // 同步推进判定计数：让依赖 {miss}/{combo} 文本的皮肤脚本
                            // （如 Murasame hit_effect 的 ciallo 大图）走完整触发链路。
                            if (fk == 3)
                            {
                                s_jdCounts[3].fetch_add(1, std::memory_order_relaxed);
                                s_jdCombo.store(0, std::memory_order_relaxed);
                            }
                            else
                            {
                                s_jdCounts[fk].fetch_add(1, std::memory_order_relaxed);
                                s_jdCombo.fetch_add(1, std::memory_order_relaxed);
                            }
                            s_fakeK++;
                        }
                    }
                }
                SkinLua::Frame(lv);
            }
            if (!mspOff2) MspDrawPass(dl, C, 0);
        }

        // 每轨音符尺寸（MSP：用贴图可见区域，1080p 基准 × u）
        float noteWL[kMaxLanes], noteHL[kMaxLanes];
        ImVec2 noteUV0L[kMaxLanes], noteUV1L[kMaxLanes];
        for (int i = 0; i < kMaxLanes; i++)
        {
            noteWL[i] = noteW;
            noteHL[i] = noteH;
            noteUV0L[i] = ImVec2(10.f / 420.f, 0.f);
            noteUV1L[i] = ImVec2(410.f / 420.f, 200.f / 400.f);
        }
        if (msp)
        {
            for (int i = 0; i < kMaxLanes; i++)
            {
                float w = s_noteNatW[i] * u, h = s_noteNatH[i] * u;
                if (w > laneW * 0.98f && w > 8.f)
                {
                    float sc = laneW * 0.98f / w;
                    w *= sc; h *= sc;
                }
                if (w > 4.f && h > 4.f)
                {
                    noteWL[i] = w;
                    noteHL[i] = h;
                    noteUV0L[i] = ImVec2(s_noteU0[i], s_noteV0[i]);
                    noteUV1L[i] = ImVec2(s_noteU1[i], s_noteV1[i]);
                }
            }
        }

        // ---- 音符（含长按条）：只在关卡真正开始后下落，未开始不显示 ----
        if (s_texNote && inPlay)
        {
            std::lock_guard<std::mutex> lk(s_notesMutex);
            for (size_t i = 0; i < s_notes.size(); i++)
            {
                const Note& n = s_notes[i];
                const bool eaten = s_consumed[i];
                const bool missedNote = s_missed[i];
                // 长按条：判定后只要还在按着，就继续显示到尾端时间
                // （否则一按下这个音符就整条消失，长条看不到）
                bool holding = false;
                if (n.hold > 0.f && eaten && s_tailDone[i] != 2 &&
                    n.lane >= 0 && n.lane < kMaxLanes)
                {
                    double tailT = n.time + (double)n.hold;
                    if (clock <= tailT + 0.02 &&
                        (keyDown[n.lane] || s_poseUntil[n.lane] > nowT))
                        holding = true;
                }
                if ((eaten || missedNote) && !holding)
                    continue;
                double dt = n.time - clock;
                float yc = judgeY - (float)dt * pxPerSec;   // 音符中心
                const bool laneOk = (n.lane >= 0 && n.lane < kMaxLanes);
                // 3D 斜轨道：音符中心与尺寸按透视投影（TrackAngle 越大越向屏幕中心收敛）
                const bool n3d = msp && Msp3DActive(C) && laneOk;
                float nSc = 1.f;
                float laneCX = lane0 + laneW * ((float)n.lane + 0.5f);
                // 3D 斜轨道：音符/长条是挂在旋转后的 Track 节点下的平面物件（贴在斜面上），
                // 四角各自透视投影 → 随坡度强烈纵向压扁（45° 轨道上音符"接近扁平"）+ 侧边向灭点收敛。
                // 逆向证据（GameAssembly.dll）：
                //  · PlayNoteKey.UpdateObject @0x180353F20 / PlayBarKey.UpdateObject @0x18034B320：
                //      transform.localPosition.y = WidgetKeyTrack3D.GetNoteDistanceFromDeltaY(deltaY)
                //      （位置=沿轨道距离；节点父级=旋转后的 Track → 贴图躺在斜面上，非屏幕轴对齐）
                //        旧实现只把中心点做透视、贴图仍是屏幕轴对齐矩形（实测用户反馈「NOTE 不够斜」）。
                auto planeQuad = [&](float x0p, float x1p, float yTop2d, float yBot2d, ImVec2 q[4])
                {
                    const float A0 = Msp3DAsolve(C, yBot2d);   // 下边（靠近判定线，距离小）
                    const float A1 = Msp3DAsolve(C, yTop2d);   // 上边（远离判定线）
                    Msp3DPlaneQuad(C, x0p, x1p, A0, A1, q);
                };
                const float nw = ((msp && laneOk) ? noteWL[n.lane] : noteW) * nSc;
                const float nh = ((msp && laneOk) ? noteHL[n.lane] : noteH) * nSc;
                if (yc < -nh && !holding)
                    continue;
                if (dt > fallTime)
                    break;
                float lx = laneCX - nw * 0.5f;
                void* ntex = (laneOk && s_texNoteL[n.lane]) ? s_texNoteL[n.lane] : s_texNote;
                void* htex = (laneOk && s_texHoldBodyL[n.lane]) ? s_texHoldBodyL[n.lane] : s_texHold;
                const bool hCustom = laneOk && s_holdCustom[n.lane];
                const bool nCustom = laneOk && s_noteCustom[n.lane];
                const ImVec2 nuv0 = (msp && laneOk) ? noteUV0L[n.lane]
                                   : (nCustom ? ImVec2(0.f, 0.f) : ImVec2(10.f / 420.f, 0.f));
                const ImVec2 nuv1 = (msp && laneOk) ? noteUV1L[n.lane]
                                   : (nCustom ? ImVec2(1.f, 1.f) : ImVec2(410.f / 420.f, 200.f / 400.f));

                // 可见度：上隐（越高越淡）+ 下隐（越贴近判定线越淡）
                float vis = 1.f;
                {
                    const float d = judgeY - yc;
                    if (d > 0.f)
                    {
                        if (upHide)
                        {
                            float f = 1.f - d / upDepth;
                            if (f < 0.f) f = 0.f;
                            vis *= f;
                        }
                        if (dnHide)
                        {
                            float f = d / dnDepth;
                            if (f > 1.f) f = 1.f;
                            vis *= f;
                        }
                    }
                }
                if (vis <= 0.02f && !holding)
                    continue;
                const ImU32 tint = IM_COL32(255, 255, 255, (int)(vis * 255.f));

                // 长按条（lnx6.png：只有 x=299..400 这一段是实体）
                if (n.hold > 0.f && htex && s_tailDone[i] != 2)
                {
                    float yHead = judgeY - (float)(n.time - clock) * pxPerSec;
                    float yTail = judgeY - (float)(n.time + n.hold - clock) * pxPerSec;
                    if (n3d)
                    {
                        float px = 0, py = 0, ps = 1.f;
                        Msp3DPoint(C, 0.f, yHead, &px, &py, &ps); yHead = py;
                        Msp3DPoint(C, 0.f, yTail, &px, &py, &ps); yTail = py;
                    }
                    if (holding)
                        yHead = judgeY;              // 已按下：从判定线向上延续
                    float hw = nw * 0.30f;
                    float hy0 = std::max(std::min(yHead, yTail), 0.f);
                    float hy1 = std::min(std::max(yHead, yTail), judgeY + nh * 0.5f);
                    if (hy1 > hy0)
                    {
                        const float hx0 = lx + nw * 0.5f - hw * 0.5f;
                        const float hx1 = lx + nw * 0.5f + hw * 0.5f;
                        const ImVec2 huv0 = hCustom ? ImVec2(0.f, 0.f) : ImVec2(299.f / 700.f, 0.f);
                        const ImVec2 huv1 = hCustom ? ImVec2(1.f, 1.f) : ImVec2(400.f / 700.f, 1.f);
                        if (holding)
                        {
                            // 按住特效：外发光 + 顶端的"松手点"亮线（ADOFAI 松手=下一拍）
                            const float pulse = 0.5f + 0.5f * (float)sin(nowT * 17.0);
                            const ImU32 glow = IM_COL32(255, 205, 132, (int)(30.f + 52.f * pulse));
                            const ImU32 body = IM_COL32(255, 236, 196, (int)(215.f + 40.f * pulse));
                            if (n3d)
                            {
                                ImVec2 q[4];
                                planeQuad(hx0 - 4.f * u, hx1 + 4.f * u, hy0 - 2.f * u, hy1 + 2.f * u, q);
                                dl->AddQuadFilled(q[0], q[1], q[2], q[3], glow);
                                planeQuad(hx0, hx1, hy0, hy1, q);
                                dl->AddImageQuad(TRef(htex), q[0], q[1], q[2], q[3],
                                                 ImVec2(huv0.x, huv1.y), huv1,
                                                 ImVec2(huv1.x, huv0.y), huv0, body);
                                if (hy1 - hy0 > 8.f * u)
                                {
                                    planeQuad(hx0 - 2.f * u, hx1 + 2.f * u, hy0, hy0 + 3.f * u, q);
                                    dl->AddQuadFilled(q[0], q[1], q[2], q[3],
                                                      IM_COL32(255, 248, 222, (int)(150.f + 105.f * pulse)));
                                }
                            }
                            else
                            {
                                dl->AddRectFilled(ImVec2(hx0 - 4.f * u, hy0 - 2.f * u),
                                                  ImVec2(hx1 + 4.f * u, hy1 + 2.f * u),
                                                  glow, 5.f * u);
                                dl->AddImage(TRef(htex), ImVec2(hx0, hy0), ImVec2(hx1, hy1),
                                             huv0, huv1, body);
                                if (hy1 - hy0 > 8.f * u)
                                    dl->AddRectFilled(ImVec2(hx0 - 2.f * u, hy0),
                                                      ImVec2(hx1 + 2.f * u, hy0 + 3.f * u),
                                                      IM_COL32(255, 248, 222, (int)(150.f + 105.f * pulse)),
                                                      1.5f * u);
                            }
                        }
                        else if (n3d)
                        {
                            ImVec2 q[4];
                            planeQuad(hx0, hx1, hy0, hy1, q);
                            dl->AddImageQuad(TRef(htex), q[0], q[1], q[2], q[3],
                                             ImVec2(huv0.x, huv1.y), huv1,
                                             ImVec2(huv1.x, huv0.y), huv0, tint);
                        }
                        else
                            dl->AddImage(TRef(htex), ImVec2(hx0, hy0), ImVec2(hx1, hy1),
                                         huv0, huv1, tint);
                    }
                    // 按住期间轨道底光脉冲（Malody 长按手感）
                    if (holding)
                    {
                        const float pulse = 0.5f + 0.5f * (float)sin(nowT * 17.0);
                        s_laneHitBg[n.lane] = std::max(s_laneHitBg[n.lane], 0.42f + 0.30f * pulse);
                    }
                }

                if (eaten || missedNote)
                {
                    if (holding && ntex)
                    {
                        // 按住期间：头部音符钉在判定线上发光，给出持续反馈
                        const float pulse = 0.5f + 0.5f * (float)sin(nowT * 20.0);
                        const ImU32 hc = IM_COL32(255, 226, 170, (int)(60.f + 90.f * pulse));
                        if (n3d)
                        {
                            ImVec2 q[4];
                            Msp3DPlaneQuad(C, lx - 3.f * u, lx + nw + 3.f * u,
                                           -nh * 0.5f - 3.f * u, nh * 0.5f + 3.f * u, q);
                            dl->AddImageQuad(TRef(ntex), q[0], q[1], q[2], q[3],
                                             ImVec2(nuv0.x, nuv1.y), nuv1,
                                             ImVec2(nuv1.x, nuv0.y), nuv0, hc);
                        }
                        else
                            dl->AddImage(TRef(ntex),
                                         ImVec2(lx - 3.f * u, judgeY - nh * 0.5f - 3.f * u),
                                         ImVec2(lx + nw + 3.f * u, judgeY + nh * 0.5f + 3.f * u),
                                         nuv0, nuv1, hc);
                    }
                    continue;                       // 头部已判定：只保留长按条
                }

                float yb = std::min(yc + nh * 0.5f, judgeY + nh * 0.5f);
                if (ntex)
                {
                    if (n3d)
                    {
                        // 平面内固定尺寸音符（宽度/长度都随透视缩放 → 斜轨上音符为收敛四边形，近似扁平）
                        ImVec2 q[4];
                        const float Ac = Msp3DAsolve(C, yc);
                        Msp3DPlaneQuad(C, lx, lx + nw, Ac - nh * 0.5f, Ac + nh * 0.5f, q);
                        dl->AddImageQuad(TRef(ntex), q[0], q[1], q[2], q[3],
                                         ImVec2(nuv0.x, nuv1.y), nuv1,
                                         ImVec2(nuv1.x, nuv0.y), nuv0, tint);
                    }
                    else
                        dl->AddImage(TRef(ntex), ImVec2(lx, yb - nh), ImVec2(lx + nw, yb),
                                     nuv0, nuv1, tint);
                }
            }
        }

        // ---- MSP 上层（皮肤 layer/order 排序中音符模块之后的模块：数字/人物/按键光…）----
        if (msp)
        {
            static const char* s_mspOff = getenv("ADOFAI_PERFECT_MSPOFF");
            if (!(s_mspOff && s_mspOff[0] == '1'))
            {
            MspDrawPass(dl, C, 1);
            MspDrawHitFx(dl, C);
            MspDrawJudge(dl, C);
            }
        }

        // ---- 判定线（皮肤 noteline 贴图，引擎定位：横跨轨道、位于判定高度）----
        // MSP 模式：只画皮肤自带的判定线（roles.line 在本皮肤目录内）；
        // 皮肤没提供时交给模块表（judgeline/noteline 矢量模块），绝不用内置 noteinex 顶替。
        const bool drawLine = s_texLine && (!msp || s_mspHasLine);
        if (drawLine)
        {
            int nfo[6] = {};
            const bool li = (s_lineImgName[0] != 0) && ModTexInfo(s_skinDirCur, s_lineImgName, nfo);
            float lh = 19.f * u;
            if (li && nfo[4] > 0 && nfo[5] > 0)
            {
                lh = (pfR - pfL) * (float)nfo[5] / (float)nfo[4];
                if (lh < 6.f * u) lh = 6.f * u;
                if (lh > 48.f * u) lh = 48.f * u;
            }
            ImVec2 luv0(0.f, 0.f), luv1(1.f, 1.f);
            if (li && nfo[0] > 0 && nfo[1] > 0)
            {
                luv0 = ImVec2((float)nfo[2] / nfo[0], (float)nfo[3] / nfo[1]);
                luv1 = ImVec2((float)(nfo[2] + nfo[4]) / nfo[0], (float)(nfo[3] + nfo[5]) / nfo[1]);
            }
            dl->AddImage(TRef(s_texLine), ImVec2(pfL, judgeY - lh * 0.5f),
                         ImVec2(pfR, judgeY + lh * 0.5f), luv0, luv1,
                         IM_COL32(255, 255, 255, 235));
        }
        else if (!msp)
            dl->AddRectFilled(ImVec2(pfL, judgeY - 2.f * u), ImVec2(pfR, judgeY + 2.f * u),
                              IM_COL32(255, 205, 153, 220));

        if (!msp)
        {
        // ---- 判定闪条（judge-N：1=黄 2=绿 3=粉） ----
        if (hitFx && inPlay)
        {
            int lastKind = s_jdLastKind.load(std::memory_order_relaxed);
            double lastT = s_jdLastTime.load(std::memory_order_relaxed);
            int lane = s_jdLastLane.load(std::memory_order_relaxed);
            float age = (float)(clock - lastT);
            if (lastKind >= 0 && age >= 0.f && age < 0.16f && lane >= 0 && lane < cols)
            {
                int idx = (lastKind == 0 || lastKind == 1) ? 1 : (lastKind == 2) ? 2 : 3;
                if (s_texJudgeBar[idx])
                {
                    float a = 1.f - age / 0.16f;
                    float x0 = lane0 + laneW * lane + (laneW - noteW) * 0.5f;
                    float hh = 26.f * u;
                    dl->AddImage(TRef(s_texJudgeBar[idx]),
                                 ImVec2(x0, judgeY + 10.f * u),
                                 ImVec2(x0 + noteW, judgeY + 10.f * u + hh),
                                 ImVec2(0.f, 0.f), ImVec2(1.f, 1.f),
                                 IM_COL32(255, 255, 255, (int)(a * 200.f)));
                }
            }
        }

        // ---- 键块 + 键条（notepress 渐变 / 底部实心段） ----
        for (int i = 0; i < cols; i++)
        {
            bool active = keyDown[i] || laneNear[i] > 0.55f;   // 按键反馈（常开）
            float lx = lane0 + laneW * i;
            float bx0 = lx + (laneW - noteW) * 0.5f;
            float bx1 = bx0 + noteW;
            dl->AddRectFilled(ImVec2(bx0, keyTop), ImVec2(bx1, keyTop + keyH),
                              IM_COL32(58, 47, 37, 240), 8.f * u, ImDrawFlags_RoundCornersTop);
            // 中间两键（白键）用 notepress2，其余用 notepress1；MSP 皮肤优先用自带按键光
            const int c0 = (cols - 1) / 2, c1 = cols / 2;
            void* tex = s_texPressL[i] ? s_texPressL[i] : ((i >= c0 && i <= c1) ? s_texPress2 : s_texPress);
            bool twoSeg = s_texPressL[i] ? s_pressBuiltin[i] : true;
            if (tex && !twoSeg)
            {
                dl->AddImage(TRef(tex), ImVec2(bx0, keyTop), ImVec2(bx1, keyBarTop + keyBarH),
                             ImVec2(0.f, 0.f), ImVec2(1.f, 1.f),
                             IM_COL32(255, 255, 255, active ? 225 : 92));
            }
            else if (tex)
            {
                // 上半：notepress 渐变（按下时点亮）
                dl->AddImage(TRef(tex), ImVec2(bx0, keyTop), ImVec2(bx1, keyTop + keyH),
                             ImVec2(0.f, 0.f), ImVec2(1.f, 1140.f / 1250.f),
                             IM_COL32(255, 255, 255, active ? 205 : 76));
                // 下半：底部实心键条
                dl->AddImage(TRef(tex), ImVec2(bx0, keyBarTop), ImVec2(bx1, keyBarTop + keyBarH),
                             ImVec2(0.f, 1140.f / 1250.f), ImVec2(1.f, 1.f),
                             active ? IM_COL32(255, 255, 255, 255) : IM_COL32(236, 220, 205, 255));
            }
            else
            {
                ImU32 barCol = active ? IM_COL32(255, 226, 198, 255) : IM_COL32(226, 190, 145, 245);
                dl->AddRectFilled(ImVec2(bx0, keyBarTop), ImVec2(bx1, keyBarTop + keyBarH),
                                  barCol, 4.f * u);
            }
            // 键位字母（S D F J K L / D F J K），Malody 键位提示风格
            {
                char kb[8];
                KeyName(vkp[i], kb, sizeof(kb));
                ImGui::PushFont(nullptr, 15.f * u);
                ImVec2 ks = ImGui::CalcTextSize(kb);
                DrawTextColored(dl, ImVec2((bx0 + bx1) * 0.5f - ks.x * 0.5f,
                                           keyBarTop + keyBarH - ks.y - 1.f * u),
                                active ? IM_COL32(70, 52, 34, 255) : IM_COL32(150, 116, 82, 235), kb);
                ImGui::PopFont();
            }
        }

        // ---- 打击特效（hits-1..9 九帧爆发动画，约 0.27s） ----
        if (hitFx && inPlay)
        {
            const float kFxDur = 0.27f;
            const int kFxN = (s_hitFxN > 1) ? s_hitFxN : 1;
            const float kFxStep = kFxDur / (float)kFxN;
            for (size_t i = 0; i < s_fx.size();)
            {
                HitFx& fx = s_fx[i];
                float age = (float)(nowT - fx.t0);
                if (age > kFxDur)
                {
                    s_fx.erase(s_fx.begin() + (long)i);
                    continue;
                }
                if (fx.lane >= 0 && fx.lane < cols)
                {
                    float px = lane0 + laneW * fx.lane + laneW * 0.5f;
                    if (fx.kind == 3)
                    {
                        // 漏键：判定线处一圈暗红提示
                        float k = age / kFxDur;
                        float hw = noteW * 0.5f, hh = 20.f * u * (0.6f + 0.4f * k);
                        dl->AddRectFilled(ImVec2(px - hw, judgeY - hh * 0.5f),
                                          ImVec2(px + hw, judgeY + hh * 0.5f),
                                          IM_COL32(190, 60, 70, (int)((1.f - k) * 120.f)), 3.f * u);
                    }
                    else if (s_texHits[0])
                    {
                        int frame = (int)(age / kFxStep);
                        if (frame < 0) frame = 0;
                        if (frame > kFxN - 1) frame = kFxN - 1;
                        void* tex = s_texHits[frame];
                        if (tex)
                        {
                            float size = 208.f * u * fx.scale * (0.90f + 0.24f * (float)frame / (float)(kFxN > 1 ? kFxN - 1 : 1));
                            dl->AddImage(TRef(tex), ImVec2(px - size * 0.5f, judgeY - size * 0.5f),
                                         ImVec2(px + size * 0.5f, judgeY + size * 0.5f),
                                         ImVec2(0.f, 0.f), ImVec2(1.f, 1.f));
                        }
                    }
                }
                i++;
            }
        }

        // ---- 中央：连击 + 精准度（4K 自算成绩） ----
        {
            int combo = s_jdCombo.load(std::memory_order_relaxed);
            int total = s_jdTotal.load(std::memory_order_relaxed);
            double weight = s_jdWeight.load(std::memory_order_relaxed);
            double acc = (total > 0) ? weight / total : 1.0;
            if (acc < 0.0) acc = 0.0;
            if (acc > 1.0) acc = 1.0;
            if (combo >= 2 && s_texCombo[0])
            {
                char cb[16];
                snprintf(cb, sizeof(cb), "%d", combo);
                if (s_digitFullCombo)
                    DrawDigitsFullCenter(dl, s_texCombo, ImVec2(cx, 298.f * u), cb, 45.f * u,
                                         IM_COL32(255, 255, 255, 240), s_digitAspectCombo);
                else
                    DrawDigitsCenter(dl, s_texCombo, ImVec2(cx, 298.f * u), cb, 45.f * u,
                                     IM_COL32(255, 255, 255, 240));
            }
            int accI = (int)(acc * 10000.0 + 0.5);
            char accBuf[24];
            snprintf(accBuf, sizeof(accBuf), "%d.%02d%%", accI / 100, accI % 100);
            if (s_texAcc[0])
            {
                if (s_digitFullAcc)
                    DrawDigitsFullCenter(dl, s_texAcc, ImVec2(cx, 369.f * u), accBuf, 30.f * u,
                                         IM_COL32(255, 255, 255, 245), s_digitAspectAcc);
                else
                    DrawDigitsCenter(dl, s_texAcc, ImVec2(cx, 369.f * u), accBuf, 30.f * u,
                                     IM_COL32(255, 255, 255, 245));
            }
        }

        // ---- 中央判定提示（Malody judge 模块：m5judgerrd* 五色变体 + 文字）----
        //   0=MARVELOUS(绿) 1=PERFECT(黄) 2=GOOD(橙) 3=MISS(红)
        //   动画仿 rrdv52.lua：命中瞬间 105% → 73.5% 收缩 + 渐隐
        if (inPlay && s_judgePop.load(std::memory_order_relaxed) &&
            (s_texJudgePop[0] || s_judgeAnimN[0] > 0 || s_judgeAnimN[2] > 0 || s_judgeAnimN[3] > 0))
        {
            int kind = s_jdLastKind.load(std::memory_order_relaxed);
            double t0 = s_jdLastTime.load(std::memory_order_relaxed);
            double age = clock - t0;
            const double kPopDur = 0.55;
            if (kind >= 0 && kind <= 3 && age >= 0.0 && age < kPopDur)
            {
                const int imgIdx[4] = { 0, 1, 2, 4 };   // a=绿 b=黄 c=橙 e=红（d 备用）
                int animK = (kind <= 1) ? 0 : (kind == 2 ? 2 : 3);   // Best / Cool? / Good / Miss
                void* tex = nullptr;
                bool animTex = false;
                if (s_judgeAnimN[animK] > 0)
                {
                    int fr = (int)(age * (double)s_judgeAnimFps[animK]);
                    fr %= s_judgeAnimN[animK];
                    if (fr < 0) fr = 0;
                    tex = s_judgeAnim[animK][fr];
                    animTex = tex != nullptr;
                }
                if (!tex) tex = s_texJudgePop[imgIdx[kind]];
                float k = (float)(age / kPopDur);
                float ease = 1.f - (1.f - k) * (1.f - k);
                float scale = 1.05f + (0.735f - 1.05f) * ease;      // 105% → 73.5%
                float a = 1.f - k * k;
                if (a < 0.f) a = 0.f;
                int alpha = (int)(255.f * a);
                float w = (animTex ? 430.f : 474.f) * u * scale * 0.50f;
                float h = (animTex ? 215.f : 512.f) * u * scale * 0.50f;
                float pcx = cx, pcy = 566.f * u;
                if (tex)
                    dl->AddImage(TRef(tex), ImVec2(pcx - w * 0.5f, pcy - h * 0.5f),
                                 ImVec2(pcx + w * 0.5f, pcy + h * 0.5f),
                                 ImVec2(0.f, 0.f), ImVec2(1.f, 1.f),
                                 IM_COL32(255, 255, 255, alpha));
                const char* label = (kind == 0) ? "MARVELOUS" : (kind == 1) ? "PERFECT"
                                    : (kind == 2) ? "GOOD" : "MISS";
                ImU32 lc = (kind == 0) ? IM_COL32(126, 255, 158, alpha)
                          : (kind == 1) ? IM_COL32(255, 226, 120, alpha)
                          : (kind == 2) ? IM_COL32(255, 172, 92, alpha)
                                        : IM_COL32(255, 92, 92, alpha);
                ImGui::PushFont(nullptr, 30.f * u);
                ImVec2 ts = ImGui::CalcTextSize(label);
                DrawTextColored(dl, ImVec2(pcx - ts.x * 0.5f, pcy + h * 0.5f + 2.f * u), lc, label);
                if (kind < 3)
                {
                    double dtMs = s_jdLastOff.load(std::memory_order_relaxed) * 1000.0;
                    if (fabs(dtMs) >= 6.0)
                    {
                        char hb[48];
                        snprintf(hb, sizeof(hb), "%s %dms",
                                 I18N::Tr(dtMs > 0 ? I18N::LBL_FAST : I18N::LBL_SLOW),
                                 (int)(fabs(dtMs) + 0.5));
                        ImGui::PushFont(nullptr, 17.f * u);
                        ImVec2 hs = ImGui::CalcTextSize(hb);
                        DrawTextColored(dl, ImVec2(pcx - hs.x * 0.5f, pcy + h * 0.5f + 34.f * u),
                                        IM_COL32(255, 255, 255, (int)(190.f * a)), hb);
                        ImGui::PopFont();
                    }
                }
                ImGui::PopFont();
            }
        }

        // ---- 左上：圆环 + 时间条 + 曲目信息 ----
        {
            const float rgx = 100.f * u, rgy = 91.f * u;
            const float rOut = 97.f * u;
            // 进度弧（缺口居中在正上方）
            double prog = (double)st.percentComplete;
            if (prog < 0.0) prog = 0.0;
            if (prog > 1.0) prog = 1.0;
            float sweep = (float)(prog * 6.2831853);
            float a0 = -1.5707963f + (6.2831853f - sweep) * 0.5f;
            if (sweep > 0.01f)
            {
                dl->PathClear();
                dl->PathArcTo(ImVec2(rgx, rgy), rOut, a0, a0 + sweep, 72);
                dl->PathStroke(IM_COL32(228, 199, 187, 255), 0, 13.f * u);
            }
            if (s_texRing)
                dl->AddImage(TRef(s_texRing), ImVec2(rgx - 125.f * u, rgy - 125.f * u),
                             ImVec2(rgx + 125.f * u, rgy + 125.f * u));
            if (s_texRingOut)
                dl->AddImage(TRef(s_texRingOut), ImVec2(rgx - 100.f * u, rgy - 100.f * u),
                             ImVec2(rgx + 100.f * u, rgy + 100.f * u));
            if (s_texRingIn)
                dl->AddImage(TRef(s_texRingIn), ImVec2(rgx - 95.f * u, rgy - 95.f * u),
                             ImVec2(rgx + 95.f * u, rgy + 95.f * u));
            if (s_texLogo)
                dl->AddImage(TRef(s_texLogo), ImVec2(rgx - 48.f * u, rgy - 48.f * u),
                             ImVec2(rgx + 48.f * u, rgy + 48.f * u),
                             ImVec2(3.f / 172.f, 0.f), ImVec2(168.f / 172.f, 1.f));

            // 橘色时间条（右端旗形斜切）
            ImVec2 bar[5] = { ImVec2(218.f * u, 16.f * u), ImVec2(476.f * u, 16.f * u),
                              ImVec2(487.f * u, 23.5f * u), ImVec2(473.f * u, 61.f * u),
                              ImVec2(218.f * u, 61.f * u) };
            dl->AddConvexPolyFilled(bar, 5, IM_COL32(239, 183, 125, 255));
            if (s_texPnum)
                DrawClockAnton(dl, s_texPnumSet, ImVec2(229.f * u, 21.f * u), 42.f * u, songT);

            const char* name = (st.levelName[0] != 0) ? st.levelName : "ADOFAI";
            // 自定义 / 额外关卡的 levelName 实际是场景名（scnGame），换成可读的标题
            if (strncmp(name, "scn", 3) == 0)
                name = "\xe8\x87\xaa\xe5\xae\x9a\xe4\xb9\x89\xe5\x85\xb3\xe5\x8d\xa1";   // 自定义关卡
            ImGui::PushFont(nullptr, 26.f * u);
            DrawTextShadow(dl, ImVec2(220.f * u, 70.f * u), IM_COL32(235, 232, 228, 255), name);
            ImGui::PopFont();
            char line[128];
            snprintf(line, sizeof(line), "BPM:%.2f", s_bpm.load(std::memory_order_relaxed));
            ImGui::PushFont(nullptr, 19.f * u);
            DrawTextShadow(dl, ImVec2(220.f * u, 104.f * u), IM_COL32(234, 192, 129, 255), line);
            char keyLabel[10];
            int kln = 0;
            for (int i = 0; i < cols && kln < 8; i++)
                keyLabel[kln++] = (char)vkp[i];
            keyLabel[kln] = 0;
            snprintf(line, sizeof(line), "%dK ADOFAI lv.%d  [%s]",
                     cols, s_level.load(std::memory_order_relaxed), keyLabel);
            DrawTextShadow(dl, ImVec2(220.f * u, 130.f * u), IM_COL32(224, 196, 176, 235), line);
            ImGui::PopFont();
        }

        // ---- 右上：玩家条 + 彩虹条 + 5 数字 + 头像 ----
        {
            const float barX0 = gw - 455.f * u, barX1 = gw - 104.f * u;
            dl->AddRectFilled(ImVec2(barX0, 26.f * u), ImVec2(barX1, 59.f * u),
                              IM_COL32(255, 255, 255, 86), 16.f * u);
            ImGui::PushFont(nullptr, 20.f * u);
            const char* pname = "\xc2\xb7" "ADOFAI PERFECT" "\xc2\xb7";
            ImVec2 psz = ImGui::CalcTextSize(pname);
            DrawTextColored(dl, ImVec2((barX0 + barX1) * 0.5f - psz.x * 0.5f, 31.f * u),
                            IM_COL32(70, 66, 64, 235), pname);
            ImGui::PopFont();

            // 彩虹条（judgercolor 的饱和段）
            float rbX0 = gw - 390.5f * u, rbX1 = gw - 104.f * u;
            if (s_texJudge)
                dl->AddImage(TRef(s_texJudge), ImVec2(rbX0, 59.f * u), ImVec2(rbX1, 77.f * u),
                             ImVec2(0.f, 6.f / 92.f), ImVec2(1.f, 40.f / 92.f));
            // 数字：每轨 KPS + 合计（anton 白色字体）
            if (s_texPnumSet[0])
            {
                int cnt = cols + 1;
                float span = (rbX1 - rbX0) - 20.f * u;
                for (int i = 0; i < cnt; i++)
                {
                    char nb[16];
                    snprintf(nb, sizeof(nb), "%d", s_kps[i]);
                    DrawDigitsAntonCenter(dl, s_texPnumSet,
                                          ImVec2(rbX0 + 10.f * u + span * ((float)i + 0.5f) / (float)cnt,
                                                 68.f * u),
                                          nb, 22.f * u, IM_COL32(255, 255, 255, 245));
                }
            }
            // 头像
            if (s_texAvatar)
                dl->AddImage(TRef(s_texAvatar), ImVec2(gw - 98.f * u, 15.f * u),
                             ImVec2(gw - 23.f * u, 90.f * u));
        }

        // ---- 右上角小人：常驻完整小人 + 打击时叠加变化的一部分 ----
        {
            int pose = -1;
            for (int i = 0; i < cols; i++)
                if (s_poseUntil[i] > nowT)
                    pose = i & 3;                 // 皮肤只有 4 张打击姿势，6K 取模复用
            if (pose < 0)
            {
                int lk = s_jdLastKind.load(std::memory_order_relaxed);
                double lt = s_jdLastTime.load(std::memory_order_relaxed);
                int ll = s_jdLastLane.load(std::memory_order_relaxed);
                if (lk >= 0 && lk < 3 && ll >= 0 && ll < cols && (clock - lt) < 0.16)
                    pose = ll & 3;
            }
            if (s_texCharIdle)
            {
                float s = 345.f * u;
                ImVec2 p0(gw - s, 165.f * u), p1(gw, 165.f * u + s);
                // 底图：完整小人常驻显示
                dl->AddImage(TRef(s_texCharIdle), p0, p1);
                // 叠加：rurudokey-1..4 只是"右半/左半 + 头部"的替换图，
                // 必须画在底图之上，绝不能替换整张，否则另一半会消失
                if (pose >= 0 && s_texCharKey[pose])
                    dl->AddImage(TRef(s_texCharKey[pose]), p0, p1);
            }
        }

        // ---- 右下角：托腮小人 ----
        if (s_texCheek)
            dl->AddImage(TRef(s_texCheek), ImVec2(gw - 279.f * u, 781.f * u),
                         ImVec2(gw, 781.f * u + 301.f * u));
        }   // if (!msp)

        // ---- 左下角：冰与火之舞画面缩略图（"小窗"，不含本工具 UI）----
        //   位置/大小可在设置页调整（1080p 基准，随分辨率等比缩放）
        {
            // 只在关卡进行中显示（4K 谱面盖住主画面时才有意义；
            // 菜单/编辑器里主画面未被遮挡，显示小窗只会重复一层 UI）
            bool mini = s_miniOn.load(std::memory_order_relaxed) && inPlay;
            RenderHook_SetCaptureWanted(mini);
            int mw = 0, mh = 0;
            void* cap = mini ? RenderHook_GetCaptureTex(&mw, &mh) : nullptr;
            if (cap && mw > 0 && mh > 0)
            {
                float mx = (float)s_miniX.load(std::memory_order_relaxed) * u;
                float my = (float)s_miniY.load(std::memory_order_relaxed) * u;
                float sw = (float)s_miniW.load(std::memory_order_relaxed) * u;
                float sh = (float)s_miniH.load(std::memory_order_relaxed) * u;
                if (sw < 60.f * u) sw = 60.f * u;
                if (sh < 34.f * u) sh = 34.f * u;
                ImVec2 p0(mx, my), p1(mx + sw, my + sh);
                dl->AddRectFilled(ImVec2(p0.x - 3.f * u, p0.y - 3.f * u),
                                  ImVec2(p1.x + 3.f * u, p1.y + 3.f * u),
                                  IM_COL32(0, 0, 0, 165), 7.f * u);
                dl->AddImage(TRef(cap), p0, p1, ImVec2(0.f, 0.f), ImVec2(1.f, 1.f),
                             IM_COL32(255, 255, 255, 255));
                dl->AddRect(p0, p1, IM_COL32(255, 219, 168, 230), 5.f * u, 0, 2.f * u);
            }
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }
    // ---------------- 键位自定义（4K / 6K 共用；确认后才写入生效） ----------------
    static const int kCapVKs[] = {
        'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
        '0','1','2','3','4','5','6','7','8','9',
        VK_SPACE, VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN, VK_TAB, VK_RETURN, VK_BACK, VK_DELETE,
        0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0, 0xDB, 0xDC, 0xDD, 0xDE,
    };
    static int s_capLocked[64];
    static int s_capLockedN = 0;
    // 设置页标签列（按当前语言动态计算，避免俄语/英语长标签压住控件）
    static float g_lblColX = 90.f;
    float LabelCol(std::initializer_list<const char*> labels, float minW)
    {
        float w = minW;
        for (const char* t : labels)
        {
            float x = ImGui::CalcTextSize(t).x + 12.f;
            if (x > w) w = x;
        }
        return w;
    }

    static void KeyName(int vk, char* out, size_t n)
    {
        if (vk >= 'A' && vk <= 'Z') { snprintf(out, n, "%c", (char)vk); return; }
        if (vk >= '0' && vk <= '9') { snprintf(out, n, "%c", (char)vk); return; }
        switch (vk)
        {
        case VK_SPACE:  snprintf(out, n, "SP"); break;
        case VK_LEFT:   snprintf(out, n, "<");  break;
        case VK_UP:     snprintf(out, n, "^");  break;
        case VK_DOWN:   snprintf(out, n, "v");  break;
        case VK_RIGHT:  snprintf(out, n, ">");  break;
        case VK_TAB:    snprintf(out, n, "TB"); break;
        case VK_RETURN: snprintf(out, n, "EN"); break;
        case VK_BACK:   snprintf(out, n, "BS"); break;
        case VK_DELETE: snprintf(out, n, "DE"); break;
        default:        snprintf(out, n, "%c", (vk >= 32 && vk < 127) ? (char)vk : '?'); break;
        }
    }

    // \u8fdb\u5165\u6355\u83b7\u65f6\u5148\u8bb0\u4e0b\u5f53\u524d\u5df2\u6309\u4f4f\u7684\u952e\uff0c\u907f\u514d\u628a\u201c\u6b63\u5728\u6309\u7740\u7684\u65e7\u952e\u201d\u5f53\u6210\u65b0\u952e
    static void LockCaptureKeys()
    {
        s_capLockedN = 0;
        for (int k : kCapVKs)
            if ((GetAsyncKeyState(k) & 0x8000) && s_capLockedN < 64)
                s_capLocked[s_capLockedN++] = k;
    }
    static bool CaptureKeyLocked(int vk)
    {
        for (int i = 0; i < s_capLockedN; i++)
            if (s_capLocked[i] == vk) return true;
        return false;
    }
    static int PollNewKey()
    {
        for (int k : kCapVKs)
            if ((GetAsyncKeyState(k) & 0x8000) && !CaptureKeyLocked(k))
                return k;
        return 0;
    }

    void GetModeKeys(int mi, int* out, int* n)
    {
        if (mi < 0) mi = 0;
        if (mi >= kModeN) mi = kModeN - 1;
        const int c = kLanesOf[mi];
        for (int i = 0; i < c; i++)
            out[i] = s_vk[mi][i].load(std::memory_order_relaxed);
        *n = c;
    }

    void DrawKeyBinder(const char* pid, int modeIndex)
    {
        char lblEdit[80], lblDef[80], lblOk[80], lblCancel[80];
        snprintf(lblEdit, sizeof(lblEdit), "%s##edit", I18N::Tr(I18N::LBL_EDIT));
        snprintf(lblDef, sizeof(lblDef), "%s##def", I18N::Tr(I18N::LBL_RESTORE));
        snprintf(lblOk, sizeof(lblOk), "%s##ok", I18N::Tr(I18N::LBL_OK));
        snprintf(lblCancel, sizeof(lblCancel), "%s##cancel", I18N::Tr(I18N::LBL_CANCEL));
        const int n = kLanesOf[modeIndex];
        std::atomic<int>* live = s_vk[modeIndex];
        const int* defs = kVKDefs[modeIndex];
        int* draft = s_draft[modeIndex];
        bool& editing = s_keyEdit[modeIndex];
        int& cap = s_keyCap[modeIndex];
        bool& inited = s_keyInit[modeIndex];
        if (!inited)
        {
            for (int i = 0; i < n; i++)
                draft[i] = live[i].load(std::memory_order_relaxed);
            inited = true;
        }

        ImGui::PushID(pid);
        if (!editing)
        {
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_KEYS));            // \u952e\u4f4d
            ImGui::SameLine();
            ImGui::SetCursorPosX(g_lblColX);
            if (ImGui::Button(lblEdit, ImVec2(56, 24)))       // \u4fee\u6539
            {
                for (int i = 0; i < n; i++)
                    draft[i] = live[i].load(std::memory_order_relaxed);
                editing = true;
                cap = -1;
            }
            ImGui::SameLine();
            if (ImGui::Button(lblDef, ImVec2(80, 24)))  // \u6062\u590d\u9ed8\u8ba4
            {
                for (int i = 0; i < n; i++)
                {
                    live[i].store(defs[i], std::memory_order_relaxed);
                    draft[i] = defs[i];
                }
            }
            ImGui::SameLine();
            char cur[16];
            int c = 0;
            for (int i = 0; i < n && c < 12; i++)
                cur[c++] = (char)live[i].load(std::memory_order_relaxed);
            cur[c] = 0;
            ImGui::TextDisabled("%s", cur);
        }
        else
        {
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_KEYS));
            ImGui::SameLine();
            ImGui::SetCursorPosX(g_lblColX);
            for (int i = 0; i < n; i++)
            {
                char nm[8];
                KeyName(draft[i], nm, sizeof(nm));
                char lbl[24];
                if (cap == i) snprintf(lbl, sizeof(lbl), "[%s]##k%d", nm, i);
                else          snprintf(lbl, sizeof(lbl), "%s##k%d", nm, i);
                if (cap == i)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.26f, 0.59f, 0.98f, 0.85f));
                if (ImGui::Button(lbl, ImVec2(40, 24)))
                {
                    cap = (cap == i) ? -1 : i;
                    if (cap == i) LockCaptureKeys();
                }
                if (cap == i)
                    ImGui::PopStyleColor();
                if (i + 1 < n) ImGui::SameLine();
            }

            if (cap >= 0)
            {
                if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
                    cap = -1;
                else
                {
                    int got = PollNewKey();
                    if (got)
                    {
                        bool dup = false;
                        for (int j = 0; j < n; j++)
                            if (j != cap && draft[j] == got) dup = true;
                        if (!dup)
                        {
                            draft[cap] = got;
                            cap = -1;
                        }
                    }
                }
            }

            bool dupAny = false;
            for (int i = 0; i < n && !dupAny; i++)
                for (int j = i + 1; j < n; j++)
                    if (draft[i] == draft[j]) dupAny = true;

            ImGui::SetCursorPosX(g_lblColX);
            if (dupAny)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.20f, 0.55f));
            if (ImGui::Button(lblOk, ImVec2(70, 24)) && !dupAny)   // \u786e\u5b9a
            {
                for (int i = 0; i < n; i++)
                    live[i].store(draft[i], std::memory_order_relaxed);
                editing = false;
                cap = -1;
                char cur[16];
                int c = 0;
                for (int i = 0; i < n && c < 12; i++)
                    cur[c++] = (char)draft[i];
                cur[c] = 0;
                Log::Printf("[%dK] key bind -> %s", n, cur);
            }
            if (dupAny)
                ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button(lblCancel, ImVec2(70, 24)))          // \u53d6\u6d88
            {
                editing = false;
                cap = -1;
            }
            ImGui::SameLine();
            if (dupAny)
                ImGui::TextDisabled(I18N::Tr(I18N::LBL_KEYDUP));   // \u952e\u4f4d\u91cd\u590d
            else if (cap >= 0)
                ImGui::TextDisabled(I18N::Tr(I18N::LBL_PRESSNEW));
            else
                ImGui::TextDisabled(I18N::Tr(I18N::LBL_PRESSKEY));  // \u70b9\u69fd\u4f4d\u540e\u6309\u65b0\u952e
        }
        ImGui::PopID();
    }

    // ---------------- 主窗口设置页 ----------------
    void BeginCard4K(const char* id, float height)
    {
        (void)height;   // 兼容旧调用点：卡片高度改为随内容自适应，滚动统一交给页面外层
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.f, 1.f, 1.f, 0.045f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
        ImGui::BeginChild(id, ImVec2(0.f, 0.f), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_None);
    }
    void EndCard4K()
    {
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    // 标题行 + 右侧开关；返回开关是否被点击。
    // 调用后光标停在标题行下方（描述/后续控件从这里开始），避免文字与开关重叠。
    bool TitleToggleRow(const char* id, float titleSize, const char* title, bool* v)
    {
        const float x0 = ImGui::GetCursorPosX();
        const float y0 = ImGui::GetCursorPosY();
        const float avail = ImGui::GetContentRegionAvail().x;
        ImGui::PushFont(nullptr, titleSize);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        ImGui::SetCursorPosX(x0 + avail - 62.f);
        ImGui::SetCursorPosY(y0 - 1.f);
        const bool clicked = MiniToggle(id, v);
        ImGui::SetCursorPos(ImVec2(x0, y0 + titleSize + 10.f));
        return clicked;
    }

    bool MiniToggle(const char* id, bool* v)
    {
        float w = 52.f, h = 26.f;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
        if (clicked)
            *v = !*v;
        ImU32 bg = *v ? IM_COL32(46, 190, 160, 235) : IM_COL32(0, 0, 0, 110);
        if (ImGui::IsItemHovered())
            bg = *v ? IM_COL32(60, 220, 185, 255) : IM_COL32(255, 255, 255, 40);
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, h * 0.5f);
        float kx = p.x + 4.f + (*v ? 1.f : 0.f) * (w - 8.f - (h - 8.f));
        dl->AddCircleFilled(ImVec2(kx + (h - 8.f) * 0.5f, p.y + h * 0.5f), (h - 8.f) * 0.5f,
                            IM_COL32(250, 250, 252, 255));
        return clicked;
    }

    // ---------------- 皮肤管理（皮肤页） ----------------
    const char* SkinDefaultRoot()
    {
        static char root[MAX_PATH * 2] = { 0 };
        if (!root[0])
        {
            char modPath[MAX_PATH * 2] = { 0 };
            HMODULE hm = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)&SkinDefaultRoot, &hm) && hm)
                GetModuleFileNameA(hm, modPath, sizeof(modPath));
            char* slash = strrchr(modPath, '\\');
            if (slash) *slash = 0;
            snprintf(root, sizeof(root), "%s\\skin", modPath);
        }
        return root;
    }

    static std::vector<std::string> s_skinList;   // 可用皮肤目录（含 rurudokey.png）
    static bool DirHasSkinPub(const char* d) { return DirHasSkin(d); }

    static std::vector<std::string> s_skinListTitle;
    static std::vector<std::string> s_skinListCreator;

    void SkinRescan()
    {
        s_skinList.clear();
        s_skinListTitle.clear();
        s_skinListCreator.clear();
        auto add = [](const std::string& d) {
            if (!DirHasSkin(d.c_str())) return;
            for (auto& e : s_skinList)
                if (_stricmp(e.c_str(), d.c_str()) == 0) return;
            char title[128] = { 0 }, creator[128] = { 0 };
            int modeId = -1;
            SkinMsp::ReadMeta(d.c_str(), title, sizeof(title), creator, sizeof(creator), &modeId);
            std::string tn = title[0] ? title : "";
            std::string cn = creator[0] ? creator : "";
            if (tn.empty())
            {
                const char* pp = d.c_str();
                const char* bb = strrchr(pp, '\\');
                tn = bb ? bb + 1 : pp;
            }
            s_skinList.push_back(d);
            s_skinListTitle.push_back(tn);
            s_skinListCreator.push_back(cn);
        };
        // 1) 运行目录 skin\(子目录) 与 skin 本身
        std::string root = SkinDefaultRoot();
        add(root);
        {
            std::string pat = root + "\\*";
            WIN32_FIND_DATAA fd{};
            HANDLE h = FindFirstFileA(pat.c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
                    add(root + "\\" + fd.cFileName);
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
        }
        // 2) 游戏目录 skin\(子目录)（用户选择/进程目录）
        {
            char gd[MAX_PATH * 2] = { 0 };
            I18N::Prefs::GetStr("game_dir", gd, sizeof(gd), "");
            std::string g = gd[0] ? gd : "";
            if (g.empty())
            {
                char exe[MAX_PATH * 2] = { 0 };
                GetModuleFileNameA(nullptr, exe, sizeof(exe));
                g = exe;
                size_t k = g.find_last_of("\\/");
                g = (k == std::string::npos) ? "" : g.substr(0, k);
            }
            if (!g.empty())
            {
                std::string sroot = g + "\\skin";
                add(sroot);
                std::string pat = sroot + "\\*";
                WIN32_FIND_DATAA fd{};
                HANDLE h = FindFirstFileA(pat.c_str(), &fd);
                if (h != INVALID_HANDLE_VALUE)
                {
                    do
                    {
                        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
                        add(sroot + "\\" + fd.cFileName);
                    } while (FindNextFileA(h, &fd));
                    FindClose(h);
                }
            }
        }
        // 3) 当前生效目录（环境变量/自定义）
        add(SkinDir());
        Log::Printf("[skin] %d skin(s) found", (int)s_skinList.size());
    }

    int SkinCount()
    {
        if (s_skinList.empty()) SkinRescan();
        return (int)s_skinList.size();
    }
    const char* SkinPathAt(int i)
    {
        if (i < 0 || i >= (int)s_skinList.size()) return "";
        return s_skinList[i].c_str();
    }
    const char* SkinName(int i)
    {
        if (i < 0 || i >= (int)s_skinList.size()) return "";
        const char* p = s_skinList[i].c_str();
        const char* base = strrchr(p, '\\');
        return base ? base + 1 : p;
    }
    const char* SkinTitle(int i)
    {
        if (i < 0 || i >= (int)s_skinListTitle.size()) return "";
        return s_skinListTitle[i].c_str();
    }
    const char* SkinCreator(int i)
    {
        if (i < 0 || i >= (int)s_skinListCreator.size()) return "";
        return s_skinListCreator[i].c_str();
    }
    const char* SkinActive()
    {
        const char* d = SkinDir();
        return d ? d : "";
    }
    bool SkinSetActiveFull(const char* dir)
    {
        // 自动化截图/回归测试锁定：ADOFAI_PERFECT_SKIN_LOCK=1 时忽略皮肤切换
        // （避免其他窗口的点击落到皮肤列表按钮上导致测试中途换皮）
        {
            static const char* lk = getenv("ADOFAI_PERFECT_SKIN_LOCK");
            if (lk && lk[0] == '1') return false;
        }
        if (!dir || !dir[0] || !DirHasSkin(dir))
            return false;
        snprintf(s_skinDir, sizeof(s_skinDir), "%s", dir);
        I18N::Prefs::SetStr("skin_dir", dir);
        I18N::Prefs::SetStr("skin_mode", "msp");    // 选外部皮肤目录 = 外部 MSP 模式
        I18N::Prefs::SetInt("skin_mode_v2", 1);
        s_skinBuiltin.store(false, std::memory_order_relaxed);
        I18N::Prefs::Save();
        s_skinReload = true;
        s_skinLoaded = false;   // LoadSkin() 仅在未就绪时被调用，这里必须置位
        Log::Printf("[skin] active -> %s", dir);
        return true;
    }
    bool SkinBuiltinMode()
    {
        return s_skinBuiltin.load(std::memory_order_relaxed);
    }
    void SkinSetBuiltinMode(bool on)
    {
        if (s_skinBuiltin.load(std::memory_order_relaxed) == on)
            return;
        s_skinBuiltin.store(on, std::memory_order_relaxed);
        I18N::Prefs::SetStr("skin_mode", on ? "builtin" : "msp");
        I18N::Prefs::SetInt("skin_mode_v2", 1);
        I18N::Prefs::Save();
        s_skinReload = true;
        s_skinLoaded = false;
        Log::Printf("[skin] mode -> %s", on ? "builtin" : "msp");
    }
    void SkinOpenFolder()
    {
        std::string root = SkinDefaultRoot();
        CreateDirectoryA(root.c_str(), nullptr);
        ShellExecuteA(nullptr, "open", root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    bool SkinImportMsp(const wchar_t* mspPath, char* msg, int n)
    {
        char out[MAX_PATH * 2] = { 0 };
        char err[160] = { 0 };
        if (!SkinMsp::ImportArchive(mspPath, out, sizeof(out), err, sizeof(err)))
        {
            if (msg && n) snprintf(msg, (size_t)n, "%s", err[0] ? err : "failed");
            return false;
        }
        SkinRescan();
        const char* base = strrchr(out, '\\');
        base = base ? base + 1 : out;
        if (msg && n) snprintf(msg, (size_t)n, "%s", base);
        Log::Printf("[skin] imported -> %s", out);
        return true;
    }

    bool SkinImportMspDialog(char* msg, int n)
    {
        wchar_t file[MAX_PATH * 2] = { 0 };
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = GetActiveWindow();
        ofn.lpstrFilter = L"Malody Skin (*.msp)\0*.msp\0All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH * 2;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        ofn.lpstrTitle = L"Import Malody skin (.msp)";
        if (!GetOpenFileNameW(&ofn)) return false;
        return SkinImportMsp(file, msg, n);
    }

    bool HasChart()
    {
        return s_noteCount.load(std::memory_order_relaxed) > 0;
    }

    int NoteCount()
    {
        return s_noteCount.load(std::memory_order_relaxed);
    }

    void GetDiag(char* buf, int n)
    {
        strncpy_s(buf, (size_t)n, s_diag, _TRUNCATE);
    }

    // ---------------- 配置档案访问器（设置页保存/加载） ----------------
    //   which: 0=en 1=speed 2=offset 3=style 4=judge 5=uphide 6=dnhide 7=autooff
    //          8=autoplay 9=macro
    int ModeLaneCount(int mi) { return (mi >= 0 && mi < kModeN) ? kLanesOf[mi] : 0; }
    int ModeSettingGet(int mi, int which)
    {
        if (mi < 0 || mi >= kModeN) return 0;
        switch (which)
        {
        case 0: return s_en[mi].load(std::memory_order_relaxed) ? 1 : 0;
        case 1: return s_speed[mi].load(std::memory_order_relaxed);
        case 2: return s_offsetMS[mi].load(std::memory_order_relaxed);
        case 3: return s_style[mi].load(std::memory_order_relaxed);
        case 4: return s_judgeSet[mi].load(std::memory_order_relaxed);
        case 5: return s_upHide[mi].load(std::memory_order_relaxed) ? 1 : 0;
        case 6: return s_dnHide[mi].load(std::memory_order_relaxed) ? 1 : 0;
        case 7: return s_autoOff[mi].load(std::memory_order_relaxed) ? 1 : 0;
        case 8: return s_autoPlay[mi].load(std::memory_order_relaxed) ? 1 : 0;
        case 9: return s_macroPlay[mi].load(std::memory_order_relaxed) ? 1 : 0;
        default: return 0;
        }
    }
    void ModeSettingSet(int mi, int which, int v)
    {
        if (mi < 0 || mi >= kModeN) return;
        switch (which)
        {
        case 0:
            if (v) { for (int i = 0; i < kModeN; i++) s_en[i].store(i == mi, std::memory_order_relaxed); }
            else     s_en[mi].store(false, std::memory_order_relaxed);
            break;
        case 1: s_speed[mi].store(v, std::memory_order_relaxed); break;
        case 2: s_offsetMS[mi].store(v, std::memory_order_relaxed); break;
        case 3: s_style[mi].store(v, std::memory_order_relaxed); break;
        case 4: s_judgeSet[mi].store(v, std::memory_order_relaxed); break;
        case 5: s_upHide[mi].store(v != 0, std::memory_order_relaxed); break;
        case 6: s_dnHide[mi].store(v != 0, std::memory_order_relaxed); break;
        case 7: s_autoOff[mi].store(v != 0, std::memory_order_relaxed); break;
        case 8: s_autoPlay[mi].store(v != 0, std::memory_order_relaxed); break;
        case 9: s_macroPlay[mi].store(v != 0, std::memory_order_relaxed); break;
        default: break;
        }
    }

    // ---------------- 宏 / 录制访问器（宏页 + 配置档案共用） ----------------
    static void RecCfgLoadOnce()
    {
        static bool done = false;
        if (done) return;
        done = true;
        char d[MAX_PATH * 2] = { 0 };
        I18N::Prefs::GetStr("rec_dir", d, sizeof(d), "");
        if (d[0]) snprintf(s_recDir, sizeof(s_recDir), "%s", d);
        else      snprintf(s_recDir, sizeof(s_recDir), "%srecords", I18N::Prefs::Dir());
        int fps = I18N::Prefs::GetInt("rec_fps", 60);
        int mbps = I18N::Prefs::GetInt("rec_mbps", 20);
        s_recFps.store((fps >= 15 && fps <= 240) ? fps : 60);
        s_recMbps.store((mbps >= 2 && mbps <= 200) ? mbps : 20);
        s_recAuto.store(I18N::Prefs::GetInt("rec_auto", 1) != 0);
        s_macroFire.store(I18N::Prefs::GetInt("macro_fire", 0) != 0);
    }
    static void RecCfgSave()
    {
        I18N::Prefs::SetStr("rec_dir", s_recDir);
        I18N::Prefs::SetInt("rec_fps", s_recFps.load(std::memory_order_relaxed));
        I18N::Prefs::SetInt("rec_mbps", s_recMbps.load(std::memory_order_relaxed));
        I18N::Prefs::SetInt("rec_auto", s_recAuto.load(std::memory_order_relaxed) ? 1 : 0);
        I18N::Prefs::SetInt("macro_fire", s_macroFire.load(std::memory_order_relaxed) ? 1 : 0);
        I18N::Prefs::Save();
    }
    int  MacroAccGet()   { return s_macroAcc.load(std::memory_order_relaxed); }
    void MacroAccSet(int v) { s_macroAcc.store(std::max(90, std::min(100, v)), std::memory_order_relaxed); }
    int  MacroHumanGet() { return s_macroHuman.load(std::memory_order_relaxed); }
    void MacroHumanSet(int v) { s_macroHuman.store(std::max(0, std::min(100, v)), std::memory_order_relaxed); }
    int  RecFpsGet()     { RecCfgLoadOnce(); return s_recFps.load(std::memory_order_relaxed); }
    void RecFpsSet(int v) { RecCfgLoadOnce(); s_recFps.store(std::max(15, std::min(240, v)), std::memory_order_relaxed); RecCfgSave(); }
    int  RecMbpsGet()    { RecCfgLoadOnce(); return s_recMbps.load(std::memory_order_relaxed); }
    void RecMbpsSet(int v) { RecCfgLoadOnce(); s_recMbps.store(std::max(2, std::min(200, v)), std::memory_order_relaxed); RecCfgSave(); }
    int  RecAutoGet()    { RecCfgLoadOnce(); return s_recAuto.load(std::memory_order_relaxed) ? 1 : 0; }
    void RecAutoSet(int v) { RecCfgLoadOnce(); s_recAuto.store(v != 0, std::memory_order_relaxed); RecCfgSave(); }
    const char* RecDirGet() { RecCfgLoadOnce(); return s_recDir; }
    void RecDirSet(const char* dir)
    {
        RecCfgLoadOnce();
        if (dir && dir[0]) snprintf(s_recDir, sizeof(s_recDir), "%s", dir);
        RecCfgSave();
    }
    void RecCfgApply()
    {
        RecCfgLoadOnce();
        const bool on = s_recOn.load(std::memory_order_relaxed);
        if (!on)
            s_recPaused.store(false, std::memory_order_relaxed);
        const bool paused = s_recPaused.load(std::memory_order_relaxed);
        RenderHook_SetRecordWanted(on && !paused);
        if (!on && GameRecorder::Active())
            GameRecorder::Stop();
        if (on && !paused && !s_recAuto.load(std::memory_order_relaxed) && !GameRecorder::Active())
            GameRecorder::Start(s_recDir, s_recFps.load(std::memory_order_relaxed),
                                s_recMbps.load(std::memory_order_relaxed));
    }
    int  RecOnGet() { return s_recOn.load(std::memory_order_relaxed) ? 1 : 0; }
    void RecOnSet(int v) { s_recOn.store(v != 0, std::memory_order_relaxed); RecCfgApply(); }
    int  RecPausedGet() { return s_recPaused.load(std::memory_order_relaxed) ? 1 : 0; }
    void RecPausedSet(int v)
    {
        RecCfgLoadOnce();
        const bool p = (v != 0);
        s_recPaused.store(p, std::memory_order_relaxed);
        if (p)
        {
            // 暂停 = 结束当前分段（写完 MP4 moov 尾部）；恢复时写新文件
            if (GameRecorder::Active())
            {
                GameRecorder::Stop();
                Log::Printf("[rec] paused - segment finalized");
            }
            RenderHook_SetRecordWanted(false);
        }
        else
        {
            RecCfgApply();
            if (s_recOn.load(std::memory_order_relaxed) && !GameRecorder::Active())
            {
                GameRecorder::Start(s_recDir, s_recFps.load(std::memory_order_relaxed),
                                    s_recMbps.load(std::memory_order_relaxed));
                Log::Printf("[rec] resumed - new segment");
            }
        }
    }

    // ---------------- 冰与火宏（原生关卡）访问器 ----------------
    bool FireMacroEnabled() { return s_macroFire.load(std::memory_order_relaxed); }
    void FireMacroSet(int v)
    {
        RecCfgLoadOnce();
        const bool on = (v != 0);
        s_macroFire.store(on, std::memory_order_relaxed);
        if (on)
        {
            // 参与模式互斥：冰与火宏开启时关闭 4K/5K/6K/10K 的宏打歌与自动打歌
            bool wasOther = false;
            for (int i = 0; i < kModeN; i++)
            {
                if (s_macroPlay[i].exchange(false, std::memory_order_relaxed)) wasOther = true;
                if (s_autoPlay[i].exchange(false, std::memory_order_relaxed))  wasOther = true;
            }
            if (wasOther) AutoPlayStop();
        }
        RecCfgSave();
        Log::Printf("[fire] macro mode %s", on ? "ON" : "OFF");
    }

    // 复用宏打歌的拟人抖动模型：返回本次目标的毫秒偏移（含 σ / 慢漂移 / 偶发手滑）
    double MacroTimingOffsetMs(double winMs) { return AutoHumanOffsetMs(winMs, true); }
    void RecStatsGet(int* in, int* written, int* dropped, const char** file)
    {
        int fi = 0, fo = 0, dr = 0;
        double sec = 0.0;
        GameRecorder::GetStats(&fi, &fo, &dr, &sec);
        if (in) *in = fi;
        if (written) *written = fo;
        if (dropped) *dropped = dr;
        if (file) *file = GameRecorder::CurrentFile();
    }
    int ModeKeyGet(int mi, int slot)
    {
        if (mi < 0 || mi >= kModeN || slot < 0 || slot >= 10) return 0;
        if (!s_keyInit[mi])
        {
            for (int i = 0; i < 10; i++)
            {
                s_vk[mi][i].store(kVKDefs[mi][i], std::memory_order_relaxed);
                s_draft[mi][i] = kVKDefs[mi][i];
            }
            s_keyInit[mi] = true;
        }
        return s_vk[mi][slot].load(std::memory_order_relaxed);
    }
    void ModeKeySet(int mi, int slot, int vk)
    {
        if (mi < 0 || mi >= kModeN || slot < 0 || slot >= 10) return;
        if (vk <= 0) return;
        if (!s_keyInit[mi])
        {
            for (int i = 0; i < 10; i++)
            {
                s_vk[mi][i].store(kVKDefs[mi][i], std::memory_order_relaxed);
                s_draft[mi][i] = kVKDefs[mi][i];
            }
            s_keyInit[mi] = true;
        }
        s_vk[mi][slot].store(vk, std::memory_order_relaxed);
        s_draft[mi][slot] = vk;
        s_keyEdit[mi] = false;
        s_keyCap[mi] = -1;
    }

    // ---------------- 主窗口设置页（4K / 5K / 6K / 10K 共用一套布局） ----------------
    void DrawModePage(int mi)
    {
        const int cols = kLanesOf[mi];
        char id[64];

        // 卡片 1：标题 + 开关（四模式互斥）
        snprintf(id, sizeof(id), "##cardm%d_main", mi);
        BeginCard4K(id, 92.f);
        {
            char title[48];
            snprintf(title, sizeof(title), "%dK 下坠谱面", cols);
            bool en = s_en[mi].load(std::memory_order_relaxed);
            snprintf(id, sizeof(id), "##m%d_en", mi);
            if (TitleToggleRow(id, 19.f, title, &en))
            {
                for (int i = 0; i < kModeN; i++)
                    s_en[i].store((i == mi) ? en : false, std::memory_order_relaxed);
                if (en)
                    Log::Printf("[UI] %dK assist enabled", cols);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 166, 182, 255));
            ImGui::TextWrapped("%s", ModeDesc(mi));
            ImGui::PopStyleColor();
        }
        EndCard4K();
        ImGui::Spacing();

        // 卡片 2：流速 / 延迟 / 转换风格 / 键位 / 底板 / 特效 / 统计
        snprintf(id, sizeof(id), "##cardm%d_set", mi);
        BeginCard4K(id, 356.f);          // 多出"上隐/下隐/自动调整延迟"一行后重排
        {
            const float colW = LabelCol({ I18N::Tr(I18N::LBL_SPEED), I18N::Tr(I18N::LBL_OFFSET),
                                          I18N::Tr(I18N::LBL_JUDGEW), I18N::Tr(I18N::LBL_STYLE),
                                          I18N::Tr(I18N::LBL_BOARD), I18N::Tr(I18N::LBL_KEYS) });
            g_lblColX = colW;
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 6.f));

            int spd = s_speed[mi].load(std::memory_order_relaxed);
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_SPEED));
            ImGui::SameLine();
            ImGui::SetCursorPosX(colW);
            ImGui::SetNextItemWidth(-14.f);
            snprintf(id, sizeof(id), "##m%dspeed", mi);
            if (ImGui::SliderInt(id, &spd, 1, 12))
                s_speed[mi].store(spd);

            // 延迟 + 一键校准（用最近 32 次命中的平均偏差把判定中心拉回 0）
            int off = s_offsetMS[mi].load(std::memory_order_relaxed);
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_OFFSET));
            ImGui::SameLine();
            ImGui::SetCursorPosX(colW);
            const float calW = 76.f;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - calW - 8.f);
            snprintf(id, sizeof(id), "##m%doff", mi);
            if (ImGui::SliderInt(id, &off, -400, 400, "%d ms"))
                s_offsetMS[mi].store(off);
            ImGui::SameLine();
            snprintf(id, sizeof(id), "%s##m%dcal", I18N::Tr(I18N::LBL_CALIB), mi);
            if (ImGui::Button(id, ImVec2(calW, 0)))
            {
                int bn = 0;
                double avg = BiasAvgMs(&bn);
                if (bn >= 4)
                {
                    int no = (int)lround((double)off + avg);
                    if (no < -400) no = -400;
                    if (no > 400)  no = 400;
                    s_offsetMS[mi].store(no);
                    BiasClear();
                    Log::Printf("[%dK] calibrated offset %d -> %d ms (avg %.1f)", cols, off, no, avg);
                }
            }

            // 判定宽度：严判 / 标准 / 宽判
            int js = s_judgeSet[mi].load(std::memory_order_relaxed);
            if (js < 0 || js > 2) js = 1;
            const char* jwNames[3] = { I18N::Tr(I18N::LBL_JW0), I18N::Tr(I18N::LBL_JW1), I18N::Tr(I18N::LBL_JW2) };
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_JUDGEW));
            ImGui::SameLine();
            ImGui::SetCursorPosX(colW);
            ImGui::SetNextItemWidth(-14.f);
            snprintf(id, sizeof(id), "##m%djw", mi);
            if (ImGui::BeginCombo(id, jwNames[js]))
            {
                for (int i = 0; i < 3; i++)
                {
                    bool sel = (i == js);
                    if (ImGui::Selectable(jwNames[i], sel) && !sel)
                    {
                        s_judgeSet[mi].store(i, std::memory_order_relaxed);
                        BiasClear();
                    }
                    if (sel)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            // 转换风格：经典 / 叠 / 技 / 乱 / 切 / 冰火手法（切换后下一帧自动重建谱面）
            int style = s_style[mi].load(std::memory_order_relaxed);
            if (style < 0 || style > 5) style = 0;
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_STYLE));
            ImGui::SameLine();
            ImGui::SetCursorPosX(colW);
            ImGui::SetNextItemWidth(-14.f);
            snprintf(id, sizeof(id), "##m%dstyle", mi);
            if (ImGui::BeginCombo(id, kStyleNames[style]))
            {
                for (int i = 0; i < 6; i++)
                {
                    bool sel = (i == style);
                    if (ImGui::Selectable(kStyleNames[i], sel) && i != style)
                    {
                        s_style[mi].store(i, std::memory_order_relaxed);
                        Log::Printf("[UI] %dK style -> %s", cols, kStyleNames[i]);
                    }
                    if (sel)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            int op = s_opacity.load(std::memory_order_relaxed);
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_BOARD));
            ImGui::SameLine();
            ImGui::SetCursorPosX(colW);
            ImGui::SetNextItemWidth(-14.f);
            snprintf(id, sizeof(id), "##m%dop", mi);
            if (ImGui::SliderInt(id, &op, 0, 255))
                s_opacity.store(op);

            ImGui::Spacing();
            snprintf(id, sizeof(id), "##m%dkb", mi);
            DrawKeyBinder(id, mi);
            ImGui::Spacing();

            bool hfx = s_hitFx.load(std::memory_order_relaxed);
            snprintf(id, sizeof(id), "##m%dfx", mi);
            if (MiniToggle(id, &hfx))
                s_hitFx.store(hfx);
            ImGui::SameLine();
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_HITFX));
            ImGui::SameLine();
            {
                float tx2 = ImGui::GetCursorPosX() + 18.f;
                if (tx2 < colW + 74.f) tx2 = colW + 74.f;
                ImGui::SetCursorPosX(tx2);
            }
            bool jp = s_judgePop.load(std::memory_order_relaxed);
            snprintf(id, sizeof(id), "##m%djp", mi);
            if (MiniToggle(id, &jp))
                s_judgePop.store(jp);
            ImGui::SameLine();
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_JUDGE));

            // ---- 上隐 / 下隐 / 自动调整延迟 ----
            bool uh = s_upHide[mi].load(std::memory_order_relaxed);
            snprintf(id, sizeof(id), "##m%duh", mi);
            if (MiniToggle(id, &uh))
                s_upHide[mi].store(uh, std::memory_order_relaxed);
            ImGui::SameLine();
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_UPHIDE));
            ImGui::SameLine();
            {
                float tx3 = ImGui::GetCursorPosX() + 18.f;
                if (tx3 < colW + 74.f) tx3 = colW + 74.f;
                ImGui::SetCursorPosX(tx3);
            }
            bool dh = s_dnHide[mi].load(std::memory_order_relaxed);
            snprintf(id, sizeof(id), "##m%ddh", mi);
            if (MiniToggle(id, &dh))
                s_dnHide[mi].store(dh, std::memory_order_relaxed);
            ImGui::SameLine();
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_DNHIDE));
            ImGui::SameLine();
            {
                float tx3 = ImGui::GetCursorPosX() + 18.f;
                if (tx3 < colW + 200.f) tx3 = colW + 200.f;
                ImGui::SetCursorPosX(tx3);
            }
            bool ao = s_autoOff[mi].load(std::memory_order_relaxed);
            snprintf(id, sizeof(id), "##m%dao", mi);
            if (MiniToggle(id, &ao))
            {
                s_autoOff[mi].store(ao, std::memory_order_relaxed);
                BiasClear();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_AUTOOFF));

            // ---- 自动打歌 / 宏打歌 / 录制 ----
            bool ap = s_autoPlay[mi].load(std::memory_order_relaxed);
            snprintf(id, sizeof(id), "##m%dap", mi);
            if (MiniToggle(id, &ap))
            {
                s_autoPlay[mi].store(ap, std::memory_order_relaxed);
                if (ap) s_macroPlay[mi].store(false, std::memory_order_relaxed);   // 互斥
                if (!ap) AutoPlayStop();
                Log::Printf("[UI] %dK auto play -> %d", cols, (int)ap);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_AUTOPLAY));
            ImGui::SameLine();
            {
                float tx4 = ImGui::GetCursorPosX() + 18.f;
                if (tx4 < colW + 74.f) tx4 = colW + 74.f;
                ImGui::SetCursorPosX(tx4);
            }
            bool mp = s_macroPlay[mi].load(std::memory_order_relaxed);
            snprintf(id, sizeof(id), "##m%dmp", mi);
            if (MiniToggle(id, &mp))
            {
                s_macroPlay[mi].store(mp, std::memory_order_relaxed);
                if (mp) s_autoPlay[mi].store(false, std::memory_order_relaxed);
                if (!mp) AutoPlayStop();
                Log::Printf("[UI] %dK macro play -> %d", cols, (int)mp);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_MACRO));
            ImGui::SameLine();
            {
                float tx5 = ImGui::GetCursorPosX() + 18.f;
                if (tx5 < colW + 200.f) tx5 = colW + 200.f;
                ImGui::SetCursorPosX(tx5);
            }
            bool ro = s_recOn.load(std::memory_order_relaxed);
            snprintf(id, sizeof(id), "##m%drec", mi);
            if (MiniToggle(id, &ro))
                RecOnSet(ro ? 1 : 0);
            ImGui::SameLine();
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_RECORD));
            if (s_recOn.load(std::memory_order_relaxed))
            {
                int ri = 0, rw = 0, rd = 0;
                const char* rf = nullptr;
                RecStatsGet(&ri, &rw, &rd, &rf);
                ImGui::TextDisabled("[rec] %d/%d%s", rw, ri, rd ? " (drop)" : "");
            }

            {
                int bn = 0;
                double avg = BiasAvgMs(&bn);
                if (bn > 0)
                    ImGui::TextDisabled("%s: %+.1f ms  (%s)", I18N::Tr(I18N::LBL_BIAS), avg,
                                        I18N::Tr(avg > 0 ? I18N::LBL_FAST : I18N::LBL_SLOW));
                else
                    ImGui::TextDisabled("%s: --", I18N::Tr(I18N::LBL_BIAS));
            }
            {
                int combo = s_jdCombo.load(std::memory_order_relaxed);
                int total = s_jdTotal.load(std::memory_order_relaxed);
                double weight = s_jdWeight.load(std::memory_order_relaxed);
                double acc = total > 0 ? weight / (double)total : 1.0;
                ImGui::TextDisabled("Notes %d \xc2\xb7 Lv.%d \xc2\xb7 Acc %.2f%% \xc2\xb7 Combo %d (max %d)",
                                    s_noteCount.load(std::memory_order_relaxed),
                                    s_level.load(std::memory_order_relaxed),
                                    acc * 100.0, combo,
                                    s_jdMaxCombo.load(std::memory_order_relaxed));
            }
            {
                char diag[192];
                GetDiag(diag, sizeof(diag));
                if (diag[0])
                    ImGui::TextDisabled("%s", diag);
            }
            ImGui::PopStyleVar();
        }
        EndCard4K();
        ImGui::Spacing();

        // 卡片 3：左下角游戏画面小窗（四模式共用同一组参数）
        snprintf(id, sizeof(id), "##cardm%d_mini", mi);
        BeginCard4K(id, 118.f);
        {
            bool mo = s_miniOn.load(std::memory_order_relaxed);
            snprintf(id, sizeof(id), "##m%dmini", mi);
            if (MiniToggle(id, &mo))
                s_miniOn.store(mo);
            ImGui::SameLine();
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_MINION));

            int mx = s_miniX.load(std::memory_order_relaxed);
            int my = s_miniY.load(std::memory_order_relaxed);
            const float kItemSp = ImGui::GetStyle().ItemSpacing.x;
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_POS));
            ImGui::SameLine();
            ImGui::SetCursorPosX(90.f);
            float halfW = (ImGui::GetContentRegionAvail().x - kItemSp) * 0.5f;
            if (halfW < 70.f) halfW = 70.f;
            ImGui::SetNextItemWidth(halfW);
            snprintf(id, sizeof(id), "##m%dminiX", mi);
            if (ImGui::SliderInt(id, &mx, -400, 2000, "X %d"))
                s_miniX.store(mx);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(halfW);
            snprintf(id, sizeof(id), "##m%dminiY", mi);
            if (ImGui::SliderInt(id, &my, -400, 1400, "Y %d"))
                s_miniY.store(my);

            int mw = s_miniW.load(std::memory_order_relaxed);
            int mh = s_miniH.load(std::memory_order_relaxed);
            ImGui::TextUnformatted(I18N::Tr(I18N::LBL_SIZE));
            ImGui::SameLine();
            ImGui::SetCursorPosX(90.f);
            const float kBtnW = 52.f;
            float halfW2 = (ImGui::GetContentRegionAvail().x - kBtnW - kItemSp * 2.f) * 0.5f;
            if (halfW2 < 70.f) halfW2 = 70.f;
            ImGui::SetNextItemWidth(halfW2);
            snprintf(id, sizeof(id), "##m%dminiW", mi);
            if (ImGui::SliderInt(id, &mw, 80, 1600, "W %d"))
                s_miniW.store(mw);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(halfW2);
            snprintf(id, sizeof(id), "##m%dminiH", mi);
            if (ImGui::SliderInt(id, &mh, 45, 1000, "H %d"))
                s_miniH.store(mh);
            ImGui::SameLine();
            snprintf(id, sizeof(id), "##m%dminiR", mi);
            if (ImGui::Button(id, ImVec2(kBtnW, 0)))
            {
                s_miniX.store(16);
                s_miniY.store(848);
                s_miniW.store(384);
                s_miniH.store(216);
            }
        }
        EndCard4K();
    }

    // ---------------- 宏模式页（宏打歌 + 自动录制） ----------------
    void DrawMacroPage()
    {
        RecCfgLoadOnce();

        // 卡片 1：宏打歌（内部虚拟按键 + 拟人化）
        BeginCard4K("##cardMacro", 200.f);
        {
            ImGui::TextUnformatted(I18N::Tr(I18N::MACRO_TITLE));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 166, 182, 255));
            ImGui::TextWrapped("%s", I18N::Tr(I18N::MACRO_DESC));
            ImGui::PopStyleColor();
            ImGui::Spacing();

            ImGui::TextUnformatted(I18N::Tr(I18N::MACRO_MODES));
            ImGui::SameLine();
            ImGui::SetCursorPosX(120.f);
            for (int mi = 0; mi < kModeN; mi++)
            {
                if (mi) ImGui::SameLine();
                char lb[32];
                snprintf(lb, sizeof(lb), "%dK##mpm%d", kLanesOf[mi], mi);
                bool on = s_macroPlay[mi].load(std::memory_order_relaxed);
                if (ImGui::Checkbox(lb, &on))
                {
                    s_macroPlay[mi].store(on, std::memory_order_relaxed);
                    if (on) s_autoPlay[mi].store(false, std::memory_order_relaxed);
                    else    AutoPlayStop();
                }
            }
            // 冰与火（原生关卡）：宏直接代打游戏本体判定线（角度判定），
            // 与 4K/5K/6K/10K 宏互斥；开启期间强制 no-fail 并临时关闭游戏自动演奏。
            {
                bool fire = s_macroFire.load(std::memory_order_relaxed);
                ImGui::SameLine();
                if (ImGui::Checkbox(I18N::Tr(I18N::MACRO_FIRE), &fire))
                    FireMacroSet(fire ? 1 : 0);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(140, 146, 162, 255));
            ImGui::TextWrapped("%s", I18N::Tr(I18N::MACRO_FIRE_HINT));
            ImGui::PopStyleColor();

            int acc = MacroAccGet();
            ImGui::TextUnformatted(I18N::Tr(I18N::MACRO_ACC));
            ImGui::SameLine();
            ImGui::SetCursorPosX(120.f);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::SliderInt("##macrocacc", &acc, 90, 100, "%d%%"))
                MacroAccSet(acc);

            int hum = MacroHumanGet();
            ImGui::TextUnformatted(I18N::Tr(I18N::MACRO_HUMAN));
            ImGui::SameLine();
            ImGui::SetCursorPosX(120.f);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::SliderInt("##macrochum", &hum, 0, 100, "%d%%"))
                MacroHumanSet(hum);

            ImGui::Spacing();
            if (ImGui::Button(I18N::Tr(I18N::MACRO_CALIB), ImVec2(150.f, 26.f)))
            {
                for (int mi = 0; mi < kModeN; mi++)
                    s_autoOff[mi].store(true, std::memory_order_relaxed);
                BiasClear();
                Log::Printf("[macro] auto offset calibration enabled (all modes)");
            }
            ImGui::SameLine();
            {
                int bn = 0;
                double avg = BiasAvgMs(&bn);
                if (bn > 0)
                    ImGui::TextDisabled("%s: %+.1f ms (%s)", I18N::Tr(I18N::LBL_BIAS), avg,
                                        I18N::Tr(avg > 0 ? I18N::LBL_FAST : I18N::LBL_SLOW));
                else
                    ImGui::TextDisabled("%s: --", I18N::Tr(I18N::LBL_BIAS));
            }
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(140, 146, 162, 255));
            ImGui::TextWrapped("%s", I18N::Tr(I18N::MACRO_CALIBHINT));
            ImGui::PopStyleColor();
        }
        EndCard4K();
        ImGui::Spacing();
    }

    // ---------------- 录制页（小窗录制：游戏原生画面 → H.264/MP4） ----------------
    // 与宏打歌页分离：录制开关 / 自动跟随 / 输出目录 / 帧率码率 / 开始-暂停-继续
    void DrawRecordPage()
    {
        RecCfgLoadOnce();

        BeginCard4K("##cardRecPage", 0.f);
        {
            ImGui::TextUnformatted(I18N::Tr(I18N::REC_TITLE));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 166, 182, 255));
            ImGui::TextWrapped("%s", I18N::Tr(I18N::REC_PAGEDESC));
            ImGui::PopStyleColor();
            ImGui::Spacing();

            bool ron = s_recOn.load(std::memory_order_relaxed);
            if (MiniToggle("##recOn", &ron))
                RecOnSet(ron ? 1 : 0);
            ImGui::SameLine();
            ImGui::TextUnformatted(I18N::Tr(I18N::REC_TITLE));
            ImGui::SameLine();
            {
                float tx = ImGui::GetCursorPosX() + 18.f;
                if (tx < 200.f) tx = 200.f;
                ImGui::SetCursorPosX(tx);
            }
            bool rauto = RecAutoGet() != 0;
            if (MiniToggle("##recAuto", &rauto))
                RecAutoSet(rauto ? 1 : 0);
            ImGui::SameLine();
            ImGui::TextUnformatted(I18N::Tr(I18N::REC_AUTO));

            static char s_dirBuf[MAX_PATH * 2] = { 0 };
            static bool s_dirInit = false;
            if (!s_dirInit)
            {
                s_dirInit = true;
                snprintf(s_dirBuf, sizeof(s_dirBuf), "%s", RecDirGet());
            }
            ImGui::TextUnformatted(I18N::Tr(I18N::REC_DIR));
            ImGui::SameLine();
            ImGui::SetCursorPosX(120.f);
            const float btnW = 74.f;
            float avail = ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemWidth(avail - btnW * 2.f - 16.f);
            if (ImGui::InputText("##recdir", s_dirBuf, sizeof(s_dirBuf), ImGuiInputTextFlags_EnterReturnsTrue))
                RecDirSet(s_dirBuf);
            ImGui::SameLine();
            if (ImGui::Button(I18N::Tr(I18N::ST_APPLY), ImVec2(btnW, 0)))
                RecDirSet(s_dirBuf);
            ImGui::SameLine();
            if (ImGui::Button(I18N::Tr(I18N::REC_OPEN), ImVec2(btnW + 8.f, 0)))
                ShellExecuteA(nullptr, "open", RecDirGet(), nullptr, nullptr, SW_SHOWNORMAL);

            int fps = RecFpsGet();
            ImGui::TextUnformatted(I18N::Tr(I18N::REC_FPS));
            ImGui::SameLine();
            ImGui::SetCursorPosX(120.f);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::SliderInt("##recfps", &fps, 15, 144, "%d"))
                RecFpsSet(fps);

            int mbps = RecMbpsGet();
            ImGui::TextUnformatted(I18N::Tr(I18N::REC_MBPS));
            ImGui::SameLine();
            ImGui::SetCursorPosX(120.f);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::SliderInt("##recmbps", &mbps, 2, 100, "%d Mbps"))
                RecMbpsSet(mbps);

            {
                int ri = 0, rw = 0, rd = 0;
                const char* rf = nullptr;
                RecStatsGet(&ri, &rw, &rd, &rf);
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 166, 182, 255));
                ImGui::TextWrapped("%s: %d / %d%s%s%s", I18N::Tr(I18N::REC_STATS), rw, ri,
                                   rd ? " (drop)" : "", (rf && rf[0]) ? " -> " : "",
                                   (rf && rf[0]) ? rf : "");
                ImGui::PopStyleColor();
            }
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(140, 146, 162, 255));
            ImGui::TextWrapped("%s", I18N::Tr(I18N::REC_HINT));
            ImGui::PopStyleColor();

            // 开始 / 暂停 / 继续（暂停会结束当前分段，恢复时写新文件）
            ImGui::Spacing();
            const bool recOnNow = s_recOn.load(std::memory_order_relaxed) != 0;
            const bool paused   = RecPausedGet() != 0;
            const bool active   = GameRecorder::Active();
            const float bw = 110.f;
            if (!recOnNow || (!active && !paused))
            {
                if (ImGui::Button(I18N::Tr(I18N::REC_START), ImVec2(bw, 28.f)))
                {
                    RecPausedSet(0);
                    if (!s_recOn.load(std::memory_order_relaxed))
                        RecOnSet(1);
                    else
                        RecCfgApply();
                }
            }
            else if (!paused)
            {
                ImGui::BeginDisabled(!active);
                if (ImGui::Button(I18N::Tr(I18N::REC_PAUSE), ImVec2(bw, 28.f)))
                    RecPausedSet(1);
                ImGui::EndDisabled();
            }
            else
            {
                if (ImGui::Button(I18N::Tr(I18N::REC_RESUME), ImVec2(bw, 28.f)))
                    RecPausedSet(0);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s%s", active ? "* " : "", paused ? I18N::Tr(I18N::REC_PAUSED_HINT) : "");
        }
        EndCard4K();
    }

    int ApiModeIndex() { return ActiveModeIndex(); }

    bool ApiModeEnabled(int mi)
    {
        if (mi < 0 || mi >= kModeN) return false;
        return s_en[mi].load(std::memory_order_relaxed);
    }

    void ApiSetModeEnabled(int mi, bool on)
    {
        if (mi < 0 || mi >= kModeN) return;
        for (int i = 0; i < kModeN; i++)
            s_en[i].store((i == mi) ? on : false, std::memory_order_relaxed);
        if (on)
            Log::Printf("[api] %dK assist enabled by script", kLanesOf[mi]);
    }

    int ApiSpeed(int mi)
    {
        if (mi < 0 || mi >= kModeN) return 0;
        return s_speed[mi].load(std::memory_order_relaxed);
    }
    void ApiSetSpeed(int mi, int v)
    {
        if (mi < 0 || mi >= kModeN) return;
        if (v < 1) v = 1;
        if (v > 20) v = 20;
        s_speed[mi].store(v, std::memory_order_relaxed);
    }

    int ApiOffset(int mi)
    {
        if (mi < 0 || mi >= kModeN) return 0;
        return s_offsetMS[mi].load(std::memory_order_relaxed);
    }
    void ApiSetOffset(int mi, int v)
    {
        if (mi < 0 || mi >= kModeN) return;
        if (v < -400) v = -400;
        if (v > 400) v = 400;
        s_offsetMS[mi].store(v, std::memory_order_relaxed);
    }

    int ApiStyle(int mi)
    {
        if (mi < 0 || mi >= kModeN) return 0;
        return s_style[mi].load(std::memory_order_relaxed);
    }
    void ApiSetStyle(int mi, int v)
    {
        if (mi < 0 || mi >= kModeN) return;
        if (v < 0) v = 0;
        if (v >= ApiStyleCount()) v = ApiStyleCount() - 1;
        s_style[mi].store(v, std::memory_order_relaxed);
    }

    int ApiStyleCount() { return 6; }   // 经典 / 叠 / 技 / 乱 / 切 / 冰火手法
    const char* ApiStyleName(int i) { return (i >= 0 && i < 6) ? kStyleNames[i] : ""; }

    const char* ApiModeName(int mi)
    {
        static const char* kN[kModeN] = { "4K", "5K", "6K", "10K" };
        return (mi >= 0 && mi < kModeN) ? kN[mi] : "-";
    }

    int ApiLevel() { return s_level.load(std::memory_order_relaxed); }
    double ApiSongTime() { return s_songTime.load(std::memory_order_relaxed); }
    double ApiBpm() { return (double)s_bpm.load(std::memory_order_relaxed); }

    bool ApiPlaying()
    {
        CheatState::Status st;
        GameBridge::GetStatusSnapshot(&st);
        return st.gameworld && !st.paused &&
               (s_songStarted.load(std::memory_order_relaxed) ||
                s_noteCount.load(std::memory_order_relaxed) > 0);
    }

    int ApiJudgeCombo() { return s_jdCombo.load(std::memory_order_relaxed); }

    float ApiJudgeAcc()
    {
        int    total  = s_jdTotal.load(std::memory_order_relaxed);
        double weight = s_jdWeight.load(std::memory_order_relaxed);
        return (total > 0) ? (float)(weight / (double)total) : 1.0f;
    }

    int ApiJudgeCount(int kind)
    {
        return (kind >= 0 && kind < 4) ? s_jdCounts[kind].load(std::memory_order_relaxed) : 0;
    }

    const char* ApiLevelName()
    {
        static char s_name[160];
        CheatState::Status st;
        GameBridge::GetStatusSnapshot(&st);
        strncpy_s(s_name, sizeof(s_name), st.levelName, _TRUNCATE);
        return s_name;
    }
}
