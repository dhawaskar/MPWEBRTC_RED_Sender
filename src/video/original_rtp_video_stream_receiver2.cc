/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "video/rtp_video_stream_receiver2.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <vector>
#include <numeric>


#include "absl/algorithm/container.h"
#include "absl/base/macros.h"
#include "absl/memory/memory.h"
#include "absl/types/optional.h"
#include "media/base/media_constants.h"
#include "modules/pacing/packet_router.h"
#include "modules/remote_bitrate_estimator/include/remote_bitrate_estimator.h"
#include "modules/rtp_rtcp/include/receive_statistics.h"
#include "modules/rtp_rtcp/include/rtp_cvo.h"
#include "modules/rtp_rtcp/include/ulpfec_receiver.h"
#include "modules/rtp_rtcp/source/create_video_rtp_depacketizer.h"
#include "modules/rtp_rtcp/source/rtp_dependency_descriptor_extension.h"
#include "modules/rtp_rtcp/source/rtp_format.h"
#include "modules/rtp_rtcp/source/rtp_generic_frame_descriptor.h"
#include "modules/rtp_rtcp/source/rtp_generic_frame_descriptor_extension.h"
#include "modules/rtp_rtcp/source/rtp_header_extensions.h"
#include "modules/rtp_rtcp/source/rtp_packet_received.h"
#include "modules/rtp_rtcp/source/rtp_rtcp_config.h"
#include "modules/rtp_rtcp/source/video_rtp_depacketizer.h"
#include "modules/rtp_rtcp/source/video_rtp_depacketizer_raw.h"
#include "modules/utility/include/process_thread.h"
#include "modules/video_coding/frame_object.h"
#include "modules/video_coding/h264_sprop_parameter_sets.h"
#include "modules/video_coding/h264_sps_pps_tracker.h"
#include "modules/video_coding/nack_module2.h"
#include "modules/video_coding/packet_buffer.h"
#include "rtc_base/checks.h"
#include "rtc_base/location.h"
#include "rtc_base/logging.h"
#include "rtc_base/experiments/struct_parameters_parser.h"
#include "rtc_base/numerics/safe_minmax.h"
#include "rtc_base/strings/string_builder.h"
#include "system_wrappers/include/field_trial.h"
#include "system_wrappers/include/metrics.h"
#include "system_wrappers/include/ntp_time.h"
#include "video/receive_statistics_proxy2.h"
#include "api/mp_collector.h"
#include "api/mp_global.h"

int64_t sandy_start_time=0;
int64_t sandy_end_time=0;
int64_t sandy_previous_frame_time=0;
int64_t sandy_time_window=0;
int64_t sandy_time_window_trigger=0;
int64_t full_signal_sent_time=0;
int64_t packets_count=0;
int mp_print_count=0;
namespace webrtc {

namespace {
// TODO(philipel): Change kPacketBufferStartSize back to 32 in M63 see:
//                 crbug.com/752886
constexpr int kPacketBufferStartSize = 512;
// constexpr int kPacketBufferMaxSize = 2048;//2048
constexpr int kPacketBufferMaxSize = 8192;//MPRTP
constexpr int linearwindow=100;
// int64_t sandy_key_request=0;
int PacketBufferMaxSize() {
  // The group here must be a positive power of 2, in which case that is used as
  // size. All other values shall result in the default value being used.
  const std::string group_name =
      webrtc::field_trial::FindFullName("WebRTC-PacketBufferMaxSize");
  int packet_buffer_max_size = kPacketBufferMaxSize;
  if (!group_name.empty() &&
      (sscanf(group_name.c_str(), "%d", &packet_buffer_max_size) != 1 ||
       packet_buffer_max_size <= 0 ||
       // Verify that the number is a positive power of 2.
       (packet_buffer_max_size & (packet_buffer_max_size - 1)) != 0)) {
    RTC_LOG(LS_WARNING) << "Invalid packet buffer max size: " << group_name;
    packet_buffer_max_size = kPacketBufferMaxSize;
  }
  return packet_buffer_max_size;
}

std::unique_ptr<ModuleRtpRtcpImpl2> CreateRtpRtcpModule(
    Clock* clock,
    ReceiveStatistics* receive_statistics,
    Transport* outgoing_transport,
    RtcpRttStats* rtt_stats,
    RtcpPacketTypeCounterObserver* rtcp_packet_type_counter_observer,
    RtcpCnameCallback* rtcp_cname_callback,
    uint32_t local_ssrc) {
  RtpRtcpInterface::Configuration configuration;
  configuration.clock = clock;
  configuration.audio = false;
  configuration.receiver_only = true;
  configuration.receive_statistics = receive_statistics;
  configuration.outgoing_transport = outgoing_transport;
  configuration.rtt_stats = rtt_stats;
  configuration.rtcp_packet_type_counter_observer =
      rtcp_packet_type_counter_observer;
  configuration.rtcp_cname_callback = rtcp_cname_callback;
  configuration.local_media_ssrc = local_ssrc;

  std::unique_ptr<ModuleRtpRtcpImpl2> rtp_rtcp =
      ModuleRtpRtcpImpl2::Create(configuration);
  rtp_rtcp->SetRTCPStatus(RtcpMode::kCompound);

  return rtp_rtcp;
}

std::unique_ptr<NackModule2> MaybeConstructNackModule(
    TaskQueueBase* current_queue,
    const VideoReceiveStream::Config& config,
    Clock* clock,
    NackSender* nack_sender,
    KeyFrameRequestSender* keyframe_request_sender,int pathid) {
  if (config.rtp.nack.rtp_history_ms == 0)
    return nullptr;

  return std::make_unique<NackModule2>(current_queue, clock, nack_sender,
                                       keyframe_request_sender,TimeDelta::Millis(20),pathid);
}

static const int kPacketLogIntervalMs = 10000;

}  // namespace

RtpVideoStreamReceiver2::RtcpFeedbackBuffer::RtcpFeedbackBuffer(
    KeyFrameRequestSender* key_frame_request_sender,
    NackSender* nack_sender,
    LossNotificationSender* loss_notification_sender)
    : key_frame_request_sender_(key_frame_request_sender),
      nack_sender_(nack_sender),
      loss_notification_sender_(loss_notification_sender),
      request_key_frame_(false) {
  RTC_DCHECK(key_frame_request_sender_);
  RTC_DCHECK(nack_sender_);
  RTC_DCHECK(loss_notification_sender_);
}

void RtpVideoStreamReceiver2::RtcpFeedbackBuffer::RequestKeyFrame() {
  // RTC_LOG(INFO)<<"sandyofo the OFO increasing and buffer is full ";
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  request_key_frame_ = true;
}

void RtpVideoStreamReceiver2::RtcpFeedbackBuffer::SendNack(
    const std::vector<uint16_t>& sequence_numbers,
    bool buffering_allowed,int pathid) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  RTC_DCHECK(!sequence_numbers.empty());
  if(pathid!=2){//sandy: Primary path NACK list
    nack_sequence_numbers_p_.insert(nack_sequence_numbers_p_.end(),
                                sequence_numbers.cbegin(),
                                sequence_numbers.cend());
  }else{
    nack_sequence_numbers_s_.insert(nack_sequence_numbers_s_.end(),
                                sequence_numbers.cbegin(),
                                sequence_numbers.cend());
  }//sandy: secondary path NACK list
  if (!buffering_allowed) {
    // Note that while *buffering* is not allowed, *batching* is, meaning that
    // previously buffered messages may be sent along with the current message.
    SendBufferedRtcpFeedback();
  }
}

void RtpVideoStreamReceiver2::RtcpFeedbackBuffer::SendLossNotification(
    uint16_t last_decoded_seq_num,
    uint16_t last_received_seq_num,
    bool decodability_flag,
    bool buffering_allowed) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  RTC_DCHECK(buffering_allowed);
  RTC_DCHECK(!lntf_state_)
      << "SendLossNotification() called twice in a row with no call to "
         "SendBufferedRtcpFeedback() in between.";
  lntf_state_ = absl::make_optional<LossNotificationState>(
      last_decoded_seq_num, last_received_seq_num, decodability_flag);
}

void RtpVideoStreamReceiver2::RtcpFeedbackBuffer::SendBufferedRtcpFeedback() {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);

  bool request_key_frame = false;
  std::vector<uint16_t> nack_sequence_numbers_p,nack_sequence_numbers_s;
  absl::optional<LossNotificationState> lntf_state;

  std::swap(request_key_frame, request_key_frame_);
  std::swap(nack_sequence_numbers_p, nack_sequence_numbers_p_);
  std::swap(nack_sequence_numbers_s, nack_sequence_numbers_s_);
  std::swap(lntf_state, lntf_state_);

  if (lntf_state) {
    // If either a NACK or a key frame request is sent, we should buffer
    // the LNTF and wait for them (NACK or key frame request) to trigger
    // the compound feedback message.
    // Otherwise, the LNTF should be sent out immediately.
    const bool buffering_allowed =
        request_key_frame || !nack_sequence_numbers_p.empty()||!nack_sequence_numbers_s.empty();

    loss_notification_sender_->SendLossNotification(
        lntf_state->last_decoded_seq_num, lntf_state->last_received_seq_num,
        lntf_state->decodability_flag, buffering_allowed);
  }

  if (request_key_frame) {
    // RTC_LOG(INFO)<<"sandyofo the OFO increasing and buffer is full ";
    key_frame_request_sender_->RequestKeyFrame();
  } 
  else{ 
    if (!nack_sequence_numbers_p.empty())
      nack_sender_->SendNack(nack_sequence_numbers_p, true,1);//calls the video_receiver_stream2.cc file
    if(!nack_sequence_numbers_s.empty())
      nack_sender_->SendNack(nack_sequence_numbers_s, true,2);//calls the video_receiver_stream2.cc file
  }
}

RtpVideoStreamReceiver2::RtpVideoStreamReceiver2(
    TaskQueueBase* current_queue,
    Clock* clock,
    Transport* transport,
    RtcpRttStats* rtt_stats,
    PacketRouter* packet_router,
    const VideoReceiveStream::Config* config,
    ReceiveStatistics* rtp_receive_statistics,
    RtcpPacketTypeCounterObserver* rtcp_packet_type_counter_observer,
    RtcpCnameCallback* rtcp_cname_callback,
    ProcessThread* process_thread,
    NackSender* nack_sender,
    KeyFrameRequestSender* keyframe_request_sender,
    video_coding::OnCompleteFrameCallback* complete_frame_callback,
    rtc::scoped_refptr<FrameDecryptorInterface> frame_decryptor,
    rtc::scoped_refptr<FrameTransformerInterface> frame_transformer)
    : clock_(clock),
      config_(*config),
      packet_router_(packet_router),
      process_thread_(process_thread),
      ntp_estimator_(clock),
      rtp_header_extensions_(config_.rtp.extensions),
      forced_playout_delay_max_ms_("max_ms", absl::nullopt),
      forced_playout_delay_min_ms_("min_ms", absl::nullopt),
      rtp_receive_statistics_(rtp_receive_statistics),
      ulpfec_receiver_(UlpfecReceiver::Create(config->rtp.remote_ssrc,
                                              this,
                                              config->rtp.extensions)),
      receiving_(false),
      last_packet_log_ms_(-1),
      rtp_rtcp_(CreateRtpRtcpModule(clock,
                                    rtp_receive_statistics_,
                                    transport,
                                    rtt_stats,
                                    rtcp_packet_type_counter_observer,
                                    rtcp_cname_callback,
                                    config_.rtp.local_ssrc)),
      complete_frame_callback_(complete_frame_callback),
      keyframe_request_sender_(keyframe_request_sender),
      // TODO(bugs.webrtc.org/10336): Let |rtcp_feedback_buffer_| communicate
      // directly with |rtp_rtcp_|.
      rtcp_feedback_buffer_(this, nack_sender, this),
      nack_module_(MaybeConstructNackModule(current_queue,
                                            config_,
                                            clock_,
                                            &rtcp_feedback_buffer_,
                                            &rtcp_feedback_buffer_,1)),
      nack_module_s_(MaybeConstructNackModule(current_queue,
                                            config_,
                                            clock_,
                                            &rtcp_feedback_buffer_,
                                            &rtcp_feedback_buffer_,2)),
      packet_buffer_(clock_, kPacketBufferStartSize, PacketBufferMaxSize()),
      has_received_frame_(false),
      frames_decryptable_(false),
      absolute_capture_time_receiver_(clock) {
  constexpr bool remb_candidate = true;
  if (packet_router_)
    packet_router_->AddReceiveRtpModule(rtp_rtcp_.get(), remb_candidate);

  RTC_DCHECK(config_.rtp.rtcp_mode != RtcpMode::kOff)
      << "A stream should not be configured with RTCP disabled. This value is "
         "reserved for internal usage.";
  // TODO(pbos): What's an appropriate local_ssrc for receive-only streams?
  RTC_DCHECK(config_.rtp.local_ssrc != 0);
  RTC_DCHECK(config_.rtp.remote_ssrc != config_.rtp.local_ssrc);

  rtp_rtcp_->SetRTCPStatus(config_.rtp.rtcp_mode);
  rtp_rtcp_->SetRemoteSSRC(config_.rtp.remote_ssrc);

  static const int kMaxPacketAgeToNack = 450;
  const int max_reordering_threshold = (config_.rtp.nack.rtp_history_ms > 0)
                                           ? kMaxPacketAgeToNack
                                           : kDefaultMaxReorderingThreshold;
  rtp_receive_statistics_->SetMaxReorderingThreshold(config_.rtp.remote_ssrc,
                                                     max_reordering_threshold);
  // TODO(nisse): For historic reasons, we applied the above
  // max_reordering_threshold also for RTX stats, which makes little sense since
  // we don't NACK rtx packets. Consider deleting the below block, and rely on
  // the default threshold.
  if (config_.rtp.rtx_ssrc) {
    rtp_receive_statistics_->SetMaxReorderingThreshold(
        config_.rtp.rtx_ssrc, max_reordering_threshold);
  }
  if (config_.rtp.rtcp_xr.receiver_reference_time_report)
    rtp_rtcp_->SetRtcpXrRrtrStatus(true);

  ParseFieldTrial(
      {&forced_playout_delay_max_ms_, &forced_playout_delay_min_ms_},
      field_trial::FindFullName("WebRTC-ForcePlayoutDelay"));

  process_thread_->RegisterModule(rtp_rtcp_.get(), RTC_FROM_HERE);

  if (config_.rtp.lntf.enabled) {
    loss_notification_controller_ =
        std::make_unique<LossNotificationController>(&rtcp_feedback_buffer_,
                                                     &rtcp_feedback_buffer_);
  }

  reference_finder_ =
      std::make_unique<video_coding::RtpFrameReferenceFinder>(this);

  // Only construct the encrypted receiver if frame encryption is enabled.
  if (config_.crypto_options.sframe.require_frame_encryption) {
    buffered_frame_decryptor_ =
        std::make_unique<BufferedFrameDecryptor>(this, this);
    if (frame_decryptor != nullptr) {
      buffered_frame_decryptor_->SetFrameDecryptor(std::move(frame_decryptor));
    }
  }

  if (frame_transformer) {
    frame_transformer_delegate_ = new rtc::RefCountedObject<
        RtpVideoStreamReceiverFrameTransformerDelegate>(
        this, std::move(frame_transformer), rtc::Thread::Current(),
        config_.rtp.remote_ssrc);
    frame_transformer_delegate_->Init();
  }
}

RtpVideoStreamReceiver2::~RtpVideoStreamReceiver2() {
  RTC_DCHECK(secondary_sinks_.empty());

  process_thread_->DeRegisterModule(rtp_rtcp_.get());

  if (packet_router_)
    packet_router_->RemoveReceiveRtpModule(rtp_rtcp_.get());
  UpdateHistograms();
  if (frame_transformer_delegate_)
    frame_transformer_delegate_->Reset();
}

void RtpVideoStreamReceiver2::AddReceiveCodec(
    const VideoCodec& video_codec,
    const std::map<std::string, std::string>& codec_params,
    bool raw_payload) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  payload_type_map_.emplace(
      video_codec.plType,
      raw_payload ? std::make_unique<VideoRtpDepacketizerRaw>()
                  : CreateVideoRtpDepacketizer(video_codec.codecType));
  pt_codec_params_.emplace(video_codec.plType, codec_params);
}

absl::optional<Syncable::Info> RtpVideoStreamReceiver2::GetSyncInfo() const {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  Syncable::Info info;
  if (rtp_rtcp_->RemoteNTP(&info.capture_time_ntp_secs,
                           &info.capture_time_ntp_frac, nullptr, nullptr,
                           &info.capture_time_source_clock,1) != 0) {//sandy: Always use path 1 for synchronization
    return absl::nullopt;
  }

  if (!last_received_rtp_timestamp_ || !last_received_rtp_system_time_ms_) {
    return absl::nullopt;
  }
  info.latest_received_capture_timestamp = *last_received_rtp_timestamp_;
  info.latest_receive_time_ms = *last_received_rtp_system_time_ms_;

  // Leaves info.current_delay_ms uninitialized.
  return info;
}

RtpVideoStreamReceiver2::ParseGenericDependenciesResult
RtpVideoStreamReceiver2::ParseGenericDependenciesExtension(
    const RtpPacketReceived& rtp_packet,
    RTPVideoHeader* video_header) {
  if (rtp_packet.HasExtension<RtpDependencyDescriptorExtension>()) {
    webrtc::DependencyDescriptor dependency_descriptor;
    if (!rtp_packet.GetExtension<RtpDependencyDescriptorExtension>(
            video_structure_.get(), &dependency_descriptor)) {
      // Descriptor is there, but failed to parse. Either it is invalid,
      // or too old packet (after relevant video_structure_ changed),
      // or too new packet (before relevant video_structure_ arrived).
      // Drop such packet to be on the safe side.
      // TODO(bugs.webrtc.org/10342): Stash too new packet.
      RTC_LOG(LS_WARNING) << "ssrc: " << rtp_packet.Ssrc()
                          << " Failed to parse dependency descriptor.";
      return kDropPacket;
    }
    if (dependency_descriptor.attached_structure != nullptr &&
        !dependency_descriptor.first_packet_in_frame) {
      RTC_LOG(LS_WARNING) << "ssrc: " << rtp_packet.Ssrc()
                          << "Invalid dependency descriptor: structure "
                             "attached to non first packet of a frame.";
      return kDropPacket;
    }
    video_header->is_first_packet_in_frame =
        dependency_descriptor.first_packet_in_frame;
    video_header->is_last_packet_in_frame =
        dependency_descriptor.last_packet_in_frame;

    int64_t frame_id =
        frame_id_unwrapper_.Unwrap(dependency_descriptor.frame_number);
    auto& generic_descriptor_info = video_header->generic.emplace();
    generic_descriptor_info.frame_id = frame_id;
    generic_descriptor_info.spatial_index =
        dependency_descriptor.frame_dependencies.spatial_id;
    generic_descriptor_info.temporal_index =
        dependency_descriptor.frame_dependencies.temporal_id;
    for (int fdiff : dependency_descriptor.frame_dependencies.frame_diffs) {
      generic_descriptor_info.dependencies.push_back(frame_id - fdiff);
    }
    generic_descriptor_info.decode_target_indications =
        dependency_descriptor.frame_dependencies.decode_target_indications;
    if (dependency_descriptor.resolution) {
      video_header->width = dependency_descriptor.resolution->Width();
      video_header->height = dependency_descriptor.resolution->Height();
    }

    // FrameDependencyStructure is sent in dependency descriptor of the first
    // packet of a key frame and required for parsed dependency descriptor in
    // all the following packets until next key frame.
    // Save it if there is a (potentially) new structure.
    if (dependency_descriptor.attached_structure) {
      RTC_DCHECK(dependency_descriptor.first_packet_in_frame);
      if (video_structure_frame_id_ > frame_id) {
        RTC_LOG(LS_WARNING)
            << "Arrived key frame with id " << frame_id << " and structure id "
            << dependency_descriptor.attached_structure->structure_id
            << " is older than the latest received key frame with id "
            << *video_structure_frame_id_ << " and structure id "
            << video_structure_->structure_id;
        return kDropPacket;
      }
      video_structure_ = std::move(dependency_descriptor.attached_structure);
      video_structure_frame_id_ = frame_id;
      video_header->frame_type = VideoFrameType::kVideoFrameKey;
    } else {
      video_header->frame_type = VideoFrameType::kVideoFrameDelta;
    }
    return kHasGenericDescriptor;
  }

  RtpGenericFrameDescriptor generic_frame_descriptor;
  if (!rtp_packet.GetExtension<RtpGenericFrameDescriptorExtension00>(
          &generic_frame_descriptor)) {
    return kNoGenericDescriptor;
  }

  video_header->is_first_packet_in_frame =
      generic_frame_descriptor.FirstPacketInSubFrame();
  video_header->is_last_packet_in_frame =
      generic_frame_descriptor.LastPacketInSubFrame();

  if (generic_frame_descriptor.FirstPacketInSubFrame()) {
    video_header->frame_type =
        generic_frame_descriptor.FrameDependenciesDiffs().empty()
            ? VideoFrameType::kVideoFrameKey
            : VideoFrameType::kVideoFrameDelta;

    auto& generic_descriptor_info = video_header->generic.emplace();
    int64_t frame_id =
        frame_id_unwrapper_.Unwrap(generic_frame_descriptor.FrameId());
    generic_descriptor_info.frame_id = frame_id;
    generic_descriptor_info.spatial_index =
        generic_frame_descriptor.SpatialLayer();
    generic_descriptor_info.temporal_index =
        generic_frame_descriptor.TemporalLayer();
    for (uint16_t fdiff : generic_frame_descriptor.FrameDependenciesDiffs()) {
      generic_descriptor_info.dependencies.push_back(frame_id - fdiff);
    }
  }
  video_header->width = generic_frame_descriptor.Width();
  video_header->height = generic_frame_descriptor.Height();
  return kHasGenericDescriptor;
}

void RtpVideoStreamReceiver2::OnReceivedPayloadData(
    rtc::CopyOnWriteBuffer codec_payload,
    const RtpPacketReceived& rtp_packet,
    const RTPVideoHeader& video) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  auto packet = std::make_unique<video_coding::PacketBuffer::Packet>(
      rtp_packet, video, ntp_estimator_.Estimate(rtp_packet.Timestamp()),
      clock_->TimeInMilliseconds());

  // Try to extrapolate absolute capture time if it is missing.
  packet->packet_info.set_absolute_capture_time(
      absolute_capture_time_receiver_.OnReceivePacket(
          AbsoluteCaptureTimeReceiver::GetSource(packet->packet_info.ssrc(),
                                                 packet->packet_info.csrcs()),
          packet->packet_info.rtp_timestamp(),
          // Assume frequency is the same one for all video frames.
          kVideoPayloadTypeFrequency,
          packet->packet_info.absolute_capture_time()));

  packet->packet_info.set_sandy_arrival_time(rtp_packet.arrival_time_ms());
  RTPVideoHeader& video_header = packet->video_header;
  video_header.rotation = kVideoRotation_0;
  video_header.content_type = VideoContentType::UNSPECIFIED;
  video_header.video_timing.flags = VideoSendTiming::kInvalid;
  video_header.is_last_packet_in_frame |= rtp_packet.Marker();

  if (const auto* vp9_header =
          absl::get_if<RTPVideoHeaderVP9>(&video_header.video_type_header)) {
    video_header.is_last_packet_in_frame |= vp9_header->end_of_frame;
    video_header.is_first_packet_in_frame |= vp9_header->beginning_of_frame;
  }

  rtp_packet.GetExtension<VideoOrientation>(&video_header.rotation);
  rtp_packet.GetExtension<VideoContentTypeExtension>(
      &video_header.content_type);
  rtp_packet.GetExtension<VideoTimingExtension>(&video_header.video_timing);
  if (forced_playout_delay_max_ms_ && forced_playout_delay_min_ms_) {
    video_header.playout_delay.max_ms = *forced_playout_delay_max_ms_;
    video_header.playout_delay.min_ms = *forced_playout_delay_min_ms_;
  } else {
    rtp_packet.GetExtension<PlayoutDelayLimits>(&video_header.playout_delay);
  }

  ParseGenericDependenciesResult generic_descriptor_state =
      ParseGenericDependenciesExtension(rtp_packet, &video_header);
  if (generic_descriptor_state == kDropPacket)
    return;

  // Color space should only be transmitted in the last packet of a frame,
  // therefore, neglect it otherwise so that last_color_space_ is not reset by
  // mistake.
  if (video_header.is_last_packet_in_frame) {
    video_header.color_space = rtp_packet.GetExtension<ColorSpaceExtension>();
    if (video_header.color_space ||
        video_header.frame_type == VideoFrameType::kVideoFrameKey) {
      // Store color space since it's only transmitted when changed or for key
      // frames. Color space will be cleared if a key frame is transmitted
      // without color space information.
      last_color_space_ = video_header.color_space;
    } else if (last_color_space_) {
      video_header.color_space = last_color_space_;
    }
  }

  if (loss_notification_controller_) {
    if (rtp_packet.recovered()) {
      // TODO(bugs.webrtc.org/10336): Implement support for reordering.
      RTC_LOG(LS_INFO)
          << "sandystats LossNotificationController does not support reordering.";
    } else if (generic_descriptor_state == kNoGenericDescriptor) {
      RTC_LOG(LS_WARNING) << "LossNotificationController requires generic "
                             "frame descriptor, but it is missing.";
    } else {
      if (video_header.is_first_packet_in_frame) {
        RTC_DCHECK(video_header.generic);
        LossNotificationController::FrameDetails frame;
        frame.is_keyframe =
            video_header.frame_type == VideoFrameType::kVideoFrameKey;
        frame.frame_id = video_header.generic->frame_id;
        frame.frame_dependencies = video_header.generic->dependencies;
        loss_notification_controller_->OnReceivedPacket(
            rtp_packet.SequenceNumber(), &frame);
      } else {
        loss_notification_controller_->OnReceivedPacket(
            rtp_packet.SequenceNumber(), nullptr);
      }
    }
  }
  
  RTPHeader header;
  rtp_packet.GetHeader(&header);
  int pathid=header.extension.sandy;

  if (nack_module_ && pathid!=2) {
    

    const bool is_keyframe =
        video_header.is_first_packet_in_frame &&
        video_header.frame_type == VideoFrameType::kVideoFrameKey;

    //RTC_DCHECK(rtp_packet.subflow_id>0 && rtp_packet.subflow_seq>0);
    // RTC_LOG(INFO)<<"sandystats the packet path= "<<rtp_packet.subflow_id<<" seq= "<<rtp_packet.SequenceNumber()<< 
    // " mp seq= "<<rtp_packet.subflow_seq;
    packet->times_nacked = nack_module_->OnReceivedPacket(header.extension.mpflowseqnum,is_keyframe, rtp_packet.recovered(),pathid);

  } else if(nack_module_s_ && pathid==2){
    const bool is_keyframe =
        video_header.is_first_packet_in_frame &&
        video_header.frame_type == VideoFrameType::kVideoFrameKey;

    //RTC_DCHECK(rtp_packet.subflow_id>0 && rtp_packet.subflow_seq>0);
    // RTC_LOG(INFO)<<"sandystats the packet path= "<<rtp_packet.subflow_id<<" seq= "<<rtp_packet.SequenceNumber()<< 
    // " mp seq= "<<rtp_packet.subflow_seq;
    packet->times_nacked = nack_module_s_->OnReceivedPacket(header.extension.mpflowseqnum,is_keyframe, rtp_packet.recovered(),pathid);
  }
  else {
    packet->times_nacked = -1;
  }

  if (codec_payload.size() == 0) {
    RTC_DCHECK(rtp_packet.subflow_id>0 && rtp_packet.subflow_seq>0);
    NotifyReceiverOfEmptyPacket(packet->seq_num,header.extension.mpflowseqnum,pathid);
    rtcp_feedback_buffer_.SendBufferedRtcpFeedback();
    return;
  }

  if (packet->codec() == kVideoCodecH264) {
    // Only when we start to receive packets will we know what payload type
    // that will be used. When we know the payload type insert the correct
    // sps/pps into the tracker.
    if (packet->payload_type != last_payload_type_) {
      last_payload_type_ = packet->payload_type;
      InsertSpsPpsIntoTracker(packet->payload_type);
    }

    video_coding::H264SpsPpsTracker::FixedBitstream fixed =
        tracker_.CopyAndFixBitstream(
            rtc::MakeArrayView(codec_payload.cdata(), codec_payload.size()),
            &packet->video_header);

    switch (fixed.action) {
      case video_coding::H264SpsPpsTracker::kRequestKeyframe:
        // RTC_LOG(INFO)<<"sandyofo the OFO increasing and buffer is full ";
        rtcp_feedback_buffer_.RequestKeyFrame();
        rtcp_feedback_buffer_.SendBufferedRtcpFeedback();
        ABSL_FALLTHROUGH_INTENDED;
      case video_coding::H264SpsPpsTracker::kDrop:
        return;
      case video_coding::H264SpsPpsTracker::kInsert:
        packet->video_payload = std::move(fixed.bitstream);
        break;
    }

  } else {
    packet->video_payload = std::move(codec_payload);
  }

  rtcp_feedback_buffer_.SendBufferedRtcpFeedback();
  frame_counter_.Add(packet->timestamp);
  OnInsertedPacket(packet_buffer_.InsertPacket(std::move(packet)));
  // //sandykey
  // int64_t now_ms = clock_->TimeInMilliseconds();
  // if(!sandy_key_request){
  //   RequestKeyFrame();
  //   RTC_LOG(INFO)<<"sandykeyframe requested "<<now_ms;
  //   sandy_key_request=now_ms;
  // }else{
  //   if(now_ms- sandy_key_request>1000 ){
  //     RequestKeyFrame();
  //     sandy_key_request=now_ms;
  //     RTC_LOG(INFO)<<"sandykeyframe requested "<<now_ms;
  //   }
  // }
}

void RtpVideoStreamReceiver2::OnRecoveredPacket(const uint8_t* rtp_packet,
                                                size_t rtp_packet_length) {
  RtpPacketReceived packet;
  if (!packet.Parse(rtp_packet, rtp_packet_length))
    return;
  if (packet.PayloadType() == config_.rtp.red_payload_type) {
    RTC_LOG(LS_WARNING) << "Discarding recovered packet with RED encapsulation";
    return;
  }

  packet.IdentifyExtensions(rtp_header_extensions_);
  packet.set_payload_type_frequency(kVideoPayloadTypeFrequency);
  // TODO(nisse): UlpfecReceiverImpl::ProcessReceivedFec passes both
  // original (decapsulated) media packets and recovered packets to
  // this callback. We need a way to distinguish, for setting
  // packet.recovered() correctly. Ideally, move RED decapsulation out
  // of the Ulpfec implementation.

  ReceivePacket(packet);
}

// This method handles both regular RTP packets and packets recovered
// via FlexFEC.
void RtpVideoStreamReceiver2::OnRtpPacket(const RtpPacketReceived& packet) {
  
  RTPHeader header;
  packet.GetHeader(&header);
  // RTC_LOG(INFO)<<"sandyrtx the path id :"<<packet.pathid<<" in header: "<<header.extension.sandy;
  //sandy: Retransmitted packets will have different path id
  RTC_DCHECK(packet.pathid>0 && packet.pathid==header.extension.sandy);
  RTC_DCHECK_RUN_ON(&worker_task_checker_);

  if (!receiving_) {
    return;
  }

  if (!packet.recovered()) {
    // TODO(nisse): Exclude out-of-order packets?
    int64_t now_ms = clock_->TimeInMilliseconds();

    last_received_rtp_timestamp_ = packet.Timestamp();
    last_received_rtp_system_time_ms_ = now_ms;

    // Periodically log the RTP header of incoming packets.
    if (now_ms - last_packet_log_ms_ > kPacketLogIntervalMs) {
      rtc::StringBuilder ss;
      ss << "Packet received on SSRC: " << packet.Ssrc()
         << " with payload type: " << static_cast<int>(packet.PayloadType())
         << ", timestamp: " << packet.Timestamp()
         << ", sequence number: " << packet.SequenceNumber()
         << ", arrival time: " << packet.arrival_time_ms();
      int32_t time_offset;
      if (packet.GetExtension<TransmissionOffset>(&time_offset)) {
        ss << ", toffset: " << time_offset;
      }
      uint32_t send_time;
      if (packet.GetExtension<AbsoluteSendTime>(&send_time)) {
        ss << ", abs send time: " << send_time;
      }
      RTC_LOG(LS_INFO) << ss.str();
      last_packet_log_ms_ = now_ms;
    }
  }

  ReceivePacket(packet);

  // Update receive statistics after ReceivePacket.
  // Receive statistics will be reset if the payload type changes (make sure
  // that the first packet is included in the stats).
  if (!packet.recovered()) {
    rtp_receive_statistics_->OnRtpPacket(packet);
  }

  for (RtpPacketSinkInterface* secondary_sink : secondary_sinks_) {
    secondary_sink->OnRtpPacket(packet);
  }
}

void RtpVideoStreamReceiver2::RequestKeyFrame() {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  // TODO(bugs.webrtc.org/10336): Allow the sender to ignore key frame requests
  // issued by anything other than the LossNotificationController if it (the
  // sender) is relying on LNTF alone.
  if (keyframe_request_sender_) {
    // RTC_LOG(INFO)<<"sandyofo the OFO increasing and buffer is full ";
    keyframe_request_sender_->RequestKeyFrame();
  } else {
    // RTC_LOG(INFO)<<"sandyofo the OFO increasing and buffer is full ";
    rtp_rtcp_->SendPictureLossIndication();
  }
}

void RtpVideoStreamReceiver2::SendLossNotification(
    uint16_t last_decoded_seq_num,
    uint16_t last_received_seq_num,
    bool decodability_flag,
    bool buffering_allowed) {
  RTC_DCHECK(config_.rtp.lntf.enabled);
  rtp_rtcp_->SendLossNotification(last_decoded_seq_num, last_received_seq_num,
                                  decodability_flag, buffering_allowed);
}

bool RtpVideoStreamReceiver2::IsUlpfecEnabled() const {
  return config_.rtp.ulpfec_payload_type != -1;
}

bool RtpVideoStreamReceiver2::IsRetransmissionsEnabled() const {
  return config_.rtp.nack.rtp_history_ms > 0;
}

void RtpVideoStreamReceiver2::RequestPacketRetransmit(
    const std::vector<uint16_t>& sequence_numbers,int pathid) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  rtp_rtcp_->SendNack(sequence_numbers,pathid);
}

bool RtpVideoStreamReceiver2::IsDecryptable() const {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  return frames_decryptable_;
}

double RtpVideoStreamReceiver2::LinearFitSlope(
    const std::deque<RtpVideoStreamReceiver2::Frametimings> & frametimes) {
  RTC_DCHECK(frametimes.size() >= 2);
  // Compute the "center of mass".
  double sum_x = 0;
  double sum_y = 0;
  for (const auto& frametime : frametimes) {
    sum_x += frametime.count;
    sum_y += frametime.times;
  }
  double x_avg = sum_x / frametimes.size();
  double y_avg = sum_y / frametimes.size();
  // Compute the slope k = \sum (x_i-x_avg)(y_i-y_avg) / \sum (x_i-x_avg)^2
  double numerator = 0;
  double denominator = 0;
  for (const auto& frametime : frametimes) {
    double x = frametime.count;
    double y = frametime.times;
    numerator += (x - x_avg) * (y - y_avg);
    denominator += (x - x_avg) * (x - x_avg);
  }
  if (denominator == 0)
    return 0;
  return numerator / denominator;
}

void RtpVideoStreamReceiver2::MpUpdateThreshold(double modified_trend,int64_t now_ms) {
  if (last_update_ms_ == -1)
    last_update_ms_ = now_ms;

  if (fabs(modified_trend) > threshold_ + 15) {
    // Avoid adapting the threshold to big latency spikes, caused e.g.,
    // by a sudden capacity drop.
    last_update_ms_ = now_ms;
    return;
  }

  const double k = fabs(modified_trend) < threshold_ ? 0.039 : 0.0087;
  const int64_t kMaxTimeDeltaMs = 100;
  int64_t time_delta_ms = std::min(now_ms - last_update_ms_, kMaxTimeDeltaMs);
  threshold_ += k * (fabs(modified_trend) - threshold_) * time_delta_ms;
  threshold_ = rtc::SafeClamp(threshold_, 6.f, 600.f);
  last_update_ms_ = now_ms;
}

void RtpVideoStreamReceiver2::OnInsertedPacket(
    video_coding::PacketBuffer::InsertResult result) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  video_coding::PacketBuffer::Packet* first_packet = nullptr;
  int max_nack_count;
  int64_t min_recv_time;
  int64_t max_recv_time;
  int64_t now_ms=clock_->TimeInMilliseconds();
  std::vector<rtc::ArrayView<const uint8_t>> payloads;
  RtpPacketInfos::vector_type packet_infos;

  bool frame_boundary = true;
  
  double arrival_time=clock_->CurrentNtpInMilliseconds();
  packets_count=0;
  for (auto& packet : result.packets) {
    packets_count++;
    // PacketBuffer promisses frame boundaries are correctly set on each
    // packet. Document that assumption with the DCHECKs.
    RTC_DCHECK_EQ(frame_boundary, packet->is_first_packet_in_frame());
    
    if (packet->is_first_packet_in_frame()) {
      first_packet = packet.get();
      max_nack_count = packet->times_nacked;
      min_recv_time = packet->packet_info.receive_time_ms();
      max_recv_time = packet->packet_info.receive_time_ms();
      payloads.clear();
      packet_infos.clear();
      // sandy_start_time=packet->packet_info.receive_time_ms();
      sandy_start_time=packet->packet_info.sandy_arrival_time();
    } else {
      max_nack_count = std::max(max_nack_count, packet->times_nacked);
      min_recv_time =
          std::min(min_recv_time, packet->packet_info.receive_time_ms());
      max_recv_time =
          std::max(max_recv_time, packet->packet_info.receive_time_ms());
    }
    payloads.emplace_back(packet->video_payload);
    packet_infos.push_back(packet->packet_info);

    frame_boundary = packet->is_last_packet_in_frame();
    if (packet->is_last_packet_in_frame()) {
      
      if(mp_first_arrival_time_ms_<0)
        mp_first_arrival_time_ms_=arrival_time;
      // sandy_end_time=packet->packet_info.receive_time_ms();
      sandy_end_time=packet->packet_info.sandy_arrival_time();;
      auto depacketizer_it = payload_type_map_.find(first_packet->payload_type);
      RTC_CHECK(depacketizer_it != payload_type_map_.end());

      rtc::scoped_refptr<EncodedImageBuffer> bitstream =
          depacketizer_it->second->AssembleFrame(payloads);
      if (!bitstream) {
        // Failed to assemble a frame. Discard and continue.
        RTC_LOG(INFO)<<"sandyofo failed to assemble the frame";
        continue;
      }


      const video_coding::PacketBuffer::Packet& last_packet = *packet;
      OnAssembledFrame(std::make_unique<video_coding::RtpFrameObject>(
          first_packet->seq_num,                    //
          last_packet.seq_num,                      //
          last_packet.marker_bit,                   //
          max_nack_count,                           //
          min_recv_time,                            //
          max_recv_time,                            //
          first_packet->timestamp,                  //
          first_packet->ntp_time_ms,                //
          last_packet.video_header.video_timing,    //
          first_packet->payload_type,               //
          first_packet->codec(),                    //
          last_packet.video_header.rotation,        //
          last_packet.video_header.content_type,    //
          first_packet->video_header,               //
          last_packet.video_header.color_space,     //
          RtpPacketInfos(std::move(packet_infos)),  //
          std::move(bitstream)));
      //sandy: Frame is constructed now measure the rate of increase and decrease for RTP frame construction.
      // double trend = prev_trend_;
      //sandy: Signaling is not applicable to the redundant scheduler
      if(!(mpcollector_->MpGetScheduler().find("red")!=std::string::npos) && mpcollector_->MpISsecondPathOpen()){
        if(sandy_previous_frame_time==0){
          sandy_previous_frame_time=sandy_end_time;
        }
        if(sandy_time_window==0){
          sandy_time_window=now_ms;
        }
        double gap=0;
        double ifd=0;
        if(int(packet->video_header.frame_type)!=3){
          // RTC_LOG(LS_INFO)<<"sandyframe type: "<<int(packet->video_header.frame_type);
          gap=std::abs(double(sandy_end_time- sandy_start_time));
          ifd=std::abs(double(sandy_end_time- sandy_previous_frame_time));
        }
        int nack=max_nack_count;
        if(!sandy_time_window_trigger){
          sandy_time_window_trigger=now_ms;
        }
        double gap_trend=0;
        double ifd_trend=0;
          float gap_avg=0;
          float ifd_avg=0;
          float nack_avg=0;
          if(mp_gaps_.size()>0)
            gap_avg=(std::accumulate(mp_gaps_.begin(),mp_gaps_.end(),0))/mp_gaps_.size();
          if(mp_ifd_.size()>0)
            ifd_avg=(std::accumulate(mp_ifd_.begin(),mp_ifd_.end(),0))/mp_ifd_.size();
          if(mp_nack_.size()>0)
            nack_avg=(std::accumulate(mp_nack_.begin(),mp_nack_.end(),0))/mp_nack_.size();
        // if(now_ms - sandy_time_window>1000){
        //   ++num_of_deltas_;
        //   if(frame_timings_.size()>=linearwindow){
        //     gap_trend = LinearFitSlope(frame_timings_);  
        //   }
        //   if(frame_ifd_.size()>=linearwindow){
        //     ifd_trend = LinearFitSlope(frame_ifd_);
        //   }
        //   if(ifd_avg<100 ||  gap_avg<100){
        //     //sandy: Loss based analysis
        //     if(gap_trend>0.001 && ifd_trend>0.001 && (ifd_avg>33.0 || gap_avg>50)){
        //       // if(gap_trend>0.001 || ifd_trend >0.001){
        //       // if(ifd_avg>33.0 && gap_avg>50){
        //         //sandy: Disabling the path comes at very high cost and hence try to send the half signal first
        //         if(!full_signal_sent_time){
        //           full_signal_sent_time=clock_->TimeInMilliseconds();
        //           // rtp_rtcp_->SendAppRequestHalf();
        //           RTC_LOG(INFO)<<"sandyofo send the reduction signal by half "<<full_signal_sent_time;
        //         }else if(now_ms- full_signal_sent_time>1500){
        //           RTC_LOG(INFO)<<"sandyofo send the set to one signal";
        //           //sandy: Since you have made siganl cleared you should restart the trend process
        //           // rtp_rtcp_->SendAppRequestFull();
        //           frame_timings_.clear();
        //           frame_ifd_.clear();
        //           mp_smoothed_timings_ifd_=0;
        //           mp_smoothed_timings_=0;
        //           full_signal_sent_time=0;
        //         }
        //     }
        //   }else {
        //     //sandy: Delay based analysis
        //     if(gap_trend>0.001 && ifd_trend>0.001 && (ifd_avg>500 || gap_avg>500)){
        //       // if(gap_trend>0.001 || ifd_trend >0.001){
        //       // if(ifd_avg>33.0 && gap_avg>50){
        //         //sandy: Disabling the path comes at very high cost and hence try to send the half signal first
        //         if(!full_signal_sent_time){
        //           full_signal_sent_time=clock_->TimeInMilliseconds();
        //           // rtp_rtcp_->SendAppRequestHalf();
        //           RTC_LOG(INFO)<<"sandyofo send the reduction signal by half "<<full_signal_sent_time;
        //         }else if(now_ms- full_signal_sent_time>1500){
        //           RTC_LOG(INFO)<<"sandyofo send the set to one signal";
        //           //sandy: Since you have made siganl cleared you should restart the trend process
        //           // rtp_rtcp_->SendAppRequestFull();
        //           frame_timings_.clear();
        //           frame_ifd_.clear();
        //           mp_smoothed_timings_ifd_=0;
        //           mp_smoothed_timings_=0;
        //           full_signal_sent_time=0;
        //         }
        //     }
        //   }

        //   RTC_LOG(LS_INFO)<<"sandyofoavg "<<arrival_time-mp_first_arrival_time_ms_<<" gap "<<gap_avg<<" IFD "<< 
        //   ifd_avg<<" gap trend "<<gap_trend<<" ifd trend "<<ifd_trend<<" gap size= "<<frame_timings_.size() 
        //   <<" IFD size: "<<frame_ifd_.size()<<" Nack Avg: "<<nack_avg<<"Nack size:"<<mp_nack_.size()<< " RTT: "<<std::min(mpcollector_->MpGetRTT1(),mpcollector_->MpGetRTT2());
          
        //   mp_gaps_.clear();
        //   mp_ifd_.clear();        
        //   mp_nack_.clear();
        //   sandy_time_window=now_ms;
        //   if(frame_timings_.size() > linearwindow){
        //     frame_timings_.pop_front();
        //   }
        //   if(frame_ifd_.size()>linearwindow){
        //     frame_ifd_.pop_front();
        //   }
        //   if(gap>0){
        //     mp_smoothed_timings_=0.9 * mp_smoothed_timings_ +
        //               0.1 * gap;
        //     mp_gaps_.push_back(gap);
        //     frame_timings_.emplace_back(double(arrival_time-mp_first_arrival_time_ms_),mp_smoothed_timings_);
        //   }
        //   if(ifd>0){
        //     mp_smoothed_timings_ifd_=0.9 * mp_smoothed_timings_ifd_ +
        //               0.1 * ifd;
        //     mp_ifd_.push_back(ifd);
        //     frame_ifd_.emplace_back(double(arrival_time-mp_first_arrival_time_ms_),mp_smoothed_timings_ifd_);
        //   }
        //   if(nack>0){
        //     mp_nack_.push_back(nack);
        //   }
        // }else{
          if(frame_timings_.size() > linearwindow){
            frame_timings_.pop_front();
          }
          if(gap>0){
            mp_gaps_.push_back(gap);
            mp_smoothed_timings_=0.9 * mp_smoothed_timings_ +
                      0.1 * gap;
            frame_timings_.emplace_back(double(arrival_time-mp_first_arrival_time_ms_),mp_smoothed_timings_);
          }
          if(frame_ifd_.size()>linearwindow){
            frame_ifd_.pop_front();
          }
          if(ifd>0){
            mp_smoothed_timings_ifd_=0.9 * mp_smoothed_timings_ifd_ +
                      0.1 * ifd;
            mp_ifd_.push_back(ifd);
            frame_ifd_.emplace_back(double(arrival_time-mp_first_arrival_time_ms_),mp_smoothed_timings_ifd_);
          }
          if(nack>0){
            mp_nack_.push_back(nack);
          }  
        if(frame_timings_.size()==linearwindow){
            gap_trend = LinearFitSlope(frame_timings_);  
        }
        if(frame_ifd_.size()==linearwindow){
          ifd_trend = LinearFitSlope(frame_ifd_);
        }       
        RTC_LOG(LS_INFO)<<" sandyofo "<<arrival_time-mp_first_arrival_time_ms_<<" Gather Delay "<<gap<<" Gather Delay smooth "<<mp_smoothed_timings_ 
        <<" Gather Delay Average "<<gap_avg<<" IFD Delay "<<ifd<<" IFD Delay smooth "<<mp_smoothed_timings_ifd_<<" IFD Average "<<ifd_avg<< 
        " Gather slop "<<gap_trend<<" IFD slope "<<ifd_trend<<" RTP packets count "<<packets_count;
        sandy_previous_frame_time=sandy_end_time;
        
        sandy_end_time=0;
        sandy_start_time=0;
      }        
    }
  }
  // RTC_LOG(LS_INFO)<<"sandyofo  the packets count in frame: "<<packets_count;
  packets_count=0;
  RTC_DCHECK(frame_boundary);
  if (result.buffer_cleared) {
    
    if(arrival_time!=last_key_frame_time){//sandy: You just requested the Key frame and do not request it again.
      RTC_LOG(INFO)<<"sandyofo the OFO increasing and buffer is full ";
      RequestKeyFrame();
    }
  }
}

void RtpVideoStreamReceiver2::OnAssembledFrame(
    std::unique_ptr<video_coding::RtpFrameObject> frame) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  RTC_DCHECK(frame);

  const absl::optional<RTPVideoHeader::GenericDescriptorInfo>& descriptor =
      frame->GetRtpVideoHeader().generic;

  if (loss_notification_controller_ && descriptor) {
    loss_notification_controller_->OnAssembledFrame(
        frame->first_seq_num(), descriptor->frame_id,
        absl::c_linear_search(descriptor->decode_target_indications,
                              DecodeTargetIndication::kDiscardable),
        descriptor->dependencies);
  }

  // If frames arrive before a key frame, they would not be decodable.
  // In that case, request a key frame ASAP.
  if (!has_received_frame_) {
    if (frame->FrameType() != VideoFrameType::kVideoFrameKey) {
      // |loss_notification_controller_|, if present, would have already
      // requested a key frame when the first packet for the non-key frame
      // had arrived, so no need to replicate the request.
      if (!loss_notification_controller_) {
        // RTC_LOG(INFO)<<"sandyofo the OFO increasing and buffer is full ";
        RequestKeyFrame();
      }
    }
    has_received_frame_ = true;
  }

  // Reset |reference_finder_| if |frame| is new and the codec have changed.
  if (current_codec_) {
    bool frame_is_newer =
        AheadOf(frame->Timestamp(), last_assembled_frame_rtp_timestamp_);

    if (frame->codec_type() != current_codec_) {
      if (frame_is_newer) {
        // When we reset the |reference_finder_| we don't want new picture ids
        // to overlap with old picture ids. To ensure that doesn't happen we
        // start from the |last_completed_picture_id_| and add an offset in case
        // of reordering.
        reference_finder_ =
            std::make_unique<video_coding::RtpFrameReferenceFinder>(
                this, last_completed_picture_id_ +
                          std::numeric_limits<uint16_t>::max());
        current_codec_ = frame->codec_type();
      } else {
        // Old frame from before the codec switch, discard it.
        return;
      }
    }

    if (frame_is_newer) {
      last_assembled_frame_rtp_timestamp_ = frame->Timestamp();
    }
  } else {
    current_codec_ = frame->codec_type();
    last_assembled_frame_rtp_timestamp_ = frame->Timestamp();
  }

  if (buffered_frame_decryptor_ != nullptr) {
    buffered_frame_decryptor_->ManageEncryptedFrame(std::move(frame));
  } else if (frame_transformer_delegate_) {
    frame_transformer_delegate_->TransformFrame(std::move(frame));
  } else {
    reference_finder_->ManageFrame(std::move(frame));
  }
}

void RtpVideoStreamReceiver2::OnCompleteFrame(
    std::unique_ptr<video_coding::EncodedFrame> frame) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  video_coding::RtpFrameObject* rtp_frame =
      static_cast<video_coding::RtpFrameObject*>(frame.get());
  last_seq_num_for_pic_id_[rtp_frame->id.picture_id] =
      rtp_frame->last_seq_num();

  last_completed_picture_id_ =
      std::max(last_completed_picture_id_, frame->id.picture_id);
  complete_frame_callback_->OnCompleteFrame(std::move(frame));
}

void RtpVideoStreamReceiver2::OnDecryptedFrame(
    std::unique_ptr<video_coding::RtpFrameObject> frame) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  reference_finder_->ManageFrame(std::move(frame));
}

void RtpVideoStreamReceiver2::OnDecryptionStatusChange(
    FrameDecryptorInterface::Status status) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  // Called from BufferedFrameDecryptor::DecryptFrame.
  frames_decryptable_ =
      (status == FrameDecryptorInterface::Status::kOk) ||
      (status == FrameDecryptorInterface::Status::kRecoverable);
}

void RtpVideoStreamReceiver2::SetFrameDecryptor(
    rtc::scoped_refptr<FrameDecryptorInterface> frame_decryptor) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  if (buffered_frame_decryptor_ == nullptr) {
    buffered_frame_decryptor_ =
        std::make_unique<BufferedFrameDecryptor>(this, this);
  }
  buffered_frame_decryptor_->SetFrameDecryptor(std::move(frame_decryptor));
}

void RtpVideoStreamReceiver2::SetDepacketizerToDecoderFrameTransformer(
    rtc::scoped_refptr<FrameTransformerInterface> frame_transformer) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  frame_transformer_delegate_ =
      new rtc::RefCountedObject<RtpVideoStreamReceiverFrameTransformerDelegate>(
          this, std::move(frame_transformer), rtc::Thread::Current(),
          config_.rtp.remote_ssrc);
  frame_transformer_delegate_->Init();
}

void RtpVideoStreamReceiver2::UpdateRtt(int64_t max_rtt_ms) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  if (nack_module_ && nack_module_s_){
    nack_module_->UpdateRtt(max_rtt_ms);
    nack_module_s_->UpdateRtt(max_rtt_ms);
  }
}

absl::optional<int64_t> RtpVideoStreamReceiver2::LastReceivedPacketMs() const {
  return packet_buffer_.LastReceivedPacketMs();
}

absl::optional<int64_t> RtpVideoStreamReceiver2::LastReceivedKeyframePacketMs()
    const {
  return packet_buffer_.LastReceivedKeyframePacketMs();
}

void RtpVideoStreamReceiver2::AddSecondarySink(RtpPacketSinkInterface* sink) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  RTC_DCHECK(!absl::c_linear_search(secondary_sinks_, sink));
  secondary_sinks_.push_back(sink);
}

void RtpVideoStreamReceiver2::RemoveSecondarySink(
    const RtpPacketSinkInterface* sink) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  auto it = absl::c_find(secondary_sinks_, sink);
  if (it == secondary_sinks_.end()) {
    // We might be rolling-back a call whose setup failed mid-way. In such a
    // case, it's simpler to remove "everything" rather than remember what
    // has already been added.
    RTC_LOG(LS_WARNING) << "Removal of unknown sink.";
    return;
  }
  secondary_sinks_.erase(it);
}

void RtpVideoStreamReceiver2::ManageFrame(
    std::unique_ptr<video_coding::RtpFrameObject> frame) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  reference_finder_->ManageFrame(std::move(frame));
}

void RtpVideoStreamReceiver2::ReceivePacket(const RtpPacketReceived& packet) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  if (packet.payload_size() == 0) {
    // Padding or keep-alive packet.
    // TODO(nisse): Could drop empty packets earlier, but need to figure out how
    // they should be counted in stats.
    RTPHeader header;
    packet.GetHeader(&header);
    int pathid=header.extension.sandy;
    RTC_DCHECK(pathid>0 && header.extension.mpflowseqnum>0);
    NotifyReceiverOfEmptyPacket(packet.SequenceNumber(), 
      header.extension.mpflowseqnum,pathid);
    return;
  }
  if (packet.PayloadType() == config_.rtp.red_payload_type) {
    ParseAndHandleEncapsulatingHeader(packet);
    return;
  }

  const auto type_it = payload_type_map_.find(packet.PayloadType());
  if (type_it == payload_type_map_.end()) {
    return;
  }
  absl::optional<VideoRtpDepacketizer::ParsedRtpPayload> parsed_payload =
      type_it->second->Parse(packet.PayloadBuffer());
  if (parsed_payload == absl::nullopt) {
    RTC_LOG(LS_WARNING) << "Failed parsing payload.";
    return;
  }

  OnReceivedPayloadData(std::move(parsed_payload->video_payload), packet,
                        parsed_payload->video_header);
}

void RtpVideoStreamReceiver2::ParseAndHandleEncapsulatingHeader(
    const RtpPacketReceived& packet) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  if (packet.PayloadType() == config_.rtp.red_payload_type &&
      packet.payload_size() > 0) {
    if (packet.payload()[0] == config_.rtp.ulpfec_payload_type) {
      // Notify video_receiver about received FEC packets to avoid NACKing these
      // packets.
      RTPHeader header;
    packet.GetHeader(&header);
    int pathid=header.extension.sandy;
    RTC_DCHECK(pathid>0 && header.extension.mpflowseqnum>0);
      NotifyReceiverOfEmptyPacket(packet.SequenceNumber(), 
        header.extension.mpflowseqnum,pathid);
    }
    if (!ulpfec_receiver_->AddReceivedRedPacket(
            packet, config_.rtp.ulpfec_payload_type)) {
      return;
    }
    ulpfec_receiver_->ProcessReceivedFec();
  }
}

// In the case of a video stream without picture ids and no rtx the
// RtpFrameReferenceFinder will need to know about padding to
// correctly calculate frame references.
void RtpVideoStreamReceiver2::NotifyReceiverOfEmptyPacket( 
  uint16_t seq_num,uint16_t mp_seq_num,int pathid) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);

  reference_finder_->PaddingReceived(seq_num);

  OnInsertedPacket(packet_buffer_.InsertPadding(seq_num));
  if (nack_module_ && pathid!=2)  {

    nack_module_->OnReceivedPacket(mp_seq_num,/* is_keyframe = */ false,
                                   /* is _recovered = */ false,pathid);
  }else if(nack_module_s_ && pathid==2){
    nack_module_s_->OnReceivedPacket(mp_seq_num,/* is_keyframe = */ false,
                                   /* is _recovered = */ false,pathid);
  }
  if (loss_notification_controller_) {
    // TODO(bugs.webrtc.org/10336): Handle empty packets.
    RTC_LOG(LS_WARNING)
        << "LossNotificationController does not expect empty packets.";
  }
}

bool RtpVideoStreamReceiver2::DeliverRtcp(const uint8_t* rtcp_packet,
                                          size_t rtcp_packet_length) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);

  if (!receiving_) {
    return false;
  }

  rtp_rtcp_->IncomingRtcpPacket(rtcp_packet, rtcp_packet_length);

  int64_t rtt = 0;
  rtp_rtcp_->RTT(config_.rtp.remote_ssrc, &rtt, nullptr, nullptr, nullptr,1);
  //sandy: Always use path 1 RTT 
  if (rtt == 0) {
    // Waiting for valid rtt.
    return true;
  }
  uint32_t ntp_secs = 0;
  uint32_t ntp_frac = 0;
  uint32_t rtp_timestamp = 0;
  uint32_t recieved_ntp_secs = 0;
  uint32_t recieved_ntp_frac = 0;
  if (rtp_rtcp_->RemoteNTP(&ntp_secs, &ntp_frac, &recieved_ntp_secs,
                           &recieved_ntp_frac, &rtp_timestamp,1) != 0) {//sandy: Always use path 1 RTT
    // Waiting for RTCP.
    return true;
  }
  NtpTime recieved_ntp(recieved_ntp_secs, recieved_ntp_frac);
  int64_t time_since_recieved =
      clock_->CurrentNtpInMilliseconds() - recieved_ntp.ToMs();
  // Don't use old SRs to estimate time.
  if (time_since_recieved <= 1) {
    ntp_estimator_.UpdateRtcpTimestamp(rtt, ntp_secs, ntp_frac, rtp_timestamp);
    absl::optional<int64_t> remote_to_local_clock_offset_ms =
        ntp_estimator_.EstimateRemoteToLocalClockOffsetMs();
    if (remote_to_local_clock_offset_ms.has_value()) {
      absolute_capture_time_receiver_.SetRemoteToLocalClockOffset(
          Int64MsToQ32x32(*remote_to_local_clock_offset_ms));
    }
  }

  return true;
}

void RtpVideoStreamReceiver2::FrameContinuous(int64_t picture_id) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  if (!nack_module_ && !nack_module_s_)
    return;

  int seq_num = -1;
  auto seq_num_it = last_seq_num_for_pic_id_.find(picture_id);
  if (seq_num_it != last_seq_num_for_pic_id_.end())
    seq_num = seq_num_it->second;
  if (seq_num != -1){
    //sandy: I have added the nack module with mpsequence numbers and hence I am cpmmenting out these two below line
    // nack_module_->ClearUpTo(seq_num);
    // nack_module_s_->ClearUpTo(seq_num);//sandy:This could be problem
  }
}

void RtpVideoStreamReceiver2::FrameDecoded(int64_t picture_id) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  // Running on the decoder thread.
  int seq_num = -1;
  auto seq_num_it = last_seq_num_for_pic_id_.find(picture_id);
  if (seq_num_it != last_seq_num_for_pic_id_.end()) {
    seq_num = seq_num_it->second;
    last_seq_num_for_pic_id_.erase(last_seq_num_for_pic_id_.begin(),
                                   ++seq_num_it);
  }

  if (seq_num != -1) {
    packet_buffer_.ClearTo(seq_num);
    reference_finder_->ClearTo(seq_num);
  }
}

void RtpVideoStreamReceiver2::SignalNetworkState(NetworkState state) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  rtp_rtcp_->SetRTCPStatus(state == kNetworkUp ? config_.rtp.rtcp_mode
                                               : RtcpMode::kOff);
}

void RtpVideoStreamReceiver2::StartReceive() {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  receiving_ = true;
}

void RtpVideoStreamReceiver2::StopReceive() {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);
  receiving_ = false;
}

void RtpVideoStreamReceiver2::UpdateHistograms() {
  FecPacketCounter counter = ulpfec_receiver_->GetPacketCounter();
  if (counter.first_packet_time_ms == -1)
    return;

  int64_t elapsed_sec =
      (clock_->TimeInMilliseconds() - counter.first_packet_time_ms) / 1000;
  if (elapsed_sec < metrics::kMinRunTimeInSeconds)
    return;

  if (counter.num_packets > 0) {
    RTC_HISTOGRAM_PERCENTAGE(
        "WebRTC.Video.ReceivedFecPacketsInPercent",
        static_cast<int>(counter.num_fec_packets * 100 / counter.num_packets));
  }
  if (counter.num_fec_packets > 0) {
    RTC_HISTOGRAM_PERCENTAGE("WebRTC.Video.RecoveredMediaPacketsInPercentOfFec",
                             static_cast<int>(counter.num_recovered_packets *
                                              100 / counter.num_fec_packets));
  }
  if (config_.rtp.ulpfec_payload_type != -1) {
    RTC_HISTOGRAM_COUNTS_10000(
        "WebRTC.Video.FecBitrateReceivedInKbps",
        static_cast<int>(counter.num_bytes * 8 / elapsed_sec / 1000));
  }
}

void RtpVideoStreamReceiver2::InsertSpsPpsIntoTracker(uint8_t payload_type) {
  RTC_DCHECK_RUN_ON(&worker_task_checker_);

  auto codec_params_it = pt_codec_params_.find(payload_type);
  if (codec_params_it == pt_codec_params_.end())
    return;

  RTC_LOG(LS_INFO) << "Found out of band supplied codec parameters for"
                      " payload type: "
                   << static_cast<int>(payload_type);

  H264SpropParameterSets sprop_decoder;
  auto sprop_base64_it =
      codec_params_it->second.find(cricket::kH264FmtpSpropParameterSets);

  if (sprop_base64_it == codec_params_it->second.end())
    return;

  if (!sprop_decoder.DecodeSprop(sprop_base64_it->second.c_str()))
    return;

  tracker_.InsertSpsPpsNalus(sprop_decoder.sps_nalu(),
                             sprop_decoder.pps_nalu());
}

}  // namespace webrtc
