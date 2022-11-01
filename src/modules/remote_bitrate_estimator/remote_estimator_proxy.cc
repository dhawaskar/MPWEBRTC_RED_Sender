/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/remote_bitrate_estimator/remote_estimator_proxy.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

#include "modules/rtp_rtcp/source/rtcp_packet/transport_feedback.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/numerics/safe_minmax.h"
#include "system_wrappers/include/clock.h"

//int64_t packets_count=0,sandy_ptime=0;
namespace webrtc {

// Impossible to request feedback older than what can be represented by 15 bits.
// const int RemoteEstimatorProxy::kMaxNumberOfPackets = (1 << 15);
const int RemoteEstimatorProxy::kMaxNumberOfPackets = (1 << 15);//sandy: I added it

// The maximum allowed value for a timestamp in milliseconds. This is lower
// than the numerical limit since we often convert to microseconds.
static constexpr int64_t kMaxTimeMs =
    std::numeric_limits<int64_t>::max() / 1000;

RemoteEstimatorProxy::RemoteEstimatorProxy(
    Clock* clock,
    TransportFeedbackSenderInterface* feedback_sender,
    const WebRtcKeyValueConfig* key_value_config,
    NetworkStateEstimator* network_state_estimator)
    : clock_(clock),
      feedback_sender_(feedback_sender),
      send_config_(key_value_config),
      last_process_time_ms_(-1),
      last_process_time_ms_s_(-1),
      last_process_time_ms_p_(-1),
      network_state_estimator_(network_state_estimator),
      media_ssrc_(0),
      feedback_packet_count_p_(0),
      feedback_packet_count_s_(0),
      send_interval_ms_(send_config_.default_interval->ms()),//250ms
      send_periodic_feedback_(true),
      previous_abs_send_time_(0),
      abs_send_timestamp_(clock->CurrentTime()) {
  //sandy: Network state estimation is not computed in receiver and sent as the part of periodic feed back
  // RTC_LOG(INFO)
  //     << "sandytfb Maximum interval between transport feedback RTCP messages (ms): "
  //     << send_config_.max_interval->ms()<<"network_state_estimator "<<network_state_estimator<<"\n";
}

RemoteEstimatorProxy::~RemoteEstimatorProxy() {}

void RemoteEstimatorProxy::IncomingPacket(int64_t arrival_time_ms,
                                          size_t payload_size,
                                          const RTPHeader& header) {


  
  if (arrival_time_ms < 0 || arrival_time_ms > kMaxTimeMs) {
    // RTC_DLOG(LS_ERROR) << "sandyrtp Arrival time out of bounds: " << arrival_time_ms;
    return;
  }
  rtc::CritScope cs(&lock_);
  media_ssrc_ = header.ssrc;
  int64_t seq = 0;
  //sandy: MpWebRTC implementationd details
  //First simply send the packets to specific path and then implement the contents.
  int pathid=-1,subflow_seq=-1;
  int64_t seq_p = 0,seq_s=0;
  if(header.payloadType==50)RTC_LOG(INFO)<<"sandyasymmetry the packet path id="<<header.payloadType;
  if(header.extension.hassandy && header.extension.hasMpTransportSequenceNumber){
    //sandy: When packets from second path flow in primary it means second connection
    //was turned off and hence they flew in primary. But there is no accounting for those 
    //packets in primary path. Hence I will make them as second path. This can effect the primary
    //path delay time but it is okay
    if(header.extension.sandy==2){
      pathid=2;
    }else if(header.extension.sandy==1){
      pathid=1;
    }
    //sandy: Retranmission packets should be counted too
    else if(header.extension.sandy<=0||header.extension.sandy==4||header.extension.sandy==1){
      pathid=1;
    }else{
      pathid=2;
    }
    subflow_seq=unwrapper_.Unwrap(header.extension.mptransportSequenceNumber);
    // RTC_DLOG(LS_ERROR)<<"sandystats received RTP packet path id="<<pathid<<" mp Transport seq= "<<subflow_seq<<":"<< 
    // " Transport seq= "<<unwrapper_.Unwrap(header.extension.transportSequenceNumber)<<" mp subflow "<< 
    // header.extension.mpflowseqnum<<" seq= "<<header.sequenceNumber;
  }
  // int64_t sandy_ctime=clock_->TimeInMilliseconds();
  // packets_count++;
  // if(!sandy_ptime){
  //   sandy_ptime=sandy_ctime;
  // }else{
  //   if(sandy_ctime-sandy_ptime>=1000){//Check number of packets received per second.
  //     RTC_LOG(INFO)<<"sandy the total packets received "<<packets_count;
  //     packets_count=1;
  //     sandy_ptime=sandy_ctime;
  //   }
  // }

  //Sandy: Primary path info
  if(pathid!=2){
    seq_p=subflow_seq;
    if (send_periodic_feedback_) {
      if (periodic_window_start_seq_p_ &&
          packet_arrival_times_p_.lower_bound(*periodic_window_start_seq_p_) ==
              packet_arrival_times_p_.end()) {
        // Start new feedback packet, cull old packets.
        for (auto it = packet_arrival_times_p_.begin();
             it != packet_arrival_times_p_.end() && it->first < seq_p &&
             arrival_time_ms - it->second >= send_config_.back_window->ms();) {
          it = packet_arrival_times_p_.erase(it);
        }
      }
      if (!periodic_window_start_seq_p_ || seq_p < *periodic_window_start_seq_p_) {
        periodic_window_start_seq_p_ = seq_p;
      }
    }

    // We are only interested in the first time a packet is received.
    if (packet_arrival_times_p_.find(seq_p) != packet_arrival_times_p_.end()){
      RTC_DLOG(LS_ERROR)<<"sandytferror packet is already in time history";
      return;
    }

    packet_arrival_times_p_[seq_p] = arrival_time_ms;

    // Limit the range of sequence numbers to send feedback for.
    auto first_arrival_time_to_keep = packet_arrival_times_p_.lower_bound(
        packet_arrival_times_p_.rbegin()->first - kMaxNumberOfPackets);
    if (first_arrival_time_to_keep != packet_arrival_times_p_.begin()) {
      packet_arrival_times_p_.erase(packet_arrival_times_p_.begin(),
                                  first_arrival_time_to_keep);
      if (send_periodic_feedback_) {
        // |packet_arrival_times_| cannot be empty since we just added one
        // element and the last element is not deleted.
        RTC_DCHECK(!packet_arrival_times_p_.empty());
        periodic_window_start_seq_p_ = packet_arrival_times_p_.begin()->first;
      }
    }

    if (header.extension.feedback_request) {
      // Send feedback packet immediately.
      //RTC_LOG(INFO)<<"sandytfb : sending the feedback immediately\n";
      SendFeedbackOnRequest(seq_p, header.extension.feedback_request.value(),1);
    }

  }else if (pathid==2){
    seq_s=subflow_seq;
    if (send_periodic_feedback_) {
      if (periodic_window_start_seq_s_ &&
          packet_arrival_times_s_.lower_bound(*periodic_window_start_seq_s_) ==
              packet_arrival_times_s_.end()) {
        // Start new feedback packet, cull old packets.
        for (auto it = packet_arrival_times_s_.begin();
             it != packet_arrival_times_s_.end() && it->first < seq_s &&
             arrival_time_ms - it->second >= send_config_.back_window->ms();) {
          it = packet_arrival_times_s_.erase(it);
        }
      }
      if (!periodic_window_start_seq_s_ || seq_s < *periodic_window_start_seq_s_) {
        periodic_window_start_seq_s_ = seq_s;
      }
    }

    // We are only interested in the first time a packet is received.
    if (packet_arrival_times_s_.find(seq_s) != packet_arrival_times_s_.end())
      return;

    packet_arrival_times_s_[seq_s] = arrival_time_ms;

    // Limit the range of sequence numbers to send feedback for.
    auto first_arrival_time_to_keep = packet_arrival_times_s_.lower_bound(
        packet_arrival_times_s_.rbegin()->first - kMaxNumberOfPackets);
    if (first_arrival_time_to_keep != packet_arrival_times_s_.begin()) {
      packet_arrival_times_s_.erase(packet_arrival_times_s_.begin(),
                                  first_arrival_time_to_keep);
      if (send_periodic_feedback_) {
        // |packet_arrival_times_| cannot be empty since we just added one
        // element and the last element is not deleted.
        RTC_DCHECK(!packet_arrival_times_s_.empty());
        periodic_window_start_seq_s_ = packet_arrival_times_s_.begin()->first;
      }
    }

    if (header.extension.feedback_request) {
      // Send feedback packet immediately.
      //RTC_LOG(INFO)<<"sandytfb : sending the feedback immediately\n";
      SendFeedbackOnRequest(seq_s, header.extension.feedback_request.value(),2);
    }
  }
  //sandy: Secondary path info
  
  // if(network_state_estimator_){
  //   RTC_DLOG(INFO)<<"sandy the network_state_estimator_ is enabled";
  // }
  // if(header.extension.hasAbsoluteSendTime){
  //   RTC_DLOG(INFO)<<"sandy the header has abs_send_timestamp_s";
  // }
  if (network_state_estimator_ && header.extension.hasAbsoluteSendTime) {//sandy:Not enabled so don't worry

    //RTC_DLOG(INFO)<<"sandytfb network_state_estimator_ is part of feedback_packet\n";
    PacketResult packet_result;
    packet_result.receive_time = Timestamp::Millis(arrival_time_ms);
    // Ignore reordering of packets and assume they have approximately the same
    // send time.
    abs_send_timestamp_ += std::max(
        header.extension.GetAbsoluteSendTimeDelta(previous_abs_send_time_),
        TimeDelta::Millis(0));
    previous_abs_send_time_ = header.extension.absoluteSendTime;
    packet_result.sent_packet.send_time = abs_send_timestamp_;
    // TODO(webrtc:10742): Take IP header and transport overhead into account.
    packet_result.sent_packet.size =
        DataSize::Bytes(header.headerLength + payload_size);
    packet_result.sent_packet.sequence_number = seq;
    network_state_estimator_->OnReceivedPacket(packet_result);
  }
}

bool RemoteEstimatorProxy::LatestEstimate(std::vector<unsigned int>* ssrcs,
                                          unsigned int* bitrate_bps) const {
  return false;
}

int64_t RemoteEstimatorProxy::TimeUntilNextProcess() {
  rtc::CritScope cs(&lock_);
  if (!send_periodic_feedback_) {
    // Wait a day until next process.
    return 24 * 60 * 60 * 1000;
  } else if (last_process_time_ms_ != -1) {
    int64_t now = clock_->TimeInMilliseconds();
    if (now - last_process_time_ms_ < send_interval_ms_)
      return last_process_time_ms_ + send_interval_ms_ - now;
  }
  return 0;
}

void RemoteEstimatorProxy::Process() {
  rtc::CritScope cs(&lock_);
  if (!send_periodic_feedback_) {
    return;
  }
  last_process_time_ms_ = clock_->TimeInMilliseconds();

  SendPeriodicFeedbacks();
}

void RemoteEstimatorProxy::OnBitrateChanged(int bitrate_bps) {

  // TwccReportSize = Ipv4(20B) + UDP(8B) + SRTP(10B) +
  // AverageTwccReport(30B)
  // TwccReport size at 50ms interval is 24 byte.
  // TwccReport size at 250ms interval is 36 byte.
  // AverageTwccReport = (TwccReport(50ms) + TwccReport(250ms)) / 2
  constexpr int kTwccReportSize = 20 + 8 + 10 + 30;//sandy: Changing from 30 to 32
  const double kMinTwccRate =
      kTwccReportSize * 8.0 * 1000.0 / send_config_.max_interval->ms();
  const double kMaxTwccRate =
      kTwccReportSize * 8.0 * 1000.0 / send_config_.min_interval->ms();

  // Let TWCC reports occupy 5% of total bandwidth.
  rtc::CritScope cs(&lock_);
  send_interval_ms_ = static_cast<int>(
      0.5 + kTwccReportSize * 8.0 * 1000.0 /
                rtc::SafeClamp(send_config_.bandwidth_fraction * bitrate_bps,
                               kMinTwccRate, kMaxTwccRate));
  //sandy I added this. Since bitrate update is summation of both path, interval could be too small and hence I increase it by 2
  //send_interval_ms_*=2;
  //RTC_LOG(INFO)<<" sandy the interval for sending the Transport feedback "<<send_interval_ms_;
}

void RemoteEstimatorProxy::SetSendPeriodicFeedback(
    bool send_periodic_feedback) {
  rtc::CritScope cs(&lock_);
  send_periodic_feedback_ = send_periodic_feedback;
}

void RemoteEstimatorProxy::SendPeriodicFeedbacks() {
 
  if (!periodic_window_start_seq_p_ && !periodic_window_start_seq_s_){
    // RTC_DLOG(LS_ERROR)<<"sandytferror nothing to send the feedback";
    return;
  }

  //sandy: Below code is never called no need to worry
  std::unique_ptr<rtcp::RemoteEstimate> remote_estimate;
  if (network_state_estimator_) {
    absl::optional<NetworkStateEstimate> state_estimate =
        network_state_estimator_->GetCurrentEstimate();
    if (state_estimate) {
      remote_estimate = std::make_unique<rtcp::RemoteEstimate>();
      remote_estimate->SetEstimate(state_estimate.value());
    }
  }

  if(last_process_time_ms_p_<0){
    last_process_time_ms_p_=clock_->TimeInMilliseconds();
  }
  else{
    if(periodic_window_start_seq_p_){  
      //&& (clock_->TimeInMilliseconds()-last_process_time_ms_p_ >send_interval_ms_)){
      //RTC_DLOG(LS_ERROR)<<"sandytferror sending RTCP feedback Primary starting at "<< 
      *periodic_window_start_seq_p_;
      for (auto begin_iterator = packet_arrival_times_p_.lower_bound(*periodic_window_start_seq_p_);begin_iterator != packet_arrival_times_p_.cend(); 
         begin_iterator = packet_arrival_times_p_.lower_bound(*periodic_window_start_seq_p_)) { 
          auto feedback_packet = std::make_unique<rtcp::TransportFeedback>(1);//sandy: path id should be 1
          periodic_window_start_seq_p_ = BuildFeedbackPacket(
              feedback_packet_count_p_++, media_ssrc_, *periodic_window_start_seq_p_,
              begin_iterator, packet_arrival_times_p_.cend(), feedback_packet.get());

          RTC_DCHECK(feedback_sender_ != nullptr);
          std::vector<std::unique_ptr<rtcp::RtcpPacket>> packets;
          packets.push_back(std::move(feedback_packet));
          feedback_sender_->SendCombinedRtcpPacket(std::move(packets),1);//sandy
          // feedback_sender_->SendCombinedRtcpPacket(std::move(packets));//sandy
          //count++;
      }
      last_process_time_ms_p_=clock_->TimeInMilliseconds();
      //RTC_DLOG(LS_ERROR)<<"sandytferror sent RTCP feedback Primary ending at "<< *periodic_window_start_seq_p_;
    }
  }

  //sandy: Send the secondary path feedback
  if(last_process_time_ms_s_<0){
    last_process_time_ms_s_=clock_->TimeInMilliseconds();
  }else{
    if (periodic_window_start_seq_s_){    
    //&&(clock_->TimeInMilliseconds()-last_process_time_ms_s_ >send_interval_ms_)){//sandy: Secondary feedback count should be greater.
      // RTC_DLOG(LS_ERROR)<<"sandytferror sending RTCP feedback secondary";
      for (auto begin_iterator = packet_arrival_times_s_.lower_bound(*periodic_window_start_seq_s_);begin_iterator != packet_arrival_times_s_.cend(); 
         begin_iterator = packet_arrival_times_s_.lower_bound(*periodic_window_start_seq_s_)) { 
          auto feedback_packet = std::make_unique<rtcp::TransportFeedback>(2);//sandy: path id should be 2 as this is secondary path
          periodic_window_start_seq_s_ = BuildFeedbackPacket(
              feedback_packet_count_s_++, media_ssrc_, *periodic_window_start_seq_s_,
              begin_iterator, packet_arrival_times_s_.cend(), feedback_packet.get());

          RTC_DCHECK(feedback_sender_ != nullptr);
          std::vector<std::unique_ptr<rtcp::RtcpPacket>> packets;
          packets.push_back(std::move(feedback_packet));
          feedback_sender_->SendCombinedRtcpPacket(std::move(packets),2);//sandy
          // feedback_sender_->SendCombinedRtcpPacket(std::move(packets));//sandy
      }
      last_process_time_ms_s_=clock_->TimeInMilliseconds();
    }
  }
}

void RemoteEstimatorProxy::SendFeedbackOnRequest(
    int64_t sequence_number,
    const FeedbackRequest& feedback_request,int pathid) {

  
  if (feedback_request.sequence_count == 0) {
    return;
  }

  if(pathid!=2){
    //sandy: Below is the function that create pointer to the transport feed back
    //RTC_DLOG(LS_ERROR)<<"sandy received dynamic request Primary";
    auto feedback_packet_p = std::make_unique<rtcp::TransportFeedback>(
      feedback_request.include_timestamps,pathid);
    int64_t first_sequence_number = sequence_number - feedback_request.sequence_count + 1;
    auto begin_iterator = packet_arrival_times_p_.lower_bound(first_sequence_number);
    auto end_iterator = packet_arrival_times_p_.upper_bound(sequence_number);

    BuildFeedbackPacket(feedback_packet_count_p_++, media_ssrc_,
                        first_sequence_number, begin_iterator, end_iterator,
                        feedback_packet_p.get());

    // Clear up to the first packet that is included in this feedback packet.
    packet_arrival_times_p_.erase(packet_arrival_times_p_.begin(), begin_iterator);

    RTC_DCHECK(feedback_sender_ != nullptr);
    std::vector<std::unique_ptr<rtcp::RtcpPacket>> packets;
    packets.push_back(std::move(feedback_packet_p));
    feedback_sender_->SendCombinedRtcpPacket(std::move(packets),1);
    // feedback_sender_->SendCombinedRtcpPacket(std::move(packets));
    return;
  }else if(pathid==2){
    //sandy: Below is the function that create pointer to the transport feed back
    //RTC_DLOG(LS_ERROR)<<"sandyt received dynamic request secondary";
    auto feedback_packet_s = std::make_unique<rtcp::TransportFeedback>(
      feedback_request.include_timestamps,pathid);
    int64_t first_sequence_number = sequence_number - feedback_request.sequence_count + 1;
    auto begin_iterator = packet_arrival_times_s_.lower_bound(first_sequence_number);
    auto end_iterator = packet_arrival_times_s_.upper_bound(sequence_number);

    BuildFeedbackPacket(feedback_packet_count_s_++, media_ssrc_,
                        first_sequence_number, begin_iterator, end_iterator,
                        feedback_packet_s.get());

    // Clear up to the first packet that is included in this feedback packet.
    packet_arrival_times_s_.erase(packet_arrival_times_s_.begin(), begin_iterator);

    RTC_DCHECK(feedback_sender_ != nullptr);
    std::vector<std::unique_ptr<rtcp::RtcpPacket>> packets;
    packets.push_back(std::move(feedback_packet_s));
    feedback_sender_->SendCombinedRtcpPacket(std::move(packets),2);//sandy
    // feedback_sender_->SendCombinedRtcpPacket(std::move(packets));

    return;
  }
}

int64_t RemoteEstimatorProxy::BuildFeedbackPacket(
    uint8_t feedback_packet_count,
    uint32_t media_ssrc,
    int64_t base_sequence_number,
    std::map<int64_t, int64_t>::const_iterator begin_iterator,
    std::map<int64_t, int64_t>::const_iterator end_iterator,
    rtcp::TransportFeedback* feedback_packet) {

  int count=0;
  //RTC_LOG(INFO)<<"sandytfb creating the feedback_packet\n";
  RTC_DCHECK(begin_iterator != end_iterator);

  // TODO(sprang): Measure receive times in microseconds and remove the
  // conversions below.
  feedback_packet->SetMediaSsrc(media_ssrc);
  // Base sequence number is the expected first sequence number. This is known,
  // but we might not have actually received it, so the base time shall be the
  // time of the first received packet in the feedback.
  feedback_packet->SetBase(static_cast<uint16_t>(base_sequence_number & 0xFFFF),
                           begin_iterator->second * 1000);
  feedback_packet->SetFeedbackSequenceNumber(feedback_packet_count);
  int64_t next_sequence_number = base_sequence_number;
  for (auto it = begin_iterator; it != end_iterator; ++it) {
    if (!feedback_packet->AddReceivedPacket(
            static_cast<uint16_t>(it->first & 0xFFFF), it->second * 1000)) {
      // If we can't even add the first seq to the feedback packet, we won't be
      // able to build it at all.
      RTC_CHECK(begin_iterator != it);//sandy: This should not be commented out but I have done it for higher bitrate anyway

      // Could not add timestamp, feedback packet might be full. Return and
      // try again with a fresh packet.
      break;
    }
    next_sequence_number = it->first + 1;
    count++;
  }
  //RTC_DLOG(LS_ERROR)<<"sandy total feedback sent= "<<count;
  return next_sequence_number;
}

}  // namespace webrtc
