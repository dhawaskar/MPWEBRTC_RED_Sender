/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/rtp_rtcp/source/rtp_rtcp_impl2.h"

#include <string.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "api/transport/field_trial_based_config.h"
#include "modules/rtp_rtcp/source/rtcp_packet/dlrr.h"
#include "modules/rtp_rtcp/source/rtp_rtcp_config.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "api/mp_collector.h"
#include "api/mp_global.h"

#ifdef _WIN32
// Disable warning C4355: 'this' : used in base member initializer list.
#pragma warning(disable : 4355)
#endif

namespace webrtc {
namespace {
const int64_t kRtpRtcpMaxIdleTimeProcessMs = 5;
const int64_t kRtpRtcpRttProcessTimeMs = 1000;
const int64_t kRtpRtcpBitrateProcessTimeMs = 10;
const int64_t kDefaultExpectedRetransmissionTimeMs = 125;
}  // namespace
//sandy: Below is where the sender is initialised
ModuleRtpRtcpImpl2::RtpSenderContext::RtpSenderContext(
    const RtpRtcpInterface::Configuration& config)
    : packet_history_p(config.clock, config.enable_rtx_padding_prioritization),
      packet_history_s(config.clock, config.enable_rtx_padding_prioritization),
      packet_sender(config, &packet_history_p,&packet_history_s),//rtp_sender_eggress
      non_paced_sender(&packet_sender),
      packet_generator(
          config,
          &packet_history_p,&packet_history_s,
          config.paced_sender ? config.paced_sender : &non_paced_sender) {}

ModuleRtpRtcpImpl2::ModuleRtpRtcpImpl2(const Configuration& configuration)
    : rtcp_sender_(configuration),
      rtcp_receiver_(configuration, this),
      clock_(configuration.clock),
      last_bitrate_process_time_(clock_->TimeInMilliseconds()),
      last_rtt_process_time_p_(clock_->TimeInMilliseconds()),
      last_rtt_process_time_s_(clock_->TimeInMilliseconds()),
      next_process_time_p_(clock_->TimeInMilliseconds() +
                         kRtpRtcpMaxIdleTimeProcessMs),
      next_process_time_s_(clock_->TimeInMilliseconds() +
                         kRtpRtcpMaxIdleTimeProcessMs),
      packet_overhead_(28),  // IPV4 UDP.
      nack_last_time_sent_full_ms_(0),
      nack_last_seq_number_sent_(0),
      remote_bitrate_(configuration.remote_bitrate_estimator),
      rtt_stats_(configuration.rtt_stats),
      rtt_ms_p_(0),
      rtt_ms_s_(0) {
  process_thread_checker_.Detach();
  if (!configuration.receiver_only) {
    rtp_sender_ = std::make_unique<RtpSenderContext>(configuration);
    // Make sure rtcp sender use same timestamp offset as rtp sender.
    rtcp_sender_.SetTimestampOffset(
        rtp_sender_->packet_generator.TimestampOffset());
  }

  // Set default packet size limit.
  // TODO(nisse): Kind-of duplicates
  // webrtc::VideoSendStream::Config::Rtp::kDefaultMaxPacketSize.
  const size_t kTcpOverIpv4HeaderSize = 40;
  SetMaxRtpPacketSize(IP_PACKET_SIZE - kTcpOverIpv4HeaderSize);
}

ModuleRtpRtcpImpl2::~ModuleRtpRtcpImpl2() {
  RTC_DCHECK_RUN_ON(&construction_thread_checker_);
}

// static
std::unique_ptr<ModuleRtpRtcpImpl2> ModuleRtpRtcpImpl2::Create(
    const Configuration& configuration) {
  RTC_DCHECK(configuration.clock);
  RTC_DCHECK(TaskQueueBase::Current());
  return std::make_unique<ModuleRtpRtcpImpl2>(configuration);
}

// Returns the number of milliseconds until the module want a worker thread
// to call Process.
int64_t ModuleRtpRtcpImpl2::TimeUntilNextProcess() {
  RTC_DCHECK_RUN_ON(&process_thread_checker_);
  return std::max<int64_t>(0,
                           next_process_time_p_ - clock_->TimeInMilliseconds());
}

// Process any pending tasks such as timeouts (non time critical events).
void ModuleRtpRtcpImpl2::Process() {
  RTC_DCHECK_RUN_ON(&process_thread_checker_);
  const int64_t now = clock_->TimeInMilliseconds();
  // TODO(bugs.webrtc.org/11581): Figure out why we need to call Process() 200
  // times a second.
  next_process_time_p_ = now + kRtpRtcpMaxIdleTimeProcessMs;
  next_process_time_s_ = now + kRtpRtcpMaxIdleTimeProcessMs;

  if (rtp_sender_) {
    if (now >= last_bitrate_process_time_ + kRtpRtcpBitrateProcessTimeMs) {
      rtp_sender_->packet_sender.ProcessBitrateAndNotifyObservers();
      last_bitrate_process_time_ = now;
      // TODO(bugs.webrtc.org/11581): Is this a bug? At the top of the function,
      // next_process_time_ is incremented by 5ms, here we effectively do a
      // std::min() of (now + 5ms, now + 10ms). Seems like this is a no-op?
      next_process_time_p_ =
          std::min(next_process_time_p_, now + kRtpRtcpBitrateProcessTimeMs);
      next_process_time_s_ =
          std::min(next_process_time_s_, now + kRtpRtcpBitrateProcessTimeMs);
    }
  }

  // TODO(bugs.webrtc.org/11581): We update the RTT once a second, whereas other
  // things that run in this method are updated much more frequently. Move the
  // RTT checking over to the worker thread, which matches better with where the
  // stats are maintained.
  bool process_rtt_p = now >= last_rtt_process_time_p_ + kRtpRtcpRttProcessTimeMs;
  bool process_rtt_s = now >= last_rtt_process_time_s_ + kRtpRtcpRttProcessTimeMs;

  if (rtcp_sender_.Sending()) {
    // Process RTT if we have received a report block and we haven't
    // processed RTT for at least |kRtpRtcpRttProcessTimeMs| milliseconds.
    // Note that LastReceivedReportBlockMs() grabs a lock, so check
    // |process_rtt| first.
    if (process_rtt_p && rtcp_receiver_.LastReceivedReportBlockMs(1) > last_rtt_process_time_p_) {
      std::vector<RTCPReportBlock> receive_blocks;
      rtcp_receiver_.StatisticsReceived(&receive_blocks,1);
      int64_t max_rtt = 0;
      for (std::vector<RTCPReportBlock>::iterator it = receive_blocks.begin();
           it != receive_blocks.end(); ++it) {
        int64_t rtt = 0;
        rtcp_receiver_.RTT(it->sender_ssrc, &rtt, NULL, NULL, NULL,1);//sandy:yet to implement
        // RTC_LOG(INFO)<<"sandyrtt1 the primary path rtt is "<<rtt<<"\n";
        max_rtt = (rtt > max_rtt) ? rtt : max_rtt;
      }
      // Report the rtt.
      if (rtt_stats_ && max_rtt != 0){
        // RTC_LOG(INFO)<<"sandyrtt1 primary update of RTT to call_stats2.cc-> video_receiver_stream2.cc";
        rtt_stats_->OnRttUpdate(max_rtt,1); 
      }
    }
    if (process_rtt_s && rtcp_receiver_.LastReceivedReportBlockMs(2) > last_rtt_process_time_s_ && 
      mpcollector_->MpISsecondPathOpen()) {
      std::vector<RTCPReportBlock> receive_blocks;
      rtcp_receiver_.StatisticsReceived(&receive_blocks,2);
      int64_t max_rtt = 0;
      for (std::vector<RTCPReportBlock>::iterator it = receive_blocks.begin();
           it != receive_blocks.end(); ++it) {
        int64_t rtt = 0;
        rtcp_receiver_.RTT(it->sender_ssrc, &rtt, NULL, NULL, NULL,2);//sandy:yet to implement
        // RTC_LOG(INFO)<<"sandyrtt1 the secondary path rtt is "<<rtt<<"\n";
        max_rtt = (rtt > max_rtt) ? rtt : max_rtt;
      }
      // Report the rtt.
      if (rtt_stats_ && max_rtt != 0){
        // RTC_LOG(INFO)<<"sandyrtt1 secondary update of RTT to call_stats2.cc-> video_receiver_stream2.cc";
        rtt_stats_->OnRttUpdate(max_rtt,2); 
      }
    }

    // Verify receiver reports are delivered and the reported sequence number
    // is increasing.
    // TODO(bugs.webrtc.org/11581): The timeout value needs to be checked every
    // few seconds (see internals of RtcpRrTimeout). Here, we may be polling it
    // a couple of hundred times a second, which isn't great since it grabs a
    // lock. Note also that LastReceivedReportBlockMs() (called above) and
    // RtcpRrTimeout() both grab the same lock and check the same timer, so
    // it should be possible to consolidate that work somehow.
    if (rtcp_receiver_.RtcpRrTimeout(1)) {
      RTC_LOG_F(LS_WARNING) << "Timeout: No RTCP RR received for primary.";
    } else if(rtcp_receiver_.RtcpRrTimeout(2)){
      RTC_LOG_F(LS_WARNING) << "Timeout: No RTCP RR received secondary.";
    } else if (rtcp_receiver_.RtcpRrSequenceNumberTimeout()) {
      RTC_LOG_F(LS_WARNING) << "Timeout: No increase in RTCP RR extended "
                               "highest sequence number.";
    }
    
    if (remote_bitrate_ && rtcp_sender_.TMMBR()) { //sandy: This is not enabled so do not worry
      unsigned int target_bitrate = 0;

      // RTC_LOG(INFO)<<"sandy the remote_bitrate_ is enabled\n";
      std::vector<unsigned int> ssrcs;
      if (remote_bitrate_->LatestEstimate(&ssrcs, &target_bitrate)) {
        if (!ssrcs.empty()) {
          target_bitrate = target_bitrate / ssrcs.size();
        }
        rtcp_sender_.SetTargetBitrate(target_bitrate);
      }
    }
  } else {
    // Report rtt from receiver.
    if (process_rtt_p) {
      int64_t rtt_ms;
      //sandy:This is never called becuase XrRrRTT message is not used
      if (rtt_stats_ && rtcp_receiver_.GetAndResetXrRrRtt(&rtt_ms,1)) {//sandy: yet to implement
        rtt_stats_->OnRttUpdate(rtt_ms,1);
      }
    }else if(process_rtt_s){
      int64_t rtt_ms;
      if (rtt_stats_ && rtcp_receiver_.GetAndResetXrRrRtt(&rtt_ms,2)) {//sandy: yet to implement
        rtt_stats_->OnRttUpdate(rtt_ms,2);
      }
    }
  }

  // Get processed rtt for primary path.
  if (process_rtt_p) {
    last_rtt_process_time_p_ = now;
    // TODO(bugs.webrtc.org/11581): Is this a bug? At the top of the function,
    // next_process_time_ is incremented by 5ms, here we effectively do a
    // std::min() of (now + 5ms, now + 1000ms). Seems like this is a no-op?
    next_process_time_p_ = std::min(
        next_process_time_p_, last_rtt_process_time_p_ + kRtpRtcpRttProcessTimeMs);
    if (rtt_stats_) {
      // Make sure we have a valid RTT before setting.
      int64_t last_rtt = rtt_stats_->LastProcessedRtt(1);
      if (last_rtt >= 0)
        set_rtt_ms(last_rtt,1);//sandy:yet to implement dont know so have put it as 1
    }
  }

  // Get processed rtt for secondary path.
  if (process_rtt_s && mpcollector_->MpISsecondPathOpen()) {
    last_rtt_process_time_s_ = now;
    // TODO(bugs.webrtc.org/11581): Is this a bug? At the top of the function,
    // next_process_time_ is incremented by 5ms, here we effectively do a
    // std::min() of (now + 5ms, now + 1000ms). Seems like this is a no-op?
    next_process_time_s_ = std::min(
        next_process_time_s_, last_rtt_process_time_s_ + kRtpRtcpRttProcessTimeMs);
    if (rtt_stats_) {
      // Make sure we have a valid RTT before setting.
      int64_t last_rtt = rtt_stats_->LastProcessedRtt(2);
      if (last_rtt >= 0)
        set_rtt_ms(last_rtt,2);//sandy:yet to implement dont know so have put it as 1
    }
  }

  if (rtcp_sender_.TimeToSendRTCPReport(false,1)){
    //sandy: Send the report for both the path
    // RTC_LOG(INFO)<<"sandyrtt sending primary path RTCP report: ";
    //rtcp_sender_.SendRTCP(GetFeedbackState(), kRtcpReport,0,0,1);//sandy: Add secondary path as well here
    rtcp_sender_.SendRTCP(GetFeedbackStatePrimary(),kRtcpReport,0,0,1);
    // rtcp_sender_.SendRTCP(GetFeedbackStateSecondary(), kRtcpReport,0,0,2);
    RTC_LOG(INFO)<<"sandyrtcperror send primary path report";
  }

  if(rtcp_sender_.TimeToSendRTCPReport(false,2)&& mpcollector_->MpISsecondPathOpen() 
  && !( mpcollector_->MpGetScheduler().find("red")!=std::string::npos)){//sandy: Check if second path open
    rtcp_sender_.SendRTCP(GetFeedbackStateSecondary(), kRtcpReport,0,0,2);
  // rtcp_sender_.SendRTCP(GetFeedbackStatePrimary(),kRtcpReport,0,0,1);
    RTC_LOG(INFO)<<"sandyrtcperror send secondary path report";
  }

  //MpWebRTC: Sending the primary and secondary feed back sandesh


  if (rtcp_sender_.TMMBR() && rtcp_receiver_.UpdateTmmbrTimers()) {
    rtcp_receiver_.NotifyTmmbrUpdated();
  }
  
}

void ModuleRtpRtcpImpl2::SetRtxSendStatus(int mode) {
  rtp_sender_->packet_generator.SetRtxStatus(mode);
}

int ModuleRtpRtcpImpl2::RtxSendStatus() const {
  return rtp_sender_ ? rtp_sender_->packet_generator.RtxStatus() : kRtxOff;
}

void ModuleRtpRtcpImpl2::SetRtxSendPayloadType(int payload_type,
                                               int associated_payload_type) {
  rtp_sender_->packet_generator.SetRtxPayloadType(payload_type,
                                                  associated_payload_type);
}

absl::optional<uint32_t> ModuleRtpRtcpImpl2::RtxSsrc() const {
  return rtp_sender_ ? rtp_sender_->packet_generator.RtxSsrc() : absl::nullopt;
}

absl::optional<uint32_t> ModuleRtpRtcpImpl2::FlexfecSsrc() const {
  if (rtp_sender_) {
    return rtp_sender_->packet_generator.FlexfecSsrc();
  }
  return absl::nullopt;
}

void ModuleRtpRtcpImpl2::IncomingRtcpPacket(const uint8_t* rtcp_packet,
                                            const size_t length) {
  rtcp_receiver_.IncomingPacket(rtcp_packet, length);
}

void ModuleRtpRtcpImpl2::RegisterSendPayloadFrequency(int payload_type,
                                                      int payload_frequency) {
  rtcp_sender_.SetRtpClockRate(payload_type, payload_frequency);
}

int32_t ModuleRtpRtcpImpl2::DeRegisterSendPayload(const int8_t payload_type) {
  return 0;
}

uint32_t ModuleRtpRtcpImpl2::StartTimestamp() const {
  return rtp_sender_->packet_generator.TimestampOffset();
}

// Configure start timestamp, default is a random number.
void ModuleRtpRtcpImpl2::SetStartTimestamp(const uint32_t timestamp) {
  rtcp_sender_.SetTimestampOffset(timestamp);
  rtp_sender_->packet_generator.SetTimestampOffset(timestamp);
  rtp_sender_->packet_sender.SetTimestampOffset(timestamp);
}

uint16_t ModuleRtpRtcpImpl2::SequenceNumber() const {
  return rtp_sender_->packet_generator.SequenceNumber();
}

// Set SequenceNumber, default is a random number.
void ModuleRtpRtcpImpl2::SetSequenceNumber(const uint16_t seq_num) {
  rtp_sender_->packet_generator.SetSequenceNumber(seq_num);
}

void ModuleRtpRtcpImpl2::SetRtpState(const RtpState& rtp_state) {
  rtp_sender_->packet_generator.SetRtpState(rtp_state);
  rtp_sender_->packet_sender.SetMediaHasBeenSent(rtp_state.media_has_been_sent);
  rtcp_sender_.SetTimestampOffset(rtp_state.start_timestamp);
}

void ModuleRtpRtcpImpl2::SetRtxState(const RtpState& rtp_state) {
  rtp_sender_->packet_generator.SetRtxRtpState(rtp_state);
}

RtpState ModuleRtpRtcpImpl2::GetRtpState() const {
  RtpState state = rtp_sender_->packet_generator.GetRtpState();
  state.media_has_been_sent = rtp_sender_->packet_sender.MediaHasBeenSent();
  return state;
}

RtpState ModuleRtpRtcpImpl2::GetRtxState() const {
  return rtp_sender_->packet_generator.GetRtxRtpState();
}

void ModuleRtpRtcpImpl2::SetRid(const std::string& rid) {
  if (rtp_sender_) {
    rtp_sender_->packet_generator.SetRid(rid);
  }
}

void ModuleRtpRtcpImpl2::SetMid(const std::string& mid) {
  if (rtp_sender_) {
    rtp_sender_->packet_generator.SetMid(mid);
  }
  // TODO(bugs.webrtc.org/4050): If we end up supporting the MID SDES item for
  // RTCP, this will need to be passed down to the RTCPSender also.
}

void ModuleRtpRtcpImpl2::SetCsrcs(const std::vector<uint32_t>& csrcs) {
  rtcp_sender_.SetCsrcs(csrcs);
  rtp_sender_->packet_generator.SetCsrcs(csrcs);
}

// TODO(pbos): Handle media and RTX streams separately (separate RTCP
// feedbacks).
RTCPSender::FeedbackState ModuleRtpRtcpImpl2::GetFeedbackState() {
  RTCPSender::FeedbackState state;
  // This is called also when receiver_only is true. Hence below
  // checks that rtp_sender_ exists.
  if (rtp_sender_) {
    StreamDataCounters rtp_stats;
    StreamDataCounters rtx_stats;
    rtp_sender_->packet_sender.GetDataCounters(&rtp_stats, &rtx_stats,1); //Sandy: Default is for primary path
    //rtp_sender_->packet_sender.GetDataCounters(&rtp_stats_1, &rtx_stats_1,2);

    // RTC_LOG(INFO)<<"sandystats primary packets sent : "<<(rtp_stats.transmitted.packets + rtx_stats.transmitted.packets)<<
    // " bytes sent: "<<(rtp_stats.transmitted.payload_bytes+rtx_stats.transmitted.payload_bytes)<<" secondary packets sent "<< 
    // (rtp_stats_1.transmitted.packets + rtx_stats_1.transmitted.packets)<<" bytes sent "<< 
    // (rtp_stats_1.transmitted.payload_bytes+rtx_stats_1.transmitted.payload_bytes)<<" \n";
    //sandy: Let us test if it return same value 

    state.packets_sent =
        rtp_stats.transmitted.packets + rtx_stats.transmitted.packets;
    state.media_bytes_sent = rtp_stats.transmitted.payload_bytes +
                             rtx_stats.transmitted.payload_bytes;
    state.send_bitrate =
        rtp_sender_->packet_sender.GetSendRates(1).Sum().bps<uint32_t>();//sandy: Add MP-path support here as well.
  }
  state.receiver = &rtcp_receiver_;

  LastReceivedNTP(&state.last_rr_ntp_secs, &state.last_rr_ntp_frac,
                  &state.remote_sr,1);

  state.last_xr_rtis = rtcp_receiver_.ConsumeReceivedXrReferenceTimeInfo();

  return state;
}


RTCPSender::FeedbackState ModuleRtpRtcpImpl2::GetFeedbackStatePrimary() {
  RTCPSender::FeedbackState state;
  // This is called also when receiver_only is true. Hence below
  // checks that rtp_sender_ exists.
  if (rtp_sender_) {
    StreamDataCounters rtp_stats;
    StreamDataCounters rtx_stats;
    rtp_sender_->packet_sender.GetDataCounters(&rtp_stats, &rtx_stats,1);
    state.packets_sent =
        rtp_stats.transmitted.packets + rtx_stats.transmitted.packets;
    state.media_bytes_sent = rtp_stats.transmitted.payload_bytes +
                             rtx_stats.transmitted.payload_bytes;
    state.send_bitrate =
        rtp_sender_->packet_sender.GetSendRates(1).Sum().bps<uint32_t>();

    // RTC_LOG(INFO)<<"sandy: the sending rate of primary "<<state.send_bitrate/1000<<" kbps";
  }
  state.receiver = &rtcp_receiver_;

  LastReceivedNTP(&state.last_rr_ntp_secs, &state.last_rr_ntp_frac,
                  &state.remote_sr,1);

  //sandy: Extended reports are not being exhanges hence this need not to be implemented.
  state.last_xr_rtis = rtcp_receiver_.ConsumeReceivedXrReferenceTimeInfo();

  return state;
}

RTCPSender::FeedbackState ModuleRtpRtcpImpl2::GetFeedbackStateSecondary() {
  RTCPSender::FeedbackState state;
  // This is called also when receiver_only is true. Hence below
  // checks that rtp_sender_ exists.
  if (rtp_sender_) {
    StreamDataCounters rtp_stats;
    StreamDataCounters rtx_stats;
    rtp_sender_->packet_sender.GetDataCounters(&rtp_stats, &rtx_stats,2);
    state.packets_sent =
        rtp_stats.transmitted.packets + rtx_stats.transmitted.packets;
    state.media_bytes_sent = rtp_stats.transmitted.payload_bytes +
                             rtx_stats.transmitted.payload_bytes;
    state.send_bitrate =
        rtp_sender_->packet_sender.GetSendRates(2).Sum().bps<uint32_t>();
    // RTC_LOG(INFO)<<"sandy: the sending rate of secondary "<<state.send_bitrate/1000<<" kbps";
  }
  state.receiver = &rtcp_receiver_;

  LastReceivedNTP(&state.last_rr_ntp_secs, &state.last_rr_ntp_frac,
                  &state.remote_sr,2);
  //sandy: Extended reports are not being exhanges hence this need not to be implemented.
  state.last_xr_rtis = rtcp_receiver_.ConsumeReceivedXrReferenceTimeInfo();

  return state;
}


// TODO(nisse): This method shouldn't be called for a receive-only
// stream. Delete rtp_sender_ check as soon as all applications are
// updated.
int32_t ModuleRtpRtcpImpl2::SetSendingStatus(const bool sending) {
  if (rtcp_sender_.Sending() != sending) {
    // Sends RTCP BYE when going from true to false
    if (rtcp_sender_.SetSendingStatus(GetFeedbackStatePrimary(), sending) != 0) {
      RTC_LOG(LS_WARNING) << "Failed to send RTCP BYE for primary";
    }
    if (rtcp_sender_.SetSendingStatus(GetFeedbackStateSecondary(), sending) != 0) {
      RTC_LOG(LS_WARNING) << "Failed to send RTCP BYE for secondary";
    }
  }
  return 0;
}

bool ModuleRtpRtcpImpl2::Sending() const {
  return rtcp_sender_.Sending();
}

// TODO(nisse): This method shouldn't be called for a receive-only
// stream. Delete rtp_sender_ check as soon as all applications are
// updated.
void ModuleRtpRtcpImpl2::SetSendingMediaStatus(const bool sending) {
  if (rtp_sender_) {
    rtp_sender_->packet_generator.SetSendingMediaStatus(sending);
  } else {
    RTC_DCHECK(!sending);
  }
}

bool ModuleRtpRtcpImpl2::SendingMedia() const {
  return rtp_sender_ ? rtp_sender_->packet_generator.SendingMedia() : false;
}

bool ModuleRtpRtcpImpl2::IsAudioConfigured() const {
  return rtp_sender_ ? rtp_sender_->packet_generator.IsAudioConfigured()
                     : false;
}

void ModuleRtpRtcpImpl2::SetAsPartOfAllocation(bool part_of_allocation) {
  RTC_CHECK(rtp_sender_);
  rtp_sender_->packet_sender.ForceIncludeSendPacketsInAllocation(
      part_of_allocation);
}
//sandy: Sent the RTP Frame here
bool ModuleRtpRtcpImpl2::OnSendingRtpFrame(uint32_t timestamp,
                                           int64_t capture_time_ms,
                                           int payload_type,
                                           bool force_sender_report) {
  if (!Sending())
    return false;

  rtcp_sender_.SetLastRtpTime(timestamp, capture_time_ms, payload_type);
  // Make sure an RTCP report isn't queued behind a key frame.

  //sandy: Send the report for both path
  if (rtcp_sender_.TimeToSendRTCPReport(force_sender_report,1)){//sandy:redundant
    //RTC_LOG(INFO)<<"sandyrtt sending RTCP report: "<<kRtcpReport<<"\n";
    // RTC_LOG(INFO)<<"sandy just requesting to send the sender report\n";
    rtcp_sender_.SendRTCP(GetFeedbackStatePrimary(), kRtcpReport,0,0,1);
    // rtcp_sender_.SendRTCP(GetFeedbackStateSecondary(), kRtcpReport,0,0,2);
  }
  if (mpcollector_->MpISsecondPathOpen()&& rtcp_sender_.TimeToSendRTCPReport(force_sender_report,2)){//sandy: send it for each path
    //RTC_LOG(INFO)<<"sandyrtt sending RTCP report: "<<kRtcpReport<<"\n";
    // RTC_LOG(INFO)<<"sandy just requesting to send the sender report\n";
    if(!( mpcollector_->MpGetScheduler().find("red")!=std::string::npos)){
      rtcp_sender_.SendRTCP(GetFeedbackStateSecondary(), kRtcpReport,0,0,2);
      // rtcp_sender_.SendRTCP(GetFeedbackStatePrimary(), kRtcpReport,0,0,1);
    }
  }

  return true;
}

bool ModuleRtpRtcpImpl2::TrySendPacket(RtpPacketToSend* packet,
                                       const PacedPacketInfo& pacing_info) {
  RTC_DCHECK(rtp_sender_);
  // TODO(sprang): Consider if we can remove this check.
  if (!rtp_sender_->packet_generator.SendingMedia()) {
    return false;
  }
  rtp_sender_->packet_sender.SendPacket(packet, pacing_info);
  return true;
}

void ModuleRtpRtcpImpl2::OnPacketsAcknowledged(//sandy: Add this with path id
    rtc::ArrayView<const uint16_t> sequence_numbers,int pathid) {

  // RTC_LOG(INFO)<<"sandyrtp packet acknowledgment\n";//yet to implement
  RTC_DCHECK(rtp_sender_);
  if(pathid!=2)
    rtp_sender_->packet_history_p.CullAcknowledgedPackets(sequence_numbers);//sandy: I do not know this yet
  else
    rtp_sender_->packet_history_s.CullAcknowledgedPackets(sequence_numbers);//sandy: I do not know this yet
}

bool ModuleRtpRtcpImpl2::SupportsPadding() const {
  RTC_DCHECK(rtp_sender_);
  return rtp_sender_->packet_generator.SupportsPadding();
}

bool ModuleRtpRtcpImpl2::SupportsRtxPayloadPadding() const {
  RTC_DCHECK(rtp_sender_);
  return rtp_sender_->packet_generator.SupportsRtxPayloadPadding();
}

std::vector<std::unique_ptr<RtpPacketToSend>>
ModuleRtpRtcpImpl2::GeneratePadding(size_t target_size_bytes) {
  RTC_DCHECK(rtp_sender_);
  return rtp_sender_->packet_generator.GeneratePadding(
      target_size_bytes, rtp_sender_->packet_sender.MediaHasBeenSent());
}

std::vector<RtpSequenceNumberMap::Info>
ModuleRtpRtcpImpl2::GetSentRtpPacketInfos(
    rtc::ArrayView<const uint16_t> sequence_numbers) const {
  RTC_DCHECK(rtp_sender_);
  return rtp_sender_->packet_sender.GetSentRtpPacketInfos(sequence_numbers);
}

size_t ModuleRtpRtcpImpl2::ExpectedPerPacketOverhead() const {
  if (!rtp_sender_) {
    return 0;
  }
  return rtp_sender_->packet_generator.ExpectedPerPacketOverhead();
}

size_t ModuleRtpRtcpImpl2::MaxRtpPacketSize() const {
  RTC_DCHECK(rtp_sender_);
  return rtp_sender_->packet_generator.MaxRtpPacketSize();
}

void ModuleRtpRtcpImpl2::SetMaxRtpPacketSize(size_t rtp_packet_size) {
  RTC_DCHECK_LE(rtp_packet_size, IP_PACKET_SIZE)
      << "rtp packet size too large: " << rtp_packet_size;
  RTC_DCHECK_GT(rtp_packet_size, packet_overhead_)
      << "rtp packet size too small: " << rtp_packet_size;

  rtcp_sender_.SetMaxRtpPacketSize(rtp_packet_size);
  if (rtp_sender_) {
    rtp_sender_->packet_generator.SetMaxRtpPacketSize(rtp_packet_size);
  }
}

RtcpMode ModuleRtpRtcpImpl2::RTCP() const {
  return rtcp_sender_.Status();
}

// Configure RTCP status i.e on/off.
void ModuleRtpRtcpImpl2::SetRTCPStatus(const RtcpMode method) {
  rtcp_sender_.SetRTCPStatus(method);
}

int32_t ModuleRtpRtcpImpl2::SetCNAME(const char* c_name) {
  return rtcp_sender_.SetCNAME(c_name);
}

int32_t ModuleRtpRtcpImpl2::RemoteNTP(uint32_t* received_ntpsecs,
                                      uint32_t* received_ntpfrac,
                                      uint32_t* rtcp_arrival_time_secs,
                                      uint32_t* rtcp_arrival_time_frac,
                                      uint32_t* rtcp_timestamp,int pathid) const {//sandy: This function is important for the clock synchronization.
  if(pathid!=2){
    return rtcp_receiver_.NTP(received_ntpsecs, received_ntpfrac,
                            rtcp_arrival_time_secs, rtcp_arrival_time_frac,
                            rtcp_timestamp,1)
             ? 0
             : -1;
  }else{
    return rtcp_receiver_.NTP(received_ntpsecs, received_ntpfrac,
                            rtcp_arrival_time_secs, rtcp_arrival_time_frac,
                            rtcp_timestamp,2)
             ? 0
             : -1;
  }
}

// Get RoundTripTime.
//sandy: Studying this
int32_t ModuleRtpRtcpImpl2::RTT(const uint32_t remote_ssrc,
                                int64_t* rtt,
                                int64_t* avg_rtt,
                                int64_t* min_rtt,
                                int64_t* max_rtt,int pathid) const {
  int32_t ret;
  if(pathid!=2){
    ret = rtcp_receiver_.RTT(remote_ssrc, rtt, avg_rtt, min_rtt, max_rtt,pathid);
    if (rtt && *rtt == 0) {
      // Try to get RTT from RtcpRttStats class.
      rtt_ms(1,rtt);//sandy: yet to implement
    }
  }else{
    ret = rtcp_receiver_.RTT(remote_ssrc, rtt, avg_rtt, min_rtt, max_rtt,pathid);
    if (rtt && *rtt == 0) {
      // Try to get RTT from RtcpRttStats class.
      rtt_ms(2,rtt);//sandy: yet to implement
    }
  }
  
  return ret;
}

int64_t ModuleRtpRtcpImpl2::ExpectedRetransmissionTimeMs() const {
  int64_t expected_retransmission_time_ms = 0;
  rtt_ms(1,&expected_retransmission_time_ms);//sandy: yet to implement
  if (expected_retransmission_time_ms > 0) {
    return expected_retransmission_time_ms;
  }
  // No rtt available (|kRtpRtcpRttProcessTimeMs| not yet passed?), so try to
  // poll avg_rtt_ms directly from rtcp receiver.
  if (rtcp_receiver_.RTT(rtcp_receiver_.RemoteSSRC(), nullptr,
                         &expected_retransmission_time_ms, nullptr,
                         nullptr,1) == 0) {//sandy: yet to implement
    return expected_retransmission_time_ms;
  }
  return kDefaultExpectedRetransmissionTimeMs;
}

// Force a send of an RTCP packet.: Usully when pli or fir is requested from receiver
// Normal SR and RR are triggered via the process function.
int32_t ModuleRtpRtcpImpl2::SendRTCP(RTCPPacketType packet_type) {//sandy: Request to send the Picture loss infromation:pli or key frame
  if(packet_type==kAppSubtypeMpHalf || packet_type==kAppSubtypeMpFull){
    RTCPSender::FeedbackState state;
    RTC_LOG(INFO)<<"sandyofo creating App specific message\n";
    if(mpcollector_->MpGetBestPathId()==1)
      return rtcp_sender_.SendRTCP(GetFeedbackStatePrimary(), packet_type,0,0,1);//sandy: Pli is sent via primary path
    else 
      return rtcp_sender_.SendRTCP(GetFeedbackStateSecondary(), packet_type,0,0,2);
  }

  //sandy: This is sent on demand and send it for both paths
  if(!mpcollector_->MpISsecondPathOpen() || ( mpcollector_->MpGetScheduler().find("red")!=std::string::npos)){
    return rtcp_sender_.SendRTCP(GetFeedbackStatePrimary(), packet_type,0,0,1);//sandy: Pli is sent via primary path
  }else{
    //sandy: Always send this request via primary path
    // rtcp_sender_.SendRTCP(GetFeedbackStatePrimary(), packet_type,0,0,1);//sandy: Pli is sent via primary path
    if(mpcollector_->MpGetBestPathId()==1)
      return rtcp_sender_.SendRTCP(GetFeedbackStatePrimary(), packet_type,0,0,1);//sandy: Pli is sent via primary path
    else 
      return rtcp_sender_.SendRTCP(GetFeedbackStateSecondary(), packet_type,0,0,2);
  } 
}

void ModuleRtpRtcpImpl2::SetRtcpXrRrtrStatus(bool enable) {
  rtcp_receiver_.SetRtcpXrRrtrStatus(enable);
  rtcp_sender_.SendRtcpXrReceiverReferenceTime(enable);
}

bool ModuleRtpRtcpImpl2::RtcpXrRrtrStatus() const {
  return rtcp_sender_.RtcpXrReceiverReferenceTime();
}

void ModuleRtpRtcpImpl2::GetSendStreamDataCounters(
    StreamDataCounters* rtp_counters,
    StreamDataCounters* rtx_counters,int pathid) const {
  rtp_sender_->packet_sender.GetDataCounters(rtp_counters, rtx_counters,pathid);//This is called from audio so never called
}

// Received RTCP report.
int32_t ModuleRtpRtcpImpl2::RemoteRTCPStat(
    std::vector<RTCPReportBlock>* receive_blocks) const {
  return rtcp_receiver_.StatisticsReceived(receive_blocks,1);//sandy yet to implement you have to send to
  //both primary and secondary path unlike sending for path=1 above
}

std::vector<ReportBlockData> ModuleRtpRtcpImpl2::GetLatestReportBlockData(int pathid)
    const {
  return rtcp_receiver_.GetLatestReportBlockData(pathid);
}

// (REMB) Receiver Estimated Max Bitrate.
void ModuleRtpRtcpImpl2::SetRemb(int64_t bitrate_bps,
                                 std::vector<uint32_t> ssrcs) {
  rtcp_sender_.SetRemb(bitrate_bps, std::move(ssrcs));
}

void ModuleRtpRtcpImpl2::UnsetRemb() {
  rtcp_sender_.UnsetRemb();
}

void ModuleRtpRtcpImpl2::SetExtmapAllowMixed(bool extmap_allow_mixed) {
  rtp_sender_->packet_generator.SetExtmapAllowMixed(extmap_allow_mixed);
}

void ModuleRtpRtcpImpl2::RegisterRtpHeaderExtension(absl::string_view uri,
                                                    int id) {


  // RTC_LOG(INFO)<<"Checking the RTP extension header \t"<<uri<<"\t"<<id<<"\n";
  bool registered =
      rtp_sender_->packet_generator.RegisterRtpHeaderExtension(uri, id);
  RTC_CHECK(registered);
}

int32_t ModuleRtpRtcpImpl2::DeregisterSendRtpHeaderExtension(
    const RTPExtensionType type) {
  return rtp_sender_->packet_generator.DeregisterRtpHeaderExtension(type);
}
void ModuleRtpRtcpImpl2::DeregisterSendRtpHeaderExtension(
    absl::string_view uri) {
  rtp_sender_->packet_generator.DeregisterRtpHeaderExtension(uri);
}

void ModuleRtpRtcpImpl2::SetTmmbn(std::vector<rtcp::TmmbItem> bounding_set) {
  rtcp_sender_.SetTmmbn(std::move(bounding_set));
}

// Send a Negative acknowledgment packet.//sandy: this is not called
int32_t ModuleRtpRtcpImpl2::SendNACK(const uint16_t* nack_list,
                                     const uint16_t size) {
  uint16_t nack_length = size;
  uint16_t start_id = 0;
  int64_t now_ms = clock_->TimeInMilliseconds();
  if (TimeToSendFullNackList(now_ms)) {
    nack_last_time_sent_full_ms_ = now_ms;
  } else {
    // Only send extended list.
    if (nack_last_seq_number_sent_ == nack_list[size - 1]) {
      // Last sequence number is the same, do not send list.
      return 0;
    }
    // Send new sequence numbers.
    for (int i = 0; i < size; ++i) {
      if (nack_last_seq_number_sent_ == nack_list[i]) {
        start_id = i + 1;
        break;
      }
    }
    nack_length = size - start_id;
  }

  // Our RTCP NACK implementation is limited to kRtcpMaxNackFields sequence
  // numbers per RTCP packet.
  if (nack_length > kRtcpMaxNackFields) {
    nack_length = kRtcpMaxNackFields;
  }
  nack_last_seq_number_sent_ = nack_list[start_id + nack_length - 1];
  RTC_LOG(INFO)<<"sandyrtt sending RTCP report: NACK "<<kRtcpReport<<"\n"; //This is not called sandy
  return rtcp_sender_.SendRTCP(GetFeedbackState(), kRtcpNack, nack_length,
                               &nack_list[start_id]);
}

void ModuleRtpRtcpImpl2::SendNack(
    const std::vector<uint16_t>& sequence_numbers,int pathid) {
  // RTC_LOG(INFO)<<"sandyrtt sending RTCP report: NACK for path="<<pathid<<"\n"; //This is not called sandy
  if(pathid!=2){
    rtcp_sender_.SendRTCP(GetFeedbackStatePrimary(), kRtcpNack, sequence_numbers.size(),
                        sequence_numbers.data(),pathid);
  }else{
    rtcp_sender_.SendRTCP(GetFeedbackStateSecondary(), kRtcpNack, sequence_numbers.size(),
                        sequence_numbers.data(),pathid);
  }
}

bool ModuleRtpRtcpImpl2::TimeToSendFullNackList(int64_t now) const {
  // Use RTT from RtcpRttStats class if provided.
  int64_t rtt =0;
  rtt_ms(1,&rtt);//sandy:yet to implement
  if (rtt == 0) {
    rtcp_receiver_.RTT(rtcp_receiver_.RemoteSSRC(), NULL, &rtt, NULL, NULL,1);
  }

  const int64_t kStartUpRttMs = 100;
  int64_t wait_time = 5 + ((rtt * 3) >> 1);  // 5 + RTT * 1.5.
  if (rtt == 0) {
    wait_time = kStartUpRttMs;
  }

  // Send a full NACK list once within every |wait_time|.
  return now - nack_last_time_sent_full_ms_ > wait_time;
}

// Store the sent packets, needed to answer to Negative acknowledgment requests.
void ModuleRtpRtcpImpl2::SetStorePacketsStatus(const bool enable,
                                               const uint16_t number_to_store) {//sandy: Added this pathid

  //RTC_LOG(INFO)<<"sandyrtp check this as well\n"; //sandy: Just initialisation here thats all
  rtp_sender_->packet_history_p.SetStorePacketsStatus(
      enable ? RtpPacketHistory::StorageMode::kStoreAndCull
             : RtpPacketHistory::StorageMode::kDisabled,
      number_to_store);
  rtp_sender_->packet_history_s.SetStorePacketsStatus(
      enable ? RtpPacketHistory::StorageMode::kStoreAndCull
             : RtpPacketHistory::StorageMode::kDisabled,
      number_to_store);
}

bool ModuleRtpRtcpImpl2::StorePackets(int pathid) const {//sandy: Added this path id here
  if(pathid!=2)
  return rtp_sender_->packet_history_p.GetStorageMode() !=
         RtpPacketHistory::StorageMode::kDisabled;
  else
    return rtp_sender_->packet_history_s.GetStorageMode() !=
         RtpPacketHistory::StorageMode::kDisabled;
}

void ModuleRtpRtcpImpl2::SendCombinedRtcpPacket(
    std::vector<std::unique_ptr<rtcp::RtcpPacket>> rtcp_packets,int pathid) {

  // RTC_LOG(INFO)<<"sandyrtcp: Sending SendCombinedRtcpPacket\n";
  rtcp_sender_.SendCombinedRtcpPacket(std::move(rtcp_packets),pathid);//sandy:you need to implement here as well 
}

int32_t ModuleRtpRtcpImpl2::SendLossNotification(uint16_t last_decoded_seq_num,
                                                 uint16_t last_received_seq_num,
                                                 bool decodability_flag,
                                                 bool buffering_allowed) {


  //sandy: This function sends the loss notification from receiver to sender encoder. Since this is
  //frame level process, we not have path id for this.
  return rtcp_sender_.SendLossNotification(
      GetFeedbackStatePrimary(), last_decoded_seq_num, last_received_seq_num,
      decodability_flag, buffering_allowed);
}

void ModuleRtpRtcpImpl2::SetRemoteSSRC(const uint32_t ssrc) {
  // Inform about the incoming SSRC.
  rtcp_sender_.SetRemoteSSRC(ssrc);
  rtcp_receiver_.SetRemoteSSRC(ssrc);
}

// TODO(nisse): Delete video_rate amd fec_rate arguments.
void ModuleRtpRtcpImpl2::BitrateSent(uint32_t* total_rate,
                                     uint32_t* video_rate,
                                     uint32_t* fec_rate,
                                     uint32_t* nack_rate) const {
  RtpSendRates send_rates = rtp_sender_->packet_sender.GetSendRatesOverall();//+rtp_sender_->packet_sender.GetSendRates(2);//sandy yet to implement I do not know so adding it up
  *total_rate = send_rates.Sum().bps<uint32_t>();
  if (video_rate)
    *video_rate = 0;
  if (fec_rate)
    *fec_rate = 0;
  *nack_rate = send_rates[RtpPacketMediaType::kRetransmission].bps<uint32_t>();
}

RtpSendRates ModuleRtpRtcpImpl2::GetSendRates() const {//sandy: yet to implement
  return rtp_sender_->packet_sender.GetSendRatesOverall();// sandy:do not know yet to implement
}

void ModuleRtpRtcpImpl2::OnRequestSendReport() {
  // RTC_LOG(INFO)<<"sandy requesting the sending report\n";
  SendRTCP(kRtcpSr);//sandy: studying this
}

void ModuleRtpRtcpImpl2::OnReceivedNack(
    const std::vector<uint16_t>& nack_sequence_numbers,int pathid) {

  RTC_LOG(INFO)<<"sandystats received  NAK for pathid "<<pathid<<"\n";
  if (!rtp_sender_)
    return;

  if (!StorePackets(pathid) || nack_sequence_numbers.empty()) {
    return;
  }
  // Use RTT from RtcpRttStats class if provided.
  
  if(pathid!=2){
    int64_t  rtt1 =0.0;
    rtt_ms(1,&rtt1);////sandy: yet to implement
    // RTC_LOG(INFO)<<"sandyrtt the primary path rtt is "<<rtt1<<'\n';
    if (rtt1 == 0){
      rtcp_receiver_.RTT(rtcp_receiver_.RemoteSSRC(), NULL, &rtt1, NULL, NULL,1);////sandy: yet to implement
    }
    rtp_sender_->packet_generator.OnReceivedNack(nack_sequence_numbers, rtt1,pathid);
  }else if(pathid==2){
    int64_t rtt2 =0;
    // RTC_LOG(INFO)<<"sandyrtt the secondary path rtt before is "<<rtt2<<'\n';
    rtt_ms(2,&rtt2);////sandy: yet to implement
    // RTC_LOG(INFO)<<"sandyrtt the secondary path rtt is "<<rtt2<<'\n';
    if (rtt2 == 0){
      rtcp_receiver_.RTT(rtcp_receiver_.RemoteSSRC(), NULL, &rtt2, NULL, NULL,2);////sandy: yet to implement
    }
    rtp_sender_->packet_generator.OnReceivedNack(nack_sequence_numbers, rtt2,pathid);
  }
  //RTC_LOG(INFO)<<"sandyrtt the primary path rtt is "<<rtt<<'\n';
}

void ModuleRtpRtcpImpl2::OnReceivedRtcpReportBlocks(
    const ReportBlockList& report_blocks) {
  if (rtp_sender_) {
    uint32_t ssrc = SSRC();
    absl::optional<uint32_t> rtx_ssrc;
    if (rtp_sender_->packet_generator.RtxStatus() != kRtxOff) {
      rtx_ssrc = rtp_sender_->packet_generator.RtxSsrc();
    }

    for (const RTCPReportBlock& report_block : report_blocks) {
      if (ssrc == report_block.source_ssrc) {
        rtp_sender_->packet_generator.OnReceivedAckOnSsrc(
            report_block.extended_highest_sequence_number_p);//sandy: Sent the original sequence number
      } else if (rtx_ssrc && *rtx_ssrc == report_block.source_ssrc) {
        rtp_sender_->packet_generator.OnReceivedAckOnRtxSsrc(
            report_block.extended_highest_sequence_number_p);//sandy: Sent the original sequence number
      }
    }
  }
}

bool ModuleRtpRtcpImpl2::LastReceivedNTP(
    uint32_t* rtcp_arrival_time_secs,  // When we got the last report.
    uint32_t* rtcp_arrival_time_frac,
    uint32_t* remote_sr,int pathid) const {
  // Remote SR: NTP inside the last received (mid 16 bits from sec and frac).
  uint32_t ntp_secs = 0;
  uint32_t ntp_frac = 0;

  if (!rtcp_receiver_.NTP(&ntp_secs, &ntp_frac, rtcp_arrival_time_secs,
                          rtcp_arrival_time_frac, NULL,pathid)) {
    return false;
  }
  *remote_sr =
      ((ntp_secs & 0x0000ffff) << 16) + ((ntp_frac & 0xffff0000) >> 16);
  return true;
}

void ModuleRtpRtcpImpl2::set_rtt_ms(int64_t rtt_ms,int pathid) {//sandy: Add path id here
  {
    
    if(pathid!=2){
      rtc::CritScope cs(&critical_section_rtt_p_);
      rtt_ms_p_ = rtt_ms;
      if (rtp_sender_) {
        rtp_sender_->packet_history_p.SetRtt(rtt_ms);
      }
    }
    else if (pathid==2 && mpcollector_->MpISsecondPathOpen()){
      rtc::CritScope cs(&critical_section_rtt_s_);
      rtt_ms_s_=rtt_ms;
      if (rtp_sender_) {
       rtp_sender_->packet_history_s.SetRtt(rtt_ms);
      }
    }
  }
}

void ModuleRtpRtcpImpl2::rtt_ms(int pathid,int64_t *rtt_ms) const {//sandy:yet to implement
  
  if(pathid!=2){ //sandy:primary path RTT
    rtc::CritScope cs(&critical_section_rtt_p_);
    // RTC_LOG(INFO)<<"sandyrtt the primary path rtt is before"<<rtt_ms_p_<<":"<<*rtt_ms<<"\n";
    *rtt_ms=rtt_ms_p_;
     // RTC_LOG(INFO)<<"sandyrtt the primary path rtt is after"<<rtt_ms_p_<<":"<<*rtt_ms<<"\n";
  }else{//sandy: secondary path RTT
    rtc::CritScope cs(&critical_section_rtt_s_);
    // RTC_LOG(INFO)<<"sandyrtt the secondary path rtt is before"<<rtt_ms_s_<<":"<<*rtt_ms<<"\n";
    *rtt_ms=rtt_ms_s_;
    // RTC_LOG(INFO)<<"sandyrtt the secondary path rtt is after"<<rtt_ms_s_<<":"<<*rtt_ms<<"\n";
  }
}

void ModuleRtpRtcpImpl2::SetVideoBitrateAllocation(
    const VideoBitrateAllocation& bitrate) {
  rtcp_sender_.SetVideoBitrateAllocation(bitrate);
}

RTPSender* ModuleRtpRtcpImpl2::RtpSender() {
  return rtp_sender_ ? &rtp_sender_->packet_generator : nullptr;
}

const RTPSender* ModuleRtpRtcpImpl2::RtpSender() const {
  return rtp_sender_ ? &rtp_sender_->packet_generator : nullptr;
}

}  // namespace webrtc
