#if defined(_WIN32)
#include "modules/UvcAsyncRead.h"
#include <gtest/gtest.h>
#include <future>
using corevideo::modules::UvcAsyncRead;

TEST(UvcAsyncRead, ARequestWithoutAnyCallbackRemainsBoundedAndIsNotReissued) {
  auto* callback = new UvcAsyncRead();
  int requests = 0; DWORD flags = 0; LONGLONG pts = 0; IMFSample* sample = nullptr;
  const auto request = [&] { ++requests; return S_OK; };
  for (int i = 0; i < 5; ++i)
    EXPECT_EQ(callback->take(request, std::chrono::milliseconds(1), &flags, &pts, &sample), S_FALSE);
  EXPECT_EQ(requests, 1); EXPECT_EQ(sample, nullptr);
  callback->cancel(); callback->Release();
}

TEST(UvcAsyncRead, CancellationWakesWaitAndLateCallbacksCannotPublishIntoRetiredOwner) {
  auto* callback = new UvcAsyncRead();
  std::promise<void> requested; auto ready = requested.get_future();
  auto worker = std::async(std::launch::async, [&] { DWORD flags; LONGLONG pts; IMFSample* sample = nullptr;
    return callback->take([&] { requested.set_value(); return S_OK; }, std::chrono::seconds(5), &flags, &pts, &sample); });
  ASSERT_EQ(ready.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  callback->cancel();
  EXPECT_EQ(worker.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_EQ(worker.get(), MF_E_SHUTDOWN);
  callback->OnReadSample(S_OK, 0, 123, 456, nullptr);
  DWORD flags; LONGLONG pts; IMFSample* sample = nullptr;
  int requests = 0;
  EXPECT_EQ(callback->take([&] { ++requests; return S_OK; }, std::chrono::milliseconds(0), &flags, &pts, &sample), MF_E_SHUTDOWN);
  EXPECT_EQ(requests, 0); EXPECT_EQ(sample, nullptr); callback->Release();
}

TEST(UvcAsyncRead, DeliversOriginalReplyAndPrimesOneNextRequestWithoutOverwritingIt) {
  auto* callback = new UvcAsyncRead();
  int requests = 0; DWORD flags; LONGLONG pts; IMFSample* sample = nullptr;
  const auto request = [&] { ++requests; return S_OK; };
  EXPECT_EQ(callback->take(request, std::chrono::milliseconds(0), &flags, &pts, &sample), S_FALSE);
  callback->OnReadSample(S_OK, 0, MF_SOURCE_READERF_STREAMTICK, 123, nullptr);
  EXPECT_EQ(callback->take(request, std::chrono::milliseconds(0), &flags, &pts, &sample), S_OK);
  EXPECT_EQ(flags, MF_SOURCE_READERF_STREAMTICK); EXPECT_EQ(pts, 123); EXPECT_EQ(requests, 2);
  callback->OnReadSample(E_FAIL, 0, 0, 456, nullptr);
  callback->OnReadSample(S_OK, 0, 0, 999, nullptr); // duplicate must not replace failure
  EXPECT_EQ(callback->take(request, std::chrono::milliseconds(0), &flags, &pts, &sample), E_FAIL);
  EXPECT_EQ(pts, 456); EXPECT_EQ(requests, 2);
  callback->cancel(); callback->Release();
}

TEST(UvcAsyncRead, FlushCancelsPendingRequestAndNextPollCanRearm) {
  auto* callback = new UvcAsyncRead();
  int requests = 0; DWORD flags; LONGLONG pts; IMFSample* sample = nullptr;
  const auto request = [&] { ++requests; return S_OK; };
  EXPECT_EQ(callback->take(request, std::chrono::milliseconds(0), &flags, &pts, &sample), S_FALSE);
  callback->OnFlush(0);
  EXPECT_EQ(callback->take(request, std::chrono::milliseconds(0), &flags, &pts, &sample), S_FALSE);
  EXPECT_EQ(requests, 2);
  callback->cancel(); callback->Release();
}

TEST(UvcAsyncRead, SampleReferenceSurvivesCallbackRetirementAndImmediatePrimeFailureIsRetained) {
  ASSERT_TRUE(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)));
  auto* callback = new UvcAsyncRead();
  int requests = 0; DWORD flags; LONGLONG pts; IMFSample* output = nullptr;
  const auto request = [&] { return ++requests <= 1 ? S_OK : E_FAIL; };
  EXPECT_EQ(callback->take(request, std::chrono::milliseconds(0), &flags, &pts, &output), S_FALSE);
  IMFSample* original = nullptr;
  ASSERT_TRUE(SUCCEEDED(MFCreateSample(&original)));
  original->SetSampleTime(12345);
  callback->OnReadSample(S_OK, 0, 0, 456, original);
  original->Release();
  EXPECT_EQ(callback->take(request, std::chrono::milliseconds(0), &flags, &pts, &output), S_OK);
  ASSERT_TRUE(output != nullptr);
  IMFSample* held = output;
  EXPECT_EQ(callback->take(request, std::chrono::milliseconds(0), &flags, &pts, &output), E_FAIL);
  EXPECT_EQ(requests, 2);
  callback->cancel(); callback->Release();
  LONGLONG time = 0;
  EXPECT_EQ(held->GetSampleTime(&time), S_OK); EXPECT_EQ(time, 12345);
  held->Release(); MFShutdown();
}
#endif
