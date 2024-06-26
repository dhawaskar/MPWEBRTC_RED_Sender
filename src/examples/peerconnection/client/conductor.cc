/*
 *  Copyright 2012 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "examples/peerconnection/client/conductor.h"

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <utility>
#include <vector>

#include "absl/memory/memory.h"
#include "absl/types/optional.h"
#include "api/audio/audio_mixer.h"
#include "api/audio_codecs/audio_decoder_factory.h"
#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_options.h"
#include "api/create_peerconnection_factory.h"
#include "api/rtp_sender_interface.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "examples/peerconnection/client/defaults.h"
#include "modules/audio_device/include/audio_device.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h"
#include "p2p/base/port_allocator.h"
#include "pc/video_track_source.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/rtc_certificate_generator.h"
#include "rtc_base/strings/json.h"
#include "test/vcm_capturer.h"
#include "api/mp_collector.h"
#include "api/mp_global.h"
int Mpdevice=0;
int num_devices=0;
namespace {
// Names used for a IceCandidate JSON object.
const char kCandidateSdpMidName[] = "sdpMid";
const char kCandidateSdpMlineIndexName[] = "sdpMLineIndex";
const char kCandidateSdpName[] = "candidate";

// Names used for a SessionDescription JSON object.
const char kSessionDescriptionTypeName[] = "type";
const char kSessionDescriptionSdpName[] = "sdp";

class DummySetSessionDescriptionObserver
    : public webrtc::SetSessionDescriptionObserver {
 public:
  static DummySetSessionDescriptionObserver* Create() {
    return new rtc::RefCountedObject<DummySetSessionDescriptionObserver>();
  }
  virtual void OnSuccess() { RTC_LOG(INFO) << __FUNCTION__; }
  virtual void OnFailure(webrtc::RTCError error) {
    RTC_LOG(INFO) << __FUNCTION__ << " " << ToString(error.type()) << ": "
                  << error.message();
  }
};

class CapturerTrackSource : public webrtc::VideoTrackSource {
 public:
  static rtc::scoped_refptr<CapturerTrackSource> Create() {
    // const size_t kWidth = 4096;//1920
    // const size_t kHeight = 2160;//1080
    const size_t kWidth = 1920;//640;//1920
    const size_t kHeight = 1080;//480;//1080
    // const size_t kWidth = 920;
    // const size_t kHeight = 480;
    // const size_t kWidth = 3840;//2048;//1920
    // const size_t kHeight = 2160;//;//1080
    // const size_t kWidth = 2048;//2048;//1920
    //  const size_t kHeight = 1080;//;//1080
    const size_t kFps = 30;
    std::unique_ptr<webrtc::test::VcmCapturer> capturer;
    std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
        webrtc::VideoCaptureFactory::CreateDeviceInfo());
    if (!info) {
      return nullptr;
    }
    num_devices = info->NumberOfDevices();
    
    for (int i = 0; i < num_devices; ++i) {
      RTC_LOG(INFO)<<"sandycamera device number "<<i;
    }
    for (int i = 0; i < num_devices; ++i) {
      // sandy: Below code is to block specific device.
      // if(i!=1)
      //   continue;
      if(Mpdevice>0 && Mpdevice-1==i && num_devices>1){
        RTC_LOG(INFO)<<"sandycamera the number of devices "<<num_devices<<" skipping "<<i;
        continue;
      }
      capturer = absl::WrapUnique(
          webrtc::test::VcmCapturer::Create(kWidth, kHeight, kFps, i));
      if (capturer) {
        Mpdevice=i+1;
        RTC_LOG(INFO)<<"sandycamera the number of devices "<<num_devices<<"Device number "<<Mpdevice;
        return new rtc::RefCountedObject<CapturerTrackSource>(
            std::move(capturer));
      }
    }

    return nullptr;
  }

 protected:
  explicit CapturerTrackSource(
      std::unique_ptr<webrtc::test::VcmCapturer> capturer)
      : VideoTrackSource(/*remote=*/false), capturer_(std::move(capturer)) {}

 private:
  rtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return capturer_.get();
  }
  std::unique_ptr<webrtc::test::VcmCapturer> capturer_;
};

}  // namespace

Conductor::Conductor(PeerConnectionClient* client, MainWindow* main_wnd,MainWindow* mp_main_wnd,MainWindow* mp_main_wnd1)
    : peer_id_(-1), loopback_(false), client_(client), main_wnd_(main_wnd),mp_main_wnd_(mp_main_wnd),mp_main_wnd1_(mp_main_wnd1) {
  client_->RegisterObserver(this);
  main_wnd->RegisterObserver(this);
  if(mp_main_wnd)mp_main_wnd_->RegisterObserver(this);
  if(mp_main_wnd1)mp_main_wnd1_->RegisterObserver(this);
}

Conductor::~Conductor() {
  RTC_DCHECK(!peer_connection_);
}

bool Conductor::connection_active() const {
  return peer_connection_ != nullptr;
}

void Conductor::Close() {
  client_->SignOut();
  DeletePeerConnection();
}

bool Conductor::InitializePeerConnection() {
  RTC_DCHECK(!peer_connection_factory_);
  RTC_DCHECK(!peer_connection_);

  peer_connection_factory_ = webrtc::CreatePeerConnectionFactory(
      nullptr /* network_thread */, nullptr /* worker_thread */,
      nullptr /* signaling_thread */, nullptr /* default_adm */,
      webrtc::CreateBuiltinAudioEncoderFactory(),
      webrtc::CreateBuiltinAudioDecoderFactory(),
      webrtc::CreateBuiltinVideoEncoderFactory(),
      webrtc::CreateBuiltinVideoDecoderFactory(), nullptr /* audio_mixer */,
      nullptr /* audio_processing */);

  if (!peer_connection_factory_) {
    main_wnd_->MessageBox("Error", "Failed to initialize PeerConnectionFactory",
                          true);
    DeletePeerConnection();
    return false;
  }

  if (!CreatePeerConnection(/*dtls=*/true)) {
    main_wnd_->MessageBox("Error", "CreatePeerConnection failed", true);
    DeletePeerConnection();
  }
 AddTracks();

  return peer_connection_ != nullptr;
}

bool Conductor::ReinitializePeerConnectionForLoopback() {
  loopback_ = true;
  std::vector<rtc::scoped_refptr<webrtc::RtpSenderInterface>> senders =
      peer_connection_->GetSenders();
  peer_connection_ = nullptr;
  if (CreatePeerConnection(/*dtls=*/false)) {
    for (const auto& sender : senders) {
      peer_connection_->AddTrack(sender->track(), sender->stream_ids());
    }
    peer_connection_->CreateOffer(
        this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  }
  return peer_connection_ != nullptr;
}

bool Conductor::CreatePeerConnection(bool dtls) {
  RTC_DCHECK(peer_connection_factory_);
  RTC_DCHECK(!peer_connection_);

  webrtc::PeerConnectionInterface::RTCConfiguration config;
  //sandy: Please set the gathering state to continuous
  config.continual_gathering_policy=webrtc::PeerConnectionInterface::GATHER_CONTINUALLY;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  config.enable_dtls_srtp = dtls;
  webrtc::PeerConnectionInterface::IceServer server,server1;
  //sandy: Set your relay or turn server here please
  server.uri = GetPeerConnectionString();
  // std::vector<std::string> url_string,url_string1;
  // url_string.push_back("turn:128.110.219.35:3478?transport=udp");
  // server.urls=url_string;
  // server.username="sandy";
  // server.password="sandy";
  // config.servers.push_back(server);
  // //sandy: Relay 2
  // url_string1.push_back("turn:128.110.219.126:3478?transport=udp");
  // server1.urls=url_string1;
  // server1.username="sandy";
  // server1.password="sandy";
  // config.servers.push_back(server1);

  peer_connection_ = peer_connection_factory_->CreatePeerConnection(
      config, nullptr, nullptr, this);
  return peer_connection_ != nullptr;
}

void Conductor::DeletePeerConnection() {
  main_wnd_->StopLocalRenderer();
  main_wnd_->StopRemoteRenderer();
  if(mp_main_wnd_){
    mp_main_wnd_->StopLocalRenderer();
    mp_main_wnd_->StopRemoteRenderer();
  }
  if(mp_main_wnd1_){
    mp_main_wnd1_->StopLocalRenderer();
    mp_main_wnd1_->StopRemoteRenderer();
  }
  peer_connection_ = nullptr;
  peer_connection_factory_ = nullptr;
  peer_id_ = -1;
  loopback_ = false;
}

void Conductor::EnsureStreamingUI() {
  RTC_DCHECK(peer_connection_);
  if (main_wnd_->IsWindow()) {
    if (main_wnd_->current_ui() != MainWindow::STREAMING)
      main_wnd_->SwitchToStreamingUI();
  }
  if(mp_main_wnd_ && mp_main_wnd_->IsWindow()){
    if (mp_main_wnd_->current_ui() != MainWindow::STREAMING)
      mp_main_wnd_->SwitchToStreamingUI();
  }
  if(mp_main_wnd1_ && mp_main_wnd1_->IsWindow()){
    if (mp_main_wnd1_->current_ui() != MainWindow::STREAMING)
      mp_main_wnd1_->SwitchToStreamingUI();
  }
  
}

//
// PeerConnectionObserver implementation.
//

void Conductor::OnAddTrack(
    rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
    const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>&
        streams) {
  RTC_LOG(INFO) << __FUNCTION__ << " " << receiver->id()<<"media_type="<<receiver->media_type();
  if(receiver->media_type()==1 && video_track_count==0){
    main_wnd_->QueueUIThreadCallback(NEW_TRACK_ADDED,
                                   receiver->track().release());
    video_track_count++;
  }else if(mp_main_wnd_ && receiver->media_type()==1 && video_track_count==1){
    mp_main_wnd_->QueueUIThreadCallback(MP_NEW_TRACK_ADDED,
                                   receiver->track().release());
    video_track_count++;
  }else if(mp_main_wnd1_ && receiver->media_type()==1 && video_track_count==2){
    mp_main_wnd1_->QueueUIThreadCallback(MP_NEW_TRACK_ADDED1,
                                   receiver->track().release());
  }
  else{
    main_wnd_->QueueUIThreadCallback(NEW_TRACK_ADDED,
                                   receiver->track().release());
  }
}

void Conductor::OnRemoveTrack(
    rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
  RTC_LOG(INFO) << __FUNCTION__ << " " << receiver->id();
  main_wnd_->QueueUIThreadCallback(TRACK_REMOVED, receiver->track().release());
  if(mp_main_wnd_)mp_main_wnd_->QueueUIThreadCallback(TRACK_REMOVED, receiver->track().release());
  if(mp_main_wnd1_)mp_main_wnd1_->QueueUIThreadCallback(TRACK_REMOVED, receiver->track().release());
}

void Conductor::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
  RTC_LOG(INFO) << __FUNCTION__ << " " << candidate->sdp_mline_index();
  // For loopback test. To save some connecting delay.
  if (loopback_) {
    if (!peer_connection_->AddIceCandidate(candidate)) {
      RTC_LOG(WARNING) << "Failed to apply the received candidate";
    }
    return;
  }
  std::string sandy_candidate;
  candidate->ToString(&sandy_candidate);
  RTC_LOG(INFO)<<"sandycandidate: "<< sandy_candidate;
  // if(sandy_candidate.find("host",0,4)!=std::string::npos){
  //   RTC_LOG(INFO)<<"sandycandidate: this is host and skipping";
  //   return;
  // }
  // if(sandy_candidate.find("192.168.241.187",0,std::strlen("192.168.241.187"))!=std::string::npos){
  //   RTC_LOG(INFO)<<"sandycandidate: this is not a good ip";
  //   return;
  // }
  Json::StyledWriter writer;
  Json::Value jmessage;

  jmessage[kCandidateSdpMidName] = candidate->sdp_mid();
  jmessage[kCandidateSdpMlineIndexName] = candidate->sdp_mline_index();
  std::string sdp;
  if (!candidate->ToString(&sdp)) {
    RTC_LOG(LS_ERROR) << "Failed to serialize candidate";
    return;
  }
  jmessage[kCandidateSdpName] = sdp;
  SendMessage(writer.write(jmessage));
}

//
// PeerConnectionClientObserver implementation.
//

void Conductor::OnSignedIn() {
  RTC_LOG(INFO) << __FUNCTION__;
  main_wnd_->SwitchToPeerList(client_->peers());
  if(mp_main_wnd_)mp_main_wnd_->SwitchToPeerList(client_->peers());//sandycamera: Here is where you need to add all the other windows.
   if(mp_main_wnd1_)mp_main_wnd1_->SwitchToPeerList(client_->peers());//sandycamera: Here is where you need to add all the other windows.
}

void Conductor::OnDisconnected() {
  RTC_LOG(INFO) << __FUNCTION__;

  DeletePeerConnection();

  if (main_wnd_->IsWindow())
    main_wnd_->SwitchToConnectUI();
  if (mp_main_wnd_ &&mp_main_wnd_->IsWindow())
    mp_main_wnd_->SwitchToConnectUI();
  if (mp_main_wnd1_ &&mp_main_wnd1_->IsWindow())
    mp_main_wnd1_->SwitchToConnectUI();
}

void Conductor::OnPeerConnected(int id, const std::string& name) {
  RTC_LOG(INFO) << __FUNCTION__;
  // Refresh the list if we're showing it.
  if (main_wnd_->current_ui() == MainWindow::LIST_PEERS)
    main_wnd_->SwitchToPeerList(client_->peers());
}

void Conductor::OnPeerDisconnected(int id) {
  RTC_LOG(INFO) << __FUNCTION__;
  if (id == peer_id_) {
    RTC_LOG(INFO) << "Our peer disconnected";
    main_wnd_->QueueUIThreadCallback(PEER_CONNECTION_CLOSED, NULL);
  } else {
    // Refresh the list if we're showing it.
    if (main_wnd_->current_ui() == MainWindow::LIST_PEERS)
      main_wnd_->SwitchToPeerList(client_->peers());
  }
}

void Conductor::OnMessageFromPeer(int peer_id, const std::string& message) {
  RTC_DCHECK(peer_id_ == peer_id || peer_id_ == -1);
  RTC_DCHECK(!message.empty());

  if (!peer_connection_.get()) {
    RTC_DCHECK(peer_id_ == -1);
    peer_id_ = peer_id;

    if (!InitializePeerConnection()) {
      RTC_LOG(LS_ERROR) << "Failed to initialize our PeerConnection instance";
      client_->SignOut();
      return;
    }
  } else if (peer_id != peer_id_) {
    RTC_DCHECK(peer_id_ != -1);
    RTC_LOG(WARNING)
        << "Received a message from unknown peer while already in a "
           "conversation with a different peer.";
    return;
  }

  Json::Reader reader;
  Json::Value jmessage;
  if (!reader.parse(message, jmessage)) {
    RTC_LOG(WARNING) << "Received unknown message. " << message;
    return;
  }
  std::string type_str;
  std::string json_object;

  rtc::GetStringFromJsonObject(jmessage, kSessionDescriptionTypeName,
                               &type_str);
  if (!type_str.empty()) {
    if (type_str == "offer-loopback") {
      // This is a loopback call.
      // Recreate the peerconnection with DTLS disabled.
      if (!ReinitializePeerConnectionForLoopback()) {
        RTC_LOG(LS_ERROR) << "Failed to initialize our PeerConnection instance";
        DeletePeerConnection();
        client_->SignOut();
      }
      return;
    }
    absl::optional<webrtc::SdpType> type_maybe =
        webrtc::SdpTypeFromString(type_str);
    if (!type_maybe) {
      RTC_LOG(LS_ERROR) << "Unknown SDP type: " << type_str;
      return;
    }
    webrtc::SdpType type = *type_maybe;
    std::string sdp;
    if (!rtc::GetStringFromJsonObject(jmessage, kSessionDescriptionSdpName,
                                      &sdp)) {
      RTC_LOG(WARNING) << "Can't parse received session description message.";
      return;
    }
    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
        webrtc::CreateSessionDescription(type, sdp, &error);
    if (!session_description) {
      RTC_LOG(WARNING) << "Can't parse received session description message. "
                          "SdpParseError was: "
                       << error.description;
      return;
    }
    RTC_LOG(INFO) << " Received session description :" << message;
    peer_connection_->SetRemoteDescription(
        DummySetSessionDescriptionObserver::Create(),
        session_description.release());
    if (type == webrtc::SdpType::kOffer) {
      peer_connection_->CreateAnswer(
          this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
    }
  } else {
    std::string sdp_mid;
    int sdp_mlineindex = 0;
    std::string sdp;
    if (!rtc::GetStringFromJsonObject(jmessage, kCandidateSdpMidName,
                                      &sdp_mid) ||
        !rtc::GetIntFromJsonObject(jmessage, kCandidateSdpMlineIndexName,
                                   &sdp_mlineindex) ||
        !rtc::GetStringFromJsonObject(jmessage, kCandidateSdpName, &sdp)) {
      RTC_LOG(WARNING) << "Can't parse received message.";
      return;
    }
    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::IceCandidateInterface> candidate(
        webrtc::CreateIceCandidate(sdp_mid, sdp_mlineindex, sdp, &error));
    if (!candidate.get()) {
      RTC_LOG(WARNING) << "Can't parse received candidate message. "
                          "SdpParseError was: "
                       << error.description;
      return;
    }
    if (!peer_connection_->AddIceCandidate(candidate.get())) {
      RTC_LOG(WARNING) << "Failed to apply the received candidate";
      return;
    }
    RTC_LOG(INFO) << " Received candidate :" << message;
  }
}

void Conductor::OnMessageSent(int err) {
  // Process the next pending message if any.
  main_wnd_->QueueUIThreadCallback(SEND_MESSAGE_TO_PEER, NULL);
  if(mp_main_wnd_)mp_main_wnd_->QueueUIThreadCallback(SEND_MESSAGE_TO_PEER, NULL);
if(mp_main_wnd1_)mp_main_wnd1_->QueueUIThreadCallback(SEND_MESSAGE_TO_PEER, NULL);
}

void Conductor::OnServerConnectionFailure() {
  main_wnd_->MessageBox("Error", ("Failed to connect to " + server_).c_str(),
                        true);
}

//
// MainWndCallback implementation.
//

void Conductor::StartLogin(const std::string& server, int port) {
  if (client_->is_connected())
    return;
  server_ = server;
  client_->Connect(server, port, GetPeerName());
}

void Conductor::DisconnectFromServer() {
  if (client_->is_connected())
    client_->SignOut();
}

void Conductor::ConnectToPeer(int peer_id) {
  RTC_DCHECK(peer_id_ == -1);
  RTC_DCHECK(peer_id != -1);

  if (peer_connection_.get()) {
    main_wnd_->MessageBox(
        "Error", "We only support connecting to one peer at a time", true);
    return;
  }

  
  if (InitializePeerConnection()) {
    using RTCOfferAnswerOptions =  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions;
    RTCOfferAnswerOptions sandy_options;
    sandy_options.offer_to_receive_video=0;
    sandy_options.offer_to_receive_audio=0;
    sandy_options.raw_packetization_for_video=true;
    peer_id_ = peer_id;
    peer_connection_->CreateOffer(
        // this, sandy_options);//sandy: Adding options to turn off video and audio
        this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  } else {
    main_wnd_->MessageBox("Error", "Failed to initialize PeerConnection", true);
  }
}

void Conductor::AddTracks() {
  if (!peer_connection_->GetSenders().empty()) {
    return;  // Already added tracks.
  }

  // rtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track(
  //     peer_connection_factory_->CreateAudioTrack(
  //         kAudioLabel, peer_connection_factory_->CreateAudioSource(
  //                          cricket::AudioOptions())));
  // auto result_or_error = peer_connection_->AddTrack(audio_track, {kStreamId});
  // if (!result_or_error.ok()) {
  //   RTC_LOG(LS_ERROR) << "Failed to add audio track to PeerConnection: "
  //                     << result_or_error.error().message();
  // }

  rtc::scoped_refptr<CapturerTrackSource> video_device =
      CapturerTrackSource::Create();
  if (video_device) {
    rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_(
        peer_connection_factory_->CreateVideoTrack(kVideoLabel, video_device));
    main_wnd_->StartLocalRenderer(video_track_);

    //sandy: Forcing the first camera to not send anything
    auto result_or_error = peer_connection_->AddTrack(video_track_, {kStreamId});
    if (!result_or_error.ok()) {
      RTC_LOG(LS_ERROR) << "Failed to add video track to PeerConnection: "
                        << result_or_error.error().message();
    }
    main_wnd_->SwitchToStreamingUI();


  } else {
    RTC_LOG(LS_ERROR) << "OpenVideoCaptureDevice failed";
  }
  // Sandycamera: Add onother yet same video track. You only need to enable this.
  // if(num_devices>1){
    // RTC_LOG(INFO)<<"sandycamera adding the second stream";
    // rtc::scoped_refptr<CapturerTrackSource> video_device1 =
    //   CapturerTrackSource::Create();
   
    // //sandy:second stream
    // rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_1(
    //   peer_connection_factory_->CreateVideoTrack("MpWebRTCTrack", video_device));
    // if(mp_main_wnd_)mp_main_wnd_->StartLocalRenderer(video_track_1);
    //   auto result_or_error = peer_connection_->AddTrack(video_track_1, {"MpWebRTCStream"});
    //   if (!result_or_error.ok()) {
    //     RTC_LOG(LS_ERROR) << "Failed to add video track to PeerConnection: "
    //                       << result_or_error.error().message();
    //  }else{
    //     mpcollector_->MpSetNumberOfCameraStreams(2);
    //  }



    // // // //  //Sandy: Creating third stream
    //  rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_2(
    //   peer_connection_factory_->CreateVideoTrack("MpWebRTCTrack1", video_device));
    // if(mp_main_wnd1_)mp_main_wnd1_->StartLocalRenderer(video_track_2);
    //   result_or_error = peer_connection_->AddTrack(video_track_2, {"MpWebRTCStream1"});
    //   if (!result_or_error.ok()) {
    //     RTC_LOG(LS_ERROR) << "Failed to add video track to PeerConnection: "
    //                       << result_or_error.error().message();
    //  }else{
    //    mpcollector_->MpSetNumberOfCameraStreams(3);
    // }

     
  // }else {
  //   RTC_LOG(LS_ERROR) << "OpenVideoCaptureDevice for second camera failed";
  // }  
  
  if(mp_main_wnd_)mp_main_wnd_->SwitchToStreamingUI();
   if(mp_main_wnd1_)mp_main_wnd1_->SwitchToStreamingUI();
}

void Conductor::DisconnectFromCurrentPeer() {
  RTC_LOG(INFO) << __FUNCTION__;
  if (peer_connection_.get()) {
    client_->SendHangUp(peer_id_);
    DeletePeerConnection();
  }

  if (main_wnd_->IsWindow())
    main_wnd_->SwitchToPeerList(client_->peers());
}

void Conductor::UIThreadCallback(int msg_id, void* data) {
  switch (msg_id) {
    case PEER_CONNECTION_CLOSED:
      RTC_LOG(INFO) << "PEER_CONNECTION_CLOSED";
      DeletePeerConnection();

      if (main_wnd_->IsWindow()) {
        if (client_->is_connected()) {
          main_wnd_->SwitchToPeerList(client_->peers());
        } else {
          main_wnd_->SwitchToConnectUI();
        }
      } else {
        DisconnectFromServer();
      }
      break;

    case SEND_MESSAGE_TO_PEER: {
      RTC_LOG(INFO) << "SEND_MESSAGE_TO_PEER";
      std::string* msg = reinterpret_cast<std::string*>(data);
      if (msg) {
        // For convenience, we always run the message through the queue.
        // This way we can be sure that messages are sent to the server
        // in the same order they were signaled without much hassle.
        pending_messages_.push_back(msg);
      }

      if (!pending_messages_.empty() && !client_->IsSendingMessage()) {
        msg = pending_messages_.front();
        pending_messages_.pop_front();

        if (!client_->SendToPeer(peer_id_, *msg) && peer_id_ != -1) {
          RTC_LOG(LS_ERROR) << "SendToPeer failed";
          DisconnectFromServer();
        }
        delete msg;
      }

      if (!peer_connection_.get())
        peer_id_ = -1;

      break;
    }

    case MP_NEW_TRACK_ADDED: {
      
      auto* track = reinterpret_cast<webrtc::MediaStreamTrackInterface*>(data);
      if(track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind){
        RTC_LOG(INFO)<<" ***** sandycamera second track being added  and put it in seperate window****";
        auto* video_track = static_cast<webrtc::VideoTrackInterface*>(track);
        if(mp_main_wnd_)mp_main_wnd_->StartRemoteRenderer(video_track);
      }
      track->Release();
      break;
    }
    case MP_NEW_TRACK_ADDED1: {
      
      auto* track = reinterpret_cast<webrtc::MediaStreamTrackInterface*>(data);
      if(track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind){
        RTC_LOG(INFO)<<" ***** sandycamera third track being added  and put it in seperate window****";
        auto* video_track = static_cast<webrtc::VideoTrackInterface*>(track);
        if(mp_main_wnd1_)mp_main_wnd1_->StartRemoteRenderer(video_track);
      }
      track->Release();
      break;
    }

    case NEW_TRACK_ADDED: {
      
      auto* track = reinterpret_cast<webrtc::MediaStreamTrackInterface*>(data);
      if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
        RTC_LOG(INFO)<<" ***** sandycamera new track being added ****";
        auto* video_track = static_cast<webrtc::VideoTrackInterface*>(track);
        main_wnd_->StartRemoteRenderer(video_track);
      }
      track->Release();
      break;
    }

    case TRACK_REMOVED: {
      // Remote peer stopped sending a track.
      auto* track = reinterpret_cast<webrtc::MediaStreamTrackInterface*>(data);
      track->Release();
      break;
    }

    default:
      RTC_NOTREACHED();
      break;
  }
}

void Conductor::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
  peer_connection_->SetLocalDescription(
      DummySetSessionDescriptionObserver::Create(), desc);

  std::string sdp;
  desc->ToString(&sdp);

  // For loopback test. To save some connecting delay.
  if (loopback_) {
    // Replace message type from "offer" to "answer"
    std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
        webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp);
    peer_connection_->SetRemoteDescription(
        DummySetSessionDescriptionObserver::Create(),
        session_description.release());
    return;
  }

  Json::StyledWriter writer;
  Json::Value jmessage;
  jmessage[kSessionDescriptionTypeName] =
      webrtc::SdpTypeToString(desc->GetType());
  jmessage[kSessionDescriptionSdpName] = sdp;
  SendMessage(writer.write(jmessage));
}

void Conductor::OnFailure(webrtc::RTCError error) {
  RTC_LOG(LERROR) << ToString(error.type()) << ": " << error.message();
}

void Conductor::SendMessage(const std::string& json_object) {
  std::string* msg = new std::string(json_object);
  main_wnd_->QueueUIThreadCallback(SEND_MESSAGE_TO_PEER, msg);
  // if(mp_main_wnd_)mp_main_wnd_->QueueUIThreadCallback(SEND_MESSAGE_TO_PEER, msg);//sandy
}
