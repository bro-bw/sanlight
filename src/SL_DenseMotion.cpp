#include "SL_DenseMotion.h"
#include "SL_Parallel.h"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace {
float bound(float x,float low,float high){return std::max(low,std::min(high,x));}
bool stopped(SL_TrackAbort abort,void* data){return abort&&abort(data);}

template<int C> cv::Vec<float,C> at(const cv::Mat& image,float x,float y) {
    x=bound(x,0.f,float(image.cols-1));y=bound(y,0.f,float(image.rows-1));
    int ix=int(x),iy=int(y),jx=std::min(ix+1,image.cols-1),jy=std::min(iy+1,image.rows-1);
    float tx=x-ix,ty=y-iy;
    const auto* a=image.ptr<cv::Vec<float,C>>(iy);
    const auto* b=image.ptr<cv::Vec<float,C>>(jy);
    cv::Vec<float,C> result;
    for(int c=0;c<C;c++)result[c]=(a[ix][c]+(a[jx][c]-a[ix][c])*tx)*(1-ty)+(b[ix][c]+(b[jx][c]-b[ix][c])*tx)*ty;
    return result;
}
float difference(const cv::Vec4f& a,const cv::Vec4f& b) {
    const auto d=a-b;return (d[0]*d[0]+d[1]*d[1]+d[2]*d[2])/3.f+d[3]*d[3];
}
bool prepare(const float* input,int w,int h,int cols,int rows,cv::Mat& color,cv::Mat& gray,SL_TrackAbort abort,void* data) {
    cv::Mat source(h,w,CV_32FC4,const_cast<float*>(input)),rgb(rows,cols,CV_8UC3);
    color.create(rows,cols,CV_32FC4);
    if(!SL_ParallelRows(rows,[&](int y){
        auto* values=color.ptr<cv::Vec4f>(y);auto* bytes=rgb.ptr<cv::Vec3b>(y);
        const float sy=(y+.5f)*h/rows-.5f;
        for(int x=0;x<cols;x++) {
            auto v=at<4>(source,(x+.5f)*w/cols-.5f,sy);
            for(int c=0;c<4;c++)if(!std::isfinite(v[c]))v[c]=0;
            values[x]=v;
            for(int c=0;c<3;c++)bytes[x][c]=cv::saturate_cast<unsigned char>(v[c]*255.f);
        }
    },abort,data))return false;
    cv::cvtColor(rgb,gray,cv::COLOR_RGB2GRAY);return !stopped(abort,data);
}
}

bool SL_DenseMotion(const float* previous,const float* current,int w,int h,
    SL_TrackResult& result,SL_TrackAbort abort,void* data) {
    if(!previous||!current||w<1||h<1||stopped(abort,data))return false;
    try {
        // Publish a fallback result only after successful completion; stale masks cannot survive.
        auto legacy=[&]() {
            SL_TrackResult temp;
            if(!SL_TrackDepth(previous,current,nullptr,w,h,768,.35f,temp,abort,data,true,true,true,false,.05f))return false;
            result=std::move(temp);return true;
        };
        // Very narrow layers cannot provide DIS's minimum patch pyramid.
        if(std::min(w,h)<32)return legacy();
        const size_t n=size_t(w)*h;
        if(n>SIZE_MAX/(4*sizeof(float)))return false;
        if(!std::memcmp(previous,current,n*4*sizeof(float))) {
            SL_TrackResult still;
            still.flowX.assign(n,0.f);still.flowY.assign(n,0.f);still.confidence.resize(n);
            for(size_t i=0;i<n;i++)still.confidence[i]=current[4*i+3]<.05f?0.f:1.f;
            result=std::move(still);return true;
        }
        const float scale=std::min(1.f,768.f/std::max(w,h));
        const int cols=std::max(1,int(w*scale)),rows=std::max(1,int(h*scale));
        if(std::min(cols,rows)<32)return legacy();
        // This statically linked OpenCV runtime belongs only to this plugin module.
        // Bound its worker count; do not inherit a machine-sized OpenCV thread pool.
        static std::once_flag threads;
        std::call_once(threads,[]{cv::setNumThreads(4);});
        cv::Mat oldColor,color,oldGray,gray,backward,forward;
        if(!prepare(previous,w,h,cols,rows,oldColor,oldGray,abort,data)||
           !prepare(current,w,h,cols,rows,color,gray,abort,data))return false;
        auto estimator=cv::DISOpticalFlow::create(cv::DISOpticalFlow::PRESET_MEDIUM);
        // Preserve small puppet contours at the finest bounded motion scale.
        estimator->setFinestScale(0);
        // No initial flow is supplied: out-of-order AE requests cannot seed different motion.
        estimator->calc(gray,oldGray,backward);
        if(stopped(abort,data))return false;
        estimator->calc(oldGray,gray,forward);
        if(stopped(abort,data))return false;
        SL_TrackResult next;
        cv::Mat coverage;
        {
            coverage=cv::Mat(rows,cols,CV_32FC1,cv::Scalar(0));
            for(int y=0;y<rows;y++) {
                if(stopped(abort,data))return false;
                for(int x=0;x<cols;x++) {
                    const float alpha=bound(oldColor.at<cv::Vec4f>(y,x)[3],0.f,1.f);
                    if(alpha<.05f)continue;
                    const auto v=forward.at<cv::Vec2f>(y,x);
                    const float px=x+v[0],py=y+v[1];
                    if(!std::isfinite(px)||!std::isfinite(py)||px<-1||py<-1||px>=cols||py>=rows)continue;
                    const int ix=int(std::floor(px)),iy=int(std::floor(py));
                    const float tx=px-ix,ty=py-iy;
                    for(int dy=0;dy<2;dy++)for(int dx=0;dx<2;dx++) {
                        const int xx=ix+dx,yy=iy+dy;
                        if(xx<0||yy<0||xx>=cols||yy>=rows)continue;
                        coverage.at<float>(yy,xx)+=alpha*(dx?tx:1-tx)*(dy?ty:1-ty);
                    }
                }
            }
            next.historyReject.assign(n,0.f);
        }
        cv::Mat oldInterior,interior;
        {
            cv::extractChannel(oldColor,oldInterior,3);cv::extractChannel(color,interior,3);
            cv::threshold(oldInterior,oldInterior,.98,1.,cv::THRESH_BINARY);
            cv::threshold(interior,interior,.98,1.,cv::THRESH_BINARY);
            auto kernel=cv::getStructuringElement(cv::MORPH_RECT,cv::Size(5,5));
            cv::erode(oldInterior,oldInterior,kernel,cv::Point(-1,-1),1,cv::BORDER_CONSTANT,cv::Scalar(0));
            cv::erode(interior,interior,kernel,cv::Point(-1,-1),1,cv::BORDER_CONSTANT,cv::Scalar(0));
        }
        next.flowX.resize(n);next.flowY.resize(n);next.confidence.resize(n);
        const float sx=float(cols)/w,sy=float(rows)/h;
        if(!SL_ParallelRows(h,[&](int y){for(int x=0;x<w;x++) {
            const size_t i=size_t(y)*w+x;
            if(current[4*i+3]<.05f){next.flowX[i]=next.flowY[i]=next.confidence[i]=0;continue;}
            float qx=(x+.5f)*sx-.5f,qy=(y+.5f)*sy-.5f;

            auto v=at<2>(backward,qx,qy),reverse=at<2>(forward,qx+v[0],qy+v[1]);
            float cycle=std::hypot(v[0]+reverse[0],v[1]+reverse[1]);
            const auto a=at<4>(color,qx,qy),b=at<4>(oldColor,qx+v[0],qy+v[1]);
            float residual=difference(a,b);
            if(cycle<2.f&&a[3]>.99f&&b[3]>.99f) {
                auto candidate=v-(v+reverse)*.5f;
                const auto corrected=at<4>(oldColor,qx+candidate[0],qy+candidate[1]);
                const float error=difference(a,corrected);
                if(corrected[3]>.99f&&error<=residual*1.1f+.0001f) {
                    v=candidate;residual=error;reverse=at<2>(forward,qx+v[0],qy+v[1]);
                    cycle=std::hypot(v[0]+reverse[0],v[1]+reverse[1]);
                }
            }
            const float rx=x+v[0]/sx,ry=y+v[1]/sy;
            next.confidence[i]=(rx<0||ry<0||rx>w-1||ry>h-1)?0.f:std::exp(-cycle*.35f-residual*12.f);
            next.flowX[i]=v[0]/sx;next.flowY[i]=v[1]/sy;
            {
                float missing=1.f-bound(at<1>(coverage,qx,qy)[0]/std::max(.1f,current[4*i+3]),0.f,1.f);
                float guard=std::min(at<1>(interior,qx,qy)[0],at<1>(oldInterior,qx+v[0],qy+v[1])[0]);
                next.historyReject[i]=missing*guard;
            }
        }},abort,data))return false;
        result=std::move(next);return true;
    } catch(const cv::Exception&) {return false;}
      catch(const std::bad_alloc&) {return false;}
}
