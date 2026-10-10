#define NOMINMAX
#include <windows.h>
#include "modules/AsyncVirtualCameraPublisher.h"
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <thread>
using namespace corevideo::modules;
using Clock=std::chrono::steady_clock;
int64_t nowUs(){return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();}
struct Row{int64_t sequence,due,submitted,arrived;bool pixelsCorrect;};
struct Sink : IVirtualCameraPublisher {
  VirtualCameraStatus current;
  bool framer=false;
  std::vector<Row> rows;
  std::unique_ptr<std::atomic<int64_t>[]> submitted=std::make_unique<std::atomic<int64_t>[]>(10801);
  explicit Sink(bool on):framer(on){rows.reserve(10800);}
  bool start(int,int,int) override {current.enabled=true;current.state="live";return true;}
  void stop() override {current.enabled=false;current.state="off";}
  void publish(const ProgramFrame&) override {}
  VirtualCameraStatus status() const override {return current;}
  void publishNv12IdentifiedOnWorker(std::shared_ptr<const std::vector<uint8_t>> bytes,int w,int h,int64_t seq,int64_t at) override {
    ++current.framesPublished;
    if(seq>0) rows.push_back({seq,at/10,submitted[seq].load(),nowUs(),w==1920&&h==1080&&bytes->at(400*1920+960)==100&&bytes->at(950*1920+960)==(framer?58:100)});
  }
};
int main(int argc,char**argv){
  if(argc!=4)return 2;
  const int seconds=std::atoi(argv[1]);const bool on=std::string(argv[2])=="on";
  if(seconds<30||seconds>180||(std::string(argv[2])!="on"&&std::string(argv[2])!="off"))return 2;
  try {
    auto sink=std::make_unique<Sink>(on);auto* capture=sink.get();
    AsyncVirtualCameraPublisher camera(std::move(sink));camera.setFramerEnabled(on);camera.start(1920,1080,60);
    const auto clean=std::make_shared<const std::vector<uint8_t>>(1920*1080*3/2,100);
    const auto warmupEnd=Clock::now()+std::chrono::seconds(2);
    while(Clock::now()<warmupEnd){camera.publishNv12Identified(clean,1920,1080,0,0);std::this_thread::sleep_for(std::chrono::milliseconds(16));}
    if(camera.status().state!="live"||(on&&camera.status().framerState!="active"))throw std::runtime_error("Stage failed to activate");
    const auto before=camera.status();
    HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    if(!timer)throw std::runtime_error("High-resolution timer unavailable");
    const int64_t start=nowUs();
    for(int64_t sequence=1;sequence<=seconds*60;++sequence){
      const int64_t due=start+sequence*1000000/60;
      while(nowUs()<due){LARGE_INTEGER wait;wait.QuadPart=-((due-nowUs())*10+1);SetWaitableTimer(timer,&wait,0,nullptr,nullptr,FALSE);WaitForSingleObject(timer,1000);}
      capture->submitted[sequence].store(nowUs());
      camera.publishNv12Identified(clean,1920,1080,sequence,due*10);
    }
    CloseHandle(timer);std::this_thread::sleep_for(std::chrono::milliseconds(50));camera.stop();
    const auto stopBy=Clock::now()+std::chrono::seconds(2);
    while(camera.status().state!="off"&&Clock::now()<stopBy)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto after=camera.status();if(after.state!="off")throw std::runtime_error("Stop did not settle");
    std::ofstream csv(std::string(argv[3])+".csv");csv<<"sequence,dueUs,submittedUs,arrivedUs,pixelsCorrect\n";
    int64_t gaps=0,late=0,bad=0,last=0,maxDelay=0,maxInterval=0,previousArrival=0,maxQueueDelay=0,producerLate=0,workerLate=0;
    for(const auto&r:capture->rows){
      csv<<r.sequence<<','<<r.due<<','<<r.submitted<<','<<r.arrived<<','<<r.pixelsCorrect<<'\n';
      gaps+=r.sequence-last-1;last=r.sequence;if(r.arrived-r.due>=16667)++late;if(!r.pixelsCorrect)++bad;
      if(r.arrived-r.submitted>=16667)++workerLate;
      maxQueueDelay=std::max(maxQueueDelay,r.arrived-r.submitted);
      maxDelay=std::max(maxDelay,r.arrived-r.due);if(previousArrival)maxInterval=std::max(maxInterval,r.arrived-previousArrival);previousArrival=r.arrived;
    }
    gaps+=seconds*60-last;
    std::ofstream produced(std::string(argv[3])+"-producer.csv");produced<<"sequence,dueUs,submittedUs\n";
    for(int64_t seq=1;seq<=seconds*60;++seq){
      const auto due=start+seq*1000000/60,submitted=capture->submitted[seq].load();
      produced<<seq<<','<<due<<','<<submitted<<'\n';
      if(submitted-due>=16667)++producerLate;
    }
    const auto replaced=after.pendingFramesReplaced-before.pendingFramesReplaced;
    const bool pass=gaps==0&&late==0&&bad==0&&replaced==0;
    std::printf("{\"scope\":\"Production camera worker with a test sink; no SHM, receiver, Program, recording or meeting qualification\",\"framer\":%s,\"seconds\":%d,\"submitted\":%d,\"published\":%zu,\"identityGaps\":%lld,\"lateBeyondNextTick\":%lld,\"badPixels\":%lld,\"pendingReplaced\":%llu,\"startupPendingReplaced\":%llu,\"producerLateFrames\":%lld,\"workerLateAfterSubmission\":%lld,\"maxQueueToArrivalUs\":%lld,\"maxDelayUs\":%lld,\"maxIntervalUs\":%lld,\"result\":\"%s\"}\n",on?"true":"false",seconds,seconds*60,capture->rows.size(),gaps,late,bad,static_cast<unsigned long long>(replaced),static_cast<unsigned long long>(before.pendingFramesReplaced),producerLate,workerLate,maxQueueDelay,maxDelay,maxInterval,pass?"PASS":"FAIL");
    return pass?0:1;
  }catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 2;}
}
