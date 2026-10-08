#pragma once
// ============================================================
// GameDir.h — 游戏目录（设置页可选；未设置时 = 进程 exe 所在目录）
//   · 供皮肤/资源定位使用，不依赖任何 MOD 加载器
//   · 持久化在 adofai_perfect.cfg 的 game_dir= 键（I18N::Prefs）
// ============================================================
namespace GameDir
{
    const char* Get();                    // 生效中的游戏目录（无末尾反斜杠）
    void        Set(const char* dir);     // 校验 -> 持久化（空串/无效 = 恢复自动）
    bool        Valid(const char* dir);   // 目录内存在游戏数据 / exe
    void        OpenFolder();
}