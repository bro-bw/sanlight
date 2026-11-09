#include "../../src/SL_Surface.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

bool SL_TrackDepthBuild8Memory(const float*,const float*,const float*,int,int,int,float,
    SL_TrackResult&,SL_TrackAbort,void*,bool,bool,bool);
bool SL_TemporalSurfaceBuild8Memory(const float*,const float*,const float*,float*,int,int,
    float,float,bool,float,SL_TrackAbort,void*,bool);

struct Frame {std::vector<float> image,surface;};
static Frame make(int w,int h,int time) {
    size_t n=size_t(w)*h;Frame f;f.image.resize(n*4);f.surface.resize(n*4);
    const float alphas[]={0.f,.02f,.049f,.05f,.2f,.5f,.99f,1.f};
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
        size_t i=size_t(y)*w+x;float xx=x-.7f*std::sin(time*.18f+y*.03f),yy=y-.4f*std::sin(time*.11f+x*.02f);
        float alpha=x<3||y<3||x+3>=w||y+3>=h?alphas[(x+y)%8]:1.f;
        if(x>w/3&&x<w/3+4)alpha=alphas[(y+time)%8];
        for(int c=0;c<3;c++)f.image[i*4+c]=.5f+alpha*.18f*std::sin(xx*(.07f+c*.03f)+yy*(.09f+c*.01f));
        f.image[i*4]+=.0002f*std::sin(time*.43f);
        if(time==4&&x>w/2&&y>h/2)f.image[i*4+1]+=.13f;
        f.image[i*4+3]=alpha;
        f.surface[i]=.5f+.12f*std::sin(xx*.06f+time*.8f)+.07f*std::cos(yy*.05f-time*.7f);
        float nx=.6f*std::sin(xx*.04f+time*.3f),ny=.5f*std::cos(yy*.07f-time*.23f),nz=.8f;
        float inv=1.f/std::sqrt(nx*nx+ny*ny+nz*nz);
        f.surface[n+i*3]=.5f+.5f*nx*inv;f.surface[n+i*3+1]=.5f+.5f*ny*inv;f.surface[n+i*3+2]=.5f+.5f*nz*inv;
    }
    return f;
}
static bool identical(const std::vector<float>& a,const std::vector<float>& b) {
    return a.size()==b.size()&&(a.empty()||!memcmp(a.data(),b.data(),a.size()*sizeof(float)));
}
static bool stopped(void*) {return true;}
int main() {
    const int sizes[][2]={{19,17},{80,88},{257,129},{271,811}};
    size_t compared=0;
    for(const auto& size:sizes) {
        int w=size[0],h=size[1];auto a=make(w,h,0),b=make(w,h,1);
        SL_TrackResult expected,actual,noDepth;
        if(!SL_TrackDepthBuild8Memory(a.image.data(),b.image.data(),a.surface.data(),w,h,768,.35f,
            expected,nullptr,nullptr,true,true,true))return 1;
        if(!SL_TrackDepth(a.image.data(),b.image.data(),a.surface.data(),w,h,768,.35f,
            actual,nullptr,nullptr,true,true,true))return 2;
        if(!identical(expected.depth,actual.depth)||!identical(expected.flowX,actual.flowX)||
            !identical(expected.flowY,actual.flowY)||!identical(expected.confidence,actual.confidence))return 3;
        noDepth=actual;
        if(!SL_TrackDepth(a.image.data(),b.image.data(),nullptr,w,h,768,.35f,
            noDepth,nullptr,nullptr,true,true,true,false))return 4;
        if(!noDepth.depth.empty()||noDepth.depth.capacity()||!identical(expected.flowX,noDepth.flowX)||
            !identical(expected.flowY,noDepth.flowY)||!identical(expected.confidence,noDepth.confidence))return 5;
        if(!SL_TrackDepth(a.image.data(),a.image.data(),nullptr,w,h,768,.35f,
            noDepth,nullptr,nullptr,true,true,true,false))return 6;
        if(!noDepth.depth.empty()||noDepth.depth.capacity())return 7;
        for(size_t i=0;i<noDepth.flowX.size();i++)
            if(noDepth.flowX[i]!=0.f||noDepth.flowY[i]!=0.f||noDepth.confidence[i]!=1.f)return 8;
        for(bool align:{false,true})for(bool rotate:{false,true})for(float strength:{.25f,.85f}) {
            auto previous=a;auto oldState=a.surface,newState=a.surface;
            const int frames=w>260?3:8;
            for(int time=1;time<=frames;time++) {
                auto next=make(w,h,time);
                auto oldNext=next.surface,newNext=next.surface;
                float seconds=time%2?1.f/30:1.f/120;
                bool oldOk=SL_TemporalSurfaceBuild8Memory(previous.image.data(),next.image.data(),oldState.data(),oldNext.data(),
                    w,h,strength,.2f,align,seconds,nullptr,nullptr,rotate);
                bool newOk=SL_TemporalSurface(previous.image.data(),next.image.data(),newState.data(),newNext.data(),
                    w,h,strength,.2f,align,seconds,nullptr,nullptr,rotate);
                if(!oldOk||!newOk||!identical(oldNext,newNext)) {
                    printf("Mismatch %dx%d frame %d align %d rotate %d strength %.2f\n",w,h,time,align,rotate,strength);return 9;
                }
                ++compared;oldState=std::move(oldNext);newState=std::move(newNext);previous=std::move(next);
            }
        }
        if(SL_TrackDepth(a.image.data(),b.image.data(),nullptr,w,h,768,.35f,
            noDepth,stopped,nullptr,true,true,true,false))return 10;
        printf("PASS %dx%d: default-depth compatibility, motion-only output, static/cancel, alpha thresholds and temporal sequence\n",w,h);
    }
    printf("PASS %zu temporal outputs byte-identical to frozen build8; four full-resolution float planes removed from Surface working set\n",compared);
    return 0;
}
