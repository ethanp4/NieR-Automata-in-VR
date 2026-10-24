// log.cpp - thread-safe, crash-tolerant log file (NieRVR.log).
#include "core/log.h"
#include "core/config.h"
#include <stdio.h>

static HANDLE s_logFile = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION s_logCs;
static LARGE_INTEGER s_qpcFreq, s_qpcStart;

void LogOpen(const wchar_t* path) {
	InitializeCriticalSection(&s_logCs);
	QueryPerformanceFrequency(&s_qpcFreq);
	QueryPerformanceCounter(&s_qpcStart);
	// Keep the previous session's log as NieRVR.prev.log for comparison
	wchar_t prev[MAX_PATH];
	lstrcpynW(prev, path, MAX_PATH - 16);
	int n = lstrlenW(prev);
	if (n > 4) {
		prev[n - 4] = 0;  // strip ".log"
		lstrcatW(prev, L".prev.log");
		MoveFileExW(path, prev, MOVEFILE_REPLACE_EXISTING);
	}
	// Writes go straight to the OS cache, so the log survives a game crash.
	s_logFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
							FILE_ATTRIBUTE_NORMAL, nullptr);
}

Utf8::Utf8(const wchar_t* w) {
	s[0] = 0;
	if (w && !WideCharToMultiByte(CP_UTF8, 0, w, -1, s, sizeof(s), nullptr, nullptr)) s[sizeof(s) - 1] = 0;
}

static void LogWrite(const char* fmt, va_list ap) {
	if (s_logFile == INVALID_HANDLE_VALUE) return;
	char line[2048];
	SYSTEMTIME st;
	GetLocalTime(&st);
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	double secs = (double)(now.QuadPart - s_qpcStart.QuadPart) / (double)s_qpcFreq.QuadPart;
	int n = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%9.3f] [t%5u] ", st.wHour, st.wMinute, st.wSecond,
					 st.wMilliseconds, secs, (unsigned)GetCurrentThreadId());
	if (n > (int)sizeof(line) - 3) n = (int)sizeof(line) - 3;
	int m = vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
	if (m < 0) m = 0;  // a bad format string: keep the time stamp
	int total = n + m;
	if (total > (int)sizeof(line) - 3) total = (int)sizeof(line) - 3;
	line[total++] = '\r';
	line[total++] = '\n';
	EnterCriticalSection(&s_logCs);
	DWORD written;
	WriteFile(s_logFile, line, (DWORD)total, &written, nullptr);
	LeaveCriticalSection(&s_logCs);
}

void Log(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	LogWrite(fmt, ap);
	va_end(ap);
}

void LogV(const char* fmt, ...) {
	if (!g_cfg.verbose) return;
	va_list ap;
	va_start(ap, fmt);
	LogWrite(fmt, ap);
	va_end(ap);
}

bool LogBudget(volatile LONG* counter, LONG max) {
	LONG n = InterlockedIncrement(counter);
	if (n == max) Log("(the next message repeats; further repeats are suppressed)");
	return n <= max;
}
