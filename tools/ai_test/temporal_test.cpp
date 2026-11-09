#include "../../src/SL_Temporal.h"
#include <vector>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>

struct Frame {std::vector<float> image,depth,normal,truth,truthNormal;};
const int w=96,h=120;const size_t n=(size_t)w*h;
Frame frame(int time) {
    Frame f;f.image.resize(n*4);f.depth.resize(n);f.normal.resize(n*3);
    f.truth.resize(n);f.truthNormal.resize(n*3);
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
        size_t i=(size_t)y*w+x;float sx=float(x-time),sy=float(y);
        float a=sx>12&&sx<76&&y>10&&y<110?1.f:0.f;
        f.image[i*4]=a*(.45f+.18f*std::sin(sx*.22f)*std::cos(sy*.11f))+(1-a)*.5f;
        f.image[i*4+1]=a*(.4f+.2f*std::cos(sx*.12f+sy*.17f))+(1-a)*.5f;
        f.image[i*4+2]=a*(.35f+.25f*std::sin(sx*.16f+sy*.09f))+(1-a)*.5f;
        f.image[i*4+3]=a;
        f.truth[i]=.5f+.15f*std::sin(sx*.06f)+.08f*std::cos(sy*.07f);
        float jitter=.035f*std::sin(time*2.4f+sx*.015f);
        f.depth[i]=f.truth[i]+jitter;
        float nx=.25f*std::sin(sx*.06f),ny=.18f*std::cos(sy*.07f),nz=1;
        float len=std::sqrt(nx*nx+ny*ny+nz*nz);
        f.truthNormal[i*3]=nx/len*.5f+.5f;f.truthNormal[i*3+1]=ny/len*.5f+.5f;f.truthNormal[i*3+2]=nz/len*.5f+.5f;
        nx+=.16f*std::sin(time*2.4f+sx*.015f);
        len=std::sqrt(nx*nx+ny*ny+nz*nz);
        f.normal[i*3]=nx/len*.5f+.5f;f.normal[i*3+1]=ny/len*.5f+.5f;f.normal[i*3+2]=nz/len*.5f+.5f;
    }return f;
}
Frame evaluate(const std::vector<Frame>& frames,int time,bool enabled) {
    Frame result=frames[time-2];
    for(int k=time-1;k<=time;k++) {
        Frame next=frames[k];
        if(enabled && !SL_TemporalRefine(result.image.data(),next.image.data(),result.depth.data(),result.normal.data(),
            next.depth.data(),next.normal.data(),w,h,.65f,.6f,true))throw 1;
        result=std::move(next);
    }return result;
}
int main() {
    std::vector<Frame> frames;
    for(int i=0;i<12;i++)frames.push_back(frame(i));
    double raw=0,filtered=0,rawN=0,filteredN=0;int count=0;
    for(int t=2;t<12;t++) {
        Frame output=evaluate(frames,t,true);
        for(int y=20;y<100;y++)for(int x=25+t;x<65+t;x++) {
            size_t i=(size_t)y*w+x;double r=frames[t].depth[i]-frames[t].truth[i],s=output.depth[i]-frames[t].truth[i];
            raw+=r*r;filtered+=s*s;
            for(int k=0;k<3;k++) {
                r=frames[t].normal[i*3+k]-frames[t].truthNormal[i*3+k];s=output.normal[i*3+k]-frames[t].truthNormal[i*3+k];
                rawN+=r*r;filteredN+=s*s;
            }count++;
        }
    }
    printf("Synthetic depth RMSE raw=%.6f stabilized=%.6f; normal RMSE raw=%.6f stabilized=%.6f\n",
        std::sqrt(raw/count),std::sqrt(filtered/count),std::sqrt(rawN/count/3),std::sqrt(filteredN/count/3));
    if(filtered>raw*.8||filteredN>rawN*.8)return 1;
    // Seeking does not consume any previously rendered output.
    Frame ordered=evaluate(frames,7,true);
    for(int t : {10,2,7,3,9,7}) {
        Frame output=evaluate(frames,t,true);
        if(t==7 && (output.depth!=ordered.depth||output.normal!=ordered.normal))return 2;
    }
    Frame identity=frames[4];
    if(!SL_TemporalRefine(identity.image.data(),identity.image.data(),frames[4].depth.data(),frames[4].normal.data(),
        identity.depth.data(),identity.normal.data(),w,h,.65f,.6f,true))return 3;
    if(identity.depth!=frames[4].depth||identity.normal!=frames[4].normal)return 4;
    Frame disabled=frames[5];
    SL_TemporalRefine(frames[4].image.data(),disabled.image.data(),frames[4].depth.data(),frames[4].normal.data(),
        disabled.depth.data(),disabled.normal.data(),w,h,0,.6f,true);
    if(disabled.depth!=frames[5].depth||disabled.normal!=frames[5].normal)return 5;
    Frame cut=frames[5];for(size_t i=0;i<n;i++)for(int k=0;k<3;k++)cut.image[i*4+k]=.98f;
    Frame unchanged=cut;
    SL_TemporalRefine(frames[4].image.data(),cut.image.data(),frames[4].depth.data(),frames[4].normal.data(),
        cut.depth.data(),cut.normal.data(),w,h,.8f,.6f,true);
    if(cut.depth!=unchanged.depth||cut.normal!=unchanged.normal)return 6;
    Frame exposed=frames[5],blank=frames[4];for(size_t i=0;i<n;i++)blank.image[i*4+3]=0;
    SL_TemporalRefine(blank.image.data(),exposed.image.data(),blank.depth.data(),blank.normal.data(),
        exposed.depth.data(),exposed.normal.data(),w,h,.8f,.6f,true);
    if(exposed.depth!=frames[5].depth||exposed.normal!=frames[5].normal)return 7;
    Frame change=frames[5];for(auto& d:change.depth)d=.95f;for(size_t i=0;i<n;i++){change.normal[i*3]=.95f;change.normal[i*3+2]=.5f;}
    unchanged=change;
    SL_TemporalRefine(frames[4].image.data(),change.image.data(),frames[4].depth.data(),frames[4].normal.data(),
        change.depth.data(),change.normal.data(),w,h,.8f,1,true);
    for(int y=20;y<100;y++)for(int x=35;x<60;x++)if(std::fabs(change.depth[y*w+x]-.95f)>.015f)return 8;
    Frame cancelled=frames[5];
    if(SL_TemporalRefine(frames[4].image.data(),cancelled.image.data(),frames[4].depth.data(),frames[4].normal.data(),
        cancelled.depth.data(),cancelled.normal.data(),w,h,.8f,.6f,true,[](void*){return true;}))return 9;
    puts("PASS static identity, disabled identity, bounded history/order, cuts, exposure, real depth changes, cancellation");
}
