#pragma once
// ============================================================
// Log.h — 轻量日志（写入 %LOCALAPPDATA%\ADOFvec\adofai_cheat.log + OutputDebugString）
// ============================================================
#include <cstdint>

namespace Log
{
    void Init();
    void Printf(const char* fmt, ...);
}
