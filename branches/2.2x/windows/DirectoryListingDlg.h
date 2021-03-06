/* 
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

#ifndef DIRLIST_SEARCH_DLG_H
#define DIRLIST_SEARCH_DLG_H

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000
#include <atlcrack.h>

#include "../client/DirectoryListing.h"
#include "../client/Util.h"
#include "../client/ResourceManager.h"

class DirectoryListingDlg : public CDialogImpl<DirectoryListingDlg> {
public:
	bool display;
	bool remove;
	tstring search;
	int fileType;

	enum { IDD = IDD_DIRLIST_DLG };

	DirectoryListingDlg();
	~DirectoryListingDlg();

	BEGIN_MSG_MAP_EX(DirectoryListingDlg)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		MESSAGE_HANDLER(WM_SETFOCUS, onFocus)
		COMMAND_ID_HANDLER(IDOK, OnCloseCmd)
		COMMAND_ID_HANDLER(IDCANCEL, OnCloseCmd)
		COMMAND_ID_HANDLER(IDC_AS_ACTION, onAction)
		COMMAND_HANDLER(IDC_BROWSE, BN_CLICKED, onBrowse)
	END_MSG_MAP()


	LRESULT onFocus(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
		ctrlSearch.SetFocus();
		return FALSE;
	}

	LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnCloseCmd(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT onAction(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT onBrowse(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled);

private:
//	enum { BUF_LEN = 1024 };
	CImageList ftImage;

	CEdit ctrlSearch, ctrlCheatingDescription, ctrlTarget;
	CComboBox cRaw;
	CComboBoxEx ctrlFileType;
	CComboBox cAction;
	CButton cDisplay;
	void onPartialSearch(tstring aSearch, int type);
};
#endif
