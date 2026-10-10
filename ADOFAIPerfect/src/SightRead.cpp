// ============================================================
// SightRead.cpp — 辅助读谱（无轨）
//   不改谱、不代打：把"何时按 / 往哪转 / 转多少 / 这串几拍 / 特殊砖"
//   显式画出来。三种布局：侧栏面板 / 底部横条 / 顶部横条（打歌时不占用
//   视线离开判定区的横向方案）。
// ============================================================
#include "ChartCore.h"
#include "Lang.h"
#include "CheatState.h"
#include "GameBridge.h"
#include "RenderHook.h"
#include "StreamMode.h"
#include "imgui.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>
#include <algorithm>

namespace Chart4K
{
    // ============================================================
    // 辅助读谱（无轨）——数据层
    //
    // 冰与火「读谱难」的根因（反编译 + 社区资料核实，勿猜）：
    //   ① 节奏写在几何里（核心难点）：行星角速度恒为 π/拍
    //      （scrPlanet.Update_RefreshAngles: angle = snapped + Δt/beat·π·speed），
    //      一块砖要扫过的角是 angleLength（scrLevelMaker）：
    //        普通砖 angleLength = π + 转角 ⇒ 间隔(拍) = 1 + 转角/180°
    //      于是 90°=1.5 拍、45°=1.25 拍、180°=2 拍——谱面时值全是附点/
    //      复合节奏，玩家必须把画面几何实时翻译成节拍才能按准；
    //   ② 方向即形状：isCCW 决定顺时针/逆时针公转，连续换向与大回环
    //      时画面会把视线带偏，需要提前知道下一块的转向与转角；
    //   ③ 时值改写：中旋(midspin)/长按(holdLength)/多押(tapsNeeded)/
    //      变速(speed)/BPM 变化会让"1 块 = 1 拍"的直觉失效；
    //   ④ 高速失效：判定窗口本质是固定角度窗（30/45/60°×marginScale，
    //      scrMisc.GetAdjustedAngleBoundaryInDeg），换算成毫秒会随
    //      BPM×speed 线性缩水（150BPM 完美 ±60ms → 300BPM ±30ms），
    //      所以毫秒倒计时/太鼓式时间条在高速下必然越来越没用。
    //
    // 因此本页不代打、不改谱，只把上面几件事显式画出来：
    //   · 行星角度盘：实时角误差 + 固定角度窗（任何 BPM/变速语义一致）
    //   · 拍数谱：每块的"间隔 = 1 + 转角/180°"直接标成 1 / 1.25 / 1.33 /
    //     1.5…拍；圆弧字形（弧长=转角、箭头=方向）配合拍点网格，
    //     把几何读成节奏，高速下形状与节奏点仍然成立
    //   · 换向 / 大回转 / 中旋 / 长按 / 多押 / 变速 的显式预警标记
    // 数据全部来自已反编译确认的字段：entryTime / entryTimePitchAdj /
    // angleLength / isCCW / holdLength / tapsNeeded / midSpin / isFake /
    // auto，不猜结构体。
    // ============================================================
    struct ReadTile
    {
        double t = 0.0;        // 命中时刻（song 秒）
        double dt = 0.0;       // 距上一块（秒）
        float  turnDeg = 0.f;  // 本块回转角（度；长按已扣掉整圈）
        int    dir = 1;        // -1 = 逆时针 CCW / +1 = 顺时针 CW
        float  hold = 0.f;     // 长按持续（秒，仅首块）
        int    taps = 1;       // 多押次数（>1 = 连点砖）
        bool   midSpin = false;
        bool   fake = false;     // isFake：装饰砖（不判定）
        bool   autoPlay = false; // auto：游戏自动打击
        bool   valid = false;    // 需要玩家打击
    };
    // 快照发布：桥接线程整体替换，渲染线程 atomic_load（不拷贝、不加锁）
    static std::atomic<std::shared_ptr<const std::vector<ReadTile>>> s_readSnap;
    static std::atomic<int> s_readTotal{ 0 };
    static std::atomic<int> s_readCur{ 0 };

    // ---- 用户设置（读谱页，与 4K/5K/6K/10K 完全独立，可同时开启）----
    static std::atomic<bool> s_readOn{ false };
    static std::atomic<int>  s_readAhead{ 8 };      // 预告块数
    static std::atomic<int>  s_readLayout{ 1 };     // 0 侧栏 / 1 底部横条（打歌推荐）/ 2 顶部横条
    static std::atomic<int>  s_readAnchor{ 0 };     // 0 左上 / 1 右上 / 2 左下 / 3 右下
    static std::atomic<int>  s_readPosX{ 0 }, s_readPosY{ 0 };   // 锚点内偏移（1080p 基准）
    static std::atomic<int>  s_readScale{ 100 };    // 整体缩放 %
    static std::atomic<int>  s_readDensity{ 100 };  // 时间轴纵向密度 %
    static std::atomic<int>  s_readOpacity{ 232 };  // 不透明度 0-255
    static std::atomic<int>  s_readOffsetMS{ 0 };   // 读谱专用延迟校准
    static std::atomic<double> s_lastSpeed{ 1.0 };  // 最近一次实时行星速度倍率（拍数换算用）
    static std::atomic<bool> s_readGrid{ true };    // 拍点网格
    static std::atomic<bool> s_readDial{ true };    // 转向轨迹盘
    static std::atomic<bool> s_readRhythm{ true };  // 间隔分数（1/4…）
    static std::atomic<bool> s_readMarks{ true };   // 特殊砖标记
    static std::atomic<bool> s_readHint{ true };    // 技术提示
    static std::atomic<bool> s_readAngle{ true };   // 角度判定条（实时，按游戏的判定角）
    static std::atomic<bool> s_readWindows{ true }; // 判定窗口分带（纯/准/有效）
    static std::atomic<bool> s_readAdapt{ true };   // 高速自动简化

    // 地砖 → 读谱砖（保留 中旋/装饰/自动 的区别，供读谱页标注）
    void BuildReadTiles(const RawFloor* fl, int n)
    {
        auto tiles = std::make_shared<std::vector<ReadTile>>();
        if (n > 0)
            tiles->reserve((size_t)n);
        const double kPi = 3.14159265358979323846;
        double prev = -1e9;
        for (int i = 0; i < n; i++)
        {
            const RawFloor& r = fl[i];
            ReadTile t;
            t.t  = g_clockSongUnits ? r.t : r.tp;
            t.dt = (prev > -1e8) ? (t.t - prev) : 0.0;
            double turn = r.ang;
            if (r.holdLen > 0)
                turn -= (double)r.holdLen * 2.0 * kPi;   // 长按的整圈不计入转角
            if (turn < 0.0)
                turn = 0.0;
            t.turnDeg  = (float)(turn * 180.0 / kPi);
            t.dir      = r.ccw ? -1 : 1;
            t.taps     = (r.taps > 0) ? r.taps : 1;
            t.midSpin  = r.midSpin;
            t.fake     = r.fake;
            t.autoPlay = r.autoPlay;
            t.valid    = r.valid;
            // holdLength >= 0 即长按块（与 scrLevelMaker.DrawHolds 一致）；
            // holdLength == 0 是最短长条，同样要显示。
            if (r.holdLen >= 0 && i + 1 < n)
            {
                double t1 = g_clockSongUnits ? fl[i + 1].t : fl[i + 1].tp;
                if (t1 > t.t)
                    t.hold = (float)(t1 - t.t);
            }
            prev = t.t;
            tiles->push_back(t);
        }
        s_readTotal.store((int)tiles->size(), std::memory_order_relaxed);
        s_readSnap.store(std::shared_ptr<const std::vector<ReadTile>>(std::move(tiles)),
                         std::memory_order_release);
    }

    void ClearReadTiles()
    {
        s_readTotal.store(0, std::memory_order_relaxed);
        s_readCur.store(0, std::memory_order_relaxed);
        s_readSnap.store(std::shared_ptr<const std::vector<ReadTile>>(), std::memory_order_release);
    }
    // ============================================================
    // 辅助读谱（无轨）——渲染层
    //   设计依据（读谱难点 → 对应手段）：
    //     ① 何时按     → 等比时间轴（纵轴=真实时间）+ 拍点网格 + 倒计时环
    //     ② 往哪转/转多少 → 转向轨迹盘：以"绕砖公转"的圆弧画出入射/出射切线
    //        + 箭头，后面几块连成形状（形状记忆 > 数字记忆）
    //     ③ 这串是几拍 → 间隔分数（1/2、1/3 tresillo、1/4、1/6…）
    //     ④ 特殊砖     → 长按尾条 / 多押×N / 中旋·装饰·自动 空心标记
    //     ⑤ 变速       → 分数与色码按局部 dt 实时重算，BPM 变化立刻反映
    // ============================================================

    // 间隔 → 密度色（越密越暖）：1/8 狂潮红 / 1/4 橙 / 1/2 黄 / 慢 绿
    static ImU32 ReadSpeedColor(double dt)
    {
        if (!(dt > 0.0))  return IM_COL32(150, 156, 170, 235);
        if (dt < 0.105)   return IM_COL32(255, 92, 92, 255);
        if (dt < 0.180)   return IM_COL32(255, 158, 64, 255);
        if (dt < 0.300)   return IM_COL32(255, 226, 110, 255);
        if (dt < 0.550)   return IM_COL32(126, 216, 148, 255);
        return IM_COL32(120, 178, 236, 255);
    }

    // 间隔 → 节奏分数（相对当前拍）：1/1 1/2 1/3 1/4 1/6 1/8
    static void ReadRhythmText(double dt, double beat, char* out, size_t n)
    {
        out[0] = 0;
        if (!(dt > 0.001) || !(beat > 0.01)) { snprintf(out, n, "--"); return; }
        double v = beat / dt;                       // 每拍几音
        static const double cand[6] = { 1, 2, 3, 4, 6, 8 };
        static const char*  name[6] = { "1/1", "1/2", "1/3", "1/4", "1/6", "1/8" };
        int best = 0; double bd = 1e9;
        for (int i = 0; i < 6; i++)
        {
            double d = fabs(v - cand[i]);
            if (d < bd) { bd = d; best = i; }
        }
        if (bd <= cand[best] * 0.16)
            snprintf(out, n, "%s", name[best]);
        else
            snprintf(out, n, "%.2fx", v);
    }

    // 本块间隔拍数。逆向依据（勿猜）：
    //   scrPlanet.Update_RefreshAngles: angle = snapped + Δt/beat·π·speed
    //     ⇒ 行星角速度恒为 180°/拍（×speed），与当前砖无关；
    //   scrLevelMaker.CalculateFloorEntryTimes: 间隔 = 扫过角 ÷ (180°/拍)，
    //     普通砖 angleLength = π + 转角 ⇒ 间隔(拍) = 1 + 转角/180°；
    //     中旋/长按/变速由 dt 直接换算。
    static double ReadBeatsOf(const ReadTile& r, double beatDur)
    {
        if (beatDur > 0.01 && r.dt > 0.0)
        {
            double b = r.dt / beatDur;
            double ang = 1.0 + (double)r.turnDeg / 180.0;
            if (r.hold <= 0.001f && !r.midSpin && fabs(b - ang) < 0.12)
                b = ang;                       // 与几何一致时用几何真值（抗 BPM 估计误差）
            if (b < 0.1) b = 0.1;
            if (b > 16.0) b = 16.0;
            return b;
        }
        return 1.0;
    }
    static void ReadBeatsText(double b, char* out, size_t n)
    {
        static const double cand[9] = { 1.0, 1.25, 4.0/3.0, 1.5, 5.0/3.0, 1.75, 2.0, 2.5, 3.0 };
        static const char*  name[9] = { "1", "1.25", "1.33", "1.5", "1.67", "1.75", "2", "2.5", "3" };
        for (int i = 0; i < 9; i++)
            if (fabs(b - cand[i]) < 0.045) { snprintf(out, n, "%s拍", name[i]); return; }
        snprintf(out, n, "%.2f拍", b);
    }
    // 时值色：整拍白 / 附点(1.5·2.5)蓝 / 十六分复合(1.25·1.75)紫 / 三连系(1.33·1.67)橙
    static ImU32 ReadBeatsColor(double b)
    {
        double f = b - floor(b);
        if (f < 0.06) return IM_COL32(214, 220, 232, 255);
        if (fabs(f - 0.5) < 0.06) return IM_COL32(120, 190, 255, 255);
        if (fabs(f - 0.25) < 0.06 || fabs(f - 0.75) < 0.06) return IM_COL32(196, 150, 255, 255);
        return IM_COL32(255, 150, 90, 255);
    }

    // 转向圆弧字形：弧长∝转角、箭头=旋转方向；直行画短横。
    static void DrawTurnGlyph(ImDrawList* dl, float cx, float cy, float rad,
                              float turnDeg, int dir, ImU32 col, float u, bool hot)
    {
        if (turnDeg < 2.f)
        {
            dl->AddLine(ImVec2(cx - rad * 0.8f, cy), ImVec2(cx + rad * 0.8f, cy), col, 2.2f * u);
            return;
        }
        float sweep = 3.14159265f * fminf(turnDeg, 300.f) / 180.f;
        if (sweep > 4.6f) sweep = 4.6f;
        float a0 = -1.5707963f - sweep * 0.5f;
        float s  = (dir < 0) ? -1.f : 1.f;
        float a1 = a0 + s * sweep;
        dl->PathArcTo(ImVec2(cx, cy), rad, a0, a1, 30);
        dl->PathStroke(col, 0, hot ? 3.1f * u : 2.3f * u);
        float ex = cx + cosf(a1) * rad, ey = cy + sinf(a1) * rad;
        float tx = -sinf(a1) * s, ty = cosf(a1) * s;
        float qx = -ty, qy = tx;
        dl->AddTriangleFilled(ImVec2(ex + tx * 5.5f * u, ey + ty * 5.5f * u),
                              ImVec2(ex + qx * 3.6f * u, ey + qy * 3.6f * u),
                              ImVec2(ex - qx * 3.6f * u, ey - qy * 3.6f * u), col);
    }

    // 未来若干块的技术词（连打 / 大回转 / 换向 / 流水 / 长按 / 多押 / 中旋）
    static void ReadHintText(const std::vector<ReadTile>& T, int nxt, char* out, size_t cap)
    {
        out[0] = 0;
        const int n = (int)T.size();
        if (nxt < 0 || nxt >= n) return;
        const ReadTile& a = T[nxt];
        int wrote = 0;
        auto add = [&](const char* str) {
            int len = (int)strlen(out);
            snprintf(out + len, cap - (size_t)len, "%s%s", wrote ? " · " : "", str);
            wrote++;
        };
        char tmp[40];
        if (a.hold > 0.001f) add("长按");
        if (a.midSpin)       add("中旋");
        if (a.taps > 1) { snprintf(tmp, sizeof(tmp), "多押×%d", a.taps); add(tmp); }
        if (a.dt > 0.0 && a.dt < 0.105) add("连点");
        if (a.turnDeg >= 180.f)      add("大回环");
        else if (a.turnDeg >= 120.f) add("大回转");
        int alt = 0, stream = 0;
        for (int k = 0; k < 4 && nxt + k + 1 < n; k++)
        {
            if (T[nxt + k].dir != T[nxt + k + 1].dir) alt++;
            if (T[nxt + k].dt > 0.0 && T[nxt + k].dt < 0.30 && T[nxt + k].turnDeg < 90.f) stream++;
        }
        if (alt >= 3)         add("换向");
        else if (stream >= 3) add("流水");
        if (!wrote) add("平稳");
    }

    // ============================================================
    // 角度判定（辅助读谱的核心，全部来自逆向证据）：
    //   冰与火判定不是"时间窗"，而是"星球与目标角的夹角"：
    //     scrPlanet.SwitchChosen → scrMisc.GetHitMargin(
    //         cachedAngle, targetExitAngle, isCW, bpm*speed, pitch, marginScale)
    //     err = (angle - targetExitAngle) * (isCW ? 1 : -1)   // 弧度；负=早 正=晚
    //   窗口（scrMisc.GetAdjustedAngleBoundaryInDeg）：
    //     Pure    = max(30°·marginScale, 0.02s → 角度)
    //     Perfect = max(45°·marginScale, 0.03s → 角度)
    //     Counted = max(60°·marginScale, 基础秒 → 角度)  基础秒：宽 0.091 / 普 0.065 / 严 0.04
    //     三个秒数再 ÷ speedTrial（下限 0.025s）
    //     角度(度) = 秒 × pitch × 3 × bpm × speed
    //   典型 BPM 下窗口就是固定的 30°/45°/60°：用"度"显示，任何速度都等价；
    //   换成"毫秒"会随 BPM 缩小（150BPM 完美约 ±60ms，300BPM 约 ±30ms）——
    //   时间窗减半而角度窗不变，这正是高速难读、且毫秒倒计时在高速失效的本质。
    // ============================================================
    struct ReadJudge
    {
        bool   live = false;   // true = 读到了真实星球角度（非预测）
        bool   beyond = false;    // 还在 ±Counted 之外（接近中 / 已越过）
        double errDeg = 0.0;   // 负 = 早，正 = 晚
        double counted = 60.0, perfect = 45.0, pure = 30.0;
        double dps = 0.0;      // 角速度（度/秒）= pitch*3*bpm*speed
        double bpm = 0.0, speed = 1.0;
    };

    static ReadJudge ReadJudgeNow(double clock, const std::vector<ReadTile>& T, int nvp)
    {
        ReadJudge j;
        GameBridge::PlanetLive pl{};
        GameBridge::GetPlanetLive(&pl);
        CheatState::Status st{};
        GameBridge::GetStatusSnapshot(&st);

        double bpm = s_bpm.load(std::memory_order_relaxed);
        if (!(bpm > 20.0 && bpm < 1200.0))
        {
            double d = (nvp < (int)T.size() && T[nvp].dt > 0.02) ? T[nvp].dt : 0.5;
            bpm = 60.0 / d;
        }
        double speed = (pl.speed > 0.02 && pl.speed < 100.0) ? pl.speed : 1.0;
        s_lastSpeed.store(speed, std::memory_order_relaxed);
        double pitch = s_clockRate.load(std::memory_order_relaxed);
        if (!(pitch > 0.25 && pitch < 4.0)) pitch = 1.0;
        double dps = 3.0 * bpm * speed * pitch;
        if (dps < 20.0) dps = 20.0;
        j.dps = dps; j.bpm = bpm; j.speed = speed;

        double base  = (st.difficulty == 0) ? 0.091 : (st.difficulty == 2) ? 0.04 : 0.065;
        double trial = (st.speedTrial > 0.05) ? st.speedTrial : 1.0;
        double ms    = (st.marginScale > 0.05) ? st.marginScale : 1.0;
        double n0 = fmax(base / trial, 0.025);
        double a0 = fmax(0.03 / trial, 0.025);
        double a2 = fmax(0.02 / trial, 0.025);
        j.counted = fmax(60.0 * ms, n0 * dps);
        j.perfect = fmax(45.0 * ms, a0 * dps);
        j.pure    = fmax(30.0 * ms, a2 * dps);

        // |err|>3600°（十圈）只可能是菜单/切场景的残留对象，按预测值处理
        if (pl.ok && fabs(pl.errDeg) < 3600.0)
        {
            j.live = true;
            j.errDeg = pl.errDeg;                       // 不归一：还能转 270° 就显示 270°
        }
        else if (nvp < (int)T.size())
        {
            j.errDeg = (clock - T[nvp].t) * dps;        // 退化：按时间预测，负 = 还没到
        }
        j.beyond = (j.errDeg < -j.counted) || (j.errDeg > j.counted);
        return j;
    }

    static ImU32 ReadJudgeBandColor(const ReadJudge& j)
    {
        double a = fabs(j.errDeg);
        if (a <= j.pure)    return IM_COL32(122, 232, 152, 255);
        if (a <= j.perfect) return IM_COL32(240, 214, 96, 255);
        if (a <= j.counted) return IM_COL32(255, 158, 84, 255);
        return (j.errDeg < 0.0) ? IM_COL32(130, 158, 196, 255) : IM_COL32(255, 96, 96, 255);
    }

    // 行星角度盘：把"星球转到了判定角的哪里"画成表盘（左早右晚，顶部=判定角）。
    //   角速度恒为 180°/拍、窗口是固定角度（见 ReadJudgeNow 注释），
    //   所以表盘的语义在任何 BPM/变速下一致——这是毫秒倒计时做不到的。
    static void DrawPlanetDial(ImDrawList* dl, ImFont* font, float cx, float cy, float R,
                               float u, const ReadJudge& j, float op)
    {
        auto A = [op](ImU32 c, float mul) {
            int a = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * op * mul);
            if (a < 0) a = 0; if (a > 255) a = 255;
            return (c & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
        };
        auto A1 = [&A](ImU32 c) { return A(c, 1.f); };
        const float top  = -1.5707963f;
        const float kDeg = 3.14159265f / 180.f;
        double counted = (j.counted > 0.5) ? j.counted : 60.0;
        double perfect = (j.perfect > 0.5) ? j.perfect : counted * 0.75;
        double pure    = (j.pure    > 0.5) ? j.pure    : counted * 0.5;
        const float scale = 128.f / (float)(counted * 1.30);   // 显示压缩：Counted ≈ ±100°
        auto arc = [&](double half, ImU32 col, float thick) {
            float s0 = top + (float)(-half * scale) * kDeg;
            float s1 = top + (float)( half * scale) * kDeg;
            dl->PathArcTo(ImVec2(cx, cy), R, s0, s1, 48);
            dl->PathStroke(col, 0, thick);
        };
        dl->AddCircleFilled(ImVec2(cx, cy), R + 4.f * u, A(IM_COL32(8, 9, 12, 255), 0.72f));
        dl->AddCircle(ImVec2(cx, cy), R, A(IM_COL32(255, 255, 255, 255), 0.14f), 0, 1.6f * u);
        arc(counted, A(IM_COL32(255, 158, 84, 255), 0.55f), fmaxf(3.f, R * 0.30f));
        arc(perfect, A(IM_COL32(240, 214, 96, 255), 0.70f), fmaxf(3.f, R * 0.22f));
        arc(pure,    A(IM_COL32(122, 232, 152, 255), 0.85f), fmaxf(3.f, R * 0.15f));
        dl->AddLine(ImVec2(cx, cy - R - 3.f * u), ImVec2(cx, cy - R * 0.55f),
                    A(IM_COL32(255, 255, 255, 255), 0.85f), 2.0f * u);
        double a = (double)j.errDeg * scale * kDeg;
        if (a >  2.36) a =  2.36;
        if (a < -2.36) a = -2.36;
        float px = cx + sinf((float)a) * R;
        float py = cy - cosf((float)a) * R;
        ImU32 col = ReadJudgeBandColor(j);
        float glow = (fabs(j.errDeg) <= pure) ? 1.f : 0.55f;
        dl->AddCircleFilled(ImVec2(px, py), fmaxf(4.f * u, R * 0.16f), A1(col));
        dl->AddCircle(ImVec2(px, py), fmaxf(7.f * u, R * 0.28f), A(col, glow), 0, 2.0f * u);
        char eb[32];
        if (j.beyond)
            snprintf(eb, sizeof(eb), "%s%.0f°", (j.errDeg < 0.0) ? "早" : "晚", fabs(j.errDeg));
        else
            snprintf(eb, sizeof(eb), "%.0f°", j.errDeg);
        dl->AddText(font, fmaxf(11.f * u, R * 0.30f),
                    ImVec2(cx - font->CalcTextSizeA(fmaxf(11.f * u, R * 0.30f), 1e9f, 0.f, eb).x * 0.5f,
                           cy + R * 0.28f), A1(col), eb);
    }

    // 判定条：±Counted 线性刻度（与游戏内 HitErrorMeter 同一语义的比例刻度），
    // 绿=纯窗口 黄=完美窗口 橙=有效窗口，指针停在实时角误差处。
    static void DrawJudgeStrip(ImDrawList* dl, ImFont* font, float x, float y, float w, float h,
                               float u, const ReadJudge& j, float op)
    {
        (void)font;
        auto A = [op](ImU32 c, float mul) {
            int a = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * op * mul);
            if (a < 0) a = 0; if (a > 255) a = 255;
            return (c & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
        };
        auto A1 = [&A](ImU32 c) { return A(c, 1.f); };
        const float cx = x + w * 0.5f, halfW = w * 0.5f, r = h * 0.5f;
        double denom = (j.counted > 0.5) ? j.counted : 0.5;
        float zp = (float)(j.pure / denom) * halfW;
        float zf = (float)(j.perfect / denom) * halfW;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), A(IM_COL32(255, 158, 84, 255), 0.20f), r);
        dl->AddRectFilled(ImVec2(cx - zf, y), ImVec2(cx + zf, y + h), A(IM_COL32(240, 214, 96, 255), 0.26f), r);
        dl->AddRectFilled(ImVec2(cx - zp, y), ImVec2(cx + zp, y + h), A(IM_COL32(122, 232, 152, 255), 0.32f), r);
        dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), A(IM_COL32(255, 255, 255, 255), 0.10f), r, 0, 1.f * u);
        dl->AddLine(ImVec2(cx, y + 1.f * u), ImVec2(cx, y + h - 1.f * u),
                    A(IM_COL32(255, 255, 255, 255), 0.30f), 1.2f * u);
        double rel = j.errDeg / denom;
        if (rel > 1.0) rel = 1.0;
        if (rel < -1.0) rel = -1.0;
        float nx = cx + (float)rel * (halfW - 1.5f * u);
        ImU32 col = ReadJudgeBandColor(j);
        dl->AddTriangleFilled(ImVec2(nx, y - 1.f * u), ImVec2(nx - 4.5f * u, y - 9.f * u),
                              ImVec2(nx + 4.5f * u, y - 9.f * u), A1(col));
        dl->AddRectFilled(ImVec2(nx - 1.4f * u, y), ImVec2(nx + 1.4f * u, y + h), A1(col), 1.4f * u);
    }

    // 一整套角度判定显示：判定条 + 大号误差（度）+ 窗口数字；返回占用像素高度
    static float DrawJudgeBlock(ImDrawList* dl, ImFont* font, float x, float y, float w,
                                float u, const ReadJudge& j, float op, bool big)
    {
        auto A = [op](ImU32 c, float mul) {
            int a = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * op * mul);
            if (a < 0) a = 0; if (a > 255) a = 255;
            return (c & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
        };
        auto Txt = [&](float tx, float ty, ImU32 c, const char* t, float sz) {
            dl->AddText(font, sz, ImVec2(tx, ty), c, t);
        };
        auto TxtR = [&](float rx, float ty, ImU32 c, const char* t, float sz) {
            dl->AddText(font, sz, ImVec2(rx - font->CalcTextSizeA(sz, 1e9f, 0.f, t).x, ty), c, t);
        };

        const float sh = 14.f * u;
        if (s_readWindows.load(std::memory_order_relaxed))
            DrawJudgeStrip(dl, font, x, y, w, sh, u, j, op);
        else
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + sh), A(IM_COL32(26, 28, 36, 255), 0.95f), sh * 0.5f);

        char buf[64];
        ImU32 col = ReadJudgeBandColor(j);
        const float fs = (big ? 24.f : 20.f) * u;
        const char* side = nullptr;
        if (j.beyond && j.errDeg < 0.0) snprintf(buf, sizeof(buf), "还差 %.0f°", -j.errDeg);
        else if (j.beyond)              snprintf(buf, sizeof(buf), "已过 %.0f°", j.errDeg);
        else { snprintf(buf, sizeof(buf), "%+.0f°", j.errDeg); side = (j.errDeg < 0.0) ? "早" : "晚"; }
        const float ty = y + sh + 4.f * u;
        Txt(x + 1.f * u, ty, col, buf, fs);
        float numW = font->CalcTextSizeA(fs, 1e9f, 0.f, buf).x;
        if (side)
            Txt(x + numW + 7.f * u, ty + fs * 0.30f, col, side, fs * 0.60f);

        snprintf(buf, sizeof(buf), "完美%.0f 准%.0f 有效%.0f", j.pure, j.perfect, j.counted);
        TxtR(x + w, ty + fs * 0.10f, A(IM_COL32(164, 172, 188, 255), 0.95f), buf, 12.f * u);
        TxtR(x + w, ty + fs * 0.10f + 15.f * u,
             A(IM_COL32(122, 130, 148, 255), 0.9f), j.live ? "实时角度" : "预测角度", 11.f * u);
        return sh + 4.f * u + fs * 1.30f;
    }

    // 未来若干个"要按"的块：圆弧字形（弧长=转角、箭头=方向）+ 时值拍数。
    //   后缀：xN 多押 / H 长按 / ! 换向 / ~ 变速。固定位置绘制，
    //   打歌时不用追着时间轴看，形状一眼可读。
    static void DrawNextTurns(ImDrawList* dl, ImFont* font, float x, float y, float maxW,
                              float u, const std::vector<ReadTile>& T, int nxt, int maxN,
                              float op, double beatDur)
    {
        auto A = [op](ImU32 c, float mul) {
            int a = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * op * mul);
            if (a < 0) a = 0; if (a > 255) a = 255;
            return (c & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
        };
        auto A1 = [&A](ImU32 c) { return A(c, 1.f); };
        auto Txt = [&](float tx, float ty, ImU32 c, const char* t, float sz) {
            dl->AddText(font, sz, ImVec2(tx, ty), c, t);
        };
        Txt(x, y + 6.f * u, A(IM_COL32(140, 148, 164, 255), 0.95f), "后续", 12.f * u);
        float cx = x + 30.f * u;
        int drawn = 0, prev = -1;
        for (int i = nxt; i < (int)T.size() && drawn < maxN; i++)
        {
            const ReadTile& r = T[i];
            if (!r.valid) continue;
            ImU32 col = (r.turnDeg < 2.f) ? IM_COL32(170, 180, 200, 255)
                      : (r.turnDeg >= 120.f) ? IM_COL32(255, 132, 132, 255)
                      : (r.dir < 0 ? IM_COL32(120, 190, 255, 255) : IM_COL32(255, 190, 120, 255));
            DrawTurnGlyph(dl, cx + 10.f * u, y + 9.f * u, 8.5f * u, r.turnDeg, r.dir, A1(col), u, i == nxt);
            char buf[24];
            double b = ReadBeatsOf(r, beatDur);
            ReadBeatsText(b, buf, sizeof(buf));
            Txt(cx, y + 23.f * u, A1(ReadBeatsColor(b)), buf, 11.f * u);
            const char* tag = nullptr;
            if (r.taps > 1)            { snprintf(buf, sizeof(buf), "x%d", r.taps); tag = buf; }
            else if (r.hold > 0.001f)  tag = "H";
            bool swing = (prev >= 0 && T[prev].dir != r.dir &&
                          T[prev].turnDeg >= 45.f && r.turnDeg >= 45.f);
            bool vspeed = (prev >= 0 && T[prev].dt > 0.01 && r.dt > 0.01 &&
                           (r.dt < T[prev].dt * 0.72 || r.dt > T[prev].dt * 1.38));
            if (!tag && swing) tag = "!";
            if (!tag && vspeed) tag = "~";
            if (tag)
                Txt(cx + 21.f * u, y + 1.f * u, A1(IM_COL32(255, 150, 90, 255)), tag, 12.f * u);
            cx += 31.f * u;
            if (cx > x + maxW - 26.f * u) break;
            prev = i;
            drawn++;
        }
    }

    // ============================================================
    // 打歌用横向条（重做版）
    //   打歌时视线不能离开判定区，所以关键信息固定在条左侧一个不动的
    //   方块里，随时间滚动的部分只当背景参考：
    //     · 角度判定条：实时角误差；0=命中角，绿/黄/橙 = 纯/准/有效窗口
    //     · 大号"±x° / 还差 x° / 已过 x°"：现在该按，还是早了/晚了
    //     · 后续转向速览：接下来 3~4 块的转向与特殊砖（换向 / 变速 / 多押）
    //   高 BPM（间隔 <0.16s 或每秒 >11 块）自动简化：时间轴只留圆弧字形
    //   与拍点，拍数小字只保留最近两块 —— 高速时形状与节奏点才读得过来。
    // ============================================================
    static void DrawReadHorizBar(const std::vector<ReadTile>& T, int total, int nxt, int nvp,
                                 double clock, float u, float op, int layout)
    {
        float gw = 1280.f, gh = 720.f;
        RenderHook::GetGameWindowSize(&gw, &gh);
        if (gw < 200.f || gh < 200.f)
            return;

        const double bpmRaw = s_bpm.load(std::memory_order_relaxed);
        double beat = (bpmRaw > 20.0 && bpmRaw < 500.0) ? 60.0 / bpmRaw : 0.0;
        if (!(beat > 0.01))
        {
            double d = 0.0;
            for (int i = nxt; i < total && i < nxt + 16; i++)
                if (T[i].dt > 0.05) { d = T[i].dt; break; }
            beat = (d > 0.05) ? d * 2.0 : 0.5;
        }

        int ahead = s_readAhead.load(std::memory_order_relaxed);
        if (ahead < 3)  ahead = 3;
        if (ahead > 24) ahead = 24;
        int lastIdx = nxt + ahead;
        if (lastIdx > total - 1) lastIdx = total - 1;
        double span = T[lastIdx].t - clock;
        if (!(span > 0.30)) span = 0.30;

        const double ndt = T[nvp].dt;
        const int visTiles = lastIdx - nxt + 1;
        const bool compact =
                             (s_readAdapt.load(std::memory_order_relaxed) &&
                              ((ndt > 0.0 && ndt < 0.16) || (span > 0.01 && visTiles / span > 11.0)));

        const float margin = 24.f * u;
        const float barH   = 124.f * u;
        float bx0 = margin, bx1 = gw - margin;
        float by0 = (layout == 1) ? (gh - barH - 26.f * u) : (30.f * u);
        bx0 += (float)s_readPosX.load(std::memory_order_relaxed) * u;
        bx1 += (float)s_readPosX.load(std::memory_order_relaxed) * u;
        by0 += (float)s_readPosY.load(std::memory_order_relaxed) * u;

        const float gaugeW = 334.f * u;              // 左侧固定块：行星盘 + 角度判定 + 后续转向
        const float gx0 = bx0 + 13.f * u;
        const float gx1 = bx0 + gaugeW;
        const float t0  = bx0 + gaugeW + 8.f * u;    // 时间轴左界
        const float nowX = t0 + (bx1 - t0) * 0.22f;
        const float cy = by0 + barH * 0.5f;

        float dens = (float)s_readDensity.load(std::memory_order_relaxed) / 100.f;
        if (dens < 0.4f) dens = 0.4f;
        if (dens > 2.2f) dens = 2.2f;
        float pxPerSec = (bx1 - nowX - 26.f * u) * dens / (float)span;
        if (pxPerSec < 45.f * u)  pxPerSec = 45.f * u;
        if (pxPerSec > 900.f * u) pxPerSec = 900.f * u;

        const ReadJudge jr = ReadJudgeNow(clock, T, nvp);
        const bool gauge = s_readAngle.load(std::memory_order_relaxed);

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(gw, gh), ImGuiCond_Always);
        ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                              ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                              ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##readbar", nullptr, fl);
        StreamMode::MarkWindow("##readbar", StreamMode::EL_READ);   // 直播模式：辅助读谱（横条）
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImFont* font = ImGui::GetFont();
        auto A = [op](ImU32 c, float mul) {
            int a = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * op * mul);
            if (a < 0) a = 0; if (a > 255) a = 255;
            return (c & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
        };
        auto A1 = [&A](ImU32 c) { return A(c, 1.f); };
        auto Txt = [&](float x, float y, ImU32 c, const char* t, float sz) {
            dl->AddText(font, sz, ImVec2(x, y), c, t);
        };

        dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by0 + barH), A(IM_COL32(12, 13, 17, 255), 0.90f), 12.f * u);
        dl->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by0 + barH), A(IM_COL32(255, 255, 255, 255), 0.11f), 12.f * u, 0, 1.4f * u);

        // ---- 左：角度判定（实时）+ 后续转向速览 ----
        if (gauge)
        {
            dl->AddRectFilled(ImVec2(gx0 - 6.f * u, by0 + 7.f * u), ImVec2(gx1 + 4.f * u, by0 + barH - 7.f * u),
                              A(IM_COL32(255, 255, 255, 255), 0.045f), 9.f * u);
            const float dialD = 86.f * u;
            const double beatDur = (jr.speed > 0.02) ? (beat / jr.speed) : beat;
            DrawPlanetDial(dl, font, gx0 + dialD * 0.5f, by0 + 12.f * u + dialD * 0.5f,
                           dialD * 0.5f - 2.f * u, u, jr, op);
            const float tx0 = gx0 + dialD + 6.f * u;
            float jh = DrawJudgeBlock(dl, font, tx0, by0 + 10.f * u, gx1 - tx0, u, jr, op, true);
            DrawNextTurns(dl, font, tx0, by0 + 10.f * u + jh + 2.f * u, gx1 - tx0,
                          u, T, nvp, compact ? 3 : 4, op, beatDur);
            dl->AddLine(ImVec2(t0 - 4.f * u, by0 + 9.f * u), ImVec2(t0 - 4.f * u, by0 + barH - 9.f * u),
                        A(IM_COL32(255, 255, 255, 255), 0.07f), 1.f * u);
        }

        const float top = by0 + 6.f * u, bot = by0 + barH - 6.f * u;
        const float yTurn = (layout == 1) ? (by0 + barH - 38.f * u) : (by0 + 22.f * u);
        const float yRh   = (layout == 1) ? (by0 + barH - 16.f * u) : (by0 + 45.f * u);

        if (s_readGrid.load(std::memory_order_relaxed) && beat > 0.05)
        {
            double phase = T[nxt].t - floor(T[nxt].t / beat + 0.5) * beat;
            int m0 = (int)ceil((clock - 0.35 - phase) / beat);
            int m1 = (int)floor((clock + span + 0.2 - phase) / beat);
            if (m1 - m0 > 400) m1 = m0 + 400;
            for (int m = m0; m <= m1; m++)
            {
                double gt = phase + (double)m * beat;
                float gx = nowX + (float)((gt - clock) * pxPerSec);
                if (gx < t0 + 2.f * u || gx > bx1 - 2.f * u) continue;
                dl->AddLine(ImVec2(gx, top), ImVec2(gx, bot), A(IM_COL32(255, 255, 255, 255), 0.09f), 1.f * u);
            }
        }

        const bool marks = s_readMarks.load(std::memory_order_relaxed);
        const bool rhy   = s_readRhythm.load(std::memory_order_relaxed);
        char line[64];
        int first = nxt - 2;
        if (first < 1) first = 1;
        for (int i = first; i < total; i++)
        {
            const ReadTile& r = T[i];
            float x = nowX + (float)((r.t - clock) * pxPerSec);
            if (x < t0 - 20.f * u) continue;
            if (x > bx1 + 20.f * u) break;
            const bool isNext = (i == nvp);
            ImU32 col = ReadSpeedColor(r.dt);
            if (i > 0 && T[i - 1].dir != r.dir && T[i - 1].turnDeg >= 45.f && r.turnDeg >= 45.f &&
                !T[i - 1].fake && !r.fake)
                dl->AddLine(ImVec2(x, top), ImVec2(x, bot), A(IM_COL32(255, 196, 90, 255), 0.30f), 1.6f * u);

            if (r.valid)
            {
                dl->AddRectFilled(ImVec2(x - 2.4f * u, top + 6.f * u), ImVec2(x + 2.4f * u, bot - 6.f * u),
                                  A1(col), 2.4f * u);
                dl->AddCircleFilled(ImVec2(x, cy), (isNext ? 9.f : 6.f) * u, A1(col));
                if (isNext)
                    dl->AddCircle(ImVec2(x, cy), 15.f * u, A(col, 0.6f), 0, 2.f * u);
            }
            else
            {
                dl->AddCircle(ImVec2(x, cy), 6.f * u, A(IM_COL32(168, 178, 198, 255), 0.8f), 0, 1.8f * u);
                if (r.midSpin)
                    dl->AddCircleFilled(ImVec2(x, cy), 2.6f * u, A(IM_COL32(88, 168, 255, 255), 0.95f));
            }
            if (r.hold > 0.001f)
            {
                float w = (float)r.hold * pxPerSec;
                dl->AddRectFilled(ImVec2(x, cy + 8.f * u), ImVec2(x + w, cy + 13.f * u),
                                  A(IM_COL32(64, 224, 200, 255), 0.85f), 2.5f * u);
            }

            float gapNext = (i + 1 < total) ? (float)((T[i + 1].t - r.t) * pxPerSec) : 1e6f;
            if (gapNext > 30.f * u)
            {
                {
                    const bool closeR = isNext || i < nxt + (compact ? 2 : 24);
                    ImU32 gcol = (r.turnDeg < 2.f) ? IM_COL32(170, 180, 200, 255)
                               : (r.turnDeg >= 120.f) ? IM_COL32(255, 132, 132, 255)
                               : (r.dir < 0 ? IM_COL32(120, 190, 255, 255) : IM_COL32(255, 190, 120, 255));
                    if (closeR)
                    {
                        float gr = fminf(isNext ? 10.5f : 8.f, fmaxf(5.f, gapNext * 0.24f / (u > 0.f ? u : 1.f)));
                        DrawTurnGlyph(dl, x, yTurn + 9.f * u, gr * u, r.turnDeg, r.dir, A1(gcol), u, isNext);
                    }
                    if (!compact && gapNext > 30.f * u)
                    {
                        snprintf(line, sizeof(line), "%.0f°", r.turnDeg);
                        Txt(x - 10.f * u, yTurn - 13.f * u, A1(gcol), line, 11.f * u);
                    }
                    if (rhy && closeR)
                    {
                        char bb[16];
                        double bv = ReadBeatsOf(r, (jr.speed > 0.02) ? (beat / jr.speed) : beat);
                        ReadBeatsText(bv, bb, sizeof(bb));
                        Txt(x - 14.f * u, yRh, A1(ReadBeatsColor(bv)), bb, 12.f * u);
                    }
                }
                if (marks)
                {
                    const char* tag = (r.hold > 0.001f) ? "H" : r.midSpin ? "M" : r.autoPlay ? "A" : r.fake ? "F" : nullptr;
                    if (r.taps > 1) { snprintf(line, sizeof(line), "x%d", r.taps); Txt(x - 8.f * u, top + 2.f * u, A1(IM_COL32(255, 150, 90, 255)), line, 12.f * u); }
                    else if (tag)   { Txt(x - 4.f * u, top + 2.f * u,
                                          A1(r.midSpin ? IM_COL32(88, 168, 255, 255)
                                                       : IM_COL32(255, 150, 90, 255)), tag, 12.f * u); }
                }
            }
        }

        {
            float beatPulse = (beat > 0.05) ? (float)fmod(clock, beat) / (float)beat : 0.f;
            float pulse = 0.55f + 0.45f * (float)fmax(0.0, cos(beatPulse * 6.2831853f));
            dl->AddLine(ImVec2(nowX, top - 2.f * u), ImVec2(nowX, bot + 2.f * u),
                        A(IM_COL32(90, 170, 255, 255), pulse), 2.6f * u);
        }

        {
            double dtn = T[nvp].t - clock;
            const char* src = jr.live ? "" : "~";
            snprintf(line, sizeof(line), "%s%+.0f ms", src, dtn * 1000.0);
            Txt(nowX + 8.f * u, top + 4.f * u, A(IM_COL32(120, 220, 255, 255), 1.f), line, 16.f * u);
            if (s_readHint.load(std::memory_order_relaxed))
            {
                char hint[128];
                ReadHintText(T, nxt, hint, sizeof(hint));
                Txt(nowX + 8.f * u, top + 24.f * u, A(IM_COL32(255, 196, 120, 255), 0.95f), hint, 14.f * u);
            }
            snprintf(line, sizeof(line), "BPM %.0f", bpmRaw > 20.f ? bpmRaw : 60.0 / beat);
            float tw = font->CalcTextSizeA(14.f * u, 1e9f, 0.f, line).x;
            Txt(bx1 - tw - 10.f * u, (layout == 1) ? (by0 + 4.f * u) : (by0 + barH - 22.f * u),
                A(IM_COL32(150, 200, 255, 255), 0.95f), line, 14.f * u);
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    void DrawReadOverlay()
    {
        std::shared_ptr<const std::vector<ReadTile>> snap;
        double clock = 0.0;
        float gw = 1280.f, gh = 720.f;
        RenderHook::GetGameWindowSize(&gw, &gh);
        if (gw < 200.f || gh < 200.f)
            return;
        {
            if (!s_readOn.load(std::memory_order_relaxed))
                return;
            snap = s_readSnap.load(std::memory_order_acquire);
            if (!snap || snap->size() < 3)
                return;

            CheatState::Status st;
            GameBridge::GetStatusSnapshot(&st);
            const bool songStarted = s_songStarted.load(std::memory_order_relaxed);
            const bool fsmPlaying  = (st.state == 2 || st.state == 3 || st.state == 4);
            const bool clockMoving = (WallNow() - s_clockMovedWall.load(std::memory_order_relaxed)) < 0.35;
            if (!(st.gameworld && !st.paused && (songStarted || fsmPlaying || clockMoving)))
                return;
            clock = RenderClockOff(s_readOffsetMS.load(std::memory_order_relaxed));
            if (clock < -0.25 || clock > snap->back().t + 3.0)
                return;
        }

        const std::vector<ReadTile>& T = *snap;
        const int    total = (int)T.size();

        int nxt = (int)(std::lower_bound(T.begin(), T.end(), clock,
                     [](const ReadTile& a, double v) { return a.t < v; }) - T.begin());
        if (nxt < 1) nxt = 1;
        if (nxt > total - 1) nxt = total - 1;
        s_readCur.store(nxt, std::memory_order_relaxed);
        int nvp = nxt;                                    // 下一块"需要按"的砖
        while (nvp < total - 1 && !T[nvp].valid) nvp++;

        const float nowT = (float)ImGui::GetTime();
        const float u    = (gh / 1080.f) * ((float)s_readScale.load(std::memory_order_relaxed) / 100.f);
        const float op   = (float)s_readOpacity.load(std::memory_order_relaxed) / 255.f;
        auto A = [op](ImU32 c, float mul) {
            int a = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * op * mul);
            if (a < 0) a = 0;
            if (a > 255) a = 255;
            return (c & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
        };
        auto A1 = [&A](ImU32 c) { return A(c, 1.f); };

        // 横向布局：底部/顶部横条（打歌时用，视线不用离开判定区）
        const int layout = s_readLayout.load(std::memory_order_relaxed);
        if (layout == 1 || layout == 2)
        {
            DrawReadHorizBar(T, total, nxt, nvp, clock, u, op, layout);
            return;
        }

        // ---- 面板几何（1080p 基准 × 缩放；锚点 + 微调）----
        const float headH  = 142.f * u;
        const float bodyH  = 452.f * u;
        const float dialH  = 186.f * u;
        const float panelW = 348.f * u;
        const float panelH = headH + bodyH + dialH;
        int anchor = s_readAnchor.load(std::memory_order_relaxed);
        if (anchor < 0 || anchor > 4) anchor = 0;
        float px, py;
        if (anchor == 4)                               // 居中：面板正对屏幕中央
        {
            px = (gw - panelW) * 0.5f;
            py = (gh - panelH) * 0.5f;
        }
        else
        {
            px = (anchor & 1) ? (gw - panelW - 40.f * u) : (40.f * u);
            py = (anchor >= 2) ? (gh - panelH - 56.f * u) : (185.f * u);
        }
        px += (float)s_readPosX.load(std::memory_order_relaxed) * u;
        py += (float)s_readPosY.load(std::memory_order_relaxed) * u;

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(gw, gh), ImGuiCond_Always);
        ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                              ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                              ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##readoverlay", nullptr, fl);
        StreamMode::MarkWindow("##readoverlay", StreamMode::EL_READ);  // 直播模式：辅助读谱（无轨条）
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImFont* font = ImGui::GetFont();
        auto Txt  = [&](float x, float y, ImU32 c, const char* t, float sz) {
            dl->AddText(font, sz, ImVec2(x, y), c, t);
        };
        auto TxtR = [&](float right, float y, ImU32 c, const char* t, float sz) {
            dl->AddText(font, sz, ImVec2(right - font->CalcTextSizeA(sz, 1e9f, 0.f, t).x, y), c, t);
        };
        auto TxtC = [&](float cx, float y, ImU32 c, const char* t, float sz) {
            dl->AddText(font, sz, ImVec2(cx - font->CalcTextSizeA(sz, 1e9f, 0.f, t).x * 0.5f, y), c, t);
        };

        dl->AddRectFilled(ImVec2(px, py), ImVec2(px + panelW, py + panelH),
                          A(IM_COL32(12, 13, 17, 255), 0.93f), 12.f * u);
        dl->AddRect(ImVec2(px, py), ImVec2(px + panelW, py + panelH),
                    A(IM_COL32(255, 255, 255, 255), 0.11f), 12.f * u, 0, 1.4f * u);

        // ---- 拍长（BPM 或按最近的 dt 反推；变速时按局部值）----
        const double bpmRaw = s_bpm.load(std::memory_order_relaxed);
        double beat = (bpmRaw > 20.0 && bpmRaw < 500.0) ? 60.0 / bpmRaw : 0.0;
        if (!(beat > 0.01))
        {
            double d = 0.0;
            for (int i = nxt; i < total && i < nxt + 16; i++)
                if (T[i].dt > 0.05) { d = T[i].dt; break; }
            beat = (d > 0.05) ? d * 2.0 : 0.5;
        }

        // ---- 头部 ----
        char line[192];
        Txt(px + 14.f * u, py + 9.f * u, A1(IM_COL32(240, 244, 252, 255)), "辅助读谱 · 无轨", 19.f * u);
        snprintf(line, sizeof(line), "BPM %.1f", bpmRaw > 20.f ? bpmRaw : 60.0 / beat);
        TxtR(px + panelW - 14.f * u, py + 12.f * u, A1(IM_COL32(150, 200, 255, 255)), line, 16.f * u);
        {
            char rh[16];
            {
                double spd = s_lastSpeed.load(std::memory_order_relaxed);
                if (!(spd > 0.02 && spd < 100.0)) spd = 1.0;
                ReadBeatsText(ReadBeatsOf(T[nvp], beat / spd), rh, sizeof(rh));
            }
            snprintf(line, sizeof(line), "第 %d / %d 块", nxt, total - 1);
            Txt(px + 14.f * u, py + 34.f * u, A1(IM_COL32(176, 184, 200, 255)), line, 14.f * u);
            snprintf(line, sizeof(line), "%s · %.0fms", rh, T[nvp].dt * 1000.0);
            TxtR(px + panelW - 14.f * u, py + 35.f * u, A1(IM_COL32(170, 178, 194, 255)), line, 14.f * u);
            if (s_readHint.load(std::memory_order_relaxed))
            {
                ReadHintText(T, nxt, line, sizeof(line));
                Txt(px + 14.f * u, py + 120.f * u, A1(IM_COL32(255, 196, 120, 255)), line, 15.f * u);
            }
        }
        // 角度判定（实时，按游戏的判定角）+ 未来转向速览
        if (s_readAngle.load(std::memory_order_relaxed))
        {
            const ReadJudge jr = ReadJudgeNow(clock, T, nvp);
            float jh = DrawJudgeBlock(dl, font, px + 14.f * u, py + 56.f * u, panelW - 28.f * u,
                                      u, jr, op, false);
            DrawNextTurns(dl, font, px + 14.f * u, py + 56.f * u + jh + 3.f * u, panelW - 28.f * u,
                          u, T, nvp, 4, op, beat / ((jr.speed > 0.02) ? jr.speed : 1.0));
        }

        // ---- 时间轴几何 ----
        const int aheadCfg = s_readAhead.load(std::memory_order_relaxed);
        int ahead = aheadCfg;
        if (ahead < 3) ahead = 3;
        if (ahead > 24) ahead = 24;
        int lastIdx = nxt + ahead;
        if (lastIdx > total - 1) lastIdx = total - 1;
        double span = T[lastIdx].t - clock;
        if (!(span > 0.30)) span = 0.30;
        float dens = (float)s_readDensity.load(std::memory_order_relaxed) / 100.f;
        if (dens < 0.4f) dens = 0.4f;
        if (dens > 2.2f) dens = 2.2f;
        float pxPerSec = (bodyH - 24.f * u) * dens / (float)span;
        if (pxPerSec < 90.f * u)   pxPerSec = 90.f * u;
        if (pxPerSec > 2600.f * u) pxPerSec = 2600.f * u;

        const float bodyTop = py + headH;
        const float bodyBot = bodyTop + bodyH;
        const float nowY    = bodyTop + 52.f * u;
        const float mk      = px + 132.f * u;      // 标记列
        const float x1      = px + panelW - 14.f * u;

        dl->PushClipRect(ImVec2(px + 4.f * u, bodyTop), ImVec2(px + panelW - 4.f * u, bodyBot), true);

        // 拍点网格（相位：窗口内可打击砖残差投票，抗 tresillo 干扰）
        if (s_readGrid.load(std::memory_order_relaxed) && beat > 0.05)
        {
            double phase = T[nxt].t - floor(T[nxt].t / beat + 0.5) * beat;
            int bestN = 0;
            double bestPh = phase;
            for (int a = -14; a <= 14; a += 2)
            {
                double cand = T[nxt].t + (double)a * 0.03 * beat;
                double ph = cand - floor(cand / beat + 0.5) * beat;
                int cnt = 0;
                for (int i = nxt; i < total && i < nxt + 24; i++)
                {
                    double r = T[i].t - floor(T[i].t / beat + 0.5) * beat;
                    double d = fabs(r - ph);
                    if (d > beat * 0.5) d = beat - d;
                    if (d < beat * 0.06) cnt++;
                }
                if (cnt > bestN) { bestN = cnt; bestPh = ph; }
            }
            phase = bestPh;
            int m0 = (int)ceil((clock - 0.35 - phase) / beat);
            int m1 = (int)floor((clock + span + 0.15 - phase) / beat);
            if (m1 - m0 > 400) m1 = m0 + 400;
            for (int m = m0; m <= m1; m++)
            {
                double gt = phase + (double)m * beat;
                float  gy = nowY + (float)((gt - clock) * pxPerSec);
                if (gy < bodyTop - 4.f * u || gy > bodyBot + 4.f * u) continue;
                double rel = gt - clock;
                bool strong = (rel < 0.09 && rel > -0.25);
                dl->AddLine(ImVec2(px + 11.f * u, gy), ImVec2(px + panelW - 11.f * u, gy),
                            strong ? A(IM_COL32(255, 255, 255, 255), 0.22f)
                                   : A(IM_COL32(255, 255, 255, 255), 0.075f),
                            strong ? 2.4f * u : 1.f * u);
            }
        }

        // 行（等比时间：纵轴 = 真实时间；现在线以上为已过）
        {
            double spd = s_lastSpeed.load(std::memory_order_relaxed);
            if (!(spd > 0.02 && spd < 100.0)) spd = 1.0;
            const double beatDur = beat / spd;
            int first = nxt - 2;
            if (first < 1) first = 1;
            const bool marks = s_readMarks.load(std::memory_order_relaxed);
            for (int i = first; i < total; i++)
            {
                const ReadTile& r = T[i];
                float y = nowY + (float)((r.t - clock) * pxPerSec);
                if (y < bodyTop - 28.f * u) continue;
                if (y > bodyBot + 26.f * u) break;

                const bool isNext = (i == nvp);
                ImU32 col = ReadSpeedColor(r.dt);
                float gapNext = (i + 1 < total) ? (float)((T[i + 1].t - r.t) * pxPerSec) : 1e6f;

                dl->AddLine(ImVec2(mk + 14.f * u, y), ImVec2(x1, y),
                            A(IM_COL32(255, 255, 255, 255), r.valid ? 0.15f : 0.06f),
                            isNext ? 2.4f * u : 1.2f * u);

                if (marks && r.hold > 0.001f)
                {
                    float hy = (float)r.hold * pxPerSec;
                    dl->AddRectFilled(ImVec2(x1 - 11.f * u, y), ImVec2(x1 - 3.f * u, y + hy),
                                      A(IM_COL32(64, 224, 200, 255), 0.85f), 4.f * u);
                }

                ImVec2 mp(mk, y);
                if (r.valid)
                {
                    dl->AddCircleFilled(mp, (isNext ? 10.f : 6.5f) * u, A1(col));
                    if (isNext)
                    {
                        float pr = (17.f + 5.f * sinf(nowT * 12.566f)) * u;
                        dl->AddCircle(mp, pr, A(col, 0.55f), 0, 2.2f * u);
                    }
                }
                else
                {
                    dl->AddCircle(mp, 7.f * u, A(IM_COL32(168, 178, 198, 255), 0.85f), 0, 2.f * u);
                    if (r.midSpin)
                        dl->AddCircleFilled(mp, 2.6f * u, A(IM_COL32(88, 168, 255, 255), 0.95f));
                }

                if (gapNext > 21.f * u)
                {
                    {
                        char bb[16];
                        double bv = ReadBeatsOf(r, beatDur);
                        ReadBeatsText(bv, bb, sizeof(bb));
                        snprintf(line, sizeof(line), "%s %.0f°", bb, r.turnDeg);
                        TxtR(mk - 20.f * u, y - 8.f * u,
                             (r.turnDeg >= 120.f) ? A(IM_COL32(255, 132, 132, 255), 1.f)
                                                  : A(IM_COL32(226, 232, 244, 255), 0.95f),
                             line, 14.f * u);
                    }
                    Txt(mk - 16.f * u, y - 8.f * u,
                        A1(r.dir < 0 ? IM_COL32(120, 190, 255, 255) : IM_COL32(255, 190, 120, 255)),
                        r.dir < 0 ? "逆" : "顺", 15.f * u);

                    if (marks)
                    {
                        const char* tag = (r.hold > 0.001f) ? "长按"
                                        : r.midSpin ? "中旋"
                                        : r.autoPlay ? "自动"
                                        : r.fake ? "装饰" : nullptr;
                        if (r.taps > 1)
                        {
                            snprintf(line, sizeof(line), "多押×%d", r.taps);
                            Txt(mk + 16.f * u, y - 8.f * u, A1(IM_COL32(255, 150, 90, 255)), line, 14.f * u);
                        }
                        else if (tag)
                            Txt(mk + 16.f * u, y - 8.f * u,
                                A1(r.midSpin ? IM_COL32(88, 168, 255, 255)
                                             : IM_COL32(255, 150, 90, 255)), tag, 14.f * u);
                    }

                    if (s_readRhythm.load(std::memory_order_relaxed))
                    {
                        char rh[16];
                        ReadRhythmText(r.dt, beat, rh, sizeof(rh));
                        TxtR(x1, y - 9.f * u, A(IM_COL32(192, 200, 216, 255), 0.92f), rh, 14.f * u);
                        if (gapNext > 36.f * u)
                        {
                            snprintf(line, sizeof(line), "%.0fms", r.dt * 1000.0);
                            TxtR(x1, y + 3.f * u, A(IM_COL32(140, 146, 160, 255), 0.9f), line, 12.f * u);
                        }
                    }
                }
            }
        }

        // 现在线 + 倒计时环
        {
            float beatPulse = (beat > 0.05)
                ? (float)fmod(clock, beat) / (float)beat : 0.f;
            float pulse = 0.55f + 0.45f * (float)fmax(0.0, cos(beatPulse * 6.2831853f));
            dl->AddLine(ImVec2(px + 8.f * u, nowY), ImVec2(px + panelW - 8.f * u, nowY),
                        A(IM_COL32(90, 170, 255, 255), pulse), 2.6f * u);
            dl->AddLine(ImVec2(px + 8.f * u, nowY), ImVec2(mk + 14.f * u, nowY),
                        A(IM_COL32(90, 170, 255, 255), pulse * 0.5f), 2.6f * u);

            {
                float rcx = px + 42.f * u, rcy = nowY + 40.f * u, rr = 31.f * u;
                ReadJudge jd{};
                if (s_readAngle.load(std::memory_order_relaxed))
                    jd = ReadJudgeNow(clock, T, nvp);
                DrawPlanetDial(dl, font, rcx, rcy, rr, u, jd, op);
                snprintf(line, sizeof(line), "%d ms", (int)((T[nvp].t - clock) * 1000.0));
                TxtC(rcx, rcy + rr + 5.f * u, A(IM_COL32(150, 158, 174, 255), 1.f), line, 12.f * u);
            }
        }
        dl->PopClipRect();

        // ---- 转向轨迹盘（下一块：绕砖公转的圆弧 + 出入射切线）----
        if (s_readDial.load(std::memory_order_relaxed))
        {
            const float dcx = px + 92.f * u;
            const float dcy = bodyBot + 88.f * u;
            const float R   = 44.f * u;
            const ReadTile& nr = T[nxt];
            float a0 = -1.5707963f;
            float sw = (float)(nr.dir * nr.turnDeg) * 3.14159265f / 180.f;
            float a1 = a0 + sw;

            dl->AddCircle(ImVec2(dcx, dcy), R, A(IM_COL32(255, 255, 255, 255), 0.12f), 0, 2.f * u);
            dl->AddCircleFilled(ImVec2(dcx, dcy), 4.5f * u, A(IM_COL32(255, 255, 255, 255), 0.35f));

            // 入射：从切线方向进入（t 方向 = (-sin a0, cos a0) 的反向）
            float tinx = -sinf(a0) * (float)nr.dir, tiny = cosf(a0) * (float)nr.dir;
            float ex0 = dcx + cosf(a0) * R, ey0 = dcy + sinf(a0) * R;
            dl->AddLine(ImVec2(ex0 - tinx * 46.f * u, ey0 - tiny * 46.f * u), ImVec2(ex0, ey0),
                        A(IM_COL32(200, 210, 226, 255), 0.75f), 2.2f * u);
            // 公转弧
            dl->PathArcTo(ImVec2(dcx, dcy), R, a0, a1, 48);
            dl->PathStroke(A1(ReadSpeedColor(nr.dt)), 0, 3.6f * u);
            // 出射切线 + 箭头
            float ex1 = dcx + cosf(a1) * R, ey1 = dcy + sinf(a1) * R;
            float toutx = -sinf(a1) * (float)nr.dir, touty = cosf(a1) * (float)nr.dir;
            float ex2 = ex1 + toutx * 46.f * u, ey2 = ey1 + touty * 46.f * u;
            dl->AddLine(ImVec2(ex1, ey1), ImVec2(ex2, ey2), A1(ReadSpeedColor(nr.dt)), 3.2f * u);
            {
                float ax = ex2, ay = ey2;
                float bx = -touty, by = toutx;
                dl->AddTriangleFilled(ImVec2(ax + toutx * 12.f * u, ay + touty * 12.f * u),
                                      ImVec2(ax + bx * 6.f * u, ay + by * 6.f * u),
                                      ImVec2(ax - bx * 6.f * u, ay - by * 6.f * u),
                                      A1(ReadSpeedColor(nr.dt)));
            }
            if (nr.turnDeg < 2.f)
                snprintf(line, sizeof(line), "直行");
            else if (nr.turnDeg > 178.f)
                snprintf(line, sizeof(line), "折返 %.0f°", nr.turnDeg);
            else
                snprintf(line, sizeof(line), "%.0f° %s", nr.turnDeg, nr.dir < 0 ? "逆" : "顺");
            TxtC(dcx, dcy + R + 16.f * u,
                 A1(nr.dir < 0 ? IM_COL32(120, 190, 255, 255) : IM_COL32(255, 190, 120, 255)),
                 line, 17.f * u);
            if (nr.midSpin)
                TxtC(dcx, dcy + R + 36.f * u, A1(IM_COL32(88, 168, 255, 255)), "中旋砖（自动）", 13.f * u);

            // 后续形状预览：把之后 4 块连成折线（形状记忆 > 数字记忆）
            {
                float sx = px + 214.f * u, sy = bodyBot + dialH - 26.f * u;
                float hx = 0.f, hy = -1.f;
                Txt(sx - 46.f * u, bodyBot + 12.f * u, A(IM_COL32(150, 158, 174, 255), 1.f), "后续形状", 13.f * u);
                for (int k = 0; k < 4; k++)
                {
                    int idx = nxt + k + 1;
                    if (idx >= total) break;
                    const ReadTile& r = T[idx];
                    float th = (float)(r.dir * r.turnDeg) * 3.14159265f / 180.f;
                    float nx = hx * cosf(th) - hy * sinf(th);
                    float ny = hx * sinf(th) + hy * cosf(th);
                    hx = nx; hy = ny;
                    float len = 26.f * u;
                    float ex = sx + hx * len, ey = sy + hy * len;
                    dl->AddLine(ImVec2(sx, sy), ImVec2(ex, ey), A(ReadSpeedColor(r.dt), 0.85f), 3.f * u);
                    dl->AddCircleFilled(ImVec2(ex, ey), 3.4f * u, A1(ReadSpeedColor(r.dt)));
                    sx = ex; sy = ey;
                }
                dl->AddCircleFilled(ImVec2(px + 214.f * u, bodyBot + dialH - 26.f * u), 3.4f * u,
                                    A(IM_COL32(200, 210, 226, 255), 0.8f));
            }
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    // ---------------- 主窗口：辅助读谱 设置页 ----------------
    static void ReadSection(const char* text)
    {
        ImGui::Dummy(ImVec2(0.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.36f, 0.95f, 0.80f, 1.f));
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x - 2.f;
        if (w > 8.f)
            ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 8.f), ImVec2(p.x + w, p.y + 8.f),
                                                IM_COL32(255, 255, 255, 26), 1.f);
        ImGui::Dummy(ImVec2(0.f, 4.f));
    }
    static void ReadRowLabel(const char* label, float colW)
    {
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        ImGui::SetCursorPosX(colW);
    }

    void DrawReadSettingsPage()
    {
        const float colW = LabelCol({ I18N::Tr(I18N::RD_LAYOUT), I18N::Tr(I18N::LBL_POS),
                                      I18N::Tr(I18N::RD_SCALE), I18N::Tr(I18N::RD_AHEAD) });

        // ---- 卡片 1：总开关 ----
        BeginCard4K("##cardR_main", 100.f);
        {
            bool en = s_readOn.load(std::memory_order_relaxed);
            if (TitleToggleRow("##read_en", 19.f, I18N::Tr(I18N::RD_TITLE), &en))
            {
                s_readOn.store(en, std::memory_order_relaxed);
                Log::Printf("[UI] sightread %s", en ? "on" : "off");
            }
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(158, 165, 182, 255));
            ImGui::TextWrapped("%s", I18N::Tr(I18N::RD_DESC));
            ImGui::PopStyleColor();
        }
        EndCard4K();
        ImGui::Spacing();

        // ---- 卡片 2：布局与位置 ----
        BeginCard4K("##cardR_layout", 172.f);
        {
            ReadSection(I18N::Tr(I18N::RD_SEC_LAYOUT));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 7.f));

            int lay = s_readLayout.load(std::memory_order_relaxed);
            const char* layCur = (lay == 1) ? I18N::Tr(I18N::RD_L_BOTTOM)
                              : (lay == 2) ? I18N::Tr(I18N::RD_L_TOP)
                                           : I18N::Tr(I18N::RD_L_SIDE);
            ReadRowLabel(I18N::Tr(I18N::RD_LAYOUT), colW);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::BeginCombo("##read_layout", layCur))
            {
                for (int li = 0; li < 3; li++)
                {
                    const char* nm = (li == 0) ? I18N::Tr(I18N::RD_L_SIDE)
                                   : (li == 1) ? I18N::Tr(I18N::RD_L_BOTTOM)
                                               : I18N::Tr(I18N::RD_L_TOP);
                    bool sel = (li == lay);
                    if (ImGui::Selectable(nm, sel) && !sel)
                        s_readLayout.store(li, std::memory_order_relaxed);
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 196, 120, 255));
            ImGui::TextWrapped("%s", I18N::Tr(I18N::RD_INPLAY_HINT));
            ImGui::PopStyleColor();

            int anchor = s_readAnchor.load(std::memory_order_relaxed);
            ReadRowLabel(I18N::Tr(I18N::LBL_POS), colW);
            ImGui::SetNextItemWidth(-14.f);
            const char* posNames[5] = { I18N::Tr(I18N::LBL_POS_LT), I18N::Tr(I18N::LBL_POS_RT),
                                        I18N::Tr(I18N::LBL_POS_LB), I18N::Tr(I18N::LBL_POS_RB),
                                        I18N::Tr(I18N::LBL_POS_CT) };
            if (anchor < 0 || anchor > 4) anchor = 0;
            if (ImGui::BeginCombo("##read_anchor", posNames[anchor]))
            {
                for (int ai = 0; ai < 5; ai++)
                {
                    bool sel = (ai == anchor);
                    if (ImGui::Selectable(posNames[ai], sel) && !sel)
                        s_readAnchor.store(ai, std::memory_order_relaxed);
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            int rx = s_readPosX.load(std::memory_order_relaxed);
            int ry = s_readPosY.load(std::memory_order_relaxed);
            const float kItemSp = ImGui::GetStyle().ItemSpacing.x;
            ReadRowLabel(I18N::Tr(I18N::LBL_FINETUNE), colW);
            float halfW = (ImGui::GetContentRegionAvail().x - kItemSp) * 0.5f;
            if (halfW < 70.f) halfW = 70.f;
            ImGui::SetNextItemWidth(halfW);
            if (ImGui::SliderInt("##read_px", &rx, -300, 300, "X %d"))
                s_readPosX.store(rx, std::memory_order_relaxed);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(halfW);
            if (ImGui::SliderInt("##read_py", &ry, -300, 300, "Y %d"))
                s_readPosY.store(ry, std::memory_order_relaxed);
            ImGui::PopStyleVar();
        }
        EndCard4K();
        ImGui::Spacing();

        // ---- 卡片 3：显示与校准 ----
        BeginCard4K("##cardR_view", 248.f);
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 7.f));
            ReadSection(I18N::Tr(I18N::RD_SEC_VIEW));

            int sc = s_readScale.load(std::memory_order_relaxed);
            ReadRowLabel(I18N::Tr(I18N::RD_SCALE), colW);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::SliderInt("##read_scale", &sc, 60, 180, "%d %%"))
                s_readScale.store(sc, std::memory_order_relaxed);

            int dn = s_readDensity.load(std::memory_order_relaxed);
            ReadRowLabel(I18N::Tr(I18N::RD_DENSITY), colW);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::SliderInt("##read_dens", &dn, 50, 200, "%d %%"))
                s_readDensity.store(dn, std::memory_order_relaxed);

            int opac = s_readOpacity.load(std::memory_order_relaxed);
            ReadRowLabel(I18N::Tr(I18N::LBL_OPACITY), colW);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::SliderInt("##read_opac", &opac, 40, 255))
                s_readOpacity.store(opac, std::memory_order_relaxed);

            ReadSection(I18N::Tr(I18N::RD_SEC_TUNE));

            int ah = s_readAhead.load(std::memory_order_relaxed);
            ReadRowLabel(I18N::Tr(I18N::RD_AHEAD), colW);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::SliderInt("##read_ahead", &ah, 3, 24, "%d"))
                s_readAhead.store(ah, std::memory_order_relaxed);

            int off = s_readOffsetMS.load(std::memory_order_relaxed);
            ReadRowLabel(I18N::Tr(I18N::RD_CALIB), colW);
            ImGui::SetNextItemWidth(-14.f);
            if (ImGui::SliderInt("##read_off", &off, -400, 400, "%d ms"))
                s_readOffsetMS.store(off, std::memory_order_relaxed);
            ImGui::PopStyleVar();
        }
        EndCard4K();
        ImGui::Spacing();

        // ---- 卡片 4：信息标记（2 列开关网格） ----
        BeginCard4K("##cardR_sig", 140.f);
        {
            ReadSection(I18N::Tr(I18N::RD_SEC_SIGNAL));
            const float rowX = ImGui::GetCursorPosX();
            const float colw2 = (ImGui::GetContentRegionAvail().x - 18.f) * 0.5f;
            auto Cell = [&](const char* id, const char* label, bool* v, bool second)
            {
                if (second)
                {
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(rowX + colw2);
                }
                bool t = *v;
                if (MiniToggle(id, &t)) *v = t;
                ImGui::SameLine();
                ImGui::TextUnformatted(label);
            };
            bool g = s_readGrid.load(std::memory_order_relaxed);
            bool d = s_readDial.load(std::memory_order_relaxed);
            Cell("##read_grid", I18N::Tr(I18N::RD_GRID), &g, false);
            Cell("##read_dial", I18N::Tr(I18N::RD_DIAL), &d, true);
            if (g != s_readGrid.load(std::memory_order_relaxed)) s_readGrid.store(g, std::memory_order_relaxed);
            if (d != s_readDial.load(std::memory_order_relaxed)) s_readDial.store(d, std::memory_order_relaxed);
            ImGui::Dummy(ImVec2(0.f, 2.f));
            bool rhy = s_readRhythm.load(std::memory_order_relaxed);
            bool mk = s_readMarks.load(std::memory_order_relaxed);
            Cell("##read_rhy", I18N::Tr(I18N::RD_RHYTHM), &rhy, false);
            Cell("##read_mk", I18N::Tr(I18N::RD_MARKS), &mk, true);
            if (rhy != s_readRhythm.load(std::memory_order_relaxed)) s_readRhythm.store(rhy, std::memory_order_relaxed);
            if (mk != s_readMarks.load(std::memory_order_relaxed)) s_readMarks.store(mk, std::memory_order_relaxed);
            ImGui::Dummy(ImVec2(0.f, 2.f));
            bool hint = s_readHint.load(std::memory_order_relaxed);
            Cell("##read_hint", I18N::Tr(I18N::RD_HINT), &hint, false);
            if (hint != s_readHint.load(std::memory_order_relaxed)) s_readHint.store(hint, std::memory_order_relaxed);
            ImGui::Dummy(ImVec2(0.f, 2.f));
            bool ang = s_readAngle.load(std::memory_order_relaxed);
            bool win = s_readWindows.load(std::memory_order_relaxed);
            Cell("##read_angle", I18N::Tr(I18N::RD_ANGLE), &ang, false);
            Cell("##read_win", I18N::Tr(I18N::RD_WINDOWS), &win, true);
            if (ang != s_readAngle.load(std::memory_order_relaxed)) s_readAngle.store(ang, std::memory_order_relaxed);
            if (win != s_readWindows.load(std::memory_order_relaxed)) s_readWindows.store(win, std::memory_order_relaxed);
            ImGui::Dummy(ImVec2(0.f, 2.f));
            bool ad = s_readAdapt.load(std::memory_order_relaxed);
            Cell("##read_adapt", I18N::Tr(I18N::RD_ADAPT), &ad, false);
            if (ad != s_readAdapt.load(std::memory_order_relaxed)) s_readAdapt.store(ad, std::memory_order_relaxed);
        }
        EndCard4K();
        ImGui::Spacing();

        // ---- 卡片 5：实时状态 ----
        BeginCard4K("##cardR_stat", 168.f);
        {
            ReadSection(I18N::Tr(I18N::RD_LIVE));
            std::shared_ptr<const std::vector<ReadTile>> snap = s_readSnap.load(std::memory_order_acquire);
            int total = s_readTotal.load(std::memory_order_relaxed);
            if (!snap || total < 3)
            {
                ImGui::TextDisabled("%s", I18N::Tr(I18N::RD_NODATA));
            }
            else
            {
                const std::vector<ReadTile>& Tv = *snap;
                double clock = RenderClockOff(s_readOffsetMS.load(std::memory_order_relaxed));
                int nxt = (int)(std::lower_bound(Tv.begin(), Tv.end(), clock,
                             [](const ReadTile& a, double v) { return a.t < v; }) - Tv.begin());
                if (nxt < 1) nxt = 1;
                if (nxt > total - 1) nxt = total - 1;
                int nvp = nxt;
                while (nvp < total - 1 && !Tv[nvp].valid) nvp++;

                const double bpmRaw = s_bpm.load(std::memory_order_relaxed);
                double beat = (bpmRaw > 20.0 && bpmRaw < 500.0) ? 60.0 / bpmRaw
                            : (Tv[nvp].dt > 0.05 ? Tv[nvp].dt * 2.0 : 0.5);
                char rh[16];
                ReadRhythmText(Tv[nvp].dt, beat, rh, sizeof(rh));
                double dtn = (Tv[nvp].t - clock) * 1000.0;

                ImGui::PushFont(nullptr, 24.f);
                ImGui::TextColored(dtn >= 0 ? ImVec4(0.36f, 0.95f, 0.80f, 1.f)
                                            : ImVec4(1.f, 0.55f, 0.55f, 1.f),
                                   "%+.0f ms", dtn);
                ImGui::PopFont();
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(170, 178, 194, 255));
                ImGui::TextUnformatted(I18N::Tr(dtn >= 0 ? I18N::LBL_FAST : I18N::LBL_SLOW));
                ImGui::PopStyleColor();

                ImGui::TextDisabled("%d / %d   BPM %.1f   %s   %.0f ms", nxt, total - 1,
                                    bpmRaw > 20.f ? bpmRaw : 60.0 / beat, rh, Tv[nvp].dt * 1000.0);
                if (Tv[nvp].valid)
                    ImGui::TextDisabled("%s %.0f°  %s", I18N::Tr(I18N::RD_DIAL),
                                        Tv[nvp].turnDeg, Tv[nvp].dir < 0 ? "CCW" : "CW");
                {
                    ReadJudge jr = ReadJudgeNow(clock, Tv, nvp);
                    ImU32 jc = ReadJudgeBandColor(jr);
                    ImVec4 jv(((jc >> IM_COL32_R_SHIFT) & 0xFF) / 255.f,
                              ((jc >> IM_COL32_G_SHIFT) & 0xFF) / 255.f,
                              ((jc >> IM_COL32_B_SHIFT) & 0xFF) / 255.f, 1.f);
                    if (jr.beyond && jr.errDeg < 0.0)
                        ImGui::TextColored(jv, "角误差 还差 %.0f°", -jr.errDeg);
                    else if (jr.beyond)
                        ImGui::TextColored(jv, "角误差 已过 %.0f°", jr.errDeg);
                    else
                        ImGui::TextColored(jv, "角误差 %+.0f°  %s", jr.errDeg, jr.errDeg < 0.0 ? "早" : "晚");
                    ImGui::TextDisabled("窗口 纯%.0f / 准%.0f / 有效%.0f°   ·   %s   %.0f°/s",
                                        jr.pure, jr.perfect, jr.counted,
                                        jr.live ? "实时角度" : "预测角度", jr.dps);
                }
                char hint[128];
                ReadHintText(Tv, nxt, hint, sizeof(hint));
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 196, 120, 255));
                ImGui::TextWrapped("%s", hint);
                ImGui::PopStyleColor();
            }
        }
        EndCard4K();
    }

    // ---------------- 辅助读谱设置访问器（配置档案整包保存用） ----------------
    //   which: 0=on 1=ahead 2=layout 3=anchor 4=posX 5=posY 6=scale 7=density
    //          8=opacity 9=offsetMS 10=grid 11=dial 12=rhythm 13=marks 14=hint
    //          15=angle 16=windows 17=adapt
    int ReadSettingGet(int which)
    {
        switch (which)
        {
        case 0:  return s_readOn.load(std::memory_order_relaxed) ? 1 : 0;
        case 1:  return s_readAhead.load(std::memory_order_relaxed);
        case 2:  return s_readLayout.load(std::memory_order_relaxed);
        case 3:  return s_readAnchor.load(std::memory_order_relaxed);
        case 4:  return s_readPosX.load(std::memory_order_relaxed);
        case 5:  return s_readPosY.load(std::memory_order_relaxed);
        case 6:  return s_readScale.load(std::memory_order_relaxed);
        case 7:  return s_readDensity.load(std::memory_order_relaxed);
        case 8:  return s_readOpacity.load(std::memory_order_relaxed);
        case 9:  return s_readOffsetMS.load(std::memory_order_relaxed);
        case 10: return s_readGrid.load(std::memory_order_relaxed) ? 1 : 0;
        case 11: return s_readDial.load(std::memory_order_relaxed) ? 1 : 0;
        case 12: return s_readRhythm.load(std::memory_order_relaxed) ? 1 : 0;
        case 13: return s_readMarks.load(std::memory_order_relaxed) ? 1 : 0;
        case 14: return s_readHint.load(std::memory_order_relaxed) ? 1 : 0;
        case 15: return s_readAngle.load(std::memory_order_relaxed) ? 1 : 0;
        case 16: return s_readWindows.load(std::memory_order_relaxed) ? 1 : 0;
        case 17: return s_readAdapt.load(std::memory_order_relaxed) ? 1 : 0;
        default: return 0;
        }
    }
    void ReadSettingSet(int which, int v)
    {
        switch (which)
        {
        case 0:  s_readOn.store(v != 0, std::memory_order_relaxed); break;
        case 1:  s_readAhead.store(std::max(3, std::min(24, v)), std::memory_order_relaxed); break;
        case 2:  s_readLayout.store(std::max(0, std::min(2, v)), std::memory_order_relaxed); break;
        case 3:  s_readAnchor.store(std::max(0, std::min(4, v)), std::memory_order_relaxed); break;
        case 4:  s_readPosX.store(std::max(-2000, std::min(2000, v)), std::memory_order_relaxed); break;
        case 5:  s_readPosY.store(std::max(-2000, std::min(2000, v)), std::memory_order_relaxed); break;
        case 6:  s_readScale.store(std::max(30, std::min(300, v)), std::memory_order_relaxed); break;
        case 7:  s_readDensity.store(std::max(30, std::min(300, v)), std::memory_order_relaxed); break;
        case 8:  s_readOpacity.store(std::max(0, std::min(255, v)), std::memory_order_relaxed); break;
        case 9:  s_readOffsetMS.store(std::max(-500, std::min(500, v)), std::memory_order_relaxed); break;
        case 10: s_readGrid.store(v != 0, std::memory_order_relaxed); break;
        case 11: s_readDial.store(v != 0, std::memory_order_relaxed); break;
        case 12: s_readRhythm.store(v != 0, std::memory_order_relaxed); break;
        case 13: s_readMarks.store(v != 0, std::memory_order_relaxed); break;
        case 14: s_readHint.store(v != 0, std::memory_order_relaxed); break;
        case 15: s_readAngle.store(v != 0, std::memory_order_relaxed); break;
        case 16: s_readWindows.store(v != 0, std::memory_order_relaxed); break;
        case 17: s_readAdapt.store(v != 0, std::memory_order_relaxed); break;
        default: break;
        }
    }
}
