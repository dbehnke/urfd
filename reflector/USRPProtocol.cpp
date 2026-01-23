//  Copyright © 2015 Jean-Luc Deltombe (LX3JL). All rights reserved.

// urfd -- The universal reflector
// Copyright © 2023 Thomas A. Early N7TAE
// Copyright © 2023 Doug McLain AD8DP
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "Defines.h"
#include <string.h>
#include "USRPClient.h"
#include "USRPProtocol.h"

#include "Global.h"

const uint8_t USRP_TYPE_VOICE = 0;
const uint8_t USRP_TYPE_TEXT  = 2;

const uint8_t USRP_KEYUP_FALSE  = 0;
const uint8_t USRP_KEYUP_TRUE   = 1;

const uint8_t TLV_TAG_SET_INFO = 8;

////////////////////////////////////////////////////////////////////////////////////////
// operation

bool CUSRPProtocol::Initialize(const char *type, const EProtocol ptype, const uint16_t port, const bool has_ipv4, const bool has_ipv6)
{
	CBuffer buffer;
	m_uiStreamId = 0;
	std::ifstream file;
	std::streampos size;

	// base class, create the listing port for the read-write client
	if (! CProtocol::Initialize(type, ptype, port, has_ipv4, has_ipv6))
		return false;

	m_Module = g_Configure.GetAutolinkModule(g_Keys.usrp.module);

	// create the one special USRP Tx/Rx client
	auto scs = g_Configure.GetString(g_Keys.usrp.callsign);
	if (scs.compare("NONE"))
	{
		m_Callsign.SetCallsign(scs, false);
		m_txPort = g_Configure.GetUnsigned(g_Keys.usrp.txport);
		CIp ip(AF_INET, m_txPort, g_Configure.GetString(g_Keys.usrp.ip).c_str());
		auto newclient = std::make_shared<CUSRPClient>(m_Callsign, ip);
		newclient->SetReflectorModule(m_Module);
		g_Reflector.GetClients()->AddClient(newclient);
		g_Reflector.ReleaseClients();
	}

	// now create "listen-only" clients, as many as specified
	if (g_Configure.Contains(g_Keys.usrp.filepath))
		file.open(g_Configure.GetString(g_Keys.usrp.filepath), std::ios::in | std::ios::binary | std::ios::ate);
	if ( file.is_open() )
	{
		// read file
		size = file.tellg();
		if ( size > 0 )
		{
			// read file into buffer
			buffer.resize((int)size+1);
			file.seekg (0, std::ios::beg);
			file.read((char *)buffer.data(), (int)size);

			// close file
			file.close();
		}
	}

	if ( buffer.size() > 0 )
	{
		char *ptr1 = (char *)buffer.data();
		char *ptr2;

		while ( (ptr2 = ::strchr(ptr1, '\n')) != nullptr )
		{
			*ptr2 = 0;
			char *ip;
			char *port;
			char *clientcs;

			if ( ((ip = ::strtok(ptr1, ";")) != nullptr) &&
				((port = ::strtok(nullptr, ";")) != nullptr) &&
				((clientcs = ::strtok(nullptr, ";")) != nullptr) )
			{
				uint16_t ui = atoi(port);
				CIp Ip(AF_INET, ui, ip);
				CCallsign cs;
				cs.SetCallsign(clientcs, false);
				auto newclient = std::make_shared<CUSRPClient>(cs, Ip);
				newclient->SetReflectorModule(m_Module);
				g_Reflector.GetClients()->AddClient(newclient);
				g_Reflector.ReleaseClients();
			}
			ptr1 = ptr2+1;
		}
	}

	// update time
	m_LastKeepaliveTime.start();

	m_bControlEnabled = g_Configure.GetBoolean(g_Keys.dashboard.control_enable);

	// done
	return true;
}



////////////////////////////////////////////////////////////////////////////////////////
// task

void CUSRPProtocol::Task(void)
{
	CBuffer   Buffer;
	CIp       Ip;
	CCallsign Callsign;
	char      ToLinkModule;
	std::unique_ptr<CDvHeaderPacket> Header;
	std::unique_ptr<CDvFramePacket>  Frame;

	// process registrations
	HandleRegistrations();

	// handle incoming packets
#if USRP_IPV6==true
#if USRP_IPV4==true
	if ( ReceiveDS(Buffer, Ip, 20) )
#else
	if ( Receive6(Buffer, Ip, 20) )
#endif
#else
	if ( Receive4(Buffer, Ip, 20) )
#endif
	{
		// crack the packet
		if ( IsValidDvPacket(Ip, Buffer, Header, Frame) )
		{
			// push the packet
			OnDvFramePacketIn(Frame, &Ip);
		}
		else if( IsValidDvHeaderPacket(Ip, Buffer, Header) )
		{
			// callsign muted?
			if ( g_GateKeeper.MayTransmit(Header->GetMyCallsign(), Ip, EProtocol::usrp) )
			{
				OnDvHeaderPacketIn(Header, Ip);
			}
		}
		else if ( IsValidDvLastPacket(Buffer) )
		{
			// do nothing
			std::cout << "USRP_KEYUP_FALSE received" << std::endl; // DEBUG
		}
		else
		{
			// invalid packet
			std::string title("Unknown USRP packet from ");
			title += Ip.GetAddress();
			Buffer.Dump(title);
		}
	}

	// handle end of streaming timeout
	CheckStreamsTimeout();

	// handle queue from reflector
	HandleQueue();

	// keep client alive
	if ( m_LastKeepaliveTime.time() > USRP_KEEPALIVE_PERIOD )
	{
		//
		HandleKeepalives();

		// update time
		m_LastKeepaliveTime.start();
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// streams helpers

void CUSRPProtocol::OnDvHeaderPacketIn(std::unique_ptr<CDvHeaderPacket> &Header, const CIp &Ip)
{
	// find the stream
	auto stream = GetStream(Header->GetStreamId(), &Ip);
	if ( stream )
	{
		// stream already open
		// skip packet, but tickle the stream
		stream->Tickle();
	}
	else
	{
		// no stream open yet, open a new one
		CCallsign my(Header->GetMyCallsign());
		CCallsign rpt1(Header->GetRpt1Callsign());
		CCallsign rpt2(Header->GetRpt2Callsign());

		// find this client
		std::shared_ptr<CClient>client = g_Reflector.GetClients()->FindClient(Ip, EProtocol::usrp);
		if ( client )
		{
			// get client callsign
			rpt1 = client->GetCallsign();
			auto m = client->GetReflectorModule();
			Header->SetRpt2Module(m);
			rpt2.SetCSModule(m);
			// and try to open the stream
			if ( (stream = g_Reflector.OpenStream(Header, client)) != nullptr )
			{
				// keep the handle
				m_Streams[stream->GetStreamId()] = stream;
			}
		}
		// release
		g_Reflector.ReleaseClients();

		// update last heard
		g_Reflector.GetUsers()->Hearing(my, rpt1, rpt2, EProtocol::usrp);
		g_Reflector.ReleaseUsers();
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// queue helper

void CUSRPProtocol::HandleQueue(void)
{
	while (! m_Queue.IsEmpty())
	{
		// get the packet
		auto packet = m_Queue.Pop();

		// get our sender's id
		const auto module = packet->GetPacketModule();
		CBuffer buffer;
		
		// DIAGNOSTIC: Track packet processing
		static uint32_t packetCount = 0;
		bool isHeader = packet->IsDvHeader();
		bool isFrame = packet->IsDvFrame();
		bool isLastPacket = packet->IsLastPacket();
		bool shouldLog = isHeader || isLastPacket || (++packetCount % 10 == 1);
		
		if (shouldLog) {
			std::cout << "USRPProtocol::HandleQueue #" << packetCount 
			          << " - Module=" << module
			          << ", Header=" << (isHeader ? "YES" : "no")
			          << ", Frame=" << (isFrame ? "YES" : "no")
			          << (isLastPacket ? ", FINAL PACKET" : "")
			          << std::endl;
		}

		// check if it's header and update cache
		if ( packet->IsDvHeader() )
		{
			// this relies on queue feeder setting valid module id
			// m_StreamsCache[module] will be created if it doesn't exist
			m_StreamsCache[module].m_dvHeader = CDvHeaderPacket((const CDvHeaderPacket &)*packet.get());
			m_StreamsCache[module].m_iSeqCounter = 0;
			EncodeUSRPHeaderPacket(m_StreamsCache[module].m_dvHeader, m_StreamsCache[module].m_iSeqCounter++, buffer);
			
			std::cout << "USRPProtocol: HEADER cached for module " << module 
			          << ", callsign=" << m_StreamsCache[module].m_dvHeader.GetMyCallsign().GetCS()
			          << ", encoded " << buffer.size() << " bytes"
			          << std::endl;
		}
		else if ( packet->IsDvFrame() )
		{
			// Get PCM data to check if it's valid
			const CDvFramePacket *framePacket = dynamic_cast<const CDvFramePacket*>(packet.get());
			const uint8_t *pcmData = nullptr;
			if (framePacket) {
				pcmData = framePacket->GetCodecData(ECodecType::usrp);
			}
			
			static uint32_t frameEncodeCount = 0;
			bool shouldLogFrame = isLastPacket || (++frameEncodeCount % 10 == 1);
			
			if (shouldLogFrame) {
				std::cout << "USRPProtocol: Encoding FRAME #" << frameEncodeCount 
				          << " for module " << module
				          << ", PCM data=" << (pcmData ? "VALID" : "NULL")
				          << (isLastPacket ? ", FINAL PACKET" : "")
				          << std::endl;
			}
			
			EncodeUSRPPacket(m_StreamsCache[module].m_dvHeader, (const CDvFramePacket &)*packet.get(), m_StreamsCache[module].m_iSeqCounter++, buffer, packet->IsLastPacket());
		}

		// send it
		if ( buffer.size() > 0 )
		{
			// DIAGNOSTIC: Track client distribution
			int totalUSRPClients = 0;
			int eligibleClients = 0;
			int sentCount = 0;
			
			// and push it to all our clients linked to the module and who are not streaming in
			CClients *clients = g_Reflector.GetClients();
			auto it = clients->begin();
			std::shared_ptr<CClient>client = nullptr;
			while ( (client = clients->FindNextClient(EProtocol::usrp, it)) != nullptr )
			{
				totalUSRPClients++;
				
				bool isMaster = client->IsAMaster();
				char clientModule = client->GetReflectorModule();
				bool moduleMatches = (clientModule == module);
				bool shouldSend = !isMaster && moduleMatches;
				
				// DIAGNOSTIC: Log each client's state (only for headers or every 10th frame)
				static uint32_t clientCheckCount = 0;
				if (isHeader || (++clientCheckCount % 10 == 1)) {
					std::cout << "  USRP client: " << client->GetCallsign().GetCS()
					          << " @ " << client->GetIp().GetAddress()
					          << " - Module=" << clientModule
					          << ", IsMaster=" << (isMaster ? "YES" : "no")
					          << ", ModuleMatch=" << (moduleMatches ? "YES" : "no")
					          << ", WillSend=" << (shouldSend ? "YES" : "NO")
					          << std::endl;
				}
				
				// is this client busy ?
				if ( shouldSend )
				{
					eligibleClients++;
					// no, send the packet
					Send(buffer, client->GetIp());
					sentCount++;
				}
			}
			g_Reflector.ReleaseClients();
			
			// DIAGNOSTIC: Summary of distribution
			static uint32_t distCount = 0;
			if (isHeader || (++distCount % 10 == 1)) {
				std::cout << "USRPProtocol: Packet distribution summary - "
				          << "TotalUSRPClients=" << totalUSRPClients
				          << ", Eligible=" << eligibleClients
				          << ", Sent=" << sentCount
				          << ", BufferSize=" << buffer.size()
				          << std::endl;
			}
		}
	}
}


////////////////////////////////////////////////////////////////////////////////////////
// packet decoding helpers

bool CUSRPProtocol::IsValidDvPacket(const CIp &Ip, const CBuffer &Buffer, std::unique_ptr<CDvHeaderPacket> &header, std::unique_ptr<CDvFramePacket> &frame)
{
	if(!memcmp(Buffer.data(), "USRP", 4) && (Buffer.size() == 352) && (Buffer.data()[20] == USRP_TYPE_VOICE) && (Buffer.data()[15] == USRP_KEYUP_TRUE) )
	{
		auto stream = GetStream(m_uiStreamId, &Ip);
		if ( stream && !stream->IsOpen() ) {
			// Stream exists but is closed. This happens after a forced NNG reconnection.
			// We MUST remove it from m_Streams so that a new stream ID can be generated
			// and Reflector::OpenStream will accept it.
			m_Streams.erase(m_uiStreamId);
			stream = nullptr; // force finding/creation logic below
		}
		
		if ( !stream )
		{
			m_uiStreamId = static_cast<uint32_t>(::rand());
			CCallsign csMY;
			
			// check map if control enabled
			bool bFound = false;
			if (m_bControlEnabled) {
				std::lock_guard<std::mutex> lock(m_IpMapMutex);
				auto it = m_IpMap.find(Ip.GetAddr());
				if (it != m_IpMap.end()) {
					csMY = it->second;
					bFound = true;
				}
			}
			
			if (!bFound)
				csMY = m_Callsign;
			
			CCallsign rpt1 = m_Callsign;
			CCallsign rpt2 = m_ReflectorCallsign;
			rpt1.SetCSModule(m_Module);
			rpt2.SetCSModule(' ');
			header = std::unique_ptr<CDvHeaderPacket>(new CDvHeaderPacket(csMY, CCallsign("CQCQCQ"), rpt1, rpt2, m_uiStreamId, true));
			OnDvHeaderPacketIn(header, Ip);
		}

		int16_t pcm[160];
		for(int i = 0; i < 160; ++i){
			pcm[i] = (Buffer.data()[32+(i*2)+1] << 8) | Buffer.data()[32+(i*2)];
		}

		frame = std::unique_ptr<CDvFramePacket>(new CDvFramePacket(pcm, m_uiStreamId, false));
		return true;
	}
	return false;
}

bool CUSRPProtocol::IsValidDvHeaderPacket(const CIp &Ip, const CBuffer &Buffer, std::unique_ptr<CDvHeaderPacket> &header)
{
	if(!memcmp(Buffer.data(), "USRP", 4) && (Buffer.size() == 352) && (Buffer.data()[20] == USRP_TYPE_TEXT) && (Buffer.data()[32] == TLV_TAG_SET_INFO) ){
		auto stream = GetStream(m_uiStreamId, &Ip);
		if ( stream && !stream->IsOpen() ) {
			m_Streams.erase(m_uiStreamId);
			stream = nullptr;
		}

		if ( !stream )
		{
			uint32_t uiSrcId = ((Buffer.data()[1] << 16) | ((Buffer.data()[2] << 8) & 0xff00) | (Buffer.data()[3] & 0xff));
			m_uiStreamId = static_cast<uint32_t>(::rand());
			CCallsign csMY = CCallsign("", uiSrcId);
			CCallsign rpt1 = CCallsign("", uiSrcId);
			CCallsign rpt2 = m_ReflectorCallsign;
			rpt1.SetCSModule(m_Module);
			rpt2.SetCSModule(' ');
			header = std::unique_ptr<CDvHeaderPacket>(new CDvHeaderPacket(csMY, CCallsign("CQCQCQ"), rpt1, rpt2, m_uiStreamId, true));
		}
		return true;
	}
	return false;
}

bool CUSRPProtocol::IsValidDvLastPacket(const CBuffer &Buffer)
{
	if(!memcmp(Buffer.data(), "USRP", 4) && (Buffer.size() == 32) && (Buffer.data()[15] == USRP_KEYUP_FALSE) )
	{
		return true;
	}
	return false;
}

void CUSRPProtocol::EncodeUSRPHeaderPacket(const CDvHeaderPacket &Header, uint32_t iSeq, CBuffer &Buffer) const
{
	std::string cs = Header.GetMyCallsign().GetCS();
	const uint32_t cnt = htonl(iSeq);
	Buffer.resize(352);
	memset(Buffer.data(), 0, 352);
	memcpy(Buffer.data(), "USRP", 4);
	memcpy(Buffer.data() + 4, &cnt, 4);
	Buffer.data()[15] = USRP_KEYUP_FALSE;
	Buffer.data()[20] = USRP_TYPE_TEXT;
	Buffer.data()[32] = TLV_TAG_SET_INFO;
	Buffer.data()[33] = 13 + cs.size();
	memcpy(Buffer.data()+46, cs.c_str(), cs.size());
}

void CUSRPProtocol::EncodeUSRPPacket(const CDvHeaderPacket &Header, const CDvFramePacket &Frame, uint32_t iSeq, CBuffer &Buffer, bool last) const
{
	Buffer.resize(352);
	::memcpy(Buffer.data(), "USRP", 4);

	// seq
	Buffer.data()[4] = (iSeq >> 24) & 0xFF;
	Buffer.data()[5] = (iSeq >> 16) & 0xFF;
	Buffer.data()[6] = (iSeq >> 8) & 0xFF;
	Buffer.data()[7] = iSeq & 0xFF;

	// memory
	//Buffer.data()[8] = (usage >> 24) & 0xFF;
	//Buffer.data()[9] = (usage >> 16) & 0xFF;
	//Buffer.data()[10] = (usage >> 8) & 0xFF;
	//Buffer.data()[11] = usage & 0xFF;

	// keyups
	Buffer.data()[15] = (last) ? USRP_KEYUP_FALSE : USRP_KEYUP_TRUE;

	// type
	Buffer.data()[20] = USRP_TYPE_VOICE;

	// audio
	const uint8_t *pAudio = Frame.GetCodecData(ECodecType::usrp);
	
	// DIAGNOSTIC: Check PCM data validity
	static uint32_t encodeFrameCount = 0;
	if (++encodeFrameCount % 10 == 1) {
		if (pAudio) {
			// Calculate rough amplitude to detect silence
			const int16_t *pcmSamples = reinterpret_cast<const int16_t*>(pAudio);
			int32_t sumAbs = 0;
			for (int i = 0; i < 160; i++) {
				sumAbs += std::abs(pcmSamples[i]);
			}
			int avgAbs = sumAbs / 160;
			
			std::cout << "USRPProtocol::EncodeUSRPPacket #" << encodeFrameCount
			          << " - Seq=" << iSeq
			          << ", PCM=VALID"
			          << ", AvgAmplitude=" << avgAbs
			          << ", IsLast=" << (last ? "YES" : "no")
			          << ", Callsign=" << Header.GetMyCallsign().GetCS()
			          << std::endl;
		} else {
			std::cout << "USRPProtocol::EncodeUSRPPacket #" << encodeFrameCount
			          << " - Seq=" << iSeq
			          << ", PCM=NULL (ERROR!)"
			          << ", IsLast=" << (last ? "YES" : "no")
			          << ", Callsign=" << Header.GetMyCallsign().GetCS()
			          << std::endl;
		}
	}
	
	if (pAudio)
	{
		::memcpy(Buffer.data() + 32, pAudio, 320);
	}
	else
	{
		// If no PCM data, fill with silence (zeros)
		::memset(Buffer.data() + 32, 0, 320);
		
		static uint32_t nullDataCount = 0;
		if (++nullDataCount % 5 == 1) {
			std::cerr << "USRPProtocol: WARNING - NULL PCM data in frame, sending silence! (count=" 
			          << nullDataCount << ")" << std::endl;
		}
	}
}

void CUSRPProtocol::RegisterClient(const std::string &ip, const std::string &callsign)
{
	if (!m_bControlEnabled) return;

	uint32_t addr = CIp(ip.c_str()).GetAddr();
	CCallsign cs(callsign);

	{
		std::lock_guard<std::mutex> lock(m_IpMapMutex);
		m_IpMap[addr] = cs;
	}

	{
		std::lock_guard<std::mutex> lock(m_RegistrationMutex);
		m_RegistrationQueue.push_back({ip, callsign});
	}
}

void CUSRPProtocol::HandleRegistrations()
{
	std::deque<std::pair<std::string, std::string>> queue;
	{
		std::lock_guard<std::mutex> lock(m_RegistrationMutex);
		if (m_RegistrationQueue.empty()) return;
		queue.swap(m_RegistrationQueue);
	}

	for (const auto& req : queue) {
		std::string ip = req.first;
		std::string callsign = req.second;
		uint32_t addr = CIp(ip.c_str()).GetAddr();

		// Check if we need to close an existing stream for this IP
		// iterate all streams in base class
		// Note: We are now in the Task thread, so accessing m_Streams is safe!
		auto it = m_Streams.begin();
		while (it != m_Streams.end()) {
			auto stream = it->second;
			const CIp* ownerIp = stream->GetOwnerIp();
			if (ownerIp && ownerIp->GetAddr() == addr) {
				if (stream->IsOpen()) {
					std::cout << "USRP: Force closing stream for " << ip << " to update callsign to " << callsign << std::endl;
					
					// Manually demote client and close stream to avoid Reflector::CloseStream blocking wait
					// and ensure OpenStream will succeed (it checks IsAMaster())
					g_Reflector.GetClients(); // Locks clients
					auto client = stream->GetOwnerClient();
					if (client) client->NotAMaster();
					g_Reflector.ReleaseClients(); // Unlocks clients
					
					stream->ClosePacketStream();
					
					// Since we closed the stream, we should probably remove it from m_Streams map 
					// or let the next packet handling do it?
					// The original code didn't remove it from m_Streams, but CProtocol does invalidation.
					// But wait, iterating m_Streams while modifying it is dangerous.
					// CProtocol::m_Streams is an unordered_map.
					// ClosePacketStream() doesn't remove from map.
					// However, IsValidDvPacket/IsValidDvHeaderPacket logic handles expired/closes streams by checking IsOpen().
					// So leaving it in the map but Closed is 'safe' ish, as long as we don't invalidate iterators if we were erasing.
					// We are NOT erasing here, just calling methods on the object.
				}
			}
			++it;
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// keepalive helpers

void CUSRPProtocol::HandleKeepalives(void)
{
	// iterate on clients
	CClients *clients = g_Reflector.GetClients();
	auto it = clients->begin();
	std::shared_ptr<CClient>client = nullptr;
	while ( (client = clients->FindNextClient(EProtocol::usrp, it)) != nullptr )
	{
		// is this client busy ?
		//if ( client->IsAMaster() )
		//{
			// yes, just tickle it
			client->Alive();
		//}
		// check it's still with us
		//else if ( !client->IsAlive() )
		//{
			// no, remove it
			//std::cout << "USRP client " << client->GetCallsign() << " keepalive timeout" << std::endl;
			//clients->RemoveClient(client);
		//}

	}
	g_Reflector.ReleaseClients();
}
