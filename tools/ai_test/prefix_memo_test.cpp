#include "../../src/SL_PrefixMemo.h"
#include <atomic>
#include <thread>
#include <cstdio>

int main(){
    SL_PrefixMemo memo;
    const int w=77,h=67;int tested=0;
    for(int bpp:{4,8,16}){
        int pitch=w*bpp+4*bpp;std::vector<unsigned char> padded(size_t(h)*pitch),packed(size_t(h)*w*bpp);
        for(int y=0;y<h;++y)for(int x=0;x<w*bpp;++x)packed[size_t(y)*w*bpp+x]=padded[size_t(y)*pitch+x]=uint8_t(7*y+x);
        SL_HistoryKey before,after;before.a=41;before.b=13;after.a=42;after.b=7;
        SL_PrefixMemo::Signature s,query;
        auto sign=[&](const unsigned char* bytes,int stride,int ox,int oy,SL_PrefixMemo::Signature& k) {
            return SL_PrefixMemo::sign(bytes,w,h,stride,bpp,ox,oy,bpp,before,k);
        };
        if(!sign(padded.data(),pitch,-5,11,s))return 1;memo.put(s,after);
        SL_HistoryKey result;
        if(!sign(packed.data(),w*bpp,-5,11,query)||!memo.get(query,result)||result.a!=after.a||result.b!=after.b)return 2;
        if(!sign(padded.data(),pitch,-4,11,query)||memo.get(query,result))return 3;
        if(!sign(padded.data(),pitch,-5,12,query)||memo.get(query,result))return 4;
        if(sign(padded.data(),pitch+1,-5,11,query)||sign(padded.data(),w*bpp-1,-5,11,query))return 5;
        padded[0]^=1;if(!sign(padded.data(),pitch,-5,11,query)||memo.get(query,result))return 6;
        query=s;query.format+=1;if(memo.get(query,result))return 7;
        // Rounding and flush modes distinguish signatures even for identical bytes.
        const auto saved=_mm_getcsr();_mm_setcsr(saved^0x8000);
        if(!sign(packed.data(),w*bpp,-5,11,query)||memo.get(query,result))return 8;_mm_setcsr(saved);
        ++tested;
    }
    // More than four signatures for the same bucket evict the oldest entry.
    SL_PrefixMemo::Signature first{};
    first.before.a=0;
    SL_HistoryKey value;
    for(int i=0;i<5;i++) {
        auto sig=first;
        sig.before.a=uint64_t(i)*2048;
        memo.put(sig,value);
    }
    if(memo.get(first,value))return 10;
    // Make eight callers continuously replace one bucket with different full signatures.
    std::atomic<int> errors{0},hits{0},misses{0};std::vector<std::thread> callers;
    for(int t=0;t<8;++t)callers.emplace_back([&,t] {
        SL_PrefixMemo::Signature sig={};sig.before.a=uint64_t(t)*2048;sig.digest[0]=uint8_t(t);
        SL_HistoryKey expected;expected.a=t+100;expected.b=t+200;
        for(int i=0;i<8000;++i){
            memo.put(sig,expected);SL_HistoryKey received;
            if(memo.get(sig,received)){
                if(received.a!=expected.a||received.b!=expected.b)++errors;
                ++hits;
            }else ++misses;
        }
    });
    for(auto& thread:callers)thread.join();
    printf("formats=%d contended_hits=%d contended_misses=%d incorrect=%d\n",tested,hits.load(),misses.load(),errors.load());
    return errors.load()!=0 ? 9 : 0;
}
