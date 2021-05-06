/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/rtp_rtcp/source/receive_statistics_impl.h"

#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

#include "modules/remote_bitrate_estimator/test/bwe_test_logging.h"
#include "modules/rtp_rtcp/source/rtp_packet_received.h"
#include "modules/rtp_rtcp/source/rtp_rtcp_config.h"
#include "modules/rtp_rtcp/source/time_util.h"
#include "rtc_base/logging.h"
#include "system_wrappers/include/clock.h"
#include "api/mp_collector.h"
#include "api/mp_global.h"


int64_t sequence_number_primary;
uint32_t rtcp_seq_p=0,rtcp_seq_s=0;
namespace webrtc {

const int64_t kStatisticsTimeoutMs = 8000;
const int64_t kStatisticsProcessIntervalMs = 1000; //sandy: 1000 original value
const int32_t kMpGapThreshold=0;//sandy: Since re-ordering is common we will not report loss unless it is >10

StreamStatistician::~StreamStatistician() {}

StreamStatisticianImpl::StreamStatisticianImpl(uint32_t ssrc,
                                               Clock* clock,
                                               int max_reordering_threshold)
    : ssrc_(ssrc),
      clock_(clock),
      incoming_bitrate_(kStatisticsProcessIntervalMs,
                        RateStatistics::kBpsScale),
      max_reordering_threshold_(max_reordering_threshold),
      enable_retransmit_detection_(false),
      jitter_q4_(0),
      jitter_q4_p(0),
      jitter_q4_s(0),
      cumulative_loss_(0),
      cumulative_loss_rtcp_offset_(0),
      cumulative_loss_rtcp_offset_p(0),
      cumulative_loss_rtcp_offset_s(0),
      last_receive_time_ms_(0),
      last_received_timestamp_(0),
      received_seq_first_(-1),
      received_seq_max_(-1),
      last_report_cumulative_loss_(0),
      last_report_seq_max_(-1),
      primary_seq(0),
      secondary_seq(0)
       {
        // RTC_LOG(INFO)<<"The max_reordering_threshold\t"<<max_reordering_threshold<<"\n";
        if(!mpcollector_){
          // RTC_LOG(INFO)<<"MpCollector pointer:created newly\n";
          mpcollector_=new MpCollector();
        }
        // RTC_LOG(INFO)<<"MpCollector pointer\t"<<mpcollector_<<"\n";
      }

StreamStatisticianImpl::~StreamStatisticianImpl() = default;

bool StreamStatisticianImpl::UpdateOutOfOrder(const RtpPacketReceived& packet,
                                              int64_t sequence_number,
                                              int64_t now_ms) {
  
  // Check if |packet| is second packet of a stream restart.
  if (received_seq_out_of_order_) {
    // Count the previous packet as a received; it was postponed below.
    --cumulative_loss_;

    uint16_t expected_sequence_number = *received_seq_out_of_order_ + 1;
    received_seq_out_of_order_ = absl::nullopt;
    if (packet.SequenceNumber() == expected_sequence_number) {
      // Ignore sequence number gap caused by stream restart for packet loss
      // calculation, by setting received_seq_max_ to the sequence number just
      // before the out-of-order seqno. This gives a net zero change of
      // |cumulative_loss_|, for the two packets interpreted as a stream reset.
      //
      // Fraction loss for the next report may get a bit off, since we don't
      // update last_report_seq_max_ and last_report_cumulative_loss_ in a
      // consistent way.
      last_report_seq_max_ = sequence_number - 2;
      received_seq_max_ = sequence_number - 2;
      return false;
    }
  }

  

  if (std::abs(sequence_number - received_seq_max_) >
      max_reordering_threshold_) {
    // Sequence number gap looks too large, wait until next packet to check
    // for a stream restart.
    received_seq_out_of_order_ = packet.SequenceNumber();
    // Postpone counting this as a received packet until we know how to update
    // |received_seq_max_|, otherwise we temporarily decrement
    // |cumulative_loss_|. The
    // ReceiveStatisticsTest.StreamRestartDoesntCountAsLoss test expects
    // |cumulative_loss_| to be unchanged by the reception of the first packet
    // after stream reset.
    ++cumulative_loss_;
    // RTC_LOG(INFO)<<"seqsandy: Received out of order\n";
    return true;
  }

  if (sequence_number > received_seq_max_)
    return false;

  // Old out of order packet, may be retransmit.
  if (enable_retransmit_detection_ && IsRetransmitOfOldPacket(packet, now_ms)){
    receive_counters_.retransmitted.AddPacket(packet);
    // RTC_LOG(INFO)<<"seqsandy: Received duplicate packet\n";
  }
  return true;
}

void StreamStatisticianImpl::UpdateCounters(const RtpPacketReceived& packet) {


  RTC_DCHECK(packet.get_pathid()>0);
  rtc::CritScope cs(&stream_lock_);
  // RTC_LOG(INFO)<<"sandy the received packet path="<<packet.subflow_id<<" seq= "<<packet.SequenceNumber()<<
  // " mp_seq= "<<packet.subflow_seq<<" payload type"<<packet.PayloadType()<<"\n";

  RTC_DCHECK_EQ(ssrc_, packet.Ssrc());
  int64_t now_ms = clock_->TimeInMilliseconds();
    
  // RTC_LOG(INFO)<<"MpFlowID Received packet subflow id and subflow seq:\t"<<packet.subflow_id<<" : "<<packet.subflow_seq<< 
  // "\t"<<received_seq_max_primary<<"\t"<<cumulative_loss_primary<<"\t"<<IsRetransmitOfOldPacket(packet, now_ms);

  incoming_bitrate_.Update(packet.size(), now_ms);
  receive_counters_.last_packet_received_timestamp_ms = now_ms;
  receive_counters_.transmitted.AddPacket(packet);
  --cumulative_loss_;
  
  int64_t sequence_number =
      seq_unwrapper_.UnwrapWithoutUpdate(packet.SequenceNumber());


  
  if(packet.subflow_id!=2){
    primary_seq=seq_unwrapper_.UnwrapWithoutUpdate(packet.subflow_seq);
    if(prev_primary_seq==0 && primary_seq>0){
      prev_primary_seq=seq_unwrapper_.UnwrapWithoutUpdate(packet.subflow_seq);
    }
  }else{
    secondary_seq=seq_unwrapper_.UnwrapWithoutUpdate(packet.subflow_seq);
    if(prev_secondary_seq==0 && secondary_seq >0){
      prev_secondary_seq=seq_unwrapper_.UnwrapWithoutUpdate(secondary_seq);
    }
  }
  //sandy
  if(packet.subflow_id==1){
    if(primary_seq>0 && prev_primary_seq >0 &&  primary_seq>prev_primary_seq){  
        if(received_seq_first_primary<0)
          received_seq_first_primary=primary_seq;
        if((primary_seq-prev_primary_seq-1)>kMpGapThreshold)
          cumulative_loss_primary+=(primary_seq-prev_primary_seq-1);
    }
    // RTC_LOG(INFO)<<"cumulative_loss_primary: "<<cumulative_loss_primary<<" seq "<< primary_seq <<" prev "<< prev_primary_seq<<"\t"<<sequence_number<<"\t"<<cumulative_loss_<<"\n";
    received_seq_max_primary=primary_seq;
    p_packets++;
    if(primary_seq>prev_primary_seq)
      prev_primary_seq=primary_seq;
    // else if(primary_seq<prev_primary_seq)
    //   prev_primary_seq++;
    //Jitter computation
  }
  else{
    if(secondary_seq>0 && prev_secondary_seq >0 && secondary_seq>prev_secondary_seq) { 
        if(received_seq_first_secondary<0)
          received_seq_first_secondary=secondary_seq;
        if((secondary_seq-prev_secondary_seq-1)>kMpGapThreshold)
        cumulative_loss_secondary+=(secondary_seq-prev_secondary_seq-1);
    }
    // RTC_LOG(INFO)<<"cumulative_loss_secondary: "<<cumulative_loss_secondary<<" seq "<< secondary_seq <<" prev "<< prev_secondary_seq<<"\t"<<sequence_number<<"\n";
    received_seq_max_secondary=secondary_seq;
    s_packets++;
    if(secondary_seq>prev_secondary_seq)
      prev_secondary_seq=secondary_seq;
    // else if(secondary_seq<prev_secondary_seq)
    //   prev_secondary_seq++;
  }
  
  if (!ReceivedRtpPacket()) {
    received_seq_first_ = sequence_number;
    last_report_seq_max_ = sequence_number - 1;
    received_seq_max_ = sequence_number - 1;
    receive_counters_.first_packet_time_ms = now_ms;
  } else if (UpdateOutOfOrder(packet, sequence_number, now_ms)) {
    return;
  }
  

  // In order packet.
  cumulative_loss_ += sequence_number - received_seq_max_;
  received_seq_max_ = sequence_number;

  seq_unwrapper_.UpdateLast(sequence_number);

  // If new time stamp and more than one in-order packet received, calculate
  // new jitter statistics.


  //if(packet.subflow_id==1|| packet.subflow_id!=2){
  if (packet.subflow_id==1 && packet.Timestamp() != last_received_timestamp_p){
    UpdateJitter_p(packet, now_ms);
    last_received_timestamp_p = packet.Timestamp();
    last_receive_time_ms_p = now_ms;
  }
  

  if (packet.subflow_id==2 && packet.Timestamp() != last_received_timestamp_s){
    UpdateJitter_s(packet, now_ms);
    last_received_timestamp_s = packet.Timestamp();
    last_receive_time_ms_s = now_ms;
  }
  

  if (packet.Timestamp() != last_received_timestamp_ &&
      (receive_counters_.transmitted.packets -
       receive_counters_.retransmitted.packets) > 1) {
    UpdateJitter(packet, now_ms);
  }
  last_received_timestamp_ = packet.Timestamp();
  last_receive_time_ms_ = now_ms;

  

  // RTC_LOG(INFO)<<"sandyjitter: The original primary and secondary "<<jitter_q4_<<" "<<jitter_q4_p<<" "<<(jitter_q4_s>>4)<<"\n";
}
void StreamStatisticianImpl::UpdateJitter_p(const RtpPacketReceived& packet,
                                          int64_t receive_time_ms) {
  int64_t receive_diff_ms = receive_time_ms - last_receive_time_ms_p;
  // RTC_LOG(INFO)<<"Last receive time\t"<<receive_time_ms<<":"<<last_receive_time_ms_p<<":"<<packet.Timestamp()<<":"<<last_received_timestamp_p<<"\n";
  RTC_DCHECK_GE(receive_diff_ms, 0);
  uint32_t receive_diff_rtp = static_cast<uint32_t>(
      (receive_diff_ms * packet.payload_type_frequency()) / 1000);
  int32_t time_diff_samples =
      receive_diff_rtp - (packet.Timestamp() - last_received_timestamp_p);

  time_diff_samples = std::abs(time_diff_samples);

  // lib_jingle sometimes deliver crazy jumps in TS for the same stream.
  // If this happens, don't update jitter value. Use 5 secs video frequency
  // as the threshold.
  if (time_diff_samples < 450000) {
    // Note we calculate in Q4 to avoid using float.
    int32_t jitter_diff_q4 = (time_diff_samples << 4) - jitter_q4_p;
    jitter_q4_p += ((jitter_diff_q4 + 8) >> 4);
  }
}
void StreamStatisticianImpl::UpdateJitter_s(const RtpPacketReceived& packet,
                                          int64_t receive_time_ms) {
  int64_t receive_diff_ms = receive_time_ms - last_receive_time_ms_s;
  RTC_DCHECK_GE(receive_diff_ms, 0);
  uint32_t receive_diff_rtp = static_cast<uint32_t>(
      (receive_diff_ms * packet.payload_type_frequency()) / 1000);
  int32_t time_diff_samples =
      receive_diff_rtp - (packet.Timestamp() - last_received_timestamp_s);

  time_diff_samples = std::abs(time_diff_samples);

  // lib_jingle sometimes deliver crazy jumps in TS for the same stream.
  // If this happens, don't update jitter value. Use 5 secs video frequency
  // as the threshold.
  if (time_diff_samples < 450000) {
    // Note we calculate in Q4 to avoid using float.
    int32_t jitter_diff_q4 = (time_diff_samples << 4) - jitter_q4_s;
    jitter_q4_s += ((jitter_diff_q4 + 8) >> 4);
  }
  
}

void StreamStatisticianImpl::UpdateJitter(const RtpPacketReceived& packet,
                                          int64_t receive_time_ms) {
  int64_t receive_diff_ms = receive_time_ms - last_receive_time_ms_;
  // RTC_LOG(INFO)<<"Last receive time\t"<<receive_time_ms<<":"<<last_receive_time_ms_<<":"<<packet.Timestamp()<<":"<<last_received_timestamp_<<"\n";
  RTC_DCHECK_GE(receive_diff_ms, 0);
  uint32_t receive_diff_rtp = static_cast<uint32_t>(
      (receive_diff_ms * packet.payload_type_frequency()) / 1000);
  int32_t time_diff_samples =
      receive_diff_rtp - (packet.Timestamp() - last_received_timestamp_);

  time_diff_samples = std::abs(time_diff_samples);

  // lib_jingle sometimes deliver crazy jumps in TS for the same stream.
  // If this happens, don't update jitter value. Use 5 secs video frequency
  // as the threshold.
  if (time_diff_samples < 450000) {
    // Note we calculate in Q4 to avoid using float.
    int32_t jitter_diff_q4 = (time_diff_samples << 4) - jitter_q4_;
    jitter_q4_ += ((jitter_diff_q4 + 8) >> 4);
  }
}

void StreamStatisticianImpl::SetMaxReorderingThreshold(
    int max_reordering_threshold) {
  rtc::CritScope cs(&stream_lock_);
  max_reordering_threshold_ = max_reordering_threshold;
}

void StreamStatisticianImpl::EnableRetransmitDetection(bool enable) {
  rtc::CritScope cs(&stream_lock_);
  enable_retransmit_detection_ = enable;
}

RtpReceiveStats StreamStatisticianImpl::GetStats() const {//sandy: yet to implement
  rtc::CritScope cs(&stream_lock_);
  RtpReceiveStats stats;
  stats.packets_lost = cumulative_loss_;
  // TODO(nisse): Can we return a float instead?
  // Note: internal jitter value is in Q4 and needs to be scaled by 1/16.
  stats.jitter = jitter_q4_ >> 4;
  stats.last_packet_received_timestamp_ms =
      receive_counters_.last_packet_received_timestamp_ms;
  stats.packet_counter = receive_counters_.transmitted;
  return stats;
}

bool StreamStatisticianImpl::GetActiveStatisticsAndReset(
    RtcpStatistics* statistics,int pathid) {
  rtc::CritScope cs(&stream_lock_);
  RtcpStatistics statistics_original;
  if (clock_->TimeInMilliseconds() - last_receive_time_ms_ >=
      kStatisticsTimeoutMs) {
    // Not active.
    return false;
  }
  if (!ReceivedRtpPacket()) {
    return false;
  }  
  if(pathid==1) {
    *statistics =MpPrimaryCalculateRtcpStatistics();
    statistics_original=CalculateRtcpStatistics();
    statistics->extended_highest_sequence_number=statistics_original.extended_highest_sequence_number;
    // RTC_LOG(INFO)<<"sandy sending the extended_highest_sequence_number original setting for primary "<< 
    //     statistics->extended_highest_sequence_number<<"\n";
  }else if(pathid==2 && mpcollector_->MpISsecondPathOpen()){
    *statistics =MpSecondaryCalculateRtcpStatistics();
    statistics_original=CalculateRtcpStatistics();
    statistics->extended_highest_sequence_number=statistics_original.extended_highest_sequence_number;
    // RTC_LOG(INFO)<<"sandy sending the extended_highest_sequence_number original setting for secondary "<< 
    //     statistics->extended_highest_sequence_number<<"\n";
  }else{
    *statistics = CalculateRtcpStatistics();
  }
  if(!mpcollector_->MpISsecondPathOpen() && pathid==2){
    RTC_DLOG(LS_ERROR)<<"sandy please fix the bugs like this\n";
  }

  return true;
}

RtcpStatistics StreamStatisticianImpl::MpPrimaryCalculateRtcpStatistics() {
  RtcpStatistics stats;
  int64_t exp_primary_since_last=0;
  exp_primary_since_last=received_seq_max_primary- received_seq_max_primary_last;
  if(exp_primary_since_last<0){
    exp_primary_since_last=0;//sandy Fix this bug
  }
  RTC_DCHECK_GE(exp_primary_since_last, 0);
  //RTC_LOG(INFO)<<"Mpsandypackets "<<received_seq_max_primary<<" "<<received_seq_max_primary_last<<" "<<p_packets<<" "<< exp_since_last;
  p_packets=0;
  int primary_fraction_lost=0;
  int32_t primary_lost_since_last;
  primary_lost_since_last= cumulative_loss_primary- cumulative_loss_primary_last;
  if(exp_primary_since_last >0 && primary_lost_since_last >0){
    primary_fraction_lost=(255 * primary_lost_since_last / exp_primary_since_last);
  }else{
    primary_fraction_lost=0;
  }
  if(primary_lost_since_last>kMpGapThreshold)//sandy: If loss is greater than 10 packets: RTP packets come here are ordered so no need to keep this thershold
  //if(primary_lost_since_last>0)//sandy: If loss is greater than 10 packets  
    stats.packets_lost_p=cumulative_loss_primary+cumulative_loss_rtcp_offset_p;
  else
    stats.packets_lost_p=0;
  if (stats.packets_lost_p < 0) {
    // Clamp to zero. Work around to accomodate for senders that misbehave with
    // negative cumulative loss.
    stats.packets_lost_p = 0;
    cumulative_loss_rtcp_offset_p = -cumulative_loss_primary;
  }
  stats.jitter_p = jitter_q4_p >> 4;
  received_seq_max_primary_last=received_seq_max_primary;
  cumulative_loss_primary_last=cumulative_loss_primary;
  stats.subflowfractionlost_p=primary_fraction_lost;
  stats.extended_highest_sequence_number_p=static_cast<uint32_t>(received_seq_max_primary);
  return stats;
}


RtcpStatistics StreamStatisticianImpl::MpSecondaryCalculateRtcpStatistics() {
  RtcpStatistics stats;
  int64_t exp_secondary_since_last=0; 
  exp_secondary_since_last=received_seq_max_secondary- received_seq_max_secondary_last;
  if(exp_secondary_since_last<0){
    exp_secondary_since_last=0;
  }
  RTC_DCHECK_GE(exp_secondary_since_last, 0);
  s_packets=0;
  int secondary_fraction_lost=0;
  int32_t secondary_lost_since_last;
  secondary_lost_since_last= cumulative_loss_secondary- cumulative_loss_secondary_last;
  if(exp_secondary_since_last >0 && secondary_lost_since_last >0){
    secondary_fraction_lost=(255 * secondary_lost_since_last / exp_secondary_since_last);
  }else{
    secondary_fraction_lost=0;
  }
  if(secondary_lost_since_last>kMpGapThreshold)//sandy: If loss is greater than 10 packets
  // if(secondary_lost_since_last>0)//sandy: If loss is greater than 10 packets
    stats.packets_lost_s=cumulative_loss_secondary+cumulative_loss_rtcp_offset_s;
  else
    stats.packets_lost_s=0;
  if (stats.packets_lost_s < 0) {
    // Clamp to zero. Work around to accomodate for senders that misbehave with
    // negative cumulative loss.
    stats.packets_lost_s = 0;
    cumulative_loss_rtcp_offset_s = -cumulative_loss_secondary;
  }
  stats.jitter_s = jitter_q4_s >> 4;
  received_seq_max_secondary_last=received_seq_max_secondary;
  cumulative_loss_secondary_last=cumulative_loss_secondary;
  stats.subflowfractionlost_s=secondary_fraction_lost;
  stats.extended_highest_sequence_number_s=static_cast<uint32_t>(received_seq_max_secondary);
  return stats;
}

//sandy: computing the loss rate
RtcpStatistics StreamStatisticianImpl::CalculateRtcpStatistics() {

  //RTC_LOG(INFO)<<"calculating RTCP stats\n";  
  RtcpStatistics stats;
  // Calculate fraction lost.
  int64_t exp_since_last = received_seq_max_ - last_report_seq_max_;
  RTC_DCHECK_GE(exp_since_last, 0);
  
  // int64_t exp_primary_since_last=0;
  // exp_primary_since_last=received_seq_max_primary- received_seq_max_primary_last;
  // RTC_LOG(INFO)<<"Mpsandypackets "<<received_seq_max_primary<<" "<<received_seq_max_primary_last<<" "<<p_packets<<" "<< exp_since_last;
  // p_packets=0;

  // int64_t exp_secondary_since_last=0; 
  // exp_secondary_since_last=received_seq_max_secondary- received_seq_max_secondary_last;
  // s_packets=0;
  
  int32_t lost_since_last = cumulative_loss_ - last_report_cumulative_loss_;
  if (exp_since_last > 0 && lost_since_last > 0) {
    // Scale 0 to 255, where 255 is 100% loss.
    stats.fraction_lost =
        static_cast<uint8_t>(255 * lost_since_last / exp_since_last);
  } else {
    stats.fraction_lost = 0;
  }
  //sandy:
  // int primary_fraction_lost=0;
  // int secondary_fraction_lost=0;
  // int32_t primary_lost_since_last;
  // primary_lost_since_last= cumulative_loss_primary- cumulative_loss_primary_last;
  
  // int32_t secondary_lost_since_last;
  // secondary_lost_since_last= cumulative_loss_secondary- cumulative_loss_secondary_last;
  
  // if(exp_primary_since_last >0 && primary_lost_since_last >0){
  //   primary_fraction_lost=(255 * primary_lost_since_last / exp_primary_since_last);
  // }else{
  //   primary_fraction_lost=0;
  // }
  // if(exp_secondary_since_last >0 && secondary_lost_since_last >0){
  //   secondary_fraction_lost=(255 * secondary_lost_since_last / exp_secondary_since_last);
  // }else{
  //   secondary_fraction_lost=0;
  // }
  // RTC_LOG(INFO)<<"mploss fraction lost from code\t"<<stats.fraction_lost<<" from lost from sandy primary\t"<<primary_fraction_lost<<" from sandy secondary\t"<<secondary_fraction_lost<<"\t" 
  // <<"Total packets on primary "<<exp_primary_since_last<<" count "<<p_packets<<" total packets on secondary "<<exp_secondary_since_last<<" count "<<s_packets<<"\n"; 
  //RTC_LOG(INFO)<<"mploss code: sandy expected_packets time "<<clock_->TimeInMilliseconds()<<" "<<exp_since_last<<" : "<<exp_primary_since_last<<" lost since last "<<lost_since_last<< 
  // " : "<<primary_lost_since_last<<" loss "<<stats.fraction_lost<<" : "<<primary_fraction_lost<<"\n";
  //RTC_LOG(INFO)<<"mploss code: sandy expected_packets time "<<clock_->TimeInMilliseconds()<<" : "<<stats.fraction_lost<<" : "<<primary_fraction_lost<<" : "<<secondary_fraction_lost<<"\n";
  // TODO(danilchap): Ensure |stats.packets_lost| is clamped to fit in a signed
  // 24-bit value.
  stats.packets_lost = cumulative_loss_ + cumulative_loss_rtcp_offset_;
  // stats.packets_lost_p=cumulative_loss_primary+cumulative_loss_rtcp_offset_p;
  // stats.packets_lost_s=cumulative_loss_secondary+cumulative_loss_rtcp_offset_s;
  // stats.packets_lost_p=100;
  // stats.packets_lost_s=200;
  if (stats.packets_lost < 0) {
    // Clamp to zero. Work around to accomodate for senders that misbehave with
    // negative cumulative loss.
    stats.packets_lost = 0;
    cumulative_loss_rtcp_offset_ = -cumulative_loss_;
  }
  // if (stats.packets_lost_p < 0) {
  //   // Clamp to zero. Work around to accomodate for senders that misbehave with
  //   // negative cumulative loss.
  //   stats.packets_lost_p = 0;
  //   cumulative_loss_rtcp_offset_p = -cumulative_loss_primary;
  // }
  // if (stats.packets_lost_s < 0) {
  //   // Clamp to zero. Work around to accomodate for senders that misbehave with
  //   // negative cumulative loss.
  //   stats.packets_lost_s = 0;
  //   cumulative_loss_rtcp_offset_s = -cumulative_loss_secondary;
  // }

  stats.extended_highest_sequence_number =
      static_cast<uint32_t>(received_seq_max_);

  // RTC_LOG(INFO)<<"sandy sending the extended_highest_sequence_number original "<< 
  //       stats.extended_highest_sequence_number<<"\n";
  // Note: internal jitter value is in Q4 and needs to be scaled by 1/16.
  stats.jitter = jitter_q4_ >> 4;
  // stats.jitter_p = jitter_q4_p >> 4;
  // stats.jitter_s = jitter_q4_s >> 4;

  // Only for report blocks in RTCP SR and RR.
  last_report_cumulative_loss_ = cumulative_loss_;
  last_report_seq_max_ = received_seq_max_;
  BWE_TEST_LOGGING_PLOT_WITH_SSRC(1, "cumulative_loss_pkts",
                                  clock_->TimeInMilliseconds(),
                                  cumulative_loss_, ssrc_);
  BWE_TEST_LOGGING_PLOT_WITH_SSRC(
      1, "received_seq_max_pkts", clock_->TimeInMilliseconds(),
      (received_seq_max_ - received_seq_first_), ssrc_);
  
  return stats;
}
//sandy: yet to implement
absl::optional<int> StreamStatisticianImpl::GetFractionLostInPercent() const {
  rtc::CritScope cs(&stream_lock_);
  int64_t expected_packets=0,expected_packets_primary=0,expected_packets_secondary=0;
  int32_t cumulative_loss_overall;

  if (!ReceivedRtpPacket()) {
    return absl::nullopt;
  }
  if(mpcollector_->MpISsecondPathOpen()){
    expected_packets_primary=1+received_seq_max_primary- received_seq_first_primary;
    expected_packets_secondary=1+received_seq_max_secondary- received_seq_first_secondary;
    expected_packets+=expected_packets_primary+expected_packets_secondary;
    cumulative_loss_overall=cumulative_loss_primary+cumulative_loss_secondary;
  }else{
    expected_packets = 1 + received_seq_max_ - received_seq_first_;
    cumulative_loss_overall=cumulative_loss_;    
  }
  
  if (expected_packets <= 0) {
    return absl::nullopt;
  }
  if (cumulative_loss_overall <= 0) {
    return 0;
  }
  return 100 * static_cast<int64_t>(cumulative_loss_overall) / expected_packets;//sandy: returned the overall
}

StreamDataCounters StreamStatisticianImpl::GetReceiveStreamDataCounters()
    const {
  rtc::CritScope cs(&stream_lock_);
  return receive_counters_;
}

uint32_t StreamStatisticianImpl::BitrateReceived() const {
  rtc::CritScope cs(&stream_lock_);
  return incoming_bitrate_.Rate(clock_->TimeInMilliseconds()).value_or(0);
}

bool StreamStatisticianImpl::IsRetransmitOfOldPacket(
    const RtpPacketReceived& packet,
    int64_t now_ms) const {
  uint32_t frequency_khz = packet.payload_type_frequency() / 1000;
  RTC_DCHECK_GT(frequency_khz, 0);

  int64_t time_diff_ms = now_ms - last_receive_time_ms_;

  // Diff in time stamp since last received in order.
  uint32_t timestamp_diff = packet.Timestamp() - last_received_timestamp_;
  uint32_t rtp_time_stamp_diff_ms = timestamp_diff / frequency_khz;

  int64_t max_delay_ms = 0;

  // Jitter standard deviation in samples.
  float jitter_std = std::sqrt(static_cast<float>(jitter_q4_ >> 4));

  // 2 times the standard deviation => 95% confidence.
  // And transform to milliseconds by dividing by the frequency in kHz.
  max_delay_ms = static_cast<int64_t>((2 * jitter_std) / frequency_khz);

  // Min max_delay_ms is 1.
  if (max_delay_ms == 0) {
    max_delay_ms = 1;
  }
  return time_diff_ms > rtp_time_stamp_diff_ms + max_delay_ms;
}

std::unique_ptr<ReceiveStatistics> ReceiveStatistics::Create(Clock* clock) {
  return std::make_unique<ReceiveStatisticsImpl>(clock);
}

ReceiveStatisticsImpl::ReceiveStatisticsImpl(Clock* clock)
    : clock_(clock),
      last_returned_ssrc_(0),
      max_reordering_threshold_(kDefaultMaxReorderingThreshold) {}

ReceiveStatisticsImpl::~ReceiveStatisticsImpl() {
  while (!statisticians_.empty()) {
    delete statisticians_.begin()->second;
    statisticians_.erase(statisticians_.begin());
  }
}

void ReceiveStatisticsImpl::OnRtpPacket(const RtpPacketReceived& packet) {
  //sandy: Receiving of RTP packet 
  // StreamStatisticianImpl instance is created once and only destroyed when
  // this whole ReceiveStatisticsImpl is destroyed. StreamStatisticianImpl has
  // it's own locking so don't hold receive_statistics_lock_ (potential
  // deadlock).
  GetOrCreateStatistician(packet.Ssrc())->UpdateCounters(packet);
}

StreamStatisticianImpl* ReceiveStatisticsImpl::GetStatistician(
    uint32_t ssrc) const {
  rtc::CritScope cs(&receive_statistics_lock_);
  const auto& it = statisticians_.find(ssrc);
  if (it == statisticians_.end())
    return NULL;
  return it->second;
}

StreamStatisticianImpl* ReceiveStatisticsImpl::GetOrCreateStatistician(
    uint32_t ssrc) {
  rtc::CritScope cs(&receive_statistics_lock_);
  StreamStatisticianImpl*& impl = statisticians_[ssrc];
  if (impl == nullptr) {  // new element
    impl = new StreamStatisticianImpl(ssrc, clock_, max_reordering_threshold_);
  }
  return impl;
}

void ReceiveStatisticsImpl::SetMaxReorderingThreshold(
    int max_reordering_threshold) {
  std::map<uint32_t, StreamStatisticianImpl*> statisticians;
  {
    rtc::CritScope cs(&receive_statistics_lock_);
    max_reordering_threshold_ = max_reordering_threshold;
    statisticians = statisticians_;
  }
  for (auto& statistician : statisticians) {
    statistician.second->SetMaxReorderingThreshold(max_reordering_threshold);
  }
}

void ReceiveStatisticsImpl::SetMaxReorderingThreshold(
    uint32_t ssrc,
    int max_reordering_threshold) {
  GetOrCreateStatistician(ssrc)->SetMaxReorderingThreshold(
      max_reordering_threshold);
}

void ReceiveStatisticsImpl::EnableRetransmitDetection(uint32_t ssrc,
                                                      bool enable) {
  GetOrCreateStatistician(ssrc)->EnableRetransmitDetection(enable);
}

std::vector<rtcp::ReportBlock> ReceiveStatisticsImpl::RtcpReportBlocks(
    size_t max_blocks,int pathid) {

  
  std::map<uint32_t, StreamStatisticianImpl*> statisticians;
  {
    rtc::CritScope cs(&receive_statistics_lock_);
    statisticians = statisticians_;
  }
  std::vector<rtcp::ReportBlock> result;
  result.reserve(std::min(max_blocks, statisticians.size()));


  auto add_report_block = [&result](uint32_t media_ssrc,
                                    StreamStatisticianImpl* statistician,int pathid) {
    // Do we have receive statistics to send?
    RtcpStatistics stats;
    if (!statistician->GetActiveStatisticsAndReset(&stats,pathid))
      return;
    result.emplace_back();
    rtcp::ReportBlock& block = result.back();
    block.SetMediaSsrc(media_ssrc);
    // RTC_LOG(INFO)<<"sandy comparition of original and my implementation fractional lost "<<stats.packets_lost<<":"<< 
    // stats.packets_lost_p<<" packet lost "<<stats.packets_lost<<":"<<stats.packets_lost_p<<" jitter "<<stats.jitter<<":"<< 
    // stats.jitter_p<<" highest sequence number "<<stats.extended_highest_sequence_number<<":"<<stats.extended_highest_sequence_number_p<<"\n";
    if(pathid!=2){//sandy:primary path
        //sandy: Here based on the path please change the fractional lost
        block.SetFractionLost(stats.subflowfractionlost_p);
        //sandy: Here based on the path please change the cumulative lost
        if (!block.SetCumulativeLost(stats.packets_lost_p)) {
          RTC_LOG(LS_WARNING) << "Cumulative lost is oversized.";
          result.pop_back();
          return;
        }
        ////sandy: Here based on the path please change the highest sequence number
        block.SetExtHighestSeqNum(stats.extended_highest_sequence_number_p);
        //sandy: Here based on the path please change the jitter
        block.SetJitter(stats.jitter_p);
        //RTC_LOG(INFO)<<"sandy sending the extended_highest_sequence_number original for primary path "<< 
        //stats.extended_highest_sequence_number<<"\n";
        block.SetExtHighestSeqNum_P(stats.extended_highest_sequence_number);//sandy: save the original sequence number
        block.SetRtcpSeq(rtcp_seq_p++);
        
    }else{//sandy:secondary path
        //sandy: Here based on the path please change the fractional lost
        block.SetFractionLost(stats.subflowfractionlost_s);
        //sandy: Here based on the path please change the cumulative lost
        if (!block.SetCumulativeLost(stats.packets_lost_s)) {
          RTC_LOG(LS_WARNING) << "Cumulative lost is oversized.";
          result.pop_back();
          return;
        }
        ////sandy: Here based on the path please change the highest sequence number
        block.SetExtHighestSeqNum(stats.extended_highest_sequence_number_s);
        //sandy: Here based on the path please change the jitter
        block.SetJitter(stats.jitter_s);
        //RTC_LOG(INFO)<<"sandy sending the extended_highest_sequence_number original for secondary path "<< 
        //stats.extended_highest_sequence_number<<"\n";
        block.SetExtHighestSeqNum_P(stats.extended_highest_sequence_number);//sandy: save the original sequence number
        block.SetRtcpSeq(rtcp_seq_s++);
    }   
  };

  const auto start_it = statisticians.upper_bound(last_returned_ssrc_);
  for (auto it = start_it;
       result.size() < max_blocks && it != statisticians.end(); ++it)
    add_report_block(it->first, it->second,pathid);
  for (auto it = statisticians.begin();
       result.size() < max_blocks && it != start_it; ++it)
    add_report_block(it->first, it->second,pathid);

  if (!result.empty())
    last_returned_ssrc_ = result.back().source_ssrc();
  return result;
}

}  // namespace webrtc
