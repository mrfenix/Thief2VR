#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <share.h>

static FILE* g_log;
static CRITICAL_SECTION g_lock;

void LogInit()
{
    InitializeCriticalSection(&g_lock);
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    while (n > 0 && path[n - 1] != '\\')
        --n;
    lstrcpyA(path + n, "thief2vr.log");
    g_log = _fsopen(path, "w", _SH_DENYWR);
}

void Log(const char* fmt, ...)
{
    if (!g_log)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    EnterCriticalSection(&g_lock);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_lock);
}
