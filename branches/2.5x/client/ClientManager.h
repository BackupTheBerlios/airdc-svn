/*
 * Copyright (C) 2001-2013 Jacek Sieka, arnetheduck on gmail point com
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

#ifndef DCPLUSPLUS_DCPP_CLIENT_MANAGER_H
#define DCPLUSPLUS_DCPP_CLIENT_MANAGER_H

#include "TimerManager.h"
#include "ClientManagerListener.h"

#include "CID.h"
#include "Client.h"
#include "HintedUser.h"
#include "OnlineUser.h"
#include "Search.h"
#include "SettingsManager.h"
#include "Singleton.h"
#include "Socket.h"

namespace dcpp {

class UserCommand;

class ClientManager : public Speaker<ClientManagerListener>, 
	private ClientListener, public Singleton<ClientManager>, 
	private TimerManagerListener
{
	typedef unordered_map<CID*, UserPtr> UserMap;
	typedef UserMap::iterator UserIter;

public:
	Client* createClient(const string& aHubURL);
	Client* getClient(const string& aHubURL);
	void putClient(Client* aClient);
	void setClientUrl(const string& aOldUrl, const string& aNewUrl);

	size_t getUserCount() const;
	int64_t getAvailable() const;

	string getField(const CID& cid, const string& hintUrl, const char* field) const;

	OrderedStringSet getHubSet(const CID& cid) const;
	StringList getHubUrls(const CID& cid) const;
	StringList getHubNames(const CID& cid) const;
	StringList getNicks(const CID& cid, bool allowCID = true) const;
	pair<int64_t, int> getShareInfo(const HintedUser& user) const;
	void getUserInfoList(const UserPtr& user, User::UserInfoList& aList_) const;

	StringList getNicks(const HintedUser& user) { return getNicks(user.user->getCID()); }
	StringList getHubNames(const HintedUser& user) { return getHubNames(user.user->getCID()); }
	StringList getHubUrls(const HintedUser& user) { return getHubUrls(user.user->getCID()); }

	StringPair ClientManager::getNickHubPair(const UserPtr& user, string& hint);

	template<class NameOperator>
	string formatUserList(const HintedUser& user, bool removeDuplicates) const {
		OnlineUserList ouList;

		RLock l(cs);
		auto hinted = getUsers(user, ouList);

		if (removeDuplicates) {
			ouList.erase(unique(ouList.begin(), ouList.end(), [](const OnlineUserPtr& a, const OnlineUserPtr& b) { return compare(NameOperator()(a), NameOperator()(b)) == 0; }), ouList.end());
			if (hinted) {
				//erase users with the hinted nick
				auto p = equal_range(ouList.begin(), ouList.end(), hinted, OnlineUser::NickSort());
				ouList.erase(p.first, p.second);
			}
		}

		string ret = hinted ? NameOperator()(hinted) + " " : Util::emptyString;
		if (!ouList.empty())
			ret += Util::listToStringT<OnlineUserList, NameOperator>(ouList, hinted ? true : false, hinted ? false : true);
		return ret;
	}

	string getFormatedNicks(const HintedUser& user) const;
	string getFormatedHubNames(const HintedUser& user) const;

	StringPairList getHubs(const CID& cid);

	map<string, Identity> getIdentities(const UserPtr &u) const;
	
	string getNick(const UserPtr& u, const string& hintUrl, bool allowFallback = true) const;
	StringPairList getNickHubPair(const CID& cid, string& hint) const;

	string getDLSpeed(const CID& cid) const;
	uint8_t getSlots(const CID& cid) const;

	bool isConnected(const string& aUrl) const;
	
	uint64_t search(string& who, SearchPtr aSearch);

	void directSearch(const HintedUser& user, int aSizeMode, int64_t aSize, int aFileType, const string& aString, const string& aToken, const StringList& aExtList, const string& aDir);
	
	void cancelSearch(void* aOwner);
		
	void infoUpdated();

	UserPtr getUser(const string& aNick, const string& aHubUrl) noexcept;
	UserPtr getUser(const CID& cid) noexcept;

	// usage needs to be locked!
	const UserMap& getUsers() const { return users; }

	string findHub(const string& ipPort, bool nmdc) const;
	const string& findHubEncoding(const string& aUrl) const;

	/**
	* @param priv discard any user that doesn't match the hint.
	* @return OnlineUser* found by CID and hint; might be only by CID if priv is false.
	*/
	OnlineUser* findOnlineUser(const HintedUser& user) const;
	OnlineUser* findOnlineUser(const CID& cid, const string& hintUrl) const;

	UserPtr findUser(const string& aNick, const string& aHubUrl) const noexcept { return findUser(makeCid(aNick, aHubUrl)); }
	UserPtr findUser(const CID& cid) const noexcept;
	HintedUser findLegacyUser(const string& nick) const noexcept;

	UserPtr findUserByNick(const string& aNick, const string& aHubUrl) const noexcept;
	
	void updateNick(const UserPtr& user, const string& nick) noexcept;
	string getMyNick(const string& hubUrl) const;
	
	void setIPUser(const UserPtr& user, const string& IP, const string& udpPort = Util::emptyString);
	
	optional<ProfileToken> findProfile(UserConnection& p, const string& userSID);
	void listProfiles(const UserPtr& aUser, ProfileTokenSet& profiles);

	string findMySID(const UserPtr& aUser, string& aHubUrl, bool allowFallback);

	bool isOp(const UserPtr& aUser, const string& aHubUrl) const;
	bool isStealth(const string& aHubUrl) const;

	/** Constructs a synthetic, hopefully unique CID */
	CID makeCid(const string& nick, const string& hubUrl) const noexcept;

	void putOnline(OnlineUser* ou) noexcept;
	void putOffline(OnlineUser* ou, bool disconnect = false) noexcept;

	UserPtr& getMe();
	string getClientStats();
	
	bool sendUDP(AdcCommand& c, const CID& to, bool noCID=false, bool noPassive=false, const string& encryptionKey=Util::emptyString, const string& aHubUrl=Util::emptyString);

	bool connect(const UserPtr& aUser, const string& aToken, bool allowUrlChange, string& lastError_, string& hubHint_, bool& isProtocolError);
	bool privateMessage(const HintedUser& user, const string& msg, string& error_, bool thirdPerson);
	void userCommand(const HintedUser& user, const UserCommand& uc, ParamMap& params, bool compatibility);

	bool isActive() const;
	bool isActive(const string& aHubUrl) const;

	void lockRead() noexcept { cs.lock_shared(); }
	void unlockRead() noexcept { cs.unlock_shared(); }

	const Client::List& getClients() const { return clients; }
	void getOnlineClients(StringList& onlineClients);

	CID getMyCID();
	const CID& getMyPID();

	void resetProfiles(const ProfileTokenList& aProfiles, ShareProfilePtr aDefaultProfile);

	bool connectADCSearchResult(const CID& aCID, string& token_, string& hubUrl_, string& connection_, uint8_t& slots_);
	bool connectNMDCSearchResult(const string& userIP, const string& hubIpPort, HintedUser& user, string& nick, string& connection_, string& file, string& hubName);
private:

	typedef unordered_map<CID*, std::string> NickMap;

	typedef unordered_multimap<CID*, OnlineUser*> OnlineMap;
	typedef OnlineMap::iterator OnlineIter;
	typedef OnlineMap::const_iterator OnlineIterC;
	typedef pair<OnlineIter, OnlineIter> OnlinePair;
	typedef pair<OnlineIterC, OnlineIterC> OnlinePairC;
	
	Client::List clients;
	mutable SharedMutex cs;
	
	UserMap users;
	OnlineMap onlineUsers;	
	NickMap nicks;

	UserPtr me;

	Socket udp;
	
	CID pid;	

	friend class Singleton<ClientManager>;

	ClientManager();

	virtual ~ClientManager();

	void updateUser(const OnlineUser& user) noexcept;

	OnlineUserPtr getUsers(const HintedUser& aUser, OnlineUserList& users) const;
		
	/// @return OnlineUser* found by CID and hint; discard any user that doesn't match the hint.
	OnlineUser* findOnlineUserHint(const CID& cid, const string& hintUrl) const {
		OnlinePairC p;
		return findOnlineUserHint(cid, hintUrl, p);
	}
	/**
	* @param p OnlinePair of all the users found by CID, even those who don't match the hint.
	* @return OnlineUser* found by CID and hint; discard any user that doesn't match the hint.
	*/
	OnlineUser* findOnlineUserHint(const CID& cid, const string& hintUrl, OnlinePairC& p) const;

	void onSearch(const Client* c, const AdcCommand& adc, OnlineUser& from);

	// ClientListener
	void on(Connected, const Client* c) noexcept;
	void on(UserUpdated, const Client*, const OnlineUserPtr& user) noexcept;
	void on(UsersUpdated, const Client* c, const OnlineUserList&) noexcept;
	void on(Failed, const string&, const string&) noexcept;
	void on(HubUpdated, const Client* c) noexcept;
	void on(HubUserCommand, const Client*, int, int, const string&, const string&) noexcept;
	void on(NmdcSearch, Client* aClient, const string& aSeeker, int aSearchType, int64_t aSize,
		int aFileType, const string& aString, bool) noexcept;
	// TimerManagerListener
	void on(TimerManagerListener::Minute, uint64_t aTick) noexcept;
};

} // namespace dcpp

#endif // !defined(CLIENT_MANAGER_H)