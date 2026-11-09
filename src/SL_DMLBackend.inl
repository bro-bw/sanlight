// Included by SL_AI.cpp only in the Windows DirectML trial build.
// All functions run under the existing AI mutex. No host-global DLL search policy changes.
#include <d3d12.h>
#include <DirectML.h>
#include <dxgi1_2.h>
#include <psapi.h>
#include <wrl/client.h>

namespace sl_dml {
using Microsoft::WRL::ComPtr;

struct File {
    HANDLE handle=INVALID_HANDLE_VALUE;
    explicit File(const std::wstring& path):handle(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)) {}
    ~File(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
    File(const File&)=delete;
    bool sameHandle(HANDLE other) const {
        FILE_ID_INFO a={},b={};
        return GetFileInformationByHandleEx(handle,FileIdInfo,&a,sizeof(a))&&
            GetFileInformationByHandleEx(other,FileIdInfo,&b,sizeof(b))&&
            a.VolumeSerialNumber==b.VolumeSerialNumber&&!memcmp(&a.FileId,&b.FileId,sizeof(a.FileId));
    }
    bool same(const File& other) const {return sameHandle(other.handle);}
};
static std::wstring modulePath(HMODULE module) {
    std::wstring path(32768,L'\0');
    const DWORD n=GetModuleFileNameW(module,&path[0],DWORD(path.size()));
    if(!n||n>=path.size())return {};
    path.resize(n);return path;
}
static bool matches(HMODULE module,HANDLE expected) {
    if(!module)return false;
    const auto path=modulePath(module);
    return !path.empty()&&File(path).sameHandle(expected);
}
static bool testFault(const wchar_t* point) {
#ifdef SL_GPU_TESTING
    wchar_t value[32]={};
    return GetEnvironmentVariableW(L"SANLIGHT_TEST_GPU_FAIL",value,32)<32&&!wcscmp(value,point);
#else
    (void)point;return false;
#endif
}
static std::wstring hostFolder() {
#ifdef SL_GPU_TESTING
    wchar_t fixture[32768]={};DWORD n=GetEnvironmentVariableW(L"SANLIGHT_TEST_AE_DIR",fixture,32768);
    if(n&&n<32768)return fixture;
#endif
    const auto path=modulePath(nullptr);const auto slash=path.find_last_of(L'\\');
    if(slash==std::wstring::npos)return {};
    const wchar_t* exe=path.c_str()+slash+1;
    if(_wcsicmp(exe,L"AfterFX.exe")&&_wcsicmp(exe,L"AfterFX.com")&&_wcsicmp(exe,L"aerender.exe"))return {};
    return path.substr(0,slash);
}
// Even absolute loads must not rely on an unrelated previously loaded DirectML
// dependency. Decline the GPU attempt if this process has ambiguous copies.
static bool importsUnambiguous(const File& runtime,const File& directml) {
    DWORD bytes=0;
    if(!K32EnumProcessModules(GetCurrentProcess(),nullptr,0,&bytes))return false;
    std::vector<HMODULE> list(bytes/sizeof(HMODULE)+32);
    const DWORD capacity=DWORD(list.size()*sizeof(HMODULE));
    if(!K32EnumProcessModules(GetCurrentProcess(),list.data(),capacity,&bytes)||bytes>capacity)return false;
    for(size_t i=0;i<bytes/sizeof(HMODULE);++i) {
        const auto path=modulePath(list[i]);if(path.empty())return false;
        const wchar_t* name=wcsrchr(path.c_str(),L'\\');if(!name)continue;
        if(!_wcsicmp(name+1,L"DirectML.dll")&&!directml.same(File(path)))return false;
        if(!_wcsicmp(name+1,L"onnxruntime.dll")&&!runtime.same(File(path)))return false;
    }
    return true;
}

struct Attempt {
    HMODULE ort=nullptr,dml=nullptr,dxgi=nullptr,d3d=nullptr;
    const OrtApi* api=nullptr;OrtEnv* env=nullptr;OrtSessionOptions* options=nullptr;
    OrtSession* session=nullptr;OrtMemoryInfo* memory=nullptr;OrtAllocator* alloc=nullptr;
    ~Attempt(){
        if(session)api->ReleaseSession(session);
        if(memory)api->ReleaseMemoryInfo(memory);
        if(options)api->ReleaseSessionOptions(options);
        if(env)api->ReleaseEnv(env);
        if(ort)FreeLibrary(ort);
        if(dml)FreeLibrary(dml);
        if(d3d)FreeLibrary(d3d);
        if(dxgi)FreeLibrary(dxgi);
    }
    bool success(OrtStatus* status) const {
        if(!status)return true;
        SL_AI_SetErr(api->GetErrorMessage(status));api->ReleaseStatus(status);return false;
    }
};
static bool reject(const char* why){SL_AI_SetErr(why);return false;}
// Read the graph from the already identified open file. Re-resolving a path
// through a replaced junction could otherwise open bytes different from the key.
static bool openGraph(Attempt& attempt,const File& graph) {
    LARGE_INTEGER bytes={};
    if(!GetFileSizeEx(graph.handle,&bytes)||bytes.QuadPart<=0||uint64_t(bytes.QuadPart)>SIZE_MAX)
        return reject("Invalid model length");
    HANDLE mapping=CreateFileMappingW(graph.handle,nullptr,PAGE_READONLY,0,0,nullptr);
    if(!mapping)return reject("Cannot map model");
    const void* view=MapViewOfFile(mapping,FILE_MAP_READ,0,0,0);
    OrtStatus* error=nullptr;
    if(view)error=attempt.api->CreateSessionFromArray(attempt.env,view,size_t(bytes.QuadPart),attempt.options,&attempt.session);
    if(view)UnmapViewOfFile(view);
    CloseHandle(mapping);
    if(!view)return reject("Cannot read mapped model");
    return attempt.success(error);
}
static bool initialize(const wchar_t* modelPath,int threads) {
    const auto dir=hostFolder();if(dir.empty())return reject("AE host directory unavailable");
    const auto runtimePath=dir+L"\\onnxruntime.dll",directmlPath=dir+L"\\DirectML.dll";
    File runtime(runtimePath),dmlFile(directmlPath),graph(modelPath);
    uint64_t identity[4]={},dmlHash[2]={};
    if(!SL_AI_HashFile(graph.handle,identity)||!SL_AI_HashFile(runtime.handle,identity+2)||
       !SL_AI_HashFile(dmlFile.handle,dmlHash))return reject("AE GPU files or model missing/unreadable");
    if(!importsUnambiguous(runtime,dmlFile))return reject("Unverified or conflicting host runtime");
    Attempt attempt;
    const DWORD flags=LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32;
    attempt.dml=LoadLibraryExW(directmlPath.c_str(),nullptr,flags);
    attempt.ort=LoadLibraryExW(runtimePath.c_str(),nullptr,flags);
    if(!matches(attempt.dml,dmlFile.handle)||!matches(attempt.ort,runtime.handle))return reject("AE runtime identity mismatch");
    attempt.dxgi=LoadLibraryExW(L"dxgi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    attempt.d3d=LoadLibraryExW(L"d3d12.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!attempt.dxgi||!attempt.d3d)return reject("DirectX 12 unavailable");
    auto factoryFn=reinterpret_cast<decltype(&CreateDXGIFactory1)>(GetProcAddress(attempt.dxgi,"CreateDXGIFactory1"));
    auto deviceFn=reinterpret_cast<decltype(&D3D12CreateDevice)>(GetProcAddress(attempt.d3d,"D3D12CreateDevice"));
    auto mlFn=reinterpret_cast<decltype(&DMLCreateDevice)>(GetProcAddress(attempt.dml,"DMLCreateDevice"));
    auto getBase=reinterpret_cast<decltype(&OrtGetApiBase)>(GetProcAddress(attempt.ort,"OrtGetApiBase"));
    // Public ONNX Runtime DML provider ABI from dml_provider_factory.h.
    using AppendDml=OrtStatus*(ORT_API_CALL*)(OrtSessionOptions*,IDMLDevice*,ID3D12CommandQueue*);
    auto append=reinterpret_cast<AppendDml>(GetProcAddress(attempt.ort,"OrtSessionOptionsAppendExecutionProviderEx_DML"));
    if(!factoryFn||!deviceFn||!mlFn||!getBase||!append)return reject("AE DirectML provider API unavailable");
    const auto* base=getBase();
    if(!base||!(attempt.api=base->GetApi(ORT_API_VERSION)))return reject("AE ONNX API version unavailable");
    ComPtr<IDXGIFactory1> factory;
    if(FAILED(factoryFn(IID_PPV_ARGS(&factory))))return reject("DXGI factory unavailable");
    ComPtr<IDXGIAdapter1> selected;DXGI_ADAPTER_DESC1 desc={};
    for(UINT i=0;;++i){
        ComPtr<IDXGIAdapter1> adapter;
        if(factory->EnumAdapters1(i,&adapter)!=S_OK)break;
        DXGI_ADAPTER_DESC1 item={};
        if(FAILED(adapter->GetDesc1(&item))||(item.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))continue;
        if(!selected||item.DedicatedVideoMemory>desc.DedicatedVideoMemory){
            selected=adapter;desc=item;
        }
    }
    if(!selected||testFault(L"adapter"))return reject("No hardware adapter");
    LARGE_INTEGER driver={};
    if(FAILED(selected->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver)))return reject("GPU driver identity unavailable");
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<IDMLDevice> ml;
    if(FAILED(deviceFn(selected.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))))return reject("D3D12 device unavailable");
    const LUID actual=device->GetAdapterLuid();
    if(memcmp(&actual,&desc.AdapterLuid,sizeof(actual)))return reject("GPU adapter changed");
    D3D12_COMMAND_QUEUE_DESC description={};description.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    if(FAILED(device->CreateCommandQueue(&description,IID_PPV_ARGS(&queue)))||
       FAILED(mlFn(device.Get(),DML_CREATE_DEVICE_FLAG_NONE,IID_PPV_ARGS(&ml))))return reject("DirectML device unavailable");
    const auto* api=attempt.api;
    if(!attempt.success(api->CreateEnv(ORT_LOGGING_LEVEL_ERROR,"SanLightDML",&attempt.env))||
       !attempt.success(api->CreateSessionOptions(&attempt.options))||
       !attempt.success(api->SetIntraOpNumThreads(attempt.options,threads))||
       !attempt.success(api->DisableMemPattern(attempt.options))||
       !attempt.success(api->SetSessionExecutionMode(attempt.options,ORT_SEQUENTIAL))||
       !attempt.success(append(attempt.options,ml.Get(),queue.Get())))return false;
    if(testFault(L"session"))return reject("Injected session creation failure");
    if(!openGraph(attempt,graph)||
       !attempt.success(api->CreateCpuMemoryInfo(OrtArenaAllocator,OrtMemTypeDefault,&attempt.memory))||
       !attempt.success(api->GetAllocatorWithDefaultOptions(&attempt.alloc)))return false;
    if(!importsUnambiguous(runtime,dmlFile))return reject("Host runtime changed during setup");
    char* input=nullptr;char* output=nullptr;
    const bool names=attempt.success(api->SessionGetInputName(attempt.session,0,attempt.alloc,&input))&&input&&
        attempt.success(api->SessionGetOutputName(attempt.session,0,attempt.alloc,&output))&&output;
    if(names) {
        strncpy_s(g_inputName,sizeof(g_inputName),input,_TRUNCATE);
        strncpy_s(g_outputName,sizeof(g_outputName),output,_TRUNCATE);
    }
    if(input)api->AllocatorFree(attempt.alloc,input);
    if(output)api->AllocatorFree(attempt.alloc,output);
    if(!names)return reject("GPU model tensor names unavailable");
    SL_HistoryKey key;const char domain[]="SanLight DirectML explicit adapter v1";
    key.append(domain,sizeof(domain));key.append(identity+2,2*sizeof(uint64_t));key.append(dmlHash,sizeof(dmlHash));
    key.value(desc.VendorId);key.value(desc.DeviceId);key.value(desc.SubSysId);key.value(desc.Revision);
    key.value(actual.LowPart);key.value(actual.HighPart);key.value(driver.QuadPart);key.value(threads);
    identity[2]=key.a;identity[3]=key.b;
    memcpy(g_cacheIdentity,identity,sizeof(identity));g_cacheIdentityValid=true;
    g_hOrtDll=attempt.ort;g_ort=api;g_env=attempt.env;g_opt=attempt.options;g_sess=attempt.session;
    g_mem=attempt.memory;g_alloc=attempt.alloc;
    attempt.env=nullptr;attempt.options=nullptr;attempt.session=nullptr;attempt.memory=nullptr;
    // The successful kernel owns these four module references for its lifetime,
    // matching the existing AEX's process-lifetime ORT session.
    attempt.ort=attempt.dml=attempt.dxgi=attempt.d3d=nullptr;
    char deviceName[192]={};
    if(WideCharToMultiByte(CP_ACP,0,desc.Description,-1,deviceName,sizeof(deviceName),nullptr,nullptr)>0)
        _snprintf_s(g_backendInfo,sizeof(g_backendInfo),_TRUNCATE,"DirectML GPU: %s",deviceName);
    else
        sprintf_s(g_backendInfo,"DirectML GPU: %04X/%04X",desc.VendorId,desc.DeviceId);
    g_usingGPU=true;g_ok=true;SL_AI_SetErr("ok");return true;
}
}
