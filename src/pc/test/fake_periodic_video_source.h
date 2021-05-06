/*
 *  Copyright 2018 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef PC_TEST_FAKE_PERIODIC_VIDEO_SOURCE_H_
#define PC_TEST_FAKE_PERIODIC_VIDEO_SOURCE_H_

#include <memory>

#include "api/video/video_source_interface.h"
#include "media/base/fake_frame_source.h"
#include "media/base/video_broadcaster.h"
#include "rtc_base/critical_section.h"
#include "rtc_base/task_queue_for_test.h"
#include "rtc_base/task_utils/repeating_task.h"

namespace webrtc {

class FakePeriodicVideoSource final
    : public rtc::VideoSourceInterface<VideoFrame> {
 public:
  static constexpr int kDefaultFrameIntervalMs = 33;
  static constexpr int kDefaultWidth = 640;
  static constexpr int kDefaultHeight = 480;

  struct Config {
    int width = kDefaultWidth;
    int height = kDefaultHeight;
    int frame_interval_ms = kDefaultFrameIntervalMs;
    VideoRotation rotation = kVideoRotation_0;
    int64_t timestamp_offset_ms = 0;
  };

  FakePeriodicVideoSource() : FakePeriodicVideoSource(Config()) {}
  explicit FakePeriodicVideoSource(Config config)
      : frame_source_(
            config.width,
            config.height,
            config.frame_interval_ms * rtc::kNumMicrosecsPerMillisec,
            config.timestamp_offset_ms * rtc::kNumMicrosecsPerMillisec),
        task_queue_(std::make_unique<TaskQueueForTest>(
            "FakePeriodicVideoTrackSource")) {
    thread_checker_.Detach();
    frame_source_.SetRotation(config.rotation);

    TimeDelta frame_interval = TimeDelta::Millis(config.frame_interval_ms);
    RepeatingTaskHandle::Start(task_queue_->Get(), [this, frame_interval] {
      if (broadcaster_.wants().rotation_applied) {
        broadcaster_.OnFrame(frame_source_.GetFrameRotationApplied());
      } else {
        broadcaster_.OnFrame(frame_source_.GetFrame());
      }
      return frame_interval;
    });
  }

  rtc::VideoSinkWants wants() const {
    rtc::CritScope cs(&crit_);
    return wants_;
  }

  void RemoveSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink) override {
    RTC_DCHECK(thread_checker_.IsCurrent());
    broadcaster_.RemoveSink(sink);
  }

  void AddOrUpdateSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
                       const rtc::VideoSinkWants& wants) override {
    RTC_DCHECK(thread_checker_.IsCurrent());
    {
      rtc::CritScope cs(&crit_);
      wants_ = wants;
    }
    broadcaster_.AddOrUpdateSink(sink, wants);
  }

  void Stop() {
    RTC_DCHECK(task_queue_);
    task_queue_.reset();
  }

 private:
  rtc::ThreadChecker thread_checker_;

  rtc::VideoBroadcaster broadcaster_;
  cricket::FakeFrameSource frame_source_;
  rtc::CriticalSection crit_;
  rtc::VideoSinkWants wants_ RTC_GUARDED_BY(&crit_);

  std::unique_ptr<TaskQueueForTest> task_queue_;
};

}  // namespace webrtc

#endif  // PC_TEST_FAKE_PERIODIC_VIDEO_SOURCE_H_
