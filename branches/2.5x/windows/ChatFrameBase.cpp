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

#include "stdafx.h"
#include "../client/stdinc.h"
#include "ChatFrameBase.h"
#include "WinUtil.h"
#include "EmoticonsDlg.h"
#include "EmoticonsManager.h"
#include "SearchFrm.h"
#include "MainFrm.h"
#include "Players.h"
#include "ExMessageBox.h"
#include "ChatCommands.h"

#include "../client/AutoSearchManager.h"
#include "../client/HashManager.h"
#include "../client/SettingsManager.h"
#include "../client/Util.h"
#include "../client/ShareManager.h"
#include "../client/LogManager.h"
#include "../client/ShareScannerManager.h"
#include "../client/QueueManager.h"
#include "../client/ShareManager.h"
#include "../client/ClientManager.h"
#include "../client/ConnectivityManager.h"
#include "../client/ThrottleManager.h"
#include "../client/version.h"

extern EmoticonsManager* emoticonsManager;

ChatFrameBase::ChatFrameBase() : /*clientContainer(WC_EDIT, this, EDIT_MESSAGE_MAP)*/ menuItems(0),
		lineCount(1), curCommandPosition(0), cancelHashing(false), resizePressed(false) {
}

ChatFrameBase::~ChatFrameBase() { }

LRESULT ChatFrameBase::onClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& bHandled) {
	cancelHashing = true;
	tasks.wait();

	bHandled = FALSE;
	return 0;
}

LRESULT ChatFrameBase::OnForwardMsg(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/) {
	LPMSG pMsg = (LPMSG)lParam;
	if((pMsg->message >= WM_MOUSEFIRST) && (pMsg->message <= WM_MOUSELAST))
		ctrlTooltips.RelayEvent(pMsg);
	return 0;
}

void ChatFrameBase::init(HWND m_hWnd, RECT rcDefault) {
	ctrlClient.Create(m_hWnd, rcDefault, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | 
		WS_VSCROLL | ES_MULTILINE | ES_NOHIDESEL | ES_READONLY, WS_EX_CLIENTEDGE | WS_EX_ACCEPTFILES, IDC_CLIENT);


	ctrlClient.Subclass();
	ctrlClient.LimitText(0);
	ctrlClient.SetFont(WinUtil::font);

	ctrlClient.setFormatLinks(true);
	ctrlClient.setFormatReleases(true);
	ctrlClient.setAllowClear(true);
	
	//ctrlClient.SetAutoURLDetect(false);
	//ctrlClient.SetEventMask(ctrlClient.GetEventMask() | ENM_LINK);
	ctrlClient.SetBackgroundColor(WinUtil::bgColor);
	//ctrlClient.setClient(aClient);

	expandDown = ResourceLoader::loadIcon(IDI_EXPAND_DOWN, 16);
	expandUp = ResourceLoader::loadIcon(IDI_EXPAND_UP, 16);

	ctrlTooltips.Create(m_hWnd, rcDefault, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_BALLOON, WS_EX_TOPMOST);	
	ctrlTooltips.SetDelayTime(TTDT_AUTOMATIC, 600);
	ctrlTooltips.Activate(TRUE);

	if(SETTING(SHOW_MULTILINE)){
		ctrlResize.Create(m_hWnd, rcDefault, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | BS_FLAT | BS_ICON | BS_CENTER, 0, IDC_RESIZE);
		ctrlResize.SetIcon(expandUp);
		ctrlResize.SetFont(WinUtil::font);
		ctrlTooltips.AddTool(ctrlResize.m_hWnd, CTSTRING(MULTILINE_INPUT));
	}

	ctrlMessage.Create(m_hWnd, rcDefault, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_VSCROLL |
		ES_AUTOHSCROLL | ES_MULTILINE | ES_AUTOVSCROLL, WS_EX_CLIENTEDGE);
	ctrlMessage.SetFont(WinUtil::font);
	ctrlMessage.SetLimitText(9999);
	lineCount = 1; //ApexDC

	if(SETTING(SHOW_EMOTICON)){
		ctrlEmoticons.Create(m_hWnd, rcDefault, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | BS_FLAT | BS_BITMAP | BS_CENTER, 0, IDC_EMOT);
		hEmoticonBmp.LoadFromResource(IDR_EMOTICON, _T("PNG"), _Module.get_m_hInst());
		ctrlEmoticons.SetBitmap(hEmoticonBmp);
		ctrlTooltips.AddTool(ctrlEmoticons.m_hWnd, CTSTRING(INSERT_EMOTICON));
	}

	if(SETTING(SHOW_MAGNET)) {
		bool tmp = (getUser() && (!getUser()->isSet(User::BOT) && !getUser()->isSet(User::NMDC)));
		ctrlMagnet.Create(m_hWnd, rcDefault, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | BS_FLAT | BS_ICON | BS_CENTER, 0, IDC_BMAGNET);
		ctrlMagnet.SetIcon(ResourceLoader::loadIcon(IDI_MAGNET, 20));
		ctrlTooltips.AddTool(ctrlMagnet.m_hWnd, tmp ? CTSTRING(SEND_FILE_PM) : CTSTRING(SEND_FILE_HUB));
	}
}

LRESULT ChatFrameBase::onChar(UINT uMsg, WPARAM wParam, LPARAM /*lParam*/, BOOL& bHandled) {
	if(!complete.empty() && wParam != VK_TAB && uMsg == WM_KEYDOWN)
		complete.clear();

	if (uMsg != WM_KEYDOWN && uMsg != WM_CUT && uMsg != WM_PASTE) { //ApexDC
		switch(wParam) {
			case VK_RETURN:
				if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_SHIFT) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000)) {
					bHandled = FALSE;
				}
				break;
			case VK_TAB:
				bHandled = TRUE;
  				break;
			case VK_F16: //adds strange char when using ctrl+backspace
				bHandled = TRUE;
  				break;
  			default:
  				bHandled = FALSE;
				break;
		}

		 if ((uMsg == WM_CHAR) && (GetFocus() == ctrlMessage.m_hWnd) && (wParam != VK_RETURN) && (wParam != VK_TAB) && (wParam != VK_BACK)) {
			if ((!SETTING(SOUND_TYPING_NOTIFY).empty()) && (!SETTING(SOUNDS_DISABLED)))
				WinUtil::playSound(Text::toT(SETTING(SOUND_TYPING_NOTIFY)));
		}
		return 0;
	}

	if(wParam == VK_TAB) {
		onTab();
		return 0;
	} else if((GetFocus() == ctrlMessage.m_hWnd) && (GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000) && (wParam == 'A')){
		ctrlMessage.SetSelAll();
		return 0;
	} if(wParam == VK_F3) {
		ctrlClient.findText();
		return 0;
	} 

	// don't handle these keys unless the user is entering a message
	if (GetFocus() != ctrlMessage.m_hWnd) {
		bHandled = FALSE;
		return 0;
	}

	switch(wParam) {
		case VK_RETURN:
			if( (GetKeyState(VK_CONTROL) & 0x8000) || 
				(GetKeyState(VK_MENU) & 0x8000)  ||
				(GetKeyState(VK_SHIFT) & 0x8000)) {
					bHandled = FALSE;
				} else {
					onEnter();
				}
			break;
		case VK_UP:
			if ( (GetKeyState(VK_MENU) & 0x8000) ||	( ((GetKeyState(VK_CONTROL) & 0x8000) == 0) ^ (SETTING(USE_CTRL_FOR_LINE_HISTORY) == true) ) ) {
				//scroll up in chat command history
				//currently beyond the last command?
				if (curCommandPosition > 0) {
					//check whether current command needs to be saved
					if (curCommandPosition == prevCommands.size()) {
						getLineText(currentCommand);
					}

					//replace current chat buffer with current command
					ctrlMessage.SetWindowText(prevCommands[--curCommandPosition].c_str());
				}
				// move cursor to end of line
				ctrlMessage.SetSel(ctrlMessage.GetWindowTextLength(), ctrlMessage.GetWindowTextLength());
			} else {
				bHandled = FALSE;
			}

			break;
		case VK_DOWN:
			if ( (GetKeyState(VK_MENU) & 0x8000) ||	( ((GetKeyState(VK_CONTROL) & 0x8000) == 0) ^ (SETTING(USE_CTRL_FOR_LINE_HISTORY) == true) ) ) {
				//scroll down in chat command history

				//currently beyond the last command?
				if (curCommandPosition + 1 < prevCommands.size()) {
					//replace current chat buffer with current command
					ctrlMessage.SetWindowText(prevCommands[++curCommandPosition].c_str());
				} else if (curCommandPosition + 1 == prevCommands.size()) {
					//revert to last saved, unfinished command

					ctrlMessage.SetWindowText(currentCommand.c_str());
					++curCommandPosition;
				}
				// move cursor to end of line
				ctrlMessage.SetSel(ctrlMessage.GetWindowTextLength(), ctrlMessage.GetWindowTextLength());
			} else {
				bHandled = FALSE;
			}

			break;
		case VK_PRIOR: // page up
			ctrlClient.SendMessage(WM_VSCROLL, SB_PAGEUP);

			break;
		case VK_NEXT: // page down
			ctrlClient.SendMessage(WM_VSCROLL, SB_PAGEDOWN);

			break;
		case VK_HOME:
			if (!prevCommands.empty() && (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000)) {
				curCommandPosition = 0;
				
				getLineText(currentCommand);

				ctrlMessage.SetWindowText(prevCommands[curCommandPosition].c_str());
			} else {
				bHandled = FALSE;
			}

			break;
		case VK_END:
			if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000)) {
				curCommandPosition = prevCommands.size();

				ctrlMessage.SetWindowText(currentCommand.c_str());
			} else {
				bHandled = FALSE;
				}
				break;
		default:
			bHandled = FALSE;
//ApexDC
	}

	// Kinda ugly, but oh well... will clean it later, maybe...
	//if(SETTING(MAX_RESIZE_LINES) != 1) {
		int newLineCount = ctrlMessage.GetLineCount();
		int start, end;
		ctrlMessage.GetSel(start, end);
		if(wParam == VK_DELETE || uMsg == WM_CUT ||  uMsg == WM_PASTE || (wParam == VK_BACK && (start != end))) {
			if(uMsg == WM_PASTE) {
				// We don't need to hadle these
				if(!IsClipboardFormatAvailable(CF_TEXT))
					return 0;
				if(!::OpenClipboard(WinUtil::mainWnd))
					return 0;
			}
			tstring buf;
			string::size_type i;
			getLineText(buf);
			buf = buf.substr(start, end);
			while((i = buf.find(_T("\n"))) != string::npos) {
				buf.erase(i, 1);
				newLineCount--;
			}
			if(uMsg == WM_PASTE) {
				HGLOBAL hglb = GetClipboardData(CF_TEXT); 
				if(hglb != NULL) { 
					char* lptstr = (char*)GlobalLock(hglb); 
					if(lptstr != NULL) {
						string tmp(lptstr);
						// why I have to do this this way?
						for(string::size_type i = 0; i < tmp.size(); ++i) {
							if(tmp[i] == '\n') {
								newLineCount++;
								if(newLineCount >= SETTING(MAX_RESIZE_LINES))
									break;
							}
						}
						GlobalUnlock(hglb);
					}
				}
				CloseClipboard();
			}
		} else if(wParam == VK_BACK) {
			POINT cpt;
			GetCaretPos(&cpt);
			int charIndex = ctrlMessage.CharFromPos(cpt);

			if (WinUtil::isCtrl()) {
				if (charIndex == 0)
					return 0;

				tstring s;
				getLineText(s);
				tstring newLine = s;

				//trim spaces before the word
				for (int i = charIndex-1; i >= 0; --i) {
					if (!isgraph(newLine[i])) {
						newLine.erase(i, 1);
					} else {
						break;
					}
				}

				if (s[charIndex-1] != _T('\n')) {
					//always stay on the old line here...
					auto p = newLine.find_last_of(_T("\n "), charIndex);
					if (p == tstring::npos) {
						p = 0;
					} else {
						p++;
					}

					newLine = newLine.erase(p, charIndex-p);
					charIndex = charIndex - (s.length()-newLine.length());
				} else {
					//remove the empty line
					charIndex--;
					newLineCount -= 1;
				}

				ctrlMessage.SetWindowText(newLine.c_str());
				ctrlMessage.SetFocus();
				ctrlMessage.SetSel(charIndex, charIndex);
			} else {
				charIndex = LOWORD(charIndex) - ctrlMessage.LineIndex(ctrlMessage.LineFromChar(charIndex));
				if(charIndex <= 0)
					newLineCount -= 1;
			}
		} else if((WinUtil::isCtrl() || WinUtil::isShift()) && wParam == VK_RETURN) {
			newLineCount += 1;
		}

		if((!resizePressed) && newLineCount != lineCount) {
			if(lineCount >= SETTING(MAX_RESIZE_LINES) && newLineCount >= SETTING(MAX_RESIZE_LINES)) {
				lineCount = newLineCount;
			} else {
				lineCount = newLineCount;
				UpdateLayout(FALSE);
			}
		}
//End
	//}
	return 0;
}

void ChatFrameBase::getLineText(tstring& s) {
	s.resize(ctrlMessage.GetWindowTextLength());
	ctrlMessage.GetWindowText(&s[0], ctrlMessage.GetWindowTextLength() + 1);
}

LRESULT ChatFrameBase::onResize(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& bHandled) {
	resizePressed = !resizePressed;
	ctrlResize.SetIcon(resizePressed ? expandDown : expandUp);

	//resize with the button even if user has set max lines for disabling the function otherwise.
	const int maxLines = SETTING(MAX_RESIZE_LINES) <= 1 ? 2 : SETTING(MAX_RESIZE_LINES);

	int newLineCount = resizePressed ? maxLines : 0;

	if(newLineCount != lineCount) {
		lineCount = newLineCount;
		UpdateLayout(FALSE);
	}

	bHandled = FALSE;
	return 0;
}

LRESULT ChatFrameBase::onDropFiles(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/){
	HDROP drop = (HDROP)wParam;
	tstring buf;
	buf.resize(MAX_PATH);

	UINT nrFiles;
	
	nrFiles = DragQueryFile(drop, (UINT)-1, NULL, 0);
	
	StringList paths;
	for(UINT i = 0; i < nrFiles; ++i){
		if(DragQueryFile(drop, i, &buf[0], MAX_PATH)){
			if(!PathIsDirectory(&buf[0])){
				paths.push_back(Text::fromT(buf).data());
			}
		}
	}
	DragFinish(drop);

	if (!paths.empty())
		addMagnet(paths);
	return 0;
}

LRESULT ChatFrameBase::onAddMagnet(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/){
	if (!getClient())
		return 0;

	 tstring file;
	 if(WinUtil::browseFile(file, /*m_hWnd*/ 0, false) == IDOK) {
		 StringList tmp;
		 tmp.push_back(Text::fromT(file));
		 addMagnet(tmp);
	 }
	 return 0;
 }

void ChatFrameBase::addMagnet(const StringList& aPaths) {
	if(!getClient())
		return;

	//warn the user that nmdc share will be public access.
	if((getUser() && getUser()->isSet(User::NMDC))) {
		WinUtil::ShowMessageBox(SettingsManager::NMDC_MAGNET_WARN, CTSTRING(NMDC_MAGNET_WARNING));
	}

	setStatusText(aPaths.size() > 1 ? TSTRING_F(CREATING_MAGNET_FOR_X, aPaths.size()) : TSTRING_F(CREATING_MAGNET_FOR, Text::toT(aPaths.front())));
	tasks.run([=] {
		int64_t sizeLeft = 0;
		for(auto& path: aPaths)
			sizeLeft += File::getSize(path);

		tstring ret;
		int pos = 1;
		for (auto& path: aPaths) {
			TTHValue tth;
			auto size = File::getSize(path);
			try {
				HashManager::getInstance()->getFileTTH(path, size, true, tth, sizeLeft, cancelHashing, [=] (int64_t aTimeLeft, const string& aFileName) -> void {
					//update the statusbar
					if (aTimeLeft == 0)
						return;

					callAsync([=] {
						tstring status = TSTRING_F(HASHING_X_LEFT, Text::toT(aFileName) % Text::toT(Util::formatTime(aTimeLeft, true)));
						if (aPaths.size() > 1) 
							status += _T(" (") + Text::toLower(TSTRING(FILE)) + _T(" ") + Util::toStringW(pos) + _T("/") + Util::toStringW(aPaths.size()) + _T(")");
						setStatusText(status);
					});
				});
			} catch (const Exception& e) { 
				LogManager::getInstance()->message(STRING(HASHING_FAILED) + " " + e.getError(), LogManager::LOG_ERROR);
				return;
			}

			if (!cancelHashing) {
				ShareManager::getInstance()->addTempShare(ctrlClient.getTempShareKey(), tth, path, size, getClient()->getShareProfile());
			}
			if (!ret.empty())
				ret += _T(" ");
			ret += Text::toT(WinUtil::makeMagnet(tth, Util::getFileName(path), size));
			pos++;
		}

		callAsync([=] {
			if (!cancelHashing) {
				setStatusText(aPaths.size() > 1 ? TSTRING_F(MAGNET_CREATED_FOR_X, aPaths.size()) : TSTRING_F(MAGNET_CREATED_FOR, Text::toT(aPaths.front())));
				appendTextLine(ret, true);
			}
		});
	});

}

void ChatFrameBase::setStatusText(const tstring& aLine) {
	ctrlStatus.SetText(0, (_T("[") + Text::toT(Util::getShortTimeString()) + _T("] ") + aLine).c_str());
}

LRESULT ChatFrameBase::onWinampSpam(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	tstring cmd, param, message, status;
	bool thirdPerson=false;
	if(SETTING(MEDIA_PLAYER) == 0) {
		cmd = _T("/winamp");
	} else if(SETTING(MEDIA_PLAYER) == 1) {
		cmd = _T("/itunes");
	} else if(SETTING(MEDIA_PLAYER) == 2) {
		cmd = _T("/mpc");
	} else if(SETTING(MEDIA_PLAYER) == 3) {
		cmd = _T("/wmp");
	} else if(SETTING(MEDIA_PLAYER) == 4) {
		cmd = _T("/spotify");
	} else {
		addStatusLine(CTSTRING(NO_MEDIA_SPAM));
		return 0;
	}
	if(checkCommand(cmd, param, message, status, thirdPerson)){
		if(!message.empty()) {
			sendFrameMessage(message, thirdPerson);
		}
		if(!status.empty()) {
			addStatusLine(status);
		}
	}
	return 0;
}

bool ChatFrameBase::sendFrameMessage(const tstring& aMsg, bool thirdPerson /*false*/) {
	if (!aMsg.empty()) {
		string error;
		if (sendMessage(aMsg, error, thirdPerson)) {
			return true;
		} else {
			addStatusLine(Text::toT(error));
		}
	}
	return false;
}

void ChatFrameBase::appendTextLine(const tstring& aText, bool addSpace) {
	tstring s;
	getLineText(s);
	if (addSpace && s.length() > 0 && !isspace(s.back()))
		s += _T(" ");

	ctrlMessage.SetWindowText((s + aText).c_str());
	ctrlMessage.SetFocus();
	ctrlMessage.SetSel( ctrlMessage.GetWindowTextLength(), ctrlMessage.GetWindowTextLength() );
}

LRESULT ChatFrameBase::onEmoticons(WORD /*wNotifyCode*/, WORD /*wID*/, HWND hWndCtl, BOOL& bHandled) {
	if (hWndCtl != ctrlEmoticons.m_hWnd) {
		bHandled = false;
		return 0;
	}

	EmoticonsDlg dlg;
	ctrlEmoticons.GetWindowRect(dlg.pos);
	dlg.DoModal(hWndCtl);
	if (!dlg.result.empty()) {
		appendTextLine(dlg.result, false);
	}
	return 0;
}

LRESULT ChatFrameBase::onEmoPackChange(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	TCHAR buf[256];
	emoMenu.GetMenuString(wID, buf, 256, MF_BYCOMMAND);
	if (buf != Text::toT(SETTING(EMOTICONS_FILE))) {
		SettingsManager::getInstance()->set(SettingsManager::EMOTICONS_FILE, Text::fromT(buf));
		emoticonsManager->Unload();
		emoticonsManager->Load();
	}
	return 0;
}

LRESULT ChatFrameBase::onContextMenu(UINT /*uMsg*/, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
	POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };        // location of mouse click
		
	if(reinterpret_cast<HWND>(wParam) == ctrlEmoticons) {
		if(emoMenu.m_hMenu) {
			emoMenu.DestroyMenu();
			emoMenu.m_hMenu = NULL;
		}
		emoMenu.CreatePopupMenu();
		menuItems = 0;
		emoMenu.InsertSeparatorFirst(_T("Emoticons Pack"));
		emoMenu.AppendMenu(MF_STRING, IDC_EMOMENU, _T("Disabled"));
		if (SETTING(EMOTICONS_FILE)=="Disabled") emoMenu.CheckMenuItem( IDC_EMOMENU, MF_BYCOMMAND | MF_CHECKED );
		// nacteme seznam emoticon packu (vsechny *.xml v adresari EmoPacks)
		WIN32_FIND_DATA data;
		HANDLE hFind;
		hFind = FindFirstFile(Text::toT(Util::getPath(Util::PATH_EMOPACKS) + "*.xml").c_str(), &data);
		if(hFind != INVALID_HANDLE_VALUE) {
			do {
				tstring name = data.cFileName;
				tstring::size_type i = name.rfind('.');
				name = name.substr(0, i);

				menuItems++;
				emoMenu.AppendMenu(MF_STRING, IDC_EMOMENU + menuItems, name.c_str());
				if(name == Text::toT(SETTING(EMOTICONS_FILE))) emoMenu.CheckMenuItem( IDC_EMOMENU + menuItems, MF_BYCOMMAND | MF_CHECKED );
			} while(FindNextFile(hFind, &data));
			FindClose(hFind);
		}

		emoMenu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, ctrlClient.m_hWnd);
		bHandled = TRUE;
		return 1;
	}

	return 0; 
}

void ChatFrameBase::onEnter() {
	tstring message;
	tstring status;
	bool thirdPerson = false;

	if(resizePressed && !WinUtil::isShift()) { //Shift + Enter to send
		ctrlMessage.AppendText(_T("\r\n"));
		return;
	}

	if(ctrlMessage.GetWindowTextLength() > 0) {
		tstring s;
		getLineText(s);

		// save command in history, reset current buffer pointer to the newest command
		curCommandPosition = prevCommands.size();		//this places it one position beyond a legal subscript
		if (!curCommandPosition || curCommandPosition > 0 && prevCommands[curCommandPosition - 1] != s) {
			++curCommandPosition;
			prevCommands.push_back(s);
		}
		currentCommand = Util::emptyStringT;

		// Special command
		if(s[0] == _T('/')) {
			tstring cmd = s;
			tstring param;
			if(SETTING(CLIENT_COMMANDS)) {
				addStatusLine(_T("Client command: ") + s);
			}
		
			if(!checkCommand(cmd, param, message, status, thirdPerson)) {
				if (SETTING(SEND_UNKNOWN_COMMANDS)) {
					message = s;
				} else {
					status = TSTRING(UNKNOWN_COMMAND) + _T(" ") + cmd;
				}
			}
		} else {
			if(SETTING(SERVER_COMMANDS)) {
				if(s[0] == '!' || s[0] == '+' || s[0] == '-')
					status = _T("Server command: ") + s;
			}

			message = s;
		}
	} else {
		MessageBeep(MB_ICONEXCLAMATION);
		return;
	}

	if (!status.empty()) {
		addStatusLine(status);
	}

	if (!message.empty()) {
		if (sendFrameMessage(message, thirdPerson))
			ctrlMessage.SetWindowText(Util::emptyStringT.c_str());
	} else {
		ctrlMessage.SetWindowText(Util::emptyStringT.c_str());
	}
}

#ifdef BETAVER
#define LINE2 _T("-- http://www.airdcpp.net  <AirDC++ ") _T(VERSIONSTRING) _T("r") _T(SVNVERSION) _T(" / ") _T(DCVERSIONSTRING) _T(">")
#else
#define LINE2 _T("-- http://www.airdcpp.net  <AirDC++ ") _T(VERSIONSTRING) _T(" / ") _T(DCVERSIONSTRING) _T(">")
#endif

static TCHAR *msgs[] = { _T("\r\n-- I'm a happy AirDC++ user. You could be happy too.\r\n") LINE2,
_T("\r\n-- Is it Superman? No, its AirDC++\r\n") LINE2,
_T("\r\n-- My files are burning in my computer...Download are way too fast\r\n") LINE2,
_T("\r\n-- STOP!! My client is too fast, slow down with the writings\r\n") LINE2,
_T("\r\n-- We are always behind the corner waiting to grab your nuts..i mean files\r\n") LINE2,
_T("\r\n-- Knock, knock....We are here to take your files again, we needed backup files too\r\n") LINE2,
_T("\r\n-- Why bother searching when AirDC++ can take care of everything?\r\n") LINE2,
_T("\r\n-- The only way to stop me to getting your files is closing the DC++\r\n") LINE2,
_T("\r\n-- We love your files soo much, so we try to get them over and over again...\r\n") LINE2,
_T("\r\n-- Let us thru the waiting line, we download faster than the lightning\r\n") LINE2,
_T("\r\n-- Sometimes we download so fast that we accidently get the whole person on the other side...\r\n") LINE2,
_T("\r\n-- Holy crap, my download speed is sooooo fast that it made a hole in the harddrive\r\n") LINE2,
_T("\r\n-- Once you got access to it, dont let it go...\r\n") LINE2,
_T("\r\n-- Do you feel the wind? Its the download that goes too fast..\r\n") LINE2,
_T("\r\n-- No matter what, no matter where, it's always home if AirDC++ is there\r\n") LINE2,
_T("\r\n-- Knock, knock...We are leaving back the trousers we accidently downloaded...\r\n") LINE2,
_T("\r\n-- Are you blind? you already downloaded that movie 4 times already\r\n") LINE2,
_T("\r\n-- My client has been in jail twice, has  yours?\r\n") LINE2,
_T("\r\n-- Keep your downloads close, but keep your uploads even closer\r\n") LINE2


};

#define MSGS 18

tstring ChatFrameBase::commands = Text::toT("\n\t\t\t\t\tHELP\n\
------------------------------------------------------------------------------------------------------------------------------------------------------------\n\
/refresh\t\t\t\t\tRefresh share\n\
/optimizedb\t\t\t\tRemove unused entries from the hash databases\n\
/verifydb\t\t\t\t\tOptimize and verify the integrity of the hash databases\n\
/savequeue\t\t\t\tSave download queue\n\
/stop\t\t\t\t\tStop SFV check\n\
/sharestats\t\t\t\tShow general share statistics\n\
/dbstats\t\t\t\t\tShow techical statistics about the hash database backend\n\
/monitordebug\t\t\t\tShow each change notification for monitored directories\n\
------------------------------------------------------------------------------------------------------------------------------------------------------------\n\
/search <string>\t\t\t\tSearch for...\n\
/whois [IP]\t\t\t\tFind info about user from the IP address\n\
------------------------------------------------------------------------------------------------------------------------------------------------------------\n\
/slots # \t\t\t\t\tUpload slots\n\
/extraslots # \t\t\t\tSet extra slots\n\
/smallfilesize # \t\t\t\tSet smallfile size\n\
/ts \t\t\t\t\tShow timestamp in mainchat\n\
/connection \t\t\t\tShow connection settings, IP & ports\n\
/showjoins \t\t\t\tShow user joins in mainchat\n\
/shutdown \t\t\t\tSystem shutdown\n\
------------------------------------------------------------------------------------------------------------------------------------------------------------\n\
/AirDC++ \t\t\t\t\tShow AirDC++ version in mainchat\n\
------------------------------------------------------------------------------------------------------------------------------------------------------------\n\
/away <msg>\t\t\t\tSet away message\n\
/winamp, /w\t\t\t\tShows Winamp spam in mainchat\n\
/spotify, /s\t\t\t\tShows Spotify spam in mainchat\n\
/itunes\t\t\t\t\tShows iTunes spam in mainchat\n\
/wmp\t\t\t\t\tShows Windows Media Player spam in mainchat\n\
/mpc\t\t\t\t\tShows Media Player Classic spam in mainchat\n\
/clear,/c\t\t\t\t\tClears chat\n\
/speed\t\t\t\t\tShow download/upload speeds in mainchat\n\
/stats\t\t\t\t\tShow stats in mainchat\n\
/clientstats\t\t\t\tShow general statistics about the clients in all hubs\n\
/prvstats\t\t\t\t\tView stats visible only to yourself\n\
/info\t\t\t\t\tView system info visible only to yourself\n\
/log system\t\t\t\tOpen system log\n\
/log downloads \t\t\t\tOpen downloads log\n\
/log uploads\t\t\t\tOpen uploads log\n\
/df \t\t\t\t\tShow disk space info (only visible to yourself)\n\
/dfs \t\t\t\t\tShow disk space info (as public message)\n\
/disks, /di \t\t\t\tShow detailed disk info about all mounted disks\n\
/uptime \t\t\t\t\tShow uptime info\n\
/topic\t\t\t\t\tShow topic\n\
/ctopic\t\t\t\t\tOpen link in topic\n\
/ratio, /r\t\t\t\t\tShow ratio in chat\n\
/close, /r\t\t\t\t\tClose the current tab\n\n");

string ChatFrameBase::getAwayMessage() {
	return ctrlClient.getClient() ? ctrlClient.getClient()->get(HubSettings::AwayMsg) : SETTING(DEFAULT_AWAY_MESSAGE);
}

bool ChatFrameBase::checkCommand(tstring& cmd, tstring& param, tstring& message, tstring& status, bool& thirdPerson) {
	string::size_type i = cmd.find(' ');
	if(i != string::npos) {
		param = cmd.substr(i+1);
		cmd = cmd.substr(1, i - 1);
	} else {
		cmd = cmd.substr(1);
	}

	if(stricmp(cmd.c_str(), _T("log")) == 0) {
		if(stricmp(param.c_str(), _T("system")) == 0) {
			WinUtil::openFile(Text::toT(LogManager::getInstance()->getPath(LogManager::SYSTEM)));
		} else if(stricmp(param.c_str(), _T("downloads")) == 0) {
			WinUtil::openFile(Text::toT(LogManager::getInstance()->getPath(LogManager::DOWNLOAD)));
		} else if(stricmp(param.c_str(), _T("uploads")) == 0) {
			WinUtil::openFile(Text::toT(LogManager::getInstance()->getPath(LogManager::UPLOAD)));
		} else {
			return false;
		}
	} else if(stricmp(cmd.c_str(), _T("stop")) == 0) {
		ShareScannerManager::getInstance()->Stop();
	} else if(stricmp(cmd.c_str(), _T("me")) == 0) {
		message = param;
		thirdPerson = true;
	} else if((stricmp(cmd.c_str(), _T("ratio")) == 0) || (stricmp(cmd.c_str(), _T("r")) == 0)) {
		char ratio[512];
		//thirdPerson = true;
		snprintf(ratio, sizeof(ratio), "Ratio: %s (Uploaded: %s | Downloaded: %s)",
			(SETTING(TOTAL_DOWNLOAD) > 0) ? Util::toString(((double)SETTING(TOTAL_UPLOAD)) / ((double)SETTING(TOTAL_DOWNLOAD))).c_str() : "inf.",
			Util::formatBytes(SETTING(TOTAL_UPLOAD)).c_str(),  Util::formatBytes(SETTING(TOTAL_DOWNLOAD)).c_str());
		message = Text::toT(ratio);

	} else if(stricmp(cmd.c_str(), _T("refresh"))==0) {
		//refresh path
		try {
			if(!param.empty()) {
				if(stricmp(param.c_str(), _T("incoming"))==0) {
					ShareManager::getInstance()->refresh(true, ShareManager::TYPE_MANUAL);
				} else if( ShareManager::REFRESH_PATH_NOT_FOUND == ShareManager::getInstance()->refresh( Text::fromT(param) ) ) {
					status = TSTRING(DIRECTORY_NOT_FOUND);
				}
			} else {
				ShareManager::getInstance()->refresh(false, ShareManager::TYPE_MANUAL);
			}
		} catch(const ShareException& e) {
			status = Text::toT(e.getError());
		}
	} else if(stricmp(cmd.c_str(), _T("slots"))==0) {
		int j = Util::toInt(Text::fromT(param));
		if(j > 0) {
			SettingsManager::getInstance()->set(SettingsManager::SLOTS, j);
			status = TSTRING(SLOTS_SET);
			ClientManager::getInstance()->infoUpdated();
		} else {
			status = TSTRING(INVALID_NUMBER_OF_SLOTS);
		}
	} else if(stricmp(cmd.c_str(), _T("search")) == 0) {
		if(!param.empty()) {
			SearchFrame::openWindow(param);
		} else {
			status = TSTRING(SPECIFY_SEARCH_STRING);
		}
	} else if(stricmp(cmd.c_str(), _T("airdc++")) == 0) {
		message = msgs[GET_TICK() % MSGS];
	} /*else if(stricmp(cmd.c_str(), _T("save")) == 0) {
		ShareManager::getInstance()->save();
	} */ else if(stricmp(cmd.c_str(), _T("calcprio")) == 0) {
		QueueManager::getInstance()->calculateBundlePriorities(true);
	} else if(stricmp(cmd.c_str(), _T("generatelist")) == 0) {
		ShareManager::getInstance()->generateOwnList(0);
	} else if(stricmp(cmd.c_str(), _T("as")) == 0) {
		AutoSearchManager::getInstance()->runSearches();
	} else if(stricmp(cmd.c_str(), _T("clientstats")) == 0) {
		status = Text::toT(ClientManager::getInstance()->getClientStats());
	} else if(stricmp(cmd.c_str(), _T("monitordebug")) == 0) {
		auto cur = ShareManager::getInstance()->getMonitorDebug();
		ShareManager::getInstance()->setMonitorDebug(!cur);
		status = cur ? _T("Debug disabled") : _T("Debug enabled");
	} else if(stricmp(cmd.c_str(), _T("handlechanges")) == 0) {
		ShareManager::getInstance()->handleChangedFiles();
	} else if(stricmp(cmd.c_str(), _T("compact")) == 0) {
		MainFrame::getMainFrame()->addThreadedTask([this] { HashManager::getInstance()->compact(); });
	} else if(stricmp(cmd.c_str(), _T("allow")) == 0) {
		if(!param.empty()) {
			QueueManager::getInstance()->shareBundle(Text::fromT(param));
		} else {
			status = _T("Please specify the bundle name!");
		}
	} else if(stricmp(cmd.c_str(), _T("setlistdirty")) == 0) {
		auto profiles = ShareManager::getInstance()->getProfiles();
		ProfileTokenSet pts;
		for(auto& sp: profiles)
			pts.insert(sp->getToken());

		ShareManager::getInstance()->setProfilesDirty(pts, true);
	} else if(stricmp(cmd.c_str(), _T("away")) == 0) {
		if(AirUtil::getAway()) {
			AirUtil::setAway(AWAY_OFF);
			MainFrame::setAwayButton(false);
			status = TSTRING(AWAY_MODE_OFF);
		} else {
			AirUtil::setAway(AWAY_MANUAL);
			MainFrame::setAwayButton(true);
			
			ParamMap sm;
			status = TSTRING(AWAY_MODE_ON) + _T(" ") + Text::toT(AirUtil::getAwayMessage(getAwayMessage(), sm));
		}
	} else if(WebShortcuts::getInstance()->getShortcutByKey(cmd) != NULL) {
		WinUtil::searchSite(WebShortcuts::getInstance()->getShortcutByKey(cmd), Text::fromT(param), false);
	} else if(stricmp(cmd.c_str(), _T("u")) == 0) {
		if (!param.empty()) {
			WinUtil::openLink(Text::toT(Util::encodeURI(Text::fromT(param))));
		}
	} else if(stricmp(cmd.c_str(), _T("optimizedb")) == 0) {
		HashManager::getInstance()->startMaintenance(false);
	} else if(stricmp(cmd.c_str(), _T("verifydb")) == 0) {
		HashManager::getInstance()->startMaintenance(true);
	} else if(stricmp(cmd.c_str(), _T("shutdown")) == 0) {
		MainFrame::setShutDown(!(MainFrame::getShutDown()));
		if (MainFrame::getShutDown()) {
			status = TSTRING(SHUTDOWN_ON);
		} else {
			status = TSTRING(SHUTDOWN_OFF);
		}
	} else if((stricmp(cmd.c_str(), _T("disks")) == 0) || (stricmp(cmd.c_str(), _T("di")) == 0)) {
		status = ChatCommands::diskInfo();
	} else if(stricmp(cmd.c_str(), _T("stats")) == 0) {
		message = Text::toT(ChatCommands::generateStats());
	} else if(stricmp(cmd.c_str(), _T("prvstats")) == 0) {
		status = Text::toT(ChatCommands::generateStats());
	} else if(stricmp(cmd.c_str(), _T("dbstats")) == 0) {
		status = Text::toT(HashManager::getInstance()->getDbStats());
	} else if(stricmp(cmd.c_str(), _T("sharestats")) == 0) {
		status = Text::toT(ShareManager::getInstance()->getStats());
	} else if(stricmp(cmd.c_str(), _T("speed")) == 0) {
		status = ChatCommands::Speedinfo();
	} else if(stricmp(cmd.c_str(), _T("info")) == 0) {
		status = ChatCommands::UselessInfo();
	} else if(stricmp(cmd.c_str(), _T("df")) == 0) {
		status = ChatCommands::DiskSpaceInfo();
	} else if(stricmp(cmd.c_str(), _T("dfs")) == 0) {
		message = ChatCommands::DiskSpaceInfo();
	} else if(stricmp(cmd.c_str(), _T("uptime")) == 0) {
		message = Text::toT(ChatCommands::uptimeInfo());
	} else if(stricmp(cmd.c_str(), _T("f")) == 0) {
		ctrlClient.findText();
	} else if(stricmp(cmd.c_str(), _T("whois")) == 0) {
		WinUtil::openLink(_T("http://www.ripe.net/perl/whois?form_type=simple&full_query_string=&searchtext=") + Text::toT(Util::encodeURI(Text::fromT(param))));
	} else if((stricmp(cmd.c_str(), _T("clear")) == 0) || (stricmp(cmd.c_str(), _T("cls")) == 0)) {
		ctrlClient.SetWindowText(Util::emptyStringT.c_str());
	} else if(Util::stricmp(cmd.c_str(), _T("conn")) == 0 || Util::stricmp(cmd.c_str(), _T("connection")) == 0) {
		status = Text::toT(ConnectivityManager::getInstance()->getInformation());
	} else if(stricmp(cmd.c_str(), _T("extraslots"))==0) {
		int j = Util::toInt(Text::fromT(param));
		if(j > 0) {
			SettingsManager::getInstance()->set(SettingsManager::EXTRA_SLOTS, j);
			status = TSTRING(EXTRA_SLOTS_SET);
		} else {
			status = TSTRING(INVALID_NUMBER_OF_SLOTS);
		}
	} else if(stricmp(cmd.c_str(), _T("smallfilesize"))==0) {
		int j = Util::toInt(Text::fromT(param));
		if(j >= 64) {
			SettingsManager::getInstance()->set(SettingsManager::SET_MINISLOT_SIZE, j);
			status = TSTRING(SMALL_FILE_SIZE_SET);
		} else {
			status = TSTRING(INVALID_SIZE);
		}
	} else if(Util::stricmp(cmd.c_str(), _T("upload")) == 0) {
		auto value = Util::toInt(Text::fromT(param));
		ThrottleManager::setSetting(ThrottleManager::getCurSetting(SettingsManager::MAX_UPLOAD_SPEED_MAIN), value);
		status = value ? CTSTRING_F(UPLOAD_LIMIT_SET_TO, value) : CTSTRING(UPLOAD_LIMIT_DISABLED);
	} else if(Util::stricmp(cmd.c_str(), _T("download")) == 0) {
		auto value = Util::toInt(Text::fromT(param));
		ThrottleManager::setSetting(ThrottleManager::getCurSetting(SettingsManager::MAX_DOWNLOAD_SPEED_MAIN), value);
		status = value ? CTSTRING_F(DOWNLOAD_LIMIT_SET_TO, value) : CTSTRING(DOWNLOAD_LIMIT_DISABLED);
	} else if(stricmp(cmd.c_str(), _T("wmp")) == 0) { // Mediaplayer Support
		string spam = Players::getWMPSpam(FindWindow(_T("WMPlayerApp"), NULL));
		if(!spam.empty()) {
			if(spam != "no_media") {
				message = Text::toT(spam);
			} else {
				status = _T("You have no media playing in Windows Media Player");
			}
		} else {
			status = _T("Supported version of Windows Media Player is not running");
		}

	} else if((stricmp(cmd.c_str(), _T("spotify")) == 0) || (stricmp(cmd.c_str(), _T("s")) == 0)) {
		string spam = Players::getSpotifySpam(FindWindow(_T("SpotifyMainWindow"), NULL));
		if(!spam.empty()) {
			if(spam != "no_media") {
				message = Text::toT(spam);
			} else {
				status = _T("You have no media playing in Spotify");
			}
		} else {
			status = _T("Supported version of Spotify is not running");
		}

	} else if(stricmp(cmd.c_str(), _T("itunes")) == 0) {
		string spam = Players::getItunesSpam(FindWindow(_T("iTunes"), _T("iTunes")));
		if(!spam.empty()) {
			if(spam != "no_media") {
				message = Text::toT(spam);
			} else {
				status = _T("You have no media playing in iTunes");
			}
		} else {
			status = _T("Supported version of iTunes is not running");
		}
	} else if(stricmp(cmd.c_str(), _T("mpc")) == 0) {
		string spam = Players::getMPCSpam();
		if(!spam.empty()) {
			message = Text::toT(spam);
		} else {
			status = _T("Supported version of Media Player Classic is not running");
		}
	} else if((stricmp(cmd.c_str(), _T("winamp")) == 0) || (stricmp(cmd.c_str(), _T("w")) == 0)) {
		string spam = Players::getWinAmpSpam();
		if (!spam.empty()) {
			message = Text::toT(spam);
		} else {
			status = _T("Supported version of Winamp is not running");
		}
	} else {
		return checkFrameCommand(cmd, param, message, status, thirdPerson);
	}

	//check if /me was added by the command
	i = message.find(' ');
	if(i != string::npos && message.substr(1, i - 1) == _T("me")) {
		message = message.substr(i+1);
		thirdPerson = true;
	}
	return true;
}