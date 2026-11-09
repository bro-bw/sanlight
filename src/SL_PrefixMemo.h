#pragma once
#include "SL_History.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cstring>
#include <xmmintrin.h>

// Memoize a versioned native-pixel prefix transition. Each hit validates all source bytes.
// Every cache hit still checks all native source pixels supplied by the host.
class SL_PrefixMemo {
public:
    struct Signature {
        SL_HistoryKey before;
        int width, height, originX, originY, format;
        unsigned floatMode;
        std::array<unsigned char,32> digest;
    };
private:
    struct Entry { bool valid=false; Signature input{}; SL_HistoryKey output; };
    struct Bucket {std::array<Entry,4> entries{};unsigned next=0;};
    std::array<Bucket,512> buckets{};
    std::mutex lock;
    struct HashApi {
        HMODULE module=LoadLibraryExW(L"bcrypt.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        decltype(&BCryptHash) one=nullptr;
        decltype(&BCryptCreateHash) create=nullptr;
        decltype(&BCryptHashData) update=nullptr;
        decltype(&BCryptFinishHash) finish=nullptr;
        decltype(&BCryptDestroyHash) destroy=nullptr;
        HashApi() {
            if(!module)return;
            one=reinterpret_cast<decltype(one)>(GetProcAddress(module,"BCryptHash"));
            create=reinterpret_cast<decltype(create)>(GetProcAddress(module,"BCryptCreateHash"));
            update=reinterpret_cast<decltype(update)>(GetProcAddress(module,"BCryptHashData"));
            finish=reinterpret_cast<decltype(finish)>(GetProcAddress(module,"BCryptFinishHash"));
            destroy=reinterpret_cast<decltype(destroy)>(GetProcAddress(module,"BCryptDestroyHash"));
        }
        ~HashApi(){if(module)FreeLibrary(module);}
    };
    static bool same(const Signature& x,const Signature& y) {
        return x.before.a==y.before.a && x.before.b==y.before.b &&
               x.width==y.width && x.height==y.height && x.originX==y.originX &&
               x.originY==y.originY && x.format==y.format && x.floatMode==y.floatMode &&
               x.digest==y.digest;
    }
    Bucket& bucket(const Signature& s) {
        return buckets[(s.before.a ^ (s.before.a >> 32) ^ s.before.b) % buckets.size()];
    }
public:
    static bool sign(const unsigned char* data,int w,int h,int stride,int pixelBytes,
                     int originX,int originY,int format,const SL_HistoryKey& before,Signature& s) {
        if(!data || w<1 || h<1 || pixelBytes<1 || stride<1 || stride%pixelBytes!=0) return false;
        const uint64_t row=uint64_t(w)*pixelBytes;
        if(row>uint64_t(stride))return false;
        static const HashApi api;
        s.before=before;s.width=w;s.height=h;s.originX=originX;s.originY=originY;s.format=format;
        s.floatMode=_mm_getcsr() & 0xFFC0u; // mask exception status bits
        if(row==uint64_t(stride) && row*h<=ULONG_MAX && api.one)
            return api.one(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,const_cast<unsigned char*>(data),
                           ULONG(row*h),s.digest.data(),ULONG(s.digest.size()))==0;
        if(!api.create||!api.update||!api.finish||!api.destroy)return false;
        BCRYPT_HASH_HANDLE hash=nullptr;
        if(api.create(BCRYPT_SHA256_ALG_HANDLE,&hash,nullptr,0,nullptr,0,0)!=0)return false;
        bool ok=true;
        for(int y=0;y<h && ok;y++)
            ok=api.update(hash,const_cast<unsigned char*>(data+size_t(y)*stride),ULONG(row),0)==0;
        if(ok)ok=api.finish(hash,s.digest.data(),ULONG(s.digest.size()),0)==0;
        api.destroy(hash);
        return ok;
    }
    static SL_HistoryKey advance(const Signature& s) {
        // Native digest + interpretation/canvas placement completely determines converted pixels.
        // Different representations may miss the cache; they must never alias incorrect pixels.
        SL_HistoryKey after=s.before;
        const uint32_t nativeEncoding=0x324E4154u;after.value(nativeEncoding);
        after.value(s.width);after.value(s.height);after.value(s.originX);after.value(s.originY);
        after.value(s.format);after.value(s.floatMode);after.append(s.digest.data(),s.digest.size());
        return after;
    }
    bool get(const Signature& s,SL_HistoryKey& after) {
        std::lock_guard<std::mutex> guard(lock);
        for(const auto& entry:bucket(s).entries) {
            if(entry.valid && same(entry.input,s)) {after=entry.output;return true;}
        }
        return false;
    }
    void put(const Signature& s,const SL_HistoryKey& after) {
        std::lock_guard<std::mutex> guard(lock);
        auto& group=bucket(s);
        for(const auto& entry:group.entries)if(entry.valid && same(entry.input,s))return;
        group.entries[group.next]={true,s,after};
        group.next=(group.next+1)%group.entries.size();
    }
};
