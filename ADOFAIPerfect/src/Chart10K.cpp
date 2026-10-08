// ============================================================
// Chart10K.cpp — 10K（ASDFG+HJKL;） 下坠谱面：分键引擎 + 设置页
//   经典引擎 4/4，独立算法（状态机 / 指法语汇 / 连打循环都在本文件）
// ============================================================
#include "ChartCore.h"
#include <cmath>
#include <algorithm>

namespace Chart4K
{
    // ---------------- 经典引擎 4/4：10K（A S D F G | H J K L ;） ----------------
    //   双手各 5 键；写谱以"波浪/蝴蝶"运动为主：
    //   · 连打用 20 音蝴蝶循环（中心→外侧展开→回中心，每键恰好 2 次、双手
    //     逐音交替），长流每循环旋转起点 / 反向一次，均匀且不呆板
    //   · 楼梯沿 10 键方向性扫动，跨过中央时自然换手
    //   · 大回转落最外侧 A/; 做重音；长按占用自动避让
    static const int kCycle10K[20] = {
        4, 5, 3, 6, 2, 7, 1, 8, 0, 9,
        5, 4, 6, 3, 7, 2, 8, 1, 9, 0
    };
    int NextLane10K(State10K& st, int dir, double turn, double t, double hold)
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
                // 就地呼吸：在楼梯落点与内侧邻键间弹一下
                int gl = st.grooveLane;
                if (gl < 0 || gl > 9) gl = 4;
                int in = (gl <= 4) ? (gl + 1) : (gl - 1);
                lane = (st.prevLane == gl) ? in : gl;
            }
            else if (st.walkSteps > 0)
            {
                int step = (st.walkLane >= 0 && st.walkLane <= 9 && st.use[st.walkLane] > 3) ? 2 : 1;
                lane = st.walkLane + st.walkDir * step;
                if (lane < 0 || lane > 9)
                {
                    st.walkDir = -st.walkDir;
                    lane = st.walkLane + st.walkDir * step;
                    if (lane < 0) lane = 0;
                    if (lane > 9) lane = 9;
                }
                st.walkSteps--;
                st.walkLane = lane;
                if (st.walkSteps <= 0)
                {
                    st.grooveLane = lane;                       // 楼梯落点就地呼吸
                    st.groove = 2;
                }
            }
            else
            {
                if (st.cycLeft <= 0)                    // 进入新循环：从最少用的键起步
                {
                    int best = 0;
                    for (int i = 1; i < 20; i++)
                        if (st.use[kCycle10K[i]] < st.use[kCycle10K[best]])
                            best = i;
                    st.cycPos  = best;
                    st.cycDir  = st.cycDir ? -st.cycDir : 1;
                    st.cycLeft = 20;
                    // 全键扫（约每 3 个循环一次）：从用量最少的键起步扫过 8 键，
                    // 外侧 A/; 不会饱和，扫动过程天然跨手
                    if (((int)(t * 1000.0) % 2) == 0)
                    {
                        int best2 = 0;
                        for (int i = 1; i < 10; i++)
                            if (st.use[i] < st.use[best2]) best2 = i;
                        if (st.use[best2] + 2 <= st.use[4] && st.use[best2] + 2 <= st.use[5])
                        {
                            // 起点键本身必须发出声
                            st.walkDir   = (best2 < 5) ? 1 : -1;
                            st.walkLane  = best2 - st.walkDir;
                            st.walkSteps = 9;
                        }
                    }
                }
                lane = kCycle10K[st.cycPos];
                st.cycPos = (st.cycPos + st.cycDir + 20) % 20;
                st.cycLeft--;
                if (st.cycLeft <= 0)                    // 循环结束接一小段楼梯
                {
                    st.walkLane  = lane;
                    st.walkDir   = (lane < 5) ? 1 : -1;
                    st.walkSteps = 3 + (int)(turn * 2.0) % 3;
                }
            }
        }
        else
        {
            st.groove = 3;
            st.grooveLane = 4;
            st.walkSteps = 0;
            st.cycLeft = 0;
            int hand = (dir < 0) ? 0 : 1;
            if (!cold && hand == st.lastHand && dt <= kSameHandDt)
                hand = 1 - hand;
            // 回转幅度 → 0..4 深度（线性：转得越多手越往外移）
            int lanes[5]; int n = HandLanes(hand, 10, lanes);
            int depth = (int)(turn / 4.712389 * (double)(n - 1) + 0.5);
            if (depth < 0) depth = 0;
            if (depth >= n) depth = n - 1;
            lane = lanes[depth];
        }
        const int hn = (lane < 5) ? 0 : 1;
        bool busy = (lane == st.lastOfHand[hn] && dt < kRepeatGuard) || (st.holdUntil[lane] > t);
        if (busy)
        {
            int lanes[5]; int n = HandLanes(hn, 10, lanes);
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
        st.lastOfHand[(lane < 5) ? 0 : 1] = lane;
        st.prevLane = lane;
        st.prevTime = t;
        st.lastHand  = (lane < 5) ? 0 : 1;
        return lane;
    }
    const char* ModeDesc10K()
    {
        return "ASDFG + HJKL; \u952e\u4f4d \u00b7 \u7ecf\u5178\u5f15\u64ce\uff1a20 \u97f3\u8774\u8776\u5faa\u73af + \u697c\u68af\u4ea4\u66ff\uff0c\u5341\u952e\u5747\u5300";
    }

    void DrawSettings10KPage() { DrawModePage(3); }
}
