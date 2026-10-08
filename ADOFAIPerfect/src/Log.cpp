#include "Log.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace Log
{
    static HANDLE g_file = nullptr;

    void Init()
    {
        char path[MAX_PATH];
        // ADOF_LOG_DIR=<目录>：把日志写到指定目录（调试/受限环境用；缺省 LOCALAPPDATA\ADOFvec）
        if (GetEnvironmentVariableA("ADOF_LOG_DIR", path, MAX_PATH) > 0 && path[0])
        {
            CreateDirectoryA(path, nullptr);
        }
        else
        {
            if (!GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH))
                GetTempPathA(MAX_PATH, path);
            CreateDirectoryA(path, nullptr);
            strncat_s(path, "\\ADOFvec", _TRUNCATE);
        }
        CreateDirectoryA(path, nullptr);
        strncat_s(path, "\\adofai_perfect.log", _TRUNCATE);
        g_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        Printf("---- ADOFAI PERFECT log start ----");
    }

    void Printf(const char* fmt, ...)
    {
        char buf[2048];
        SYSTEMTIME st;
        GetLocalTime(&st);
        int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                            "[%02d:%02d:%02d.%03d] ",
                            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        if (n < 0) n = 0;                      // _snprintf_s _TRUNCATE 返回 -1 时不能拿它做偏移
        if ((size_t)n > sizeof(buf) - 3) n = (int)(sizeof(buf) - 3);
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, fmt, ap);
        va_end(ap);
        // 原文：len 被 _TRUNCATE 截断到 sizeof(buf)-1（=2047）时，
        // buf[len+1] 会写到 buf[2048] —— 越过数组一位，直接破坏 /GS cookie，
        // 触发 __report_gsfailure（0xC0000409）整进程闪退。
        // 任何接近 2KB 的日志（如 MOD 异常的完整堆栈）都会命中，必须留出 \r\n\0 三个字节。
        size_t len = strlen(buf);
        if (len > sizeof(buf) - 3) len = sizeof(buf) - 3;
        buf[len] = '\r';
        buf[len + 1] = '\n';
        buf[len + 2] = '\0';

        OutputDebugStringA(buf);
        if (g_file && g_file != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(g_file, buf, (DWORD)(len + 2), &written, nullptr);
        }
    }
}
