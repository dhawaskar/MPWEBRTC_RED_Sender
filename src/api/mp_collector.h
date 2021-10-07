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
	long primary_congestion_wnd_= 1200;
	long secondary_congestion_wnd_= 1200;
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
	double ratio_=0;
	std::string scheduler_="rr";

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
	void MpSetBestPathId(int pathid){ 
		best_pathid_=pathid;
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

};



#endif  // P2P_CLIENT_MPCOLLECTOR_H_
