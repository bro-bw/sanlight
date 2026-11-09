#ifdef SL_TEST_BUILD7
#include "../../../../../versions/v1.3-build7/src/SanLight.cpp"
#else
#include "../../src/SanLight.cpp"
#endif
#include <cstdio>
#include <set>
#include <map>

static std::vector<PF_ParamDef> defs(1);
static std::map<A_long,A_long> requests;
static int outstanding=0,pixelOutstanding=0,suiteOutstanding=0;
static PF_EffectWorld source={},output={};
static PF_WorldSuite2 worldSuite={};static PF_Iterate8Suite2 iterateSuite={};
static bool animated=false;
static bool cropInput=false;
static bool evictAfterPre=false;
static bool evictDuringShade=false;
static int normalMapRequests=0, depthMapRequests=0, inputStateRequests=0;
static A_long expandedTime=-1;
static A_long editTime=-1,failCheckout=-1;
static std::map<A_long,std::vector<PF_Pixel>> framePixels;
static std::map<A_long,PF_EffectWorld> frameWorlds;
static PF_ParamUtilsSuite3 parameterSuite={};
static bool stateSupport=false;
static int hostRevision=1,lightingRevision=1,pixelCalls=0;
static A_long cancelProgress=-1;
static bool forcedAbort=false;
static PF_Err progress(PF_ProgPtr,A_long current,A_long){return current==cancelProgress?PF_Interrupt_CANCEL:PF_Err_NONE;}
static PF_Err getState(PF_ProgPtr,PF_ParamIndex index,const A_Time*,const A_Time*,PF_State* state){
    ++inputStateRequests;
    *state={};state->reservedAL[0]=hostRevision;state->reservedAL[1]=index==SL_INPUT?0:lightingRevision;return 0;
}
static PF_Err sameState(PF_ProgPtr,const PF_State* a,const PF_State* b,A_Boolean* same){
    *same=!memcmp(a,b,sizeof(PF_State));return 0;
}
static PF_Err add(PF_ProgPtr,PF_ParamIndex,PF_ParamDef* p){defs.push_back(*p);return 0;}
static PF_Err parameter(PF_ProgPtr,PF_ParamIndex i,A_long,A_long,A_u_long,PF_ParamDef* p){*p=defs.at(i);++outstanding;return 0;}
static PF_Err checkParameter(PF_ProgPtr,PF_ParamDef*){--outstanding;return 0;}
static PF_Err abortRender(PF_ProgPtr){return forcedAbort?PF_Interrupt_CANCEL:0;}
static PF_Err layer(PF_ProgPtr,PF_ParamIndex i,A_long id,const PF_RenderRequest* r,A_long t,A_long,A_u_long,PF_CheckoutResult* result){
    *result={};
    if(i==SL_NORMAL_LAYER)++normalMapRequests;
    if(i==SL_DEPTH_LAYER)++depthMapRequests;
    if(i!=SL_INPUT)return 0;
    if(r->channel_mask!=PF_ChannelMask_ARGB)return PF_Err_BAD_CALLBACK_PARAM;
    requests[id]=t;
    if(t>=0)result->result_rect=result->max_result_rect={0,0,64,64};
    if(t==expandedTime)result->max_result_rect.right=72;
    return 0;
}
static PF_Err pixels(PF_ProgPtr,A_long id,PF_EffectWorld** result){
    ++pixelCalls;
    if(id==failCheckout)return PF_Err_BAD_CALLBACK_PARAM;
    if(requests.at(id)<0)return PF_Err_BAD_CALLBACK_PARAM;
    if(animated) {
        A_long time=requests.at(id);auto& buffer=framePixels[time];
        if(buffer.empty()) {
            buffer.resize(64*64);const auto* input=(const PF_Pixel*)source.data;
            float shift=.8f*sinf(time*.012f);
            for(int y=0;y<64;y++)for(int x=0;x<64;x++) {
                float px=(std::max)(0.f,(std::min)(63.f,x-shift));int left=(int)px,right=(std::min)(63,left+1);
                float t=px-left;const PF_Pixel& a=input[y*64+left];const PF_Pixel& b=input[y*64+right];
                auto& p=buffer[y*64+x];
                p.alpha=(A_u_char)(a.alpha*(1-t)+b.alpha*t);
                p.red=(A_u_char)(a.red*(1-t)+b.red*t);p.green=(A_u_char)(a.green*(1-t)+b.green*t);
                p.blue=(A_u_char)(a.blue*(1-t)+b.blue*t);
                if(time==editTime && x>=28&&x<36&&y>=28&&y<36)p.red=240;
            }
            auto& world=frameWorlds[time];world=source;world.data=buffer.data();
        }
        auto& world=frameWorlds.at(time);world=source;world.data=buffer.data();
        if(cropInput) {
            int crop=1+(time/40)%3;world.origin_x=crop;world.width=64-crop*2;
            world.data=buffer.data()+crop;
        }
        *result=&world;++pixelOutstanding;return 0;
    }
    *result=&source;++pixelOutstanding;return 0;
}
static PF_Err checkPixels(PF_ProgPtr,A_long){--pixelOutstanding;return 0;}
static PF_Err checkoutOutput(PF_ProgPtr,PF_EffectWorld** result){*result=&output;return 0;}
static PF_Err format(const PF_EffectWorld*,PF_PixelFormat* fmt){*fmt=PF_PixelFormat_ARGB32;return 0;}
static PF_Err iterate(PF_InData*,A_long,A_long,PF_EffectWorld*,const PF_Rect*,void* ctx,
    PF_Err (*fn)(void*,A_long,A_long,PF_Pixel*,PF_Pixel*),PF_EffectWorld* dest){
    if(evictDuringShade) {
        SL_historyCache.clear();
        SL_checkpointCache.clear();
    }
    for(int y=0;y<dest->height;y++)for(int x=0;x<dest->width;x++) {
        PF_Err e=fn(ctx,x,y,nullptr,(PF_Pixel*)((char*)dest->data+y*dest->rowbytes)+x);if(e)return e;
    }return 0;
}
static SPErr acquire(const char* name,int32 version,const void** result){
    *result=nullptr;
    if(stateSupport && version==3 && !strcmp(name,kPFParamUtilsSuite)) {*result=&parameterSuite;++suiteOutstanding;return 0;}
    if(version!=2)return 13;
    if(!strcmp(name,kPFWorldSuite))*result=&worldSuite;
    if(!strcmp(name,kPFIterate8Suite))*result=&iterateSuite;
    if(!*result)return 13;++suiteOutstanding;return 0;
}
static SPErr release(const char*,int32){--suiteOutstanding;return 0;}
int main(){
#ifndef SL_TEST_BUILD7
    SL_diskCacheEnabled=false;
#endif
    PF_InData in={};PF_OutData out={};PF_UtilCallbacks utilities={};
    utilities.ansi.strcpy=strcpy;utilities.ansi.sprintf=sprintf;in.utils=&utilities;
    in.inter.add_param=add;in.inter.checkout_param=parameter;in.inter.checkin_param=checkParameter;in.inter.abort=abortRender;
    in.inter.progress=progress;
    in.width=in.height=64;in.time_scale=1000;in.time_step=40;in.current_time=80;
    SPBasicSuite basic={};basic.AcquireSuite=acquire;basic.ReleaseSuite=release;in.pica_basicP=&basic;
    worldSuite.PF_GetPixelFormat=format;iterateSuite.iterate=iterate;
    parameterSuite.PF_GetCurrentState=getState;parameterSuite.PF_AreStatesIdentical=sameState;
    if(ParamsSetup(&in,&out,nullptr,nullptr)||defs.size()!=SL_NUM_PARAMS)return 1;
    std::set<int> ids;for(size_t i=1;i<defs.size();i++)if(!ids.insert(defs[i].uu.id).second)return 2;
    if(defs[SL_AI_QUALITY].u.pd.value!=4||defs[SL_AI_SMOOTH].u.fs_d.value!=10||defs[SL_AI_STRENGTH].u.fs_d.value!=1000)return 3;
    if(defs[SL_TEMPORAL_ROTATION].u.bd.value || defs[SL_TEMPORAL_ROTATION].uu.id!=126 || defs[SL_END_TEMPORAL].uu.id!=125)return 47;
#ifndef SL_TEST_BUILD7
    if(!defs[SL_PERSISTENT_CACHE].u.bd.value||defs[SL_PERSISTENT_CACHE].uu.id!=127)return 66;
#endif
    if(GlobalSetup(&in,&out,nullptr,nullptr)||out.my_version!=624144||out.out_flags!=0x6000406||out.out_flags2!=0x21400)return 4;
    // Use fast AI only for this fixture; shipping defaults above remain unchanged.
    defs[SL_AI_QUALITY].u.pd.value=1;
    std::vector<PF_Pixel> inputPixels(64*64),outputPixels(64*64);
    for(int y=0;y<64;y++)for(int x=0;x<64;x++)inputPixels[y*64+x]={(A_u_char)(x>5&&x<58?255:0),(A_u_char)(x*3),(A_u_char)(y*3),100};
    source.data=inputPixels.data();source.width=source.height=64;source.rowbytes=64*4;
    output=source;output.data=outputPixels.data();
    auto render=[&](bool enabled)->PF_Err {
        requests.clear();defs[SL_TEMPORAL_ENABLED].u.bd.value=enabled;
        PF_PreRenderInput pri={};pri.output_request.rect={0,0,64,64};
        pri.output_request.channel_mask=PF_ChannelMask_ARGB;
        PF_PreRenderOutput pro={};PF_PreRenderCallbacks cb={};cb.checkout_layer=layer;
        PF_PreRenderExtra pre={&pri,&pro,&cb};PF_Err e=PreRender(&in,&out,&pre);if(e)return e;
        if(evictAfterPre){SL_historyCache.clear();SL_checkpointCache.clear();}
        if(enabled&&in.current_time==80) {
            const SL_Ctx* prepared=static_cast<const SL_Ctx*>(pro.pre_render_data);
            for(A_long i=0;i<2;++i) {
                if(requests.count(SL_HISTORY_CHECKOUT+i)==0||requests.at(SL_HISTORY_CHECKOUT+i)!=40*i)return 10;
#ifdef SL_TEST_BUILD7
                const bool replayDeclared=true;
#else
                const bool replayDeclared=i<prepared->firstNewHistoryIndex;
#endif
                const auto replay=requests.find(SL_HISTORY_CHECKOUT+2+i);
                if((replay!=requests.end())!=replayDeclared||(replayDeclared&&replay->second!=40*i))return 10;
            }
        }
        if(!enabled&&(requests.count(SL_HISTORY_CHECKOUT)||requests.count(SL_HISTORY_CHECKOUT+1)))return 11;
        PF_SmartRenderInput ri={};ri.pre_render_data=pro.pre_render_data;
        PF_SmartRenderCallbacks rcb={};rcb.checkout_layer_pixels=pixels;rcb.checkin_layer_pixels=checkPixels;rcb.checkout_output=checkoutOutput;
        PF_SmartRenderExtra re={&ri,&rcb};e=SmartRender(&in,&out,&re);
        pro.delete_pre_render_data_func(pro.pre_render_data);return e;
    };
    if(render(false))return 5;auto raw=outputPixels;
    if(render(true))return 6;if(memcmp(raw.data(),outputPixels.data(),raw.size()*sizeof(PF_Pixel)))return 7;
    in.current_time=0;if(render(true))return 8;
    if(outstanding||pixelOutstanding||suiteOutstanding)return 9;
    for(int t : {1000,200,800,1000}) {
        in.current_time=t;if(render(true))return 15;
        if(memcmp(raw.data(),outputPixels.data(),raw.size()*sizeof(PF_Pixel)))return 16;
    }
    SL_historyCache.clear();if(render(true))return 17;
    if(memcmp(raw.data(),outputPixels.data(),raw.size()*sizeof(PF_Pixel)))return 18;
    inputPixels[32*64+32].red=240;
    if(render(false))return 19;auto edited=outputPixels;
    if(render(true)||memcmp(edited.data(),outputPixels.data(),edited.size()*sizeof(PF_Pixel)))return 20;
    animated=true;in.current_time=240;framePixels.clear();frameWorlds.clear();
    if(render(true))return 21;auto animation=outputPixels;
    in.current_time=80;if(render(true))return 22;
    in.current_time=240;if(render(true)||memcmp(animation.data(),outputPixels.data(),animation.size()*sizeof(PF_Pixel)))return 23;
    SL_historyCache.clear();if(render(true)||memcmp(animation.data(),outputPixels.data(),animation.size()*sizeof(PF_Pixel)))return 24;
    editTime=120;framePixels.clear();frameWorlds.clear();if(render(true))return 25;auto revised=outputPixels;
    SL_historyCache.clear();if(render(true)||memcmp(revised.data(),outputPixels.data(),revised.size()*sizeof(PF_Pixel)))return 26;
    editTime=280;framePixels.clear();frameWorlds.clear();
    if(render(true)||memcmp(animation.data(),outputPixels.data(),animation.size()*sizeof(PF_Pixel)))return 27;
    failCheckout=SL_HISTORY_CHECKOUT;
    if(!render(true)||outstanding||pixelOutstanding||suiteOutstanding)return 28;
    failCheckout=-1;animated=false;
    stateSupport=true;animated=true;editTime=-1;framePixels.clear();frameWorlds.clear();
    SL_historyCache.clear();SL_checkpointCache.clear();in.current_time=120;
    if(render(true))return 29;
    in.current_time=160;pixelCalls=0;if(render(true)||pixelCalls!=1)return 30;auto fast=outputPixels;
    pixelCalls=0;if(render(true)||pixelCalls!=1||memcmp(fast.data(),outputPixels.data(),fast.size()*sizeof(PF_Pixel)))return 31;
    stateSupport=false;SL_historyCache.clear();SL_checkpointCache.clear();
    if(render(true)||memcmp(fast.data(),outputPixels.data(),fast.size()*sizeof(PF_Pixel)))return 32;
    stateSupport=true;if(render(true))return 33; // promote a pixel-validated checkpoint
    pixelCalls=0;if(render(true)||pixelCalls!=1)return 34;
    editTime=80;++hostRevision;framePixels.clear();frameWorlds.clear();pixelCalls=0;
    if(render(true)||pixelCalls<=1)return 35;auto afterEdit=outputPixels;
    stateSupport=false;SL_historyCache.clear();SL_checkpointCache.clear();
    if(render(true)||memcmp(afterEdit.data(),outputPixels.data(),afterEdit.size()*sizeof(PF_Pixel)))return 36;
    stateSupport=true;editTime=-1;++hostRevision;framePixels.clear();frameWorlds.clear();
    SL_historyCache.clear();SL_checkpointCache.clear();in.current_time=180;
    if(render(true)||requests[SL_HISTORY_CHECKOUT]!=0||requests[SL_HISTORY_CHECKOUT+4]!=160)return 37;
    auto subframe=outputPixels;
    in.current_time=200;if(render(true))return 38;auto gridFrame=outputPixels;
    stateSupport=false;SL_historyCache.clear();SL_checkpointCache.clear();
    if(render(true)||memcmp(gridFrame.data(),outputPixels.data(),gridFrame.size()*sizeof(PF_Pixel)))return 39;
    in.current_time=180;
    if(render(true)||memcmp(subframe.data(),outputPixels.data(),subframe.size()*sizeof(PF_Pixel)))return 40;
    animated=false;
    // A cropped output reads the corresponding full-frame source location.
    output.width=16;output.height=12;output.rowbytes=16*4;output.origin_x=17;output.origin_y=19;
    defs[SL_OPACITY].u.fs_d.value=0;
    if(render(true))return 12;
    if(memcmp(outputPixels.data(),&inputPixels[19*64+17],sizeof(PF_Pixel)))return 13;
    if(outstanding||pixelOutstanding||suiteOutstanding)return 14;
    output=source;output.data=outputPixels.data();defs[SL_OPACITY].u.fs_d.value=100;
    animated=true;in.current_time=200;framePixels.clear();frameWorlds.clear();
    SL_historyCache.clear();SL_checkpointCache.clear();
    if(render(true))return 41;auto fullCanvas=outputPixels;
    cropInput=true;SL_historyCache.clear();SL_checkpointCache.clear();
    if(render(true))return 42;
    for(size_t i=0;i<fullCanvas.size();i++)if(fullCanvas[i].alpha && memcmp(&fullCanvas[i],&outputPixels[i],sizeof(PF_Pixel)))return 43;
    // A normal-strength edit must invalidate the cached joint surface.
    defs[SL_AI_STRENGTH].u.fs_d.value=500;
    if(render(true))return 44;auto adjusted=outputPixels;
    SL_historyCache.clear();SL_checkpointCache.clear();
    if(render(true)||memcmp(adjusted.data(),outputPixels.data(),adjusted.size()*sizeof(PF_Pixel)))return 45;
    if(outstanding||pixelOutstanding||suiteOutstanding)return 46;
    defs[SL_TEMPORAL_ROTATION].u.bd.value=TRUE;
    if(render(true))return 48;auto rotated=outputPixels;
    SL_historyCache.clear();SL_checkpointCache.clear();
    if(render(true)||memcmp(rotated.data(),outputPixels.data(),rotated.size()*sizeof(PF_Pixel)))return 49;
    // A light-only edit preserves the surface anchor and declares only the current frame.
    cropInput=false;stateSupport=true;in.current_time=160;editTime=-1;++hostRevision;
    framePixels.clear();frameWorlds.clear();SL_historyCache.clear();SL_checkpointCache.clear();
    if(render(true))return 50;
    ++lightingRevision;defs[SL_LIGHT_INTENSITY].u.fs_d.value=200;pixelCalls=0;
    if(render(true)||pixelCalls!=1||requests.size()!=1)return 51;auto relit=outputPixels;
    stateSupport=false;SL_historyCache.clear();SL_checkpointCache.clear();
    if(render(true)||memcmp(relit.data(),outputPixels.data(),relit.size()*sizeof(PF_Pixel)))return 52;
    stateSupport=true;if(render(true))return 53;
    // The pre-render anchor remains owned even if the global cache is evicted before SmartRender.
    // Also evict the newly computed surface after its planes are bound for shading.
    evictAfterPre=true;evictDuringShade=true;in.current_time=200;pixelCalls=0;
    if(render(true)||pixelCalls!=1||requests.size()!=1)return 54;auto pinned=outputPixels;
    evictAfterPre=false;evictDuringShade=false;stateSupport=false;SL_historyCache.clear();SL_checkpointCache.clear();
    if(render(true)||memcmp(pinned.data(),outputPixels.data(),pinned.size()*sizeof(PF_Pixel)))return 55;
    // An intermediate frame, not the current frame, grows beyond the previous canvas.
    stateSupport=true;expandedTime=120;in.current_time=80;++hostRevision;
    SL_historyCache.clear();SL_checkpointCache.clear();if(render(true))return 56;
    in.current_time=160;if(render(true)||requests.size()!=9)return 57;auto expanded=outputPixels;
    stateSupport=false;SL_historyCache.clear();SL_checkpointCache.clear();
    if(render(true)||memcmp(expanded.data(),outputPixels.data(),expanded.size()*sizeof(PF_Pixel)))return 58;
    if(outstanding||pixelOutstanding||suiteOutstanding)return 59;
    // Interrupted cold seeks retain completed work; an earlier target must not inherit a future canvas.
    stateSupport=true;expandedTime=-1;in.current_time=480;++hostRevision;
    SL_historyCache.clear();SL_checkpointCache.clear();cancelProgress=9;
    if(render(true)!=PF_Interrupt_CANCEL||outstanding||pixelOutstanding||suiteOutstanding)return 60;
    cancelProgress=-1;SL_historyCache.clear();pixelCalls=0;
    if(render(true)||requests.size()!=9||pixelCalls!=9)return 61;auto resumed=outputPixels;
    stateSupport=false;SL_historyCache.clear();SL_checkpointCache.clear();
    if(render(true)||memcmp(resumed.data(),outputPixels.data(),resumed.size()*sizeof(PF_Pixel)))return 62;
    stateSupport=true;expandedTime=440;++hostRevision;SL_historyCache.clear();SL_checkpointCache.clear();cancelProgress=9;
    if(render(true)!=PF_Interrupt_CANCEL)return 63;
    cancelProgress=-1;SL_historyCache.clear();in.current_time=320;
    if(render(true)||requests.size()!=17)return 64;
    if(outstanding||pixelOutstanding||suiteOutstanding)return 65;
    if(normalMapRequests||depthMapRequests)return 67;
    const int previousStates=inputStateRequests;
    defs[SL_AI_SOURCE].u.pd.value=2;
    if(render(false)||normalMapRequests!=1||depthMapRequests!=1||inputStateRequests!=previousStates)return 68;
    if(outstanding||pixelOutstanding||suiteOutstanding)return 69;
    puts("PASS AI skips custom map declarations; manual mode declares both maps without temporal state");
    puts("PASS cold seek cancellation/resume and partial-checkpoint target horizon");
    puts("PASS light-only reuse, pre-render declaration pruning, pinned eviction, intermediate canvas expansion");
    puts("PASS changing input bounds and normal-strength cache invalidation");
    puts("PASS real AI SmartRender: static/dynamic input, random seek, cold/warm cache, prefix edits, future edits, host-state fast continuation, promotion/invalidation, first frame, ROI, failure cleanup");
    return 0;
}
