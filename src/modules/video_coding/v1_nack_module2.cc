/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/video_coding/nack_module2.h"

#include <algorithm>
#include <limits>

#include "api/units/timestamp.h"
#include "rtc_base/checks.h"
#include "rtc_base/experiments/field_trial_parser.h"
#include "rtc_base/logging.h"
#include "rtc_base/task_queue.h"
#include "system_wrappers/include/field_trial.h"
#include "api/mp_collector.h"
#include "api/mp_global.h"

namespace webrtc {

namespace {
//const int kMpNackTherhsold = 10;//sandy: Added this
const int kMaxPacketAge = 10000;//sandy: For Mp-webrT
const int kMaxNackPackets = 1000;//sandy: For Mp-WebRTC I will tkae this as 1500 as packets coming from both path.
const int kDefaultRttMs = 100;
const int kMaxNackRetries = 10;
const int kMaxReorderedPackets = 128;
const int kNumReorderingBuckets = 10;
const int kDefaultSendNackDelayMs = 0;
const int32_t kMpGapThreshold=0;//sandy: Since re-ordering is common we will not report loss unless it is >10

int64_t GetSendNackDelay() {
  int64_t delay_ms = strtol(
      webrtc::field_trial::FindFullName("WebRTC-SendNackDelayMs").c_str(),
      nullptr, 10);
  if (delay_ms > 0 && delay_ms <= 20) {
    // RTC_LOG(LS_INFO) << "SendNackDelay is set to " << delay_ms;
    return delay_ms;
  }
  return kDefaultSendNackDelayMs;
}
}  // namespace

constexpr TimeDelta NackModule2::kUpdateInterval;

NackModule2::NackInfo::NackInfo()
    : seq_num(0), send_at_seq_num(0), sent_at_time(-1), retries(0) {}

NackModule2::NackInfo::NackInfo(uint16_t seq_num,
                                uint16_t send_at_seq_num,
                                int64_t created_at_time,int pathid)
    : seq_num(seq_num),
      send_at_seq_num(send_at_seq_num),
      created_at_time(created_at_time),
      sent_at_time(-1),
      retries(0),
      pathid_(pathid) {}

NackModule2::BackoffSettings::BackoffSettings(TimeDelta min_retry,
                                              TimeDelta max_rtt,
                                              double base)
    : min_retry_interval(min_retry), max_rtt(max_rtt), base(base) {}

absl::optional<NackModule2::BackoffSettings>
NackModule2::BackoffSettings::ParseFromFieldTrials() {
  // Matches magic number in RTPSender::OnReceivedNack().
  const TimeDelta kDefaultMinRetryInterval = TimeDelta::Millis(5);
  // Upper bound on link-delay considered for exponential backoff.
  // Selected so that cumulative delay with 1.25 base and 10 retries ends up
  // below 3s, since above that there will be a FIR generated instead.
  const TimeDelta kDefaultMaxRtt = TimeDelta::Millis(160);
  // Default base for exponential backoff, adds 25% RTT delay for each retry.
  const double kDefaultBase = 1.25;

  FieldTrialParameter<bool> enabled("enabled", false);
  FieldTrialParameter<TimeDelta> min_retry("min_retry",
                                           kDefaultMinRetryInterval);
  FieldTrialParameter<TimeDelta> max_rtt("max_rtt", kDefaultMaxRtt);
  FieldTrialParameter<double> base("base", kDefaultBase);
  ParseFieldTrial({&enabled, &min_retry, &max_rtt, &base},
                  field_trial::FindFullName("WebRTC-ExponentialNackBackoff"));

  if (enabled) {
    return NackModule2::BackoffSettings(min_retry.Get(), max_rtt.Get(),
                                        base.Get());
  }
  return absl::nullopt;
}

NackModule2::NackModule2(TaskQueueBase* current_queue,
                         Clock* clock,
                         NackSender* nack_sender,
                         KeyFrameRequestSender* keyframe_request_sender,
                         TimeDelta update_interval /*= kUpdateInterval*/)
    : worker_thread_(current_queue),
      update_interval_(update_interval),
      clock_(clock),
      nack_sender_(nack_sender),
      keyframe_request_sender_(keyframe_request_sender),
      reordering_histogram_(kNumReorderingBuckets, kMaxReorderedPackets),
      initialized_(false),
      initialized_mp_(false),
      rtt_ms_(kDefaultRttMs),
      newest_seq_num_(0),
      newest_seq_num_p_(0),
      newest_seq_num_s_(0),
      send_nack_delay_ms_(GetSendNackDelay()),
      backoff_settings_(BackoffSettings::ParseFromFieldTrials()) {
  RTC_DCHECK(clock_);
  RTC_DCHECK(nack_sender_);
  RTC_DCHECK(keyframe_request_sender_);
  RTC_DCHECK_GT(update_interval.ms(), 0);
  RTC_DCHECK(worker_thread_);
  RTC_DCHECK(worker_thread_->IsCurrent());

  repeating_task_ = RepeatingTaskHandle::DelayedStart(
      TaskQueueBase::Current(), update_interval_,
      [this]() {

        if(!mpcollector_)
          mpcollector_=new MpCollector();

        RTC_DCHECK_RUN_ON(worker_thread_);
        //sandy: Please get the nack batch for both primary and secondary paths
        std::vector<uint16_t> nack_batch = GetNackBatch(kTimeOnly,1);
        if (!nack_batch.empty()) {
          // This batch of NACKs is triggered externally; there is no external
          // initiator who can batch them with other feedback messages.
          nack_sender_->SendNack(nack_batch, /*buffering_allowed=*/false,1);//sandy: yet to implement
        }
        nack_batch = GetNackBatch(kTimeOnly,2);
        if (!nack_batch.empty()) {
          // This batch of NACKs is triggered externally; there is no external
          // initiator who can batch them with other feedback messages.
          nack_sender_->SendNack(nack_batch, /*buffering_allowed=*/false,2);//sandy: yet to implement
        }
        return update_interval_;
      },
      clock_);
}

NackModule2::~NackModule2() {
  RTC_DCHECK_RUN_ON(worker_thread_);
  repeating_task_.Stop();
}

int NackModule2::OnReceivedPacket(uint16_t seq_num, uint16_t mp_seq_num,bool is_keyframe,int pathid) {
  RTC_DCHECK_RUN_ON(worker_thread_);
  return OnReceivedPacket(seq_num, is_keyframe, false,pathid);
}

int NackModule2::OnReceivedPacket(uint16_t seq_num,uint16_t mp_seq_num,
                                  bool is_keyframe,
                                  bool is_recovered,int pathid) {

  RTC_DCHECK(pathid>0);
  RTC_DCHECK_RUN_ON(worker_thread_);
  
  if(mpcollector_->MpISsecondPathOpen() && mp_seq_num!=seq_num && seq_num>0 && mp_seq_num>0 && !initialized_mp_){//sandy: This indicates Mp_webRTC is enabled
    RTC_LOG(INFO)<<"sandystatsnack the mp-webrtc is enabled with seq="<<seq_num<<" and mp_seq ="<<mp_seq_num<<" pathid "<<pathid;
    initialized_mp_=true;
  }


  bool is_retransmitted = true;

  // RTC_LOG(INFO)<<"sandystatsnack:Received the packet "<<seq_num<<" path= "<<pathid<< 
  // " subflow seq= "<<mp_seq_num<<"\n";

  if(initialized_mp_ && newest_seq_num_p_==0 && pathid!=2){
    newest_seq_num_p_=mp_seq_num;
  }else if(initialized_mp_ && newest_seq_num_s_==0 && pathid==2){
    newest_seq_num_s_=mp_seq_num;
  }

  

  if (!initialized_) {
    newest_seq_num_ = seq_num;
    if (is_keyframe)
      keyframe_list_.insert(seq_num);
    initialized_ = true;
    return 0;
  }

  // Since the |newest_seq_num_| is a packet we have actually received we know
  // that packet has never been Nacked.
  if (seq_num == newest_seq_num_)
    return 0;

  if (AheadOf(newest_seq_num_, seq_num)) {
    // An out of order packet has been received.
    auto nack_list_it = nack_list_.find(seq_num);
    int nacks_sent_for_packet = 0;
    if (nack_list_it != nack_list_.end()) {
      nacks_sent_for_packet = nack_list_it->second.retries;
      nack_list_.erase(nack_list_it);
    }
    if (!is_retransmitted)
      UpdateReorderingStatistics(seq_num);
    return nacks_sent_for_packet;
  }

  if(initialized_mp_ &&  pathid!=2 && mp_seq_num>0){
    if(nack_list_p_.size()>1000)
      nack_list_p_.erase(nack_list_p_.begin(),nack_list_p_.end());
    nack_list_p_[mp_seq_num]=seq_num;//add packet to primary path map
    //RTC_LOG(INFO)<<"sandystatsnack added the packet to the list of primary path mp="<<mp_seq_num<< 
    //" seq= "<<nack_list_p_[mp_seq_num]<<" size= "<<nack_list_p_.size();
  }else if(initialized_mp_ && pathid==2 && mp_seq_num>0){
    if(nack_list_s_.size()>1000)
      nack_list_s_.erase(nack_list_s_.begin(),nack_list_s_.end());
    
    nack_list_s_[mp_seq_num]=seq_num;//add packet to primary path map
    // RTC_LOG(INFO)<<"sandystatsnack added the packet to the list of secondary path mp="<<mp_seq_num<< 
    // " seq= "<<nack_list_s_[mp_seq_num]<<" size "<<nack_list_s_.size();
  }

  // Keep track of new keyframes.
  if (is_keyframe)
    keyframe_list_.insert(seq_num);

  // And remove old ones so we don't accumulate keyframes.
  auto it = keyframe_list_.lower_bound(seq_num - kMaxPacketAge);
  if (it != keyframe_list_.begin())
    keyframe_list_.erase(keyframe_list_.begin(), it);

  if (is_recovered) {
    recovered_list_.insert(seq_num);

    // Remove old ones so we don't accumulate recovered packets.
    auto it = recovered_list_.lower_bound(seq_num - kMaxPacketAge);
    if (it != recovered_list_.begin())
      recovered_list_.erase(recovered_list_.begin(), it);

    // Do not send nack for packets recovered by FEC or RTX.
    return 0;
  }
  //sandy: Sandesh here changed +1 to +2 because sequence number gap is 2
  if(initialized_mp_ && pathid!=2 && mp_seq_num!=newest_seq_num_p_)
    AddPacketsToNack(newest_seq_num_ + 1, seq_num,  newest_seq_num_p_+1, mp_seq_num,pathid);
  else if(initialized_mp_ && pathid==2 && mp_seq_num!=newest_seq_num_s_)
    AddPacketsToNack(newest_seq_num_ + 1, seq_num,  newest_seq_num_s_+1, mp_seq_num,pathid);
  else if(!initialized_mp_)
    AddPacketsToNack(newest_seq_num_ + 1, seq_num,0,0,pathid);//sandy: Run default algorithm using -2
  
  // if(mp_seq_num==seq_num)//sandy: If mp_seq and seq are same then it is single path

  //   AddPacketsToNack(newest_seq_num_ + 1, seq_num,pathid);
  // else if(mp_seq_num!=0)
  //   AddPacketsToNack(newest_seq_num_ + 2, seq_num,pathid);
  newest_seq_num_ = seq_num;
  if(initialized_mp_ && pathid!=2){
    newest_seq_num_p_=mp_seq_num;
  }else if(initialized_mp_ && pathid==2){
    newest_seq_num_s_=mp_seq_num;
  }

  // Are there any nacks that are waiting for this seq_num.
  std::vector<uint16_t> nack_batch = GetNackBatch(kSeqNumOnly,1);
  if (!nack_batch.empty()) {
    // This batch of NACKs is triggered externally; the initiator can
    // batch them with other feedback messages.
    // RTC_LOG(INFO)<<"sandystatsnack sending the primary nack list "<<nack_batch.size();
    nack_sender_->SendNack(nack_batch, /*buffering_allowed=*/true,1);//sandy: send NACK batch for primary path
  }
  nack_batch = GetNackBatch(kSeqNumOnly,2);
  if (!nack_batch.empty()) {
    // This batch of NACKs is triggered externally; the initiator can
    // batch them with other feedback messages.
    // RTC_LOG(INFO)<<"sandystatsnack sending the secondary nack list "<<nack_batch.size();
    nack_sender_->SendNack(nack_batch, /*buffering_allowed=*/true,2);//sandy: send NACK batch for secondary path
  }

  return 0;
}

void NackModule2::ClearUpTo(uint16_t seq_num) {
  // Called via RtpVideoStreamReceiver2::FrameContinuous on the network thread.
  worker_thread_->PostTask(ToQueuedTask(task_safety_, [seq_num, this]() {
    RTC_DCHECK_RUN_ON(worker_thread_);
    nack_list_.erase(nack_list_.begin(), nack_list_.lower_bound(seq_num));
    keyframe_list_.erase(keyframe_list_.begin(),
                         keyframe_list_.lower_bound(seq_num));
    recovered_list_.erase(recovered_list_.begin(),
                          recovered_list_.lower_bound(seq_num));
  }));
}

void NackModule2::UpdateRtt(int64_t rtt_ms) {
  RTC_DCHECK_RUN_ON(worker_thread_);
  rtt_ms_ = rtt_ms;
}

bool NackModule2::RemovePacketsUntilKeyFrame() {
  // Called on worker_thread_.
  while (!keyframe_list_.empty()) {
    auto it = nack_list_.lower_bound(*keyframe_list_.begin());

    if (it != nack_list_.begin()) {
      // We have found a keyframe that actually is newer than at least one
      // packet in the nack list.
      nack_list_.erase(nack_list_.begin(), it);
      return true;
    }

    // If this keyframe is so old it does not remove any packets from the list,
    // remove it from the list of keyframes and try the next keyframe.
    keyframe_list_.erase(keyframe_list_.begin());
  }
  return false;
}
// void NackModule2::SetMpSeq(uint16_t seq_num){
//   RTC_DCHECK_RUN_ON(worker_thread_);

//   mp_seq_num=seq_num;
// }
void NackModule2::AddPacketsToNack(uint16_t seq_num_start, uint16_t seq_num_end, 
  uint16_t mp_seq_num_start,uint16_t mp_seq_num_end,int pathid) {
  // Called on worker_thread_.
  // Remove old packets.

  // RTC_LOG(INFO)<<"sandystatsnackstatsnack adding packet to nack received"<<seq_num_start<<" from pathid ="<<pathid<< 
  // " mp_seq start "<<mp_seq_num_start<<" mp_seq_end "<<mp_seq_num_end<<"\n";
  mp_seq_num_=mp_seq_num_end;

  auto it = nack_list_.lower_bound(seq_num_end - kMaxPacketAge);
  nack_list_.erase(nack_list_.begin(), it);
  uint16_t num_new_nacks=0;
  if( pathid!=2 && mp_seq_num_start>0){//sandy:Primary path nack handler
      num_new_nacks=ForwardDiff(mp_seq_num_start,mp_seq_num_end);
      // RTC_LOG(INFO)<<"sandystatsnack the primary NACK gap is "<<num_new_nacks<<" s: "<< 
      // mp_seq_num_start <<" e: "<<mp_seq_num_end<<" last mp= "<<newest_seq_num_p_<<" last seq= " 
      // <<seq_num_start-1<<" cur seq= "<<seq_num_end<<"\n";
  }else if( pathid==2 && mp_seq_num_start>0){//sandy:secondary path nack handler
      num_new_nacks=ForwardDiff(mp_seq_num_start,mp_seq_num_end);
      // RTC_LOG(INFO)<<"sandystatsnack the secondary NACK gap is "<<num_new_nacks<<" s: "<<mp_seq_num_start  
      //  <<" e: "<<mp_seq_num_end<<" last mp= "<<newest_seq_num_s_<<" last seq= " 
      // <<seq_num_start-1<<" cur seq= "<<seq_num_end<<"\n";
  }
  else{
    num_new_nacks=ForwardDiff(seq_num_start,seq_num_end);
    // RTC_LOG(INFO)<<"sandystatsnack the default NACK gap "<<num_new_nacks<<" s: "<< 
    //   seq_num_start <<" e: "<<seq_num_end<<"\n";
  }
  //sandy: Packets received here are already sorted and hence no need to keeo reorder buffer here
  // if(num_new_nacks< kMpNackTherhsold){//Nack thershold ,sandy: I added this as re-orering could be happening so
  //   return;
  // }
  // uint64_t p_nack_size=0;
  // uint64_t s_nack_size=0;
  // p_nack_size=GetNackSize(1);
  // s_nack_size=GetNackSize(2);
  // RTC_LOG(INFO) << "sandystatsnack: NACK list size "<<pathid<< 
  //       "Primary size "<<p_nack_size<<" secondary path nack size "<< 
  //       s_nack_size<<" total "<<nack_list_.size()<<" Is Mp enabled? "<<initialized_mp_ <<" diff "<<num_new_nacks 
  //       <<" total max "<<kMaxNackPackets;

  if (nack_list_.size() + num_new_nacks > kMaxNackPackets) {
      while (RemovePacketsUntilKeyFrame() &&
             nack_list_.size() + num_new_nacks > kMaxNackPackets) {
      }

      if (nack_list_.size() + num_new_nacks > kMaxNackPackets) {
        nack_list_.clear();
        keyframe_request_sender_->RequestKeyFrame();//sandy: yet to implement this
        return;
      }
  }

  if( pathid!=2 && mp_seq_num_end>0 && num_new_nacks>kMpGapThreshold){
    InsertPacketNackPrimary(mp_seq_num_start-1,mp_seq_num_end,seq_num_end,pathid);
  }else if( pathid==2 && mp_seq_num_end>0 && num_new_nacks>kMpGapThreshold){
    InsertPacketNackSecondary(mp_seq_num_start-1,mp_seq_num_end,seq_num_end,pathid);
  }else if(!initialized_mp_) {
    InsertPacketNack(seq_num_start, mp_seq_num_end,seq_num_end,pathid);
  }
  
  
}
void NackModule2::InsertPacketNackPrimary(uint16_t original_seq,uint16_t mp_seq_num,
                                   uint16_t seq_num_end,int pathid){
  
    

     auto it=nack_list_p_.find(original_seq);
      if(it!=nack_list_p_.end()){
        original_seq=it->second+2;
        // RTC_LOG(INFO)<<"sandystatsnack the starting seq for primary = "<<original_seq<<" the ending seq_num_end= "<< 
        // seq_num_end<<" mp_seq = "<<mp_seq_num<<"\n";
      }else{
        return;
      }

      // RTC_LOG(INFO)<<"sandystatsnack the insertion logic primary"<<original_seq<<" from pathid ="<<pathid<<" mp_seq: " 
      //   <<mp_seq_num<<" : "<<mp_seq_num_<<"\n";

      for (uint16_t seq_num =original_seq; seq_num !=seq_num_end; (seq_num+=2)) {//sandy: +1 is for Mp-WebRTC
        
        if (recovered_list_.find(seq_num) != recovered_list_.end())
          continue;
        RTC_DCHECK(nack_list_.find(seq_num) == nack_list_.end());
        NackInfo nack_info(seq_num, seq_num + WaitNumberOfPackets(0.5),
                           clock_->TimeInMilliseconds(),pathid);
        
        nack_list_[seq_num] = nack_info; //sandy: Adding to the NACK list
        RTC_LOG(INFO)<<"sandystatsnackstatsnack insterted into primary nack list "<<seq_num<<" from pathid ="<<pathid<< 
        " mp_seq: "<<mp_seq_num<<" : "<<mp_seq_num_<<" nack size is "<<nack_list_.size()<<"\n";
      }
     
  
}

void NackModule2::InsertPacketNackSecondary(uint16_t original_seq,uint16_t mp_seq_num,
                                   uint16_t seq_num_end,int pathid){

  
    auto it=nack_list_s_.find(original_seq);
      if(it!=nack_list_s_.end()){
        original_seq=it->second+2;
        // RTC_LOG(INFO)<<"sandystatsnack the starting seq for secondary = "<<original_seq<<" the ending seq_num_end= "<< 
        // seq_num_end<<" mp_seq = "<<mp_seq_num<<"\n";
      }else{
        return;
      }

    // RTC_LOG(INFO)<<"sandystatsnack the insertion logic secondary"<<original_seq<<" from pathid ="<<pathid<<" mp_seq: " 
    //     <<mp_seq_num<<" : "<<mp_seq_num_<<"\n";

    for (uint16_t seq_num = original_seq; seq_num !=seq_num_end;(seq_num+=2)) {//sandy: +1 is for Mp-WebRTC
        
        if (recovered_list_.find(seq_num) != recovered_list_.end())
          continue;
        RTC_DCHECK(nack_list_.find(seq_num) == nack_list_.end());
        NackInfo nack_info(seq_num,seq_num + WaitNumberOfPackets(0.5),
                           clock_->TimeInMilliseconds(),pathid);
        
        
        nack_list_[seq_num] = nack_info;//sandy: Adding to the NACK list
        RTC_LOG(INFO)<<"sandystatsnackstatsnack insterted into secondary nack list "<<seq_num<<" from pathid ="<< 
        pathid<<" mp_seq: "<<mp_seq_num<<" : "<<mp_seq_num_<<" nack size is "<<nack_list_.size()<<"\n";
    }
  
  
}

void NackModule2::InsertPacketNack(uint16_t seq_num_start,uint16_t mp_seq_num,
                                   uint16_t seq_num_end,int pathid){
    // RTC_LOG(INFO)<<"sandystatsnack the insertion logic "<<seq_num_end<<" from pathid ="<<pathid<<" mp_seq: " 
    //     <<mp_seq_num<<" : "<<mp_seq_num_<<"\n";
    // if(mp_seq_num!=seq_num_end){
    //   if(pathid!=2)
    //     InsertPacketNackPrimary(newest_seq_num_p_,mp_seq_num,seq_num_end,pathid);
    //   else
    //     InsertPacketNackSecondary(newest_seq_num_s_,mp_seq_num,seq_num_end,pathid);
    // }
    for (uint16_t seq_num = seq_num_start; seq_num != seq_num_end; ++seq_num) {//sandy: +1 is for Mp-WebRTC
        if (recovered_list_.find(seq_num) != recovered_list_.end())
          continue;
        NackInfo nack_info(seq_num, seq_num + WaitNumberOfPackets(0.5),
                           clock_->TimeInMilliseconds(),pathid);
        RTC_DCHECK(nack_list_.find(seq_num) == nack_list_.end());
        
        nack_list_[seq_num] = nack_info;//sandy: Adding to the NACK list
        RTC_LOG(INFO)<<"sandystatsnack insterted into default nack list "<<seq_num<<" from pathid ="<<pathid<<  
        " mp_seq "<<mp_seq_num_<<" : "<<mp_seq_num_<<" nack size is "<<nack_list_.size()<<"\n";
    }
    
  }

//sandy: Adding the below function to get the total nack size of each path
uint64_t NackModule2::GetNackSize(int pathid){
  uint64_t count=0;
  auto it = nack_list_.begin();
  while (it != nack_list_.end()) {
    if(pathid==it->second.pathid_){
      count++;
    }
  }
  return count; 
}
std::vector<uint16_t> NackModule2::GetNackBatch(NackFilterOptions options,int pathid) {
  // Called on worker_thread_.

  bool consider_seq_num = options != kTimeOnly;
  bool consider_timestamp = options != kSeqNumOnly;
  Timestamp now = clock_->CurrentTime();
  std::vector<uint16_t> nack_batch;

  auto it = nack_list_.begin();
  while (it != nack_list_.end()) {
    TimeDelta resend_delay = TimeDelta::Millis(rtt_ms_);
    if (backoff_settings_) {
      resend_delay =
          std::max(resend_delay, backoff_settings_->min_retry_interval);
      if (it->second.retries > 1) {
        TimeDelta exponential_backoff =
            std::min(TimeDelta::Millis(rtt_ms_), backoff_settings_->max_rtt) *
            std::pow(backoff_settings_->base, it->second.retries - 1);
        resend_delay = std::max(resend_delay, exponential_backoff);
      }
    }

    bool delay_timed_out =
        now.ms() - it->second.created_at_time >= send_nack_delay_ms_;
    bool nack_on_rtt_passed =
        now.ms() - it->second.sent_at_time >= resend_delay.ms();
    bool nack_on_seq_num_passed =
        it->second.sent_at_time == -1 &&
        AheadOrAt(newest_seq_num_, it->second.send_at_seq_num);
    if (delay_timed_out && ((consider_seq_num && nack_on_seq_num_passed) ||
                            (consider_timestamp && nack_on_rtt_passed))) {
      if(pathid==it->second.pathid_){
        //RTC_LOG(INFO)<<"sandystatsnack the pathid for the batch prepared is "<<it->second.pathid_<<"\n";
        nack_batch.emplace_back(it->second.seq_num);
        ++it->second.retries;
        it->second.sent_at_time = now.ms();
        if (it->second.retries >= kMaxNackRetries) {
          RTC_LOG(LS_WARNING) << "Sequence number " << it->second.seq_num
                              << " removed from NACK list due to max retries.";
          it = nack_list_.erase(it);
        } else {
          ++it;
        }
        continue;
      }
    }
    ++it;
  }
  return nack_batch;
}

void NackModule2::UpdateReorderingStatistics(uint16_t seq_num) {
  // Running on worker_thread_.
  RTC_DCHECK(AheadOf(newest_seq_num_, seq_num));
  uint16_t diff = ReverseDiff(newest_seq_num_, seq_num);
  reordering_histogram_.Add(diff);
}

int NackModule2::WaitNumberOfPackets(float probability) const {
  // Called on worker_thread_;
  if (reordering_histogram_.NumValues() == 0)
    return 0;
  return reordering_histogram_.InverseCdf(probability);
}

}  // namespace webrtc
