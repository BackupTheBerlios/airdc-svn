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

#include "ResourceLoader.h"
#include "UsersFrame.h"
#include "LineDlg.h"
#include "TextFrame.h"
#include "../client/ClientManager.h"

int UsersFrame::columnIndexes[] = { COLUMN_NICK, COLUMN_NICKS, COLUMN_HUB, COLUMN_SEEN, COLUMN_DESCRIPTION };
int UsersFrame::columnSizes[] = { 150, 200, 300, 150, 200 };
static ResourceManager::Strings columnNames[] = { ResourceManager::AUTO_GRANT, ResourceManager::ONLINE_NICKS, ResourceManager::LAST_HUB, ResourceManager::LAST_SEEN, ResourceManager::DESCRIPTION };

struct FieldName {
	string field;
	tstring name;
	tstring (*convert)(const string &val);
};
static tstring formatBytes(const string& val) {
	return Text::toT(Util::formatBytes(val));
}

static tstring formatSpeed(const string& val) {
	return Text::toT(boost::str(boost::format("%1%/s") % Util::formatBytes(val)));
}

static const FieldName fields[] =
{
	{ "NI", _T("Nick: "), &Text::toT },
	{ "AW", _T("Away: "), &Text::toT },
	{ "DE", _T("Description: "), &Text::toT },
	{ "EM", _T("E-Mail: "), &Text::toT },
	{ "SS", _T("Shared: "), &formatBytes },
	{ "SF", _T("Shared files: "), &Text::toT },
	{ "US", _T("Upload speed: "), &formatSpeed },
	{ "DS", _T("Download speed: "), &formatSpeed },
	{ "SL", _T("Total slots: "), &Text::toT },
	{ "FS", _T("Free slots: "), &Text::toT },
	{ "HN", _T("Hubs (normal): "), &Text::toT },
	{ "HR", _T("Hubs (registered): "), &Text::toT },
	{ "HO", _T("Hubs (op): "), &Text::toT },
	{ "I4", _T("IP (v4): "), &Text::toT },
	{ "I6", _T("IP (v6): "), &Text::toT },
	//{ "U4", _T("Search port (v4): "), &Text::toT },
	//{ "U6", _T("Search port (v6): "), &Text::toT },
	//{ "SU", _T("Features: "), &Text::toT },
	{ "VE", _T("Application version: "), &Text::toT },
	{ "AP", _T("Application: "), &Text::toT },
	{ "ID", _T("CID: "), &Text::toT },
	//{ "KP", _T("TLS Keyprint: "), &Text::toT },
	{ "CO", _T("Connection: "), &Text::toT },
	//{ "CT", _T("Client type: "), &Text::toT },
	{ "TA", _T("Tag: "), &Text::toT },
	{ "", _T(""), 0 }
};


LRESULT UsersFrame::onCreate(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& bHandled)
{
	CreateSimpleStatusBar(ATL_IDS_IDLEMESSAGE, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | SBARS_SIZEGRIP);
	ctrlStatus.Attach(m_hWndStatusBar);

	ctrlUsers.Create(m_hWnd, rcDefault, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | 
		WS_HSCROLL | WS_VSCROLL | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS, WS_EX_CLIENTEDGE, IDC_USERS);
	ctrlUsers.SetExtendedListViewStyle(LVS_EX_LABELTIP | LVS_EX_HEADERDRAGDROP | LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
	
	ctrlInfo.Create(m_hWnd, rcDefault, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE, WS_EX_CLIENTEDGE);
	ctrlInfo.Subclass();
	ctrlInfo.SetReadOnly(TRUE);
	ctrlInfo.SetFont(WinUtil::font);
	ctrlInfo.SetBackgroundColor(WinUtil::bgColor); 
	ctrlInfo.SetDefaultCharFormat(WinUtil::m_ChatTextGeneral);

	SetSplitterPanes(ctrlUsers.m_hWnd, ctrlInfo.m_hWnd);
	m_nProportionalPos = SETTING(FAV_USERS_SPLITTER_POS);
	SetSplitterExtendedStyle(SPLIT_PROPORTIONAL);

	images.Create(16, 16, ILC_COLOR32 | ILC_MASK,  0, 2);
	images.AddIcon(ResourceLoader::loadIcon(IDR_PRIVATE, 16));
	images.AddIcon(ResourceLoader::loadIcon(IDR_PRIVATE_OFF, 16));
	ctrlUsers.SetImageList(images, LVSIL_SMALL);

	ctrlUsers.SetBkColor(WinUtil::bgColor);
	ctrlUsers.SetTextBkColor(WinUtil::bgColor);
	ctrlUsers.SetTextColor(WinUtil::textColor);
	
	// Create listview columns
	WinUtil::splitTokens(columnIndexes, SETTING(USERSFRAME_ORDER), COLUMN_LAST);
	WinUtil::splitTokens(columnSizes, SETTING(USERSFRAME_WIDTHS), COLUMN_LAST);
	
	for(uint8_t j=0; j<COLUMN_LAST; j++) {
		ctrlUsers.InsertColumn(j, CTSTRING_I(columnNames[j]), LVCFMT_LEFT, columnSizes[j], j);
	}
	
	ctrlUsers.setColumnOrderArray(COLUMN_LAST, columnIndexes);
	ctrlUsers.setSortColumn(COLUMN_NICK);

	FavoriteManager::getInstance()->addListener(this);
	SettingsManager::getInstance()->addListener(this);

	ctrlUsers.SetRedraw(FALSE);

	auto ul = FavoriteManager::getInstance()->getFavoriteUsers();
	for(auto& u: ul | map_values)
		addUser(u);

	ctrlUsers.SetRedraw(TRUE);
	CRect rc(SETTING(USERS_LEFT), SETTING(USERS_TOP), SETTING(USERS_RIGHT), SETTING(USERS_BOTTOM));
	if(! (rc.top == 0 && rc.bottom == 0 && rc.left == 0 && rc.right == 0) )
		MoveWindow(rc, TRUE);

	WinUtil::SetIcon(m_hWnd, IDI_FAVORITE_USERS);

	startup = false;

	bHandled = FALSE;
	return TRUE;

}

LRESULT UsersFrame::onContextMenu(UINT /*uMsg*/, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
	if (reinterpret_cast<HWND>(wParam) == ctrlUsers && ctrlUsers.GetSelectedCount() > 0 ) { 
		POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

		if(pt.x == -1 && pt.y == -1) {
			WinUtil::getContextMenuPos(ctrlUsers, pt);
		}

		UserPtr u = nullptr;
		tstring x;
		if (ctrlUsers.GetSelectedCount() == 1) {
			auto ui = ctrlUsers.getItemData(WinUtil::getFirstSelectedIndex(ctrlUsers));
			x = ui->columns[COLUMN_NICK];
			u = ui->getUser();
		} else {
			x = _T("");
		}
	
		OMenu usersMenu;
		usersMenu.CreatePopupMenu();
		usersMenu.AppendMenu(MF_STRING, IDC_OPEN_USER_LOG, CTSTRING(OPEN_USER_LOG));
		usersMenu.AppendMenu(MF_SEPARATOR);
		appendUserItems(usersMenu, true, u);
		usersMenu.AppendMenu(MF_SEPARATOR);
		usersMenu.AppendMenu(MF_STRING, IDC_EDIT, CTSTRING(PROPERTIES));
		usersMenu.AppendMenu(MF_STRING, IDC_REMOVE, CTSTRING(REMOVE));

		if (!x.empty())
			usersMenu.InsertSeparatorFirst(x);
		
		usersMenu.open(m_hWnd, TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt);

		return TRUE; 
	}
	bHandled = FALSE;
	return FALSE; 
}
	
void UsersFrame::UpdateLayout(BOOL bResizeBars /* = TRUE */) {
	RECT rect;
	GetClientRect(&rect);
	// position bars and offset their dimensions
	UpdateBarsPosition(rect, bResizeBars);
		
	if(ctrlStatus.IsWindow()) {
		CRect sr;
		int w[3];
		ctrlStatus.GetClientRect(sr);
		int tmp = (sr.Width()) > 316 ? 216 : ((sr.Width() > 116) ? sr.Width()-100 : 16);
			
		w[0] = sr.right - tmp;
		w[1] = w[0] + (tmp-16)/2;
		w[2] = w[0] + (tmp-16);
			
		ctrlStatus.SetParts(3, w);
	}
		
	CRect rc = rect;
	ctrlUsers.MoveWindow(rc);
	SetSplitterRect(rc);
}

LRESULT UsersFrame::onRemove(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	int i = -1;
	while( (i = ctrlUsers.GetNextItem(-1, LVNI_SELECTED)) != -1) {
		ctrlUsers.getItemData(i)->remove();
	}
	return 0;
}

LRESULT UsersFrame::onEdit(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if(ctrlUsers.GetSelectedCount() == 1) {
		int i = ctrlUsers.GetNextItem(-1, LVNI_SELECTED);
		UserInfo* ui = ctrlUsers.getItemData(i);
		dcassert(i != -1);
		LineDlg dlg;
		dlg.description = TSTRING(DESCRIPTION);
		dlg.title = ui->columns[COLUMN_NICK];
		dlg.line = ui->columns[COLUMN_DESCRIPTION];
		if(dlg.DoModal(m_hWnd)) {
			FavoriteManager::getInstance()->setUserDescription(ui->user, Text::fromT(dlg.line));
			ui->columns[COLUMN_DESCRIPTION] = dlg.line;
			ctrlUsers.updateItem(i);
		}	
	}
	return 0;
}

LRESULT UsersFrame::onItemChanged(int /*idCtrl*/, LPNMHDR pnmh, BOOL& /*bHandled*/) {
	NMITEMACTIVATE* l = (NMITEMACTIVATE*)pnmh;
	if(!startup && l->iItem != -1 && ((l->uNewState & LVIS_STATEIMAGEMASK) != (l->uOldState & LVIS_STATEIMAGEMASK))) {
		FavoriteManager::getInstance()->setAutoGrant(ctrlUsers.getItemData(l->iItem)->user, ctrlUsers.GetCheckState(l->iItem) != FALSE);
 	}
	 if ( (l->uChanged & LVIF_STATE) && (l->uNewState & LVIS_SELECTED) != (l->uOldState & LVIS_SELECTED) ) {
		if (l->uNewState & LVIS_SELECTED){
			if(l->iItem != -1) {
				updateInfoText(ctrlUsers.getItemData(l->iItem));
			}
		} else { //deselected
			ctrlInfo.SetWindowText(_T(""));
		}
	 }
  	return 0;
}

LRESULT UsersFrame::onDoubleClick(int /*idCtrl*/, LPNMHDR pnmh, BOOL& bHandled) {
	NMITEMACTIVATE* item = (NMITEMACTIVATE*) pnmh;

	if(item->iItem != -1) {
		PostMessage(WM_COMMAND, IDC_GETLIST, 0);
	} else {
		bHandled = FALSE;
	}

	return 0;
}

LRESULT UsersFrame::onKeyDown(int /*idCtrl*/, LPNMHDR pnmh, BOOL& bHandled) {
	NMLVKEYDOWN* kd = (NMLVKEYDOWN*) pnmh;
	switch(kd->wVKey) {
	case VK_DELETE:
		PostMessage(WM_COMMAND, IDC_REMOVE, 0);
		break;
	case VK_RETURN:
		PostMessage(WM_COMMAND, IDC_EDIT, 0);
		break;
	default:
		bHandled = FALSE;
	}
	return 0;
}
void UsersFrame::updateInfoText(const UserInfo* ui){
	vector<Identity> idents = ClientManager::getInstance()->getIdentities(ui->getUser());
	if(!idents.empty()) {
		auto info = idents[0].getInfo();
		for(size_t i = 1; i < idents.size(); ++i) {
			for(auto& j: idents[i].getInfo()) {
				info[j.first] = j.second;
			}
		}
		tstring tmp = _T("\r\n");
		tmp += _T("User Information: \r\n");
		for(auto f = fields; !f->field.empty(); ++f) {
			auto i = info.find(f->field);
			if(i != info.end()) {
				tmp += f->name + f->convert(i->second) + _T("\r\n");
				info.erase(i);
			}
		}
		ctrlInfo.SetWindowText(tmp.c_str());
	} else
		ctrlInfo.SetWindowText(_T(""));
}

void UsersFrame::addUser(const FavoriteUser& aUser) {
	int i = ctrlUsers.insertItem(new UserInfo(aUser), 0);
	bool b = aUser.isSet(FavoriteUser::FLAG_GRANTSLOT);
	ctrlUsers.SetCheckState(i, b);
	updateUser(aUser.getUser());
}

void UsersFrame::updateUser(const UserPtr& aUser) {
	for(int i = 0; i < ctrlUsers.GetItemCount(); ++i) {
		UserInfo *ui = ctrlUsers.getItemData(i);
		if(ui->user == aUser) {
			ui->columns[COLUMN_SEEN] = aUser->isOnline() ? TSTRING(ONLINE) : Text::toT(Util::formatTime("%Y-%m-%d %H:%M", FavoriteManager::getInstance()->getLastSeen(aUser)));
			if(aUser->isOnline()) {
				ctrlUsers.SetItem(i,0,LVIF_IMAGE, NULL, 0, 0, 0, NULL);
				ui->columns[COLUMN_NICKS] = WinUtil::getNicks(aUser, Util::emptyString);
				ui->columns[COLUMN_HUB] = WinUtil::getHubNames(aUser, Util::emptyString).first;
			} else {
				ui->columns[COLUMN_NICKS] =  TSTRING(OFFLINE);
				ctrlUsers.SetItem(i,0,LVIF_IMAGE, NULL, 1, 0, 0, NULL);
			}
			ctrlUsers.updateItem(i);
		}
	}
}

void UsersFrame::removeUser(const FavoriteUser& aUser) {
	for(int i = 0; i < ctrlUsers.GetItemCount(); ++i) {
		const UserInfo *ui = ctrlUsers.getItemData(i);
		if(ui->user == aUser.getUser()) {
			ctrlUsers.DeleteItem(i);
			delete ui;
			return;
		}
	}
}

LRESULT UsersFrame::onClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& bHandled) {
	if(!closed) {
		FavoriteManager::getInstance()->removeListener(this);
		SettingsManager::getInstance()->removeListener(this);
		closed = true;
		WinUtil::setButtonPressed(IDC_FAVUSERS, false);
		PostMessage(WM_CLOSE);
		return 0;
	} else {
		CRect rc;
		if(!IsIconic()){
			//Get position of window
			GetWindowRect(&rc);
				
			//convert the position so it's relative to main window
			::ScreenToClient(GetParent(), &rc.TopLeft());
			::ScreenToClient(GetParent(), &rc.BottomRight());
				
			//save the position
			SettingsManager::getInstance()->set(SettingsManager::USERS_BOTTOM, (rc.bottom > 0 ? rc.bottom : 0));
			SettingsManager::getInstance()->set(SettingsManager::USERS_TOP, (rc.top > 0 ? rc.top : 0));
			SettingsManager::getInstance()->set(SettingsManager::USERS_LEFT, (rc.left > 0 ? rc.left : 0));
			SettingsManager::getInstance()->set(SettingsManager::USERS_RIGHT, (rc.right > 0 ? rc.right : 0));
			SettingsManager::getInstance()->set(SettingsManager::FAV_USERS_SPLITTER_POS, m_nProportionalPos);
		}
		WinUtil::saveHeaderOrder(ctrlUsers, SettingsManager::USERSFRAME_ORDER, 
			SettingsManager::USERSFRAME_WIDTHS, COLUMN_LAST, columnIndexes, columnSizes);
	
		for(int i = 0; i < ctrlUsers.GetItemCount(); ++i) {
			delete ctrlUsers.getItemData(i);
		}

		bHandled = FALSE;
		return 0;
	}
}

void UsersFrame::UserInfo::update(const FavoriteUser& u) {
	columns[COLUMN_NICK] = Text::toT(u.getNick());
	columns[COLUMN_NICKS] =  user.user->isOnline() ? WinUtil::getNicks(u.getUser(), Util::emptyString) : TSTRING(OFFLINE);
	columns[COLUMN_HUB] = user.user->isOnline() ? WinUtil::getHubNames(u.getUser(), Util::emptyString).first : Text::toT(u.getUrl());
	columns[COLUMN_SEEN] = user.user->isOnline() ? TSTRING(ONLINE) : Text::toT(Util::formatTime("%Y-%m-%d %H:%M", u.getLastSeen()));
	columns[COLUMN_DESCRIPTION] = Text::toT(u.getDescription());
}

LRESULT UsersFrame::onSpeaker(UINT /*uMsg*/, WPARAM wParam, LPARAM lParam, BOOL& /*bHandled*/) {
	if(wParam == USER_UPDATED) {
		updateUser(((Identity*)lParam)->getUser());
	}
	return 0;
}
			
LRESULT UsersFrame::onOpenUserLog(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if(ctrlUsers.GetSelectedCount() == 1) {
		int i = ctrlUsers.GetNextItem(-1, LVNI_SELECTED);
		const UserInfo* ui = ctrlUsers.getItemData(i);
		dcassert(i != -1);

		ParamMap params;
		params["hubNI"] = Util::toString(ClientManager::getInstance()->getHubNames(ui->getUser()->getCID()));
		params["hubURL"] = Util::toString(ClientManager::getInstance()->getHubUrls(ui->getUser()->getCID()));
		params["userCID"] = ui->getUser()->getCID().toBase32(); 
		params["userNI"] = ClientManager::getInstance()->getNicks(ui->getUser()->getCID())[0];
		params["myCID"] = ClientManager::getInstance()->getMe()->getCID().toBase32();

		string file = LogManager::getInstance()->getPath(LogManager::PM, params);
		if(Util::fileExists(file)) {
			WinUtil::viewLog(file);
		} else {
			MessageBox(CTSTRING(NO_LOG_FOR_USER), CTSTRING(NO_LOG_FOR_USER), MB_OK );	  
		}	
	}
		return 0;
}

void UsersFrame::on(SettingsManagerListener::Save, SimpleXML& /*xml*/) noexcept {
	bool refresh = false;
	if(ctrlUsers.GetBkColor() != WinUtil::bgColor) {
		ctrlUsers.SetBkColor(WinUtil::bgColor);
		ctrlUsers.SetTextBkColor(WinUtil::bgColor);
		refresh = true;
	}
	if(ctrlUsers.GetTextColor() != WinUtil::textColor) {
		ctrlUsers.SetTextColor(WinUtil::textColor);
		refresh = true;
	}
	if(refresh == true) {
		RedrawWindow(NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
	}
}