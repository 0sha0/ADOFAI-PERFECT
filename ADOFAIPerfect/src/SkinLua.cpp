// ============================================================
// SkinLua.cpp - Malody MSP skin Lua sandbox
//
// Reverse-engineering evidence (MalodyV GameAssembly.dll, IL2CPP):
//   * SkinModuleBase.get_X (RVA 0x59BE20) reads anchoredPosition3D.x and divides
//     by wzg([rcx+0x38]); set_X (0x59C630) writes anchoredPosition3D.x = v*wzg.
//     Awake (0x59B4DD-0x59B546) sets wzg = parentRect.height/1080.0f
//     (constant VA 0x182274DB0 == 1080.0f) => script X/Y/Width/Height units are
//     design pixels on a 1080-tall canvas == RoleMod.dx with dxu=1.
//   * ApiGame.SceneScale (RVA 0x332110) = FieldMeta("SceneScale") = Meta.Key.scale.
//     Mango.lua X = -840*ss*sw reproduces info.asm dx=-295/+295/300/612.
//   * ApiGame.Time/AudioLength/StartTime are milliseconds; tween start/finish are
//     absolute song ms (EmoCosine start=-3000).
//   * Tween table: start/finish/from/to/ease/repeats/repeatType/delay/custom.
//     ease 0=In 1=Linear 2=Out 3=InOut (same as MspEase in ChartCore).
//   * Module:GetBool/GetNumber/GetString(name) resolve to skin modules with
//     usage=98 (ScriptValue) whose meta.desc equals name, returning
//     ModuleParamValue {1 valF, 2 valS, 3 valB}. Verified on Mango info.asm:
//     desc "offset switch" modules carry valB=1 / valS="1500".
//   * Module:Find of an unknown name returns null in Malody and the script dies on
//     the next call (Cynosure references score/acc/judge1/... absent from its own
//     info.asm). The sandbox returns a writable virtual module instead so the rest
//     of the script still runs.
//   * Clone(mod,name) / Shadow(mod,ms) create persistent/temporary instances;
//     Play() starts the module built-in ModuleAnimation (field 20).
// ============================================================
#include "SkinLua.h"
#include "Log.h"

#include <lua.hpp>
#include <sol.hpp>

#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SkinLua
{
namespace
{
    constexpr int P_X = 0, P_Y = 1, P_W = 2, P_H = 3, P_A = 4, P_ROT = 5;
    constexpr int P_SX = 8, P_SY = 9;          // DoScale（基准尺寸的倍率）

    struct Anim
    {
        int    prop = P_X;
        double start = 0, finish = 1;
        float  from = 0, to = 0;
        int    ease = 1;
        int    repeatType = 0;
        double repeats = 0;
        double delay = 0;
    };

    struct Inst
    {
        int  src = -1;                  // g_mods 下标；-1 = 虚拟/无源
        bool virt = false;              // Find() 未命中的虚拟模块
        bool extra = false;             // clone / shadow（额外绘制实例）
        bool shadow = false;
        std::string desc;
        double bornMs = 0, lifeMs = 0;
        bool dead = false;
        bool playing = false;
        double playT0 = 0;
        bool visLogged = false;         // 证据日志：该 Clone/Shadow 首次可见只记一次

        bool oX = false, oY = false, oW = false, oH = false, oA = false, oRZ = false,
             oFill = false, oText = false, oColor = false, oSX = false, oSY = false;
        float X = 0, Y = 0, W = 0, H = 0, RZ = 0, fill = 0, SX = 1.f, SY = 1.f;
        int   A = 100;
        int   cr = 255, cg = 255, cb = 255;
        std::string text;
        std::vector<Anim> anims;
    };

    static const SkinMsp::RoleMod* g_mods = nullptr;
    static int    g_modN = 0;
    static Env    g_env{};
    static Live   g_live{};
    static bool   g_ready = false;
    static std::string g_dir, g_err;
    static std::string g_audioDir;          // Audio:* 只允许读取该目录内的 wav
    static std::unique_ptr<sol::state> g_L;
    static std::vector<std::unique_ptr<Inst>> g_insts;
    static std::unordered_map<int, Inst*> g_byMod;
    static std::unordered_map<std::string, Inst*> g_byName;
    static double g_nowMs = 0.0;

    struct Event { int type = 0; int kind = 0; int lane = 1; int noteType = 1; double off = 0; };
    static std::mutex g_qmx;
    static std::vector<Event> g_hits, g_inputs;
    static Event g_cur;
    static std::unordered_map<std::string, double> s_fieldMeta;   // SetFieldMeta overrides

    static float Ui() { return (g_env.windowH > 1.f) ? g_env.windowH / 1080.f : 1.f; }

    // Malody Audio:* 的最小实现：皮肤脚本用 Audio:Load(name) 拿句柄、Audio:Play(handle, vol)
    // 播放判定音（Rurudo rrdv52.lua L43/L52/L526、Init L185-189）。沙箱只允许皮肤目录内的
    // 纯文件名（拒绝路径穿越）；播放走 winmm!PlaySoundA（异步、单实例，新音效顶掉旧的，
    // 与 Malody 行为足够接近；音量参数 Malody 用百分比，PlaySound 不支持，忽略）。
    typedef MCIERROR(WINAPI* PFN_MciSendStringA)(LPCSTR, LPSTR, UINT, HWND);
    static PFN_MciSendStringA Mci()
    {
        static PFN_MciSendStringA s_mci = nullptr;
        static bool s_tried = false;
        if (!s_tried)
        {
            s_tried = true;
            if (HMODULE h = LoadLibraryA("winmm.dll"))
                s_mci = (PFN_MciSendStringA)GetProcAddress(h, "mciSendStringA");
        }
        return s_mci;
    }
    static void StopSkinSound()
    {
        if (PFN_MciSendStringA mci = Mci())
            mci("close adof_skin_snd", nullptr, 0, nullptr);
        typedef BOOL(WINAPI* PFN_PlaySoundA)(LPCSTR, HMODULE, DWORD);
        static PFN_PlaySoundA s_play = nullptr;
        static bool s_tried = false;
        if (!s_tried)
        {
            s_tried = true;
            if (HMODULE h = LoadLibraryA("winmm.dll"))
                s_play = (PFN_PlaySoundA)GetProcAddress(h, "PlaySoundA");
        }
        if (s_play) s_play(nullptr, nullptr, 0);
    }
    static void PlaySkinSound(const std::string& file, int volPct = 100)
    {
        if (file.empty() || g_audioDir.empty()) return;
        if (file.find("..") != std::string::npos) return;
        if (file.find(':') != std::string::npos || file.find('\\') != std::string::npos || file.find('/') != std::string::npos) return;
        char path[MAX_PATH * 2 + 64];
        snprintf(path, sizeof(path), "%s\\%s", g_audioDir.c_str(), file.c_str());
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return;
        {
            static int s_playLog = 0;
            if (s_playLog < 24) { s_playLog++; Log::Printf("[skinlua] Audio.Play '%s' vol=%d", file.c_str(), volPct); }
        }
        { static const char* m = getenv("ADOFAI_PERFECT_MUTE_SKIN"); if (m && m[0] == '1') return; }
        // .wav 走 PlaySound；.mp3/.ogg 等走 MCI（Malody 皮肤如 Murasame 的 ciallo.mp3）
        std::string ext;
        if (const char* dot = strrchr(file.c_str(), '.')) ext = dot;
        if (_stricmp(ext.c_str(), ".wav") != 0)
        {
            if (PFN_MciSendStringA mci = Mci())
            {
                char cmd[MAX_PATH * 2 + 128];
                mci("close adof_skin_snd", nullptr, 0, nullptr);
                snprintf(cmd, sizeof(cmd), "open \"%s\" type mpegvideo alias adof_skin_snd", path);
                if (mci(cmd, nullptr, 0, nullptr) == 0)
                {
                    if (volPct >= 0)
                    {
                        int v = volPct > 100 ? 1000 : volPct * 10;
                        snprintf(cmd, sizeof(cmd), "setaudio adof_skin_snd volume to %d", v);
                        mci(cmd, nullptr, 0, nullptr);
                    }
                    mci("play adof_skin_snd", nullptr, 0, nullptr);
                    return;
                }
            }
        }
        typedef BOOL(WINAPI* PFN_PlaySoundA)(LPCSTR, HMODULE, DWORD);
        static PFN_PlaySoundA s_play = nullptr;
        static bool s_tried = false;
        if (!s_tried)
        {
            s_tried = true;
            if (HMODULE h = LoadLibraryA("winmm.dll"))
                s_play = (PFN_PlaySoundA)GetProcAddress(h, "PlaySoundA");
        }
        if (s_play) s_play(path, nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    }

    // ============================================================
    // 皮肤目录文件读取（Malody Game:ReadFile / Game:ReadBytes 的实现）
    //   逆向证据：MurasameSkinLuaFallback（Lua 5.3 字节码）在 load 阶段调用
    //     bytes = Game:ReadBytes("MurasameSkinLuaFallback")
    //     local chunk = load(bytes:ToArray(), nil, nil, _G)
    //   以及每个模块脚本自己调用 Game:ReadFile("config.ini") 解析皮肤配置。
    //   只允许读皮肤目录内文件，拒绝 ".."、盘符与绝对路径。
    // ============================================================
    static bool SkinPath(const std::string& name, std::string* out)
    {
        if (name.empty() || g_dir.empty()) return false;
        if (name.find("..") != std::string::npos) return false;
        if (name.find(':') != std::string::npos) return false;
        if (name[0] == '\\' || name[0] == '/') return false;
        char p[MAX_PATH * 2 + 64];
        snprintf(p, sizeof(p), "%s\\%s", g_dir.c_str(), name.c_str());
        if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) return false;
        if (out) *out = p;
        return true;
    }
    static bool ReadSkinFile(const std::string& name, std::string& data)
    {
        data.clear();
        std::string path;
        if (!SkinPath(name, &path) && !SkinPath(name + ".lua", &path))
            return false;
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz < 0 || sz > (64 << 20)) { fclose(f); return false; }
        data.resize((size_t)sz);
        if (sz > 0 && fread(&data[0], 1, (size_t)sz, f) != (size_t)sz) { fclose(f); data.clear(); return false; }
        fclose(f);
        return true;
    }
    // 脚本使用轨迹（证据日志，每个皮肤只记一次/限量）：
    //   Read/Find/Use/Drive 四类，可直接证明"哪张图在什么时候被脚本真正用了"。
    static std::unordered_map<std::string, bool> g_readLogged;
    static std::unordered_map<std::string, bool> g_findLogged;
    static std::unordered_map<std::string, int>  g_useCount;
    static std::unordered_map<int, bool>         g_driveLogged;
    static int g_visLogN = 0;
    static void LogUseOnce(const char* how, const std::string& key, int limit)
    {
        int& n = g_useCount[std::string(how) + ":" + key];
        if (n >= limit) return;
        n++;
        // combo/t 是"脚本在什么时候真的用了这张图"的直接证据（如连击触发的 ciallo 大图）
        Log::Printf("[skinlua] %s '%s' (#%d) combo=%d t=%.0fms", how, key.c_str(), n, g_live.combo, g_nowMs);
    }

    struct Bytes { std::string data; };

    static float ToF(const sol::object& o)
    {
        if (o.is<double>())   return (float)o.as<double>();
        if (o.is<int64_t>())  return (float)o.as<int64_t>();
        if (o.is<bool>())     return o.as<bool>() ? 1.f : 0.f;
        if (o.is<const char*>()) return (float)atof(o.as<const char*>());
        if (o.is<std::string>()) return (float)atof(o.as<std::string>().c_str());
        return 0.f;
    }
    static std::string ToStr(const sol::object& o)
    {
        if (o.is<std::string>()) return o.as<std::string>();
        if (o.is<const char*>()) return o.as<const char*>();
        if (o.is<int64_t>()) { char b[32]; snprintf(b, sizeof(b), "%lld", (long long)o.as<int64_t>()); return b; }
        if (o.is<double>())
        {
            double d = o.as<double>();
            char b[32];
            if (d == (double)(long long)d) snprintf(b, sizeof(b), "%lld", (long long)d);
            else snprintf(b, sizeof(b), "%g", d);
            return b;
        }
        if (o.is<bool>()) return o.as<bool>() ? "true" : "false";
        return std::string();
    }
    static float TblF(const sol::table& t, const char* k, float dflt)
    {
        sol::object o = t[k];
        if (!o.valid() || o == sol::nil) return dflt;
        return ToF(o);
    }
    static bool TblHas(const sol::table& t, const char* k)
    {
        sol::object o = t[k];
        return o.valid() && o != sol::nil;
    }

    static Inst* NewInst()
    {
        g_insts.emplace_back(new Inst());
        return g_insts.back().get();
    }
    static Inst* FindOrCreateReal(int idx)
    {
        auto it = g_byMod.find(idx);
        if (it != g_byMod.end()) return it->second;
        Inst* in = NewInst();
        in->src = idx;
        g_byMod[idx] = in;
        return in;
    }
    static Inst* FindByName(const char* name)
    {
        std::string k = name ? name : "";
        auto it = g_byName.find(k);
        if (it != g_byName.end()) return it->second;
        Inst* in = NewInst();
        in->virt = true;
        in->desc = k;
        g_byName[k] = in;
        return in;
    }
    // ---------- 基础值（引擎静态值，设计像素）----------
    static float BaseX(const Inst* in)
    {
        if (in->src < 0) return 0.f;
        const SkinMsp::RoleMod& m = g_mods[in->src];
        if (m.dxu == 1) return m.dx;
        if (m.dxu == 2) return m.dx / Ui();
        return m.dx * 0.01f * g_env.windowW / Ui();
    }
    static float BaseY(const Inst* in)
    {
        if (in->src < 0) return 0.f;
        const SkinMsp::RoleMod& m = g_mods[in->src];
        if (m.dyu == 1) return m.dy;
        if (m.dyu == 2) return m.dy / Ui();
        return m.dy * 0.01f * g_env.windowH / Ui();
    }
    static float BaseW(const Inst* in)
    {
        if (in->src < 0) return 0.f;
        const SkinMsp::RoleMod& m = g_mods[in->src];
        if (m.wu == 1) return m.w;
        if (m.wu == 2) return m.w / Ui();
        return m.w * 0.01f * g_env.windowW / Ui();
    }
    static float BaseH(const Inst* in)
    {
        if (in->src < 0) return 0.f;
        const SkinMsp::RoleMod& m = g_mods[in->src];
        if (m.hu == 1) return m.h;
        if (m.hu == 2) return m.h / Ui();
        return m.h * 0.01f * g_env.windowH / Ui();
    }

    static float Ease(int e, float t)
    {
        if (t < 0.f) t = 0.f;
        if (t > 1.f) t = 1.f;
        switch (e)
        {
        case 0: return t * t;
        case 2: return 1.f - (1.f - t) * (1.f - t);
        case 3: return (t < 0.5f) ? (2.f * t * t) : (1.f - 2.f * (1.f - t) * (1.f - t));
        default: return t;
        }
    }
    // start/finish 为绝对歌曲毫秒；repeats>0 时按 repeatType 循环（2=往返）。
    // delay 语义（逆向自 rrdv52.lua 的 DoRotate 用法）：带 repeats 时是"每轮之间的间隔"，
    // 一轮 = 动画时长 + delay，间隔期保持终值；而不是只把整个动画推迟一次。
    // 证据：m5logo DoRotate(300ms, repeats=∞, delay=2000ms) / cout DoRotate(1000ms, delay=2000ms)
    // —— 旧实现把 delay 当一次性前置偏移 + 每 300ms 转一圈，实测 M 图标转速明显过快。
    static float AnimAt(const Anim& a, double now)
    {
        const double dur = a.finish - a.start;
        if (dur <= 0.0001) return a.to;
        const double dly = (a.repeats > 0.0 && a.delay > 0.0) ? a.delay : 0.0;
        double t = now - a.start - (dly > 0.0 ? 0.0 : a.delay);
        if (t < 0.0) return a.from;
        float ph;
        if (a.repeats > 0.0)
        {
            if (a.repeatType == 2)
            {
                const double half = dur + dly;
                if (dly > 0.0)
                {
                    double u = fmod(t, half * 2.0);
                    if (u >= dur && u < half) return a.to;                      // 去程后停顿
                    if (u >= half && u < half + dur)                            // 回程
                        return a.from + (a.to - a.from) * Ease(a.ease, (float)(1.0 - (u - half) / dur));
                    if (u >= half + dur) return a.from;                         // 回程后停顿
                    ph = (float)(u / dur);
                }
                else
                {
                    double w = fmod(t / dur, 2.0);
                    ph = (w > 1.0) ? (float)(2.0 - w) : (float)w;
                }
            }
            else
            {
                double u = fmod(t, dur + dly);
                if (dly > 0.0 && u >= dur) return a.to;                         // 轮间停顿保持终值
                ph = (float)(u / dur);
            }
        }
        else
        {
            double u = t / dur;
            if (u > 1.0) u = 1.0;
            ph = (float)u;
        }
        return a.from + (a.to - a.from) * Ease(a.ease, ph);
    }

    // ---------- 文本（与 ChartCore 的 desc 规则一致）----------
    static std::string Subst(const char* in)
    {
        std::string out;
        for (const char* p = in; *p;)
        {
            if (*p != '{') { out += *p++; continue; }
            const char* e = strchr(p, '}');
            if (!e) break;
            std::string key(p + 1, e - p - 1);
            char rep[160] = { 0 };
            if (key == "title") snprintf(rep, sizeof(rep), "%s", (g_env.levelName && g_env.levelName[0]) ? g_env.levelName : "ADOFAI");
            else if (key == "bpm") snprintf(rep, sizeof(rep), "%.0f", g_env.bpm);
            else if (key == "ver") snprintf(rep, sizeof(rep), "lv.0");
            else if (key == "level") snprintf(rep, sizeof(rep), "0");
            else if (key == "player") snprintf(rep, sizeof(rep), "ADOFAI-PERFECT");
            else if (key == "judge") snprintf(rep, sizeof(rep), "%s", g_env.judgeName ? g_env.judgeName : "NORMAL");
            else if (key == "mod") snprintf(rep, sizeof(rep), "Normal");
            else if (key == "combo") snprintf(rep, sizeof(rep), "%d", g_live.combo);
            else if (key == "maxcombo") snprintf(rep, sizeof(rep), "%d", g_live.maxCombo);
            else if (key == "acc") snprintf(rep, sizeof(rep), "%.2f%%", g_live.acc * 100.0);
            else if (key == "best" || key == "marv") snprintf(rep, sizeof(rep), "%d", g_live.counts[0]);
            else if (key == "cool") snprintf(rep, sizeof(rep), "%d", g_live.counts[1]);
            else if (key == "good") snprintf(rep, sizeof(rep), "%d", g_live.counts[2]);
            else if (key == "miss") snprintf(rep, sizeof(rep), "%d", g_live.counts[3]);
            else if (key == "score") snprintf(rep, sizeof(rep), "%07d", (int)g_live.score);
            else if (key == "hp") snprintf(rep, sizeof(rep), "%.2f%%", g_live.hp * 100.f);
            else if (key == "hp_int") snprintf(rep, sizeof(rep), "%d", (int)(g_live.hp * 100.f));
            else if (key == "progress") snprintf(rep, sizeof(rep), "%.2f%%", g_live.progress * 100.f);
            else if (key == "kps") snprintf(rep, sizeof(rep), "%d", g_live.kps);
            else if (key == "kpsmax") snprintf(rep, sizeof(rep), "%d", g_live.kpsMax);
            else if (key == "time" || key == "audio")
                snprintf(rep, sizeof(rep), "%02d:%02d", (int)(g_live.songMs / 1000.0) / 60, (int)(g_live.songMs / 1000.0) % 60);
            else if (key == "total" || key == "length")
                snprintf(rep, sizeof(rep), "%02d:%02d", (int)(g_env.audioLengthMs / 1000.0) / 60, (int)(g_env.audioLengthMs / 1000.0) % 60);
            else return std::string();   // 未知占位符：视为无该绑定值，走 desc 规则
            out += rep;
            p = e + 1;
        }
        return out;
    }

    static std::string ComputeText(const Inst* in)
    {
        if (in->oText) return in->text;
        // 虚拟模块（Find 未命中）：返回 "0" 而不是空串。Malody 里这类名字通常由引擎
        // 提供数值（tonumber 后参与运算），空串会让 tonumber 得到 nil 并在下一行报
        // "attempt to compare nil with number"（实测 Cynosure Update 第 159 行）。
        if (in->src < 0) return std::string("0");
        const SkinMsp::RoleMod& m = g_mods[in->src];
        if (m.usage == 98) { if (m.valS[0]) return std::string(m.valS); char b[32]; snprintf(b, sizeof(b), "%g", m.valF); return b; }
        if (m.text[0] && strchr(m.text, '{'))
        {
            std::string s = Subst(m.text);
            if (!s.empty()) return s;
        }
        const char* d = m.desc[0] ? m.desc : (m.numBase[0] ? m.numBase : "");
        // Malody 引擎绑定的两个文本值：difficulty 名称与 mod 名称（Kalpa/Cynosure
        // 在 Init 里用它们推导偏移条宽度与速度倍率；返回空串会让脚本在下一行
        // 算术 nil 报错，整个 Init 中断）。
        if (strstr(d, "diff"))
        {
            const int lv = g_env.level;
            if (lv <= 0) return std::string("NORMAL");
            if (lv <= 12) return std::string("EASY");
            if (lv <= 16) return std::string("NORMAL");
            if (lv <= 20) return std::string("HARD");
            return std::string("HARD");
        }
        if (strstr(d, "mod")) return std::string("Normal");
        char buf[80] = { 0 };
        if (strstr(d, "acc") || strstr(d, "rate")) snprintf(buf, sizeof(buf), "%.2f%%", g_live.acc * 100.0);
        else if (strstr(d, "combo")) snprintf(buf, sizeof(buf), "%d", g_live.combo);
        else if (strstr(d, "score")) snprintf(buf, sizeof(buf), "%d", (int)g_live.score);
        else if (strstr(d, "hp")) snprintf(buf, sizeof(buf), "%d", (int)(g_live.hp * 100.f));
        else if (strstr(d, "bpm")) snprintf(buf, sizeof(buf), "%.2f", g_env.bpm);
        else if (strstr(d, "kps")) snprintf(buf, sizeof(buf), "%d", g_live.kps);
        else if (strstr(d, "timel") || strstr(d, "timer")) snprintf(buf, sizeof(buf), "%.2f", g_live.songMs / 1000.0);
        else if (m.text[0]) return std::string(m.text);
        return std::string(buf);
    }
    // ---------- 求值 ----------
    static void Compute(const Inst* in, double now, Patch* p, float* outAlpha)
    {
        float X = BaseX(in), Y = BaseY(in), W = BaseW(in), H = BaseH(in);
        float A = (in->src >= 0) ? (float)g_mods[in->src].alpha : 100.f;
        float R = (in->src >= 0) ? (float)g_mods[in->src].rotate : 0.f;
        float sx = 0.f, sy = 0.f, fill = -1.f;
        if (in->oX) X = in->X;
        if (in->oY) Y = in->Y;
        if (in->oW) W = in->W;
        if (in->oH) H = in->H;
        if (in->oA) A = (float)in->A;
        if (in->oRZ) R = in->RZ;
        if (in->oFill) fill = in->fill;
        for (const Anim& a : in->anims)
        {
            const float v = AnimAt(a, now);
            switch (a.prop)
            {
            case P_X: X = v; break;
            case P_Y: Y = v; break;
            case P_W: W = v; break;
            case P_H: H = v; break;
            case P_A: A = v; break;
            case P_ROT: R = v; break;
            case P_SX: sx = v; break;
            case P_SY: sy = v; break;
            default: break;
            }
        }
        if (sx > 0.f) W = BaseW(in) * sx;
        if (sy > 0.f) H = BaseH(in) * sy;
        if (in->oSX) W *= in->SX;
        if (in->oSY) H *= in->SY;
        p->x = true; p->xv = X;
        p->y = true; p->yv = Y;
        if (in->oW || in->oH || sx > 0.f || sy > 0.f || in->oSX || in->oSY) { p->w = true; p->wv = W; p->h = true; p->hv = H; }
        p->alpha = true; p->av = (int)A;
        p->rotate = true; p->rv = (int)R;
        if (fill >= 0.f) { p->fill = true; p->fillv = fill; }
        if (in->oColor) { p->color = true; p->cr = in->cr; p->cg = in->cg; p->cb = in->cb; }
        if (in->oText) p->text = in->text.c_str();
        if (outAlpha) *outAlpha = A;
    }

    static float GetProp(const Inst* in, int prop)
    {
        switch (prop)
        {
        case P_X: return in->oX ? in->X : BaseX(in);
        case P_Y: return in->oY ? in->Y : BaseY(in);
        case P_W: return in->oW ? in->W : BaseW(in);
        case P_H: return in->oH ? in->H : BaseH(in);
        case P_A: return in->oA ? (float)in->A : ((in->src >= 0) ? (float)g_mods[in->src].alpha : 100.f);
        case P_SX: return in->oSX ? in->SX : 1.f;
        case P_SY: return in->oSY ? in->SY : 1.f;
        default: return 0.f;
        }
    }
    static void SetProp(Inst* in, int prop, float v)
    {
        switch (prop)
        {
        case P_X: in->oX = true; in->X = v; break;
        case P_Y: in->oY = true; in->Y = v; break;
        case P_W: in->oW = true; in->W = v; break;
        case P_H: in->oH = true; in->H = v; break;
        case P_A: in->oA = true; in->A = (int)v; break;
        case P_ROT: in->oRZ = true; in->RZ = v; break;
        case P_SX: in->oSX = true; in->SX = v; break;
        case P_SY: in->oSY = true; in->SY = v; break;
        default: break;
        }
        if (prop == P_X || prop == P_Y || prop == P_W || prop == P_H || prop == P_A || prop == P_ROT ||
            prop == P_SX || prop == P_SY)
            in->anims.erase(std::remove_if(in->anims.begin(), in->anims.end(),
                [prop](const Anim& a) { return a.prop == prop; }), in->anims.end());
    }

    static void AddAnimProp(Inst* in, int prop, const sol::object& arg)
    {
        if (arg.is<double>() || arg.is<int64_t>() || arg.is<bool>()) { SetProp(in, prop, ToF(arg)); return; }
        if (!arg.is<sol::table>()) return;
        sol::table t = arg.as<sol::table>();
        Anim a;
        a.prop = prop;
        a.start = TblHas(t, "start") ? (double)ToF(t["start"]) : 0.0;
        a.finish = TblHas(t, "finish") ? (double)ToF(t["finish"]) : a.start + 1.0;
        a.from = TblF(t, "from", 0.f);
        a.to = TblF(t, "to", 0.f);
        a.ease = (int)TblF(t, "ease", 1.f);
        a.repeatType = (int)TblF(t, "repeatType", 0.f);
        a.repeats = TblHas(t, "repeats") ? (double)ToF(t["repeats"]) : 0.0;
        a.delay = TblHas(t, "delay") ? (double)ToF(t["delay"]) : 0.0;
        // 同一属性只保留最新一条：Malody 后写覆盖；Phigros/Kalpa 用两段 DoAlpha 接力
        in->anims.erase(std::remove_if(in->anims.begin(), in->anims.end(),
            [prop](const Anim& x) { return x.prop == prop; }), in->anims.end());
        in->anims.push_back(a);
        static int s_animLog = 0;
        if (s_animLog < 40 && in->src >= 0 && in->src < g_modN)
        {
            s_animLog++;
            Log::Printf("[skinlua] anim mod#%d desc='%s' prop=%d start=%.0f->%.0f from=%.1f to=%.1f",
                        in->src, g_mods[in->src].desc ? g_mods[in->src].desc : "?",
                        a.prop, a.start, a.finish, a.from, a.to);
        }
    }

    // ---------- Lua 绑定 ----------
    struct ModRef { Inst* p = nullptr; };

    static void BindTypes(sol::state& L)
    {
        L.new_usertype<ModRef>("MalodyModule", sol::no_constructor,
            "X", sol::property([](ModRef& r) { return GetProp(r.p, P_X); },
                               [](ModRef& r, sol::object v) { SetProp(r.p, P_X, ToF(v)); }),
            "Y", sol::property([](ModRef& r) { return GetProp(r.p, P_Y); },
                               [](ModRef& r, sol::object v) { SetProp(r.p, P_Y, ToF(v)); }),
            "Width", sol::property([](ModRef& r) { return GetProp(r.p, P_W); },
                               [](ModRef& r, sol::object v) { SetProp(r.p, P_W, ToF(v)); }),
            "Height", sol::property([](ModRef& r) { return GetProp(r.p, P_H); },
                               [](ModRef& r, sol::object v) { SetProp(r.p, P_H, ToF(v)); }),
            "Alpha", sol::property([](ModRef& r) { return GetProp(r.p, P_A); },
                               [](ModRef& r, sol::object v) { SetProp(r.p, P_A, ToF(v)); }),
            "Text", sol::property([](ModRef& r) { return ComputeText(r.p); },
                               [](ModRef& r, sol::object v) { r.p->oText = true; r.p->text = ToStr(v); }),
            "Value", sol::property([](ModRef& r) { return r.p->oFill ? r.p->fill * 100.f : 0.f; },
                               [](ModRef& r, sol::object v) { r.p->oFill = true; r.p->fill = ToF(v) / 100.f; }),
            "RotateX", sol::property([](ModRef&) { return 0.f; }, [](ModRef&, sol::object) {}),
            "RotateY", sol::property([](ModRef&) { return 0.f; }, [](ModRef&, sol::object) {}),
            "RotateZ", sol::property([](ModRef& r) { return GetProp(r.p, P_ROT); },
                               [](ModRef& r, sol::object v) { SetProp(r.p, P_ROT, ToF(v)); }),
            "Rotate", sol::property([](ModRef& r) { return GetProp(r.p, P_ROT); },
                               [](ModRef& r, sol::object v) { SetProp(r.p, P_ROT, ToF(v)); }),
            "ScaleX", sol::property([](ModRef& r) { return GetProp(r.p, P_SX); },
                               [](ModRef& r, sol::object v) { SetProp(r.p, P_SX, ToF(v)); }),
            "ScaleY", sol::property([](ModRef& r) { return GetProp(r.p, P_SY); },
                               [](ModRef& r, sol::object v) { SetProp(r.p, P_SY, ToF(v)); }),
            "DoMoveX", [](ModRef& r, sol::object t) { AddAnimProp(r.p, P_X, t); },
            "DoMoveY", [](ModRef& r, sol::object t) { AddAnimProp(r.p, P_Y, t); },
            "DoMove", [](ModRef& r, sol::object tx, sol::object ty)
            { AddAnimProp(r.p, P_X, tx); AddAnimProp(r.p, P_Y, ty); },
            "DoAlpha", [](ModRef& r, sol::object t) { AddAnimProp(r.p, P_A, t); },
            "DoWidth", [](ModRef& r, sol::object t) { AddAnimProp(r.p, P_W, t); },
            "DoHeight", [](ModRef& r, sol::object t) { AddAnimProp(r.p, P_H, t); },
            "DoResize", [](ModRef& r, sol::object tw, sol::object th)
            { AddAnimProp(r.p, P_W, tw); AddAnimProp(r.p, P_H, th); },
            "DoScale", [](ModRef& r, sol::object tx, sol::object ty)
            { AddAnimProp(r.p, P_SX, tx); AddAnimProp(r.p, P_SY, ty); },
            "DoScaleX", [](ModRef& r, sol::object t) { AddAnimProp(r.p, P_SX, t); },
            "DoScaleY", [](ModRef& r, sol::object t) { AddAnimProp(r.p, P_SY, t); },
            "DoRotate", [](ModRef& r, sol::object t) { AddAnimProp(r.p, P_ROT, t); },
            "DoFrame", [](ModRef&, sol::object) {},
            "SetColor", [](ModRef& r, sol::variadic_args va)
            {
                float c[3] = { 255.f, 255.f, 255.f };
                int n = 0;
                for (auto it = va.begin(); it != va.end() && n < 3; ++it, ++n) c[n] = ToF(*it);
                if (n == 1)
                {
                    sol::object o0 = va.get<sol::object>(0);
                    if (o0.is<sol::table>())
                    {
                        sol::table t = o0.as<sol::table>();
                        c[0] = TblF(t, "r", 255.f); c[1] = TblF(t, "g", 255.f); c[2] = TblF(t, "b", 255.f);
                    }
                }
                r.p->oColor = true;
                r.p->cr = (int)c[0]; r.p->cg = (int)c[1]; r.p->cb = (int)c[2];
            },
            "Play", [](ModRef& r) { r.p->playing = true; r.p->playT0 = g_nowMs; },
            "Stop", [](ModRef& r) { r.p->playing = false; r.p->anims.clear(); });
        // Game:ReadBytes 返回的字节数组（Murasame fallback：load(bytes:ToArray(), ...)）
        L.new_usertype<Bytes>("MalodyBytes", sol::no_constructor,
            "ToArray", [](Bytes& b) { return b.data; },
            "Length", [](Bytes& b) { return (int64_t)b.data.size(); },
            "__len", [](Bytes& b) { return (int64_t)b.data.size(); });
    }

    static bool CallFn(const char* name, bool required)
    {
        if (!g_L) return false;
        sol::protected_function fn = (*g_L)[name];
        if (required && !fn.valid())
        {
            g_err = std::string("missing function ") + name;
            return false;
        }
        if (!fn.valid()) return true;
        sol::protected_function_result r = fn();
        if (!r.valid())
        {
            sol::error e = r;
            // 限流：同一错误最多每 2 秒记一次（Update 每帧都会调，否则日志爆炸）
            static std::string s_last;
            static double s_lastLog = -1e9;
            static double s_lastSame = -1e9;
            static double s_now = 0;
            s_now += 1.0;   // 单调计数（不依赖时钟）
            const std::string msg = std::string(name) + ": " + e.what();
            if (msg != s_last || s_now - s_lastSame > 120.0)
            {
                Log::Printf("[skinlua] %s error: %s", name, e.what());
                s_last = msg;
                s_lastSame = s_now;
            }
            g_err = msg;
            return false;
        }
        return true;
    }

} // namespace
// ================= 公开 API =================
bool Active() { return g_ready; }
bool HasOnHit() { return g_ready && g_L && (*g_L)["OnHit"].valid(); }
const char* LastError() { return g_err.c_str(); }

void Unload()
{
    g_ready = false;
    g_L.reset();
    g_insts.clear();
    g_byMod.clear();
    g_byName.clear();
    g_mods = nullptr;
    g_modN = 0;
}

bool Load(const char* skinDir, const char* scriptFile,
          const SkinMsp::RoleMod* mods, int modCount, const Env& env)
{
    Unload();
    const char* noLua = getenv("ADOFAI_PERFECT_NOSKINLUA");
    if (noLua && noLua[0] == '1') { g_err = "disabled by env"; return false; }
    if (!skinDir || !scriptFile || !scriptFile[0] || !mods || modCount <= 0)
    {
        g_err = "no script";
        return false;
    }
    g_mods = mods; g_modN = modCount; g_env = env; g_dir = skinDir; g_audioDir = skinDir;

    char path[MAX_PATH * 2 + 128];
    snprintf(path, sizeof(path), "%s\\%s", skinDir, scriptFile);
    FILE* f = fopen(path, "rb");
    if (!f) { g_err = std::string("cannot open ") + path; Log::Printf("[skinlua] %s", g_err.c_str()); return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz >(1 << 20)) { fclose(f); g_err = "bad script size"; return false; }
    std::string src((size_t)sz, 0);
    if (fread(&src[0], 1, (size_t)sz, f) != (size_t)sz) { fclose(f); g_err = "read fail"; return false; }
    fclose(f);

    try
    {
        g_L.reset(new sol::state());
        g_L->open_libraries(sol::lib::base, sol::lib::string, sol::lib::table,
                            sol::lib::math, sol::lib::utf8, sol::lib::coroutine,
                            sol::lib::package);
        sol::state& L = *g_L;
        BindTypes(L);
        g_readLogged.clear();
        g_findLogged.clear();
        g_useCount.clear();
        g_driveLogged.clear();
        g_visLogN = 0;
        // package.loadlib：只允许皮肤目录内的 DLL。Murasame 入口脚本会尝试
        //   package.loadlib("murasame_skin_native.dll", "luaopen_murasame_skin_native")
        // 该 DLL 依赖 Malody 的 xlua.dll（逆向：dumpbin /imports 实证），本进程无法解析其导入，
        // 因此默认拒绝并让皮肤自己的 Lua fallback 接管（这正是皮肤内置的兼容路径）。
        // 需要实验 native 路径时设 ADOFAI_PERFECT_ALLOW_NATIVE=1。
        {
            sol::table pkg = L["package"];
            if (pkg.valid())
            {
                // 不允许 require 去加载任意 C 模块；Lua 搜索路径仅限皮肤目录
                pkg["cpath"] = "";
                pkg["path"] = g_dir + "\\?.lua";
                sol::protected_function orig = pkg["loadlib"];
                pkg["loadlib"] = [orig](sol::variadic_args va) -> sol::object
                {
                    std::string name = va.size() ? ToStr(va.get<sol::object>(0)) : std::string();
                    std::string full;
                    static const bool s_allow = (getenv("ADOFAI_PERFECT_ALLOW_NATIVE") && getenv("ADOFAI_PERFECT_ALLOW_NATIVE")[0] == '1');
                    const bool isDll = name.size() > 4 && !_stricmp(name.c_str() + name.size() - 4, ".dll");
                    if (!s_allow || !isDll || !SkinPath(name, &full))
                    {
                        LogUseOnce("LoadLibBlocked", name.empty() ? std::string("(nil)") : name, 3);
                        return sol::make_object(*g_L, sol::nil);
                    }
                    sol::protected_function_result r = orig(full);
                    if (!r.valid()) return sol::make_object(*g_L, sol::nil);
                    return r.get<sol::object>(0);
                };
            }
        }

        // ---- Module ----
        sol::table mod = L.create_named_table("Module");
        // 注意：`Module:Find(x)` 语法会把 Module 表自身作为第 1 个实参传入，
        // 所以所有 Module.* 函数都用 variadic_args 取"最后一个"实参（同时兼容
        // `Module.Find(x)` 的点调用）。这是 Malody 皮肤脚本最常见的调用形式。
        mod["Find"] = [](sol::variadic_args va) -> ModRef
        {
            ModRef r{};
            std::string n = va.size() ? ToStr(va.get<sol::object>(va.size() - 1)) : std::string();
            auto it = g_byName.find(n);
            if (it != g_byName.end()) { r.p = it->second; return r; }
            for (int i = 0; i < g_modN; i++)
                if (g_mods[i].desc[0] && n == g_mods[i].desc)
                {
                    r.p = FindOrCreateReal(i);
                    if (g_findLogged.size() < 400 && !g_findLogged[n])
                    { g_findLogged[n] = true; Log::Printf("[skinlua] Find '%s' -> mod#%d desc='%s'", n.c_str(), i, g_mods[i].desc); }
                    return r;
                }
            for (int i = 0; i < g_modN; i++)
                if (g_mods[i].desc[0] && !_stricmp(g_mods[i].desc, n.c_str()))
                {
                    r.p = FindOrCreateReal(i);
                    if (g_findLogged.size() < 400 && !g_findLogged[n])
                    { g_findLogged[n] = true; Log::Printf("[skinlua] Find '%s' -> mod#%d desc='%s' (ci)", n.c_str(), i, g_mods[i].desc); }
                    return r;
                }
            r.p = FindByName(n.c_str());
            if (g_findLogged.size() < 400 && !g_findLogged[n])
            { g_findLogged[n] = true; Log::Printf("[skinlua] Find '%s' -> VIRTUAL (no module desc match)", n.c_str()); }
            return r;
        };
        mod["Shadow"] = [](sol::variadic_args va) -> ModRef
        {
            Inst* src = nullptr;
            if (va.size() >= 2)
            {
                sol::object m = va.get<sol::object>(va.size() - 2);
                if (m.is<ModRef>()) src = m.as<ModRef>().p;
            }
            const float lifeF = va.size() ? ToF(va.get<sol::object>(va.size() - 1)) : 0.f;
            Inst* in = NewInst();
            in->extra = true;
            in->shadow = true;
            in->src = (src ? src->src : -1);
            in->desc = (src ? src->desc : std::string());
            in->bornMs = g_nowMs;
            in->lifeMs = lifeF;
            if (src)
            {
                in->X = src->X; in->Y = src->Y; in->W = src->W; in->H = src->H;
                in->oX = src->oX; in->oY = src->oY; in->oW = src->oW; in->oH = src->oH;
                in->A = src->A; in->oA = src->oA; in->RZ = src->RZ; in->oRZ = src->oRZ;
                in->anims = src->anims;
            }
            LogUseOnce("Shadow", src && src->desc[0] ? src->desc : std::string("?"), 5);
            return ModRef{ in };
        };
        mod["Clone"] = [](sol::variadic_args va) -> ModRef
        {
            Inst* src = nullptr;
            if (va.size() >= 2)
            {
                sol::object m = va.get<sol::object>(va.size() - 2);
                if (m.is<ModRef>()) src = m.as<ModRef>().p;
            }
            std::string n = va.size() ? ToStr(va.get<sol::object>(va.size() - 1)) : std::string();
            auto it = g_byName.find(n);
            if (it != g_byName.end()) return ModRef{ it->second };
            Inst* in = NewInst();
            in->extra = true;
            in->src = (src ? src->src : -1);
            in->desc = n;
            if (src)
            {
                in->X = src->X; in->Y = src->Y; in->W = src->W; in->H = src->H;
                in->oX = src->oX; in->oY = src->oY; in->oW = src->oW; in->oH = src->oH;
                in->A = src->A; in->oA = src->oA;
                in->anims = src->anims;
            }
            g_byName[n] = in;
            LogUseOnce("Clone", src && src->desc[0] ? src->desc : n, 5);
            return ModRef{ in };
        };
        mod["GetBool"] = [](sol::variadic_args va) -> bool
        {
            std::string n = va.size() ? ToStr(va.get<sol::object>(va.size() - 1)) : std::string();
            for (int i = 0; i < g_modN; i++)
                if (g_mods[i].usage == 98 && g_mods[i].desc[0] && n == g_mods[i].desc)
                    return g_mods[i].valB != 0 || g_mods[i].valF != 0.f;
            return false;
        };
        mod["GetNumber"] = [](sol::variadic_args va) -> double
        {
            std::string n = va.size() ? ToStr(va.get<sol::object>(va.size() - 1)) : std::string();
            for (int i = 0; i < g_modN; i++)
                if (g_mods[i].usage == 98 && g_mods[i].desc[0] && n == g_mods[i].desc)
                    return g_mods[i].valF != 0.f ? (double)g_mods[i].valF : atof(g_mods[i].valS);
            return 0.0;
        };
        mod["GetString"] = [](sol::variadic_args va) -> std::string
        {
            std::string n = va.size() ? ToStr(va.get<sol::object>(va.size() - 1)) : std::string();
            for (int i = 0; i < g_modN; i++)
                if (g_mods[i].usage == 98 && g_mods[i].desc[0] && n == g_mods[i].desc)
                {
                    if (g_mods[i].valS[0]) return g_mods[i].valS;
                    char b[32]; snprintf(b, sizeof(b), "%g", g_mods[i].valF);
                    return b;
                }
            return std::string();
        };
        mod["SetFieldMeta"] = [](sol::variadic_args va)
        {
            if (va.size() >= 2)
                s_fieldMeta[ToStr(va.get<sol::object>(va.size() - 2))] = (double)ToF(va.get<sol::object>(va.size() - 1));
        };
        mod["FieldMeta"] = [](sol::variadic_args va) -> double
        {
            std::string n = va.size() ? ToStr(va.get<sol::object>(va.size() - 1)) : std::string();
            auto it = s_fieldMeta.find(n);
            if (it != s_fieldMeta.end()) return it->second;
            if (n == "SceneScale") return g_env.sceneScale;
            if (n == "TrackAngle" || n == "Angle") return (double)g_env.trackAngle;
            if (n == "Keys") return 4.0;
            return 0.0;
        };
        mod["Log"] = [](sol::variadic_args va)
        { if (va.size()) Log::Printf("[skinlua] %s", ToStr(va.get<sol::object>(va.size() - 1)).c_str()); };

        // ---- Game ----
        sol::table g = L.create_named_table("Game");
        g["Width"] = []() { return g_env.windowW; };
        g["Height"] = []() { return g_env.windowH; };
        g["Time"] = []() { return g_nowMs; };
        g["AudioLength"] = []() { return g_env.audioLengthMs; };
        g["StartTime"] = []() { return g_env.startTimeMs; };
        // Game:ReadFile(name) / Game:ReadBytes(name)：皮肤脚本自读 config.ini 与模块文件
        g["ReadFile"] = [](sol::variadic_args va) -> std::string
        {
            std::string n = va.size() ? ToStr(va.get<sol::object>(va.size() - 1)) : std::string();
            std::string data;
            const bool ok = ReadSkinFile(n, data);
            if (g_readLogged.size() < 400 && !g_readLogged["F:" + n])
            { g_readLogged["F:" + n] = true; Log::Printf("[skinlua] ReadFile '%s' -> %s", n.c_str(), ok ? "ok" : "missing"); }
            return ok ? data : std::string();
        };
        g["ReadBytes"] = [](sol::variadic_args va) -> sol::object
        {
            std::string n = va.size() ? ToStr(va.get<sol::object>(va.size() - 1)) : std::string();
            std::string data;
            const bool ok = ReadSkinFile(n, data);
            if (g_readLogged.size() < 400 && !g_readLogged["B:" + n])
            { g_readLogged["B:" + n] = true; Log::Printf("[skinlua] ReadBytes '%s' -> %s (%d bytes)", n.c_str(), ok ? "ok" : "missing", (int)data.size()); }
            if (!ok) return sol::make_object(*g_L, sol::nil);
            return sol::make_object(*g_L, Bytes{ data });
        };
        g["SceneScale"] = []() { return g_env.sceneScale; };
        g["TrackAngle"] = []() { return (double)g_env.trackAngle; };
        // Malody 皮肤模块用 Game:HitEvent()/Game:InputEvent() 读取当前判定事件
        // （逆向证据：Murasame hit_effect 模块 OnHit 首 4 条指令 = Game.HitEvent():JudgeResult()）。
        g["HitEvent"] = []() { return g_cur; };
        g["InputEvent"] = []() { return g_cur; };
        g["BpmCount"] = []() { return g_env.bpmCount; };
        g["BpmAt"] = [](sol::variadic_args va) -> sol::table
        {
            const int i = va.size() ? (int)ToF(va.get<sol::object>(va.size() - 1)) : 0;
            sol::table t = g_L->create_table();
            if (i >= 0 && i < g_env.bpmCount && g_env.bpmVals)
            {
                t["time"] = g_env.bpmTimes ? (double)g_env.bpmTimes[i] : 0.0;
                t["bpm"] = (double)g_env.bpmVals[i];
            }
            else { t["time"] = 0.0; t["bpm"] = (double)g_env.bpm; }
            return t;
        };
        g["ChartInfo"] = [](sol::variadic_args va) -> sol::object
        {
            std::string k = va.size() ? ToStr(va.get<sol::object>(va.size() - 1)) : std::string();
            sol::state& LS = *g_L;
            if (k == "Note" || k == "Notes" || k == "Count") return sol::make_object(LS, g_env.noteCount);
            if (k == "Key" || k == "Keys") return sol::make_object(LS, g_env.columns);
            if (k == "Combo") return sol::make_object(LS, g_live.combo);
            if (k == "MaxCombo") return sol::make_object(LS, g_live.maxCombo);
            if (k == "Score") return sol::make_object(LS, (double)g_live.score);
            if (k == "Acc" || k == "Accuracy") return sol::make_object(LS, g_live.acc * 100.0);
            if (k == "HP") return sol::make_object(LS, (double)(g_live.hp * 100.f));
            if (k == "Progress") return sol::make_object(LS, (double)g_live.progress);
            if (k == "Time") return sol::make_object(LS, g_nowMs);
            if (k == "Length") return sol::make_object(LS, g_env.audioLengthMs);
            if (k == "BPM") return sol::make_object(LS, (double)g_env.bpm);
            if (k == "Title" || k == "Song") return sol::make_object(LS, std::string(g_env.levelName ? g_env.levelName : ""));
            if (k == "Level") return sol::make_object(LS, 0);
            if (k == "Artist" || k == "Creator") return sol::make_object(LS, std::string(""));
            if (k == "Mod") return sol::make_object(LS, std::string(""));
            return sol::make_object(LS, sol::nil);
        };
        g["FieldMeta"] = [](sol::variadic_args va) -> double
        {
            std::string n = va.size() ? ToStr(va.get<sol::object>(va.size() - 1)) : std::string();
            auto it = s_fieldMeta.find(n);
            if (it != s_fieldMeta.end()) return it->second;
            if (n == "SceneScale") return g_env.sceneScale;
            if (n == "TrackAngle" || n == "Angle") return (double)g_env.trackAngle;
            if (n == "Keys") return 4.0;
            return 0.0;
        };
        g["Log"] = [](sol::variadic_args va)
        { if (va.size()) Log::Printf("[skinlua] %s", ToStr(va.get<sol::object>(va.size() - 1)).c_str()); };
        g["IsVersionGE"] = [](sol::variadic_args) { return true; };
        g["IsVersionGT"] = [](sol::variadic_args) { return true; };
        // Game:PlayMeta(key)：Malody 运行时 meta。judge=判定等级（WINDOWS_*_1..5 用，默认 3）。
        g["PlayMeta"] = [](sol::variadic_args va) -> sol::object
        {
            std::string n = va.size() ? ToStr(va.get<sol::object>(va.size() - 1)) : std::string();
            if (n == "judge" || n == "Judge") return sol::make_object(*g_L, 3);
            if (n == "UPSCROLL" || n == "upscroll") return sol::make_object(*g_L, 0);
            if (n == "Speed" || n == "speed") return sol::make_object(*g_L, 1.0);
            return sol::make_object(*g_L, sol::nil);
        };

        L.new_usertype<Event>("MalodyEvent", sol::no_constructor,
            "JudgeResult", [](Event& e) { return e.kind + 1; },
            "Offset", [](Event& e) { return e.off; },
            "HitX", [](Event& e) { return e.lane; },
            "Lane", [](Event& e) { return e.lane; },
            "NoteType", [](Event& e) { return e.noteType; },
            "Type", [](Event& e) { return e.kind; },
            "NoteInfo", [](Event&, sol::variadic_args) { return std::string(); },
            "HitInfo", [](Event&, sol::variadic_args) { return std::string(); });

        g["HitEvent"] = []() { return g_cur; };
        g["InputEvent"] = []() { return g_cur; };

        // ---- Audio ----（Malody 皮肤 API；Rurudo rrdv52.lua 使用）
        sol::table au = L.create_named_table("Audio");
        au["Load"] = [](sol::variadic_args va) -> sol::table
        {
            std::string f = va.size() ? ToStr(va.get<sol::object>(va.size() - 1)) : std::string();
            Log::Printf("[skinlua] Audio.Load '%s'", f.c_str());
            sol::table t = g_L->create_table();
            t["file"] = f;
            return t;
        };
        au["Play"] = [](sol::variadic_args va)
        {
            if (!va.size()) return;
            sol::object o = va.get<sol::object>(0);
            // 兼容 Audio:Play(handle, vol)（冒号调用首参=Audio 自身）与 Audio.Play(handle, vol)。
            // 判据：首参 table 若无 file 字段，则句柄在其后一个参数里。
            if (o.is<sol::table>())
            {
                sol::object fo = o.as<sol::table>()["file"];
                const bool hasFile = fo.valid() && fo.is<std::string>();
                if (!hasFile && va.size() > 1)
                {
                    sol::object o1 = va.get<sol::object>(1);
                    if (o1.is<sol::table>())
                    {
                        sol::object f1 = o1.as<sol::table>()["file"];
                        if (f1.valid() && f1.is<std::string>()) o = o1;
                    }
                    else if (o1.is<std::string>()) o = o1;
                }
            }
            std::string f;
            if (o.is<std::string>()) f = o.as<std::string>();
            else if (o.is<sol::table>())
            {
                sol::object fo = o.as<sol::table>()["file"];
                if (fo.valid() && fo.is<std::string>()) f = fo.as<std::string>();
            }
            else if (o.valid()) f = ToStr(o);
            int vol = 100;
            if (va.size() > 1)
            {
                sol::object last = va.get<sol::object>(va.size() - 1);
                if (last.is<double>() || last.is<int64_t>()) vol = (int)ToF(last);
            }
            PlaySkinSound(f, vol);
        };
        au["Stop"] = [](sol::variadic_args)
        {
            StopSkinSound();
        };
        au["Volume"] = [](sol::variadic_args) { return 100.0; };
        au["SetVolume"] = [](sol::variadic_args) {};

        L["starttime"] = env.startTimeMs;
        L["audiotime"] = env.audioLengthMs;

        auto r = L.script(src, std::string(path));
        if (!r.valid())
        {
            sol::error e = r;
            g_err = e.what();
            Log::Printf("[skinlua] load error: %s", e.what());
            g_L.reset();
            return false;
        }
        g_ready = true;
        Log::Printf("[skinlua] entry: Init=%d Update=%d OnHit=%d OnInput=%d OnRetry=%d",
                    (*g_L)["Init"].valid() ? 1 : 0, (*g_L)["Update"].valid() ? 1 : 0,
                    (*g_L)["OnHit"].valid() ? 1 : 0, (*g_L)["OnInput"].valid() ? 1 : 0,
                    (*g_L)["OnRetry"].valid() ? 1 : 0);
        if (!CallFn("Init", false))
            Log::Printf("[skinlua] Init failed: %s", g_err.c_str());
        else
            Log::Printf("[skinlua] loaded %s (%d modules)", scriptFile, modCount);
        return true;
    }
    catch (const std::exception& e)
    {
        g_err = e.what();
        Log::Printf("[skinlua] exception: %s", e.what());
        g_L.reset();
        g_ready = false;
        return false;
    }
}

void Frame(const Live& live)
{
    g_live = live;
    g_nowMs = live.songMs;
    if (!g_ready || !g_L) return;

    std::vector<Event> hits, inputs;
    {
        std::lock_guard<std::mutex> lk(g_qmx);
        hits.swap(g_hits);
        inputs.swap(g_inputs);
    }
    const bool hasHitFn = (*g_L)["OnHit"].valid();
    const bool hasInputFn = (*g_L)["OnInput"].valid();
    if (hits.size() > 64) hits.erase(hits.begin(), hits.begin() + (hits.size() - 64));
    if (hasHitFn)
        for (const Event& e : hits)
        {
            g_cur = e;
            static int s_dispLog = 0;
            if (s_dispLog < 8)
            {
                s_dispLog++;
                Log::Printf("[skinlua] dispatch OnHit kind=%d off=%.1f (#%d)", e.kind, e.off, s_dispLog);
            }
            CallFn("OnHit", false);
        }
    if (hasInputFn)
        for (const Event& e : inputs) { g_cur = e; CallFn("OnInput", false); }
    CallFn("Update", false);

    for (auto& up : g_insts)
    {
        Inst* in = up.get();
        if (in->shadow && !in->dead && in->lifeMs > 0.0 && g_nowMs > in->bornMs + in->lifeMs)
            in->dead = true;
    }
    // 证据日志：Clone/Shadow 第一次真正可见（Alpha>2）时记录 desc/combo/t。
    // 例：Murasame 的 ciallo 大图在连击触发时由 hit_effect.lua 激活 → 这里能看到精确 combo。
    if (g_visLogN < 80)
    {
        for (auto& up : g_insts)
        {
            Inst* in = up.get();
            if (!in->extra || in->dead || in->visLogged) continue;
            Patch p{};
            Compute(in, g_nowMs, &p, nullptr);
            if (p.av > 2)
            {
                in->visLogged = true;
                g_visLogN++;
                const char* d = (in->src >= 0 && in->src < g_modN) ? g_mods[in->src].desc : in->desc.c_str();
                Log::Printf("[skinlua] extra '%s' visible combo=%d t=%.0fms (clone/shadow activated)",
                            (d && d[0]) ? d : "?", g_live.combo, g_nowMs);
                if (g_visLogN >= 80) break;
            }
        }
    }
}

void Eval(int modIdx, double songMs, Patch* out)
{
    if (!out) return;
    auto it = g_byMod.find(modIdx);
    if (it == g_byMod.end() || !it->second) return;
    Compute(it->second, songMs, out, nullptr);
    // 证据日志：脚本第一次真正驱动某模块（说明该 desc/图片由 Lua 决定位置/透明度/文本）
    if ((out->x || out->y || out->w || out->h || out->alpha || out->rotate || out->fill || out->text) &&
        g_driveLogged.size() < 300 && !g_driveLogged[modIdx])
    {
        g_driveLogged[modIdx] = true;
        const char* d = (modIdx >= 0 && modIdx < g_modN) ? g_mods[modIdx].desc : "?";
        Log::Printf("[skinlua] drive mod#%d desc='%s' (script props applied)", modIdx, d ? d : "?");
    }
}

bool Touched(int modIdx)
{
    auto it = g_byMod.find(modIdx);
    if (it == g_byMod.end() || !it->second) return false;
    const Inst* in = it->second;
    return in->oX || in->oY || in->oW || in->oH || in->oA || in->oRZ ||
           in->oFill || in->oText || in->oColor || in->oSX || in->oSY ||
           !in->anims.empty();
}

int Extras(Extra* out, int maxN)
{
    if (!out || maxN <= 0) return 0;
    int n = 0;
    for (auto& up : g_insts)
    {
        Inst* in = up.get();
        if (!in->extra || in->dead || in->src < 0) continue;
        if (n >= maxN) break;
        Extra& e = out[n];
        e = Extra{};
        e.srcMod = in->src;
        e.shadow = in->shadow;
        e.playing = in->playing;
        e.playT0 = in->playT0;
        Compute(in, g_nowMs, &e.patch, nullptr);
        n++;
    }
    return n;
}

void PushHit(int kind, double offsetMs)
{
    std::lock_guard<std::mutex> lk(g_qmx);
    Event e;
    e.type = 1; e.kind = kind; e.off = offsetMs; e.lane = 1; e.noteType = 1;
    if (g_hits.size() < 512) g_hits.push_back(e);
}

void PushInput(int type, int hitx)
{
    std::lock_guard<std::mutex> lk(g_qmx);
    Event e;
    e.type = type; e.kind = type; e.lane = hitx; e.noteType = 0;
    if (g_inputs.size() < 512) g_inputs.push_back(e);
}

} // namespace SkinLua
