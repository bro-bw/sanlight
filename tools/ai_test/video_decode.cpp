#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
int wmain(int argc,wchar_t** argv) {
    if(argc!=3)return 1;
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(MFStartup(MF_VERSION)))return 2;
    ComPtr<IMFAttributes> attributes;MFCreateAttributes(&attributes,2);
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,TRUE);
    ComPtr<IMFSourceReader> reader;
    HRESULT hr=MFCreateSourceReaderFromURL(argv[1],attributes.Get(),&reader);
    if(FAILED(hr)){printf("Open failed %08lx\n",hr);return 3;}
    ComPtr<IMFMediaType> type;MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32);
    hr=reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,type.Get());
    if(FAILED(hr)){printf("RGB conversion failed %08lx\n",hr);return 4;}
    PROPVARIANT duration={};
    reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,MF_PD_DURATION,&duration);
    LONGLONG length=duration.uhVal.QuadPart;
    printf("Duration %.3f seconds\n",length/10000000.0);
    PropVariantClear(&duration);
    CreateDirectoryW(argv[2],nullptr);
    for(int i=0;i<12;i++) {
        PROPVARIANT position={};position.vt=VT_I8;position.hVal.QuadPart=length*i/12;
        hr=reader->SetCurrentPosition(GUID_NULL,position);if(FAILED(hr))return 5;
        ComPtr<IMFSample> sample;LONGLONG time=0;
        for(int attempts=0;attempts<1000;attempts++) {
            DWORD stream=0,flags=0;sample.Reset();
            hr=reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,&stream,&flags,&time,&sample);
            if(FAILED(hr)||(flags&MF_SOURCE_READERF_ENDOFSTREAM))break;
            if(sample&&time>=position.hVal.QuadPart)break;
        }
        if(!sample){printf("Frame unavailable %d\n",i);continue;}
        ComPtr<IMFMediaType> format;reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,&format);
        UINT32 w=0,h=0;MFGetAttributeSize(format.Get(),MF_MT_FRAME_SIZE,&w,&h);
        LONG stride=0;UINT32 storedStride=0;
        if(SUCCEEDED(format->GetUINT32(MF_MT_DEFAULT_STRIDE,&storedStride)))stride=(LONG)storedStride;
        else MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1,w,&stride);
        ComPtr<IMFMediaBuffer> buffer;sample->ConvertToContiguousBuffer(&buffer);
        BYTE* data=nullptr;DWORD size=0;buffer->Lock(&data,nullptr,&size);
        if(!data||size<size_t(abs(stride))*h){if(data)buffer->Unlock();return 6;}
        BITMAPFILEHEADER file={};BITMAPINFOHEADER info={};
        file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+w*h*4;
        info.biSize=sizeof(info);info.biWidth=w;info.biHeight=-(LONG)h;info.biPlanes=1;info.biBitCount=32;
        std::wstring path=std::wstring(argv[2])+L"\\frame-"+std::to_wstring(i)+L".bmp";
        FILE* output=nullptr;_wfopen_s(&output,path.c_str(),L"wb");if(!output)return 7;
        fwrite(&file,sizeof(file),1,output);fwrite(&info,sizeof(info),1,output);
        const BYTE* top=data+(stride<0?(h-1)*size_t(-stride):0);
        for(UINT32 y=0;y<h;y++)fwrite(top+ptrdiff_t(y)*stride,4,w,output);
        fclose(output);buffer->Unlock();
        printf("Frame %d: %.3fs, %ux%u\n",i,time/10000000.0,w,h);
    }
    reader.Reset();MFShutdown();CoUninitialize();return 0;
}
