// download.h - HTTPS downloads checked by SHA256 (download.cpp).
#pragma once
#include <windows.h>

// Downloads to `path` (via path.part), only kept when its SHA256 matches. got/total: progress in
// bytes (total 0 = unknown); *cancel stops it. err: why it failed.
bool HttpDownload(const wchar_t* url, const wchar_t* path, const char* sha256, volatile LONG64* got,
				  volatile LONG64* total, volatile LONG* cancel, char* err, size_t errCap);
bool FileSha256(const wchar_t* path, char hex[65]);  // lowercase hex; false if the file can't be read
