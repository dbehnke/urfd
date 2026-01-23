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


#include "PacketStream.h"
#include <iostream>

////////////////////////////////////////////////////////////////////////////////////////
// constructor

CPacketStream::CPacketStream(char module) : m_PSModule(module)
{
	m_bOpen = false;
	m_uiStreamId = 0;
	m_uiPacketCntr = 0;
	m_OwnerClient = nullptr;
	m_CodecStream = nullptr;
}

bool CPacketStream::InitCodecStream()
{
	m_CodecStream = std::unique_ptr<CCodecStream>(new CCodecStream(this, m_PSModule));
	if (m_CodecStream)
		return m_CodecStream->InitCodecStream();
	else
	{
		std::cerr << "Could not create a CCodecStream for module '" << m_PSModule << "'" << std::endl;
		return true;
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// open / close

bool CPacketStream::OpenPacketStream(const CDvHeaderPacket &DvHeader, std::shared_ptr<CClient>client)
{
	// not already open?
	if ( !m_bOpen )
	{
		std::cout << "PacketStream[" << m_PSModule << "]: OpenPacketStream - Opening stream (m_bOpen was false)" << std::endl;
		// update status
		m_bOpen = true;
		m_uiStreamId = DvHeader.GetStreamId();
		m_uiPacketCntr = 0;
		m_DvHeader = DvHeader;
		m_OwnerClient = client;
		m_LastPacketTime.start();
		if (m_CodecStream)
		{
			std::cout << "PacketStream[" << m_PSModule << "]: Calling ResetStats to start recording" << std::endl;
			m_CodecStream->ResetStats(m_uiStreamId, m_DvHeader.GetCodecIn());
		}
		return true;
	}
	std::cout << "PacketStream[" << m_PSModule << "]: OpenPacketStream - FAILED! Stream already open (m_bOpen was true)" << std::endl;
	return false;
}

void CPacketStream::ClosePacketStream(void)
{
	std::cout << "PacketStream[" << m_PSModule << "]: ClosePacketStream - Closing stream (setting m_bOpen = false)" << std::endl;
	// update status
	m_bOpen = false;
	m_uiStreamId = 0;
	m_OwnerClient.reset();
	if (m_CodecStream)
		m_CodecStream->ReportStats();
}

////////////////////////////////////////////////////////////////////////////////////////
// push & pop

void CPacketStream::Push(std::unique_ptr<CPacket> Packet)
{
	// update stream dependent packet data
	m_LastPacketTime.start();
	if (Packet->IsDvFrame())
	{
		Packet->UpdatePids(m_uiPacketCntr++);
	}
	
	// DIAGNOSTIC: Log packet push details
	bool isDvFrame = Packet->IsDvFrame();
	bool isLocalOrigin = Packet->IsLocalOrigin();
	bool isLastPacket = Packet->IsLastPacket();
	bool hasCodecStream = (m_CodecStream != nullptr);
	
	if (isLastPacket) {
		std::cout << "PacketStream[" << m_PSModule << "]::Push - FINAL PACKET: "
		          << "IsDvFrame=" << (isDvFrame ? "YES" : "no")
		          << ", IsLocalOrigin=" << (isLocalOrigin ? "YES" : "no")
		          << ", HasCodecStream=" << (hasCodecStream ? "YES" : "no")
		          << ", WillTranscode=" << (hasCodecStream && isDvFrame && isLocalOrigin ? "YES" : "NO")
		          << std::endl;
	}
	
	// ... Is there a CodecStream (is this module transcoded)?
	// AND Is this voice data?
	// AND Is this from a local client and not from an interlinked URF
	if ( m_CodecStream && Packet->IsDvFrame() && Packet->IsLocalOrigin())
	{
		// yes, push packet to trancoder queue
		// first, recast to a CDvFramePacket
		auto Frame = std::unique_ptr<CDvFramePacket>(static_cast<CDvFramePacket *>(Packet.release()));
		// trancoder will push it to m_Queue after transcoding
		// is completed
		if (isLastPacket) {
			std::cout << "PacketStream[" << m_PSModule << "]::Push - Sending final packet to TRANSCODER" << std::endl;
		}
		m_CodecStream->Push(std::move(Frame));
	}
	else
	{
		// no, just bypass transcoder
		if (isLastPacket) {
			std::cout << "PacketStream[" << m_PSModule << "]::Push - Sending final packet DIRECTLY to queue (bypassing transcoder)" << std::endl;
		}
		m_Queue.Push(std::move(Packet));
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// get

const CIp *CPacketStream::GetOwnerIp(void)
{
	if ( m_OwnerClient != nullptr )
	{
		return &(m_OwnerClient->GetIp());
	}
	return nullptr;
}

std::string CPacketStream::StopRecording()
{
	if (m_CodecStream)
		return m_CodecStream->StopRecording();
	return "";
}
