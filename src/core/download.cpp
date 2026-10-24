// download.cpp - one HTTPS file download with Windows' own WinHTTP, checked by SHA256 (bcrypt).
//
// Runs on the caller's thread (dlss.cpp starts one for it). The file is written next to its
// destination as "<name>.part", hashed while it arrives, and only renamed into place when the hash
// matches: a cut or wrong download never leaves a broken file behind. Redirects (GitHub sends large
// files on to its download host) are followed by WinHTTP; the system's proxy settings are used.
#include "core/download.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <winhttp.h>
#include <bcrypt.h>

namespace {
struct Sha256 {
	BCRYPT_ALG_HANDLE alg = nullptr;
	BCRYPT_HASH_HANDLE h = nullptr;
	bool ok = false;
	Sha256() {
		ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
			 BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) >= 0;
	}
	~Sha256() {
		if (h) BCryptDestroyHash(h);
		if (alg) BCryptCloseAlgorithmProvider(alg, 0);
	}
	void Add(const void* p, DWORD n) { ok = ok && BCryptHashData(h, (PUCHAR)p, n, 0) >= 0; }
	// lowercase hex, 64 characters
	bool Hex(char out[65]) {
		uint8_t d[32];
		if (!ok || BCryptFinishHash(h, d, 32, 0) < 0) return false;
		for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
		return true;
	}
};

struct Inet {
	HINTERNET h = nullptr;
	~Inet() {
		if (h) WinHttpCloseHandle(h);
	}
};
}  // namespace

bool HttpDownload(const wchar_t* url, const wchar_t* path, const char* sha256, volatile LONG64* got,
				  volatile LONG64* total, volatile LONG* cancel, char* err, size_t errCap) {
	*got = 0;
	*total = 0;
	err[0] = 0;
	URL_COMPONENTS u = {};
	u.dwStructSize = sizeof(u);
	wchar_t host[256], pathPart[1024];
	u.lpszHostName = host;
	u.dwHostNameLength = 256;
	u.lpszUrlPath = pathPart;
	u.dwUrlPathLength = 1024;
	if (!WinHttpCrackUrl(url, 0, 0, &u)) {
		snprintf(err, errCap, "bad address");
		return false;
	}
	// automatic proxy needs Windows 8.1; older systems fall back to the default proxy settings
	Inet s, c, r;
	s.h = WinHttpOpen(L"NieRVR", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
	if (!s.h) s.h = WinHttpOpen(L"NieRVR", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
	if (s.h) c.h = WinHttpConnect(s.h, host, u.nPort, 0);
	if (c.h)
		r.h = WinHttpOpenRequest(c.h, L"GET", pathPart, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
								 u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
	if (!r.h || !WinHttpSendRequest(r.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
		!WinHttpReceiveResponse(r.h, nullptr)) {
		snprintf(err, errCap, "no connection (error %u)", (unsigned)GetLastError());
		return false;
	}
	DWORD status = 0, sz = sizeof(status);
	WinHttpQueryHeaders(r.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
	if (status != 200) {
		snprintf(err, errCap, "the server answered %u", (unsigned)status);
		return false;
	}
	wchar_t len[32];
	sz = sizeof(len);
	if (WinHttpQueryHeaders(r.h, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, len, &sz, nullptr)) *total = _wtoi64(len);

	wchar_t part[MAX_PATH];
	snwprintf(part, MAX_PATH, L"%ls.part", path);
	HANDLE f = CreateFileW(part, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) {
		snprintf(err, errCap, "can't write to the game's folder (error %u)", (unsigned)GetLastError());
		return false;
	}
	Sha256 hash;
	static char buf[1 << 16];
	bool ok = true;
	for (;;) {
		if (*cancel) {
			snprintf(err, errCap, "cancelled");
			ok = false;
			break;
		}
		DWORD n = 0, wrote = 0;
		if (!WinHttpReadData(r.h, buf, sizeof(buf), &n)) {
			snprintf(err, errCap, "the connection broke (error %u)", (unsigned)GetLastError());
			ok = false;
			break;
		}
		if (n == 0) break;  // done
		hash.Add(buf, n);
		if (!WriteFile(f, buf, n, &wrote, nullptr) || wrote != n) {
			snprintf(err, errCap, "can't write (disk full?)");
			ok = false;
			break;
		}
		InterlockedAdd64(got, n);
	}
	CloseHandle(f);
	char hex[65];
	if (ok && (!hash.Hex(hex) || _stricmp(hex, sha256) != 0)) {
		snprintf(err, errCap, "the file isn't the expected one (checksum)");
		ok = false;
	}
	if (ok && !MoveFileExW(part, path, MOVEFILE_REPLACE_EXISTING)) {
		snprintf(err, errCap, "can't put the file in place (error %u)", (unsigned)GetLastError());
		ok = false;
	}
	if (!ok) DeleteFileW(part);
	return ok;
}

bool FileSha256(const wchar_t* path, char hex[65]) {
	HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
						   FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (f == INVALID_HANDLE_VALUE) return false;
	Sha256 hash;
	static char buf[1 << 16];
	DWORD n = 0;
	bool ok = true;
	while ((ok = ReadFile(f, buf, sizeof(buf), &n, nullptr)) && n > 0) hash.Add(buf, n);
	CloseHandle(f);
	return ok && hash.Hex(hex);
}
