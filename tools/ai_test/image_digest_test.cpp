#include "../../src/SL_ImageDigest.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <cstdint>
#include <climits>
void need(bool yes,const char* text){if(!yes)throw std::runtime_error(text);}
uint64_t legacy(const unsigned char* p,size_t n){uint64_t h=1469598103934665603ULL;for(size_t i=0;i<n;i++)h=(h^p[i])*1099511628211ULL;return h;}
int main(){try{
 auto digest=SL_ImageDigest::compute("abc",3);
#ifdef SL_TEST_NO_IMAGE_DIGEST
 need(!digest.valid&&!(digest==digest),"failed digest produced a cache hit");puts("PASS unavailable digest forces cache miss");return 0;
#endif
 unsigned char abc[]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
 need(digest.valid&&!memcmp(digest.bytes.data(),abc,32),"known SHA256 vector");
 need(SL_ImageDigest::compute(nullptr,0).valid,"empty digest");
 need(!SL_ImageDigest::compute(nullptr,8).valid,"null nonempty input");
 need(!SL_ImageDigest::compute("x",size_t(ULONG_MAX)+1).valid,"oversize length");
 std::vector<unsigned char> pixels(size_t(1713)*2447*16);uint32_t state=821;
 for(auto& p:pixels){state=1664525u*state+1013904223u;p=state>>24;}
 auto original=SL_ImageDigest::compute(pixels.data(),pixels.size());need(original.valid,"large source");
 for(size_t at:{size_t(0),size_t(3),pixels.size()/2,pixels.size()-1}){
  pixels[at]^=1;auto changed=SL_ImageDigest::compute(pixels.data(),pixels.size());need(changed.valid&&!(changed==original),"pixel edit not detected");pixels[at]^=1;
 }
 need(original==SL_ImageDigest::compute(pixels.data(),pixels.size()),"identical bytes missed");
 double oldMs=0,newMs=0;volatile uint64_t sink=0;
 for(int round=0;round<8;round++){
  auto t=std::chrono::steady_clock::now();sink^=legacy(pixels.data(),pixels.size());auto t2=std::chrono::steady_clock::now();
  auto hash=SL_ImageDigest::compute(pixels.data(),pixels.size());need(hash==original,"repeated result");auto t3=std::chrono::steady_clock::now();
  oldMs+=std::chrono::duration<double,std::milli>(t2-t).count();newMs+=std::chrono::duration<double,std::milli>(t3-t2).count();
 }
 printf("PASS SHA256 vectors, full-byte edits, identical buffers and invalid arguments; 1713x2447 RGBA float legacy %.3f ms -> native digest %.3f ms (%llu)\n",oldMs/8,newMs/8,(unsigned long long)sink);return 0;
}catch(const std::exception&e){printf("FAIL %s\n",e.what());return 1;}}
