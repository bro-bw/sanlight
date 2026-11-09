/*******************************************************************/
/* SL_AI — 内置单目深度模型 (Depth Anything v2 small, ONNX)          */
/*                                                                 */
/* 动态加载插件目录 SanLight_AI\onnxruntime.dll + .onnx 模型,        */
/* 输入合成像素 -> 相对逆深度 -> 近度图 + 梯度法线图,               */
/* 带单条目缓存 (输入哈希), 线程安全。任何失败 → SL_AI_Available()  */
/* 返回 false, 插件回退到用户指定的贴图层。                          */
/*******************************************************************/
#ifndef SL_AI_H
#define SL_AI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 (幂等, 线程安全)。成功后 SL_AI_Available() 为真。 */
void SL_AI_Init(void);

/* 模型是否可用 (DLL + 模型都加载成功) */
int  SL_AI_Available(void);

/* Content identity of the loaded model/runtime; false disables persistent reuse. */
int SL_AI_CacheIdentity(uint64_t outIdentity[4]);
/* Thread policy used when opening a model session. Does not initialize ONNX. */
int SL_AI_ThreadCount(void);
/* Report the selected backend without initializing a session. */
int SL_AI_UsingGPU(void);
const char* SL_AI_BackendInfo(void);

/* 对合成后的 RGBA (0..1, 预乘与否皆可, alpha 用于忽略透明区) 图像推理。
   w,h: 图像尺寸;  quality: 0=280 1=392 2=518 3=700;
   smooth: 深度高斯平滑半径 px (0..10);
   输出 (均为层尺寸 w*h, float, 调用方独占并负责 free):
     outNear: 近度 0..1 (1=近) — 白=近 语义
     outNorm: 法线图, 每像素 3 通道 (R,G,B 已编码 0..1, G-up 惯例)
   normStrength: 法线梯度强度倍率 (典型 50..300, 默认 100)
   返回 0 成功, 非 0 失败。内部缓存: 同一模型输入+参数避免重复推理；
   每次成功调用都返回独立缓冲区，调用方负责 free。 */
int SL_AI_Generate(const float* rgba /*w*h*4*/, int w, int h,
				   int quality, int smooth, float normStrength,
				   float** outNear, float** outNorm);

/* Optional persistent cache of raw model output, before alpha normalization.
   inferenceRan is set to 1 only when ONNX Run is actually attempted, else 0.
   Derived maps are always rebuilt for the current alpha/size/smoothing/strength.
   The original Generate API never creates persistent cache files. */
int SL_AI_GenerateCached(const float* rgba,int w,int h,int quality,int smooth,float normStrength,
    float** outNear,float** outNorm,int persistentCache,int* inferenceRan);

/* Rebuild geometry-derived normals after temporal depth refinement. */
void SL_AI_RebuildNormals(const float* nearMap, const float* rgba, int w, int h,
    float strength, float* outNorm);

/* 最后一次错误信息 (调试用) */
const char* SL_AI_LastError(void);

#ifdef __cplusplus
}
#endif

#endif /* SL_AI_H */
