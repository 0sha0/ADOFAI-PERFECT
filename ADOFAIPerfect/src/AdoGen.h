#pragma once
// ============================================================
// AdoGen.h — 冰与火（ADOFAI）键盘手法：拆手序（hand-split）排键引擎
//
// 手法来源（公开实现）：
//   · ReADOFAIMacro（作者 Tony Limps，MIT）的按键分配 allocateFingers()：
//       BPM 归一化 → 按"归一化后的一拍"把音符切成窗口 → 窗口内的音符数决定指法
//       （1 个 = 单指；N 个 = 同一只手的 N 根手指；超过一只手容量就对半分给两手递归）
//       → 这个窗口的度数决定"下一个窗口换不换手"。
//   · 参考移植与逐窗口 oracle：fallingCB/LaneReader 的 HandPlanner.cs
//     （该移植已与参考实现 28 张谱 / 17558 个窗口逐窗口 100% 对齐）。
//
// 为什么这是"大佬打法"而不是"把键铺满"：
//   · 慢音（窗口 >= 1 拍）→ 同一根手指重复（叠键），而不是到处乱跑；
//   · 一拍左右的音 → 两手交替（左内键 / 右内键）；
//   · 一拍里挤进 >= 2 个音 → 单手两指轮指（外轮 K J / D F）；
//   · 只有"一拍内的音多到超过一只手的手指容量"时才拆到另一只手。
//   全程手位停在食指/中指的内侧，没有"为了用到每一个键而移动手位"的行为。
//
// 只用音符时间：degree ≡ 3 × Δt(秒) × 归一化BPM（180° = 归一化后的一拍）。
// 同键物理约束（>= laneFloorMs）与长按占用只在"人手真的做不到"时才顶替到同手邻指。
//
// 本文件不依赖引擎（无 mono / ImGui），可独立编译单测。
// ============================================================
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <algorithm>
#include <functional>

namespace AdoGen {

struct Ev
{
    double  t    = 0.0;    // 音符时刻（秒）
    double  dt   = 0.0;    // 与上一音间隔（秒）
    double  turn = 0.0;    // 本音回转角（度，保留给重音/未来使用）
    int     dir  = 1;      // +1 CW / -1 CCW
    float   hold = 0.f;    // 长按（秒，仅首音）
    uint8_t taps = 1;      // 同刻点击数（>=2 → 押）
};

struct Lane
{
    int primary  = 0;
    int extraN   = 0;
    int extra[4] = { 0, 0, 0, 0 };
};

struct Cfg
{
    int    cols        = 4;
    int    tier        = 2;       // 0..3（由等级估算给出；仅用于日志/统计）
    double laneFloorMs = 75.0;    // 同键最小间隔
    double levelBpm    = 0.0;     // 关卡 BPM（<=0 时按参考实现兜底 100）
    double maxHardResistBpm = 840.0;  // "最大硬抗 BPM"：越低越偏轮指（参考实现默认 840）
    bool   outerRoll   = true;    // 轮指方向：true = 外轮 K J / D F；false = 内轮 J K / F D
};

struct Stats
{
    int notes      = 0;
    int windows    = 0;
    int count1     = 0;      // 单指窗口（换手 → 两手交替 / 慢音 → 单指重复）
    int count2     = 0;      // 两手两指轮指窗口
    int count3     = 0;
    int count4plus = 0;
    int rolls      = 0;      // count >= 2 的窗口数
    int taps       = 0;      // count == 1 的窗口数
    int handSwitches = 0;    // 换手次数（窗口级）
    int chords     = 0;
    int chordKeys  = 0;
    int maxChord   = 0;
    int swaps      = 0;      // 物理顶替次数（同键冷却/长按占用）
    int holds      = 0;
    double baseBpm = 0.0;    // 归一化后的 BPM（窗口单位）
};

namespace detail {

// 手别：5K（SDFJK）是 3+2 分工，若按 cols/2 会把左手的 F 划给右手。
inline int SplitOf(int cols)
{
    switch (cols)
    {
    case 4:  return 2;    // DF | JK
    case 5:  return 3;    // SDF | JK
    case 6:  return 3;    // SDF | JKL
    case 10: return 5;    // ASDFG | HJKL;
    case 16: return 8;    // PAD 左二列 | 右二列
    default: return cols / 2;
    }
}
inline int HandOf(int lane, int cols) { return (lane < SplitOf(cols)) ? 0 : 1; }

// 手内键位（由内到外）：左 = [split-1 .. 0]，右 = [split .. cols-1]
// slot 1 = 食指位（内）、slot 2 = 中指位、slot 3 = 无名指……与拆手序的 p1/p2 对应。
inline int HandLanes(int hand, int cols, int* out)
{
    const int sp = SplitOf(cols);
    int n = 0;
    if (hand == 0) { for (int l = sp - 1; l >= 0; --l) out[n++] = l; }
    else           { for (int l = sp; l < cols; ++l)  out[n++] = l; }
    return n;
}

// 参考实现的 BPM 归一化，逐字移植：
//   while (bpm <= 550) bpm *= 2;  while (bpm > 1100) bpm /= 2;
//   if (bpm > 最大硬抗BPM) bpm /= 2;
inline double NormaliseBpm(double levelBpm, double maxHardResistBpm)
{
    double bpm = levelBpm;
    if (!std::isfinite(bpm) || bpm <= 0.0) bpm = 100.0;   // 参考实现的兜底
    while (bpm <= 550.0) bpm *= 2.0;
    while (bpm > 1100.0) bpm /= 2.0;
    if (maxHardResistBpm > 0.0 && bpm > maxHardResistBpm) bpm /= 2.0;
    return bpm;
}

// 参考实现的换手规则，逐字移植（degree = 该窗口的归一化度数）：
//   <= 270 换手 / <= 450 不换 / <= 630 换 / < 720 不换 / >= 720 回主手
inline bool NextPreferred(bool preferred, double degree)
{
    const double eps = 1e-9;
    if (degree <= 270.0 + eps) return !preferred;
    if (degree <= 450.0 + eps) return preferred;
    if (degree <= 630.0 + eps) return !preferred;
    if (degree <  720.0 - eps) return preferred;
    return true;
}

// 押（同刻多键）形状候选：全部来自真实键位分工，不做别扭的跨手半押。
// 双押覆盖内押/宽押/交叉押；三押 2+1 与 1+2 交替；四押/五押 2+2 或全键。
inline int ChordCandidates(int cols, int k, int cand[12][6], int size[12])
{
    int c = 0;
    auto add = [&](std::initializer_list<int> a) {
        int m = 0;
        for (int v : a) cand[c][m++] = v;
        size[c++] = m;
    };
    if (k < 2) k = 2;
    if (k > 5) k = 5;
    if (k > cols) k = cols;
    switch (cols)
    {
    case 4:   // D0 F1 | J2 K3
        if (k == 2)      { add({1,2}); add({0,3}); add({0,2}); add({1,3}); }
        else if (k == 3) { add({0,1,2}); add({1,2,3}); add({0,2,3}); add({0,1,3}); }
        else             { add({0,1,2,3}); }
        break;
    case 5:   // S0 D1 F2 | J3 K4
        if (k == 2)      { add({2,3}); add({1,4}); add({2,4}); add({1,3}); }
        else if (k == 3) { add({1,2,3}); add({2,3,4}); add({1,2,4}); add({1,3,4}); }
        else if (k == 4) { add({1,2,3,4}); add({0,1,3,4}); }
        else             { add({0,1,2,3,4}); }
        break;
    case 6:   // S0 D1 F2 | J3 K4 L5
        if (k == 2)      { add({2,3}); add({1,4}); add({0,5}); add({1,3}); add({2,4}); }
        else if (k == 3) { add({1,2,3}); add({2,3,4}); add({1,2,4}); add({1,3,4}); add({2,4,5}); add({0,1,3}); }
        else if (k == 4) { add({1,2,3,4}); add({0,1,4,5}); add({1,2,4,5}); add({0,1,3,4}); }
        else if (k == 5) { add({1,2,3,4,5}); add({0,1,2,3,4}); }
        else             { add({0,1,2,3,4,5}); }
        break;
    default:  // 10K  A0 S1 D2 F3 G4 | H5 J6 K7 L8 ;9
        if (k == 2)      { add({4,5}); add({3,6}); add({2,7}); add({1,8}); add({0,9}); add({3,5}); add({4,6}); }
        else if (k == 3) { add({3,4,5}); add({4,5,6}); add({2,3,5}); add({4,6,7}); add({3,4,6}); add({4,5,7}); }
        else if (k == 4) { add({3,4,5,6}); add({2,3,4,5}); add({4,5,6,7}); add({2,3,5,6}); add({3,4,6,7}); }
        else             { add({3,4,5,6,7}); add({2,3,4,5,6}); add({4,5,6,7,8}); add({1,2,3,4,5}); }
        break;
    }
    return c;
}

} // namespace detail

// ---------------- 主生成 ----------------
inline void Generate(const Ev* ev, int n, const Cfg& cfg, std::vector<Lane>& out, Stats* st)
{
    using namespace detail;
    out.assign((size_t)(n > 0 ? n : 0), Lane{});
    if (st) *st = Stats{};
    if (n <= 0 || cfg.cols < 4) return;
    if (st) st->notes = n;

    const int    cols  = cfg.cols;
    const double floor = cfg.laneFloorMs / 1000.0;
    const double baseBpm = NormaliseBpm(cfg.levelBpm, cfg.maxHardResistBpm);
    if (st) st->baseBpm = baseBpm;

    int hl[2][8];
    const int hn[2] = { HandLanes(0, cols, hl[0]), HandLanes(1, cols, hl[1]) };

    double lastUse[16];
    double holdUntil[16];
    for (int i = 0; i < 16; i++) { lastUse[i] = -1e9; holdUntil[i] = -1e9; }

    // 一个音的度数：180° = 归一化后的一拍 = 3 × Δt × baseBpm。
    // 末音取"前一段间隔"（参考实现用末尾哨兵复制角度，等价于这个处理）。
    auto DegOf = [&](int i) -> double {
        double sec;
        if (i + 1 < n)      sec = ev[i + 1].t - ev[i].t;
        else if (i > 0)     sec = ev[i].t - ev[i - 1].t;
        else                return 0.0;
        if (!(sec > 0.0)) sec = 0.0;                 // 回卷 / 同刻
        return 180.0 * sec * baseBpm / 60.0;
    };

    std::vector<int> laneAt((size_t)n, 0);

    // 落一个指法：slot 是手内序号（1 = 食指位）。
    // 只有当"同键在冷却/被长按占用/与上一音同键且间隔小于物理下限"时才顶替到同手邻指——
    // 这是保真的：参考实现是宏，不需要考虑人手，我们只补人手真做不到的那部分。
    auto Place = [&](int idx, int hand, int slot) {
        const double t = ev[idx].t;
        int h = hand, s = slot;
        if (s < 1) s = 1;
        if (s > hn[h]) s = hn[h];
        const bool avoidPrev = (idx > 0) && ((t - ev[idx - 1].t) < floor);
        const int  prevL = (idx > 0) ? laneAt[(size_t)idx - 1] : -1;

        auto okLane = [&](int l) {
            if (l < 0 || l >= cols) return false;
            if (holdUntil[l] > t) return false;
            if ((t - lastUse[l]) < floor) return false;
            if (avoidPrev && l == prevL) return false;
            return true;
        };
        int l = hl[h][s - 1];
        if (!okLane(l))
        {
            int pick = -1;
            for (int d = 1; d < hn[h]; d++)
            {
                const int s1 = s - d, s2 = s + d;
                if (s1 >= 1 && okLane(hl[h][s1 - 1])) { pick = hl[h][s1 - 1]; break; }
                if (s2 <= hn[h] && okLane(hl[h][s2 - 1])) { pick = hl[h][s2 - 1]; break; }
            }
            if (pick >= 0) { l = pick; if (st) st->swaps++; }
            else           { if (avoidPrev && l == prevL) l = hl[h][(s % hn[h])]; }
        }
        laneAt[(size_t)idx] = l;
        lastUse[l] = t;
        if (ev[idx].hold > 0.f) { holdUntil[l] = t + (double)ev[idx].hold + 0.05; if (st) st->holds++; }
        out[(size_t)idx].primary = l;
        return l;
    };

    // 窗口内按指法分配，逐字移植参考实现的 Emit/allocateFingers：
    //   window <= 该手容量 → 该手按 slot 顺序（外轮 = 降序 N..1，内轮 = 升序 1..N）；
    //   否则对半拆给两手递归（起手的那只手拿较小的那一半）。
    std::function<void(int,int,bool)> Emit = [&](int first, int window, bool mainHand) {
        if (window <= 0) return;
        const int hand = mainHand ? 1 : 0;                 // 主手 = 右手
        if (window <= hn[hand])
        {
            for (int i = 0; i < window; i++)
            {
                const int slot = cfg.outerRoll ? (window - i) : (i + 1);
                Place(first + i, hand, slot);
            }
            return;
        }
        const int half = window / 2;
        Emit(first, half, mainHand);
        Emit(first + half, window - half, !mainHand);
    };

    // 押（同刻多键）：参考实现只有单行星，这里按真实键位分工取形状（不与上一音撞键、
    // 避开冷却/占用），并当作一个"窗口"推进换手状态。
    int chordVariant[6] = { 0, 0, 0, 0, 0, 0 };
    auto PlaceChord = [&](int idx, int K) {
        int cand[12][6], size[12];
        const int nc = ChordCandidates(cols, K, cand, size);
        int lanes[6] = { 0, 0, 0, 0, 0, 0 };
        int got = 0;
        if (nc > 0)
        {
            int best = -1; long bestScore = 0;
            const int v = chordVariant[K]++;
            for (int c = 0; c < nc; c++)
            {
                const int i = (v % nc + c) % nc;
                if (size[i] != K) continue;
                long score = 0;
                for (int j = 0; j < size[i]; j++)
                {
                    const int l = cand[i][j];
                    const double t = ev[idx].t;
                    if (idx > 0 && l == laneAt[(size_t)idx - 1] && (t - ev[idx - 1].t) < floor) score += 2000;
                    if (holdUntil[l] > t)                score += 900;
                    else if ((t - lastUse[l]) < floor)   score += 600;
                }
                if (best < 0 || score < bestScore) { best = i; bestScore = score; }
            }
            if (best < 0) best = 0;
            got = size[best];
            for (int j = 0; j < got; j++) lanes[j] = cand[best][j];
        }
        if (got <= 0) { got = 1; lanes[0] = hl[1][0]; }
        // 物理修复：占用/冷却中的键 → 换同手邻键（不重复）
        const double t = ev[idx].t;
        for (int i = 0; i < got; i++)
        {
            if (holdUntil[lanes[i]] <= t && (t - lastUse[lanes[i]]) >= floor) continue;
            int fixed = -1;
            for (int l = 0; l < cols; l++)
            {
                if (holdUntil[l] > t || (t - lastUse[l]) < floor) continue;
                bool dup = false;
                for (int j = 0; j < got; j++) if (j != i && lanes[j] == l) dup = true;
                if (!dup) { fixed = l; break; }
            }
            if (fixed >= 0) lanes[i] = fixed;
        }
        out[(size_t)idx].primary = lanes[0];
        out[(size_t)idx].extraN  = got - 1;
        for (int i = 1; i < got; i++) out[(size_t)idx].extra[i - 1] = lanes[i];
        for (int i = 0; i < got; i++)
        {
            laneAt[(size_t)idx] = lanes[i];
            lastUse[lanes[i]] = t;
            if (i == 0 && ev[idx].hold > 0.f) holdUntil[lanes[i]] = t + (double)ev[idx].hold + 0.05;
        }
        if (st)
        {
            st->chords++; st->chordKeys += got;
            if (got > st->maxChord) st->maxChord = got;
        }
    };

    // ---- 主循环：切窗口 → 分配指法 → 决定下一窗换不换手 ----
    bool preferred = true;         // true = 主手（右手）
    int  first = 0;
    while (first < n)
    {
        if (ev[first].taps >= 2)
        {
            int K = (int)ev[first].taps;
            if (K > cols) K = cols;
            if (K > 5) K = 5;
            PlaceChord(first, K);
            if (st) { st->windows++; st->count1++; st->taps++; }
            preferred = NextPreferred(preferred, DegOf(first));
            first++;
            continue;
        }

        double degree = DegOf(first);
        int window = 1;
        while (degree < 180.0 - 1e-9 && first + window < n && ev[first + window].taps < 2)
        {
            degree += DegOf(first + window);
            window++;
        }

        const bool wasPreferred = preferred;
        Emit(first, window, preferred);

        if (st)
        {
            st->windows++;
            if (window == 1)      st->count1++;
            else if (window == 2) { st->count2++; st->rolls++; }
            else if (window == 3) { st->count3++; st->rolls++; }
            else                  { st->count4plus++; st->rolls++; }
            if (window == 1) st->taps++;
        }

        preferred = NextPreferred(preferred, degree);
        if (preferred != wasPreferred && st) st->handSwitches++;
        first += window;
    }
}

} // namespace AdoGen