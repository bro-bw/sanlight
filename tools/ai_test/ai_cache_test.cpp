#ifdef SL_AI_BASELINE8
#include "../../../../../versions/v1.3-build8/src/SL_AI.cpp"
#else
#include "../../src/SL_AI.cpp"
#endif
#include <chrono>
#include <cstdio>
#include <vector>
#include <string>

struct Scenario {int w,h,quality,smooth;float strength;int image,alpha;int coldRun;};
static const Scenario cases[]={
    {96,80,3,10,1000,0,0,1},
    {96,80,3,10,1000,0,0,0},
    {96,80,3,10,1000,0,1,0},
    {96,80,3,2,300,0,0,0},
    {123,95,3,10,1000,0,0,1},
    {64,64,3,10,1000,2,0,1},
    {128,96,3,4,600,2,1,0},
    {96,80,0,10,1000,0,0,1},
    {96,80,3,10,1000,1,0,1}
};
static std::vector<float> image(const Scenario& scene) {
    std::vector<float> pixels(size_t(scene.w)*scene.h*4);
    for(int y=0;y<scene.h;y++)for(int x=0;x<scene.w;x++) {
        float* p=pixels.data()+(size_t(y)*scene.w+x)*4;
        p[0]=.15f+.6f*x/(scene.w-1);p[1]=.12f+.55f*y/(scene.h-1);
        p[2]=.2f+.15f*sinf(x*.13f)*cosf(y*.17f);
        if(scene.image==1)p[0]=fminf(.95f,p[0]+.1f);
        if(scene.image==2)p[0]=p[1]=p[2]=0;
        p[3]=scene.alpha&&(x<scene.w/3||(x>scene.w/2&&x<scene.w*2/3&&y>scene.h/3))?0.f:1.f;
    }
    return pixels;
}
static bool equal(const std::vector<float>& a,const std::vector<float>& b) {
    return a.size()==b.size()&&(a.empty()||!memcmp(a.data(),b.data(),a.size()*sizeof(float)));
}
static void clearRAM() {
    free(g_cacheNear);free(g_cacheNorm);g_cacheNear=g_cacheNorm=nullptr;
#ifndef SL_AI_BASELINE8
    g_rawCache.clear();g_rawCacheValid=false;
#endif
}
static bool generate(const Scenario& scene,bool cached,std::vector<float>& result,int& ran) {
    auto rgba=image(scene);float *depth=nullptr,*normal=nullptr;ran=-1;
#ifdef SL_AI_BASELINE8
    int error=SL_AI_Generate(rgba.data(),scene.w,scene.h,scene.quality,scene.smooth,scene.strength,&depth,&normal);
#else
    int error=SL_AI_GenerateCached(rgba.data(),scene.w,scene.h,scene.quality,scene.smooth,scene.strength,&depth,&normal,cached?1:0,&ran);
#endif
    if(error||!depth||!normal){fprintf(stderr,"AI error %d: %s\n",error,SL_AI_LastError());free(depth);free(normal);return false;}
    size_t n=size_t(scene.w)*scene.h;result.resize(n*4);
    memcpy(result.data(),depth,n*4);memcpy(result.data()+n,normal,n*12);free(depth);free(normal);return true;
}
static bool writeResults(const std::wstring& file,const std::vector<std::vector<float>>& results) {
    FILE* f=nullptr;if(_wfopen_s(&f,file.c_str(),L"wb")||!f)return false;
    bool okay=true;for(const auto& value:results) {
        uint64_t count=value.size();okay=okay&&fwrite(&count,sizeof(count),1,f)==1&&fwrite(value.data(),4,value.size(),f)==value.size();
    }
    fclose(f);return okay;
}
static bool readResults(const std::wstring& file,std::vector<std::vector<float>>& results) {
    FILE* f=nullptr;if(_wfopen_s(&f,file.c_str(),L"rb")||!f)return false;
    bool okay=true;results.clear();
    for(const auto& scene:cases) {
        uint64_t count=0;if(fread(&count,sizeof(count),1,f)!=1||count!=size_t(scene.w)*scene.h*4){okay=false;break;}
        results.emplace_back(size_t(count));
        if(fread(results.back().data(),4,size_t(count),f)!=count){okay=false;break;}
    }
    if(okay&&fgetc(f)!=EOF)okay=false;fclose(f);return okay;
}
#ifndef SL_AI_BASELINE8
static std::wstring rawFile(const std::wstring& root,const SL_HistoryKey& key) {
    wchar_t filename[40]={};swprintf_s(filename,L"%016llx%016llx.sldc",key.a,key.b);
    return root+L"\\raw-model-v1\\"+filename;
}
static uint64_t cacheBytes(const std::wstring& root) {
    WIN32_FIND_DATAW data={};HANDLE files=FindFirstFileW((root+L"\\raw-model-v1\\*.sldc").c_str(),&data);
    if(files==INVALID_HANDLE_VALUE)return 0;uint64_t result=0;
    do {result+=(uint64_t(data.nFileSizeHigh)<<32)|data.nFileSizeLow;}while(FindNextFileW(files,&data));FindClose(files);return result;
}
static SL_HistoryKey rawKey(const Scenario& scene,const uint64_t* identity) {
    auto rgba=image(scene);int q=QUALITY_SIZES[scene.quality];std::vector<float> input(size_t(q)*q*3);
    SL_DownscaleNormCHW(rgba.data(),scene.w,scene.h,q,q,input.data());return SL_AI_RawKey(input.data(),q,identity);
}
static int warm(const std::wstring& root,const std::wstring& reference) {
    SetEnvironmentVariableW(L"SANLIGHT_DISABLE_DISK_CACHE",nullptr);SetEnvironmentVariableW(L"SANLIGHT_CACHE_DIR",root.c_str());
    std::vector<std::vector<float>> expected;if(!readResults(reference,expected))return 31;
    int runs=0;for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        std::vector<float> result;int ran;
        if(!generate(cases[i],true,result,ran)||ran!=0||!equal(result,expected[i]))return 32;
        runs+=ran;
    }
    printf("PASS fresh process: cases=9 actual_ONNX_Run=%d all surfaces bit-exact\n",runs);return 0;
}
#endif
int wmain(int argc,wchar_t** argv) {
#ifdef SL_AI_BASELINE8
    if(argc!=2)return 1;
    std::vector<std::vector<float>> references;
    for(const auto& scene:cases){clearRAM();std::vector<float> value;int ran;if(!generate(scene,false,value,ran))return 2;references.push_back(std::move(value));}
    if(!writeResults(argv[1],references))return 3;
    puts("PASS frozen build8 reference generated (9 cases)");return 0;
#else
    if(argc==4&&!wcscmp(argv[1],L"--warm"))return warm(argv[2],argv[3]);
    if(argc!=2)return 1;
    wchar_t cwd[32768]={};if(!GetCurrentDirectoryW(32768,cwd))return 2;
    wchar_t suffix[80]={};swprintf_s(suffix,L"\\run-%lu-%llu",GetCurrentProcessId(),GetTickCount64());
    std::wstring root=std::wstring(cwd)+suffix;
    if(!CreateDirectoryW(root.c_str(),nullptr))return 3;
    std::wstring cacheRoot=root+L"\\cache";
    SetEnvironmentVariableW(L"SANLIGHT_CACHE_DIR",cacheRoot.c_str());
    SetEnvironmentVariableW(L"SANLIGHT_DISABLE_DISK_CACHE",nullptr);
    std::vector<std::vector<float>> oldResults;if(!readResults(argv[1],oldResults))return 4;
    std::vector<std::vector<float>> references;
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        clearRAM();std::vector<float> value;int ran;
        if(!generate(cases[i],false,value,ran)||ran!=1||!equal(value,oldResults[i]))return 5;
        references.push_back(std::move(value));
    }
    if(GetFileAttributesW(cacheRoot.c_str())!=INVALID_FILE_ATTRIBUTES)return 6;
    // The legacy entry point must also remain free of persistent writes.
    clearRAM();auto rgba=image(cases[0]);float *depth=nullptr,*normal=nullptr;
    if(SL_AI_Generate(rgba.data(),cases[0].w,cases[0].h,3,10,1000,&depth,&normal))return 7;
    free(depth);free(normal);if(GetFileAttributesW(cacheRoot.c_str())!=INVALID_FILE_ATTRIBUTES)return 8;
    puts("PASS original and disabled APIs: no persistent files; output matches frozen build8");
    clearRAM();int coldRuns=0;
    auto start=std::chrono::steady_clock::now();
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        std::vector<float> result;int ran;
        if(!generate(cases[i],true,result,ran)||!equal(result,references[i]))return 9;
        if(ran!=cases[i].coldRun){printf("Unexpected run count at case %zu: %d\n",i,ran);return 10;}
        coldRuns+=ran;
    }
    printf("PASS cold cache: cases=9 actual_ONNX_Run=%d elapsed=%.3fs stored=%llu bytes\n",coldRuns,
        std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count(),
        static_cast<unsigned long long>(cacheBytes(cacheRoot)));
    std::wstring reference=root+L"\\reference.bin";if(!writeResults(reference,references))return 11;
    wchar_t exe[32768]={};if(!GetModuleFileNameW(nullptr,exe,32768))return 12;
    std::wstring command=L"\""+std::wstring(exe)+L"\" --warm \""+cacheRoot+L"\" \""+reference+L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(),command.end());mutableCommand.push_back(0);
    STARTUPINFOW startup={};startup.cb=sizeof(startup);PROCESS_INFORMATION process={};
    if(!CreateProcessW(exe,mutableCommand.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))return 13;
    WaitForSingleObject(process.hProcess,120000);DWORD status=1;GetExitCodeProcess(process.hProcess,&status);
    CloseHandle(process.hProcess);CloseHandle(process.hThread);if(status){printf("Warm process failed: %lu\n",status);return 14;}
    puts("PASS fresh-process disk reuse: all 9 cases use zero ONNX Run calls");

    uint64_t identity[4];if(!SL_AI_CacheIdentity(identity))return 15;
    SL_HistoryKey originalKey=rawKey(cases[0],identity);uint64_t changed[4];memcpy(changed,identity,sizeof(changed));changed[0]^=1;
    SL_HistoryKey changedKey=rawKey(cases[0],changed);
    if(!(originalKey<changedKey)&&!(changedKey<originalKey))return 16;
    if(GetFileAttributesW(rawFile(cacheRoot,changedKey).c_str())!=INVALID_FILE_ATTRIBUTES)return 17;
    // Simulate a distinct loaded model identity without modifying any model file.
    clearRAM();g_cacheIdentity[0]^=1;std::vector<float> result;int ran;
    if(!generate(cases[0],true,result,ran)||ran!=1||!equal(result,references[0]))return 18;
    g_cacheIdentity[0]^=1;clearRAM();
    if(!generate(cases[0],true,result,ran)||ran||!equal(result,references[0]))return 19;
    puts("PASS model/runtime identity partitioning (synthetic alternate identity fixture)");

    SetEnvironmentVariableW(L"SANLIGHT_DISABLE_DISK_CACHE",L"1");clearRAM();
    uint64_t before=cacheBytes(cacheRoot);
    if(!generate(cases[0],true,result,ran)||ran!=1||!equal(result,references[0])||cacheBytes(cacheRoot)!=before)return 20;
    SetEnvironmentVariableW(L"SANLIGHT_DISABLE_DISK_CACHE",nullptr);
    clearRAM();if(!generate(cases[0],false,result,ran)||ran!=1||!equal(result,references[0]))return 21;
    puts("PASS process disable setting and per-call opt-out bypass persistent reads/writes");

    std::wstring corrupted=rawFile(cacheRoot,originalKey);
    HANDLE file=CreateFileW(corrupted.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE)return 22;
    LARGE_INTEGER position={};position.QuadPart=96;unsigned char byte=0;DWORD count=0;
    bool okay=SetFilePointerEx(file,position,nullptr,FILE_BEGIN)&&ReadFile(file,&byte,1,&count,nullptr)&&count==1;
    byte^=0x5a;okay=okay&&SetFilePointerEx(file,position,nullptr,FILE_BEGIN)&&WriteFile(file,&byte,1,&count,nullptr)&&count==1;
    CloseHandle(file);if(!okay)return 23;
    clearRAM();if(!generate(cases[0],true,result,ran)||ran!=1||!equal(result,references[0]))return 24;
    clearRAM();if(!generate(cases[0],true,result,ran)||ran||!equal(result,references[0]))return 25;
    puts("PASS damaged raw cache falls back to inference and repaired data remains exact");
    wprintf(L"Fixtures: %s\n",root.c_str());return 0;
#endif
}
