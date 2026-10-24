#pragma once
#include <windows.h>

bool HttpDownload(const wchar_t* url, const wchar_t* path, const char* sha256, volatile LONG64* got,
				  volatile LONG64* total, volatile LONG* cancel, char* err, size_t errCap);
bool FileSha256(const wchar_t* path, char hex[65]);  // lowercase hex; false if the file can't be read
