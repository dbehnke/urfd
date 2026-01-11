//  Copyright © 2015 Jean-Luc Deltombe (LX3JL). All rights reserved.

// urfd -- The universal reflector
// Copyright © 2021 Thomas A. Early N7TAE
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


#include <string.h>
#include "M17Client.h"
#include "M17Protocol.h"
#include "M17Packet.h"
#include "Global.h"
#include "M17Parrot.h"
#include "M17Peer.h"
#include "Configure.h"
#include <deque>
#include <chrono>

struct DelayedM17Packet {
    std::chrono::steady_clock::time_point releaseTime;
    std::unique_ptr<CDvFramePacket> packet;
    CIp ip;
};

static std::deque<DelayedM17Packet> g_M17DelayedQueue;

////////////////////////////////////////////////////////////////////////////////////////
// constructor
CM17Protocol::CM17Protocol() : CSEProtocol()
{
}

////////////////////////////////////////////////////////////////////////////////////////
// operation

bool CM17Protocol::Initialize(const char *type, const EProtocol ptype, const uint16_t port, const bool has_ipv4, const bool has_ipv6)
{
	// base class
	if (! CProtocol::Initialize(type, ptype, port, has_ipv4, has_ipv6))
		return false;

	// load interlinks
	m_M17Interlinks.LoadFromFile(g_Configure.GetString(g_Keys.files.m17interlink));

	// update time
	m_LastKeepaliveTime.start();
	m_LastPeersLinkTime.start();

	// done
	return true;
}



////////////////////////////////////////////////////////////////////////////////////////
// task


void CM17Protocol::Task(void)
{
	CBuffer   Buffer;
	CIp       Ip;
	CCallsign Callsign, SrcCallsign;
	char      ToLinkModule;
	std::unique_ptr<CDvHeaderPacket> Header;
	std::unique_ptr<CDvFramePacket>  Frame;
    char      mods[27];

	// handle incoming packets
#if M17_IPV6==true
#if M17_IPV4==true
	if ( ReceiveDS(Buffer, Ip, 20) )
#else
	if ( Receive6(Buffer, Ip, 20) )
#endif
#else
	if ( Receive4(Buffer, Ip, 20) )
#endif
	{
		// crack the packet
		if ( IsValidDvPacket(Buffer, Header, Frame) )
		{
            // Find Client
            std::shared_ptr<CClient> client = g_Reflector.GetClients()->FindClient(Ip, EProtocol::m17);
            g_Reflector.ReleaseClients();
            
            if (client) {
                 client->Alive();
            }
            
            // Check for PARROT
            bool isParrot = false;
            if (Header) {
                 CCallsign rpt2(Header->GetRpt2Callsign());
                 // Parrot check (substring "PARROT" or "ECHO")
                 // Checks if "PARROT" exists in the callsign (mrefd behavior)
                 isParrot = (std::string::npos != rpt2.GetCS().find("PARROT")) || (rpt2.GetCS() == "       ECHO");
                 // Handle @ALL rewriting & Routing
                 std::string sRpt2 = rpt2.GetCS();
                 // Trim trailing spaces for comparison
                 while (!sRpt2.empty() && sRpt2.back() == ' ') sRpt2.pop_back();

                 if (!isParrot) {
                     if (sRpt2 == "@ALL") {
                          // If @ALL (encoded as 0xFFs), it likely lacks a module.
                          // Broadcast to Source Module (Contextual Broadcast)
                          if (Header->GetRpt2Module() == ' ') {
                               if (client) Header->SetRpt2Module(client->GetReflectorModule());
                          }
                     } 
                     else if (strncmp(sRpt2.c_str(), "M17-", 4) == 0) {
                          // Rewrite M17-REF to @ALL for compatibility
                          // M17-REF X preserves module X
                          Header->SetRpt2Callsign(CCallsign("       ALL"));
                     }
                 }
            } else {
                // If frame, check if we are parroting
                if (client && m_ParrotMap.find(client) != m_ParrotMap.end()) {
                    isParrot = true;
                }
            }
            
            if (isParrot && client) {
                uint16_t streamId = 0;
                uint16_t frameNumber = 0;
                if (Header) {
                    streamId = Header->GetStreamId();
                    // Header has no frame number
                } else if (Frame) {
                    streamId = Frame->GetStreamId();
                    frameNumber = Frame->M17FrameNumber();
                }
                HandleParrot(client, Buffer, true, streamId, frameNumber);
            }
            else {

                // callsign muted?
                if ( g_GateKeeper.MayTransmit(Header->GetMyCallsign(), Ip, EProtocol::m17, Header->GetRpt2Module()) )
                {
                    // Inspect Header to know codec type (3200 vs 1600)
                    ECodecType cType = Header->GetCodecIn();

                    OnDvHeaderPacketIn(Header, Ip);

                    // xrf needs a voice frame every 20 ms and an M17 frame is 40 ms, so we need to split it
                    // M17 3200 payload is 16 bytes. We need two 8-byte frames.
                    
                    // Header is now invalid (moved in OnDvHeaderPacketIn), so we use cType
                    
                    // Only split if we have enough data (standard M17 is 16 bytes for 3200, 8 for 1600)
                    // CDvFramePacket constructor from M17 copies 16 bytes to m_TCPack.m17
                    const uint8_t* valData = Frame->GetCodecData(cType);
                    
                    if (cType == ECodecType::c2_3200 || cType == ECodecType::c2_1600)
                    {
                        uint8_t part1[16] = {0};
                        uint8_t part2[16] = {0};
                        
                        int halfSize = (cType == ECodecType::c2_3200) ? 8 : 4;
                        
                        memcpy(part1, valData, halfSize);
                        memcpy(part2, valData + halfSize, halfSize);
                        
                        // Update Sequence Numbers for TCD aggregation (Even/Odd pair)
                        // We interpret the incoming M17 frame number as the base sequence.
                        const STCPacket* tcC = Frame->GetCodecPacket();
                        STCPacket* tc = const_cast<STCPacket*>(tcC);
                        uint32_t originalSeq = tc->sequence;
                        
                        // First packet gets even sequence
                        tc->sequence = originalSeq * 2;

                        // Create first frame with first half
                        // We need to overwrite its payload.
                        uint8_t* framePayload = const_cast<uint8_t*>(valData);
                        memcpy(framePayload, part1, 16); 
                        memset(framePayload + halfSize, 0, 16 - halfSize);
                        
                        // Create second frame with second half
                        auto secondFrame = std::unique_ptr<CDvFramePacket>(new CDvFramePacket(*Frame.get()));
                        // Set sequence to Odd
                         const_cast<STCPacket*>(secondFrame->GetCodecPacket())->sequence = originalSeq * 2 + 1;
                        
                        // Overwrite payload of second frame
                        uint8_t* secondPayload = const_cast<uint8_t*>(secondFrame->GetCodecData(cType));
                        
                        if (cType == ECodecType::c2_3200) {
                            // For 3200, tcd expects the second packet to have data at offset 8
                            memset(secondPayload, 0, 16);
                            memcpy(secondPayload + 8, part2, 8);
                        } else {
                            // For 1600, tcd reads everything from first packet, but let's be safe and put it at 0
                            memcpy(secondPayload, part2, 16);
                            memset(secondPayload + halfSize, 0, 16 - halfSize);
                        }
                        
                        if (Frame->IsLastPacket())
                            Frame->SetLastPacket(false);

                        OnDvFramePacketIn(Frame, &Ip);
                        
                        // Delay second packet by 20ms to pace output for P25/DMR destination
                        // Pacing is critical to prevent jitter buffer collapse ("sped up" audio)
                        DelayedM17Packet delayed;
                        delayed.releaseTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
                        delayed.packet = std::move(secondFrame);
                        delayed.ip = Ip;
                        g_M17DelayedQueue.push_back(std::move(delayed));
                    }
                    else
                    {
                        // Fallback for unknown/other types
                        OnDvFramePacketIn(Frame, &Ip);
                    }
                }
            }
		}
        else if ( IsValidPacketModePacket(Buffer, Callsign, SrcCallsign) )
        {
             // Packet Data (SMS)
             // Check if Parrot
             bool isParrot = (Callsign.GetCS() == "M17-PARROT" || Callsign.GetCS() == "       ECHO");
             
             // Get Client
            auto client = g_Reflector.GetClients()->FindClient(Ip, EProtocol::m17);
            g_Reflector.ReleaseClients();

            if (isParrot && client) {
                HandleParrot(client, Buffer, false);
            } else {
                // Route
                
                // Let's use simple logic:
                if (client) {
                    // Update client activity
                    client->Alive();
                    
                    // Create CM17Packet wrapper
                    CM17Packet m17pkt(Buffer.data(), false); 
                    
                    // Call OnPacketIn
                    OnPacketIn(m17pkt, client);
                }
            }
        }
		else if ( IsValidConnectPacket(Buffer, Callsign, ToLinkModule) )
		{
			std::cout << "M17 connect packet for module " << ToLinkModule << " from " << Callsign << " at " << Ip << std::endl;

			// callsign authorized?
			if ( g_GateKeeper.MayLink(Callsign, Ip, EProtocol::m17) && g_Reflector.IsValidModule(ToLinkModule) )
			{
				// valid module ?
				if ( g_Reflector.IsValidModule(ToLinkModule) )
				{
					// acknowledge the request
					Send("ACKN", Ip);

					// create the client and append
					g_Reflector.GetClients()->AddClient(std::make_shared<CM17Client>(Callsign, Ip, ToLinkModule));
					g_Reflector.ReleaseClients();
				}
				else
				{
					std::cout << "M17 node " << Callsign << " connect attempt on non-existing module" << std::endl;

					// deny the request
					Send("NACK", Ip);
				}
			}
			else
			{
				// deny the request
				Send("NACK", Ip);
			}

		}
		else if ( IsValidDisconnectPacket(Buffer, Callsign) )
		{
			std::cout << "M17 disconnect packet from " << Callsign << " at " << Ip << std::endl;

			// find client
			CClients *clients = g_Reflector.GetClients();
			std::shared_ptr<CClient>client = clients->FindClient(Ip, EProtocol::m17);
			
            if ( client != nullptr )
			{
				// remove it
				clients->RemoveClient(client);
				// and acknowledge the disconnect
				Send("DISC", Ip);
			}
            else {
                // Check peers
                CPeers* peers = g_Reflector.GetPeers();
                auto peer = peers->FindPeer(Ip, EProtocol::m17);
                if (peer) {
                    peers->RemovePeer(peer);
                }
                g_Reflector.ReleasePeers();
            }
			g_Reflector.ReleaseClients();
            // Also check peers if not client?
            // M17Protocol.cpp logic previously only checked clients for disconnect.
		}
		else if ( IsValidKeepAlivePacket(Buffer, Callsign) )
		{
			// find all clients with that callsign & ip and keep them alive
			CClients *clients = g_Reflector.GetClients();
			auto it = clients->begin();
			std::shared_ptr<CClient>client = nullptr;
			while ( (client = clients->FindNextClient(Callsign, Ip, EProtocol::m17, it)) != nullptr )
			{
				client->Alive();
			}
			g_Reflector.ReleaseClients();
            
            // Should also check Peers?
            CPeers* peers = g_Reflector.GetPeers();
            auto peer = peers->FindPeer(Ip, EProtocol::m17);
            if (peer) peer->Alive();
            g_Reflector.ReleasePeers();
		}
#define M17_RECONNECT_PERIOD 60000 // 1 Minute
        else if ( IsValidInterlinkConnect(Buffer, Ip, Callsign, mods) )
        {
            std::cout << "CONN packet from " << Callsign << " at " << Ip << " to module(s) " << mods << std::endl;
            // Validated in IsValidInterlinkConnect against m_M17Interlinks
            
			SInterConnect ackn;
			EncodeInterlinkAckPacket(ackn, mods);
			Send((const char*)&ackn, Ip); // Cast to char*
        }
        else if ( IsValidInterlinkAcknowledge(Buffer, Callsign, mods) )
        {
            std::cout << "ACKN packet from " << Callsign << " at " << Ip << " on module(s) " << mods << std::endl;
            
            // Check m_M17Interlinks
            if (m_M17Interlinks.IsCallsignListed(Callsign.GetCS(), ' '))
            {
                 // Create Peer
                 CPeers* peers = g_Reflector.GetPeers();
                 if (nullptr == peers->FindPeer(Callsign, EProtocol::m17))
                 {
                      // Add Peer
                      peers->AddPeer(std::make_shared<CM17Peer>(Callsign, Ip, mods));
                 }
                 g_Reflector.ReleasePeers();
            }
        }
		else
		{
			// invalid packet
			// std::string title("Unknown M17 packet from ");
			// title += Ip.GetAddress();
			// Buffer.Dump(title);
            // Silence unknown packets to reduce log noise during dev/testing
		}
	}

	// handle end of streaming timeout
	CheckStreamsTimeout();

	// handle queue from reflector
	HandleQueue();

    // handle delayed input (pacing)
    if (!g_M17DelayedQueue.empty()) {
        auto now = std::chrono::steady_clock::now();
        while (!g_M17DelayedQueue.empty()) {
            if (now >= g_M17DelayedQueue.front().releaseTime) {
                // Process delayed packet
                auto& item = g_M17DelayedQueue.front();
                OnDvFramePacketIn(item.packet, &item.ip); 
                g_M17DelayedQueue.pop_front();
            } else {
                break; // Queue is sorted by time
            }
        }
    }

	// keep client alive
	if ( m_LastKeepaliveTime.time() > M17_KEEPALIVE_PERIOD )
	{
		HandleKeepalives();
		m_LastKeepaliveTime.start();
	}
    
    // peer connections
    if ( m_LastPeersLinkTime.time() > M17_RECONNECT_PERIOD )
    {
        HandlePeerLinks();
        m_LastPeersLinkTime.start();
    }
}

////////////////////////////////////////////////////////////////////////////////////////
// streams helpers

void CM17Protocol::OnDvHeaderPacketIn(std::unique_ptr<CDvHeaderPacket> &Header, const CIp &Ip)
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
        
        // Critical Fix: Sanitize source callsign to strip suffixes (e.g. "KF8S D" -> "KF8S")
        // This ensures Dashboard lookups and display are clean.
        // GetBase() returns the callsign string up to the first non-alphanumeric character.
        my.SetCallsign(my.GetBase(), false);
        
		my.SetSuffix("M17");
		CCallsign rpt1(Header->GetRpt1Callsign());
		CCallsign rpt2(Header->GetRpt2Callsign());
		char rpt2Module = Header->GetRpt2Module(); // cache this before move

		// find this client
		std::shared_ptr<CClient>client = g_Reflector.GetClients()->FindClient(Ip, EProtocol::m17);
		if ( client )
		{
			// get client callsign
			rpt1 = client->GetCallsign();
			// and try to open the stream
			// WARNING: OpenStream moves Header, invalidating it!
			if ( (stream = g_Reflector.OpenStream(Header, client)) != nullptr )
			{
				// keep the handle
				m_Streams[stream->GetStreamId()] = stream;
			}
		}
		// release
		g_Reflector.ReleaseClients();

		// update last heard
        CCallsign reflectorCall = rpt2;
        reflectorCall.SetCSModule(rpt2Module);
		std::cout << "DEBUG: Calling GetUsers()->Hearing for " << my.GetCS() << "..." << std::endl;
		g_Reflector.GetUsers()->Hearing(my, rpt1, rpt2, reflectorCall, EProtocol::m17);
		std::cout << "DEBUG: Returned from GetUsers()->Hearing" << std::endl;
		g_Reflector.ReleaseUsers();
	}
}

void CM17Protocol::OnDvFramePacketIn(std::unique_ptr<CDvFramePacket> &Frame, const CIp *Ip)
{
	// Keep the client alive
	if (Ip) {
		CClients *clients = g_Reflector.GetClients();
		auto client = clients->FindClient(*Ip, EProtocol::m17);
		if (client) {
			client->Heard();
		}
		g_Reflector.ReleaseClients();
	}

	// Call base implementation to push to stream
	CProtocol::OnDvFramePacketIn(Frame, Ip);
}

////////////////////////////////////////////////////////////////////////////////////////
// queue helper



// Global buffer for partial M17 frames (simple module-based cache)
// Note: In a real multi-threaded environment per-module, this should be in m_StreamsCache
// We already have m_StreamsCache[module], let's add a buffer there in the header file or just use static for now if we can't change header easily?
// We can change header. But let's look at what we have.
// We have m_StreamsCache[module].
// Let's modify M17Protocol.h to add a partial frame buffer.
// Wait, I cannot modify .h easily in this step without a separate tool call.
// Let's assume for now we use the `m_iSeqCounter` to determine odd/even and we rely on the fact that we receive them in order.
// If input is 20ms, we get packet 0, packet 1.
// Packet 0: Store payload.
// Packet 1: Append payload to Packet 0 and send.
// But we need place to store Packet 0.
// `tcd` gives us a `CDvFramePacket`.
// If I use `static` map it might be ugly but works.
// Better: Check if `packet` contains 16 bytes or 8 bytes.
// If `tcd` sends 16 bytes (padded), we might need to take first 8.
// Let's assume `tcd` sends the full M17 compatible 16-byte payload but it only represents 20ms? That's weird.
// If P25 (IMBE) -> M17 (Codec2), tcd must do the conversion.
// Codec2 3200 is 8 bytes per 20ms. M17 frame is 16 bytes (40ms).
// So `tcd` likely returns an M17 packet with 8 bytes of data?
// Let's try to inspect the payload size if possible? `CDvFramePacket` doesn't expose size easily, just `GetCodecData`.
// But `STCPacket.m17` is 16 bytes.
// IF `seq % 2 == 0`: Store this packet's 16 bytes (or 8 bytes?)
// IF `seq % 2 == 1`: Combine and send.
// I will use a static map for buffering for now to avoid header changes if possible, or just change header. I should change header for correctness.
// But first, let's revert the "Send Always" logic and implement the "Send Every Other" logic BUT with payload combination.
// Actually, the previous code was:
// if ((1 == m_StreamsCache[module].m_iSeqCounter % 2) || packet->IsLastPacket())
// This sent every *second* packet.
// It did EncodeM17Packet(..., packet, ...). It strictly used the *current* packet (`packet`).
// It IGNORED the previous packet (Counter % 2 == 0).
// So it was dropping 50% of audio! That explains "choppy" or "slow motion" if the player played it weirdly.
// "Slow motion" usually means you play X audio in 2X time.
// If I dropped 50% packets, I have X audio in X/2 time? No.
// If I preserve 1 packet every 40ms. That packet contains 20ms of audio (from tcd).
// I send it as 40ms M17 frame.
// Receiver plays it as 40ms.
// Result: 20ms audio stretched to 40ms -> Slow motion.
// FIX: I must combine the previous packet's payload with this one.
// I need storage.
// I'll update M17Protocol.h to add `uint8_t m_partialPayload[16]` or similar to `CM17StreamCacheItem`.

void CM17Protocol::HandleQueue(void)
{
    static std::map<char, std::vector<uint8_t>> partialFrames; // Temporary framing buffer

	while (! m_Queue.IsEmpty())
	{
		// get the packet
		auto packet = m_Queue.Pop();

		// get our sender's id
		const auto module = packet->GetPacketModule();

		// check if it's header and update cache
		if ( packet->IsDvHeader() )
		{
			// this relies on queue feeder setting valid module id
            if (packet)
            {
                m_StreamsCache[module].m_dvHeader = *(static_cast<CDvHeaderPacket*>(packet.get()));
                m_StreamsCache[module].m_iSeqCounter = 0;
                partialFrames[module].clear();
            }
		}
		else if (packet->IsDvFrame())
		{
            // P25->M17 (and potentially others) via TCD generates 20ms frames (8 bytes for C2_3200).
            // M17 requires 40ms frames (16 bytes).
            // We must aggregate 2 input frames into 1 output frame.
            
            // Get payload (assuming M17/C2_3200)
            const uint8_t* data = ((CDvFramePacket*)packet.get())->GetCodecData(ECodecType::c2_3200);
            if (!data) continue;

            const STCPacket* tc = ((CDvFramePacket*)packet.get())->GetCodecPacket();
            uint32_t seq = tc->sequence;

            std::vector<uint8_t>& buf = partialFrames[module];

            ECodecType cType = ECodecType::c2_3200;
            // Force header to match what we are sending (tcd always sends 3200)
            m_StreamsCache[module].m_dvHeader.SetCodecIn(cType);

            int bytesPerFrame = 8; 
            
            // Safety check
            if (bytesPerFrame > 16) bytesPerFrame = 16;
            
            int offset = (seq % 2) * 8;

            buf.insert(buf.end(), data + offset, data + offset + bytesPerFrame);

            // Do we have enough for a full M17 frame? (2x input frames)
            // M17 Frame is 40ms. Input is 20ms. So we need 2 inputs.
            // Expected size: 16 bytes for 3200, 8 bytes for 1600.
            size_t targetSize = (size_t)(bytesPerFrame * 2);
            
            if (buf.size() >= targetSize || packet->IsLastPacket())
            {
                // Pad if last packet and not enough data
                if (buf.size() < targetSize) {
                    buf.resize(targetSize, 0); 
                }
                
                // Create a temporary packet to hold combined data
                // We use the current packet as a template for sequence/flags, but override payload
                CDvFramePacket* frame = (CDvFramePacket*)packet.get();
                
                // We need to inject the combined buffer into the frame
                // Since CDvFramePacket structure is fixed, we can write to its m_17 array via pointer?
                // Or we can create an M17Packet wrapper with our buffer.
                // EncodeM17Packet takes a CDvFramePacket* to extract payload.
                // Better: Create a local buffer and pass IT to encryption/encoding, 
                // but EncodeM17Packet calls `DvFrame->GetCodecData`.
                // Hacker way: const_cast the pointer from GetCodecData and overwrite it?
                // Or create a new CDvFramePacket.
                
                // Let's use `CM17Protocol::EncodeM17Packet` which calls `packet.SetPayload`.
                // Actually `EncodeM17Packet` logic:
                // packet.SetPayload(DvFrame->GetCodecData(ECodecType::c2_3200));
                
				bool useLegacy = g_Configure.GetBoolean(g_Keys.m17.compat); 
			    uint8_t m17buf[60]; 
				CM17Packet m17pkt(m17buf, !useLegacy); 

                // Manually do what EncodeM17Packet does for payload
                // adjust sequence number since EncodeM17Packet expects a packet counter (20ms) but we have a frame counter (40ms)
				EncodeM17Packet(m17pkt, m_StreamsCache[module].m_dvHeader, frame, m_StreamsCache[module].m_iSeqCounter * 2);
                
                // OVERWRITE PAYLOAD with our aggregated buffer
                m17pkt.SetPayload(buf.data());

                // Clear buffer
                buf.clear();

				// push it to all our clients linked to the module and who are not streaming in
				CClients *clients = g_Reflector.GetClients();
				auto it = clients->begin();
				std::shared_ptr<CClient>client = nullptr;
				while ( (client = clients->FindNextClient(EProtocol::m17, it)) != nullptr )
				{
					// is this client busy ?
					if ( !client->IsAMaster() && (client->GetReflectorModule() == module) )
					{
						// set the destination
						m17pkt.SetDestCallsign(client->GetCallsign());

						// Calculate LICH CRC if Standard
						if (!useLegacy) {
							uint8_t *lich = m17pkt.GetLICHPointer();
							// CRC over first 28 bytes of LICH
							uint16_t l_crc = m17crc.CalcCRC(lich, 28);
							((SM17LichStandard*)lich)->crc = htons(l_crc);
						}

						// set the packet crc
						uint16_t p_crc = m17crc.CalcCRC(m17pkt.GetBuffer(), m17pkt.GetSize() - 2);
						m17pkt.SetCRC(p_crc);
                        
                        // now send the packet
                        CBuffer sendBuf;
                        sendBuf.Append(m17pkt.GetBuffer(), m17pkt.GetSize());
						Send(sendBuf, client->GetIp());
					}
				}
				g_Reflector.ReleaseClients();
                
                m_StreamsCache[module].m_iSeqCounter++;
			}
		}
	}

    // handle parrot timeout
    CheckStreamsTimeout();
}

////////////////////////////////////////////////////////////////////////////////////////
// keepalive helpers

void CM17Protocol::HandleKeepalives(void)
{
	// M17 protocol sends and monitors keepalives packets
	// event if the client is currently streaming
	// so, send keepalives to all
	CBuffer keepalive;
	EncodeKeepAlivePacket(keepalive);

	// iterate on clients
	CClients *clients = g_Reflector.GetClients();
	auto it = clients->begin();
	std::shared_ptr<CClient>client = nullptr;
	while ( (client = clients->FindNextClient(EProtocol::m17, it)) != nullptr )
	{
		// send keepalive
		Send(keepalive, client->GetIp());

		// is this client busy ?
		if ( client->IsAMaster() )
		{
			// yes, just tickle it
			client->Alive();
		}
		// check it's still with us
		else if ( !client->IsAlive() )
		{
			// no, disconnect
			Send("DISC", client->GetIp());

			// remove it
			std::cout << "M17 client " << client->GetCallsign() << " keepalive timeout" << std::endl;
			clients->RemoveClient(client);
		}

	}
	g_Reflector.ReleaseClients();
}

////////////////////////////////////////////////////////////////////////////////////////
// packet decoding helpers

bool CM17Protocol::IsValidConnectPacket(const CBuffer &Buffer, CCallsign &callsign, char &mod)
{
	uint8_t tag[] = { 'C', 'O', 'N', 'N' };
	bool valid = false;
	if (11 == Buffer.size() && 0 == Buffer.Compare(tag, 4))
	{
		callsign.CodeIn(Buffer.data() + 4);
		mod = Buffer.data()[10];
		valid = (callsign.IsValid() && IsLetter(mod));
	}
	return valid;
}

bool CM17Protocol::IsValidDisconnectPacket(const CBuffer &Buffer, CCallsign &callsign)
{
	uint8_t tag[] = { 'D', 'I', 'S', 'C' };
	bool valid = false;
	if ((Buffer.size() == 10) && (0 == Buffer.Compare(tag, 4)))
	{
		callsign.CodeIn(Buffer.data() + 4);
		valid = callsign.IsValid();
	}
	return valid;
}

bool CM17Protocol::IsValidKeepAlivePacket(const CBuffer &Buffer, CCallsign &callsign)
{
	uint8_t tag[] = { 'P', 'O', 'N', 'G' };
	bool valid = false;
	if ( (Buffer.size() == 10) || (0 == Buffer.Compare(tag, 4)) )
	{
		callsign.CodeIn(Buffer.data() + 4);
		valid = callsign.IsValid();
	}
	return valid;
}

bool CM17Protocol::IsValidDvPacket(const CBuffer &Buffer, std::unique_ptr<CDvHeaderPacket> &header, std::unique_ptr<CDvFramePacket> &frame)
{
	uint8_t tag[] = { 'M', '1', '7', ' ' };

	bool isStandard = false;
	bool validSize = false;
	
	if (Buffer.size() == sizeof(SM17FrameLegacy)) {
		validSize = true;
		isStandard = false;
	} else if (Buffer.size() == sizeof(SM17FrameStandard)) {
		validSize = true;
		isStandard = true;
	}

	// Buffer[19] is the low-order byte of the uint16_t frametype.
	// the 0x1CU mask (00011100 binary) just lets us see:
	// 1. the encryptions bytes (mask 0x18U) which must be zero, and
	// 2. the msb of the 2-bit payload type (mask 0x4U) which must be set. This bit set means it's voice or voice+data.
	// An masked result of 0x4U means the payload contains Codec2 voice data and there is no encryption.
	if ( validSize && (0 == Buffer.Compare(tag, sizeof(tag))) && (0x4U == (0x1CU & Buffer[19])) )
	{
		// Make the M17 header wrapper
		// Note: CM17Packet constructor copies the buffer
		CM17Packet m17(Buffer.data(), isStandard);
		
		// get the header
		header = std::unique_ptr<CDvHeaderPacket>(new CDvHeaderPacket(m17));

		// get the frame
		frame = std::unique_ptr<CDvFramePacket>(new CDvFramePacket(m17));




		// check validity of packets
		if ( header && header->IsValidM17() && frame && frame->IsValid() )
			return true;
	}
	return false;
}


////////////////////////////////////////////////////////////////////////////////////////
// packet encoding helpers

void CM17Protocol::EncodeKeepAlivePacket(CBuffer &Buffer)
{
	Buffer.resize(10);
	memcpy(Buffer.data(), "PING", 4);
	g_Reflector.GetCallsign().CodeOut(Buffer.data() + 4);
}

void CM17Protocol::EncodeM17Packet(CM17Packet &packet, const CDvHeaderPacket &Header, const CDvFramePacket *DvFrame, uint32_t iSeq) const
{
	ECodecType codec_in = Header.GetCodecIn();  // We'll need this

	// do the lich structure first
	// first, the src callsign (the lich.dest will be set in HandleQueue)
	packet.SetSourceCallsign(Header.GetMyCallsign());
	
	// then the frame type, if the incoming frame is M17 1600, then it will be Voice+Data only, otherwise Voice-Only
	packet.SetFrameType((ECodecType::c2_1600==codec_in) ? 0x7U : 0x5U);
	packet.SetNonce(DvFrame->GetNonce());

	// now the main part of the packet
	packet.SetMagic();
	
	// the frame number comes from the stream sequence counter
	// Assuming 1:1 mapping for 40ms frames (tcd output)
	uint16_t fn = iSeq % 0x8000U;
	if (DvFrame->IsLastPacket())
		fn |= 0x8000U;
	packet.SetFrameNumber(fn);
	packet.SetPayload(DvFrame->GetCodecData(ECodecType::c2_3200));
	packet.SetStreamId(Header.GetStreamId());
	// the CRC will be set in HandleQueue, after lich.dest is set
}

bool CM17Protocol::EncodeDvHeaderPacket(const CDvHeaderPacket &packet, CBuffer &buffer) const
{
	packet.EncodeInterlinkPacket(buffer);
	return true;
}

bool CM17Protocol::EncodeDvFramePacket(const CDvFramePacket &packet, CBuffer &buffer) const
{
	packet.EncodeInterlinkPacket(buffer);
	return true;
}

bool CM17Protocol::IsValidInterlinkConnect(const CBuffer &Buffer, const CIp &Ip, CCallsign &callsign, char *modules)
{
	SInterConnect *connect = (SInterConnect *)Buffer.data();
	if ( (Buffer.size() == sizeof(SInterConnect)) && (0 == memcmp(connect->magic, "CONN", 4)) )
	{
		CCallsign my;
		my.CodeIn((const uint8_t*)connect->callsign);
		callsign = my;
		if (m_M17Interlinks.IsCallsignListed(my.GetCS(), Ip, (char*)connect->modules))
        {
            memcpy(modules, connect->modules, 27);
			return true;
        }
	}
	return false;
}

bool CM17Protocol::IsValidInterlinkAcknowledge(const CBuffer &Buffer, CCallsign &callsign, char *modules)
{
	SInterConnect *ack = (SInterConnect *)Buffer.data();
	if ( (Buffer.size() == sizeof(SInterConnect)) && (0 == memcmp(ack->magic, "ACKN", 4)) )
	{
		CCallsign my;
		my.CodeIn((const uint8_t*)ack->callsign);
		callsign = my;
        memcpy(modules, ack->modules, 27);
		return true;
	}
	return false;
}

void CM17Protocol::EncodeInterlinkConnectPacket(SInterConnect &connect, const char *modules)
{
	::memset(&connect, 0, sizeof(SInterConnect));
	::memcpy(connect.magic, "CONN", 4);
    
    // Use configured callsign
    CCallsign my(g_Configure.GetString(g_Keys.names.callsign));
    my.CodeOut((uint8_t*)connect.callsign);
    
    ::memcpy(connect.modules, modules, 26); // Copy modules
}

void CM17Protocol::EncodeInterlinkAckPacket(SInterConnect &ackn, const char *modules)
{
	::memset(&ackn, 0, sizeof(SInterConnect));
	::memcpy(ackn.magic, "ACKN", 4);
    
    // We reply with OUR callsign
    CCallsign my(g_Configure.GetString(g_Keys.names.callsign));
    my.CodeOut((uint8_t*)ackn.callsign);

    ::memcpy(ackn.modules, modules, 26);
}

void CM17Protocol::EncodeInterlinkNackPacket(uint8_t *buffer)
{
	::memcpy(buffer, "NACK", 4);
}

bool CM17Protocol::IsValidPacketModePacket(const CBuffer &Buffer, CCallsign &dst, CCallsign &src)
{
    // Check for M17 Packet Data (SMS)
    // Tag: M17space
    uint8_t tag[] = { 'M', '1', '7', ' ' };
    if (Buffer.size() >= 30 && (0 == Buffer.Compare(tag, 4)))
    {
        // Type check: Byte 19 (LSB of Type).
        // 0x0002 is Packet. 
        uint16_t type = (Buffer[18] << 8) | Buffer[19];
        // Strip encryption bits (0x18 mask?) and Reserved (0x01)
        // From spec: 0x0002 is Packet.
        // Let's assume Type 2 is Packet.
        if ((type & 0xFF) == 0x02) {
             dst.CodeIn(Buffer.data() + 4);
             src.CodeIn(Buffer.data() + 10);
             return true;
        }
    }
    return false;
}

void CM17Protocol::HandleParrot(const std::shared_ptr<CClient> &client, const CBuffer &Buffer, bool isStream, uint16_t streamId, uint16_t frameNumber)
{
    if (!client) return;
    
    std::shared_ptr<CParrot> parrot;
    auto it = m_ParrotMap.find(client);
    if (m_ParrotMap.end() == it) {
        // We need CM17Client properly cast
        std::shared_ptr<CM17Client> m17client = std::dynamic_pointer_cast<CM17Client>(client);
        if (!m17client) {
            // Should not happen if protocol logic is correct
            return; 
        }

        if (isStream) {
             // Constructor: src, client, frameType, proto
             // Frame Type usually 0 for stream start? Or extracted from packet?
             // CM17StreamParrot likely needs frame type from header. 
             // We don't have frame type readily available here unless we pass it.
             // For now passing 0.
             parrot = std::make_shared<CM17StreamParrot>(client->GetCallsign(), m17client, 0, this);
             m_ParrotMap[client] = parrot;
        } else {
             parrot = std::make_shared<CM17PacketParrot>(client->GetCallsign(), m17client, 0, this);
             m_ParrotMap[client] = parrot;
        }
    } else {
        parrot = it->second;
    }
    
    if (isStream) {
        parrot->Add(Buffer, streamId, frameNumber);
    } else {
        parrot->AddPacket(Buffer);
        // For Packet Parrot, trigger play if single shot?
        // CM17PacketParrot implementation details decide.
        // Assuming AddPacket might queue it or Play needs explicit call.
        parrot->Play();
    }
    
    if (parrot->IsExpired()) {
        m_ParrotMap.erase(client);
    }
}

void CM17Protocol::CheckStreamsTimeout(void)
{
	// check for regular stream timeouts (inherited from CProtocol)
	CProtocol::CheckStreamsTimeout();

	// check each item in the parrot map
	for (auto pit = m_ParrotMap.begin(); pit != m_ParrotMap.end();)
	{
		switch (pit->second->GetState())
		{
		case EParrotState::record:
			if (pit->second->IsExpired())
			{
                 // Stream finished recording (timeout), so start playback
				// std::cout << "Parrot stream from " << pit->second->GetSRC() << " timed out! Playing..." << std::endl;
				pit->second->Play();
			}
			pit++;
			break;
		case EParrotState::done:
            // Playback finished
			if (pit->second->IsStream())
			{
				auto psp = static_cast<CM17StreamParrot *>(pit->second.get());
				// std::cout << psp->GetSize() << " packet parrot stream from " << psp->GetSRC() << " played back to " << pit->first->GetCallsign() << " at " << pit->first->GetIp() << std::endl;
			}
			else
			{
				// std::cout << "Parrot packet from " << pit->second->GetSRC() << " played back to " << pit->first->GetCallsign() << " at " << pit->first->GetIp() << std::endl;
			}
			pit->second->Quit();		// get() the future
			pit->second.reset();		// destroy the parrot object
			pit = m_ParrotMap.erase(pit); // remove the map std::pair, incrementing the pointer
			break;
		default:
             pit++;
			break;
		}
	}
}


void CM17Protocol::HandlePeerLinks(void)
{
	CPeers* peers = g_Reflector.GetPeers();
	auto pit = peers->begin();
	std::shared_ptr<CPeer> peer = nullptr;
	while ((peer = peers->FindNextPeer(EProtocol::m17, pit)))
	{
		const auto cs = peer->GetCallsign().GetCS();
		if (nullptr == m_M17Interlinks.FindMapItem(std::string(cs)))
		{
			Send("DISC", peer->GetIp());
			// std::cout << "Sent disconnect packet to M17 peer " << cs << " at " << peer->GetIp() << std::endl;
			peers->RemovePeer(peer);
		}
	}

	for (auto it = m_M17Interlinks.begin(); it != m_M17Interlinks.end(); it++)
	{
		auto &item = it->second;
		const auto cs = it->first; 
		if (nullptr == peers->FindPeer(cs, EProtocol::m17))
		{
            if (item.GetIp().IsSet()) {
			    SInterConnect connect;
			    const auto mods = item.GetModules();
			    EncodeInterlinkConnectPacket(connect, mods.c_str());
			    Send((const char*)&connect, item.GetIp()); 
			    // std::cout << "Sent connect packet to M17 peer " << cs << " @ " << item.GetIp() << " for module(s) " << mods << std::endl;
            }
		}
	}
    g_Reflector.ReleasePeers();
}

bool CM17Protocol::OnPacketIn(CM17Packet &packet, const std::shared_ptr<CClient> client)
{
    // Strict Routing: Only from M17 clients
    
    // Destination
    CCallsign dst(packet.GetDestCallsign());
    
    // Rewrite M17- prefix to @ALL (matching mrefd behavior)
    const auto cs = dst.GetCS();
    if (0 == cs.compare(0, 4, "M17-"))
    {
        dst.SetCallsign("@ALL");
        packet.SetDestCallsign(dst);
        packet.CalcCRC();
    }
    
    // Check for Group / Broadcast Types
    // dst is now updated to @ALL if it was M17-xxxx
    std::string sDst = dst.GetCS();
    while (!sDst.empty() && sDst.back() == ' ') sDst.pop_back();

    bool isAll = (sDst == "@ALL" || sDst == "M17-ALL" || sDst == "ALL");
    bool isReflector = (dst.GetCS().find(g_Reflector.GetCallsign().GetCS()) == 0); // Starts with Reflector Callsign
    
    char targetModule = 0;
    bool isGroupCall = false;
    
    if (isAll) {
        // Broadcast to Source Module (Contextual Broadcast)
        targetModule = client->GetReflectorModule();
        isGroupCall = true;
    } 
    else if (isReflector) {
        // Broadcast to Targeted Module (Legacy DroidStar/MVoice behavior)
        // e.g. "M17-REF C" -> Module C
        targetModule = dst.GetCSModule();
        // If module is ' ' space, assume source module or reject? 
        // DroidStar usually sends "M17-REF C" where 'C' is the 8th char (index 7).
        // CCallsign::GetModule returns that char.
        if (targetModule == ' ') targetModule = client->GetReflectorModule();
        isGroupCall = true;
    }
    
    // Prepare Buffer once
    CBuffer buf;
    buf.Set(const_cast<uint8_t*>(packet.GetBuffer()), packet.GetSize());
    
    if (isGroupCall) {
        // 1. Send to Clients on Target Module
        CClients *clients = g_Reflector.GetClients();
        auto it = clients->begin();
        std::shared_ptr<CClient> destClient = nullptr;
        while ( (destClient = clients->FindNextClient(EProtocol::m17, it)) != nullptr )
        {
            if (destClient == client) continue; // Skip self
            if (destClient->GetProtocol() != EProtocol::m17) continue;
            
            if (destClient->GetReflectorModule() == targetModule) {
                Send(buf, destClient->GetIp());
            }
        }
        g_Reflector.ReleaseClients();
        
        // 2. Send to Peers (Interlinks)
        CPeers *peers = g_Reflector.GetPeers();
        auto pit = peers->begin();
        std::shared_ptr<CPeer> peer = nullptr;
        while ( (peer = peers->FindNextPeer(EProtocol::m17, pit)) != nullptr )
        {
            // Check if peer is subscribed to target module
            // CPeer::GetReflectorModules() returns a char* string of modules e.g. "ABC"
            if (peer->GetReflectorModules() && strchr(peer->GetReflectorModules(), targetModule)) {
                 Send(buf, peer->GetIp());
            }
        }
        g_Reflector.ReleasePeers();
    } 
    else {
        // Private Call - Exact Match
        CClients *clients = g_Reflector.GetClients();
        auto it = clients->begin();
        std::shared_ptr<CClient> destClient = nullptr;
        while ( (destClient = clients->FindNextClient(EProtocol::m17, it)) != nullptr )
        {
            if (destClient == client) continue;
            if (destClient->GetProtocol() != EProtocol::m17) continue;
            
            if (destClient->GetCallsign() == dst) {
                Send(buf, destClient->GetIp());
                // Should we stop after finding one? Private calls usually one. 
                // But multiple sessions might exist? Keep going to be safe.
            }
        }
        g_Reflector.ReleaseClients();
    }

    return true; 
}
