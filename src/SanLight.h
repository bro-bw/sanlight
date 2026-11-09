/*******************************************************************/
/* SanLight (三渲二光源) — Normality 式法线/深度重光照              */
/*                                                                 */
/* 输入:  法线贴图层 (Lotus-normal 等, 标准法线编码)                */
/*        深度贴图层 (Lotus-depth, 白=近 黑=远)                     */
/* 光源:  内置点光源 X/Y (可拖拽点) + Z 轴 + 强度/颜色/衰减半径,    */
/*        或跟随合成里的 AE 灯光层 (MadLight/Normality 同款思路)    */
/* 输出:  漫反射/高光/轮廓光 明暗 -> 内置色阶 (AE Levels 语义)      */
/*        -> 仅光影/正片叠底/叠加/柔光/相加 五种应用方式            */
/*                                                                 */
/* SmartFX, 8/16/32 bpc, 多线程安全 (逐像素无状态)。                */
/*******************************************************************/

#ifndef SANLIGHT_H
#define SANLIGHT_H

#include "AEConfig.h"
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AEFX_SuiteHelper.h"
#include "String_Utils.h"
#include "AEGP_SuiteHandler.h"

#ifdef AE_OS_WIN
	#include <Windows.h>
#endif

#define SL_NAME			"三渲二光源 SanLight"
#define SL_DESCRIPTION	"法线+深度贴图重光照 (Normality 式)。\r点光源 X/Y/Z + 强度/衰减, 内置色阶压硬边光影。"

/* 双构建: 中文本地化 AE (<26.2) 按 GBK 读取参数名/菜单名;
   AE 26.2+ 按 UTF-8。两个构建的 match name 不同, 可共存。 */
#ifdef SL_UTF8_BUILD
	#define SL_MENU_NAME	"SanLight v1.3 UTF8"
	#define SL_MATCHNAME	"San Light 1.3 UTF8"
#else
	#define SL_MENU_NAME	"SanLight v1.3"
	#define SL_MATCHNAME	"San Light 1.3"
#endif

#define SL_MAJOR_VERSION	1
#define SL_MINOR_VERSION	3
#define SL_BUG_VERSION		0
#define SL_STAGE_VERSION	3			/* released */
#define SL_BUILD_VERSION	23

/* 参数索引 (必须与 ParamsSetup 添加顺序一致; 分组头/尾也占位) */
enum {
	SL_DISK_INPUT = 0,

	/* ---- 贴图 ---- */
	SL_DISK_GRP_MAPS,
	SL_DISK_NORMAL_LAYER,
	SL_DISK_DEPTH_LAYER,
	SL_DISK_DEPTH_INVERT,
	SL_DISK_NORMAL_Y_UP,
	SL_DISK_DEPTH_RANGE,
	SL_DISK_END_MAPS,

	/* ---- 内置 AI ---- */
	SL_DISK_GRP_AI,
	SL_DISK_AI_SOURCE,
	SL_DISK_AI_QUALITY,
	SL_DISK_AI_SMOOTH,
	SL_DISK_AI_STRENGTH,
	SL_DISK_END_AI,

	/* ---- 点光源 ---- */
	SL_DISK_GRP_LIGHT,
	SL_DISK_LIGHT_MODE,
	SL_DISK_LIGHT_POS,
	SL_DISK_LIGHT_Z,
	SL_DISK_LIGHT_INTENSITY,
	SL_DISK_LIGHT_COLOR,
	SL_DISK_LIGHT_FALLOFF,
	SL_DISK_END_LIGHT,

	/* ---- 明暗 ---- */
	SL_DISK_GRP_SHADING,
	SL_DISK_AMBIENT,
	SL_DISK_DIFFUSE,
	SL_DISK_SPECULAR,
	SL_DISK_SHININESS,
	SL_DISK_RIM,
	SL_DISK_SOFTNESS,
	SL_DISK_END_SHADING,

	/* ---- 色阶 (AE Levels 语义) ---- */
	SL_DISK_GRP_LEVELS,
	SL_DISK_LV_IN_BLACK,
	SL_DISK_LV_GAMMA,
	SL_DISK_LV_IN_WHITE,
	SL_DISK_LV_OUT_BLACK,
	SL_DISK_LV_OUT_WHITE,
	SL_DISK_END_LEVELS,

	/* ---- 输出 ---- */
	SL_DISK_GRP_OUTPUT,
	SL_DISK_VIEW,
	SL_DISK_APPLY,
	SL_DISK_OPACITY,
	SL_DISK_END_OUTPUT,
	SL_DISK_GRP_BACKLIGHT,
	SL_DISK_BACKLIGHT_ENABLED,
	SL_DISK_BACKLIGHT_START,
	SL_DISK_BACKLIGHT_DISTANCE,
	SL_DISK_END_BACKLIGHT,
	SL_DISK_GRP_EDGE,
	SL_DISK_EDGE_KEEP,
	SL_DISK_EDGE_WIDTH,
	SL_DISK_END_EDGE,
	SL_DISK_GRP_TEMPORAL = 120,
	SL_DISK_TEMPORAL_ENABLED,
	SL_DISK_TEMPORAL_STRENGTH,
	SL_DISK_TEMPORAL_RESPONSE,
	SL_DISK_TEMPORAL_ALIGNMENT,
	SL_DISK_END_TEMPORAL,
	SL_DISK_TEMPORAL_ROTATION = 126,
	SL_DISK_PERSISTENT_CACHE = 127,

	SL_DISK_NUM_PARAMS
};

// Runtime indices follow the UI; disk IDs above remain stable for saved projects.
enum {
	SL_INPUT = 0,
	SL_GRP_MAPS,
	SL_AI_SOURCE,
	SL_AI_QUALITY,
	SL_AI_SMOOTH,
	SL_AI_STRENGTH,
	SL_NORMAL_LAYER,
	SL_DEPTH_LAYER,
	SL_DEPTH_INVERT,
	SL_NORMAL_Y_UP,
	SL_DEPTH_RANGE,
	SL_END_MAPS,
	SL_GRP_LIGHT,
	SL_LIGHT_MODE,
	SL_LIGHT_POS,
	SL_LIGHT_Z,
	SL_LIGHT_INTENSITY,
	SL_LIGHT_COLOR,
	SL_LIGHT_FALLOFF,
	SL_GRP_BACKLIGHT,
	SL_BACKLIGHT_ENABLED,
	SL_BACKLIGHT_START,
	SL_BACKLIGHT_DISTANCE,
	SL_END_BACKLIGHT,
	SL_GRP_EDGE,
	SL_EDGE_KEEP,
	SL_EDGE_WIDTH,
	SL_END_EDGE,
	SL_END_LIGHT,
	SL_GRP_SHADING,
	SL_AMBIENT,
	SL_DIFFUSE,
	SL_SPECULAR,
	SL_SHININESS,
	SL_RIM,
	SL_SOFTNESS,
	SL_END_SHADING,
	SL_GRP_LEVELS,
	SL_LV_IN_BLACK,
	SL_LV_GAMMA,
	SL_LV_IN_WHITE,
	SL_LV_OUT_BLACK,
	SL_LV_OUT_WHITE,
	SL_END_LEVELS,
	SL_GRP_OUTPUT,
	SL_VIEW,
	SL_APPLY,
	SL_OPACITY,
	SL_END_OUTPUT,
	SL_GRP_TEMPORAL,
	SL_TEMPORAL_ENABLED,
	SL_TEMPORAL_STRENGTH,
	SL_TEMPORAL_RESPONSE,
	SL_TEMPORAL_ALIGNMENT,
	SL_TEMPORAL_ROTATION,
	SL_PERSISTENT_CACHE,
	SL_END_TEMPORAL,
	SL_NUM_PARAMS
};

/* AE 唯一入口 (必须以未修饰名字导出) */
extern "C" {
	DllExport
	PF_Err
	EffectMain(
		PF_Cmd			cmd,
		PF_InData*		in_data,
		PF_OutData*		out_data,
		PF_ParamDef*	params[],
		PF_LayerDef*	output,
		void*			extra);
}

#endif /* SANLIGHT_H */
