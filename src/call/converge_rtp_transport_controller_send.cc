/*
 *  Copyright (c) 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
/* Sandy: this is the original file*/
#include "call/rtp_transport_controller_send.h"

#include <memory>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "absl/types/optional.h"
#include "api/transport/goog_cc_factory.h"
#include "api/transport/network_types.h"
#include "api/units/data_rate.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "call/rtp_video_sender.h"
#include "logging/rtc_event_log/events/rtc_event_remote_estimate.h"
#include "logging/rtc_event_log/events/rtc_event_route_change.h"
#include "modules/rtp_rtcp/source/rtcp_packet/transport_feedback.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/rate_limiter.h"
#include "api/mp_collector.h"
#include "api/mp_global.h"
#include <limits>

namespace webrtc {
namespace {
static const int64_t kRetransmitWindowSizeMs = 500;
static const size_t kMaxOverheadBytes = 500;

constexpr TimeDelta kPacerQueueUpdateInterval = TimeDelta::Millis(25);

TargetRateConstraints ConvertConstraints(int min_bitrate_bps,
                                         int max_bitrate_bps,
                                         int start_bitrate_bps,
                                         Clock* clock) {
  TargetRateConstraints msg;
  msg.at_time = Timestamp::Millis(clock->TimeInMilliseconds());
  msg.min_data_rate = min_bitrate_bps >= 0
                          ? DataRate::BitsPerSec(min_bitrate_bps)
                          : DataRate::Zero();
  msg.max_data_rate = max_bitrate_bps > 0
                          ? DataRate::BitsPerSec(max_bitrate_bps)
                          : DataRate::Infinity();
  if (start_bitrate_bps > 0)
    msg.starting_rate = DataRate::BitsPerSec(start_bitrate_bps);
  return msg;
}

TargetRateConstraints ConvertConstraints(const BitrateConstraints& contraints,
                                         Clock* clock) {
  return ConvertConstraints(contraints.min_bitrate_bps,
                            contraints.max_bitrate_bps,
                            contraints.start_bitrate_bps, clock);
}

bool IsEnabled(const WebRtcKeyValueConfig* trials, absl::string_view key) {
  RTC_DCHECK(trials != nullptr);
  return absl::StartsWith(trials->Lookup(key), "Enabled");
}

bool IsRelayed(const rtc::NetworkRoute& route) {
  return route.local.uses_turn() || route.remote.uses_turn();
}

}  // namespace

RtpTransportControllerSend::RtpTransportControllerSend(
    Clock* clock,
    webrtc::RtcEventLog* event_log,
    NetworkStatePredictorFactoryInterface* predictor_factory,
    NetworkControllerFactoryInterface* controller_factory,
    const BitrateConstraints& bitrate_config,
    std::unique_ptr<ProcessThread> process_thread,
    TaskQueueFactory* task_queue_factory,
    const WebRtcKeyValueConfig* trials)
    : clock_(clock),
      event_log_(event_log),
      bitrate_configurator_(bitrate_config),
      process_thread_(std::move(process_thread)),
      use_task_queue_pacer_(IsEnabled(trials, "WebRTC-TaskQueuePacer")),
      process_thread_pacer_(use_task_queue_pacer_
                                ? nullptr
                                : new PacedSender(clock,
                                                  &packet_router_,
                                                  event_log,
                                                  trials,
                                                  process_thread_.get())),
      task_queue_pacer_(
          use_task_queue_pacer_
              ? new TaskQueuePacedSender(
                    clock,
                    &packet_router_,
                    event_log,
                    trials,
                    task_queue_factory,
                    /*hold_back_window = */ PacingController::kMinSleepTime)
              : nullptr),
      observer_(nullptr),
      controller_factory_override_(controller_factory),
      controller_factory_fallback_(
          std::make_unique<GoogCcNetworkControllerFactory>(predictor_factory)),
      process_interval_(controller_factory_fallback_->GetProcessInterval()),
      last_report_block_time_(Timestamp::Millis(clock_->TimeInMilliseconds())),
      last_report_block_time_s_(Timestamp::Millis(clock_->TimeInMilliseconds())),
      reset_feedback_on_route_change_(
          !IsEnabled(trials, "WebRTC-Bwe-NoFeedbackReset")),
      send_side_bwe_with_overhead_(
          IsEnabled(trials, "WebRTC-SendSideBwe-WithOverhead")),
      add_pacing_to_cwin_(
          IsEnabled(trials, "WebRTC-AddPacingToCongestionWindowPushback")),
      relay_bandwidth_cap_("relay_cap", DataRate::PlusInfinity()),
      transport_overhead_bytes_per_packet_(0),
      network_available_(false),
      retransmission_rate_limiter_(clock, kRetransmitWindowSizeMs),
      task_queue_(task_queue_factory->CreateTaskQueue(
          "rtp_send_controller",
          TaskQueueFactory::Priority::HIGH)) {


  ParseFieldTrial({&relay_bandwidth_cap_},
                  trials->Lookup("WebRTC-Bwe-NetworkRouteConstraints"));
  initial_config_.constraints = ConvertConstraints(bitrate_config, clock_);
  initial_config_.event_log = event_log;
  initial_config_.key_value_config = trials;
  RTC_DCHECK(bitrate_config.start_bitrate_bps > 0);

  pacer()->SetPacingRates(
      DataRate::BitsPerSec(bitrate_config.start_bitrate_bps), DataRate::Zero());

  if (!use_task_queue_pacer_) {
    process_thread_->Start();
  }
  if(!mpcollector_){
    // RTC_LOG(INFO)<<"MpCollector pointer:created newly\n";
    mpcollector_=new MpCollector();
  }
}

RtpTransportControllerSend::~RtpTransportControllerSend() {
  if (!use_task_queue_pacer_) {
    process_thread_->Stop();
  }
}

RtpVideoSenderInterface* RtpTransportControllerSend::CreateRtpVideoSender(
    std::map<uint32_t, RtpState> suspended_ssrcs,
    const std::map<uint32_t, RtpPayloadState>& states,
    const RtpConfig& rtp_config,
    int rtcp_report_interval_ms,
    Transport* send_transport,
    const RtpSenderObservers& observers,
    RtcEventLog* event_log,
    std::unique_ptr<FecController> fec_controller,
    const RtpSenderFrameEncryptionConfig& frame_encryption_config,
    rtc::scoped_refptr<FrameTransformerInterface> frame_transformer) {
  video_rtp_senders_.push_back(std::make_unique<RtpVideoSender>(
      clock_, suspended_ssrcs, states, rtp_config, rtcp_report_interval_ms,
      send_transport, observers,
      // TODO(holmer): Remove this circular dependency by injecting
      // the parts of RtpTransportControllerSendInterface that are really used.
      this, event_log, &retransmission_rate_limiter_, std::move(fec_controller),
      frame_encryption_config.frame_encryptor,
      frame_encryption_config.crypto_options, std::move(frame_transformer)));
  return video_rtp_senders_.back().get();
}

void RtpTransportControllerSend::DestroyRtpVideoSender(
    RtpVideoSenderInterface* rtp_video_sender) {
  std::vector<std::unique_ptr<RtpVideoSenderInterface>>::iterator it =
      video_rtp_senders_.end();
  for (it = video_rtp_senders_.begin(); it != video_rtp_senders_.end(); ++it) {
    if (it->get() == rtp_video_sender) {
      break;
    }
  }
  RTC_DCHECK(it != video_rtp_senders_.end());
  video_rtp_senders_.erase(it);
}

void RtpTransportControllerSend::UpdateControlState() {
  absl::optional<TargetTransferRate> update = control_handler_->GetUpdate();
  if (!update)
    return;

  //sandy: You need to put limit of 25Mbps same as in webrtc_video_engine.cc because that limit is applied to each path
  //resuliting in double of the limit and hence put it here
  // if(update->target_rate.bps()>20000000){
  //   update->target_rate=DataRate::BitsPerSec(20000000);
  // }
  retransmission_rate_limiter_.SetMaxRate(update->target_rate.bps());
  // We won't create control_handler_ until we have an observers.
  RTC_DCHECK(observer_ != nullptr);
  observer_->OnTargetTransferRate(*update);
}

RtpPacketPacer* RtpTransportControllerSend::pacer() {
  if (use_task_queue_pacer_) {
    return task_queue_pacer_.get();
  }
  return process_thread_pacer_.get();
}

const RtpPacketPacer* RtpTransportControllerSend::pacer() const {
  if (use_task_queue_pacer_) {
    return task_queue_pacer_.get();
  }
  return process_thread_pacer_.get();
}

rtc::TaskQueue* RtpTransportControllerSend::GetWorkerQueue() {
  return &task_queue_;
}

PacketRouter* RtpTransportControllerSend::packet_router() {
  return &packet_router_;
}

NetworkStateEstimateObserver*
RtpTransportControllerSend::network_state_estimate_observer() {
  return this;
}

TransportFeedbackObserver*
RtpTransportControllerSend::transport_feedback_observer() {
  return this;
}

RtpPacketSender* RtpTransportControllerSend::packet_sender() {
  if (use_task_queue_pacer_) {
    return task_queue_pacer_.get();
  }
  return process_thread_pacer_.get();
}

void RtpTransportControllerSend::SetAllocatedSendBitrateLimits(
    BitrateAllocationLimits limits) {
  RTC_DCHECK_RUN_ON(&task_queue_);
  streams_config_.min_total_allocated_bitrate = limits.min_allocatable_rate;
  streams_config_.max_padding_rate = limits.max_padding_rate;
  streams_config_.max_total_allocated_bitrate = limits.max_allocatable_rate;
  UpdateStreamsConfig();
}
void RtpTransportControllerSend::SetPacingFactor(float pacing_factor) {
  RTC_DCHECK_RUN_ON(&task_queue_);
  streams_config_.pacing_factor = pacing_factor;
  UpdateStreamsConfig();
}
void RtpTransportControllerSend::SetQueueTimeLimit(int limit_ms) {
  pacer()->SetQueueTimeLimit(TimeDelta::Millis(limit_ms));
}
StreamFeedbackProvider*
RtpTransportControllerSend::GetStreamFeedbackProvider() {
  return &feedback_demuxer_;
}

void RtpTransportControllerSend::RegisterTargetTransferRateObserver(
    TargetTransferRateObserver* observer) {
  task_queue_.PostTask([this, observer] {
    RTC_DCHECK_RUN_ON(&task_queue_);
    RTC_DCHECK(observer_ == nullptr);
    observer_ = observer;
    observer_->OnStartRateUpdate(*initial_config_.constraints.starting_rate);
    MaybeCreateControllers();
  });
}

bool RtpTransportControllerSend::IsRelevantRouteChange(
    const rtc::NetworkRoute& old_route,
    const rtc::NetworkRoute& new_route) const {
  // TODO(bugs.webrtc.org/11438): Experiment with using more information/
  // other conditions.
  bool connected_changed = old_route.connected != new_route.connected;
  bool route_ids_changed =
      old_route.local.network_id() != new_route.local.network_id() ||
      old_route.remote.network_id() != new_route.remote.network_id();
  if (relay_bandwidth_cap_->IsFinite()) {
    bool relaying_changed = IsRelayed(old_route) != IsRelayed(new_route);
    return connected_changed || route_ids_changed || relaying_changed;
  } else {
    return connected_changed || route_ids_changed;
  }
}

void RtpTransportControllerSend::OnNetworkRouteChanged(
    const std::string& transport_name,
    const rtc::NetworkRoute& network_route) {
  // Check if the network route is connected.

  if (!network_route.connected) {
    // TODO(honghaiz): Perhaps handle this in SignalChannelNetworkState and
    // consider merging these two methods.
    return;
  }

  absl::optional<BitrateConstraints> relay_constraint_update =
      ApplyOrLiftRelayCap(IsRelayed(network_route));

  // Check whether the network route has changed on each transport.
  auto result =
      network_routes_.insert(std::make_pair(transport_name, network_route));
  auto kv = result.first;
  bool inserted = result.second;
  if (inserted || !(kv->second == network_route)) {
    RTC_LOG(LS_INFO) << "Network route changed on transport " << transport_name
                     << ": new_route = " << network_route.DebugString();
    if (!inserted) {
      RTC_LOG(LS_INFO) << "old_route = " << kv->second.DebugString();
    }
  }

  if (inserted) {
    if (relay_constraint_update.has_value()) {
      UpdateBitrateConstraints(*relay_constraint_update);
    }
    task_queue_.PostTask([this, network_route] {
      RTC_DCHECK_RUN_ON(&task_queue_);
      transport_overhead_bytes_per_packet_ = network_route.packet_overhead;
    });
    // No need to reset BWE if this is the first time the network connects.
    return;
  }

  const rtc::NetworkRoute old_route = kv->second;
  kv->second = network_route;

  // Check if enough conditions of the new/old route has changed
  // to trigger resetting of bitrates (and a probe).
  if (IsRelevantRouteChange(old_route, network_route)) {
    BitrateConstraints bitrate_config = bitrate_configurator_.GetConfig();
    RTC_LOG(LS_INFO) << "Reset bitrates to min: "
                     << bitrate_config.min_bitrate_bps
                     << " bps, start: " << bitrate_config.start_bitrate_bps
                     << " bps,  max: " << bitrate_config.max_bitrate_bps
                     << " bps.";
    RTC_DCHECK_GT(bitrate_config.start_bitrate_bps, 0);

    if (event_log_) {
      event_log_->Log(std::make_unique<RtcEventRouteChange>(
          network_route.connected, network_route.packet_overhead));
    }
    NetworkRouteChange msg;
    msg.at_time = Timestamp::Millis(clock_->TimeInMilliseconds());
    msg.constraints = ConvertConstraints(bitrate_config, clock_);
    task_queue_.PostTask([this, msg, network_route] {
      RTC_DCHECK_RUN_ON(&task_queue_);
      transport_overhead_bytes_per_packet_ = network_route.packet_overhead;
      if (reset_feedback_on_route_change_) {
        transport_feedback_adapter_.SetNetworkRoute(network_route);
        transport_feedback_adapter_s_.SetNetworkRoute(network_route);
      }
      NetworkControlUpdate update1,update2;
      if (controller_ && controller_s_) {
        update1=controller_->OnNetworkRouteChange(msg);
        update2=controller_s_->OnNetworkRouteChange(msg);
        PostUpdates(update1,update2);
      } else {
        UpdateInitialConstraints(msg.constraints);
      }
      pacer()->UpdateOutstandingData(DataSize::Zero());
    });
  }
}

void RtpTransportControllerSend::OnNetworkAvailability(bool network_available) {
  RTC_LOG(LS_VERBOSE) << "SignalNetworkState "
                      << (network_available ? "Up" : "Down");
  NetworkAvailability msg;
  msg.at_time = Timestamp::Millis(clock_->TimeInMilliseconds());
  msg.network_available = network_available;
  task_queue_.PostTask([this, msg]() {
    RTC_DCHECK_RUN_ON(&task_queue_);
    if (network_available_ == msg.network_available)
      return;
    network_available_ = msg.network_available;
    if (network_available_) {
      pacer()->Resume();
    } else {
      pacer()->Pause();
    }
    pacer()->UpdateOutstandingData(DataSize::Zero());

    if (controller_ && controller_s_) {
      NetworkControlUpdate update1,update2;
      control_handler_->SetNetworkAvailability(network_available_);
      // control_handler_s_->SetNetworkAvailability(network_available_);
      update1=controller_->OnNetworkAvailability(msg);
      update2=controller_s_->OnNetworkAvailability(msg);
      PostUpdates(update1,update2);
      UpdateControlState();
    } else {
      MaybeCreateControllers();
    }
  });

  for (auto& rtp_sender : video_rtp_senders_) {
    rtp_sender->OnNetworkAvailability(network_available);
  }
}

RtcpBandwidthObserver* RtpTransportControllerSend::GetBandwidthObserver() {
  return this;
}
int64_t RtpTransportControllerSend::GetPacerQueuingDelayMs() const {
  return pacer()->OldestPacketWaitTime().ms();
}
absl::optional<Timestamp> RtpTransportControllerSend::GetFirstPacketTime()
    const {
  return pacer()->FirstSentPacketTime();
}
void RtpTransportControllerSend::EnablePeriodicAlrProbing(bool enable) {
  task_queue_.PostTask([this, enable]() {
    RTC_DCHECK_RUN_ON(&task_queue_);
    streams_config_.requests_alr_probing = enable;
    UpdateStreamsConfig();
  });
}
void RtpTransportControllerSend::OnSentPacket(
    const rtc::SentPacket& sent_packet) {

  
  task_queue_.PostTask([this, sent_packet]() {
    NetworkControlUpdate update1,update2;
    absl::optional<SentPacket> packet_msg;
    RTC_DCHECK_RUN_ON(&task_queue_);
    int pathid=sent_packet.pathid;

    RTC_LOG(INFO)<<"sandystats sent packet on path id= "<<pathid<<"packet type = "<<(sent_packet.packet_id<0?"RTCP":"RTP");
     /*
    sandy: When the redundent scheduler is in place,we should only use one single sender and receiver controller.Hence srtp_transport.cc 
    drops packets when redundant seq comes in and similarly at sender we should only not of unique sequence numbers
    */
    //Sandy: When dual camera is used with multipath, packets cannot be dropped and hence need to differentiate the steams
    // if(( mpcollector_->MpGetScheduler().find("red")!=std::string::npos) && mpcollector_->MpISsecondPathOpen() 
    //   &&sent_packet.packet_id>0){
    //   if(send_seq_list_.find(sent_packet.packet_id)!= send_seq_list_.end()){
    //     return;
    //   }else{
    //     if(send_seq_list_.size()>MPBUFFERSIZE)
    //       send_seq_list_.erase(send_seq_list_.begin(),send_seq_list_.end());
    //     send_seq_list_.insert ( std::pair<int,int>(sent_packet.packet_id,sent_packet.packet_id) );
    //   }
    //   RTC_LOG(INFO)<<"sandystats received packet on rtp_read seq= "<<sent_packet.packet_id<<" pathid= "<<sent_packet.pathid;
    //   pathid=1;
    // }
    /*
    */

    if(sent_packet.packet_id != -1){
      // RTC_LOG(INFO)<<"sandystats the packet is sent on path id ="<<pathid;
      RTC_DCHECK(pathid>0);
    }
    
    /*Sandy: For Redundant scheduler, you sending the same packets on both path 1 and path 2  
    and hence do not account them on P1 and P2. Below is for RTCP packets,if you look in p2p_transport_channel.cc file we have sent 
    RTCP on primary path first and hence we only consider that and drop same RTCP sent on secondary path*/
     //Sandy: When dual camera is used with multipath, packets cannot be dropped and hence need to differentiate the steams
    // if(( mpcollector_->MpGetScheduler().find("red")!=std::string::npos) && mpcollector_->MpISsecondPathOpen() &&
    //   pathid==2 && sent_packet.packet_id<0){
    //   return;
    // }
    if(pathid!=2){
      packet_msg =
        transport_feedback_adapter_.ProcessSentPacket(sent_packet);  
    }else if(pathid==2){
      packet_msg =
        transport_feedback_adapter_s_.ProcessSentPacket(sent_packet);
    }
    
    pacer()->UpdateOutstandingData(
        transport_feedback_adapter_.GetOutstandingData()+transport_feedback_adapter_s_.GetOutstandingData());

    

    if (packet_msg && controller_ && pathid!=2 ){
      update1=controller_->OnSentPacket(*packet_msg);
    }else if(packet_msg && controller_s_ && pathid==2){
      update2=controller_s_->OnSentPacket(*packet_msg);
    }

    PostUpdates(update1,update2);
  });
}

void RtpTransportControllerSend::OnReceivedPacket(
    const ReceivedPacket& packet_msg) {
  task_queue_.PostTask([this, packet_msg]() {
    NetworkControlUpdate update1,update2;
    RTC_DCHECK_RUN_ON(&task_queue_);
    int pathid=packet_msg.pathid;
    //RTC_LOG(INFO)<<"sandystats received packet transport controller "<<pathid;
    if (controller_ && pathid!=2){
      update1=controller_->OnReceivedPacket(packet_msg);
    }else if(controller_s_ && pathid==2){
      update2=controller_s_->OnReceivedPacket(packet_msg);
    }

    PostUpdates(update1,update2);
  });
}

void RtpTransportControllerSend::UpdateBitrateConstraints(
    const BitrateConstraints& updated) {
  TargetRateConstraints msg = ConvertConstraints(updated, clock_);
  task_queue_.PostTask([this, msg]() {
    NetworkControlUpdate update1,update2;
    RTC_DCHECK_RUN_ON(&task_queue_);
    if (controller_ && controller_s_) {
      update1=controller_->OnTargetRateConstraints(msg);
      update2=controller_s_->OnTargetRateConstraints(msg);
      PostUpdates(update1,update2);
    } else {
      UpdateInitialConstraints(msg);
    }
  });
}

void RtpTransportControllerSend::SetSdpBitrateParameters(
    const BitrateConstraints& constraints) {
  absl::optional<BitrateConstraints> updated =
      bitrate_configurator_.UpdateWithSdpParameters(constraints);
  if (updated.has_value()) {
    UpdateBitrateConstraints(*updated);
  } else {
    RTC_LOG(LS_VERBOSE)
        << "WebRTC.RtpTransportControllerSend.SetSdpBitrateParameters: "
           "nothing to update";
  }
}

void RtpTransportControllerSend::SetClientBitratePreferences(
    const BitrateSettings& preferences) {
  absl::optional<BitrateConstraints> updated =
      bitrate_configurator_.UpdateWithClientPreferences(preferences);
  if (updated.has_value()) {
    UpdateBitrateConstraints(*updated);
  } else {
    RTC_LOG(LS_VERBOSE)
        << "WebRTC.RtpTransportControllerSend.SetClientBitratePreferences: "
           "nothing to update";
  }
}

absl::optional<BitrateConstraints>
RtpTransportControllerSend::ApplyOrLiftRelayCap(bool is_relayed) {
  DataRate cap = is_relayed ? relay_bandwidth_cap_ : DataRate::PlusInfinity();
  return bitrate_configurator_.UpdateWithRelayCap(cap);
}

void RtpTransportControllerSend::OnTransportOverheadChanged(
    size_t transport_overhead_bytes_per_packet) {
  if (transport_overhead_bytes_per_packet >= kMaxOverheadBytes) {
    RTC_LOG(LS_ERROR) << "Transport overhead exceeds " << kMaxOverheadBytes;
    return;
  }

  pacer()->SetTransportOverhead(
      DataSize::Bytes(transport_overhead_bytes_per_packet));

  // TODO(holmer): Call AudioRtpSenders when they have been moved to
  // RtpTransportControllerSend.
  for (auto& rtp_video_sender : video_rtp_senders_) {
    rtp_video_sender->OnTransportOverheadChanged(
        transport_overhead_bytes_per_packet);
  }
}

void RtpTransportControllerSend::AccountForAudioPacketsInPacedSender(
    bool account_for_audio) {
  pacer()->SetAccountForAudioPackets(account_for_audio);
}

void RtpTransportControllerSend::IncludeOverheadInPacedSender() {
  pacer()->SetIncludeOverhead();
}

void RtpTransportControllerSend::OnReceivedEstimatedBitrate(uint32_t bitrate) {
  RemoteBitrateReport msg;
  msg.receive_time = Timestamp::Millis(clock_->TimeInMilliseconds());
  msg.bandwidth = DataRate::BitsPerSec(bitrate);
  task_queue_.PostTask([this, msg]() {
    RTC_DCHECK_RUN_ON(&task_queue_);
    if (controller_)
      PostUpdates(controller_->OnRemoteBitrateReport(msg),NetworkControlUpdate());
  });
}

//Sandy: Received the ACK for all the RTP packet sent
void RtpTransportControllerSend::OnReceivedRtcpReceiverReport(
    const ReportBlockList& report_blocks,
    int64_t rtt_ms,
    int64_t now_ms,int pathid) {

  task_queue_.PostTask([this, report_blocks, now_ms,pathid]() {
    RTC_DCHECK_RUN_ON(&task_queue_);
    OnReceivedRtcpReceiverReportBlocks(report_blocks, now_ms,pathid);
  });

  task_queue_.PostTask([this, now_ms, rtt_ms,pathid]() {
    RTC_DCHECK_RUN_ON(&task_queue_);
    NetworkControlUpdate update1,update2;
    RoundTripTimeUpdate report;
    report.receive_time = Timestamp::Millis(now_ms);
    report.round_trip_time = TimeDelta::Millis(rtt_ms);
    report.smoothed = false;
    if (controller_ && !report.round_trip_time.IsZero() && pathid!=2){
      RTC_LOG(INFO)<<"sandy: P1 RTT "<<report.round_trip_time.ms()<<" ms";
      update1=controller_->OnRoundTripTimeUpdate(report);
    }
    else if(controller_s_ && !report.round_trip_time.IsZero() && pathid==2){
      RTC_LOG(INFO)<<"sandy: P2 RTT "<<report.round_trip_time.ms()<<" ms";
      update2=controller_s_->OnRoundTripTimeUpdate(report);
    }
    PostUpdates(update1,update2);
  });
}

void RtpTransportControllerSend::OnAddPacket(
    const RtpPacketSendInfo& packet_info) {
  feedback_demuxer_.AddPacket(packet_info);

  Timestamp creation_time = Timestamp::Millis(clock_->TimeInMilliseconds());
  task_queue_.PostTask([this, packet_info, creation_time]() {
    RTC_DCHECK_RUN_ON(&task_queue_);

    int pathid=packet_info.pathid;
    RTC_LOG(INFO)<<"sandystats added packet on path "<<pathid;
    RTC_DCHECK(pathid>0);
    if(pathid!=2){
      transport_feedback_adapter_.AddPacket(
        packet_info,
        send_side_bwe_with_overhead_ ? transport_overhead_bytes_per_packet_ : 0,
        creation_time);  
    }else{
      transport_feedback_adapter_s_.AddPacket(
        packet_info,
        send_side_bwe_with_overhead_ ? transport_overhead_bytes_per_packet_ : 0,
        creation_time);
    }
    RTC_LOG(INFO)<<"sandystats done adding packet on path "<<pathid;
    
  });
}

void RtpTransportControllerSend::OnTransportFeedback(
    const rtcp::TransportFeedback& feedback) {

  int pathid=feedback.pathid();
  RTC_LOG(INFO)<<"sandyratio TFB packet on path "<<pathid;
  RTC_DCHECK(pathid>0);
  feedback_demuxer_.OnTransportFeedback(feedback,pathid);
  auto feedback_time = Timestamp::Millis(clock_->TimeInMilliseconds());

  task_queue_.PostTask([this, feedback, feedback_time,pathid]() {
    absl::optional<TransportPacketsFeedback> feedback_msg;
    NetworkControlUpdate update1,update2;
    RTC_DCHECK_RUN_ON(&task_queue_);
    
    

    if(pathid!=2){
      feedback_msg =
        transport_feedback_adapter_.ProcessTransportFeedback(feedback,
                                                             feedback_time);
    }else if(pathid==2){
      feedback_msg =
        transport_feedback_adapter_s_.ProcessTransportFeedback(feedback,
                                                             feedback_time);
    }
    
    if (feedback_msg && controller_ && pathid!=2) {
      update1=controller_->OnTransportPacketsFeedback(*feedback_msg);
    }else if(feedback_msg && controller_s_ && pathid==2){
      update2=controller_s_->OnTransportPacketsFeedback(*feedback_msg);
    }
    PostUpdates(update1,update2);

    pacer()->UpdateOutstandingData(
        transport_feedback_adapter_.GetOutstandingData()+transport_feedback_adapter_s_.GetOutstandingData());
  });
}

void RtpTransportControllerSend::OnRemoteNetworkEstimate(
    NetworkStateEstimate estimate) {
  if (event_log_) {
    event_log_->Log(std::make_unique<RtcEventRemoteEstimate>(
        estimate.link_capacity_lower, estimate.link_capacity_upper));
  }
  estimate.update_time = Timestamp::Millis(clock_->TimeInMilliseconds());
  task_queue_.PostTask([this, estimate] {
    RTC_DCHECK_RUN_ON(&task_queue_);
    if (controller_)
      PostUpdates(controller_->OnNetworkStateEstimate(estimate),NetworkControlUpdate());
  });
}

void RtpTransportControllerSend::MaybeCreateControllers() {//sandy: Create controller for both primary and secondary
  RTC_DCHECK(!controller_);
  RTC_DCHECK(!control_handler_);
  RTC_DCHECK(!controller_s_);
  // RTC_DCHECK(!control_handler_s_);

  if (!network_available_ || !observer_)
    return;
  control_handler_ = std::make_unique<CongestionControlHandler>();
  // control_handler_s_ = std::make_unique<CongestionControlHandler>();

  initial_config_.constraints.at_time =
      Timestamp::Millis(clock_->TimeInMilliseconds());
  initial_config_.stream_based_config = streams_config_;

  // TODO(srte): Use fallback controller if no feedback is available.
  if (controller_factory_override_) {
    RTC_LOG(LS_INFO) << "Creating overridden congestion controller";
    controller_ = controller_factory_override_->Create(initial_config_);
    process_interval_ = controller_factory_override_->GetProcessInterval();
    controller_s_ = controller_factory_override_->Create(initial_config_);
  } else {
    RTC_LOG(LS_INFO) << "Creating fallback congestion controller";
    controller_ = controller_factory_fallback_->Create(initial_config_);
    process_interval_ = controller_factory_fallback_->GetProcessInterval();
    controller_s_ = controller_factory_fallback_->Create(initial_config_);
  }
  UpdateControllerWithTimeInterval();
  StartProcessPeriodicTasks();
}

void RtpTransportControllerSend::UpdateInitialConstraints(
    TargetRateConstraints new_contraints) {
  if (!new_contraints.starting_rate)
    new_contraints.starting_rate = initial_config_.constraints.starting_rate;
  RTC_DCHECK(new_contraints.starting_rate);
  initial_config_.constraints = new_contraints;
}

void RtpTransportControllerSend::StartProcessPeriodicTasks() {
  if (!pacer_queue_update_task_.Running()) {
    pacer_queue_update_task_ = RepeatingTaskHandle::DelayedStart(
        task_queue_.Get(), kPacerQueueUpdateInterval, [this]() {
          RTC_DCHECK_RUN_ON(&task_queue_);
          TimeDelta expected_queue_time = pacer()->ExpectedQueueTime();
          if(mpcollector_->MpISsecondPathOpen() &&!( mpcollector_->MpGetScheduler().find("red")!=std::string::npos)){
          //sandy: Change #1 double the Queue size
            control_handler_->SetPacerQueue(expected_queue_time/4);
          }else{
            control_handler_->SetPacerQueue(expected_queue_time);
          }
          // control_handler_s_->SetPacerQueue(expected_queue_time);
          UpdateControlState();
          return kPacerQueueUpdateInterval;
        });
  }
  controller_task_.Stop();
  controller_task_s_.Stop();
  if (process_interval_.IsFinite()) {
    controller_task_ = RepeatingTaskHandle::DelayedStart(
        task_queue_.Get(), process_interval_, [this]() {
          RTC_DCHECK_RUN_ON(&task_queue_);
          UpdateControllerWithTimeInterval();
          return process_interval_;
        });

    controller_task_s_ = RepeatingTaskHandle::DelayedStart(
        task_queue_.Get(), process_interval_, [this]() {
          RTC_DCHECK_RUN_ON(&task_queue_);
          UpdateControllerWithTimeInterval();
          return process_interval_;
        });
  }
}

void RtpTransportControllerSend::UpdateControllerWithTimeInterval() {
  NetworkControlUpdate update1,update2;
  RTC_DCHECK(controller_ && controller_s_);
  ProcessInterval msg,msg2;
  msg.at_time = Timestamp::Millis(clock_->TimeInMilliseconds());
  msg2.at_time=msg.at_time;
  if (add_pacing_to_cwin_){
    msg.pacer_queue = pacer()->QueueSizeData();
    msg2.pacer_queue = pacer()->QueueSizeData();
  }
  update1=controller_->OnProcessInterval(msg);
  update2=controller_s_->OnProcessInterval(msg);
  PostUpdates(update1,update2);
}

void RtpTransportControllerSend::UpdateStreamsConfig() {
  streams_config_.at_time = Timestamp::Millis(clock_->TimeInMilliseconds());
  NetworkControlUpdate update1,update2;
  if (controller_)
    update1=controller_->OnStreamsConfig(streams_config_);
  if(controller_s_)
    update2=controller_s_->OnStreamsConfig(streams_config_);
  PostUpdates(update1,update2);
}

//sandy: Function to detect best path. The path with highest bitrate,lowest RTT and loss
int RtpTransportControllerSend::MpFindBestPath(int64_t rtt1,int64_t rtt2,double loss1,double loss2,double rate1,double rate2){
 

  mpcollector_->MpSetRTT1(rtt1);
  mpcollector_->MpSetRTT2(rtt2);
  mpcollector_->MpSetLoss1(loss1);
  mpcollector_->MpSetLoss2(loss2);

  //sandy: If the rtt is too long then that path should be blocked
  // if(rtt1>3000 || rtt2>3000){
  //    RTC_DLOG(LS_ERROR)<<"sandyofo sender itself blocking the path by setting full signal";
  //    mpcollector_->MpSetFullSignal();
  // }

  double P1time=0;
  double P2time=0;
  int bestpath=0;

  if(rtt1==0 && rtt2==0){
    return 1;
  }
  
  rate1/=1000;
  rate2/=1000;
  //sandy: Find the completion time
  P1time= (double(1/rate1))+double(rtt1/2);
  P2time= (double(1/rate2))+double(rtt2/2);
  mpcollector_->MpSetLossBasedPathId(0);
  if(P1time<=P2time)
    bestpath=1;
  else
    bestpath=2;


  // if(std::abs(rtt1-rtt2)<50 && (loss1>=0.25 || loss2>=0.25) ){
  //   if(loss1>=0.25 && loss1>loss2){
  //     bestpath=2;
  //     mpcollector_->MpSetLossBasedPathId(bestpath);
  //   }else if(loss2>=0.25 && loss2>loss1){
  //     bestpath=1;
  //     mpcollector_->MpSetLossBasedPathId(bestpath);
  //   }
  // }else{
  // if(rtt1<100 && rtt2 <100){
  if(loss1>=0.1 && loss2>=0.1){
    if(loss1>loss2){
      bestpath=2;
      mpcollector_->MpSetLossBasedPathId(bestpath);
    }
    else{
      bestpath=1;
      mpcollector_->MpSetLossBasedPathId(bestpath);
    }
  }else if(loss1>=.1 && loss1>loss2){
    bestpath=2;
    mpcollector_->MpSetLossBasedPathId(bestpath);
  }else if(loss2>=0.1 && loss2>loss1){
    bestpath=1;
    mpcollector_->MpSetLossBasedPathId(bestpath);
  }
  // }
  // }


  //sandy: If the loss is small and rtt difference is small then choose the path with highest bitrate as best
  if((std::abs(rate1-rate2)>=(0.75*(std::max(rate1,rate2))) && std::abs(rtt1-rtt2)<25)|| 
    ((std::abs(rate1-rate2)>=(0.75*(std::max(rate1,rate2)))) && loss1<0.1 && loss2<0.1 && std::abs(rtt1-rtt2)<25)|| 
    (std::abs(rate1-rate2)>=(0.75*(std::max(rate1,rate2))) && loss1<=0.0 && loss2<=0.0 && std::abs(rtt1-rtt2)<50)){
  // if(loss1<0.15 && loss2 <0.15 && std::abs(rtt1-rtt2)<25){ //&& std::abs(rate1-rate2)>=95*(std::max(rate1,rate2))/10){
    if(rate1>rate2)
      bestpath=1;
    else
      bestpath=2;
  }
  RTC_LOG(INFO)<<"sandyratio: bestpath= "<<bestpath<<" C1 time= "<<P1time<<" c2 time= "<<P2time;
  //sandy: Check if there is Halfsignaled RTT set and if check if RTT has reduced and if then clear the half signal
  if(rtt1>0 && mpcollector_->MpGetHalfSignal()<1.0 && mpcollector_->MpGetHalfSignaledPathId()==1 && 
    (rtt1<=mpcollector_->MpGetHalfSignaledRtt()/4)){//&&  rtt1<100 
    if(mpcollector_->MpGetHalfSignalLoss()!=0 && mpcollector_->MpGetLoss1()<mpcollector_->MpGetHalfSignalLoss()){//sandy: If the loss is also set
      RTC_LOG(LS_INFO)<<"sandyofo clearing the half signal set on path 1 old RTT:"<<mpcollector_->MpGetHalfSignaledRtt() 
      <<" new RTT: "<<rtt1<<" old loss: "<<mpcollector_->MpGetHalfSignalLoss()<<" new loss "<<mpcollector_->MpGetLoss1();
      mpcollector_->MpClearHalfSignal();
    }else{
      RTC_LOG(LS_INFO)<<"sandyofo clearing the half signal set on path 1 old RTT:" 
      <<mpcollector_->MpGetHalfSignaledRtt()<<" new RTT: "<<rtt1;
      mpcollector_->MpClearHalfSignal();
    }
  }else if(rtt2>0 && mpcollector_->MpGetHalfSignal()<1.0 && mpcollector_->MpGetHalfSignaledPathId()==2 && 
    (rtt2<=mpcollector_->MpGetHalfSignaledRtt()/4)){ //&&   rtt2<100 
    if(mpcollector_->MpGetHalfSignalLoss()!=0 && mpcollector_->MpGetLoss2()<mpcollector_->MpGetHalfSignalLoss()){//sandy: If the loss is also set
      RTC_LOG(LS_INFO)<<"sandyofo clearing the half signal set on path 2 old RTT:"<<mpcollector_->MpGetHalfSignaledRtt() 
      <<" new RTT: "<<rtt2<<" old loss: "<<mpcollector_->MpGetHalfSignalLoss()<<" new loss "<<mpcollector_->MpGetLoss2();
      mpcollector_->MpClearHalfSignal();
    }else{
      RTC_LOG(LS_INFO)<<"sandyofo clearing the half signal set on path 2 old RTT:"
      <<mpcollector_->MpGetHalfSignaledRtt()<<" new RTT: "<<rtt2;
      mpcollector_->MpClearHalfSignal();
    }
  }
  return bestpath;
}

void RtpTransportControllerSend::MpFindRatio(int64_t rtt1,int64_t rtt2,int64_t rate1,int64_t rate2,int pathid){
  double mpratio;
  
  
  if(pathid==1)
    mpratio=double(double (rate1)/double(rate2));
  else
    mpratio=double(double (rate2)/double(rate1));
  if(rtt1==0 && rtt2==0){
    mpratio=0;
  }

  RTC_LOG(INFO)<<"sandyratio: the scheduler ratio= "<<mpratio<<" P1 Rate "<<rate1<<" bps "<<" P2 Rate "<<rate2<<" bps "<<" RTT1 "<<rtt1<<" ms "<<" RTT2 "<<rtt2<<" ms ";
  mpcollector_->MpSetRatio(mpratio);
}

void RtpTransportControllerSend::PostUpdates(NetworkControlUpdate update,NetworkControlUpdate update2) {
  int window_scheduler=0;


  if(mpcollector_->MpGetScheduler().find("window")!=std::string::npos&&mpcollector_->MpISsecondPathOpen())
    window_scheduler=1;

  if((mpcollector_->MpGetScheduler().find("red")!=std::string::npos) || !mpcollector_->MpISsecondPathOpen() ||
  (mpcollector_->MpGetFullSignal() && mpcollector_->MpGetFullSignaledPathId()==2 && 
  mpcollector_->MpISsecondPathOpen())){//sandy: You have to check if redundant is in place or Fullsignal is set
    if (update.congestion_window) {
      pp_congestion_window=update.congestion_window;
      pacer()->SetCongestionWindow(*update.congestion_window);
    }
    if (update.pacer_config) {
      pacer()->SetPacingRates(update.pacer_config->data_rate(),
                              update.pacer_config->pad_rate());
      pp_pacer_config=update.pacer_config;
    }
    for (const auto& probe : update.probe_cluster_configs) {
      pacer()->CreateProbeCluster(probe.target_data_rate, probe.id);
    }
    if (update.target_rate) {
      control_handler_->SetTargetRate(*update.target_rate);
      UpdateControlState();
      pp_target_rate=update.target_rate;
    }
    // RTC_LOG(LS_INFO)<<"sandyofo the single path is triggered and primary path is used";
  }else if(mpcollector_->MpGetFullSignal() && mpcollector_->MpGetFullSignaledPathId()==1 && 
  mpcollector_->MpISsecondPathOpen()){//sandy: If the primary path has received the signal
    // RTC_LOG(LS_INFO)<<"sandyofo the single path is triggered and secondary path is used";
    if (update2.congestion_window) {
      sp_congestion_window=update2.congestion_window;
      pacer()->SetCongestionWindow(*update2.congestion_window);
    }
    if (update2.pacer_config) {
      pacer()->SetPacingRates(update2.pacer_config->data_rate(),
                              update2.pacer_config->pad_rate());
      sp_pacer_config=update2.pacer_config;
    }
    for (const auto& probe : update2.probe_cluster_configs) {
      pacer()->CreateProbeCluster(probe.target_data_rate, probe.id);
    }
    if (update2.target_rate) {
      control_handler_->SetTargetRate(*update2.target_rate);
      UpdateControlState();
      sp_target_rate=update2.target_rate;
    }
  }else{
    if(update.congestion_window && update2.congestion_window){
      // RTC_LOG(INFO)<<"sandysched pacer congestion window: P1 and P2";
      sp_congestion_window=update2.congestion_window;
      pp_congestion_window=update.congestion_window;
      pacer()->SetCongestionWindow(*update.congestion_window+*update2.congestion_window);
    }else if(!update.congestion_window && update2.congestion_window){
      // RTC_LOG(INFO)<<"sandysched pacer congestion window: P2 only";
      sp_congestion_window=update2.congestion_window;
      pacer()->SetCongestionWindow(*pp_congestion_window+*update2.congestion_window);
    }else if(update.congestion_window && !update2.congestion_window){
      // RTC_LOG(INFO)<<"sandysched pacer congestion window: P1 only";
      pp_congestion_window=update.congestion_window;
      pacer()->SetCongestionWindow(*update.congestion_window+*sp_congestion_window);
    }
    //Pacing rates
    if (update.pacer_config && update2.pacer_config) {
      RTC_LOG(INFO)<<"sandysched pacer rate: P1 and P2";
      pp_pacer_config=update.pacer_config;
      sp_pacer_config=update2.pacer_config;
      pacer()->SetPacingRates(update.pacer_config->data_rate()+update2.pacer_config->data_rate(),
                              update.pacer_config->pad_rate()+update2.pacer_config->pad_rate());
      // if(mpcollector_->MpGetPrimaryWindow()<=0)
        mpcollector_->MpSetPrimaryWindow(pp_pacer_config->data_window.bytes()/2.5);
      // if(mpcollector_->MpGetSecondaryWindow()<=0)
        mpcollector_->MpSetSecondaryWindow(sp_pacer_config->data_window.bytes()/2.5);
      RTC_LOG(INFO)<<"sandysched the P1 window: "<<pp_pacer_config->data_window.bytes()/2.5<<" : "<<pp_pacer_config->pad_window.bytes() 
      <<"P2 window: "<<sp_pacer_config->data_window.bytes()/2.5<<" : "<<sp_pacer_config->pad_window.bytes(); 
    }else if(!update.pacer_config && update2.pacer_config){
      RTC_LOG(INFO)<<"sandysched pacer rate: P2 only";
      sp_pacer_config=update2.pacer_config;
      // if(mpcollector_->MpGetSecondaryWindow()<=0)
        mpcollector_->MpSetSecondaryWindow(sp_pacer_config->data_window.bytes()/2.5);
      pacer()->SetPacingRates(pp_pacer_config->data_rate()+update2.pacer_config->data_rate(),
                                pp_pacer_config->pad_rate()+update2.pacer_config->pad_rate());
        // if(mpcollector_->MpGetPrimaryWindow()<=0)
          mpcollector_->MpSetPrimaryWindow(pp_pacer_config->data_window.bytes()/2.5);
        RTC_LOG(INFO)<<"sandysched the P1 window: "<<pp_pacer_config->data_window.bytes()/2.5<<" : "<<pp_pacer_config->pad_window.bytes() 
      <<"P2 window: "<<sp_pacer_config->data_window.bytes()/2.5<<" : "<<sp_pacer_config->pad_window.bytes(); 
      pacer()->SetPacingRates(update2.pacer_config->data_rate()+update2.pacer_config->data_rate(),
                                update2.pacer_config->pad_rate()+update2.pacer_config->pad_rate());
    }else if (update.pacer_config && !update2.pacer_config){
      RTC_LOG(INFO)<<"sandysched pacer rate: P1 only";
      pp_pacer_config=update.pacer_config;
      // if(mpcollector_->MpGetPrimaryWindow()<=0)
        mpcollector_->MpSetPrimaryWindow(pp_pacer_config->data_window.bytes()/2.5);
      
      if(sp_pacer_config){
        pacer()->SetPacingRates(update.pacer_config->data_rate()+sp_pacer_config->data_rate(),
                                update.pacer_config->pad_rate()+sp_pacer_config->pad_rate());
        // if(mpcollector_->MpGetSecondaryWindow()<=0)
          mpcollector_->MpSetSecondaryWindow(sp_pacer_config->data_window.bytes()/2.5);
        RTC_LOG(INFO)<<"sandysched the P1 window: "<<pp_pacer_config->data_window.bytes()/2.5<<" : "<<pp_pacer_config->pad_window.bytes() 
      <<"P2 window: "<<sp_pacer_config->data_window.bytes()/2.5<<" : "<<sp_pacer_config->pad_window.bytes(); 
      pacer()->SetPacingRates(update.pacer_config->data_rate()+update.pacer_config->data_rate(),
                                update.pacer_config->pad_rate()+update.pacer_config->pad_rate());
      }
    }
    //probing 
    for (const auto& probe : update.probe_cluster_configs) {
      pacer()->CreateProbeCluster(probe.target_data_rate*2, probe.id);//sandy: For probing I will consider sum
    }
    //Target rate
    if (update.target_rate && update2.target_rate) {
      // RTC_LOG(INFO)<<"sandysched  received P1 and P2 updates";
      pp_target_rate=update.target_rate;
      sp_target_rate=update2.target_rate;
      RTC_LOG(INFO)<<"sandyratio the P1 & p2 rate update: "<<update.target_rate->target_rate.bps()  
      <<"P2 rate: "<<update2.target_rate->target_rate.bps()<<"P1 : P2 RTT"<<update.target_rate->network_estimate.round_trip_time.ms()<<":"<<  
      update2.target_rate->network_estimate.round_trip_time.ms()<<"Loss report= "<<update.target_rate->network_estimate.loss_rate_ratio<<":"<< 
      update2.target_rate->network_estimate.loss_rate_ratio;
      //sandy:Set the best path
      mpcollector_->MpSetBestPathId(MpFindBestPath(update.target_rate->network_estimate.round_trip_time.ms(), //RTT1
                      update2.target_rate->network_estimate.round_trip_time.ms(), //RTT2
                      update.target_rate->network_estimate.loss_rate_ratio, //Loss1
                      update2.target_rate->network_estimate.loss_rate_ratio, //Loss 2
                      update.target_rate->target_rate.bps(),  //Rate 1
                      update2.target_rate->target_rate.bps())); //Rate 2
      MpFindRatio(update.target_rate->network_estimate.round_trip_time.ms(),update2.target_rate->network_estimate.round_trip_time.ms(), 
          update.target_rate->target_rate.bps(),update2.target_rate->target_rate.bps(),mpcollector_->MpGetBestPathId());

      update.target_rate->target_rate+=update2.target_rate->target_rate;
      update.target_rate->stable_target_rate+=update2.target_rate->stable_target_rate;
      update.target_rate->network_estimate.bandwidth+=update2.target_rate->network_estimate.bandwidth;
      update.target_rate->cwnd_reduce_ratio+=update2.target_rate->cwnd_reduce_ratio;
      update.target_rate->network_estimate.loss_rate_ratio=std::min(update.target_rate->network_estimate.loss_rate_ratio, 
        update2.target_rate->network_estimate.loss_rate_ratio);
      //For RTT and BEW just keep the maxium of two
      update.target_rate->network_estimate.round_trip_time=std::min(update.target_rate->network_estimate.round_trip_time, 
        update2.target_rate->network_estimate.round_trip_time);
      update.target_rate->network_estimate.bwe_period=std::min(update.target_rate->network_estimate.bwe_period, 
        update2.target_rate->network_estimate.bwe_period);

      
      control_handler_->SetTargetRate(*update.target_rate);
      UpdateControlState();
    }else if(!update.target_rate && update2.target_rate){
      // RTC_LOG(INFO)<<"sandysched  received P2 updates only";
      sp_target_rate=update2.target_rate;
      if(pp_target_rate){
        RTC_LOG(INFO)<<"sandyratio the P2 only rate update: "<<pp_target_rate->target_rate.bps()  
        <<"P2 rate: "<<update2.target_rate->target_rate.bps()<<"P1 : P2 RTT"<<pp_target_rate->network_estimate.round_trip_time.ms()<<":"<<  
        update2.target_rate->network_estimate.round_trip_time.ms()<<"Loss report= "<<pp_target_rate->network_estimate.loss_rate_ratio<<":"<< 
        update2.target_rate->network_estimate.loss_rate_ratio; 

        mpcollector_->MpSetBestPathId(MpFindBestPath(pp_target_rate->network_estimate.round_trip_time.ms(), //RTT1
                      update2.target_rate->network_estimate.round_trip_time.ms(), //RTT2
                      pp_target_rate->network_estimate.loss_rate_ratio, //Loss1
                      update2.target_rate->network_estimate.loss_rate_ratio, //Loss 2
                      pp_target_rate->target_rate.bps(),  //Rate 1
                      update2.target_rate->target_rate.bps())); //Rate 2
        MpFindRatio(pp_target_rate->network_estimate.round_trip_time.ms(), //RTT1
                  update2.target_rate->network_estimate.round_trip_time.ms(), //RTT2
                  pp_target_rate->target_rate.bps(),  //Rate 1
                  update2.target_rate->target_rate.bps(),mpcollector_->MpGetBestPathId());

        update2.target_rate->target_rate+=pp_target_rate->target_rate;
        update2.target_rate->stable_target_rate+=pp_target_rate->stable_target_rate;
        update2.target_rate->network_estimate.bandwidth+=pp_target_rate->network_estimate.bandwidth;    
        update2.target_rate->cwnd_reduce_ratio+=pp_target_rate->cwnd_reduce_ratio;
        update2.target_rate->network_estimate.loss_rate_ratio=std::min(update2.target_rate->network_estimate.loss_rate_ratio,
          pp_target_rate->network_estimate.loss_rate_ratio);
        //RTT and BEW time sould be of primary paths
        update2.target_rate->network_estimate.round_trip_time=std::min(update2.target_rate->network_estimate.round_trip_time, 
          pp_target_rate->network_estimate.round_trip_time);
        update2.target_rate->network_estimate.bwe_period=std::min(update2.target_rate->network_estimate.bwe_period, 
          pp_target_rate->network_estimate.bwe_period);        
      }else{
        update2.target_rate->target_rate+=update2.target_rate->target_rate;
        update2.target_rate->stable_target_rate+=update2.target_rate->stable_target_rate;
        update2.target_rate->network_estimate.bandwidth+=update2.target_rate->network_estimate.bandwidth;
        update2.target_rate->cwnd_reduce_ratio+=update2.target_rate->cwnd_reduce_ratio;
      }
      control_handler_->SetTargetRate(*update2.target_rate);
      UpdateControlState();
    }else if(update.target_rate && !update2.target_rate){
      // RTC_LOG(INFO)<<"sandysched  received P1 updates only";
      pp_target_rate=update.target_rate;
      if(sp_target_rate){
        RTC_LOG(INFO)<<"sandyratio the P1 only rate update: "<<update.target_rate->target_rate.bps()  
        <<"P2 rate: "<<sp_target_rate->target_rate.bps()<<"P1 : P2 RTT"<<update.target_rate->network_estimate.round_trip_time.ms()<<":"<<  
        sp_target_rate->network_estimate.round_trip_time.ms()<<"Loss report= "<<update.target_rate->network_estimate.loss_rate_ratio<<":"<< 
        sp_target_rate->network_estimate.loss_rate_ratio; 

        //sandy:Set the best path
        mpcollector_->MpSetBestPathId(MpFindBestPath(update.target_rate->network_estimate.round_trip_time.ms(), //RTT1
                      sp_target_rate->network_estimate.round_trip_time.ms(), //RTT2
                      update.target_rate->network_estimate.loss_rate_ratio, //Loss1
                      sp_target_rate->network_estimate.loss_rate_ratio, //Loss 2
                      update.target_rate->target_rate.bps(),  //Rate 1
                      sp_target_rate->target_rate.bps())); //Rate 2
        MpFindRatio(update.target_rate->network_estimate.round_trip_time.ms(), //RTT1
                  sp_target_rate->network_estimate.round_trip_time.ms(), //RTT2
                  update.target_rate->target_rate.bps(),  //Rate 1
                  sp_target_rate->target_rate.bps(),mpcollector_->MpGetBestPathId());

        update.target_rate->target_rate+=sp_target_rate->target_rate;
        update.target_rate->stable_target_rate+=sp_target_rate->stable_target_rate;
        update.target_rate->network_estimate.bandwidth+=sp_target_rate->network_estimate.bandwidth;
        update.target_rate->cwnd_reduce_ratio+=sp_target_rate->cwnd_reduce_ratio;
        update.target_rate->network_estimate.loss_rate_ratio=std::min(update.target_rate->network_estimate.loss_rate_ratio,
          sp_target_rate->network_estimate.loss_rate_ratio);
        //RTT and BEW time sould be of primary paths
        update.target_rate->network_estimate.round_trip_time=std::min(update.target_rate->network_estimate.round_trip_time, 
        sp_target_rate->network_estimate.round_trip_time);
        update.target_rate->network_estimate.bwe_period=std::min(update.target_rate->network_estimate.bwe_period, 
        sp_target_rate->network_estimate.bwe_period);
        
      }else{
        update.target_rate->target_rate+=update.target_rate->target_rate;
        update.target_rate->stable_target_rate+=update.target_rate->stable_target_rate;
        update.target_rate->network_estimate.bandwidth+=update.target_rate->network_estimate.bandwidth;
        update.target_rate->cwnd_reduce_ratio+=update.target_rate->cwnd_reduce_ratio;
        //Loss,RTT and BEW time sould be of primary paths
      }
      control_handler_->SetTargetRate(*update.target_rate);
      UpdateControlState();
    }
    // RTC_LOG(INFO)<<"**** sandysched ****";

  }
}

void RtpTransportControllerSend::OnReceivedRtcpReceiverReportBlocks(
    const ReportBlockList& report_blocks,
    int64_t now_ms,int pathid) {
  if (report_blocks.empty())
    return;

  int total_packets_lost_delta = 0;
  int total_packets_delta = 0;

  // Compute the packet loss from all report blocks.
  if(pathid!=2){
    for (const RTCPReportBlock& report_block : report_blocks) {
      auto it = last_report_blocks_.find(report_block.source_ssrc);
      if (it != last_report_blocks_.end()) {
        auto number_of_packets = report_block.extended_highest_sequence_number -
                                 it->second.extended_highest_sequence_number;
        total_packets_delta += number_of_packets;
        auto lost_delta = report_block.packets_lost - it->second.packets_lost;
        RTC_LOG(INFO)<<"sandystats loss report "<<lost_delta<<" pathid= "<<pathid<< "prev"<< 
        it->second.packets_lost<<" current "<<report_block.packets_lost;
        if(lost_delta<0){
          lost_delta*=-1;
          if(lost_delta>65535)
            lost_delta=0;
        }
        total_packets_lost_delta += lost_delta;
      }
      last_report_blocks_[report_block.source_ssrc] = report_block;
    }
  }else if(pathid==2){
    RTC_LOG(INFO)<<"sandyred this is non-red scheduler";
    for (const RTCPReportBlock& report_block : report_blocks) {
      auto it = last_report_blocks_s_.find(report_block.source_ssrc);
      if (it != last_report_blocks_s_.end()) {
        auto number_of_packets = report_block.extended_highest_sequence_number -
                                 it->second.extended_highest_sequence_number;
        total_packets_delta += number_of_packets;
        auto lost_delta = report_block.packets_lost - it->second.packets_lost;
        // RTC_LOG(INFO)<<"sandystats loss report "<<lost_delta<<" pathid= "<<pathid;
        if(lost_delta<0){
          lost_delta*=-1;
          if(lost_delta>65535)
            lost_delta=0;
        }        
        total_packets_lost_delta += lost_delta;
      }
      last_report_blocks_s_[report_block.source_ssrc] = report_block;
    }
  }
  // Can only compute delta if there has been previous blocks to compare to. If
  // not, total_packets_delta will be unchanged and there's nothing more to do.
  if (!total_packets_delta)
    return;
  int packets_received_delta = total_packets_delta - total_packets_lost_delta;
  // To detect lost packets, at least one packet has to be received. This check
  // is needed to avoid bandwith detection update in
  // VideoSendStreamTest.SuspendBelowMinBitrate

  if (packets_received_delta < 1)
    return;
  Timestamp now = Timestamp::Millis(now_ms);
  TransportLossReport msg;
  msg.packets_lost_delta = total_packets_lost_delta;
  msg.packets_received_delta = packets_received_delta;
  msg.receive_time = now;
  msg.end_time = now;
  NetworkControlUpdate update;
  if (controller_ && pathid!=2){
    msg.start_time = last_report_block_time_;
    PostUpdates(controller_->OnTransportLossReport(msg),update);
    last_report_block_time_ = now;
  }
  else if(controller_s_ && pathid==2){
    RTC_LOG(INFO)<<"sandyred this is non-red scheduler";
    msg.start_time = last_report_block_time_s_;
    PostUpdates(update,controller_s_->OnTransportLossReport(msg));
    last_report_block_time_s_ = now;
  }
  
}

}  // namespace webrtc