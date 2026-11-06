#include "modules/Interfaces.h"
#include "EncoderCapacityProbeTestSupport.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

namespace {
using Microsoft::WRL::ComPtr;
struct SoftwareRequest {
  std::string previous;
  explicit SoftwareRequest(bool software) {
    if (const char* value = std::getenv("COREVIDEO_QA_PROGRAM_SOFTWARE_ENCODER")) previous = value;
    _putenv_s("COREVIDEO_QA_PROGRAM_SOFTWARE_ENCODER", software ? "1" : "");
  }
  ~SoftwareRequest() { _putenv_s("COREVIDEO_QA_PROGRAM_SOFTWARE_ENCODER", previous.c_str()); }
};

void checkProgramTail(int audioFrames, bool software) {
  SoftwareRequest policy(software);
  const corevideo::testing::ForcedEncoderCapacity capacity;
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  ASSERT_TRUE(encoder);
  const auto directory = std::filesystem::temp_directory_path() /
      ("corevideo-program-tail-1186-" + std::to_string(GetCurrentProcessId()) + "-" +
       std::to_string(GetTickCount64()) + "-" + std::to_string(audioFrames) + (software ? "-software" : "-preferred"));
  corevideo::modules::RecordingSessionRequest request;
  request.targetFolder = directory.string(); request.filenamePrefix = "tail";
  request.width = 320; request.height = 180; request.fps = 60;
  request.programNv12 = true;
  encoder->configureRecording(request);
  encoder->start({"recording"}, {});
  corevideo::modules::ProgramFrame frame;
  frame.width = frame.programNv12Width = 320;
  frame.height = frame.programNv12Height = 180;
  frame.productionAnchorNs = 1;
  frame.programNv12.assign(320 * 180 * 3 / 2, 128);
  int64_t epoch = 0;
  for (int i = 0; i < 6; ++i) {
    frame.frameNumber = i + 1; frame.productionSlot = i;
    frame.timelineTimestamp100ns = i ? epoch + i * 10'000'000LL / 60 : 0;
    std::fill_n(frame.programNv12.begin(), 320 * 180, static_cast<uint8_t>(40 + i * 24));
    encoder->submit(frame);
    if (i == 0) { epoch = encoder->session().recordingMuxEpoch100ns; ASSERT_GT(epoch, 0); }
  }
  std::vector<float> pcm(static_cast<size_t>(audioFrames) * 2);
  for (int i = 0; i < audioFrames; ++i)
    pcm[i * 2] = pcm[i * 2 + 1] = static_cast<float>(0.15 * std::sin(i * 6.283185307179586 * 1000 / 48000));
  encoder->submitAudioAt(pcm.data(), audioFrames, 2, 48000, epoch);
  encoder->stopRecording();
  const auto final = encoder->session();
  ASSERT_TRUE(final.recordingError.empty()) << final.recordingError;
  EXPECT_EQ(final.recordingVideoFrameCount, 6);
  EXPECT_EQ(final.recordingVideoTailFrameCount, 0) << "close appended a duplicate Program identity";
  EXPECT_EQ(final.recordingMuxVideoFrameCount, 6);
  EXPECT_GE(final.recordingAudioSampleCount, audioFrames) << "accepted audio tail was truncated";
  if (software) EXPECT_EQ(final.programEncoderPath, "software");
  std::printf("[program-tail] file=%s software_requested=%d measured_path=%s audio_frames=%d\n",
      final.recordingArtifactPath.c_str(), software, final.programEncoderPath.c_str(), audioFrames);

  ComPtr<IMFSourceReader> reader;
  const std::filesystem::path path(final.recordingArtifactPath);
  ASSERT_TRUE(SUCCEEDED(MFCreateSourceReaderFromURL(path.wstring().c_str(), nullptr, &reader)));
  ASSERT_TRUE(SUCCEEDED(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE)));
  ASSERT_TRUE(SUCCEEDED(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE)));
  ComPtr<IMFMediaType> type;
  ASSERT_TRUE(SUCCEEDED(MFCreateMediaType(&type)));
  type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  ASSERT_TRUE(SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get())));
  int pictures = 0;
  LONGLONG videoEnd = 0;
  for (;;) {
    DWORD flags = 0; LONGLONG pts = 0;
    ComPtr<IMFSample> sample;
    ASSERT_TRUE(SUCCEEDED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &pts, &sample)));
    if (sample) {
      ComPtr<IMFMediaBuffer> buffer; ASSERT_TRUE(SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)));
      BYTE* bytes = nullptr; DWORD length = 0;
      ASSERT_TRUE(SUCCEEDED(buffer->Lock(&bytes, nullptr, &length)));
      const auto luma = length ? bytes[0] : 0;
      buffer->Unlock();
      EXPECT_NEAR(static_cast<int>(luma), 40 + pictures * 24, 5) << "decoded identity changed/repeated";
      LONGLONG duration = 0; ASSERT_TRUE(SUCCEEDED(sample->GetSampleDuration(&duration)));
      videoEnd = pts + duration;
      ++pictures;
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    ASSERT_LE(pictures, 20);
  }
  EXPECT_EQ(pictures, 6);
  const auto expectedEnd = (std::max)(1'000'000LL, static_cast<LONGLONG>(audioFrames) * 10'000'000LL / 48000);
  EXPECT_NEAR(static_cast<double>(videoEnd), static_cast<double>(expectedEnd), 1000.0)
      << "final real sample did not retain the common endpoint";
  reader.Reset(); type.Reset();

  ASSERT_TRUE(SUCCEEDED(MFCreateSourceReaderFromURL(path.wstring().c_str(), nullptr, &reader)));
  reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
  reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
  ASSERT_TRUE(SUCCEEDED(MFCreateMediaType(&type)));
  type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
  type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2); type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
  type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16); type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
  type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000);
  ASSERT_TRUE(SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type.Get())));
  int decodedFrames = 0, tailSamples = 0;
  double tailSquares = 0;
  for (;;) {
    DWORD flags = 0; LONGLONG pts = 0;
    ComPtr<IMFSample> sample;
    ASSERT_TRUE(SUCCEEDED(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &pts, &sample)));
    if (sample) {
      ComPtr<IMFMediaBuffer> buffer; ASSERT_TRUE(SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)));
      BYTE* bytes = nullptr; DWORD length = 0;
      ASSERT_TRUE(SUCCEEDED(buffer->Lock(&bytes, nullptr, &length)));
      const auto* values = reinterpret_cast<const int16_t*>(bytes);
      for (DWORD i = 0; i < length / 4; ++i) {
        const auto at = pts + static_cast<LONGLONG>(i) * 10'000'000LL / 48000;
        if (at >= static_cast<LONGLONG>(audioFrames - 480) * 10'000'000LL / 48000 &&
            at < static_cast<LONGLONG>(audioFrames) * 10'000'000LL / 48000) {
          const auto value = values[i * 2] / 32768.0;
          tailSquares += value * value; ++tailSamples;
        }
      }
      decodedFrames += length / 4;
      buffer->Unlock();
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    ASSERT_LE(decodedFrames, 48000);
  }
  EXPECT_GE(decodedFrames, audioFrames);
  EXPECT_GT(tailSamples, 0);
  EXPECT_GT(tailSamples ? std::sqrt(tailSquares / tailSamples) : 0.0, 0.03)
      << "last accepted audio content did not survive finalization";
  // Keep the tiny real file for independent FFmpeg/packet inspection.
}
}

TEST(ProgramRecordingTail, AudioLongerKeepsOneFinalPictureAndAllAudio) { checkProgramTail(9600, false); }
TEST(ProgramRecordingTail, EqualEndpointsDoNotInventAPicture) { checkProgramTail(4800, false); }
TEST(ProgramRecordingTail, VideoLongerKeepsPicturesAndPadsOnlyAudio) { checkProgramTail(2400, false); }
TEST(ProgramRecordingTail, SoftwareWriterPreservesTheSameTailContract) { checkProgramTail(9600, true); }
#endif
