/*
 * Copyright (C) 2001-2011 Jacek Sieka, arnetheduck on gmail point com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef DCPLUSPLUS_DCPP_SEARCH_MANAGER_H
#define DCPLUSPLUS_DCPP_SEARCH_MANAGER_H

#include "SettingsManager.h"

#include "Socket.h"
#include "User.h"
#include "Thread.h"
#include "Client.h"
#include "Singleton.h"

#include "SearchManagerListener.h"
#include "TimerManager.h"
#include "AdcCommand.h"
#include "ClientManager.h"
#include "ResourceManager.h"

namespace dcpp {

class SocketException;

class SearchManager : public Speaker<SearchManagerListener>, public Singleton<SearchManager>, public Thread, private TimerManagerListener
{
public:
	enum SizeModes {
		SIZE_DONTCARE = 0x00,
		SIZE_ATLEAST = 0x01,
		SIZE_ATMOST = 0x02,
		SIZE_EXACT = 0x03
	};

	enum TypeModes {
		TYPE_ANY = 0,
		TYPE_AUDIO,
		TYPE_COMPRESSED,
		TYPE_DOCUMENT,
		TYPE_EXECUTABLE,
		TYPE_PICTURE,
		TYPE_VIDEO,
		TYPE_DIRECTORY,
		TYPE_TTH,
		TYPE_LAST
	};
private:
	static const char* types[TYPE_LAST];
public:
	static const char* getTypeStr(int type);
	
	void search(const string& aName, int64_t aSize, TypeModes aTypeMode, SizeModes aSizeMode, const string& aToken, Search::searchType sType, void* aOwner = NULL);
	void search(const string& aName, const string& aSize, TypeModes aTypeMode, SizeModes aSizeMode, const string& aToken, Search::searchType sType, void* aOwner = NULL) {
		search(aName, Util::toInt64(aSize), aTypeMode, aSizeMode, aToken, sType, aOwner);
	}

	uint64_t search(StringList& who, const string& aName, int64_t aSize, TypeModes aTypeMode, SizeModes aSizeMode, const string& aToken, const StringList& aExtList, Search::searchType sType, void* aOwner = NULL);
	uint64_t search(StringList& who, const string& aName, const string& aSize, TypeModes aTypeMode, SizeModes aSizeMode, const string& aToken, const StringList& aExtList, Search::searchType sType, void* aOwner = NULL) {
		return search(who, aName, Util::toInt64(aSize), aTypeMode, aSizeMode, aToken, aExtList, sType, aOwner);
 	}
	//static string clean(const string& aSearchString);
	
	void respond(const AdcCommand& cmd, const CID& cid, bool isUdpActive, const string& hubIpPort);

	const string& getPort() const { return port; }

	void listen();
	void disconnect() noexcept;
	void onSearchResult(const string& aLine) {
		onData((const uint8_t*)aLine.data(), aLine.length(), Util::emptyString);
	}

	void onRES(const AdcCommand& cmd, const UserPtr& from, const string& remoteIp = Util::emptyString);
	void onPSR(const AdcCommand& cmd, UserPtr from, const string& remoteIp = Util::emptyString);
	void onPBD(const AdcCommand& cmd, UserPtr from);
	AdcCommand toPSR(bool wantResponse, const string& myNick, const string& hubIpPort, const string& tth, const vector<uint16_t>& partialInfo) const;
	AdcCommand toPBD(const string& hubIpPort, const string& bundle, const string& aTTH, bool reply, bool add, bool notify = false) const;

private:
	enum ItemT {
		SEARCHTIME		= 0,
		LOCALTOKEN		= 1,
		HUBURL			= 2,
	};

	typedef tuple<uint64_t, string, string> SearchItem;

	boost::unordered_map<string, SearchItem> searches;
	CriticalSection cs;

	std::unique_ptr<Socket> socket;
	string port;
	bool stop;
	friend class Singleton<SearchManager>;

	SearchManager();

	static std::string normalizeWhitespace(const std::string& aString);
	int run();

	~SearchManager();
	void onData(const uint8_t* buf, size_t aLen, const string& address);

	string getPartsString(const PartsInfo& partsInfo) const;
	
	void on(TimerManagerListener::Minute, uint64_t aTick) noexcept;
};

} // namespace dcpp

#endif // !defined(SEARCH_MANAGER_H)

/**
 * @file
 * $Id: SearchManager.h 568 2011-07-24 18:28:43Z bigmuscle $
 */
