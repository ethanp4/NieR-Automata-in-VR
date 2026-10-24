// frame_capture.cpp - press [Debug] CaptureKey (F11) in game: the next complete frame of the game's
// D3D11 activity (constant-buffer contents, shader binds, draws, render targets, shader bytecode) is
// written to NieRVR_capture_N.bin, and likely projection matrices are summarised in the log.
// tools/analyze_capture.py reads the file.
#include "debug/frame_capture.h"
#include "core/config.h"
#include "core/log.h"
#include "core/main.h"
#include "core/pure.h"
#include <stdio.h>
#include <string.h>

namespace {
enum RecordType : uint16_t {
	R_FRAME_BEGIN = 1,  // u32 frame, u32 0, u32 0
	R_SET_CB = 2,       // u8 stage, u8 pad[3], u32 start, u32 count, u64 bufs[count]
	R_CB_DATA = 3,      // u64 buf, u32 byteWidth, u8 via(0 map,1 update,2 readback), u8 pad[3], data
	R_SET_SHADER = 4,   // u8 stage, pad[7], u64 shader
	R_DRAW = 5,         // u8 kind(0 Draw,1 Indexed,2 Inst,3 IndexedInst), pad[3], u32 a, u32 b, u32 c
	R_SET_RT = 6,       // u32 numRTV, u32 w, u32 h, u32 fmt, u64 dsv, u64 rtv0
	R_VIEWPORT = 7,     // u32 n, then n * 6 floats
	R_SHADER_BLOB = 8,  // u8 stage, pad[7], u64 shader, u64 hash, bytecode
	R_DISPATCH = 9,     // u32 x, y, z
	R_EXEC_CMDLIST = 10,
	R_FRAME_END = 12,
	R_SET_SRV = 13,  // u8 stage, pad[3], u32 start, u32 count, count * {u64 res, u32 w, u32 h, u32 fmt, u32 dim}
	R_SET_UAV = 14,  // same layout as R_SET_SRV (stage = CS)
};
static_assert(sizeof(D3D11_VIEWPORT) == 24, "R_VIEWPORT layout");

struct RecordHeader {
	uint16_t type;
	uint16_t pad;
	uint32_t size;
};

ID3D11Device* s_dev;
ID3D11DeviceContext* s_ctx;
volatile bool s_capturing;
bool s_armed;
bool s_keyWasDown;
unsigned s_captureIndex;
SRWLOCK s_recordLock = SRWLOCK_INIT;
uint8_t* s_buf;
size_t s_len, s_cap;
bool s_overflow;

void Emit(uint16_t type, const void* a, uint32_t alen, const void* b = nullptr, uint32_t blen = 0) {
	AcquireSRWLockExclusive(&s_recordLock);
	size_t need = sizeof(RecordHeader) + alen + blen;
	if (!s_buf || s_len + need > s_cap) {
		s_overflow = true;
	} else {
		RecordHeader h = {type, 0, alen + blen};
		memcpy(s_buf + s_len, &h, sizeof(h));
		memcpy(s_buf + s_len + sizeof(h), a, alen);
		if (blen) memcpy(s_buf + s_len + sizeof(h) + alen, b, blen);
		s_len += need;
	}
	ReleaseSRWLockExclusive(&s_recordLock);
}

// Constant buffers seen this frame: kept alive (AddRef) until the frame is written.
struct SeenBuffer {
	ID3D11Buffer* buf;
	bool hasData;
};
const int kMaxSeen = 8192;
SeenBuffer s_seen[kMaxSeen];
int s_seenCount;

SeenBuffer* SeenFind(ID3D11Buffer* b) {
	for (int i = 0; i < s_seenCount; i++)
		if (s_seen[i].buf == b) return &s_seen[i];
	return nullptr;
}
void SeenAdd(ID3D11Buffer* b, bool data) {
	if (!b) return;
	AcquireSRWLockExclusive(&s_recordLock);
	SeenBuffer* s = SeenFind(b);
	if (!s && s_seenCount < kMaxSeen) {
		s = &s_seen[s_seenCount++];
		s->buf = b;
		s->hasData = false;
		b->AddRef();
	}
	if (s && data) s->hasData = true;
	ReleaseSRWLockExclusive(&s_recordLock);
}

// Buffers currently mapped (Map .. Unmap) during the capture.
struct MappedBuffer {
	ID3D11Resource* res;
	void* data;
	UINT size;
};
MappedBuffer s_mapped[128];

void EmitCbData(ID3D11Buffer* buf, UINT byteWidth, uint8_t via, const void* data, UINT len) {
	struct {
		uint64_t buf;
		uint32_t byteWidth;
		uint8_t via, pad[3];
	} h = {(uint64_t)(uintptr_t)buf, byteWidth, via, {0, 0, 0}};
	static_assert(sizeof(h) == 16, "R_CB_DATA layout");
	Emit(R_CB_DATA, &h, sizeof(h), data, len);
	SeenAdd(buf, true);
}

// Shader-resource / UAV bindings: which textures and buffers each pass reads and writes, so data
// flow between passes (e.g. compute passes) can be followed offline.
struct ViewRecord {
	uint64_t res;
	uint32_t w, h, fmt, dim;
};
static_assert(sizeof(ViewRecord) == 24, "ViewRecord layout");
template <typename View>
void DescribeView(View* v, ViewRecord* r) {
	*r = {};
	if (!v) return;
	ID3D11Resource* res = nullptr;
	v->GetResource(&res);
	if (!res) return;
	r->res = (uint64_t)(uintptr_t)res;
	D3D11_RESOURCE_DIMENSION dim;
	res->GetType(&dim);
	r->dim = (uint32_t)dim;
	if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
		D3D11_TEXTURE2D_DESC d;
		((ID3D11Texture2D*)res)->GetDesc(&d);
		r->w = d.Width;
		r->h = d.Height;
		r->fmt = d.Format;
	} else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
		D3D11_BUFFER_DESC d;
		((ID3D11Buffer*)res)->GetDesc(&d);
		r->w = d.ByteWidth;
		r->h = d.StructureByteStride;
	}
	res->Release();
}
// NieR binds pixel shader resources many slots at a time (more than 16), so long calls go out in
// chunks of 16, without the trailing empty slots.
template <typename View>
void EmitViews(uint16_t type, uint8_t stage, UINT start, UINT n, View* const* views) {
	if (!views) return;
	if (n > 128) n = 128;
	while (n > 0 && !views[n - 1]) n--;
	for (UINT base = 0; base < n; base += 16) {
		UINT k = n - base < 16 ? n - base : 16;
		struct {
			uint8_t stage, pad[3];
			uint32_t start, count;
		} h = {stage, {0, 0, 0}, start + base, k};
		static_assert(sizeof(h) == 12, "R_SET_SRV layout");
		ViewRecord records[16];
		for (UINT i = 0; i < k; i++) DescribeView(views[base + i], &records[i]);
		Emit(type, &h, sizeof(h), records, k * sizeof(ViewRecord));
	}
}

// --- frame end: readback, shader blobs, file, summary ---------------------------------------
void ReadbackUnseenBuffers(int* readbacks) {
	for (int i = 0; i < s_seenCount; i++) {
		SeenBuffer& s = s_seen[i];
		if (s.hasData) continue;
		D3D11_BUFFER_DESC d;
		s.buf->GetDesc(&d);
		if (d.ByteWidth > 65536) continue;
		d.Usage = D3D11_USAGE_STAGING;
		d.BindFlags = 0;
		d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		d.MiscFlags = 0;
		d.StructureByteStride = 0;
		ID3D11Buffer* staging = nullptr;
		if (FAILED(s_dev->CreateBuffer(&d, nullptr, &staging))) continue;
		s_ctx->CopyResource(staging, s.buf);
		D3D11_MAPPED_SUBRESOURCE m;
		if (SUCCEEDED(s_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
			struct {
				uint64_t buf;
				uint32_t byteWidth;
				uint8_t via, pad[3];
			} h = {(uint64_t)(uintptr_t)s.buf, d.ByteWidth, 2, {0, 0, 0}};
			static_assert(sizeof(h) == 16, "R_CB_DATA layout");
			Emit(R_CB_DATA, &h, sizeof(h), m.pData, d.ByteWidth);
			s_ctx->Unmap(staging, 0);
			(*readbacks)++;
		}
		staging->Release();
	}
}

void EmitShaderBlob(ShaderStage stage, void* shader, uint64_t hash, const uint8_t* blob, uint32_t len) {
	struct {
		uint8_t stage, pad[7];
		uint64_t sh, hash;
	} h = {stage, {0}, (uint64_t)(uintptr_t)shader, hash};
	static_assert(sizeof(h) == 24, "R_SHADER_BLOB layout");
	Emit(R_SHADER_BLOB, &h, sizeof(h), blob, len);
}

// Logs the projection matrices found in this frame's constant buffers.
void Summarise() {
	struct Candidate {
		uint64_t buf;
		uint32_t offset;
		pure::ProjInfo p;
		unsigned draws;
	} cands[24];
	int nc = 0;
	unsigned draws = 0, cbData = 0, rts = 0, dispatches = 0;
	for (size_t off = 0; off + sizeof(RecordHeader) <= s_len;) {
		RecordHeader h;
		memcpy(&h, s_buf + off, sizeof(h));
		const uint8_t* p = s_buf + off + sizeof(h);
		if (h.type == R_DRAW) draws++;
		if (h.type == R_SET_RT) rts++;
		if (h.type == R_DISPATCH) dispatches++;
		if (h.type == R_CB_DATA && h.size >= 16) {
			cbData++;
			uint64_t buf;
			memcpy(&buf, p, 8);
			const uint8_t* data = p + 16;
			uint32_t len = h.size - 16;
			for (uint32_t o = 0; o + 64 <= len && nc < 24; o += 16) {
				float m[16];
				memcpy(m, data + o, 64);
				pure::ProjInfo pi;
				if (!pure::classify_projection(m, &pi)) continue;
				bool dup = false;
				for (int i = 0; i < nc; i++)
					if (cands[i].buf == buf && cands[i].offset == o) dup = true;
				if (!dup) cands[nc++] = {buf, o, pi, 0};
			}
		}
		off += sizeof(h) + h.size;
	}
	// how many draws had each candidate buffer bound (any stage)
	uint64_t bound[4][14] = {};
	for (size_t off = 0; off + sizeof(RecordHeader) <= s_len;) {
		RecordHeader h;
		memcpy(&h, s_buf + off, sizeof(h));
		const uint8_t* p = s_buf + off + sizeof(h);
		if (h.type == R_SET_CB) {
			uint8_t st = p[0];
			uint32_t start, count;
			memcpy(&start, p + 4, 4);
			memcpy(&count, p + 8, 4);
			for (uint32_t i = 0; i < count && st < 4 && start + i < 14; i++) memcpy(&bound[st][start + i], p + 12 + i * 8, 8);
		} else if (h.type == R_DRAW) {
			for (int i = 0; i < nc; i++) {
				bool used = false;
				for (int s = 0; s < 4 && !used; s++)
					for (int k = 0; k < 14 && !used; k++) used = bound[s][k] == cands[i].buf;
				if (used) cands[i].draws++;
			}
		}
		off += sizeof(h) + h.size;
	}
	Log("capture: frame had %u draws, %u dispatches, %u render-target changes, %u constant-buffer uploads%s", draws,
		dispatches, rts, cbData, s_overflow ? " (CAPTURE TRUNCATED: buffer full)" : "");
	if (!nc) Log("capture: no perspective projection matrix found in constant buffers this frame");
	for (int i = 0; i < nc; i++) {
		const pure::ProjInfo& p = cands[i].p;
		Log("capture: projection in cbuffer %p +%u: %s-major, vfov %.2f deg, aspect %.4f, off-centre (%.4f, %.4f), "
			"z = %.6f + %.6f/z, w sign %.0f, used by %u draws",
			(void*)(uintptr_t)cands[i].buf, cands[i].offset, p.rowMajor ? "row" : "column", (double)p.vfovDeg,
			(double)p.aspect, (double)p.cx, (double)p.cy, (double)p.A, (double)p.B, (double)p.wSign, cands[i].draws);
	}
}

void WriteCapture() {
	wchar_t path[MAX_PATH];
	lstrcpynW(path, g_modDir, MAX_PATH - 40);
	char name[48];
	snprintf(name, sizeof(name), "NieRVR_capture_%u.bin", s_captureIndex);
	int n = lstrlenW(path);
	for (int i = 0; name[i] && n < MAX_PATH - 1; i++) path[n++] = (wchar_t)name[i];
	path[n] = 0;
	HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) {
		Log("ERROR: capture: cannot write %s (error %u)", name, (unsigned)GetLastError());
		return;
	}
	const char magic[8] = {'N', 'V', 'R', 'C', 'A', 'P', '0', '1'};
	DWORD w;
	WriteFile(f, magic, 8, &w, nullptr);
	for (size_t off = 0; off < s_len;) {
		DWORD chunk = (DWORD)(s_len - off > (16u << 20) ? (16u << 20) : s_len - off);
		if (!WriteFile(f, s_buf + off, chunk, &w, nullptr)) break;
		off += chunk;
	}
	CloseHandle(f);
	Log("capture: wrote %s (%u KB)", name, (unsigned)((s_len + 8) >> 10));
}

void FinishCapture() {
	s_capturing = false;
	int readbacks = 0;
	ReadbackUnseenBuffers(&readbacks);
	int blobs = ShaderTableTakeUsed(EmitShaderBlob);
	uint32_t z[3] = {s_captureIndex, 0, 0};
	Emit(R_FRAME_END, z, 12);
	Summarise();
	Log("capture: %d constant buffers read back, %d shaders included, %u shaders known", readbacks, blobs,
		ShaderTableCount());
	WriteCapture();
	AcquireSRWLockExclusive(&s_recordLock);  // a late hook call on another thread must see s_buf == null
	uint8_t* buf = s_buf;
	s_buf = nullptr;
	s_len = s_cap = 0;
	for (int i = 0; i < s_seenCount; i++) s_seen[i].buf->Release();
	s_seenCount = 0;
	for (auto& e : s_mapped) e.res = nullptr;
	ReleaseSRWLockExclusive(&s_recordLock);
	VirtualFree(buf, 0, MEM_RELEASE);
}
}  // namespace

void CaptureSetDevice(ID3D11Device* device, ID3D11DeviceContext* context) {
	s_dev = device;
	s_ctx = context;
}

bool CaptureRecording(ID3D11DeviceContext* c) { return s_capturing && c == s_ctx; }

void CaptureBeforePresent() {
	if (!s_ctx) return;
	if (s_capturing) FinishCapture();
	// hotkey (edge-triggered, only while the game window has focus)
	int vk = g_cfg.probe ? g_cfg.keyCapture : 0;
	if (vk > 0 && vk < 256) {
		bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
		bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;  // Ctrl+key is another hotkey
		if (down && !s_keyWasDown && !ctrl) {
			HWND fg = GetForegroundWindow();
			DWORD pid = 0;
			if (fg) GetWindowThreadProcessId(fg, &pid);
			if (pid == GetCurrentProcessId()) s_armed = true;
		}
		s_keyWasDown = down;
	}
}

void CaptureAfterPresent() {
	if (!s_armed || s_capturing) return;
	s_armed = false;
	s_cap = 256u << 20;  // reserved+committed lazily by the OS as it is touched
	s_buf = (uint8_t*)VirtualAlloc(nullptr, s_cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!s_buf) {
		Log("ERROR: capture: could not allocate capture memory");
		s_cap = 0;
		return;
	}
	s_len = 0;
	s_overflow = false;
	s_captureIndex++;
	uint32_t h[3] = {s_captureIndex, 0, 0};
	Emit(R_FRAME_BEGIN, h, 12);
	Log("capture: capturing frame (capture %u)", s_captureIndex);
	s_capturing = true;
}

void CaptureSetConstantBuffers(ShaderStage stage, UINT start, UINT n, ID3D11Buffer* const* buffers) {
	if (!buffers || n > 14) return;
	struct {
		uint8_t stage, pad[3];
		uint32_t start, count;
	} h = {(uint8_t)stage, {0, 0, 0}, start, n};
	static_assert(sizeof(h) == 12, "R_SET_CB layout");
	uint64_t ptrs[14];
	for (UINT i = 0; i < n; i++) {
		ptrs[i] = (uint64_t)(uintptr_t)buffers[i];
		SeenAdd(buffers[i], false);
	}
	Emit(R_SET_CB, &h, sizeof(h), ptrs, n * 8);
}

void CaptureSetShader(ShaderStage stage, void* shader) {
	struct {
		uint8_t stage, pad[7];
		uint64_t sh;
	} h = {(uint8_t)stage, {0}, (uint64_t)(uintptr_t)shader};
	static_assert(sizeof(h) == 16, "R_SET_SHADER layout");
	Emit(R_SET_SHADER, &h, sizeof(h));
	ShaderTableMarkUsed(shader);
}

void CaptureDraw(uint8_t kind, UINT a, UINT b, UINT c) {
	struct {
		uint8_t kind, pad[3];
		uint32_t a, b, c;
	} h = {kind, {0, 0, 0}, a, b, c};
	static_assert(sizeof(h) == 16, "R_DRAW layout");
	Emit(R_DRAW, &h, sizeof(h));
}

void CaptureMap(ID3D11Resource* buffer, void* data, UINT size) {
	AcquireSRWLockExclusive(&s_recordLock);
	for (auto& e : s_mapped)
		if (!e.res) {
			e = {buffer, data, size};
			break;
		}
	ReleaseSRWLockExclusive(&s_recordLock);
}

void CaptureUnmap(ID3D11Resource* buffer) {
	MappedBuffer found = {};
	AcquireSRWLockExclusive(&s_recordLock);
	for (auto& e : s_mapped)
		if (e.res == buffer) {
			found = e;
			e.res = nullptr;
			break;
		}
	ReleaseSRWLockExclusive(&s_recordLock);
	if (found.res && found.data) EmitCbData((ID3D11Buffer*)buffer, found.size, 0, found.data, found.size);
}

void CaptureUpdate(ID3D11Resource* buffer, const void* data, UINT size) {
	EmitCbData((ID3D11Buffer*)buffer, size, 1, data, size);
}

void CaptureSetRenderTargets(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
	struct {
		uint32_t n, w, h, fmt;
		uint64_t dsv, rtv0;
	} rec = {n, 0, 0, 0, (uint64_t)(uintptr_t)dsv, 0};
	static_assert(sizeof(rec) == 32, "R_SET_RT layout");
	ID3D11View* view = (n && rtvs && rtvs[0]) ? (ID3D11View*)rtvs[0] : (ID3D11View*)dsv;
	if (n && rtvs) rec.rtv0 = (uint64_t)(uintptr_t)rtvs[0];
	if (view) {
		ID3D11Resource* res = nullptr;
		view->GetResource(&res);
		D3D11_RESOURCE_DIMENSION dim;
		if (res) {
			res->GetType(&dim);
			if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
				D3D11_TEXTURE2D_DESC td;
				((ID3D11Texture2D*)res)->GetDesc(&td);
				rec.w = td.Width;
				rec.h = td.Height;
				rec.fmt = td.Format;
			}
			// record resources (not views), so render targets can be matched with later SRV reads
			if (n && rtvs && rtvs[0]) rec.rtv0 = (uint64_t)(uintptr_t)res;
			res->Release();
		}
	}
	if (dsv) {
		ID3D11Resource* dres = nullptr;
		dsv->GetResource(&dres);
		if (dres) {
			rec.dsv = (uint64_t)(uintptr_t)dres;
			dres->Release();
		}
	}
	Emit(R_SET_RT, &rec, sizeof(rec));
}

void CaptureViewports(UINT n, const D3D11_VIEWPORT* viewports) {
	if (!viewports || n > 16) return;
	uint32_t count = n;
	Emit(R_VIEWPORT, &count, 4, viewports, n * sizeof(D3D11_VIEWPORT));
}

void CaptureExecuteCommandList() {
	uint32_t zero = 0;
	Emit(R_EXEC_CMDLIST, &zero, 4);
}

void CaptureShaderResources(ShaderStage stage, UINT start, UINT n, ID3D11ShaderResourceView* const* views) {
	EmitViews(R_SET_SRV, stage, start, n, views);
}

void CaptureUnorderedAccessViews(UINT start, UINT n, ID3D11UnorderedAccessView* const* views) {
	EmitViews(R_SET_UAV, ST_CS, start, n, views);
}

void CaptureDispatch(UINT x, UINT y, UINT z) {
	uint32_t groups[3] = {x, y, z};
	Emit(R_DISPATCH, groups, 12);
}
