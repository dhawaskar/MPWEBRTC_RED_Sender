/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_CONGESTION_CONTROLLER_RTP_TRANSPORT_FEEDBACK_ADAPTER_H_
#define MODULES_CONGESTION_CONTROLLER_RTP_TRANSPORT_FEEDBACK_ADAPTER_H_

#include <deque>
#include <map>
#include <utility>
#include <vector>

#include "api/transport/network_types.h"
#include "modules/include/module_common_types_public.h"
#include "modules/rtp_rtcp/include/rtp_rtcp_defines.h"
#include "rtc_base/critical_section.h"
#include "rtc_base/network/sent_packet.h"
#include "rtc_base/network_route.h"
#include "rtc_base/thread_annotations.h"
#include "rtc_base/thread_checker.h"

namespace webrtc {

struct PacketFeedback {
  PacketFeedback() = default;
  // Time corresponding to when this object was created.
  Timestamp creation_time = Timestamp::MinusInfinity();
  SentPacket sent;
  // Time corresponding to when the packet was received. Timestamped with the
  // receiver's clock. For unreceived packet, Timestamp::PlusInfinity() is
  // used.
  Timestamp receive_time = Timestamp::PlusInfinity();

  // The network route that this packet is associated with.
  rtc::NetworkRoute network_route;
};

class InFlightBytesTracker {
 public:
  void AddInFlightPacketBytes(const PacketFeedback& packet,int pathid);
  void RemoveInFlightPacketBytes(const PacketFeedback& packet,int pathid);
  DataSize GetOutstandingData(const rtc::NetworkRoute& network_route,int pathid) const;

 private:
  struct NetworkRouteComparator {
    bool operator()(const rtc::NetworkRoute& a,
                    const rtc::NetworkRoute& b) const;
  };
  //std::map<rtc::NetworkRoute, DataSize, NetworkRouteComparator> in_flight_data_;
public:
  std::map<rtc::NetworkRoute, DataSize, NetworkRouteComparator> in_flight_data_p_;//sandy:Mp-WebRTC
  std::map<rtc::NetworkRoute, DataSize, NetworkRouteComparator> in_flight_data_s_;//sandy:Mp-WebRTC
};

class TransportFeedbackAdapter {
 public:
  TransportFeedbackAdapter();

  void AddPacket(const RtpPacketSendInfo& packet_info,
                 size_t overhead_bytes,
                 Timestamp creation_time);
  absl::optional<SentPacket> ProcessSentPacket(
      const rtc::SentPacket& sent_packet);

  absl::optional<TransportPacketsFeedback> ProcessTransportFeedback(
      const rtcp::TransportFeedback& feedback,
      Timestamp feedback_receive_time);

  void SetNetworkRoute(const rtc::NetworkRoute& network_route);

  DataSize GetOutstandingData(int pathid) const;

 private:
  enum class SendTimeHistoryStatus { kNotAdded, kOk, kDuplicate };

  std::vector<PacketResult> ProcessTransportFeedbackInner(
      const rtcp::TransportFeedback& feedback,
      Timestamp feedback_receive_time);


  std::vector<PacketResult> ProcessTransportFeedbackInnerPrimary(//sandy:Mp-WebRTC
      const rtcp::TransportFeedback& feedback,
      Timestamp feedback_receive_time);
  std::vector<PacketResult> ProcessTransportFeedbackInnerSecondary(//sandy:Mp-WebRTC
      const rtcp::TransportFeedback& feedback,
      Timestamp feedback_receive_time);

  DataSize pending_untracked_size_ = DataSize::Zero();
  DataSize pending_untracked_size_p_ = DataSize::Zero();
  DataSize pending_untracked_size_s_ = DataSize::Zero();
  Timestamp last_send_time_ = Timestamp::MinusInfinity();
  Timestamp last_send_time_p_ = Timestamp::MinusInfinity();
  Timestamp last_send_time_s_ = Timestamp::MinusInfinity();
  Timestamp last_untracked_send_time_ = Timestamp::MinusInfinity();
  Timestamp last_untracked_send_time_p_ = Timestamp::MinusInfinity();
  Timestamp last_untracked_send_time_s_ = Timestamp::MinusInfinity();
  SequenceNumberUnwrapper seq_num_unwrapper_;
  SequenceNumberUnwrapper seq_num_unwrapper_p_;
  SequenceNumberUnwrapper seq_num_unwrapper_s_;
  //std::map<int64_t, PacketFeedback> history_;
  std::map<int64_t, PacketFeedback> history_p_;
  std::map<int64_t, PacketFeedback> history_s_;

  std::map<int64_t, int64_t> seq_mp_map_p_; //<seq,mp_seq,pathid>
  std::map<int64_t, int64_t> seq_mp_map_s_;
  // Sequence numbers are never negative, using -1 as it always < a real
  // sequence number.
  int64_t last_ack_seq_num_ = -1;
  int64_t last_ack_seq_num_p_=-1;
  int64_t last_ack_seq_num_s_=-1;
  InFlightBytesTracker in_flight_;
  //InFlightBytesTracker in_flight_p_;
  //InFlightBytesTracker in_flight_s_;

  Timestamp current_offset_ = Timestamp::MinusInfinity();
  Timestamp current_offset_p_ = Timestamp::MinusInfinity();//sandy
  Timestamp current_offset_s_ = Timestamp::MinusInfinity();//sandy
  TimeDelta last_timestamp_ = TimeDelta::MinusInfinity();
  TimeDelta last_timestamp_p_ = TimeDelta::MinusInfinity();//sandy
  TimeDelta last_timestamp_s_ = TimeDelta::MinusInfinity();//sandy

  rtc::NetworkRoute network_route_;

  int64_t history_time_p=0;
  int64_t history_time_s=0;
};

}  // namespace webrtc

#endif  // MODULES_CONGESTION_CONTROLLER_RTP_TRANSPORT_FEEDBACK_ADAPTER_H_
