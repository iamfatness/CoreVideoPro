// Explicit, isolated fault experiment; excluded from normal build/package.
// Produces real NV12 identities at rational 60Hz, recording actual publication
// times and injected odd-seqlock windows. Never opens the production mapping.
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "modules/VirtualCameraShm.h"
#include "modules/VirtualCameraCorrelationMapping.h"
#include "modules/DeliveryCounterPattern.h"
using namespace corevideo::modules;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
static void check(HRESULT hr, const char* stage) {
  if (FAILED(hr)) { char b[128]; std::snprintf(b, sizeof(b), "%s: 0x%08lx", stage, static_cast<unsigned long>(hr)); throw std::runtime_error(b); }
}
static int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}
struct Handle { HANDLE h=nullptr; ~Handle() { if(h && h!=INVALID_HANDLE_VALUE) CloseHandle(h); } };
static void refuseLiveCore() {
  Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
  if(snapshot.h==INVALID_HANDLE_VALUE) throw std::runtime_error("Process inspection failed");
  PROCESSENTRY32W process{}; process.dwSize=sizeof(process);
  if(!Process32FirstW(snapshot.h,&process)) throw std::runtime_error("Process inspection failed");
  do {
    if (_wcsicmp(process.szExeFile,L"corevideo-native.exe")==0 ||
        _wcsicmp(process.szExeFile,L"CoreVideoPro.WinUI.exe")==0)
      throw std::runtime_error("Close CoreVideo before this isolated camera experiment");
  } while(Process32NextW(snapshot.h,&process));
}
struct Row { uint32_t identity; int64_t dueUs, beginUs, endUs; };
int main(int argc,char**argv) {
  if(argc!=4 || std::string(argv[3])!="--isolated-test") {
    std::fprintf(stderr,"Usage: publication-qa SECONDS(1..180) ODD_US(0..25000) --isolated-test\n"); return 2;
  }
  char* end=nullptr; const long seconds=std::strtol(argv[1],&end,10);
  if(!end || *end || seconds<1 || seconds>180) return 2;
  const long oddUs=std::strtol(argv[2],&end,10);
  if(!end || *end || oddUs<0 || oddUs>25000) return 2;
  try {
    refuseLiveCore();
    // A unique publisher lock prevents two diagnostic writers to the same slot.
    Handle exclusive{CreateMutexW(nullptr,TRUE,L"Local\\CoreVideoPublicationQaWriter")};
    if(!exclusive.h || GetLastError()==ERROR_ALREADY_EXISTS) throw std::runtime_error("QA writer already active");
    check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");
    struct ComStop { ~ComStop(){CoUninitialize();} } comStop;
    check(MFStartup(MF_VERSION),"MF startup");
    struct MfStop { ~MfStop(){MFShutdown();} } mfStop;
    _putenv_s("COREVIDEO_VCAM_SHM_DIR","");
    const auto dir=virtualCameraShmDir();
    const auto parent=dir.substr(0,dir.find_last_of('\\'));
    CreateDirectoryA(parent.c_str(),nullptr); CreateDirectoryA(dir.c_str(),nullptr);
    Handle file{openVirtualCameraShmFile(true)};
    if(file.h==INVALID_HANDLE_VALUE) throw std::runtime_error("QA mapping open failed");
    Handle mapping; void* view=mapVirtualCameraShmView(file.h,true,&mapping.h);
    if(!view) throw std::runtime_error("QA mapping failed");
    struct View { void* p; ~View(){UnmapViewOfFile(p);} } cleanup{view};
    auto* header=static_cast<VirtualCameraShmHeader*>(view);
    auto* payload=static_cast<uint8_t*>(view)+sizeof(*header);
    header->seq=header->seq|1u; MemoryBarrier();
    header->magic=kVirtualCameraMagic; header->width=1920; header->height=1080;
    header->fps=60; header->byteLen=0; header->frameNumber=0;
    MemoryBarrier(); header->seq=(header->seq|1u)+1u;
    VirtualCameraCorrelationMapping correlation; correlation.start(true,file.h);
    ensureVirtualCameraServeLogFile();
    ComPtr<IMFVirtualCamera> camera;
    check(MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource,
      MFVirtualCameraLifetime_Session,MFVirtualCameraAccess_CurrentUser,L"CoreVideo Pro Camera",
      L"{8B4B2C9E-2C4A-4E1D-9C7A-CDEF01234567}",nullptr,0,&camera),"create camera");
    struct CameraStop { ComPtr<IMFVirtualCamera>& c; ~CameraStop(){if(c){c->Stop();c->Remove();}} } stop{camera};
    check(camera->Start(nullptr),"start camera");
    Handle timer{CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS)};
    if(!timer.h) throw std::runtime_error("High-resolution timer unavailable");
    const auto waitUntil=[&](int64_t deadline) {
      while(nowUs()<deadline) {
        LARGE_INTEGER due; due.QuadPart=-((deadline-nowUs())>0 ? (deadline-nowUs())*10 : 1);
        if(!SetWaitableTimer(timer.h,&due,0,nullptr,nullptr,FALSE) ||
           WaitForSingleObject(timer.h,1000)!=WAIT_OBJECT_0) throw std::runtime_error("QA timer failed");
      }
    };
    std::vector<uint8_t> nv12(kVirtualCameraMaxPayload,128);
    std::memset(nv12.data(),16,1920*1080);
    std::vector<Row> rows; rows.reserve(static_cast<size_t>(seconds)*60);
    const auto start=nowUs()+100000;
    for(uint32_t id=1;id<=static_cast<uint32_t>(seconds)*60;++id) {
      // Prepare pixels before the critical write window; only the controlled
      // wait and payload copy occur while the shared header is odd.
      const int cellWidth=(1920/kDeliveryCounterCells)&~1;
      for(int cell=0;cell<kDeliveryCounterCells;++cell) {
        const uint8_t value=deliveryCounterBit(id,cell)?235:16;
        for(int y=0;y<32;++y) {
          std::memset(nv12.data()+y*1920+cell*cellWidth,value,cellWidth);
          std::memset(nv12.data()+(1080-32+y)*1920+cell*cellWidth,value==235?16:235,cellWidth);
        }
      }
      const int64_t due=start+(int64_t(id)-1)*1000000/60;
      waitUntil(due);
      correlation.begin(); header->seq=header->seq|1u; MemoryBarrier();
      const auto begin=nowUs();
      waitUntil(begin+oddUs);
      std::memcpy(payload,nv12.data(),nv12.size());
      header->byteLen=static_cast<uint32_t>(nv12.size()); header->frameNumber=id;
      MemoryBarrier(); header->seq=(header->seq|1u)+1u;
      const auto completed=nowUs();
      correlation.finish(header->seq,id,id,due*10);
      rows.push_back({id,due,begin,completed});
    }
    // Flush bounded evidence only after the timed publication loop.
    std::printf("{\"schema\":\"camera-publication-qa-v1\",\"fps\":60,\"seconds\":%ld,\"oddUs\":%ld,\"isolated\":true}\n",seconds,oddUs);
    for(const auto&r:rows) std::printf("{\"identity\":%u,\"dueUs\":%lld,\"beginUs\":%lld,\"endUs\":%lld}\n",r.identity,r.dueUs,r.beginUs,r.endUs);
    std::printf("{\"complete\":true,\"published\":%zu}\n",rows.size());
    return 0;
  }catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 2;}
}
