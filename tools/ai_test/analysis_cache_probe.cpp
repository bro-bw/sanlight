#include "../../src/SL_History.h"
#include "../../src/SL_DiskCache.h"
#include "../../src/SL_AI.h"
#include "../../src/SL_Surface.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <string>

namespace {
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now()-start).count();
}
struct FileCloser {void operator()(FILE* p) const {if(p)fclose(p);}};
using File=std::unique_ptr<FILE,FileCloser>;
File openFile(const wchar_t* path) {
    FILE* f=nullptr;_wfopen_s(&f,path,L"rb");return File(f);
}
bool digestFile(const std::wstring& path,SL_HistoryKey& key) {
    auto file=openFile(path.c_str());if(!file)return false;
    std::vector<unsigned char> block(1024*1024);
    uint64_t total=0;
    for(;;) {
        size_t amount=fread(block.data(),1,block.size(),file.get());
        if(amount){key.append(block.data(),amount);total+=amount;}
        if(amount<block.size())break;
    }
    if(ferror(file.get()))return false;
    key.value(total);return total>0;
}
bool positiveInteger(const wchar_t* text,uint32_t& value) {
    if(!text||!*text||*text==L'-')return false;
    wchar_t* end=nullptr;unsigned long long number=wcstoull(text,&end,10);
    if(!end||*end||!number||number>UINT32_MAX)return false;
    value=static_cast<uint32_t>(number);return true;
}
}

int wmain(int argc,wchar_t** argv) {
    const auto wholeStart=Clock::now();
    if(argc<3||argc>5) {
        puts("Usage: analysis_cache_probe sequence.bin cache_directory [frame_limit [normal_strength]]");
        puts("Prototype only: sequential content-validated AI analysis, not AE render integration.");
        return 1;
    }
    uint32_t limit=UINT32_MAX;
    if(argc>=4&&!positiveInteger(argv[3],limit)){puts("Invalid frame limit");return 1;}
    float normalStrength=1000.f;
    if(argc==5) {
        wchar_t* end=nullptr;normalStrength=wcstof(argv[4],&end);
        if(!end||*end||!std::isfinite(normalStrength)||normalStrength<0||normalStrength>1000.f) {
            puts("Normal strength must be 0..1000");return 1;
        }
    }
    auto file=openFile(argv[1]);if(!file){puts("Cannot open sequence");return 2;}
    uint32_t width=0,height=0,frames=0;float fps=0;
    if(fread(&width,4,1,file.get())!=1||fread(&height,4,1,file.get())!=1||
        fread(&frames,4,1,file.get())!=1||fread(&fps,4,1,file.get())!=1) {
        puts("Invalid sequence header");return 3;
    }
    constexpr uint64_t maxValue=512ULL*1024*1024;
    const uint64_t pixels64=uint64_t(width)*height;
    if(!width||!height||width>16384||height>16384||!frames||frames>1000000||
        !std::isfinite(fps)||fps<=0||fps>1000||pixels64>maxValue/(4*sizeof(float))) {
        puts("Sequence dimensions, frame count or FPS exceed bounds");return 3;
    }
    const size_t pixels=static_cast<size_t>(pixels64);
    const uint32_t count=(std::min)(frames,limit);
    const int quality=3,smooth=10;
    const float strength=.85f,response=.2f,seconds=1.f/fps;
    const bool align=true,rotate=false;

    // The namespace is deliberately separate from host keys. A changed model,
    // runtime, input prefix or algorithm setting cannot reuse another analysis.
    SL_HistoryKey prefix;
    constexpr char schema[]="SanLight sequential-analysis-probe float32 surface v1 build8-semantics";
    prefix.append(schema,sizeof(schema));prefix.value(width);prefix.value(height);prefix.value(fps);
    prefix.value(quality);prefix.value(smooth);prefix.value(normalStrength);
    prefix.value(strength);prefix.value(response);prefix.value(seconds);prefix.value(align);prefix.value(rotate);
    const int cpuThreads=SL_AI_ThreadCount();
    if(cpuThreads!=4)prefix.value(cpuThreads);
    wchar_t module[MAX_PATH]={};
    DWORD length=GetModuleFileNameW(nullptr,module,MAX_PATH);
    if(!length||length>=MAX_PATH){puts("Cannot locate executable");return 4;}
    std::wstring directory(module);size_t slash=directory.find_last_of(L"\\/");
    if(slash==std::wstring::npos)return 4;
    directory.resize(slash);
    auto modelStart=Clock::now();
    if(!digestFile(directory+L"\\SanLight_AI\\depth_anything.onnx",prefix)||
        !digestFile(directory+L"\\SanLight_AI\\onnxruntime.dll",prefix)) {
        puts("Cannot hash model/runtime beside executable");return 4;
    }
    const double identitySeconds=elapsed(modelStart);
    SL_DiskCache disk(argv[2],2ULL*1024*1024*1024,maxValue);
    printf("PROTOTYPE width=%u height=%u source_frames=%u frames=%u fps=%.6f model_size=700 smooth=10 normal_strength=%.6f stable_strength=0.85 response=0.2 compression=%d\n",
        width,height,frames,count,fps,normalStrength,SL_DiskCache::compressionAvailable()?1:0);
    fflush(stdout);
    std::vector<unsigned char> bytes(pixels*4);
    std::vector<float> previousImage,previousSurface;
    SL_HistoryKey aggregate;
    double sourceSeconds=0,inputHashSeconds=0,surfaceHashSeconds=0,diskReadSeconds=0,diskWriteSeconds=0;
    double aiSeconds=0,temporalSeconds=0;
    uint32_t hits=0,aiCalls=0,temporalCalls=0,writes=0,writeFailures=0;
    for(uint32_t index=0;index<count;index++) {
        const auto frameStart=Clock::now();
        auto started=Clock::now();
        if(fread(bytes.data(),1,bytes.size(),file.get())!=bytes.size()) {
            printf("Truncated input at frame %u\n",index);return 5;
        }
        std::vector<float> image(pixels*4);
        const float byteScale=1.f/255.f;
        for(size_t i=0;i<pixels;i++) {
            const float alpha=bytes[i*4+3]*byteScale;
            for(int c=0;c<3;c++)image[i*4+c]=(bytes[i*4+c]*byteScale)*alpha+.5f*(1.f-alpha);
            image[i*4+3]=alpha;
        }
        sourceSeconds+=elapsed(started);
        started=Clock::now();
        prefix.value(index);prefix.append(image.data(),image.size()*sizeof(float));
        inputHashSeconds+=elapsed(started);

        std::vector<float> surface;
        started=Clock::now();bool hit=disk.get(prefix,surface);
        if(hit&&surface.size()!=pixels*4){hit=false;surface.clear();}
        diskReadSeconds+=elapsed(started);
        if(hit)++hits;
        else {
            float *depth=nullptr,*normal=nullptr;
            started=Clock::now();++aiCalls;
            int rc=SL_AI_Generate(image.data(),int(width),int(height),quality,smooth,normalStrength,&depth,&normal);
            std::unique_ptr<float,decltype(&free)> ownedDepth(depth,free),ownedNormal(normal,free);
            aiSeconds+=elapsed(started);
            if(rc||!depth||!normal){printf("AI failed: %s\n",SL_AI_LastError());return 6;}
            surface.resize(pixels*4);
            memcpy(surface.data(),depth,pixels*sizeof(float));
            memcpy(surface.data()+pixels,normal,pixels*3*sizeof(float));
            ownedDepth.reset();ownedNormal.reset();
            if(!previousSurface.empty()) {
                started=Clock::now();++temporalCalls;
                if(!SL_TemporalSurface(previousImage.data(),image.data(),previousSurface.data(),surface.data(),
                    int(width),int(height),strength,response,align,seconds,nullptr,nullptr,rotate))return 7;
                temporalSeconds+=elapsed(started);
            }
            started=Clock::now();
            if(disk.put(prefix,surface))++writes;else ++writeFailures;
            diskWriteSeconds+=elapsed(started);
        }
        started=Clock::now();aggregate.append(surface.data(),surface.size()*sizeof(float));
        surfaceHashSeconds+=elapsed(started);
        previousImage=std::move(image);previousSurface=std::move(surface);
        printf("frame=%u/%u cache=%s elapsed_s=%.4f\n",index+1,count,hit?"hit":"miss",elapsed(frameStart));
        fflush(stdout);
    }
    printf("SUMMARY frames=%u hits=%u ai_calls=%u temporal_calls=%u writes=%u write_failures=%u identity_s=%.6f source_s=%.6f input_hash_s=%.6f surface_hash_s=%.6f disk_read_s=%.6f disk_write_s=%.6f ai_s=%.6f temporal_s=%.6f total_s=%.6f surface_hash=%016llx%016llx\n",
        count,hits,aiCalls,temporalCalls,writes,writeFailures,identitySeconds,sourceSeconds,inputHashSeconds,
        surfaceHashSeconds,diskReadSeconds,diskWriteSeconds,aiSeconds,temporalSeconds,elapsed(wholeStart),
        static_cast<unsigned long long>(aggregate.a),static_cast<unsigned long long>(aggregate.b));
    return writeFailures?8:0;
}
