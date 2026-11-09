#include "../../src/SanLight.cpp"
#include <vector>
#include <stdio.h>
int main() {
    FILE* f = NULL; int dims[2];
    if (fopen_s(&f,"probe_input.bin","rb")) return 1;
    if (fread(dims,sizeof(int),2,f)!=2) return 2;
    fclose(f);
    size_t n=(size_t)dims[0]*dims[1];
    std::vector<float> maps(n*4);
    if(fopen_s(&f,"probe_output.bin","rb")) return 3;
    if(fread(maps.data(),sizeof(float),n*4,f)!=n*4) return 4;
    fclose(f);
    SL_Ctx c={};
    c.hasDepth=c.hasNormal=c.normalYUp=true;
    c.mapW=dims[0]; c.mapH=dims[1];
    c.depthWorld.w=c.normalWorld.w=dims[0];
    c.depthWorld.h=c.normalWorld.h=dims[1];
    c.depthWorld.fdata=maps.data(); c.depthWorld.chans=1;
    c.normalWorld.fdata=maps.data()+n; c.normalWorld.chans=3;
    c.depthRange=1000; c.lightX=1019.3f; c.lightY=261.2f;
    c.intensity=1.68f; c.diffuse=1.73f; c.softness=.304f; c.falloff=2500;
    c.backlightEnabled=true; c.backlightStart=200; c.backlightDistance=800; c.view=3;
    float src[4]={1,1,1,1}, out[4]; size_t previous=n;
    for(int z=-500;z>=-2100;z-=200) {
        c.lightZ=(float)z; size_t lit=0;
        for(int y=0;y<dims[1];y++) for(int x=0;x<dims[0];x++) {
            SL_ShadePixel(c,x,y,src,out);
            if(!isfinite(out[0])) return 5;
            if(out[0]>.05f) ++lit;
        }
        printf("Z=%d lit=%zu\n",z,lit);
        if(lit>previous) return 6;
        previous=lit;
    }
    if(previous!=0) return 7;
    c.lightZ=700;
    for(int y=0;y<dims[1];y+=19) for(int x=0;x<dims[0];x+=19) {
        float old[4]; c.backlightEnabled=false; SL_ShadePixel(c,x,y,src,old);
        c.backlightEnabled=true; SL_ShadePixel(c,x,y,src,out);
        if(fabsf(old[0]-out[0])>1e-6f) return 8;
    }
    c.lightZ=-5000; c.view=2; c.ambient=.3f; c.specular=4; c.rim=2; c.shininess=24;
    SL_ShadePixel(c,400,150,src,out);
    if(fabsf(out[0]-.3f)>1e-6f) return 9;
    puts("PASS: shrinking lit area, full suppression, front light unchanged, ambient retained");
    c.lightZ=-1700; c.view=3; c.edgeKeep=1; c.edgeWidth=6;
    size_t kept=0, original=0;
    for(int y=0;y<dims[1];y++) for(int x=0;x<dims[0];x++) {
        float base[4];
        c.backlightEnabled=false; SL_ShadePixel(c,x,y,src,base);
        c.backlightEnabled=true; SL_ShadePixel(c,x,y,src,out);
        if(out[0]>base[0]+1e-5f) return 10;
        if(out[0]>.05f) ++kept;
        if(base[0]>.05f) ++original;
    }
    printf("Edge preservation: %zu lit pixels vs %zu without suppression\n",kept,original);
    if(!kept || kept>=original) return 11;
    // A flat interior without a depth discontinuity stays suppressed.
    std::fill(maps.begin(),maps.begin()+n,.5f);
    c.lightZ=-5000;
    SL_ShadePixel(c,400,150,src,out);
    if(out[0]!=0.f) return 12;
    puts("PASS: edge light retained, no added light, flat interior remains suppressed");
}
