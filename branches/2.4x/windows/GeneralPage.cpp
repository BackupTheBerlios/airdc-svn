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

#include "../client/version.h"
#include "../client/AirUtil.h"
#include "../client/Localization.h"
#include "Resource.h"

#include "GeneralPage.h"
#include "WinUtil.h"

PropPage::TextItem GeneralPage::texts[] = {
	{ IDC_SETTINGS_PERSONAL_INFORMATION, ResourceManager::SETTINGS_PERSONAL_INFORMATION },
	{ IDC_SETTINGS_NICK, ResourceManager::NICK },
	{ IDC_SETTINGS_EMAIL, ResourceManager::EMAIL },
	{ IDC_SETTINGS_DESCRIPTION, ResourceManager::DESCRIPTION },
	{ IDC_SETTINGS_PROFILE, ResourceManager::SETTINGS_PROFILE },
	{ IDC_PUBLIC, ResourceManager::PROFILE_PUBLIC },
	{ IDC_RAR, ResourceManager::PROFILE_RAR },
	{ IDC_PRIVATE_HUB, ResourceManager::PROFILE_PRIVATE },
	{ IDC_LANGUAGE_CAPTION, ResourceManager::SETTINGS_LANGUAGE },
	{ IDC_LANGUAGE_NOTE, ResourceManager::LANGUAGE_NOTE },
	{ IDC_AUTO_AWAY, ResourceManager::SETTINGS_AUTO_AWAY },
	{ IDC_SETTINGS_DEFAULT_AWAY_MSG, ResourceManager::SETTINGS_DEFAULT_AWAY_MSG },
	{ IDC_AWAY_MODE, ResourceManager::AWAY_MODE },
	{ IDC_AWAY_IDLE_TEXT_BEGIN, ResourceManager::AWAY_IDLE_TIME_BEGIN },
	{ IDC_AWAY_IDLE_TEXT_END, ResourceManager::AWAY_IDLE_TIME_END },
	{ 0, ResourceManager::SETTINGS_AUTO_AWAY }
};

PropPage::Item GeneralPage::items[] = {
	{ IDC_NICK,			SettingsManager::NICK,			PropPage::T_STR }, 
	{ IDC_EMAIL,		SettingsManager::EMAIL,			PropPage::T_STR }, 
	{ IDC_DESCRIPTION,	SettingsManager::DESCRIPTION,	PropPage::T_STR },
	{ IDC_AUTO_AWAY,	SettingsManager::AUTO_AWAY,	    PropPage::T_BOOL },
 	{ IDC_DEFAULT_AWAY_MESSAGE, SettingsManager::DEFAULT_AWAY_MESSAGE, PropPage::T_STR },
	{ IDC_AWAY_IDLE_TIME, SettingsManager::AWAY_IDLE_TIME, PropPage::T_INT },
	{ 0, 0, PropPage::T_END }
};

void GeneralPage::write()
{
	if(IsDlgButtonChecked(IDC_PUBLIC)){
		AirUtil::setProfile(0);
	} else if(IsDlgButtonChecked(IDC_RAR)) {
		AirUtil::setProfile(1);
	} else if(IsDlgButtonChecked(IDC_PRIVATE_HUB)){
		AirUtil::setProfile(2);
	}

	Localization::setLanguage(ctrlLanguage.GetCurSel());
	PropPage::write((HWND)(*this), items);
}

LRESULT GeneralPage::onInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
{
	PropPage::translate((HWND)(*this), texts);
	PropPage::read((HWND)(*this), items);

	setMinMax(IDC_AWAY_SPIN, 0, 60);

	ctrlLanguage.Attach(GetDlgItem(IDC_LANGUAGE));

	switch(SETTING(SETTINGS_PROFILE)) {
		case SettingsManager::PROFILE_PUBLIC: 
			CheckDlgButton(IDC_PUBLIC, BST_CHECKED);
			//setRarProfile(false);
			break;
		case SettingsManager::PROFILE_RAR: 
			CheckDlgButton(IDC_RAR, BST_CHECKED);
			//setRarProfile(true);
			break;
		case SettingsManager::PROFILE_PRIVATE: 
			CheckDlgButton(IDC_PRIVATE_HUB, BST_CHECKED); 
			//setRarProfile(false);
			break;
		default: 
			CheckDlgButton(IDC_PUBLIC, BST_CHECKED);
			//setRarProfile(false);
			break;
	}

	WinUtil::appendLanguageMenu(ctrlLanguage);

	nick.Attach(GetDlgItem(IDC_NICK));
	nick.LimitText(35);
	nick.Detach();

	desc.Attach(GetDlgItem(IDC_DESCRIPTION));
	desc.LimitText(35);
	desc.Detach();
	desc.Attach(GetDlgItem(IDC_SETTINGS_EMAIL));
	desc.LimitText(35);
	desc.Detach();
	return TRUE;
}

LRESULT GeneralPage::onTextChanged(WORD /*wNotifyCode*/, WORD wID, HWND hWndCtl, BOOL& /*bHandled*/)
{
	TCHAR buf[SETTINGS_BUF_LEN];

	GetDlgItemText(wID, buf, SETTINGS_BUF_LEN);
	tstring old = buf;


	// Strip '$', '|', '<', '>' and ' ' from text
	TCHAR *b = buf, *f = buf, c;
	while( (c = *b++) != 0 )
	{
		if(c != '$' && c != '|' && (wID == IDC_DESCRIPTION || c != ' ') && ( (wID != IDC_NICK && wID != IDC_DESCRIPTION && wID != IDC_SETTINGS_EMAIL) || (c != '<' && c != '>') ) )
			*f++ = c;
	}

	*f = '\0';

	if(old != buf)
	{
		// Something changed; update window text without changing cursor pos
		CEdit tmp;
		tmp.Attach(hWndCtl);
		int start, end;
		tmp.GetSel(start, end);
		tmp.SetWindowText(buf);
		if(start > 0) start--;
		if(end > 0) end--;
		tmp.SetSel(start, end);
		tmp.Detach();
	}

	return TRUE;
}

LRESULT GeneralPage::onProfile(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if(IsDlgButtonChecked(IDC_PUBLIC)){
		CheckDlgButton(IDC_RAR, BST_UNCHECKED);
		CheckDlgButton(IDC_PRIVATE_HUB, BST_UNCHECKED);
		//setRarProfile(false);
	}
	else if(IsDlgButtonChecked(IDC_RAR)){
		CheckDlgButton(IDC_PRIVATE_HUB, BST_UNCHECKED);
		CheckDlgButton(IDC_PUBLIC, BST_UNCHECKED);
		//setRarProfile(true);
	}
	else {
		CheckDlgButton(IDC_RAR, BST_UNCHECKED);
		CheckDlgButton(IDC_PUBLIC, BST_UNCHECKED);
		//setRarProfile(false);
	}
	return TRUE;
}