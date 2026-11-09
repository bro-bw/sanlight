#include "SL_Surface.h"
#ifdef SANLIGHT_DIS_MOTION
#include "SL_DenseMotion.h"
#endif
#include "SL_Parallel.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <xmmintrin.h>

namespace {
float clamp(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
float smooth(float lo, float hi, float v) {
    float t = clamp((v - lo) / (hi - lo), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
struct Vec {
    float x, y, z;
    Vec operator+(Vec v) const { return {x + v.x, y + v.y, z + v.z}; }
    Vec operator-(Vec v) const { return {x - v.x, y - v.y, z - v.z}; }
    Vec operator*(float s) const { return {x * s, y * s, z * s}; }
};
float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec unit(Vec v) {
    float d = std::sqrt(dot(v, v));
    return d > 1e-8f ? v * (1.f / d) : Vec{0.f, 0.f, 1.f};
}
Vec decode(const float* p) { return unit({p[0]*2.f-1.f, p[1]*2.f-1.f, p[2]*2.f-1.f}); }
void encode(Vec v, float* p) {
    v = unit(v);
    p[0] = v.x*.5f+.5f; p[1] = v.y*.5f+.5f; p[2] = v.z*.5f+.5f;
}
float cubic(float a, float b, float c, float d, float t) {
    return b + .5f*t*(c-a+t*(2.f*a-5.f*b+4.f*c-d+t*(3.f*(b-c)+d-a)));
}
void sampleRGBA(const float* image, int w, int h, float x, float y, float (&out)[4]) {
    x=clamp(x,0.f,float(w-1)); y=clamp(y,0.f,float(h-1));
    int x0=int(x),y0=int(y),x1=std::min(x0+1,w-1),y1=std::min(y0+1,h-1);
    float tx=x-x0,ty=y-y0;
    const float* a=image+(size_t(y0)*w+x0)*4;
    const float* b=image+(size_t(y0)*w+x1)*4;
    const float* d=image+(size_t(y1)*w+x0)*4;
    const float* e=image+(size_t(y1)*w+x1)*4;
    for(int c=0;c<4;++c) out[c]=(a[c]+(b[c]-a[c])*tx)*(1.f-ty)+(d[c]+(e[c]-d[c])*tx)*ty;
}
struct SurfaceSample { float depth = 0, coverage = 0; Vec normal{0,0,1}; };

// Depth and the three normal channels share sampling coordinates. Keep every
// lane's operation order identical to the scalar cubic expression above.
__m128 cubic4(__m128 a,__m128 b,__m128 c,__m128 d,__m128 t) {
    auto inner=_mm_sub_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(3.f),_mm_sub_ps(b,c)),d),a);
    auto middle=_mm_sub_ps(_mm_add_ps(_mm_sub_ps(_mm_mul_ps(_mm_set1_ps(2.f),a),_mm_mul_ps(_mm_set1_ps(5.f),b)),_mm_mul_ps(_mm_set1_ps(4.f),c)),d);
    middle=_mm_add_ps(middle,_mm_mul_ps(t,inner));
    auto outer=_mm_add_ps(_mm_sub_ps(c,a),_mm_mul_ps(t,middle));
    return _mm_add_ps(b,_mm_mul_ps(_mm_mul_ps(_mm_set1_ps(.5f),t),outer));
}
__m128 choose4(__m128 mask,__m128 yes,__m128 no) {
    return _mm_or_ps(_mm_and_ps(mask,yes),_mm_andnot_ps(mask,no));
}
// std::min/max keep their first argument for unordered comparisons and ties.
__m128 minimum4(__m128 a,__m128 b){return choose4(_mm_cmplt_ps(b,a),b,a);}
__m128 maximum4(__m128 a,__m128 b){return choose4(_mm_cmplt_ps(a,b),b,a);}
__m128 surface4(const float* surface,size_t n,size_t j) {
    const float* normal=surface+n+j*3;
    return _mm_setr_ps(surface[j],normal[0],normal[1],normal[2]);
}

SurfaceSample sampleSurface(const float* surface, const float* image, int w, int h, float x, float y) {
    SurfaceSample result;
    if(x<0.f||y<0.f||x>float(w-1)||y>float(h-1))return result;
    const size_t n=size_t(w)*h;
    int x0=int(x),y0=int(y); float tx=x-x0,ty=y-y0;
    float values[4]={0,0,0,0};
    bool cubicSafe=x0>=1&&y0>=1&&x0+2<w&&y0+2<h;
    if(cubicSafe)for(int yy=y0-1;yy<=y0+2;yy++)for(int xx=x0-1;xx<=x0+2;xx++)
        if(image[(size_t(yy)*w+xx)*4+3]<.99f)cubicSafe=false;
    if(cubicSafe) {
        // Limited cubic advection avoids the repeated bilinear blur of frame-to-frame transport.
        __m128 rows[4],minimum=_mm_set1_ps(1e30f),maximum=_mm_set1_ps(-1e30f);
        const auto xFraction=_mm_set1_ps(tx);
        for(int row=0;row<4;row++) {
            const size_t j=size_t(y0+row-1)*w+x0-1;
            const auto a=surface4(surface,n,j),b=surface4(surface,n,j+1),c=surface4(surface,n,j+2),d=surface4(surface,n,j+3);
            if(row==1||row==2) {
                minimum=minimum4(minimum,b);minimum=minimum4(minimum,c);
                maximum=maximum4(maximum,b);maximum=maximum4(maximum,c);
            }
            rows[row]=cubic4(a,b,c,d,xFraction);
        }
        const auto sampled=cubic4(rows[0],rows[1],rows[2],rows[3],_mm_set1_ps(ty));
        _mm_storeu_ps(values,maximum4(minimum,minimum4(maximum,sampled)));
        result.coverage=1.f;
    } else {
        auto channels=_mm_setzero_ps();
        for(int dy=0;dy<2;dy++)for(int dx=0;dx<2;dx++) {
            int xx=std::min(x0+dx,w-1),yy=std::min(y0+dy,h-1);size_t j=size_t(yy)*w+xx;
            float weight=(dx?tx:1.f-tx)*(dy?ty:1.f-ty)*clamp(image[j*4+3],0.f,1.f);
            result.coverage+=weight;
            channels=_mm_add_ps(channels,_mm_mul_ps(surface4(surface,n,j),_mm_set1_ps(weight)));
        }
        if(result.coverage>1e-6f)channels=_mm_div_ps(channels,_mm_set1_ps(result.coverage));
        _mm_storeu_ps(values,channels);
    }
    result.depth=values[0];result.normal=decode(values+1);return result;
}

Vec rotateToward(Vec previous, Vec target, float angleLimit, float blend) {
    float cosine=clamp(dot(previous,target),-1.f,1.f);
    // At zero step the existing path normalizes the previous normal, except
    // at angle < 1e-6 (the float cosine is exactly one in that range).
    if(angleLimit==0.f&&blend==0.f)return cosine==1.f?previous:unit(previous);
    float angle=std::acos(cosine);
    if(angle<1e-6f)return previous;
    float step=std::min(angle*blend,angleLimit);
    Vec tangent=target-previous*cosine;
    float length=std::sqrt(dot(tangent,tangent));
    if(length<1e-6f) {
        // The normal convention usually keeps Z positive; still handle antipodal input.
        tangent=std::fabs(previous.x)<.8f ? Vec{1,0,0}-previous*previous.x : Vec{0,1,0}-previous*previous.y;
    }
    tangent=unit(tangent);
    return unit(previous*std::cos(step)+tangent*std::sin(step));
}
}

bool SL_TrackSurfaceMotion(const float* previous,const float* current,int w,int h,
    SL_TrackResult& flow,SL_TrackAbort abort,void* abortData) {
#ifdef SANLIGHT_DIS_MOTION
    return SL_DenseMotion(previous,current,w,h,flow,abort,abortData);
#else
    return SL_TrackDepth(previous,current,nullptr,w,h,768,.35f,flow,abort,abortData,true,true,true,false,.05f);
#endif
}

bool SL_TemporalSurface(const float* oldImage,const float* image,const float* oldSurface,
    float* surface,int w,int h,float strength,float response,bool align,float seconds,
    SL_TrackAbort abort,void* abortData,bool rotateNormals,SL_TrackResult* preparedFlow) {
    if(!oldImage||!image||!oldSurface||!surface||w<1||h<1)return false;
    if(abort&&abort(abortData))return false;
    strength=clamp(strength,0.f,1.f);response=clamp(response,0.f,1.f);
    if(strength==0.f)return true;
    const size_t n=size_t(w)*h;
    const float ownership=smooth(0.f,.5f,strength);
    if(!std::memcmp(oldImage,image,n*4*sizeof(float))) {
        if(ownership==1.f)std::copy(oldSurface,oldSurface+n*4,surface);
        else for(size_t i=0;i<n;i++) {
            surface[i]=oldSurface[i]*ownership+surface[i]*(1.f-ownership);
            encode(unit(decode(oldSurface+n+i*3)*ownership+decode(surface+n+i*3)*(1.f-ownership)),surface+n+i*3);
        }
        return true;
    }
    seconds=clamp(seconds,.00001f,1.f);
    SL_TrackResult localFlow;
    SL_TrackResult& flow=preparedFlow?*preparedFlow:localFlow;
    if(align&&!preparedFlow&&!SL_TrackSurfaceMotion(oldImage,image,w,h,flow,abort,abortData))return false;

    // Refine the tracker's arrays in place; transported scalar depth is not used here.
    std::vector<float> confidence=align?std::move(flow.confidence):std::vector<float>(n);
    std::vector<float> flowX=align?std::move(flow.flowX):std::vector<float>(n);
    std::vector<float> flowY=align?std::move(flow.flowY):std::vector<float>(n);
    std::vector<float> innovation(n,1.f);
    double residualSum=0,matchedSum=0,visible=0,overlap=0;
    for(int y=0;y<h;y++) {
        if(abort&&abort(abortData))return false;
        for(int x=0;x<w;x++) {
            size_t i=size_t(y)*w+x;
            if(image[i*4+3]<.05f) {
                confidence[i]=flowX[i]=flowY[i]=0.f;
                continue;
            }
            float dx=flowX[i],dy=flowY[i];
            float rx=x+dx,ry=y+dy;
            float oldPixel[4];
            sampleRGBA(oldImage,w,h,rx,ry,oldPixel);
            float oldAlpha=oldPixel[3],residual=0.f;
            for(int c=0;c<3;c++) {
                float d=image[i*4+c]-oldPixel[c];residual+=d*d/3.f;
            }
            residual=std::sqrt(residual);
            // When adjacent motion already explains the pixels, a new AI shape is not
            // evidence of a material change. Do not integrate that noise indefinitely.
            if(align)innovation[i]=smooth(.01f,.05f,residual);
            float reliability=align?confidence[i]:1.f;
            confidence[i]=reliability*std::exp(-residual*10.f)*smooth(.05f,.8f,oldAlpha);
            bool inside=rx>=0.f&&ry>=0.f&&rx<=w-1&&ry<=h-1;
            residualSum+=residual;matchedSum+=confidence[i];visible+=1;
            if(inside&&oldAlpha>.5f)overlap+=1;
        }
    }
    // A confidence dip is not a scene cut. Reset only when source appearance also changes substantially.
    if(visible==0)return true;
    if((residualSum/visible>.30&&matchedSum/visible<.12)||
        (overlap/visible<.08&&residualSum/visible>.15))return true;

    if(align) {
        // Fill unreliable motion from nearby motion, never from a new AI shape.
        // Keeping this regularization on displacement avoids differentiating flow noise into normals.
        const int radius=std::max(1,int(std::max(w,h)/256.f));
        auto filteredX=flowX,filteredY=flowY;
        if(!SL_ParallelRows(h,[&](int y) {
            for(int x=0;x<w;x++) {
                size_t i=size_t(y)*w+x;if(image[i*4+3]<.05f)continue;
                float sumX=0,sumY=0,weights=0;
                for(int dy=-2;dy<=2;dy++)for(int dx=-2;dx<=2;dx++) {
                    int xx=x+dx*radius,yy=y+dy*radius;
                    if(xx<0||yy<0||xx>=w||yy>=h)continue;
                    size_t j=size_t(yy)*w+xx;if(image[j*4+3]<.05f)continue;
                    float difference=0;
                    for(int c=0;c<3;c++)difference+=std::fabs(image[i*4+c]-image[j*4+c]);
                    float weight=std::exp(-.25f*(dx*dx+dy*dy)-difference*5.f)*(.01f+confidence[j]*confidence[j]);
                    sumX+=flowX[j]*weight;sumY+=flowY[j]*weight;weights+=weight;
                }
                if(weights>0.f) {
                    float blend=.2f+.8f*(1.f-smooth(.15f,.85f,confidence[i]));
                    filteredX[i]=flowX[i]*(1.f-blend)+sumX/weights*blend;
                    filteredY[i]=flowY[i]*(1.f-blend)+sumY/weights*blend;
                }
            }
        },abort,abortData))return false;
        flowX.swap(filteredX);flowY.swap(filteredY);
    }

    const float rateScale=1.15f-strength;
    const float depthLimit=(.004f+.04f*response)*rateScale*seconds;
    const float angleLimit=(.03f+.6f*response)*rateScale*seconds;
    const float convergence=1.f-std::exp(-(.15f+1.5f*response)*rateScale*seconds);
    auto rotationAt=[&](int x,int y,int radius,float& reliability) {
        reliability=0.f;
        if(x<radius||y<radius||x+radius>=w||y+radius>=h)return 0.f;
        const size_t left=size_t(y)*w+x-radius,right=left+radius*2;
        const size_t top=size_t(y-radius)*w+x,bottom=top+size_t(radius*2)*w;
        for(size_t j:{left,right,top,bottom})if(image[j*4+3]<.95f)return 0.f;
        reliability=std::min(std::min(confidence[left],confidence[right]),std::min(confidence[top],confidence[bottom]));
        float inv=1.f/(radius*2);
        float xx=(flowX[right]-flowX[left])*inv,xy=(flowX[bottom]-flowX[top])*inv;
        float yx=(flowY[right]-flowY[left])*inv,yy=(flowY[bottom]-flowY[top])*inv;
        // Rotation of the backward map. Encoded normal Y is up, reversing the image-plane sign.
        return std::atan2(yx-xy,2.f+xx+yy);
    };
    if(!SL_ParallelRows(h,[&](int y) {
        for(int x=0;x<w;x++) {
            size_t i=size_t(y)*w+x;
            if(image[i*4+3]<.05f)continue;
            auto previous=sampleSurface(oldSurface,oldImage,w,h,x+flowX[i],y+flowY[i]);
            // An opaque old sample may belong to another part after disocclusion.
            // Release its geometry only when both motion and appearance disagree.
            float materialOwnership=ownership;
            if(align) {
                float original[4];sampleRGBA(oldImage,w,h,x+flowX[i],y+flowY[i],original);
                float mismatch=0;
                for(int c=0;c<3;c++){float delta=original[c]-image[4*i+c];mismatch+=delta*delta/3.f;}
                mismatch=std::sqrt(mismatch);
                const float rejection=smooth(.12f,.32f,mismatch)*(1.f-smooth(.2f,.55f,confidence[i]));
                materialOwnership*=1.f-rejection;
            }
            if(align&&flow.historyReject.size()==n)
                materialOwnership*=1.f-smooth(.65f,.95f,flow.historyReject[i]);
            if(previous.coverage<.1f)continue; // Truly newly visible material has no previous surface.
            if(align&&rotateNormals) {
                const int radius=std::max(8,std::max(w,h)/48);
                float confidenceA=0,confidenceB=0;
                float a=rotationAt(x,y,radius,confidenceA),b=rotationAt(x,y,radius*2,confidenceB);
                // Only coherent rotation at two spatial scales may rotate the carried normals.
                // Do not apply noisy local stretch/shear to the normal slopes.
                float weight=smooth(.25f,.75f,std::min(confidenceA,confidenceB))*(1.f-smooth(.004f,.03f,std::fabs(a-b)));
                weight*=smooth(.0015f,.004f,std::fabs((a+b)*.5f));
                float angle=clamp((a+b)*.5f,-3.f*seconds,3.f*seconds)*weight;
                float c=std::cos(angle),s=std::sin(angle);
                previous.normal={c*previous.normal.x-s*previous.normal.y,s*previous.normal.x+c*previous.normal.y,previous.normal.z};
            }
            const float update=innovation[i];
            float nextDepth=previous.depth+clamp((surface[i]-previous.depth)*convergence*update,-depthLimit*update,depthLimit*update);
            Vec rawNormal=decode(surface+n+i*3);
            Vec normal=rotateToward(previous.normal,rawNormal,angleLimit*update,convergence*update);
            surface[i]=clamp(nextDepth*materialOwnership+surface[i]*(1.f-materialOwnership),0.f,1.f);
            encode(unit(normal*materialOwnership+rawNormal*(1.f-materialOwnership)),surface+n+i*3);
        }
    },abort,abortData))return false;
    return true;
}
