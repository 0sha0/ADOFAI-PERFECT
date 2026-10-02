#include "Chart4K.h"
#include "CheatState.h"
#include "MonoApi.h"
#include "GameBridge.h"
#include "Log.h"
#include "RenderHook.h"

#include "imgui.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <mutex>
#include <string>
#include <algorithm>

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

    // ---------------- 用户设置 ----------------
    static std::atomic<bool>  s_enabled{ false };  // 4K 下坠谱默认关闭（菜单"4K辅助"页开启）
    static std::atomic<int>   s_speed{ 4 };       // Malody 式流速
    static std::atomic<int>   s_offsetMS{ 0 };    // 额外延迟补偿
    static std::atomic<int>   s_opacity{ 242 };   // 谱面底板不透明度
    static std::atomic<bool>  s_hitFx{ true };    // 打击特效（爆发 + 判定闪条）
    static std::atomic<bool>  s_judgePop{ true }; // 中央判定提示（MARV/PERFECT/GOOD/MISS）
    // 左下角"冰与火之舞"画面缩略图（1080p 基准坐标：x/y/w/h 随分辨率等比缩放）
    static std::atomic<bool>  s_miniOn{ true };
    static std::atomic<int>   s_miniX{ 16 }, s_miniY{ 848 };
    static std::atomic<int>   s_miniW{ 384 }, s_miniH{ 216 };

    // ---------------- 6K 模式设置（键位 S D F J K L） ----------------
    static std::atomic<bool>  s_en6k{ false };      // 6K 下坠谱默认关闭（菜单"6K模式"页开启）
    static std::atomic<int>   s_speed6k{ 6 };       // 6K 流速（轨道更窄，默认更快）
    static std::atomic<int>   s_offsetMS6k{ 0 };    // 6K 独立延迟补偿
    // 生成引擎固定为"经典"（4K / 6K 各自独立实现，见 NextLane4K / NextLane6K）。
    // 多押砖（tapsNeeded>1）固定按"自动"处理：平均间隔 >= 75ms 完整展开，
    // 更密的（>13 NPS，人手按不出来）合并为 1 个。

    // 键位：默认 4K = D F J K ／ 6K = S D F J K L（菜单里可改，确认后才生效）
    static const int kVK4Def[4] = { 'D', 'F', 'J', 'K' };
    static const int kVK6Def[6] = { 'S', 'D', 'F', 'J', 'K', 'L' };
    static std::atomic<int> s_vk4[4] = { { 'D' }, { 'F' }, { 'J' }, { 'K' } };
    static std::atomic<int> s_vk6[6] = { { 'S' }, { 'D' }, { 'F' }, { 'J' }, { 'K' }, { 'L' } };
    static int  s_draft4[4] = { 'D', 'F', 'J', 'K' };
    static int  s_draft6[6] = { 'S', 'D', 'F', 'J', 'K', 'L' };
    static bool s_keyEdit4 = false, s_keyEdit6 = false;   // 是否处于编辑（未确认）状态
    static int  s_keyCap4 = -1, s_keyCap6 = -1;           // 正在等待新键的槽位
    static bool s_keyInit4 = false, s_keyInit6 = false;
    static void KeyName(int vk, char* out, size_t n);   // 定义在下方（键位编辑）

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
    static bool  g_clockSongUnits = true;
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
    static std::atomic<double> s_songTime{ 0.0 };
    static std::atomic<float>  s_bpm{ 0.f };
    static std::atomic<bool>   s_inLevel{ false };
    static std::atomic<bool>   s_songStarted{ false };   // song really started (state is unreliable in custom levels)
    static std::atomic<double> s_clockMovedWall{ 0.0 };  // last wall time the song clock was seen moving
    static std::atomic<bool>   s_anchored{ false };      // level start seen (clock re-anchored / restarted)
    static std::atomic<int>    s_level{ 0 };
    static std::atomic<int>    s_combo{ 0 };
    static std::atomic<int>    s_maxCombo{ 0 };
    static std::atomic<float>  s_acc{ 0.f };
    static std::atomic<int>    s_noteCount{ 0 };
    static std::atomic<bool>   s_resetKeys{ false };
    static std::atomic<int>    s_keyPressCount[6] = { {0}, {0}, {0}, {0}, {0}, {0} };

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
    static std::atomic<int>    s_jdCombo{ 0 };
    static std::atomic<int>    s_jdMaxCombo{ 0 };
    static std::atomic<int>    s_jdTotal{ 0 };
    static std::atomic<double> s_jdWeight{ 0.0 };
    static std::atomic<int>    s_jdLastKind{ -1 };
    static std::atomic<double> s_jdLastTime{ -100.0 };
    static std::atomic<int>    s_jdLastLane{ 0 };
    static std::atomic<double> s_jdLastOff{ 0.0 };   // 最近一次判定偏差（秒，负=早）

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
    static float                s_laneFlash[6] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };  // 判定闪条强度
    static float                s_laneHitBg[6] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };  // 判定底光强度

    static std::atomic<int>    s_chartGen{ 0 };     // 谱面重建计数（渲染线程清运行时状态）

    // 渲染线程时钟：桥接线程每 ~7ms 更新 song 时间，这里用 QPC 线性外推，
    // 避免音符下落出现 7ms 级别的抖动（音画同步的关键）
    static std::atomic<double> s_clockWall{ 0.0 };  // 桥接线程采样时钟时的 QPC 秒
    static std::atomic<double> s_clockRate{ 1.0 };  // 实测 song 秒 / 墙秒（song.pitch）
    // ---------------- 皮肤贴图 ----------------
    // 皮肤目录解析优先级：
    //   1) 环境变量 ADOFAI_PERFECT_SKIN
    //   2) 运行目录下 skin_dir.txt 首行指定的目录（自定义皮肤）
    //   3) 运行目录（exe/DLL 同级）下的 skin\（发布包自带；含子目录时自动选有效子目录）
    //   4) 兜底：Steam 库中的 MalodyV\skin
    static char s_skinDir[MAX_PATH * 2] = { 0 };

    static bool DirHasSkin(const char* dir)
    {
        char buf[MAX_PATH * 2];
        snprintf(buf, sizeof(buf), "%s\\rurudokey.png", dir);
        DWORD a = GetFileAttributesA(buf);
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
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

    static const char* SkinDir()
    {
        if (s_skinDir[0])
            return s_skinDir;

        // 未找到时不每帧全盘重扫：每 300 次调用最多重试一次
        static int s_missN = 0;
        if (s_missN > 0)
        {
            if (++s_missN < 300)
                return s_skinDir;
            s_missN = 1;
        }

        // 1) 环境变量
        char env[MAX_PATH * 2];
        DWORD en = GetEnvironmentVariableA("ADOFAI_PERFECT_SKIN", env, sizeof(env));
        if (en > 0 && en < sizeof(env) && DirHasSkin(env))
            snprintf(s_skinDir, sizeof(s_skinDir), "%s", env);
        if (s_skinDir[0])
            return s_skinDir;

        // 2) DLL 同目录 skin_dir.txt
        char modPath[MAX_PATH * 2] = { 0 };
        HMODULE hm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)&SkinDir, &hm) && hm)
            GetModuleFileNameA(hm, modPath, sizeof(modPath));
        if (modPath[0])
        {
            char* slash = strrchr(modPath, '\\');
            if (slash)
                *slash = 0;
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
                    if (line[0] && DirHasSkin(line))
                        snprintf(s_skinDir, sizeof(s_skinDir), "%s", line);
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

        // 3c) 各盘符常见 Steam 库目录
        static const char* kSub[] = {
            "Program Files (x86)\\Steam", "Program Files\\Steam", "Steam",
            "SteamLibrary", "Games\\SteamLibrary", "SteamLibrary2",
        };
        for (char drv = 'C'; drv <= 'H' && !s_skinDir[0]; drv++)
        {
            for (const char* sub : kSub)
            {
                char root[MAX_PATH * 2];
                snprintf(root, sizeof(root), "%c:\\%s", drv, sub);
                if (TrySkinRoot(root, s_skinDir, sizeof(s_skinDir)))
                    break;
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
    static void* s_texCharIdle = nullptr;               // rurudokey.png（右上常驻完整小人）
    static void* s_texCharKey[4] = {};                  // rurudokey-1..4.png（打击姿势）
    static void* s_texCheek = nullptr;                  // m5judgerrda.png（右下托腮小人）
    static void* s_texJudgePop[5] = {};                 // m5judgerrda..e（中央判定提示 5 色变体）
    static void* s_texNote = nullptr;                   // notex-1.png
    static void* s_texPress = nullptr;                  // notepress1.png
    static void* s_texJudge = nullptr;                  // judgercolor.png（彩虹判定线）
    static void* s_texLogo = nullptr;                   // m5logo.png
    static void* s_texAvatar = nullptr;                 // progressrrd.png
    static void* s_texCombo[12] = {};                   // combo-0..9 + % + .
    static void* s_texAcc[12] = {};                     // acc-0..9 + % + .
    static void* s_texBurst = nullptr;                  // hits-1.png（打击爆发特效）
    static void* s_texHitBg = nullptr;                  // notehitbg.png（判定区底光，4 条）
    static void* s_texLine = nullptr;                   // noteinex.png（判定线）
    static void* s_texGrid = nullptr;                   // bggrid.png（轨道网格）
    static void* s_texJudgeBar[4] = {};                 // judge-0..3.png（判定闪条）
    static void* s_texHold = nullptr;                   // lnx6.png（长按条）
    static void* s_texPress2 = nullptr;                 // notepress2.png（内侧两键）
    static void* s_texHits[9] = {};                     // hits-1..9.png（打击特效 9 帧）
    static void* s_texPnum = nullptr;                   // anton-0.png（数字字体句柄）
    static void* s_texPnumSet[12] = {};                 // anton-0..9 + % + .
    static int    s_kps[7] = { 0, 0, 0, 0, 0, 0, 0 };   // 每轨 KPS + 合计（右上数字）
    static void* s_texRing = nullptr;                   // circle.png（金色辉光环）
    static void* s_texRingOut = nullptr;                // circleout2.png（蓝色虚线圆）
    static void* s_texRingIn = nullptr;                 // circlein2.png（白色细圆）
    static int   s_skinTries = 0;
    static bool  s_skinLoaded = false;

    static void* LoadTex(const char* file)
    {
        const char* dir = SkinDir();
        if (!dir[0])
            return nullptr;
        char buf[512];
        snprintf(buf, sizeof(buf), "%s\\%s", dir, file);
        return RenderHook_LoadTexture(buf);
    }

    // 返回是否全部就绪；D3D 设备未就绪时下一帧重试
    static bool LoadSkin()
    {
        static bool s_loggedDir = false;
        if (!s_loggedDir)
        {
            s_loggedDir = true;
            Log::Printf("[4K] skin dir = %s", SkinDir());
        }
        if (s_skinLoaded)
            return true;
        s_texCharIdle = LoadTex("rurudokey.png");
        for (int i = 0; i < 4; i++)
        {
            char n[32];
            snprintf(n, sizeof(n), "rurudokey-%d.png", i + 1);
            s_texCharKey[i] = LoadTex(n);
        }
        s_texCheek = LoadTex("m5judgerrda.png");
        {
            const char* jp[5] = { "m5judgerrda.png", "m5judgerrdb.png", "m5judgerrdc.png",
                                  "m5judgerrdd.png", "m5judgerrde.png" };
            for (int i = 0; i < 5; i++)
                s_texJudgePop[i] = LoadTex(jp[i]);
        }
        s_texNote = LoadTex("notex-1.png");
        s_texPress = LoadTex("notepress1.png");
        s_texJudge = LoadTex("judgercolor.png");
        s_texLogo = LoadTex("m5logo.png");
        s_texAvatar = LoadTex("progressrrd.png");
        for (int i = 0; i <= 9; i++)
        {
            char n[32];
            snprintf(n, sizeof(n), "combo-%d.png", i);
            s_texCombo[i] = LoadTex(n);
            snprintf(n, sizeof(n), "acc-%d.png", i);
            s_texAcc[i] = LoadTex(n);
        }
        s_texCombo[10] = LoadTex("combo-10.png");
        s_texCombo[11] = LoadTex("combo-11.png");
        s_texAcc[10] = LoadTex("acc-10.png");
        s_texAcc[11] = LoadTex("acc-11.png");
        s_texBurst = LoadTex("hits-1.png");
        s_texHitBg = LoadTex("notehitbg.png");
        s_texLine = LoadTex("noteinex.png");
        s_texGrid = LoadTex("bggrid.png");
        s_texHold = LoadTex("lnx6.png");
        s_texPress2 = LoadTex("notepress2.png");
        for (int i = 0; i < 9; i++)
        {
            char n[32];
            snprintf(n, sizeof(n), "hits-%d.png", i + 1);
            s_texHits[i] = LoadTex(n);
        }
        s_texBurst = s_texHits[0];
        for (int i = 0; i <= 9; i++)
        {
            char n[32];
            snprintf(n, sizeof(n), "anton-%d.png", i);
            s_texPnumSet[i] = LoadTex(n);
        }
        s_texPnum = s_texPnumSet[0];
        s_texRing = LoadTex("circle.png");
        s_texRingOut = LoadTex("circleout2.png");
        s_texRingIn = LoadTex("circlein2.png");
        for (int i = 0; i < 4; i++)
        {
            char n[32];
            snprintf(n, sizeof(n), "judge-%d.png", i);
            s_texJudgeBar[i] = LoadTex(n);
        }

        if (s_texCharIdle && s_texNote && s_texJudge && s_texCombo[0] && s_texAcc[0])
        {
            s_skinLoaded = true;
            Log::Printf("[4K] skin ready: char=%d cheek=%d note=%d judge=%d combo=%d acc=%d hits=%d hitbg=%d line=%d grid=%d jbar=%d press2=%d pnum=%d",
                        s_texCharIdle != 0, s_texCheek != 0, s_texNote != 0,
                        s_texJudge != 0, s_texCombo[0] != 0, s_texAcc[0] != 0,
                        s_texHits[0] != 0, s_texHitBg != 0, s_texLine != 0, s_texGrid != 0,
                        s_texJudgeBar[0] != 0, s_texPress2 != 0, s_texPnumSet[0] != 0);
        }
        else if (++s_skinTries % 60 == 0)
        {
            Log::Printf("[4K] skin pending: char=%d note=%d judge=%d combo=%d acc=%d",
                        s_texCharIdle != 0, s_texNote != 0, s_texJudge != 0,
                        s_texCombo[0] != 0, s_texAcc[0] != 0);
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

    // 地砖原始数据（POD）
    struct RawFloor
    {
        double t;        // entryTime（song 秒）
        double tp;       // entryTimePitchAdj（真实秒）
        double ang;      // angleLength（弧度）
        int    holdLen;  // holdLength
        int    taps;     // tapsNeeded
        bool   ccw;
        bool   valid;    // 需要玩家打击的块
    };

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
                r.valid = !(*(bool*)((char*)f + oF_midSpin)) &&
                          !(*(bool*)((char*)f + oF_isFake)) &&
                          !(*(bool*)((char*)f + oF_auto));
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
    // 分键引擎：4K / 6K 是两套彼此独立的算法（不是一个算法换轨道数）
    // ============================================================
    static constexpr double kFastDt = 0.45;     // 连打阈值（<=0.45s 视为连打）
    static constexpr double kRepeatGuard = 0.60;// 同键保护窗
    static constexpr double kSameHandDt = 1.00; // 慢速允许同手的时间窗

    // ---------------- 4K 引擎（DFJK：0=D 1=F 2=J 3=K） ----------------
    //   左手 D/F，右手 J/K；食指=内(F/J)，中指=外(D/K)。
    //   · 连打（<=0.45s）：强制换手 + 保持当前手指
    //         → 短爆发是干净的 F J 交互，长流被同键保护自然滚成 F J D K，
    //           永不出现单手轮指
    //   · 稀疏音：逆时针=左手 / 顺时针=右手；大回转（>=252°）=外键重音 D/K
    //   · 同键 0.6s 内不重复；被长按占用的键不落
    static constexpr double kAccent4K = 4.40;   // 4K 大回转阈值（>=252°）

    struct State4K
    {
        int    prevLane = -1;
        int    lastHand = 0;
        int    lastOfHand[2] = { -1, -1 };
        double holdUntil[4] = { -1e9, -1e9, -1e9, -1e9 };
        double prevTime = -1e9;
        int    streamFinger = 0;
    };
    static inline int Lane4K(int hand, int finger)
    {
        return (hand == 0) ? (1 - finger) : (2 + finger);
    }
    static int NextLane4K(State4K& st, int dir, double turn, double t, double hold)
    {
        const double dt = t - st.prevTime;
        const bool fast = (dt > 0.0) && (dt <= kFastDt);
        const bool cold = (st.prevLane < 0) || (dt > 2.5);
        int hand = 0, finger = 0;
        if (cold || !fast)
        {
            hand   = (dir < 0) ? 0 : 1;
            finger = (turn >= kAccent4K) ? 1 : 0;
            if (!cold && hand == st.lastHand && dt <= kSameHandDt)
                hand = 1 - hand;
            st.streamFinger = finger;
        }
        else
        {
            hand   = 1 - st.lastHand;      // 连打换手
            finger = st.streamFinger;      // 保持同指：F J F J
        }
        int lane = Lane4K(hand, finger);
        if (lane == st.lastOfHand[hand] && dt < kRepeatGuard)
        {
            int alt = Lane4K(hand, 1 - finger);
            if (alt != lane) { finger = 1 - finger; lane = alt; }
        }
        if (st.holdUntil[lane] > t)
        {
            int alt = Lane4K(hand, 1 - finger);
            if (alt != lane && st.holdUntil[alt] <= t && alt != st.lastOfHand[hand])
                lane = alt;
        }
        if (hold > 0.0)
            st.holdUntil[lane] = t + hold;
        st.lastOfHand[hand] = lane;
        st.prevLane = lane;
        st.prevTime = t;
        st.lastHand = hand;
        return lane;
    }

    // ---------------- 6K 引擎（SDFJKL：0=S 1=D 2=F 3=J 4=K 5=L） ----------------
    //   左手 S/D/F，右手 J/K/L；内指=食指 F/J，中指 D/K，外指=无名指 S/L。
    //
    //   6K 与 4K 最大的不同：每只手三根手指、键距更宽，写谱必须考虑
    //   "手在键盘上的位置"，而不是逐音随手挑键（osu!mania mapping guide /
    //   10K+ playstyles：避免别扭手位；楼梯、交叉、滚筒是 6K 的基本语汇）。
    //
    //   连打采用 6K 专属的"双螺旋"24 音循环（每 24 音每键恰好 4 次，
    //   双手逐音交替、每只手内指法 0→1→2 步进，天然无同手同键）：
    //     F L D K S J | F L D K S J | D K F L S J | D K F L S J
    //   即 内-外 / 中-中 / 外-内 交叉 → 再镜像回来，是最常见的 6K 交叉楼梯；
    //   长连打每 24 音相位翻转一次（前/后半交换），不呆板也不偏离循环。
    //
    //   · 外侧 S/L 只出现在循环固定位置（每 6 音一次）与慢速大回转重音，
    //     不会连续压在无名指上
    //   · 稀疏音：逆时针=左手 / 顺时针=右手；大回转（>=252°）=外侧 S/L，
    //     中回转（>=166°）=中指 D/K，小回转=食指 F/J
    //   · 同键 0.6s 内不重复；被长按占用的键不落
    static constexpr double kAccent6K  = 4.40;  // 大回转（>=252°）→ 外侧 S/L
    static constexpr double kMidTurn6K = 2.90;  // 中回转（>=166°）→ 中指 D/K

    // 24 音循环表：每项 = 一次双手交替的两音 (先手手指, 后手手指)
    static const int kCycle6K[12][2] = {
        { 0, 2 }, { 1, 1 }, { 2, 0 }, { 0, 2 }, { 1, 1 }, { 2, 0 },
        { 1, 1 }, { 0, 2 }, { 2, 0 }, { 1, 1 }, { 0, 2 }, { 2, 0 }
    };

    struct State6K
    {
        int    prevLane = -1;
        int    lastHand = 0;
        int    lastOfHand[2] = { -1, -1 };
        double holdUntil[6] = { -1e9, -1e9, -1e9, -1e9, -1e9, -1e9 };
        double prevTime = -1e9;
        int    streamPos = -1;    // 连打循环位置（-1 = 当前不在连打）
        int    streamCycle = 0;   // 连打内完成的循环数（奇偶决定相位）
        int    startHand = 0;     // 本段连打的起手（与上一音换手）
    };
    static inline int Lane6K(int hand, int finger)
    {
        return (hand == 0) ? (2 - finger) : (3 + finger);
    }
    static int NextLane6K(State6K& st, int dir, double turn, double t, double hold)
    {
        const double dt = t - st.prevTime;
        const bool fast = (dt > 0.0) && (dt <= kFastDt);
        const bool cold = (st.prevLane < 0) || (dt > 2.5);
        int hand = 0, finger = 0;
        if (cold || !fast)
        {
            // 稀疏音：按谱面几何落键 + 回转幅度决定手指；
            // 中速段（<=1s）强制换手，避免同手连着出音（单手轮指苗头）
            st.streamPos = -1;
            if (cold || dt > kSameHandDt)
                hand = (dir < 0) ? 0 : 1;
            else
                hand = 1 - st.lastHand;
            finger = (turn >= kAccent6K) ? 2 : ((turn >= kMidTurn6K) ? 1 : 0);
        }
        else
        {
            // 连打：6K 专属双螺旋循环（双手逐音交替，24 音每键 4 次）
            if (st.streamPos < 0)
            {
                st.streamPos   = 0;
                st.streamCycle = 0;
                st.startHand   = 1 - st.lastHand;   // 与上一音换手起拍
            }
            if (st.streamPos >= 24)
            {
                st.streamPos = 0;
                st.streamCycle++;
            }
            int pos = st.streamPos + ((st.streamCycle & 1) ? 12 : 0);
            if (pos >= 24)
                pos -= 24;
            const int* pr = kCycle6K[pos >> 1];
            if (pos & 1) { hand = 1 - st.startHand; finger = pr[1]; }
            else         { hand = st.startHand;     finger = pr[0]; }
            st.streamPos++;
        }
        int lane = Lane6K(hand, finger);
        // 同键保护 + 长按避让：在本手 3 根手指里换用可用键
        // （注意手指必须 0..2 内换，不能用 1-finger：外指 2 会算出 -1 跑到另一只手）
        bool laneBusy = (lane == st.lastOfHand[hand] && dt < kRepeatGuard)
                        || (st.holdUntil[lane] > t);
        if (laneBusy)
        {
            for (int k = 1; k <= 2; k++)
            {
                int f = (finger + k) % 3;
                int alt = Lane6K(hand, f);
                if (alt != st.lastOfHand[hand] && st.holdUntil[alt] <= t)
                { finger = f; lane = alt; break; }
            }
        }
        if (hold > 0.0)
            st.holdUntil[lane] = t + hold;
        st.lastOfHand[hand] = lane;
        st.prevLane = lane;
        st.prevTime = t;
        st.lastHand = hand;
        return lane;
    }

    // ---------------- 难度（Malody 式 Lv） ----------------
    // 用本机 Malody 谱库 270 份真实谱面（152 份带 Lv. 的 4K 谱 + 12 份 6K 谱）
    // 做最小二乘回归校准，特征为：
    //   p95(NPS@8s) = 8 秒滑窗密度的 95 分位（代表持续 NPS）
    //   ln(音符数)  = 耐力 / 稳定性加成（Malody 社区定级对长谱显著更高）
    // 拟合结果（留一交叉验证 RMSE 2.6 / MAE 2.0；旧公式 RMSE 10.4，系统性偏低 10 级）
    //   Lv = -24.175 + 3.131 × p95_8 + 5.076 × ln(n)
    static double WindowNpsPercentile(const std::vector<Note>& notes, double win, double q)
    {
        size_t n = notes.size();
        if (n == 0)
            return 0.0;
        std::vector<double> v;
        v.reserve(n);
        size_t lo = 0;
        for (size_t hi = 0; hi < n; hi++)
        {
            while (notes[hi].time - notes[lo].time > win)
                lo++;
            double span = notes[hi].time - notes[lo].time;
            if (span < win * 0.5)
                span = win * 0.5;
            v.push_back((double)(hi - lo + 1) / span);
        }
        std::sort(v.begin(), v.end());
        size_t idx = (size_t)((double)(v.size() - 1) * q + 0.5);
        if (idx >= v.size())
            idx = v.size() - 1;
        return v[idx];
    }

    static int EstimateLevel(const std::vector<Note>& notes)
    {
        size_t n = notes.size();
        if (n < 4)
            return 1;
        double p95 = WindowNpsPercentile(notes, 8.0, 0.95);
        double lv = -24.175 + 3.131 * p95 + 5.076 * std::log((double)n);
        if (lv < 1.0) lv = 1.0;
        if (lv > 45.0) lv = 45.0;
        return (int)std::lround(lv);
    }

    // ---------------- 谱面构建 ----------------
    static void DumpChartTxt();
    static void BuildChart(const RawFloor* fl, int n)
    {
        const int  cols  = s_en6k.load(std::memory_order_relaxed) ? 6 : 4;
        const int  half  = cols / 2;
        std::vector<Note> notes;
        notes.reserve((size_t)n);
        State4K ls4;
        State6K ls6;
        // 手感自检统计
        int  statNotes = 0, statStream = 0, statStreamAlt = 0, statJack = 0;
        int  statSameHand = 0;     // 连打里同手连续（轮指苗头，应恒为 0）
        int  statHandRepeat = 0;   // 同一只手连续两次落同键（锚点，应恒为 0）
        int  statPrevHandLane[2] = { -1, -1 };
        int  statLane[6] = { 0, 0, 0, 0, 0, 0 };
        int  statLaneRun = 0, statLaneRunMax = 0;
        int  statMashed = 0;
        int  prevL = -1;
        double prevT = -1e9;
        // 第 0 块是起点（玩家站在那里，不用按也不出音）；
        // 最后一格是终点块：scrPlanet 在 nextfloor==null 时播结束动画，
        // 玩家必须按下这一格才算通关 —— 所以最后一格必须转成音符。
        for (int i = 1; i < n; i++)
        {
            const RawFloor& r = fl[i];
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
            // 多押砖（tapsNeeded>1）：一格内 N 次快按。展开后平均间隔 < 75ms
            // （>13 NPS）时人类根本按不出来，自动合并（对应游戏内
            // "多押砖 = HitOnce" 选项）。
            {
                int maxTaps = (int)std::floor((t1 - t0) / 0.075);
                if (maxTaps < 1)
                    maxTaps = 1;
                if (taps > maxTaps)
                    taps = maxTaps;
            }
            if (taps != ((r.taps > 0) ? r.taps : 1))
                statMashed++;
            // 终点块不存在"下一格"，长按无意义（游戏中该块 holdLength 不会
            // 延长任何东西），按普通单键处理
            float hold = (r.holdLen > 0 && hasNext) ? (float)(t1 - t0) : 0.f;
            for (int k = 0; k < taps; k++)
            {
                Note nt;
                nt.time = t0 + (t1 - t0) * (double)k / (double)taps;
                nt.lane = (cols == 6)
                              ? NextLane6K(ls6, dir, turn / taps, nt.time,
                                           (k == 0) ? (double)hold : 0.0)
                              : NextLane4K(ls4, dir, turn / taps, nt.time,
                                           (k == 0) ? (double)hold : 0.0);
                nt.hold = (k == 0) ? hold : 0.f;
                nt.dir = dir;
                nt.turn = (float)(turn / taps);
                notes.push_back(nt);

                double dt = nt.time - prevT;
                bool stream = (dt <= kFastDt && dt > 0.0);
                if (prevL >= 0)
                {
                    bool sameLane = (nt.lane == prevL);
                    if (sameLane && dt < 0.5)
                        statJack++;
                    int hNow = (nt.lane < half) ? 0 : 1;
                    if (statPrevHandLane[hNow] == nt.lane && dt < kRepeatGuard)
                        statHandRepeat++;
                    statPrevHandLane[hNow] = nt.lane;
                    int hand = (nt.lane < half) ? 0 : 1;
                    int prevHand = (prevL < half) ? 0 : 1;
                    if (stream)
                    {
                        statStream++;
                        if (hand != prevHand)
                            statStreamAlt++;
                        else
                            statSameHand++;
                    }
                    if (sameLane) { if (++statLaneRun > statLaneRunMax) statLaneRunMax = statLaneRun; }
                    else          statLaneRun = 1;
                }
                statLane[nt.lane % 6]++;
                prevL = nt.lane;
                prevT = nt.time;
                statNotes++;
            }
        }
        int lv = EstimateLevel(notes);
        s_level.store(lv, std::memory_order_relaxed);

        // 与上一份谱面完全一致就不重建（3 秒周期刷新不应清空连击/按键计数）
        static int    sigSize = -1;
        static double sigFirst = 0.0, sigLast = 0.0, sigLaneSum = 0.0;
        static int    sigMode = -1;       // 4K/6K 切换也要重建
        int modeNow = cols;
        double laneSum = 0.0;
        for (size_t i = 0; i < notes.size(); i++)
            laneSum += notes[i].lane;
        if (!notes.empty() && (int)notes.size() == sigSize &&
            fabs(notes.front().time - sigFirst) < 1e-7 &&
            fabs(notes.back().time - sigLast) < 1e-7 &&
            fabs(laneSum - sigLaneSum) < 1e-9 && modeNow == sigMode)
            return;

        if (statNotes > 0)
        {
            if (cols == 6)
                Log::Printf("[6K] pattern: notes=%d stream=%d handAlt=%.1f%% sameHand=%d handRepeat=%d jack=%d maxLaneRun=%d mashed=%d lane[SDFJKL]=%d/%d/%d/%d/%d/%d",
                            statNotes, statStream,
                            statStream > 0 ? 100.0 * statStreamAlt / statStream : 100.0,
                            statSameHand, statHandRepeat, statJack, statLaneRunMax, statMashed,
                            statLane[0], statLane[1], statLane[2], statLane[3], statLane[4], statLane[5]);
            else
                Log::Printf("[4K] pattern: notes=%d stream=%d handAlt=%.1f%% sameHand=%d handRepeat=%d jack=%d maxLaneRun=%d mashed=%d lane[DFJK]=%d/%d/%d/%d",
                            statNotes, statStream,
                            statStream > 0 ? 100.0 * statStreamAlt / statStream : 100.0,
                            statSameHand, statHandRepeat, statJack, statLaneRunMax, statMashed,
                            statLane[0], statLane[1], statLane[2], statLane[3]);
        }
        sigSize = (int)notes.size();
        sigFirst = notes.empty() ? 0.0 : notes.front().time;
        sigLast = notes.empty() ? 0.0 : notes.back().time;
        sigLaneSum = laneSum;
        sigMode = modeNow;
        {
            std::lock_guard<std::mutex> lk(s_notesMutex);
            s_notes.swap(notes);
            s_chartSize = (int)s_notes.size();
            s_consumed.assign(s_notes.size(), 0);
            s_missed.assign(s_notes.size(), 0);
        }
        s_anchored.store(false, std::memory_order_relaxed);   // new chart -> wait for the next real level start
        s_fx.clear();
        for (int i = 0; i < 6; i++)
            s_laneFlash[i] = 0.f;
        for (int i = 0; i < 4; i++)
            s_jdCounts[i].store(0, std::memory_order_relaxed);
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
        // 关卡加载瞬间 listFloors 会连续重建，去重后只在"音符数变化"时打印/导出
        static int s_lastBuilt = -1;
        if (s_chartSize != s_lastBuilt)
        {
            s_lastBuilt = s_chartSize;
            Log::Printf("[%dK] chart built: %d notes, %.1fs, Lv.%d",
                        cols, s_chartSize, dur, lv);
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
        fprintf(fp, "# notes=%d\n# idx time dt lane hold dir turn\n", (int)s_notes.size());
        for (size_t i = 0; i < s_notes.size(); i++)
        {
            double dt = (i == 0) ? 0.0 : s_notes[i].time - s_notes[i - 1].time;
            fprintf(fp, "%d %.6f %.6f %d %.3f %d %.4f\n",
                    (int)i, s_notes[i].time, dt, s_notes[i].lane,
                    s_notes[i].hold, s_notes[i].dir, s_notes[i].turn);
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
                bool past = (s_notes[i].time < clock - 0.15);   // kJudgeWin
                s_consumed[i] = past ? 1 : 0;
                s_missed[i] = 0;
            }
        }
        s_jdCombo.store(0, std::memory_order_relaxed);
        s_jdTotal.store(0, std::memory_order_relaxed);
        s_jdWeight.store(0.0, std::memory_order_relaxed);
        for (int i = 0; i < 4; i++)
            s_jdCounts[i].store(0, std::memory_order_relaxed);
        s_jdLastKind.store(-1, std::memory_order_relaxed);
        s_chartGen.fetch_add(1, std::memory_order_relaxed);   // 渲染侧清特效/KPS
        Log::Printf("[4K] judge resync @ %.2fs", clock);
    }
    // ---------------- 时钟（渲染线程插值） ----------------
    static double WallNow()
    {
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        return (double)c.QuadPart / (double)f.QuadPart;
    }

    // 渲染线程用的歌曲时钟 = 桥接采样值 + QPC 外推 * 实测速率 + 用户偏移
    static double RenderClock()
    {
        double lead = WallNow() - s_clockWall.load(std::memory_order_relaxed);
        if (lead < 0.0) lead = 0.0;
        if (lead > 0.05) lead = 0.05;
        bool mode6 = s_en6k.load(std::memory_order_relaxed);
        int  offMs = mode6 ? s_offsetMS6k.load(std::memory_order_relaxed)
                           : s_offsetMS.load(std::memory_order_relaxed);
        double off = (double)offMs / 1000.0;
        return s_songTime.load(std::memory_order_relaxed) +
               lead * s_clockRate.load(std::memory_order_relaxed) + off;
    }

    // ---------------- 桥接线程 Tick ----------------
    void Tick()
    {
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
    // Marvelous < 30ms / Perfect < 75ms / Good < 150ms / 超窗未击 = Miss
    static constexpr double kJudgeWin = 0.150;
    static constexpr double kJudgeMarv = 0.030;
    static constexpr double kJudgePerf = 0.075;
    static const double kJudgeWeight[4] = { 1.0, 0.9, 0.5, 0.0 };

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
                if (dt > kJudgeWin)          // 时间有序：后面的只会更远
                    break;
                double ad = dt < 0 ? -dt : dt;
                if (ad <= kJudgeWin && ad < bestAd)
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
        int kind = (bestAd < kJudgeMarv) ? 0 : (bestAd < kJudgePerf) ? 1 : 2;
        s_jdCounts[kind].fetch_add(1, std::memory_order_relaxed);
        s_jdTotal.fetch_add(1, std::memory_order_relaxed);
        s_jdWeight.fetch_add(kJudgeWeight[kind], std::memory_order_relaxed);
        int combo = s_jdCombo.fetch_add(1, std::memory_order_relaxed) + 1;
        int mc = s_jdMaxCombo.load(std::memory_order_relaxed);
        while (combo > mc && !s_jdMaxCombo.compare_exchange_weak(mc, combo)) {}
        s_jdLastKind.store(kind, std::memory_order_relaxed);
        s_jdLastTime.store(clock, std::memory_order_relaxed);
        s_jdLastLane.store(lane, std::memory_order_relaxed);
        s_jdLastOff.store(bestDt, std::memory_order_relaxed);
        if (s_hitFx.load(std::memory_order_relaxed))
            JudgePushFx(lane, kind);
    }

    static void JudgeMissScan(double clock)
    {
        int missed = 0;
        {
            std::lock_guard<std::mutex> lk(s_notesMutex);
            for (size_t i = 0; i < s_notes.size(); i++)
            {
                if (s_consumed[i] || s_missed[i])
                    continue;
                if (s_notes[i].time < clock - kJudgeWin)
                {
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
            if (s_hitFx.load(std::memory_order_relaxed))
            {
                HitFx fx;
                fx.lane = -1;
                fx.kind = 3;
                fx.t0 = ImGui::GetTime();
                fx.scale = 1.f;
                s_fx.push_back(fx);
                if (s_fx.size() > 64)
                    s_fx.erase(s_fx.begin());
            }
        }
    }

    // 渲染线程手持的判定状态（谱面重建时重置）
    static void ResetJudgeRuntime(int gen)
    {
        s_fx.clear();
        for (int i = 0; i < 4; i++)
        {
            s_laneFlash[i] = 0.f;
            s_laneHitBg[i] = 0.f;
        }
        (void)gen;
    }

    // ---------------- 渲染：小工具 ----------------
    static inline float U(float v, float u) { return v * u; }

    static void DrawTextColored(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text)
    {
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), pos, col, text);
    }
    // 带阴影的文字（压在游戏自带 UI 上也能看清）
    static void DrawTextShadow(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text)
    {
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(pos.x + 1.f, pos.y + 1.f), IM_COL32(0, 0, 0, 170), text);
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

    // ---------------- 主绘制 ----------------
    // 版面按示例图（Malody V · Rurudo 4K）逐像素实测重建，1080p 基准 u = gh/1080：
    //   轨道外框 744u（金边 11u）· 内侧键区 177u×4 · 音符 168×63u
    //   判定线 y=915u（音符中心落点）· 底光带 88u · 键块 971..1031 · 键条 1031..1051
    //   左上 圆环(100,91) 半径97 · 时间条 (218,16)-(487,61) · 曲名/BPM/难度 x=220（避开圆环与游戏自带文字）
    //   右上 玩家条 y26..59 · 彩虹条 y59..77 · 5 数字 y68 · 头像 gw-98..gw-23
    //   右上小人 345u @ (gw-345,165) 常驻 + 打击时叠加变化图（不可替换整张）
    //   右下托腮小人 279x301u @ (gw-279,781)
    void DrawPlayfield()
    {
        const bool mode6 = s_en6k.load(std::memory_order_relaxed);
        if (!s_enabled.load(std::memory_order_relaxed) && !mode6)
        {
            RenderHook_SetCaptureWanted(false);
            return;
        }
        // 4K = D F J K ／ 6K = S D F J K L（两模式互斥，菜单里切换）
        const int  cols = mode6 ? 6 : 4;
        const int  half = cols / 2;
        int vkBuf[6];
        for (int i = 0; i < cols; i++)
            vkBuf[i] = (mode6 ? s_vk6[i] : s_vk4[i]).load(std::memory_order_relaxed);
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
                            (int)s_enabled.load(std::memory_order_relaxed),
                            (int)s_notes.size(), (int)live, (int)cons, (int)miss, firstDt);
            }
        }
        const int    speed = std::max(1, (mode6 ? s_speed6k : s_speed).load(std::memory_order_relaxed));
        const float  fallTime = 4.6f / (float)speed;

        // ---- 换谱 → 重置渲染侧运行时（特效 / 底光 / KPS） ----
        static double s_keyHist[6][64] = {};
        static int    s_keyHistPos[6] = { 0, 0, 0, 0, 0, 0 };
        {
            static int s_fxGen = -1;
            int gen = s_chartGen.load(std::memory_order_relaxed);
            if (gen != s_fxGen)
            {
                s_fxGen = gen;
                ResetJudgeRuntime(gen);
                for (int i = 0; i < 6; i++)
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
        static bool   s_prevKey[6] = { false, false, false, false, false, false };
        static double s_poseUntil[6] = { 0, 0, 0, 0, 0, 0 };
        bool keyDown[6] = { false, false, false, false, false, false };
        bool keyHit[6] = { false, false, false, false, false, false };
        for (int i = 0; i < cols; i++)
        {
            keyDown[i] = (GetAsyncKeyState(vkp[i]) & 0x8000) != 0;
            if (keyDown[i] && !s_prevKey[i] && inPlay)
            {
                keyHit[i] = true;
                s_keyPressCount[i].fetch_add(1, std::memory_order_relaxed);
                s_keyHist[i][(s_keyHistPos[i]++) & 63] = nowT;
                JudgePress(i, clock);
                s_poseUntil[i] = nowT + 0.18;
            }
            if (keyDown[i] && inPlay)
                s_poseUntil[i] = std::max(s_poseUntil[i], nowT + 0.10);
            s_prevKey[i] = keyDown[i];
        }
        if (s_resetKeys.exchange(false, std::memory_order_relaxed))
            for (int i = 0; i < 6; i++)
                s_keyPressCount[i].store(0, std::memory_order_relaxed);
        if (inPlay)
            JudgeMissScan(clock);

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

        // ---- 几何（全部 1080p 基准） ----
        const float pfL = cx - 372.f * u, pfR = cx + 372.f * u;
        const float goldW = 11.f * u;
        const float innerL = pfL + goldW, innerR = pfR - goldW;
        const float innerW = innerR - innerL;
        // 键区总宽固定 708u：4K 每轨 177u（音符 168x63）；6K 每轨 118u（音符 110x52）
        const float laneW = (cols == 6) ? (118.f * u) : (177.f * u);
        const float lane0 = innerL + 7.f * u;
        const float noteW = (cols == 6) ? (110.f * u) : (168.f * u);
        const float noteH = (cols == 6) ? (52.f * u) : (63.f * u);
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
                              ImGuiWindowFlags_NoSavedSettings;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin(mode6 ? "##6k_playfield" : "##4k_playfield", nullptr, fl);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // ---- 轨道底板 + 网格 + 金边 ----
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

        // ---- 音符临近程度（键块随之亮起；未开始不做） ----
        float laneNear[6] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
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
        for (int i = 0; i < 6; i++)
        {
            s_laneFlash[i] *= (float)pow(0.02, (1.0 / 60.0) * 8.0);   // 判定闪条快速衰减
            s_laneHitBg[i] *= (float)pow(0.02, (1.0 / 60.0) * 5.0);
            if (keyHit[i])
            {
                s_laneFlash[i] = std::max(s_laneFlash[i], 0.55f);
                s_laneHitBg[i] = std::max(s_laneHitBg[i], 0.85f);
            }
        }

        // ---- 判定底光（notehitbg：原图 1680x200 切成 4 段，6K 时切成 6 段） ----
        if (s_texHitBg)
        {
            float by0 = judgeY, by1 = judgeY + 88.6f * u;
            float segW = 708.f * u / (float)cols;
            for (int i = 0; i < cols; i++)
            {
                float a = 0.20f * laneNear[i] + s_laneHitBg[i];
                if (a <= 0.004f)
                    continue;
                if (a > 1.f) a = 1.f;
                float x0 = lane0 + segW * i;
                float x1 = x0 + segW;
                dl->AddImage(TRef(s_texHitBg), ImVec2(x0, by0), ImVec2(x1, by1),
                             ImVec2((i * (1680.f / 6.f) + 4.f) / 1680.f, 4.f / 200.f),
                             ImVec2(((i + 1) * (1680.f / 6.f) - 4.f) / 1680.f, 196.f / 200.f),
                             IM_COL32(255, 255, 255, (int)(a * 235.f)));
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
                if (n.hold > 0.f && eaten && n.lane >= 0 && n.lane < 6)
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
                if (yc < -noteH && !holding)
                    continue;
                if (dt > fallTime)
                    break;
                float lx = lane0 + laneW * n.lane + (laneW - noteW) * 0.5f;

                // 长按条（lnx6.png：只有 x=299..400 这一段是实体）
                if (n.hold > 0.f && s_texHold)
                {
                    float yHead = judgeY - (float)(n.time - clock) * pxPerSec;
                    float yTail = judgeY - (float)(n.time + n.hold - clock) * pxPerSec;
                    if (holding)
                        yHead = judgeY;              // 已按下：从判定线向上延续
                    float hw = noteW * 0.30f;
                    float hy0 = std::max(std::min(yHead, yTail), 0.f);
                    float hy1 = std::min(std::max(yHead, yTail), judgeY + noteH * 0.5f);
                    if (hy1 > hy0)
                        dl->AddImage(TRef(s_texHold),
                                     ImVec2(lx + noteW * 0.5f - hw * 0.5f, hy0),
                                     ImVec2(lx + noteW * 0.5f + hw * 0.5f, hy1),
                                     ImVec2(299.f / 700.f, 0.f), ImVec2(400.f / 700.f, 1.f));
                    // 长按期间保持轨道底光（Malody 长按手感）
                    if (holding)
                        s_laneHitBg[n.lane] = std::max(s_laneHitBg[n.lane], 0.45f);
                }

                if (eaten || missedNote)
                    continue;                       // 头部已判定：只保留长按条

                float yb = std::min(yc + noteH * 0.5f, judgeY + noteH * 0.5f);
                dl->AddImage(TRef(s_texNote), ImVec2(lx, yb - noteH), ImVec2(lx + noteW, yb),
                             ImVec2(10.f / 420.f, 0.f), ImVec2(410.f / 420.f, 200.f / 400.f));
            }
        }

        // ---- 判定线（noteinex.png：整条横跨轨道，4 段虚线拼接） ----
        if (s_texLine)
            dl->AddImage(TRef(s_texLine), ImVec2(pfL, judgeY - 9.5f * u),
                         ImVec2(pfR, judgeY + 9.5f * u),
                         ImVec2(10.f / 1680.f, 0.f), ImVec2(1669.f / 1680.f, 1.f),
                         IM_COL32(255, 255, 255, 235));
        else
            dl->AddRectFilled(ImVec2(pfL, judgeY - 2.f * u), ImVec2(pfR, judgeY + 2.f * u),
                              IM_COL32(255, 205, 153, 220));

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
            // 4K：内侧两键用 notepress2 ／ 6K：六个键统一用 notepress1
            void* tex = (cols == 4 && (i == 1 || i == 2)) ? s_texPress2 : s_texPress;
            if (tex)
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
            const float kFxStep = kFxDur / 9.f;
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
                        if (frame > 8) frame = 8;
                        void* tex = s_texHits[frame];
                        if (tex)
                        {
                            float size = 208.f * u * fx.scale * (0.90f + 0.24f * (frame / 8.f));
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
                DrawDigitsCenter(dl, s_texCombo, ImVec2(cx, 298.f * u), cb, 45.f * u,
                                 IM_COL32(255, 255, 255, 240));
            }
            int accI = (int)(acc * 10000.0 + 0.5);
            char accBuf[24];
            snprintf(accBuf, sizeof(accBuf), "%d.%02d%%", accI / 100, accI % 100);
            if (s_texAcc[0])
                DrawDigitsCenter(dl, s_texAcc, ImVec2(cx, 369.f * u), accBuf, 30.f * u,
                                 IM_COL32(255, 255, 255, 245));
        }

        // ---- 中央判定提示（Malody judge 模块：m5judgerrd* 五色变体 + 文字）----
        //   0=MARVELOUS(绿) 1=PERFECT(黄) 2=GOOD(橙) 3=MISS(红)
        //   动画仿 rrdv52.lua：命中瞬间 105% → 73.5% 收缩 + 渐隐
        if (inPlay && s_judgePop.load(std::memory_order_relaxed) && s_texJudgePop[0])
        {
            int kind = s_jdLastKind.load(std::memory_order_relaxed);
            double t0 = s_jdLastTime.load(std::memory_order_relaxed);
            double age = clock - t0;
            const double kPopDur = 0.55;
            if (kind >= 0 && kind <= 3 && age >= 0.0 && age < kPopDur)
            {
                const int imgIdx[4] = { 0, 1, 2, 4 };   // a=绿 b=黄 c=橙 e=红（d 备用）
                void* tex = s_texJudgePop[imgIdx[kind]];
                float k = (float)(age / kPopDur);
                float ease = 1.f - (1.f - k) * (1.f - k);
                float scale = 1.05f + (0.735f - 1.05f) * ease;      // 105% → 73.5%
                float a = 1.f - k * k;
                if (a < 0.f) a = 0.f;
                int alpha = (int)(255.f * a);
                float w = 474.f * u * scale * 0.50f;
                float h = 512.f * u * scale * 0.50f;
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

    static void DrawKeyBinder(const char* pid, bool mode6)
    {
        const int n = mode6 ? 6 : 4;
        std::atomic<int>* live = mode6 ? s_vk6 : s_vk4;
        const int* defs = mode6 ? kVK6Def : kVK4Def;
        int* draft = mode6 ? s_draft6 : s_draft4;
        bool& editing = mode6 ? s_keyEdit6 : s_keyEdit4;
        int& cap = mode6 ? s_keyCap6 : s_keyCap4;
        bool& inited = mode6 ? s_keyInit6 : s_keyInit4;
        if (!inited)
        {
            for (int i = 0; i < n; i++)
                draft[i] = live[i].load(std::memory_order_relaxed);
            inited = true;
        }

        ImGui::PushID(pid);
        if (!editing)
        {
            ImGui::TextUnformatted("\xe9\x94\xae\xe4\xbd\x8d");            // \u952e\u4f4d
            ImGui::SameLine();
            ImGui::SetCursorPosX(90.f);
            if (ImGui::Button("\xe4\xbf\xae\xe6\x94\xb9##edit", ImVec2(56, 24)))       // \u4fee\u6539
            {
                for (int i = 0; i < n; i++)
                    draft[i] = live[i].load(std::memory_order_relaxed);
                editing = true;
                cap = -1;
            }
            ImGui::SameLine();
            if (ImGui::Button("\xe6\x81\xa2\xe5\xa4\x8d\xe9\xbb\x98\xe8\xae\xa4##def", ImVec2(80, 24)))  // \u6062\u590d\u9ed8\u8ba4
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
            ImGui::TextUnformatted("\xe9\x94\xae\xe4\xbd\x8d");
            ImGui::SameLine();
            ImGui::SetCursorPosX(90.f);
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

            ImGui::SetCursorPosX(90.f);
            if (dupAny)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.20f, 0.55f));
            if (ImGui::Button("\xe7\xa1\xae\xe5\xae\x9a##ok", ImVec2(70, 24)) && !dupAny)   // \u786e\u5b9a
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
                Log::Printf("[4K] key bind %s -> %s", mode6 ? "6K" : "4K", cur);
            }
            if (dupAny)
                ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("\xe5\x8f\x96\xe6\xb6\x88##cancel", ImVec2(70, 24)))          // \u53d6\u6d88
            {
                editing = false;
                cap = -1;
            }
            ImGui::SameLine();
            if (dupAny)
                ImGui::TextDisabled("\xe9\x94\xae\xe4\xbd\x8d\xe9\x87\x8d\xe5\xa4\x8d");   // \u952e\u4f4d\u91cd\u590d
            else if (cap >= 0)
                ImGui::TextDisabled("\xe8\xaf\xb7\xe6\x8c\x89\xe6\x96\xb0\xe9\x94\xae..."); // \u8bf7\u6309\u65b0\u952e...
            else
                ImGui::TextDisabled("\xe7\x82\xb9\xe6\xa7\xbd\u4f4d\u540e\u6309\u65b0\u952e");  // \u70b9\u69fd\u4f4d\u540e\u6309\u65b0\u952e
        }
        ImGui::PopID();
    }

    // ---------------- 主窗口设置页 ----------------
    static void BeginCard4K(const char* id, float height)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.f, 1.f, 1.f, 0.045f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
        ImGui::BeginChild(id, ImVec2(-1, height), ImGuiChildFlags_None);
    }
    static void EndCard4K()
    {
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    static bool MiniToggle(const char* id, bool* v)
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

    void DrawSettingsPage()
    {
        BeginCard4K("##card4k_main", 92.f);
        {
            float rowY = ImGui::GetCursorPosY();
            ImGui::PushFont(nullptr, 19.f);
            ImGui::TextUnformatted("4K \xe4\xb8\x8b\xe5\x9d\xa0\xe8\xb0\xb1\xe9\x9d\xa2");
            ImGui::PopFont();
            bool en = s_enabled.load(std::memory_order_relaxed);
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 62.f);
            ImGui::SetCursorPosY(rowY - 1.f);
            if (MiniToggle("##4k_en", &en))
            {
                s_enabled.store(en, std::memory_order_relaxed);
                if (en)
                    s_en6k.store(false, std::memory_order_relaxed);   // 两模式互斥
            }

            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 166, 182, 255));
            ImGui::TextWrapped("DFJK \xe8\xaf\xbb\xe8\xb0\xb1 \xc2\xb7 \xe5\xae\x9e\xe9\x99\x85\xe4\xbb\x8d\xe5\x9c\xa8\xe7\x8e\xa9\xe5\x86\xb0\xe4\xb8\x8e\xe7\x81\xab \xc2\xb7 Malody Rurudo 4K \xe7\x9a\xae\xe8\x82\xa4");
            ImGui::PopStyleColor();
        }
        EndCard4K();
        ImGui::Spacing();

        BeginCard4K("##card4k_set", 244.f);
        {
            int spd = s_speed.load(std::memory_order_relaxed);
            ImGui::TextUnformatted("\xe6\xb5\x81\xe9\x80\x9f");
            ImGui::SameLine();
            ImGui::SetCursorPosX(90.f);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::SliderInt("##4kspd", &spd, 1, 12))
                s_speed.store(spd);

            int off = s_offsetMS.load(std::memory_order_relaxed);
            ImGui::TextUnformatted("\xe5\xbb\xb6\xe8\xbf\x9f");
            ImGui::SameLine();
            ImGui::SetCursorPosX(90.f);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::SliderInt("##4koff", &off, -400, 400, "%d ms"))
                s_offsetMS.store(off);

            int op = s_opacity.load(std::memory_order_relaxed);
            ImGui::TextUnformatted("\xe5\xba\x95\xe6\x9d\xbf");
            ImGui::SameLine();
            ImGui::SetCursorPosX(90.f);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::SliderInt("##4kop", &op, 0, 255))
                s_opacity.store(op);

            ImGui::Spacing();
            DrawKeyBinder("##kb4", false);

            bool hfx = s_hitFx.load(std::memory_order_relaxed);
            if (MiniToggle("##4kfx", &hfx))
                s_hitFx.store(hfx);
            ImGui::SameLine();
            ImGui::TextUnformatted(" \xe6\x89\x93\xe5\x87\xbb\xe7\x89\xb9\xe6\x95\x88");
            bool jp = s_judgePop.load(std::memory_order_relaxed);
            if (MiniToggle("##4kjp", &jp))
                s_judgePop.store(jp);
            ImGui::SameLine();
            ImGui::TextUnformatted(" \xe5\x88\xa4\xe5\xae\x9a\xe6\x8f\x90\xe7\xa4\xba");
            {
                int combo = s_jdCombo.load(std::memory_order_relaxed);
                int total = s_jdTotal.load(std::memory_order_relaxed);
                double weight = s_jdWeight.load(std::memory_order_relaxed);
                double acc = total > 0 ? weight / (double)total : 1.0;
                ImGui::TextDisabled("\xe8\xb0\xb1\xe9\x9d\xa2 %d \xe9\x9f\xb3\xe7\xac\xa6 \xc2\xb7 Lv.%d \xc2\xb7 Acc %.2f%% \xc2\xb7 \xe8\xbf\x9e\xe5\x87\xbb %d (max %d)",
                                    s_noteCount.load(std::memory_order_relaxed),
                                    s_level.load(std::memory_order_relaxed),
                                    acc * 100.0, combo,
                                    s_jdMaxCombo.load(std::memory_order_relaxed));
            }
            char diag[192];
            GetDiag(diag, sizeof(diag));
            if (diag[0])
                ImGui::TextDisabled("%s", diag);
        }
        EndCard4K();

        ImGui::Spacing();
        BeginCard4K("##card4k_mini", 152.f);
        {
            bool mo = s_miniOn.load(std::memory_order_relaxed);
            if (MiniToggle("##4kmini", &mo))
                s_miniOn.store(mo);
            ImGui::SameLine();
            ImGui::TextUnformatted(" \xe6\xb8\xb8\xe6\x88\x8f\xe7\x94\xbb\xe9\x9d\xa2\xe5\xb0\x8f\xe7\xaa\x97\xef\xbc\x88\xe5\xb7\xa6\xe4\xb8\x8b\xe8\xa7\x92\xef\xbc\x89");

            int mx = s_miniX.load(std::memory_order_relaxed);
            int my = s_miniY.load(std::memory_order_relaxed);
            const float kItemSp = ImGui::GetStyle().ItemSpacing.x;
            ImGui::TextUnformatted("\xe4\xbd\x8d\xe7\xbd\xae");
            ImGui::SameLine();
            ImGui::SetCursorPosX(90.f);
            float halfW = (ImGui::GetContentRegionAvail().x - kItemSp) * 0.5f;
            if (halfW < 70.f) halfW = 70.f;
            ImGui::SetNextItemWidth(halfW);
            if (ImGui::SliderInt("##4kminiX", &mx, -400, 2000, "X %d"))
                s_miniX.store(mx);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(halfW);
            if (ImGui::SliderInt("##4kminiY", &my, -400, 1400, "Y %d"))
                s_miniY.store(my);

            int mw = s_miniW.load(std::memory_order_relaxed);
            int mh = s_miniH.load(std::memory_order_relaxed);
            ImGui::TextUnformatted("\xe5\xa4\xa7\xe5\xb0\x8f");
            ImGui::SameLine();
            ImGui::SetCursorPosX(90.f);
            const float kBtnW = 52.f;
            float halfW2 = (ImGui::GetContentRegionAvail().x - kBtnW - kItemSp * 2.f) * 0.5f;
            if (halfW2 < 70.f) halfW2 = 70.f;
            ImGui::SetNextItemWidth(halfW2);
            if (ImGui::SliderInt("##4kminiW", &mw, 80, 1600, "W %d"))
                s_miniW.store(mw);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(halfW2);
            if (ImGui::SliderInt("##4kminiH", &mh, 45, 1000, "H %d"))
                s_miniH.store(mh);
            ImGui::SameLine();
            if (ImGui::Button("\xe9\x87\x8d\xe7\xbd\xae##4kminiR", ImVec2(kBtnW, 0)))
            {
                s_miniX.store(16);
                s_miniY.store(848);
                s_miniW.store(384);
                s_miniH.store(216);
            }
        }
        EndCard4K();
    }

    // ============================================================
    // 6K 模式页（键位 S D F J K L）
    //   手感设计（Malody / Etterna 6K 写谱惯例）：
    //     · 连打一律双手交替，外侧 S/L 只在慢速重音出现（小指/无名指
    //       打不了连打，这是 6K 与 4K 最大的区别）
    //     · 连打用 6K 专属 24 音双螺旋循环（交叉楼梯，每键 4 次，均匀），
    //       长流每 24 音相位翻转一次，不呆板也不偏向某几个键
    //     · 稀疏音按谱面几何：逆时针=左手（S D F）/ 顺时针=右手（J K L）
    //     · 大回转落外侧 S/L 当重音，中回转落中指 D/K
    // ============================================================
    void DrawSettings6KPage()
    {
        BeginCard4K("##card6k_main", 92.f);
        {
            float rowY = ImGui::GetCursorPosY();
            ImGui::PushFont(nullptr, 19.f);
            ImGui::TextUnformatted("6K \xe4\xb8\x8b\xe5\x9d\xa0\xe8\xb0\xb1\xe9\x9d\xa2");   // 6K 下坠谱面
            ImGui::PopFont();
            bool en = s_en6k.load(std::memory_order_relaxed);
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 62.f);
            ImGui::SetCursorPosY(rowY - 1.f);
            if (MiniToggle("##6k_en", &en))
            {
                s_en6k.store(en, std::memory_order_relaxed);
                if (en)
                    s_enabled.store(false, std::memory_order_relaxed);   // 两模式互斥
            }
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 166, 182, 255));
            ImGui::TextWrapped("SDFJKL \xe8\xaf\xbb\xe8\xb0\xa1 \xc2\xb7 S/D/F \xe5\xb7\xa6\xe6\x89\x8b \xc2\xb7 J/K/L \xe5\x8f\xb3\xe6\x89\x8b \xc2\xb7 \xe6\xa0\x87\xe5\x87\x86 6K \xe5\xae\xbd\xe8\xbd\xa8\xe9\x81\x93");
            ImGui::PopStyleColor();
        }
        EndCard4K();
        ImGui::Spacing();

        BeginCard4K("##card6k_set", 118.f);
        {
            int spd = s_speed6k.load(std::memory_order_relaxed);
            ImGui::TextUnformatted("\xe6\xb5\x81\xe9\x80\x9f");
            ImGui::SameLine();
            ImGui::SetCursorPosX(90.f);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::SliderInt("##6kspd", &spd, 1, 12))
                s_speed6k.store(spd);

            int off = s_offsetMS6k.load(std::memory_order_relaxed);
            ImGui::TextUnformatted("\xe5\xbb\xb6\xe8\xbf\x9f");
            ImGui::SameLine();
            ImGui::SetCursorPosX(90.f);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::SliderInt("##6koff", &off, -400, 400, "%d ms"))
                s_offsetMS6k.store(off);

            ImGui::Spacing();
            DrawKeyBinder("##kb6", true);

        }
        EndCard4K();
        ImGui::Spacing();

        BeginCard4K("##card6k_share", 132.f);
        {
            int op = s_opacity.load(std::memory_order_relaxed);
            ImGui::TextUnformatted("\xe5\xba\x95\xe6\x9d\xbf");
            ImGui::SameLine();
            ImGui::SetCursorPosX(90.f);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::SliderInt("##6kop", &op, 0, 255))
                s_opacity.store(op);

            bool hfx = s_hitFx.load(std::memory_order_relaxed);
            if (MiniToggle("##6kfx", &hfx))
                s_hitFx.store(hfx);
            ImGui::SameLine();
            ImGui::TextUnformatted(" \xe6\x89\x93\xe5\x87\xbb\xe7\x89\xb9\xe6\x95\x88");
            bool jp = s_judgePop.load(std::memory_order_relaxed);
            if (MiniToggle("##6kjp", &jp))
                s_judgePop.store(jp);
            ImGui::SameLine();
            ImGui::TextUnformatted(" \xe5\x88\xa4\xe5\xae\x9a\xe6\x8f\x90\xe7\xa4\xba");
            bool mo = s_miniOn.load(std::memory_order_relaxed);
            if (MiniToggle("##6kmini", &mo))
                s_miniOn.store(mo);
            ImGui::SameLine();
            ImGui::TextUnformatted(" \xe5\xb7\xa6\xe4\xb8\x8b\xe8\xa7\x92\xe5\xb0\x8f\xe7\xaa\x97");

            int combo = s_jdCombo.load(std::memory_order_relaxed);
            int total = s_jdTotal.load(std::memory_order_relaxed);
            double weight = s_jdWeight.load(std::memory_order_relaxed);
            double acc = total > 0 ? weight / (double)total : 1.0;
            ImGui::TextDisabled("\xe8\xb0\xb1\xe9\x9d\xa2 %d \xe9\x9f\xb3\xe7\xac\xa6 \xc2\xb7 Lv.%d \xc2\xb7 Acc %.2f%% \xc2\xb7 \xe8\xbf\x9e\xe5\x87\xbb %d (max %d)",
                                s_noteCount.load(std::memory_order_relaxed),
                                s_level.load(std::memory_order_relaxed),
                                acc * 100.0, combo,
                                s_jdMaxCombo.load(std::memory_order_relaxed));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 155, 170, 255));
            ImGui::TextWrapped("\xe5\xb0\x8f\xe7\xaa\x97\xe4\xbd\x8d\xe7\xbd\xae/\xe5\xa4\xa7\xe5\xb0\x8f\xe5\x9c\xa8 4K \xe9\xa1\xb5\xe8\xb0\x83\xe6\x95\xb4\xef\xbc\x88\xe4\xb8\xa4\xe6\xa8\xa1\xe5\xbc\x8f\xe5\x85\xb1\xe7\x94\xa8\xef\xbc\x89");
            ImGui::PopStyleColor();
        }
        EndCard4K();
    }
}
