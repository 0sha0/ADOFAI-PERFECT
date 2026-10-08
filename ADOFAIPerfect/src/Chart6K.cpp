// ============================================================
// Chart6K.cpp — 6K（SDFJKL） 下坠谱面：分键引擎 + 设置页
//   经典引擎 2/4，独立算法（状态机 / 指法语汇 / 连打循环都在本文件）
// ============================================================
#include "ChartCore.h"
#include <cmath>
#include <algorithm>

namespace Chart4K
{
    // ---------------- 经典引擎 2/4：6K（SDFJKL：0..5） ----------------
    //   左手 S/D/F，右手 J/K/L；内指=食指 F/J，中指 D/K，外指=无名指 S/L。
    //   连打采用 6K 专属"双螺旋"24 音循环（每 24 音每键恰好 4 次，双手逐音
    //   交替、每只手内指法 0→1→2 步进，天然无同手同键）：
    //     F L D K S J | F L D K S J | D K F L S J | D K F L S J
    //   长连打每 24 音相位翻转一次；外侧 S/L 只出现在循环固定位置与慢速大回转。
    static const int kCycle6K[12][2] = {
        { 0, 2 }, { 1, 1 }, { 2, 0 }, { 0, 2 }, { 1, 1 }, { 2, 0 },
        { 1, 1 }, { 0, 2 }, { 2, 0 }, { 1, 1 }, { 0, 2 }, { 2, 0 }
    };
    static inline int Lane6K(int hand, int finger)
    {
        return (hand == 0) ? (2 - finger) : (3 + finger);
    }
    int NextLane6K(State6K& st, int dir, double turn, double t, double hold)
    {
        const double dt = t - st.prevTime;
        const bool fast = (dt > 0.0) && (dt <= kFastDt);
        const bool cold = (st.prevLane < 0) || (dt > 2.5);
        int hand = 0, finger = 0;
        if (cold || !fast)
        {
            st.streamPos = -1;
            if (cold || dt > kSameHandDt)
                hand = (dir < 0) ? 0 : 1;
            else
                hand = 1 - st.lastHand;
            finger = (turn >= kAccent4K) ? 2 : ((turn >= kMidTurn6K) ? 1 : 0);
        }
        else
        {
            if (st.streamPos < 0)
            {
                st.streamPos   = 0;
                st.streamCycle = 0;
                st.startHand   = 1 - st.lastHand;
            }
            if (st.streamPos >= 24)
            {
                st.streamPos = 0;
                st.streamCycle++;
            }
            int pos = st.streamPos + ((st.streamCycle & 1) ? 12 : 0);
            if (pos >= 24)
                pos -= 24;
            const int* pr = kCycle6K[pos >> 1];
            if (pos & 1) { hand = 1 - st.startHand; finger = pr[1]; }
            else         { hand = st.startHand;     finger = pr[0]; }
            st.streamPos++;
        }
        int lane = Lane6K(hand, finger);
        bool laneBusy = (lane == st.lastOfHand[hand] && dt < kRepeatGuard)
                        || (st.holdUntil[lane] > t);
        if (laneBusy)
        {
            for (int k = 1; k <= 2; k++)
            {
                int f = (finger + k) % 3;
                int alt = Lane6K(hand, f);
                if (alt != st.lastOfHand[hand] && st.holdUntil[alt] <= t)
                { finger = f; lane = alt; break; }
            }
        }
        if (hold > 0.0)
            st.holdUntil[lane] = t + hold;
        st.lastOfHand[hand] = lane;
        st.prevLane = lane;
        st.prevTime = t;
        st.lastHand = hand;
        return lane;
    }
    const char* ModeDesc6K()
    {
        return "SDFJKL \u952e\u4f4d \u00b7 \u7ecf\u5178\u5f15\u64ce\uff1a\u516d\u952e\u53cc\u87ba\u65cb\u8fde\u6253\uff0cS/L \u53ea\u505a\u6162\u901f\u91cd\u97f3";
    }

    void DrawSettings6KPage() { DrawModePage(2); }
}
