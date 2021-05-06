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
#include <deque>




// namespace webrtc {
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

	int MPWebRTC_enabled=0;
	int MPSecond_path=0;   //Check if the second path is set
	uint32_t primary_seq=0;//Sequence number of primary path with path id 1
	uint32_t secondary_seq=0;//sequence numner of secondary path with path id 2
	uint32_t total_packets_sent=0;//Total packets from the rtp_sender_egress
	int pathid=0;
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
	bool MpISsecondPathOpen(){ //Check if Second path in Mp-WebRTC is active?
		return MPSecond_path;
	}
	std::string MpGetScheduler(){
		return scheduler_;
	}
	void MpSetScheduler(std::string scheduler){
		scheduler_=scheduler;
	}
	void MpSetSecondPath(int stat){
		MPSecond_path=stat;
	}

};



#endif  // P2P_CLIENT_MPCOLLECTOR_H_
