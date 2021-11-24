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

#include "Main.h"
#include <string.h>
#include "M17Client.h"
#include "M17Protocol.h"
#include "M17Packet.h"
#include "Reflector.h"
#include "GateKeeper.h"

////////////////////////////////////////////////////////////////////////////////////////
// operation

bool CM17Protocol::Initialize(const char *type, const EProtocol ptype, const uint16_t port, const bool has_ipv4, const bool has_ipv6)
{
	// base class
	if (! CProtocol::Initialize(type, ptype, port, has_ipv4, has_ipv6))
		return false;

	// update time
	m_LastKeepaliveTime.start();

	// done
	return true;
}



////////////////////////////////////////////////////////////////////////////////////////
// task

void CM17Protocol::Task(void)
{
	CBuffer   Buffer;
	CIp       Ip;
	CCallsign Callsign;
	char      ToLinkModule;
	std::unique_ptr<CDvHeaderPacket> Header;
	std::unique_ptr<CDvFramePacket>  Frame;

	// handle incoming packets
#if DSTAR_IPV6==true
#if DSTAR_IPV4==true
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
			// callsign muted?
			if ( g_GateKeeper.MayTransmit(Header->GetMyCallsign(), Ip, EProtocol::m17, Header->GetRpt2Module()) )
			{
				OnDvHeaderPacketIn(Header, Ip);

				OnDvFramePacketIn(Frame, &Ip);
				OnDvFramePacketIn(Frame, &Ip); // push two packet because we need a packet every 20 ms
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
					Buffer.Set("ACKN");
					Send(Buffer, Ip);

					// create the client and append
					g_Reflector.GetClients()->AddClient(std::make_shared<CM17Client>(Callsign, Ip, ToLinkModule));
					g_Reflector.ReleaseClients();
				}
				else
				{
					std::cout << "M17 node " << Callsign << " connect attempt on non-existing module" << std::endl;

					// deny the request
					Buffer.Set("NACK");
					Send(Buffer, Ip);
				}
			}
			else
			{
				// deny the request
				Buffer.Set("NACK");
				Send(Buffer, Ip);
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
				Buffer.Set("DISC");
				Send(Buffer, Ip);
			}
			g_Reflector.ReleaseClients();
		}
		else if ( IsValidKeepAlivePacket(Buffer, Callsign) )
		{
			//std::cout << "M17 keepalive packet from " << Callsign << " at " << Ip << std::endl;

			// find all clients with that callsign & ip and keep them alive
			CClients *clients = g_Reflector.GetClients();
			auto it = clients->begin();
			std::shared_ptr<CClient>client = nullptr;
			while ( (client = clients->FindNextClient(Callsign, Ip, EProtocol::m17, it)) != nullptr )
			{
				client->Alive();
			}
			g_Reflector.ReleaseClients();
		}
		else
		{
			// invalid packet
			std::string title("Unknown M17 packet from ");
			title += Ip.GetAddress();
			Buffer.Dump(title);
		}
	}

	// handle end of streaming timeout
	CheckStreamsTimeout();

	// handle queue from reflector
	HandleQueue();

	// keep client alive
	if ( m_LastKeepaliveTime.time() > M17_KEEPALIVE_PERIOD )
	{
		//
		HandleKeepalives();

		// update time
		m_LastKeepaliveTime.start();
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// streams helpers

void CM17Protocol::OnDvHeaderPacketIn(std::unique_ptr<CDvHeaderPacket> &Header, const CIp &Ip)
{
	// find the stream
	auto stream = GetStream(Header->GetStreamId());
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
		std::shared_ptr<CClient>client = g_Reflector.GetClients()->FindClient(Ip, EProtocol::m17);
		if ( client )
		{
			// get client callsign
			rpt1 = client->GetCallsign();
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
		g_Reflector.GetUsers()->Hearing(my, rpt1, rpt2);
		g_Reflector.ReleaseUsers();
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// queue helper

void CM17Protocol::HandleQueue(void)
{
	m_Queue.Lock();
	while ( !m_Queue.empty() )
	{
		// get the packet
		auto packet = m_Queue.pop();

		// get our sender's id
		const auto module = packet->GetModule();

		// check if it's header and update cache
		if ( packet->IsDvHeader() )
		{
			// this relies on queue feeder setting valid module id
			// m_StreamsCache[module] will be created if it doesn't exist
			m_StreamsCache[module].m_dvHeader = CDvHeaderPacket((const CDvHeaderPacket &)*packet.get());
			m_StreamsCache[module].m_iSeqCounter = 0;
		}
		else if (packet->IsSecond() || packet->IsLastPacket())
		{
			// encode it
			SM17Frame frame;
			memcpy(frame.magic, "M17 ", 4);
			if ( packet->IsLastPacket() )
			{
				EncodeLastM17Packet(frame, m_StreamsCache[module].m_dvHeader, (const CDvFramePacket &)*packet.get(), m_StreamsCache[module].m_iSeqCounter++);
			}
			else if ( packet->IsDvFrame() )
			{
				EncodeM17Packet(frame, m_StreamsCache[module].m_dvHeader, (const CDvFramePacket &)*packet.get(), m_StreamsCache[module].m_iSeqCounter++);
			}

			// push it to all our clients linked to the module and who are not streaming in
			CClients *clients = g_Reflector.GetClients();
			auto it = clients->begin();
			std::shared_ptr<CClient>client = nullptr;
			while ( (client = clients->FindNextClient(EProtocol::m17, it)) != nullptr )
			{
				// is this client busy ?
				if ( !client->IsAMaster() && (client->GetReflectorModule() == module) )
				{
					CBuffer Buffer(frame.magic, sizeof(SM17Frame));
					// no, send the packet
					Send(Buffer, client->GetIp());

				}
			}
			g_Reflector.ReleaseClients();
		}
	}
	m_Queue.Unlock();
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
			CBuffer disconnect;
			disconnect.Set("DISC");
			Send(disconnect, client->GetIp());

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

	if ( (Buffer.size() == sizeof(SM17Frame)) && (0 == Buffer.Compare(tag, sizeof(tag))) && (0x4U == (0x1CU & Buffer[13])) )
	// Buffer[13] is the lsb byte of the frametype. 0x4 means payload contains Codec2 voice data and there is no encryption.
	// the 0x1CU mask just lets us see the encryptions bytes (must be zero) and the msb of the payload type (must be set)
	{
		// Make the M17 header
		CM17Packet m17(Buffer.data());
		// get the header
		header = std::unique_ptr<CDvHeaderPacket>(new CDvHeaderPacket(m17));

		// get the frame
		frame = std::unique_ptr<CDvFramePacket>(new CDvFramePacket(m17));

		// check validity of packets
		if ( header && header->IsValid() && frame && frame->IsValid() )
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

void CM17Protocol::EncodeM17Packet(SM17Frame &frame, const CDvHeaderPacket &Header, const CDvFramePacket &DvFrame, uint32_t iSeq) const
{
	ECodecType codec_in = Header.GetCodecIn();  // We'll need this


	// do the lich structure first
	// first, the dest and src callsigns
	Header.GetRpt2Callsign().CodeOut(frame.lich.addr_dst);
	CCallsign from = g_Reflector.GetCallsign();
	from.SetModule(Header.GetModule());
	from.CodeOut(frame.lich.addr_src);
	// then the frame type, if the incoming frame is NOT an M17 1600, then it will be Voice only
	frame.lich.frametype = htons((ECodecType::c2_1600==codec_in) ? 0x7U : 0x5U);
	memcpy(frame.lich.nonce, DvFrame.GetNonce(), 14);

	// now the main part of the packet
	memcpy(frame.magic, "M17 ", 4);
	// the frame number comes from the stream sequence counter
	frame.framenumber = htons(iSeq % 0x8000U);
	memcpy(frame.payload, DvFrame.GetCodecData(codec_in), 16);
	frame.streamid = Header.GetStreamId();	// no host<--->network byte swapping since we never do any math on this value
	// finally, calcualte the m17 CRC value and load it
	frame.crc = htons(m17crc.CalcCRC(frame.magic, sizeof(SM17Frame)-2));
}

void CM17Protocol::EncodeLastM17Packet(SM17Frame &frame, const CDvHeaderPacket &Header, const CDvFramePacket &DvFrame, uint32_t iSeq) const
{
	EncodeM17Packet(frame, Header, DvFrame, iSeq);
	frame.framenumber |= 0x8000U;
}
