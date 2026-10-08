#pragma once
// ============================================================
// AdoGen.h — 冰与火手法（ADOFAI 键盘手法）下坠谱生成核心
//
// 目标：像大佬那样打 —— 交互整段是交互、轮指整段是轮指；押（双押/
// 三押/四押/五押）按真实键位分工取内押/宽押/交叉押；变化只发生在
// 乐句边界（换手/换气），绝不逐音抖动，没人机味。
//
// 手法语汇（社区叫法）：
//   · 交互 —— 双手内键严格交替（4K F/J）。一段到底：段内不换手法、
//             不换手序。某一手的内键在冷却（同键 75ms / 长按）时，
//             用同一只手的其余键由内到外顶替，绝不跳到另一只手。
//   · 轮指 —— 单手两键严格交替（4K 右 J/K、左 F/D；5K 右 J/K、左
//             F/D；6K/10K 各自的内两指）。一段到底；长段在乐句边界
//             换手，换手不换手法。目标键冷却时用同对另一键顶替并保留
//             欠账，下一音回到目标键 —— 仍是严格交替，不出现同键连击。
//   · 混/插 —— 轮指段在乐句换气点允许一次点缀（每段至多一次，由乐句/
//             动机确定性选择）：混F（另一手内键单音）/ 插J（本手内键连
//             按两下）/ 插F-D（另一手内键快速两连 flam）。物理上做不到
//             （同键冷却）时自动退化为混F或跳过，绝不出散键。
//   · 押 —— 同刻多键。双押取内押/宽押/交叉押；三押 2+1 与 1+2 交替；
//             四押/五押取 2+2 / 全键 / 全键减一。取形状时同时考虑
//             与上一音不撞键、双手用量均衡、按键冷却。
//
// 去人机味：段内锁死手法；手法切换需要 dt 跨过门限并持续 >= 4 音
//           （迟滞 + 短段吸收）；换手/换气只在乐句边界；同一谱面 →
//           同一输出（确定性，不随重开漂移）。
// 物理约束：同键 >= laneFloorMs、长按占用一只手、押内不重复键。
//
// 本文件不依赖引擎（无 mono / ImGui），可独立编译单测。
// ============================================================
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <algorithm>

namespace AdoGen {

struct Ev
{
    double  t    = 0.0;    // 音符时刻（秒）
    double  dt   = 0.0;    // 与上一音间隔（秒）
    double  turn = 0.0;    // 本音回转角（度）
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
    int    tier        = 2;      // 0..3（由等级估算给出）
    double laneFloorMs = 75.0;   // 同键最小间隔
};

struct Stats
{
    int notes     = 0;
    int chords    = 0;
    int chordKeys = 0;
    int maxChord  = 0;
    int rollNotes = 0;
    int altNotes  = 0;
    int inserts   = 0;
    int switches  = 0;
    int pairFlips = 0;
};

namespace detail {

// 手别：5K（SDFJK）是 3+2 分工，若按 cols/2 会把左手的 F 划给右手，
// 轮指/交互/押全部错位 —— 按真实键位分工修正。
inline int SplitOf(int cols) { return (cols == 5) ? 3 : cols / 2; }
inline int HandOf(int lane, int cols) { return (lane < SplitOf(cols)) ? 0 : 1; }

// 手内键位（由内到外）：左 = [split-1 .. 0]，右 = [split .. cols-1]
inline int HandLanes(int hand, int cols, int* out)
{
    const int sp = SplitOf(cols);
    int n = 0;
    if (hand == 0) { for (int l = sp - 1; l >= 0; --l) out[n++] = l; }
    else           { for (int l = sp; l < cols; ++l)  out[n++] = l; }
    return n;
}

// 一对"手法键"（轮指对 / 交互对）。next = 欠账：0 → 下一音欠 a，1 → 欠 b。
struct Pair
{
    int a = -1, b = -1, next = 0;
    bool valid() const { return a >= 0 && b >= 0 && a != b; }
};

struct St
{
    int    cols  = 4;
    double floor = 0.075;
    double lastUse[16];
    double holdUntil[16];
    int    use[16];
    int    lastLane = -1, lastHand = -1;
    double lastT = -1e9;
    double busyUntil[2] = { -1e9, -1e9 };   // 长按占用：该手在此刻前不能出键
    int    tech = -1;                       // -1 未定 / 0 轮指 / 1 交互
    int    rollHand = -1;
    Pair   roll;                            // 轮指对（单手两键）
    Pair   alt;                             // 交互对（左内键、右内键）
    Pair   solo;                            // 一只手被长按占用时的单手游走对
    int    soloHand = -1;
    int    chordBySize[6] = { 0, 0, 0, 0, 0, 0 };   // 每种押大小的形状轮换计数（不互相挤占）
    int    breathUsed = 0;
    int    phrase = 0, motif = 0;
    int    phraseLen = 12, phraseLeft = 12;
    uint32_t rng = 1u;

    bool Ok(int l, double t) const
    {
        return l >= 0 && l < cols && holdUntil[l] <= t && (t - lastUse[l]) >= floor;
    }
    int Nearby(int lane, double t)
    {
        if (lane >= 0 && lane < cols && Ok(lane, t)) return lane;
        for (int d = 1; d < cols; d++)
        {
            if (lane - d >= 0 && Ok(lane - d, t)) return lane - d;
            if (lane + d < cols && Ok(lane + d, t)) return lane + d;
        }
        return std::max(0, std::min(cols - 1, lane));
    }
    // 同一只手内取可用键（由内到外）——手法不跨手的关键
    int SameHand(int hand, double t, int avoid)
    {
        int lanes[8];
        const int n = HandLanes(hand, cols, lanes);
        for (int i = 0; i < n; i++)
            if (lanes[i] != avoid && Ok(lanes[i], t)) return lanes[i];
        for (int i = 0; i < n; i++)
            if (Ok(lanes[i], t)) return lanes[i];
        return Nearby(lanes[0], t);
    }
    void Commit(int l, double t, float hold)
    {
        lastUse[l] = t; use[l]++;
        if (hold > 0.f)
        {
            holdUntil[l] = t + (double)hold + 0.05;
            const int h = HandOf(l, cols);
            busyUntil[h] = std::max(busyUntil[h], t + (double)hold);
        }
        lastLane = l; lastHand = HandOf(l, cols); lastT = t;
    }
    int FreeHandAt(double t) const
    {
        const bool l = busyUntil[0] <= t, r = busyUntil[1] <= t;
        if (l && r) return 2;
        if (l) return 0;
        if (r) return 1;
        return -1;
    }
    int HandLoad(int hand) const
    {
        int lanes[8];
        const int n = HandLanes(hand, cols, lanes);
        int s = 0;
        for (int i = 0; i < n; i++) s += use[lanes[i]];
        return s;
    }
    void SeedPair(Pair& p, int a, int b)
    {
        p.a = a; p.b = b;
        p.next = (lastLane == a) ? 1 : 0;      // a 刚响过 → 先欠 b
    }
    // 严格交替取键：目标键冷却 → 用同对另一键顶替且保留欠账（下一音仍回目标键）
    int TakePair(Pair& p, double t)
    {
        if (!p.valid()) return Nearby(0, t);
        const int want = p.next ? p.b : p.a;
        if (Ok(want, t)) { p.next ^= 1; return want; }
        const int other = (want == p.a) ? p.b : p.a;
        if (Ok(other, t)) return other;        // 欠账不推进
        p.next ^= 1;
        return Nearby(want, t);
    }
    void NewPhrase()
    {
        phrase++;
        phraseLen = 10 + (int)((rng >> 5) % 7u);
        motif = (int)((rng >> 11) % 6u);
        phraseLeft = phraseLen;
        rng = rng * 1664525u + 1013904223u;
    }
    void StepPhrase(int by)
    {
        if (by <= 0) return;
        phraseLeft -= by;
        if (phraseLeft <= 0) NewPhrase();
    }
};

// ---------------- 押（同刻多键）形状候选 ----------------
// 全部来自真实键位分工（左：SDF…，右：JKL…），不做别扭的跨手半押。
// 双押覆盖 内押/宽押/交叉押，三押 2+1 与 1+2 交替，四押/五押 2+2 或全键。
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

// 押形状选择：不与上一音撞键 / 双方手用量均衡 / 键位冷却，同分时按 variant
// 轮换（相邻押形状自然交替）。返回键数。
inline int PickChord(St& g, int cols, int k, int variant, double t, int* out)
{
    int cand[12][6], size[12];
    const int nc = ChordCandidates(cols, k, cand, size);
    if (nc <= 0) { out[0] = std::min(SplitOf(cols), cols - 1); return 1; }
    int best = -1;
    long bestScore = 0;
    for (int c = 0; c < nc; c++)
    {
        const int i = (variant % nc + c) % nc;
        if (size[i] != k) continue;
        long score = 0;
        int cnt[2] = { 0, 0 };
        for (int j = 0; j < size[i]; j++)
        {
            const int l = cand[i][j];
            if (l == g.lastLane && (t - g.lastT) <= 0.20) score += 2000;   // 紧挨上一音才避同指
            if (!g.Ok(l, t))     score += 600;       // 冷却中的键
            cnt[HandOf(l, cols)]++;
        }
        const int diff = std::abs((g.HandLoad(0) + cnt[0]) - (g.HandLoad(1) + cnt[1]));
        score += 6 * std::min(250, diff);
        if (best < 0 || score < bestScore) { best = i; bestScore = score; }
    }
    if (best < 0) best = 0;
    for (int j = 0; j < size[best]; j++) out[j] = cand[best][j];
    return size[best];
}

// 两只手都被长按占用时的兜底：取没被按住、最久未用的键
inline int PickFreeChord(St& g, int k, double t, int* out)
{
    const int cols = g.cols;
    int idx[16];
    for (int i = 0; i < cols; i++) idx[i] = i;
    auto cost = [&](int l) -> long {
        long c = 0;
        if (g.holdUntil[l] > t) c += 100000;
        if (!g.Ok(l, t))        c += 5000;
        c += g.use[l] * 10;
        if (l == g.lastLane)    c += 50;
        return c;
    };
    std::stable_sort(idx, idx + cols, [&](int p, int q) { return cost(p) < cost(q); });
    for (int i = 0; i < k; i++) out[i] = idx[i];
    return k;
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

    St g;
    g.cols  = cfg.cols;
    g.floor = cfg.laneFloorMs / 1000.0;
    for (int i = 0; i < 16; i++) { g.lastUse[i] = -1e9; g.holdUntil[i] = -1e9; g.use[i] = 0; }
    g.rng = 0x9E3779B9u ^ (uint32_t)(cfg.cols * 131 + cfg.tier * 7919);
    g.NewPhrase();

    // 轮指门限：只有持续快过门限（>=4 音）才整段轮指；其余整段交互
    const double rollDt =
        (cfg.tier >= 3) ? 0.070 : (cfg.tier == 2) ? 0.076 : (cfg.tier == 1) ? 0.084 : 0.095;
    const int breathEvery = (cfg.tier >= 3) ? 16 : (cfg.tier == 2) ? 14 : 12;

    int hl[2][8];
    const int hn[2] = { HandLanes(0, cfg.cols, hl[0]), HandLanes(1, cfg.cols, hl[1]) };

    // ---- 手法标签：迟滞（进轮指要明显更快，出轮指要明显更慢）+ 短段吸收 ----
    std::vector<uint8_t> lab((size_t)n);
    uint8_t cur = 1;
    for (int i = 0; i < n; i++)
    {
        if (ev[i].taps >= 2) { lab[(size_t)i] = 2; cur = 1; continue; }
        const double dt = ev[i].dt;
        if (dt > 0.0)
        {
            if (cur == 0) { if (dt > rollDt * 1.10) cur = 1; }
            else          { if (dt <= rollDt * 0.92) cur = 0; }
        }
        lab[(size_t)i] = cur;
    }
    for (int i = 0; i < n; )
    {
        int j = i + 1;
        while (j < n && lab[(size_t)j] == lab[(size_t)i]) j++;
        if (j - i < 4 && lab[(size_t)i] != 2)
        {
            uint8_t tgt = lab[(size_t)i];
            if (i > 0 && lab[(size_t)i - 1] != 2)  tgt = lab[(size_t)i - 1];
            else if (j < n && lab[(size_t)j] != 2) tgt = lab[(size_t)j];
            for (int k = i; k < j; k++) lab[(size_t)k] = tgt;
        }
        i = j;
    }

    int pos = 0;
    while (pos < n)
    {
        // ---------------- 押（同刻多键：双押/三押/四押/五押） ----------------
        if (ev[pos].taps >= 2)
        {
            const Ev& e = ev[pos];
            int K = (int)e.taps;
            if (K > cfg.cols) K = cfg.cols;
            if (K > 5) K = 5;
            if (K < 2) K = 2;
            int lanes[6] = { 0, 0, 0, 0, 0, 0 };
            int got = 0;
            const int fhC = g.FreeHandAt(e.t);
            if (fhC == 0 || fhC == 1)
            {
                // 一只手被长按占用：只能由另一只手在它自己的键数内取押
                got = std::min(K, hn[fhC]);
                for (int i = 0; i < got; i++) lanes[i] = hl[fhC][i];
            }
            else if (fhC == -1)
            {
                got = PickFreeChord(g, K, e.t, lanes);
            }
            else
            {
                got = PickChord(g, cfg.cols, K, g.chordBySize[K]++, e.t, lanes);
            }
            for (int i = 0; i < got; i++)          // 物理修复（占用/冷却中的键）
            {
                if (g.Ok(lanes[i], e.t)) continue;
                const int fixed = g.Nearby(lanes[i], e.t);
                bool dup = false;
                for (int j = 0; j < got; j++) if (j != i && lanes[j] == fixed) dup = true;
                if (!dup) lanes[i] = fixed;
            }
            out[(size_t)pos].primary = lanes[0];
            out[(size_t)pos].extraN  = got - 1;
            for (int i = 1; i < got; i++) out[(size_t)pos].extra[i - 1] = lanes[i];
            for (int i = 0; i < got; i++) g.Commit(lanes[i], e.t, (i == 0) ? e.hold : 0.f);
            if (st)
            {
                st->chords++;
                st->chordKeys += got;
                if (got > st->maxChord) st->maxChord = got;
            }
            // 押按掉了"欠账"键 → 换到同对另一键，避免同指马上连击
            bool used[16] = { false };
            for (int i = 0; i < got; i++) used[lanes[i]] = true;
            if (g.roll.valid() && used[g.roll.next ? g.roll.b : g.roll.a]) g.roll.next ^= 1;
            if (g.alt.valid()  && used[g.alt.next  ? g.alt.b  : g.alt.a])  g.alt.next  ^= 1;
            if (g.solo.valid() && used[g.solo.next ? g.solo.b : g.solo.a]) g.solo.next ^= 1;
            g.breathUsed = 0;
            g.StepPhrase(1);
            pos++;
            continue;
        }

        const uint8_t cls0 = lab[(size_t)pos];
        int end = pos + 1;
        while (end < n && lab[(size_t)end] == cls0) end++;
        const int segLen = end - pos;

        // 长按占用一只手 → 本段只能由另一只手游走（押轮切换）：强制轮指
        int fh = -1;
        for (int i = pos; i < end; i++)
        {
            const int f2 = g.FreeHandAt(ev[i].t);
            if (f2 == 0 || f2 == 1) { fh = f2; break; }
        }
        const bool forceHand = (fh == 0 || fh == 1);
        const uint8_t cls = forceHand ? 0 : cls0;

        // ---------------- 轮指段（单手两键，一段到底） ----------------
        if (cls == 0)
        {
            if (g.tech != 0 || !g.roll.valid() || (forceHand && g.rollHand != fh))
            {
                if (st) st->switches++;
                int hand;
                if (forceHand) hand = fh;
                else if (g.lastHand >= 0 && (ev[pos].t - g.lastT) <= 1.2) hand = g.lastHand;
                else
                {
                    const int l0 = g.HandLoad(0), l1 = g.HandLoad(1);
                    hand = (l0 == l1) ? (int)((g.phrase + g.motif) & 1) : (l0 < l1) ? 0 : 1;
                }
                g.rollHand = hand;
                g.SeedPair(g.roll, hl[hand][0],
                           (hn[hand] >= 2) ? hl[hand][1] : hl[hand][0]);
                if (st) st->pairFlips++;
                g.tech = 0;
                g.breathUsed = 0;
            }

            // 换气点（长段 + 大回转处；每段至多一次）
            int breathAt = -1;
            if (segLen >= breathEvery && g.breathUsed == 0)
            {
                for (int i = 1; i + 1 < segLen; i++)
                    if (ev[pos + i].turn >= 252.0) { breathAt = i; break; }
                if (breathAt < 0 && segLen >= breathEvery + 6) breathAt = segLen / 2;
            }
            const int variant = forceHand ? 1 : (int)((g.phrase * 2 + g.motif) % 3);
            const int phraseAtStart = g.phrase;
            int lastSwitchPhrase = -1;                   // 一次句界只换一次手（防逐音抖动）

            for (int k = 0; k < segLen; )
            {
                const int idx = pos + k;
                const int fhn = g.FreeHandAt(ev[idx].t);
                if ((fhn == 0 || fhn == 1) && fhn != g.rollHand)
                {
                    // 段中途换手轮（长按落在轮指手上）：改由空出的那只手严格轮指
                    if (g.soloHand != fhn || !g.solo.valid())
                    {
                        g.soloHand = fhn;
                        g.SeedPair(g.solo, hl[fhn][0],
                                   (hn[fhn] >= 2) ? hl[fhn][1] : hl[fhn][0]);
                        if (st) st->pairFlips++;
                    }
                    const int l = g.TakePair(g.solo, ev[idx].t);
                    out[(size_t)idx].primary = l;
                    g.Commit(l, ev[idx].t, ev[idx].hold);
                    if (st) st->rollNotes++;
                    g.StepPhrase(1);
                    k++;
                    continue;
                }
                if (k == breathAt && k + 1 < segLen)
                {
                    const int other = 1 - HandOf(g.roll.a, cfg.cols);
                    const int o0 = hl[other][0];
                    const int o1 = (hn[other] >= 2) ? hl[other][1] : hl[other][0];
                    const int owed = g.roll.next ? g.roll.b : g.roll.a;
                    int v = variant;
                    if (v == 1 && !(g.Ok(owed, ev[idx].t) && g.Ok(owed, ev[idx + 1].t)))
                        v = 0;                                   // 插J 做不到 → 混F
                    if (v == 2 && !(g.Ok(o0, ev[idx].t) && g.Ok(o1, ev[idx + 1].t)))
                        v = 0;                                   // 插F-D 做不到 → 混F
                    if (v == 0 && !g.Ok(o0, ev[idx].t))
                        v = -1;                                  // 都做不到 → 跳过点缀
                    if (v == 0)                                      // 混F：另一手内键单音
                    {
                        const int l = g.Nearby(o0, ev[idx].t);
                        out[(size_t)idx].primary = l;
                        g.Commit(l, ev[idx].t, ev[idx].hold);
                        k += 1;
                    }
                    else if (v == 1)                                 // 插J：本手内键连按两下
                    {
                        const int l = g.Nearby(owed, ev[idx].t);
                        out[(size_t)idx].primary = l;
                        g.Commit(l, ev[idx].t, ev[idx].hold);
                        const int l2 = g.Nearby(owed, ev[idx + 1].t);
                        out[(size_t)idx + 1].primary = l2;
                        g.Commit(l2, ev[idx + 1].t, ev[idx + 1].hold);
                        g.roll.next = (owed == g.roll.a) ? 1 : 0;
                        k += 2;
                    }
                    else if (v == 2)                                 // 插F-D：另一手内键快速两连
                    {
                        const int l = g.Nearby(o0, ev[idx].t);
                        out[(size_t)idx].primary = l;
                        g.Commit(l, ev[idx].t, ev[idx].hold);
                        const int l2 = g.Nearby(o1, ev[idx + 1].t);
                        out[(size_t)idx + 1].primary = l2;
                        g.Commit(l2, ev[idx + 1].t, ev[idx + 1].hold);
                        k += 2;
                    }
                    if (v >= 0)
                    {
                        g.breathUsed = 1;
                        if (st) st->inserts++;
                        g.StepPhrase((v == 0) ? 1 : 2);
                        continue;
                    }
                }
                const int l = g.TakePair(g.roll, ev[idx].t);         // 严格交替
                out[(size_t)idx].primary = l;
                g.Commit(l, ev[idx].t, ev[idx].hold);
                if (st) st->rollNotes++;
                g.StepPhrase(1);
                // 长段：每两条乐句在句界换手一次（换手不换手法，防手酸）
                if (!forceHand && segLen >= 30 && g.FreeHandAt(ev[idx].t) == 2 &&
                    g.phrase != lastSwitchPhrase &&
                    (g.phrase - phraseAtStart) >= 2 && ((g.phrase - phraseAtStart) & 1) == 0)
                {
                    lastSwitchPhrase = g.phrase;
                    g.rollHand = 1 - g.rollHand;
                    g.SeedPair(g.roll, hl[g.rollHand][0],
                               (hn[g.rollHand] >= 2) ? hl[g.rollHand][1] : hl[g.rollHand][0]);
                    if (st) st->pairFlips++;
                }
                k++;
            }
            pos = end;
            continue;
        }

        // ---------------- 交互段（双手内键，一段到底） ----------------
        if (g.tech != 1 || !g.alt.valid())
        {
            if (st) st->switches++;
            g.SeedPair(g.alt, hl[0][0], hl[1][0]);
            if (g.lastLane == g.alt.a)      g.alt.next = 1;      // F 刚响 → 先 J
            else if (g.lastLane == g.alt.b) g.alt.next = 0;
            else if (g.lastLane >= 0)       g.alt.next = (HandOf(g.lastLane, cfg.cols) == 0) ? 1 : 0;
            else                            g.alt.next = (int)((g.phrase + g.motif) & 1);
            g.tech = 1;
        }
        int accentLeft = 2;
        for (int k = 0; k < segLen; k++)
        {
            const int idx = pos + k;
            const Ev& e2 = ev[idx];
            const int fhn = g.FreeHandAt(e2.t);
            if (fhn == 0 || fhn == 1)
            {
                // 一只手被长按占用 → 该手不出键，改由另一只手严格轮指顶上
                if (g.soloHand != fhn || !g.solo.valid())
                {
                    g.soloHand = fhn;
                    g.SeedPair(g.solo, hl[fhn][0],
                               (hn[fhn] >= 2) ? hl[fhn][1] : hl[fhn][0]);
                    if (st) st->pairFlips++;
                }
                const int l = g.TakePair(g.solo, e2.t);
                out[(size_t)idx].primary = l;
                g.Commit(l, e2.t, e2.hold);
                if (st) st->rollNotes++;
                g.StepPhrase(1);
                continue;
            }
            const bool accent = (e2.turn >= 252.0) && (e2.dt >= 0.35) && accentLeft > 0;
            int l;
            if (accent)
            {
                accentLeft--;
                const int hand = g.alt.next;                      // 本音轮到的手
                int lanes[8];
                const int n2 = HandLanes(hand, cfg.cols, lanes);
                l = -1;
                for (int i = std::min(1, n2 - 1); i >= 0; i--)    // 优先外键做重音
                    if (g.Ok(lanes[i], e2.t)) { l = lanes[i]; break; }
                if (l < 0) l = g.Nearby(lanes[0], e2.t);
            }
            else
            {
                const int want = g.alt.next ? g.alt.b : g.alt.a;
                l = g.Ok(want, e2.t) ? want : g.SameHand(HandOf(want, cfg.cols), e2.t, want);
            }
            g.alt.next ^= 1;                                      // 严格手序交替
            out[(size_t)idx].primary = l;
            g.Commit(l, e2.t, e2.hold);
            if (st) st->altNotes++;
            g.StepPhrase(1);
        }
        pos = end;
    }
}

} // namespace AdoGen
