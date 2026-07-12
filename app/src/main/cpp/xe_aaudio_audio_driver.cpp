/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xe_aaudio_audio_driver.h"

#include <cstring>

#include "xenia/apu/apu_flags.h"
#include "xenia/apu/conversion.h"
#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"
#include "xenia/base/testrig_debug_server.h"  // TESTRIG(audio)

namespace xe {
namespace apu {
namespace aaudio {

AAudioAudioDriver::AAudioAudioDriver(Memory* memory,
                                     xe::threading::Semaphore* semaphore)
    : semaphore_(semaphore) {}

AAudioAudioDriver::~AAudioAudioDriver() {
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
}

bool AAudioAudioDriver::Initialize() {
  aaudio_result_t result;

  result = AAudio_createStreamBuilder(&builder_);
  if (result != AAUDIO_OK) {
    XELOGE("AAudio_createStreamBuilder failed: {}", result);
    return false;
  }

  AAudioStreamBuilder_setFormat(builder_, AAUDIO_FORMAT_PCM_FLOAT);
  AAudioStreamBuilder_setSampleRate(builder_, host_frame_frequency_);
  AAudioStreamBuilder_setChannelCount(builder_, host_frame_channels_);
  AAudioStreamBuilder_setFramesPerDataCallback(builder_, channel_samples_);
  //AAudioStreamBuilder_setBufferCapacityInFrames(builder_, channel_samples_ * 2);

  AAudioStreamBuilder_setDataCallback(builder_, AudioCallback, this);
  AAudioStreamBuilder_setErrorCallback(builder_, AudioErrorCallback, this);

  AAudioStreamBuilder_setPerformanceMode(builder_, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
  AAudioStreamBuilder_setSharingMode(builder_, AAUDIO_SHARING_MODE_EXCLUSIVE);

  result = AAudioStreamBuilder_openStream(builder_, &stream_);
  if (result != AAUDIO_OK) {
    XELOGE("AAudioStreamBuilder_openStream failed: {}", result);
    return false;
  }

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    for (int i = 0; i < 2; i++) {
      float* buffer = new float[x360_frame_samples_];
      frames_unused_.push(buffer);
    }
  }

  stream_initialized_ = true;

  result = AAudioStream_requestStart(stream_);
  if (result != AAUDIO_OK) {
    XELOGE("AAudioStream_requestStart failed: {}", result);
    return false;
  }

  // TESTRIG(audio): expose live AAudio output stream state - see
  // docs/TEST_HARNESS.md.
  xe::testrig::Expose(xe::testrig::kPortAudio, "audio",
                       [this]() { return FormatDebugSnapshot(); });

  return true;
}

// TESTRIG(audio): live snapshot of AAudio output stream state - buffer
// under/overrun (xrun) count, actual (not just requested) performance and
// sharing mode (AAudio may silently fall back from EXCLUSIVE to SHARED, or
// from LOW_LATENCY to NONE, if the requested mode isn't available), and
// Xenia's own frame queue depth (empty queue = the callback fed silence,
// i.e. a starvation underrun on Xenia's side even if AAudio itself has no
// xrun to report).
std::string AAudioAudioDriver::FormatDebugSnapshot() {
  if (!stream_initialized_ || !stream_) {
    return "AAudio stream not initialized";
  }

  aaudio_stream_state_t state = AAudioStream_getState(stream_);
  int32_t xrun_count = AAudioStream_getXRunCount(stream_);
  aaudio_performance_mode_t perf_mode = AAudioStream_getPerformanceMode(stream_);
  aaudio_sharing_mode_t sharing_mode = AAudioStream_getSharingMode(stream_);
  int32_t sample_rate = AAudioStream_getSampleRate(stream_);
  int32_t channel_count = AAudioStream_getChannelCount(stream_);
  int32_t buffer_size = AAudioStream_getBufferSizeInFrames(stream_);
  int32_t buffer_capacity = AAudioStream_getBufferCapacityInFrames(stream_);
  int32_t frames_per_burst = AAudioStream_getFramesPerBurst(stream_);
  int64_t frames_written = AAudioStream_getFramesWritten(stream_);
  int64_t frames_read = AAudioStream_getFramesRead(stream_);

  size_t queued;
  size_t unused;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    queued = frames_queued_.size();
    unused = frames_unused_.size();
  }

  // AAudio may silently downgrade the requested mode if it can't be granted
  // (e.g. EXCLUSIVE falling back to SHARED, or LOW_LATENCY falling back to
  // NONE) - decode to text so a fallback is obvious without memorizing the
  // enum values.
  const char* perf_mode_text =
      perf_mode == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY   ? "LOW_LATENCY"
      : perf_mode == AAUDIO_PERFORMANCE_MODE_POWER_SAVING ? "POWER_SAVING"
      : perf_mode == AAUDIO_PERFORMANCE_MODE_NONE         ? "NONE"
                                                           : "UNKNOWN";
  const char* sharing_mode_text =
      sharing_mode == AAUDIO_SHARING_MODE_EXCLUSIVE ? "EXCLUSIVE"
      : sharing_mode == AAUDIO_SHARING_MODE_SHARED  ? "SHARED"
                                                     : "UNKNOWN";

  return fmt::format(
      "state: {}\n"
      "xrun_count: {}\n"
      "performance_mode: {} (requested LOW_LATENCY)\n"
      "sharing_mode: {} (requested EXCLUSIVE)\n"
      "sample_rate: {}\n"
      "channel_count: {}\n"
      "buffer_size_frames: {}\n"
      "buffer_capacity_frames: {}\n"
      "frames_per_burst: {}\n"
      "frames_written: {}\n"
      "frames_read: {}\n"
      "xenia_frames_queued: {} (0 = fed silence this callback)\n"
      "xenia_frames_unused: {}",
      AAudio_convertStreamStateToText(state), xrun_count, perf_mode_text,
      sharing_mode_text, sample_rate, channel_count, buffer_size,
      buffer_capacity, frames_per_burst, frames_written, frames_read, queued,
      unused);
}

void AAudioAudioDriver::Pause() {
  if (stream_initialized_) {
    AAudioStream_requestPause(stream_);
  }
}

void AAudioAudioDriver::Resume() {
  if (stream_initialized_) {
    AAudioStream_requestStart(stream_);
  }
}

void AAudioAudioDriver::SetVolume(float volume) {
    //FIXME
}

aaudio_data_callback_result_t AAudioAudioDriver::AudioCallback(
    AAudioStream* stream,
    void* userdata,
    void* audioData,
    int32_t numFrames) {
  SCOPE_profile_cpu_f("apu");

  auto driver = static_cast<AAudioAudioDriver*>(userdata);
  float* output_buffer = reinterpret_cast<float*>(audioData);
  const int32_t samples_count = numFrames * 2;

  std::unique_lock<std::mutex> guard(driver->frames_mutex_);

  if (driver->frames_queued_.empty()) {
    std::memset(output_buffer, 0, samples_count * sizeof(float));
  } else {
    auto buffer = driver->frames_queued_.front();
    driver->frames_queued_.pop();

    if (cvars::mute) {
      std::memset(output_buffer, 0, samples_count * sizeof(float));
    } else {
        conversion::sequential_6_BE_to_interleaved_2_LE(
                output_buffer, buffer, channel_samples_);
    }

    driver->frames_unused_.push(buffer);

      auto ret = driver->semaphore_->Release(1, nullptr);
      assert_true(ret);
  }

  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AAudioAudioDriver::AudioErrorCallback(
    AAudioStream* stream,
    void* userdata,
    aaudio_result_t error) {
  XELOGE("AAudio stream error: {}", error);
}

void AAudioAudioDriver::SubmitFrame(float* samples) {
    const auto input_frame = samples;
  float* output_frame;

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[x360_frame_samples_];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  std::memcpy(output_frame, input_frame, x360_frame_samples_*sizeof(float));

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
  }
}

void AAudioAudioDriver::Shutdown() {
  if (stream_) {
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
  }

  if (builder_) {
    AAudioStreamBuilder_delete(builder_);
    builder_ = nullptr;
  }

  stream_initialized_ = false;

  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }

  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

}  // namespace aaudio
}  // namespace apu
}  // namespace xe
