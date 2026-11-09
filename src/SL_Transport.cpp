#include "SL_Temporal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
float bound(float v,float a,float b) {return std::max(a,std::min(b,v));}
float ramp(float a,float b,float x) {float t=bound((x-a)/(b-a),0,1);return t*t*(3-2*t);}
float sample(const float* p,int w,int h,int channels,float x,float y,int c) {
    x=bound(x,0,float(w-1));y=bound(y,0,float(h-1));
    int x0=int(x),y0=int(y),x1=std::min(x0+1,w-1),y1=std::min(y0+1,h-1);
    float tx=x-x0,ty=y-y0;
    float a=p[(size_t(y0)*w+x0)*channels+c],b=p[(size_t(y0)*w+x1)*channels+c];
    float d=p[(size_t(y1)*w+x0)*channels+c],e=p[(size_t(y1)*w+x1)*channels+c];
    return (a+(b-a)*tx)*(1-ty)+(d+(e-d)*tx)*ty;
}
}

bool SL_TemporalDepth(const float* oldImage,const float* image,const float* oldDepth,
    float* depth,int w,int h,float strength,float response,bool align,float seconds,
    SL_TrackAbort abort,void* abortData) {
    if(!oldImage||!image||!oldDepth||!depth||w<1||h<1)return false;
    if(abort&&abort(abortData))return false;
    strength=bound(strength,0,1);response=bound(response,0,1);
    if(strength==0)return true;
    size_t n=size_t(w)*h;
    if(!std::memcmp(oldImage,image,n*4*sizeof(float))) {
        const float hold=ramp(0,.5f,strength);
        for(size_t i=0;i<n;i++)depth[i]=oldDepth[i]*hold+depth[i]*(1-hold);
        return true;
    }
    seconds=bound(seconds,.001f,1.f);
    SL_TrackResult flow;
    if(align&&!SL_TrackDepth(oldImage,image,oldDepth,w,h,768,.15f,flow,abort,abortData))return false;
    std::vector<float> trust(n),motion(n),transport(n),correction(n);
    double residualSum=0,trustSum=0;size_t visible=0;
    for(int y=0;y<h;y++) {
        if(abort&&abort(abortData))return false;
        for(int x=0;x<w;x++) {
            size_t i=size_t(y)*w+x;
            float rx=x+(align?flow.flowX[i]:0),ry=y+(align?flow.flowY[i]:0);
            transport[i]=align?flow.depth[i]:oldDepth[i];
            if(image[i*4+3]<.05f||rx<0||ry<0||rx>w-1||ry>h-1)continue;
            float a=sample(oldImage,w,h,4,rx,ry,3),residual=0;
            for(int c=0;c<3;c++) {
                float d=image[i*4+c]-sample(oldImage,w,h,4,rx,ry,c);
                residual+=d*d/3;
            }
            residual=std::sqrt(residual);
            motion[i]=std::hypot(rx-x,ry-y)/std::max(w,h);
            float correspondence=align?flow.confidence[i]:1;
            trust[i]=ramp(.05f,.85f,a)*(1-ramp(.1f,.45f,std::fabs(a-image[i*4+3])))*
                ramp(.2f,.8f,correspondence)*(1-ramp(.04f,.2f,residual))*
                (1-ramp(.08f,.25f,motion[i]));
            residualSum+=residual;trustSum+=trust[i];++visible;
        }
    }
    if(!visible||residualSum/visible>.18||trustSum/visible<.08)return true;
    // Robust two-pass affine alignment rejects local shape changes/outliers.
    float scale=1,offset=0;
    for(int pass=0;pass<2;pass++) {
        double sw=0,sx=0,sy=0,sxx=0,syy=0,sxy=0;
        for(size_t i=0;i<n;i++) {
            if(trust[i]<.7f||image[i*4+3]<.9f)continue;
            double x=depth[i],y=transport[i],weight=trust[i];
            if(pass)weight*=1-ramp(.025f,.12f,std::fabs(float(x)*scale+offset-float(y)));
            sw+=weight;sx+=weight*x;sy+=weight*y;sxx+=weight*x*x;syy+=weight*y*y;sxy+=weight*x*y;
        }
        if(sw<32)break;
        double vx=sxx-sx*sx/sw,vy=syy-sy*sy/sw,cov=sxy-sx*sy/sw;
        if(vx<=1e-6||vy<=1e-6||cov/std::sqrt(vx*vy)<.85)break;
        scale=bound(float(cov/vx),.67f,1.5f);offset=bound(float((sy-scale*sx)/sw),-.2f,.2f);
    }
    for(size_t i=0;i<n;i++)correction[i]=bound(depth[i]*scale+offset,0,1)-transport[i];
    // Smooth the innovation field, never the carried shape itself. Gaps are barriers.
    std::vector<float> horizontal(n),filtered(n);
    for(int axis=0;axis<2;axis++)for(int y=0;y<h;y++) {
        if(abort&&abort(abortData))return false;
        for(int x=0;x<w;x++) {
            size_t i=size_t(y)*w+x;const auto& source=axis?horizontal:correction;
            float sum=source[i],weights=1;
            for(int direction : {-1,1})for(int d=1;d<=3;d++) {
                int xx=x+(axis?0:direction*d),yy=y+(axis?direction*d:0);
                if(xx<0||yy<0||xx>=w||yy>=h)break;
                size_t j=size_t(yy)*w+xx;
                if(image[j*4+3]<.05f)break;
                float color=0;for(int c=0;c<3;c++)color+=std::fabs(image[i*4+c]-image[j*4+c]);
                float weight=std::exp(-d*d/8.f-color*8.f)*trust[j];
                sum+=source[j]*weight;weights+=weight;
            }
            (axis?filtered:horizontal)[i]=sum/weights;
        }
    }
    std::vector<float> candidateCorrection(n),allowance(n);
    for(size_t i=0;i<n;i++) {
        if((i%size_t(w))==0&&abort&&abort(abortData))return false;
        float matched=trust[i];if(matched<=0)continue;
        allowance[i]=((.006f+.06f*response)*seconds+motion[i]*(.08f+.5f*response))*(1.15f-strength);
        float quiet=(.001f+.005f*(1-response))*(1-ramp(.001f,.02f,motion[i]));
        float delta=filtered[i]*ramp(quiet,quiet+.015f,std::fabs(filtered[i]));
        candidateCorrection[i]=bound(delta,-allowance[i],allowance[i]);
    }
    // Limit changes in slope as well as depth: lighting depends on the gradient.
    // This operates on AI innovation only, not the genuine deformation in the flow.
    const float slopeStep=(.0002f+.001f*response)*seconds*(1.15f-strength);
    for(int pass=0;pass<4;pass++)for(int row=0;row<h;row++) {
        if(abort&&abort(abortData))return false;
        int y=(pass&1)?h-1-row:row;
        for(int column=0;column<w;column++) {
            int x=(pass&1)?w-1-column:column;size_t i=size_t(y)*w+x;
            if(trust[i]<.7f||image[i*4+3]<.5f)continue;
            for(int axis=0;axis<2;axis++) {
                int xx=x+(axis?0:((pass&1)?1:-1)),yy=y+(axis?((pass&1)?1:-1):0);
                if(xx<0||yy<0||xx>=w||yy>=h)continue;
                size_t j=size_t(yy)*w+xx;
                if(trust[j]<.7f||image[j*4+3]<.5f)continue;
                candidateCorrection[i]=bound(candidateCorrection[i],candidateCorrection[j]-slopeStep,candidateCorrection[j]+slopeStep);
            }
            candidateCorrection[i]=bound(candidateCorrection[i],-allowance[i],allowance[i]);
        }
    }
    for(size_t i=0;i<n;i++) {
        if((i%size_t(w))==0&&abort&&abort(abortData))return false;
        float matched=trust[i];if(matched<=0)continue;
        float candidate=bound(transport[i]+candidateCorrection[i],0,1);
        // A trusted match owns the geometry. AI is bounded correction, not a new shape.
        float hold=ramp(0,.5f,strength)*ramp(.15f,.7f,matched);
        depth[i]=bound(candidate*hold+depth[i]*(1-hold),0,1);
    }
    return true;
}
