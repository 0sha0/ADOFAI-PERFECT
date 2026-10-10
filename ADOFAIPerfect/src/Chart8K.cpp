// ============================================================
// Chart8K.cpp — 8K（A S D F | J K L ;） 下坠谱面：分键引擎 + 设置页
//   标准 8 键（osu!mania / Etterna 通行布局）：左手 A S D F，右手 J K L ;。
//   本引擎与 4K/5K/6K/10K 的状态机 / 指法语汇完全独立：
//     · 连打用 16 音"内外波浪"循环（中心→外侧展开→回中心，每键恰好 2 次、
//       双手逐音交替），每循环旋转起点 / 反向一次，均匀且不呆板；
//     · 楼梯沿 8 键单方向扫动，跨过中央（lane 3→4）自然换手；
//     · 大回转落最外侧 A / ;（lane 0 / 7）做重音；长按占用自动避让。
// ============================================================
#include "ChartCore.h"
#include <cmath>
#include <algorithm>

namespace Chart4K
{
    // 16 音波浪循环：3 4 | 2 5 | 1 6 | 0 7 然后反向 4 3 | 5 2 | 6 1 | 7 0
    //   （左手 3,2,1,0 = F D S A；右手 4,5,6,7 = J K L ;）每键恰好两次、逐音换手。
    static const int kCycle8K[16] = {
        3, 4, 2, 5, 1, 6, 0, 7,
        4, 3, 5, 2, 6, 1, 7, 0
    };

    int NextLane8K(State8K& st, int dir, double turn, double t, double hold)
    {
        const double dt   = t - st.prevTime;
        const bool   fast = (dt > 0.0) && (dt <= kFastDt);
        const bool   cold = (st.prevLane < 0) || (dt > 2.5);
        int lane = -1;

        if (!cold && fast)
        {
            if (st.groove > 0)
            {
                st.groove--;
                // 就地呼吸：在楼梯落点与内侧邻键间弹一下
                int gl = st.grooveLane;
                if (gl < 0 || gl > 7) gl = 3;
                int in = (gl <= 3) ? (gl + 1) : (gl - 1);
                lane = (st.prevLane == gl) ? in : gl;
            }
            else if (st.walkSteps > 0)
            {
                int step = (st.walkLane >= 0 && st.walkLane <= 7 && st.use[st.walkLane] > 3) ? 2 : 1;
                lane = st.walkLane + st.walkDir * step;
                if (lane < 0 || lane > 7)
                {
                    st.walkDir = -st.walkDir;
                    lane = st.walkLane + st.walkDir * step;
                    if (lane < 0) lane = 0;
                    if (lane > 7) lane = 7;
                }
                st.walkSteps--;
                st.walkLane = lane;
                if (st.walkSteps <= 0)
                {
                    st.grooveLane = lane;                 // 楼梯落点就地呼吸
                    st.groove = 2;
                }
            }
            else
            {
                if (st.cycLeft <= 0)                      // 进入新循环：从最少用的键起步
                {
                    int best = 0;
                    for (int i = 1; i < 16; i++)
                        if (st.use[kCycle8K[i]] < st.use[kCycle8K[best]])
                            best = i;
                    st.cycPos  = best;
                    st.cycDir  = st.cycDir ? -st.cycDir : 1;
                    st.cycLeft = 16;
                    // 全键扫（约每两个循环一次）：从用量最少的键起步扫过 8 键
                    if (((int)(t * 1000.0) % 2) == 0)
                    {
                        int best2 = 0;
                        for (int i = 1; i < 8; i++)
                            if (st.use[i] < st.use[best2]) best2 = i;
                        if (st.use[best2] + 2 <= st.use[3] && st.use[best2] + 2 <= st.use[4])
                        {
                            st.walkDir   = (best2 < 4) ? 1 : -1;
                            st.walkLane  = best2 - st.walkDir;
                            st.walkSteps = 7;
                        }
                    }
                }
                lane = kCycle8K[st.cycPos];
                st.cycPos = (st.cycPos + st.cycDir + 16) % 16;
                st.cycLeft--;
                if (st.cycLeft <= 0)                      // 循环结束接一小段楼梯
                {
                    st.walkLane  = lane;
                    st.walkDir   = (lane < 4) ? 1 : -1;
                    st.walkSteps = 2 + (int)(turn * 2.0) % 3;
                }
            }
        }
        else
        {
            st.groove = 3;
            st.grooveLane = 3;
            st.walkSteps = 0;
            st.cycLeft = 0;
            int hand = (dir < 0) ? 0 : 1;
            if (!cold && hand == st.lastHand && dt <= kSameHandDt)
                hand = 1 - hand;
            // 回转幅度 → 0..3 深度（线性：转得越多手越往外移）
            int lanes[4]; int n = HandLanes(hand, 8, lanes);
            int depth = (int)(turn / 4.712389 * (double)(n - 1) + 0.5);
            if (depth < 0) depth = 0;
            if (depth >= n) depth = n - 1;
            lane = lanes[depth];
        }

        const int hn = (lane < 4) ? 0 : 1;
        bool busy = (lane == st.lastOfHand[hn] && dt < kRepeatGuard) || (st.holdUntil[lane] > t);
        if (busy)
        {
            int lanes[4]; int n = HandLanes(hn, 8, lanes);
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
        st.lastOfHand[(lane < 4) ? 0 : 1] = lane;
        st.prevLane = lane;
        st.prevTime = t;
        st.lastHand = (lane < 4) ? 0 : 1;
        return lane;
    }

    const char* ModeDesc8K()
    {
        return "ASDF + JKL; \u952e\u4f4d \u00b7 \u7ecf\u5178\u5f15\u64ce\uff1a16 \u97f3"
               "\u5185\u5916\u6ce2\u6d6a\u5faa\u73af + \u697c\u68af\u4ea4\u66ff\uff0c"
               "\u516b\u952e\u5747\u5300\uff08\u5de6\u624b ASDF\u3001\u53f3\u624b JKL;\uff09\u3002";
    }

    void DrawSettings8KPage() { DrawModePage(kMode8K); }
}
