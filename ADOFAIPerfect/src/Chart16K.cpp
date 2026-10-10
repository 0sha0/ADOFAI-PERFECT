// ============================================================
// Chart16K.cpp - 16K（PAD / 4x4）引擎  [完全独立算法]
//
//   PAD 不是下落式键盘模式，而是 jubeat / Malody Pad 的 4x4 方形面板。
//   （Malody 客户端 IL2CPP dump 证据：bbbg.NotePad { ushort index;
//   ushort endIndex; } 共 16 块面板 0..15；WidgetPadPanel 暴露 16 个
//   WidgetPadKeySprite。）面板谱是"空间谱"：写谱语汇 = 邻接移动 /
//   绕环 / 阶梯 / 十字图形 / 双板交互 / 押取聚簇 + 拆手，
//   绝不是把单轨流摊到 16 块板上（那正是旧版的问题）。
//
//   经典 16K 手法（音游社区通用语汇，jubeat / Malody Pad 写谱惯例）：
//     0 经典 Classic  邻接移动为主 + 左右手分区平衡 + 高速段强制换手
//     1 绕环 Ring     沿外圈 12 板回旋（回旋图形），方向跟随星球转向
//     2 阶梯 Stair    沿行 / 列 / 对角线轨道的单调阶梯，到端点折返
//     3 十字 Cross    沿十字臂（横臂 / 竖臂）行走的图形谱（図形ネタ）
//     4 交互 Trill    双板交替（音押 trill）
//     5 冰火拆手序    ADOFAI 专用：星球逐砖交替（火 / 冰）→ 左右手
//                     严格交替（偶数音 = 火手区，奇数音 = 冰手区）；
//                     押（同刻多键）按拆手序分配：第 1 点给当前手区、
//                     第 2 点给另一手区、依此类推，各点落在本手区内
//
//   index = row*4 + col，row 0 = 最上行（与 ChartCore.cpp 的面板渲染、
//   Malody Pad 布局一致）。手分区按 jubeat 惯例：左两列 = 左手（火），
//   右两列 = 右手（冰）。
//
//   本文件刻意独立：与 4K/5K/6K/10K 轨道引擎不共用任何代码。
//   同一谱面 + 同一风格 → 输出稳定（确定性 RNG）。
// ============================================================
#include "ChartCore.h"
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace Chart4K
{
namespace
{
    // ---------------- 面板几何 ----------------
    inline int P16Row(int i)  { return i >> 2; }
    inline int P16Col(int i)  { return i & 3; }
    inline int P16Hand(int i) { return (i & 3) < 2 ? 0 : 1; }   // 左两列 / 右两列
    inline int P16Abs(int v)  { return v < 0 ? -v : v; }
    inline int P16Dist(int a, int b)                            // Chebyshev 距离
    {
        const int dr = P16Abs(P16Row(a) - P16Row(b));
        const int dc = P16Abs(P16Col(a) - P16Col(b));
        return dr > dc ? dr : dc;
    }

    // 外圈 12 板，顺时针（左上 → 右上 → 右下 → 左下）
    const int kRing[12] = { 0, 1, 2, 3, 7, 11, 15, 14, 13, 12, 8, 4 };
    const int kCorner[4] = { 0, 3, 12, 15 };
    const int kInner[4]  = { 5, 6, 9, 10 };

    // 阶梯轨道：4 行 + 4 列 + 2 条主对角线（各 4 板）
    const int kTracks[10][4] = {
        { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 8, 9, 10, 11 }, { 12, 13, 14, 15 },   // 行
        { 0, 4, 8, 12 }, { 1, 5, 9, 13 }, { 2, 6, 10, 14 }, { 3, 7, 11, 15 },   // 列
        { 0, 5, 10, 15 }, { 3, 6, 9, 12 },                                      // 对角线
    };
    const int kTrackN = 10;

    // 十字臂：两条横臂（row1 / row2）+ 两条竖臂（col1 / col2）
    const int kArm[4][4] = {
        { 4, 5, 6, 7 },     // 横臂上
        { 8, 9, 10, 11 },   // 横臂下
        { 1, 5, 9, 13 },    // 竖臂左
        { 2, 6, 10, 14 },   // 竖臂右
    };

    // 手区面板（火 = 左两列，冰 = 右两列）
    inline int P16ZonePanel(int hand, int r, int cHalf)
    {
        const int c = hand * 2 + cHalf;
        return r * 4 + c;
    }

    struct P16
    {
        int      last = -1;
        double   lastT = -1e9;
        double   lastUse[16];
        double   holdUntil[16];
        int      use[16];
        double   floor = 0.085;          // 同板最小再按间隔
        int      jumpCap = 3;            // 难度相关的最大跨距
        uint32_t rng = 0x9E3779B9u;
        int      lastHand[2] = { -1, -1 };   // 每只手最近按的面板（拆手序用）

        // 绕环
        int ringPos = 0;
        // 阶梯
        int stairTrack = 0;
        int stairPos   = 0;
        int stairDir   = 1;      // 沿轨道行走方向（端点折返时翻转）
        // 十字
        int crossArm = 0;
        int crossPos = 0;
        // 交互
        int trillA = 0, trillB = 1, trillNext = 0;
        // 乐句（用于周期性换形）
        int phraseLeft = 0;
        int repeat = 0;

        uint32_t Rnd() { rng = rng * 1664525u + 1013904223u; return rng; }
        bool Ok(int i, double t) const
        {
            return i >= 0 && i < 16 && (t - lastUse[i]) >= floor && holdUntil[i] <= t;
        }
        void Commit(int i, double t, float hold)
        {
            lastUse[i] = t; use[i]++; last = i; lastT = t;
            lastHand[P16Hand(i)] = i;
            if (hold > 0.f) holdUntil[i] = t + (double)hold + 0.05;
        }
        void NewPhrase(int len) { phraseLeft = len; }
        void Step()             { if (--phraseLeft <= 0) phraseLeft = 8 + (int)(Rnd() % 7u); }
    };

    // 兜底：附近最近可用面板；全占用则取最久未用面板
    int Nearest(P16& s, int lane, double t)
    {
        if (lane >= 0 && lane < 16 && s.Ok(lane, t)) return lane;
        if (lane < 0) lane = 5;
        const int r0 = P16Row(lane), c0 = P16Col(lane);
        for (int d = 1; d <= 6; d++)
        {
            for (int dr = -d; dr <= d; dr++)
            {
                for (int dc = -d; dc <= d; dc++)
                {
                    const int cheb = P16Abs(dr) > P16Abs(dc) ? P16Abs(dr) : P16Abs(dc);
                    if (cheb != d) continue;
                    const int rr = r0 + dr, cc = c0 + dc;
                    if (rr < 0 || rr > 3 || cc < 0 || cc > 3) continue;
                    const int i = rr * 4 + cc;
                    if (s.Ok(i, t)) return i;
                }
            }
        }
        int best = 0; double bt = 1e18;
        for (int i = 0; i < 16; i++) if (s.lastUse[i] < bt) { bt = s.lastUse[i]; best = i; }
        return best;
    }

    inline bool FastStream(double dt) { return dt > 0.0 && dt <= kFastDt; }

    // ---------------- 0 经典：邻接移动 + 分区平衡 ----------------
    int PickClassic(P16& s, double dt, double turnDeg, double t)
    {
        const bool cold = (s.last < 0) || (dt > 2.5);
        const bool fast = FastStream(dt);
        static const long kDist[4] = { 0, 0, 60, 260 };

        int hUse[2] = { 0, 0 };
        for (int q = 0; q < 16; q++) hUse[P16Hand(q)] += s.use[q];

        int  best = -1; long bestSc = 0;
        for (int c = 0; c < 16; c++)
        {
            if (!s.Ok(c, t)) continue;
            const int d  = cold ? 1 : P16Dist(s.last, c);
            const int dd = d > 3 ? 3 : d;
            long sc = kDist[dd] + (d == 0 ? 260 : 0);      // 邻居移动优先，原地最差
            if (d > s.jumpCap) sc += 380;                  // 难度相关的跨距上限
            sc += 9 * s.use[c];                            // 全板均匀使用
            const int h = P16Hand(c);
            sc += 3 * (h ? hUse[1] : hUse[0]) - 3 * (h ? hUse[0] : hUse[1]);   // 分区平衡
            if (fast && s.last >= 0 && P16Hand(s.last) == h) sc += 240;        // 高速段强制换手
            if (best < 0 || sc < bestSc) { best = c; bestSc = sc; }
        }
        if (best < 0) best = Nearest(s, s.last < 0 ? 5 : s.last, t);
        return best;
    }

    // ---------------- 1 绕环：外圈回旋，方向跟随星球 ----------------
    int PickRing(P16& s, double dt, double turnDeg, int dir, double t)
    {
        const bool cold = (s.last < 0) || (dt > 2.5);
        const bool fast = FastStream(dt);
        if (cold) s.ringPos = (int)(s.Rnd() % 12u);

        // 转角越大，沿环走的格数越多；高速段退回最小步
        int steps = (int)std::lround(turnDeg / 40.0);
        if (steps < 1) steps = 1;
        if (steps > 4) steps = 4;
        if (fast && s.repeat >= 1) steps = 1;

        int p = s.ringPos + (dir >= 0 ? steps : -steps);
        p %= 12; if (p < 0) p += 12;
        s.ringPos = p;

        int cand = kRing[p];
        if (turnDeg >= 252.0)                                  // 大回转 = 角落重音
        {
            const int c = kCorner[p / 3];
            if (s.Ok(c, t)) cand = c;
        }
        else if (s.phraseLeft == 1 && (s.Rnd() % 100u) < 14u)  // 乐句边界收进内圈
        {
            const int c = kInner[s.Rnd() & 3u];
            if (s.Ok(c, t)) cand = c;
        }
        if (!s.Ok(cand, t)) cand = Nearest(s, cand, t);
        return cand;
    }

    // ---------------- 2 阶梯：沿轨道单调行走，到端折返 ----------------
    int PickStair(P16& s, double dt, double turnDeg, int dir, double t)
    {
        const bool cold = (s.last < 0) || (dt > 2.5);
        const bool fast = FastStream(dt);

        auto trackUsable = [&](int tr) -> bool
        {
            for (int q = 0; q < 4; q++) if (!s.Ok(kTracks[tr][q], t)) return false;
            return true;
        };
        // 换轨：冷启动 / 乐句边界 / 大回转（换到平行轨道做出"阶梯换行"）
        bool switchTrack = cold || s.phraseLeft == 1 || turnDeg >= 252.0;
        if (switchTrack)
        {
            int tr = -1;
            if (turnDeg >= 252.0 && !cold)
            {
                // 大回转：换到下一平行轨道（行→行 / 列→列，保持阶梯形状）
                for (int k = 1; k <= 3 && tr < 0; k++)
                {
                    const int cand = (s.stairTrack + k) % kTrackN;
                    if (cand / 4 == s.stairTrack / 4 && trackUsable(cand)) tr = cand;
                }
            }
            if (tr < 0)
            {
                // 任意可用轨道，靠近当前面板者优先
                long bestSc = 0;
                for (int k = 0; k < kTrackN; k++)
                {
                    if (!trackUsable(k)) continue;
                    const int  head = (dir >= 0) ? kTracks[k][0] : kTracks[k][3];
                    const long sc   = (long)(cold ? 0 : 10 * P16Dist(s.last, head))
                                    + 7 * (s.use[kTracks[k][0]] + s.use[kTracks[k][3]]);
                    if (tr < 0 || sc < bestSc) { tr = k; bestSc = sc; }
                }
            }
            if (tr < 0) tr = s.stairTrack;
            s.stairTrack = tr;
            s.stairPos   = (dir >= 0) ? 0 : 3;
            s.stairDir   = (dir >= 0) ? 1 : -1;
        }

        // 步长：转角大 → 跨步大；高速段固定 1
        const int step = fast ? 1 : (turnDeg >= 120.0 ? 2 : 1);

        int pos = s.stairPos + s.stairDir * step;
        while (pos > 3 || pos < 0)                     // 端点折返
        {
            s.stairDir = -s.stairDir;
            pos = (pos > 3) ? 6 - pos : -pos;
        }
        s.stairPos = pos;
        int cand = kTracks[s.stairTrack][pos];
        if (!s.Ok(cand, t)) cand = Nearest(s, cand, t);
        return cand;
    }

    // ---------------- 3 十字：沿十字臂行走的图形谱 ----------------
    int PickCross(P16& s, double dt, double turnDeg, int dir, double t)
    {
        const bool cold = (s.last < 0) || (dt > 2.5);
        const bool fast = FastStream(dt);

        auto armUsable = [&](int a) -> bool
        {
            for (int q = 0; q < 4; q++) if (!s.Ok(kArm[a][q], t)) return false;
            return true;
        };
        // 换臂：冷启动 / 乐句边界 / 大回转（横臂 ↔ 竖臂）
        if (cold || s.phraseLeft == 1 || turnDeg >= 252.0)
        {
            int a = -1;
            if (turnDeg >= 252.0 && !cold)
                a = (s.crossArm < 2) ? (2 + (s.crossArm & 1)) : (s.crossArm - 2);  // 垂直臂
            if (a < 0 || !armUsable(a))
            {
                a = -1; long bestSc = 0;
                for (int k = 0; k < 4; k++)
                {
                    if (!armUsable(k)) continue;
                    const int  head = (dir >= 0) ? kArm[k][0] : kArm[k][3];
                    const long sc   = (long)(cold ? 0 : 10 * P16Dist(s.last, head))
                                    + 7 * (s.use[kArm[k][0]] + s.use[kArm[k][3]]);
                    if (a < 0 || sc < bestSc) { a = k; bestSc = sc; }
                }
            }
            if (a < 0) a = s.crossArm;
            s.crossArm = a;
            s.crossPos = (dir >= 0) ? 0 : 3;
        }

        int pos = s.crossPos + (dir >= 0 ? 1 : -1);
        if (pos > 3 || pos < 0)                        // 臂端折返：换到相邻的平行臂
        {
            if (s.crossArm < 2) s.crossArm = (s.crossArm ^ 1);          // 横臂上 ↔ 下
            else                s.crossArm = 2 + ((s.crossArm - 2) ^ 1); // 竖臂左 ↔ 右
            if (!armUsable(s.crossArm)) s.crossArm = s.crossArm ^ 1;
            pos = (pos > 3) ? 3 : 0;
        }
        s.crossPos = pos;
        int cand = kArm[s.crossArm][s.crossPos];
        if (!s.Ok(cand, t)) cand = Nearest(s, cand, t);
        return cand;
    }

    // ---------------- 4 交互：双板交替 ----------------
    int PickTrill(P16& s, double dt, double t)
    {
        const bool cold = (s.last < 0) || (dt > 2.5);
        const int  want = s.trillNext ? s.trillB : s.trillA;
        if (cold || !s.Ok(s.trillA, t) || !s.Ok(s.trillB, t))
        {
            // 选一对相邻横板（同手区内 / 跨手各一对），优先远离上一面板
            int ba = -1, bb = -1; long bsc = 0;
            for (int r = 0; r < 4; r++)
            {
                for (int c = 0; c < 2; c++)
                {
                    const int a = r * 4 + c * 2;   // 列 0 或 2
                    const int b = a + 1;           // 右邻板
                    if (!s.Ok(a, t) || !s.Ok(b, t)) continue;
                    long sc = (long)(10 * (s.use[a] + s.use[b]));
                    if (a == s.last || b == s.last) sc += 40;   // 挪开，别停在原地
                    if (ba < 0 || sc < bsc) { ba = a; bb = b; bsc = sc; }
                }
            }
            if (ba < 0) { ba = 8; bb = 9; }
            s.trillA = ba; s.trillB = bb;
            s.trillNext = (s.last == ba) ? 1 : 0;
        }
        if (s.Ok(want, t)) { s.trillNext ^= 1; return want; }
        const int other = s.trillNext ? s.trillA : s.trillB;
        if (s.Ok(other, t)) return other;
        return Nearest(s, want, t);
    }

    // ---------------- 5 冰火拆手序：星球交替 → 左右分区严格交替 ----------------
    //   ADOFAI 的两颗星球逐砖交替（火 / 冰），所以 16K 手法序 = 偶数音落火手区
    //   （左两列）、奇数音落冰手区（右两列），双手永不连击同一区。
    //   区内选板：以本手区上一板为锚点，转角越大跨得越远（限 jumpCap）。
    int PickFireIce(P16& s, int idx, double dt, double turnDeg, double t)
    {
        const bool cold = (s.lastHand[0] < 0 && s.lastHand[1] < 0) || (dt > 2.5);
        const int  hand = idx & 1;                     // 火手 / 冰手 严格交替
        const int  anchor = s.lastHand[hand];

        int cand = -1;
        if (anchor >= 0 && !cold)
        {
            // 锚点出发，按转角定跨距，在区内按 Chebyshev 环搜最近可用板
            int step = (int)std::lround(turnDeg / 60.0);
            if (step < 1) step = 1;
            if (step > s.jumpCap) step = s.jumpCap;
            const int ar = P16Row(anchor), ac = (P16Col(anchor) & 1);   // 区内半列 0/1
            long bestSc = 0;
            for (int r = 0; r < 4; r++)
            {
                for (int h = 0; h < 2; h++)
                {
                    const int i = P16ZonePanel(hand, r, h);
                    if (!s.Ok(i, t)) continue;
                    int d = P16Abs(r - ar) > P16Abs(h - ac) ? P16Abs(r - ar) : P16Abs(h - ac);
                    if (d > s.jumpCap + 1) continue;
                    long sc = (long)P16Abs(d - step) * 120     // 贴合目标跨距
                            + 9 * s.use[i];
                    if (d == 0) sc += 200;                     // 区内也尽量避免原地
                    if (cand < 0 || sc < bestSc) { cand = i; bestSc = sc; }
                }
            }
        }
        if (cand < 0)
        {
            // 冷启动 / 区内全占用：本手区内最近未用板（无锚点则取区内侧板）
            const int seed = (anchor >= 0) ? anchor : P16ZonePanel(hand, 1 + (idx & 1) % 2, 0);
            cand = Nearest(s, seed, t);
            if (P16Hand(cand) != hand)
            {
                long bestSc = 0;
                for (int r = 0; r < 4; r++)
                    for (int h = 0; h < 2; h++)
                    {
                        const int i = P16ZonePanel(hand, r, h);
                        if (!s.Ok(i, t)) continue;
                        const long sc = 9 * s.use[i];
                        if (cand < 0 || sc < bestSc) { cand = i; bestSc = sc; }
                    }
            }
        }
        return cand;
    }

    // ---------------- 押（同刻多键）----------------
    //   经典系手法：紧凑可达聚簇（行 / 列 / 2x2 块），双手均衡。
    int PickPadChord(P16& s, int k, int variant, double t, int* out)
    {
        if (k < 2) k = 2;
        if (k > 8) k = 8;
        int cand[48][8];
        int n = 0;
        for (int r = 0; r < 4 && k <= 4; r++)
            for (int c = 0; c + k <= 4; c++)
            {
                if (n >= 48) break;
                for (int i = 0; i < k; i++) cand[n][i] = r * 4 + c + i;
                n++;
            }
        for (int c = 0; c < 4 && k <= 4; c++)
            for (int r = 0; r + k <= 4; r++)
            {
                if (n >= 48) break;
                for (int i = 0; i < k; i++) cand[n][i] = (r + i) * 4 + c;
                n++;
            }
        if (k == 4)
            for (int br = 0; br < 3; br++)
                for (int bc = 0; bc < 3; bc++)
                {
                    if (n >= 48) break;
                    cand[n][0] = br * 4 + bc;       cand[n][1] = br * 4 + bc + 1;
                    cand[n][2] = (br + 1) * 4 + bc; cand[n][3] = (br + 1) * 4 + bc + 1;
                    n++;
                }
        if (k > 4)
            for (int r = 0; r < 4; r++)
            {
                if (n >= 48) break;
                int m = 0;
                for (int c = 0; c < 4; c++) cand[n][m++] = r * 4 + c;
                const int r2 = (r + 1) & 3;
                for (int c = 0; c < 4 && m < k; c++) cand[n][m++] = r2 * 4 + c;
                if (m == k) n++;
            }
        if (n == 0)
        {
            int m = 0;
            for (int i = 0; i < 16 && m < k; i++) if (s.Ok(i, t)) out[m++] = i;
            for (; m < k; m++) out[m] = m & 15;
            return k;
        }
        int  bestI = -1; long bestSc = 0;
        for (int ci = 0; ci < n; ci++)
        {
            const int i = (variant + ci) % n;
            long sc = 0; int h[2] = { 0, 0 };
            for (int j = 0; j < k; j++)
            {
                const int l = cand[i][j];
                if (l == s.last) sc += 300;
                if (!s.Ok(l, t)) sc += 400;
                sc += 8 * s.use[l];
                h[P16Hand(l)]++;
            }
            sc += 120 * (h[0] > h[1] ? h[0] - h[1] : h[1] - h[0]);   // 双手均衡
            if (bestI < 0 || sc < bestSc) { bestI = i; bestSc = sc; }
        }
        for (int j = 0; j < k; j++) out[j] = cand[bestI][j];
        return k;
    }

    //   冰火拆手序押：按"拆手序"逐点分配 —— 第 1 点给当前音的手区，
    //   第 2 点给另一手区，第 3 点回到当前手区……每点落在该手区内、
    //   靠近该手区上一板、避开本刻已占面板与长按。
    int SplitChordFireIce(P16& s, int k, int idx, double t, int* out)
    {
        if (k < 2) k = 2;
        if (k > 8) k = 8;
        bool taken[16] = {};
        for (int j = 0; j < k; j++)
        {
            const int hand = (idx + j) & 1;
            const int anchor = s.lastHand[hand];
            int cand = -1; long bestSc = 0;
            for (int r = 0; r < 4; r++)
            {
                for (int h = 0; h < 2; h++)
                {
                    const int i = P16ZonePanel(hand, r, h);
                    if (!s.Ok(i, t) || taken[i]) continue;
                    long sc = 9 * s.use[i];
                    if (anchor >= 0) sc += 40L * P16Dist(anchor, i);   // 靠近本手上一板
                    else             sc += 6L * P16Abs(r - 1);         // 无锚点偏区内侧
                    if (cand < 0 || sc < bestSc) { cand = i; bestSc = sc; }
                }
            }
            if (cand < 0)
            {
                // 手区被长按占满：换邻近手区兜底（保证物理可打）
                for (int i = 0; i < 16 && cand < 0; i++)
                    if (!taken[i] && s.Ok(i, t)) cand = i;
                if (cand < 0) cand = j & 15;
            }
            taken[cand] = true;
            out[j] = cand;
        }
        return k;
    }
} // namespace

void GeneratePad16K(const PadEv* ev, int n, int style, int level,
                    int* outLanes, std::vector<std::vector<int>>* chordOut)
{
    if (chordOut) chordOut->assign((size_t)(n > 0 ? n : 0), std::vector<int>());
    if (n <= 0 || !outLanes) return;
    for (int i = 0; i < n; i++) outLanes[i] = 0;

    P16 s;
    s.floor   = 0.085;
    s.jumpCap = (level >= 26) ? 4 : (level >= 18) ? 3 : 2;
    for (int i = 0; i < 16; i++) { s.lastUse[i] = -1e9; s.holdUntil[i] = -1e9; s.use[i] = 0; }
    s.rng = 0x9E3779B9u ^ (uint32_t)(style * 2654435761u) ^ (uint32_t)(level * 97);
    s.NewPhrase(10);

    int nChord = 0;
    for (int i = 0; i < n; i++)
    {
        const PadEv& e = ev[i];
        if (e.taps >= 2)
        {
            int k = (int)e.taps; if (k > 8) k = 8;
            int lanes[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
            const int got = (style == 5)
                ? SplitChordFireIce(s, k, i, e.t, lanes)
                : PickPadChord(s, k, s.phraseLeft + i, e.t, lanes);
            outLanes[i] = lanes[0];
            if (chordOut)
            {
                std::vector<int>& v = (*chordOut)[(size_t)i];
                for (int j = 1; j < got; j++) v.push_back(lanes[j]);
            }
            for (int j = 0; j < got; j++) s.Commit(lanes[j], e.t, (j == 0) ? e.hold : 0.f);
            s.repeat = 0;
            nChord++;
            s.Step();
            continue;
        }

        int l;
        if (s.last < 0)   // 开谱第一音：五条经典起手位（内圈侧板）
        {
            static const int kStart[6] = { 5, 4, 0, 5, 8, 5 };   // 经典/绕环/阶梯/十字/交互/冰火
            l = kStart[style >= 0 && style <= 5 ? style : 0];
            if (!s.Ok(l, e.t)) l = Nearest(s, l, e.t);
        }
        else
        {
            switch (style)
            {
            case 1:  l = PickRing(s, e.dt, e.turn, e.dir, e.t);     break;
            case 2:  l = PickStair(s, e.dt, e.turn, e.dir, e.t);    break;
            case 3:  l = PickCross(s, e.dt, e.turn, e.dir, e.t);    break;
            case 4:  l = PickTrill(s, e.dt, e.t);                   break;
            case 5:  l = PickFireIce(s, i, e.dt, e.turn, e.t);      break;
            default: l = PickClassic(s, e.dt, e.turn, e.t);         break;
            }
        }
        if (!s.Ok(l, e.t)) l = Nearest(s, l, e.t);
        outLanes[i] = l;
        s.repeat = (l == s.last) ? (s.repeat + 1) : 0;
        s.Commit(l, e.t, e.hold);
        s.Step();
    }
    Log::Printf("[16K] pad gen: style=%d level=%d notes=%d chords=%d", style, level, n, nChord);
}

const char* ModeDesc16K()
{
    return "16K（PAD / 4x4 面板）：按经典 16K 写谱手法独立生成 —— "
           "经典（邻接移动）/ 绕环（外圈回旋）/ 阶梯（行·列·对角折返）/ "
           "十字（图形谱）/ 交互（双板 trill）/ 冰火拆手序（火左冰右严格交替 + 押按拆手分配）。";
}

} // namespace Chart4K
