#pragma once

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

#include "Client.h"

////////////////////////////////////////////////////////////////////////////////////////
// define


////////////////////////////////////////////////////////////////////////////////////////
// class

class CM17Client : public CClient
{
public:
	// constructors
	CM17Client();
	CM17Client(const CCallsign &, const CIp &, char);
	CM17Client(const CM17Client &);

	// destructor
	virtual ~CM17Client() {};

	// identity
	const char *GetProtocolName(void) const     { return "M17"; }
	bool IsNode(void) const                     { return true; }

	// status
	bool IsAlive(void) const;
};

////////////////////////////////////////////////////////////////////////////////////////