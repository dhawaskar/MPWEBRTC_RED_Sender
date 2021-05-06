/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/congestion_controller/rtp/transport_feedback_adapter.h"

#include <stdlib.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "absl/algorithm/container.h"
#include "api/units/timestamp.h"
#include "modules/rtp_rtcp/include/rtp_rtcp_defines.h"
#include "modules/rtp_rtcp/source/rtcp_packet/transport_feedback.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "system_wrappers/include/field_trial.h"
#include "api/mp_collector.h"
#include "api/mp_global.h"


namespace webrtc {

constexpr TimeDelta kSendTimeHistoryWindow = TimeDelta::Seconds(300);
Clock* const clock_=Clock::GetRealTimeClock();

//constexpr int64_t kMpSendTimeHistoryWindow = 120;

void InFlightBytesTracker::AddInFlightPacketBytes(
    const PacketFeedback& packet,int pathid) {
  RTC_DCHECK(packet.sent.send_time.IsFinite());
  if(pathid!=2){
    auto it = in_flight_data_p_.find(packet.network_route);
    if (it != in_flight_data_p_.end()) {
      it->second += packet.sent.size;
    } else {
      in_flight_data_p_.insert({packet.network_route, packet.sent.size});
    }
  }else{
    auto it = in_flight_data_s_.find(packet.network_route);
    if (it != in_flight_data_s_.end()) {
      it->second += packet.sent.size;
    } else {
      in_flight_data_s_.insert({packet.network_route, packet.sent.size});
    }
  }
}

void InFlightBytesTracker::RemoveInFlightPacketBytes(
    const PacketFeedback& packet,int pathid) {
  if (packet.sent.send_time.IsInfinite())
    return;
  if(pathid!=2){
    auto it = in_flight_data_p_.find(packet.network_route);
    if (it != in_flight_data_p_.end()) {
      RTC_DCHECK_GE(it->second, packet.sent.size);
      it->second -= packet.sent.size;
      if (it->second.IsZero())
        in_flight_data_p_.erase(it);
    }
  }else{
    auto it = in_flight_data_s_.find(packet.network_route);
    if (it != in_flight_data_s_.end()) {
      RTC_DCHECK_GE(it->second, packet.sent.size);
      it->second -= packet.sent.size;
      if (it->second.IsZero())
        in_flight_data_s_.erase(it);
    }
  }
  
}

DataSize InFlightBytesTracker::GetOutstandingData(
    const rtc::NetworkRoute& network_route,int pathid) const {

  if(pathid!=2){
    auto it = in_flight_data_p_.find(network_route);
    if (it != in_flight_data_p_.end()) {
      return it->second;
    }
    else 
      return DataSize::Zero();
  }else if(pathid==2){
    auto it = in_flight_data_s_.find(network_route);
    if (it != in_flight_data_s_.end()) 
      return it->second;
    else 
      return DataSize::Zero();
  }
  return DataSize::Zero();
}

// Comparator for consistent map with NetworkRoute as key.
bool InFlightBytesTracker::NetworkRouteComparator::operator()(
    const rtc::NetworkRoute& a,
    const rtc::NetworkRoute& b) const {
  if (a.local.network_id() != b.local.network_id())
    return a.local.network_id() < b.local.network_id();
  if (a.remote.network_id() != b.remote.network_id())
    return a.remote.network_id() < b.remote.network_id();

  if (a.local.adapter_id() != b.local.adapter_id())
    return a.local.adapter_id() < b.local.adapter_id();
  if (a.remote.adapter_id() != b.remote.adapter_id())
    return a.remote.adapter_id() < b.remote.adapter_id();

  if (a.local.uses_turn() != b.local.uses_turn())
    return a.local.uses_turn() < b.local.uses_turn();
  if (a.remote.uses_turn() != b.remote.uses_turn())
    return a.remote.uses_turn() < b.remote.uses_turn();

  return a.connected < b.connected;
}

TransportFeedbackAdapter::TransportFeedbackAdapter() = default;


void TransportFeedbackAdapter::AddPacket(const RtpPacketSendInfo& packet_info,
                                         size_t overhead_bytes,
                                         Timestamp creation_time) {
  PacketFeedback packet;
  packet.creation_time = creation_time;
  packet.sent.sequence_number =
      seq_num_unwrapper_.Unwrap(packet_info.transport_sequence_number);

  packet.sent.mp_sequence_number =
      seq_num_unwrapper_.Unwrap(packet_info.mp_transport_sequence_number);//sandy: Mp-WebRTC
  packet.sent.pathid=packet_info.pathid;
  packet.sent.size = DataSize::Bytes(packet_info.length + overhead_bytes);
  packet.sent.audio = packet_info.packet_type == RtpPacketMediaType::kAudio;
  packet.network_route = network_route_;
  packet.sent.pacing_info = packet_info.pacing_info;

  // RTC_LOG(INFO)<<"sandystats: packet added now to history pathid= "<<packet.sent.pathid<< 
  // " mp transport seq= "<<packet.sent.mp_sequence_number<<" transport seq= "<< 
  // packet.sent.sequence_number<<" last acknowledged p= "<<last_ack_seq_num_p_<<" s= "<<last_ack_seq_num_s_;

  // sandy: If the pathid is -1 means no paths are set yet.
  RTC_DCHECK(packet.sent.pathid>0);
  if(packet.sent.pathid!=2){
      while (!history_p_.empty() && 
        creation_time - history_p_.begin()->second.creation_time > kSendTimeHistoryWindow) {
            if (history_p_.begin()->second.sent.mp_sequence_number > last_ack_seq_num_p_){
              in_flight_.RemoveInFlightPacketBytes(history_p_.begin()->second,packet.sent.pathid);
              history_p_.erase(history_p_.begin());
            }else{
              break;
            }
            
      }
      history_p_.insert(std::make_pair(packet.sent.mp_sequence_number, packet));
      seq_mp_map_p_.insert(std::make_pair(packet.sent.sequence_number,packet.sent.mp_sequence_number));
      RTC_DLOG(LS_ERROR)<<"sandy adding the packet to primary packet history seq= "<<packet.sent.sequence_number<< 
      " mp_seq= "<<packet.sent.mp_sequence_number<<" path= "<<packet.sent.pathid;
  }else if(packet.sent.pathid==2){
      while (!history_s_.empty() && creation_time - history_s_.begin()->second.creation_time > kSendTimeHistoryWindow) {
            if (history_s_.begin()->second.sent.mp_sequence_number > last_ack_seq_num_s_){
              in_flight_.RemoveInFlightPacketBytes(history_s_.begin()->second,packet.sent.pathid);
              history_s_.erase(history_s_.begin());
            }else{
              break;
            }
            
      }
      history_s_.insert(std::make_pair(packet.sent.mp_sequence_number, packet));
      seq_mp_map_s_.insert(std::make_pair(packet.sent.sequence_number,packet.sent.mp_sequence_number));
      RTC_DLOG(LS_ERROR)<<"sandy adding the packet to secondary packet history seq= "<<packet.sent.sequence_number<< 
      " mp_seq= "<<packet.sent.mp_sequence_number<<" path= "<<packet.sent.pathid;
  }
}

absl::optional<SentPacket> TransportFeedbackAdapter::ProcessSentPacket(
    const rtc::SentPacket& sent_packet) {

  // RTC_LOG(INFO)<<"sandystats: packet sent now transport seq= "<< 
  // sent_packet.packet_id<<"\n";

  auto send_time = Timestamp::Millis(sent_packet.send_time_ms);
  
  int64_t unwrapped_seq_num=0;
  int pathid=-1;

  // if(sent_packet.pathid_>0 && sent_packet.packet_id != -1 && false){//sandy: When the contorller is placed this field might not be set
  //   pathid=sent_packet.pathid_;
  //   unwrapped_seq_num=seq_num_unwrapper_.Unwrap(sent_packet.packet_mpid_);
  // }else{
    //sandy:The 510 needs to be less than feedback window in remote_estimator_proxy.cc;if (periodic_window_start_seq_s_ && packet_arrival_times_s_.size() > 1000)
  // if(seq_mp_map_s_.size()>110){
  //   while(seq_mp_map_s_.size()>110)
  //    seq_mp_map_s_.erase(seq_mp_map_s_.begin());
  // }
  // if(seq_mp_map_p_.size()>110){
  //   while(seq_mp_map_p_.size()>110)
  //     seq_mp_map_p_.erase(seq_mp_map_p_.begin());
  // }
    
  if(sent_packet.packet_id != -1){
    auto its=seq_mp_map_s_.find(seq_num_unwrapper_.Unwrap(sent_packet.packet_id));
    auto itp=seq_mp_map_p_.find(seq_num_unwrapper_.Unwrap(sent_packet.packet_id));
      
    if(itp!=seq_mp_map_p_.end()){
      unwrapped_seq_num = itp->second;
      pathid=1;
      seq_mp_map_p_.erase(itp);
    }else if(its!=seq_mp_map_s_.end()){
      unwrapped_seq_num = its->second;
      pathid=2;
      seq_mp_map_s_.erase(its);
    }else{  
      RTC_LOG(INFO)<<"sandystats coult not fing map for packet id= "<<seq_num_unwrapper_.Unwrap(sent_packet.packet_id)<<"primary mp seq= "<<itp->second 
      <<"secondary mp seq= "<<its->second;
      RTC_DCHECK(pathid>0);  
    }
  }else{
    pathid=1;
  }
  
  if(pathid==1 ){//sandy: Mp-WebRTC
        if (sent_packet.packet_id != -1) {
          // int64_t unwrapped_seq_num =
          //     seq_num_unwrapper_.Unwrap(sent_packet.packet_mpid_);
          auto it = history_p_.find(unwrapped_seq_num);
          if (it != history_p_.end()) {
            bool packet_retransmit = it->second.sent.send_time.IsFinite();
            it->second.sent.send_time = send_time;
        //     RTC_DLOG(LS_ERROR)<<"sandy setting the packet time to primary packet history seq= "<<sent_packet.packet_id<< 
        // " mp_seq= "<<unwrapped_seq_num<<":"<<it->second.sent.mp_sequence_number<<" path= "<<pathid<< 
        // ":"<<it->second.sent.pathid<<" route "<<it->second.network_route.DebugString()<<" inflight size: "<<in_flight_.in_flight_data_p_.size();
            last_send_time_p_ = std::max(last_send_time_p_, send_time);
            // TODO(srte): Don't do this on retransmit.
            if (!pending_untracked_size_p_.IsZero()) {
              if (send_time < last_untracked_send_time_p_)
                RTC_LOG(LS_WARNING)
                    << "appending acknowledged data for out of order packet. (Diff: "
                    << ToString(last_untracked_send_time_p_ - send_time) << " ms.)";
              it->second.sent.prior_unacked_data += pending_untracked_size_p_;
              pending_untracked_size_p_ = DataSize::Zero();
            }
            if (!packet_retransmit) {
              if (it->second.sent.mp_sequence_number > last_ack_seq_num_p_){
  //               RTC_LOG(INFO)<<"sandystats: packet added now to in flight pathid= "<<it->second.sent.pathid<< 
  // " mp transport seq= "<<it->second.sent.mp_sequence_number<<" transport seq= "<< 
  // it->second.sent.sequence_number<<" last acknowledged p= "<<last_ack_seq_num_p_<<" s= "<<last_ack_seq_num_s_;
                in_flight_.AddInFlightPacketBytes(it->second,1);
              }
              it->second.sent.data_in_flight = GetOutstandingData(1);//primary pathid 1
              return it->second.sent;
            }
          }else{
            RTC_DCHECK(1==0);
          }
      } else if (sent_packet.info.included_in_allocation) {
        if (send_time < last_send_time_p_) {
          RTC_LOG(LS_WARNING) << "ignoring untracked data for out of order packet.";
        }
        pending_untracked_size_p_ +=
            DataSize::Bytes(sent_packet.info.packet_size_bytes);
        last_untracked_send_time_p_ = std::max(last_untracked_send_time_p_, send_time);
      }
      return absl::nullopt;
  }else if(pathid==2){//sandy: Mp-WebRTC
      if (sent_packet.packet_id != -1) {
        // int64_t unwrapped_seq_num =
        //     seq_num_unwrapper_.Unwrap(sent_packet.packet_mpid_);
        auto it = history_s_.find(unwrapped_seq_num);
        if (it != history_s_.end()) {
          bool packet_retransmit = it->second.sent.send_time.IsFinite();
          it->second.sent.send_time = send_time;
      //     RTC_DLOG(LS_ERROR)<<"sandy adding the packet time to secondary packet history seq= "<<sent_packet.packet_id<< 
      // " mp_seq= "<<unwrapped_seq_num<<":"<<it->second.sent.mp_sequence_number<<" path= "<<pathid<< 
      //   ":"<<it->second.sent.pathid<<" route "<<it->second.network_route.DebugString()<<" inflight size: " 
      //   <<in_flight_.in_flight_data_s_.size()<<" time "<<clock_->TimeInMilliseconds();
          last_send_time_s_ = std::max(last_send_time_s_, send_time);
          // TODO(srte): Don't do this on retransmit.
          if (!pending_untracked_size_s_.IsZero()) {
            if (send_time < last_untracked_send_time_s_)
              RTC_LOG(LS_WARNING)
                  << "appending acknowledged data for out of order packet. (Diff: "
                  << ToString(last_untracked_send_time_s_ - send_time) << " ms.)";
            it->second.sent.prior_unacked_data += pending_untracked_size_s_;
            pending_untracked_size_s_ = DataSize::Zero();
          }
          if (!packet_retransmit) {
            if (it->second.sent.mp_sequence_number > last_ack_seq_num_s_){
  //             RTC_LOG(INFO)<<"sandystats: packet added now to in flight pathid= "<<it->second.sent.pathid<< 
  // " mp transport seq= "<<it->second.sent.mp_sequence_number<<" transport seq= "<< 
  // it->second.sent.sequence_number<<" last acknowledged p= "<<last_ack_seq_num_p_<<" s= "<<last_ack_seq_num_s_;
              in_flight_.AddInFlightPacketBytes(it->second,2);
            }
            it->second.sent.data_in_flight = GetOutstandingData(2);//primary pathid 1
            return it->second.sent;
          }
        }else{
            RTC_DCHECK(1==0);
        }
      } else if (sent_packet.info.included_in_allocation) {
        if (send_time < last_send_time_s_) {
          RTC_LOG(LS_WARNING) << "ignoring untracked data for out of order packet.";
        }
        pending_untracked_size_s_ +=
            DataSize::Bytes(sent_packet.info.packet_size_bytes);
        last_untracked_send_time_s_ = std::max(last_untracked_send_time_s_, send_time);
      }
      return absl::nullopt;
  }
  return absl::nullopt;
}

absl::optional<TransportPacketsFeedback>
TransportFeedbackAdapter::ProcessTransportFeedback(
    const rtcp::TransportFeedback& feedback,
    Timestamp feedback_receive_time) {

  // RTC_DLOG(LS_ERROR)<<"sandyrtcp path id of RTCP packet received\t"<<feedback.pathid()<<"\n";
  if (feedback.GetPacketStatusCount() == 0) {
    RTC_DLOG(LS_ERROR) << "sandytferror Empty transport feedback packet received.";
    return absl::nullopt;
  }

  TransportPacketsFeedback msg;
  if(feedback.pathid()!=2){//sandy:Mp-WebRTC primary path
      msg.feedback_time = feedback_receive_time;
      msg.prior_in_flight = in_flight_.GetOutstandingData(network_route_,1);
      msg.packet_feedbacks =ProcessTransportFeedbackInnerPrimary(feedback, feedback_receive_time);
      if (msg.packet_feedbacks.empty()){
        RTC_LOG(INFO)<<"sandytferror no feedback for primary path\n";
        return absl::nullopt;
      }

      auto it = history_p_.find(last_ack_seq_num_p_);
      if (it != history_p_.end()) {
        msg.first_unacked_send_time = it->second.sent.send_time;
      }
      msg.data_in_flight = in_flight_.GetOutstandingData(network_route_,1);
  }else if(feedback.pathid()==2){//sandy:Mp-WebRTC seconadry path
      msg.feedback_time = feedback_receive_time;
      msg.prior_in_flight = in_flight_.GetOutstandingData(network_route_,2);
      msg.packet_feedbacks =ProcessTransportFeedbackInnerSecondary(feedback, feedback_receive_time);
      if (msg.packet_feedbacks.empty()){
        RTC_LOG(INFO)<<"sandytferror no feedback for secondary path\n";
        return absl::nullopt;
      }

      auto it = history_s_.find(last_ack_seq_num_s_);
      if (it != history_s_.end()) {
        msg.first_unacked_send_time = it->second.sent.send_time;
      }
      msg.data_in_flight = in_flight_.GetOutstandingData(network_route_,2);
  }
  // RTC_DLOG(LS_ERROR)<<"sandy: received feedback count="<<feedback.GetPacketStatusCount()<<" sending to GCC "<<
  msg.packet_feedbacks.size();
  return msg;
}

void TransportFeedbackAdapter::SetNetworkRoute(
    const rtc::NetworkRoute& network_route) {
  RTC_DLOG(LS_ERROR)<<"sandynetwork route signal "<<network_route_.DebugString();
  network_route_ = network_route;
}

DataSize TransportFeedbackAdapter::GetOutstandingData(int pathid) const {
  if(pathid==1)
    return in_flight_.GetOutstandingData(network_route_,pathid);
  else
    return in_flight_.GetOutstandingData(network_route_,pathid);
}
//sandy: Adding the feed back packet : sandy-MpWebRTC
std::vector<PacketResult>
TransportFeedbackAdapter::ProcessTransportFeedbackInnerPrimary(
    const rtcp::TransportFeedback& feedback,
    Timestamp feedback_receive_time) {
  // Add timestamp deltas to a local time base selected on first packet arrival.
  // This won't be the true time base, but makes it easier to manually inspect
  // time stamps.

  // RTC_LOG(INFO)<<"sandyrtcp path id of RTCP packet received\t"<<feedback.pathid()<<"\n";
  if (last_timestamp_p_.IsInfinite()) {
    current_offset_p_ = feedback_receive_time;
  } else {
    // TODO(srte): We shouldn't need to do rounding here.
    const TimeDelta delta = feedback.GetBaseDelta(last_timestamp_p_)
                                .RoundDownTo(TimeDelta::Millis(1));
    // Protect against assigning current_offset_ negative value.
    if (delta < Timestamp::Zero() - current_offset_p_) {
      RTC_LOG(LS_WARNING) << "Unexpected feedback timestamp received.";
      current_offset_p_ = feedback_receive_time;
    } else {
      current_offset_p_ += delta;
    }
  }
  last_timestamp_p_ = feedback.GetBaseTime();

  std::vector<PacketResult> packet_result_vector;
  packet_result_vector.reserve(feedback.GetPacketStatusCount());

  size_t failed_lookups = 0;
  size_t ignored = 0;
  TimeDelta packet_offset = TimeDelta::Zero();
  for (const auto& packet : feedback.GetAllPackets()) {
    int64_t seq_num = seq_num_unwrapper_.Unwrap(packet.sequence_number());
    // RTC_DLOG(LS_ERROR)<<"sandyrtcp primary path base mp_transport_sequence_number "<<seq_num<<"\n";
    if (seq_num > last_ack_seq_num_p_) {
      // Starts at history_.begin() if last_ack_seq_num_ < 0, since any valid
      // sequence number is >= 0.
      for (auto it = history_p_.upper_bound(last_ack_seq_num_p_);
           it != history_p_.upper_bound(seq_num); ++it) {
        in_flight_.RemoveInFlightPacketBytes(it->second,1);
      }
      last_ack_seq_num_p_ = seq_num;
    }

    auto it = history_p_.find(seq_num);
    if (it == history_p_.end()) {
      RTC_DLOG(LS_ERROR)<<"sandytferror: not able to find the sent packet after ACK primary "<<seq_num;
      ++failed_lookups;
      continue;
    }

    if (it->second.sent.send_time.IsInfinite()) {
      // TODO(srte): Fix the tests that makes this happen and make this a
      // DCHECK.
      RTC_DLOG(LS_ERROR)
          << "sandytferror Received feedback before packet was indicated as sent primary";
      RTC_DLOG(LS_ERROR)<<"sandytferror received feedback before packet sent primary seq= "<<packet.sequence_number()<< 
      " mp_seq= "<<packet.sequence_number()<<" path= "<<1;

      continue;
    }

    PacketFeedback packet_feedback = it->second;
    if (packet.received()) {
      packet_offset += packet.delta();
      //sandy: Receive time is set
      packet_feedback.receive_time =
          current_offset_p_ + packet_offset.RoundDownTo(TimeDelta::Millis(1));
      // Note: Lost packets are not removed from history because they might be
      // reported as received by a later feedback.
      history_p_.erase(it);
    }else{
      RTC_DLOG(LS_ERROR)<<"sandytferror: primary is not received";
    }
    if (packet_feedback.network_route == network_route_) {
      PacketResult result;
      //sandy: Sent packet is set
      result.sent_packet = packet_feedback.sent;
      // RTC_DLOG(LS_ERROR)<<"sandy the feedback vector path id of packet "<<result.sent_packet.pathid<<" mp sequence "<<result.sent_packet.mp_sequence_number<<"\n";
      result.receive_time = packet_feedback.receive_time;
      packet_result_vector.push_back(result);
    } else {
      RTC_DLOG(LS_ERROR)<<"sandytferror: you do not have any receiver report";
      ++ignored;
    }
  }

  if (failed_lookups > 0) {
    RTC_DLOG(LS_ERROR) << "sandytferror Failed to lookup send time for " << failed_lookups
                        << " packet" << (failed_lookups > 1 ? "s" : "")
                        << ". Send time history too small?";
  }
  if (ignored > 0) {
    RTC_DLOG(LS_ERROR) << "sandytferror Ignoring " << ignored
                     << " packets because they were sent on a different route.";
  }

  return packet_result_vector;
}


//sandy:Mp-WebRTC for secondary path
std::vector<PacketResult>
TransportFeedbackAdapter::ProcessTransportFeedbackInnerSecondary(
    const rtcp::TransportFeedback& feedback,
    Timestamp feedback_receive_time) {
  // Add timestamp deltas to a local time base selected on first packet arrival.
  // This won't be the true time base, but makes it easier to manually inspect
  // time stamps.

  


  // RTC_LOG(INFO)<<"sandyrtcp path id of RTCP packet received\t"<<feedback.pathid()<<"\n";
  if (last_timestamp_s_.IsInfinite()) {
    current_offset_s_ = feedback_receive_time;
  } else {
    // TODO(srte): We shouldn't need to do rounding here.
    const TimeDelta delta = feedback.GetBaseDelta(last_timestamp_s_)
                                .RoundDownTo(TimeDelta::Millis(1));
    // Protect against assigning current_offset_ negative value.
    if (delta < Timestamp::Zero() - current_offset_s_) {
      RTC_DLOG(LS_ERROR) << "Unexpected feedback timestamp received.";
      current_offset_s_ = feedback_receive_time;
    } else {
      current_offset_s_ += delta;
    }
  }
  last_timestamp_s_ = feedback.GetBaseTime();

  std::vector<PacketResult> packet_result_vector;
  packet_result_vector.reserve(feedback.GetPacketStatusCount());

  size_t failed_lookups = 0;
  size_t ignored = 0;
  TimeDelta packet_offset = TimeDelta::Zero();
  for (const auto& packet : feedback.GetAllPackets()) {
    int64_t seq_num = seq_num_unwrapper_.Unwrap(packet.sequence_number());
    // RTC_DLOG(LS_ERROR)<<"sandyrtcp secondary path base mp_transport_sequence_number "<<seq_num<<"\n";
    if (seq_num > last_ack_seq_num_s_) {
      // Starts at history_.begin() if last_ack_seq_num_ < 0, since any valid
      // sequence number is >= 0.
      for (auto it = history_s_.upper_bound(last_ack_seq_num_s_);
           it != history_s_.upper_bound(seq_num); ++it) {
        in_flight_.RemoveInFlightPacketBytes(it->second,2);
      }
      last_ack_seq_num_s_ = seq_num;
    }

    auto it = history_s_.find(seq_num);
    if (it == history_s_.end()) {
      RTC_DLOG(LS_ERROR)<<"sandytferror: not able to find the sent packet after ACK secondary "<<seq_num;
      ++failed_lookups;
      continue;
    }

    if (it->second.sent.send_time.IsInfinite()) {
      // TODO(srte): Fix the tests that makes this happen and make this a
      // DCHECK.
      RTC_DLOG(LS_ERROR)
          << "sandytferror Received feedback before packet was indicated as sent secondary";
      RTC_DLOG(LS_ERROR)<<"sandytferror received feedback before packet sent secondary seq= "<<packet.sequence_number()<< 
      " mp_seq= "<<packet.sequence_number()<<" path= "<<2<<" time "<<clock_->TimeInMilliseconds();

      continue;
    }

    PacketFeedback packet_feedback = it->second;
    if (packet.received()) {
      packet_offset += packet.delta();
      packet_feedback.receive_time =
          current_offset_s_ + packet_offset.RoundDownTo(TimeDelta::Millis(1));
      // Note: Lost packets are not removed from history because they might be
      // reported as received by a later feedback.
      history_s_.erase(it);
    }else{
      RTC_DLOG(LS_ERROR)<<"sandytferror second path not received";
    }
    if (packet_feedback.network_route == network_route_) {
      PacketResult result;
      result.sent_packet = packet_feedback.sent;
      // RTC_DLOG(LS_ERROR)<<"sandy the feedback vector path id of packet "<<result.sent_packet.pathid<<" mp sequence "<<result.sent_packet.mp_sequence_number<<"\n";
      result.receive_time = packet_feedback.receive_time;
      packet_result_vector.push_back(result);
    } else {
      ++ignored;
    }
  }

  if (failed_lookups > 0) {
    RTC_LOG(LS_ERROR) << "sandytferror Failed to lookup send time for " << failed_lookups
                        << " packet" << (failed_lookups > 1 ? "s" : "")
                        << ". Send time history too small?";
  }
  if (ignored > 0) {
    RTC_LOG(LS_INFO) << "sandytferror Ignoring " << ignored
                     << " packets because they were sent on a different route.";
  }

  return packet_result_vector; 
  
}
}  // namespace webrtc
