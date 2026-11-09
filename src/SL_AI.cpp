/*******************************************************************/
/* SL_AI — see SL_AI.h                                              */
/*                                                                 */
/* ONNX Runtime 以 C API 动态加载 (不依赖导入库, DLL 放在           */
/* 插件目录的 SanLight_AI 子目录), 模型同目录。                     */
/*******************************************************************/

#ifdef _WIN32
#include <Windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <xmmintrin.h>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

/* onnxruntime 1.19 C API 头 (NuGet build/native/include) */
#include "onnxruntime_c_api.h"

#include "SL_AI.h"
#include "SL_History.h"
#include "SL_DiskCache.h"
#include "SL_Parallel.h"
#include "SL_ImageDigest.h"

/* ---------------------------------------------------------------- */
/* state                                                            */

static std::mutex	g_lock;
static bool			g_triedInit = false;
static bool			g_ok = false;
static HMODULE		g_hOrtDll = NULL;
static const OrtApi* g_ort = NULL;
static OrtEnv*		g_env = NULL;
static OrtSessionOptions* g_opt = NULL;
static OrtSession*	g_sess = NULL;
static OrtMemoryInfo* g_mem = NULL;
static OrtAllocator* g_alloc = NULL;
static char			g_inputName[128] = { 0 };
static char			g_outputName[128] = { 0 };
static char			g_err[256] = { 0 };
static uint64_t g_cacheIdentity[4]={};
static bool g_cacheIdentityValid=false;
static bool g_usingGPU=false;
static char g_backendInfo[256]="Not initialized";

/* 缓存 (单条目): 输入哈希 + 质量 + 平滑 + 强度 -> 近度/法线 */
static SL_ImageDigest g_cacheHash;
static int			g_cacheQuality = -1, g_cacheSmooth = -1;
static float		g_cacheStrength = -1.f;
static int			g_cacheW = 0, g_cacheH = 0;
static float*		g_cacheNear = NULL;		/* w*h */
static float*		g_cacheNorm = NULL;		/* w*h*3 */
static SL_HistoryKey g_rawCacheKey;
static std::vector<float> g_rawCache;
static bool g_rawCacheValid=false;

static const int QUALITY_SIZES[4] = { 280, 392, 518, 700 };

static void SL_AI_SetErr(const char* msg) {
	strncpy_s(g_err, sizeof(g_err), msg, _TRUNCATE);
}

const char* SL_AI_LastError(void) {
	static thread_local char errorCopy[sizeof(g_err)];
	std::lock_guard<std::mutex> guard(g_lock);
	memcpy(errorCopy, g_err, sizeof(errorCopy));
	errorCopy[sizeof(errorCopy) - 1] = '\0';
	return errorCopy;
}
int SL_AI_UsingGPU(void) {
	std::lock_guard<std::mutex> guard(g_lock);
	return g_usingGPU?1:0;
}
const char* SL_AI_BackendInfo(void) {
	static thread_local char status[sizeof(g_backendInfo)];
	std::lock_guard<std::mutex> guard(g_lock);
	memcpy(status,g_backendInfo,sizeof(status));
	return status;
}

/* ---------------------------------------------------------------- */
/* helpers                                                          */

/* 双线性下采样到 qw x qh (RGB, 0..255 -> 归一化 ImageNet) */
static void SL_DownscaleNormCHW(const float* rgba, int w, int h,
								int qw, int qh, float* out /*3*qw*qh*/) {
	const float mean[3] = { 0.485f, 0.456f, 0.406f };
	const float stdv[3] = { 0.229f, 0.224f, 0.225f };
	const float sx = (float)w / qw, sy = (float)h / qh;
	for (int qy = 0; qy < qh; ++qy) {
		const float fy = (qy + 0.5f) * sy - 0.5f;
		int y0 = (int)floorf(fy); const float ty = fy - y0;
		if (y0 < 0) y0 = 0; if (y0 > h - 2) y0 = (h > 1) ? h - 2 : 0;
		for (int qx = 0; qx < qw; ++qx) {
			const float fx = (qx + 0.5f) * sx - 0.5f;
			int x0 = (int)floorf(fx); const float tx = fx - x0;
			if (x0 < 0) x0 = 0; if (x0 > w - 2) x0 = (w > 1) ? w - 2 : 0;
			float rgb[3] = { 0.f, 0.f, 0.f };
			const float wts[4] = { (1 - tx) * (1 - ty), tx * (1 - ty),
								   (1 - tx) * ty,       tx * ty };
			for (int k = 0; k < 4; ++k) {
				const int px = (x0 + (k & 1) < w) ? x0 + (k & 1) : w - 1;
				const int py = (y0 + (k >> 1) < h) ? y0 + (k >> 1) : h - 1;
				const float* p = rgba + ((size_t)py * w + px) * 4;
				rgb[0] += p[0] * wts[k]; rgb[1] += p[1] * wts[k]; rgb[2] += p[2] * wts[k];
			}
			float* o = out + ((size_t)qy * qw + qx);
			o[0 * (size_t)qw * qh] = (rgb[0] - mean[0]) / stdv[0];
			o[1 * (size_t)qw * qh] = (rgb[1] - mean[1]) / stdv[1];
			o[2 * (size_t)qw * qh] = (rgb[2] - mean[2]) / stdv[2];
		}
	}
}

/* 双线性上采样单通道 */
static void SL_Upsample(const float* src, int sw, int sh,
						float* dst, int dw, int dh) {
	const float sx = (float)sw / dw, sy = (float)sh / dh;
	for (int y = 0; y < dh; ++y) {
		const float fy = (y + 0.5f) * sy - 0.5f;
		int y0 = (int)floorf(fy); const float ty = fy - y0;
		if (y0 < 0) y0 = 0; if (y0 > sh - 2) y0 = (sh > 1) ? sh - 2 : 0;
		for (int x = 0; x < dw; ++x) {
			const float fx = (x + 0.5f) * sx - 0.5f;
			int x0 = (int)floorf(fx); const float tx = fx - x0;
			if (x0 < 0) x0 = 0; if (x0 > sw - 2) x0 = (sw > 1) ? sw - 2 : 0;
			const int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
			const int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
			const float top = src[(size_t)y0 * sw + x0] * (1 - tx) + src[(size_t)y0 * sw + x1] * tx;
			const float bot = src[(size_t)y1 * sw + x0] * (1 - tx) + src[(size_t)y1 * sw + x1] * tx;
			dst[(size_t)y * dw + x] = top + (bot - top) * ty;
		}
	}
}

// A row is classified in short blocks. A block can skip per-pixel alpha reads
// only when its entire sampling window is opaque or below the gap threshold.
static constexpr int SL_BLUR_BLOCK = 16;
enum SL_BlurCoverage : unsigned char { SL_MIXED = 0, SL_OPAQUE = 1, SL_EMPTY = 2 };

template<bool vertical>
static void SL_BlurRow(const float* input, float* output, const float* rgba,
        const unsigned char* blocks, int w, int h, int y, int r, const float* kern) {
    // Build this sum in the exact order used by the scalar kernel.
    float opaqueTotal = kern[r];
    for (int pass = 0; pass < 2; ++pass)
        for (int d = 1; d <= r; ++d) opaqueTotal += kern[r+d];

    const size_t row = size_t(y) * w;
    const ptrdiff_t stride = vertical ? w : 1;
    const int blockCount = 1 + (w - 1) / SL_BLUR_BLOCK;
    for (int block = 0; block < blockCount; ++block) {
        const int first = block * SL_BLUR_BLOCK;
        const int end = first + (std::min)(SL_BLUR_BLOCK, w - first);
        unsigned char coverage = blocks ? blocks[size_t(y)*blockCount + block] : SL_MIXED;
        if (coverage != SL_MIXED) {
            if (vertical) {
                if (coverage == SL_OPAQUE && (y < r || y >= h-r)) coverage = SL_MIXED;
                for (int yy = (std::max)(0,y-r); coverage && yy <= (std::min)(h-1,y+r); ++yy)
                    coverage &= blocks[size_t(yy)*blockCount + block];
            } else {
                const unsigned char* flags = blocks + size_t(y)*blockCount;
                if (coverage == SL_OPAQUE && (first < r || end > w-r)) coverage = SL_MIXED;
                // Maximum supported radius is smaller than one block.
                if (block) coverage &= flags[block-1];
                if (block+1 < blockCount) coverage &= flags[block+1];
            }
        }
        int x = first;
        if (coverage != SL_MIXED) {
            const bool opaque = coverage == SL_OPAQUE;
            const __m128 center = _mm_set1_ps(kern[r]);
            const __m128 total = opaque ? _mm_set1_ps(opaqueTotal) : center;
            for (; x+4 <= end; x+=4) {
                const float* source = input + row + x;
                __m128 sum = _mm_mul_ps(_mm_loadu_ps(source),center);
                if (opaque) {
                    for (int direction = -1; direction <= 1; direction += 2)
                        for (int dist = 1; dist <= r; ++dist)
                            sum = _mm_add_ps(sum,_mm_mul_ps(_mm_loadu_ps(source+direction*dist*stride),
                                                           _mm_set1_ps(kern[r+dist])));
                }
                _mm_storeu_ps(output+row+x,_mm_div_ps(sum,total));
            }
        }
        // Semi-transparent edges, clipped kernels, and the short final tail.
        for (; x < end; ++x) {
            const size_t i = row + x;
            float sum = input[i] * kern[r], total = kern[r];
            const int coord = vertical ? y : x, extent = vertical ? h : w;
            for (int direction = -1; direction <= 1; direction += 2) {
                for (int dist = 1; dist <= r; ++dist) {
                    int sample = coord + direction*dist;
                    if (sample < 0 || sample >= extent) break;
                    size_t j = i + direction*dist*stride;
                    float a = rgba[4*j+3];
                    if (a < .05f) break;
                    float weight = kern[r+dist] * a;
                    sum += input[j]*weight; total += weight;
                }
            }
            output[i] = sum / total;
        }
    }
}

static void SL_GaussBlur(float* img, int w, int h, int r, const float* rgba, float* normScratch) {
    if (r <= 0) return;
    const int n = 2*r+1;
    float kern[21];
    std::unique_ptr<float, decltype(&free)> tmp((float*)malloc(size_t(w)*h*sizeof(float)),free);
    if (!tmp) throw std::bad_alloc();
    float sum = 0.f;
    for (int i = 0; i < n; ++i) {
        const float d = (float)(i-r);
        kern[i] = expf(-(d*d)/(2.f*r*r/4.f+1e-5f)); sum += kern[i];
    }
    for (int i = 0; i < n; ++i) kern[i] /= sum;
    const int blockCount = 1 + (w-1)/SL_BLUR_BLOCK;
    // Normal output is already allocated and not populated until after this call.
    unsigned char* blocks = r >= 3 && w >= SL_BLUR_BLOCK ?
        reinterpret_cast<unsigned char*>(normScratch) : nullptr;
    SL_ParallelRows(h,[&](int y) {
        if (blocks) {
            for (int b = 0; b < blockCount; ++b) {
                int x = b*SL_BLUR_BLOCK, count = (std::min)(SL_BLUR_BLOCK,w-x);
                const float* alpha = rgba + 4*(size_t(y)*w+x)+3;
                bool opaque = true, empty = true;
                for (int k=0; k<count; ++k) {
                    opaque &= alpha[4*k] == 1.f;
                    empty &= alpha[4*k] < .05f;
                }
                blocks[size_t(y)*blockCount+b] = opaque ? SL_OPAQUE : empty ? SL_EMPTY : SL_MIXED;
            }
        }
        SL_BlurRow<false>(img,tmp.get(),rgba,blocks,w,h,y,r,kern);
    });
    SL_ParallelRows(h,[&](int y) {
        SL_BlurRow<true>(tmp.get(),img,rgba,blocks,w,h,y,r,kern);
    });
}

/* 近度 -> 法线 (G-up 编码)。surface z(朝镜头) ∝ nearness,
   N ∝ (-dS/dx, -dS/dy_screen, 1/scale), scale 由 normStrength 控制。 */
static void SL_NormalsFromNearness(const float* nearMap, int w, int h,
								   float strength, float* outNorm /*w*h*3*/, const float* rgba) {
	const float s = strength / 100.f * 128.f;	/* 梯度量级补偿 */
	for (int y = 0; y < h; ++y) {
		const int ym = (y > 0) ? y - 1 : 0, yp = (y < h - 1) ? y + 1 : h - 1;
		for (int x = 0; x < w; ++x) {
			const int xm = (x > 0) ? x - 1 : 0, xp = (x < w - 1) ? x + 1 : w - 1;
			const size_t i = (size_t)y*w+x;
			const bool left = xm != x && rgba[((size_t)y*w+xm)*4+3] >= .05f;
			const bool right = xp != x && rgba[((size_t)y*w+xp)*4+3] >= .05f;
			const bool top = ym != y && rgba[((size_t)ym*w+x)*4+3] >= .05f;
			const bool bottom = yp != y && rgba[((size_t)yp*w+x)*4+3] >= .05f;
			const float gx = ((right ? nearMap[(size_t)y*w+xp] : nearMap[i]) -
				(left ? nearMap[(size_t)y*w+xm] : nearMap[i])) / (left && right ? 2.f : 1.f);
			const float gy = ((bottom ? nearMap[(size_t)yp*w+x] : nearMap[i]) -
				(top ? nearMap[(size_t)ym*w+x] : nearMap[i])) / (top && bottom ? 2.f : 1.f);
			float nx = -gx * s;
			float nyScreen = -gy * s;
			float nz = 1.f;
			const float len = sqrtf(nx * nx + nyScreen * nyScreen + nz * nz) + 1e-9f;
			nx /= len; nyScreen /= len; nz /= len;
			float* o = outNorm + ((size_t)y * w + x) * 3;
			o[0] = nx * 0.5f + 0.5f;			/* R */
			o[1] = -nyScreen * 0.5f + 0.5f;		/* G-up 编码 */
			o[2] = nz * 0.5f + 0.5f;			/* B */
		}
	}
}

void SL_AI_RebuildNormals(const float* nearMap, const float* rgba, int w, int h,
    float strength, float* outNorm) {
    if (!nearMap || !rgba || !outNorm || w < 1 || h < 1) return;
    // Do not repeatedly blur transported geometry: that erodes the shape over time.
    SL_NormalsFromNearness(nearMap, w, h, strength, outNorm, rgba);
}

/* ---------------------------------------------------------------- */
/* init                                                             */

static bool SL_AI_HashFile(HANDLE file,uint64_t* hash) {
	if(file==INVALID_HANDLE_VALUE)return false;
	hash[0]=1469598103934665603ULL;hash[1]=0x9e3779b97f4a7c15ULL;
	unsigned char buffer[65536];DWORD count=0;
	for(;;) {
		if(!ReadFile(file,buffer,sizeof(buffer),&count,nullptr))return false;
		if(!count)return true;
		for(DWORD i=0;i<count;i++) {
			hash[0]=(hash[0]^buffer[i])*1099511628211ULL;
			hash[1]^=buffer[i]+0x9e3779b97f4a7c15ULL+(hash[1]<<6)+(hash[1]>>2);
		}
	}
}

// Respect the process affinity mask. Leave capacity for the host and upstream effects.
int SL_AI_ThreadCount(void) {
	DWORD_PTR allowed=0, system=0;
	if(!GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system) || !allowed) return 4;
	int available=0;
	for(; allowed; allowed &= allowed-1) ++available;
	const int automatic=(std::min)(available,(std::min)(8,(std::max)(4,available/3)));
	wchar_t setting[4]={};
	const DWORD length=GetEnvironmentVariableW(L"SANLIGHT_AI_THREADS",setting,4);
	if(length==1 && setting[0]>=L'1' && setting[0]<=L'8')
		return (std::min)(available,int(setting[0]-L'0'));
	return automatic;
}

#ifdef SANLIGHT_EXPERIMENTAL_DML
#include "SL_DMLBackend.inl"
#endif
static void SL_AI_InitLocked(void) {
	if (g_triedInit) return;
	g_triedInit = true;

	/* 本 DLL 所在目录 */
	WCHAR selfPath[MAX_PATH];
	HMODULE hSelf = NULL;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
							GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							(LPCWSTR)&g_triedInit, &hSelf) ||
		!GetModuleFileNameW(hSelf, selfPath, MAX_PATH)) {
		SL_AI_SetErr("cannot locate plugin dir");
		return;
	}
	WCHAR* slash = wcsrchr(selfPath, L'\\');
	if (slash) *slash = 0;

	WCHAR dllPath[MAX_PATH], modelPath[MAX_PATH];
	_snwprintf_s(dllPath, MAX_PATH, _TRUNCATE, L"%s\\SanLight_AI\\onnxruntime.dll", selfPath);
	_snwprintf_s(modelPath, MAX_PATH, _TRUNCATE, L"%s\\SanLight_AI\\depth_anything.onnx", selfPath);
	strcpy_s(g_backendInfo,"CPU");
#ifdef SANLIGHT_EXPERIMENTAL_DML
	try {
		if(sl_dml::initialize(modelPath,SL_AI_ThreadCount()))return;
	} catch(const std::bad_alloc&) {SL_AI_SetErr("GPU setup allocation failed");}
	catch(...) {SL_AI_SetErr("GPU setup unavailable");}
	_snprintf_s(g_backendInfo,sizeof(g_backendInfo),_TRUNCATE,"CPU fallback: %s",g_err);
#endif
	// Keep both files read-locked through loading, so the identity describes
	// the model/runtime actually used rather than a later file replacement.
	struct File {
		HANDLE value;
		~File(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
	};
	File model{CreateFileW(modelPath,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
	File runtime{CreateFileW(dllPath,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
	g_cacheIdentityValid=SL_AI_HashFile(model.value,g_cacheIdentity)&&SL_AI_HashFile(runtime.value,g_cacheIdentity+2);

#ifdef SANLIGHT_EXPERIMENTAL_DML
	g_hOrtDll = LoadLibraryExW(dllPath,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
	if(g_hOrtDll&&!sl_dml::matches(g_hOrtDll,runtime.value)) {
		SL_AI_SetErr("CPU fallback runtime identity mismatch; use CPU build 12");return;
	}
#else
	g_hOrtDll = LoadLibraryW(dllPath);
#endif
	if (!g_hOrtDll) {
		SL_AI_SetErr("onnxruntime.dll not found in SanLight_AI");
		return;
	}

	typedef const OrtApiBase* (__stdcall * OrtGetApiBaseFn)(void);
	OrtGetApiBaseFn getApiBase = (OrtGetApiBaseFn)GetProcAddress(g_hOrtDll, "OrtGetApiBase");
	if (!getApiBase) { SL_AI_SetErr("OrtGetApiBase missing"); return; }

	const OrtApiBase* base = getApiBase();
	g_ort = base->GetApi(ORT_API_VERSION);
	if (!g_ort) { SL_AI_SetErr("GetApi failed (version)"); return; }

	if (g_ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "SanLight", &g_env) != 0) {
		SL_AI_SetErr("CreateEnv failed"); return;
	}
	if (g_ort->CreateSessionOptions(&g_opt) != 0) {
		SL_AI_SetErr("CreateSessionOptions failed"); return;
	}
	const int threads=SL_AI_ThreadCount();
	if(OrtStatus* status=g_ort->SetIntraOpNumThreads(g_opt, threads)) {
		SL_AI_SetErr(g_ort->GetErrorMessage(status));g_ort->ReleaseStatus(status);return;
	}
	// Keep the original four-thread cache identity. Partition other policies so
	// numerical differences on another processor cannot mix two histories.
	if(g_cacheIdentityValid && threads!=4) {
		SL_HistoryKey policy;
		policy.append(g_cacheIdentity+2,2*sizeof(uint64_t));
		const char tag[]="SanLight CPU inference threads";
		policy.append(tag,sizeof(tag));policy.value(threads);
		g_cacheIdentity[2]=policy.a;g_cacheIdentity[3]=policy.b;
	}
	/* 图优化默认全开, 无需设置 */
	if (g_ort->CreateSession(g_env, modelPath, g_opt, &g_sess) != 0) {
		SL_AI_SetErr("CreateSession failed (model missing/broken?)");
		return;
	}
	if (g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &g_mem) != 0) {
		SL_AI_SetErr("CreateCpuMemoryInfo failed"); return;
	}
	if (g_ort->GetAllocatorWithDefaultOptions(&g_alloc) != 0) {
		SL_AI_SetErr("GetAllocatorWithDefaultOptions failed"); return;
	}

	/* 运行时读取输入/输出名 (不给死名字留坑) */
	char* iname = NULL, * oname = NULL;
	if (g_ort->SessionGetInputName(g_sess, 0, g_alloc, &iname) != 0 || !iname) {
		SL_AI_SetErr("SessionGetInputName failed"); return;
	}
	if (g_ort->SessionGetOutputName(g_sess, 0, g_alloc, &oname) != 0 || !oname) {
		g_ort->AllocatorFree(g_alloc, iname);
		SL_AI_SetErr("SessionGetOutputName failed"); return;
	}
	strncpy_s(g_inputName, sizeof(g_inputName), iname, _TRUNCATE);
	strncpy_s(g_outputName, sizeof(g_outputName), oname, _TRUNCATE);
	g_ort->AllocatorFree(g_alloc, iname);
	g_ort->AllocatorFree(g_alloc, oname);

	g_ok = true;
	SL_AI_SetErr("ok");
}

void SL_AI_Init(void) {
	std::lock_guard<std::mutex> guard(g_lock);
	SL_AI_InitLocked();
}

int SL_AI_Available(void) {
	std::lock_guard<std::mutex> guard(g_lock);
	if (!g_triedInit) SL_AI_InitLocked();
	return g_ok ? 1 : 0;
}

int SL_AI_CacheIdentity(uint64_t outIdentity[4]) {
	if(!outIdentity)return 0;
	std::lock_guard<std::mutex> guard(g_lock);
	if(!g_triedInit)SL_AI_InitLocked();
	if(!g_ok||!g_cacheIdentityValid)return 0;
	memcpy(outIdentity,g_cacheIdentity,sizeof(g_cacheIdentity));return 1;
}

/* ---------------------------------------------------------------- */
/* inference                                                        */

static SL_HistoryKey SL_AI_RawKey(const float* input,int q,const uint64_t identity[4]) {
	SL_HistoryKey key;
	const char domain[]="SanLight raw model output";
	const uint32_t schema=1;
	key.append(domain,sizeof(domain));key.value(schema);key.append(identity,4*sizeof(uint64_t));
	key.value(q);key.append(input,size_t(q)*q*3*sizeof(float));return key;
}

static SL_DiskCache* SL_AI_RawDisk() noexcept {
	try {
		wchar_t setting[32768]={};
		DWORD n=GetEnvironmentVariableW(L"SANLIGHT_DISABLE_DISK_CACHE",setting,32768);
		if(n&&n<32768&&setting[0]==L'1')return nullptr;
		static std::unique_ptr<SL_DiskCache> cache=[]() -> std::unique_ptr<SL_DiskCache> {
			wchar_t value[32768]={};DWORD count=GetEnvironmentVariableW(L"SANLIGHT_CACHE_DIR",value,32768);
			std::wstring root;
			auto directory=[](const std::wstring& path) {
				if(!CreateDirectoryW(path.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return false;
				DWORD flags=GetFileAttributesW(path.c_str());
				return flags!=INVALID_FILE_ATTRIBUTES&&(flags&FILE_ATTRIBUTE_DIRECTORY);
			};
			if(count&&count<32768) {
				root.assign(value,count);if(!directory(root))return {};
			} else {
				count=GetEnvironmentVariableW(L"LOCALAPPDATA",value,32768);
				if(!count||count>=32768)return {};
				root.assign(value,count);root+=L"\\SanLight";if(!directory(root))return {};
				root+=L"\\Cache";if(!directory(root))return {};
			}
			root+=L"\\raw-model-v1";
			return std::unique_ptr<SL_DiskCache>(new SL_DiskCache(root,512ULL*1024*1024,700ULL*700*4));
		}();
		return cache.get();
	} catch(...) {return nullptr;}
}

static int SL_AI_GenerateImpl(const float* rgba,int w,int h,int quality,int smooth,float normStrength,
	float** outNear,float** outNorm,int persistentCache,int* inferenceRan) {
	if(inferenceRan)*inferenceRan=0;
	if (!rgba || w <= 0 || h <= 0 || !outNear || !outNorm) return 1;
	*outNear = NULL;
	*outNorm = NULL;
	if (quality < 0 || quality > 3) quality = 2;
	if (smooth < 0) smooth = 0; if (smooth > 10) smooth = 10;
	if (normStrength < 1.f) normStrength = 1.f;
	if (normStrength > 1000.f) normStrength = 1000.f;

	std::lock_guard<std::mutex> guard(g_lock);
	if (!g_triedInit) SL_AI_InitLocked();
	if (!g_ok) {
		SL_AI_SetErr("AI unavailable (init failed)");
		return 2;
	}

	/* 缓存命中? (哈希 + 派生参数完全一致) */
	const int q = QUALITY_SIZES[quality];
	const size_t nPix = (size_t)w * h;

	// The normalized tensor is determined by RGBA, dimensions and quality.
	// A derived-map hit can return before allocating or resampling that tensor.
	const SL_ImageDigest hash = SL_ImageDigest::compute(rgba, nPix*4*sizeof(float));
	if (hash == g_cacheHash && quality == g_cacheQuality &&
		smooth == g_cacheSmooth && normStrength == g_cacheStrength &&
		w == g_cacheW && h == g_cacheH && g_cacheNear && g_cacheNorm) {
		float* nearCopy = (float*)malloc(nPix * sizeof(float));
		float* normCopy = (float*)malloc(nPix * 3 * sizeof(float));
		if (!nearCopy || !normCopy) { free(nearCopy); free(normCopy); return 8; }
		memcpy(nearCopy, g_cacheNear, nPix * sizeof(float));
		memcpy(normCopy, g_cacheNorm, nPix * 3 * sizeof(float));
		*outNear = nearCopy;
		*outNorm = normCopy;
		return 0;
	}

	using Buffer=std::unique_ptr<float,decltype(&free)>;
	Buffer ownedInput((float*)malloc((size_t)q*q*3*sizeof(float)),free);
	Buffer ownedDepthQ((float*)malloc((size_t)q*q*sizeof(float)),free);
	float* input=ownedInput.get();float* depthQ=ownedDepthQ.get();
	if (!input || !depthQ) return 3;
	SL_DownscaleNormCHW(rgba, w, h, q, q, input);

	const bool rawCaching=persistentCache!=0&&g_cacheIdentityValid;
	SL_HistoryKey rawKey;
	SL_DiskCache* rawDisk=nullptr;
	const float* outData=nullptr;
	if(rawCaching) {
		rawKey=SL_AI_RawKey(input,q,g_cacheIdentity);rawDisk=SL_AI_RawDisk();
		if(g_rawCacheValid&&!(rawKey<g_rawCacheKey)&&!(g_rawCacheKey<rawKey)&&g_rawCache.size()==size_t(q)*q)
			outData=g_rawCache.data();
		else if(rawDisk) {
			std::vector<float> restored;
			if(rawDisk->get(rawKey,restored)&&restored.size()==size_t(q)*q) {
				g_rawCache.swap(restored);g_rawCacheKey=rawKey;g_rawCacheValid=true;outData=g_rawCache.data();
			}
		}
	}
	struct Tensor {
		OrtValue* value=nullptr;
		~Tensor(){if(value)g_ort->ReleaseValue(value);}
	} inT,outT;
	if(!outData) {
		int64_t inDims[4]={1,3,(int64_t)q,(int64_t)q};
		OrtStatus* st=g_ort->CreateTensorWithDataAsOrtValue(g_mem,input,
			3*size_t(q)*q*sizeof(float),inDims,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&inT.value);
		if(st) {
			g_ort->ReleaseStatus(st);SL_AI_SetErr("CreateTensorWithDataAsOrtValue failed");return 4;
		}
		const char* inNames[1]={g_inputName};const char* outNames[1]={g_outputName};
#if defined(SANLIGHT_EXPERIMENTAL_DML) && defined(SL_GPU_TESTING)
		if(g_usingGPU&&sl_dml::testFault(L"run"))outNames[0]="__injected_unknown_output__";
#endif
		if(inferenceRan)*inferenceRan=1;
		st=g_ort->Run(g_sess,nullptr,inNames,(const OrtValue* const*)&inT.value,1,outNames,1,&outT.value);
		g_ort->ReleaseValue(inT.value);inT.value=nullptr;
		if(st||!outT.value) {
			if(st){
				if(g_usingGPU)_snprintf_s(g_err,sizeof(g_err),_TRUNCATE,"DirectML Run failed; use CPU build 12: %s",g_ort->GetErrorMessage(st));
				else SL_AI_SetErr(g_ort->GetErrorMessage(st));
				g_ort->ReleaseStatus(st);
			}
			else SL_AI_SetErr("Run returned no output");
			return 5;
		}
		OrtTensorTypeAndShapeInfo* shape=nullptr;
		st=g_ort->GetTensorTypeAndShape(outT.value,&shape);
		if(st||!shape){if(st)g_ort->ReleaseStatus(st);SL_AI_SetErr("GetTensorShape failed");return 6;}
		size_t elements=0;ONNXTensorElementDataType type=ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
		st=g_ort->GetTensorShapeElementCount(shape,&elements);
		if(!st)st=g_ort->GetTensorElementType(shape,&type);
		g_ort->ReleaseTensorTypeAndShapeInfo(shape);
		if(st||elements!=size_t(q)*q||type!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
			if(st)g_ort->ReleaseStatus(st);SL_AI_SetErr("Unexpected model depth shape/type");return 6;
		}
		float* modelOutput=nullptr;st=g_ort->GetTensorMutableData(outT.value,(void**)&modelOutput);
		if(st||!modelOutput){if(st)g_ort->ReleaseStatus(st);SL_AI_SetErr("GetTensorData failed");return 6;}
		outData=modelOutput;
		if(rawCaching) {
			try {
				g_rawCacheValid=false;g_rawCache.assign(modelOutput,modelOutput+size_t(q)*q);
				g_rawCacheKey=rawKey;g_rawCacheValid=true;
				if(rawDisk)rawDisk->put(rawKey,g_rawCache);
			} catch(const std::bad_alloc&) {} // A cache failure must not change the generated surface.
		}
	}

	/* Relative inverse depth: larger model values are nearer. */
	float dmin = 1e30f, dmax = -1e30f;
	for (int i = 0; i < q * q; ++i) {
		const int ax = (int)(((i%q)+.5f)*w/q), ay = (int)(((i/q)+.5f)*h/q);
		if (rgba[((size_t)ay*w+ax)*4+3] < .5f) continue;
		const float v = outData[i];
		if (v < dmin) dmin = v;
		if (v > dmax) dmax = v;
	}
	if (dmin > dmax) { dmin = 0.f; dmax = 1.f; }
	const float drange = (dmax - dmin > 1e-6f) ? (dmax - dmin) : 1.f;
	for (int i = 0; i < q * q; ++i) {
		depthQ[i] = fmaxf(0.f, fminf(1.f, (outData[i] - dmin) / drange));
	}
	if(outT.value){g_ort->ReleaseValue(outT.value);outT.value=nullptr;}
	ownedInput.reset();

	/* 3. 上采样到层尺寸 + 平滑 */
	Buffer ownedNear((float*)malloc(nPix * sizeof(float)), free);
	Buffer ownedNorm((float*)malloc(nPix * 3 * sizeof(float)), free);
	float* nearMap = ownedNear.get();
	float* norm = ownedNorm.get();
	if (!nearMap || !norm) return 7;
	SL_Upsample(depthQ, q, q, nearMap, w, h);
	ownedDepthQ.reset();
	SL_GaussBlur(nearMap, w, h, smooth, rgba, norm);

	/* 4. 梯度法线 */
	SL_NormalsFromNearness(nearMap, w, h, normStrength, norm, rgba);

	/* 5. 替换缓存 */
	float* nearCopy = (float*)malloc(nPix * sizeof(float));
	float* normCopy = (float*)malloc(nPix * 3 * sizeof(float));
	if (!nearCopy || !normCopy) {
		free(nearCopy); free(normCopy);
		return 8;
	}
	memcpy(nearCopy, nearMap, nPix * sizeof(float));
	memcpy(normCopy, norm, nPix * 3 * sizeof(float));
	free(g_cacheNear); free(g_cacheNorm);
	g_cacheNear = ownedNear.release(); g_cacheNorm = ownedNorm.release();
	g_cacheHash = hash; g_cacheQuality = quality; g_cacheSmooth = smooth;
	g_cacheStrength = normStrength; g_cacheW = w; g_cacheH = h;

	*outNear = nearCopy;
	*outNorm = normCopy;
	return 0;
}

int SL_AI_GenerateCached(const float* rgba,int w,int h,int quality,int smooth,float normStrength,
	float** outNear,float** outNorm,int persistentCache,int* inferenceRan) {
	if(inferenceRan)*inferenceRan=0;
	try {return SL_AI_GenerateImpl(rgba,w,h,quality,smooth,normStrength,outNear,outNorm,persistentCache,inferenceRan);}
	catch(...) {
		std::lock_guard<std::mutex> guard(g_lock);SL_AI_SetErr("AI buffer allocation failed");return 3;
	}
}

int SL_AI_Generate(const float* rgba,int w,int h,int quality,int smooth,float normStrength,
	float** outNear,float** outNorm) {
	return SL_AI_GenerateCached(rgba,w,h,quality,smooth,normStrength,outNear,outNorm,0,nullptr);
}
