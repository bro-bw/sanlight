#ifdef SL_BASELINE_BUILD5
#include "../../../../../versions/v1.3-build5/src/SL_Surface.h"
#else
#include "../../src/SL_Surface.h"
#endif
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <string>

int wmain(int argc,wchar_t** argv) {
    if(argc!=3&&argc!=4)return 1;FILE *images=nullptr,*surfaces=nullptr;
    _wfopen_s(&images,argv[1],L"rb");_wfopen_s(&surfaces,argv[2],L"rb");if(!images||!surfaces)return 2;
    unsigned dims[3],other[3];float fps,otherFps;
    if(fread(dims,4,3,images)!=3||fread(&fps,4,1,images)!=1||fread(other,4,3,surfaces)!=3||fread(&otherFps,4,1,surfaces)!=1)return 3;
    if(dims[0]!=other[0]||dims[1]!=other[1]||dims[2]!=other[2]||fps!=otherFps||dims[2]<2)return 4;
    int w=dims[0],h=dims[1],frames=dims[2];if(w<1||h<1||w>4096||h>4096||fps<=0)return 5;
    size_t n=size_t(w)*h;std::vector<unsigned char> bytes(n*4);std::vector<float> image(n*4),state(n*4),previousImage,previousState,initial;
    auto start=std::chrono::steady_clock::now();
    for(int t=0;t<=2*(frames-1);t++) {
        int k=t<frames?t:2*(frames-1)-t;
        _fseeki64(images,16+(__int64)k*n*4,SEEK_SET);_fseeki64(surfaces,16+(__int64)k*n*16,SEEK_SET);
        if(fread(bytes.data(),4,n,images)!=n||fread(state.data(),4,n*4,surfaces)!=n*4)return 6;
        for(size_t i=0;i<n;i++){float a=bytes[i*4+3]/255.f;for(int c=0;c<3;c++)image[i*4+c]=bytes[i*4+c]/255.f*a+.5f*(1-a);image[i*4+3]=a;}
        if(!t)initial=state;
        else if(!SL_TemporalSurface(previousImage.data(),image.data(),previousState.data(),state.data(),w,h,.85f,.2f,true,1/fps))return 7;
        previousImage=image;previousState=state;
        if(t%15==0){printf("Round-trip step %d/%d, source frame %d\n",t,2*(frames-1),k);fflush(stdout);}
    }
    std::vector<float> angles;double depth=0;
    for(int y=8;y<h-8;y++)for(int x=8;x<w-8;x++) {
        size_t i=size_t(y)*w+x;if(image[4*i+3]<.99f)continue;
        float dot=0,aa=0,bb=0;
        for(int c=0;c<3;c++){float a=initial[n+3*i+c]*2-1,b=state[n+3*i+c]*2-1;dot+=a*b;aa+=a*a;bb+=b*b;}
        dot=dot/std::sqrt(aa*bb);angles.push_back(std::acos(std::max(-1.f,std::min(1.f,dot)))*57.2957795f);
        depth+=std::fabs(state[i]-initial[i]);
    }
    if(angles.empty())return 8;std::sort(angles.begin(),angles.end());double mean=0;for(float angle:angles)mean+=angle;
    printf("Same-pose return: normal mean %.6f deg, median %.6f deg, p90 %.6f deg, p99 %.6f deg; depth MAE %.9f; %.2fs\n",
        mean/angles.size(),angles[angles.size()/2],angles[size_t(angles.size()*.9)],angles[size_t(angles.size()*.99)],depth/angles.size(),std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
    if(argc==4) {
        FILE* result=nullptr;std::wstring path=std::wstring(argv[3])+L"\\roundtrip-surfaces.bin";
        _wfopen_s(&result,path.c_str(),L"wb");if(!result)return 10;
        unsigned header[3]={unsigned(w),unsigned(h),2};
        bool ok=fwrite(header,4,3,result)==3&&fwrite(&fps,4,1,result)==1&&
            fwrite(initial.data(),4,n*4,result)==n*4&&fwrite(state.data(),4,n*4,result)==n*4;
        fclose(result);if(!ok)return 11;
    }
    fclose(images);fclose(surfaces);
    return mean/angles.size()<5&&angles[size_t(angles.size()*.9)]<10?0:9;
}
