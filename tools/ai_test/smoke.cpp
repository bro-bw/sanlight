#include "../../src/SL_AI.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
int main() {
    float pixels[16 * 16 * 4];
    for (int i = 0; i < 16 * 16; ++i) {
        pixels[i*4] = (i % 16) / 15.f;
        pixels[i*4+1] = (i / 16) / 15.f;
        pixels[i*4+2] = 0.5f;
        pixels[i*4+3] = 1.f;
    }
    float *a = NULL, *b = NULL, *c = NULL, *d = NULL;
    if (SL_AI_Generate(pixels, 16, 16, 0, 2, 100.f, &a, &b)) {
        printf("Inference failed: %s\n", SL_AI_LastError()); return 1;
    }
    if (SL_AI_Generate(pixels, 16, 16, 0, 2, 100.f, &c, &d)) return 2;
    if (a == c || b == d || memcmp(a, c, 256*sizeof(float)) || memcmp(b, d, 768*sizeof(float))) return 3;
    for (int i=0; i<256; ++i) if (!isfinite(a[i]) || a[i]<0 || a[i]>1) return 4;
    free(c); free(d);
    if (SL_AI_Generate(pixels, 1, 1, 0, 2, 100.f, &c, &d)) return 5;
    for (int i=0; i<768; ++i) if (!isfinite(b[i])) return 6;
    free(a); free(b); free(c); free(d);
    puts("PASS: model inference, independent cached outputs, retained buffers, single pixel input");
    return 0;
}
