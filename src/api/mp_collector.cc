#include "api/mp_collector.h"

#include "modules/rtp_rtcp/source/byte_io.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include <vector>
#include <deque>

using namespace cricket;
using namespace std;

// namespace webrtc {
// namespace rtcp {


#include "p2p/base/ice_controller_factory_interface.h"
#include "p2p/base/ice_controller_interface.h"
#include "p2p/base/connection.h"
#include "p2p/base/connection_info.h"
#include "p2p/base/p2p_transport_channel.h"

MpCollector::MpCollector(){}

int MpCollector::MpGetLocalCandidates() {
	return 10;
}

void MpCollector::MpStoreCandidate(std::string ip, int port,uint32_t priority,std::string transport_name,  
		uint16_t network_id,uint16_t network_cost,std::string protocol) {
	RTC_LOG(INFO)<<"sandy: adding the candidate\n";
	SandyCandidate sandycandidate;
	sandycandidate.addr=ip;
	sandycandidate.port=port;
	sandycandidate.priority=priority;
	sandycandidate.transport_name=transport_name;
	sandycandidate.network_id=network_id;
	sandycandidate.network_cost=network_cost;
	sandycandidate.protocol=protocol;
	sandycandidates.push_back(sandycandidate);
	RTC_LOG(INFO)<<"sandy: added the candidate\t"<<sandycandidates.size();
	
}
void MpCollector::MpPrintCandidates()const{
	RTC_LOG(INFO)<<"sandy: candidate from stored \t"<<sandycandidates.size()<<"\t";
	for (auto it: sandycandidates)
		RTC_LOG(INFO)<<"Addr:"<<it.addr 
						<<"\t"<<"port:"<<it.port  
						<<"\t"<<"priority:"<<it.priority 
						<<"\t"<<"transport_name:"<<it.transport_name 
						<<"\t"<<"network_id:"<<it.network_id 
						<<"\t"<<"network_cost:"<<it.network_cost 
						<<"\t"<<"protocol:"<<it.protocol  
						<<"\n";

}

void MpCollector::MpStoreConnections (std::string local_ip,int local_port,std::string remote_ip,int remote_port,int received){
	SandyConnection sandyconnection;
	sandyconnection.local_addr=local_ip;
	sandyconnection.local_port=local_port;
	sandyconnection.remote_addr=remote_ip;
	sandyconnection.remote_port=remote_port;
	sandyconnection.received=received;
	sandyconnections.push_back(sandyconnection);
	RTC_LOG(INFO)<<"sandy: added the connection\t"<<sandyconnections.size()<<" "<< 
	sandyconnection.local_addr<<":"<<sandyconnection.local_port<<" to "<<sandyconnection.remote_addr<<":"<<sandyconnection.remote_port<<" received? "<<sandyconnection.received<<"\n";
}
 void MpCollector::MpPrintConnections() const{
 	RTC_LOG(INFO)<<"sandy: secondary connection stored";
 	for(auto connection: sandyconnections){
 		RTC_LOG(INFO)<<"connection local addr:"<<connection.local_addr 
 						<<"\t local port:"<<connection.local_port 
 						<<"\t remote addr:"<<connection.remote_addr 
 						<<"\t remote port:"<<connection.remote_port 
 						<<"\n";
 	}
 }


bool  MpCollector::MpGetLocalConnection (SandyConnection *sandyconnection){
	for(auto conn: sandyconnections){
		if(!conn.received){
			sandyconnection->local_addr=conn.local_addr;
			sandyconnection->local_port=conn.local_port;
			sandyconnection->remote_addr=conn.remote_addr;
			sandyconnection->remote_port=conn.remote_port;
			sandyconnection->received=conn.received;
			return 1;
		}
	}
	return 0;
}

bool MpCollector::MpGetReceivedConnection (SandyConnection *sandyconnection){
	for(auto conn: sandyconnections){
		if(conn.received){
			sandyconnection->local_addr=conn.local_addr;
			sandyconnection->local_port=conn.local_port;
			sandyconnection->remote_addr=conn.remote_addr;
			sandyconnection->remote_port=conn.remote_port;
			sandyconnection->received=conn.received;
			return 1;
		}
	}
	return 0;
}

bool MpCollector::MpCheckConnections(std::string local_ip,int local_port,std::string remote_ip,int remote_port){
	if(local_port==0 && remote_port==0 && (local_ip.compare("0.0.0.0")==0) && (remote_ip.compare("0.0.0.0")==0)){
		return 1;
	}
	for (auto conn: sandyconnections){
		if(local_ip==conn.local_addr && local_port==conn.local_port && 
		   remote_ip==conn.remote_addr && remote_port==conn.remote_port )
				return 1;
	}
	return 0;
}



// }//end of rtcp
// } // end of webrtc
