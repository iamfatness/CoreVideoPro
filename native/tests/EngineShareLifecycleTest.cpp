#include "engine-share.h"
#include "renderer-callback-gate.h"
#include "engine-writer.h"
#include <gtest/gtest.h>
#include <functional>
#include <condition_variable>
#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
struct Harness {
    int created=0, destroyed=0, subscribed=0, unsubscribed=0, callbacks=0;
    bool failSubscribe=false, workerCallbacks=false, startupCallbacks=true;
    std::function<void()> duringUnsubscribe;
} h;
void notify(ZOOMSDK::IZoomSDKRendererDelegate *delegate) {
    auto callback=[delegate] {
        delegate->onRawDataStatusChanged(ZOOMSDK::IZoomSDKRendererDelegate::RawData_Off);
        YUVRawDataI420 frame;
        delegate->onRawDataFrameReceived(&frame);
        ++h.callbacks;
    };
    if (h.workerCallbacks) { std::thread worker(callback); worker.join(); }
    else callback();
}
class Renderer : public ZOOMSDK::IZoomSDKRenderer {
public:
    explicit Renderer(ZOOMSDK::IZoomSDKRendererDelegate *d):delegate(d){}
    ZOOMSDK::SDKError setRawDataResolution(ZOOMSDK::ZoomSDKResolution) override {
        if(h.startupCallbacks)notify(delegate); return ZOOMSDK::SDKERR_SUCCESS;
    }
    ZOOMSDK::SDKError subscribe(unsigned,ZOOMSDK::ZoomSDKRawDataType) override {
        ++h.subscribed; if(h.startupCallbacks)notify(delegate);
        return h.failSubscribe ? ZOOMSDK::SDKERR_FAILED : ZOOMSDK::SDKERR_SUCCESS;
    }
    ZOOMSDK::SDKError unSubscribe() override {
        ++h.unsubscribed; notify(delegate);
        if (h.duringUnsubscribe) h.duringUnsubscribe();
        return ZOOMSDK::SDKERR_SUCCESS;
    }
    ZOOMSDK::IZoomSDKRendererDelegate *delegate;
};
template<class T> class List : public ZOOMSDK::IList<T> {
public:
    std::vector<T> items;
    int GetCount() override {return static_cast<int>(items.size());}
    T GetItem(int i) override {return items.at(i);}
};
class Controller : public ZOOMSDK::IMeetingShareController {
public:
    Controller() {users.items={1};sources.items={{1,10,ZOOMSDK::Sharing_Other_Share_Begin}};}
    ZOOMSDK::SDKError SetEvent(ZOOMSDK::IMeetingShareCtrlEvent *e) override {event=e;return ZOOMSDK::SDKERR_SUCCESS;}
    ZOOMSDK::IList<unsigned>* GetViewableSharingUserList() override {return &users;}
    ZOOMSDK::IList<ZOOMSDK::ZoomSDKSharingSourceInfo>* GetSharingSourceInfoList(unsigned) override {return &sources;}
    List<unsigned> users;
    List<ZOOMSDK::ZoomSDKSharingSourceInfo> sources;
    ZOOMSDK::IMeetingShareCtrlEvent *event=nullptr;
};
void start(EngineShare &share,Controller &controller) {
    share.attach(&controller);
    share.subscribe("share-lifecycle-regression",kIpcInvalidFd);
    share.set_raw_media_active(true);
}
}
namespace ZOOMSDK {
SDKError createRenderer(IZoomSDKRenderer **out,IZoomSDKRendererDelegate *delegate) {
    ++h.created;*out=new Renderer(delegate);if(h.startupCallbacks)notify(delegate);return SDKERR_SUCCESS;
}
SDKError destroyRenderer(IZoomSDKRenderer *renderer) {
    auto *fake=static_cast<Renderer*>(renderer);
    notify(fake->delegate);
    if(h.workerCallbacks) {std::thread worker([&]{fake->delegate->onRendererBeDestroyed();});worker.join();}
    else fake->delegate->onRendererBeDestroyed();
    ++h.destroyed;delete fake;return SDKERR_SUCCESS;
}
}
TEST(EngineShareLifecycle, ShareEndAllowsSynchronousRendererCallbacks) {
    h={};h.startupCallbacks=false;Controller controller;EngineShare share;start(share,controller);
    controller.sources.items.clear();
    share.onSharingStatus({1,10,ZOOMSDK::Sharing_Other_Share_End});
    EXPECT_EQ(h.unsubscribed,1);EXPECT_EQ(h.destroyed,1);EXPECT_EQ(h.created,1);
}
TEST(EngineShareLifecycle, SdkCanWaitForCallbacksOnAnotherThread) {
    h={};h.workerCallbacks=true;Controller controller;EngineShare share;start(share,controller);
    share.detach();EXPECT_EQ(h.unsubscribed,1);EXPECT_EQ(h.destroyed,1);
}
TEST(EngineShareLifecycle, SwitchingShareRetiresOnlyOldRenderer) {
    h={};Controller controller;EngineShare share;start(share,controller);
    controller.sources.items={{2,20,ZOOMSDK::Sharing_Other_Share_Begin}};
    share.onSharingStatus(controller.sources.items.front());
    EXPECT_EQ(h.created,2);EXPECT_EQ(h.destroyed,1);
    share.resubscribe_all();EXPECT_EQ(h.created,2);
    share.detach();EXPECT_EQ(h.destroyed,2);
}
TEST(EngineShareLifecycle, StopAndRestartRawMediaPreservesTargets) {
    h={};Controller controller;EngineShare share;start(share,controller);
    share.set_raw_media_active(false);EXPECT_EQ(h.destroyed,1);
    share.set_raw_media_active(true);EXPECT_EQ(h.created,2);
    share.detach();EXPECT_EQ(h.destroyed,2);
}
TEST(EngineShareLifecycle, LastTargetRetirementDoesNotDoubleDestroy) {
    h={};Controller controller;EngineShare share;start(share,controller);
    share.unsubscribe("share-lifecycle-regression");share.unsubscribe_all();share.detach();
    EXPECT_EQ(h.destroyed,1);EXPECT_EQ(h.unsubscribed,1);
}
TEST(EngineShareLifecycle, FailedSubscribeDestroysRendererAndCanRetry) {
    h={};h.failSubscribe=true;Controller controller;EngineShare share;start(share,controller);
    EXPECT_EQ(h.created,1);EXPECT_EQ(h.destroyed,1);EXPECT_EQ(h.unsubscribed,0);
    h.failSubscribe=false;share.resubscribe_all();EXPECT_EQ(h.created,2);
    share.detach();EXPECT_EQ(h.destroyed,2);
}
TEST(EngineShareLifecycle, ShareEventDuringTeardownReconcilesAfterOwnershipCommit) {
    h={};Controller controller;EngineShare share;start(share,controller);
    bool sent=false;
    h.duringUnsubscribe=[&] {
        if(sent)return;sent=true;
        controller.sources.items={{2,20,ZOOMSDK::Sharing_Other_Share_Begin}};
        share.onSharingStatus(controller.sources.items.front());
    };
    share.onSharingStatus({1,10,ZOOMSDK::Sharing_Other_Share_End});
    EXPECT_EQ(h.created,2);EXPECT_EQ(h.destroyed,1);
    share.resubscribe_all();EXPECT_EQ(h.created,2);
    h.duringUnsubscribe={};share.detach();EXPECT_EQ(h.destroyed,2);
}
TEST(EngineShareLifecycle, RepeatedShareEndStartAndSwitchSurvive) {
    h={};Controller controller;EngineShare share;start(share,controller);
    for(unsigned i=0;i<100;++i) {
        controller.sources.items.clear();share.onSharingStatus({1,0,ZOOMSDK::Sharing_Other_Share_End});
        controller.sources.items={{1,10+i,ZOOMSDK::Sharing_Other_Share_Begin}};
        share.onSharingStatus(controller.sources.items.front());
    }
    share.detach();EXPECT_EQ(h.created,101);EXPECT_EQ(h.destroyed,101);
}
TEST(RendererCallbackGate, TransitionReleasesStateLockAndRestoresAdmission) {
    RendererCallbackGate gate;std::mutex mutex;std::unique_lock<std::mutex> lock(mutex);
    {
        RendererCallbackGate::Transition transition(gate,lock);
        EXPECT_FALSE(lock.owns_lock());
        auto callback=gate.enter(mutex);EXPECT_FALSE(callback.owns_lock());
        bool acquired=false;std::thread worker([&]{std::lock_guard<std::mutex> state(mutex);acquired=true;});worker.join();
        EXPECT_TRUE(acquired);
    }
    EXPECT_TRUE(lock.owns_lock());lock.unlock();
    auto callback=gate.enter(mutex);EXPECT_TRUE(callback.owns_lock());
}

TEST(RendererCallbackGate, TransitionWaitsForAnAdmittedCallbackToFinish) {
    RendererCallbackGate gate;std::mutex state,signal;
    std::condition_variable cv;bool admitted=false,released=false;
    std::atomic<bool> sdkEntered{false};
    std::thread callback([&] {
        auto lock=gate.enter(state);
        {std::lock_guard<std::mutex> s(signal);admitted=true;}cv.notify_all();
        std::unique_lock<std::mutex> s(signal);cv.wait(s,[&]{return released;});
    });
    {std::unique_lock<std::mutex> s(signal);cv.wait(s,[&]{return admitted;});}
    std::thread lifecycle([&] {
        std::unique_lock<std::mutex> lock(state);
        RendererCallbackGate::Transition transition(gate,lock);sdkEntered=true;
    });
    EXPECT_FALSE(sdkEntered.load());
    {std::lock_guard<std::mutex> s(signal);released=true;}cv.notify_all();
    callback.join();lifecycle.join();EXPECT_TRUE(sdkEntered.load());
}
TEST(RendererCallbackGate, SdkExceptionRestoresStateLockAndCallbackAdmission) {
    RendererCallbackGate gate;std::mutex state;std::unique_lock<std::mutex> lock(state);
    try {RendererCallbackGate::Transition transition(gate,lock);throw std::runtime_error("SDK failure");}
    catch(const std::runtime_error&) {}
    EXPECT_TRUE(lock.owns_lock());EXPECT_FALSE(gate.suspended());
}

TEST(EngineShareLifecycle, FramesResumeWithTargetsIntactAfterRendererRestart) {
    h={};Controller controller;EngineShare share;start(share,controller);
    share.set_raw_media_active(false);share.set_raw_media_active(true);
    YUVRawDataI420 frame;share.onRawDataFrameReceived(&frame);
    ShmRegion reader;
    const bool opened=shm_region_open_read(reader,EngineIpc::shm_prefix()+"share-lifecycle-regression",sizeof(ShmFrameHeader)+6);
    EXPECT_TRUE(opened);
    if(opened) {
        auto *header=static_cast<ShmFrameHeader*>(reader.ptr);
        EXPECT_EQ(header->width,2u);EXPECT_EQ(header->height,2u);
        EXPECT_EQ(header->y_len,4u);EXPECT_TRUE(header->sequence>0);
        shm_region_destroy(reader);
    }
    share.unsubscribe_all();EXPECT_EQ(h.destroyed,2);
}
TEST(EngineShareLifecycle, SdkCanWaitForShareControllerEventDuringTeardown) {
    h={};Controller controller;EngineShare share;start(share,controller);
    controller.sources.items.clear();
    h.duringUnsubscribe=[&] {
        std::thread event([&]{share.onSharingStatus({1,10,ZOOMSDK::Sharing_Other_Share_End});});event.join();
    };
    share.onSharingStatus({1,10,ZOOMSDK::Sharing_Other_Share_End});
    EXPECT_EQ(h.destroyed,1);EXPECT_EQ(h.created,1);h.duringUnsubscribe={};
}
