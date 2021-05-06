/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 *
 */

#ifndef MODULES_RTP_RTCP_SOURCE_RTCP_PACKET_REPORT_BLOCK_H_
#define MODULES_RTP_RTCP_SOURCE_RTCP_PACKET_REPORT_BLOCK_H_

#include <stddef.h>
#include <stdint.h>

namespace webrtc {
namespace rtcp {

// A ReportBlock represents the Sender Report packet from
// RFC 3550 section 6.4.1.
class ReportBlock {
 public:
  //static const size_t kLength = 24; Since I have added the extender I changed 24 to 32
  static const size_t kLength = 28;//sandy: Change it in the sender_report.h as well. This is a hard coded value.

  ReportBlock();
  ~ReportBlock() {}

  bool Parse(const uint8_t* buffer, size_t length);

  // Fills buffer with the ReportBlock.
  // Consumes ReportBlock::kLength bytes.
  void Create(uint8_t* buffer) const;

  void SetMediaSsrc(uint32_t ssrc) { source_ssrc_ = ssrc; }
  void SetFractionLost(uint8_t fraction_lost) {
    fraction_lost_ = fraction_lost;
  }
  bool SetCumulativeLost(int32_t cumulative_lost);
  void SetExtHighestSeqNum(uint32_t ext_highest_seq_num) {
    extended_high_seq_num_ = ext_highest_seq_num;
  }
  void SetExtHighestSeqNum_P(uint32_t ext_highest_seq_num_p) {//sandy: save the original sequence number
    extended_high_seq_num_p_ = ext_highest_seq_num_p;
  }
  void SetRtcpSeq(uint32_t rtcp_seq) {//sandy: save the original sequence number
    rtcp_seq_=rtcp_seq;
  }
  // void SetExtHighestSeqNum_S(uint32_t ext_highest_seq_num_s) {
  //   extended_high_seq_num_s_ = ext_highest_seq_num_s;
  // }
  void SetJitter(uint32_t jitter) { jitter_ = jitter; }
  // void SetJitter_P(uint32_t jitter) { jitter_p_ = jitter; }
  // void SetJitter_S(uint32_t jitter) { jitter_s_ = jitter; }
  void SetLastSr(uint32_t last_sr) { last_sr_ = last_sr; }
  void SetDelayLastSr(uint32_t delay_last_sr) {
    delay_since_last_sr_ = delay_last_sr;
  }
  //sandy:
  // void SetSubFlowFractionLost_P(int subflowfractionlost_p){
  //   subflowfractionlost_p_=subflowfractionlost_p;
  // }
  // void SetSubFlowFractionLost_S(int subflowfractionlost_s){
  //   subflowfractionlost_s_=subflowfractionlost_s;
  // }
  // void SetCumulativeLost_P(int32_t cumulative_lost_p){
  //   cumulative_lost_p_=cumulative_lost_p;
  // }
  // void SetCumulativeLost_S(int32_t cumulative_lost_s){
  //   cumulative_lost_s_=cumulative_lost_s;
  // }


  uint32_t source_ssrc() const { return source_ssrc_; }
  uint8_t fraction_lost() const { return fraction_lost_; }
  int32_t cumulative_lost_signed() const { return cumulative_lost_; }
  // Deprecated - returns max(0, cumulative_lost_), not negative values.
  uint32_t cumulative_lost() const;
  uint32_t extended_high_seq_num() const { return extended_high_seq_num_; }
  uint32_t extended_high_seq_num_p() const { return extended_high_seq_num_p_; }//sandy: save the original sequence number
  // uint32_t extended_high_seq_num_s() const { return extended_high_seq_num_s_; }
  uint32_t jitter() const { return jitter_; }
  // uint32_t jitter_p() const { return jitter_p_; }
  // uint32_t jitter_s() const { return jitter_s_; }
  uint32_t last_sr() const { return last_sr_; }
  uint32_t delay_since_last_sr() const { return delay_since_last_sr_; }
  uint32_t rtcp_seq() const {return rtcp_seq_;}
  // int subflowfractionlost_p()const { return subflowfractionlost_p_;}
  // int subflowfractionlost_s()const { return subflowfractionlost_s_;}
  // int32_t cumulative_lost_p() const { return cumulative_lost_p_; }
  // int32_t cumulative_lost_s() const { return cumulative_lost_s_; }

 private:
  uint32_t source_ssrc_;     // 32 bits
  uint8_t fraction_lost_;    // 8 bits representing a fixed point value 0..1
  int32_t cumulative_lost_;  // Signed 24-bit value
  uint32_t extended_high_seq_num_;  // 32 bits
  uint32_t jitter_;                 // 32 bits
  uint32_t last_sr_;                // 32 bits
  uint32_t delay_since_last_sr_;    // 32 bits, units of 1/65536 seconds
  uint32_t rtcp_seq_=0;
  //sandy: dummy header
  //webrtc::rtcp::MpCollector mpc_;
  //webrtc::rtcp::MpCollector *mpcollector_;
  // uint8_t  mpwebrtc_type_;
  // uint8_t  block_len_;
  // uint16_t rtp_port_; 
  // uint32_t rtp_address_;

  //sandy:
  //Below are wrong way of implementation so remove it
  // int subflowfractionlost_p_;
  // int subflowfractionlost_s_;
  // int32_t cumulative_lost_p_;
  // int32_t cumulative_lost_s_;
  // uint32_t jitter_p_;                 // 32 bits
  // uint32_t jitter_s_;                 // 32 bits
  uint32_t extended_high_seq_num_p_;  // 32 bits//sandy: save the original sequence number
  // uint32_t extended_high_seq_num_s_;  // 32 bits
};

}  // namespace rtcp
}  // namespace webrtc
#endif  // MODULES_RTP_RTCP_SOURCE_RTCP_PACKET_REPORT_BLOCK_H_
