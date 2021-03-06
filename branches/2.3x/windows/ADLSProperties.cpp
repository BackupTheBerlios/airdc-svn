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

#include "stdafx.h"
#include "../client/DCPlusPlus.h"
#include "Resource.h"

#include "ADLSProperties.h"
#include "../client/ADLSearch.h"
#include "../client/FavoriteManager.h"
#include "../client/pme.h"
#include "WinUtil.h"
#define GET_TEXT(id, var) \
	GetDlgItemText(id, buf, 1024); \
	var = buf;

// Initialize dialog
LRESULT ADLSProperties::OnInitDialog(UINT, WPARAM, LPARAM, BOOL&) {
	// Translate the texts
	SetWindowText(CTSTRING(ADLS_PROPERTIES));
	SetDlgItemText(IDC_ADLSP_SEARCH, CTSTRING(ADLS_SEARCH_STRING));
	SetDlgItemText(IDC_ADLSP_TYPE, CTSTRING(ADLS_TYPE));
	SetDlgItemText(IDC_ADLSP_SIZE_MIN, CTSTRING(ADLS_SIZE_MIN));
	SetDlgItemText(IDC_ADLSP_SIZE_MAX, CTSTRING(ADLS_SIZE_MAX));
	SetDlgItemText(IDC_ADLSP_UNITS, CTSTRING(ADLS_UNITS));
	SetDlgItemText(IDC_ADLSP_DESTINATION, CTSTRING(ADLS_DESTINATION));
	SetDlgItemText(IDC_IS_ACTIVE, CTSTRING(ADLS_ENABLED));
	SetDlgItemText(IDC_AUTOQUEUE, CTSTRING(ADLS_DOWNLOAD));
	SetDlgItemText(IDC_IS_FORBIDDEN, CTSTRING(FORBIDDEN));
	SetDlgItemText(IDC_ADLS_COMMENT_STRING, CTSTRING(COMMENT));
	SetDlgItemText(IDC_IS_CASE_SENSITIVE, CTSTRING(CASE_SENSITIVE));
	SetDlgItemText(IDC_REGEXP_TESTER, CTSTRING(REGEXP_TESTER));
	SetDlgItemText(IDC_REGEXP_TEST, CTSTRING(MATCH));
	SetDlgItemText(IDC_REGEXP, CTSTRING(ADLS_REGEXP));

	// Initialize dialog items
	ctrlSearch.Attach(GetDlgItem(IDC_SEARCH_STRING));
	ctrlComment.Attach(GetDlgItem(IDC_ADLS_COMMENT));
	ctrlDestDir.Attach(GetDlgItem(IDC_DEST_DIR));
	ctrlMinSize.Attach(GetDlgItem(IDC_MIN_FILE_SIZE));
	ctrlMaxSize.Attach(GetDlgItem(IDC_MAX_FILE_SIZE));
	ctrlActive.Attach(GetDlgItem(IDC_IS_ACTIVE));
	ctrlAutoQueue.Attach(GetDlgItem(IDC_AUTOQUEUE));
	ctrlRegexp.Attach(GetDlgItem(IDC_REGEXP));
	ctrlCaseSensitive.Attach(GetDlgItem(IDC_IS_CASE_SENSITIVE));

	ctrlSearchType.Attach(GetDlgItem(IDC_SOURCE_TYPE));
	ctrlSearchType.AddString(CTSTRING(FILENAME));
	ctrlSearchType.AddString(CTSTRING(DIRECTORY));
	ctrlSearchType.AddString(CTSTRING(ADLS_FULL_PATH));
	ctrlSearchType.AddString(_T("TTH"));

	ctrlSizeType.Attach(GetDlgItem(IDC_SIZE_TYPE));
	ctrlSizeType.AddString(CTSTRING(B));
	ctrlSizeType.AddString(CTSTRING(KiB));
	ctrlSizeType.AddString(CTSTRING(MiB));
	ctrlSizeType.AddString(CTSTRING(GiB));
	
	// Load search data
	ctrlSearch.SetWindowText(Text::toT(search->searchString).c_str());
	ctrlComment.SetWindowText(Text::toT(search->adlsComment).c_str());
	ctrlDestDir.SetWindowText(Text::toT(search->destDir).c_str());
	ctrlMinSize.SetWindowText((search->minFileSize > 0 ? Util::toStringW(search->minFileSize) : _T("")).c_str());
	ctrlMaxSize.SetWindowText((search->maxFileSize > 0 ? Util::toStringW(search->maxFileSize) : _T("")).c_str());
	ctrlActive.SetCheck(search->isActive ? 1 : 0);
	ctrlAutoQueue.SetCheck(search->isAutoQueue ? 1 : 0);
	ctrlRegexp.SetCheck(search->isRegexp ? 1 : 0);
	ctrlSearchType.SetCurSel(search->sourceType);
	ctrlSizeType.SetCurSel(search->typeFileSize);
	::SendMessage(GetDlgItem(IDC_IS_FORBIDDEN), BM_SETCHECK, search->isForbidden ? 1 : 0, 0L);
	::SendMessage(GetDlgItem(IDC_REGEXP), BM_SETCHECK, search->isRegexp ? 1 : 0, 0L);
	::SendMessage(GetDlgItem(IDC_IS_CASE_SENSITIVE), BM_SETCHECK, search->isCaseSensitive ? 1 : 0, 0L);

	fixControls();

	// Center dialog
	CenterWindow(GetParent());

	return FALSE;
}

LRESULT ADLSProperties::OnRegExpTester(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	TCHAR buf[1024];
	tstring exp, text;
	GET_TEXT(IDC_SEARCH_STRING, exp);
	GET_TEXT(IDC_TEST_STRING, text);
	MessageBox(Text::toT(matchRegExp(Text::fromT(exp), Text::fromT(text), (::SendMessage(GetDlgItem(IDC_IS_CASE_SENSITIVE), BM_GETCHECK, 0, 0L) != 0))).c_str(), CTSTRING(REGEXP_TESTER), MB_OK);
	return 0;
}
string ADLSProperties::matchRegExp(const string& aExp, const string& aString, const bool& caseSensitive /*= true*/) {
	string str1 = aExp;
	string str2 = aString;
	try {
		PME reg(aExp, caseSensitive ? "" : "i");
		if(reg.match(aString)){
			return STRING(REGEXP_MATCH);
		} else{
			return STRING(REGEXP_MISMATCH);
		};
	} catch(...) {
		return STRING(INVALID_REGEXP);
	}
}

// Exit dialog
LRESULT ADLSProperties::OnCloseCmd(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
	if(wID == IDOK) {
		// Update search
		TCHAR buf[256];

		ctrlSearch.GetWindowText(buf, 256);
		search->searchString = Text::fromT(buf);
		ctrlComment.GetWindowText(buf, 521);
		search->adlsComment = Text::fromT(buf);
		ctrlDestDir.GetWindowText(buf, 256);
		search->destDir = Text::fromT(buf);

		ctrlMinSize.GetWindowText(buf, 256);
		search->minFileSize = (_tcslen(buf) == 0 ? -1 : Util::toInt64(Text::fromT(buf)));
		ctrlMaxSize.GetWindowText(buf, 256);
		search->maxFileSize = (_tcslen(buf) == 0 ? -1 : Util::toInt64(Text::fromT(buf)));

		search->isActive = (ctrlActive.GetCheck() == 1);
		search->isAutoQueue = (ctrlAutoQueue.GetCheck() == 1);

		search->sourceType = (ADLSearch::SourceType)ctrlSearchType.GetCurSel();
		search->typeFileSize = (ADLSearch::SizeType)ctrlSizeType.GetCurSel();
		search->isForbidden = (::SendMessage(GetDlgItem(IDC_IS_FORBIDDEN), BM_GETCHECK, 0, 0L) != 0);
		search->isRegexp = (ctrlRegexp.GetCheck() == 1);
		search->isCaseSensitive = (::SendMessage(GetDlgItem(IDC_IS_CASE_SENSITIVE), BM_GETCHECK, 0, 0L) != 0);
		if(search->isForbidden) {
			search->destDir = "Forbidden Files";
		}
	}

	EndDialog(wID);
	return 0;
}

void ADLSProperties::fixControls() {
	if(ctrlSearchType.GetCurSel() == ADLSearch::TTHash) {
		::EnableWindow(GetDlgItem(IDC_REGEXP),				FALSE);
		ctrlRegexp.SetCheck(0);
	} else {
		::EnableWindow(GetDlgItem(IDC_REGEXP),				TRUE);
	}

	BOOL isRegExp = (ctrlRegexp.GetCheck() == 1);
	::EnableWindow(GetDlgItem(IDC_REGEXP_TEST),				isRegExp);
	::EnableWindow(GetDlgItem(IDC_TEST_STRING),				isRegExp);
	::EnableWindow(GetDlgItem(IDC_IS_CASE_SENSITIVE),		isRegExp);

	if (!isRegExp) {
		ctrlCaseSensitive.SetCheck(false);
	}
}

/**
 * @file
 * $Id: ADLSProperties.cpp 238 2006-08-21 18:21:40Z bigmuscle $
 */
