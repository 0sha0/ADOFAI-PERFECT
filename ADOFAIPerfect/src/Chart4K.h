#pragma once
// ============================================================
// Chart4K.h — 4K 辅助页
//
// 把冰与火的无轨谱面实时转换为 Malody 式 4K（DFJK）下落谱：
//   · 谱面：scrLevelMaker._instance.listFloors，scrFloor.entryTime =
//     每块绝对命中时刻（BPM/变速已由游戏折算），angleLength+isCCW = 转向角
//   · 同步：scrConductor._instance.dspTimeSong（游戏歌曲时钟，纯字段读取）
//   · 分键（手感，参考 osu!mania 官方 mapping guide）：
//       - 逆时针 → 左手 (D/F)，顺时针 → 右手 (J/K)
//       - 同方向连打在两手两键内交替（roll），换向时按角度大小落键
//       - 直行块（|角|≈0）= 同键连击（jack，对应游戏中重复音）
//       - 重复节奏型（motif）复用同一键位
//   · 皮肤：加载 Malody V 皮肤目录（rurudokey 小人 / notex 音符 /
//     judgercolor 判定线 / combo 数字），DFJK 按下小人同步敲击
//   · 连击数 + 精准度（游戏真实 Acc）+ Malody 式难度 Lv 自动计算
// ============================================================
#include <atomic>

namespace Chart4K
{
    void Tick();              // 桥接线程：歌曲时钟 / 换谱检测 / 谱面提取 / 难度计算
    void DrawPlayfield();     // 渲染线程：绘制下坠谱面（独立透明窗口）
    void DrawSettingsPage();  // 渲染线程：主窗口里的 4K辅助 设置页
    void DrawSettings6KPage();// 渲染线程：主窗口里的 6K模式 设置页
    void MainThreadResolve(); // 仅游戏主线程调用：安全创建所需类的 vtable

    bool HasChart();                       // 是否已成功解析出谱面
    int  NoteCount();                      // 当前谱面音符数
    void GetDiag(char* buf, int n);         // 诊断文本（供设置页显示）
}
