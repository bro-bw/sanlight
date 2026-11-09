#pragma once
// Include after SL_HistoryKey is defined. This cache stores content-addressed
// float arrays, never PF_State or effect_ref, and owns only its versioned files.
#include <Windows.h>
#include <compressapi.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <string>
#include <vector>

class SL_DiskCache {
public:
    using Abort = bool (*)(void*);
private:
    static constexpr uint32_t blockBytes=1024*1024;
    // Small raw model outputs keep the v1 encoding. Full surfaces benefit from
    // grouping like exponent/mantissa bytes before the ordinary XPRESS codec.
    static constexpr uint64_t shuffleThreshold=4ULL*1024*1024;
    static constexpr uint64_t initialHash=1469598103934665603ULL;
    struct Header {
        char magic[8];
        uint32_t version,headerBytes;
        uint64_t keyA,keyB,valueCount,rawBytes,fileBytes,payloadHash;
        uint32_t blockSize,blockCount;
        uint64_t headerHash;
    };
    struct Block {
        uint32_t rawBytes,storedBytes,codec,reserved;
        uint64_t hash;
    };
    static_assert(sizeof(Header)==80,"Disk cache header layout");
    static_assert(sizeof(Block)==24,"Disk cache block layout");
    struct Handle {
        HANDLE value=INVALID_HANDLE_VALUE;
        Handle()=default;
        explicit Handle(HANDLE v):value(v){}
        ~Handle(){if(value!=INVALID_HANDLE_VALUE&&value!=nullptr)CloseHandle(value);}
        Handle(const Handle&)=delete;
        Handle& operator=(const Handle&)=delete;
        Handle(Handle&& other) noexcept:value(other.value){other.value=INVALID_HANDLE_VALUE;}
        bool valid() const {return value!=INVALID_HANDLE_VALUE&&value!=nullptr;}
    };
    struct CodecApi {
        HMODULE module=nullptr;
        decltype(&CreateCompressor) createCompressor=nullptr;
        decltype(&Compress) compress=nullptr;
        decltype(&CloseCompressor) closeCompressor=nullptr;
        decltype(&CreateDecompressor) createDecompressor=nullptr;
        decltype(&Decompress) decompress=nullptr;
        decltype(&CloseDecompressor) closeDecompressor=nullptr;
        CodecApi() {
            module=LoadLibraryExW(L"cabinet.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(!module)return;
            createCompressor=reinterpret_cast<decltype(createCompressor)>(GetProcAddress(module,"CreateCompressor"));
            compress=reinterpret_cast<decltype(compress)>(GetProcAddress(module,"Compress"));
            closeCompressor=reinterpret_cast<decltype(closeCompressor)>(GetProcAddress(module,"CloseCompressor"));
            createDecompressor=reinterpret_cast<decltype(createDecompressor)>(GetProcAddress(module,"CreateDecompressor"));
            decompress=reinterpret_cast<decltype(decompress)>(GetProcAddress(module,"Decompress"));
            closeDecompressor=reinterpret_cast<decltype(closeDecompressor)>(GetProcAddress(module,"CloseDecompressor"));
        }
        ~CodecApi(){if(module)FreeLibrary(module);}
        bool available() const {
            return createCompressor&&compress&&closeCompressor&&createDecompressor&&decompress&&closeDecompressor;
        }
    };
    struct Compressor {
        CodecApi& api;COMPRESSOR_HANDLE handle=nullptr;
        Compressor(CodecApi& a,bool enabled):api(a) {
            if(enabled&&api.available())api.createCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,nullptr,&handle);
        }
        ~Compressor(){if(handle)api.closeCompressor(handle);}
    };
    struct Decompressor {
        CodecApi& api;DECOMPRESSOR_HANDLE handle=nullptr;
        explicit Decompressor(CodecApi& a):api(a){}
        ~Decompressor(){if(handle)api.closeDecompressor(handle);}
        bool expand(const void* source,size_t stored,void* destination,size_t raw) {
            if(!handle&&(!api.available()||!api.createDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,nullptr,&handle)))return false;
            SIZE_T written=0;
            return api.decompress(handle,source,stored,destination,raw,&written)&&written==raw;
        }
    };
    struct MutexGuard {
        Handle handle;bool owned=false;
        explicit MutexGuard(const wchar_t* name):handle(CreateMutexW(nullptr,FALSE,name)){}
        ~MutexGuard(){if(owned)ReleaseMutex(handle.value);}
        bool acquire(Abort abort,void* data) {
            if(!handle.valid())return false;
            // Abort callbacks are outside the cache's cross-process mutex.
            for(int tries=0;tries<200;tries++) {
                if(abort&&abort(data))return false;
                DWORD state=WaitForSingleObject(handle.value,50);
                if(state==WAIT_OBJECT_0||state==WAIT_ABANDONED){owned=true;return true;}
                if(state!=WAIT_TIMEOUT)return false;
            }
            return false;
        }
    };
    struct Entry {std::wstring name;uint64_t bytes,age;};
    std::wstring root_,mutexName_;
    uint64_t budget_,maxValue_;
    bool compress_;

    static CodecApi& codec(){static CodecApi instance;return instance;}
    static uint64_t hash(const void* data,size_t bytes,uint64_t value=initialHash) {
        // Word-wise hashing avoids another byte-wise pass over full-size surfaces.
        const auto* source=static_cast<const unsigned char*>(data);
        while(bytes>=8) {
            uint64_t word;memcpy(&word,source,8);
            value=(value^word)*1099511628211ULL;value^=value>>32;
            source+=8;bytes-=8;
        }
        while(bytes--){value=(value^*source++)*1099511628211ULL;}
        return value;
    }
    static bool read(HANDLE file,void* data,DWORD bytes) {
        DWORD got=0;return ReadFile(file,data,bytes,&got,nullptr)&&got==bytes;
    }
    static void toBytePlanes(const unsigned char* raw,unsigned char* planes,size_t count) {
        for(size_t i=0;i<count;++i) {
            planes[i]=raw[4*i];planes[count+i]=raw[4*i+1];
            planes[2*count+i]=raw[4*i+2];planes[3*count+i]=raw[4*i+3];
        }
    }
    static void fromBytePlanes(const unsigned char* planes,unsigned char* raw,size_t count) {
        for(size_t i=0;i<count;++i) {
            raw[4*i]=planes[i];raw[4*i+1]=planes[count+i];
            raw[4*i+2]=planes[2*count+i];raw[4*i+3]=planes[3*count+i];
        }
    }
    static bool write(HANDLE file,const void* data,DWORD bytes) {
        DWORD sent=0;return WriteFile(file,data,bytes,&sent,nullptr)&&sent==bytes;
    }
    static bool fileSize(HANDLE file,uint64_t& bytes) {
        LARGE_INTEGER size={};if(!GetFileSizeEx(file,&size)||size.QuadPart<0)return false;
        bytes=uint64_t(size.QuadPart);return true;
    }
    static std::wstring name(const SL_HistoryKey& key) {
        wchar_t value[40]={};swprintf_s(value,L"%016llx%016llx.sldc",key.a,key.b);return value;
    }
    std::wstring path(const std::wstring& name) const {return root_+L"\\"+name;}
    static bool keyFromName(const wchar_t* filename,SL_HistoryKey& key) {
        if(wcslen(filename)!=37||wcscmp(filename+32,L".sldc"))return false;
        key.a=key.b=0;
        for(int i=0;i<32;i++) {
            unsigned value;
            if(filename[i]>=L'0'&&filename[i]<=L'9')value=unsigned(filename[i]-L'0');
            else if(filename[i]>=L'a'&&filename[i]<=L'f')value=unsigned(filename[i]-L'a'+10);
            else return false;
            uint64_t& target=i<16?key.a:key.b;target=(target<<4)|value;
        }
        return true;
    }
    bool rootReady(bool create) const {
        if(root_.empty())return false;
        DWORD flags=GetFileAttributesW(root_.c_str());
        if(flags==INVALID_FILE_ATTRIBUTES&&create) {
            if(!CreateDirectoryW(root_.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return false;
            flags=GetFileAttributesW(root_.c_str());
        }
        return flags!=INVALID_FILE_ATTRIBUTES&&(flags&FILE_ATTRIBUTE_DIRECTORY)&&!(flags&FILE_ATTRIBUTE_REPARSE_POINT);
    }
    static bool headerValid(const Header& h,const SL_HistoryKey& key,uint64_t size,uint64_t limit) {
        if(memcmp(h.magic,"SLDCS001",8)||h.version!=1||h.headerBytes!=sizeof(Header)||
            h.keyA!=key.a||h.keyB!=key.b||h.rawBytes>limit||h.rawBytes%sizeof(float)||
            h.valueCount!=h.rawBytes/sizeof(float)||h.fileBytes!=size||h.blockSize!=blockBytes||
            h.blockCount!=(h.rawBytes+blockBytes-1)/blockBytes||
            h.headerHash!=hash(&h,offsetof(Header,headerHash)))return false;
        const uint64_t overhead=sizeof(Header)+uint64_t(h.blockCount)*sizeof(Block);
        return size>=overhead&&size<=overhead+h.rawBytes;
    }
    static Handle openRead(const std::wstring& location) {
        return Handle(CreateFileW(location.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    }
    bool ownedHeader(const std::wstring& filename,Header& h,uint64_t& size) const {
        SL_HistoryKey key;if(!keyFromName(filename.c_str(),key))return false;
        DWORD flags=GetFileAttributesW(path(filename).c_str());
        if(flags==INVALID_FILE_ATTRIBUTES||(flags&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return false;
        auto file=openRead(path(filename));
        return file.valid()&&fileSize(file.value,size)&&read(file.value,&h,sizeof(h))&&
            headerValid(h,key,size,maxValue_);
    }
    void cleanOldTemporaries() const {
        FILETIME now={};GetSystemTimeAsFileTime(&now);
        const uint64_t clock=(uint64_t(now.dwHighDateTime)<<32)|now.dwLowDateTime;
        const uint64_t day=24ULL*60*60*10000000;
        WIN32_FIND_DATAW found={};HANDLE search=FindFirstFileW(path(L"*.sldc.part-*").c_str(),&found);
        if(search==INVALID_HANDLE_VALUE)return;
        struct Close {HANDLE h;~Close(){FindClose(h);}} close{search};
        do {
            if(found.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))continue;
            uint64_t age=(uint64_t(found.ftLastWriteTime.dwHighDateTime)<<32)|found.ftLastWriteTime.dwLowDateTime;
            if(age>clock||clock-age<day)continue;
            std::wstring filename=found.cFileName;
            if(filename.size()<48||filename.size()>128||filename.substr(37,6)!=L".part-")continue;
            SL_HistoryKey key;if(!keyFromName(filename.substr(0,37).c_str(),key))continue;
            unsigned parts=1,digits=0;bool valid=true;
            for(size_t i=43;i<filename.size();i++) {
                if(filename[i]>=L'0'&&filename[i]<=L'9'){++digits;continue;}
                if(filename[i]!=L'-'||digits==0){valid=false;break;}
                ++parts;digits=0;
            }
            if(!valid||!digits||(parts!=3&&parts!=4))continue;
            // No sharing: an active writer or reader prevents removal, even if its
            // timestamp is stale. Delete by the checked handle, never by a new path lookup.
            Handle file(CreateFileW(path(filename).c_str(),GENERIC_READ|DELETE,0,nullptr,OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,nullptr));
            Header h={};uint64_t size=0;
            if(!file.valid()||!fileSize(file.value,size)||size<sizeof(h)||!read(file.value,&h,sizeof(h)))continue;
            bool complete=headerValid(h,key,size,maxValue_);
            bool initial=!memcmp(h.magic,"SLDCS001",8)&&h.version==1&&h.headerBytes==sizeof(h)&&
                h.keyA==key.a&&h.keyB==key.b&&h.rawBytes<=maxValue_&&h.rawBytes%4==0&&
                h.valueCount==h.rawBytes/4&&h.fileBytes==sizeof(h)&&h.payloadHash==initialHash&&
                h.blockSize==blockBytes&&h.blockCount==(h.rawBytes+blockBytes-1)/blockBytes&&
                h.headerHash==hash(&h,offsetof(Header,headerHash));
            if(complete||initial) {
                FILE_DISPOSITION_INFO disposition={TRUE};
                SetFileInformationByHandle(file.value,FileDispositionInfo,&disposition,sizeof(disposition));
            }
        } while(FindNextFileW(search,&found));
    }
    bool publish(const std::wstring& temporary,const std::wstring& filename,const Header& incoming) const {
        // Only exact cache filenames with valid ownership headers may be replaced or evicted.
        cleanOldTemporaries();
        std::vector<Entry> entries;
        DWORD targetFlags=GetFileAttributesW(path(filename).c_str());
        if(targetFlags!=INVALID_FILE_ATTRIBUTES) {
            Header old={};uint64_t oldSize=0;
            if(!ownedHeader(filename,old,oldSize))return false;
        }
        // Count even unverified candidate names. If this upper bound fits, the true
        // committed size fits too; no ownership checks or evictions are needed.
        const uint64_t remaining=budget_-incoming.fileBytes;
        uint64_t upperBound=0;
        WIN32_FIND_DATAW found={};
        HANDLE search=FindFirstFileW(path(L"*.sldc").c_str(),&found);
        if(search!=INVALID_HANDLE_VALUE) {
            struct Close {HANDLE h;~Close(){FindClose(h);}} close{search};
            do {
                if(found.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))continue;
                SL_HistoryKey candidate;
                if(filename==found.cFileName||!keyFromName(found.cFileName,candidate))continue;
                const uint64_t size=(uint64_t(found.nFileSizeHigh)<<32)|found.nFileSizeLow;
                if(size>(std::numeric_limits<uint64_t>::max)()-upperBound)return false;
                upperBound+=size;
                entries.push_back({found.cFileName,size,
                    (uint64_t(found.ftLastWriteTime.dwHighDateTime)<<32)|found.ftLastWriteTime.dwLowDateTime});
            } while(FindNextFileW(search,&found));
            if(GetLastError()!=ERROR_NO_MORE_FILES)return false;
        } else if(GetLastError()!=ERROR_FILE_NOT_FOUND)return false;
        if(upperBound>remaining) {
            // Under pressure only files with valid ownership headers count. Foreign
            // lookalikes may trigger validation but must never cause an eviction.
            uint64_t total=0;
            size_t owned=0;
            for(size_t i=0;i<entries.size();++i) {
                Header header={};uint64_t size=0;
                if(!ownedHeader(entries[i].name,header,size))continue;
                if(size>(std::numeric_limits<uint64_t>::max)()-total)return false;
                total+=size;
                entries[i].bytes=size;
                if(owned!=i)entries[owned]=std::move(entries[i]);
                ++owned;
            }
            entries.resize(owned);
            if(total>remaining) {
                std::sort(entries.begin(),entries.end(),[](const Entry& a,const Entry& b){return a.age<b.age;});
                for(const auto& item:entries) {
                    if(total<=remaining)break;
                    if(DeleteFileW(path(item.name).c_str()))total-=item.bytes;
                }
                if(total>remaining)return false;
            }
        }
        return MoveFileExW(temporary.c_str(),path(filename).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
    }
public:
    explicit SL_DiskCache(const std::wstring& root,uint64_t budgetBytes=2ULL*1024*1024*1024,
        uint64_t maxValueBytes=512ULL*1024*1024,bool compression=true)
        :budget_(budgetBytes),maxValue_((std::min)(maxValueBytes,uint64_t(2)*1024*1024*1024)),compress_(compression) {
        // Resolve only the supplied path; no environment lookup or filesystem mutation.
        if(root.empty())return;
        DWORD required=GetFullPathNameW(root.c_str(),0,nullptr,nullptr);if(!required)return;
        std::vector<wchar_t> absolute(size_t(required)+1);
        DWORD length=GetFullPathNameW(root.c_str(),DWORD(absolute.size()),absolute.data(),nullptr);
        if(!length||length>=absolute.size())return;
        root_.assign(absolute.data(),length);
        while(root_.size()>3&&(root_.back()==L'\\'||root_.back()==L'/'))root_.pop_back();
        std::wstring normalized=root_;
        for(auto& c:normalized){if(c==L'/')c=L'\\';if(c>=L'A'&&c<=L'Z')c+=L'a'-L'A';}
        uint64_t id=hash(normalized.data(),normalized.size()*sizeof(wchar_t));
        wchar_t mutex[64]={};swprintf_s(mutex,L"Local\\SanLightDiskCache-v1-%016llx",id);mutexName_=mutex;
    }
    uint64_t budgetBytes() const {return budget_;}
    const std::wstring& root() const {return root_;}
    static bool compressionAvailable() {return codec().available();}

    bool get(const SL_HistoryKey& key,std::vector<float>& value,Abort abort=nullptr,void* data=nullptr) const noexcept {
        try {
            if((abort&&abort(data))||!rootReady(false))return false;
            std::wstring location=path(name(key));
            DWORD flags=GetFileAttributesW(location.c_str());
            if(flags==INVALID_FILE_ATTRIBUTES||(flags&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return false;
            auto file=openRead(location);uint64_t size=0;Header h={};
            if(!file.valid()||!fileSize(file.value,size)||!read(file.value,&h,sizeof(h))||
                !headerValid(h,key,size,maxValue_)||h.rawBytes>size_t(-1))return false;
            std::vector<float> result(size_t(h.valueCount));
            std::vector<unsigned char> compressed,planes;
            Decompressor decoder(codec());
            uint64_t offset=0,position=sizeof(h),payloadHash=initialHash;
            for(uint32_t i=0;i<h.blockCount;i++) {
                if(abort&&abort(data))return false;
                Block block={};
                if(!read(file.value,&block,sizeof(block))||block.reserved||block.codec>2||
                    block.rawBytes!=(std::min)(uint64_t(blockBytes),h.rawBytes-offset)||
                    block.storedBytes==0||block.storedBytes>block.rawBytes||
                    (block.codec==0&&block.rawBytes!=block.storedBytes)||
                    position+sizeof(block)+block.storedBytes>h.fileBytes)return false;
                auto* destination=reinterpret_cast<unsigned char*>(result.data())+offset;
                if(block.codec==0) {
                    if(!read(file.value,destination,block.rawBytes))return false;
                } else {
                    if(compressed.empty())compressed.resize(size_t((std::min)(h.rawBytes,uint64_t(blockBytes))));
                    if(!read(file.value,compressed.data(),block.storedBytes))return false;
                    auto* decoded=destination;
                    if(block.codec==2) {
                        if(planes.size()<block.rawBytes)planes.resize(block.rawBytes);
                        decoded=planes.data();
                    }
                    if(!decoder.expand(compressed.data(),block.storedBytes,decoded,block.rawBytes))return false;
                    if(block.codec==2)fromBytePlanes(decoded,destination,block.rawBytes/sizeof(float));
                }
                if(hash(destination,block.rawBytes)!=block.hash)return false;
                payloadHash=hash(&block.hash,sizeof(block.hash),payloadHash);
                offset+=block.rawBytes;position+=sizeof(block)+block.storedBytes;
            }
            if(offset!=h.rawBytes||position!=h.fileBytes||payloadHash!=h.payloadHash||(abort&&abort(data)))return false;
            value.swap(result);return true;
        } catch(...) {return false;}
    }

    bool put(const SL_HistoryKey& key,const std::vector<float>& value,Abort abort=nullptr,void* data=nullptr) const noexcept {
        try {
            if(value.size()>maxValue_/sizeof(float)||(abort&&abort(data))||!rootReady(true)||budget_<sizeof(Header))return false;
            const uint64_t rawBytes=uint64_t(value.size())*sizeof(float);
            if(rawBytes>(std::numeric_limits<uint32_t>::max)()*uint64_t(blockBytes))return false;
            static std::atomic<uint64_t> counter{0};
            wchar_t suffix[96]={};swprintf_s(suffix,L".part-%lu-%lu-%llu-%llu",GetCurrentProcessId(),GetCurrentThreadId(),
                GetTickCount64(),static_cast<unsigned long long>(++counter));
            std::wstring filename=name(key),temporary=path(filename+suffix);
            Handle file(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
            if(!file.valid())return false;
            struct RemoveTemporary {
                std::wstring location;bool committed=false;
                ~RemoveTemporary(){if(!committed)DeleteFileW(location.c_str());}
            } cleanup{temporary};
            // Explicit close before cleanup on every exit (the temporary isn't shared).
            struct CloseBeforeCleanup {
                Handle& file;~CloseBeforeCleanup(){if(file.valid()){CloseHandle(file.value);file.value=INVALID_HANDLE_VALUE;}}
            } closeBeforeCleanup{file};
            Header h={};memcpy(h.magic,"SLDCS001",8);h.version=1;h.headerBytes=sizeof(h);
            h.keyA=key.a;h.keyB=key.b;h.valueCount=value.size();h.rawBytes=rawBytes;
            h.fileBytes=sizeof(h);h.payloadHash=initialHash;h.blockSize=blockBytes;
            h.blockCount=uint32_t((rawBytes+blockBytes-1)/blockBytes);
            h.headerHash=hash(&h,offsetof(Header,headerHash));
            if(!write(file.value,&h,sizeof(h)))return false;
            Compressor encoder(codec(),compress_);std::vector<unsigned char> buffer,planes;
            if(encoder.handle) {
                buffer.resize(size_t((std::min)(uint64_t(blockBytes),rawBytes)));
                if(rawBytes>=shuffleThreshold) {
                    try {planes.resize(blockBytes);}
                    catch(const std::bad_alloc&) {} // Ordinary XPRESS still works without the optional plane buffer.
                }
            }
            const auto* bytes=reinterpret_cast<const unsigned char*>(value.data());
            for(uint64_t offset=0;offset<rawBytes;) {
                if(abort&&abort(data))return false;
                Block block={};block.rawBytes=uint32_t((std::min)(uint64_t(blockBytes),rawBytes-offset));
                block.storedBytes=block.rawBytes;block.hash=hash(bytes+offset,block.rawBytes);
                const void* stored=bytes+offset;SIZE_T compressed=0;
                const unsigned char* compressionSource=bytes+offset;
                if(!planes.empty()&&block.rawBytes>=256) {
                    toBytePlanes(compressionSource,planes.data(),block.rawBytes/sizeof(float));
                    compressionSource=planes.data();
                }
                if(encoder.handle&&block.rawBytes>=256&&encoder.api.compress(encoder.handle,compressionSource,
                    block.rawBytes,buffer.data(),block.rawBytes,&compressed)&&compressed<block.rawBytes) {
                    block.codec=planes.empty()?1:2;block.storedBytes=uint32_t(compressed);stored=buffer.data();
                }
                if(h.fileBytes+sizeof(block)+block.storedBytes>budget_||
                    !write(file.value,&block,sizeof(block))||!write(file.value,stored,block.storedBytes))return false;
                h.fileBytes+=sizeof(block)+block.storedBytes;
                h.payloadHash=hash(&block.hash,sizeof(block.hash),h.payloadHash);offset+=block.rawBytes;
            }
            h.headerHash=hash(&h,offsetof(Header,headerHash));LARGE_INTEGER zero={};
            if(!SetFilePointerEx(file.value,zero,nullptr,FILE_BEGIN)||!write(file.value,&h,sizeof(h))||
                !FlushFileBuffers(file.value))return false;
            CloseHandle(file.value);file.value=INVALID_HANDLE_VALUE;
            MutexGuard mutex(mutexName_.c_str());if(!mutex.acquire(abort,data))return false;
            if(!rootReady(false)||!publish(temporary,filename,h))return false;
            cleanup.committed=true;return true;
        } catch(...) {return false;}
    }
};
