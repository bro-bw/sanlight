#pragma once
#include "SL_History.h"
#include "SL_DiskCache.h"
#include <memory>
#include <string>

// Tests turn this off until they provide an isolated cache directory.
static bool SL_diskCacheEnabled=true;

static SL_DiskCache* SL_SurfaceDisk() noexcept {
    if(!SL_diskCacheEnabled)return nullptr;
    try {
        static std::unique_ptr<SL_DiskCache> cache=[]() -> std::unique_ptr<SL_DiskCache> {
            wchar_t setting[32768]={};
            DWORD n=GetEnvironmentVariableW(L"SANLIGHT_DISABLE_DISK_CACHE",setting,32768);
            if(n&&n<32768&&setting[0]==L'1')return {};
            n=GetEnvironmentVariableW(L"SANLIGHT_CACHE_DIR",setting,32768);
            std::wstring root;
            if(n&&n<32768)root.assign(setting,n);
            else {
                n=GetEnvironmentVariableW(L"LOCALAPPDATA",setting,32768);
                if(!n||n>=32768)return {};
                root.assign(setting,n);root+=L"\\SanLight";
                CreateDirectoryW(root.c_str(),nullptr);root+=L"\\Cache";
                CreateDirectoryW(root.c_str(),nullptr);root+=L"\\surface-v1";
            }
            return std::unique_ptr<SL_DiskCache>(new SL_DiskCache(root));
        }();
        return cache.get();
    } catch(...) {return nullptr;}
}
