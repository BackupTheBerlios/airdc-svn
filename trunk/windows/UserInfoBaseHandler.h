/*
 * Copyright (C) 2011-2013 AirDC++ Project
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

#ifndef USERINFOBASEHANDLER_H
#define USERINFOBASEHANDLER_H

#include "resource.h"
#include "OMenu.h"

#include "../client/ClientManager.h"
#include "../client/FavoriteManager.h"
#include "../client/UserInfoBase.h"
#include "../client/Util.h"

#include "WinUtil.h"
#include <boost/bind.hpp>

// emulation for non-list objects
template<class T>
struct UserListHandler {
	UserListHandler(T& _aHandler) : handler(_aHandler) { }

	void forEachSelected(void (UserInfoBase::*func)()) {
		(handler.*func)();
	}

	template<class _Function>
	_Function forEachSelectedT(_Function pred) {
		pred(&handler);
		return pred;
	}

private:
	T& handler;
};

template<class T>
class UserInfoBaseHandler {
public:
	BEGIN_MSG_MAP(UserInfoBaseHandler)
		COMMAND_RANGE_HANDLER(IDC_GRANTSLOT, IDC_UNGRANTSLOT, onGrantSlot)
	END_MSG_MAP()
	bool pmItems;
	bool listItems;

	UserInfoBaseHandler(bool appendPmItems=true, bool appendListItems=true) : pmItems(appendPmItems), listItems(appendListItems) { }

	virtual void handleMatchQueue() {
		((T*)this)->getUserList().forEachSelectedT(boost::bind(&UserInfoBase::matchQueue, _1));
	}
	virtual void handleGetList() {
		((T*)this)->getUserList().forEachSelectedT(boost::bind(&UserInfoBase::getList, _1));
	}
	virtual void handleBrowseList() {
		((T*)this)->getUserList().forEachSelectedT(boost::bind(&UserInfoBase::browseList, _1));
	}
	virtual void handleGetBrowseList() {
		((T*)this)->getUserList().forEachSelectedT(boost::bind(&UserInfoBase::getBrowseList, _1));
	}

	virtual void handleAddToFavorites() {
		((T*)this)->getUserList().forEachSelected(&UserInfoBase::addFav);
	}
	virtual void handlePrivateMessage() {
		((T*)this)->getUserList().forEachSelectedT(boost::bind(&UserInfoBase::pm, _1));
	}

	virtual void handleConnectFav() {
		((T*)this)->getUserList().forEachSelected(&UserInfoBase::connectFav);
	}
	LRESULT onGrantSlot(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
		switch(wID) {
			case IDC_GRANTSLOT:		((T*)this)->getUserList().forEachSelectedT(boost::bind(&UserInfoBase::grant, _1)); break;
			case IDC_GRANTSLOT_DAY:	((T*)this)->getUserList().forEachSelectedT(boost::bind(&UserInfoBase::grantDay, _1)); break;
			case IDC_GRANTSLOT_HOUR:	((T*)this)->getUserList().forEachSelectedT(boost::bind(&UserInfoBase::grantHour, _1)); break;
			case IDC_GRANTSLOT_WEEK:	((T*)this)->getUserList().forEachSelectedT(boost::bind(&UserInfoBase::grantWeek, _1)); break;
			case IDC_UNGRANTSLOT:	((T*)this)->getUserList().forEachSelected(&UserInfoBase::ungrant); break;
		}
		return 0;
	}
	virtual void handleRemoveAll() { 
		((T*)this)->getUserList().forEachSelected(&UserInfoBase::removeAll);
	}

	struct UserTraits {
		UserTraits() : noFullList(true), favOnly(true), nonFavOnly(true), allFullList(true) { }
		void operator()(const UserInfoBase* ui) {
			if(ui->getUser()) {
				if(!ui->getUser()->isSet(User::NMDC) && ClientManager::getInstance()->getShareInfo(HintedUser(ui->getUser(), ui->getHubUrl())).second > SETTING(FULL_LIST_DL_LIMIT)) { 
					allFullList = false;
				} else {
					noFullList = false;
				}

				bool fav = ui->getUser()->isFavorite();
				if(fav)
					nonFavOnly = false;
				if(!fav)
					favOnly = false;
			}
		}

		bool noFullList;
		bool allFullList;
		bool favOnly;
		bool nonFavOnly;
	};

	template<class K>
	void appendListMenu(const UserPtr& aUser, const User::UserInfoList& list, OMenu* subMenu, bool addShareInfo) {
		for (auto& i: list) {
			string url = i.hubUrl;
			subMenu->appendItem(Text::toT(i.hubName) + (addShareInfo ? (_T(" (") + Util::formatBytesW(i.shared) + _T(")")) : Util::emptyStringT), 
				[this, aUser, url] { K()(aUser, url); });
		}
	}

	void appendUserItems(OMenu& menu, bool showFullList = true, const UserPtr& aUser = nullptr) {
		UserTraits traits = ((T*)this)->getUserList().forEachSelectedT(UserTraits());
		bool multipleHubs = false;

		auto appendSingleDownloadItems = [&](bool hubUnknown) -> void {
			int commonFlags = aUser && hubUnknown ? OMenu::FLAG_DISABLED : 0;
			int defaultFlag = commonFlags > 0 ? commonFlags : OMenu::FLAG_DEFAULT;

			menu.appendSeparator();
			if (showFullList || traits.allFullList) {
				menu.appendItem(TSTRING(GET_FILE_LIST), [=] { handleGetList(); }, defaultFlag);
			}
			if (!traits.noFullList && !traits.allFullList) {
				menu.appendItem(TSTRING(GET_BROWSE_LIST), [=] { handleGetBrowseList(); }, defaultFlag);
			}
			menu.appendItem(TSTRING(BROWSE_FILE_LIST), [=] { handleBrowseList(); }, defaultFlag);
			menu.appendItem(TSTRING(MATCH_QUEUE), [=] { handleMatchQueue(); }, commonFlags);
			menu.appendSeparator();
		};

		User::UserInfoList list;
		if (aUser) {
			ClientManager::getInstance()->getUserInfoList(aUser, list);
			if (list.size() > 1) {
				multipleHubs = true;
				if (pmItems)
					appendListMenu<WinUtil::PM>(aUser, list, menu.createSubMenu(CTSTRING(SEND_PRIVATE_MESSAGE)), false);

				if (listItems) {
					//combine items in the list based on the share size
					User::UserInfoList shareList(list.begin(), list.end());
					for (auto i = shareList.begin(); i != shareList.end(); ++i) {
						StringList names { i->hubName };

						auto matchPos = i;

						//erase all following entries with the same share size
						while ((matchPos = find_if(i+1, shareList.end(), [&i](const User::UserHubInfo& uhi) { return uhi.shared == i->shared; })) != shareList.end()) {
							names.push_back(matchPos->hubName);
							shareList.erase(matchPos);
						}

						//combine the hub names with this share size
						i->hubName = Util::listToString(names);
					}

					if (shareList.size() > 1) {
						menu.appendSeparator();
						appendListMenu<WinUtil::BrowseList>(aUser, shareList, menu.createSubMenu(CTSTRING(BROWSE_FILE_LIST)), true);
						if (showFullList || traits.allFullList)
							appendListMenu<WinUtil::GetList>(aUser, shareList, menu.createSubMenu(CTSTRING(GET_FILE_LIST)), true);
						if (!traits.noFullList && !traits.allFullList)
							appendListMenu<WinUtil::GetBrowseList>(aUser, shareList, menu.createSubMenu(CTSTRING(GET_BROWSE_LIST)), true);
						appendListMenu<WinUtil::MatchQueue>(aUser, shareList, menu.createSubMenu(CTSTRING(MATCH_QUEUE)), true);
					} else {
						appendSingleDownloadItems(false);
					}
				}

				//if(!traits.nonFavOnly)
				//	appendListMenu<WinUtil::ConnectFav>(aUser, list, menu.createSubMenu(CTSTRING(CONNECT_FAVUSER_HUB)), false);
			}
		} 
		
		if (!multipleHubs) {
			if (pmItems)
				menu.appendItem(TSTRING(SEND_PRIVATE_MESSAGE), [=] { handlePrivateMessage(); }, !listItems ? OMenu::FLAG_DEFAULT : 0);

			if (listItems)
				appendSingleDownloadItems(list.empty() ? true : false);

			//if(!traits.nonFavOnly)
			//	menu.AppendMenu(MF_STRING, IDC_CONNECT, CTSTRING(CONNECT_FAVUSER_HUB));
		}

		if(!traits.favOnly) {
			menu.appendItem(TSTRING(ADD_TO_FAVORITES), [=] { handleAddToFavorites(); });
			menu.appendSeparator();
		}
		menu.appendItem(TSTRING(REMOVE_FROM_ALL), [=] { handleRemoveAll(); });
		menu.AppendMenu(MF_POPUP, (UINT)(HMENU)WinUtil::grantMenu, CTSTRING(GRANT_SLOTS_MENU));
	}	
};

#endif // !defined(USERINFOBASEHANDLER_H)