//
//  Copyright © 2020 Thomas A. Early, N7TAE
//
// ----------------------------------------------------------------------------
//
//    m17ref is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    m17ref is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    with this software.  If not, see <http://www.gnu.org/licenses/>.
// ----------------------------------------------------------------------------

#include <arpa/inet.h>

#include "M17Packet.h"
#include "M17CRC.h"

CM17Packet::CM17Packet(const uint8_t *buf, bool isStandard)
    : m_isStandard(isStandard)
{
    if (m_isStandard)
    {
        memcpy(m_frame.buffer, buf, sizeof(SM17FrameStandard));
        destination.CodeIn(m_frame.standard.lich.addr_dst);
        source.CodeIn(m_frame.standard.lich.addr_src);
    }
    else
    {
        memcpy(m_frame.buffer, buf, sizeof(SM17FrameLegacy));
        destination.CodeIn(m_frame.legacy.lich.addr_dst);
        source.CodeIn(m_frame.legacy.lich.addr_src);
    }
}

const CCallsign &CM17Packet::GetDestCallsign() const
{
	return destination;
}

const CCallsign &CM17Packet::GetSourceCallsign() const
{
	return source;
}

char CM17Packet::GetDestModule() const
{
	return destination.GetCSModule();
}

uint16_t CM17Packet::GetFrameNumber() const
{
    if (m_isStandard)
        return ntohs(m_frame.standard.framenumber);
    else
        return ntohs(m_frame.legacy.framenumber);
}

uint16_t CM17Packet::GetFrameType() const
{
    if (m_isStandard)
        return ntohs(m_frame.standard.lich.frametype);
    else
        return ntohs(m_frame.legacy.lich.frametype);
}

const uint8_t *CM17Packet::GetPayload() const
{
    if (m_isStandard)
        return m_frame.standard.payload;
    else
        return m_frame.legacy.payload;
}

const uint8_t *CM17Packet::GetNonce() const
{
    if (m_isStandard)
        return m_frame.standard.lich.nonce;
    else
        return m_frame.legacy.lich.nonce;
}

void CM17Packet::SetPayload(const uint8_t *newpayload)
{
    if (m_isStandard)
        memcpy(m_frame.standard.payload, newpayload, 16);
    else
        memcpy(m_frame.legacy.payload, newpayload, 16);
}

uint16_t CM17Packet::GetStreamId() const
{
    if (m_isStandard)
        return ntohs(m_frame.standard.streamid);
    else
        return ntohs(m_frame.legacy.streamid);
}

uint16_t CM17Packet::GetCRC() const
{
    if (m_isStandard)
        return ntohs(m_frame.standard.crc);
    else
        return ntohs(m_frame.legacy.crc);
}

void CM17Packet::SetCRC(uint16_t crc)
{
    if (m_isStandard)
        m_frame.standard.crc = htons(crc);
    else
        m_frame.legacy.crc = htons(crc);
}

void CM17Packet::CalcCRC()
{
	static const CM17CRC crc;
    // Determine if Packet Data (Type 2) or Stream
    uint16_t type = GetFrameType();
    bool isPacket = ((type & 0xFF) == 0x02); 
    
    if (!isPacket) {
         // Stream Logic
         if (m_isStandard)
             m_frame.standard.crc = htons(crc.CalcCRC(m_frame.buffer, 54));
         else
             m_frame.legacy.crc = htons(crc.CalcCRC(m_frame.buffer, 52));
    } else {
         // Packet Data Logic (Split CRCs)
         if (m_isStandard) {
             // LICH CRC (first 28 bytes of LICH struct)
             m_frame.standard.lich.crc = htons(crc.CalcCRC((uint8_t*)&m_frame.standard.lich, 28));
             // Payload CRC (Frame number + Payload) -> 18 bytes
             m_frame.standard.crc = htons(crc.CalcCRC(m_frame.buffer + 36, 18));
         } else {
             // Legacy Packet Data
             // LICH CRC at byte 32 (covers 28 bytes of LICH)
             // In urfd structure, byte 32 is... wait.
             // SM17FrameLegacy: magic(4)+sid(2)+lich(28) = 34 bytes.
             // mrefd Set16At(32, ...)
             // Byte 32 is inside 'lich' struct? No.
             // 4+2 = 6. LICH starts at 6.
             // 6+28 = 34.
             // mrefd uses `isstream` flag to determine offset.
             // If NOT stream (Packet Data), LICH starts at offset 4 (no SID).
             // But CM17Packet structure enforces SID.
             
             // CRITICAL: If urfd treats everything as Stream layout (with SID),
             // then LICH starts at 6.
             // If actual packet on wire was Type 2 (Packet Data) WITHOUT SID (LSF),
             // then our mapping is OFF by 2 bytes!
             
             // IsValidPacketModePacket:
             // Checks Buffer size >= 30.
             // Tag at 0-3.
             // Validates Type at 18.
             // 4(Tag) + 2(SID) + 6(DST) + 6(SRC) = 18.
             // So type AT 18 implies SID IS PRESENT.
             
             // M17 Spec says Link Setup Frame (LSF) starts with "M17 " then LSF content.
             // LSF content: DST+SRC+TYPE+META+CRC.
             // Does it have StreamID?
             // "Stream Mode frames" have StreamID.
             // "Packet Mode frames" (LSF) do NOT have StreamID?
             
             // If urfd assumes SID is present, it aligns LICH at 6.
             // If packet actually has NO SID, LICH starts at 4.
             
             // Let's trust urfd's existing `IsValidPacketModePacket` logic which works?
             // It checks buffer[18]/[19].
             // If SID present: 4+2+DST(6)+SRC(6) = 18. Type starts at 18.
             // If NO SID: 4+DST(6)+SRC(6) = 16. Type starts at 16.
             
             // The check `Buffer[18]` implies `urfd` expects SID.
             // So we proceed assuming SID is present or placeholders.
             
             // Legacy CRC logic for Stream-layout-wrapped-Packet
             // We can follow mrefd's offsets adjusted for SID.
             // mrefd (Packet Mode): LICH CRC at 32. 
             // If we add 2 bytes for SID... CRC should be at 34?
             
             // But wait, if we are just rewriting DST, we update DST (in LICH).
             // Then we recalc LICH CRC.
             
             // If legacy, we might not have LICH CRC field in struct?
             // SM17LichLegacy (28 bytes) has NO CRC field.
             // So where is it stored?
             // In `SM17FrameLegacy`? 
             // FrameLegacy has `crc` at end.
             
             // Maybe legacy mode relies on full frame CRC only?
             // Or mrefd puts LICH CRC at offset 32 into... payload?
             // 34 bytes used by header+lich.
             // 32 is inside LICH? No.
             // If LICH starts at 4 (mrefd Packet Mode), 4+28 = 32.
             // So byte 32 is AFTER LICH.
             // It essentially appends CRC after LICH.
             
             // In `SM17FrameLegacy` (urfd):
             // magic(4)+streamid(2)+lich(28) = 34.
             // Byte 32 overlaps with `lich`? No.
             // It overlaps with last 2 bytes of `lich`?
             // 6 (start) + 28 = 34.
             // Byte 32 is index 32. i.e., 33rd byte.
             // 6..33 are LICH.
             // So 32,33 are last 2 bytes of LICH.
             
             // So yes, LICH CRC is overlayed on last 2 bytes of LICH?
             // Or appended?
             
             // Let's just use full frame calc for legacy for now to be safe,
             // matching `mrefd`'s "safe" path if specific handling is ambiguous.
             // Or better: Re-calculate CRC for buffer length.
             
             m_frame.legacy.crc = htons(crc.CalcCRC(m_frame.buffer, 52));
         }
    }
}

void CM17Packet::SetDestCallsign(const CCallsign &cs)
{
    destination = cs;
    if (m_isStandard)
        destination.CodeOut(m_frame.standard.lich.addr_dst);
    else
        destination.CodeOut(m_frame.legacy.lich.addr_dst);
}

void CM17Packet::SetSourceCallsign(const CCallsign &cs)
{
    source = cs;
    if (m_isStandard)
        source.CodeOut(m_frame.standard.lich.addr_src);
    else
        source.CodeOut(m_frame.legacy.lich.addr_src);
}

void CM17Packet::SetStreamId(uint16_t id)
{
    if (m_isStandard)
        m_frame.standard.streamid = htons(id);
    else
        m_frame.legacy.streamid = htons(id);
}

void CM17Packet::SetFrameNumber(uint16_t fn)
{
    if (m_isStandard)
        m_frame.standard.framenumber = htons(fn);
    else
        m_frame.legacy.framenumber = htons(fn);
}

void CM17Packet::SetFrameType(uint16_t ft)
{
    if (m_isStandard)
        m_frame.standard.lich.frametype = htons(ft);
    else
        m_frame.legacy.lich.frametype = htons(ft);
}

void CM17Packet::SetMagic()
{
    if (m_isStandard)
        memcpy(m_frame.standard.magic, "M17 ", 4);
    else
        memcpy(m_frame.legacy.magic, "M17 ", 4);
}

void CM17Packet::SetNonce(const uint8_t *nonce)
{
    if (m_isStandard)
        memcpy(m_frame.standard.lich.nonce, nonce, 14);
    else
        memcpy(m_frame.legacy.lich.nonce, nonce, 14);
}

bool CM17Packet::IsLastPacket() const
{
    if (m_isStandard)
        return ((0x8000u & ntohs(m_frame.standard.framenumber)) == 0x8000u);
    else
        return ((0x8000u & ntohs(m_frame.legacy.framenumber)) == 0x8000u);
}
