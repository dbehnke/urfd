/*
 * M17Peer.h
 */

#pragma once

#include "Peer.h"

class CM17Peer : public CPeer
{
public:
	CM17Peer(const CCallsign &callsign, const CIp &ip, const char *modules) : CPeer(callsign, ip, modules, CVersion()) {}
	virtual ~CM17Peer() {}

	virtual EProtocol GetProtocol(void) const override { return EProtocol::m17; }
	virtual const char *GetProtocolName(void) const override { return "M17"; }
};
