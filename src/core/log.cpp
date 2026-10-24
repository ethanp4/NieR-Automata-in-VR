// log.cpp - thread-safe, crash-tolerant log file (NieRVR.log).
#include "core/log.h"
#include "core/config.h"
#include <stdio.h>

static HANDLE s_logFile = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION s_logLock;
static LARGE_INTEGER s_qpcFrequency, s_qpcStart;

void LogOpen(const wchar_t* path) {
	InitializeCriticalSection(&s_logLock);
	QueryPerformanceFrequency(&s_qpcFrequency);
	QueryPerformanceCounter(&s_qpcStart);
	// Keep the previous session's log as NieRVR.prev.log for comparison
	wchar_t prev[MAX_PATH];
	lstrcpynW(prev, path, MAX_PATH - 16);
	int length = lstrlenW(prev);
	if (length > 4) {
		prev[length - 4] = 0;  // strip ".log"
		lstrcatW(prev, L".prev.log");
		MoveFileExW(path, prev, MOVEFILE_REPLACE_EXISTING);
	}
	// Writes go straight to the OS cache, so the log survives a game crash.
	s_logFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
							FILE_ATTRIBUTE_NORMAL, nullptr);
}

Utf8::Utf8(const wchar_t* text) {
	s[0] = 0;
	if (text && !WideCharToMultiByte(CP_UTF8, 0, text, -1, s, sizeof(s), nullptr, nullptr)) s[sizeof(s) - 1] = 0;
}

static void LogWrite(const char* fmt, va_list args) {
	if (s_logFile == INVALID_HANDLE_VALUE) return;
	char line[2048];
	SYSTEMTIME time;
	GetLocalTime(&time);
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	double secs = (double)(now.QuadPart - s_qpcStart.QuadPart) / (double)s_qpcFrequency.QuadPart;
	int stampLen = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%9.3f] [t%5u] ", time.wHour, time.wMinute, time.wSecond,
							time.wMilliseconds, secs, (unsigned)GetCurrentThreadId());
	if (stampLen > (int)sizeof(line) - 3) stampLen = (int)sizeof(line) - 3;
	int textLen = vsnprintf(line + stampLen, sizeof(line) - stampLen - 2, fmt, args);
	if (textLen < 0) textLen = 0;  // a bad format string: keep the time stamp
	int total = stampLen + textLen;
	if (total > (int)sizeof(line) - 3) total = (int)sizeof(line) - 3;
	line[total++] = '\r';
	line[total++] = '\n';
	EnterCriticalSection(&s_logLock);
	DWORD written;
	WriteFile(s_logFile, line, (DWORD)total, &written, nullptr);
	LeaveCriticalSection(&s_logLock);
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
	LONG count = InterlockedIncrement(counter);
	if (count == max) Log("(the next message repeats; further repeats are suppressed)");
	return count <= max;
}
