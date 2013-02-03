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
#include "ConnectivityManager.h"

#include "ClientManager.h"
#include "ConnectionManager.h"
#include "format.h"
#include "MappingManager.h"
#include "SearchManager.h"
#include "SettingsManager.h"
#include "version.h"
#include "AirUtil.h"

#include "ResourceManager.h"

namespace dcpp {

ConnectivityManager::ConnectivityManager() :
autoDetectedV4(false),
autoDetectedV6(false),
runningV4(false),
runningV6(false),
mapperV6(true),
mapperV4(false)
{
}

bool ConnectivityManager::get(SettingsManager::BoolSetting setting) const {
	if(SETTING(AUTO_DETECT_CONNECTION) || SETTING(AUTO_DETECT_CONNECTION6)) {
		auto i = autoSettings.find(setting);
		if(i != autoSettings.end()) {
			return boost::get<bool>(i->second);
		}
	}
	return SettingsManager::getInstance()->get(setting);
}

int ConnectivityManager::get(SettingsManager::IntSetting setting) const {
	if(SETTING(AUTO_DETECT_CONNECTION) || SETTING(AUTO_DETECT_CONNECTION6)) {
		auto i = autoSettings.find(setting);
		if(i != autoSettings.end()) {
			return boost::get<int>(i->second);
		}
	}
	return SettingsManager::getInstance()->get(setting);
}

const string& ConnectivityManager::get(SettingsManager::StrSetting setting) const {
	if(SETTING(AUTO_DETECT_CONNECTION) || SETTING(AUTO_DETECT_CONNECTION6)) {
		auto i = autoSettings.find(setting);
		if(i != autoSettings.end()) {
			return boost::get<const string&>(i->second);
		}
	}
	return SettingsManager::getInstance()->get(setting);
}

void ConnectivityManager::set(SettingsManager::StrSetting setting, const string& str) {
	if(SETTING(AUTO_DETECT_CONNECTION) || SETTING(AUTO_DETECT_CONNECTION6)) {
		autoSettings[setting] = str;
	} else {
		SettingsManager::getInstance()->set(setting, str);
	}
}

void ConnectivityManager::detectConnection() {
	if(isRunning())
		return;

	runningV4 = SETTING(AUTO_DETECT_CONNECTION);
	runningV6 = SETTING(AUTO_DETECT_CONNECTION6);

	statusV4.clear();
	statusV6.clear();
	fire(ConnectivityManagerListener::Started());

	if(mapperV4.getOpened()) {
		mapperV4.close();
	}

	if(mapperV6.getOpened()) {
		mapperV6.close();
	}

	disconnect();

	// restore auto settings to their default value.
	int settings[] = { SettingsManager::TCP_PORT, SettingsManager::TLS_PORT, SettingsManager::UDP_PORT,
		SettingsManager::EXTERNAL_IP, SettingsManager::EXTERNAL_IP6, SettingsManager::NO_IP_OVERRIDE,
		SettingsManager::BIND_ADDRESS, SettingsManager::BIND_ADDRESS6,
		SettingsManager::INCOMING_CONNECTIONS, SettingsManager::INCOMING_CONNECTIONS6, 
		SettingsManager::OUTGOING_CONNECTIONS };

	for(const auto setting: settings) {
		if(setting >= SettingsManager::STR_FIRST && setting < SettingsManager::STR_LAST) {
			autoSettings[setting] = SettingsManager::getInstance()->getDefault(static_cast<SettingsManager::StrSetting>(setting));
		} else if(setting >= SettingsManager::INT_FIRST && setting < SettingsManager::INT_LAST) {
			autoSettings[setting] = SettingsManager::getInstance()->getDefault(static_cast<SettingsManager::IntSetting>(setting));
		} else if(setting >= SettingsManager::BOOL_FIRST && setting < SettingsManager::BOOL_LAST) {
			autoSettings[setting] = SettingsManager::getInstance()->getDefault(static_cast<SettingsManager::BoolSetting>(setting));
		} else {
			dcassert(0);
		}
	}

	log(STRING(CONN_DETERMINING), LogManager::LOG_INFO, TYPE_BOTH);

	/*if (runningV4)
		log(STRING(CONN_DETERMINING), LogManager::LOG_INFO, false);
	if (runningV6)
		log(STRING(CONN_DETERMINING), LogManager::LOG_INFO, true);*/

	try {
		listen();
	} catch(const Exception& e) {
		autoSettings[SettingsManager::INCOMING_CONNECTIONS] = SettingsManager::INCOMING_PASSIVE;
		autoSettings[SettingsManager::INCOMING_CONNECTIONS6] = SettingsManager::INCOMING_PASSIVE;
		log(STRING_F(CONN_PORT_X_FAILED, e.getError()), LogManager::LOG_ERROR, TYPE_NORMAL);
		fire(ConnectivityManagerListener::Finished());
		runningV4 = false;
		runningV6 = false;
		return;
	}

	autoDetectedV4 = runningV4;
	autoDetectedV6 = runningV6;

	if(runningV4 && !AirUtil::getLocalIp(false, false).empty()) {
		autoSettings[SettingsManager::INCOMING_CONNECTIONS] = SettingsManager::INCOMING_ACTIVE;
		log(STRING(CONN_DIRECT_DETECTED), LogManager::LOG_INFO, TYPE_V4);
		fire(ConnectivityManagerListener::Finished());
		runningV4 = false;
	}

	if(runningV6 && !AirUtil::getLocalIp(true, false).empty()) {
		autoSettings[SettingsManager::INCOMING_CONNECTIONS6] = SettingsManager::INCOMING_ACTIVE;
		log(STRING(CONN_DIRECT_DETECTED), LogManager::LOG_INFO, TYPE_V6);
		fire(ConnectivityManagerListener::Finished());
		runningV6 = false;
	}

	autoSettings[SettingsManager::INCOMING_CONNECTIONS] = SettingsManager::INCOMING_ACTIVE_UPNP;

	log(STRING(CONN_NAT_DETECTED), LogManager::LOG_INFO, TYPE_BOTH);

	if (runningV6)
		startMapping(true);
	if (runningV4)
		startMapping(false);
}

void ConnectivityManager::setup(bool settingsChanged) {
	if(SETTING(AUTO_DETECT_CONNECTION) || SETTING(AUTO_DETECT_CONNECTION6)) {
		if(!autoDetectedV6 || !autoDetectedV4) {
			detectConnection();
		}
	} else {
		if(autoDetectedV6 || autoDetectedV4) {
			autoSettings.clear();
		}
		if(autoDetectedV6 || autoDetectedV4 || settingsChanged) {
			if(settingsChanged || (SETTING(INCOMING_CONNECTIONS) != SettingsManager::INCOMING_ACTIVE_UPNP)) {
				mapperV4.close();
			}
			if(settingsChanged || (SETTING(INCOMING_CONNECTIONS6) != SettingsManager::INCOMING_ACTIVE_UPNP)) {
				mapperV6.close();
			}
			startSocket();
		} else {
			// previous mappings had failed; try again
			startMapping();
		}
	}
}

void ConnectivityManager::close() {
	mapperV4.close();
	mapperV6.close();
}

void ConnectivityManager::editAutoSettings() {
	SettingsManager::getInstance()->set(SettingsManager::AUTO_DETECT_CONNECTION, false);

	auto sm = SettingsManager::getInstance();
	for(auto i = autoSettings.cbegin(), iend = autoSettings.cend(); i != iend; ++i) {
		if(i->first >= SettingsManager::STR_FIRST && i->first < SettingsManager::STR_LAST) {
			sm->set(static_cast<SettingsManager::StrSetting>(i->first), boost::get<const string&>(i->second));
		} else if(i->first >= SettingsManager::INT_FIRST && i->first < SettingsManager::INT_LAST) {
			sm->set(static_cast<SettingsManager::IntSetting>(i->first), boost::get<int>(i->second));
		}
	}
	autoSettings.clear();

	fire(ConnectivityManagerListener::SettingChanged());
}

string ConnectivityManager::getInformation() const {
	if(isRunning()) {
		return "Connectivity settings are being configured; try again later";
	}

	string autoStatusV4 = ok(false) ? str(boost::format("enabled - %1%") % getStatus(false)) : "disabled";
	string autoStatusV6 = ok(true) ? str(boost::format("enabled - %1%") % getStatus(true)) : "disabled";

	auto getMode = [&](bool v6) -> string { 
		switch(CONNSETTING(INCOMING_CONNECTIONS)) {
		case SettingsManager::INCOMING_ACTIVE:
			{
				return "Direct connection to the Internet (no router or manual router configuration)";
				break;
			}
		case SettingsManager::INCOMING_ACTIVE_UPNP:
			{
				return str(boost::format("Active mode behind a router that %1% can configure; port mapping status: %2%") % APPNAME % (v6 ? mapperV6.getStatus() : mapperV4.getStatus()));
				break;
			}
		case SettingsManager::INCOMING_PASSIVE:
			{
				return "Passive mode";
				break;
			}
		default:
			return "Disabled";
		}
	};

	auto field = [](const string& s) { return s.empty() ? "undefined" : s; };

	return str(boost::format(
		"Connectivity information:\n\n"
		"Automatic connectivity setup (v4) is: %1%\n\n"
		"Automatic connectivity setup (v6) is: %2%\n\n"
		"\tMode (v4): %3%\n"
		"\tMode (v6): %4%\n"
		"\tExternal IP (v4): %5%\n"
		"\tExternal IP (v6): %6%\n"
		"\tBound interface (v4): %7%\n"
		"\tBound interface (v6): %8%\n"
		"\tTransfer port: %9%\n"
		"\tEncrypted transfer port: %10%\n"
		"\tSearch port: %11%") % autoStatusV4 % autoStatusV6 % getMode(false) % getMode(true) %
		field(CONNSETTING(EXTERNAL_IP)) % field(CONNSETTING(EXTERNAL_IP6)) %
		field(CONNSETTING(BIND_ADDRESS)) % field(CONNSETTING(BIND_ADDRESS6)) %
		field(ConnectionManager::getInstance()->getPort()) % field(ConnectionManager::getInstance()->getSecurePort()) %
		field(SearchManager::getInstance()->getPort()));
}

void ConnectivityManager::startMapping(bool v6) {
	if (v6) {
		runningV6 = true;
		if(!mapperV6.open()) {
			runningV6 = false;
		}
	} else {
		runningV4 = true;
		if(!mapperV4.open()) {
			runningV4 = false;
		}
	}
}

void ConnectivityManager::mappingFinished(const string& mapper, bool v6) {
	if(SETTING(AUTO_DETECT_CONNECTION)) {
		if(mapper.empty()) {
			disconnect();
			autoSettings[SettingsManager::INCOMING_CONNECTIONS] = SettingsManager::INCOMING_PASSIVE;
			log(STRING(CONN_ACTIVE_FAILED), LogManager::LOG_WARNING, v6 ? TYPE_V6 : TYPE_V4);
		} else {
			SettingsManager::getInstance()->set(SettingsManager::MAPPER, mapper);
		}
		fire(ConnectivityManagerListener::Finished());
	}

	if (v6)
		runningV6 = false;
	else
		runningV4 = false;
}

void ConnectivityManager::log(const string& message, LogManager::Severity sev, LogType aType) {
	if (aType == TYPE_NORMAL) {
		LogManager::getInstance()->message(message, sev);
	} else {
		string proto;
		//auto addTime = [this, &nextSearch] (bool toEnabled) -> void {
		//auto getMessage = [&](const string& proto) { return STRING_F(CONNECTIVITY_X, "IPv4 & IPv6") + ": " + message; }

		if (aType == TYPE_BOTH && runningV4 && runningV6) {
			statusV6 = message;
			statusV4 = message;
			proto = "IPv4 & IPv6";
		} else if (aType == TYPE_V4 || (aType == TYPE_BOTH && runningV4)) {
			proto = "IPv4";
			statusV4 = message;
		} else if (aType == TYPE_V6 || (aType == TYPE_BOTH && runningV6)) {
			proto = "IPv6";
			statusV6 = message;
		}

		LogManager::getInstance()->message(STRING(CONNECTIVITY) + "(" + proto + "): " + message, sev);
		fire(ConnectivityManagerListener::Message(), message);
	}

	/*if((SETTING(AUTO_DETECT_CONNECTION) && !v6) || (SETTING(AUTO_DETECT_CONNECTION6) && v6)) {
		status = move(message);
		LogManager::getInstance()->message(STRING(CONNECTIVITY) + ": " + status, sev);
		fire(ConnectivityManagerListener::Message(), status);
	} else {
		LogManager::getInstance()->message(message, sev);
	}*/
}

const string& ConnectivityManager::getStatus(bool v6) const { 
	return v6 ? statusV6 : statusV4; 
}

StringList ConnectivityManager::getMappers(bool v6) const {
	if (v6) {
		return mapperV6.getMappers();
	} else {
		return mapperV4.getMappers();
	}
}

void ConnectivityManager::startSocket() {
	autoDetectedV4 = false;
	autoDetectedV6 = false;

	disconnect();

	if(ClientManager::getInstance()->isActive()) {
		listen();

		// must be done after listen calls; otherwise ports won't be set
		startMapping();
	}
}

void ConnectivityManager::startMapping() {
	if(SETTING(INCOMING_CONNECTIONS) == SettingsManager::INCOMING_ACTIVE_UPNP && !runningV4)
		startMapping(false);

	if(SETTING(INCOMING_CONNECTIONS6) == SettingsManager::INCOMING_ACTIVE_UPNP && !runningV6)
		startMapping(true);
}

void ConnectivityManager::listen() {
	try {
		ConnectionManager::getInstance()->listen();
	} catch(const Exception&) {
		throw Exception(STRING(TRANSFER_PORT));
	}

	try {
		SearchManager::getInstance()->listen();
	} catch(const Exception&) {
		throw Exception(STRING(SEARCH_PORT));
	}
}

void ConnectivityManager::disconnect() {
	SearchManager::getInstance()->disconnect();
	ConnectionManager::getInstance()->disconnect();
}

} // namespace dcpp
