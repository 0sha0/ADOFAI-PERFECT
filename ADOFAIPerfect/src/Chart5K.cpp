// ============================================================
// Chart5K.cpp — 5K（SDFJK） 下坠谱面：分键引擎 + 设置页
//   经典引擎 3/4，独立算法（状态机 / 指法语汇 / 连打循环都在本文件）
// ============================================================
#include "ChartCore.h"
#include <cmath>
#include <algorithm>

namespace Chart4K
{
    // ---------------- 经典引擎 3/4：5K（S D F | J K） ----------------
    //   左：食指 F(2) 内 / 中指 D(1) / 无名指 S(0) 外；右：食指 J(3) 内 / 中指 K(4)。
    //   5K 写谱惯例：中心两键 F/J 是"呼吸键"，连打优先 F J 交互；楼梯沿 5 键
    //   方向性扫过（1~2 格步进、端点折返），让 5 个键的使用自然均匀；外侧键
    //   只做慢速重音，不用来打快连打（无名指不耐久）。
    static inline int Lane5K(int hand, int depth)
    {
        return (hand == 0) ? (2 - depth) : (3 + depth);
    }
    int NextLane5K(State5K& st, int dir, double turn, double t, double hold)
    {
        const double dt = t - st.prevTime;
        const bool fast = (dt > 0.0) && (dt <= kFastDt);
        const bool cold = (st.prevLane < 0) || (dt > 2.5);
        int lane = -1;
        if (!cold && fast)
        {
            if (st.groove > 0)
            {
                st.groove--;
                // 就地呼吸：在上段楼梯的落点与其内侧邻键间弹一下
                // （落点在外侧时外键也能参与，避免全程只压 F/J）
                int gl = st.grooveLane;
                if (gl < 0 || gl > 4) gl = 2;
                int in = (gl <= 2) ? (gl + 1) : (gl - 1);
                lane = (st.prevLane == gl) ? in : gl;
            }
            else
            {
                if (st.walkSteps <= 0)
                {
                    st.walkLane = (st.prevLane >= 0) ? st.prevLane : 2;
                    st.walkDir  = (dir < 0) ? -1 : 1;
                    if (st.walkLane >= 3) st.walkDir = -st.walkDir;
                    st.walkSteps = 3 + (int)(turn * 2.0) % 3;   // 3~5 格一条楼梯
                    // 全键扫（约每 3 条楼梯一次）：与用量最少的键相差明显时，
                    // 从该键起步向中心方向扫过整排，保持五键均匀、自然跨手
                    if (((int)(t * 1000.0) % 2) == 0)
                    {
                        int best = 0;
                        for (int i = 1; i < 5; i++)
                            if (st.use[i] < st.use[best]) best = i;
                        if (st.use[best] + 1 <= st.use[2] && st.use[best] + 1 <= st.use[3])
                        {
                            // 起点键本身必须发出声：回退一格再步进一格
                            st.walkDir  = (best <= 2) ? 1 : -1;
                            st.walkLane = best - st.walkDir;
                            st.walkSteps = 6;
                        }
                    }
                }
                int step = (st.walkLane >= 0 && st.walkLane <= 4 && st.use[st.walkLane] > 2) ? 2 : 1;
                lane = st.walkLane + st.walkDir * step;
                if (lane < 0 || lane > 4)
                {
                    st.walkDir = -st.walkDir;
                    lane = st.walkLane + st.walkDir * step;
                    if (lane < 0) lane = 0;
                    if (lane > 4) lane = 4;
                }
                st.walkSteps--;
                st.walkLane = lane;
                if (st.walkSteps <= 0)
                {
                    st.grooveLane = lane;                       // 楼梯落点就地呼吸
                    st.groove = (st.use[2] + st.use[3] > st.use[0] + st.use[1] + st.use[4] + 2) ? 1 : 3;
                }
            }
        }
        else
        {
            st.groove = 2;
            st.grooveLane = 2;
            st.walkSteps = 0;
            int hand = (dir < 0) ? 0 : 1;
            if (!cold && hand == st.lastHand && dt <= kSameHandDt)
                hand = 1 - hand;
            // 回转幅度 → 手内深度：小转落内指，90°落中指，
            // 180°+ 落外侧（左 S / 右 K）——转得越多手越往外移
            int depth = (turn >= kMidTurn6K) ? 2 : ((turn >= 1.2) ? 1 : 0);
            if (hand == 1 && depth > 1) depth = 1;   // 右手只有两键
            lane = Lane5K(hand, depth);
        }
        const int hn = (lane < 3) ? 0 : 1;
        bool busy = (lane == st.lastOfHand[hn] && dt < kRepeatGuard) || (st.holdUntil[lane] > t);
        if (busy)
        {
            int lanes[3]; int n = HandLanes(hn, 5, lanes);
            for (int k = 0; k < n; k++)
            {
                int alt = lanes[k];
                if (alt == lane) continue;
                if (st.holdUntil[alt] <= t && !(alt == st.lastOfHand[hn] && dt < kRepeatGuard))
                { lane = alt; break; }
            }
        }
        if (hold > 0.0)
            st.holdUntil[lane] = t + hold;
        st.use[lane]++;
        st.lastOfHand[(lane < 3) ? 0 : 1] = lane;
        st.prevLane = lane;
        st.prevTime = t;
        st.lastHand  = (lane < 3) ? 0 : 1;
        return lane;
    }
    const char* ModeDesc5K()
    {
        return "SDFJK \u952e\u4f4d \u00b7 \u7ecf\u5178\u5f15\u64ce\uff1aF/J \u4e2d\u5fc3\u547c\u5438 + 3~5 \u683c\u697c\u68af\uff0c\u4e94\u952e\u7528\u91cf\u5747\u5300";
    }

    void DrawSettings5KPage() { DrawModePage(1); }
}
