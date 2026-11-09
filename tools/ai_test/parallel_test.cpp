#include "../../src/SL_Parallel.h"
#include <cstdio>
#include <stdexcept>
#include <chrono>

struct AbortData {std::thread::id owner;int calls=0;bool wrongThread=false;};
bool cancel(void* opaque) {
    auto& data=*static_cast<AbortData*>(opaque);
    if(std::this_thread::get_id()!=data.owner)data.wrongThread=true;
    return ++data.calls>=4;
}
int main() {
    std::vector<int> result(256,0);
    if(!SL_ParallelRows(256,[&](int row){result[row]=row*row;}))return 1;
    for(int i=0;i<256;i++)if(result[i]!=i*i)return 2;
    AbortData data{std::this_thread::get_id()};std::atomic<int> active{0};
    if(SL_ParallelRows(256,[&](int){++active;std::this_thread::sleep_for(std::chrono::milliseconds(1));--active;},cancel,&data))return 3;
    if(data.wrongThread||active.load()!=0)return 4;
    bool caught=false;
    try {SL_ParallelRows(256,[&](int row){if(row==8)throw std::runtime_error("test");});}
    catch(const std::runtime_error&){caught=true;}
    if(!caught)return 5;
    if(!SL_ParallelRows(256,[&](int row){result[row]=row;}))return 6;
    for(int i=0;i<256;i++)if(result[i]!=i)return 7;
    puts("PASS independent rows, caller-only abort callbacks, cancellation join, exception propagation and reuse");
}
