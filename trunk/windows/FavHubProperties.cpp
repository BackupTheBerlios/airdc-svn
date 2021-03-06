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

#include "stdafx.h"
#include "Resource.h"
#include "WinUtil.h"
#include "MainFrm.h"

#include "FavHubProperties.h"
#include "PropertiesDlg.h"

#include "../client/FavoriteManager.h"
#include "../client/ResourceManager.h"
#include "../client/tribool.h"
#include "../client/ShareManager.h"


FavHubProperties::FavHubProperties(FavoriteHubEntry *_entry) : entry(_entry), loaded(false) { }

LRESULT FavHubProperties::OnInitDialog(UINT, WPARAM, LPARAM, BOOL&)
{
	// Translate dialog
	SetWindowText(CTSTRING(FAVORITE_HUB_PROPERTIES));
	SetDlgItemText(IDCANCEL, CTSTRING(CANCEL));
	SetDlgItemText(IDC_FH_HUB, CTSTRING(HUB));
	SetDlgItemText(IDC_FH_IDENT, CTSTRING(FAVORITE_HUB_IDENTITY));
	SetDlgItemText(IDC_FH_NAME, CTSTRING(HUB_NAME));
	SetDlgItemText(IDC_FH_ADDRESS, CTSTRING(HUB_ADDRESS));
	SetDlgItemText(IDC_FH_HUB_DESC, CTSTRING(DESCRIPTION));
	SetDlgItemText(IDC_FH_NICK, CTSTRING(NICK));
	SetDlgItemText(IDC_FH_PASSWORD, CTSTRING(PASSWORD));
	SetDlgItemText(IDC_FH_USER_DESC, CTSTRING(DESCRIPTION));
	SetDlgItemText(IDC_FH_CONN, CTSTRING(FAVORITE_HUB_CONNECTION));
	SetDlgItemText(IDC_STEALTH, CTSTRING(STEALTH_MODE));
	SetDlgItemText(IDC_FAV_NO_PM, CTSTRING(FAV_NO_PM));
	SetDlgItemText(IDC_SHOW_JOIN, CTSTRING(FAV_SHOW_JOIN));
	SetDlgItemText(IDC_HIDE_SHARE, CTSTRING(HIDE_SHARE));
	SetDlgItemText(IDC_FAV_SEARCH_INTERVAL, CTSTRING(MINIMUM_SEARCH_INTERVAL));
	SetDlgItemText(IDC_FAVGROUP, CTSTRING(GROUP));
	SetDlgItemText(IDC_LOGMAINCHAT, CTSTRING(FAV_LOG_CHAT));
	SetDlgItemText(IDC_CHAT_NOTIFY, CTSTRING(CHAT_NOTIFY));
	SetDlgItemText(IDC_FAILOVER, CTSTRING(ACCEPT_FAILOVERS_FAV));
	SetDlgItemText(IDC_AWAY_MSG_LBL, CTSTRING(CUSTOM_AWAY_MESSAGE));

	SetDlgItemText(IDC_LOGMAINCHAT, CTSTRING(FAV_LOG_CHAT));
	SetDlgItemText(IDC_HUBSETTINGS, CTSTRING(GLOBAL_SETTING_OVERRIDES));
	SetDlgItemText(IDC_SEARCH_INTERVAL_DEFAULT, CTSTRING(USE_DEFAULT));

	SetDlgItemText(IDC_FAV_SHAREPROFILE_CAPTION, CTSTRING(SHARE_PROFILE));
	SetDlgItemText(IDC_EDIT_PROFILES, CTSTRING(EDIT_PROFILES));
	SetDlgItemText(IDC_PROFILES_NOTE, CTSTRING(PROFILES_NOTE));

	// Fill in values
	SetDlgItemText(IDC_HUBNAME, Text::toT(entry->getName()).c_str());
	SetDlgItemText(IDC_HUBDESCR, Text::toT(entry->getDescription()).c_str());
	SetDlgItemText(IDC_HUBADDR, Text::toT(entry->getServerStr()).c_str());
	SetDlgItemText(IDC_NICK, Text::toT(entry->get(HubSettings::Nick)).c_str());
	SetDlgItemText(IDC_HUBPASS, Text::toT(entry->getPassword()).c_str());
	SetDlgItemText(IDC_USERDESC, Text::toT(entry->get(HubSettings::Description)).c_str());
	SetDlgItemText(IDC_EMAIL, Text::toT(entry->get(HubSettings::Email)).c_str());

	SetDlgItemText(IDC_AWAY_MSG, Text::toT(entry->get(HubSettings::AwayMsg)).c_str());

	CheckDlgButton(IDC_STEALTH, entry->getStealth() ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_FAV_NO_PM, entry->getFavNoPM() ? BST_CHECKED : BST_UNCHECKED);

	CheckDlgButton(IDC_SHOW_JOIN, toInt(entry->get(HubSettings::ShowJoins)));
	CheckDlgButton(IDC_SHOW_JOIN_FAV, toInt(entry->get(HubSettings::FavShowJoins)));
	CheckDlgButton(IDC_LOGMAINCHAT, toInt(entry->get(HubSettings::LogMainChat)));
	CheckDlgButton(IDC_CHAT_NOTIFY, toInt(entry->get(HubSettings::ChatNotify)));
	CheckDlgButton(IDC_FAILOVER, toInt(entry->get(HubSettings::AcceptFailovers)));

	CheckDlgButton(IDC_FAV_NO_PM, entry->getFavNoPM() ? BST_CHECKED : BST_UNCHECKED);

	auto searchInterval = entry->get(HubSettings::SearchInterval);
	CheckDlgButton(IDC_SEARCH_INTERVAL_DEFAULT, searchInterval == HubSettings::getMinInt() ? BST_CHECKED : BST_UNCHECKED);
	SetDlgItemText(IDC_FAV_SEARCH_INTERVAL_BOX, Util::toStringW(searchInterval).c_str());

	bool isAdcHub = entry->isAdcHub();

	CComboBox combo;
	combo.Attach(GetDlgItem(IDC_FAVGROUP_BOX));
	combo.AddString(_T("---"));
	combo.SetCurSel(0);

	const FavHubGroups& favHubGroups = FavoriteManager::getInstance()->getFavHubGroups();
	for(const auto& name: favHubGroups | map_keys) {
		int pos = combo.AddString(Text::toT(name).c_str());
		
		if(name == entry->getGroup())
			combo.SetCurSel(pos);
	}

	combo.Detach();

	// TODO: add more encoding into wxWidgets version, this is enough now
	// FIXME: following names are Windows only!
	combo.Attach(GetDlgItem(IDC_ENCODING));
	combo.AddString(_T("System default"));
	combo.AddString(_T("English_United Kingdom.1252"));
	combo.AddString(_T("Czech_Czech Republic.1250"));
	combo.AddString(_T("Russian_Russia.1251"));
	combo.AddString(Text::toT(Text::utf8).c_str());

	ctrlProfile.Attach(GetDlgItem(IDC_FAV_SHAREPROFILE));
	appendProfiles();
	hideShare = entry->getShareProfile() && entry->getShareProfile()->getToken() == SP_HIDDEN;
	CheckDlgButton(IDC_HIDE_SHARE, hideShare ? BST_CHECKED : BST_UNCHECKED);


	if(isAdcHub) {
		combo.SetCurSel(4); // select UTF-8 for ADC hubs
		combo.EnableWindow(false);
		if (hideShare)
			ctrlProfile.EnableWindow(false);
	} else {
		ctrlProfile.EnableWindow(false);
		if(entry->getEncoding().empty()) {
			combo.SetCurSel(0);
		} else {
			combo.SetWindowText(Text::toT(entry->getEncoding()).c_str());
		}
	}

	combo.Detach();

	// connection modes
	auto appendCombo = [](CComboBox& combo, int curMode) {
		combo.InsertString(0, CTSTRING(DEFAULT));
		combo.InsertString(1, CTSTRING(DISABLED));
		combo.InsertString(2, CTSTRING(ACTIVE_MODE));
		combo.InsertString(3, CTSTRING(PASSIVE_MODE));

		if(curMode == HubSettings::getMinInt())
			combo.SetCurSel(0);
		else if(curMode == SettingsManager::INCOMING_DISABLED)
			combo.SetCurSel(1);
		else if(curMode == SettingsManager::INCOMING_ACTIVE)
			combo.SetCurSel(2);
		else if(curMode == SettingsManager::INCOMING_PASSIVE)
			combo.SetCurSel(3);
	};

	modeCombo4.Attach(GetDlgItem(IDC_MODE4));
	modeCombo6.Attach(GetDlgItem(IDC_MODE6));

	appendCombo(modeCombo4, entry->get(HubSettings::Connection));
	appendCombo(modeCombo6, entry->get(HubSettings::Connection6));

	//external ips
	SetDlgItemText(IDC_SERVER4, Text::toT(entry->get(HubSettings::UserIp)).c_str());
	SetDlgItemText(IDC_SERVER6, Text::toT(entry->get(HubSettings::UserIp6)).c_str());

	fixControls();

	CEdit tmp;
	tmp.Attach(GetDlgItem(IDC_HUBNAME));
	tmp.SetFocus();
	tmp.SetSel(0,-1);
	tmp.Detach();
	tmp.Attach(GetDlgItem(IDC_NICK));
	tmp.LimitText(35);
	tmp.Detach();
	tmp.Attach(GetDlgItem(IDC_USERDESC));
	tmp.LimitText(50);
	tmp.Detach();
	tmp.Attach(GetDlgItem(IDC_EMAIL));
	tmp.LimitText(50);
	tmp.Detach();
	tmp.Attach(GetDlgItem(IDC_HUBPASS));
	tmp.SetPasswordChar('*');
	tmp.Detach();
	
	CUpDownCtrl updown;
	updown.Attach(GetDlgItem(IDC_FAV_SEARCH_INTERVAL_SPIN));
	updown.SetRange32(5, 9999);
	updown.Detach();

	CenterWindow(GetParent());
	loaded = true;
	return FALSE;
}

LRESULT FavHubProperties::onClickedHideShare(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if (IsDlgButtonChecked(IDC_HIDE_SHARE)) {
		ctrlProfile.EnableWindow(false);
		hideShare = true;
	} else {
		hideShare = false;
		TCHAR buf[512];
		GetDlgItemText(IDC_HUBADDR, buf, 256);
		if (AirUtil::isAdcHub(Text::fromT(buf)))
			ctrlProfile.EnableWindow(true);
	}
	return FALSE;
}

LRESULT FavHubProperties::fixControls(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	fixControls();
	return FALSE;
}

void FavHubProperties::fixControls() {
	auto usingDefaultInterval = IsDlgButtonChecked(IDC_SEARCH_INTERVAL_DEFAULT);
	::EnableWindow(GetDlgItem(IDC_FAV_SEARCH_INTERVAL_BOX),			!usingDefaultInterval);
	::EnableWindow(GetDlgItem(IDC_FAV_SEARCH_INTERVAL_SPIN),		!usingDefaultInterval);
	if (usingDefaultInterval)
		SetDlgItemText(IDC_FAV_SEARCH_INTERVAL_BOX, Util::toStringW(SettingsManager::getInstance()->get(SettingsManager::MINIMUM_SEARCH_INTERVAL)).c_str());
}

void FavHubProperties::appendProfiles() {
	auto profiles = ShareManager::getInstance()->getProfiles();

	int counter = 0;
	for(auto j = profiles.begin(); j != profiles.end()-1; j++) {
		ctrlProfile.InsertString(counter, Text::toT((*j)->getDisplayName()).c_str());
		if (counter == 0 || *j == entry->getShareProfile())
			ctrlProfile.SetCurSel(counter);
		counter++;
	}
}

LRESULT FavHubProperties::OnEditProfiles(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	MainFrame::getMainFrame()->openSettings(PropertiesDlg::PAGE_SHARING);
	while(ctrlProfile.GetCount()) {
		ctrlProfile.DeleteString(0);
	}
	appendProfiles();
	return FALSE;
}

LRESULT FavHubProperties::OnCloseCmd(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
	if(wID == IDOK)
	{
		TCHAR buf[1024];
		GetDlgItemText(IDC_HUBADDR, buf, 256);
		if(buf[0] == '\0') {
			WinUtil::showMessageBox(TSTRING(INCOMPLETE_FAV_HUB), MB_ICONWARNING);
			return 0;
		}

		//check the primary address for dupes
		string addresses = Text::fromT(buf);
		size_t pos = addresses.find(";");

		if (!FavoriteManager::getInstance()->isUnique(pos != string::npos ? addresses.substr(0, pos) : addresses, entry->getToken())){
			WinUtil::showMessageBox(TSTRING(FAVORITE_HUB_ALREADY_EXISTS), MB_ICONWARNING);
			return 0;
		}

		//validate the encoding
		GetDlgItemText(IDC_ENCODING, buf, 512);
		if(_tcschr(buf, _T('.')) == NULL && _tcscmp(buf, Text::toT(Text::utf8).c_str()) != 0 && _tcscmp(buf, _T("System default")) != 0)
		{
			WinUtil::showMessageBox(TSTRING(INVALID_ENCODING), MB_ICONWARNING);
			return 0;
		}

		//set the values
		entry->setEncoding(Text::fromT(buf));
		entry->setServerStr(addresses);

		GetDlgItemText(IDC_HUBNAME, buf, 256);
		entry->setName(Text::fromT(buf));
		GetDlgItemText(IDC_HUBDESCR, buf, 256);
		entry->setDescription(Text::fromT(buf));
		GetDlgItemText(IDC_HUBPASS, buf, 256);
		entry->setPassword(Text::fromT(buf));
		entry->setStealth(IsDlgButtonChecked(IDC_STEALTH) == 1);
		entry->setFavNoPM(IsDlgButtonChecked(IDC_FAV_NO_PM) == 1);

		//Hub settings
		GetDlgItemText(IDC_NICK, buf, 256);
		entry->get(HubSettings::Nick) = Text::fromT(buf);

		GetDlgItemText(IDC_USERDESC, buf, 256);
		entry->get(HubSettings::Description) = Text::fromT(buf);

		GetDlgItemText(IDC_EMAIL, buf, 256);
		entry->get(HubSettings::Email) = Text::fromT(buf);

		GetDlgItemText(IDC_AWAY_MSG, buf, 1024);
		entry->get(HubSettings::AwayMsg) = Text::fromT(buf);

		entry->get(HubSettings::ShowJoins) = to3bool(IsDlgButtonChecked(IDC_SHOW_JOIN));
		entry->get(HubSettings::FavShowJoins) = to3bool(IsDlgButtonChecked(IDC_SHOW_JOIN));
		entry->get(HubSettings::LogMainChat) = to3bool(IsDlgButtonChecked(IDC_LOGMAINCHAT));
		entry->get(HubSettings::ChatNotify) = to3bool(IsDlgButtonChecked(IDC_CHAT_NOTIFY));
		entry->get(HubSettings::AcceptFailovers) = to3bool(IsDlgButtonChecked(IDC_FAILOVER));

		auto val = HubSettings::getMinInt();
		if (!IsDlgButtonChecked(IDC_SEARCH_INTERVAL_DEFAULT)) {
			GetDlgItemText(IDC_FAV_SEARCH_INTERVAL_BOX, buf, 512);
			val = Util::toInt(Text::fromT(buf));
		}
		entry->get(HubSettings::SearchInterval) = val;

		
		CComboBox combo;
		combo.Attach(GetDlgItem(IDC_FAV_DLG_GROUP));
	
		if(combo.GetCurSel() == 0)
		{
			entry->setGroup(Util::emptyString);
		}
		else
		{
			tstring text(combo.GetWindowTextLength() + 1, _T('\0'));
			combo.GetWindowText(&text[0], text.size());
			text.resize(text.size()-1);
			entry->setGroup(Text::fromT(text));
		}
		combo.Detach();


		// connection modes
		auto getConnMode = [](const CComboBox& combo) -> int {
			 if (combo.GetCurSel() == 1)
				return SettingsManager::INCOMING_DISABLED;
			else if (combo.GetCurSel() == 2)
				return SettingsManager::INCOMING_ACTIVE;
			else if (combo.GetCurSel() == 3)
				return SettingsManager::INCOMING_PASSIVE;

			return HubSettings::getMinInt();
		};

		entry->get(HubSettings::Connection) = getConnMode(modeCombo4);
		entry->get(HubSettings::Connection6) = getConnMode(modeCombo6);

		//external ip addresses
		GetDlgItemText(IDC_SERVER4, buf, 512);
		entry->get(HubSettings::UserIp) = Text::fromT(buf);

		GetDlgItemText(IDC_SERVER6, buf, 512);
		entry->get(HubSettings::UserIp6) = Text::fromT(buf);

		auto p = ShareManager::getInstance()->getProfiles();

		if(hideShare) {
			entry->setShareProfile(*(p.end()-1));
		} else {
			if(entry->isAdcHub()) {
				entry->setShareProfile(p[ctrlProfile.GetCurSel()]);
			} else {
				entry->setShareProfile(p[0]);
			}
		}

		FavoriteManager::getInstance()->save();
	}
	loaded = false;
	EndDialog(wID);
	return 0;
}

LRESULT FavHubProperties::OnTextChanged(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if (!loaded)
		return 0;
	
	if (wID == IDC_HUBADDR) {
		CComboBox combo;
		combo.Attach(GetDlgItem(IDC_ENCODING));
		tstring address;
		address.resize(1024);
		address.resize(GetDlgItemText(IDC_HUBADDR, &address[0], 1024));

		if(AirUtil::isAdcHub(Text::fromT(address))) {
			if (!hideShare)
				ctrlProfile.EnableWindow(true);
			combo.SetCurSel(4); // select UTF-8 for ADC hubs
			::EnableWindow(GetDlgItem(IDC_STEALTH),	0);
			combo.EnableWindow(false);
		} else {
			ctrlProfile.EnableWindow(false);
			::EnableWindow(GetDlgItem(IDC_STEALTH),	1);
			combo.EnableWindow(true);
		}
		combo.Detach();
	}

	return TRUE;
}