#include "../../src/SL_Surface.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>

const int w=96,h=112;
const size_t n=size_t(w)*h;
struct Frame { std::vector<float> image,state; };
Frame frame(int t,bool rotation) {
    Frame f;f.image.resize(n*4);f.state.resize(n*4);
    const float phase=6.28318530718f*(t%60)/60.f;
    const float shift=3*std::sin(phase),angle=rotation?.16f*std::sin(phase):0;
    float c=std::cos(angle),s=std::sin(angle);
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
        size_t i=size_t(y)*w+x;
        float xx=x-w*.5f-shift,yy=y-h*.5f;
        float sx=c*xx+s*yy+w*.5f,sy=-s*xx+c*yy+h*.5f;
        if(!rotation){sx-=1.2f*std::sin(phase)*std::sin(y*.07f);sy-=.7f*std::sin(phase)*std::sin(x*.1f);}
        float alpha=(sx>8&&sx<w-8&&sy>8&&sy<h-8)?1:0;
        f.image[4*i]=.5f+alpha*(.17f*std::sin(sx*.43f)+.11f*std::cos(sy*.31f));
        f.image[4*i+1]=.5f+alpha*(.19f*std::sin(sy*.29f+sx*.13f));
        f.image[4*i+2]=.5f+alpha*.15f*std::cos(sy*.13f-sx*.37f);f.image[4*i+3]=alpha;
        f.state[i]=.5f+.12f*std::sin(sx*.067f)+.09f*std::cos(sy*.074f);
        float nx=.65f*std::sin(sx*.09f),ny=.5f*std::cos(sy*.072f),nz=.8f;
        float length=std::sqrt(nx*nx+ny*ny+nz*nz);
        // Encoded G is upward, hence the image-plane rotation has the opposite sign.
        f.state[n+3*i]=(c*nx+s*ny)/length*.5f+.5f;
        f.state[n+3*i+1]=(-s*nx+c*ny)/length*.5f+.5f;
        f.state[n+3*i+2]=nz/length*.5f+.5f;
    }
    return f;
}
int main() {
    auto start=std::chrono::steady_clock::now();
    for(bool rotation:{false,true}) {
        auto initial=frame(0,rotation),previous=initial;auto state=initial.state;
        double totalError=0;size_t totalCount=0;
        for(int t=1;t<=360;t++) {
            auto current=frame(t,rotation);auto raw=current.state;
            // Independent per-frame estimation noise, with a known material-space ground truth.
            for(size_t i=0;i<n;i++) {
                raw[i]+=.03f*std::sin(t*1.4f+i%w*.09f);
                raw[n+3*i]+=.025f*std::sin(t*.91f+i%w*.08f);
                raw[n+3*i+1]+=.025f*std::cos(t*1.31f+i/w*.07f);
            }
            if(!SL_TemporalSurface(previous.image.data(),current.image.data(),state.data(),raw.data(),w,h,.85f,.2f,true,1.f/30,nullptr,nullptr,rotation))return 1;
            for(int y=20;y<h-20;y++)for(int x=20;x<w-20;x++) {
                size_t i=size_t(y)*w+x;
                for(int k=0;k<3;k++){float d=(raw[n+3*i+k]-current.state[n+3*i+k])*2;totalError+=d*d;}++totalCount;
            }
            if(t%60==0) {
                double square=0,depth=0;size_t count=0;
                for(int y=20;y<h-20;y++)for(int x=20;x<w-20;x++) {
                    size_t i=size_t(y)*w+x;float dz=raw[i]-current.state[i];depth+=dz*dz;
                    for(int k=0;k<3;k++){float d=(raw[n+3*i+k]-current.state[n+3*i+k])*2;square+=d*d;}++count;
                }
                printf("%s cycle %d: normal RMSE %.6f depth RMSE %.6f\n",rotation?"rotation":"deformation",t/60,std::sqrt(square/count),std::sqrt(depth/count));fflush(stdout);
                if(std::sqrt(square/count)>(rotation?.025:.003)||std::sqrt(depth/count)>.001)return 2;
            }
            state=std::move(raw);previous=std::move(current);
        }
        printf("%s all-frame normal RMSE %.6f\n",rotation?"rotation":"deformation",std::sqrt(totalError/totalCount));fflush(stdout);
        if(std::sqrt(totalError/totalCount)>(rotation?.05:.005))return 3;
    }
    printf("Elapsed %.3f seconds\n",std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
}
