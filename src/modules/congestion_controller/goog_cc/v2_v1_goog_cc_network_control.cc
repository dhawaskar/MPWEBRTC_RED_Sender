/*
 *  Copyright (c) 2018 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_CONGESTION_CONTROLLER_GOOG_CC_GOOG_CC_NETWORK_CONTROL_H_
#define MODULES_CONGESTION_CONTROLLER_GOOG_CC_GOOG_CC_NETWORK_CONTROL_H_

#include <stdint.h>

#include <deque>
#include <memory>
#include <vector>

#include "absl/types/optional.h"
#include "api/network_state_predictor.h"
#include "api/rtc_event_log/rtc_event_log.h"
#include "api/transport/field_trial_based_config.h"
#include "api/transport/network_control.h"
#include "api/transport/network_types.h"
#include "api/transport/webrtc_key_value_config.h"
#include "api/units/data_rate.h"
#include "api/units/data_size.h"
#include "api/units/timestamp.h"
#include "modules/congestion_controller/goog_cc/acknowledged_bitrate_estimator_interface.h"
#include "modules/congestion_controller/goog_cc/alr_detector.h"
#include "modules/congestion_controller/goog_cc/congestion_window_pushback_controller.h"
#include "modules/congestion_controller/goog_cc/delay_based_bwe.h"
#include "modules/congestion_controller/goog_cc/probe_controller.h"
#include "modules/congestion_controller/goog_cc/send_side_bandwidth_estimation.h"
#include "rtc_base/constructor_magic.h"
#include "rtc_base/experiments/field_trial_parser.h"
#include "rtc_base/experiments/rate_control_settings.h"

namespace webrtc {
struct GoogCcConfig {
  std::unique_ptr<NetworkStateEstimator> network_state_estimator = nullptr;
  std::unique_ptr<NetworkStatePredictor> network_state_predictor = nullptr;
  bool feedback_only = false;
};

class GoogCcNetworkController : public NetworkControllerInterface {
 public:
  GoogCcNetworkController(NetworkControllerConfig config,
                          GoogCcConfig goog_cc_config);
  ~GoogCcNetworkController() override;

  // NetworkControllerInterface
  NetworkControlUpdate OnNetworkAvailability(NetworkAvailability msg) override;
  NetworkControlUpdate OnNetworkRouteChange(NetworkRouteChange msg) override;
  NetworkControlUpdate OnProcessInterval(ProcessInterval msg) override;
  NetworkControlUpdate OnRemoteBitrateReport(RemoteBitrateReport msg) override;
  NetworkControlUpdate OnRoundTripTimeUpdate(RoundTripTimeUpdate msg) override;
  NetworkControlUpdate OnSentPacket(SentPacket msg) override;
  NetworkControlUpdate OnReceivedPacket(ReceivedPacket msg) override;
  NetworkControlUpdate OnStreamsConfig(StreamsConfig msg) override;
  NetworkControlUpdate OnTargetRateConstraints(
      TargetRateConstraints msg) override;
  NetworkControlUpdate OnTransportLossReport(TransportLossReport msg) override;
  NetworkControlUpdate OnTransportPacketsFeedback(
      TransportPacketsFeedback msg,int pathid=-1) override;
  void OnTransportPacketsFeedbackPrimary(
      TransportPacketsFeedback msg,int pathid=-1,NetworkControlUpdate *update=nullptr);
  void OnTransportPacketsFeedbackSecondary(
      TransportPacketsFeedback msg,int pathid=-1,NetworkControlUpdate *update=nullptr);
  NetworkControlUpdate OnNetworkStateEstimate(
      NetworkStateEstimate msg) override;

  NetworkControlUpdate GetNetworkState(Timestamp at_time,int pathid) const;

 private:
  friend class GoogCcStatePrinter;
  std::vector<ProbeClusterConfig> ResetConstraintsPrimary(
      TargetRateConstraints new_constraints);
  std::vector<ProbeClusterConfig> ResetConstraintsSecondary(
      TargetRateConstraints new_constraints);
  void ClampConstraints();
  void MaybeTriggerOnNetworkChanged(NetworkControlUpdate* update,
                                    Timestamp at_time);
  void MaybeTriggerOnNetworkChangedPrimary(NetworkControlUpdate* update,
                                    Timestamp at_time);
  void MaybeTriggerOnNetworkChangedSecondary(NetworkControlUpdate* update,
                                    Timestamp at_time);
  void UpdateCongestionWindowSize();
  void UpdateCongestionWindowSizePrimary();
  void UpdateCongestionWindowSizeSecondary();
  PacerConfig GetPacingRates(Timestamp at_time) const;
  const FieldTrialBasedConfig trial_based_config_;

  const WebRtcKeyValueConfig* const key_value_config_;
  RtcEventLog* const event_log_;
  const bool packet_feedback_only_;


  FieldTrialFlag safe_reset_on_route_change_;
  FieldTrialFlag safe_reset_on_route_change_p_;
  FieldTrialFlag safe_reset_on_route_change_s_;

  FieldTrialFlag safe_reset_acknowledged_rate_;
  FieldTrialFlag safe_reset_acknowledged_rate_p_;
  FieldTrialFlag safe_reset_acknowledged_rate_s_;


  const bool use_min_allocatable_as_lower_bound_;
  const bool use_min_allocatable_as_lower_bound_p_;
  const bool use_min_allocatable_as_lower_bound_s_;


  const bool ignore_probes_lower_than_network_estimate_;
  const bool ignore_probes_lower_than_network_estimate_p_;
  const bool ignore_probes_lower_than_network_estimate_s_;

  const bool limit_probes_lower_than_throughput_estimate_;
  const bool limit_probes_lower_than_throughput_estimate_p_;
  const bool limit_probes_lower_than_throughput_estimate_s_;

  const RateControlSettings rate_control_settings_;
  const RateControlSettings rate_control_settings_p_;
  const RateControlSettings rate_control_settings_s_;

  const bool loss_based_stable_rate_;
  const bool loss_based_stable_rate_p_;
  const bool loss_based_stable_rate_s_;


  const std::unique_ptr<ProbeController> probe_controller_;
  const std::unique_ptr<ProbeController> probe_controller_p_;
  const std::unique_ptr<ProbeController> probe_controller_s_;


  const std::unique_ptr<CongestionWindowPushbackController>
      congestion_window_pushback_controller_;
  //sandy:Mp-WebRTC primary  
  const std::unique_ptr<CongestionWindowPushbackController>
      congestion_window_pushback_controller_p_;
  //sandy:Mp-WebRTC secondary  
  const std::unique_ptr<CongestionWindowPushbackController>
      congestion_window_pushback_controller_s_;


  //std::unique_ptr<SendSideBandwidthEstimation> bandwidth_estimation_;
  std::unique_ptr<SendSideBandwidthEstimation> bandwidth_estimation_p_;
  std::unique_ptr<SendSideBandwidthEstimation> bandwidth_estimation_s_;
  std::unique_ptr<AlrDetector> alr_detector_;
  //std::unique_ptr<AlrDetector> alr_detector_p_;
  //std::unique_ptr<AlrDetector> alr_detector_s_;
  std::unique_ptr<ProbeBitrateEstimator> probe_bitrate_estimator_;
  std::unique_ptr<ProbeBitrateEstimator> probe_bitrate_estimator_p_;
  std::unique_ptr<ProbeBitrateEstimator> probe_bitrate_estimator_s_;
  std::unique_ptr<NetworkStateEstimator> network_estimator_;
  std::unique_ptr<NetworkStateEstimator> network_estimator_p_;
  std::unique_ptr<NetworkStateEstimator> network_estimator_s_;
  std::unique_ptr<NetworkStatePredictor> network_state_predictor_;
  std::unique_ptr<NetworkStatePredictor> network_state_predictor_p_;
  std::unique_ptr<NetworkStatePredictor> network_state_predictor_s_;
  std::unique_ptr<DelayBasedBwe> delay_based_bwe_;
  std::unique_ptr<DelayBasedBwe> delay_based_bwe_p_;
  std::unique_ptr<DelayBasedBwe> delay_based_bwe_s_;
  std::unique_ptr<AcknowledgedBitrateEstimatorInterface>
      acknowledged_bitrate_estimator_;
  std::unique_ptr<AcknowledgedBitrateEstimatorInterface>
      acknowledged_bitrate_estimator_p_;
  std::unique_ptr<AcknowledgedBitrateEstimatorInterface>
      acknowledged_bitrate_estimator_s_;

  absl::optional<NetworkControllerConfig> initial_config_;

  DataRate min_target_rate_ = DataRate::Zero();
  DataRate min_target_rate_p_ = DataRate::Zero();
  DataRate min_target_rate_s_ = DataRate::Zero();
  DataRate min_data_rate_ = DataRate::Zero();
  DataRate min_data_rate_p_ = DataRate::Zero();
  DataRate min_data_rate_s_ = DataRate::Zero();
  DataRate max_data_rate_ = DataRate::PlusInfinity();
  DataRate max_data_rate_p_ = DataRate::PlusInfinity();
  DataRate max_data_rate_s_ = DataRate::PlusInfinity();
  absl::optional<DataRate> starting_rate_;
  absl::optional<DataRate> starting_rate_p_;
  absl::optional<DataRate> starting_rate_s_;

  bool first_packet_sent_ = false;
  bool first_packet_sent_p_ = false;
  bool first_packet_sent_s_ = false;

  absl::optional<NetworkStateEstimate> estimate_;
  absl::optional<NetworkStateEstimate> estimate_p_;
  absl::optional<NetworkStateEstimate> estimate_s_;

  Timestamp next_loss_update_ = Timestamp::MinusInfinity();
  Timestamp next_loss_update_p_ = Timestamp::MinusInfinity();
  Timestamp next_loss_update_s_ = Timestamp::MinusInfinity();
  int lost_packets_since_last_loss_update_ = 0;
  int lost_packets_since_last_loss_update_p_ = 0;
  int lost_packets_since_last_loss_update_s_ = 0;
  int expected_packets_since_last_loss_update_ = 0;
  int expected_packets_since_last_loss_update_p_ = 0;
  int expected_packets_since_last_loss_update_s_ = 0;

  std::deque<int64_t> feedback_max_rtts_;
  std::deque<int64_t> feedback_max_rtts_p_;
  std::deque<int64_t> feedback_max_rtts_s_;

  DataRate last_loss_based_target_rate_;
  DataRate last_loss_based_target_rate_p_;
  DataRate last_loss_based_target_rate_s_;

  DataRate last_pushback_target_rate_;
  DataRate last_pushback_target_rate_p_;
  DataRate last_pushback_target_rate_s_;

  DataRate last_stable_target_rate_;
  DataRate last_stable_target_rate_p_;
  DataRate last_stable_target_rate_s_;

  absl::optional<uint8_t> last_estimated_fraction_loss_ = 0;
  absl::optional<uint8_t> last_estimated_fraction_loss_p_ = 0;
  absl::optional<uint8_t> last_estimated_fraction_loss_s_ = 0;

  TimeDelta last_estimated_round_trip_time_ = TimeDelta::PlusInfinity();
  TimeDelta last_estimated_round_trip_time_p_ = TimeDelta::PlusInfinity();
  TimeDelta last_estimated_round_trip_time_s_ = TimeDelta::PlusInfinity();

  Timestamp last_packet_received_time_ = Timestamp::MinusInfinity();
  Timestamp last_packet_received_time_p_ = Timestamp::MinusInfinity();
  Timestamp last_packet_received_time_s_ = Timestamp::MinusInfinity();

  double pacing_factor_;
  double pacing_factor_p_;
  double pacing_factor_s_;

  DataRate min_total_allocated_bitrate_;
  DataRate min_total_allocated_bitrate_p_;
  DataRate min_total_allocated_bitrate_s_;

  DataRate max_padding_rate_;
  DataRate max_padding_rate_p_;
  DataRate max_padding_rate_s_;

  DataRate max_total_allocated_bitrate_;
  DataRate max_total_allocated_bitrate_p_;
  DataRate max_total_allocated_bitrate_s_;

  bool previously_in_alr_ = false;
  bool previously_in_alr_p_ = false;
  bool previously_in_alr_s_ = false;

  absl::optional<DataSize> current_data_window_;
  absl::optional<DataSize> current_data_window_p_;
  absl::optional<DataSize> current_data_window_s_;

  RTC_DISALLOW_IMPLICIT_CONSTRUCTORS(GoogCcNetworkController);
};

}  // namespace webrtc

#endif  // MODULES_CONGESTION_CONTROLLER_GOOG_CC_GOOG_CC_NETWORK_CONTROL_H_
