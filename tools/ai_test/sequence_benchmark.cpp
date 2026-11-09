#include "../../src/SanLight.cpp"
#include "../../src/SL_Surface.h"
#include <cstdio>
#include <string>
#include <chrono>
#include <cmath>

static void bmp(const std::wstring& path,const std::vector<unsigned char>& pixels,int w,int h) {
    FILE* file=nullptr;_wfopen_s(&file,path.c_str(),L"wb");if(!file)throw 1;
    BITMAPFILEHEADER header={};BITMAPINFOHEADER info={};
    header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(info);header.bfSize=header.bfOffBits+w*h*4;
    info.biSize=sizeof(info);info.biWidth=w;info.biHeight=-h;info.biPlanes=1;info.biBitCount=32;
    fwrite(&header,sizeof(header),1,file);fwrite(&info,sizeof(info),1,file);fwrite(pixels.data(),4,size_t(w)*h,file);fclose(file);
}
static void render(const std::vector<float>& surface,const std::vector<float>& source,int w,int h,
    std::vector<unsigned char>& preview,int column,int columns) {
    const size_t n=size_t(w)*h;const float scale=h/2447.f;
    SL_Ctx c={};c.hasNormal=c.hasDepth=c.normalYUp=true;c.mapW=w;c.mapH=h;
    c.normalWorld.w=c.depthWorld.w=w;c.normalWorld.h=c.depthWorld.h=h;
    c.normalWorld.chans=3;c.normalWorld.fdata=surface.data()+n;c.depthWorld.chans=1;c.depthWorld.fdata=surface.data();
    c.sourceWorld.w=w;c.sourceWorld.h=h;c.sourceWorld.chans=4;c.sourceWorld.fdata=source.data();
    c.lightX=w*1.2f;c.lightY=h*.1f;c.lightZ=-140*scale;c.depthRange=1000*scale;
    c.lightR=1;c.lightG=.95f;c.lightB=.87f;c.intensity=1.2f;c.falloff=2500*scale;
    c.ambient=.15f;c.diffuse=1;c.specular=.3f;c.shininess=24;c.softness=0;
    c.backlightEnabled=true;c.backlightStart=200*scale;c.backlightDistance=800*scale;c.edgeKeep=1;c.edgeWidth=10*scale;
    c.lvInW=c.lvOutW=c.lvGamma=1;c.view=1;c.apply=2;c.opacity=1;
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
        size_t i=size_t(y)*w+x;float out[4];
        if(column==0)memcpy(out,source.data()+4*i,sizeof(out));
        else SL_ShadePixel(c,x,y,source.data()+4*i,out);
        unsigned char* p=preview.data()+(size_t(y)*w*columns+column*w+x)*4;
        for(int k=0;k<3;k++)p[2-k]=(unsigned char)(255*SL_Clamp01(out[k]*out[3]));p[3]=255;
    }
}
int wmain(int argc,wchar_t** argv) {
    if(argc<3||argc>6){puts("Usage: sequence_benchmark sequence.bin output_directory [reuse-baseline] [previous-build-directory] [verify-only]");return 1;}
    const bool reuseBaseline=argc>=4;
    const bool verify=argc==6;
    const std::wstring baselineDirectory=argc>=5?argv[4]:argv[2];
    FILE* file=nullptr;_wfopen_s(&file,argv[1],L"rb");if(!file)return 2;
    unsigned dims[3];float fps=0;
    if(fread(dims,4,3,file)!=3||fread(&fps,4,1,file)!=1)return 3;
    int w=dims[0],h=dims[1],frames=dims[2];if(w<1||h<1||w>4096||h>4096||frames<1||fps<=0)return 4;
    const size_t n=size_t(w)*h;CreateDirectoryW(argv[2],nullptr);
    std::vector<unsigned char> bytes(n*4),preview(n*4*4);
    std::vector<float> previousImage,previousOld,previousNew;
    std::wstring cachePath=(verify?baselineDirectory:std::wstring(argv[2]))+L"\\raw-surfaces.bin";
    FILE* cache=nullptr;_wfopen_s(&cache,cachePath.c_str(),L"rb");bool cached=cache!=nullptr;
    if(verify&&!cached)return 16;
    if(!cached)_wfopen_s(&cache,cachePath.c_str(),L"wb");if(!cache)return 5;
    if(cached){unsigned check[3];float cf=0;if(fread(check,4,3,cache)!=3||fread(&cf,4,1,cache)!=1||memcmp(check,dims,sizeof(dims))||cf!=fps)return 6;}
    else{fwrite(dims,4,3,cache);fwrite(&fps,4,1,cache);}
    FILE* report=nullptr;_wfopen_s(&report,(std::wstring(argv[2])+L"\\metrics.csv").c_str(),L"w");if(!report)return 7;
    fprintf(report,"frame,seconds,raw_normal_delta,old_normal_delta,new_normal_delta,foreground_pixels\n");
    std::vector<float> previousRaw;
    printf("Sequence %dx%d, %d frames at %.4f fps, AI cache %s\n",w,h,frames,fps,cached?"read":"create");fflush(stdout);
    for(int t=0;t<frames;t++) {
        auto start=std::chrono::steady_clock::now();if(fread(bytes.data(),4,n,file)!=n)return 8;
        std::vector<float> image(n*4),source(n*4),raw(n*4);
        for(size_t i=0;i<n;i++) {
            float a=bytes[i*4+3]/255.f;
            for(int c=0;c<3;c++){source[i*4+c]=bytes[i*4+c]/255.f;image[i*4+c]=source[i*4+c]*a+.5f*(1-a);}
            source[i*4+3]=image[i*4+3]=a;
        }
        if(cached){if(fread(raw.data(),sizeof(float),n*4,cache)!=n*4)return 9;}
        else {
            float *d=nullptr,*norm=nullptr;
            if(SL_AI_Generate(image.data(),w,h,3,10,1000*h/2447.f,&d,&norm)){puts(SL_AI_LastError());return 10;}
            memcpy(raw.data(),d,n*sizeof(float));memcpy(raw.data()+n,norm,n*3*sizeof(float));free(d);free(norm);
            fwrite(raw.data(),sizeof(float),n*4,cache);fflush(cache);
        }
        auto old=raw,newer=raw;
        if(reuseBaseline) {
            FILE* baseline=nullptr;_wfopen_s(&baseline,(baselineDirectory+L"\\state-"+std::to_wstring(t)+L".bin").c_str(),L"rb");
            if(!baseline)return 14;
            _fseeki64(baseline,static_cast<__int64>(n)*(argc>=5?32:16),SEEK_SET);
            if(fread(old.data(),4,n*4,baseline)!=n*4){fclose(baseline);return 15;}fclose(baseline);
        }
        if(t) {
            if(!reuseBaseline) {
                if(!SL_TemporalDepth(previousImage.data(),image.data(),previousOld.data(),old.data(),w,h,.85f,.2f,true,1/fps))return 11;
                SL_AI_RebuildNormals(old.data(),image.data(),w,h,1000*h/2447.f,old.data()+n);
            }
            if(!SL_TemporalSurface(previousImage.data(),image.data(),previousNew.data(),newer.data(),w,h,.85f,.2f,true,1/fps))return 12;
        }
        double rawDelta=0,oldDelta=0,newDelta=0;size_t visible=0;
        if(t)for(size_t i=0;i<n;i++)if(image[4*i+3]>.9f&&previousImage[4*i+3]>.9f) {
            for(int c=0;c<3;c++){
                rawDelta+=std::fabs(raw[n+3*i+c]-previousRaw[n+3*i+c]);
                oldDelta+=std::fabs(old[n+3*i+c]-previousOld[n+3*i+c]);
                newDelta+=std::fabs(newer[n+3*i+c]-previousNew[n+3*i+c]);
            }++visible;
        }
        const double divisor=(std::max)(size_t(1),visible)*3.;
        fprintf(report,"%d,%.6f,%.9f,%.9f,%.9f,%llu\n",t,t/fps,rawDelta/divisor,oldDelta/divisor,newDelta/divisor,(unsigned long long)visible);fflush(report);
        if(verify&&memcmp(old.data(),newer.data(),n*4*sizeof(float))) {printf("FAIL exact surface at frame %d\n",t);return 17;}
        if(!verify) {
        render(raw,source,w,h,preview,0,4);render(raw,source,w,h,preview,1,4);
        render(old,source,w,h,preview,2,4);render(newer,source,w,h,preview,3,4);
        bmp(std::wstring(argv[2])+L"\\compare-"+std::to_wstring(t)+L".bmp",preview,w*4,h);
        // Persist exact state for independent motion-compensated analysis.
        FILE* state=nullptr;_wfopen_s(&state,(std::wstring(argv[2])+L"\\state-"+std::to_wstring(t)+L".bin").c_str(),L"wb");
        if(!state)return 13;fwrite(raw.data(),4,n*4,state);fwrite(old.data(),4,n*4,state);fwrite(newer.data(),4,n*4,state);fclose(state);
        }
        previousImage=std::move(image);previousRaw=std::move(raw);previousOld=std::move(old);previousNew=std::move(newer);
        printf("Frame %d/%d: delta raw %.6f old %.6f new %.6f; %.2fs\n",t+1,frames,rawDelta/divisor,oldDelta/divisor,newDelta/divisor,
            std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());fflush(stdout);
    }
    fclose(file);fclose(cache);fclose(report);if(verify)puts("PASS every frame exactly matches the preserved baseline surface");return 0;
}
