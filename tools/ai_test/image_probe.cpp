#include "../../src/SL_AI.h"
#include <stdio.h>
#include <stdlib.h>
#include <vector>
int main(int argc, char**) {
    FILE* f = NULL;
    if (fopen_s(&f, "probe_input.bin", "rb")) return 1;
    int dims[2];
    if (fread(dims, sizeof(int), 2, f) != 2) return 2;
    size_t n = (size_t)dims[0] * dims[1];
    std::vector<float> input(n*4);
    if (fread(input.data(), sizeof(float), n*4, f) != n*4) return 3;
    fclose(f);
    float *nearMap = NULL, *normal = NULL;
    if (SL_AI_Generate(input.data(), dims[0], dims[1], argc > 1 ? 3 : 1, argc > 1 ? 10 : 2, argc > 1 ? 1000.f : 100.f, &nearMap, &normal)) {
        puts(SL_AI_LastError()); return 4;
    }
    if (fopen_s(&f, "probe_output.bin", "wb")) return 5;
    fwrite(nearMap, sizeof(float), n, f);
    fwrite(normal, sizeof(float), n*3, f);
    fclose(f); free(nearMap); free(normal);
    puts("Image inference complete"); return 0;
}
