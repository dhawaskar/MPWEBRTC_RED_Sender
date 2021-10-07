/*
 *  Copyright (c) 2019 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include "modules/congestion_controller/rtp/transport_feedback_demuxer.h"
#include "absl/algorithm/container.h"
#include "modules/rtp_rtcp/source/rtcp_packet/transport_feedback.h"
#include "rtc_base/logging.h"
namespace webrtc {
namespace {
static const size_t kMaxPacketsInHistory = 5000;
}
void TransportFeedbackDemuxer::RegisterStreamFeedbackObserver(
    std::vector<uint32_t> ssrcs,
    StreamFeedbackObserver* observer) {
  rtc::CritScope cs(&observers_lock_);
  RTC_DCHECK(observer);
  RTC_DCHECK(absl::c_find_if(observers_, [=](const auto& pair) {
               return pair.second == observer;
             }) == observers_.end());
  observers_.push_back({ssrcs, observer});
}

void TransportFeedbackDemuxer::DeRegisterStreamFeedbackObserver(
    StreamFeedbackObserver* observer) {
  rtc::CritScope cs(&observers_lock_);
  RTC_DCHECK(observer);
  const auto it = absl::c_find_if(
      observers_, [=](const auto& pair) { return pair.second == observer; });
  RTC_DCHECK(it != observers_.end());
  observers_.erase(it);
}

void TransportFeedbackDemuxer::AddPacket(const RtpPacketSendInfo& packet_info) {
  rtc::CritScope cs(&lock_);

  int pathid=packet_info.pathid;
  RTC_DCHECK(pathid>0);

  if (packet_info.ssrc != 0) {
    StreamFeedbackObserver::StreamPacketInfo info;
    info.ssrc = packet_info.ssrc;
    info.rtp_sequence_number = packet_info.rtp_sequence_number;//sandy: Keep meta sequence number
    info.mp_rtp_sequence_number=packet_info.mp_rtp_sequence_number;
    info.received = false;
    if(pathid!=2){
      history_.insert(
        {seq_num_unwrapper_.Unwrap(packet_info.mp_transport_sequence_number),//sandy: you cannot add by transport sequence number but by MP_Transport_sequence_number
         info});
    }else{
      history_s_.insert(
        {seq_num_unwrapper_.Unwrap(packet_info.mp_transport_sequence_number),//sandy: you cannot add by transport sequence number but by MP_Transport_sequence_number
         info});
    }
    
  }
  while (history_.size() > kMaxPacketsInHistory && pathid!=2) {
    history_.erase(history_.begin());
  }
  while (history_s_.size() > kMaxPacketsInHistory && pathid==2) {
    history_s_.erase(history_s_.begin());
  }
}

void TransportFeedbackDemuxer::OnTransportFeedback(
    const rtcp::TransportFeedback& feedback,int pathid) {

  
  std::vector<StreamFeedbackObserver::StreamPacketInfo> stream_feedbacks;
  {
    rtc::CritScope cs(&lock_);
    for (const auto& packet : feedback.GetAllPackets()) {
      int64_t seq_num =
          seq_num_unwrapper_.UnwrapWithoutUpdate(packet.sequence_number());//sandy: Receiver sends the Mp-Transport
      if(pathid!=2){
        auto it = history_.find(seq_num);
        if (it != history_.end()) {
          auto packet_info = it->second;
          packet_info.received = packet.received();
          packet_info.pathid=pathid;//sandy: Save the packet pathid
          stream_feedbacks.push_back(packet_info);
          if (packet.received())
            history_.erase(it);
        }
      }else{
        auto it = history_s_.find(seq_num);
        if (it != history_s_.end()) {
          auto packet_info = it->second;
          packet_info.received = packet.received();
          packet_info.pathid=pathid;//sandy: Save the packet pathid
          stream_feedbacks.push_back(packet_info);
          if (packet.received())
            history_s_.erase(it);
        }
      }
    }
  }

  rtc::CritScope cs(&observers_lock_);
  for (auto& observer : observers_) {
    std::vector<StreamFeedbackObserver::StreamPacketInfo> selected_feedback;
    for (const auto& packet_info : stream_feedbacks) {
      if (absl::c_count(observer.first, packet_info.ssrc) > 0) {
        selected_feedback.push_back(packet_info);
      }
    }
    if (!selected_feedback.empty()) {
      observer.second->OnPacketFeedbackVector(std::move(selected_feedback),pathid);
    }
  }
}

}  // namespace webrtc
