#include "SL_Temporal.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <cstring>

namespace {
float clamp(float v,float low,float high) {return std::max(low,std::min(high,v));}
float smooth(float a,float b,float x) {
    float t=clamp((x-a)/(b-a),0,1);return t*t*(3-2*t);
}
float sample(const float* p,int w,int h,int channels,float x,float y,int channel) {
    x=clamp(x,0.f,float(w-1));y=clamp(y,0.f,float(h-1));
    int x0=int(x),y0=int(y),x1=std::min(x0+1,w-1),y1=std::min(y0+1,h-1);
    float tx=x-x0,ty=y-y0;
    float a=p[(size_t(y0)*w+x0)*channels+channel],b=p[(size_t(y0)*w+x1)*channels+channel];
    float c=p[(size_t(y1)*w+x0)*channels+channel],d=p[(size_t(y1)*w+x1)*channels+channel];
    return (a+(b-a)*tx)*(1-ty)+(c+(d-c)*tx)*ty;
}
void normal(float n[3]) {
    float length=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
    if(length<1e-6f) {n[0]=n[1]=0;n[2]=1;}
    else for(int k=0;k<3;k++)n[k]/=length;
}
}

bool SL_TemporalRefine(const float* previousRGBA,const float* currentRGBA,
    const float* previousDepth,const float* previousNormal,
    float* currentDepth,float* currentNormal,int w,int h,
    float strength,float response,bool alignMotion,SL_TrackAbort abort,void* abortData) {
    if(!previousRGBA||!currentRGBA||!previousDepth||!previousNormal||!currentDepth||!currentNormal||w<1||h<1)return false;
    strength=clamp(strength,0,1);response=clamp(response,0,1);
    if(abort && abort(abortData))return false;
    if(strength==0)return true;
    const size_t count=(size_t)w*h;
    if(!std::memcmp(previousDepth,currentDepth,count*sizeof(float)) &&
       !std::memcmp(previousNormal,currentNormal,count*3*sizeof(float)))return true;
    SL_TrackResult tracked;
    if(alignMotion && !SL_TrackDepth(previousRGBA,currentRGBA,previousDepth,w,h,
        384,.25f,tracked,abort,abortData))return false;
    std::vector<float> weight(count),px(count),py(count);
    double errorSum=0,confidenceSum=0;size_t visible=0;
    for(int y=0;y<h;y++) {
        if(abort && abort(abortData))return false;
        for(int x=0;x<w;x++) {
            const size_t i=(size_t)y*w+x;
            const float rx=x+(alignMotion?tracked.flowX[i]:0),ry=y+(alignMotion?tracked.flowY[i]:0);
            px[i]=rx;py[i]=ry;
            if(currentRGBA[i*4+3]<.05f||rx<0||ry<0||rx>w-1||ry>h-1)continue;
            const float oldAlpha=sample(previousRGBA,w,h,4,rx,ry,3);
            float residual=0;
            for(int k=0;k<3;k++) {
                float d=currentRGBA[i*4+k]-sample(previousRGBA,w,h,4,rx,ry,k);
                residual+=d*d/3;
            }
            residual=std::sqrt(residual);
            const float visibility=smooth(.05f,.7f,oldAlpha)*
                (1-smooth(.12f,.5f,std::fabs(oldAlpha-currentRGBA[i*4+3])));
            const float confidence=alignMotion ? tracked.confidence[i] : 1;
            const float displacement=std::hypot(rx-x,ry-y)/std::max(w,h);
            weight[i]=visibility*confidence*(1-smooth(.035f,.18f,residual))*
                (1-smooth(.08f,.2f,displacement));
            errorSum+=residual;confidenceSum+=weight[i];++visible;
        }
    }
    // A cut or loss of correspondence must not import the old scene's geometry.
    if(!visible||errorSum/visible>.18||confidenceSum/visible<.08)return true;
    // Estimate a bounded scale/offset only from well-matched opaque surfaces.
    // Relative monocular depth can change its global scale without a pose change.
    double sw=0,sx=0,sy=0,sxx=0,syy=0,sxy=0;
    for(size_t i=0;i<count;i++) {
        if(weight[i]<.5f || currentRGBA[i*4+3]<.9f)continue;
        double x=currentDepth[i],y=alignMotion?tracked.depth[i]:previousDepth[i],v=weight[i];
        sw+=v;sx+=v*x;sy+=v*y;sxx+=v*x*x;syy+=v*y*y;sxy+=v*x*y;
    }
    float scale=1,offset=0;
    if(sw>=32) {
        double vx=sxx-sx*sx/sw,vy=syy-sy*sy/sw,cov=sxy-sx*sy/sw;
        if(vx>1e-6 && vy>1e-6 && cov/std::sqrt(vx*vy)>.9) {
            scale=clamp(float(cov/vx),.8f,1.25f);
            offset=clamp(float((sy-scale*sx)/sw),-.1f,.1f);
        }
    }
    const float history=.96f*strength*(1-.25f*response);
    for(int y=0;y<h;y++) {
        if(abort && abort(abortData))return false;
        for(int x=0;x<w;x++) {
            const size_t i=(size_t)y*w+x;
            if(weight[i]<1e-6f)continue;
            const float oldDepth=alignMotion?tracked.depth[i]:previousDepth[i];
            const float alignedDepth=clamp(currentDepth[i]*scale+offset,0,1);
            const float change=std::fabs(alignedDepth-oldDepth);
            const float depthGate=1-smooth(.12f+.08f*(1-response),.35f+.15f*(1-response),change);
            const float reliability=weight[i]*depthGate;
            const float depthWeight=history*reliability;
            // Apply scale correction, not merely use it to decide history confidence.
            const float corrected=currentDepth[i]+(alignedDepth-currentDepth[i])*strength*reliability;
            const float delta=corrected-oldDepth;
            const float quiet=.003f+.012f*(1-response);
            const float innovation=smooth(quiet,quiet*4,std::fabs(delta));
            // Uncertain/newly exposed pixels retain the current estimate.
            const float quietGate=1-strength*reliability*(1-innovation);
            const float stabilized=oldDepth+delta*(1-depthWeight)*quietGate;
            currentDepth[i]=clamp(stabilized,0,1);
        }
    }
    return true;
}
