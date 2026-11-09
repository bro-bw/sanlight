// Host scheduling test: deterministic stubs count work, not AI/visual quality.
#define main SL_RealAIHostTests
#include "temporal_host.cpp"
#undef main
#ifndef SL_TEST_BUILD7
#include "sdk_handle_fixture.h"
#endif

static int generated=0,transported=0;
static bool cancelAfterAI=false;
extern "C" int SL_AI_Available(void) {return 1;}
#ifndef SL_TEST_BUILD7
extern "C" int SL_AI_CacheIdentity(uint64_t identity[4]) {
    identity[0]=1;identity[1]=2;identity[2]=3;identity[3]=4;return 1;
}
#endif
extern "C" const char* SL_AI_LastError(void) {return "fixture";}
extern "C" int SL_AI_Generate(const float* rgba,int w,int h,int,int,float,float** depth,float** normal) {
    ++generated;size_t n=size_t(w)*h;
    *depth=(float*)malloc(n*sizeof(float));*normal=(float*)malloc(n*3*sizeof(float));
    if(!*depth||!*normal)return 1;
    for(size_t i=0;i<n;i++) {
        (*depth)[i]=rgba[i*4];
        (*normal)[i*3]=.5f+(rgba[i*4]-.5f)*.2f;
        (*normal)[i*3+1]=.5f;(*normal)[i*3+2]=.99f;
    }
    if(cancelAfterAI)forcedAbort=true;
    return 0;
}
#ifndef SL_TEST_BUILD7
bool SL_TrackSurfaceMotion(const float*,const float*,int,int,SL_TrackResult&,SL_TrackAbort,void*) {
    return false; // Scheduler tests use a CPU-only inference stub.
}
#endif
bool SL_TemporalSurface(const float*,const float*,const float* old,float* surface,
    int w,int h,float,float,bool,float,SL_TrackAbort abort,void* data,bool
#ifndef SL_TEST_BUILD7
    ,SL_TrackResult*
#endif
    ) {
    ++transported;if(abort&&abort(data))return false;
    for(size_t i=0;i<size_t(w)*h*4;i++)surface[i]=old[i]*.9f+surface[i]*.1f;
    return true;
}
#ifndef SL_TEST_BUILD7
extern "C" int SL_AI_GenerateCached(const float* rgba,int w,int h,int q,int smooth,float strength,
    float** depth,float** normal,int,int* inferenceRan) {
    if(inferenceRan)*inferenceRan=1;
    return SL_AI_Generate(rgba,w,h,q,smooth,strength,depth,normal);
}
#endif

int main(int argc,char** argv) {
#ifndef SL_TEST_BUILD7
    SL_diskCacheEnabled=false;
#endif
    PF_InData in={};PF_OutData out={};PF_UtilCallbacks utilities={};
    utilities.ansi.strcpy=strcpy;utilities.ansi.sprintf=sprintf;in.utils=&utilities;
#ifndef SL_TEST_BUILD7
    SDKHandles::install(utilities);
    if(SL_SequenceCommand(PF_Cmd_SEQUENCE_SETUP,&in,&out))return 90;
    in.sequence_data=out.sequence_data;
    struct SequenceGuard {
        PF_InData& in;
        ~SequenceGuard(){PF_OutData out{};SL_SequenceCommand(PF_Cmd_SEQUENCE_SETDOWN,&in,&out);
            if(!SDKHandles::live.empty())std::abort();}
    } sequenceGuard{in};
#endif
    std::uintptr_t rotatingReference=0x2000;
    in.inter.add_param=add;in.inter.checkout_param=parameter;in.inter.checkin_param=checkParameter;
    in.inter.abort=abortRender;in.inter.progress=progress;
    in.width=in.height=64;in.time_scale=1000;in.time_step=40;
    SPBasicSuite basic={};basic.AcquireSuite=acquire;basic.ReleaseSuite=release;in.pica_basicP=&basic;
    worldSuite.PF_GetPixelFormat=format;iterateSuite.iterate=iterate;
    parameterSuite.PF_GetCurrentState=getState;parameterSuite.PF_AreStatesIdentical=sameState;
    if(ParamsSetup(&in,&out,nullptr,nullptr))return 1;
    defs[SL_TEMPORAL_ENABLED].u.bd.value=TRUE;stateSupport=true;animated=true;
    std::vector<PF_Pixel> input(64*64),outputBuffer(64*64);
    for(int y=0;y<64;y++)for(int x=0;x<64;x++)input[y*64+x]={(A_u_char)(x>5&&x<58?255:0),(A_u_char)(x*3),(A_u_char)(y*3),100};
    source.data=input.data();source.width=source.height=64;source.rowbytes=64*4;
    output=source;output.data=outputBuffer.data();
    struct Pending {A_long time;PF_PreRenderOutput pre={};std::map<A_long,A_long> declared;};
    auto prepare=[&](A_long time,Pending& job) {
        in.effect_ref=reinterpret_cast<PF_ProgPtr>(++rotatingReference);
        in.current_time=job.time=time;requests.clear();
        PF_PreRenderInput pri={};pri.output_request.rect={0,0,64,64};pri.bitdepth=8;
        PF_PreRenderCallbacks cb={};cb.checkout_layer=layer;
        PF_PreRenderExtra pre={&pri,&job.pre,&cb};PF_Err e=PreRender(&in,&out,&pre);
        job.declared=requests;return e;
    };
    auto finish=[&](Pending& job) {
        in.effect_ref=reinterpret_cast<PF_ProgPtr>(++rotatingReference);
        in.current_time=job.time;requests=job.declared;
        PF_SmartRenderInput ri={};ri.pre_render_data=job.pre.pre_render_data;
        PF_SmartRenderCallbacks cb={};cb.checkout_layer_pixels=pixels;cb.checkin_layer_pixels=checkPixels;cb.checkout_output=checkoutOutput;
        PF_SmartRenderExtra re={&ri,&cb};PF_Err e=SmartRender(&in,&out,&re);
        job.pre.delete_pre_render_data_func(job.pre.pre_render_data);return e;
    };
    const int count=120;std::vector<PF_Pixel> reference;
#ifndef SL_TEST_BUILD7
    if(argc==3 && !strncmp(argv[1],"--seek-",7)) {
        // The caller sets a fresh private SANLIGHT_CACHE_DIR for cold and late
        // publication, then reopens the same directory for warm/unrelated tests.
        const bool cold=!strcmp(argv[1],"--seek-cold");
        const bool warm=!strcmp(argv[1],"--seek-warm");
        const bool unrelated=!strcmp(argv[1],"--seek-unrelated");
        const bool mixed=!strcmp(argv[1],"--seek-mixed");
        const bool late=!strcmp(argv[1],"--seek-late");
        if(!(cold||warm||unrelated||mixed||late)||!getenv("SANLIGHT_CACHE_DIR"))return 33;
        SL_diskCacheEnabled=!mixed;
        if(unrelated)defs[SL_AI_STRENGTH].u.fs_d.value=543; // stub output stays comparable
        if(mixed) {
            Pending prefix;if(prepare(120,prefix)||finish(prefix))return 34;
            SL_checkpointCache.clear();SL_historyCache.clear();
        }
        Pending job;if(prepare(119*40,job))return 35;
        if(late) {
            // Data arrives after a cold plan was prepared. Drop all RAM entries
            // so the pending job can benefit only from these new disk records.
            Pending publisher;if(prepare(119*40,publisher)||finish(publisher))return 36;
            SL_checkpointCache.clear();SL_historyCache.clear();
        }
        pixelCalls=generated=transported=0;
        if(finish(job))return 37;
        FILE* f=nullptr;if(fopen_s(&f,argv[2],"rb")||!f)return 38;
        std::vector<PF_Pixel> expected(outputBuffer.size());
        bool read=fseek(f,long(118*expected.size()*sizeof(PF_Pixel)),SEEK_SET)==0 &&
                  fread(expected.data(),sizeof(PF_Pixel),expected.size(),f)==expected.size();
        fclose(f);if(!read||memcmp(expected.data(),outputBuffer.data(),expected.size()*sizeof(PF_Pixel)))return 39;
        printf("%s declared=%zu pixels=%d AI=%d temporal=%d exact\n",argv[1],job.declared.size(),pixelCalls,generated,transported);
        const size_t expectedDecl=(warm||unrelated)?239:mixed?124:120;
        const int expectedPixels=unrelated?239:mixed?124:120;
        if(job.declared.size()!=expectedDecl||pixelCalls!=expectedPixels||
           generated!=((warm||late)?0:120)||transported!=((warm||late)?0:119))return 40;
        return outstanding||pixelOutstanding||suiteOutstanding?41:0;
    }
#endif
    // Establish the expected sequential output before testing early PreRender batches.
    for(int frame=0;frame<=count;frame++) {
        Pending job;if(prepare(frame*40,job)||finish(job))return 2;
        if(frame)reference.insert(reference.end(),outputBuffer.begin(),outputBuffer.end());
    }
#ifndef SL_TEST_BUILD7
    // Verify shared storage through the real preparation and render callbacks.
    Pending retained;if(prepare(count*40,retained))return 42;
    auto* pinned=static_cast<SL_Ctx*>(retained.pre.pre_render_data)->pinnedCheckpoint;
    if(!pinned || !*pinned || SL_historyCache.get((*pinned)->prefix)!=(*pinned)->depth)return 43;
    std::weak_ptr<const std::vector<float>> observed=(*pinned)->depth;
    SL_historyCache.clear();SL_checkpointCache.clear();
    if(observed.expired() || finish(retained))return 44;
    if(memcmp(outputBuffer.data(),reference.data()+size_t(count-1)*64*64,64*64*sizeof(PF_Pixel)))return 45;
    if(!observed.expired())return 46;
    puts("PASS shared surface: content cache, host checkpoint and pinned rendering share one immutable vector");
#endif
    for(bool pressure : {false,true}) {
        SL_historyCache.clear();SL_checkpointCache.clear();
        Pending first;if(prepare(0,first)||finish(first))return 3;
        std::vector<Pending> jobs(count);
        for(int frame=0;frame<count;frame++)if(prepare((frame+1)*40,jobs[frame]))return 4;
        generated=transported=pixelCalls=0;
        for(int frame=0;frame<count;frame++) {
            if(pressure)SL_historyCache.clear(); // Simulate eviction by other layers / large surfaces.
            if(finish(jobs[frame]))return 5;
            if(memcmp(outputBuffer.data(),reference.data()+size_t(frame)*64*64,64*64*sizeof(PF_Pixel)))return 6;
        }
        printf("batch=120 pressure=%d pixel_checkouts=%d ai_calls=%d temporal_calls=%d output=identical\n",
            pressure?1:0,pixelCalls,generated,transported);
#ifndef SL_TEST_BUILD7
        if(pixelCalls!=count||generated>count||transported>count)return 7;
#endif
    }
    // Later completed frames must never become history for an earlier target.
    SL_historyCache.clear();SL_checkpointCache.clear();
    Pending first;if(prepare(0,first)||finish(first))return 11;
    std::vector<Pending> reverseJobs(12);
    for(int frame=0;frame<12;frame++)if(prepare((frame+1)*40,reverseJobs[frame]))return 12;
    for(int frame=11;frame>=0;frame--) {
        SL_historyCache.clear();if(finish(reverseJobs[frame]))return 13;
        if(memcmp(outputBuffer.data(),reference.data()+size_t(frame)*64*64,64*64*sizeof(PF_Pixel)))return 14;
    }
    if(outstanding||pixelOutstanding||suiteOutstanding)return 8;
#ifndef SL_TEST_BUILD7
    wchar_t cwd[32768]={},cachePath[32768]={};
    if(!GetCurrentDirectoryW(32768,cwd))return 15;
    if(swprintf_s(cachePath,32768,L"%s\\disk-host-%lu-%llu",cwd,GetCurrentProcessId(),GetTickCount64())<0)return 16;
    if(!SetEnvironmentVariableW(L"SANLIGHT_CACHE_DIR",cachePath))return 17;
    SL_diskCacheEnabled=true;SL_historyCache.clear();SL_checkpointCache.clear();
    for(int frame=0;frame<=6;frame++) {
        Pending job;if(prepare(frame*40,job)||finish(job))return 18;
    }
    // Simulate losing all RAM state while persisted content remains.
    SL_historyCache.clear();SL_checkpointCache.clear();generated=transported=pixelCalls=0;
    Pending cached;if(prepare(240,cached)||finish(cached))return 19;
    if(generated||transported||pixelCalls!=7)return 20;
    if(memcmp(outputBuffer.data(),reference.data()+size_t(5)*64*64,64*64*sizeof(PF_Pixel)))return 21;
    printf("disk_recovery pixel_checkouts=%d ai_calls=%d temporal_calls=%d output=identical\n",pixelCalls,generated,transported);
    defs[SL_PERSISTENT_CACHE].u.bd.value=FALSE;
    SL_historyCache.clear();SL_checkpointCache.clear();generated=transported=0;
    Pending disabled;if(prepare(240,disabled)||finish(disabled))return 27;
    if(generated!=7||transported!=6)return 28;
    if(memcmp(outputBuffer.data(),reference.data()+size_t(5)*64*64,64*64*sizeof(PF_Pixel)))return 29;
    defs[SL_PERSISTENT_CACHE].u.bd.value=TRUE;
    editTime=80;++hostRevision;framePixels.clear();frameWorlds.clear();
    SL_historyCache.clear();SL_checkpointCache.clear();generated=transported=0;
    Pending edited;if(prepare(240,edited)||finish(edited))return 22;
    if(generated!=5||transported!=5)return 23;auto editedOutput=outputBuffer;
    SL_diskCacheEnabled=false;SL_historyCache.clear();SL_checkpointCache.clear();
    Pending rebuilt;if(prepare(240,rebuilt)||finish(rebuilt))return 24;
    if(memcmp(outputBuffer.data(),editedOutput.data(),64*64*sizeof(PF_Pixel)))return 25;
    if(outstanding||pixelOutstanding||suiteOutstanding)return 26;
    SL_diskCacheEnabled=true;SL_historyCache.clear();SL_checkpointCache.clear();
    defs[SL_AI_STRENGTH].u.fs_d.value=777;cancelAfterAI=true;
    Pending cancelled;if(prepare(0,cancelled))return 30;
    if(finish(cancelled)!=PF_Interrupt_CANCEL)return 31;
    cancelAfterAI=forcedAbort=false;
    if(outstanding||pixelOutstanding||suiteOutstanding)return 32;
    puts("PASS disk recovery without inference, prefix edit invalidation and exact fallback");
    puts("PASS final-frame cancellation during cache write is not swallowed");
#endif
    if(argc>1) {
        FILE* f=nullptr;if(fopen_s(&f,argv[1],"wb")||!f)return 9;
        bool ok=fwrite(reference.data(),sizeof(PF_Pixel),reference.size(),f)==reference.size();
        fclose(f);if(!ok)return 10;
    }
    puts("PASS early batch preparation, checkpoint refresh, content-cache eviction, exact output and callback cleanup");
    return 0;
}
