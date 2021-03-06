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
#include "ConnectionManager.h"

#include "ConnectivityManager.h"
#include "ResourceManager.h"
#include "DownloadManager.h"
#include "UploadManager.h"
#include "CryptoManager.h"
#include "ClientManager.h"
#include "QueueManager.h"
#include "LogManager.h"
#include "UserConnection.h"
#include "AirUtil.h"
#include "format.h"

namespace dcpp {

string TokenManager::getToken() {
	Lock l(cs);
	string token = Util::toString(Util::rand());
	tokens.insert(token);
	return token;
}

bool TokenManager::addToken(const string& aToken) {
	Lock l(cs);
	if (tokens.find(aToken) == tokens.end()) {
		tokens.insert(aToken);
		return true;
	}
	return false;
}

void TokenManager::removeToken(const string& aToken) {
	Lock l(cs);
	auto p = tokens.find(aToken);
	if (p != tokens.end())
		tokens.erase(p);
	else
		dcassert(0);
}

ConnectionManager::ConnectionManager() : floodCounter(0), shuttingDown(false) {
	TimerManager::getInstance()->addListener(this);
	ClientManager::getInstance()->addListener(this);

	features.push_back(UserConnection::FEATURE_MINISLOTS);
	features.push_back(UserConnection::FEATURE_XML_BZLIST);
	features.push_back(UserConnection::FEATURE_ADCGET);
	if (!SettingsManager::lanMode) {
		features.push_back(UserConnection::FEATURE_TTHL);
		features.push_back(UserConnection::FEATURE_TTHF);
	}
	features.push_back(UserConnection::FEATURE_AIRDC);

	adcFeatures.push_back("AD" + UserConnection::FEATURE_ADC_BAS0);
	adcFeatures.push_back("AD" + UserConnection::FEATURE_ADC_BASE);
	adcFeatures.push_back("AD" + UserConnection::FEATURE_ADC_BZIP);
	if (!SettingsManager::lanMode) {
		adcFeatures.push_back("AD" + UserConnection::FEATURE_ADC_TIGR);
		adcFeatures.push_back("AD" + UserConnection::FEATURE_ADC_MCN1);
	}
	adcFeatures.push_back("AD" + UserConnection::FEATURE_ADC_UBN1);
}

void ConnectionManager::listen() {
	server.reset(new Server(false, Util::toString(CONNSETTING(TCP_PORT)), CONNSETTING(BIND_ADDRESS)));

	if(!CryptoManager::getInstance()->TLSOk()) {
		dcdebug("Skipping secure port: %d\n", CONNSETTING(TLS_PORT));
		return;
	}
	secureServer.reset(new Server(true, Util::toString(CONNSETTING(TLS_PORT)), CONNSETTING(BIND_ADDRESS)));
}

bool ConnectionQueueItem::allowNewConnections(int running) {
	return (running < AirUtil::getSlotsPerUser(true) || AirUtil::getSlotsPerUser(true) == 0) && (running < maxConns || maxConns == 0);
}

/**
 * Request a connection for downloading.
 * DownloadManager::addConnection will be called as soon as the connection is ready
 * for downloading.
 * @param aUser The user to connect to.
 */
void ConnectionManager::getDownloadConnection(const HintedUser& aUser, bool smallSlot) {
	dcassert(aUser.user);
	bool supportMcn = false;

	if (!DownloadManager::getInstance()->checkIdle(aUser.user, smallSlot)) {
		ConnectionQueueItem* cqi = nullptr;
		int running = 0;

		{
			WLock l(cs);
			for(auto i: downloads) {
				cqi = i;
				if (cqi->getUser() == aUser.user && !cqi->isSet(ConnectionQueueItem::FLAG_REMOVE)) {
					if (cqi->isSet(ConnectionQueueItem::FLAG_MCN1)) {
						supportMcn = true;
						if (cqi->getState() != ConnectionQueueItem::RUNNING) {
							//already has a waiting item? small slot doesn't count
							if (!smallSlot) {
								return;
							}
						} else {
							running++;
						}
					} else if (cqi->isSet(ConnectionQueueItem::FLAG_SMALL_CONF)) {
						supportMcn = true;
						//no need to continue with small slot if an item with the same type exists already (no mather whether it's running or not)
						if (smallSlot)
							return;
					} else {
						//no need to continue with non-MCN users
						return;
					}
				}
			}

			if (supportMcn && !smallSlot && !cqi->allowNewConnections(running)) {
				return;
			}

			//WLock l (cs);
			dcdebug("Get cqi");
			cqi = getCQI(aUser, true);
			if (smallSlot)
				cqi->setFlag(ConnectionQueueItem::FLAG_SMALL);
		}
	}
}

ConnectionQueueItem* ConnectionManager::getCQI(const HintedUser& aUser, bool aDownload, const string& aToken) {
	ConnectionQueueItem* cqi = new ConnectionQueueItem(aUser, aDownload, !aToken.empty() ? aToken : tokens.getToken());

	if(aDownload) {
		downloads.push_back(cqi);
	} else {
		uploads.push_back(cqi);
	}

	fire(ConnectionManagerListener::Added(), cqi);
	return cqi;
}

void ConnectionManager::putCQI(ConnectionQueueItem* cqi) {
	//allways called from inside lock

	fire(ConnectionManagerListener::Removed(), cqi);
	if(cqi->getDownload()) {
		dcassert(find(downloads.begin(), downloads.end(), cqi) != downloads.end());
		downloads.erase(remove(downloads.begin(), downloads.end(), cqi), downloads.end());
		delayedTokens[cqi->getToken()] = GET_TICK();
	} else {
		dcassert(find(uploads.begin(), uploads.end(), cqi) != uploads.end());
		uploads.erase(remove(uploads.begin(), uploads.end(), cqi), uploads.end());
	}

	tokens.removeToken(cqi->getToken());
	delete cqi;
}

UserConnection* ConnectionManager::getConnection(bool aNmdc, bool secure) noexcept {
	UserConnection* uc = new UserConnection(secure);
	uc->addListener(this);
	{
		WLock l(cs);
		userConnections.push_back(uc);
	}
	if(aNmdc)
		uc->setFlag(UserConnection::FLAG_NMDC);
	return uc;
}

void ConnectionManager::putConnection(UserConnection* aConn) {
	aConn->removeListener(this);
	aConn->disconnect(true);

	WLock l(cs);
	userConnections.erase(remove(userConnections.begin(), userConnections.end(), aConn), userConnections.end());
}

void ConnectionManager::onUserUpdated(const UserPtr& aUser) {
	RLock l(cs);
	for(auto cqi: downloads) {
		if(cqi->getUser() == aUser) {
			fire(ConnectionManagerListener::UserUpdated(), cqi);
		}
	}

	for(auto cqi: uploads) {
		if(cqi->getUser() == aUser) {
			fire(ConnectionManagerListener::UserUpdated(), cqi);
		}
	}
}

void ConnectionManager::on(TimerManagerListener::Second, uint64_t aTick) noexcept {
	StringList removedTokens;

	{
		RLock l(cs);
		int attemptLimit = SETTING(DOWNCONN_PER_SEC);
		uint16_t attempts = 0;
		for(auto cqi: downloads) {
			if(cqi->getState() != ConnectionQueueItem::ACTIVE && cqi->getState() != ConnectionQueueItem::RUNNING) {
				if(!cqi->getUser()->isOnline() || cqi->isSet(ConnectionQueueItem::FLAG_REMOVE)) {
					removedTokens.push_back(cqi->getToken());
					continue;
				}

				if(cqi->getErrors() == -1 && cqi->getLastAttempt() != 0) {
					// protocol error, don't reconnect except after a forced attempt
					continue;
				}

				if((cqi->getLastAttempt() == 0 && attempts < attemptLimit*2) || ((attemptLimit == 0 || attempts < attemptLimit) &&
					cqi->getLastAttempt() + 60 * 1000 * max(1, cqi->getErrors()) < aTick))
				{
					cqi->setLastAttempt(aTick);

					string bundleToken, hubHint = cqi->getHubUrl();

					//we'll also validate the hubhint (and that the user is online) before making any connection attempt
					QueueItem::Priority prio = QueueManager::getInstance()->hasDownload(cqi->getUser(), hubHint, cqi->isSet(ConnectionQueueItem::FLAG_SMALL), bundleToken);
					cqi->setHubUrl(hubHint);
					cqi->setLastBundle(bundleToken);
					

					if(prio == QueueItem::PAUSED) {
						removedTokens.push_back(cqi->getToken());
						continue;
					}

					bool startDown = DownloadManager::getInstance()->startDownload(prio);

					if(cqi->getState() == ConnectionQueueItem::WAITING) {
						if(startDown) {
							cqi->setState(ConnectionQueueItem::CONNECTING);							
							ClientManager::getInstance()->connect(cqi->getHintedUser(), cqi->getToken());
							fire(ConnectionManagerListener::StatusChanged(), cqi);
							attempts++;
						} else {
							cqi->setState(ConnectionQueueItem::NO_DOWNLOAD_SLOTS);
							fire(ConnectionManagerListener::Failed(), cqi, STRING(ALL_DOWNLOAD_SLOTS_TAKEN));
						}
					} else if(cqi->getState() == ConnectionQueueItem::NO_DOWNLOAD_SLOTS && startDown) {
						cqi->setState(ConnectionQueueItem::WAITING);
					}
				} else if(cqi->getState() == ConnectionQueueItem::CONNECTING && cqi->getLastAttempt() + 50*1000 < aTick) {

					cqi->setErrors(cqi->getErrors() + 1);
					fire(ConnectionManagerListener::Failed(), cqi, STRING(CONNECTION_TIMEOUT));
					cqi->setState(ConnectionQueueItem::WAITING);
				}
			} else if (cqi->isSet(ConnectionQueueItem::FLAG_REMOVE)) {
				cqi->unsetFlag(ConnectionQueueItem::FLAG_REMOVE);
			}
		}
	}

	if (!removedTokens.empty()) {
		WLock l (cs);
		for(auto& m: removedTokens) {
			auto s = find(downloads.begin(), downloads.end(), m);
			if (s != downloads.end()) {
				putCQI(*s);
			}
		}
	}
}

void ConnectionManager::addRunningMCN(const UserConnection *aSource) noexcept {
	{
		RLock l(cs);
		auto i = find(downloads.begin(), downloads.end(), aSource->getToken());
		if (i != downloads.end()) {
			ConnectionQueueItem* cqi = *i;

			//we need to check if we have queued something also while the small file connection was being established
			if (!cqi->isSet(ConnectionQueueItem::FLAG_MCN1) && !cqi->isSet(ConnectionQueueItem::FLAG_SMALL_CONF))
				return;

			cqi->setState(ConnectionQueueItem::RUNNING);
			//LogManager::getInstance()->message("Running downloads for the user: " + Util::toString(runningDownloads[aSource->getUser()]));

			int running = 0;
			for(auto cqi: downloads) {
				if (cqi->getUser() == aSource->getUser() && !cqi->isSet(ConnectionQueueItem::FLAG_SMALL_CONF) && !cqi->isSet(ConnectionQueueItem::FLAG_REMOVE)) {
					if (cqi->getState() != ConnectionQueueItem::RUNNING) {
						return;
					}
					running++;
				}
			}

			if (running > 0 && cqi->isSet(ConnectionQueueItem::FLAG_SMALL_CONF))
				return;

			if (!cqi->allowNewConnections(running) && !cqi->isSet(ConnectionQueueItem::FLAG_REMOVE))
				return;
		}
	}

	QueueItem::Priority prio = QueueManager::getInstance()->hasDownload(aSource->getHintedUser(), ClientManager::getInstance()->getHubSet(aSource->getUser()->getCID()), false);
	bool startDown = DownloadManager::getInstance()->startDownload(prio, true);

	if(prio != QueueItem::PAUSED && startDown) {
		WLock l (cs);
		ConnectionQueueItem* cqiNew = getCQI(aSource->getHintedUser(),true);
		cqiNew->setFlag(ConnectionQueueItem::FLAG_MCN1);
	}
}


void ConnectionManager::on(TimerManagerListener::Minute, uint64_t aTick) noexcept {
	WLock l(cs);
	for(auto i = delayedTokens.begin(); i != delayedTokens.end();) {
		if((i->second + (90 * 1000)) < aTick) {
			delayedTokens.erase(i++);
		} else
			++i;
	}

	for(auto j: userConnections) {
		if((j->getLastActivity() + 180*1000) < aTick) { //hmm 3 minutes?
			j->disconnect(true);
		}
	}
}

const string& ConnectionManager::getPort() const {
	return server.get() ? server->getPort() : Util::emptyString;
}

const string& ConnectionManager::getSecurePort() const {
	return secureServer.get() ? secureServer->getPort() : Util::emptyString;
}

static const uint32_t FLOOD_TRIGGER = 20000;
static const uint32_t FLOOD_ADD = 2000;

ConnectionManager::Server::Server(bool secure, const string& port_, const string& ip) :
sock(Socket::TYPE_TCP), secure(secure), die(false)
{
	sock.setLocalIp4(ip);
	port = sock.listen(port_);

	start();
}

static const uint32_t POLL_TIMEOUT = 250;

int ConnectionManager::Server::run() noexcept {
	while(!die) {
		try {
			while(!die) {
				auto ret = sock.wait(POLL_TIMEOUT, true, false);
				if(ret.first) {
					ConnectionManager::getInstance()->accept(sock, secure);
					
				}
			}
		} catch(const Exception& e) {
			dcdebug("ConnectionManager::Server::run Error: %s\n", e.getError().c_str());
		}

		bool failed = false;
		while(!die) {
			try {
				sock.disconnect();
				port = sock.listen(port);

				if(failed) {
					LogManager::getInstance()->message("Connectivity restored", LogManager::LOG_INFO);
					failed = false;
				}
				break;
			} catch(const SocketException& e) {
				dcdebug("ConnectionManager::Server::run Stopped listening: %s\n", e.getError().c_str());

				if(!failed) {
					LogManager::getInstance()->message(str(boost::format("Connectivity error: %1%") % e.getError()), LogManager::LOG_ERROR);
					failed = true;
				}

				// Spin for 60 seconds
				for(auto i = 0; i < 60 && !die; ++i) {
					Thread::sleep(1000);
				}
			}
		}
	}
	return 0;
}

/**
 * Someone's connecting, accept the connection and wait for identification...
 * It's always the other fellow that starts sending if he made the connection.
 */
void ConnectionManager::accept(const Socket& sock, bool secure) noexcept {
	uint64_t now = GET_TICK();

	if(now > floodCounter) {
		floodCounter = now + FLOOD_ADD;
	} else {
		if(false && now + FLOOD_TRIGGER < floodCounter) {
			Socket s(Socket::TYPE_TCP);
			try {
				s.accept(sock);
			} catch(const SocketException&) {
				// ...
			}
			dcdebug("Connection flood detected!\n");
			return;
		} else {
			floodCounter += FLOOD_ADD;
		}
	}
	UserConnection* uc = getConnection(false, secure);
	uc->setFlag(UserConnection::FLAG_INCOMING);
	uc->setState(UserConnection::STATE_SUPNICK);
	uc->setLastActivity(GET_TICK());
	try {
		uc->accept(sock);
	} catch(const Exception&) {
		putConnection(uc);
		delete uc;
	}
}
	
void ConnectionManager::nmdcConnect(const string& aServer, const string& aPort, const string& aNick, const string& hubUrl, const string& encoding, bool stealth, bool secure) {
	nmdcConnect(aServer, aPort, Util::emptyString, BufferedSocket::NAT_NONE, aNick, hubUrl, encoding, stealth, secure);
}

void ConnectionManager::nmdcConnect(const string& aServer, const string& aPort, const string& localPort, BufferedSocket::NatRoles natRole, const string& aNick, const string& hubUrl, const string& encoding, bool /*stealth*/, bool secure) {
	if(shuttingDown)
		return;

	UserConnection* uc = getConnection(true, secure);
	uc->setToken(aNick);
	uc->setHubUrl(hubUrl);
	uc->setEncoding(encoding);
	uc->setState(UserConnection::STATE_CONNECT);
	uc->setFlag(UserConnection::FLAG_NMDC);
	try {
		uc->connect(aServer, aPort, localPort, natRole);
	} catch(const Exception&) {
		putConnection(uc);
		delete uc;
	}
}

void ConnectionManager::adcConnect(const OnlineUser& aUser, const string& aPort, const string& aToken, bool secure) {
	adcConnect(aUser, aPort, Util::emptyString, BufferedSocket::NAT_NONE, aToken, secure);
}

void ConnectionManager::adcConnect(const OnlineUser& aUser, const string& aPort, const string& localPort, BufferedSocket::NatRoles natRole, const string& aToken, bool secure) {
	if(shuttingDown)
		return;

	UserConnection* uc = getConnection(false, secure);
	uc->setEncoding(Text::utf8);
	uc->setState(UserConnection::STATE_CONNECT);
	uc->setHubUrl(aUser.getClient().getHubUrl());
	uc->setToken(aToken);
	if(aUser.getIdentity().isOp()) {
		uc->setFlag(UserConnection::FLAG_OP);
	}
	try {
		uc->connect(aUser.getIdentity().getIp(), aPort, localPort, natRole);
	} catch(const Exception&) {
		putConnection(uc);
		delete uc;
	}
}

void ConnectionManager::disconnect() noexcept {
	server.reset();
	secureServer.reset();
}

void ConnectionManager::on(AdcCommand::SUP, UserConnection* aSource, const AdcCommand& cmd) noexcept {
	if(aSource->getState() != UserConnection::STATE_SUPNICK) {
		// Already got this once, ignore...@todo fix support updates
		dcdebug("CM::onSUP %p sent sup twice\n", (void*)aSource);
		return;
	}

	bool baseOk = false;
	bool tigrOk = false;

	for(auto& i: cmd.getParameters()) {
		if(i.compare(0, 2, "AD") == 0) {
			string feat = i.substr(2);
			if(feat == UserConnection::FEATURE_ADC_BASE || feat == UserConnection::FEATURE_ADC_BAS0) {
				baseOk = true;
				// For bas0 tiger is implicit
				if(feat == UserConnection::FEATURE_ADC_BAS0) {
					tigrOk = true;
				}
				// ADC clients must support all these...
				aSource->setFlag(UserConnection::FLAG_SUPPORTS_ADCGET);
				aSource->setFlag(UserConnection::FLAG_SUPPORTS_MINISLOTS);
				aSource->setFlag(UserConnection::FLAG_SUPPORTS_TTHF);
				aSource->setFlag(UserConnection::FLAG_SUPPORTS_TTHL);
				// For compatibility with older clients...
				aSource->setFlag(UserConnection::FLAG_SUPPORTS_XML_BZLIST);
			} else if(feat == UserConnection::FEATURE_ZLIB_GET) {
				aSource->setFlag(UserConnection::FLAG_SUPPORTS_ZLIB_GET);
			} else if(feat == UserConnection::FEATURE_ADC_BZIP) {
				aSource->setFlag(UserConnection::FLAG_SUPPORTS_XML_BZLIST);
			} else if(feat == UserConnection::FEATURE_ADC_TIGR) {
				tigrOk = true;
			} else if(feat == UserConnection::FEATURE_ADC_MCN1) {
				aSource->setFlag(UserConnection::FLAG_MCN1);
			} else if(feat == UserConnection::FEATURE_ADC_UBN1) {
				aSource->setFlag(UserConnection::FLAG_UBN1);
			}
		}
	}

	if(!baseOk) {
		aSource->send(AdcCommand(AdcCommand::SEV_FATAL, AdcCommand::ERROR_PROTOCOL_GENERIC, "Invalid SUP"));
		aSource->disconnect();
		return;
	}

	int mcn = 0;
	if(aSource->isSet(UserConnection::FLAG_MCN1)) {
		int slots = 0;
		slots = AirUtil::getSlotsPerUser(false);
		if (slots != 0)
			mcn=slots;
	}

	if(aSource->isSet(UserConnection::FLAG_INCOMING)) {
		StringList defFeatures = adcFeatures;
		if(SETTING(COMPRESS_TRANSFERS)) {
			defFeatures.push_back("AD" + UserConnection::FEATURE_ZLIB_GET);
		}
		aSource->sup(defFeatures);
		aSource->inf(false, mcn);
	} else {
		aSource->inf(true, mcn);
	}
	aSource->setState(UserConnection::STATE_INF);
}

void ConnectionManager::on(AdcCommand::STA, UserConnection*, const AdcCommand& /*cmd*/) noexcept {
	
}

void ConnectionManager::on(UserConnectionListener::Connected, UserConnection* aSource) noexcept {
	if(aSource->isSecure() && !aSource->isTrusted() && !SETTING(ALLOW_UNTRUSTED_CLIENTS)) {
		putConnection(aSource);
		//QueueManager::getInstance()->removeSource(aSource->getUser(), QueueItem::Source::FLAG_UNTRUSTED);
		return;
	}

	if(SETTING(TLS_MODE) == SettingsManager::TLS_FORCED && !aSource->isSet(UserConnection::FLAG_NMDC) && !aSource->isSecure()) {
		putConnection(aSource);
		//QueueManager::getInstance()->removeSource(aSource->getUser(), QueueItem::Source::FLAG_UNENCRYPTED);
		return;
	}

	dcassert(aSource->getState() == UserConnection::STATE_CONNECT);
	if(aSource->isSet(UserConnection::FLAG_NMDC)) {
		aSource->myNick(aSource->getToken());
		aSource->lock(CryptoManager::getInstance()->getLock(), CryptoManager::getInstance()->getPk() + "Ref=" + aSource->getHubUrl());
	} else {
		StringList defFeatures = adcFeatures;
		if(SETTING(COMPRESS_TRANSFERS)) {
			defFeatures.push_back("AD" + UserConnection::FEATURE_ZLIB_GET);
		}
		aSource->sup(defFeatures);
		aSource->send(AdcCommand(AdcCommand::SEV_SUCCESS, AdcCommand::SUCCESS, Util::emptyString).addParam("RF", aSource->getHubUrl()));
	}
	aSource->setState(UserConnection::STATE_SUPNICK);
}

void ConnectionManager::on(UserConnectionListener::MyNick, UserConnection* aSource, const string& aNick) noexcept {
	if(aSource->getState() != UserConnection::STATE_SUPNICK) {
		// Already got this once, ignore...
		dcdebug("CM::onMyNick %p sent nick twice\n", (void*)aSource);
		return;
	}

	dcassert(aNick.size() > 0);
	dcdebug("ConnectionManager::onMyNick %p, %s\n", (void*)aSource, aNick.c_str());
	dcassert(!aSource->getUser());

	if(aSource->isSet(UserConnection::FLAG_INCOMING)) {
		// Try to guess where this came from...
		pair<string, string> i = expectedConnections.remove(aNick);
		if(i.second.empty()) {
			dcassert(i.first.empty());
			dcdebug("Unknown incoming connection from %s\n", aNick.c_str());
			putConnection(aSource);
			return;
		}
        aSource->setToken(i.first);	
		aSource->setHubUrl(i.second);
		aSource->setEncoding(ClientManager::getInstance()->findHubEncoding(i.second));
	}

	string nick = Text::toUtf8(aNick, aSource->getEncoding());
	CID cid = ClientManager::getInstance()->makeCid(nick, aSource->getHubUrl());

	// First, we try looking in the pending downloads...hopefully it's one of them...
	{
		RLock l(cs);
		for(auto cqi: downloads) {
			cqi->setErrors(0);
			if((cqi->getState() == ConnectionQueueItem::CONNECTING || cqi->getState() == ConnectionQueueItem::WAITING) && 
				cqi->getUser()->getCID() == cid)
			{
				aSource->setUser(cqi->getUser());
				// Indicate that we're interested in this file...
				aSource->setFlag(UserConnection::FLAG_DOWNLOAD);
				break;
			}
		}
	}

	if(!aSource->getUser()) {
		// Make sure we know who it is, i e that he/she is connected...

		aSource->setUser(ClientManager::getInstance()->findUser(cid));
		if(!aSource->getUser() || !aSource->getUser()->isOnline()) {
			dcdebug("CM::onMyNick Incoming connection from unknown user %s\n", nick.c_str());
			putConnection(aSource);
			return;
		}
		// We don't need this connection for downloading...make it an upload connection instead...
		aSource->setFlag(UserConnection::FLAG_UPLOAD);
	}

	ClientManager::getInstance()->setIPUser(aSource->getUser(), aSource->getRemoteIp());

	if(ClientManager::getInstance()->isOp(aSource->getUser(), aSource->getHubUrl()))
		aSource->setFlag(UserConnection::FLAG_OP);

	if( aSource->isSet(UserConnection::FLAG_INCOMING) ) {
		aSource->myNick(aSource->getToken()); 
		aSource->lock(CryptoManager::getInstance()->getLock(), CryptoManager::getInstance()->getPk());
	}

	aSource->setState(UserConnection::STATE_LOCK);
}

void ConnectionManager::on(UserConnectionListener::CLock, UserConnection* aSource, const string& aLock) noexcept {
	if(aSource->getState() != UserConnection::STATE_LOCK) {
		dcdebug("CM::onLock %p received lock twice, ignoring\n", (void*)aSource);
		return;
	}

	if( CryptoManager::getInstance()->isExtended(aLock) ) {
		StringList defFeatures = features;
		if(SETTING(COMPRESS_TRANSFERS)) {
			defFeatures.push_back(UserConnection::FEATURE_ZLIB_GET);
		}

		aSource->supports(defFeatures);
	}

	aSource->setState(UserConnection::STATE_DIRECTION);
	aSource->direction(aSource->getDirectionString(), aSource->getNumber());
	aSource->key(CryptoManager::getInstance()->makeKey(aLock));

}

void ConnectionManager::on(UserConnectionListener::Direction, UserConnection* aSource, const string& dir, const string& num) noexcept {
	if(aSource->getState() != UserConnection::STATE_DIRECTION) {
		dcdebug("CM::onDirection %p received direction twice, ignoring\n", (void*)aSource);
		return;
	}

	dcassert(aSource->isSet(UserConnection::FLAG_DOWNLOAD) ^ aSource->isSet(UserConnection::FLAG_UPLOAD));
	if(dir == "Upload") {
		// Fine, the other fellow want's to send us data...make sure we really want that...
		if(aSource->isSet(UserConnection::FLAG_UPLOAD)) {
			// Huh? Strange...disconnect...
			putConnection(aSource);
			return;
		}
	} else {
		if(aSource->isSet(UserConnection::FLAG_DOWNLOAD)) {
			int number = Util::toInt(num);
			// Damn, both want to download...the one with the highest number wins...
			if(aSource->getNumber() < number) {
				// Damn! We lost!
				aSource->unsetFlag(UserConnection::FLAG_DOWNLOAD);
				aSource->setFlag(UserConnection::FLAG_UPLOAD);
			} else if(aSource->getNumber() == number) {
				putConnection(aSource);
				return;
			}
		}
	}

	dcassert(aSource->isSet(UserConnection::FLAG_DOWNLOAD) ^ aSource->isSet(UserConnection::FLAG_UPLOAD));

	aSource->setState(UserConnection::STATE_KEY);
}

void ConnectionManager::addDownloadConnection(UserConnection* uc) {
	dcassert(uc->isSet(UserConnection::FLAG_DOWNLOAD));
	bool addConn = false;
	{
		RLock l(cs);
		auto i = uc->isSet(UserConnection::FLAG_MCN1) ? std::find(downloads.begin(), downloads.end(), uc->getToken()) : std::find(downloads.begin(), downloads.end(), uc->getUser());
		if(i != downloads.end()) {
			ConnectionQueueItem* cqi = *i;
			if(cqi->getState() == ConnectionQueueItem::WAITING || cqi->getState() == ConnectionQueueItem::CONNECTING) {
				cqi->setState(ConnectionQueueItem::ACTIVE);
				if (uc->isSet(UserConnection::FLAG_MCN1)) {
					if (cqi->isSet(ConnectionQueueItem::FLAG_SMALL)) {
						uc->setFlag(UserConnection::FLAG_SMALL_SLOT);
						cqi->setFlag(ConnectionQueueItem::FLAG_SMALL_CONF);
					} else {
						cqi->setFlag(ConnectionQueueItem::FLAG_MCN1);
					}
				}

				uc->setToken(cqi->getToken());
				uc->setHubUrl(cqi->getHubUrl()); //set the correct hint for the uc, it might not even have a hint at first.
				uc->setFlag(UserConnection::FLAG_ASSOCIATED);
				fire(ConnectionManagerListener::Connected(), cqi);
				dcdebug("ConnectionManager::addDownloadConnection, leaving to downloadmanager\n");
				addConn = true;
			}
		}
	}

	if(addConn) {
		DownloadManager::getInstance()->addConnection(uc);
	} else {
		putConnection(uc);
	}
}

void ConnectionManager::addUploadConnection(UserConnection* uc) {
	dcassert(uc->isSet(UserConnection::FLAG_UPLOAD));
	bool allowAdd = true;

	{
		WLock l(cs);
		if (!uc->isSet(UserConnection::FLAG_MCN1) && find(uploads.begin(), uploads.end(), uc->getUser()) != uploads.end()) {
			//one connection per CID for non-mcn users
			allowAdd=false;
		}

		if (allowAdd) {
			allowAdd = tokens.addToken(uc->getToken());
			if (allowAdd) {
				uc->setFlag(UserConnection::FLAG_ASSOCIATED);
				ConnectionQueueItem* cqi = getCQI(uc->getHintedUser(), false, uc->getToken());
				uc->setToken(cqi->getToken()); //sync if the uc token was empty
				cqi->setState(ConnectionQueueItem::ACTIVE);
				//LogManager::getInstance()->message("Token1 CQI: " + cqi->getToken());
				fire(ConnectionManagerListener::Connected(), cqi);
			}
		}
	}

	if (!allowAdd) {
		putConnection(uc);
		return;
	}

	dcdebug("ConnectionManager::addUploadConnection, leaving to uploadmanager\n");
	//LogManager::getInstance()->message("Token1 UC: " + uc->getToken());

	UploadManager::getInstance()->addConnection(uc);
}

void ConnectionManager::on(UserConnectionListener::Key, UserConnection* aSource, const string&/* aKey*/) noexcept {
	if(aSource->getState() != UserConnection::STATE_KEY) {
		dcdebug("CM::onKey Bad state, ignoring");
		return;
	}

	aSource->setToken(Util::toString(Util::rand()));
	dcassert(aSource->getUser());

	if(aSource->isSet(UserConnection::FLAG_DOWNLOAD)) {
		addDownloadConnection(aSource);
	} else {
		addUploadConnection(aSource);
	}
}

void ConnectionManager::on(AdcCommand::INF, UserConnection* aSource, const AdcCommand& cmd) noexcept {
	if(aSource->getState() != UserConnection::STATE_INF) {
		aSource->send(AdcCommand(AdcCommand::SEV_FATAL, AdcCommand::ERROR_PROTOCOL_GENERIC, "Expecting INF"));
		aSource->disconnect();
		return;
	}

	string cid;
	if(!cmd.getParam("ID", 0, cid)) {
		aSource->send(AdcCommand(AdcCommand::SEV_FATAL, AdcCommand::ERROR_INF_MISSING, "ID missing").addParam("FL", "ID"));
		dcdebug("CM::onINF missing ID\n");
		aSource->disconnect();
		return;
	}
	UserPtr user = ClientManager::getInstance()->findUser(CID(cid));
	aSource->setUser(user);

	if(!aSource->getUser()) {
		dcdebug("CM::onINF: User not found");
		aSource->send(AdcCommand(AdcCommand::SEV_FATAL, AdcCommand::ERROR_GENERIC, "User not found"));
		putConnection(aSource);
		return;
	}

	if(!checkKeyprint(aSource)) {
		putConnection(aSource);
		return;
	}

	string token;
	if(aSource->isSet(UserConnection::FLAG_INCOMING)) {
		if(!cmd.getParam("TO", 0, token)) {
			aSource->send(AdcCommand(AdcCommand::SEV_FATAL, AdcCommand::ERROR_GENERIC, "TO missing"));
			putConnection(aSource);
			return;
		}

		// Try to find out where this came from...
		// For downloads we do know it from cqi (we made the connection after all :P ), so basicly this would be needed only for passive uploads.
		auto i = expectedConnections.remove(token);
		if(i.second.empty()) {
			//we dont want connections without hubhint laying around..
			aSource->send(AdcCommand(AdcCommand::SEV_FATAL, AdcCommand::ERROR_GENERIC, "Connection not expected"));
			putConnection(aSource);
			return;
		} else {
			aSource->setHubUrl(i.second);
		}
	} else {
		token = aSource->getToken();
	}

	dcassert(!token.empty());
	{
		RLock l(cs);
		auto i = find(downloads.begin(), downloads.end(), token);
		if (i != downloads.end()) {
			ConnectionQueueItem* cqi = *i;
			if(aSource->isSet(UserConnection::FLAG_MCN1)) {
				string slots;
				if (cmd.getParam("CO", 0, slots)) {
					cqi->setMaxConns(Util::toInt(slots));
				}
			}
			cqi->setErrors(0);
			aSource->setFlag(UserConnection::FLAG_DOWNLOAD);
		} else if (delayedTokens.find(token) == delayedTokens.end()) {
			aSource->setFlag(UserConnection::FLAG_UPLOAD);
		}
	}

	aSource->setToken(token);
	if(aSource->isSet(UserConnection::FLAG_DOWNLOAD)) {
		addDownloadConnection(aSource);
	} else if(aSource->isSet(UserConnection::FLAG_UPLOAD)) {
		addUploadConnection(aSource);
	} else {
		putConnection(aSource);
	}
}

void ConnectionManager::force(const UserPtr& aUser) {
	RLock l(cs);
	auto i = find(downloads.begin(), downloads.end(), aUser);
	if(i != downloads.end()) {
		(*i)->setLastAttempt(0);
	}
}

void ConnectionManager::force(const string& aToken) {
	RLock l(cs);
	auto i = find(downloads.begin(), downloads.end(), aToken);
	if (i != downloads.end())
		(*i)->setLastAttempt(0);
}

bool ConnectionManager::checkKeyprint(UserConnection *aSource) {
	dcassert(aSource->getUser());

	auto kp = aSource->getKeyprint();
	if(kp.empty()) {
		return true;
	}

	auto kp2 = ClientManager::getInstance()->getField(aSource->getUser()->getCID(), aSource->getHubUrl(), "KP");
	if(kp2.empty()) {
		// TODO false probably
		return true;
	}

	if(kp2.compare(0, 7, "SHA256/") != 0) {
		// Unsupported hash
		return true;
	}

	//dcdebug("Keyprint: %s vs %s\n", Encoder::toBase32(&kp[0], kp.size()).c_str(), kp2.c_str() + 7);

	vector<uint8_t> kp2v(kp.size());
	Encoder::fromBase32(&kp2[7], &kp2v[0], kp2v.size());
	if(!std::equal(kp.begin(), kp.end(), kp2v.begin())) {
		dcdebug("Keyprint Not equal...\n");
		return false;
	}

	return true;
}

void ConnectionManager::failed(UserConnection* aSource, const string& aError, bool protocolError) {
	if(aSource->isSet(UserConnection::FLAG_ASSOCIATED)) {
		if(aSource->isSet(UserConnection::FLAG_DOWNLOAD)) {
			//this may flag other user connections as removed which would possibly cause threading issues
			WLock l (cs);
			auto i = find(downloads.begin(), downloads.end(), aSource->getToken());
			dcassert(i != downloads.end());
			if (i != downloads.end()) {
				ConnectionQueueItem* cqi = *i;

				if (cqi->isSet(ConnectionQueueItem::FLAG_MCN1) && !cqi->isSet(ConnectionQueueItem::FLAG_REMOVE)) {
					//remove an existing waiting item, if exists
					auto s = find_if(downloads.begin(), downloads.end(), [&](ConnectionQueueItem* c) { 
						return c->getUser() == aSource->getUser() && !c->isSet(ConnectionQueueItem::FLAG_SMALL_CONF) && 
							c->getState() != ConnectionQueueItem::RUNNING && c != cqi && !c->isSet(ConnectionQueueItem::FLAG_REMOVE);
					});

					if (s != downloads.end())
						(*s)->setFlag(ConnectionQueueItem::FLAG_REMOVE);
				}

				cqi->setLastAttempt(GET_TICK());
				cqi->setErrors(protocolError ? -1 : (cqi->getErrors() + 1));

				cqi->setState(ConnectionQueueItem::WAITING);
				fire(ConnectionManagerListener::Failed(), cqi, aError);
			}
		} else if(aSource->isSet(UserConnection::FLAG_UPLOAD)) {
			{
				WLock l (cs);
				auto i = find(uploads.begin(), uploads.end(), aSource->getToken());
				dcassert(i != uploads.end());
				ConnectionQueueItem* cqi = *i;
				putCQI(cqi);
			}
			UploadManager::getInstance()->removeDelayUpload(*aSource);
		}
	}
	putConnection(aSource);
}

void ConnectionManager::on(UserConnectionListener::Failed, UserConnection* aSource, const string& aError) noexcept {
	failed(aSource, aError, false);
}

void ConnectionManager::on(UserConnectionListener::ProtocolError, UserConnection* aSource, const string& aError) noexcept {
	failed(aSource, aError, true);
}

void ConnectionManager::disconnect(const UserPtr& aUser) {
	RLock l(cs);
	for(auto uc: userConnections) {
		if(uc->getUser() == aUser)
			uc->disconnect(true);
	}
}

void ConnectionManager::disconnect(const string& token) {
	RLock l(cs);
	auto s = find_if(userConnections.begin(), userConnections.end(), [&](UserConnection* uc) { return compare(uc->getToken(), token) == 0; });
	if (s != userConnections.end())
		(*s)->disconnect(true);
}

void ConnectionManager::disconnect(const UserPtr& aUser, int isDownload) {
	RLock l(cs);
	for(auto uc: userConnections) {
		if(uc->getUser() == aUser && uc->isSet((Flags::MaskType)(isDownload ? UserConnection::FLAG_DOWNLOAD : UserConnection::FLAG_UPLOAD))) {
			uc->disconnect(true);
		}
	}
}

bool ConnectionManager::setBundle(const string& token, const string& bundleToken) {
	RLock l (cs);
	auto s = find_if(userConnections.begin(), userConnections.end(), [&](UserConnection* uc) { return compare(uc->getToken(), token) == 0; });
	if (s != userConnections.end()) {
		(*s)->setLastBundle(bundleToken);
		return true;
	}
	return false;
}

void ConnectionManager::shutdown() {
	TimerManager::getInstance()->removeListener(this);
	ClientManager::getInstance()->removeListener(this);
	shuttingDown = true;
	disconnect();
	{
		RLock l(cs);
		for(auto uc: userConnections) {
			uc->disconnect(true);
		}
	}
	// Wait until all connections have died out...
	while(true) {
		{
			RLock l(cs);
			if(userConnections.empty()) {
				break;
			}
		}
		Thread::sleep(50);
	}
}		

// UserConnectionListener
void ConnectionManager::on(UserConnectionListener::Supports, UserConnection* conn, const StringList& feat) noexcept {
	string sup = Util::emptyString;
	for(auto& i: feat) {
		sup += i + " ";
		if(i == UserConnection::FEATURE_MINISLOTS) {
			conn->setFlag(UserConnection::FLAG_SUPPORTS_MINISLOTS);
		} else if(i == UserConnection::FEATURE_XML_BZLIST) {
			conn->setFlag(UserConnection::FLAG_SUPPORTS_XML_BZLIST);
		} else if(i == UserConnection::FEATURE_ADCGET) {
			conn->setFlag(UserConnection::FLAG_SUPPORTS_ADCGET);
		} else if(i == UserConnection::FEATURE_ZLIB_GET) {
			conn->setFlag(UserConnection::FLAG_SUPPORTS_ZLIB_GET);
		} else if(i == UserConnection::FEATURE_TTHL) {
			conn->setFlag(UserConnection::FLAG_SUPPORTS_TTHL);
		} else if(i == UserConnection::FEATURE_TTHF) {
			conn->setFlag(UserConnection::FLAG_SUPPORTS_TTHF);
		} else if(i == UserConnection::FEATURE_AIRDC) {
			if(!conn->getUser()->isSet(User::AIRDCPLUSPLUS))
				conn->getUser()->setFlag(User::AIRDCPLUSPLUS);
		}
	}
}

} // namespace dcpp