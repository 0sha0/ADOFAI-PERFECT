// ============================================================
// Chart4K.cpp — 4K（DFJK） 下坠谱面：分键引擎 + 设置页
//   经典引擎 1/4，独立算法（状态机 / 指法语汇 / 连打循环都在本文件）
// ============================================================
#include "ChartCore.h"
#include <cmath>
#include <algorithm>

namespace Chart4K
{
    // ---------------- 经典引擎 1/4：4K（DFJK：0=D 1=F 2=J 3=K） ----------------
    //   左手 D/F，右手 J/K；食指=内(F/J)，中指=外(D/K)。
    //   · 连打（<=0.45s）：强制换手 + 保持当前手指 → 短爆发是干净的 F J 交互，
    //     长流被同键保护自然滚成 F J D K，永不出现单手轮指
    //   · 稀疏音：逆时针=左手 / 顺时针=右手；大回转（>=252°）=外键重音 D/K
    //   · 同键 0.6s 内不重复；被长按占用的键不落
    static inline int Lane4K(int hand, int finger)
    {
        return (hand == 0) ? (1 - finger) : (2 + finger);
    }
    int NextLane4K(State4K& st, int dir, double turn, double t, double hold)
    {
        const double dt = t - st.prevTime;
        const bool fast = (dt > 0.0) && (dt <= kFastDt);
        const bool cold = (st.prevLane < 0) || (dt > 2.5);
        int hand = 0, finger = 0;
        if (cold || !fast)
        {
            hand   = (dir < 0) ? 0 : 1;
            finger = (turn >= kAccent4K) ? 1 : 0;
            if (!cold && hand == st.lastHand && dt <= kSameHandDt)
                hand = 1 - hand;
            st.streamFinger = finger;
        }
        else
        {
            hand   = 1 - st.lastHand;      // 连打换手
            finger = st.streamFinger;      // 保持同指：F J F J
        }
        int lane = Lane4K(hand, finger);
        if (lane == st.lastOfHand[hand] && dt < kRepeatGuard)
        {
            int alt = Lane4K(hand, 1 - finger);
            if (alt != lane) { finger = 1 - finger; lane = alt; }
        }
        if (st.holdUntil[lane] > t)
        {
            int alt = Lane4K(hand, 1 - finger);
            if (alt != lane && st.holdUntil[alt] <= t && alt != st.lastOfHand[hand])
                lane = alt;
        }
        if (hold > 0.0)
            st.holdUntil[lane] = t + hold;
        st.lastOfHand[hand] = lane;
        st.prevLane = lane;
        st.prevTime = t;
        st.lastHand = hand;
        return lane;
    }
    const char* ModeDesc4K()
    {
        return "DFJK \u952e\u4f4d \u00b7 \u7ecf\u5178\u5f15\u64ce\uff1a\u8fde\u6253\u53cc\u5411\u4ea4\u4e92\u3001\u957f\u6309\u5360\u7528\u907f\u8ba9\u3001\u4e50\u53e5\u5316\u5206\u952e";
    }

    void DrawSettingsPage() { DrawModePage(0); }
}
