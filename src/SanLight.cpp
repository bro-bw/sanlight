/*******************************************************************/
/* SanLight (三渲二光源) v1.0 — see SanLight.h for the overview     */
/*******************************************************************/

#include "SanLight.h"
#include "SL_AI.h"
#include "SL_Temporal.h"
#include "SL_Surface.h"
#include "SL_MotionJob.h"
#include "SL_History.h"
#include "SL_Checkpoint.h"
#include "SL_Sequence.h"
#include "SL_Performance.h"
#include "SL_CacheRuntime.h"
#include "SL_PrefixMemo.h"
#include <atomic>
#include <memory>
#include <vector>
#include <limits.h>
#include <xmmintrin.h>

static const A_long SL_HISTORY_CHECKOUT = 1301;
static SL_HistoryCache SL_historyCache;
static SL_CheckpointCache SL_checkpointCache;
static SL_PrefixMemo SL_prefixMemo;

// A scheduling hint only. Surface keys and all input bytes still determine reuse.
// Reserve the target at preparation time so overlapping or reversed jobs do not
// assume their common prefix is new. Use the old recovery plan across restarts
// when an existing surface directory may already contain a later checkpoint.
static A_long SL_FirstNewHistoryIndex(A_long target, SL_DiskCache* disk) {
	static std::atomic<A_long> highestPrepared{-1};
	A_long previous = highestPrepared.load(std::memory_order_relaxed);
	while (previous < target && !highestPrepared.compare_exchange_weak(
		previous, target, std::memory_order_relaxed)) {}
	if (disk) {
		static const bool previousSessionMayHaveSurfaces = [disk] {
			try {
				if (disk->root().empty()) return true;
				WIN32_FIND_DATAW entry{};
				HANDLE search = FindFirstFileW((disk->root() + L"\\*.sldc").c_str(), &entry);
				if (search != INVALID_HANDLE_VALUE) { FindClose(search); return true; }
				const DWORD error = GetLastError();
				return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
			} catch (const std::bad_alloc&) { return true; }
		}();
		if (previousSessionMayHaveSurfaces) return target + 1;
	}
	return (std::min)(target, previous) + 1;
}

static bool SL_ReadInputState(PF_InData* in,PF_State* state) {
    if(!in->pica_basicP)return false;
    const PF_ParamUtilsSuite3* suite=nullptr;
    if(in->pica_basicP->AcquireSuite(kPFParamUtilsSuite,kPFParamUtilsSuiteVersion3,(const void**)&suite)||!suite)return false;
    // Lighting edits do not change the surface; AI/stability settings are keyed separately.
    bool ok=suite->PF_GetCurrentState && !suite->PF_GetCurrentState(in->effect_ref,
        SL_INPUT,nullptr,nullptr,state);
    in->pica_basicP->ReleaseSuite(kPFParamUtilsSuite,kPFParamUtilsSuiteVersion3);return ok;
}

#include <math.h>
#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- */
/* math helpers                                                     */

static inline float SL_Clamp01(float v) {
	if (v < 0.f) return 0.f;
	if (v > 1.f) return 1.f;
	return v;
}

static inline float SL_Smoothstep(float e0, float e1, float x) {
	float t = (x - e0) / (e1 - e0);
	if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
	return t * t * (3.f - 2.f * t);
}

static inline float SL_Luma(float r, float g, float b) {
	return 0.299f * r + 0.587f * g + 0.114f * b;
}

/* AE Levels (RGB 通道) 语义: 输入黑/输入白 -> 伽马 -> 输出黑/输出白 */
static inline float SL_Levels(float v, float inB, float inW, float gamma,
							  float outB, float outW) {
	float range = inW - inB;
	if (range < 0.004f) range = 0.004f;			/* ~1/255 下限保护 */
	float t = (v - inB) / range;
	t = SL_Clamp01(t);
	if (gamma > 0.0001f && fabsf(gamma - 1.f) > 0.0001f)
		t = powf(t, 1.f / gamma);
	return outB + (outW - outB) * t;
}

/* ---------------------------------------------------------------- */
/* SmartFX contract: the result rect must never exceed the request   */

static void SL_IntersectLRect(const PF_LRect* a, const PF_LRect* b, PF_LRect* out) {
	PF_LRect r;
	r.left   = (a->left   > b->left)   ? a->left   : b->left;
	r.top    = (a->top    > b->top)    ? a->top    : b->top;
	r.right  = (a->right  < b->right)  ? a->right  : b->right;
	r.bottom = (a->bottom < b->bottom) ? a->bottom : b->bottom;
	if (r.right <= r.left || r.bottom <= r.top) {
		r.left = r.top = r.right = r.bottom = 0;
	}
	*out = r;
}

/* ---------------------------------------------------------------- */
/* world access                                                      */

typedef struct {
	int				fmt;			/* 8 / 16 / 32 */
	A_long			w, h, row;		/* row = pixels per line */
	const A_u_char*	p8;
	const A_u_short* p16;
	const float*	p32;
	/* AI 生成的平面缓冲模式 (SL_AI 输出, 归一化 float) */
	const float*	fdata;			/* 非 NULL 时启用平面模式 */
	int				chans;			/* 1 = 近度(灰度), 3 = 法线 RGB */
} SL_World;

static void SL_FillWorld(SL_World& w, PF_EffectWorld* world, PF_PixelFormat fmt) {
	memset(&w, 0, sizeof(w));
	if (!world || !world->data) return;
	w.w = world->width;
	w.h = world->height;
	switch (fmt) {
		case PF_PixelFormat_ARGB128:
			w.fmt = 32; w.p32 = reinterpret_cast<const float*>(world->data);
			w.row = world->rowbytes / 16;
			break;
		case PF_PixelFormat_ARGB64:
			w.fmt = 16; w.p16 = reinterpret_cast<const A_u_short*>(world->data);
			w.row = world->rowbytes / 8;
			break;
		default:
			w.fmt = 8; w.p8 = reinterpret_cast<const A_u_char*>(world->data);
			w.row = world->rowbytes / 4;
			break;
	}
}

static inline void SL_Fetch(const SL_World& w, int xi, int yi, float rgba[4]) {
	if (xi < 0) xi = 0; else if (xi > w.w - 1) xi = w.w - 1;
	if (yi < 0) yi = 0; else if (yi > w.h - 1) yi = w.h - 1;
	if (w.fdata) {			/* AI 平面缓冲: 行主序 w*h*chans */
		const size_t i = (size_t)yi * w.w + xi;
		if (w.chans == 1) {
			rgba[0] = rgba[1] = rgba[2] = w.fdata[i];
		} else {
			rgba[0] = w.fdata[i * 3];
			rgba[1] = w.fdata[i * 3 + 1];
			rgba[2] = w.fdata[i * 3 + 2];
		}
		rgba[3] = 1.f;
		return;
	}
	const size_t o = ((size_t)yi * w.row + xi) * 4;
	if (w.fmt == 32) {
		const float* p = w.p32 + o;					/* ARGB float */
		rgba[0] = p[1]; rgba[1] = p[2]; rgba[2] = p[3]; rgba[3] = p[0];
	} else if (w.fmt == 16) {
		const A_u_short* p = w.p16 + o;				/* ARGB 15+1 */
		const float k = 1.f / 32768.f;
		rgba[0] = p[1] * k; rgba[1] = p[2] * k; rgba[2] = p[3] * k; rgba[3] = p[0] * k;
	} else {
		const A_u_char* p = w.p8 + o;
		const float k = 1.f / 255.f;
		rgba[0] = p[1] * k; rgba[1] = p[2] * k; rgba[2] = p[3] * k; rgba[3] = p[0] * k;
	}
}

static inline void SL_SampleBilinear(const SL_World& w, float xf, float yf, float rgba[4]) {
	if (w.w <= 0 || w.h <= 0) { rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.f; return; }
	xf -= 0.5f; yf -= 0.5f;
	const float fx = xf - floorf(xf);
	const float fy = yf - floorf(yf);
	const int x0 = (int)floorf(xf), y0 = (int)floorf(yf);
	if (w.fdata) {
		// Compute the four offsets once for all channels of a packed AI map.
		auto clamp = [](int index, int size) { return (std::min)((std::max)(0,index),size-1); };
		const int left = clamp(x0,w.w), right = clamp(x0+1,w.w);
		const size_t topRow = size_t(clamp(y0,w.h))*w.w;
		const size_t bottomRow = size_t(clamp(y0+1,w.h))*w.w;
		const int channels = w.chans == 1 ? 1 : 3;
		const float* tl = w.fdata + (topRow + left) * channels;
		const float* tr = w.fdata + (topRow + right) * channels;
		const float* bl = w.fdata + (bottomRow + left) * channels;
		const float* br = w.fdata + (bottomRow + right) * channels;
		for (int i = 0; i < channels; ++i) {
			const float top = tl[i] + (tr[i] - tl[i]) * fx;
			const float bot = bl[i] + (br[i] - bl[i]) * fx;
			rgba[i] = top + (bot - top) * fy;
		}
		if (channels == 1) rgba[1] = rgba[2] = rgba[0];
		rgba[3] = 1.f;
		return;
	}
	float p00[4], p10[4], p01[4], p11[4];
	SL_Fetch(w, x0,     y0,     p00);
	SL_Fetch(w, x0 + 1, y0,     p10);
	SL_Fetch(w, x0,     y0 + 1, p01);
	SL_Fetch(w, x0 + 1, y0 + 1, p11);
	for (int i = 0; i < 4; ++i) {
		const float top = p00[i] + (p10[i] - p00[i]) * fx;
		const float bot = p01[i] + (p11[i] - p01[i]) * fx;
		rgba[i] = top + (bot - top) * fy;
	}
}

/* 输出像素坐标 -> 贴图层坐标 (同尺寸 1:1, 不同尺寸拉伸对齐) */
static inline void SL_MapCoord(const SL_World& w, A_long mapW, A_long mapH,
							   A_long x, A_long y, float& xf, float& yf) {
	if (w.w == mapW && w.h == mapH) {
		xf = (float)x; yf = (float)y;
	} else if (mapW > 0 && mapH > 0) {
		xf = ((float)x + 0.5f) * (float)w.w / (float)mapW - 0.5f;
		yf = ((float)y + 0.5f) * (float)w.h / (float)mapH - 0.5f;
	} else {
		xf = (float)x; yf = (float)y;
	}
}

// Preserve the existing half-pixel sampling convention. AI maps matching the
// layer size always have fx=fy=0.5 here; skip general coordinate/floor work.
static inline void SL_SampleMap(const SL_World& w,A_long mapW,A_long mapH,
                               A_long x,A_long y,float rgba[4]) {
	if(w.fdata && w.w>0 && w.h>0 && w.w==mapW && w.h==mapH &&
	   x>=-4194303 && x<=4194303 && y>=-4194303 && y<=4194303) {
		auto clamp=[](A_long v,A_long size){return (std::min)((std::max)(A_long(0),v),size-1);};
		const A_long left=clamp(x-1,w.w),right=clamp(x,w.w);
		const size_t top=size_t(clamp(y-1,w.h))*w.w,bottom=size_t(clamp(y,w.h))*w.w;
		if(w.chans==1) {
			const float a=w.fdata[top+left],b=w.fdata[top+right];
			const float c=w.fdata[bottom+left],d=w.fdata[bottom+right];
			const float upper=a+(b-a)*.5f,lower=c+(d-c)*.5f;
			rgba[0]=rgba[1]=rgba[2]=upper+(lower-upper)*.5f;rgba[3]=1.f;
		} else {
			// Read exactly three floats: the final RGB sample has no fourth lane.
			auto rgb=[](const float* p){return _mm_set_ps(1.f,p[2],p[1],p[0]);};
			const __m128 a=rgb(w.fdata+3*(top+left)),b=rgb(w.fdata+3*(top+right));
			const __m128 c=rgb(w.fdata+3*(bottom+left)),d=rgb(w.fdata+3*(bottom+right));
			const __m128 half=_mm_set1_ps(.5f);
			const __m128 upper=_mm_add_ps(a,_mm_mul_ps(_mm_sub_ps(b,a),half));
			const __m128 lower=_mm_add_ps(c,_mm_mul_ps(_mm_sub_ps(d,c),half));
			_mm_storeu_ps(rgba,_mm_add_ps(upper,_mm_mul_ps(_mm_sub_ps(lower,upper),half)));
		}
		return;
	}
	float xf,yf;SL_MapCoord(w,mapW,mapH,x,y,xf,yf);SL_SampleBilinear(w,xf,yf,rgba);
}

/* ---------------------------------------------------------------- */
/* render context (read-only during the pixel loop; thread-safe)     */

typedef struct {
	bool	hasNormal, hasDepth;
	bool	hasLight3D;			/* true: 来自 AE 灯光层 */
	bool	depthInvert;
	bool	normalYUp;			/* 法线图 G 通道朝上 (OpenGL 惯例) */
	bool	followLight;

	float	lightX, lightY;		/* 点光源 (层内像素坐标) */
	float	lightZ;				/* 正值 = 在画面前方 (朝观察者) */
	float	lightR, lightG, lightB;
	float	intensity;			/* 1.0 = 100% */
	float	falloff;			/* 衰减半径 px, <=0 无衰减 */

	float	ambient;			/* 0..1 */
	float	diffuse;			/* 0..4 */
	float	specular;			/* 0..4 */
	float	shininess;			/* 1..256 */
	float	rim;				/* 0..2 */
	float	softness;			/* 0..0.4 明暗过渡宽度 (N·L 域) */
	bool backlightEnabled;
	float backlightStart, backlightDistance;
	float edgeKeep, edgeWidth;
	SL_World sourceWorld;

	float	lvInB, lvInW;		/* 0..1 */
	float	lvGamma;			/* 0.1..4 */
	float	lvOutB, lvOutW;		/* 0..1 */

	float	depthRange;			/* 深度图映射的最大 Z 位移 (px) */

	/* 内置 AI */
	int		aiSource;			/* 0 = 内置AI生成, 1 = 使用贴图层 */
	int		aiQuality;			/* 0..3 */
	int		aiSmooth;			/* 0..10 */
	float	aiStrength;			/* 法线梯度强度 1..1000 */

	int		view;				/* SL_VIEW popup, 1..7 */
	int		apply;				/* SL_APPLY popup, 1..5 */
	float	opacity;			/* 0..1 */

	A_long	mapW, mapH;			/* 输入层尺寸 */

	float	light3D[3];			/* 跟随 AE 灯光时的光源位置 (层内偏移, px) */

	SL_World normalWorld;
	SL_World depthWorld;
	float* aiNearData;
	float* aiNormData;
	bool temporalEnabled, temporalAlignment, temporalRotation;
	float temporalStrength, temporalResponse;
	A_long historyCount;
	A_long firstNewHistoryIndex;
	bool* historyChecked;
	bool inputStateValid;
	PF_State inputState;
	SL_InstanceID instanceID;
	A_long outputOffsetX, outputOffsetY;
	PF_LRect aiCanvas;
	A_long aiMapOffsetX, aiMapOffsetY;
	short pixelDepth;
	std::shared_ptr<const SL_Checkpoint>* pinnedCheckpoint;
	bool persistentCache;
	double preMilliseconds;
	A_long historyDeclarations;
} SL_Ctx;

/* ---------------------------------------------------------------- */
/* per-pixel shading kernel                                          */

static inline void SL_NormalAt(const SL_Ctx& c, A_long x, A_long y, float N[3]) {
	if (c.hasNormal) {
		float s[4];
		SL_SampleMap(c.normalWorld,c.mapW,c.mapH,x+c.aiMapOffsetX,y+c.aiMapOffsetY,s);
		float nx = s[0] * 2.f - 1.f;
		float ny = s[1] * 2.f - 1.f;
		float nz = s[2] * 2.f - 1.f;
		if (c.normalYUp) ny = -ny;		/* G 朝上: 切线空间 +Y(上) = 屏幕 -Y */
		float len = sqrtf(nx * nx + ny * ny + nz * nz);
		if (len > 1e-5f) {
			N[0] = nx / len; N[1] = ny / len; N[2] = nz / len;
			return;
		}
	}
	N[0] = 0.f; N[1] = 0.f; N[2] = 1.f;		/* 无法线层/退化: 平面朝镜头 */
}

static inline float SL_DepthNearnessAt(const SL_Ctx& c, A_long x, A_long y) {
	if (!c.hasDepth) return 0.f;			/* 无深度层: 与镜头同平面 */
	float s[4];
	SL_SampleMap(c.depthWorld,c.mapW,c.mapH,x+c.aiMapOffsetX,y+c.aiMapOffsetY,s);
	float d = SL_Luma(s[0], s[1], s[2]);
	if (c.depthInvert) d = 1.f - d;
	return d;								/* 1 = 离镜头近 */
}

/* 通道预览 (view popup 2..7) 与最终合成 (view==1) 共用同一份光影计算 */
static inline void SL_ShadePixel(const SL_Ctx& c, A_long x, A_long y,
								 const float src[4], float out[4]) {
	if (c.view == 6 || c.view == 7) {
		if (c.view == 7) {
			out[0] = out[1] = out[2] = SL_Clamp01(SL_DepthNearnessAt(c, x, y));
		} else if (c.hasNormal) {
			SL_SampleMap(c.normalWorld,c.mapW,c.mapH,x+c.aiMapOffsetX,y+c.aiMapOffsetY,out);
		} else {
			out[0] = out[1] = .5f;
			out[2] = 1.f;
		}
		out[3] = src[3];
		return;
	}
	float Nrm[3];
	SL_NormalAt(c, x, y, Nrm);

	/* --- 表面三维位置: z 正方向朝观察者 --- */
	/* 近 (白) 贴在画面平面 z≈0, 远 (黑) 向画面后方延伸 depthRange,
	   保证默认光源 Z (正值) 始终位于主体前方 */
	const float nearness = SL_DepthNearnessAt(c, x, y);
	const float z = c.hasDepth ? -c.depthRange * (1.f - nearness) : 0.f;

	/* --- 光源向量 (层内坐标, 像素单位) --- */
	float Lx, Ly, Lz;
	if (c.hasLight3D) {
		Lx = c.light3D[0] - (float)x;
		Ly = c.light3D[1] - (float)y;
		Lz = c.light3D[2] - z;
	} else {
		Lx = c.lightX - (float)x;
		Ly = c.lightY - (float)y;
		Lz = c.lightZ - z;
	}
	const float dist = sqrtf(Lx * Lx + Ly * Ly + Lz * Lz);

	/* --- 距离衰减 (半径 <=0 视为无衰减, 类似平行光) --- */
	float atten = 1.f;
	if (c.falloff > 0.5f && dist > 1e-5f) {
		const float q = dist / c.falloff;
		atten = 1.f / (1.f + q * q);
	}

	/* --- 漫反射: N·L, 明暗边界柔化 --- */
	float ndl = 0.f;
	if (dist > 1e-5f) ndl = (Nrm[0] * Lx + Nrm[1] * Ly + Nrm[2] * Lz) / dist;
	float diff;
	float back = c.backlightEnabled
		? SL_Smoothstep(c.backlightStart, c.backlightStart + c.backlightDistance, -Lz) : 0.f;
	// Preserve existing light at boundaries on the side facing the lamp.
	if (back > 0.f && c.edgeKeep > 0.f) {
        const float lateral = sqrtf(Lx * Lx + Ly * Ly);
        if (lateral > 1e-5f) {
		const float dx = Lx / lateral, dy = Ly / lateral;
		const A_long ex = (A_long)floorf(x + dx * c.edgeWidth + 0.5f);
		const A_long ey = (A_long)floorf(y + dy * c.edgeWidth + 0.5f);
		float edge = 0.f;
		if (c.hasDepth) {
			const float drop = nearness - SL_DepthNearnessAt(c, ex, ey);
			edge = SL_Smoothstep(0.015f, 0.07f, drop);
		}
		if (c.sourceWorld.w > 0 && c.sourceWorld.h > 0) {
			float a = 0.f;
			if (ex >= 0 && ey >= 0 && ex < c.sourceWorld.w && ey < c.sourceWorld.h) {
				float sample[4]; SL_Fetch(c.sourceWorld, ex, ey, sample); a = sample[3];
			}
			const float silhouette = SL_Smoothstep(0.05f, 0.8f, src[3] - a);
			if (silhouette > edge) edge = silhouette;
		}
		const float facing = SL_Smoothstep(0.f, 0.4f, Nrm[0] * dx + Nrm[1] * dy);
		back *= 1.f - c.edgeKeep * edge * facing;
	}
	}
	const float gatedNdl = ndl - back * (1.f + c.softness);
	if (c.softness > 0.0005f) diff = SL_Smoothstep(-c.softness, c.softness, gatedNdl);
	else diff = (gatedNdl > 0.f) ? gatedNdl : 0.f;
	diff *= 1.f - back;

	/* --- 高光: Blinn-Phong, 观察方向 V = +Z (朝镜头) --- */
	float spec = 0.f;
	if (dist > 1e-5f && c.specular > 0.001f) {
		const float ix = Lx / dist, iy = Ly / dist, iz = Lz / dist;
		float hx = ix, hy = iy, hz = iz + 1.f;
		const float hl = sqrtf(hx * hx + hy * hy + hz * hz);
		if (hl > 1e-5f) {
			hx /= hl; hy /= hl; hz /= hl;
			const float ndh = Nrm[0] * hx + Nrm[1] * hy + Nrm[2] * hz;
			if (ndh > 0.f) spec = powf(ndh, c.shininess);
		}
	}

	/* --- 轮廓光 (Rim): N 垂直于视线处最亮 --- */
	// A disabled rim cannot affect shading for a finite unit normal.
    // Keep the original path for nonfinite/out-of-range custom data.
    const float rimv = c.rim == 0.f && fabsf(Nrm[2]) <= 1.f
        ? 0.f : powf(1.f - fabsf(Nrm[2]), 3.f);

	const float kI = c.intensity;
	const float diffTerm = kI * atten * diff * c.diffuse;
	const float specTerm = kI * atten * spec * c.specular * (1.f - back);
	const float rimTerm  = c.rim * rimv * (1.f - back);

	/* --- 预览通道 --- */
	if (c.view >= 2) {
		float v[4] = { 0.f, 0.f, 0.f, src[3] };
		switch (c.view) {
			case 2:		/* 光影图 */
				v[0] = v[1] = v[2] = SL_Clamp01(c.ambient + diffTerm + specTerm + rimTerm);
				break;
			case 3:		/* 漫反射 */
				v[0] = v[1] = v[2] = SL_Clamp01(diffTerm);
				break;
			case 4:
				v[0] = v[1] = v[2] = SL_Clamp01(specTerm);
				break;
			case 5:
				v[0] = v[1] = v[2] = SL_Clamp01(rimTerm);
				break;
		}
		out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; out[3] = src[3];
		return;
	}

	/* --- 着色: 环境光 + 灯光颜色 * (漫反射 + 高光 + 轮廓光) --- */
	float shade[3];
	shade[0] = c.ambient + c.lightR * (diffTerm + specTerm) + c.lightR * rimTerm;
	shade[1] = c.ambient + c.lightG * (diffTerm + specTerm) + c.lightG * rimTerm;
	shade[2] = c.ambient + c.lightB * (diffTerm + specTerm) + c.lightB * rimTerm;

	/* --- 内置色阶 (AE Levels 语义) --- */
	for (int i = 0; i < 3; ++i) {
		shade[i] = SL_Levels(shade[i], c.lvInB, c.lvInW, c.lvGamma, c.lvOutB, c.lvOutW);
		shade[i] = SL_Clamp01(shade[i]);
	}

	/* --- 应用到原图 --- */
	float r = src[0], g = src[1], b = src[2];
	switch (c.apply) {
		case 2:		/* 正片叠底 */
			r = src[0] * shade[0];
			g = src[1] * shade[1];
			b = src[2] * shade[2];
			break;
		case 3: {	/* 叠加 */
			for (int i = 0; i < 3; ++i) {
				const float a2 = (i == 0) ? src[0] : (i == 1) ? src[1] : src[2];
				const float s2 = shade[i];
				const float o = (a2 < 0.5f) ? (2.f * a2 * s2)
											: (1.f - 2.f * (1.f - a2) * (1.f - s2));
				if (i == 0) r = o; else if (i == 1) g = o; else b = o;
			}
			break;
		}
		case 4: {	/* 柔光 */
			for (int i = 0; i < 3; ++i) {
				const float a2 = (i == 0) ? src[0] : (i == 1) ? src[1] : src[2];
				const float s2 = shade[i];
				float o;
				if (s2 <= 0.5f) o = a2 - (1.f - 2.f * s2) * a2 * (1.f - a2);
				else			o = a2 + (2.f * s2 - 1.f) * (sqrtf(a2) - a2);
				if (i == 0) r = o; else if (i == 1) g = o; else b = o;
			}
			break;
		}
		case 5: {	/* 相加 (只加光, 环境光不重复叠加) */
			r = src[0] + (shade[0] > c.ambient ? shade[0] - c.ambient : 0.f);
			g = src[1] + (shade[1] > c.ambient ? shade[1] - c.ambient : 0.f);
			b = src[2] + (shade[2] > c.ambient ? shade[2] - c.ambient : 0.f);
			break;
		}
		case 1:		/* 仅光影 */
		default:
			r = shade[0]; g = shade[1]; b = shade[2];
			break;
	}

	/* 不透明度: 向原图回混 */
	if (c.opacity < 0.9999f) {
		r = src[0] + (r - src[0]) * c.opacity;
		g = src[1] + (g - src[1]) * c.opacity;
		b = src[2] + (b - src[2]) * c.opacity;
	}

	out[0] = r; out[1] = g; out[2] = b; out[3] = src[3];
}

/* pixel-type adapters (AE 内存布局: A,R,G,B) */

static inline void SL_GetSrc(const PF_Pixel* p, float s[4]) {
	const float k = 1.f / 255.f;
	s[0] = p->red * k; s[1] = p->green * k; s[2] = p->blue * k; s[3] = p->alpha * k;
}
static inline void SL_GetSrc(const PF_Pixel16* p, float s[4]) {
	const float k = 1.f / 32768.f;
	s[0] = p->red * k; s[1] = p->green * k; s[2] = p->blue * k; s[3] = p->alpha * k;
}
static inline void SL_GetSrc(const PF_PixelFloat* p, float s[4]) {
	const float* f = reinterpret_cast<const float*>(p);
	s[0] = f[1]; s[1] = f[2]; s[2] = f[3]; s[3] = f[0];
}
static inline void SL_PutDst(PF_Pixel* p, const float v[4]) {
	p->red   = (A_u_char)(SL_Clamp01(v[0]) * 255.f + 0.5f);
	p->green = (A_u_char)(SL_Clamp01(v[1]) * 255.f + 0.5f);
	p->blue  = (A_u_char)(SL_Clamp01(v[2]) * 255.f + 0.5f);
	p->alpha = (A_u_char)(SL_Clamp01(v[3]) * 255.f + 0.5f);
}
static inline void SL_PutDst(PF_Pixel16* p, const float v[4]) {
	p->red   = (A_u_short)(SL_Clamp01(v[0]) * 32768.f + 0.5f);
	p->green = (A_u_short)(SL_Clamp01(v[1]) * 32768.f + 0.5f);
	p->blue  = (A_u_short)(SL_Clamp01(v[2]) * 32768.f + 0.5f);
	p->alpha = (A_u_short)(SL_Clamp01(v[3]) * 32768.f + 0.5f);
}
static inline void SL_PutDst(PF_PixelFloat* p, const float v[4]) {
	float* f = reinterpret_cast<float*>(p);
	f[1] = SL_Clamp01(v[0]); f[2] = SL_Clamp01(v[1]);
	f[3] = SL_Clamp01(v[2]); f[0] = SL_Clamp01(v[3]);
}

static PF_Err SL_PixelFunc8(void* refcon, A_long x, A_long y, PF_Pixel* inP, PF_Pixel* outP) {
	const SL_Ctx* c = reinterpret_cast<const SL_Ctx*>(refcon);
	float src[4], o[4];
	x+=c->outputOffsetX;y+=c->outputOffsetY;
	if(x<0||y<0||x>=c->sourceWorld.w||y>=c->sourceWorld.h) memset(src,0,sizeof(src));
	else SL_Fetch(c->sourceWorld,x,y,src);
	SL_ShadePixel(*c, x, y, src, o);
	SL_PutDst(outP, o);
	return PF_Err_NONE;
}
static PF_Err SL_PixelFunc16(void* refcon, A_long x, A_long y, PF_Pixel16* inP, PF_Pixel16* outP) {
	const SL_Ctx* c = reinterpret_cast<const SL_Ctx*>(refcon);
	float src[4], o[4];
	x+=c->outputOffsetX;y+=c->outputOffsetY;
	if(x<0||y<0||x>=c->sourceWorld.w||y>=c->sourceWorld.h) memset(src,0,sizeof(src));
	else SL_Fetch(c->sourceWorld,x,y,src);
	SL_ShadePixel(*c, x, y, src, o);
	SL_PutDst(outP, o);
	return PF_Err_NONE;
}
static PF_Err SL_PixelFunc32(void* refcon, A_long x, A_long y, PF_PixelFloat* inP, PF_PixelFloat* outP) {
	const SL_Ctx* c = reinterpret_cast<const SL_Ctx*>(refcon);
	float src[4], o[4];
	x+=c->outputOffsetX;y+=c->outputOffsetY;
	if(x<0||y<0||x>=c->sourceWorld.w||y>=c->sourceWorld.h) memset(src,0,sizeof(src));
	else SL_Fetch(c->sourceWorld,x,y,src);
	SL_ShadePixel(*c, x, y, src, o);
	SL_PutDst(outP, o);
	return PF_Err_NONE;
}

/* ---------------------------------------------------------------- */

static PF_Err
About(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output) {
	(void)in_data; (void)params; (void)output;
#ifdef SANLIGHT_EXPERIMENTAL_DML
	_snprintf_s(out_data->return_msg,sizeof(out_data->return_msg),_TRUNCATE,
		"SanLight v%d.%d build %d GPU 试用版\rAI 运行状态：%s",
		SL_MAJOR_VERSION,SL_MINOR_VERSION,SL_BUILD_VERSION,SL_AI_BackendInfo());
#else
	PF_SPRINTF(out_data->return_msg, "%s v%d.%d build %d\r%s",
			   "SanLight", SL_MAJOR_VERSION, SL_MINOR_VERSION, SL_BUILD_VERSION, SL_DESCRIPTION);
#endif
	return PF_Err_NONE;
}

static PF_Err
GlobalSetup(PF_InData* in_dataP, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output) {
	(void)in_dataP; (void)params; (void)output;
	out_data->my_version = PF_VERSION(SL_MAJOR_VERSION, SL_MINOR_VERSION, SL_BUG_VERSION,
									  SL_STAGE_VERSION, SL_BUILD_VERSION);
	out_data->out_flags  = PF_OutFlag_WIDE_TIME_INPUT | PF_OutFlag_NON_PARAM_VARY |
		PF_OutFlag_SEND_UPDATE_PARAMS_UI | PF_OutFlag_PIX_INDEPENDENT |
						   PF_OutFlag_DEEP_COLOR_AWARE;
	out_data->out_flags2 = PF_OutFlag2_FLOAT_COLOR_AWARE |
						   PF_OutFlag2_SUPPORTS_GET_FLATTENED_SEQUENCE_DATA |
						   PF_OutFlag2_AUTOMATIC_WIDE_TIME_INPUT |
						   PF_OutFlag2_SUPPORTS_SMART_RENDER;
	// Causal surfaces are sequential. MFR duplicated prefix replay and its memory;
	// row-level parallelism inside each frame remains enabled.
	return PF_Err_NONE;
}

static PF_Err
ParamsSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output) {
	(void)params; (void)output;
	PF_Err		err = PF_Err_NONE;
	PF_ParamDef	def;
	const A_long w = (in_data->width  > 0) ? in_data->width  : 1000;
	const A_long h = (in_data->height > 0) ? in_data->height : 1000;

	/* ================= 贴图 ================= */
	PF_ADD_TOPICX("贴图", 0, SL_DISK_GRP_MAPS);
	PF_ADD_POPUPX("贴图来源", 2, 1, "内置AI自动生成|使用自定义贴图", PF_ParamFlag_SUPERVISE, SL_DISK_AI_SOURCE);
	PF_ADD_POPUPX("AI 质量", 4, 4, "快速(280px)|标准(392px)|高(518px)|精细(700px)", 0, SL_DISK_AI_QUALITY);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("AI 平滑", 0, 10, 0, 10, 10, 0,
						 PF_ValueDisplayFlag_NONE, 0, SL_DISK_AI_SMOOTH);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("AI 法线强度", 1, 1000, 1, 1000, 1000, 0,
						 PF_ValueDisplayFlag_NONE, 0, SL_DISK_AI_STRENGTH);

	AEFX_CLR_STRUCT(def);
	PF_ADD_LAYER("法线贴图层", PF_LayerDefault_NONE, SL_DISK_NORMAL_LAYER);
	AEFX_CLR_STRUCT(def);
	PF_ADD_LAYER("深度贴图层", PF_LayerDefault_NONE, SL_DISK_DEPTH_LAYER);
	PF_ADD_CHECKBOXX("深度反转 (白=远)", FALSE, 0, SL_DISK_DEPTH_INVERT);
	PF_ADD_CHECKBOXX("法线G通道朝上", TRUE, 0, SL_DISK_NORMAL_Y_UP);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("深度范围 (Z 位移)", 0, 5000, 0, 2000, 1000, 0,
						 PF_ValueDisplayFlag_NONE, 0, SL_DISK_DEPTH_RANGE);
	PF_END_TOPIC(SL_DISK_END_MAPS);

	/* ================= 点光源 ================= */
	PF_ADD_TOPICX("点光源", 0, SL_DISK_GRP_LIGHT);
	PF_ADD_POPUPX("光源模式", 2, 1, "自定义点光源|跟随AE灯光层", 0, SL_DISK_LIGHT_MODE);
	AEFX_CLR_STRUCT(def);
	PF_ADD_POINT("光源位置 (可拖拽)", w * 45 / 100, h * 30 / 100, FALSE, SL_DISK_LIGHT_POS);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("光源Z (前为正)", -5000, 5000, -2000, 2000, 700, 0,
						 PF_ValueDisplayFlag_NONE, 0, SL_DISK_LIGHT_Z);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("光源强度 %", 0, 1000, 0, 400, 120, 0,
						 PF_ValueDisplayFlag_NONE, 0, SL_DISK_LIGHT_INTENSITY);
	AEFX_CLR_STRUCT(def);
	PF_ADD_COLOR("灯光颜色", 255, 240, 220, SL_DISK_LIGHT_COLOR);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("衰减半径 (0=不衰减)", 0, 20000, 0, 5000, 2500, 0,
						 PF_ValueDisplayFlag_NONE, 0, SL_DISK_LIGHT_FALLOFF);
	PF_ADD_TOPICX("背光抑制", 0, SL_DISK_GRP_BACKLIGHT);
	PF_ADD_CHECKBOXX("启用背光抑制", TRUE, 0, SL_DISK_BACKLIGHT_ENABLED);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("开始距离", 0, 10000, 0, 2000, 200, 0,
		PF_ValueDisplayFlag_NONE, 0, SL_DISK_BACKLIGHT_START);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("过渡距离", 1, 10000, 1, 3000, 800, 0,
		PF_ValueDisplayFlag_NONE, 0, SL_DISK_BACKLIGHT_DISTANCE);
	PF_END_TOPIC(SL_DISK_END_BACKLIGHT);
	PF_ADD_TOPICX("背光边缘", 0, SL_DISK_GRP_EDGE);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("边缘保留 %", 0, 100, 0, 100, 100, 0,
		PF_ValueDisplayFlag_NONE, 0, SL_DISK_EDGE_KEEP);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("边缘宽度", 1, 40, 1, 20, 6, 0,
		PF_ValueDisplayFlag_NONE, 0, SL_DISK_EDGE_WIDTH);
	PF_END_TOPIC(SL_DISK_END_EDGE);
	PF_END_TOPIC(SL_DISK_END_LIGHT);

	/* ================= 明暗 ================= */
	PF_ADD_TOPICX("明暗", 0, SL_DISK_GRP_SHADING);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("环境光 %", 0, 100, 0, 100, 30, 0,
						 PF_ValueDisplayFlag_NONE, 0, SL_DISK_AMBIENT);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("漫反射 %", 0, 400, 0, 200, 100, 0,
						 PF_ValueDisplayFlag_NONE, 0, SL_DISK_DIFFUSE);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("高光 %", 0, 400, 0, 200, 30, 0,
						 PF_ValueDisplayFlag_NONE, 0, SL_DISK_SPECULAR);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("高光锐利度", 1, 256, 1, 128, 24, 0,
						 PF_ValueDisplayFlag_NONE, 0, SL_DISK_SHININESS);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("轮廓光 %", 0, 200, 0, 100, 0, 0,
						 PF_ValueDisplayFlag_NONE, 0, SL_DISK_RIM);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("明暗过渡", 0, 100, 0, 100, 8, 0,
						 PF_ValueDisplayFlag_NONE, 0, SL_DISK_SOFTNESS);
	PF_END_TOPIC(SL_DISK_END_SHADING);

	/* ================= 色阶 ================= */
	PF_ADD_TOPICX("色阶 (AE Levels 语义)", 0, SL_DISK_GRP_LEVELS);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("输入黑", 0, 254, 0, 254, 0, 0,
						 PF_ValueDisplayFlag_NONE, 0, SL_DISK_LV_IN_BLACK);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("伽马 (0.10-4.00)", 10, 400, 10, 400, 100, 0,
						 PF_ValueDisplayFlag_NONE, 0, SL_DISK_LV_GAMMA);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("输入白", 1, 255, 1, 255, 255, 0,
						 PF_ValueDisplayFlag_NONE, 0, SL_DISK_LV_IN_WHITE);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("输出黑", 0, 255, 0, 255, 0, 0,
						 PF_ValueDisplayFlag_NONE, 0, SL_DISK_LV_OUT_BLACK);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("输出白", 0, 255, 0, 255, 255, 0,
						 PF_ValueDisplayFlag_NONE, 0, SL_DISK_LV_OUT_WHITE);
	PF_END_TOPIC(SL_DISK_END_LEVELS);

	/* ================= 输出 ================= */
	PF_ADD_TOPICX("输出", 0, SL_DISK_GRP_OUTPUT);
	PF_ADD_POPUPX("通道预览", 7, 1,
				  "最终结果|光影图|漫反射|高光|轮廓光|法线|深度", 0, SL_DISK_VIEW);
	PF_ADD_POPUPX("应用模式", 5, 2,
				  "仅光影|正片叠底|叠加|柔光|相加", 0, SL_DISK_APPLY);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("不透明度 %", 0, 100, 0, 100, 100, 0,
						 PF_ValueDisplayFlag_NONE, 0, SL_DISK_OPACITY);
	PF_END_TOPIC(SL_DISK_END_OUTPUT);
	PF_ADD_TOPICX("动画稳定 v1.3",0,SL_DISK_GRP_TEMPORAL);
	PF_ADD_CHECKBOXX("启用相邻帧稳定",TRUE,PF_ParamFlag_SUPERVISE,SL_DISK_TEMPORAL_ENABLED);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("稳定强度 %",0,100,0,100,85,0,
		PF_ValueDisplayFlag_NONE,0,SL_DISK_TEMPORAL_STRENGTH);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("动作响应 %",0,100,0,100,20,0,
		PF_ValueDisplayFlag_NONE,0,SL_DISK_TEMPORAL_RESPONSE);
	PF_ADD_CHECKBOXX("相邻帧运动对齐",TRUE,0,SL_DISK_TEMPORAL_ALIGNMENT);
	PF_ADD_CHECKBOXX("转动跟随（试验）",FALSE,0,SL_DISK_TEMPORAL_ROTATION);
	PF_ADD_CHECKBOXX("使用磁盘缓存",TRUE,0,SL_DISK_PERSISTENT_CACHE);
	PF_END_TOPIC(SL_DISK_END_TEMPORAL);

	out_data->num_params = SL_NUM_PARAMS;
	return err;
}

/* ---------------------------------------------------------------- */
/* 参数读取 (PreRender 阶段)                                         */

static PF_Err SL_UpdateMapUI(PF_InData* in_data, PF_ParamDef* params[]) {
	AEGP_SuiteHandler suites(in_data->pica_basicP);
	AEGP_EffectRefH effect = NULL;
	PF_Err err = suites.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(0, in_data->effect_ref, &effect);
	if (err || !effect) return err ? err : PF_Err_BAD_CALLBACK_PARAM;
	const bool custom = params[SL_AI_SOURCE]->u.pd.value == 2;
	const A_long indices[] = { SL_NORMAL_LAYER, SL_DEPTH_LAYER, SL_DEPTH_INVERT,
		SL_NORMAL_Y_UP, SL_AI_QUALITY, SL_AI_SMOOTH, SL_AI_STRENGTH,
		SL_TEMPORAL_ENABLED,SL_TEMPORAL_STRENGTH,SL_TEMPORAL_RESPONSE,SL_TEMPORAL_ALIGNMENT,SL_TEMPORAL_ROTATION,SL_PERSISTENT_CACHE };
	for (int i = 0; i < int(sizeof(indices)/sizeof(indices[0])) && !err; ++i) {
		AEGP_StreamRefH stream = NULL;
		err = suites.StreamSuite5()->AEGP_GetNewEffectStreamByIndex(0, effect, indices[i], &stream);
		if (!err && stream) {
			const bool hidden=i<4 ? !custom : custom ||
				(i>7 && !params[SL_TEMPORAL_ENABLED]->u.bd.value);
			err = suites.DynamicStreamSuite4()->AEGP_SetDynamicStreamFlag(stream,
				AEGP_DynStreamFlag_HIDDEN, FALSE, hidden);
			const A_Err disposeErr = suites.StreamSuite5()->AEGP_DisposeStream(stream);
			if (!err) err = disposeErr;
		}
	}
	const A_Err disposeErr = suites.EffectSuite4()->AEGP_DisposeEffect(effect);
	return err ? err : disposeErr;
}

static void SL_ReadScalarParams(PF_InData* in_dataP, SL_Ctx* ctx) {
	PF_ParamDef pd;
	AEFX_CLR_STRUCT(pd);
	const A_long t = in_dataP->current_time;
	const A_long dt = in_dataP->time_step;
	const A_u_long ts = in_dataP->time_scale;

	struct ParamReader {
		PF_InData* in;PF_ParamDef* p;bool checked=false;
		PF_Err get(A_long index) {
			if(checked){PF_CHECKIN_PARAM(in,p);checked=false;}
			memset(p,0,sizeof(*p));
			PF_Err e=PF_CHECKOUT_PARAM(in,index,in->current_time,in->time_step,in->time_scale,p);
			checked=!e;return e;
		}
		~ParamReader(){if(checked)PF_CHECKIN_PARAM(in,p);}
	} reader{in_dataP,&pd};
#define SL_GET(idx) reader.get(idx)

	/* 默认值 (与 ParamsSetup 一致; 取参数失败时兜底) */
	ctx->depthInvert = false;
	ctx->normalYUp   = true;
	ctx->followLight = false;
	ctx->hasLight3D  = false;
	ctx->lightX = (float)in_dataP->width * 0.45f;
	ctx->lightY = (float)in_dataP->height * 0.30f;
	ctx->lightZ = 700.f;
	ctx->lightR = 1.f; ctx->lightG = 240.f / 255.f; ctx->lightB = 220.f / 255.f;
	ctx->intensity = 1.2f;
	ctx->falloff   = 2500.f;
	ctx->ambient   = 0.30f;
	ctx->diffuse   = 1.f;
	ctx->specular  = 0.30f;
	ctx->shininess = 24.f;
	ctx->rim       = 0.f;
	ctx->softness  = 0.032f;
	ctx->backlightEnabled = true;
	ctx->backlightStart = 200.f;
	ctx->backlightDistance = 800.f;
	ctx->edgeKeep = 1.f; ctx->edgeWidth = 6.f;
	ctx->lvInB = 0.f; ctx->lvInW = 1.f; ctx->lvGamma = 1.f;
	ctx->lvOutB = 0.f; ctx->lvOutW = 1.f;
	ctx->depthRange = 1000.f;
	ctx->aiSource = 0; ctx->aiQuality = 3; ctx->aiSmooth = 10; ctx->aiStrength = 1000.f;
	ctx->view = 1; ctx->apply = 2; ctx->opacity = 1.f;
	ctx->temporalEnabled=ctx->temporalAlignment=true;
	ctx->persistentCache=true;
	ctx->temporalStrength=.85f;ctx->temporalResponse=.2f;

	if (!SL_GET(SL_AI_SOURCE))			ctx->aiSource = (pd.u.pd.value == 2) ? 1 : 0;
	if (!SL_GET(SL_AI_QUALITY))			ctx->aiQuality = (pd.u.pd.value >= 1 && pd.u.pd.value <= 4) ? (int)pd.u.pd.value - 1 : 1;
	if (!SL_GET(SL_AI_SMOOTH))			ctx->aiSmooth = (int)pd.u.fs_d.value;
	if (!SL_GET(SL_AI_STRENGTH))		ctx->aiStrength = (float)pd.u.fs_d.value;
	if (!SL_GET(SL_PERSISTENT_CACHE))ctx->persistentCache=(pd.u.bd.value!=0);

	if (!SL_GET(SL_DEPTH_INVERT))		ctx->depthInvert = (pd.u.bd.value != 0);
	if (!SL_GET(SL_NORMAL_Y_UP))		ctx->normalYUp = (pd.u.bd.value != 0);
	if (!SL_GET(SL_LIGHT_MODE))			ctx->followLight = (pd.u.pd.value == 2);
	if (!SL_GET(SL_LIGHT_POS)) {
		ctx->lightX = (float)pd.u.td.x_value / 65536.f;
		ctx->lightY = (float)pd.u.td.y_value / 65536.f;
	}
	if (!SL_GET(SL_LIGHT_Z))			ctx->lightZ = (float)pd.u.fs_d.value;
	if (!SL_GET(SL_LIGHT_INTENSITY))	ctx->intensity = (float)pd.u.fs_d.value * 0.01f;
	if (!SL_GET(SL_LIGHT_COLOR)) {
		ctx->lightR = pd.u.cd.value.red / 255.f;
		ctx->lightG = pd.u.cd.value.green / 255.f;
		ctx->lightB = pd.u.cd.value.blue / 255.f;
	}
	if (!SL_GET(SL_LIGHT_FALLOFF))		ctx->falloff = (float)pd.u.fs_d.value;
	if (!SL_GET(SL_AMBIENT))			ctx->ambient = (float)pd.u.fs_d.value * 0.01f;
	if (!SL_GET(SL_DIFFUSE))			ctx->diffuse = (float)pd.u.fs_d.value * 0.01f;
	if (!SL_GET(SL_SPECULAR))			ctx->specular = (float)pd.u.fs_d.value * 0.01f;
	if (!SL_GET(SL_SHININESS))			ctx->shininess = (float)pd.u.fs_d.value;
	if (!SL_GET(SL_RIM))				ctx->rim = (float)pd.u.fs_d.value * 0.01f;
	if (!SL_GET(SL_SOFTNESS))			ctx->softness = (float)pd.u.fs_d.value * 0.004f;
	if (!SL_GET(SL_LV_IN_BLACK))		ctx->lvInB = (float)pd.u.fs_d.value / 255.f;
	if (!SL_GET(SL_LV_GAMMA))			ctx->lvGamma = (float)pd.u.fs_d.value * 0.01f;
	if (!SL_GET(SL_LV_IN_WHITE))		ctx->lvInW = (float)pd.u.fs_d.value / 255.f;
	if (!SL_GET(SL_LV_OUT_BLACK))		ctx->lvOutB = (float)pd.u.fs_d.value / 255.f;
	if (!SL_GET(SL_LV_OUT_WHITE))		ctx->lvOutW = (float)pd.u.fs_d.value / 255.f;
	if (!SL_GET(SL_DEPTH_RANGE))		ctx->depthRange = (float)pd.u.fs_d.value;
	if (!SL_GET(SL_VIEW))				ctx->view = (int)pd.u.pd.value;
	if (!SL_GET(SL_APPLY))				ctx->apply = (int)pd.u.pd.value;
	if (!SL_GET(SL_OPACITY))			ctx->opacity = (float)pd.u.fs_d.value * 0.01f;

	if (!SL_GET(SL_BACKLIGHT_ENABLED)) ctx->backlightEnabled = (pd.u.bd.value != 0);
	if (!SL_GET(SL_BACKLIGHT_START)) ctx->backlightStart = (float)pd.u.fs_d.value;
	if (!SL_GET(SL_BACKLIGHT_DISTANCE)) ctx->backlightDistance = (float)pd.u.fs_d.value;
	if (ctx->backlightDistance < 1.f) ctx->backlightDistance = 1.f;
	if (!SL_GET(SL_EDGE_KEEP)) ctx->edgeKeep = SL_Clamp01((float)pd.u.fs_d.value * 0.01f);
	if (!SL_GET(SL_EDGE_WIDTH)) ctx->edgeWidth = (float)pd.u.fs_d.value;
	if (!SL_GET(SL_TEMPORAL_ENABLED)) ctx->temporalEnabled=pd.u.bd.value!=0;
	if (!SL_GET(SL_TEMPORAL_STRENGTH)) ctx->temporalStrength=SL_Clamp01((float)pd.u.fs_d.value*.01f);
	if (!SL_GET(SL_TEMPORAL_RESPONSE)) ctx->temporalResponse=SL_Clamp01((float)pd.u.fs_d.value*.01f);
	if (!SL_GET(SL_TEMPORAL_ALIGNMENT)) ctx->temporalAlignment=pd.u.bd.value!=0;
	if (!SL_GET(SL_TEMPORAL_ROTATION)) ctx->temporalRotation=pd.u.bd.value!=0;
#undef SL_GET
}

/* 跟随 AE 灯光层 (MadLight/Normality 思路): 找合成里第一盏灯光,
   读取合成时间下的三维位置, 转成本图层内的偏移。
   任何一步失败都返回 false, 静默回退到自定义点光源。 */
static bool SL_FollowLightPos(PF_InData* in_data, float pos[3]) {
	try {
		AEGP_SuiteHandler suites(in_data->pica_basicP);

		AEGP_LayerH ourLayer = NULL;
		if (suites.PFInterfaceSuite1()->AEGP_GetEffectLayer(in_data->effect_ref, &ourLayer) != PF_Err_NONE || !ourLayer)
			return false;

		A_Time compTime;
		if (suites.PFInterfaceSuite1()->AEGP_ConvertEffectToCompTime(
				in_data->effect_ref, in_data->current_time, in_data->time_scale, &compTime) != PF_Err_NONE)
			return false;

		AEGP_CompH compH = NULL;
		if (suites.LayerSuite7()->AEGP_GetLayerParentComp(ourLayer, &compH) != PF_Err_NONE || !compH)
			return false;

		A_long numLayers = 0;
		if (suites.LayerSuite7()->AEGP_GetCompNumLayers(compH, &numLayers) != PF_Err_NONE)
			return false;

		AEGP_LayerH lightLayer = NULL;
		for (A_long i = 0; i < numLayers; ++i) {
			AEGP_LayerH L = NULL;
			if (suites.LayerSuite7()->AEGP_GetCompLayerByIndex(compH, i, &L) != PF_Err_NONE || !L)
				continue;
			AEGP_ObjectType ot = AEGP_ObjectType_NONE;
			if (suites.LayerSuite7()->AEGP_GetLayerObjectType(L, &ot) != PF_Err_NONE)
				continue;
			if (ot == AEGP_ObjectType_LIGHT) { lightLayer = L; break; }
		}
		if (!lightLayer)
			return false;

		/* 读位置流 (兼容 2D/3D 流) */
#define SL_READ_POS(layerH, out)											\
		do {																\
			AEGP_StreamRefH sh = NULL;										\
			bool ok = false;												\
			if (suites.StreamSuite5()->AEGP_GetNewLayerStream(0, (layerH),	\
					AEGP_LayerStream_POSITION, &sh) == PF_Err_NONE && sh) {	\
				AEGP_StreamType st = AEGP_StreamType_OneD;					\
				if (suites.StreamSuite5()->AEGP_GetStreamType(sh, &st) == PF_Err_NONE) { \
					AEGP_StreamValue2 val;									\
					memset(&val, 0, sizeof(val));							\
					if (suites.StreamSuite5()->AEGP_GetNewStreamValue(0, sh, \
							AEGP_LTimeMode_CompTime, &compTime, FALSE, &val) == PF_Err_NONE) { \
						if (st == AEGP_StreamType_ThreeD || st == AEGP_StreamType_ThreeD_SPATIAL) { \
							out[0] = (float)val.val.three_d.x;				\
							out[1] = (float)val.val.three_d.y;				\
							out[2] = (float)val.val.three_d.z;				\
							ok = true;										\
						} else {											\
							out[0] = (float)val.val.two_d.x;				\
							out[1] = (float)val.val.two_d.y;				\
							out[2] = 0.f;									\
							ok = true;										\
						}													\
						suites.StreamSuite5()->AEGP_DisposeStreamValue(&val); \
					}														\
				}															\
				suites.StreamSuite5()->AEGP_DisposeStream(sh);				\
			}																\
			if (!ok) return false;											\
		} while (0)

		float ourP[3] = { 0.f, 0.f, 0.f };
		float lightP[3] = { 0.f, 0.f, 1.f };
		SL_READ_POS(ourLayer, ourP);
		SL_READ_POS(lightLayer, lightP);
#undef SL_READ_POS

		/* AE 的 z 轴背离观察者; 法线图 z 朝向观察者 -> 取反 */
		pos[0] = lightP[0] - ourP[0];
		pos[1] = lightP[1] - ourP[1];
		pos[2] = ourP[2] - lightP[2];
		return true;
	} catch (...) {
		return false;
	}
}

static void SL_DisposePreRenderData(void* p) {
	SL_Ctx* ctx = reinterpret_cast<SL_Ctx*>(p);
	if (ctx) {
		free(ctx->aiNearData);
		free(ctx->aiNormData);
		free(ctx->historyChecked);
		delete ctx->pinnedCheckpoint;
	}
	free(p);
}

static A_long SL_ScaledDimension(A_long extent,const PF_RationalScale& scale) {
	if(extent<=0)return 1;
	if(scale.num<=0 || scale.den<=0)return extent;
	return (A_long)(((long long)extent*scale.num+scale.den-1)/scale.den);
}

static void SL_ExtendCanvas(PF_LRect& canvas,const PF_LRect& bounds) {
	if(bounds.right<=bounds.left || bounds.bottom<=bounds.top)return;
	canvas.left=(std::min)(canvas.left,bounds.left);
	canvas.top=(std::min)(canvas.top,bounds.top);
	canvas.right=(std::max)(canvas.right,bounds.right);
	canvas.bottom=(std::max)(canvas.bottom,bounds.bottom);
}

static SL_HistoryKey SL_RequestSettings(const PF_InData* in,const SL_Ctx* ctx) {
	SL_HistoryKey key;
	key.value(in->width);key.value(in->height);key.value(in->quality);key.value(in->field);
	key.value(in->downsample_x.num);key.value(in->downsample_x.den);
	key.value(in->downsample_y.num);key.value(in->downsample_y.den);key.value(ctx->pixelDepth);
	key.value(ctx->aiQuality);key.value(ctx->aiSmooth);key.value(ctx->aiStrength);
	key.value(ctx->temporalStrength);key.value(ctx->temporalResponse);key.value(ctx->temporalAlignment);
	key.value(ctx->temporalRotation);key.value(in->time_step);key.value(in->time_scale);
	return key;
}

static std::shared_ptr<const SL_Checkpoint> SL_FindCheckpoint(PF_InData* in,const SL_Ctx* ctx,
	const SL_HistoryKey& settings,long long step,bool requestOnly=false,SL_CheckpointLookup* probe=nullptr) {
	if(!ctx->inputStateValid||!in->pica_basicP)return {};
	const PF_ParamUtilsSuite3* suite=nullptr;
	if(in->pica_basicP->AcquireSuite(kPFParamUtilsSuite,kPFParamUtilsSuiteVersion3,(const void**)&suite)||!suite)return {};
	struct Release {SPBasicSuite* basic;~Release(){basic->ReleaseSuite(kPFParamUtilsSuite,kPFParamUtilsSuiteVersion3);}} release{in->pica_basicP};
	return SL_checkpointCache.get(in->effect_ref,settings,in->current_time,step,
		in->current_time<0?in->current_time:0,[&](const PF_State& state){A_Boolean same=FALSE;
		return suite->PF_AreStatesIdentical&&!suite->PF_AreStatesIdentical(in->effect_ref,&state,&ctx->inputState,&same)&&same;},requestOnly,probe,ctx->instanceID);
}

static PF_Err
PreRender(PF_InData* in_dataP, PF_OutData* out_dataP, PF_PreRenderExtra* extraP) {
	const double started=SL_Milliseconds();
	PF_Err err = PF_Err_NONE;
	PF_RenderRequest req = extraP->input->output_request;

	SL_Ctx* ctx = reinterpret_cast<SL_Ctx*>(malloc(sizeof(SL_Ctx)));
	if (!ctx) return PF_Err_OUT_OF_MEMORY;
	memset(ctx, 0, sizeof(SL_Ctx));
	ctx->pixelDepth=extraP->input->bitdepth;
	ctx->instanceID=SL_ReadInstanceID(in_dataP);

	SL_ReadScalarParams(in_dataP, ctx);
	if(ctx->aiSource==0 && ctx->temporalEnabled && ctx->temporalStrength>0)
		ctx->inputStateValid=SL_ReadInputState(in_dataP,&ctx->inputState);
	// A cropped SmartFX checkout must not redefine the AI image or its history.
	ctx->aiCanvas={0,0,SL_ScaledDimension(in_dataP->width,in_dataP->downsample_x),
		SL_ScaledDimension(in_dataP->height,in_dataP->downsample_y)};
	PF_RenderRequest fullReq=req;
	if(ctx->aiSource==0) {
		fullReq.rect.left=fullReq.rect.top=-32768;
		fullReq.rect.right=fullReq.rect.bottom=32768;
		fullReq.channel_mask=PF_ChannelMask_ARGB;
		fullReq.preserve_rgb_of_zero_alpha=TRUE;
	}

	/* 输入层 */
	PF_CheckoutResult in_result;
	AEFX_CLR_STRUCT(in_result);
	ERR(extraP->cb->checkout_layer(in_dataP->effect_ref,
								   SL_INPUT, SL_INPUT,
								   &fullReq,
								   in_dataP->current_time,
								   in_dataP->time_step,
								   in_dataP->time_scale,
								   &in_result));
	if(!err)SL_ExtendCanvas(ctx->aiCanvas,in_result.max_result_rect);
	if(!err && ctx->aiSource==0 && ctx->temporalEnabled && ctx->temporalStrength>0) {
		const long long step=in_dataP->time_step<0 ? -(long long)in_dataP->time_step : in_dataP->time_step;
		const long long count=step>0 && in_dataP->current_time>0 ? ((long long)in_dataP->current_time+step-1)/step : 0;
		if(count>(LONG_MAX-SL_HISTORY_CHECKOUT)/2)err=PF_Err_OUT_OF_MEMORY;
		if(count && !err) {
			ctx->historyChecked=(bool*)calloc((size_t)count,sizeof(bool));
			if(!ctx->historyChecked)err=PF_Err_OUT_OF_MEMORY;
			else ctx->historyCount=(A_long)count;
		}
		A_long historyStart=0;
		ctx->firstNewHistoryIndex = SL_FirstNewHistoryIndex(ctx->historyCount,
			ctx->persistentCache ? SL_SurfaceDisk() : nullptr);
		if(!err) {
			try {
				auto trusted=SL_FindCheckpoint(in_dataP,ctx,SL_RequestSettings(in_dataP,ctx),step,true);
				if(trusted) {
					PF_LRect combined=trusted->canvas;SL_ExtendCanvas(combined,ctx->aiCanvas);
					if(!memcmp(&combined,&trusted->canvas,sizeof(combined))) {
						ctx->pinnedCheckpoint=new std::shared_ptr<const SL_Checkpoint>(trusted);
						ctx->aiCanvas=combined;
						historyStart=trusted->time==in_dataP->current_time?ctx->historyCount:(A_long)(trusted->time/step)+1;
					}
				}
			} catch(const std::bad_alloc&) {} // Full declarations remain a valid fallback.
		}
		auto declareHistory=[&](A_long begin,A_long end) {
		for(A_long index=begin;index<end && !err;index++) {
			if(PF_ABORT(in_dataP)){err=PF_Interrupt_CANCEL;break;}
			const long long time=step*index;
			PF_CheckoutResult previous={};
			err=extraP->cb->checkout_layer(in_dataP->effect_ref,SL_INPUT,SL_HISTORY_CHECKOUT+index,
				&fullReq,(A_long)time,in_dataP->time_step,in_dataP->time_scale,&previous);
			++ctx->historyDeclarations;
			ctx->historyChecked[index]=!err && previous.result_rect.right>previous.result_rect.left &&
				previous.result_rect.bottom>previous.result_rect.top;
			if(!err)SL_ExtendCanvas(ctx->aiCanvas,previous.max_result_rect);
			if(!err && index < ctx->firstNewHistoryIndex) {
				PF_CheckoutResult replay={};
				err=extraP->cb->checkout_layer(in_dataP->effect_ref,SL_INPUT,
					SL_HISTORY_CHECKOUT+ctx->historyCount+index,&fullReq,(A_long)time,
					in_dataP->time_step,in_dataP->time_scale,&replay);
				++ctx->historyDeclarations;
			}
		}
		};
		declareHistory(historyStart,ctx->historyCount);
		// New motion can expand the canonical canvas. Rebuild with the full history in that case.
		if(ctx->pinnedCheckpoint&&memcmp(&ctx->aiCanvas,&(*ctx->pinnedCheckpoint)->canvas,sizeof(PF_LRect))) {
			delete ctx->pinnedCheckpoint;ctx->pinnedCheckpoint=nullptr;
			declareHistory(0,historyStart);
		}
	}

	if(ctx->aiSource==1 && !err) {
		auto declareMap = [&](PF_ParamIndex index, bool& present) {
			PF_CheckoutResult result={};
			ERR(extraP->cb->checkout_layer(in_dataP->effect_ref,index,index,&req,
				in_dataP->current_time,in_dataP->time_step,in_dataP->time_scale,&result));
			present = !err && result.result_rect.right>result.result_rect.left &&
				result.result_rect.bottom>result.result_rect.top;
		};
		declareMap(SL_NORMAL_LAYER,ctx->hasNormal);
		declareMap(SL_DEPTH_LAYER,ctx->hasDepth);
	}

	ctx->mapW = in_dataP->width;
	ctx->mapH = in_dataP->height;
	ctx->preMilliseconds=SL_Milliseconds()-started;
	if(err){SL_DisposePreRenderData(ctx);return err;}

	extraP->output->pre_render_data = ctx;
	extraP->output->delete_pre_render_data_func = SL_DisposePreRenderData;

	/* 输出范围 = 输入层可见范围 ∩ 请求范围 */
	PF_LRect res;
	SL_IntersectLRect(&in_result.result_rect, &req.rect, &res);
	extraP->output->result_rect = res;

	{
		PF_LRect u = extraP->output->max_result_rect;
		if (in_result.max_result_rect.left   < u.left)   u.left   = in_result.max_result_rect.left;
		if (in_result.max_result_rect.top    < u.top)    u.top    = in_result.max_result_rect.top;
		if (in_result.max_result_rect.right  > u.right)  u.right  = in_result.max_result_rect.right;
		if (in_result.max_result_rect.bottom > u.bottom) u.bottom = in_result.max_result_rect.bottom;
		extraP->output->max_result_rect = u;
	}

	return err;
}

static PF_Err
SL_CoreRender(PF_InData* in_data,
			  PF_OutData* out_data,
			  PF_PixelFormat pixel_format,
			  PF_EffectWorld* input_worldP,
			  PF_EffectWorld* output_worldP,
			  SL_Ctx* ctx) {
	PF_Err err = PF_Err_NONE;

	switch (pixel_format) {
		case PF_PixelFormat_ARGB128: {
			AEFX_SuiteScoper<PF_iterateFloatSuite2> it(in_data, kPFIterateFloatSuite,
													   kPFIterateFloatSuiteVersion2, out_data);
			err = it->iterate(in_data, 0, output_worldP->height, NULL, NULL,
							  (void*)ctx, SL_PixelFunc32, output_worldP);
			break;
		}
		case PF_PixelFormat_ARGB64: {
			AEFX_SuiteScoper<PF_iterate16Suite2> it(in_data, kPFIterate16Suite,
													kPFIterate16SuiteVersion2, out_data);
			err = it->iterate(in_data, 0, output_worldP->height, NULL, NULL,
							  (void*)ctx, SL_PixelFunc16, output_worldP);
			break;
		}
		case PF_PixelFormat_ARGB32: {
			AEFX_SuiteScoper<PF_Iterate8Suite2> it(in_data, kPFIterate8Suite,
												   kPFIterate8SuiteVersion2, out_data);
			err = it->iterate(in_data, 0, output_worldP->height, NULL, NULL,
							  (void*)ctx, SL_PixelFunc8, output_worldP);
			break;
		}
		default:
			err = PF_Err_BAD_CALLBACK_PARAM;
			break;
	}
	return err;
}

static PF_Err
SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extraP) {
	PF_Err			err		= PF_Err_NONE,
					err2	= PF_Err_NONE;

	const SL_Ctx* prepared=reinterpret_cast<const SL_Ctx*>(extraP->input->pre_render_data);
	if (!prepared) return PF_Err_INTERNAL_STRUCT_DAMAGED;
	SL_Ctx renderContext=*prepared;
	SL_Ctx* ctx=&renderContext;
	SL_Performance perf;
	perf.time=in_data->current_time;perf.scale=in_data->time_scale;perf.owner=in_data->effect_ref;
	perf.preMs=ctx->preMilliseconds;perf.history=ctx->historyCount;
	perf.declarations=ctx->historyDeclarations;perf.state=ctx->inputStateValid;
	struct Report {SL_Performance& p;PF_Err& error;~Report(){p.write(error);}} report{perf,err};
	// Shading borrows the packed planes; retain their owner through SL_CoreRender.
	SL_RenderSurface stableDepth;
	bool inputChecked=false;

	PF_EffectWorld	*input_worldP	= NULL,
					*output_worldP	= NULL,
					*normal_worldP	= NULL,
					*depth_worldP	= NULL;

	const double inputStarted=SL_Milliseconds();
	ERR(extraP->cb->checkout_layer_pixels(in_data->effect_ref, SL_INPUT, &input_worldP));
	perf.checkoutMs+=SL_Milliseconds()-inputStarted;
	inputChecked=!err;
	ERR(extraP->cb->checkout_output(in_data->effect_ref, &output_worldP));

	if (!err && ctx->hasNormal) {
		err = extraP->cb->checkout_layer_pixels(in_data->effect_ref, SL_NORMAL_LAYER, &normal_worldP);
		if (err || !normal_worldP || !normal_worldP->data) {
			ctx->hasNormal = false;
			normal_worldP = NULL;
			err = PF_Err_NONE;
		}
	}
	if (!err && ctx->hasDepth) {
		err = extraP->cb->checkout_layer_pixels(in_data->effect_ref, SL_DEPTH_LAYER, &depth_worldP);
		if (err || !depth_worldP || !depth_worldP->data) {
			ctx->hasDepth = false;
			depth_worldP = NULL;
			err = PF_Err_NONE;
		}
	}

	try {
	if (!err && input_worldP && output_worldP &&
		input_worldP->data && output_worldP->data &&
		input_worldP->width > 0 && output_worldP->height > 0) {
		AEFX_SuiteScoper<PF_WorldSuite2> world_suite(in_data, kPFWorldSuite,
													 kPFWorldSuiteVersion2, out_data);
		PF_PixelFormat pixel_format = PF_PixelFormat_INVALID;
		ERR(world_suite->PF_GetPixelFormat(input_worldP, &pixel_format));
		SL_FillWorld(ctx->sourceWorld, input_worldP, pixel_format);
		ctx->outputOffsetX=output_worldP->origin_x-input_worldP->origin_x;
		ctx->outputOffsetY=output_worldP->origin_y-input_worldP->origin_y;
		ctx->lightX-=input_worldP->origin_x;ctx->lightY-=input_worldP->origin_y;

		if (ctx->hasNormal && normal_worldP) {
			SL_FillWorld(ctx->normalWorld, normal_worldP, pixel_format);
			if (ctx->normalWorld.w <= 0 || ctx->normalWorld.h <= 0) ctx->hasNormal = false;
		} else {
			ctx->hasNormal = false;
		}
		if (ctx->hasDepth && depth_worldP) {
			SL_FillWorld(ctx->depthWorld, depth_worldP, pixel_format);
			if (ctx->depthWorld.w <= 0 || ctx->depthWorld.h <= 0) ctx->hasDepth = false;
		} else {
			ctx->hasDepth = false;
		}
		ctx->mapW = input_worldP->width;
		ctx->mapH = input_worldP->height;

		if (!err && ctx->aiSource==0) {
			const int w=ctx->aiCanvas.right-ctx->aiCanvas.left,h=ctx->aiCanvas.bottom-ctx->aiCanvas.top;
			perf.width=w;perf.height=h;
			const size_t n=(size_t)w*h;
			ctx->mapW=w;ctx->mapH=h;
			ctx->aiMapOffsetX=input_worldP->origin_x-ctx->aiCanvas.left;
			ctx->aiMapOffsetY=input_worldP->origin_y-ctx->aiCanvas.top;
			auto pixels=[&](PF_EffectWorld* world,PF_PixelFormat format) {
				const double started=SL_Milliseconds();
				SL_World src;SL_FillWorld(src,world,format);
				std::vector<float> rgba(n*4);
				const int ox=ctx->aiCanvas.left-world->origin_x;
				const int oy=ctx->aiCanvas.top-world->origin_y;
				for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
					float s[4]={};
					if(x+ox>=0 && y+oy>=0 && x+ox<src.w && y+oy<src.h)SL_Fetch(src,x+ox,y+oy,s);
					float* o=rgba.data()+((size_t)y*w+x)*4;
					for(int k=0;k<3;k++)o[k]=s[k]*s[3]+.5f*(1-s[3]);
					o[3]=s[3];
				}
				perf.convertMs+=SL_Milliseconds()-started;return rgba;
			};
			if(!SL_AI_Available())err=PF_Err_BAD_CALLBACK_PARAM;
			else {
#ifdef SANLIGHT_EXPERIMENTAL_DML
				perf.gpu=SL_AI_UsingGPU();
#else
				perf.gpu=0;
#endif
			}
			std::vector<float> previousRGBA;
			std::vector<float> currentRGBA;
			std::vector<SL_HistoryKey> prefixKeys((size_t)ctx->historyCount+1);
			std::vector<bool> prefixHasImage((size_t)ctx->historyCount+1,false);
			A_long checkpoint=-1;
			auto abort=[](void* p)->bool{return PF_ABORT((PF_InData*)p)!=PF_Err_NONE;};
			const long long step=in_data->time_step<0 ? -(long long)in_data->time_step : in_data->time_step;
			long long previousTime=-1;
			const SL_HistoryKey requestSettings=SL_RequestSettings(in_data,ctx);
			SL_HistoryKey key=requestSettings;
			key.value(w);key.value(h);key.value(ctx->aiCanvas.left);key.value(ctx->aiCanvas.top);
			const uint32_t prefixEncoding=2;key.value(prefixEncoding);
			const bool temporal=ctx->temporalEnabled && ctx->temporalStrength>0;
			SL_DiskCache* disk=temporal&&ctx->persistentCache?SL_SurfaceDisk():nullptr;
			uint64_t modelIdentity[4]={};
			if(disk&&!SL_AI_CacheIdentity(modelIdentity))disk=nullptr;
			auto diskKey=[&](const SL_HistoryKey& prefix) {
				SL_HistoryKey result=prefix;
				const uint32_t algorithm=6;result.value(algorithm);
				result.append(modelIdentity,sizeof(modelIdentity));return result;
			};
			const SL_HistoryKey settings=key;
			auto advancePrefix = [&](PF_EffectWorld* frame,PF_PixelFormat format,std::vector<float>& rgba) {
				const int pixelBytes = format==PF_PixelFormat_ARGB32 ? 4 :
					(format==PF_PixelFormat_ARGB64 ? 8 : (format==PF_PixelFormat_ARGB128 ? 16 : 0));
				SL_PrefixMemo::Signature signature{};
				const double signStarted=SL_Milliseconds();
				const bool canMemo=n>=4096 && SL_PrefixMemo::sign(
					static_cast<const unsigned char*>(static_cast<const void*>(frame->data)),
					frame->width,frame->height,frame->rowbytes,pixelBytes,frame->origin_x,frame->origin_y,
					int(format),key,signature);
				const bool hit=canMemo && SL_prefixMemo.get(signature,key);
				perf.hashMs+=SL_Milliseconds()-signStarted;
				if(hit){++perf.prefixMemoHits;return;}
				if(canMemo)++perf.prefixMemoMisses;
				if(!canMemo&&rgba.empty())rgba=pixels(frame,format);
				const double appendStarted=SL_Milliseconds();
				if(canMemo)key=SL_PrefixMemo::advance(signature);
				else {
					const uint32_t floatEncoding=0x32464C54u;key.value(floatEncoding);
					key.append(rgba.data(),rgba.size()*sizeof(float));
				}
				perf.hashMs+=SL_Milliseconds()-appendStarted;
				if(canMemo)SL_prefixMemo.put(signature,key);
			};
			std::shared_ptr<const SL_Checkpoint> trusted;
			if(temporal) {
				trusted=ctx->pinnedCheckpoint?*ctx->pinnedCheckpoint:nullptr;
				// AE may prepare a batch before rendering it. Prefer work completed
				// since PreRender, but only with the exact already-declared canvas.
				SL_CheckpointLookup lookup;
				auto latest=SL_FindCheckpoint(in_data,ctx,settings,step,false,&lookup);
				perf.entries=lookup.entries;perf.ownerMatches=lookup.ownerMatches;
				perf.eligible=lookup.eligible;perf.stateRejected=lookup.stateRejected;
				if(latest&&(!trusted||latest->time>trusted->time)) {
					trusted=std::move(latest);perf.refreshed=1;
				}
			}
			A_long scanStart=0;
			// The selected anchor is immutable and pinned. Consume it directly until
			// replay (or a newer content hit) supplies the next pair of buffers.
			bool useTrustedHistory = bool(trusted);
			if(trusted) {
				perf.anchor=trusted->time;
				checkpoint=trusted->time==in_data->current_time ? ctx->historyCount : (A_long)(trusted->time/step);
				key=trusted->prefix;
				previousTime=trusted->time;
				scanStart=checkpoint+1;
			}
			// Validate the possibly cached prefix first; the new suffix is hashed during replay.
			const A_long validateEnd = ctx->firstNewHistoryIndex;
			for(A_long index=scanStart;index<validateEnd && !err;index++) {
				++perf.scanned;
				const bool current=index==ctx->historyCount;
				PF_EffectWorld* frame=current ? input_worldP : nullptr;
				struct Checkin {
					PF_SmartRenderCallbacks* cb;PF_ProgPtr ref;A_long id;bool active=false;
					~Checkin(){if(active)cb->checkin_layer_pixels(ref,id);}
				} checkin{extraP->cb,in_data->effect_ref,SL_HISTORY_CHECKOUT+index};
				if(!current && ctx->historyChecked[index]) {
					const double started=SL_Milliseconds();
					err=extraP->cb->checkout_layer_pixels(in_data->effect_ref,checkin.id,&frame);
					perf.checkoutMs+=SL_Milliseconds()-started;
					checkin.active=!err;if(err)break;
				}
				const long long time=current ? in_data->current_time : step*index;
				key.value(time);
				if(!frame || !frame->data) {
					const int empty=-1;key.value(empty);prefixKeys[index]=key;continue;
				}
				if(PF_ABORT(in_data)){err=PF_Interrupt_CANCEL;break;}
				PF_PixelFormat format=PF_PixelFormat_INVALID;
				ERR(world_suite->PF_GetPixelFormat(frame,&format));
				if(err)break;
				std::vector<float> rgba;
				advancePrefix(frame,format,rgba);
				prefixKeys[index]=key;
				prefixHasImage[index]=true;
				SL_RenderSurface result;
				if(temporal)result.shared=SL_historyCache.get(key);
				if(result.shared) {
					if(rgba.empty())rgba=pixels(frame,format);
					checkpoint=index;stableDepth=std::move(result);previousRGBA=std::move(rgba);
					useTrustedHistory=false;
					previousTime=time;
				}
				if(current && checkpoint!=index) {
					if(rgba.empty())rgba=pixels(frame,format);
					currentRGBA=std::move(rgba);
				}
			}
			// Validate source prefixes first, then read only the latest disk surface.
			// Opaque AE state/owner values are never persisted as content identity.
			if(disk&&!err)for(A_long index=validateEnd-1;index>checkpoint;--index) {
				if(PF_ABORT(in_data)){err=PF_Interrupt_CANCEL;break;}
				if(!prefixHasImage[index])continue;
				SL_RenderSurface result;
				const double started=SL_Milliseconds();
				const bool hit=disk->get(diskKey(prefixKeys[index]),result.local,abort,in_data)&&result.local.size()==n*4;
				perf.diskReadMs+=SL_Milliseconds()-started;
				if(PF_ABORT(in_data)){err=PF_Interrupt_CANCEL;break;}
				if(!hit){++perf.diskMisses;continue;}
				const bool current=index==ctx->historyCount;
				std::vector<float> rgba;
				if(current)rgba=std::move(currentRGBA);
				else {
					PF_EffectWorld* frame=nullptr;
					struct Checkin {
						PF_SmartRenderCallbacks* cb;PF_ProgPtr ref;A_long id;bool active=false;
						~Checkin(){if(active)cb->checkin_layer_pixels(ref,id);}
					} checkin{extraP->cb,in_data->effect_ref,SL_HISTORY_CHECKOUT+ctx->historyCount+index};
					const double checkoutStarted=SL_Milliseconds();
					err=extraP->cb->checkout_layer_pixels(in_data->effect_ref,checkin.id,&frame);
					perf.checkoutMs+=SL_Milliseconds()-checkoutStarted;
					checkin.active=!err;if(err)break;
					if(!frame||!frame->data)continue;
					PF_PixelFormat format=PF_PixelFormat_INVALID;
					ERR(world_suite->PF_GetPixelFormat(frame,&format));if(err)break;
					rgba=pixels(frame,format);
				}
				++perf.diskHits;checkpoint=index;stableDepth=std::move(result);
				previousRGBA=std::move(rgba);
				useTrustedHistory=false;
				previousTime=current?in_data->current_time:step*index;
				break;
			}
			if(checkpoint==ctx->historyCount)std::vector<float>().swap(currentRGBA);
			// Replay only after the latest verified checkpoint; eviction changes cost, not output.
			for(A_long index=checkpoint+1;index<=ctx->historyCount && !err;index++) {
				++perf.replayed;
				if(PF_ABORT(in_data)){err=PF_Interrupt_CANCEL;break;}
				if(in_data->inter.progress) {
					err=PF_PROGRESS(in_data,index+1,ctx->historyCount+1);if(err)break;
				}
				const bool current=index==ctx->historyCount;
				const long long time=current ? in_data->current_time : step*index;
				const bool newSuffix = index >= validateEnd;
				PF_EffectWorld* frame=current ? input_worldP : nullptr;
				struct Checkin {
					PF_SmartRenderCallbacks* cb;PF_ProgPtr ref;A_long id;bool active=false;
					~Checkin(){if(active)cb->checkin_layer_pixels(ref,id);}
				} checkin{extraP->cb,in_data->effect_ref,SL_HISTORY_CHECKOUT+index+(newSuffix?0:ctx->historyCount)};
				if(!current && ctx->historyChecked[index]) {
					const double started=SL_Milliseconds();
					err=extraP->cb->checkout_layer_pixels(in_data->effect_ref,checkin.id,&frame);
					perf.checkoutMs+=SL_Milliseconds()-started;
					checkin.active=!err;if(err)break;
				}
				if(newSuffix && temporal) {
					++perf.scanned;
					key.value(time);
				}
				if(!current && (!frame || !frame->data)) {
					if(newSuffix && temporal) { const int empty=-1; key.value(empty); prefixKeys[index]=key; }
					stableDepth={};previousRGBA.clear();useTrustedHistory=false;continue;
				}
				PF_PixelFormat format=PF_PixelFormat_INVALID;
				if(!current || newSuffix){ERR(world_suite->PF_GetPixelFormat(frame,&format));if(err)break;}
				auto rgba=current && !newSuffix ? std::move(currentRGBA) : pixels(frame,format);
				if(newSuffix && temporal) {
					advancePrefix(frame,format,rgba);
					prefixKeys[index]=key;
				}
				bool visible=false;
				for(size_t i=3;i<rgba.size();i+=4)if(rgba[i]>.001f){visible=true;break;}
				if(!current && !visible){stableDepth={};previousRGBA.clear();useTrustedHistory=false;continue;}
				const auto& priorImage = useTrustedHistory ? trusted->image : previousRGBA;
				const auto& priorSurface = useTrustedHistory ? *trusted->depth : stableDepth.pixels();
				SL_RenderSurface result;
				if(temporal)result.shared=SL_historyCache.get(prefixKeys[index]);
				bool cached = bool(result.shared);
				if(!cached && newSuffix && disk) {
					// A different AE process may have published this frame since the initial probe.
					const double started=SL_Milliseconds();
					cached=disk->get(diskKey(prefixKeys[index]),result.local,abort,in_data)&&result.local.size()==n*4;
					perf.diskReadMs+=SL_Milliseconds()-started;
					if(PF_ABORT(in_data)){err=PF_Interrupt_CANCEL;break;}
					if(cached)++perf.diskHits;else ++perf.diskMisses;
				}
				if(!cached) {
					if(temporal&&ctx->temporalStrength>=.5f&&!priorSurface.empty()&&rgba.size()==priorImage.size()&&
						!memcmp(rgba.data(),priorImage.data(),rgba.size()*sizeof(float))) {
						result.shared=useTrustedHistory ? trusted->depth : stableDepth.shared;
						if(!result.shared)result.local=priorSurface;
					} else {
					float *rawDepth=nullptr,*rawNormal=nullptr;
					SL_MotionJob motion;
					if(perf.gpu==1 && temporal && ctx->temporalAlignment && !priorSurface.empty() &&
						rgba.size()==priorImage.size() && n>=65536)
						motion.start(priorImage.data(),rgba.data(),w,h);
					const double aiStarted=SL_Milliseconds();++perf.ai;
					int inferenceRan=0;
					const int rc=SL_AI_GenerateCached(rgba.data(),w,h,ctx->aiQuality,ctx->aiSmooth,ctx->aiStrength,
						&rawDepth,&rawNormal,disk?1:0,&inferenceRan);
					perf.modelRuns+=inferenceRan;
					perf.aiMs+=SL_Milliseconds()-aiStarted;
					std::unique_ptr<float,decltype(&free)> ownedDepth(rawDepth,free),ownedNormal(rawNormal,free);
					if(rc||!rawDepth||!rawNormal){err=PF_Err_BAD_CALLBACK_PARAM;break;}
					result.local.resize(n*4);
					memcpy(result.local.data(),rawDepth,n*sizeof(float));
					memcpy(result.local.data()+n,rawNormal,n*3*sizeof(float));
					ownedDepth.reset();ownedNormal.reset();
					if(!priorSurface.empty() && temporal) {
						const double surfaceStarted=SL_Milliseconds();++perf.surfaces;
						SL_TrackResult* preparedFlow=motion.finish(abort,in_data);
						const bool completed=!motion.interrupted()&&SL_TemporalSurface(priorImage.data(),rgba.data(),priorSurface.data(),result.local.data(),
							w,h,ctx->temporalStrength,ctx->temporalResponse,ctx->temporalAlignment,
							in_data->time_scale ? float(time-previousTime)/in_data->time_scale : 1.f/30,abort,in_data,ctx->temporalRotation,preparedFlow);
						perf.surfaceMs+=SL_Milliseconds()-surfaceStarted;
						if(!completed){err=PF_Interrupt_CANCEL;break;}
					}
					}
					if(temporal)result.shared=SL_historyCache.put(prefixKeys[index],result.freeze());
					if(disk) {
						const double writeStarted=SL_Milliseconds();
						if(disk->put(diskKey(prefixKeys[index]),result.pixels(),abort,in_data))++perf.diskWrites;
						else ++perf.diskFailures;
						perf.diskWriteMs+=SL_Milliseconds()-writeStarted;
						if(PF_ABORT(in_data)){err=PF_Interrupt_CANCEL;break;}
					}
				}
				stableDepth=std::move(result);previousRGBA=std::move(rgba);
				useTrustedHistory=false;
				previousTime=time;
				if(!current&&temporal&&ctx->inputStateValid&&index%8==7) {
					try {
						auto saved=std::make_shared<SL_Checkpoint>();
						saved->owner=in_data->effect_ref;saved->instance=ctx->instanceID;
						saved->state=ctx->inputState;saved->settings=settings;
						saved->requestSettings=requestSettings;saved->canvas=ctx->aiCanvas;
						saved->prefix=prefixKeys[index];saved->time=(A_long)time;
						// Only an expanded canvas can incorporate bounds from later frames.
						const PF_LRect nominal={0,0,SL_ScaledDimension(in_data->width,in_data->downsample_x),
							SL_ScaledDimension(in_data->height,in_data->downsample_y)};
						saved->minimumTarget=memcmp(&nominal,&ctx->aiCanvas,sizeof(nominal))?in_data->current_time:(A_long)time;
						saved->depth=stableDepth.freeze();
						if(saved->depth){saved->image=previousRGBA;SL_checkpointCache.put(std::move(saved));}
					} catch(const std::bad_alloc&) {}
				}
			}
			if(!err && useTrustedHistory)stableDepth.shared=trusted->depth;
			if(!err && !useTrustedHistory && !stableDepth.pixels().empty() && temporal && ctx->inputStateValid) {
					try {
						auto saved=std::make_shared<SL_Checkpoint>();
						saved->owner=in_data->effect_ref;saved->instance=ctx->instanceID;
						saved->state=ctx->inputState;saved->settings=settings;
						saved->requestSettings=requestSettings;saved->canvas=ctx->aiCanvas;
						saved->prefix=prefixKeys[ctx->historyCount];saved->time=in_data->current_time;
						saved->depth=stableDepth.freeze();
						if(saved->depth) {
							saved->image=std::move(previousRGBA);
							perf.admitted=SL_checkpointCache.put(std::move(saved))?1:0;
						}
					} catch(const std::bad_alloc&) {} // caching is optional, not a render requirement
			}
			if(err && err!=PF_Interrupt_CANCEL) {
				_snprintf_s(out_data->return_msg,sizeof(out_data->return_msg),_TRUNCATE,
					"SanLight v1.3: AI failed: %s",SL_AI_LastError());
				out_data->out_flags|=PF_OutFlag_DISPLAY_ERROR_MESSAGE;
			}
			const auto& shadingSurface = stableDepth.pixels();
			if(!err && !shadingSurface.empty()) {
				ctx->depthWorld={};ctx->normalWorld={};
				ctx->depthWorld.w=ctx->normalWorld.w=w;ctx->depthWorld.h=ctx->normalWorld.h=h;
				ctx->depthWorld.fdata=shadingSurface.data();ctx->depthWorld.chans=1;
				ctx->normalWorld.fdata=shadingSurface.data()+n;ctx->normalWorld.chans=3;
				ctx->hasDepth=ctx->hasNormal=true;ctx->depthInvert=false;ctx->normalYUp=true;
			}
		}

		/* 光源: 跟随 AE 灯光层, 失败回退到可拖拽点光源 */
		ctx->hasLight3D = false;
		if (ctx->followLight && ctx->hasNormal) {
			float p[3];
			if (SL_FollowLightPos(in_data, p)) {
				ctx->light3D[0] = p[0];
				ctx->light3D[1] = p[1];
				ctx->light3D[2] = p[2];
				ctx->hasLight3D = true;
			}
		}

		const double shadeStarted=SL_Milliseconds();
		ERR(SL_CoreRender(in_data, out_data, pixel_format, input_worldP, output_worldP, ctx));
		perf.shadeMs+=SL_Milliseconds()-shadeStarted;
	}
	} catch(const std::bad_alloc&) {err=PF_Err_OUT_OF_MEMORY;}
	catch(...) {err=PF_Err_INTERNAL_STRUCT_DAMAGED;}

	if(inputChecked) ERR2(extraP->cb->checkin_layer_pixels(in_data->effect_ref, SL_INPUT));
	if (normal_worldP) {
		ERR2(extraP->cb->checkin_layer_pixels(in_data->effect_ref, SL_NORMAL_LAYER));
	}
	if (depth_worldP) {
		ERR2(extraP->cb->checkin_layer_pixels(in_data->effect_ref, SL_DEPTH_LAYER));
	}
	return err;
}

/* ---------------------------------------------------------------- */

extern "C" DllExport
PF_Err PluginDataEntryFunction(
	PF_PluginDataPtr inPtr,
	PF_PluginDataCB inPluginDataCallBackPtr,
	SPBasicSuite* inSPBasicSuitePtr,
	const char* inHostName,
	const char* inHostVersion)
{
	(void)inSPBasicSuitePtr; (void)inHostName; (void)inHostVersion;
	PF_Err result = PF_Err_INVALID_CALLBACK;
	result = PF_REGISTER_EFFECT(
		inPtr,
		inPluginDataCallBackPtr,
		SL_MENU_NAME,
		SL_MATCHNAME,
		"SanLight",
		AE_RESERVED_INFO);
	return result;
}

PF_Err
EffectMain(
	PF_Cmd cmd,
	PF_InData* in_dataP,
	PF_OutData* out_data,
	PF_ParamDef* params[],
	PF_LayerDef* output,
	void* extra)
{
	PF_Err err = PF_Err_NONE;
	try {
		switch (cmd) {
			case PF_Cmd_SEQUENCE_SETUP:
			case PF_Cmd_SEQUENCE_RESETUP:
			case PF_Cmd_SEQUENCE_FLATTEN:
			case PF_Cmd_GET_FLATTENED_SEQUENCE_DATA:
			case PF_Cmd_SEQUENCE_SETDOWN:
				err = SL_SequenceCommand(cmd, in_dataP, out_data);
				break;
			case PF_Cmd_ABOUT:
				err = About(in_dataP, out_data, params, output);
				break;
			case PF_Cmd_GLOBAL_SETUP:
				err = GlobalSetup(in_dataP, out_data, params, output);
				break;
			case PF_Cmd_PARAMS_SETUP:
				err = ParamsSetup(in_dataP, out_data, params, output);
				break;
			case PF_Cmd_UPDATE_PARAMS_UI:
			case PF_Cmd_USER_CHANGED_PARAM:
				err = SL_UpdateMapUI(in_dataP, params);
				break;
			case PF_Cmd_SMART_PRE_RENDER:
				err = PreRender(in_dataP, out_data, (PF_PreRenderExtra*)extra);
				break;
			case PF_Cmd_SMART_RENDER:
				err = SmartRender(in_dataP, out_data, (PF_SmartRenderExtra*)extra);
				break;
			default:
				break;
		}
	} catch (PF_Err& thrown_err) {
		err = thrown_err;
	} catch (...) {
		err = PF_Err_INTERNAL_STRUCT_DAMAGED;
	}
	return err;
}
