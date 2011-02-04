/*
 * Copyright (C) 2001-2006 Jacek Sieka, arnetheduck on gmail point com
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

#if !defined(GENERAL_PAGE_H)
#define GENERAL_PAGE_H

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

#include <atlcrack.h>
#include "PropPage.h"

class GeneralPage : public CPropertyPage<IDD_GENERALPAGE>, public PropPage
{
public:
	GeneralPage(SettingsManager *s) : PropPage(s) {
		SetTitle(CTSTRING(SETTINGS_GENERAL));
		m_psp.dwFlags |= PSP_RTLREADING;
	}
	~GeneralPage() { }

	BEGIN_MSG_MAP_EX(GeneralPage)
		MESSAGE_HANDLER(WM_INITDIALOG, onInitDialog)
		COMMAND_HANDLER(IDC_NICK, EN_CHANGE, onTextChanged)
		COMMAND_HANDLER(IDC_EMAIL, EN_CHANGE, onTextChanged)
		COMMAND_HANDLER(IDC_DESCRIPTION, EN_CHANGE, onTextChanged)
		COMMAND_ID_HANDLER(IDC_ENG, onLng)
		COMMAND_ID_HANDLER(IDC_LANG_SWE, onLng)
		COMMAND_ID_HANDLER(IDC_LANG_FIN, onLng)
		COMMAND_ID_HANDLER(IDC_LANG_ITA, onLng)
		COMMAND_ID_HANDLER(IDC_LANG_HUN, onLng)
		COMMAND_ID_HANDLER(IDC_LANG_RO, onLng)
		COMMAND_ID_HANDLER(IDC_LANG_DAN, onLng)
		COMMAND_ID_HANDLER(IDC_LANG_NOR, onLng)
		COMMAND_ID_HANDLER(IDC_LANG_POR, onLng)
		COMMAND_ID_HANDLER(IDC_LANG_POL, onLng)
		COMMAND_ID_HANDLER(IDC_LANG_FR, onLng)
		COMMAND_ID_HANDLER(IDC_LANG_D, onLng)
		COMMAND_ID_HANDLER(IDC_LANG_RUS, onLng)
		COMMAND_ID_HANDLER(IDC_LANG_GER, onLng)
	END_MSG_MAP()

	LRESULT onInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT onGetIP(WORD /* wNotifyCode */, WORD /* wID */, HWND /* hWndCtl */, BOOL& /* bHandled */);
	LRESULT onTextChanged(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT onClickedRadioButton(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT onLng(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);

	// Common PropPage interface
	PROPSHEETPAGE *getPSP() { return (PROPSHEETPAGE *)*this; }
	virtual void write();
	
private:
	static Item items[];
	static TextItem texts[];
	CComboBox ctrlConnection;	
	CEdit nick;
	CEdit desc;
	void fixControls();

};

#endif // !defined(GENERAL_PAGE_H)

/**
 * @file
 * $Id: GeneralPage.h 308 2007-07-13 18:57:02Z bigmuscle $
 */
