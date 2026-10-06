#pragma once
// Test-only SDK surface. The production EngineShare.cpp is compiled against
// this shim to exercise real lifecycle code without proprietary SDK binaries.
#include <cstdint>
class YUVRawDataI420 {
public:
    unsigned GetStreamWidth() const { return 2; }
    unsigned GetStreamHeight() const { return 2; }
    unsigned GetSourceID() const { return 10; }
    char *GetYBuffer() { return pixels; }
    char *GetUBuffer() { return pixels + 4; }
    char *GetVBuffer() { return pixels + 5; }
    bool IsLimitedI420() const { return false; }
    char pixels[6] = {0,0,0,0,0,0};
};
namespace ZOOMSDK {
enum SDKError { SDKERR_SUCCESS, SDKERR_FAILED };
enum ZoomSDKResolution { ZoomSDKResolution_1080P };
enum ZoomSDKRawDataType { RAW_DATA_TYPE_SHARE };
enum SharingStatus { Sharing_Other_Share_Begin, Sharing_View_Other_Sharing, Sharing_Other_Share_End };
struct ZoomSDKSharingSourceInfo { unsigned userid=1; unsigned shareSourceID=10; SharingStatus status=Sharing_Other_Share_Begin; };
struct IShareSwitchMultiToSingleConfirmHandler {};
enum ShareSettingType { ShareSettingDefault };
enum ZoomSDKVideoFileSharePlayError { SharePlayError };
class IMeetingShareCtrlEvent {
public:
    virtual ~IMeetingShareCtrlEvent() = default;
    virtual void onSharingStatus(ZoomSDKSharingSourceInfo)=0;
    virtual void onFailedToStartShare()=0;
    virtual void onLockShareStatus(bool)=0;
    virtual void onShareContentNotification(ZoomSDKSharingSourceInfo)=0;
    virtual void onMultiShareSwitchToSingleShareNeedConfirm(IShareSwitchMultiToSingleConfirmHandler*)=0;
    virtual void onShareSettingTypeChangedNotification(ShareSettingType)=0;
    virtual void onSharedVideoEnded()=0;
    virtual void onVideoFileSharePlayError(ZoomSDKVideoFileSharePlayError)=0;
    virtual void onOptimizingShareForVideoClipStatusChanged(ZoomSDKSharingSourceInfo)=0;
};
template<class T> class IList {
public:
    virtual ~IList()=default;
    virtual int GetCount()=0;
    virtual T GetItem(int)=0;
};
class IMeetingShareController {
public:
    virtual ~IMeetingShareController()=default;
    virtual SDKError SetEvent(IMeetingShareCtrlEvent*)=0;
    virtual IList<unsigned>* GetViewableSharingUserList()=0;
    virtual IList<ZoomSDKSharingSourceInfo>* GetSharingSourceInfoList(unsigned)=0;
};
class IZoomSDKRendererDelegate {
public:
    enum RawDataStatus { RawData_On, RawData_Off };
    virtual ~IZoomSDKRendererDelegate()=default;
    virtual void onRendererBeDestroyed()=0;
    virtual void onRawDataFrameReceived(YUVRawDataI420*)=0;
    virtual void onRawDataStatusChanged(RawDataStatus)=0;
};
class IZoomSDKRenderer {
public:
    virtual ~IZoomSDKRenderer()=default;
    virtual SDKError setRawDataResolution(ZoomSDKResolution)=0;
    virtual SDKError subscribe(unsigned,ZoomSDKRawDataType)=0;
    virtual SDKError unSubscribe()=0;
};
SDKError createRenderer(IZoomSDKRenderer**,IZoomSDKRendererDelegate*);
SDKError destroyRenderer(IZoomSDKRenderer*);
}
