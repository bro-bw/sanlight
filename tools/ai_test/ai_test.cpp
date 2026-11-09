/* SL_AI 离线链路测试: 合成球体 RGBA -> SL_AI_Generate -> 断言 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "../../src/SL_AI.h"

int main(void) {
	const int W = 512, H = 512;
	float* buf = (float*)malloc((size_t)W * H * 4 * sizeof(float));

	/* 球体 + 平背景 (与 preview.py 的场景一致), 合成到 0.82 灰底 */
	const float cx = W * 0.48f, cy = H * 0.45f, r = W * 0.30f;
	for (int y = 0; y < H; ++y) {
		for (int x = 0; x < W; ++x) {
			const float dx = (x - cx) / r, dy = (y - cy) / r;
			const float d2 = dx * dx + dy * dy;
			const int inside = d2 <= 1.f;
			const float shade = inside ? 1.f - 0.5f * dy : 1.f;	/* 简单上下渐变给模型一点结构 */
			float* o = buf + ((size_t)y * W + x) * 4;
			if (inside) {
				o[0] = 0.86f * shade; o[1] = 0.72f * shade; o[2] = 0.62f * shade; o[3] = 1.f;
			} else {
				o[0] = o[1] = o[2] = 0.82f; o[3] = 1.f;
			}
		}
	}

	SL_AI_Init();
	if (!SL_AI_Available()) {
		printf("AI NOT AVAILABLE: %s\n", SL_AI_LastError());
		free(buf);
		return 2;
	}
	printf("AI init ok (%s)\n", SL_AI_LastError());

	float *near = NULL, *norm = NULL;
	const int rc = SL_AI_Generate(buf, W, H, 1, 2, 100.f, &near, &norm);
	if (rc != 0 || !near || !norm) {
		printf("GENERATE FAILED rc=%d: %s\n", rc, SL_AI_LastError());
		free(buf);
		return 3;
	}

	/* 断言 1: 球心近度 > 角落背景近度 */
	const float c = near[(int)(cy * W + cx)];
	float corners = 0.f;
	const int m = 40;
	corners += near[(size_t)m * W + m] + near[(size_t)m * W + (W - m)];
	corners += near[(size_t)(H - m) * W + m] + near[(size_t)(H - m) * W + (W - m)];
	corners *= 0.25f;
	printf("nearness sphere=%.3f bg=%.3f\n", c, corners);
	if (!(c > corners + 0.05f)) { printf("FAIL: sphere not nearer\n"); return 4; }

	/* 断言 2: 法线图中心接近 (0.5,0.5,>0.5) (朝镜头), 边缘偏离 */
	const float* nc = norm + ((size_t)(int)cy * W + (int)cx) * 3;
	printf("normal center R=%.3f G=%.3f B=%.3f\n", nc[0], nc[1], nc[2]);
	if (fabsf(nc[0] - 0.5f) > 0.15f || fabsf(nc[1] - 0.5f) > 0.15f || nc[2] < 0.55f) {
		printf("FAIL: center normal not facing viewer\n"); return 5;
	}
	const float* ne = norm + ((size_t)(int)cy * W + (int)(cx - r * 0.9f)) * 3;
	printf("normal left-edge R=%.3f G=%.3f B=%.3f\n", ne[0], ne[1], ne[2]);
	if (fabsf(ne[0] - 0.5f) < 0.04f) {
		printf("FAIL: edge normal not tilted\n"); return 6;
	}

	/* Cached results must have equal contents and independent ownership. */
	float *n2 = NULL, *m2 = NULL;
	if (SL_AI_Generate(buf, W, H, 1, 2, 100.f, &n2, &m2) != 0 || n2 == near || m2 == norm ||
		memcmp(n2, near, (size_t)W * H * sizeof(float)) ||
		memcmp(m2, norm, (size_t)W * H * 3 * sizeof(float))) {
		printf("FAIL: cache miss\n"); return 7;
	}
	printf("cache hit OK\n");

	printf("ALL AI TESTS PASSED\n");
	free(near); free(norm); free(n2); free(m2);
	free(buf);
	return 0;
}
