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

#include "stdinc.h"

#include "AdcCommand.h"
#include "AdcHub.h"
#include "ChatMessage.h"
#include "ClientManager.h"
#include "ConnectionManager.h"
#include "ConnectivityManager.h"
#include "FavoriteManager.h"
#include "Localization.h"
#include "ShareManager.h"
#include "StringTokenizer.h"
#include "version.h"
#include "Util.h"
#include "UserCommand.h"
#include "CryptoManager.h"
#include "ResourceManager.h"
#include "LogManager.h"
#include "UploadManager.h"
#include "StringTokenizer.h"
#include "ThrottleManager.h"
#include "QueueManager.h"
#include "HashBloom.h"

namespace dcpp {

const string AdcHub::CLIENT_PROTOCOL("ADC/1.0");
const string AdcHub::SECURE_CLIENT_PROTOCOL_TEST("ADCS/0.10");
const string AdcHub::ADCS_FEATURE("ADC0");
const string AdcHub::TCP4_FEATURE("TCP4");
const string AdcHub::TCP6_FEATURE("TCP6");
const string AdcHub::UDP4_FEATURE("UDP4");
const string AdcHub::UDP6_FEATURE("UDP6");
const string AdcHub::NAT0_FEATURE("NAT0");
const string AdcHub::SEGA_FEATURE("SEGA");
const string AdcHub::BASE_SUPPORT("ADBASE");
const string AdcHub::BAS0_SUPPORT("ADBAS0");
const string AdcHub::TIGR_SUPPORT("ADTIGR");
const string AdcHub::UCM0_SUPPORT("ADUCM0");
const string AdcHub::BLO0_SUPPORT("ADBLO0");
const string AdcHub::ZLIF_SUPPORT("ADZLIF");
const string AdcHub::BNDL_FEATURE("BNDL");
const string AdcHub::DSCH_FEATURE("DSCH");
const string AdcHub::SUD1_FEATURE("SUD1");

const vector<StringList> AdcHub::searchExts;

AdcHub::AdcHub(const string& aHubURL) :
	Client(aHubURL, '\n'), oldPassword(false), udp(Socket::TYPE_UDP), sid(0) {
	TimerManager::getInstance()->addListener(this);
}

AdcHub::~AdcHub() {
	TimerManager::getInstance()->removeListener(this);
	clearUsers();
}

size_t AdcHub::getUserCount() const { 
	RLock l(cs); 
	size_t userCount = 0;
	for(auto& i: users) {
		if(!i.second->isHidden()) {
			++userCount;
		}
	}
	return userCount;
}

OnlineUser& AdcHub::getUser(const uint32_t aSID, const CID& aCID) {
	OnlineUser* ou = findUser(aSID);
	if(ou) {
		return *ou;
	}

	UserPtr p = ClientManager::getInstance()->getUser(aCID);

	{
		WLock l(cs);
		ou = users.emplace(aSID, new OnlineUser(p, *this, aSID)).first->second;
		ou->inc();
	}

	if(aSID != AdcCommand::HUB_SID)
		ClientManager::getInstance()->putOnline(ou);
	return *ou;
}

OnlineUser* AdcHub::findUser(const uint32_t aSID) const {
	RLock l(cs);
	auto i = users.find(aSID);
	return i == users.end() ? nullptr : i->second;
}

OnlineUser* AdcHub::findUser(const CID& aCID) const {
	RLock l(cs);
	for(const auto& ou: users | map_values) {
		if(ou->getUser()->getCID() == aCID) {
			return ou;
		}
	}
	return 0;
}

void AdcHub::getUserList(OnlineUserList& list) const {
	RLock l(cs);
	for(const auto& i: users) {
		if(i.first != AdcCommand::HUB_SID) {
			list.push_back(i.second);
		}
	}
}

void AdcHub::putUser(const uint32_t aSID, bool disconnect) {
	OnlineUser* ou = nullptr;
	{
		WLock l(cs);
		auto i = users.find(aSID);
		if(i == users.end())
			return;

		ou = i->second;
		users.erase(i);

		availableBytes -= ou->getIdentity().getBytesShared();
	}

	if(aSID != AdcCommand::HUB_SID)
		ClientManager::getInstance()->putOffline(ou, disconnect);

	fire(ClientListener::UserRemoved(), this, ou);
	ou->dec();
}

void AdcHub::clearUsers() {
	SIDMap tmp;
	{
		WLock l(cs);
		users.swap(tmp);
		availableBytes = 0;
	}

	for(auto& i: tmp) {
		if(i.first != AdcCommand::HUB_SID)
			ClientManager::getInstance()->putOffline(i.second, false);
		i.second->dec();
	}
}

void AdcHub::handle(AdcCommand::INF, AdcCommand& c) noexcept {
	if(c.getParameters().empty())
		return;

	string cid;

	OnlineUser* u = 0;
	if(c.getParam("ID", 0, cid)) {
		u = findUser(CID(cid));
		if(u) {
			if(u->getIdentity().getSID() != c.getFrom()) {
				// Same CID but different SID not allowed - buggy hub?
				string nick;
				if(!c.getParam("NI", 0, nick)) {
					nick = "[nick unknown]";
				}
				fire(ClientListener::StatusMessage(), this, u->getIdentity().getNick() + " (" + u->getIdentity().getSIDString() + 
					") has same CID {" + cid + "} as " + nick + " (" + AdcCommand::fromSID(c.getFrom()) + "), ignoring.", ClientListener::FLAG_IS_SPAM);
				return;
			}
		} else {
			u = &getUser(c.getFrom(), CID(cid));
		}
	} else if(c.getFrom() == AdcCommand::HUB_SID) {
		u = &getUser(c.getFrom(), CID());
		string fo;
		if(c.getParam("FO", 0, fo)) {
			StringTokenizer<string> addresses(fo, ',');
			FavoriteManager::getInstance()->setFailOvers(getHubUrl(), getFavToken(), move(addresses.getTokens()));
		}
	} else {
		u = findUser(c.getFrom());
	}

	if(!u) {
		dcdebug("AdcHub::INF Unknown user / no ID\n");
		return;
	}

	for(const auto& p: c.getParameters()) {
		if(p.length() < 2)
			continue;

		if(p.substr(0, 2) == "SS") {
			availableBytes -= u->getIdentity().getBytesShared();
			u->getIdentity().setBytesShared(p.substr(2));
			availableBytes += u->getIdentity().getBytesShared();
		} else {
			u->getIdentity().set(p.c_str(), p.substr(2));
		}
		
		if((p.substr(0, 2) == "VE") || (p.substr(0, 2) == "AP")) {
			if (p.find("AirDC++") != string::npos) {
				u->getUser()->setFlag(User::AIRDCPLUSPLUS);
			}
		}
	}

	if(u->getIdentity().isBot()) {
		u->getUser()->setFlag(User::BOT);
	} else {
		u->getUser()->unsetFlag(User::BOT);
	}

	if(u->getIdentity().supports(ADCS_FEATURE)) {
		u->getUser()->setFlag(User::TLS);
	}

	if(u->getUser() == getMyIdentity().getUser()) {
		state = STATE_NORMAL;
		setAutoReconnect(true);
		setMyIdentity(u->getIdentity());
		updateCounts(false, true);
	}

	if(u->getIdentity().isHub()) {
		setHubIdentity(u->getIdentity());
		fire(ClientListener::HubUpdated(), this);
	} else {
		fire(ClientListener::UserUpdated(), this, u);
	}
}

void AdcHub::handle(AdcCommand::SUP, AdcCommand& c) noexcept {
	if(state != STATE_PROTOCOL) /** @todo SUP changes */
		return;
	bool baseOk = false;
	bool tigrOk = false;
	for(const auto& p: c.getParameters()) {
		if(p == BAS0_SUPPORT) {
			baseOk = true;
			tigrOk = true;
		} else if(p == BASE_SUPPORT) {
			baseOk = true;
		} else if(p == TIGR_SUPPORT) {
			tigrOk = true;
		}
	}
	
	if(!baseOk) {
		fire(ClientListener::StatusMessage(), this, "Failed to negotiate base protocol");
		disconnect(false);
		return;
	} else if(!tigrOk) {
		oldPassword = true;
		// Some hubs fake BASE support without TIGR support =/
		fire(ClientListener::StatusMessage(), this, "Hub probably uses an old version of ADC, please encourage the owner to upgrade");
	}
}

void AdcHub::handle(AdcCommand::SID, AdcCommand& c) noexcept {
	if(state != STATE_PROTOCOL) {
		dcdebug("Invalid state for SID\n");
		return;
	}

	if(c.getParameters().empty())
		return;

	sid = AdcCommand::toSID(c.getParam(0));

	state = STATE_IDENTIFY;
	info(true);
}

void AdcHub::handle(AdcCommand::MSG, AdcCommand& c) noexcept {
	if(c.getParameters().empty())
		return;

	ChatMessage message = { c.getParam(0), findUser(c.getFrom()) };

	if(!message.from)
		return;

	string temp;
	if(c.getParam("PM", 1, temp)) { // add PM<group-cid> as well
		message.to = findUser(c.getTo());
		if(!message.to)
			return;

		message.replyTo = findUser(AdcCommand::toSID(temp));
		if(!message.replyTo)
			return;
	}

	message.thirdPerson = c.hasFlag("ME", 1);

	if(c.getParam("TS", 1, temp))
		message.timestamp = Util::toInt64(temp);

	fire(ClientListener::Message(), this, message);
}

void AdcHub::handle(AdcCommand::GPA, AdcCommand& c) noexcept {
	if(c.getParameters().empty() || c.getFrom() != AdcCommand::HUB_SID)
		return;
	salt = c.getParam(0);
	state = STATE_VERIFY;

	onPassword();
}

void AdcHub::handle(AdcCommand::QUI, AdcCommand& c) noexcept {
	uint32_t s = AdcCommand::toSID(c.getParam(0));

	OnlineUser* victim = findUser(s);
	if(victim) {

		string tmp;
		if(c.getParam("MS", 1, tmp)) {
			OnlineUser* source = 0;
			string tmp2;
			if(c.getParam("ID", 1, tmp2)) {
				source = findUser(AdcCommand::toSID(tmp2));
			}
		
			if(source) {
				tmp = victim->getIdentity().getNick() + " was kicked by " +	source->getIdentity().getNick() + ": " + tmp;
			} else {
				tmp = victim->getIdentity().getNick() + " was kicked: " + tmp;
			}
			fire(ClientListener::StatusMessage(), this, tmp, ClientListener::FLAG_IS_SPAM);
		}
	
		putUser(s, c.getParam("DI", 1, tmp)); 
	}
	
	if(s == sid) {
		// this QUI is directed to us

		string tmp;
		if(c.getParam("TL", 1, tmp)) {
			if(tmp == "-1") {
				setAutoReconnect(false);
			} else {
				setAutoReconnect(true);
				setReconnDelay(Util::toUInt32(tmp));
			}
		}
		if(!victim && c.getParam("MS", 1, tmp)) {
			fire(ClientListener::StatusMessage(), this, tmp, ClientListener::FLAG_NORMAL);
		}
		if(c.getParam("RD", 1, tmp)) {
			fire(ClientListener::Redirect(), this, tmp);
		}
	}
}

void AdcHub::handle(AdcCommand::CTM, AdcCommand& c) noexcept {
	OnlineUser* u = findUser(c.getFrom());
	if(!u || u->getUser() == ClientManager::getInstance()->getMe())
		return;
	if(c.getParameters().size() < 3)
		return;

	const string& protocol = c.getParam(0);
	const string& port = c.getParam(1);
	const string& token = c.getParam(2);

	bool secure = false;
	if(protocol == CLIENT_PROTOCOL) {
		// Nothing special
	} else if(protocol == SECURE_CLIENT_PROTOCOL_TEST && CryptoManager::getInstance()->TLSOk()) {
		secure = true;
	} else {
		unknownProtocol(c.getFrom(), protocol, token);
		return;
	}

	if (!secure && SETTING(TLS_MODE) == SettingsManager::TLS_FORCED) {
		send(AdcCommand(AdcCommand::SEV_FATAL, AdcCommand::ERROR_PROTOCOL_GENERIC, "TLS encryption required", AdcCommand::TYPE_DIRECT).setTo(c.getFrom()));
		return;
	}

	if(!u->getIdentity().isTcpActive()) {
		send(AdcCommand(AdcCommand::SEV_FATAL, AdcCommand::ERROR_PROTOCOL_GENERIC, "IP unknown", AdcCommand::TYPE_DIRECT).setTo(c.getFrom()));
		return;
	}

	ConnectionManager::getInstance()->adcConnect(*u, port, token, secure);
}

void AdcHub::handle(AdcCommand::ZON, AdcCommand& c) noexcept {
	if (c.getFrom() != AdcCommand::HUB_SID)
		return;

	try {
		sock->setMode(BufferedSocket::MODE_ZPIPE);
	} catch (const Exception& e) {
		dcdebug("AdcHub::handleZON failed with error: %s\n", e.getError().c_str());
	}
}

void AdcHub::handle(AdcCommand::ZOF, AdcCommand& c) noexcept {
	if (c.getFrom() != AdcCommand::HUB_SID)
		return;

	try {
		sock->setMode(BufferedSocket::MODE_LINE);
	} catch (const Exception& e) {
		dcdebug("AdcHub::handleZOF failed with error: %s\n", e.getError().c_str());
	}
}

void AdcHub::handle(AdcCommand::RCM, AdcCommand& c) noexcept {
	if(c.getParameters().size() < 2) {
		return;
	}

	OnlineUser* u = findUser(c.getFrom());
	if(!u || u->getUser() == ClientManager::getInstance()->getMe())
		return;

	const string& protocol = c.getParam(0);
	const string& token = c.getParam(1);

	bool secure;
	if(protocol == CLIENT_PROTOCOL) {
		secure = false;
	} else if(protocol == SECURE_CLIENT_PROTOCOL_TEST && CryptoManager::getInstance()->TLSOk()) {
		secure = true;
	} else {
		unknownProtocol(c.getFrom(), protocol, token);
		return;
	}

	if(isActive()) {
		//we are active the other guy is not, this leaves hubhint empty. 
    	connect(*u, token, secure);
		return;
	}

	if (!u->getIdentity().supports(NAT0_FEATURE))
		return;

	// Attempt to traverse NATs and/or firewalls with TCP.
	// If they respond with their own, symmetric, RNT command, both
	// clients call ConnectionManager::adcConnect.
	send(AdcCommand(AdcCommand::CMD_NAT, u->getIdentity().getSID(), AdcCommand::TYPE_DIRECT).
		addParam(protocol).addParam(Util::toString(sock->getLocalPort())).addParam(token));
}

void AdcHub::handle(AdcCommand::CMD, AdcCommand& c) noexcept {
	if(c.getParameters().size() < 1)
		return;
	const string& name = c.getParam(0);
	bool rem = c.hasFlag("RM", 1);
	if(rem) {
		fire(ClientListener::HubUserCommand(), this, (int)UserCommand::TYPE_REMOVE, 0, name, Util::emptyString);
		return;
	}
	bool sep = c.hasFlag("SP", 1);
	string sctx;
	if(!c.getParam("CT", 1, sctx))
		return;
	int ctx = Util::toInt(sctx);
	if(ctx <= 0)
		return;
	if(sep) {
		fire(ClientListener::HubUserCommand(), this, (int)UserCommand::TYPE_SEPARATOR, ctx, name, Util::emptyString);
		return;
	}
	bool once = c.hasFlag("CO", 1);
	string txt;
	if(!c.getParam("TT", 1, txt))
		return;
	fire(ClientListener::HubUserCommand(), this, (int)(once ? UserCommand::TYPE_RAW_ONCE : UserCommand::TYPE_RAW), ctx, name, txt);
}

void AdcHub::sendUDP(const AdcCommand& cmd) noexcept {
	string command;
	string ip;
	string port;
	{
		RLock l(cs);
		auto i = users.find(cmd.getTo());
		if(i == users.end()) {
			dcdebug("AdcHub::sendUDP: invalid user\n");
			return;
		}
		OnlineUser& ou = *i->second;
		if(!ou.getIdentity().isUdpActive()) {
			return;
		}
		ip = ou.getIdentity().getIp();
		port = ou.getIdentity().getUdpPort();
		command = cmd.toString(ou.getUser()->getCID());
	}

	try {
		udp.writeTo(ip, port, command);
	} catch(const SocketException& e) {
		dcdebug("AdcHub::sendUDP: write failed: %s\n", e.getError().c_str());
		udp.close();
	}
}

void AdcHub::handle(AdcCommand::STA, AdcCommand& c) noexcept {
	if(c.getParameters().size() < 2)
		return;

	OnlineUser* u = c.getFrom() == AdcCommand::HUB_SID ? &getUser(c.getFrom(), CID()) : findUser(c.getFrom());
	if(!u)
		return;

	//int severity = Util::toInt(c.getParam(0).substr(0, 1));
	if(c.getParam(0).size() != 3) {
		return;
	}

	switch(Util::toInt(c.getParam(0).substr(1))) {

		case AdcCommand::ERROR_BAD_PASSWORD:
			{
				if (c.getFrom() == AdcCommand::HUB_SID)
					setPassword(Util::emptyString);
				break;
			}

		case AdcCommand::ERROR_COMMAND_ACCESS:
			{
				if (c.getFrom() == AdcCommand::HUB_SID) {
					string tmp;
					if(c.getParam("FC", 1, tmp) && tmp.size() == 4)
						forbiddenCommands.insert(AdcCommand::toFourCC(tmp.c_str()));
				}
				break;
			}

	case AdcCommand::ERROR_PROTOCOL_UNSUPPORTED:
		{
			string tmp;
			if(c.getParam("PR", 1, tmp)) {
				if(tmp == CLIENT_PROTOCOL) {
					u->getUser()->setFlag(User::NO_ADC_1_0_PROTOCOL);
				} else if(tmp == SECURE_CLIENT_PROTOCOL_TEST) {
					u->getUser()->setFlag(User::NO_ADCS_0_10_PROTOCOL);
					u->getUser()->unsetFlag(User::TLS);
				}
				// Try again...
				ConnectionManager::getInstance()->force(u->getUser());
			}
			return;
		}
	}

	ChatMessage message = { c.getParam(1), u };
	fire(ClientListener::Message(), this, message);
}

void AdcHub::handle(AdcCommand::SCH, AdcCommand& c) noexcept {
	OnlineUser* ou = findUser(c.getFrom());
	if(!ou) {
		dcdebug("Invalid user in AdcHub::onSCH\n");
		return;
	}

	fire(ClientListener::AdcSearch(), this, c, ou->getUser()->getCID());
}


void AdcHub::handle(AdcCommand::DSC, AdcCommand& c) noexcept {
	OnlineUser* ou = findUser(c.getFrom());
	if(!ou) {
		dcdebug("Invalid user in AdcHub::onSCH\n");
		return;
	}

	fire(ClientListener::DirectSearch(), this, c, ou->getUser()->getCID());
}

void AdcHub::handle(AdcCommand::DSR, AdcCommand& c) noexcept {
	SearchManager::getInstance()->onDSR(c);
}

void AdcHub::handle(AdcCommand::RES, AdcCommand& c) noexcept {
	OnlineUser* ou = findUser(c.getFrom());
	if(!ou) {
		dcdebug("Invalid user in AdcHub::onRES\n");
		return;
	}
	SearchManager::getInstance()->onRES(c, ou->getUser());
}

void AdcHub::handle(AdcCommand::PSR, AdcCommand& c) noexcept {
	OnlineUser* ou = findUser(c.getFrom());
	if(!ou) {
		dcdebug("Invalid user in AdcHub::onPSR\n");
		return;
	}
	SearchManager::getInstance()->onPSR(c, ou->getUser());
}

void AdcHub::handle(AdcCommand::PBD, AdcCommand& c) noexcept {
	//LogManager::getInstance()->message("GOT PBD TCP: " + c.toString());
	OnlineUser* ou = findUser(c.getFrom());
	if(!ou) {
		dcdebug("Invalid user in AdcHub::onPBD\n");
		return;
	}
	SearchManager::getInstance()->onPBD(c, ou->getUser());
}


void AdcHub::handle(AdcCommand::UBD, AdcCommand& c) noexcept {
	UploadManager::getInstance()->onUBD(c);
}

void AdcHub::handle(AdcCommand::GET, AdcCommand& c) noexcept {
	if(c.getParameters().size() < 5) {
		if(c.getParameters().size() > 0) {
			if(c.getParam(0) == "blom") {
				send(AdcCommand(AdcCommand::SEV_FATAL, AdcCommand::ERROR_PROTOCOL_GENERIC,
					"Too few parameters for blom", AdcCommand::TYPE_HUB));
			} else {
				send(AdcCommand(AdcCommand::SEV_FATAL, AdcCommand::ERROR_TRANSFER_GENERIC,
					"Unknown transfer type", AdcCommand::TYPE_HUB));
			}
		} else {
			send(AdcCommand(AdcCommand::SEV_FATAL, AdcCommand::ERROR_PROTOCOL_GENERIC,
				"Too few parameters for GET", AdcCommand::TYPE_HUB));
		}
		return;
	}

	const string& type = c.getParam(0);
	string sk, sh;
	if(type == "blom" && c.getParam("BK", 4, sk) && c.getParam("BH", 4, sh))  {
		ByteVector v;
		size_t m = Util::toUInt32(c.getParam(3)) * 8;
		size_t k = Util::toUInt32(sk);
		size_t h = Util::toUInt32(sh);
				
		if(k > 8 || k < 1) {
			send(AdcCommand(AdcCommand::SEV_FATAL, AdcCommand::ERROR_TRANSFER_GENERIC, 
				"Unsupported k", AdcCommand::TYPE_HUB));
			return;
		}
		if(h > 64 || h < 1) {
			send(AdcCommand(AdcCommand::SEV_FATAL, AdcCommand::ERROR_TRANSFER_GENERIC, 
				"Unsupported h", AdcCommand::TYPE_HUB));
			return;
		}

		size_t n = 0;
		
		if (getShareProfile() != SP_HIDDEN) {
			if (SETTING(USE_PARTIAL_SHARING))
				n = QueueManager::getInstance()->getQueuedFiles();

			int64_t tmp = 0;
			ShareManager::getInstance()->getProfileInfo(getShareProfile(), tmp, n);
		}
		
		// Ideal size for m is n * k / ln(2), but we allow some slack
		// When h >= 32, m can't go above 2^h anyway since it's stored in a size_t.
		if(m > (5 * Util::roundUp((int64_t)(n * k / log(2.)), (int64_t)64)) || (h < 32 && m > static_cast<size_t>(1U << h))) {
			send(AdcCommand(AdcCommand::SEV_FATAL, AdcCommand::ERROR_TRANSFER_GENERIC,
				"Unsupported m", AdcCommand::TYPE_HUB));
			return;
		}
		
		if (m > 0) {
			dcdebug("Creating bloom filter, k=%u, m=%u, h=%u\n", k, m, h);

			HashBloom bloom;
			bloom.reset(k, m, h);
			ShareManager::getInstance()->getBloom(bloom);
			if (SETTING(USE_PARTIAL_SHARING))
				QueueManager::getInstance()->getBloom(bloom);
			bloom.copy_to(v);
		}
		AdcCommand cmd(AdcCommand::CMD_SND, AdcCommand::TYPE_HUB);
		cmd.addParam(c.getParam(0));
		cmd.addParam(c.getParam(1));
		cmd.addParam(c.getParam(2));
		cmd.addParam(c.getParam(3));
		cmd.addParam(c.getParam(4));
		send(cmd);
		if (m > 0) {
			send((char*)&v[0], v.size());
		}
	}
}

void AdcHub::handle(AdcCommand::NAT, AdcCommand& c) noexcept {
	OnlineUser* u = findUser(c.getFrom());
	if(!u || u->getUser() == ClientManager::getInstance()->getMe() || c.getParameters().size() < 3)
		return;

	const string& protocol = c.getParam(0);
	const string& port = c.getParam(1);
	const string& token = c.getParam(2);

	// bool secure = secureAvail(c.getFrom(), protocol, token);
	bool secure = false;
	if(protocol == CLIENT_PROTOCOL) {
		// Nothing special
	} else if(protocol == SECURE_CLIENT_PROTOCOL_TEST && CryptoManager::getInstance()->TLSOk()) {
		secure = true;
	} else {
		unknownProtocol(c.getFrom(), protocol, token);
		return;
	}

	// Trigger connection attempt sequence locally ...
	auto localPort = Util::toString(sock->getLocalPort());
	dcdebug("triggering connecting attempt in NAT: remote port = %s, local IP = %s, local port = %d\n", port.c_str(), sock->getLocalIp().c_str(), sock->getLocalPort());
	ConnectionManager::getInstance()->adcConnect(*u, port, localPort, BufferedSocket::NAT_CLIENT, token, secure);

	// ... and signal other client to do likewise.
	send(AdcCommand(AdcCommand::CMD_RNT, u->getIdentity().getSID(), AdcCommand::TYPE_DIRECT).addParam(protocol).
		addParam(localPort).addParam(token));
}

void AdcHub::handle(AdcCommand::RNT, AdcCommand& c) noexcept {
	// Sent request for NAT traversal cooperation, which
	// was acknowledged (with requisite local port information).
	OnlineUser* u = findUser(c.getFrom());
	if(!u || u->getUser() == ClientManager::getInstance()->getMe() || c.getParameters().size() < 3)
		return;

	const string& protocol = c.getParam(0);
	const string& port = c.getParam(1);
	const string& token = c.getParam(2);

	bool secure = false;
	if(protocol == CLIENT_PROTOCOL) {
		// Nothing special
	} else if(protocol == SECURE_CLIENT_PROTOCOL_TEST && CryptoManager::getInstance()->TLSOk()) {
		secure = true;
	} else {
		unknownProtocol(c.getFrom(), protocol, token);
		return;
	}

	// Trigger connection attempt sequence locally
	dcdebug("triggering connecting attempt in RNT: remote port = %s, local IP = %s, local port = %d\n", port.c_str(), sock->getLocalIp().c_str(), sock->getLocalPort());
	ConnectionManager::getInstance()->adcConnect(*u, port, Util::toString(sock->getLocalPort()), BufferedSocket::NAT_SERVER, token, secure);
}

void AdcHub::connect(const OnlineUser& user, const string& token) {
	connect(user, token, CryptoManager::getInstance()->TLSOk() && user.getUser()->isSet(User::TLS));
}

void AdcHub::connect(const OnlineUser& user, string const& token, bool secure) {
	if(state != STATE_NORMAL)
		return;

	if (!secure && SETTING(TLS_MODE) == SettingsManager::TLS_FORCED) {
		send(AdcCommand(AdcCommand::SEV_FATAL, AdcCommand::ERROR_PROTOCOL_GENERIC, "TLS encryption required", AdcCommand::TYPE_DIRECT).setTo(user.getIdentity().getSID()));
		return;
	}

	const string* proto;
	if(secure) {
		if(user.getUser()->isSet(User::NO_ADCS_0_10_PROTOCOL)) {
			/// @todo log
			return;
		}
		proto = &SECURE_CLIENT_PROTOCOL_TEST;
	} else {
		if(user.getUser()->isSet(User::NO_ADC_1_0_PROTOCOL)) {
			/// @todo log
			return;
		}
		proto = &CLIENT_PROTOCOL;
	}

	if(isActive()) {
		const string& port = secure ? ConnectionManager::getInstance()->getSecurePort() : ConnectionManager::getInstance()->getPort();
		if(port.empty()) {
			// Oops?
			LogManager::getInstance()->message(STRING(NOT_LISTENING), LogManager::LOG_ERROR);
			return;
		}
		send(AdcCommand(AdcCommand::CMD_CTM, user.getIdentity().getSID(), AdcCommand::TYPE_DIRECT).addParam(*proto).addParam(port).addParam(token));

		//we are expecting an incoming connection from these, map so we know where its coming from.
		ConnectionManager::getInstance()->adcExpect(token, user.getUser()->getCID(), getHubUrl());
	} else {
		send(AdcCommand(AdcCommand::CMD_RCM, user.getIdentity().getSID(), AdcCommand::TYPE_DIRECT).addParam(*proto).addParam(token));
	}
}

void AdcHub::hubMessage(const string& aMessage, bool thirdPerson) {
	if(state != STATE_NORMAL) {
		LogManager::getInstance()->message("Failed to send a hub message because of invalid hub state: " + aMessage + " (current state " + Util::toString(state) + ")", LogManager::LOG_ERROR);
		return;
	}

	AdcCommand c(AdcCommand::CMD_MSG, AdcCommand::TYPE_BROADCAST);
	c.addParam(aMessage);
	if(thirdPerson)
		c.addParam("ME", "1");
	send(c);
}

void AdcHub::privateMessage(const OnlineUserPtr& user, const string& aMessage, bool thirdPerson) {
	if(state != STATE_NORMAL) {
		LogManager::getInstance()->message("Failed to send a private message because of invalid hub state: " + aMessage + " (current state " + Util::toString(state) + ")", LogManager::LOG_ERROR);
		return;
	}

	AdcCommand c(AdcCommand::CMD_MSG, user->getIdentity().getSID(), AdcCommand::TYPE_ECHO);
	c.addParam(aMessage);
	if(thirdPerson)
		c.addParam("ME", "1");
	c.addParam("PM", getMySID());
	send(c);
}

void AdcHub::sendUserCmd(const UserCommand& command, const ParamMap& params) {
	if(state != STATE_NORMAL)
		return;
	string cmd = Util::formatParams(command.getCommand(), params, escape);
	if(command.isChat()) {
		if(command.getTo().empty()) {
			hubMessage(cmd);
		} else {
			const string& to = command.getTo();
			RLock l(cs);
			for(const auto& ou: users | map_values) {
				if(ou->getIdentity().getNick() == to) {
					privateMessage(ou, cmd);
					return;
				}
			}
		}
	} else {
		send(cmd);
	}
}

const vector<StringList>& AdcHub::getSearchExts() {
	if(!searchExts.empty())
		return searchExts;

	// the list is always immutable except for this function where it is initially being filled.
	auto& xSearchExts = const_cast<vector<StringList>&>(searchExts);

	xSearchExts.resize(6);

	/// @todo simplify this as searchExts[0] = { "mp3", "etc" } when VC++ supports initializer lists

	// these extensions *must* be sorted alphabetically!

	{
		StringList& l = xSearchExts[0];
		l.push_back("ape"); l.push_back("flac"); l.push_back("m4a"); l.push_back("mid");
		l.push_back("mp3"); l.push_back("mpc"); l.push_back("ogg"); l.push_back("ra");
		l.push_back("wav"); l.push_back("wma");
	}

	{
		StringList& l = xSearchExts[1];
		l.push_back("7z"); l.push_back("ace"); l.push_back("arj"); l.push_back("bz2");
		l.push_back("gz"); l.push_back("lha"); l.push_back("lzh"); l.push_back("rar");
		l.push_back("tar"); l.push_back("z"); l.push_back("zip");
	}

	{
		StringList& l = xSearchExts[2];
		l.push_back("doc"); l.push_back("docx"); l.push_back("htm"); l.push_back("html");
		l.push_back("nfo"); l.push_back("odf"); l.push_back("odp"); l.push_back("ods");
		l.push_back("odt"); l.push_back("pdf"); l.push_back("ppt"); l.push_back("pptx");
		l.push_back("rtf"); l.push_back("txt"); l.push_back("xls"); l.push_back("xlsx");
		l.push_back("xml"); l.push_back("xps");
	}

	{
		StringList& l = xSearchExts[3];
		l.push_back("app"); l.push_back("bat"); l.push_back("cmd"); l.push_back("com");
		l.push_back("dll"); l.push_back("exe"); l.push_back("jar"); l.push_back("msi");
		l.push_back("ps1"); l.push_back("vbs"); l.push_back("wsf");
	}

	{
		StringList& l = xSearchExts[4];
		l.push_back("bmp"); l.push_back("cdr"); l.push_back("eps"); l.push_back("gif");
		l.push_back("ico"); l.push_back("img"); l.push_back("jpeg"); l.push_back("jpg");
		l.push_back("png"); l.push_back("ps"); l.push_back("psd"); l.push_back("sfw");
		l.push_back("tga"); l.push_back("tif"); l.push_back("webp");
	}

	{
		StringList& l = xSearchExts[5];
		l.push_back("3gp"); l.push_back("asf"); l.push_back("asx"); l.push_back("avi");
		l.push_back("divx"); l.push_back("flv"); l.push_back("mkv"); l.push_back("mov");
		l.push_back("mp4"); l.push_back("mpeg"); l.push_back("mpg"); l.push_back("ogm");
		l.push_back("pxp"); l.push_back("qt"); l.push_back("rm"); l.push_back("rmvb");
		l.push_back("swf"); l.push_back("vob"); l.push_back("webm"); l.push_back("wmv");
	}

	return searchExts;
}

StringList AdcHub::parseSearchExts(int flag) {
	StringList ret;
	const auto& searchExts = getSearchExts();
	for(auto i = searchExts.cbegin(), iend = searchExts.cend(); i != iend; ++i) {
		if(flag & (1 << (i - searchExts.cbegin()))) {
			ret.insert(ret.begin(), i->begin(), i->end());
		}
	}
	return ret;
}

void AdcHub::directSearch(const OnlineUser& user, int aSizeMode, int64_t aSize, int aFileType, const string& aString, const string& aToken, const StringList& aExtList, const string& aDir) {
	if(state != STATE_NORMAL)
		return;

	AdcCommand c(AdcCommand::CMD_DSC, (user.getIdentity().getSID()), AdcCommand::TYPE_DIRECT);
	constructSearch(c, aSizeMode, aSize, aFileType, aString, aToken, aExtList, StringList(), true);
	if (!aDir.empty()) {
		c.addParam("PA", aDir);
	}

	//sendSearch(c);
	send(c);
}

void AdcHub::constructSearch(AdcCommand& c, int aSizeMode, int64_t aSize, int aFileType, const string& aString, const string& aToken, const StringList& aExtList, const StringList& excluded, bool isDirect) {
	if(!aToken.empty())
		c.addParam("TO", aToken);

	if(aFileType == SearchManager::TYPE_TTH) {
		c.addParam("TR", aString);

	} else {
		if(aSizeMode == SearchManager::SIZE_ATLEAST) {
			c.addParam("GE", Util::toString(aSize));
		} else if(aSizeMode == SearchManager::SIZE_ATMOST) {
			c.addParam("LE", Util::toString(aSize));
		} else if (aSizeMode == SearchManager::SIZE_EXACT) {
			c.addParam("GE", Util::toString(aSize));
			c.addParam("LE", Util::toString(aSize));
		}

		StringTokenizer<string> st(aString, ' ');
		for(const auto& t: st.getTokens())
			c.addParam("AN", t);

		for(const auto& e: excluded) {
			c.addParam("NO", e);
		}

		if(aFileType == SearchManager::TYPE_DIRECTORY) {
			c.addParam("TY", "2");
		}

		if(aExtList.size() > 2) {
			StringList exts = aExtList;
			sort(exts.begin(), exts.end());

			uint8_t gr = 0;
			StringList rx;

			const auto& searchExts = getSearchExts();
			for(auto i = searchExts.cbegin(), iend = searchExts.cend(); i != iend; ++i) {
				const StringList& def = *i;

				// gather the exts not present in any of the lists
				StringList temp(def.size() + exts.size());
				temp = StringList(temp.begin(), set_symmetric_difference(def.begin(), def.end(),
					exts.begin(), exts.end(), temp.begin()));

				// figure out whether the remaining exts have to be added or removed from the set
				StringList rx_;
				bool ok = true;
				for(auto diff = temp.begin(); diff != temp.end();) {
					if(find(def.cbegin(), def.cend(), *diff) == def.cend()) {
						++diff; // will be added further below as an "EX"
					} else {
						if(rx_.size() == 2) {
							ok = false;
							break;
						}
						rx_.push_back(*diff);
						diff = temp.erase(diff);
					}
				}
				if(!ok) // too many "RX"s necessary - disregard this group
					continue;

				// let's include this group!
				gr += 1 << (i - searchExts.cbegin());

				exts = temp; // the exts to still add (that were not defined in the group)

				rx.insert(rx.begin(), rx_.begin(), rx_.end());

				if(exts.size() <= 2)
					break;
				// keep looping to see if there are more exts that can be grouped
			}

			if(gr) {
				auto appendGroupInfo = [rx, exts, gr] (AdcCommand& aCmd) -> void {
					for(const auto& ext: exts)
						aCmd.addParam("EX", ext);

					aCmd.addParam("GR", Util::toString(gr));
					for(const auto& i: rx)
						aCmd.addParam("RX", i);
				};

				if (isDirect) {
					// direct search always uses SEGA, just append the group information in the current command
					appendGroupInfo(c);
					return;
				} else {
					// some extensions can be grouped; let's send a command with grouped exts.
					AdcCommand c_gr(AdcCommand::CMD_SCH, AdcCommand::TYPE_FEATURE);
					c_gr.setFeatures('+' + SEGA_FEATURE);

					const auto& params = c.getParameters();
					for(const auto& p: params)
						c_gr.addParam(p);

					appendGroupInfo(c_gr);
					sendSearch(c_gr);

					// make sure users with the feature don't receive the search twice.
					c.setType(AdcCommand::TYPE_FEATURE);
					c.setFeatures('-' + SEGA_FEATURE);
				}
			}
		}

		for(const auto& ex: aExtList)
			c.addParam("EX", ex);
	}
}

void AdcHub::search(Search* s) {
	if(state != STATE_NORMAL)
		return;

	AdcCommand c(AdcCommand::CMD_SCH, AdcCommand::TYPE_BROADCAST);

	constructSearch(c, s->sizeType, s->size, s->fileType, s->query, s->token, s->exts, s->excluded, false);

	if (!s->key.empty()) {
		c.addParam("KY", s->key);
	}

	delete s;
	sendSearch(c);
}

void AdcHub::sendSearch(AdcCommand& c) {
	if(isActive()) {
		send(c);
	} else {
		c.setType(AdcCommand::TYPE_FEATURE);
		string features = c.getFeatures();
		c.setFeatures(features + '+' + TCP4_FEATURE + '-' + NAT0_FEATURE);
		send(c);		
		c.setFeatures(features + "+" + NAT0_FEATURE);

		send(c);
	}
}

void AdcHub::password(const string& pwd) {
	if(state != STATE_VERIFY)
		return;
	if(!salt.empty()) {
		size_t saltBytes = salt.size() * 5 / 8;
		boost::scoped_array<uint8_t> buf(new uint8_t[saltBytes]);
		Encoder::fromBase32(salt.c_str(), &buf[0], saltBytes);
		TigerHash th;
		if(oldPassword) {
			CID cid = getMyIdentity().getUser()->getCID();
			th.update(cid.data(), CID::SIZE);
		}
		th.update(pwd.data(), pwd.length());
		th.update(&buf[0], saltBytes);
		send(AdcCommand(AdcCommand::CMD_PAS, AdcCommand::TYPE_HUB).addParam(Encoder::toBase32(th.finalize(), TigerHash::BYTES)));
		salt.clear();
	}
}

static void addParam(StringMap& lastInfoMap, AdcCommand& c, const string& var, const string& value) {
	auto i = lastInfoMap.find(var);
	if(i != lastInfoMap.end()) {
		if(i->second != value) {
			if(value.empty()) {
				lastInfoMap.erase(i);
			} else { 
				i->second = value;
			}
			c.addParam(var, value);
		}
	} else if(!value.empty()) {
		lastInfoMap.emplace(var, value);
		c.addParam(var, value);
	}
}

void AdcHub::info(bool /*alwaysSend*/) {
	if(state != STATE_IDENTIFY && state != STATE_NORMAL)
		return;

	reloadSettings(false);

	AdcCommand c(AdcCommand::CMD_INF, AdcCommand::TYPE_BROADCAST);

	if (state == STATE_NORMAL) {
		if(!updateCounts(false, false))
			return;
	}

	addParam(lastInfoMap, c, "ID", ClientManager::getInstance()->getMyCID().toBase32());
	addParam(lastInfoMap, c, "PD", ClientManager::getInstance()->getMyPID().toBase32());
	addParam(lastInfoMap, c, "NI", get(Nick));
	addParam(lastInfoMap, c, "DE", getDescription());
	addParam(lastInfoMap, c, "SL", Util::toString(UploadManager::getInstance()->getSlots()));
	addParam(lastInfoMap, c, "FS", Util::toString(UploadManager::getInstance()->getFreeSlots()));

	size_t fileCount = SETTING(USE_PARTIAL_SHARING) ? QueueManager::getInstance()->getQueuedFiles() : 0;
	int64_t size = 0;
	if (getShareProfile() != SP_HIDDEN)
		ShareManager::getInstance()->getProfileInfo(getShareProfile(), size, fileCount);

	addParam(lastInfoMap, c, "SS", Util::toString(size));
	addParam(lastInfoMap, c, "SF", Util::toString(fileCount));

	addParam(lastInfoMap, c, "EM", SETTING(EMAIL));
	addParam(lastInfoMap, c, "HN", Util::toString(counts[COUNT_NORMAL]));
	addParam(lastInfoMap, c, "HR", Util::toString(counts[COUNT_REGISTERED]));
	addParam(lastInfoMap, c, "HO", Util::toString(counts[COUNT_OP]));	

	addParam(lastInfoMap, c, "VE", "AirDC++ " SHORTVERSIONSTRING);
	addParam(lastInfoMap, c, "AW", AirUtil::getAway() ? "1" : Util::emptyString);
	addParam(lastInfoMap, c, "LC", Localization::getLocale());

	int limit = ThrottleManager::getInstance()->getDownLimit();
	if (limit > 0) {
		addParam(lastInfoMap, c, "DS", Util::toString(limit * 1024));
	} else {
		addParam(lastInfoMap, c, "DS", Util::toString((long)(Util::toDouble(SETTING(DOWNLOAD_SPEED))*1024*1024/8)));
	}

	limit = ThrottleManager::getInstance()->getUpLimit();
	if (limit > 0) {
		addParam(lastInfoMap, c, "US", Util::toString(limit * 1024));
	} else {
		addParam(lastInfoMap, c, "US", Util::toString((long)(Util::toDouble(SETTING(UPLOAD_SPEED))*1024*1024/8)));
	}
	
	string su(SEGA_FEATURE);

	if(CryptoManager::getInstance()->TLSOk()) {
		su += "," + ADCS_FEATURE;
		auto &kp = CryptoManager::getInstance()->getKeyprint();
		addParam(lastInfoMap, c, "KP", "SHA256/" + Encoder::toBase32(&kp[0], kp.size()));
	}

	if(CONNSETTING(NO_IP_OVERRIDE) && !getUserIp().empty()) {
		addParam(lastInfoMap, c, "I4", Socket::resolve(getUserIp(), AF_INET));
	} else {
		addParam(lastInfoMap, c, "I4", "0.0.0.0");
	}

	if(isActive()) {
		addParam(lastInfoMap, c, "U4", SearchManager::getInstance()->getPort());
		su += "," + TCP4_FEATURE;
		su += "," + UDP4_FEATURE;
		if (SETTING(ENABLE_SUDP))
			su += "," + SUD1_FEATURE;
	} else {
		su += "," + NAT0_FEATURE;
		addParam(lastInfoMap, c, "U4", "");
	}

	addParam(lastInfoMap, c, "SU", su);

	if(c.getParameters().size() > 0) {
		send(c);
	}
}

void AdcHub::refreshUserList(bool) {
	OnlineUserList v;

	RLock l(cs);
	for(const auto& i: users) {
		if(i.first != AdcCommand::HUB_SID) {
			v.push_back(i.second);
		}
	}
	fire(ClientListener::UsersUpdated(), this, v);
}

string AdcHub::checkNick(const string& aNick) {
	string tmp = aNick;
	for(size_t i = 0; i < aNick.size(); ++i) {
		if(static_cast<uint8_t>(tmp[i]) <= 32) {
			tmp[i] = '_';
		}
	}
	return tmp;
}

void AdcHub::send(const AdcCommand& cmd) {
	if(forbiddenCommands.find(AdcCommand::toFourCC(cmd.getFourCC().c_str())) == forbiddenCommands.end()) {
		if(cmd.getType() == AdcCommand::TYPE_UDP)
			sendUDP(cmd);
		send(cmd.toString(sid));
	}
}

void AdcHub::unknownProtocol(uint32_t target, const string& protocol, const string& token) {
	AdcCommand cmd(AdcCommand::SEV_FATAL, AdcCommand::ERROR_PROTOCOL_UNSUPPORTED, "Protocol unknown", AdcCommand::TYPE_DIRECT);
	cmd.setTo(target);
	cmd.addParam("PR", protocol);
	cmd.addParam("TO", token);

	send(cmd);
}

void AdcHub::on(Connected c) noexcept {
	Client::on(c);

	if(state != STATE_PROTOCOL) {
		return;
	}

	lastInfoMap.clear();
	sid = 0;
	forbiddenCommands.clear();

	AdcCommand cmd(AdcCommand::CMD_SUP, AdcCommand::TYPE_HUB);
	cmd.addParam(BAS0_SUPPORT).addParam(BASE_SUPPORT).addParam(TIGR_SUPPORT);
	
	if(SETTING(HUB_USER_COMMANDS)) {
		cmd.addParam(UCM0_SUPPORT);
	}

	if(SETTING(BLOOM_MODE) == SettingsManager::BLOOM_ENABLED) {
		cmd.addParam(BLO0_SUPPORT);
	}
	cmd.addParam(ZLIF_SUPPORT);
	send(cmd);
}

void AdcHub::on(Line l, const string& aLine) noexcept {
	Client::on(l, aLine);

	if(!Text::validateUtf8(aLine)) {
		// @todo report to user?
		return;
	}

	dispatch(aLine);
}

void AdcHub::on(Failed f, const string& aLine) noexcept {
	clearUsers();
	Client::on(f, aLine);
}

void AdcHub::on(Second s, uint64_t aTick) noexcept {
	Client::on(s, aTick);
	if(state == STATE_NORMAL && (aTick > (getLastActivity() + 120*1000)) ) {
		send("\n", 1);
	}
}

OnlineUserPtr AdcHub::findUser(const string& aNick) const { 
	RLock l(cs); 
	for(auto ou: users | map_values) { 
		if(ou->getIdentity().getNick() == aNick) { 
			return ou; 
		} 
	} 
	return nullptr; 
}

} // namespace dcpp