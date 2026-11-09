#include "../../src/SL_Temporal.h"
#include "../../src/SL_AI.h"
#include "../../src/SL_History.h"
#include <cstdio>
#include <cmath>
#include <thread>

const int w=96,h=104;
const size_t n=size_t(w)*h;
struct Frame {std::vector<float> image,depth,truth;};
Frame frame(int time,int mode) {
    Frame f;f.image.resize(n*4);f.depth.resize(n);f.truth.resize(n);
    const float shift=mode ? 2.f*std::sin(time*.12f) : 0;
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
        size_t i=size_t(y)*w+x;float sx=x-shift;
        float sy=float(y);
        if(mode==2) {
            sx-=.7f*std::sin(time*.12f)*std::sin(y*.07f);
            sy-=.5f*std::sin(time*.1f)*std::sin(x*.09f);
        }
        float alpha=sx>8&&sx<86&&y>8&&y<96 ? 1.f : 0;
        f.image[i*4]=alpha*(.4f+.2f*std::sin(sx*.21f+sy*.11f))+(1-alpha)*.5f;
        f.image[i*4+1]=alpha*(.45f+.18f*std::cos(sx*.13f-sy*.17f))+(1-alpha)*.5f;
        f.image[i*4+2]=alpha*(.35f+.16f*std::sin(sx*.09f+sy*.23f))+(1-alpha)*.5f;
        f.image[i*4+3]=alpha;
        f.truth[i]=.5f+.14f*std::sin(sx*.055f)+.07f*std::cos(sy*.06f);
        f.depth[i]=f.truth[i]+(time ? .04f*std::sin(time*1.37f+sx*.09f+y*.035f) : 0);
    }
    return f;
}
float shade(const std::vector<float>& normal,size_t i) {
    return (normal[i*3]*2-1)*.5f+(normal[i*3+1]*2-1)*.2f+(normal[i*3+2]*2-1)*.84f;
}
bool stop(void*) {return true;}
int main() {
    for(int mode : {0,1,2}) {
        Frame previous=frame(0,mode);
        std::vector<float> stable=previous.depth,rawNormal(n*3),stableNormal(n*3),truthNormal(n*3);
        double rawError=0,stableError=0,rawShade=0,stableShade=0;size_t samples=0;
        for(int t=1;t<90;t++) {
            Frame next=frame(t,mode);auto filtered=next.depth;
            if(!SL_TemporalDepth(previous.image.data(),next.image.data(),stable.data(),filtered.data(),
                w,h,.85f,.2f,true,1.f/30))return 1;
            auto saved=filtered;
            SL_AI_RebuildNormals(next.depth.data(),next.image.data(),w,h,500,rawNormal.data());
            SL_AI_RebuildNormals(filtered.data(),next.image.data(),w,h,500,stableNormal.data());
            SL_AI_RebuildNormals(next.truth.data(),next.image.data(),w,h,500,truthNormal.data());
            if(filtered!=saved)return 2; // normal rebuild must not erode state
            for(int y=22;y<82;y++) {
#pragma loop(no_vector)
            for(int x=22;x<74;x++) {
                size_t i=size_t(y)*w+x;
                const double rawDepthError=next.depth[i]-next.truth[i];
                const double stableDepthError=filtered[i]-next.truth[i];
                const double rawLightError=shade(rawNormal,i)-shade(truthNormal,i);
                const double stableLightError=shade(stableNormal,i)-shade(truthNormal,i);
                rawError+=rawDepthError*rawDepthError;stableError+=stableDepthError*stableDepthError;
                rawShade+=rawLightError*rawLightError;stableShade+=stableLightError*stableLightError;++samples;
            }}
            stable=std::move(filtered);previous=std::move(next);
        }
        printf("%s depth RMSE %.6f -> %.6f; lighting error %.6f -> %.6f\n",mode==0?"Static":mode==1?"Small motion":"Local deformation",
            std::sqrt(rawError/samples),std::sqrt(stableError/samples),std::sqrt(rawShade/samples),std::sqrt(stableShade/samples));
        printf("Independent sums: %.12f %.12f %.12f %.12f\n",rawError,stableError,rawShade,stableShade);
        if(stableError>=rawError*.6||stableShade>=rawShade*.6)return 3;
    }
    Frame old=frame(0,false),next=frame(1,false);
    auto heldStatic=old.depth;
    for(int t=0;t<1200;t++) {
        auto estimate=next.depth;
        if(!SL_TemporalDepth(old.image.data(),old.image.data(),heldStatic.data(),estimate.data(),w,h,.85f,.2f,true,1.f/30))return 16;
        heldStatic=std::move(estimate);
    }
    if(heldStatic!=old.depth)return 17;
    auto disabled=next.depth;
    if(!SL_TemporalDepth(old.image.data(),next.image.data(),old.depth.data(),disabled.data(),w,h,0,.2f,true,1.f/30)||disabled!=next.depth)return 4;
    if(SL_TemporalDepth(old.image.data(),next.image.data(),old.depth.data(),disabled.data(),w,h,1,.2f,true,1.f/30,stop))return 5;
    for(size_t i=0;i<n;i++) {
        next.image[i*4]=next.image[i*4+1]=next.image[i*4+2]=1-old.image[i*4];
        next.depth[i]=.2f;
    }
    auto cut=next.depth;
    if(!SL_TemporalDepth(old.image.data(),next.image.data(),old.depth.data(),cut.data(),w,h,.85f,.2f,false,1.f/30)||cut!=next.depth)return 6;
    next=frame(0,false);for(int y=30;y<40;y++)for(int x=30;x<40;x++)old.image[(size_t(y)*w+x)*4+3]=0;
    auto revealed=next.depth;
    if(!SL_TemporalDepth(old.image.data(),next.image.data(),old.depth.data(),revealed.data(),w,h,.85f,.2f,false,1.f/30))return 7;
    if(revealed[35*w+35]!=next.depth[35*w+35])return 8;
    // Rate limiting is measured per second, not per output frame.
    Frame held=frame(0,false),changed=held;
    std::fill(held.depth.begin(),held.depth.end(),.4f);
    for(size_t i=0;i<n;i++) {changed.image[i*4]+=.001f;changed.depth[i]=.8f;}
    for(float seconds : {1.f/24,1.f/30,1.f/60}) {
        auto bounded=changed.depth;
        if(!SL_TemporalDepth(held.image.data(),changed.image.data(),held.depth.data(),bounded.data(),
            w,h,.65f,.2f,false,seconds))return 13;
        if(std::fabs(bounded[50*w+50]-.4f)>.018f*seconds+1e-6f)return 14;
    }
    // Transparent internal gaps cannot create normals on a constant visible surface.
    auto gap=held;std::fill(gap.depth.begin(),gap.depth.end(),.4f);
    for(int y=20;y<84;y++)for(int x=44;x<52;x++) {
        gap.image[(size_t(y)*w+x)*4+3]=0;gap.depth[size_t(y)*w+x]=1;
    }
    std::vector<float> gapNormal(n*3);
    SL_AI_RebuildNormals(gap.depth.data(),gap.image.data(),w,h,1000,gapNormal.data());
    for(int y=12;y<90;y++)for(int x=12;x<82;x++) {
        size_t i=size_t(y)*w+x;if(gap.image[i*4+3]<.05f)continue;
        if(std::fabs(gapNormal[i*3]-.5f)>1e-6f||std::fabs(gapNormal[i*3+1]-.5f)>1e-6f)return 15;
    }
    SL_HistoryCache cache(128);SL_HistoryKey key;key.value(1);auto small=std::make_shared<const std::vector<float>>(8,.5f);
    cache.put(key,small);if(cache.get(key)!=small)return 9;
    SL_HistoryKey edited=key;edited.value(2);if(cache.get(edited))return 10;
    for(int i=0;i<40;i++){SL_HistoryKey k;k.value(i);cache.put(k,small);}
    if(cache.get(key))return 11;
    std::thread a([&]{for(int i=0;i<100;i++){cache.put(key,small);cache.get(key);}});
    std::thread b([&]{for(int i=0;i<100;i++){cache.get(key);}});
    a.join();b.join();cache.clear();if(cache.get(key))return 12;
    puts("PASS shape transport, lighting stability, no cumulative blur, disabled, cancellation, cuts, reveal, bounded thread-safe cache");
}
