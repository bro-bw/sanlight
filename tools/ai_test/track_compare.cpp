#include "../../src/SL_Track.h"
#include <cstdio>
#include <chrono>
#include <cmath>
bool SL_TrackDepth_Build5(const float*,const float*,const float*,int,int,int,float,SL_TrackResult&,SL_TrackAbort,void*,bool);
int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 1;FILE* file=nullptr;_wfopen_s(&file,argv[1],L"rb");if(!file)return 2;
    unsigned dims[3];float fps;fread(dims,4,3,file);fread(&fps,4,1,file);
    int w=dims[0],h=dims[1];if(w<1||h<1||w>4096||h>4096)return 3;size_t n=size_t(w)*h;
    std::vector<float> a(n*4),b(n*4),depth(n,.5f);std::vector<unsigned char> bytes(n*4);
    for(auto* image:{&a,&b}) {
        if(fread(bytes.data(),4,n,file)!=n)return 4;
        for(size_t i=0;i<n;i++) {float alpha=bytes[i*4+3]/255.f;for(int c=0;c<3;c++)(*image)[4*i+c]=bytes[4*i+c]/255.f*alpha+.5f*(1-alpha);(*image)[4*i+3]=alpha;}
    }
    fclose(file);SL_TrackResult old,current;
    auto start=std::chrono::steady_clock::now();
    if(!SL_TrackDepth_Build5(a.data(),b.data(),depth.data(),w,h,768,.35f,old,nullptr,nullptr,true))return 5;
    auto middle=std::chrono::steady_clock::now();
    if(!SL_TrackDepth(a.data(),b.data(),depth.data(),w,h,768,.35f,current,nullptr,nullptr,true,false))return 6;
    auto end=std::chrono::steady_clock::now();
    printf("Build5 %.6fs; current %.6fs; exact result %s\n",std::chrono::duration<double>(middle-start).count(),std::chrono::duration<double>(end-middle).count(),
        old.flowX==current.flowX&&old.flowY==current.flowY&&old.confidence==current.confidence&&old.depth==current.depth?"PASS":"FAIL");
    double displacement=0;float maxDisplacement=0;size_t count=0;
    for(size_t i=0;i<n;i++)if(b[i*4+3]>.9f) {
        float delta=std::hypot(current.flowX[i]-old.flowX[i],current.flowY[i]-old.flowY[i]);
        displacement+=delta;maxDisplacement=std::max(maxDisplacement,delta);++count;
    }
    printf("Foreground displacement difference: mean %.9f px, max %.9f px\n",displacement/std::max(size_t(1),count),maxDisplacement);
    return old.flowX==current.flowX&&old.flowY==current.flowY&&old.confidence==current.confidence&&old.depth==current.depth?0:7;
}
