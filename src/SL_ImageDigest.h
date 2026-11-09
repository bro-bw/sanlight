#pragma once
#include <array>
#include <cstddef>
#include <windows.h>
#include <bcrypt.h>

// Exact source bytes for the single-entry derived-map cache only. Persistent
// model/surface keys keep their existing format. Failure means a cache miss.
struct SL_ImageDigest {
    std::array<unsigned char,32> bytes{};
    bool valid=false;
    bool operator==(const SL_ImageDigest& other) const {
        return valid&&other.valid&&bytes==other.bytes;
    }
    static SL_ImageDigest compute(const void* source,size_t length) {
        SL_ImageDigest result;
        if((!source&&length)||length>ULONG_MAX)return result;
        struct Api {
            HMODULE module=LoadLibraryExW(L"bcrypt.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
            decltype(&BCryptHash) hash=module?reinterpret_cast<decltype(hash)>(GetProcAddress(module,"BCryptHash")):nullptr;
            ~Api(){if(module)FreeLibrary(module);}
        };
        static const Api api;
#ifdef SL_TEST_NO_IMAGE_DIGEST
        return result;
#endif
        if(!api.hash)return result;
        static unsigned char empty=0;
        auto input=source?const_cast<unsigned char*>(static_cast<const unsigned char*>(source)):&empty;
        result.valid=api.hash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,input,ULONG(length),result.bytes.data(),ULONG(result.bytes.size()))==0;
        return result;
    }
};
