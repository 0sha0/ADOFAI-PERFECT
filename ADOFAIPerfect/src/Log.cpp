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
        if (!GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH))
            GetTempPathA(MAX_PATH, path);
        CreateDirectoryA(path, nullptr);
        strncat_s(path, "\\ADOFvec", _TRUNCATE);
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
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, fmt, ap);
        va_end(ap);
        size_t len = strlen(buf);
        buf[len] = '\r';
        buf[len + 1] = '\n';

        OutputDebugStringA(buf);
        if (g_file && g_file != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(g_file, buf, (DWORD)(len + 2), &written, nullptr);
        }
    }
}
