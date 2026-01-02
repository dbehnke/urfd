/*
 *   Copyright (c) 2024 by Thomas A. Early N7TAE
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 */

#pragma once

#include <vector>
#include <string>
#include <map>
#include <mutex>
#include <ctime>
#include <sstream>

#include "Timer.h"

// Structure to hold subscription details
struct SSubscription {
	unsigned int tgid;
	unsigned int timeout; // seconds, 0 = infinite
	std::time_t expiry;   // absolute time
};

class CDMRScanner
{
public:
	CDMRScanner();
	virtual ~CDMRScanner();

	// Configuration
	void Configure(bool singleMode, unsigned int defaultTimeout, unsigned int holdTime);

	// Subscription Management
	void UpdateSubscriptions(const std::string& options);
	void AddSubscription(unsigned int tgid, int timeslot, unsigned int timeout);
	void RemoveSubscription(unsigned int tgid, int timeslot);
	void ClearSubscriptions();
	bool IsSubscribed(unsigned int tgid) const;

	// Packet Access Check (Scanner Logic)
	// Returns true if packet with this TG should be processed
	bool CheckAccess(unsigned int tgid);

	// Getters
	unsigned int GetFirstSubscription() const;
	unsigned int GetCurrentScanTG() const { return m_CurrentScanTG; }

private:
	mutable std::recursive_mutex m_Mutex;

	// Config
	bool m_SingleMode;
	unsigned int m_DefaultTimeout;
	unsigned int m_HoldTime;

	// State
	std::map<int, std::vector<SSubscription>> m_Subscriptions; // Map Timeslot -> List of Subscriptions
	unsigned int m_CurrentScanTG;
	CTimer m_HoldTimer;

	// Helpers
	void cleanupExpired();
    void parseOptions(const std::string& options);
};
