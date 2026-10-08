// ============================================================
// SkinMsp.cpp — Malody MSP 皮肤包解析 / 导入
//
//   证据来源：MalodyV GameAssembly.dll（IL2CPP metadata v31）反编译
//     · SkinFile / SkinFile.Types.* 消息结构
//     · Malody.Composer.SkinModuleUsage / SkinModuleSubType /
//       SkinModuleScene / SkinModuleTrigger 枚举
//   角色判定规则与内置 Rurudo 皮肤（skin\info.asm）逐模块核对过
// ============================================================
#include "SkinMsp.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace SkinMsp
{
    static bool FileExistsPath(const char* path)
    {
        DWORD a = GetFileAttributesA(path);
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }
    static bool DirExistsPath(const char* path)
    {
        DWORD a = GetFileAttributesA(path);
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    }
    static bool HasFileIn(const char* dir, const char* name)
    {
        char buf[MAX_PATH * 2];
        snprintf(buf, sizeof(buf), "%s\\%s", dir, name);
        return FileExistsPath(buf);
    }
    void DefaultRoot(char* out, int n)
    {
        // root = <dir of this DLL>\skin, same as ChartCore::SkinDefaultRoot().
        // Must not use GetModuleFileNameA(nullptr): that is the game exe dir,
        // while the release package keeps skin\ next to Injector/ADOFAIPerfect.dll.
        char mod[MAX_PATH * 2]{};
        HMODULE hm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)&DefaultRoot, &hm) && hm)
            GetModuleFileNameA(hm, mod, sizeof(mod));
        if (!mod[0])
            GetModuleFileNameA(nullptr, mod, sizeof(mod));
        char* s = strrchr(mod, '\\');
        if (s) *s = 0;
        snprintf(out, n, "%s\\skin", mod);
    }

    static bool EqI(const char* a, const char* b)
    {
        if (!a || !b) return false;
        for (; *a && *b; a++, b++)
            if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        return *a == 0 && *b == 0;
    }
    static bool HasI(const char* s, const char* sub)
    {
        if (!s || !sub || !*sub) return false;
        size_t n = strlen(sub);
        for (const char* p = s; *p; p++)
        {
            size_t i = 0;
            while (i < n && p[i] &&
                   tolower((unsigned char)p[i]) == tolower((unsigned char)sub[i])) i++;
            if (i == n) return true;
        }
        return false;
    }
    static void CopyStr(char* dst, int n, const char* src)
    {
        if (!dst || n <= 0) return;
        if (!src) { dst[0] = 0; return; }
        snprintf(dst, (size_t)n, "%s", src);
    }

    static void DeriveBase(const char* file, char* base, int n)
    {
        base[0] = 0;
        if (!file || !file[0]) return;
        const char* dot = strrchr(file, '.');
        const char* end = dot ? dot : file + strlen(file);
        const char* p = end;
        while (p > file && isdigit((unsigned char)p[-1])) p--;
        if (p > file && p[-1] == '-')
        {
            size_t len = (size_t)(p - file);
            if (len >= (size_t)n) len = (size_t)n - 1;
            memcpy(base, file, len);
            base[len] = 0;
        }
        else
        {
            size_t len = (size_t)(end - file);
            if (len >= (size_t)n) len = (size_t)n - 1;
            memcpy(base, file, len);
            base[len] = 0;
        }
    }

    struct ImgSpec
    {
        char file[96]{};
        char base[96]{};
        int  frames = 0, fps = 0, start = 0;
        int  res = 0;                     // field 2（SkinModuleRes）
        float w = 0.f, h = 0.f;
        int  wu = 0, hu = 0;              // 0=Percent 1=Unit(1080p px) 2=Px
        int  flipx = 0, flipy = 0;        // field 8/9
        int  slice[4] = { 0, 0, 0, 0 };   // field 10（repeated：左/下/右/上，9-slice）
        int  sliceN = 0;
        int  repeat = 0;                  // field 5（帧序列重复次数，0=循环）
        float fill = 0.f;                 // field 13（旧写法 float 比例）
        int  fillDir = 0;                 // field 13（ModuleParamFill 枚举：0 FromLeft 1 FromRight）
        int  valint = 0;                  // field 14
        char color[16]{};                 // "#RRGGBB"（Color 模块 / 染色）
        int  hide = 0, blend = 0, nobreak = 0;  // field 16/19/20
    };
    // 帧序列首帧号：从种子文件名推导（Phigros phira-effect-1.png → 1；Gazer AGbest-0.png → 0）。
    // 证据：info.asm 里 Phigros effect 模块 filestart 字段为 0，但文件从 -1 开始编号，
    //        只有按"种子文件自身编号 = 首帧"解释才能全部命中（Malody 动画也是从种子的下一张推进）。
    static int SeedFrameIndex(const char* file, const char* base)
    {
        if (!file || !file[0] || !base || !base[0]) return 0;
        const size_t bl = strlen(base);
        if (strncmp(file, base, bl) != 0) return 0;
        int v = 0;
        bool any = false;
        for (const char* p = file + bl; *p >= '0' && *p <= '9'; p++)
        {
            v = v * 10 + (*p - '0');
            any = true;
            if (v > 9999) break;
        }
        return any ? v : 0;
    }
    struct TextSpec
    {
        char text[96]{};
        int  font = 0;
        char color[16]{};
        float size = 0.f;                 // % 屏高
    };
    struct NumSpec
    {
        char file[96]{};
        char text[64]{};
        char base[64]{};
        int   start = 0;
        float h = 0.f, pad = 0.f;
        int   valint = 0;                 // field 7
    };
    struct ValueSpec                   // ModuleParamValue（field 12：ScriptValue 载荷）
    {
        float valF = 0.f, valMin = 0.f, valMax = 0.f;
        char  valS[64]{};
        int   valB = 0;
    };
    struct SoundSpec                   // ModuleParamSound（field 13）
    {
        char file[64]{};
        int  loop = 0, volume = 0, hitsound = 0;
    };
    struct AnimSpec                    // ModuleAnimation（field 20）
    {
        int   type = 0;
        int   startMs = 0, endMs = 0;
        float from0 = 0, from1 = 0, to0 = 0, to1 = 0;
        int   repeat = 0, repeatType = 0, delay = 0, ease = 0, nid = 0;
    };
    struct Cond
    {
        int source = 0, flag = 0;
        int valint = 0;
        float valdbl = 0.f;
        char valstr[48]{};                // field 5（字符串条件值）
    };
    struct ModSpec
    {
        int  usage = 0, type = 0;
        char desc[64]{};
        char srcFile[80]{};               // 图片模块对应的贴图名（Lua Module:Find 用）
        float x = 0.f, y = 0.f;
        float dx = 0.f, dy = 0.f;
        int  xu = 0, yu = 0, dxu = 0, dyu = 0;
        int  pivot = 4;
        bool anchorNote = false;
        int  layer = 0, order = 0, alpha = 100;
        bool alphaSet = false;            // basic.alpha 字段是否声明（缺省 = 0 = 不可见）
        int  rotate = 0;
        ImgSpec img;
        NumSpec num;
        TextSpec txt;
        ValueSpec val;
        SoundSpec snd;
        int  coexist = 0;                 // field 16
        int  arrow = -1;                  // scene Arrow=11（0 Static/1 Left/2 Up/3 Right；-1=未声明）
        // ModuleMeta（field 3）：1 creator · 2 tags · 3 desc · 4 client · 5 time · 6 disabled · 7 hideInEditor
        char metaCreator[48]{}, metaTags[64]{}, metaClient[48]{};
        long long metaTime = 0;
        int  metaDisabled = 0, metaHide = 0;
        std::vector<AnimSpec> anims;      // field 20
        std::vector<Cond> scenes;
        std::vector<Cond> triggers;
        std::vector<ImgSpec> images;
        std::vector<ImgSpec> noteImgs;
    };

    struct Pb
    {
        const unsigned char* p = nullptr;
        const unsigned char* e = nullptr;
        bool bad = false;

        Pb() {}
        Pb(const unsigned char* d, size_t n) : p(d), e(d + n) {}

        bool varint(uint64_t& v)
        {
            v = 0;
            int shift = 0;
            while (p < e)
            {
                unsigned char c = *p++;
                v |= (uint64_t)(c & 0x7f) << shift;
                if (!(c & 0x80)) return true;
                shift += 7;
                if (shift > 63) break;
            }
            bad = true;
            return false;
        }
        bool fixed32(uint32_t& v)
        {
            if (e - p < 4) { bad = true; return false; }
            v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            p += 4;
            return true;
        }
        bool fixed64(uint64_t& v)
        {
            if (e - p < 8) { bad = true; return false; }
            v = 0;
            for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
            p += 8;
            return true;
        }
        bool bytes(const unsigned char** d, size_t* n)
        {
            uint64_t l = 0;
            if (!varint(l) || l > (uint64_t)(e - p)) { bad = true; return false; }
            *d = p; *n = (size_t)l; p += l;
            return true;
        }
        bool next(uint32_t& fn, uint32_t& wt)
        {
            uint64_t k = 0;
            if (!varint(k)) return false;
            fn = (uint32_t)(k >> 3);
            wt = (uint32_t)(k & 7);
            return fn != 0;
        }
        void skip(uint32_t wt)
        {
            uint64_t v;
            const unsigned char* d; size_t n;
            switch (wt)
            {
            case 0: varint(v); break;
            case 1: if (e - p >= 8) p += 8; else bad = true; break;
            case 2: bytes(&d, &n); break;
            case 5: if (e - p >= 4) p += 4; else bad = true; break;
            default: bad = true; break;
            }
        }
    };

    static float ToF32(uint64_t v)
    {
        uint32_t u = (uint32_t)v;
        float f = 0.f;
        memcpy(&f, &u, 4);
        return f;
    }
    static float ToF64(uint64_t v)
    {
        double d = 0.0;
        memcpy(&d, &v, 8);
        return (float)d;
    }

    static void ParseImg(Pb& pb, ImgSpec& s)
    {
        uint32_t fn, wt;
        while (pb.next(fn, wt))
        {
            if (fn == 1 && wt == 2)
            {
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                snprintf(s.file, sizeof(s.file), "%.*s", (int)n, (const char*)d);
            }
            else if (fn == 2 && wt == 0) { uint64_t v; pb.varint(v); s.res = (int)v; }
            else if (fn == 3 && wt == 0) { uint64_t v; pb.varint(v); s.frames = (int)v; }
            else if (fn == 4 && wt == 0) { uint64_t v; pb.varint(v); s.fps = (int)v; }
            else if (fn == 5 && wt == 0) { uint64_t v; pb.varint(v); s.repeat = (int)v; }
            else if (fn == 6 && wt == 5) { uint32_t v = 0; pb.fixed32(v); s.w = ToF32(v); }
            else if (fn == 7 && wt == 5) { uint32_t v = 0; pb.fixed32(v); s.h = ToF32(v); }
            else if (fn == 8 && wt == 0) { uint64_t v; pb.varint(v); s.flipx = (int)v; }
            else if (fn == 9 && wt == 0) { uint64_t v; pb.varint(v); s.flipy = (int)v; }
            else if (fn == 10 && wt == 0)
            {
                uint64_t v; pb.varint(v);
                if (s.sliceN < 4) s.slice[s.sliceN++] = (int)v;
            }
            else if (fn == 11 && wt == 2)
            {
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                snprintf(s.base, sizeof(s.base), "%.*s", (int)n, (const char*)d);
            }
            else if (fn == 12 && wt == 0) { uint64_t v; pb.varint(v); s.start = (int)v; }
            else if (fn == 13 && wt == 0) { uint64_t v; pb.varint(v); s.fillDir = (int)v; }
            else if (fn == 13 && wt == 5) { uint32_t v = 0; pb.fixed32(v); s.fill = ToF32(v); }
            else if (fn == 13 && wt == 1) { uint64_t v = 0; pb.fixed64(v); s.fill = ToF64(v); }
            else if (fn == 14 && wt == 0) { uint64_t v; pb.varint(v); s.valint = (int)v; }
            else if (fn == 15 && wt == 2)
            {
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                snprintf(s.color, sizeof(s.color), "%.*s", (int)n, (const char*)d);
            }
            else if (fn == 17 && wt == 0) { uint64_t v; pb.varint(v); s.wu = (int)v; }
            else if (fn == 18 && wt == 0) { uint64_t v; pb.varint(v); s.hu = (int)v; }
            else if (fn == 16 && wt == 0) { uint64_t v; pb.varint(v); s.hide = (int)v; }
            else if (fn == 19 && wt == 0) { uint64_t v; pb.varint(v); s.blend = (int)v; }
            else if (fn == 20 && wt == 0) { uint64_t v; pb.varint(v); s.nobreak = (int)v; }
            else pb.skip(wt);
        }
    }
    static void ParseText(Pb& pb, TextSpec& s)
    {
        uint32_t fn, wt;
        while (pb.next(fn, wt))
        {
            if (fn == 1 && wt == 2)
            {
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                snprintf(s.text, sizeof(s.text), "%.*s", (int)n, (const char*)d);
            }
            else if (fn == 2 && wt == 0) { uint64_t v; pb.varint(v); s.font = (int)v; }
            else if (fn == 3 && wt == 2)
            {
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                snprintf(s.color, sizeof(s.color), "%.*s", (int)n, (const char*)d);
            }
            else if (fn == 4 && wt == 5) { uint32_t v = 0; pb.fixed32(v); s.size = ToF32(v); }
            else pb.skip(wt);
        }
    }
    static void ParseNum(Pb& pb, NumSpec& s)
    {
        uint32_t fn, wt;
        while (pb.next(fn, wt))
        {
            if (fn == 1 && wt == 2)
            {
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                snprintf(s.file, sizeof(s.file), "%.*s", (int)n, (const char*)d);
            }
            else if (fn == 2 && wt == 2)
            {
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                snprintf(s.text, sizeof(s.text), "%.*s", (int)n, (const char*)d);
            }
            else if (fn == 3 && wt == 5) { uint32_t v = 0; pb.fixed32(v); s.h = ToF32(v); }
            else if (fn == 3 && wt == 1) { uint64_t v = 0; pb.fixed64(v); s.h = ToF64(v); }
            else if (fn == 4 && wt == 5) { uint32_t v = 0; pb.fixed32(v); s.pad = ToF32(v); }
            else if (fn == 4 && wt == 1) { uint64_t v = 0; pb.fixed64(v); s.pad = ToF64(v); }
            else if (fn == 5 && wt == 2)
            {
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                snprintf(s.base, sizeof(s.base), "%.*s", (int)n, (const char*)d);
            }
            else if (fn == 6 && wt == 0) { uint64_t v; pb.varint(v); s.start = (int)v; }
            else if (fn == 7 && wt == 0) { uint64_t v; pb.varint(v); s.valint = (int)v; }
            else pb.skip(wt);
        }
    }
    // ModuleParamValue（SkinFile.Module field 12）：1 valF · 2 valS · 3 valB · 4 valMin · 5 valMax
    static void ParseValue(Pb& pb, ValueSpec& s)
    {
        uint32_t fn, wt;
        while (pb.next(fn, wt))
        {
            if (fn == 1 && wt == 5) { uint32_t v = 0; pb.fixed32(v); s.valF = ToF32(v); }
            else if (fn == 1 && wt == 1) { uint64_t v = 0; pb.fixed64(v); s.valF = ToF64(v); }
            else if (fn == 2 && wt == 2)
            {
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                snprintf(s.valS, sizeof(s.valS), "%.*s", (int)n, (const char*)d);
            }
            else if (fn == 3 && wt == 0) { uint64_t v; pb.varint(v); s.valB = (int)v; }
            else if (fn == 4 && wt == 5) { uint32_t v = 0; pb.fixed32(v); s.valMin = ToF32(v); }
            else if (fn == 5 && wt == 5) { uint32_t v = 0; pb.fixed32(v); s.valMax = ToF32(v); }
            else pb.skip(wt);
        }
    }
    // ModuleParamSound（field 13）：1 file · 2 loop · 3 volume · 4 hitsound
    static void ParseSound(Pb& pb, SoundSpec& s)
    {
        uint32_t fn, wt;
        while (pb.next(fn, wt))
        {
            if (fn == 1 && wt == 2)
            {
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                snprintf(s.file, sizeof(s.file), "%.*s", (int)n, (const char*)d);
            }
            else if (fn == 2 && wt == 0) { uint64_t v; pb.varint(v); s.loop = (int)v; }
            else if (fn == 3 && wt == 0) { uint64_t v; pb.varint(v); s.volume = (int)v; }
            else if (fn == 4 && wt == 0) { uint64_t v; pb.varint(v); s.hitsound = (int)v; }
            else pb.skip(wt);
        }
    }
    static void ParseCond(Pb& pb, Cond& c)
    {
        uint32_t fn, wt;
        while (pb.next(fn, wt))
        {
            if (fn == 1 && wt == 0) { uint64_t v; pb.varint(v); c.source = (int)v; }
            else if (fn == 2 && wt == 0) { uint64_t v; pb.varint(v); c.flag = (int)v; }
            else if (fn == 3 && wt == 0) { uint64_t v; pb.varint(v); c.valint = (int)v; }
            else if (fn == 4 && wt == 5) { uint32_t v = 0; pb.fixed32(v); c.valdbl = ToF32(v); }
            else if (fn == 4 && wt == 1) { uint64_t v = 0; pb.fixed64(v); c.valdbl = ToF64(v); }
            else if (fn == 5 && wt == 2)
            {
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                snprintf(c.valstr, sizeof(c.valstr), "%.*s", (int)n, (const char*)d);
            }
            else pb.skip(wt);
        }
    }
    static void ParseMod(Pb& pb, ModSpec& m)
    {
        uint32_t fn, wt;
        while (pb.next(fn, wt))
        {
            switch (fn)
            {
            case 1: if (wt == 0) { uint64_t v; pb.varint(v); m.usage = (int)v; } else pb.skip(wt); break;
            case 2: if (wt == 0) { uint64_t v; pb.varint(v); m.type = (int)v; } else pb.skip(wt); break;
            case 3:
            {
                if (wt != 2) { pb.skip(wt); break; }
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                Pb sub(d, n); uint32_t f2, w2;
                while (sub.next(f2, w2))
                {
                    if (f2 == 1 && w2 == 2)
                    {
                        const unsigned char* dd; size_t nn;
                        if (!sub.bytes(&dd, &nn)) break;
                        snprintf(m.metaCreator, sizeof(m.metaCreator), "%.*s", (int)nn, (const char*)dd);
                    }
                    else if (f2 == 2 && w2 == 2)
                    {
                        const unsigned char* dd; size_t nn;
                        if (!sub.bytes(&dd, &nn)) break;
                        snprintf(m.metaTags, sizeof(m.metaTags), "%.*s", (int)nn, (const char*)dd);
                    }
                    else if (f2 == 3 && w2 == 2)
                    {
                        const unsigned char* dd; size_t nn;
                        if (!sub.bytes(&dd, &nn)) break;
                        snprintf(m.desc, sizeof(m.desc), "%.*s", (int)nn, (const char*)dd);
                    }
                    else if (f2 == 4 && w2 == 2)
                    {
                        const unsigned char* dd; size_t nn;
                        if (!sub.bytes(&dd, &nn)) break;
                        snprintf(m.metaClient, sizeof(m.metaClient), "%.*s", (int)nn, (const char*)dd);
                    }
                    else if (f2 == 5 && w2 == 0) { uint64_t v; sub.varint(v); m.metaTime = (long long)v; }
                    else if (f2 == 6 && w2 == 0) { uint64_t v; sub.varint(v); m.metaDisabled = (int)v; }
                    else if (f2 == 7 && w2 == 0) { uint64_t v; sub.varint(v); m.metaHide = (int)v; }
                    else sub.skip(w2);
                }
                break;
            }
            case 4: case 5:
            {
                if (wt != 2) { pb.skip(wt); break; }
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                Pb sub(d, n); Cond c; ParseCond(sub, c);
                (fn == 4 ? m.scenes : m.triggers).push_back(c);
                if (fn == 4 && c.source == 11) m.arrow = c.valint;
                break;
            }
            case 6:
            {
                if (wt != 2) { pb.skip(wt); break; }
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                Pb sub(d, n); uint32_t f2, w2;
                while (sub.next(f2, w2))
                {
                    uint64_t v;
                    if (f2 == 2 && w2 == 0) { sub.varint(v); m.pivot = (int)v; }
                    else if (f2 == 3 && w2 == 0) { sub.varint(v); m.rotate = (int)v; }
                    else if (f2 == 4 && w2 == 5) { uint32_t fv = 0; sub.fixed32(fv); m.x = ToF32(fv); }
                    else if (f2 == 5 && w2 == 5) { uint32_t fv = 0; sub.fixed32(fv); m.y = ToF32(fv); }
                    else if (f2 == 6 && w2 == 0) { sub.varint(v); m.alpha = (int)v; m.alphaSet = true; }
                    else if (f2 == 7 && w2 == 0) { sub.varint(v); m.layer = (int)v; }
                    else if (f2 == 8 && w2 == 0) { sub.varint(v); m.order = (int)v; }
                    else if (f2 == 9 && w2 == 0) { sub.varint(v); m.xu = (int)v; }
                    else if (f2 == 10 && w2 == 0) { sub.varint(v); m.yu = (int)v; }
                    else if (f2 == 11 && w2 == 5) { uint32_t fv = 0; sub.fixed32(fv); m.dx = ToF32(fv); }
                    else if (f2 == 12 && w2 == 5) { uint32_t fv = 0; sub.fixed32(fv); m.dy = ToF32(fv); }
                    else if (f2 == 13 && w2 == 0) { sub.varint(v); m.dxu = (int)v; }
                    else if (f2 == 14 && w2 == 0) { sub.varint(v); m.dyu = (int)v; }
                    else if (f2 == 15 && w2 == 0) { sub.varint(v); m.anchorNote = (v != 0); }
                    else if (f2 == 16 && w2 == 0) { sub.varint(v); m.coexist = (int)v; }
                    else sub.skip(w2);
                }
                break;
            }
            case 8:
            {
                if (wt != 2) { pb.skip(wt); break; }
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                Pb sub(d, n); ParseText(sub, m.txt);
                break;
            }
            case 7:
            {
                if (wt != 2) { pb.skip(wt); break; }
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                Pb sub(d, n); ParseImg(sub, m.img);
                break;
            }
            case 9:
            {
                if (wt != 2) { pb.skip(wt); break; }
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                Pb sub(d, n); uint32_t f2, w2;
                while (sub.next(f2, w2))
                {
                    if (f2 == 1 && w2 == 2)
                    {
                        const unsigned char* dd; size_t nn;
                        if (!sub.bytes(&dd, &nn)) break;
                        Pb item(dd, nn); ImgSpec im; ParseImg(item, im);
                        m.images.push_back(im);
                    }
                    else sub.skip(w2);
                }
                break;
            }
            case 10:
            {
                if (wt != 2) { pb.skip(wt); break; }
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                Pb sub(d, n); ParseNum(sub, m.num);
                break;
            }
            case 11:
            {
                if (wt != 2) { pb.skip(wt); break; }
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                Pb sub(d, n); uint32_t f2, w2;
                while (sub.next(f2, w2))
                {
                    if (f2 == 1 && w2 == 2)
                    {
                        const unsigned char* dd; size_t nn;
                        if (!sub.bytes(&dd, &nn)) break;
                        Pb item(dd, nn); ImgSpec im; ParseImg(item, im);
                        m.noteImgs.push_back(im);
                    }
                    else sub.skip(w2);
                }
                break;
            }
            case 12:
            {
                if (wt != 2) { pb.skip(wt); break; }
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                Pb sub(d, n); ParseValue(sub, m.val);
                break;
            }
            case 13:
            {
                if (wt != 2) { pb.skip(wt); break; }
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                Pb sub(d, n); ParseSound(sub, m.snd);
                break;
            }
            case 20:
            {
                if (wt != 2) { pb.skip(wt); break; }
                const unsigned char* d; size_t n;
                if (!pb.bytes(&d, &n)) break;
                Pb sub(d, n); uint32_t f2, w2;
                AnimSpec a;
                while (sub.next(f2, w2))
                {
                    uint64_t v;
                    if (f2 == 1 && w2 == 0) { sub.varint(v); a.type = (int)v; }
                    else if (f2 == 2 && w2 == 0) { sub.varint(v); a.startMs = (int)v; }
                    else if (f2 == 3 && w2 == 0) { sub.varint(v); a.endMs = (int)v; }
                    else if (f2 == 4 && w2 == 5) { uint32_t fv = 0; sub.fixed32(fv); a.from0 = ToF32(fv); }
                    else if (f2 == 4 && w2 == 1) { uint64_t fv = 0; sub.fixed64(fv); a.from0 = ToF64(fv); }
                    else if (f2 == 5 && w2 == 5) { uint32_t fv = 0; sub.fixed32(fv); a.from1 = ToF32(fv); }
                    else if (f2 == 5 && w2 == 1) { uint64_t fv = 0; sub.fixed64(fv); a.from1 = ToF64(fv); }
                    else if (f2 == 6 && w2 == 5) { uint32_t fv = 0; sub.fixed32(fv); a.to0 = ToF32(fv); }
                    else if (f2 == 6 && w2 == 1) { uint64_t fv = 0; sub.fixed64(fv); a.to0 = ToF64(fv); }
                    else if (f2 == 7 && w2 == 5) { uint32_t fv = 0; sub.fixed32(fv); a.to1 = ToF32(fv); }
                    else if (f2 == 7 && w2 == 1) { uint64_t fv = 0; sub.fixed64(fv); a.to1 = ToF64(fv); }
                    else if (f2 == 8 && w2 == 0) { sub.varint(v); a.repeat = (int)v; }
                    else if (f2 == 9 && w2 == 0) { sub.varint(v); a.repeatType = (int)v; }
                    else if (f2 == 10 && w2 == 0) { sub.varint(v); a.delay = (int)v; }
                    else if (f2 == 11 && w2 == 0) { sub.varint(v); a.ease = (int)v; }
                    else if (f2 == 12 && w2 == 0) { sub.varint(v); a.nid = (int)v; }
                    else sub.skip(w2);
                }
                if (a.type != 0 && m.anims.size() < 16) m.anims.push_back(a);
                break;
            }
            default: pb.skip(wt); break;
            }
        }
    }

    static int SceneLane(const ModSpec& m)
    {
        for (const Cond& c : m.scenes)
            if (c.source == 1 && c.valint >= 1 && c.valint <= kLanes)
                return c.valint - 1;
        return -1;
    }
    static bool HasTrig(const ModSpec& m, int src)
    {
        for (const Cond& c : m.triggers)
            if (c.source == src) return true;
        return false;
    }
    static int JudgeScene(const ModSpec& m)
    {
        for (const Cond& c : m.scenes)
            if (c.source == 14 && c.valint >= 0 && c.valint < kJudgePopN)
                return c.valint;
        return -1;
    }
    static void SetLane(char dst[kLanes][96], int lane, const char* file)
    {
        if (!file || !file[0]) return;
        if (lane >= 0 && lane < kLanes) { if (!dst[lane][0]) CopyStr(dst[lane], 96, file); }
        else for (int i = 0; i < kLanes; i++) if (!dst[i][0]) CopyStr(dst[i], 96, file);
    }
    static void MarkValid(Roles* r)
    {
        for (int i = 0; i < kLanes; i++)
            if (r->note[i][0] || r->holdBody[i][0] || r->press[i][0]) { r->valid = true; return; }
        if (r->hitFxBase[0] || r->hitFxSeed[0] || r->line[0] || r->comboBase[0] ||
            r->accBase[0] || r->judgeBar[0][0] || r->charImg[0] || r->bg[0]) r->valid = true;
    }
    // 把解析出的模块原样（含位置/单位/锚点/条件/贴图）存进角色表，供整屏渲染使用。
    // 逆向依据：Malody.Scene.Composer.fky::ApplyBasicParam / ApplyImageSize（见 SkinMsp.h 注释）
    static void BuildMods(std::vector<ModSpec>& mods, Roles* r)
    {
        r->modCount = 0;
        for (ModSpec& m : mods)
        {
            if (m.type == 5006 || m.type == 5005) continue;      // Sound / Video：覆盖层不播
            if (r->modCount >= Roles::kMaxMods) break;
            RoleMod& d = r->mods[r->modCount];
            d.usage = m.usage; d.type = m.type;
            snprintf(d.desc, sizeof(d.desc), "%s", m.desc);
            d.x = m.x; d.y = m.y; d.dx = m.dx; d.dy = m.dy;
            d.xu = m.xu; d.yu = m.yu; d.dxu = m.dxu; d.dyu = m.dyu;
            d.pivot = m.pivot;
            d.layer = m.layer; d.order = m.order; d.alpha = m.alpha;
            d.alphaSet = m.alphaSet;
            d.rotate = m.rotate;
            d.anchorNote = m.anchorNote;
            snprintf(d.img, sizeof(d.img), "%s", m.img.file);
            snprintf(d.srcFile, sizeof(d.srcFile), "%s", m.img.file[0] ? m.img.file : (m.images.empty() ? "" : m.images[0].file));
            snprintf(d.base, sizeof(d.base), "%s", m.img.base);
            snprintf(d.color, sizeof(d.color), "%s", m.img.color);
            d.w = m.img.w; d.h = m.img.h; d.wu = m.img.wu; d.hu = m.img.hu;
            d.frames = m.img.frames; d.fps = m.img.fps; d.start = m.img.start;
            d.res = m.img.res;
            d.flipx = m.img.flipx; d.flipy = m.img.flipy;
            d.sliceN = m.img.sliceN;
            for (int si = 0; si < 4; si++) d.slice[si] = m.img.slice[si];
            d.fillDir = m.img.fillDir;
            d.fill = m.img.fill; d.valint = m.img.valint;
            d.hide = m.img.hide; d.blend = m.img.blend; d.nobreak = m.img.nobreak;
            d.coexist = m.coexist;
            d.repeat = m.img.repeat;
            d.numValint = m.num.valint;
            snprintf(d.metaCreator, sizeof(d.metaCreator), "%s", m.metaCreator);
            snprintf(d.metaTags, sizeof(d.metaTags), "%s", m.metaTags);
            snprintf(d.metaClient, sizeof(d.metaClient), "%s", m.metaClient);
            d.metaTime = m.metaTime; d.metaDisabled = m.metaDisabled; d.metaHide = m.metaHide;
            d.valF = m.val.valF; d.valMin = m.val.valMin; d.valMax = m.val.valMax;
            snprintf(d.valS, sizeof(d.valS), "%s", m.val.valS);
            d.valB = m.val.valB;
            snprintf(d.snd, sizeof(d.snd), "%s", m.snd.file);
            d.sndLoop = m.snd.loop; d.sndVol = m.snd.volume; d.sndHit = m.snd.hitsound;
            d.animN = 0;
            for (size_t ai = 0; ai < m.anims.size() && ai < 8; ai++)
            {
                const AnimSpec& a = m.anims[ai];
                RoleAnim& da = d.anims[d.animN++];
                da.type = a.type; da.startMs = a.startMs; da.endMs = a.endMs;
                da.from0 = a.from0; da.from1 = a.from1; da.to0 = a.to0; da.to1 = a.to1;
                da.repeat = a.repeat; da.repeatType = a.repeatType;
                da.delay = a.delay; da.ease = a.ease; da.nid = a.nid;
            }
            if (m.img.frames > 1 && m.img.file[0])
            {
                // 首帧号以种子文件为准（Phigros phira-effect-1.png → 1，Rurudo hits-1.png → 1）
                int si = SeedFrameIndex(m.img.file, m.img.base);
                if (si > 0) d.start = si;
            }
            if (!m.img.file[0] && !m.images.empty())
            {
                // 只有图像列表的模块（Judge(7)）：模块几何 = images[0]
                // 逆向：SkinModuleJudge 同样走 ApplyImageSize，参数取自 images[i]（w/h/wu/hu）
                const ImgSpec& im0 = m.images[0];
                d.w = im0.w; d.h = im0.h; d.wu = im0.wu; d.hu = im0.hu;
            }
            snprintf(d.text, sizeof(d.text), "%s", m.txt.text);
            d.size = m.txt.size;
            d.font = m.txt.font;
            if (!d.color[0]) snprintf(d.color, sizeof(d.color), "%s", m.txt.color);
            snprintf(d.numFile, sizeof(d.numFile), "%s", m.num.file);
            snprintf(d.numBase, sizeof(d.numBase), "%s", m.num.base);
            snprintf(d.numText, sizeof(d.numText), "%s", m.num.text);
            d.numH = m.num.h; d.numPad = m.num.pad;
            if (m.type == 5003) d.start = m.num.start;      // 只有数字模块才用字形起始号
            for (size_t i = 0; i < m.noteImgs.size() && i < 6; i++)
                snprintf(d.noteImg[i], sizeof(d.noteImg[i]), "%s", m.noteImgs[i].file);
            d.noteN = (int)(m.noteImgs.size() > 6 ? 6 : m.noteImgs.size());
            for (const Cond& c : m.scenes)
            {
                if (d.sceneN < 4)
                {
                    d.scene[d.sceneN].source = c.source;
                    d.scene[d.sceneN].flag = c.flag;
                    d.scene[d.sceneN].val = (c.valdbl != 0.0) ? (float)c.valdbl : (float)c.valint;
                    snprintf(d.scene[d.sceneN].valstr, sizeof(d.scene[d.sceneN].valstr), "%s", c.valstr);
                    d.sceneN++;
                }
                if (c.source == 1 && c.valint >= 1 && c.valint <= kLanes && d.lane < 0)
                    d.lane = c.valint - 1;
                else if (c.source == 14 && d.judgeScene < 0)
                    d.judgeScene = c.valint;
                else if (c.source == 7 && d.noteType < 0)
                    d.noteType = c.valint;
                if (c.source == 11)
                    d.arrow = c.valint;                    // Arrow=11（NoteArrowToInt：0 Static/1 Left/2 Up/3 Right）
            }
            d.judgeScene = JudgeScene(m);
            for (const Cond& c : m.triggers)
            {
                if (d.trigN < 6)
                {
                    d.trig[d.trigN].source = c.source;
                    d.trig[d.trigN].flag = c.flag;
                    d.trig[d.trigN].val = (c.valdbl != 0.0) ? (float)c.valdbl : (float)c.valint;
                    snprintf(d.trig[d.trigN].valstr, sizeof(d.trig[d.trigN].valstr), "%s", c.valstr);
                    d.trigN++;
                }
            }
            r->modCount++;
        }
        r->hasModules = (r->modCount > 0);
    }

    static void Classify(std::vector<ModSpec>& mods, Roles* r)
    {
        BuildMods(mods, r);
        for (ModSpec& m : mods)
        {
            if (m.usage != 99 || m.type != 5000) continue;
            const char* f = m.img.file;
            if (!f[0]) continue;
            if (EqI(m.desc, "rrdbg") && !r->charImg[0]) { CopyStr(r->charImg, 96, f); continue; }
            if (!r->bg[0] && HasI(f, "bg") && m.img.w >= 100.f && m.img.h >= 100.f && m.layer <= 2)
                CopyStr(r->bg, 96, f);
        }
        for (ModSpec& m : mods)
        {
            if (r->charImg[0]) break;
            if (m.usage != 99 || m.type != 5000) continue;
            const char* f = m.img.file;
            if (!f[0]) continue;
            if (m.img.h >= 250.f && m.x >= 50.f && m.layer <= 1 && !HasI(f, "bg") && !HasI(f, "playerbg"))
                CopyStr(r->charImg, 96, f);
        }
        char charStem[64]{};
        if (r->charImg[0])
        {
            const char* dash = strchr(r->charImg, '-');
            size_t n = dash ? (size_t)(dash - r->charImg) : strlen(r->charImg);
            char* dot = strrchr(r->charImg, '.');
            if (dot && (!dash || dot < dash)) n = (size_t)(dot - r->charImg);
            if (n > 0 && n < sizeof(charStem))
            {
                memcpy(charStem, r->charImg, n);
                charStem[n] = 0;
            }
        }
        for (ModSpec& m : mods)
        {
            if (m.usage == 1)
            {
                int lane = SceneLane(m);
                if (m.type == 6 || m.type == 3 || m.type == 4 || m.type == 5)
                {
                    if (!m.noteImgs.empty()) SetLane(r->holdHead, lane, m.noteImgs[0].file);
                    if (m.noteImgs.size() > 1) SetLane(r->holdBody, lane, m.noteImgs[1].file);
                    if (m.noteImgs.size() > 2) SetLane(r->holdTail, lane, m.noteImgs[2].file);
                    if (!r->holdBody[0][0] && !m.noteImgs.empty())
                        SetLane(r->holdBody, lane, m.noteImgs[0].file);
                }
                else if (m.type == 1 || m.type == 2 || m.type == 7 || m.type == 8 || m.type == 9)
                {
                    // Arrow=11 场景：Malody 键模式音符恒为 Static(0)（用户可打击的只有 Tap/Hold，
                    // 方向箭头属于 Slide/Cube 模式，见 dump.cs bbbg.ArrowDirection + bec::NoteArrowToInt）。
                    // 带方向的箭头模块不得当作普通音符图（否则所有音符会显示某个方向箭头）。
                    if (!m.noteImgs.empty() && m.arrow <= 0) SetLane(r->note, lane, m.noteImgs[0].file);
                }
                continue;
            }
            if (m.usage == 7)
            {
                for (size_t i = 0; i < m.images.size() && i < kJudgeBarN; i++)
                {
                    const ImgSpec& im = m.images[i];
                    if (!im.file[0]) continue;
                    if (im.frames > 1)
                    {
                        CopyStr(r->judgeAnimSeed[i], 96, im.file);
                        if (im.base[0]) CopyStr(r->judgeAnimBase[i], 96, im.base);
                        else DeriveBase(im.file, r->judgeAnimBase[i], 96);
                        r->judgeAnimCount[i] = im.frames;
                        r->judgeAnimStart[i] = SeedFrameIndex(im.file, r->judgeAnimBase[i]);
                        r->judgeAnimFps[i] = im.fps > 0 ? im.fps : 60;
                    }
                    else if (!r->judgeBar[i][0])
                        CopyStr(r->judgeBar[i], 96, im.file);
                }
                continue;
            }
            if (m.usage != 99) continue;

            if (m.type == 5000)
            {
                const char* f = m.img.file;
                if (!f[0]) continue;
                if (HasTrig(m, 7))
                {
                    int lane = SceneLane(m);
                    bool charStyle = charStem[0] && HasI(f, charStem);
                    if (charStyle && lane >= 0 && lane < 4) CopyStr(r->charPress[lane], 96, f);
                    else SetLane(r->press, lane, f);
                    continue;
                }
                int jsc = JudgeScene(m);
                if (jsc >= 0) { if (!r->judgePop[jsc][0]) CopyStr(r->judgePop[jsc], 96, f); continue; }
                if (!r->line[0] && (HasI(f, "noteinex") || HasI(m.desc, "noteline") || HasI(m.desc, "judgeline")))
                    CopyStr(r->line, 96, f);
                else if (!r->grid[0] && (HasI(f, "grid") || HasI(m.desc, "wangge")))
                    CopyStr(r->grid, 96, f);
                else if (!r->hitBg[0] && (HasI(f, "hitbg") || HasI(m.desc, "hitbg")))
                    CopyStr(r->hitBg, 96, f);
                else if (!r->judgeColor[0] && (HasI(f, "judgercolor") || HasI(f, "judgecolor")))
                    CopyStr(r->judgeColor, 96, f);
                else if (!r->avatar[0] && (HasI(f, "progress") || HasI(f, "avatar")))
                    CopyStr(r->avatar, 96, f);
                else if (!r->logo[0] && HasI(f, "logo"))
                    CopyStr(r->logo, 96, f);
                continue;
            }
            if (m.type == 5002 && HasTrig(m, 15))
            {
                if (!r->hitFxBase[0] && !r->hitFxSeed[0])
                {
                    CopyStr(r->hitFxSeed, 96, m.img.file);
                    if (m.img.base[0]) CopyStr(r->hitFxBase, 96, m.img.base);
                    else if (m.img.file[0]) DeriveBase(m.img.file, r->hitFxBase, 96);
                    r->hitFxStart = SeedFrameIndex(m.img.file, r->hitFxBase);
                    r->hitFxCount = m.img.frames;
                    r->hitFxFps = m.img.fps;
                }
                continue;
            }
            if (m.type == 5003)
            {
                const char* base = m.num.base[0] ? m.num.base : "";
                char derived[64] = {};
                if (!base[0] && m.num.file[0]) { DeriveBase(m.num.file, derived, sizeof(derived)); base = derived; }
                if (!r->comboBase[0] && (HasI(m.num.text, "{combo}") || HasI(base, "combo")))
                {
                    CopyStr(r->comboBase, 64, base);
                    r->comboStart = m.num.start;
                    r->comboHeight = m.num.h;
                }
                else if (!r->accBase[0] && (HasI(m.num.text, "{acc}") || HasI(base, "acc")))
                {
                    CopyStr(r->accBase, 64, base);
                    r->accStart = m.num.start;
                    r->accHeight = m.num.h;
                }
                continue;
            }
        }
        if (!r->cheek[0] && r->judgePop[0][0]) CopyStr(r->cheek, 96, r->judgePop[0]);
        MarkValid(r);
    }
    static bool JsonStr(const char* json, const char* key, char* out, int n)
    {
        out[0] = 0;
        if (!json || !key) return false;
        char pat[64];
        snprintf(pat, sizeof(pat), "\"%s\"", key);
        const char* p = strstr(json, pat);
        if (!p) return false;
        p = strchr(p + strlen(pat), ':');
        if (!p) return false;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p != '"') return false;
        p++;
        int i = 0;
        while (*p && *p != '"' && i < n - 1)
        {
            if (*p == '\\' && p[1]) p++;
            out[i++] = *p++;
        }
        out[i] = 0;
        return true;
    }

    static bool ReadAllBytes(const char* path, std::vector<unsigned char>& out)
    {
        FILE* fp = nullptr;
        if (fopen_s(&fp, path, "rb") != 0 || !fp) return false;
        fseek(fp, 0, SEEK_END);
        long n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (n <= 0 || n > 8 * 1024 * 1024) { fclose(fp); return false; }
        out.resize((size_t)n);
        size_t rd = fread(out.data(), 1, (size_t)n, fp);
        fclose(fp);
        return rd == (size_t)n;
    }

    struct MetaOut
    {
        float keyScale = 0.f, keyAngle = 0.f, ringDis = 0.f;
        float taikoScale = 0.f, taikoJudgeY = 0.f;
        int   keyKeys = 0, keyJudgePos = 0;
        int   keyUse3D = 0, keyLock3D = 0, keyLockScale = 0;
        int   taikoJudgePos = 0;
        char  script[96] = {};
        char  cover[96] = {};                         // Meta field 4
        char  desc[128] = {};                         // Meta field 2
        int   client = 0;                             // Meta field 6
        int   minver = 0, feature = 0, freeFlag = 0, skinid = 0;   // Meta 24/25/26/27
        long long updated = 0;                        // Meta field 28
        int   keyLocked = 0;                          // MetaModeKey 1
        int   ringLockDis = 0;                        // MetaModeRing 1
    };
    static void ParseMetaPbs(Pb& pb, char* title, int tn, char* creator, int cn, int* modeId,
                             MetaOut* mo = nullptr)
    {
        uint32_t fn, wt;
        while (pb.next(fn, wt))
        {
            if (fn != 1 || wt != 2) { pb.skip(wt); continue; }
            const unsigned char* d; size_t n;
            if (!pb.bytes(&d, &n)) break;
            Pb sub(d, n); uint32_t f2, w2;
            while (sub.next(f2, w2))
            {
                if ((f2 == 1 || f2 == 2 || f2 == 3 || f2 == 4) && w2 == 2)
                {
                    const unsigned char* dd; size_t nn;
                    if (!sub.bytes(&dd, &nn)) break;
                    if (f2 == 1 && title && tn > 0) snprintf(title, (size_t)tn, "%.*s", (int)nn, (const char*)dd);
                    if (f2 == 2 && mo) snprintf(mo->desc, sizeof(mo->desc), "%.*s", (int)nn, (const char*)dd);
                    if (f2 == 3 && creator && cn > 0) snprintf(creator, (size_t)cn, "%.*s", (int)nn, (const char*)dd);
                    if (f2 == 4 && mo) snprintf(mo->cover, sizeof(mo->cover), "%.*s", (int)nn, (const char*)dd);
                }
                else if (f2 == 5 && w2 == 0)
                {
                    uint64_t v; sub.varint(v);
                    if (modeId) *modeId = (int)v;
                }
                else if (f2 == 7 && w2 == 2)
                {
                    // Meta.Key（oneof perMode）：4=Keys(varint) 5=Scale(fixed32 float) 8=JudgePos(varint)
                    const unsigned char* kd; size_t kn;
                    if (!sub.bytes(&kd, &kn)) break;
                    Pb kb(kd, kn); uint32_t f3, w3;
                    while (kb.next(f3, w3))
                    {
                        if (f3 == 1 && w3 == 0) { uint64_t v; kb.varint(v); if (mo) mo->keyLocked = (int)v; }
                        // 依据 dump.cs MetaModeKey：angle_ 是 int（field 2），protobuf 编码 = varint。
                        // 旧实现误按 fixed32 解析，导致所有 3D 皮肤 angle 恒为 0（斜轨失效）。
                        else if (f3 == 2 && w3 == 0) { uint64_t v; kb.varint(v); if (mo) mo->keyAngle = (float)v; }
                        else if (f3 == 3 && w3 == 0) { uint64_t v; kb.varint(v); if (mo) mo->keyUse3D = (int)v; }
                        else if (f3 == 4 && w3 == 0) { uint64_t v; kb.varint(v); if (mo) mo->keyKeys = (int)v; }
                        else if (f3 == 5 && w3 == 5)
                        {
                            uint32_t v; kb.fixed32(v);
                            float fv = 0.f; memcpy(&fv, &v, 4);
                            if (mo) mo->keyScale = fv;
                        }
                        else if (f3 == 6 && w3 == 0) { uint64_t v; kb.varint(v); if (mo) mo->keyLock3D = (int)v; }
                        else if (f3 == 7 && w3 == 0) { uint64_t v; kb.varint(v); if (mo) mo->keyLockScale = (int)v; }
                        else if (f3 == 8 && w3 == 0) { uint64_t v; kb.varint(v); if (mo) mo->keyJudgePos = (int)v; }
                        else kb.skip(w3);
                    }
                }
                else if (f2 == 8 && w2 == 2)      // Meta.Mode.Ring：2 dis
                {
                    const unsigned char* rd; size_t rn;
                    if (!sub.bytes(&rd, &rn)) break;
                    Pb rb(rd, rn); uint32_t f3, w3;
                    while (rb.next(f3, w3))
                    {
                        if (f3 == 1 && w3 == 0) { uint64_t v; rb.varint(v); if (mo) mo->ringLockDis = (int)v; }
                        else if (f3 == 2 && w3 == 5)
                        {
                            uint32_t v; rb.fixed32(v);
                            float fv = 0.f; memcpy(&fv, &v, 4);
                            if (mo) mo->ringDis = fv;
                        }
                        else rb.skip(w3);
                    }
                }
                else if (f2 == 9 && w2 == 2)      // Meta.Mode.Taiko：1 scale · 2 judgePos · 3 judgeY
                {
                    const unsigned char* td; size_t tnn;
                    if (!sub.bytes(&td, &tnn)) break;
                    Pb tb(td, tnn); uint32_t f3, w3;
                    while (tb.next(f3, w3))
                    {
                        if (f3 == 1 && w3 == 5)
                        {
                            uint32_t v; tb.fixed32(v);
                            float fv = 0.f; memcpy(&fv, &v, 4);
                            if (mo) mo->taikoScale = fv;
                        }
                        else if (f3 == 2 && w3 == 0) { uint64_t v; tb.varint(v); if (mo) mo->taikoJudgePos = (int)v; }
                        else if (f3 == 3 && w3 == 5)
                        {
                            uint32_t v; tb.fixed32(v);
                            float fv = 0.f; memcpy(&fv, &v, 4);
                            if (mo) mo->taikoJudgeY = fv;
                        }
                        else tb.skip(w3);
                    }
                }
                else if (f2 == 23 && w2 == 2)     // Meta.script（皮肤 Lua 入口）
                {
                    const unsigned char* sd; size_t sn;
                    if (!sub.bytes(&sd, &sn)) break;
                    if (mo) snprintf(mo->script, sizeof(mo->script), "%.*s", (int)sn, (const char*)sd);
                }
                else if (f2 == 24 && w2 == 0) { uint64_t v; sub.varint(v); if (mo) mo->minver = (int)v; }
                else if (f2 == 6 && w2 == 0) { uint64_t v; sub.varint(v); if (mo) mo->client = (int)v; }
                else if (f2 == 25 && w2 == 0) { uint64_t v; sub.varint(v); if (mo) mo->feature = (int)v; }
                else if (f2 == 26 && w2 == 0) { uint64_t v; sub.varint(v); if (mo) mo->freeFlag = (int)v; }
                else if (f2 == 27 && w2 == 0) { uint64_t v; sub.varint(v); if (mo) mo->skinid = (int)v; }
                else if (f2 == 28 && w2 == 0) { uint64_t v; sub.varint(v); if (mo) mo->updated = (long long)v; }
                else sub.skip(w2);
            }
        }
    }

    void ReadMeta(const char* dir, char* title, int tn, char* creator, int cn, int* modeId)
    {
        if (title && tn) title[0] = 0;
        if (creator && cn) creator[0] = 0;
        if (modeId) *modeId = -1;
        char p[MAX_PATH * 2];
        snprintf(p, sizeof(p), "%s\\info.asm", dir);
        std::vector<unsigned char> buf;
        if (ReadAllBytes(p, buf))
        {
            Pb pb(buf.data(), buf.size());
            ParseMetaPbs(pb, title, tn, creator, cn, modeId);
        }
        if ((title && !title[0]) || (creator && !creator[0]))
        {
            snprintf(p, sizeof(p), "%s\\info.json", dir);
            if (ReadAllBytes(p, buf))
            {
                std::string js((const char*)buf.data(), buf.size());
                if (title && tn && !title[0]) JsonStr(js.c_str(), "title", title, tn);
                if (creator && cn && !creator[0]) JsonStr(js.c_str(), "creator", creator, cn);
            }
        }
    }

    bool LooksLikeSkin(const char* dir)
    {
        if (!dir || !dir[0] || !DirExistsPath(dir)) return false;
        if (HasFileIn(dir, "info.asm") || HasFileIn(dir, "info.json") || HasFileIn(dir, "roles.ini")) return true;
        if (HasFileIn(dir, "rurudokey.png") || HasFileIn(dir, "notex-1.png")) return true;
        char pat[MAX_PATH * 2];
        snprintf(pat, sizeof(pat), "%s\\key-note-*.png", dir);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) { FindClose(h); return true; }
        return false;
    }
    static void LoadOldFormat(const char* dir, Roles* out)
    {
        char pat[MAX_PATH * 2];
        WIN32_FIND_DATAA fd;
        std::vector<std::string> notes, holds, hitfx;
        snprintf(pat, sizeof(pat), "%s\\key-note-*.png", dir);
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do { if (!HasI(fd.cFileName, "hold")) notes.push_back(fd.cFileName); } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        snprintf(pat, sizeof(pat), "%s\\key-note-hold-*.png", dir);
        h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do { holds.push_back(fd.cFileName); } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        snprintf(pat, sizeof(pat), "%s\\key-hitlight-*.png", dir);
        h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do { hitfx.push_back(fd.cFileName); } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        if (!notes.empty())
            for (int i = 0; i < kLanes; i++)
                CopyStr(out->note[i], 96, notes[(size_t)i % notes.size()].c_str());
        if (!holds.empty())
            for (int i = 0; i < kLanes; i++)
                CopyStr(out->holdBody[i], 96, holds[(size_t)i % holds.size()].c_str());
        if (!hitfx.empty())
        {
            for (size_t i = 0; i + 1 < hitfx.size(); i++)
                for (size_t j = i + 1; j < hitfx.size(); j++)
                {
                    const char* a = strrchr(hitfx[i].c_str(), '-');
                    const char* b = strrchr(hitfx[j].c_str(), '-');
                    if (a && b && atoi(a + 1) > atoi(b + 1)) std::swap(hitfx[i], hitfx[j]);
                }
            CopyStr(out->hitFxSeed, 96, hitfx[0].c_str());
            const char* dash = strrchr(hitfx[0].c_str(), '-');
            if (dash)
            {
                size_t n = (size_t)(dash - hitfx[0].c_str()) + 1;
                if (n < 96) { memcpy(out->hitFxBase, hitfx[0].c_str(), n); out->hitFxBase[n] = 0; }
                out->hitFxStart = atoi(dash + 1);
            }
            out->hitFxCount = (int)hitfx.size();
            out->hitFxFps = 45;
        }
        MarkValid(out);
    }

    bool Load(const char* dir, Roles* out)
    {
        if (!dir || !out) return false;
        // 用 NSDMI 默认值初始化（lane/judgeScene/noteType = -1 是"未绑定"哨兵，
        // 早先的 memset 会把这些 -1 抹成 0，导致所有模块被当成轨道子物体、x 全部按轨道宽度算）
        *out = Roles{};
        out->modeId = -1;
        char p[MAX_PATH * 2];
        snprintf(p, sizeof(p), "%s\\info.asm", dir);
        std::vector<unsigned char> buf;
        if (ReadAllBytes(p, buf))
        {
            Pb top(buf.data(), buf.size());
            std::vector<ModSpec> mods;
            uint32_t fn, wt;
            while (top.next(fn, wt))
            {
                if (fn == 1 && wt == 2)
                {
                    const unsigned char* d; size_t n;
                    if (!top.bytes(&d, &n)) break;
                }
                else if (fn == 2)                       // Disabled（repeated int，可能 packed）
                {
                    if (wt == 0) { uint64_t v; top.varint(v); out->disabledN++; }
                    else if (wt == 2)
                    {
                        const unsigned char* d; size_t n;
                        if (!top.bytes(&d, &n)) break;
                        Pb pb2(d, n); uint64_t v;
                        while (pb2.p < pb2.e) { if (!pb2.varint(v)) break; out->disabledN++; }
                    }
                    else top.skip(wt);
                }
                else if (fn == 4 && wt == 2)            // Settings：ComposerSetting 1 hideDef · 2 hideImg · 3 usageFilter
                {
                    const unsigned char* d; size_t n;
                    if (!top.bytes(&d, &n)) break;
                    Pb sb(d, n); uint32_t f2, w2;
                    while (sb.next(f2, w2))
                    {
                        uint64_t v;
                        if (f2 == 1 && w2 == 0) { sb.varint(v); out->setHideDef = (int)v; }
                        else if (f2 == 2 && w2 == 0) { sb.varint(v); out->setHideImg = (int)v; }
                        else if (f2 == 3 && w2 == 0) { sb.varint(v); out->setUsageFilter = (int)v; }
                        else sb.skip(w2);
                    }
                }
                else if (fn == 5 && wt == 2)            // Defs：ModuleDef 1 type · 2 param
                {
                    const unsigned char* d; size_t n;
                    if (!top.bytes(&d, &n)) break;
                    Pb db(d, n); uint32_t f2, w2;
                    const int di = out->defsN;
                    bool defSeen = false;
                    while (db.next(f2, w2))
                    {
                        if (f2 == 1 && w2 == 0)
                        {
                            uint64_t v; db.varint(v);
                            if (di < 16) out->defsType[di] = (int)v;
                            defSeen = true;
                        }
                        else if (f2 == 2 && w2 == 2 && di < 16)
                        {
                            const unsigned char* pd; size_t pn;
                            if (!db.bytes(&pd, &pn)) break;
                            Pb pb2(pd, pn); uint32_t f3, w3;
                            while (pb2.next(f3, w3))
                            {
                                uint64_t v;
                                if (f3 == 2 && w3 == 0) { pb2.varint(v); out->defsPivot[di] = (int)v; }
                                else if (f3 == 3 && w3 == 0) { pb2.varint(v); out->defsRotate[di] = (int)v; }
                                else if (f3 == 4 && w3 == 5) { uint32_t fv = 0; pb2.fixed32(fv); out->defsX[di] = ToF32(fv); }
                                else if (f3 == 5 && w3 == 5) { uint32_t fv = 0; pb2.fixed32(fv); out->defsY[di] = ToF32(fv); }
                                else if (f3 == 6 && w3 == 0) { pb2.varint(v); out->defsAlpha[di] = (int)v; }
                                else if (f3 == 7 && w3 == 0) { pb2.varint(v); out->defsLayer[di] = (int)v; }
                                else if (f3 == 8 && w3 == 0) { pb2.varint(v); out->defsOrder[di] = (int)v; }
                                else pb2.skip(w3);
                            }
                            defSeen = true;
                        }
                        else db.skip(w2);
                    }
                    if (defSeen && di < 16) out->defsN = di + 1;
                }
                // field 2 = Disabled: composer-side metadata, NOT a runtime filter.
                // Evidence: built-in Rurudo marks modules 1-6/8-15 (logo, circles,
                // countdown bg, pause, rrd poses, title/bpm/ver/mod texts) disabled
                // yet Malody renders every one of them; Malody_Gazer marks its only
                // two Note modules (index 9/10) disabled and still draws notes.
                else if (fn == 3 && wt == 2)
                {
                    const unsigned char* d; size_t n;
                    if (!top.bytes(&d, &n)) break;
                    Pb sub(d, n); ModSpec m; ParseMod(sub, m);
                    mods.push_back(std::move(m));
                }
                else top.skip(wt);
            }
            {
                Pb mb(buf.data(), buf.size());
                MetaOut mo;
                ParseMetaPbs(mb, out->title, sizeof(out->title), out->creator, sizeof(out->creator), &out->modeId,
                             &mo);
                out->keyScale = mo.keyScale; out->keyKeys = mo.keyKeys; out->keyJudgePos = mo.keyJudgePos;
                out->keyAngle = mo.keyAngle; out->keyUse3D = mo.keyUse3D;
                out->keyLock3D = mo.keyLock3D; out->keyLockScale = mo.keyLockScale;
                out->ringDis = mo.ringDis;
                out->taikoScale = mo.taikoScale; out->taikoJudgePos = mo.taikoJudgePos; out->taikoJudgeY = mo.taikoJudgeY;
                snprintf(out->script, sizeof(out->script), "%s", mo.script);
                snprintf(out->cover, sizeof(out->cover), "%s", mo.cover);
                snprintf(out->desc, sizeof(out->desc), "%s", mo.desc);
                out->minver = mo.minver; out->feature = mo.feature; out->freeFlag = mo.freeFlag;
                out->skinid = mo.skinid; out->updated = mo.updated;
                out->client = mo.client; out->keyLocked = mo.keyLocked; out->ringLockDis = mo.ringLockDis;
            }
            Classify(mods, out);
        }
        if (!out->valid)
            LoadOldFormat(dir, out);
        return out->valid;
    }
    static void SanitizeName(const wchar_t* src, char* out, int n)
    {
        const wchar_t* base = src;
        for (const wchar_t* p = src; *p; p++)
            if (*p == L'\\' || *p == L'/') base = p + 1;
        char u8[MAX_PATH * 2]{};
        WideCharToMultiByte(CP_UTF8, 0, base, -1, u8, sizeof(u8), nullptr, nullptr);
        char* dot = strrchr(u8, '.');
        if (dot) *dot = 0;
        int i = 0;
        for (const char* p = u8; *p && i < n - 1; p++)
        {
            unsigned char c = (unsigned char)*p;
            if (isalnum(c) || c == '_' || c == '-') out[i++] = (char)c;
            else out[i++] = '_';
        }
        out[i] = 0;
        if (i == 0) snprintf(out, (size_t)n, "msp_skin");
    }

    static bool RunHidden(const wchar_t* cmdline, DWORD timeoutMs)
    {
        std::vector<wchar_t> buf(cmdline, cmdline + wcslen(cmdline) + 1);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            return false;
        DWORD r = WaitForSingleObject(pi.hProcess, timeoutMs);
        DWORD code = 1;
        if (r == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
        else TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return r == WAIT_OBJECT_0 && code == 0;
    }

    static bool ExtractZip(const wchar_t* msp, const wchar_t* dst)
    {
        wchar_t cmd[32768];
        swprintf(cmd, 32768, L"cmd.exe /c tar -xf \"%ls\" -C \"%ls\"", msp, dst);
        if (RunHidden(cmd, 60000)) return true;
        std::wstring src = msp, d = dst;
        auto esc = [](const std::wstring& s) {
            std::wstring o;
            for (wchar_t c : s) { o.push_back(c); if (c == L'\'') o.push_back(L'\''); }
            return o;
        };
        std::wstring ps = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" +
                          esc(src) + L"' -DestinationPath '" + esc(d) + L"' -Force\"";
        return RunHidden(ps.c_str(), 120000);
    }

    static bool MoveTreeUp(const char* dir)
    {
        char pat[MAX_PATH * 2];
        snprintf(pat, sizeof(pat), "%s\\*", dir);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) return false;
        char sub[MAX_PATH * 2] = {};
        int dirs = 0, files = 0;
        do {
            if (fd.cFileName[0] == '.') continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { dirs++; if (!sub[0]) snprintf(sub, sizeof(sub), "%s\\%s", dir, fd.cFileName); }
            else files++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        if (dirs != 1 || files != 0) return false;
        snprintf(pat, sizeof(pat), "%s\\*", sub);
        h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) return false;
        do {
            if (fd.cFileName[0] == '.') continue;
            char from[MAX_PATH * 2], to[MAX_PATH * 2];
            snprintf(from, sizeof(from), "%s\\%s", sub, fd.cFileName);
            snprintf(to, sizeof(to), "%s\\%s", dir, fd.cFileName);
            MoveFileExA(from, to, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        RemoveDirectoryA(sub);
        return true;
    }

    bool ImportArchive(const wchar_t* mspPath, char* outDir, int outDirLen, char* err, int errLen)
    {
        if (err && errLen) err[0] = 0;
        if (outDir && outDirLen) outDir[0] = 0;
        if (!mspPath || !mspPath[0]) { if (err && errLen) snprintf(err, (size_t)errLen, "no path"); return false; }
        char root[MAX_PATH * 2];
        DefaultRoot(root, sizeof(root));
        if (!DirExistsPath(root)) CreateDirectoryA(root, nullptr);
        char name[128]{};
        SanitizeName(mspPath, name, sizeof(name));
        char dst[MAX_PATH * 2];
        snprintf(dst, sizeof(dst), "%s\\%s", root, name);
        for (int i = 2; i < 100 && DirExistsPath(dst); i++)
            snprintf(dst, sizeof(dst), "%s\\%s_%d", root, name, i);
        CreateDirectoryA(dst, nullptr);

        wchar_t wdst[MAX_PATH * 2];
        MultiByteToWideChar(CP_UTF8, 0, dst, -1, wdst, MAX_PATH * 2);
        if (!ExtractZip(mspPath, wdst))
        {
            if (err && errLen) snprintf(err, (size_t)errLen, "extract failed");
            return false;
        }
        char ia[MAX_PATH * 2], ij[MAX_PATH * 2], key[MAX_PATH * 2];
        snprintf(ia, sizeof(ia), "%s\\info.asm", dst);
        snprintf(ij, sizeof(ij), "%s\\info.json", dst);
        if (!FileExistsPath(ia) && !FileExistsPath(ij))
            MoveTreeUp(dst);
        snprintf(key, sizeof(key), "%s\\key-note-1.png", dst);
        if (!FileExistsPath(ia) && !FileExistsPath(ij) && !FileExistsPath(key))
        {
            if (err && errLen) snprintf(err, (size_t)errLen, "not a skin package");
            return false;
        }
        if (outDir && outDirLen) snprintf(outDir, (size_t)outDirLen, "%s", dst);
        return true;
    }
}
