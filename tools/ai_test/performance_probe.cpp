#include "../../src/SL_AI.h"
#include "../../src/SL_Surface.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>

using Clock=std::chrono::steady_clock;
void report(const char* stage,Clock::time_point start) {
    printf("%s: %.3f seconds\n",stage,std::chrono::duration<double>(Clock::now()-start).count());fflush(stdout);
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=4)return 1;
    int w=_wtoi(argv[2]),h=_wtoi(argv[3]);if(w<1||h<1||w>4096||h>4096)return 2;
    FILE* f=nullptr;_wfopen_s(&f,argv[1],L"rb");if(!f)return 3;
    unsigned dims[3];float fps;
    if(fread(dims,4,3,f)!=3||fread(&fps,4,1,f)!=1||dims[2]<2)return 4;
    int sw=dims[0],sh=dims[1];size_t n=size_t(w)*h;
    std::vector<unsigned char> bytes(size_t(sw)*sh*4);
    std::vector<float> image[2];
    for(auto& im:image) {
        if(fread(bytes.data(),1,bytes.size(),f)!=bytes.size())return 5;
        im.resize(n*4);
        for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
            float xx=std::max(0.f,std::min(float(sw-1),(x+.5f)*sw/w-.5f));
            float yy=std::max(0.f,std::min(float(sh-1),(y+.5f)*sh/h-.5f));
            int x0=int(xx),y0=int(yy);float tx=xx-x0,ty=yy-y0,rgba[4]={};
            for(int dy=0;dy<2;dy++)for(int dx=0;dx<2;dx++) {
                size_t j=size_t(std::min(sh-1,y0+dy))*sw+std::min(sw-1,x0+dx);
                float weight=(dx?tx:1-tx)*(dy?ty:1-ty)/255.f;
                for(int c=0;c<4;c++)rgba[c]+=bytes[j*4+c]*weight;
            }
            size_t i=size_t(y)*w+x;
            for(int c=0;c<3;c++)im[4*i+c]=rgba[c]*rgba[3]+.5f*(1-rgba[3]);
            im[4*i+3]=rgba[3];
        }
    }
    fclose(f);printf("Working canvas %dx%d, checkpoint %.1f MiB\n",w,h,n*32./1048576.);fflush(stdout);
    auto start=Clock::now();if(!SL_AI_Available())return 6;report("Model initialization",start);
    std::vector<float> states[2];
    for(int t=0;t<3;t++) {
        float *d=nullptr,*norm=nullptr;int frame=t==2?1:0;
        start=Clock::now();
        if(SL_AI_Generate(image[frame].data(),w,h,3,10,1000.f*h/2447,&d,&norm))return 7;
        report(t==0?"First AI surface":t==1?"Repeated AI cache":"Next AI surface",start);
        states[frame].resize(n*4);memcpy(states[frame].data(),d,n*4);memcpy(states[frame].data()+n,norm,n*12);
        free(d);free(norm);
    }
    start=Clock::now();SL_TrackResult track;
    if(!SL_TrackDepth(image[0].data(),image[1].data(),states[0].data(),w,h,768,.35f,track,nullptr,nullptr,true,true,true))return 8;
    report("Motion only",start);track=SL_TrackResult{};
    start=Clock::now();
    if(!SL_TemporalSurface(image[0].data(),image[1].data(),states[0].data(),states[1].data(),w,h,.85f,.2f,true,1/fps))return 9;
    report("Whole temporal surface including motion",start);
    if(argc==4) {
        FILE* output=nullptr;_wfopen_s(&output,L"performance-state.bin",L"wb");if(!output)return 10;
        bool ok=fwrite(states[1].data(),4,n*4,output)==n*4;fclose(output);if(!ok)return 11;
    }
    return 0;
}
