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


#include "DMRMMDVMClient.h"
#include "Global.h"
#include "Configure.h"
#include "DMRMMDVMProtocol.h" // For mapping logic if accessible, or we reimplement


////////////////////////////////////////////////////////////////////////////////////////
// constructors

CDmrmmdvmClient::CDmrmmdvmClient()
{
}

CDmrmmdvmClient::CDmrmmdvmClient(const CCallsign &callsign, const CIp &ip, char reflectorModule)
	: CClient(callsign, ip, reflectorModule)
{
}

CDmrmmdvmClient::CDmrmmdvmClient(const CDmrmmdvmClient &client)
	: CClient(client)
{
}

////////////////////////////////////////////////////////////////////////////////////////
// status

bool CDmrmmdvmClient::IsAlive(void) const
{
	return (m_LastKeepaliveTime.time() < DMRMMDVM_KEEPALIVE_TIMEOUT);
}

// Multi-Module Reporting for Dashboard
void CDmrmmdvmClient::JsonReport(nlohmann::json &report)
{
	if (g_Configure.GetBoolean(g_Keys.dmr.xlx)) {
        // Legacy behavior
        CClient::JsonReport(report);
        return;
    }

    // Mini DMR Mode
	bool anySub = false;

    // Collect Subscriptions Info
    nlohmann::json jSubs = nlohmann::json::array();
    
    std::vector<unsigned int> tgs;
    m_Scanner.GetActiveTalkgroups(tgs);
    
    for(unsigned int tg : tgs) {
        if (m_Scanner.IsSubscribed(tg, 1)) {
             nlohmann::json s; s["TG"] = tg; s["Slot"] = 1; jSubs.push_back(s);
        }
        if (m_Scanner.IsSubscribed(tg, 2)) {
             nlohmann::json s; s["TG"] = tg; s["Slot"] = 2; jSubs.push_back(s);
        }
    }

	// Helper to add node entry
	auto addNode = [&](char module) {
		nlohmann::json jclient;
		jclient["Callsign"] = m_Callsign.GetCS();
		jclient["OnModule"] = std::string(1, module);
		jclient["Protocol"] = GetProtocolName();
        jclient["Subscriptions"] = jSubs;
		char s[100];
		if (std::strftime(s, sizeof(s), "%FT%TZ", std::gmtime(&m_ConnectTime)))
			jclient["ConnectTime"] = s;
		report["Clients"].push_back(jclient);
	};

    // Reimplement logic using global config.
    auto dmrdstToMod = [&](uint32_t tg) -> char {
		for (char c = 'A'; c <= 'Z'; c++) {
			std::string key = g_Keys.dmr.map_prefix + c;
			if (g_Configure.Contains(key)) {
				if (g_Configure.GetUnsigned(key) == tg) return c;
			} else {
				if (tg == (uint32_t)(4001 + (c - 'A'))) return c;
			}
		}
        return ' ';
    };

    // Process unique modules only to avoid duplicates
    std::string addedModules = "";
    
    for(unsigned int tg : tgs) {
        char mod = dmrdstToMod(tg);
        if (mod != ' ') {
            if (addedModules.find(mod) == std::string::npos) {
                addNode(mod);
                addedModules += mod;
                anySub = true;
            }
        }
    }

	// Fallback or Legacy: Use standard module if no specific subscriptions found (or in XLX mode)
	if (!anySub) {
		CClient::JsonReport(report);
	}
}
