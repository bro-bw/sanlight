#include "../../src/SL_AI.h"
#include "../../src/SL_Temporal.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <algorithm>

float sample(const float* p,int w,int h,int ch,float x,float y,int c) {
    if(x<0||y<0||x>w-1||y>h-1)return c==3?0:.5f;
    int ix=int(x),iy=int(y),jx=std::min(ix+1,w-1),jy=std::min(iy+1,h-1);float tx=x-ix,ty=y-iy;
    return (p[((size_t)iy*w+ix)*ch+c]*(1-tx)+p[((size_t)iy*w+jx)*ch+c]*tx)*(1-ty)+
        (p[((size_t)jy*w+ix)*ch+c]*(1-tx)+p[((size_t)jy*w+jx)*ch+c]*tx)*ty;
}
struct Frame{std::vector<float> rgba,depth,normal;};
int main(){
    FILE* f=nullptr;int dims[2];if(fopen_s(&f,"probe_input.bin","rb"))return 1;
    if(fread(dims,sizeof(int),2,f)!=2)return 2;
    std::vector<float> full((size_t)dims[0]*dims[1]*4);if(fread(full.data(),sizeof(float),full.size(),f)!=full.size())return 3;fclose(f);
    const int w=192,h=248;const size_t n=(size_t)w*h;
    std::vector<Frame> frames;
    auto start=std::chrono::steady_clock::now();
    for(int t=0;t<7;t++) {
        Frame frame;frame.rgba.resize(n*4);
        for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
            float local=std::exp(-std::pow((y-95.f)/40.f,2))*std::max(0.f,1.f-x/150.f);
            float rx=x-t*.8f-t*.6f*local,ry=y+t*.3f*local;
            for(int k=0;k<4;k++)frame.rgba[((size_t)y*w+x)*4+k]=sample(full.data(),dims[0],dims[1],4,
                (rx+.5f)*dims[0]/w-.5f,(ry+.5f)*dims[1]/h-.5f,k);
        }
        float *depth=nullptr,*normal=nullptr;
        if(SL_AI_Generate(frame.rgba.data(),w,h,3,10,1000,&depth,&normal))return 4;
        frame.depth.assign(depth,depth+n);frame.normal.assign(normal,normal+n*3);free(depth);free(normal);
        frames.push_back(std::move(frame));
    }
    std::vector<Frame> stable;
    for(int t=0;t<7;t++) {
        Frame result=frames[std::max(0,t-2)];
        for(int k=std::max(0,t-2)+1;k<=t;k++) {
            Frame next=frames[k];
            if(!SL_TemporalRefine(result.rgba.data(),next.rgba.data(),result.depth.data(),result.normal.data(),
                next.depth.data(),next.normal.data(),w,h,.65f,.6f,true))return 5;
            result=std::move(next);
        }stable.push_back(std::move(result));
    }
    double rawChange=0,stableChange=0,rawNormal=0,stableNormal=0;int count=0;
    for(int t=1;t<7;t++) {
        SL_TrackResult motion;
        if(!SL_TrackDepth(frames[t-1].rgba.data(),frames[t].rgba.data(),frames[t-1].depth.data(),w,h,256,.25f,motion))return 6;
        for(int y=8;y<h-8;y++)for(int x=8;x<w-8;x++) {
            size_t i=(size_t)y*w+x;float rx=x+motion.flowX[i],ry=y+motion.flowY[i];
            if(frames[t].rgba[i*4+3]<.95f||motion.confidence[i]<.8f||rx<1||ry<1||rx>w-2||ry>h-2)continue;
            double a=frames[t].depth[i]-sample(frames[t-1].depth.data(),w,h,1,rx,ry,0);
            double b=stable[t].depth[i]-sample(stable[t-1].depth.data(),w,h,1,rx,ry,0);
            rawChange+=a*a;stableChange+=b*b;
            for(int k=0;k<3;k++) {
                a=frames[t].normal[i*3+k]-sample(frames[t-1].normal.data(),w,h,3,rx,ry,k);
                b=stable[t].normal[i*3+k]-sample(stable[t-1].normal.data(),w,h,3,rx,ry,k);
                rawNormal+=a*a;stableNormal+=b*b;
            }count++;
        }
    }
    if(!count)return 7;
    printf("Actual model + synthetic Puppet-style deformation: matched samples=%d\n",count);
    printf("Adjacent depth change RMS raw=%.6f stabilized=%.6f; normal raw=%.6f stabilized=%.6f\n",
        std::sqrt(rawChange/count),std::sqrt(stableChange/count),std::sqrt(rawNormal/count/3),std::sqrt(stableNormal/count/3));
    printf("7-frame test seconds=%.3f\n",std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
    if(stableChange>rawChange||stableNormal>rawNormal)return 8;
    puts("PASS actual AI model produces current maps on each frame; adjacent variation reduced");
}
