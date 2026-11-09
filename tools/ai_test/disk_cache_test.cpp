#include "../../src/SL_History.h"
#include "../../src/SL_DiskCache.h"
#include <cstdio>
#include <thread>

static SL_HistoryKey key(uint64_t index){SL_HistoryKey result;result.a=index;result.b=index*17+1;return result;}
static std::wstring location(const std::wstring& root,SL_HistoryKey k) {
    wchar_t filename[40];swprintf_s(filename,L"%016llx%016llx.sldc",k.a,k.b);return root+L"\\"+filename;
}
static bool equal(const std::vector<float>& a,const std::vector<float>& b) {
    return a.size()==b.size()&&(a.empty()||!memcmp(a.data(),b.data(),a.size()*sizeof(float)));
}
static std::vector<unsigned char> load(const std::wstring& path) {
    HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(h==INVALID_HANDLE_VALUE)return {};
    DWORD n=GetFileSize(h,nullptr);std::vector<unsigned char> bytes(n);DWORD got=0;
    bool okay=ReadFile(h,bytes.data(),n,&got,nullptr)&&got==n;CloseHandle(h);
    return okay?bytes:std::vector<unsigned char>{};
}
static bool save(const std::wstring& path,const std::vector<unsigned char>& bytes) {
    HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);
    if(h==INVALID_HANDLE_VALUE)return false;DWORD sent=0;
    bool okay=WriteFile(h,bytes.data(),DWORD(bytes.size()),&sent,nullptr)&&sent==bytes.size();CloseHandle(h);return okay;
}
static bool makeOld(const std::wstring& path) {
    HANDLE file=CreateFileW(path.c_str(),FILE_WRITE_ATTRIBUTES,0,nullptr,OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    FILETIME now={};GetSystemTimeAsFileTime(&now);
    uint64_t age=((uint64_t(now.dwHighDateTime)<<32)|now.dwLowDateTime)-25ULL*60*60*10000000;
    FILETIME stamp={DWORD(age),DWORD(age>>32)};
    bool okay=SetFileTime(file,nullptr,nullptr,&stamp)!=FALSE;CloseHandle(file);return okay;
}
static uint64_t fixtureHash(const void* data,size_t bytes,uint64_t hash=1469598103934665603ULL) {
    const auto* p=static_cast<const unsigned char*>(data);
    while(bytes>=8){uint64_t word;memcpy(&word,p,8);hash=(hash^word)*1099511628211ULL;hash^=hash>>32;p+=8;bytes-=8;}
    while(bytes--){hash=(hash^*p++)*1099511628211ULL;}return hash;
}
static uint64_t fileBytes(const std::wstring& root,const wchar_t* wildcard=L"*.sldc") {
    WIN32_FIND_DATAW found={};HANDLE h=FindFirstFileW((root+L"\\"+wildcard).c_str(),&found);if(h==INVALID_HANDLE_VALUE)return 0;
    uint64_t total=0;do {total+=(uint64_t(found.nFileSizeHigh)<<32)|found.nFileSizeLow;}while(FindNextFileW(h,&found));
    FindClose(h);return total;
}
struct Cancel {int calls=0,at=0;};
static bool cancel(void* data){auto& c=*static_cast<Cancel*>(data);return ++c.calls>=c.at;}
static std::vector<float> samples(size_t count) {
    std::vector<float> value(count);
    for(size_t i=0;i<count;i++) {
        const uint32_t bits[]={0,0x80000000,0x7fc01234,0xff800000,0x3f800000,0x3eaaaaab,0x00000001};
        uint32_t raw=bits[(i/13)%7];memcpy(value.data()+i,&raw,4);
    }
    return value;
}
static int child(const std::wstring& root,int start) {
    SL_DiskCache cache(root,80000,1024*1024,false);auto value=samples(4096);
    for(int i=0;i<10;i++)if(!cache.put(key(start+i),value))return 91;
    return 0;
}
int wmain(int argc,wchar_t** argv) {
    if(argc==4&&!wcscmp(argv[1],L"--child"))return child(argv[2],_wtoi(argv[3]));
    wchar_t cwd[32768]={};if(!GetCurrentDirectoryW(32768,cwd))return 1;
    wchar_t suffix[80]={};swprintf_s(suffix,L"\\disk-cache-test-%lu-%llu",GetCurrentProcessId(),GetTickCount64());
    std::wstring base=std::wstring(cwd)+suffix;
    if(!CreateDirectoryW(base.c_str(),nullptr))return 2;
    auto data=samples(700001);std::vector<float> got;
    const std::wstring roundRoot=base+L"\\roundtrip";
    SL_DiskCache cache(roundRoot,32*1024*1024,16*1024*1024);
    if(GetFileAttributesW(roundRoot.c_str())!=INVALID_FILE_ATTRIBUTES)return 3;
    if(!cache.put(key(1),data)||!cache.get(key(1),got)||!equal(data,got))return 4;
    SL_DiskCache reopened(roundRoot,32*1024*1024,16*1024*1024);
    if(!reopened.get(key(1),got)||!equal(data,got))return 5;
    printf("roundtrip raw=%zu stored=%llu compression_available=%d\n",data.size()*4,
        static_cast<unsigned long long>(fileBytes(roundRoot)),SL_DiskCache::compressionAvailable()?1:0);
    if(SL_DiskCache::compressionAvailable()&&fileBytes(roundRoot)>=data.size()*4)return 6;
    auto original=load(location(roundRoot,key(1)));if(original.size()<104)return 7;
    std::vector<float> sentinel(3,8.f);got=sentinel;
    auto damaged=original;damaged[96]^=0x55; // Corrupt the first block's checksum, not codec padding.
    if(!save(location(roundRoot,key(1)),damaged)||cache.get(key(1),got)||!equal(got,sentinel))return 8;
    if(!cache.put(key(1),data)||!cache.get(key(1),got)||!equal(data,got))return 9; // Repair an owned payload.
    damaged=original;damaged.resize(damaged.size()/2);
    got=sentinel;if(!save(location(roundRoot,key(1)),damaged)||cache.get(key(1),got)||!equal(got,sentinel))return 10;
    if(cache.put(key(1),data))return 11; // Invalid ownership header/file length is preserved.
    damaged=original;damaged[8]=2;
    if(!save(location(roundRoot,key(1)),damaged)||cache.get(key(1),got)||cache.put(key(1),data))return 12;
    if(!save(location(roundRoot,key(1)),original))return 13;
    if(!save(location(roundRoot,key(2)),original)||cache.get(key(2),got)||cache.put(key(2),data))return 14;
    SL_DiskCache limited(roundRoot,32*1024*1024,1024);
    if(limited.get(key(1),got)||limited.put(key(3),data))return 15;
    damaged=original;damaged.push_back(0);
    if(!save(location(roundRoot,key(1)),damaged)||cache.get(key(1),got))return 16;
    if(!save(location(roundRoot,key(1)),original))return 17;
    puts("PASS bit-exact compression/reopen, checksum, truncation, schema, wrong key and allocation limits");

    // Large packed float arrays can use byte planes; the final short block
    // also exercises non-multiple-of-four word counts and bit-pattern recovery.
    auto large=samples(5*1024*1024/4+71);
    if(!cache.put(key(50),large)||!cache.get(key(50),got)||!equal(got,large))return 50;
    auto planeRecord=load(location(roundRoot,key(50)));
    if(planeRecord.size()<104||(SL_DiskCache::compressionAvailable()&&planeRecord[88]!=2))return 51;
    planeRecord[96]^=0x55;
    got=sentinel;
    if(!save(location(roundRoot,key(50)),planeRecord)||cache.get(key(50),got)||!equal(got,sentinel))return 52;
    if(!cache.put(key(50),large)||!cache.get(key(50),got)||!equal(got,large))return 53;
    // Incompressible float bits must be written in original order on raw fallback.
    uint32_t random=0x9e3779b9u;
    for(float& sample:large) {
        random^=random<<13;random^=random>>17;random^=random<<5;memcpy(&sample,&random,4);
    }
    if(!cache.put(key(51),large)||!cache.get(key(51),got)||!equal(got,large))return 54;
    auto rawRecord=load(location(roundRoot,key(51)));
    if(rawRecord.size()<104||rawRecord[88]!=0)return 55;
    puts("PASS optional byte-plane encoding, checksum repair and incompressible raw fallback");

    const std::wstring rawRoot=base+L"\\raw";
    SL_DiskCache raw(rawRoot,16*1024*1024,16*1024*1024,false);
    if(!raw.put(key(4),data)||!raw.get(key(4),got)||!equal(data,got)||fileBytes(rawRoot)<=data.size()*4)return 18;
    std::vector<float> empty;if(!raw.put(key(5),empty)||!raw.get(key(5),got)||!got.empty())return 19;
    Cancel stop={0,3};if(raw.put(key(6),data,cancel,&stop))return 20;
    if(GetFileAttributesW(location(rawRoot,key(6)).c_str())!=INVALID_FILE_ATTRIBUTES||fileBytes(rawRoot,L"*.part-*"))return 21;
    stop={0,3};got=sentinel;if(raw.get(key(4),got,cancel,&stop)||!equal(got,sentinel))return 22;
    stop={0,2};if(raw.put(key(4),data,cancel,&stop)||!raw.get(key(4),got)||!equal(data,got))return 23;
    puts("PASS raw fallback, empty arrays, cancellable reads/writes and temporary cleanup");

    const std::wstring budgetRoot=base+L"\\budget";
    SL_DiskCache budget(budgetRoot,40000,16*1024*1024,false);auto compactValue=samples(4096);
    if(!budget.put(key(1),compactValue))return 24;Sleep(3);
    if(!budget.put(key(2),compactValue))return 25;Sleep(3);
    const std::wstring foreign=budgetRoot+L"\\keep.txt";std::vector<unsigned char> note={'u','s','e','r'};
    if(!save(foreign,note)||!budget.put(key(3),compactValue)||fileBytes(budgetRoot)>40000||
        budget.get(key(1),got)||!budget.get(key(3),got)||load(foreign)!=note)return 26;
    if(budget.put(key(7),data)||fileBytes(budgetRoot,L"*.part-*"))return 27;
    const auto collision=location(budgetRoot,key(9));
    if(!save(collision,note)||budget.put(key(9),compactValue)||load(collision)!=note)return 28;
    const auto directory=location(budgetRoot,key(10));
    if(!CreateDirectoryW(directory.c_str(),nullptr)||budget.put(key(10),compactValue)||
        !(GetFileAttributesW(directory.c_str())&FILE_ATTRIBUTE_DIRECTORY))return 29;
    puts("PASS byte budget, oversized refusal, FIFO eviction and foreign file/directory preservation");

    // A large foreign file with a valid-looking name can exceed the scan's
    // conservative estimate. It must neither count towards the budget nor evict a real entry.
    const auto mixed=base+L"\\mixed";SL_DiskCache mixedCache(mixed,40000,16*1024*1024,false);
    if(!mixedCache.put(key(1),compactValue))return 56;
    std::vector<unsigned char> foreignPayload(60000,0x61);
    const auto impostor=location(mixed,key(99));
    if(!save(impostor,foreignPayload)||!mixedCache.put(key(2),compactValue)||
        !mixedCache.get(key(1),got)||!equal(got,compactValue)||load(impostor)!=foreignPayload)return 57;
    puts("PASS foreign lookalikes do not evict owned entries under apparent pressure");

    const std::wstring threadRoot=base+L"\\threads";
    SL_DiskCache threads(threadRoot,4*1024*1024,16*1024*1024,false);
    auto a=samples(40000),b=a;b[0]=.25f;
    if(!threads.put(key(1),a))return 30;
    std::atomic<int> errors{0},hits{0};int busyWrites=0;
    auto reader=[&] {
        for(int i=0;i<80;i++) {
            std::vector<float> result;
            if(threads.get(key(1),result)) {
                ++hits;if(!equal(result,a)&&!equal(result,b))++errors;
            }
        }
    };
    std::thread r1(reader),r2(reader),r3(reader);
    for(int i=0;i<20;i++)if(!threads.put(key(1),i%2?a:b))++busyWrites;
    r1.join();r2.join();r3.join();
    printf("concurrent errors=%d hits=%d optional_busy_writes=%d temporary_bytes=%llu\n",int(errors),int(hits),busyWrites,
        static_cast<unsigned long long>(fileBytes(threadRoot,L"*.part-*")));
    if(errors||!hits||fileBytes(threadRoot,L"*.part-*"))return 31;
    if(!threads.put(key(1),b)||!threads.get(key(1),got)||!equal(got,b))return 37;
    puts("PASS concurrent readers and atomic replacement");

    const std::wstring orphan=location(roundRoot,key(1))+L".part-99999-1-1-1";
    const std::wstring active=location(roundRoot,key(1))+L".part-99999-1-1-2";
    const std::wstring fresh=location(roundRoot,key(1))+L".part-99999-1-1-3";
    const std::wstring foreignPart=location(roundRoot,key(1))+L".part-99999-1-1-4";
    const std::wstring incomplete=location(roundRoot,key(1))+L".part-99999-1-1-5";
    auto initial=original;initial.resize(90);
    const uint64_t headerBytes=80,initialHash=1469598103934665603ULL;
    memcpy(initial.data()+48,&headerBytes,8);memcpy(initial.data()+56,&initialHash,8);
    uint64_t checksum=fixtureHash(initial.data(),72);memcpy(initial.data()+72,&checksum,8);
    if(!save(orphan,original)||!makeOld(orphan)||!save(active,original)||!makeOld(active)||
        !save(fresh,original)||!save(foreignPart,note)||!makeOld(foreignPart)||
        !save(incomplete,initial)||!makeOld(incomplete))return 38;
    HANDLE activeFile=CreateFileW(active.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
    if(activeFile==INVALID_HANDLE_VALUE)return 39;
    bool cleanupOkay=cache.put(key(77),compactValue)&&GetFileAttributesW(orphan.c_str())==INVALID_FILE_ATTRIBUTES&&
        GetFileAttributesW(incomplete.c_str())==INVALID_FILE_ATTRIBUTES&&
        GetFileAttributesW(active.c_str())!=INVALID_FILE_ATTRIBUTES&&
        load(fresh)==original&&load(foreignPart)==note;
    CloseHandle(activeFile);if(!cleanupOkay)return 40;
    if(!cache.put(key(78),compactValue)||GetFileAttributesW(active.c_str())!=INVALID_FILE_ATTRIBUTES)return 41;
    puts("PASS orphan cleanup: only owned, 24-hour-old, unlocked temporary files removed");

    const std::wstring processRoot=base+L"\\processes";
    if(!CreateDirectoryW(processRoot.c_str(),nullptr))return 32;
    wchar_t exe[32768]={};if(!GetModuleFileNameW(nullptr,exe,32768))return 33;
    PROCESS_INFORMATION processes[2]={};
    for(int i=0;i<2;i++) {
        std::wstring command=L"\""+std::wstring(exe)+L"\" --child \""+processRoot+L"\" "+std::to_wstring(100*i);
        std::vector<wchar_t> mutableCommand(command.begin(),command.end());mutableCommand.push_back(0);
        STARTUPINFOW startup={};startup.cb=sizeof(startup);
        if(!CreateProcessW(exe,mutableCommand.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&processes[i]))return 34;
    }
    for(auto& process:processes) {
        WaitForSingleObject(process.hProcess,30000);DWORD status=1;GetExitCodeProcess(process.hProcess,&status);
        CloseHandle(process.hProcess);CloseHandle(process.hThread);if(status)return 35;
    }
    if(fileBytes(processRoot)>80000||!fileBytes(processRoot)||fileBytes(processRoot,L"*.part-*"))return 36;
    puts("PASS cross-process publication and shared budget");
    wprintf(L"Test fixtures retained under %s\n",base.c_str());
    return 0;
}
