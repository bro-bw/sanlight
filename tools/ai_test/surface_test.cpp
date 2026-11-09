#include "../../src/SL_Surface.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

const int w=80,h=88;
const size_t n=size_t(w)*h;
struct V {float x,y,z;};
V normalized(V p) {float s=1/std::sqrt(p.x*p.x+p.y*p.y+p.z*p.z);return {p.x*s,p.y*s,p.z*s};}
V normal(const std::vector<float>& p,size_t i) {return normalized({p[n+i*3]*2-1,p[n+i*3+1]*2-1,p[n+i*3+2]*2-1});}
void setNormal(std::vector<float>& p,size_t i,V v) {v=normalized(v);p[n+i*3]=v.x*.5f+.5f;p[n+i*3+1]=v.y*.5f+.5f;p[n+i*3+2]=v.z*.5f+.5f;}
float normalError(V a,V b) {float x=a.x-b.x,y=a.y-b.y,z=a.z-b.z;return x*x+y*y+z*z;}
struct Frame {std::vector<float> image,surface,truth;};
Frame makeFrame(int frame,int mode) {
    Frame f;f.image.resize(n*4);f.surface.resize(n*4);f.truth.resize(n*4);
    float shift=mode?1.7f*std::sin(frame*.09f):0;
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
        size_t i=size_t(y)*w+x;
        float sx=x-shift,sy=float(y);
        if(mode>=2) {sx-=.9f*std::sin(y*.06f)*std::sin(frame*.11f);sy-=.7f*std::sin(x*.07f)*std::sin(frame*.08f);}
        float alpha=sx>7&&sx<73&&sy>7&&sy<81?1.f:0.f;
        float texture=mode==0?.001f:.12f;
        for(int c=0;c<3;c++)f.image[i*4+c]=alpha*(.5f+texture*std::sin(sx*(.09f+c*.07f)+sy*(.19f-c*.02f)))+(1-alpha)*.5f;
        // A small non-geometric color fluctuation avoids the identical-image shortcut.
        f.image[i*4]+=.0002f*std::sin(frame*1.41f);
        // This local blink causes match confidence to fall without changing the material surface.
        if(mode==3&&x>30&&x<51&&y>27&&y<59)f.image[i*4+1]+=.19f*std::sin(frame*.8f);
        f.image[i*4+3]=alpha;
        f.truth[i]=.5f+.08f*std::sin(sx*.075f)+.06f*std::cos(sy*.07f);
        V truth={.5f*std::sin(sx*.08f),.4f*std::cos(sy*.085f),.8f};
        setNormal(f.truth,i,truth);
        f.surface[i]=f.truth[i]+(frame?.08f*std::sin(frame*1.37f+sx*.075f):0.f);
        if(frame) {truth.x+=.55f*std::sin(frame*.83f+sx*.08f);truth.y+=.4f*std::cos(frame*1.3f+sy*.1f);}
        setNormal(f.surface,i,truth);
    }
    return f;
}
bool stop(void*) {return true;}
int main() {
    for(int mode=0;mode<4;mode++) {
        auto previous=makeFrame(0,mode);auto stable=previous.truth;
        double rawError=0,stableError=0,rawDepth=0,stableDepth=0;size_t samples=0;
        float largestMeanStep=0;int last=mode==1?180:70;
        for(int t=1;t<last;t++) {
            auto next=makeFrame(t,mode),filtered=next;
            if(!SL_TemporalSurface(previous.image.data(),next.image.data(),stable.data(),filtered.surface.data(),w,h,.85f,.2f,true,1.f/30))return 1;
            double frameStep=0;size_t count=0;
            for(int y=18;y<h-18;y++)for(int x=18;x<w-18;x++) {
                size_t i=size_t(y)*w+x;
                rawError+=normalError(normal(next.surface,i),normal(next.truth,i));
                stableError+=normalError(normal(filtered.surface,i),normal(next.truth,i));
                float a=next.surface[i]-next.truth[i],b=filtered.surface[i]-next.truth[i];
                rawDepth+=a*a;stableDepth+=b*b;
                frameStep+=normalError(normal(filtered.surface,i),normal(stable,i));++count;++samples;
            }
            largestMeanStep=std::max(largestMeanStep,float(std::sqrt(frameStep/count)));
            stable=std::move(filtered.surface);previous=std::move(next);
        }
        printf("mode %d (%d frames): normal RMSE %.6f -> %.6f; depth %.6f -> %.6f; maximum frame normal step %.6f\n",mode,last,std::sqrt(rawError/samples),std::sqrt(stableError/samples),std::sqrt(rawDepth/samples),std::sqrt(stableDepth/samples),largestMeanStep);
        if(stableError>rawError*.25||stableDepth>rawDepth*.4||largestMeanStep>.12f)return 2+mode;
        if(mode==0 && (std::sqrt(stableError/samples)>.01||largestMeanStep>.004f))return 20;
    }
    auto a=makeFrame(0,0),b=makeFrame(1,0);auto stable=a.truth;
    for(int t=0;t<2000;t++) {
        auto candidate=b.surface;
        if(!SL_TemporalSurface(a.image.data(),a.image.data(),stable.data(),candidate.data(),w,h,.85f,.2f,true,1.f/30))return 10;
        stable=std::move(candidate);
    }
    if(stable!=a.truth)return 11;
    auto disabled=b.surface;
    if(!SL_TemporalSurface(a.image.data(),b.image.data(),a.truth.data(),disabled.data(),w,h,0,.2f,true,1.f/30)||disabled!=b.surface)return 12;
    if(SL_TemporalSurface(a.image.data(),b.image.data(),a.truth.data(),disabled.data(),w,h,1,.2f,true,1.f/30,stop))return 13;
    // A real full-frame appearance cut must not drag the old surface into the new shot.
    for(size_t i=0;i<n;i++)for(int c=0;c<3;c++)b.image[i*4+c]=0.f;
    auto cut=b.surface;
    if(!SL_TemporalSurface(a.image.data(),b.image.data(),a.truth.data(),cut.data(),w,h,.85f,.2f,false,1.f/30)||cut!=b.surface)return 14;
    // Revealed pixels use the current surface; opaque neighbors do not sample background normals.
    b=makeFrame(1,0);
    for(int y=25;y<45;y++)for(int x=34;x<44;x++) {
        size_t i=size_t(y)*w+x;a.image[i*4+3]=0;a.truth[i]=1;setNormal(a.truth,i,{1,0,.01f});
    }
    auto reveal=b.surface;
    if(!SL_TemporalSurface(a.image.data(),b.image.data(),a.truth.data(),reveal.data(),w,h,.85f,.2f,false,1.f/30))return 15;
    if(reveal[35*w+38]!=b.surface[35*w+38])return 16;
    if(normalError(normal(reveal,35*w+33),normal(a.truth,35*w+33))>.00001f)return 17;
    // The angular correction budget is specified in seconds, including subframes.
    a=makeFrame(0,0);b=a;
    for(size_t i=0;i<n;i++) {b.image[i*4]+=.001f;setNormal(a.truth,i,{0,0,1});setNormal(b.surface,i,{1,0,.01f});}
    for(float seconds:{1.f/24,1.f/30,1.f/60,1.f/120}) {
        auto candidate=b.surface;
        if(!SL_TemporalSurface(a.image.data(),b.image.data(),a.truth.data(),candidate.data(),w,h,.85f,.2f,false,seconds))return 18;
        float angle=std::atan2(normal(candidate,40*w+40).x,normal(candidate,40*w+40).z);
        if(angle>.045f*seconds+1e-6f||angle<.04f*seconds)return 19;
    }
    puts("PASS transported normals: low texture, motion, local deformation, confidence changes, long drift, static hold, disabled, cuts, reveal, alpha gaps, angular rate, cancellation");
    return 0;
}
