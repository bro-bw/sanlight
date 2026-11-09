#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <map>
#include <mutex>
#include <memory>
#include <utility>

// Content-addressed prefix checkpoints: render order never selects the history.
struct SL_HistoryKey {
    uint64_t a=1469598103934665603ULL,b=0x9e3779b97f4a7c15ULL;
    bool operator<(const SL_HistoryKey& other) const {
        return a!=other.a ? a<other.a : b<other.b;
    }
    void append(const void* data,size_t length) {
        const auto* p=static_cast<const unsigned char*>(data);
        for(size_t i=0;i<length;i++) {
            a=(a^p[i])*1099511628211ULL;
            b^=p[i]+0x9e3779b97f4a7c15ULL+(b<<6)+(b>>2);
        }
    }
    template<class T> void value(const T& v) {append(&v,sizeof(v));}
};

using SL_SurfaceRef = std::shared_ptr<const std::vector<float>>;

// A render can finish with its own buffer if optional shared ownership cannot
// be allocated. After publication only immutable references leave the render.
struct SL_RenderSurface {
    std::vector<float> local;
    SL_SurfaceRef shared;

    const std::vector<float>& pixels() const { return shared ? *shared : local; }
    const SL_SurfaceRef& freeze() {
        if (!shared && !local.empty()) {
            try { shared = std::make_shared<const std::vector<float>>(std::move(local)); }
            catch (const std::bad_alloc&) {} // allocation occurs before the noexcept vector move
        }
        return shared;
    }
};

class SL_HistoryCache {
    struct Entry {SL_SurfaceRef depth;uint64_t used;};
    std::map<SL_HistoryKey,Entry> entries;
    std::mutex mutex;
    size_t bytes=0,limit;
    uint64_t clock=0;
public:
    explicit SL_HistoryCache(size_t memoryLimit=128*1024*1024):limit(memoryLimit) {}
    SL_SurfaceRef get(const SL_HistoryKey& key) {
        std::lock_guard<std::mutex> guard(mutex);
        auto it=entries.find(key);if(it==entries.end())return {};
        it->second.used=++clock;return it->second.depth;
    }
    SL_SurfaceRef put(const SL_HistoryKey& key,const SL_SurfaceRef& depth) {
        if (!depth) return {};
        const size_t size=depth->size()*sizeof(float);if(size>limit)return depth;
        std::lock_guard<std::mutex> guard(mutex);
        const auto existing=entries.find(key);
        if(existing!=entries.end())return existing->second.depth;
        try { entries.emplace(key,Entry{depth,++clock}); }
        catch(const std::bad_alloc&) { return depth; }
        bytes+=size;
        while(bytes>limit || entries.size()>128) {
            auto oldest=entries.begin();
            for(auto it=entries.begin();it!=entries.end();++it)
                if(it->second.used<oldest->second.used)oldest=it;
            bytes-=oldest->second.depth->size()*sizeof(float);entries.erase(oldest);
        }
        return depth;
    }
    void clear() {std::lock_guard<std::mutex> guard(mutex);entries.clear();bytes=0;}
};
