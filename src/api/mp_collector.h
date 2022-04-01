/*
Support file to collect information from webrtc

*/
#ifndef API_MP_COLLECTOR_H_
#define API_MP_COLLECTOR_H_


#include <memory>
#include <string>
#include <vector>

#include "api/turn_customizer.h"
#include "p2p/base/port_allocator.h"
#include "p2p/client/relay_port_factory_interface.h"
#include "p2p/client/turn_port_factory.h"
#include "rtc_base/checks.h"
#include "rtc_base/network.h"
#include "rtc_base/system/rtc_export.h"
#include "rtc_base/thread.h"
#include "absl/types/optional.h"
#include "api/units/data_rate.h"
#include "api/units/data_size.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "rtc_base/deprecation.h"
#include <deque>





// namespace rtcp {




class RTC_EXPORT MpCollector {
public:
	MpCollector();
	~MpCollector(){}
	struct SandyCandidate{
		std::string addr;
		int port;
		uint32_t priority;
		std::string transport_name;
		uint16_t network_id;
  		uint16_t network_cost;
  		std::string protocol;
	};

	struct SandyConnection{
		std::string local_addr;
		int local_port;
		std::string remote_addr;
		int remote_port;
		int received;
		uint64_t subflow_seq;
		uint32_t subflow_id;
		int rtt;
		int bytes_received;

	};

	std::deque<SandyCandidate> sandycandidates;	
	std::deque<SandyConnection> sandyconnections;
	long primary_congestion_wnd_= 0;
	long secondary_congestion_wnd_= 0;
	uint32_t fps_=0;
	int MPWebRTC_enabled=0;
	int MPSecond_path=0;   //Check if the second path is set
	uint32_t primary_seq=0;//Sequence number of primary path with path id 1
	uint32_t secondary_seq=0;//sequence numner of secondary path with path id 2
	uint32_t total_packets_sent=0;//Total packets from the rtp_sender_egress
	int pathid=0;
	int spath_lock_=0;
	int ppath_lock_=0;
	int best_pathid_=0;
	int halfsignaled_pathid_=0;
	int fullsignaled_pathid_=0;
	double ratio_=0;
	int loss_pathid_=0;
	std::string scheduler_="rr";
	int rtt1_=0;
	int rtt2_=0;
	double loss1_=0;
	double loss2_=0;
	double alpha_=1.0;
	double beta_=0.0;
	int64_t halfsignaltime=0;
	int64_t fullsignaltime=0;
	int halfsignaled_rtt_=0;
	double halfsignaled_loss_=0;
	int fullsignaled_rtt_=0;
	void MpStoreCandidate (std::string ip, int port,uint32_t priority,std::string transport_name, 
		uint16_t network_id,uint16_t network_cost,std::string protocol) ;
	void MpPrintCandidates() const;
	int MpGetLocalCandidates() ;	
	void MpStoreConnections (std::string local_ip,int local_port,std::string remote_ip,int remote_port,int received);
	bool MpGetLocalConnection (SandyConnection *sandyconnection) ;
	bool MpGetReceivedConnection(SandyConnection *sandyconnection);
	void MpPrintConnections()const;
	bool MpCheckConnections(std::string local_ip,int local_port,std::string remote_ip,int remote_port);

	bool MpCheckReceivedInConnections();

	int MpGetMpWebRTCStatus(){//If MP-webRTC is enabled
		return MPWebRTC_enabled;
	}
	void MpSetMpWebRTCStatus(int status){ 
		MPWebRTC_enabled=status;
	}
	int MpGetBestPathId(){//If MP-webRTC is enabled
		return best_pathid_;
	}
	// int MpGetBestPathIdRTT(){//If MP-webRTC is enabled
	// 	return best_pathid_;
	// }
	void MpSetBestPathId(int pathid){ 
		best_pathid_=pathid;
	}


	int MpGetLossBasedPathId(){//If MP-webRTC is enabled
		return loss_pathid_;
	}
	void MpSetLossBasedPathId(int losspathid){ 
		loss_pathid_=losspathid;
	}

	double MpGetRatio(){//If MP-webRTC is enabled
		return ratio_;
	}
	void MpSetRatio(double ratio){ 
		ratio_=ratio;;
	}
	bool MpISsecondPathOpen(){ //Check if Second path in Mp-WebRTC is active?
		return MPSecond_path;
	}
	void MpSetSecondPath(int stat){
		MPSecond_path=stat;
	}
	void MpSetScheduler(std::string scheduler){
		scheduler_=scheduler;
	}
	std::string MpGetScheduler(){
		return scheduler_;
	}
	long MpGetPrimaryWindow(){
		return primary_congestion_wnd_;
	}
	long MpGetSecondaryWindow(){
		return secondary_congestion_wnd_;
	}
	void MpSetPrimaryWindow(long pwnd){
		primary_congestion_wnd_=pwnd;
	}
	void MpSetSecondaryWindow(long swnd){
		secondary_congestion_wnd_=swnd;
	}
	void MpSetWindowLock(int pathid){
		if(pathid==1){
			ppath_lock_=1;
		}else{
			spath_lock_=1;
		}
	}
	void MpClearWindowLock(int pathid){
		if(pathid==1){
			ppath_lock_=0;
		}else{
			spath_lock_=0;
		}
	}
	void MpSetFrameRate(uint32_t fps){
		fps_=fps;
	}
	uint32_t MpGetFrameRate(){
		return fps_;
	}

	//sandy  : Path properties
	int MpGetRTT1(){//If MP-webRTC is enabled
		return rtt1_;
	}
	void MpSetRTT1(int rtt1){ 
		rtt1_=rtt1;
	}
	int MpGetRTT2(){//If MP-webRTC is enabled
		return rtt2_;
	}
	void MpSetRTT2(int rtt2){ 
		rtt2_=rtt2;
	}
	double MpGetLoss1(){//If MP-webRTC is enabled
		return loss1_;
	}
	void MpSetLoss1(double loss1){ 
		loss1_=loss1;
	}
	double MpGetLoss2(){//If MP-webRTC is enabled
		return loss2_;
	}
	void MpSetLoss2(double loss2){ 
		loss2_=loss2;
	}
	void MpSetHalfSignal(){
		if(rtt2_<=10 || rtt1_<=10){
			RTC_LOG(INFO)<<"sandyofo half signal are ignored as RTT are not setyet";
			return;
		}
		if(MpGetFullSignal()){//sandy: Skip this signal if full signal is already set
			RTC_LOG(INFO)<<"sandyofo half sigal are ignore as Full signal is set";
					return;
		}
		
		int64_t now=rtc::TimeMillis();
		RTC_LOG(INFO)<<"sandyofo received half signal "<<now;
		if(!halfsignaltime){
			halfsignaltime=now;
			if(MpGetBestPathId()==1){
				if(loss2_!=0)
					halfsignaled_loss_=loss2_;
				halfsignaled_rtt_=rtt2_;
				MpSetHalfSignaledPathId(2);
				RTC_LOG(INFO)<<now<<"sandyofo reduce the traffic on path 2 by half RTT: "<<rtt2_<<" Loss: "<<halfsignaled_loss_  
				<<" RTT1: "<<rtt1_<<" RTT2: "<<rtt2_<<" Loss1 "<<loss1_<<" Loss 2 "<<loss2_;
			}else{
				if(loss1_!=0)
					halfsignaled_loss_=loss1_;//sandy: It is loss enabled	
				halfsignaled_rtt_=rtt1_;
				MpSetHalfSignaledPathId(1);
				RTC_LOG(INFO)<<now<<"sandyofo reduce the traffic on path 1 by half RTT:"<<rtt1_<<" Loss: "<<halfsignaled_loss_ 
				<<" RTT1: "<<rtt1_<<" RTT2: "<<rtt2_<<" Loss1 "<<loss1_<<" Loss 2 "<<loss2_;
			}
			alpha_/=2;
		}
		else if(now-halfsignaltime>100){
			//sandy: If Half siganl is already set
			if(alpha_<1.0){
				if(halfsignaled_pathid_==1){
					//sandy: Half signal is set on primary path
					if(MpGetBestPathId()==1){
						MpClearHalfSignal();
						RTC_LOG(INFO)<<now<<"sandyofo reduce the traffic on path 2 by half with changing worst path RTT: "  
						<<rtt2_<<" Loss: "<<halfsignaled_loss_<<" RTT1: "<<rtt1_<<" RTT2: "<<rtt2_<<" Loss1 "<<loss1_<<" Loss 2 "<<loss2_;
						if(loss2_!=0)
							halfsignaled_loss_=loss2_;
						halfsignaled_rtt_=rtt2_;
						MpSetHalfSignaledPathId(2);
						alpha_=1/2;
					}else
						alpha_/=2;
				}else if(halfsignaled_pathid_==2){
					if(MpGetBestPathId()==2){
						MpClearHalfSignal();
						RTC_LOG(INFO)<<now<<"sandyofo reduce the traffic on path 1 by half with changing worst path RTT:" 
						<<rtt1_<<" Loss: "<<halfsignaled_loss_<<" RTT1: "<<rtt1_<<" RTT2: "<<rtt2_<<" Loss1 "<<loss1_<<" Loss 2 "<<loss2_;
						MpSetHalfSignaledPathId(1);
						alpha_=1/2;
						if(loss1_!=0)
							halfsignaled_loss_=loss1_;//sandy: It is loss enabled
						halfsignaled_rtt_=rtt1_;
					}else
						alpha_/=2;
				}	
			}	
			else{
				if(MpGetBestPathId()==1){
					if(loss2_!=0)
						halfsignaled_loss_=loss2_;
					halfsignaled_rtt_=rtt2_;
					MpSetHalfSignaledPathId(2);
					RTC_LOG(INFO)<<now<<"sandyofo reduce the traffic on path 2 by half RTT: "<<rtt2_<<" Loss: "<<halfsignaled_loss_
					<<" RTT1: "<<rtt1_<<" RTT2: "<<rtt2_<<" Loss1 "<<loss1_<<" Loss 2 "<<loss2_;
				}else{
					if(loss1_!=0)
						halfsignaled_loss_=loss1_;//sandy: It is loss enabled
					halfsignaled_rtt_=rtt1_;
					MpSetHalfSignaledPathId(1);
					RTC_LOG(INFO)<<now<<"sandyofo reduce the traffic on path 1 by half RTT:"<<rtt1_<<" Loss: "<<halfsignaled_loss_
					<<" RTT1: "<<rtt1_<<" RTT2: "<<rtt2_<<" Loss1 "<<loss1_<<" Loss 2 "<<loss2_;
				}
				alpha_/=2;
			}
			halfsignaltime=now;
		}
	}
	double MpGetHalfSignal(){
		return alpha_;
	}
	void MpClearHalfSignal(){
		alpha_=1.0;
		halfsignaled_rtt_=0;
		halfsignaled_loss_=0;
	}
	
	void MpSetFullSignal(){
		if(rtt2_<=10 || rtt1_<=10)
			return;
		//sandy: Skip this signal if full signal is already set
		RTC_LOG(LS_INFO)<<"sandyofo received full signal and let us see if it gets activated";
		int64_t now=rtc::TimeMillis();
		if(MpGetFullSignal()){
				RTC_LOG(LS_INFO)<<"sandyofo the full signal is already enabled";
				return;
		}
		MpClearHalfSignal();//sandy: Clear all the pending half signal
		//sandy: Ignore the signal if the RTT is too small
		// if(MpGetBestPathId()==1 && rtt2_<10){
		// 	RTC_LOG(INFO)<<now<<"sandyofo ignoring the full signal on path 2 as RTT is small "<<rtt2_<<" signaled RTT "<<fullsignaled_rtt_;
		// 	return;
		// }
		// if(MpGetBestPathId()==2 && rtt1_<10){
		// 	RTC_LOG(INFO)<<now<<"sandyofo ignoring the full signal on path 1 as RTT is small "<<rtt1_<<" signaled RTT "<<fullsignaled_rtt_;
		// 	return;
		// }

		if(!fullsignaltime){
			fullsignaltime=now;
			beta_=1.0;	
			if(MpGetBestPathId()==1){//sandy: This could be loss enabled best path and hence please check the delay as well
				if(rtt1_/2>(rtt2_/2)*500){//sandy: If the delay difference between two paths is too big, that path with higher delay will increse e2e
					fullsignaled_rtt_=rtt1_;
					MpSetFullSignaledPathId(1);
					RTC_LOG(INFO)<<now<<"sandyofo reduce the traffic on slow path to one packet 1 by e2e signaled "<<fullsignaled_rtt_<< 
					" RTT1: "<<rtt1_<<" RTT2: "<<rtt2_<<" Loss1 "<<loss1_<<" Loss 2 "<<loss2_;
				}else{
					fullsignaled_rtt_=rtt2_;
					MpSetFullSignaledPathId(2);
					RTC_LOG(INFO)<<now<<"sandyofo reduce the traffic on slow path to one packet 2 "<<rtt2_<<" signaled "<<fullsignaled_rtt_<< 
					" RTT1: "<<rtt1_<<" RTT2: "<<rtt2_<<" Loss1 "<<loss1_<<" Loss 2 "<<loss2_;
				}
			}else{
				if(rtt2_/2>500*(rtt1_/2)){
					fullsignaled_rtt_=rtt2_;
					MpSetFullSignaledPathId(2);
					RTC_LOG(INFO)<<now<<"sandyofo reduce the traffic on slow path to one packet 1 by e2e signaled "<<fullsignaled_rtt_<< 
					" RTT1: "<<rtt1_<<" RTT2: "<<rtt2_<<" Loss1 "<<loss1_<<" Loss 2 "<<loss2_;
				}else{
					fullsignaled_rtt_=rtt1_;
					MpSetFullSignaledPathId(1);
					RTC_LOG(INFO)<<now<<"sandyofo reduce the traffic on slow path to one packet 1 "<<rtt1_<<" signaled "<<fullsignaled_rtt_<< 
					" RTT1: "<<rtt1_<<" RTT2: "<<rtt2_<<" Loss1 "<<loss1_<<" Loss 2 "<<loss2_;
				}
			}
		}
		else if(now- fullsignaltime>100){
			// RTC_LOG(INFO)<<now<<"sandyofo redcuce the traffic on slow path to one packet ";
			beta_=1.0;	
			if(MpGetBestPathId()==1){
				if(rtt1_/2>(rtt2_/2)*500){//sandy: If the delay difference between two paths is too big, that path with higher delay will increse e2e
					fullsignaled_rtt_=rtt1_;
					MpSetFullSignaledPathId(1);
					RTC_LOG(INFO)<<now<<"sandyofo reduce the traffic on slow path to one packet 1 by e2e signaled "<<fullsignaled_rtt_<< 
					" RTT1: "<<rtt1_<<" RTT2: "<<rtt2_<<" Loss1 "<<loss1_<<" Loss 2 "<<loss2_;
				}else{
					fullsignaled_rtt_=rtt2_;
					MpSetFullSignaledPathId(2);
					RTC_LOG(INFO)<<now<<"sandyofo reduce the traffic on slow path to one packet 2 "<<rtt2_<<" signaled "<<fullsignaled_rtt_<< 
					" RTT1: "<<rtt1_<<" RTT2: "<<rtt2_<<" Loss1 "<<loss1_<<" Loss 2 "<<loss2_;
				}
			}else{
				if(rtt2_/2>500*(rtt1_/2)){
					fullsignaled_rtt_=rtt2_;
					MpSetFullSignaledPathId(2);
					RTC_LOG(INFO)<<now<<"sandyofo reduce the traffic on slow path to one packet 1 by e2e signaled "<<fullsignaled_rtt_<< 
					" RTT1: "<<rtt1_<<" RTT2: "<<rtt2_<<" Loss1 "<<loss1_<<" Loss 2 "<<loss2_;
				}else{
					fullsignaled_rtt_=rtt1_;
					MpSetFullSignaledPathId(1);
					RTC_LOG(INFO)<<now<<"sandyofo reduce the traffic on slow path to one packet 1 "<<rtt1_<<" signaled "<<fullsignaled_rtt_<< 
					" RTT1: "<<rtt1_<<" RTT2: "<<rtt2_<<" Loss1 "<<loss1_<<" Loss 2 "<<loss2_;
				}
			}
		}
		fullsignaltime=now;
	}
	void MpClearFullSignal(){
		beta_=0.0;
		fullsignaled_rtt_=0;
	}
	double MpGetFullSignal(){
		return beta_;
	}
	int MpGetHalfSignaledRtt(){
		return halfsignaled_rtt_;
	}
	double MpGetHalfSignalLoss(){
		return halfsignaled_loss_;
	}
	int MpGetFullSignaledRtt(){
		return fullsignaled_rtt_;
	}
	void MpSetHalfSignaledRtt(int rtt){
		halfsignaled_rtt_=rtt;
	}
	void MpSetFullSignaledRtt(int rtt){
		fullsignaled_rtt_=rtt;
	}
	int MpGetHalfSignaledPathId(){//If MP-webRTC is enabled
		return halfsignaled_pathid_;
	}
	void MpSetHalfSignaledPathId(int pathid){ 
		halfsignaled_pathid_=pathid;
	} 
	int MpGetFullSignaledPathId(){//If MP-webRTC is enabled
		return fullsignaled_pathid_;
	}
	void MpSetFullSignaledPathId(int pathid){ 
		fullsignaled_pathid_=pathid;
	} 
};



#endif  // P2P_CLIENT_MPCOLLECTOR_H_
