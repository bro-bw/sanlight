#include "SL_Track.h"
#include "SL_Parallel.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <xmmintrin.h>

namespace {
struct V { float x=0, y=0; };
struct Pixel { float r=0,g=0,b=0,a=0; };
struct Image { int w,h; std::vector<Pixel> p; };
struct Flow { int w,h; std::vector<V> p; };
float clamp(float x, float lo, float hi) { return std::max(lo,std::min(hi,x)); }
inline Pixel interpolate(const Pixel& a,const Pixel& b,const Pixel& c,const Pixel& d,float tx,float ty) {
    Pixel out;
#define MIX(k) out.k=(a.k+(b.k-a.k)*tx)*(1-ty)+(c.k+(d.k-c.k)*tx)*ty
    MIX(r); MIX(g); MIX(b); MIX(a);
#undef MIX
    return out;
}
Pixel sample(const Image& im, float x, float y) {
    x=clamp(x,0.f,float(im.w-1)); y=clamp(y,0.f,float(im.h-1));
    int x0=int(x),y0=int(y),x1=std::min(x0+1,im.w-1),y1=std::min(y0+1,im.h-1);
    return interpolate(im.p[y0*im.w+x0],im.p[y0*im.w+x1],im.p[y1*im.w+x0],im.p[y1*im.w+x1],x-x0,y-y0);
}
V sample(const Flow& f,float x,float y) {
    x=clamp(x,0.f,float(f.w-1)); y=clamp(y,0.f,float(f.h-1));
    int x0=int(x),y0=int(y),x1=std::min(x0+1,f.w-1),y1=std::min(y0+1,f.h-1);
    float tx=x-x0,ty=y-y0;
    const V &a=f.p[y0*f.w+x0],&b=f.p[y0*f.w+x1],&c=f.p[y1*f.w+x0],&d=f.p[y1*f.w+x1];
    V v;
    v.x=(a.x+(b.x-a.x)*tx)*(1-ty)+(c.x+(d.x-c.x)*tx)*ty;
    v.y=(a.y+(b.y-a.y)*tx)*(1-ty)+(c.y+(d.y-c.y)*tx)*ty;
    return v;
}
float plane(const float* p,int w,int h,float x,float y) {
    x=clamp(x,0.f,float(w-1)); y=clamp(y,0.f,float(h-1));
    int ix=int(x),iy=int(y),jx=std::min(ix+1,w-1),jy=std::min(iy+1,h-1);
    float tx=x-ix,ty=y-iy;
    return (p[iy*w+ix]*(1-tx)+p[iy*w+jx]*tx)*(1-ty)+(p[jy*w+ix]*(1-tx)+p[jy*w+jx]*tx)*ty;
}
template<class Read>
Image resize(int sourceWidth,int sourceHeight,int w,int h,Read read) {
    Image dst{w,h,std::vector<Pixel>((size_t)w*h)};
    for(int y=0;y<h;++y) {
        float sy=clamp((y+.5f)*sourceHeight/h-.5f,0.f,float(sourceHeight-1));
        int y0=int(sy),y1=std::min(y0+1,sourceHeight-1);
        for(int x=0;x<w;++x) {
            float sx=clamp((x+.5f)*sourceWidth/w-.5f,0.f,float(sourceWidth-1));
            int x0=int(sx),x1=std::min(x0+1,sourceWidth-1);
            dst.p[size_t(y)*w+x]=interpolate(read(x0,y0),read(x1,y0),read(x0,y1),read(x1,y1),sx-x0,sy-y0);
        }
    }
    return dst;
}
std::vector<Image> pyramid(const float* rgba,int w,int h,int limit) {
    float scale=std::min(1.f,float(limit)/std::max(w,h));
    std::vector<Image> levels;
    levels.push_back(resize(w,h,std::max(1,int(w*scale)),std::max(1,int(h*scale)),
        [&](int x,int y) {
            const float* p=rgba+(size_t(y)*w+x)*4;
            return Pixel{p[0],p[1],p[2],p[3]};
        }));
    while(std::min(levels.back().w,levels.back().h)>=40) {
        const Image& prev=levels.back();
        levels.push_back(resize(prev.w,prev.h,(prev.w+1)/2,(prev.h+1)/2,
            [&](int x,int y)->const Pixel& {return prev.p[size_t(y)*prev.w+x];}));
    }
    return levels;
}
float difference(Pixel a,Pixel b) {
    float dr=a.r-b.r,dg=a.g-b.g,db=a.b-b.b,da=a.a-b.a;
    return (dr*dr+dg*dg+db*db)/3.f+da*da;
}
float cost(const Image& current,const Image& reference,int x,int y,V flow,bool robust) {
    float sum=0,dr=0,dg=0,db=0;
    for(int py=-3;py<=3;py++) for(int px=-3;px<=3;px++) {
        Pixel a=sample(current,float(x+px),float(y+py));
        Pixel b=sample(reference,x+px+flow.x,y+py+flow.y);
        sum+=difference(a,b);
        if(robust){dr+=a.r-b.r;dg+=a.g-b.g;db+=a.b-b.b;}
    }
    // Remove uniform patch color shifts while retaining a small absolute-color anchor.
    return std::max(0.f,sum/49.f-(robust?.98f:0.f)*(dr*dr+dg*dg+db*db)/(3.f*49.f*49.f));
}
struct Patch { float channels[4][49]; };
Patch patchAt(const Image& image,int x,int y) {
    Patch patch;int i=0;
    for(int py=-3;py<=3;py++)for(int px=-3;px<=3;px++,++i) {
        const auto& pixel=image.p[std::max(0,std::min(image.h-1,y+py))*image.w+std::max(0,std::min(image.w-1,x+px))];
        patch.channels[0][i]=pixel.r;patch.channels[1][i]=pixel.g;
        patch.channels[2][i]=pixel.b;patch.channels[3][i]=pixel.a;
    }
    return patch;
}
float patchCost(const Patch& patch,const Image& reference,int x,int y,V flow) {
    float sum=0,dr=0,dg=0,db=0;int i=0;
    struct Column {int left,right;float fraction;};
    Column columns[7];
    for(int px=-3;px<=3;++px) {
        const float sx=clamp(x+px+flow.x,0.f,float(reference.w-1));
        const int left=int(sx);
        columns[px+3]={left,std::min(left+1,reference.w-1),sx-left};
    }
    for(int py=-3;py<=3;++py) {
        const float sy=clamp(y+py+flow.y,0.f,float(reference.h-1));
        const int top=int(sy);
        const Pixel* upper=reference.p.data()+size_t(top)*reference.w;
        const Pixel* lower=reference.p.data()+size_t(std::min(top+1,reference.h-1))*reference.w;
        for(const Column& column : columns) {
            const Pixel a{patch.channels[0][i],patch.channels[1][i],patch.channels[2][i],patch.channels[3][i]}; ++i;
            const Pixel b=interpolate(upper[column.left],upper[column.right],lower[column.left],
                                      lower[column.right],column.fraction,sy-top);
            sum+=difference(a,b);dr+=a.r-b.r;dg+=a.g-b.g;db+=a.b-b.b;
        }
    }
    return std::max(0.f,sum/49.f-.98f*(dr*dr+dg*dg+db*db)/(3.f*49.f*49.f));
}
// The 7x7 search uses the same reference image for every patch at this level.
// Planes allow four adjacent reference samples to be interpolated together.
struct Planes {
    std::vector<float> pixels;
    explicit Planes(const Image& image,bool enabled) {
        if (!enabled) return;
        const size_t n=image.p.size();
        try { pixels.resize(n*4); }
        catch (const std::bad_alloc&) { return; } // The scalar matcher uses the original image.
        for(size_t i=0;i<n;++i) {
            const Pixel& p=image.p[i];
            pixels[i]=p.r; pixels[n+i]=p.g; pixels[2*n+i]=p.b; pixels[3*n+i]=p.a;
        }
    }
};

inline __m128 horizontal(const float* row,__m128 fraction) {
    const auto left=_mm_loadu_ps(row),right=_mm_loadu_ps(row+1);
    return _mm_add_ps(left,_mm_mul_ps(_mm_sub_ps(right,left),fraction));
}

float packedCost(const Patch& target,const Image& reference,const Planes& planes,int x,int y,V flow) {
    if (planes.pixels.empty()) return patchCost(target,reference,x,y,flow);
    float fx[7];
    int left=0;
    for(int c=0;c<7;++c) {
        const float sx=(x+c-3)+flow.x;
        if(!(sx>=0 && sx<float(reference.w-1)))
            return patchCost(target,reference,x,y,flow);
        const int ix=int(sx);
        if(c==0)left=ix;
        else if(ix!=left+c)return patchCost(target,reference,x,y,flow);
        fx[c]=sx-ix;
    }
    const __m128 tx[2]={_mm_loadu_ps(fx),_mm_loadu_ps(fx+3)};
    __m128 residual=_mm_setzero_ps();
    float sum=0;
    __m128 lastLower[2][4]; int previousBottom=-1;
    for(int row=0;row<7;++row) {
        const float sy=clamp((y+row-3)+flow.y,0.f,float(reference.h-1));
        const int iy=int(sy),bottom=std::min(iy+1,reference.h-1);
        const __m128 ty=_mm_set1_ps(sy-iy),oneMinusTy=_mm_set1_ps(1.f-(sy-iy));
        const size_t n=reference.p.size();
        for(int part=0;part<2;++part) {
            const int col=3*part;
            __m128 delta[4];
            for(int ch=0;ch<4;++ch) {
                const float* p=planes.pixels.data()+ch*n+left+col;
                const auto upper=iy==previousBottom ? lastLower[part][ch] : horizontal(p+size_t(iy)*reference.w,tx[part]);
                const auto lower=horizontal(p+size_t(bottom)*reference.w,tx[part]);
                lastLower[part][ch]=lower;
                const auto interpolated=_mm_add_ps(_mm_mul_ps(upper,oneMinusTy),_mm_mul_ps(lower,ty));
                delta[ch]=_mm_sub_ps(_mm_loadu_ps(target.channels[ch]+row*7+col),interpolated);
            }
            auto d2=_mm_add_ps(_mm_mul_ps(delta[0],delta[0]),_mm_mul_ps(delta[1],delta[1]));
            d2=_mm_add_ps(d2,_mm_mul_ps(delta[2],delta[2]));
            d2=_mm_add_ps(_mm_div_ps(d2,_mm_set1_ps(3.f)),_mm_mul_ps(delta[3],delta[3]));
            float differences[4]; _mm_storeu_ps(differences,d2);
            _MM_TRANSPOSE4_PS(delta[0],delta[1],delta[2],delta[3]);
            // The groups overlap by one pixel. Accumulate in left-to-right source order.
            for(int i=part;i<4;++i){sum+=differences[i];residual=_mm_add_ps(residual,delta[i]);}
        }
        previousBottom=bottom;
    }
    float d[4]; _mm_storeu_ps(d,residual);
    return std::max(0.f,sum/49.f-.98f*(d[0]*d[0]+d[1]*d[1]+d[2]*d[2])/(3.f*49.f*49.f));
}
bool estimate(const std::vector<Image>& ref,const std::vector<Image>& cur,float smoothing,
    Flow& final,SL_TrackAbort abort,void* data,bool robust,bool keepQuiet) {
    Flow previous{};
    for(int level=int(cur.size())-1;level>=0;level--) {
        const Image& a=cur[level]; const Image& b=ref[level];
        const Planes planes(b,robust);
        Flow f{(a.w+3)/4,(a.h+3)/4,{}}; f.p.resize((size_t)f.w*f.h);
        float scaleX=previous.w ? float(a.w)/cur[level+1].w : 1.f;
        float scaleY=previous.h ? float(a.h)/cur[level+1].h : 1.f;
        if(!SL_ParallelRows(f.h,[&](int gy) {
            for(int gx=0;gx<f.w;gx++) {
                int x=std::min(gx*4,a.w-1), y=std::min(gy*4,a.h-1);
                V prediction;
                if(previous.w) {
                    prediction=sample(previous,x/(4*scaleX),y/(4*scaleY));
                    prediction.x*=scaleX; prediction.y*=scaleY;
                }
                V best=prediction;
                // The target patch is unchanged across all candidate displacements.
                const Patch patch=robust ? patchAt(a,x,y) : Patch{};
                auto matchCost=[&](V v){return robust?packedCost(patch,b,planes,x,y,v):cost(a,b,x,y,v,false);};
                float bestCost=matchCost(best),bestMatch=bestCost;
                // Below floating-point / source quantization noise, another search can only
                // invent subpixel motion on an already matching, textureless patch.
                if(!robust || !keepQuiet || bestCost>1e-8f) {
                int radius=previous.w ? 3 : 9;
                for(int dy=-radius;dy<=radius;dy++) for(int dx=-radius;dx<=radius;dx++) {
                    // Both patch costs are nonnegative. A displacement penalty alone
                    // can rule out a candidate without reading its 49-pixel patch.
                    const float penalty = .000015f * (dx*dx + dy*dy);
                    if (penalty >= bestCost || (!dx && !dy)) continue;
                    V candidate{prediction.x+dx,prediction.y+dy};
                    const float measured=matchCost(candidate);
                    float c=measured+penalty;
                    if(c<bestCost) { bestCost=c; bestMatch=measured; best=candidate; }
                }
                for(float step : {.5f,.25f,.125f,.0625f}) {
                    V center=best; const float centerMatch=bestMatch;
                    for(int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++) {
                        V candidate{center.x+dx*step,center.y+dy*step};
                        const float c=(dx || dy) ? matchCost(candidate) : centerMatch;
                        if(c<bestCost) { bestCost=bestMatch=c; best=candidate; }
                    }
                }
                }
                f.p[gy*f.w+gx]=best;
            }
        },abort,data))return false;
        // Regularization acts on displacement, not on frame history.
        for(int pass=0;pass<2;pass++) {
            Flow smoothed=f;
            if(!SL_ParallelRows(f.h,[&](int y) {for(int x=0;x<f.w;x++) {
                V total; float sum=0;
                const Pixel center=sample(a,float(x*4),float(y*4));
                for(int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++) {
                    int nx=std::max(0,std::min(f.w-1,x+dx)),ny=std::max(0,std::min(f.h-1,y+dy));
                    float weight=std::exp(-12.f*difference(center,sample(a,float(nx*4),float(ny*4))));
                    total.x+=f.p[ny*f.w+nx].x*weight; total.y+=f.p[ny*f.w+nx].y*weight; sum+=weight;
                }
                V& v=smoothed.p[y*f.w+x];
                v.x+=(total.x/sum-v.x)*smoothing; v.y+=(total.y/sum-v.y)*smoothing;
            }},abort,data))return false;
            f=std::move(smoothed);
        }
        previous=std::move(f);
    }
    final=std::move(previous); return true;
}
}

bool SL_TrackDepth(const float* referenceRGBA,const float* currentRGBA,const float* referenceDepth,
    int w,int h,int resolution,float smoothing,SL_TrackResult& result,SL_TrackAbort abort,void* abortData,bool robust,bool keepQuiet,bool symmetricMotion,bool produceDepth,float minimumAlpha) {
    if(!referenceRGBA || !currentRGBA || (produceDepth&&!referenceDepth) || w<1 || h<1) return false;
    const size_t n=(size_t)w*h;
    if(produceDepth)result.depth.resize(n);
    else std::vector<float>().swap(result.depth);
    result.confidence.resize(n); result.flowX.resize(n); result.flowY.resize(n);
    if(!memcmp(referenceRGBA,currentRGBA,n*4*sizeof(float))) {
        if(produceDepth)std::copy(referenceDepth,referenceDepth+n,result.depth.begin());
        std::fill(result.confidence.begin(),result.confidence.end(),1.f);
        if(minimumAlpha>0.f&&!produceDepth) {
            for(size_t i=0;i<n;++i)
                if(currentRGBA[i*4+3]<minimumAlpha) result.confidence[i]=0.f;
        }
        std::fill(result.flowX.begin(),result.flowX.end(),0.f); std::fill(result.flowY.begin(),result.flowY.end(),0.f);
        return true;
    }
    int limit=std::max(64,std::min(1024,resolution));
    auto ref=pyramid(referenceRGBA,w,h,limit),cur=pyramid(currentRGBA,w,h,limit);
    Flow backward,forward;
    if(!estimate(ref,cur,clamp(smoothing,0,1),backward,abort,abortData,robust,keepQuiet) ||
       !estimate(cur,ref,clamp(smoothing,0,1),forward,abort,abortData,robust,keepQuiet)) return false;
    float sx=float(cur[0].w)/w,sy=float(cur[0].h)/h;
    if(!SL_ParallelRows(h,[&](int y) {
        for(int x=0;x<w;x++) {
            size_t i=(size_t)y*w+x;
            if(minimumAlpha>0.f&&!produceDepth&&currentRGBA[i*4+3]<minimumAlpha) {
                result.confidence[i]=result.flowX[i]=result.flowY[i]=0.f;
                continue;
            }
            float qx=(x+.5f)*sx-.5f,qy=(y+.5f)*sy-.5f;
            V v=sample(backward,qx/4,qy/4);
            V reverse=sample(forward,(qx+v.x)/4,(qy+v.y)/4);
            float cycle=std::hypot(v.x+reverse.x,v.y+reverse.y);
            const Pixel current=sample(cur[0],qx,qy);
            const Pixel original=sample(ref[0],qx+v.x,qy+v.y);
            float residual=difference(current,original);
            if(symmetricMotion&&cycle<2.f&&current.a>.99f&&original.a>.99f) {
                // Split small inverse-consistency errors between both estimates. Reject
                // occlusion boundaries and corrections that worsen the appearance match.
                V candidate{v.x-.5f*(v.x+reverse.x),v.y-.5f*(v.y+reverse.y)};
                const Pixel corrected=sample(ref[0],qx+candidate.x,qy+candidate.y);
                const float correctedResidual=difference(current,corrected);
                if(corrected.a>.99f&&correctedResidual<=residual*1.1f+.0001f) {
                    v=candidate;
                    residual=correctedResidual;
                    reverse=sample(forward,(qx+v.x)/4,(qy+v.y)/4);
                    cycle=std::hypot(v.x+reverse.x,v.y+reverse.y);
                }
            }
            float rx=x+v.x/sx,ry=y+v.y/sy;
            float confidence=std::exp(-cycle*.35f-residual*12.f);
            if(rx<0 || ry<0 || rx>w-1 || ry>h-1) confidence=0;
            if(produceDepth)result.depth[i]=plane(referenceDepth,w,h,rx,ry);
            result.confidence[i]=confidence;
            result.flowX[i]=v.x/sx; result.flowY[i]=v.y/sy;
        }
    },abort,abortData))return false;
    return true;
}
