#pragma once
// SkinLua.h - Malody MSP skin Lua sandbox (see SkinLua.cpp for reverse-engineering notes)
#include "SkinMsp.h"

namespace SkinLua
{
    struct Env
    {
        float  windowW = 1920.f;      // Game:Width() (real window px)
        float  windowH = 1080.f;      // Game:Height()
        float  sceneScale = 1.f;      // Game:SceneScale() = Meta.Key.scale
        int    trackAngle = 0;        // Game:TrackAngle()
        double audioLengthMs = 0.0;   // Game:AudioLength()
        double startTimeMs = 0.0;     // Game:StartTime()
        int    noteCount = 0;         // Game:ChartInfo("Note")
        int    columns = 4;           // Game:ChartInfo("Key")：本工具下落列数（脚本决定键位布局）
        int    level = 0;             // 本工具估算等级（diff 文本用）
        int    bpmCount = 0;          // Game:BpmCount()
        const float* bpmTimes = nullptr;
        const float* bpmVals = nullptr;
        float  bpm = 0.f;
        const char* levelName = "";
        const char* judgeName = "NORMAL";   // {judge}：HARD/NORMAL/EASY（判定设置）
    };

    struct Live
    {
        double songMs = 0.0;
        int    combo = 0, maxCombo = 0;
        int    counts[4] = {};
        double acc = 1.0;
        float  hp = 1.f, progress = 0.f, score = 0.f;
        int    kps = 0, kpsMax = 0;
        bool   inPlay = false;
    };

    bool Load(const char* skinDir, const char* scriptFile,
              const SkinMsp::RoleMod* mods, int modCount, const Env& env);
    void Unload();
    bool Active();
    bool HasOnHit();        // 脚本是否定义了 OnHit（引擎据此判断打击特效由脚本负责）

    void Frame(const Live& live);

    struct Patch
    {
        bool  x = false, y = false, w = false, h = false, alpha = false, rotate = false, fill = false;
        float xv = 0, yv = 0, wv = 0, hv = 0;      // design px (1080 base)
        int   av = 100, rv = 0;
        float fillv = 0.f;
        bool  color = false;                       // SetColor(r,g,b)/SetColor{t}：命中特效/判定线染色
        int   cr = 255, cg = 255, cb = 255;
        const char* text = nullptr;
    };

    void Eval(int modIdx, double songMs, Patch* out);
    // 脚本是否真正改写过该模块（位置/尺寸/透明度/文本/动画）→ 引擎不应再用
    // 「同坐标备用图组只画一张」等名称/几何启发式覆盖脚本决定。
    bool Touched(int modIdx);

    struct Extra
    {
        int    srcMod = -1;
        Patch  patch;
        bool   playing = false;
        double playT0 = 0.0;
        bool   shadow = false;
    };
    int Extras(Extra* out, int maxN);

    void PushHit(int kind, double offsetMs);
    void PushInput(int type, int hitx);

    const char* LastError();
}
