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

#include "stdafx.h"
#include "Resource.h"
#include "../client/DCPlusPlus.h"
#include "../client/FavoriteManager.h"
#include "../client/UploadManager.h"

#include "ChatCtrl.h"
#include "EmoticonsManager.h"
#include "PrivateFrame.h"
#include "atlstr.h"
#include "MainFrm.h"
#include "IgnoreManager.h"
#include "TextFrame.h"
#include "../client/highlightmanager.h"
#include "../client/AutoSearchManager.h"
#include "ResourceLoader.h"
#include "../client/Magnet.h"
EmoticonsManager* emoticonsManager = NULL;

#define MAX_EMOTICONS 48

ChatCtrl::ChatCtrl() : ccw(_T("edit"), this), client(NULL), m_bPopupMenu(false), autoScrollToEnd(true) {
	if(emoticonsManager == NULL) {
		emoticonsManager = new EmoticonsManager();
	}
	regReleaseBoost.assign(Text::toT(AirUtil::getReleaseRegLong(true)));
	regUrlBoost.assign(_T("(((?:[a-z][\\w-]{0,10})?:/{1,3}|www\\d{0,3}[.]|magnet:\\?[^\\s=]+=|spotify:|[a-z0-9.\\-]+[.][a-z]{2,4}/)(?:[^\\s()<>]+|\\(([^\\s()<>]+|(\\([^\\s()<>]+\\)))*\\))+(?:\\(([^\\s()<>]+|(\\([^\\s()<>]+\\)))*\\)|[^\\s`()\\[\\]{};:'\".,<>?«»]))"), boost::regex_constants::icase);
	showHandCursor=false;
	lastTick = GET_TICK();
	emoticonsManager->inc();
	t_height = WinUtil::getTextHeight(m_hWnd, WinUtil::font); //? right height?

	arrowCursor = LoadCursor(NULL, MAKEINTRESOURCE(IDC_ARROW));
	handCursor = LoadCursor(NULL, MAKEINTRESOURCE(IDC_HAND));
}

ChatCtrl::~ChatCtrl() {
	//shortLinks.clear();//ApexDC
	if(emoticonsManager->unique()) {
		emoticonsManager->dec();
		emoticonsManager = NULL;
	} else {
		emoticonsManager->dec();
	}
}

void ChatCtrl::AppendText(const Identity& i, const tstring& sMyNick, const tstring& sTime, tstring sMsg, CHARFORMAT2& cf, bool bUseEmo/* = true*/) {
	SetRedraw(FALSE);

	SCROLLINFO si = { 0 };
	POINT pt = { 0 };

	si.cbSize = sizeof(si);
	si.fMask = SIF_PAGE | SIF_RANGE | SIF_POS;
	GetScrollInfo(SB_VERT, &si);
	GetScrollPos(&pt);

	LONG lSelBegin = 0, lSelEnd = 0, lTextLimit = 0, lNewTextLen = 0;
	LONG lSelBeginSaved, lSelEndSaved;

	// Unify line endings
	tstring::size_type j = 0; 
	while((j = sMsg.find(_T("\r"), j)) != tstring::npos)
		sMsg.erase(j, 1);

	GetSel(lSelBeginSaved, lSelEndSaved);
	lSelEnd = lSelBegin = GetTextLengthEx(GTL_NUMCHARS);

	bool isMyMessage = i.getUser() == ClientManager::getInstance()->getMe();
	tstring sLine = sTime + sMsg;

	// Remove old chat if size exceeds
	lNewTextLen = sLine.size();
	lTextLimit = GetLimitText();

	if(lSelEnd + lNewTextLen > lTextLimit) {
		LONG lRemoveChars = 0;
		int multiplier = 1;

		if(lNewTextLen >= lTextLimit) {
			lRemoveChars = lSelEnd;
			//shortLinks.clear();
		} else {
			while(lRemoveChars < lNewTextLen)
				lRemoveChars = LineIndex(LineFromChar(multiplier++ * lTextLimit / 10));
		}

		//remove old links (the list must be in the same order than in text)
		for(auto i = links.begin(); i != links.end();) {
			if ((*i).first.cpMin < lRemoveChars) {
				links.erase(i);
				i = links.begin();
			} else {
				//update the position
				(*i).first.cpMin -= lRemoveChars;
				(*i).first.cpMax -= lRemoveChars;
				++i;
			}
		}

		// Update selection ranges
		lSelEnd = lSelBegin -= lRemoveChars;
		lSelEndSaved -= lRemoveChars;
		lSelBeginSaved -= lRemoveChars;

		// ...and the scroll position
		pt.y -= PosFromChar(lRemoveChars).y;

		SetSel(0, lRemoveChars);
		ReplaceSel(_T(""));
	}

	// Add to the end
	SetSel(lSelBegin, lSelEnd);
	setText(sLine);

	CHARFORMAT2 enc;
	enc.bCharSet = RUSSIAN_CHARSET;
	enc.dwMask = CFM_CHARSET;

	SetSel(0, sLine.length());
	SetSelectionCharFormat(enc);


	// Format TimeStamp
	if(!sTime.empty()) {
		lSelEnd += sTime.size();
		SetSel(lSelBegin, lSelEnd - 1);
		SetSelectionCharFormat(WinUtil::m_TextStyleTimestamp);

		PARAFORMAT2 pf;
		memzero(&pf, sizeof(PARAFORMAT2));
		pf.dwMask = PFM_STARTINDENT; 
		pf.dxStartIndent = 0;
		SetParaFormat(pf);
	}

	// Authors nick
	tstring sAuthor = Text::toT(i.getNick());
	if(!sAuthor.empty()) {
		LONG iLen = (sMsg[0] == _T('*')) ? 1 : 0;
		LONG iAuthorLen = sAuthor.size() + 1;
		sMsg.erase(0, iAuthorLen + iLen);
   		
		lSelBegin = lSelEnd;
		lSelEnd += iAuthorLen + iLen;
		
		if(isMyMessage) {
			SetSel(lSelBegin, lSelBegin + iLen + 1);
			SetSelectionCharFormat(WinUtil::m_ChatTextMyOwn);
			SetSel(lSelBegin + iLen + 1, lSelBegin + iLen + iAuthorLen);
			SetSelectionCharFormat(WinUtil::m_TextStyleMyNick);
		} else {
			bool isFavorite = FavoriteManager::getInstance()->isFavoriteUser(i.getUser());

			//if(isFavorite || i.isOp()) {
				SetSel(lSelBegin, lSelBegin + iLen + 1);
				SetSelectionCharFormat(cf);
				SetSel(lSelBegin + iLen + 1, lSelEnd);
				if(isFavorite){
					SetSelectionCharFormat(WinUtil::m_TextStyleFavUsers);
				} else if(i.isOp()) {
					SetSelectionCharFormat(WinUtil::m_TextStyleOPs);
				} else {
					SetSelectionCharFormat(WinUtil::m_TextStyleNormUsers);
				}
			//} else {
			//	SetSel(lSelBegin, lSelEnd);
			//	SetSelectionCharFormat(cf);
            //}
		}
	} else {
		bool thirdPerson = false;
        switch(sMsg[0]) {
			case _T('*'):
				if(sMsg[1] != _T(' ')) break;
				thirdPerson = true;
            case _T('<'):
				tstring::size_type iAuthorLen = sMsg.find(thirdPerson ? _T(' ') : _T('>'), thirdPerson ? 2 : 1);
				if(iAuthorLen != tstring::npos) {
                    bool isOp = false, isFavorite = false;

                    
						tstring nick(sMsg.c_str() + 1);
						nick.erase(iAuthorLen - 1);
						if(client != NULL) {
						const OnlineUserPtr ou = client->findUser(Text::fromT(nick));
						if(ou != NULL) {
							isFavorite = FavoriteManager::getInstance()->isFavoriteUser(ou->getUser());
							isOp = ou->getIdentity().isOp();
						}
                    }
                    
					lSelBegin = lSelEnd;
					lSelEnd += iAuthorLen;
					sMsg.erase(0, iAuthorLen);

        			//if(isFavorite || isOp) {
        				SetSel(lSelBegin, lSelBegin + 1);
        				SetSelectionCharFormat(cf);
						SetSel(lSelBegin + 1, lSelEnd);
						if(isFavorite){
							SetSelectionCharFormat(WinUtil::m_TextStyleFavUsers);
						} else if(isOp) {
							SetSelectionCharFormat(WinUtil::m_TextStyleOPs);
						} else {
							SetSelectionCharFormat(WinUtil::m_TextStyleNormUsers);
						}
        			//} else {
        			//	SetSel(lSelBegin, lSelEnd);
        			//	SetSelectionCharFormat(cf);
                    //}
				}
        }
	}

	// Format the message part
	FormatChatLine(sMyNick, sMsg, cf, isMyMessage, sAuthor, lSelEnd, bUseEmo);

	SetSel(lSelBeginSaved, lSelEndSaved);
	if(	isMyMessage || ((si.nPage == 0 || (size_t)si.nPos >= (size_t)si.nMax - si.nPage - 5) &&
		(lSelBeginSaved == lSelEndSaved || !selectedUser.empty() || !selectedIP.empty())))
	{
		PostMessage(EM_SCROLL, SB_BOTTOM, 0);
	} else {
		SetScrollPos(&pt);
	}

	// Force window to redraw
	SetRedraw(TRUE);
	InvalidateRect(NULL);
}

void ChatCtrl::FormatChatLine(const tstring& sMyNick, tstring& sText, CHARFORMAT2& cf, bool isMyMessage, const tstring& sAuthor, LONG lSelBegin, bool bUseEmo) {
	// Set text format
	tstring sMsgLower(sText.length(), NULL);
	std::transform(sText.begin(), sText.end(), sMsgLower.begin(), _totlower);

	LONG lSelEnd = lSelBegin + sText.size();
	SetSel(lSelBegin, lSelEnd);
	SetSelectionCharFormat(isMyMessage ? WinUtil::m_ChatTextMyOwn : cf);
	
	// highlight all occurences of my nick
	long lMyNickStart = -1, lMyNickEnd = -1;
	size_t lSearchFrom = 0;	
	tstring sNick(sMyNick.length(), NULL);
	std::transform(sMyNick.begin(), sMyNick.end(), sNick.begin(), _totlower);

	bool found = false;
	while((lMyNickStart = (long)sMsgLower.find(sNick, lSearchFrom)) != tstring::npos) {
		lMyNickEnd = lMyNickStart + (long)sNick.size();
		SetSel(lSelBegin + lMyNickStart, lSelBegin + lMyNickEnd);
		SetSelectionCharFormat(WinUtil::m_TextStyleMyNick);
		lSearchFrom = lMyNickEnd;
		found = true;
	}
	
	if(found) {
		if(	!SETTING(CHATNAMEFILE).empty() && !BOOLSETTING(SOUNDS_DISABLED) &&
			!sAuthor.empty() && (stricmp(sAuthor.c_str(), sNick) != 0)) {
				::PlaySound(Text::toT(SETTING(CHATNAMEFILE)).c_str(), NULL, SND_FILENAME | SND_ASYNC);	 	
        }
		if(BOOLSETTING(FLASH_WINDOW_ON_MYNICK) 
			&& !sAuthor.empty() && (stricmp(sAuthor.c_str(), sNick) != 0))
					WinUtil::FlashWindow();
	}

	// highlight all occurences of favourite users' nicks
	FavoriteManager::FavoriteMap ul = FavoriteManager::getInstance()->getFavoriteUsers();
	for(FavoriteManager::FavoriteMap::const_iterator i = ul.begin(); i != ul.end(); ++i) {
		const FavoriteUser& pUser = i->second;

		lSearchFrom = 0;
		sNick = Text::toT(pUser.getNick());
		std::transform(sNick.begin(), sNick.end(), sNick.begin(), _totlower);

		while((lMyNickStart = (long)sMsgLower.find(sNick, lSearchFrom)) != tstring::npos) {
			lMyNickEnd = lMyNickStart + (long)sNick.size();
			SetSel(lSelBegin + lMyNickStart, lSelBegin + lMyNickEnd);
			SetSelectionCharFormat(WinUtil::m_TextStyleFavUsers);
			lSearchFrom = lMyNickEnd;
		}
	}
	

	if(BOOLSETTING(USE_HIGHLIGHT)) {
	
		ColorList *cList = HighlightManager::getInstance()->getList();
		CHARFORMAT2 hlcf;
		logged = false;
		//compare the last line against all strings in the vector
		for(ColorIter i = cList->begin(); i != cList->end(); ++i) {
			ColorSettings* cs = &(*i);
			if(cs->getIncludeNickList()) 
				continue;
			size_t pos;
			tstring msg = sText;

			//set start position for find
			pos = msg.find(_T(">"));

			//prepare the charformat
			memset(&hlcf, 0, sizeof(CHARFORMAT2));
			hlcf.cbSize = sizeof(hlcf);
			hlcf.dwReserved = 0;
			hlcf.dwMask = CFM_BACKCOLOR | CFM_COLOR | CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT;

			if(cs->getBold())		hlcf.dwEffects |= CFE_BOLD;
			if(cs->getItalic())		hlcf.dwEffects |= CFE_ITALIC;
			if(cs->getUnderline())	hlcf.dwEffects |= CFM_UNDERLINE;
			if(cs->getStrikeout())	hlcf.dwEffects |= CFM_STRIKEOUT;
		
			if(cs->getHasBgColor())
				hlcf.crBackColor = cs->getBgColor();
			else
				hlcf.crBackColor = SETTING(TEXT_GENERAL_BACK_COLOR);
	
			if(cs->getHasFgColor())
				hlcf.crTextColor = cs->getFgColor();
			else
				hlcf.crTextColor = SETTING(TEXT_GENERAL_FORE_COLOR);
		
			while( pos != string::npos ){
				if(cs->usingRegexp()) 
					pos = RegExpMatch(cs, hlcf, msg, lSelBegin);
				else 
					pos = FullTextMatch(cs, hlcf, msg, pos, lSelBegin);
			}

			matchedPopup = false;
			matchedSound = false;
		}//end for
	}


	// Links and smilies
	FormatEmoticonsAndLinks(sText, /*sMsgLower,*/ lSelBegin, bUseEmo);
}

void ChatCtrl::FormatEmoticonsAndLinks(tstring& sMsg, /*tstring& sMsgLower,*/ LONG lSelBegin, bool bUseEmo) {
	vector<ChatLink> tmpLinks;
		
	try {
		tstring::const_iterator start = sMsg.begin();
		tstring::const_iterator end = sMsg.end();
		boost::match_results<tstring::const_iterator> result;
		int pos=0;

		while(boost::regex_search(start, end, result, regUrlBoost, boost::match_default)) {
			size_t linkStart = pos + lSelBegin + result.position();
			size_t linkEnd = pos + lSelBegin + result.position() + result.length();
			SetSel(linkStart, linkEnd);
			std::string link( result[0].first, result[0].second );

			if(link.find("magnet:?") != string::npos) {
				ChatLink cl = ChatLink(link, ChatLink::TYPE_MAGNET);
				tmpLinks.push_back(cl);
				if (cl.dupe == ChatLink::DUPE_SHARE) {
					SetSelectionCharFormat(WinUtil::m_TextStyleDupe);
				} else if (cl.dupe == ChatLink::DUPE_QUEUE) {
					SetSelectionCharFormat(WinUtil::m_TextStyleQueue);
				} else {
					SetSelectionCharFormat(WinUtil::m_TextStyleURL);
				}
			} else {
				ChatLink cl = ChatLink(link, (link.find("spotify:") != string::npos ? ChatLink::TYPE_SPOTIFY : ChatLink::TYPE_URL));
				tmpLinks.push_back(cl);
				SetSelectionCharFormat(WinUtil::m_TextStyleURL);
			}

			pos=pos+result.position() + result.length();
			start = result[0].second;
		}
	
	} catch(...) { 
		//...
	}

	//replace links
	for(auto p = tmpLinks.begin(); p != tmpLinks.end(); p++) {
		tstring::size_type found = 0;
		while((found = sMsg.find(Text::toT(p->url), found)) != tstring::npos) {
			size_t linkStart =  found;
			size_t linkEnd =  found + p->url.length();

			tstring displayText = Text::toT(p->getDisplayText());
			SetSel(linkStart + lSelBegin, linkEnd + lSelBegin);

			sMsg.replace(linkStart, linkEnd - linkStart, displayText.c_str());
			setText(displayText);
			linkEnd = linkStart + displayText.size();
			//SetSel(cr.cpMin, cr.cpMax);
			found++;


			CHARRANGE cr;
			cr.cpMin = linkStart + lSelBegin;
			cr.cpMax = linkEnd + lSelBegin;
			links.push_back(make_pair(cr, *p));
		}
	}

	//Format release names
	if(SETTING(FORMAT_RELEASE) || SETTING(DUPES_IN_CHAT)) {
		tstring::const_iterator start = sMsg.begin();
		tstring::const_iterator end = sMsg.end();
		boost::match_results<tstring::const_iterator> result;
		int pos=0;

		while(boost::regex_search(start, end, result, regReleaseBoost, boost::match_default)) {
			CHARRANGE cr;
			cr.cpMin = pos + lSelBegin + result.position();
			cr.cpMax = pos + lSelBegin + result.position() + result.length();

			SetSel(cr.cpMin, cr.cpMax);

			std::string link (result[0].first, result[0].second);
			ChatLink cl = ChatLink(link, ChatLink::TYPE_RELEASE);

			if (SETTING(DUPES_IN_CHAT) && cl.dupe == ChatLink::DUPE_SHARE) {
				SetSelectionCharFormat(WinUtil::m_TextStyleDupe);
			} else if (SETTING(DUPES_IN_CHAT) && cl.dupe == ChatLink::DUPE_QUEUE) {
				SetSelectionCharFormat(WinUtil::m_TextStyleQueue);
			} else if (SETTING(DUPES_IN_CHAT) && cl.dupe == ChatLink::DUPE_FINISHED) {
				CHARFORMAT2 newFormat = WinUtil::m_TextStyleQueue;
				BYTE r, b, g;
				DWORD queue = SETTING(QUEUE_COLOR);

				r = static_cast<BYTE>(( static_cast<DWORD>(GetRValue(queue)) + static_cast<DWORD>(GetRValue(newFormat.crBackColor)) ) / 2);
				g = static_cast<BYTE>(( static_cast<DWORD>(GetGValue(queue)) + static_cast<DWORD>(GetGValue(newFormat.crBackColor)) ) / 2);
				b = static_cast<BYTE>(( static_cast<DWORD>(GetBValue(queue)) + static_cast<DWORD>(GetBValue(newFormat.crBackColor)) ) / 2);
				newFormat.crTextColor = RGB(r, g, b);

				SetSelectionCharFormat(newFormat);
			} else if (SETTING(FORMAT_RELEASE)) {
				SetSelectionCharFormat(WinUtil::m_TextStyleURL);
			}

			links.push_back(make_pair(cr, cl));
			start = result[0].second;
			pos=pos+result.position() + result.length();
		}
	}

	// insert emoticons
	if(bUseEmo && emoticonsManager->getUseEmoticons()) {
		const Emoticon::List& emoticonsList = emoticonsManager->getEmoticonsList();
		tstring::size_type lastReplace = 0;
		uint8_t smiles = 0;
		LONG lSelEnd=0;

		while(true) {
			tstring::size_type curReplace = tstring::npos;
			Emoticon* foundEmoticon = NULL;

			for(auto emoticon = emoticonsList.begin(); emoticon != emoticonsList.end(); ++emoticon) {
				tstring::size_type idxFound = sMsg.find((*emoticon)->getEmoticonText(), lastReplace);
				if(idxFound < curReplace || curReplace == tstring::npos) {
					curReplace = idxFound;
					foundEmoticon = (*emoticon);
				}
			}

			if(curReplace != tstring::npos && smiles < MAX_EMOTICONS) {
				bool insert=true;
				CHARFORMAT2 cfSel;
				cfSel.cbSize = sizeof(cfSel);
				lSelBegin += (curReplace - lastReplace);
				lSelEnd = lSelBegin + foundEmoticon->getEmoticonText().size();

				//check the position
				if ((curReplace != lastReplace) && (curReplace > 0) && isgraph(sMsg[curReplace-1])) {
					insert=false;
				}

				if (insert) {
					SetSel(lSelBegin, lSelEnd);

					GetSelectionCharFormat(cfSel);
					CImageDataObject::InsertBitmap(GetOleInterface(), foundEmoticon->getEmoticonBmp(cfSel.crBackColor));

					++smiles;
					++lSelBegin;

					//fix the positions for links after this emoticon....
					for(auto i = links.rbegin(); i != links.rend(); ++i) {
						if ((*i).first.cpMin > lSelBegin) {
							(*i).first.cpMin = (*i).first.cpMin - foundEmoticon->getEmoticonText().size() + 1;
							(*i).first.cpMax = (*i).first.cpMax - foundEmoticon->getEmoticonText().size() + 1;
						} else {
							break;
						}
					}
				} else lSelBegin = lSelEnd;
				lastReplace = curReplace + foundEmoticon->getEmoticonText().size();
			} else break;
		}
	}

}

bool ChatCtrl::HitNick(const POINT& p, tstring& sNick, int& iBegin, int& iEnd) {
	if(client == NULL) return false;
	
	int iCharPos = CharFromPos(p), line = LineFromChar(iCharPos), len = LineLength(iCharPos) + 1;
	long lSelBegin = 0, lSelEnd = 0;
	if(len < 3)
		return false;

	// Metoda FindWordBreak nestaci, protoze v nicku mohou byt znaky povazovane za konec slova
	int iFindBegin = LineIndex(line), iEnd1 = LineIndex(line) + LineLength(iCharPos);

	for(lSelBegin = iCharPos; lSelBegin >= iFindBegin; lSelBegin--) {
		if(FindWordBreak(WB_ISDELIMITER, lSelBegin))
			break;
	}
	lSelBegin++;
	for(lSelEnd = iCharPos; lSelEnd < iEnd1; lSelEnd++) {
		if(FindWordBreak(WB_ISDELIMITER, lSelEnd))
			break;
	}

	len = lSelEnd - lSelBegin;
	if(len <= 0)
		return false;

	tstring sText;
	sText.resize(len);

	GetTextRange(lSelBegin, lSelEnd, &sText[0]);

	size_t iLeft = 0, iRight = 0, iCRLF = sText.size(), iPos = sText.find(_T('<'));
	if(iPos != tstring::npos) {
		iLeft = iPos + 1;
		iPos = sText.find(_T('>'), iLeft);
		if(iPos == tstring::npos) 
			return false;

		iRight = iPos - 1;
		iCRLF = iRight - iLeft + 1;
	} else {
		iLeft = 0;
	}

	tstring sN = sText.substr(iLeft, iCRLF);
	if(sN.empty())
		return false;

	if(client->findUser(Text::fromT(sN)) != NULL) {
		sNick = sN;
		iBegin = lSelBegin + iLeft;
		iEnd = lSelBegin + iLeft + iCRLF;
		return true;
	}
    
	// Jeste pokus odmazat eventualni koncovou ':' nebo '>' 
	// Nebo pro obecnost posledni znak 
	// A taky prvni znak 
	// A pak prvni i posledni :-)
	if(iCRLF > 1) {
		sN = sText.substr(iLeft, iCRLF - 1);
		if(client->findUser(Text::fromT(sN)) != NULL) {
			sNick = sN;
   			iBegin = lSelBegin + iLeft;
   			iEnd = lSelBegin + iLeft + iCRLF - 1;
			return true;
		}

		sN = sText.substr(iLeft + 1, iCRLF - 1);
		if(client->findUser(Text::fromT(sN)) != NULL) {
        	sNick = sN;
			iBegin = lSelBegin + iLeft + 1;
			iEnd = lSelBegin + iLeft + iCRLF;
			return true;
		}

		sN = sText.substr(iLeft + 1, iCRLF - 2);
		if(client->findUser(Text::fromT(sN)) != NULL) {
			sNick = sN;
   			iBegin = lSelBegin + iLeft + 1;
			iEnd = lSelBegin + iLeft + iCRLF - 1;
			return true;
		}
	}	
	return false;
}

bool ChatCtrl::HitIP(const POINT& p, tstring& sIP, int& iBegin, int& iEnd) {
	int iCharPos = CharFromPos(p), len = LineLength(iCharPos) + 1;
	if(len < 3)
		return false;

	DWORD lPosBegin = FindWordBreak(WB_LEFT, iCharPos);
	DWORD lPosEnd = FindWordBreak(WB_RIGHTBREAK, iCharPos);
	len = lPosEnd - lPosBegin;

	tstring sText;
	sText.resize(len);
	GetTextRange(lPosBegin, lPosEnd, &sText[0]);

	for(int i = 0; i < len; i++) {
		if(!((sText[i] == 0) || (sText[i] == '.') || ((sText[i] >= '0') && (sText[i] <= '9')))) {
			return false;
		}
	}

	sText += _T('.');
	size_t iFindBegin = 0, iPos = tstring::npos, iEnd2 = 0;

	for(int i = 0; i < 4; ++i) {
		iPos = sText.find(_T('.'), iFindBegin);
		if(iPos == tstring::npos) {
			return false;
		}
		iEnd2 = atoi(Text::fromT(sText.substr(iFindBegin)).c_str());
		if((iEnd2 < 0) || (iEnd2 > 255)) {
			return false;
		}
		iFindBegin = iPos + 1;
	}

	sIP = sText.substr(0, iPos);
	iBegin = lPosBegin;
	iEnd = lPosEnd;

	return true;
}

tstring ChatCtrl::LineFromPos(const POINT& p) const {
	int iCharPos = CharFromPos(p);
	int len = LineLength(iCharPos);

	if(len < 3) {
		return Util::emptyStringT;
	}

	tstring tmp;
	tmp.resize(len);

	GetLine(LineFromChar(iCharPos), &tmp[0], len);

	return tmp;
}

LRESULT ChatCtrl::OnRButtonDown(POINT pt) {
	selectedLine = LineFromPos(pt);
	selectedUser.clear();
	selectedIP.clear();

	shareDupe=false, queueDupe=false, isMagnet=false, isTTH=false, release=false;
	ChatLink cl;
	CHARRANGE cr;
	GetSel(cr);

	if (getLink(pt, cr, cl)) {
		selectedWord = Text::toT(cl.url);
		shareDupe = cl.dupe == ChatLink::DUPE_SHARE;
		queueDupe = cl.dupe == ChatLink::DUPE_QUEUE;
		isMagnet = cl.type == ChatLink::TYPE_MAGNET;
		release = cl.type == ChatLink::TYPE_RELEASE;
		SetSel(cr.cpMin, cr.cpMax);
	} else {
		if(cr.cpMax != cr.cpMin) {
			TCHAR *buf = new TCHAR[cr.cpMax - cr.cpMin + 1];
			GetSelText(buf);
			selectedWord = Util::replace(buf, _T("\r"), _T("\r\n"));
			delete[] buf;
		} else {
			selectedWord = WordFromPos(pt);
			if (selectedWord.length() == 39) {
				isTTH = true;
			}
		}
	}

	size_t pos = selectedLine.find(_T(" <"));
	if (pos != tstring::npos) {
		tstring::size_type iAuthorLen = selectedLine.find(_T('>'), 1);
		if(iAuthorLen != tstring::npos) {
			tstring nick(selectedLine.c_str() + pos + 2);
			nick.erase(iAuthorLen - pos - 2);
			author = nick;
		}
	}

	// Po kliku dovnitr oznaceneho textu si zkusime poznamenat pripadnej nick ci ip...
	// jinak by nam to neuznalo napriklad druhej klik na uz oznaceny nick =)
	long lSelBegin = 0, lSelEnd = 0;
	GetSel(lSelBegin, lSelEnd);
	int iCharPos = CharFromPos(pt), iBegin = 0, iEnd = 0;
	if((lSelEnd > lSelBegin) && (iCharPos >= lSelBegin) && (iCharPos <= lSelEnd)) {
		if(!HitIP(pt, selectedIP, iBegin, iEnd))
			HitNick(pt, selectedUser, iBegin, iEnd);

		return 1;
	}

	// hightlight IP or nick when clicking on it
	if(HitIP(pt, selectedIP, iBegin, iEnd) || HitNick(pt, selectedUser, iBegin, iEnd)) {
		SetSel(iBegin, iEnd);
		InvalidateRect(NULL);
	}
	return 1;
}

LRESULT ChatCtrl::onExitMenuLoop(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& bHandled) {
	m_bPopupMenu = false;
	bHandled = FALSE;
	return 0;
}

LRESULT ChatCtrl::onSetCursor(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& bHandled) {
    if(m_bPopupMenu)
    {
        SetCursor(arrowCursor) ;
		return 1;
    }

	if (showHandCursor) {
		SetCursor(handCursor);
		return 1;
	}
    bHandled = FALSE;
	return 0;
}

LRESULT ChatCtrl::onContextMenu(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/) {
	POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };        // location of mouse click

	if(pt.x == -1 && pt.y == -1) {
		CRect erc;
		GetRect(&erc);
		pt.x = erc.Width() / 2;
		pt.y = erc.Height() / 2;
		ClientToScreen(&pt);
	}

	POINT ptCl = pt;
	ScreenToClient(&ptCl); 
	OnRButtonDown(ptCl);

	OMenu menu;
	menu.CreatePopupMenu();

	if (targetMenu.m_hMenu != NULL) {
		// delete target menu
		targetMenu.DestroyMenu();
		targetMenu.m_hMenu = NULL;
	}

	if (SearchMenu.m_hMenu != NULL) {
		// delete search menu
		SearchMenu.DestroyMenu();
		SearchMenu.m_hMenu = NULL;
	}

	if (copyMenu.m_hMenu != NULL) {
		// delete copy menu if it exists
		copyMenu.DestroyMenu();
		copyMenu.m_hMenu = NULL;
	}

	if(selectedUser.empty()) {

		if(!selectedIP.empty()) {
			menu.InsertSeparatorFirst(selectedIP);
			menu.AppendMenu(MF_STRING, IDC_WHOIS_IP, (TSTRING(WHO_IS) + _T(" ") + selectedIP).c_str() );
			prepareMenu(menu, ::UserCommand::CONTEXT_USER, client->getHubUrl());
			if (client && client->isOp()) {
				menu.AppendMenu(MF_SEPARATOR);
				menu.AppendMenu(MF_STRING, IDC_BAN_IP, (_T("!banip ") + selectedIP).c_str());
				menu.SetMenuDefaultItem(IDC_BAN_IP);
				menu.AppendMenu(MF_STRING, IDC_UNBAN_IP, (_T("!unban ") + selectedIP).c_str());
				menu.AppendMenu(MF_SEPARATOR);
			}
		} else if (release) {
			menu.InsertSeparatorFirst(_T("Release"));
		} else {
			menu.InsertSeparatorFirst(_T("Text"));
		}

		menu.AppendMenu(MF_STRING, ID_EDIT_COPY, CTSTRING(COPY));
		menu.AppendMenu(MF_STRING, IDC_COPY_ACTUAL_LINE,  CTSTRING(COPY_LINE));
		menu.AppendMenu(MF_SEPARATOR);
		menu.AppendMenu(MF_STRING, IDC_SEARCH, CTSTRING(SEARCH));

		if (shareDupe || queueDupe) {
			menu.AppendMenu(MF_SEPARATOR);
			menu.AppendMenu(MF_STRING, IDC_OPEN_FOLDER, CTSTRING(OPEN_FOLDER));
		}

		if (isMagnet && !author.empty()) {
			if (author == Text::toT(client->getMyNick())) {
				/* show an option to remove the item */
			} else {
				targetMenu.CreatePopupMenu();
				menu.AppendMenu(MF_SEPARATOR);
				menu.AppendMenu(MF_STRING, IDC_DOWNLOAD, CTSTRING(DOWNLOAD));
				menu.AppendMenu(MF_POPUP, (UINT_PTR)(HMENU)targetMenu, CTSTRING(DOWNLOAD_TO));

				targetMenu.InsertSeparatorFirst(TSTRING(DOWNLOAD_TO));
				WinUtil::appendDirsMenu(targetMenu);
			}
		} else if (release) {
			//autosearch menus
			targetMenu.CreatePopupMenu();
			menu.AppendMenu(MF_SEPARATOR);
			menu.AppendMenu(MF_STRING, IDC_DOWNLOAD, CTSTRING(DOWNLOAD));
			menu.AppendMenu(MF_POPUP, (UINT_PTR)(HMENU)targetMenu, CTSTRING(DOWNLOAD_TO));

			targetMenu.InsertSeparatorFirst(TSTRING(DOWNLOAD_TO));
			WinUtil::appendDirsMenu(targetMenu);
		} else if (isTTH) {
			menu.AppendMenu(MF_STRING, IDC_SEARCH_BY_TTH, CTSTRING(SEARCH_TTH));
		}

		SearchMenu.CreatePopupMenu();
		menu.AppendMenu(MF_SEPARATOR);
		menu.AppendMenu(MF_POPUP, (UINT)(HMENU)SearchMenu, CTSTRING(SEARCH_SITES));
		WinUtil::AppendSearchMenu(SearchMenu);
	} else {
		bool isMe = (selectedUser == Text::toT(client->getMyNick()));

		// click on nick
		copyMenu.CreatePopupMenu();
		copyMenu.InsertSeparatorFirst(TSTRING(COPY));

		for(int j=0; j < OnlineUser::COLUMN_LAST; j++) {
			copyMenu.AppendMenu(MF_STRING, IDC_COPY + j, CTSTRING_I(HubFrame::columnNames[j]));
		}

		menu.InsertSeparatorFirst(selectedUser);

		if(BOOLSETTING(LOG_PRIVATE_CHAT)) {
			menu.AppendMenu(MF_STRING, IDC_OPEN_USER_LOG,  CTSTRING(OPEN_USER_LOG));
			menu.AppendMenu(MF_SEPARATOR);
			menu.AppendMenu(MF_STRING, IDC_USER_HISTORY,  CTSTRING(VIEW_HISTORY));
			menu.AppendMenu(MF_SEPARATOR);
		}		

		menu.AppendMenu(MF_STRING, IDC_SELECT_USER, CTSTRING(SELECT_USER_LIST));
		menu.AppendMenu(MF_SEPARATOR);
		
		if(!isMe) {
			menu.AppendMenu(MF_STRING, IDC_PUBLIC_MESSAGE, CTSTRING(SEND_PUBLIC_MESSAGE));
			menu.AppendMenu(MF_STRING, IDC_PRIVATEMESSAGE, CTSTRING(SEND_PRIVATE_MESSAGE));
			menu.AppendMenu(MF_SEPARATOR);
			
			const OnlineUserPtr ou = client->findUser(Text::fromT(selectedUser));
			if (client->isOp() || !ou->getIdentity().isOp()) {
				if(HubFrame::ignoreList.find(ou->getUser()) == HubFrame::ignoreList.end()) {
					menu.AppendMenu(MF_STRING, IDC_IGNORE, CTSTRING(IGNORE_USER));
				} else {    
					menu.AppendMenu(MF_STRING, IDC_UNIGNORE, CTSTRING(UNIGNORE_USER));
				}
				menu.AppendMenu(MF_SEPARATOR);
			}
		}
		
		menu.AppendMenu(MF_POPUP, (UINT)(HMENU)copyMenu, CTSTRING(COPY));
		
		if(!isMe) {
			menu.AppendMenu(MF_POPUP, (UINT)(HMENU)WinUtil::grantMenu, CTSTRING(GRANT_SLOTS_MENU));
			menu.AppendMenu(MF_SEPARATOR);
			menu.AppendMenu(MF_STRING, IDC_GETLIST, CTSTRING(GET_FILE_LIST));
			menu.AppendMenu(MF_STRING, IDC_BROWSELIST, CTSTRING(BROWSE_FILE_LIST));
			menu.AppendMenu(MF_STRING, IDC_MATCH_QUEUE, CTSTRING(MATCH_QUEUE));
			menu.AppendMenu(MF_SEPARATOR);
			menu.AppendMenu(MF_STRING, IDC_ADD_TO_FAVORITES, CTSTRING(ADD_TO_FAVORITES));
		}

		// add user commands
		prepareMenu(menu, ::UserCommand::CONTEXT_USER, client->getHubUrl());

		/*// default doubleclick action
		switch(SETTING(CHAT_DBLCLICK)) {
        case 0:
			menu.SetMenuDefaultItem(IDC_SELECT_USER);
			break;
        case 1:
			menu.SetMenuDefaultItem(IDC_PUBLIC_MESSAGE);
			break;
        case 2:
			menu.SetMenuDefaultItem(IDC_PRIVATEMESSAGE);
			break;
        case 3:
			menu.SetMenuDefaultItem(IDC_GETLIST);
			break;
        case 4:
			menu.SetMenuDefaultItem(IDC_MATCH_QUEUE);
			break;
        case 6:
			menu.SetMenuDefaultItem(IDC_ADD_TO_FAVORITES);
			break;
		} */ 
			//Bold the Best solution for getting user users list instead.
			if(AirUtil::isAdcHub(client->getHubUrl())) {
				menu.SetMenuDefaultItem(IDC_BROWSELIST);
			} else {
				menu.SetMenuDefaultItem(IDC_GETLIST);
			}
	}

	menu.AppendMenu(MF_SEPARATOR);
	menu.AppendMenu(MF_STRING, ID_EDIT_SELECT_ALL, CTSTRING(SELECT_ALL));
	menu.AppendMenu(MF_STRING, ID_EDIT_CLEAR_ALL, CTSTRING(CLEAR_CHAT));
	
	//flag to indicate pop up menu.
    m_bPopupMenu = true;
	menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, m_hWnd);

	return 0;
}

LRESULT ChatCtrl::onOpenDupe(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	tstring path;
	if (release) {
		if (shareDupe) {
			path = ShareManager::getInstance()->getDirPath(Text::fromT(selectedWord));
		} else {
			path = QueueManager::getInstance()->getDirPath(Text::fromT(selectedWord));
		}
	} else {
		Magnet m = Magnet(Text::fromT(selectedWord));
		if (m.hash.empty())
			return 0;

		if (shareDupe) {
			try {
				path = Text::toT(ShareManager::getInstance()->getRealPath(m.getTTH()));
			} catch(...) { }
		} else {
			StringList targets = QueueManager::getInstance()->getTargets(m.getTTH());
			if (!targets.empty()) {
				path = Text::toT(targets.front());
			}
		}
	}
	if (path.empty())
		return 0;

	WinUtil::openFolder(path);
	return 0;
}

LRESULT ChatCtrl::onDownload(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if (release) {
		if(selectedWord.size() < 5) //we dont accept anything under 5 chars
			return 0;

		addAutoSearch(SETTING(DOWNLOAD_DIRECTORY), TargetUtil::TARGET_PATH);
	} else {
		downloadMagnet(SETTING(DOWNLOAD_DIRECTORY));
	}
	SetSelNone();
	return 0;
}

void ChatCtrl::addAutoSearch(const string& aPath, uint8_t targetType) {
	AutoSearchManager::getInstance()->addAutoSearch(Text::fromT(selectedWord), aPath, (TargetUtil::TargetType)targetType, true);
}

void ChatCtrl::downloadMagnet(const string& aPath) {

	Magnet m = Magnet(Text::fromT(selectedWord));
	OnlineUserPtr u = client->findUser(Text::fromT(author));
	if (u) {
		try {
			QueueManager::getInstance()->add(aPath + m.fname, m.fsize, m.getTTH(), 
				!u->getUser()->isSet(User::BOT) ? HintedUser(u->getUser(), client->getHubUrl()) : HintedUser(UserPtr(), Util::emptyString));
		} catch (...) {}
	}
}

LRESULT ChatCtrl::onDownloadTo(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if(selectedWord.size() < 5) //we dont accept anything under 5 chars
		return 0;

	tstring target = Text::toT(SETTING(DOWNLOAD_DIRECTORY));
	if(WinUtil::browseDirectory(target, m_hWnd)) {
		SettingsManager::getInstance()->addDirToHistory(target);
		if (release) {
			addAutoSearch(Text::fromT(target), TargetUtil::TARGET_PATH);
		} else {
			downloadMagnet(Text::fromT(target));
		}
	}
	SetSelNone();
	return 0;
}

LRESULT ChatCtrl::onDownloadFavoriteDirs(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if (release) {
		if(selectedWord.size() < 5) //we dont accept anything under 5 chars
			return 0;

		TargetUtil::TargetType targetType;
		string vTarget;
		if (WinUtil::getVirtualName(wID, vTarget, targetType))
			addAutoSearch(vTarget, targetType);
	} else {
		string target;
		if (WinUtil::getTarget(wID, target, 0)) {
			downloadMagnet(target);
		}
	}
	SetSelNone();
	return 0;
}

LRESULT ChatCtrl::onSize(UINT /*uMsg*/, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
	if(wParam != SIZE_MINIMIZED && HIWORD(lParam) > 0 && autoScrollToEnd) {
		scrollToEnd();
	}

	bHandled = FALSE;
	return 0;
}

LRESULT ChatCtrl::onLButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& bHandled) {
	selectedLine.clear();
	selectedIP.clear();
	selectedUser.clear();
	selectedWord.clear();

	bHandled = FALSE;
	return 0;
}


LRESULT ChatCtrl::onMouseMove(UINT /*uMsg*/, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
	

	bHandled=FALSE;
	if (lastTick+20 > GET_TICK())
		return FALSE;
	lastTick=GET_TICK();

	if(wParam != 0) { //dont update cursor and check for links when marking something.
		if (showHandCursor)
			SetCursor(arrowCursor);
		showHandCursor=false;
		return TRUE;
	}

	POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };        // location of mouse click
	if (isLink(pt)) {
		if (!showHandCursor) {
			showHandCursor=true;
			SetCursor(handCursor);
		}
		return TRUE;
	} else if (showHandCursor) {
		showHandCursor=false;
		SetCursor(arrowCursor);
		return TRUE;
	}

	return FALSE;
}

bool ChatCtrl::isLink(POINT pt) {
	int iCharPos = CharFromPos(pt), /*line = LineFromChar(iCharPos),*/ len = LineLength(iCharPos) + 1;
	if(len < 3)
		return false;

	POINT p_ichar = PosFromChar(iCharPos);
	if(pt.x > (p_ichar.x + 3)) { //+3 is close enough, dont want to be too strict about it?
		return false;
	}

	if(pt.y > (p_ichar.y +  (t_height*1.5))) { //times 1.5 so dont need to be totally exact
		return false;
	}


	for(auto i = links.rbegin(); i != links.rend(); ++i) {
		if( iCharPos >= i->first.cpMin && iCharPos <= i->first.cpMax ) {
			return true;
		}
	}

	return false;
}

bool ChatCtrl::getLink(POINT pt, CHARRANGE& cr, ChatLink& link) {
	int iCharPos = CharFromPos(pt);
	POINT p_ichar = PosFromChar(iCharPos);
	
	//Validate that we are actually clicking over a link.
	if(pt.x > (p_ichar.x + 3)) { 
		return false;
	}
	if(pt.y > (p_ichar.y +  (t_height*1.5))) {
		return false;
	}

	for(auto i = links.rbegin(); i != links.rend(); ++i) {
		if( iCharPos >= i->first.cpMin && iCharPos <= i->first.cpMax ) {
			link = i->second;
			cr = i->first;
			return true;
		}
	}
	return false;
}

LRESULT ChatCtrl::onLeftButtonUp(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
	bHandled = onClientEnLink(uMsg, wParam, lParam, bHandled);
	return bHandled = TRUE ? 0: 1;
}

bool ChatCtrl::onClientEnLink(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/) {
	POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

	ChatLink cl;
	CHARRANGE cr;
	if (!getLink(pt, cr, cl))
		return 0;


	if (cl.type == ChatLink::TYPE_MAGNET)
		SendMessage(IDC_HANDLE_MAGNET,0, (LPARAM)new tstring(Text::toT(cl.url)));
	else
		WinUtil::openLink(Text::toT(cl.url));

	return 1;
}

LRESULT ChatCtrl::onEditCopy(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	CHARRANGE cr;
	GetSel(cr);
	if(cr.cpMax != cr.cpMin)
		Copy();
	else
		WinUtil::setClipboard(selectedWord);
	
	return 0;
}

LRESULT ChatCtrl::onEditSelectAll(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	SetSelAll();
	return 0;
}

LRESULT ChatCtrl::onEditClearAll(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	SetWindowText(_T(""));
	return 0;
}

LRESULT ChatCtrl::onCopyActualLine(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if(!selectedLine.empty()) {
		WinUtil::setClipboard(selectedLine);
	}
	return 0;
}

LRESULT ChatCtrl::onBanIP(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if(!selectedIP.empty()) {
		tstring s = _T("!banip ") + selectedIP;
		client->hubMessage(Text::fromT(s));
	}
	SetSelNone();
	return 0;
}

LRESULT ChatCtrl::onUnBanIP(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if(!selectedIP.empty()) {
		tstring s = _T("!unban ") + selectedIP;
		client->hubMessage(Text::fromT(s));
	}
	SetSelNone();
	return 0;
}


LRESULT ChatCtrl::onWhoisIP(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if(!selectedIP.empty()) {
 		WinUtil::openLink(_T("http://www.ripe.net/perl/whois?form_type=simple&full_query_string=&searchtext=") + selectedIP);
 	}
	SetSelNone();
	return 0;
}

LRESULT ChatCtrl::onOpenUserLog(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	OnlineUserPtr ou = client->findUser(Text::fromT(selectedUser));
	if(ou) {
		ParamMap params;

		params["userNI"] = ou->getIdentity().getNick();
		params["hubNI"] = client->getHubName();
		params["myNI"] = client->getMyNick();
		params["userCID"] = ou->getUser()->getCID().toBase32();
		params["hubURL"] = client->getHubUrl();

		string file = LogManager::getInstance()->getPath(LogManager::PM, params);
		if(Util::fileExists(file)) {
			WinUtil::viewLog(file, wID == IDC_USER_HISTORY);
		} else {
			MessageBox(CTSTRING(NO_LOG_FOR_USER),CTSTRING(NO_LOG_FOR_USER), MB_OK );	  
		}
	}

	return 0;
}

LRESULT ChatCtrl::onPrivateMessage(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	OnlineUserPtr ou = client->findUser(Text::fromT(selectedUser));
	if(ou)
		PrivateFrame::openWindow(HintedUser(ou->getUser(), client->getHubUrl()), Util::emptyStringT, client);

	return 0;
}

LRESULT ChatCtrl::onGetList(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	OnlineUserPtr ou = client->findUser(Text::fromT(selectedUser));
	if(ou)
		ou->getList();

	return 0;
}

LRESULT ChatCtrl::onBrowseList(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	OnlineUserPtr ou = client->findUser(Text::fromT(selectedUser));
	if(ou)
		ou->browseList();

	return 0;
}
LRESULT ChatCtrl::onMatchQueue(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	OnlineUserPtr ou = client->findUser(Text::fromT(selectedUser));
	if(ou)
		ou->matchQueue();

	return 0;
}

LRESULT ChatCtrl::onGrantSlot(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	const OnlineUserPtr ou = client->findUser(Text::fromT(selectedUser));
	if(ou) {
		uint64_t time = 0;
		switch(wID) {
			case IDC_GRANTSLOT:			time = 600; break;
			case IDC_GRANTSLOT_DAY:		time = 3600; break;
			case IDC_GRANTSLOT_HOUR:	time = 24*3600; break;
			case IDC_GRANTSLOT_WEEK:	time = 7*24*3600; break;
			case IDC_UNGRANTSLOT:		time = 0; break;
		}
		
		if(time > 0)
			UploadManager::getInstance()->reserveSlot(HintedUser(ou->getUser(), client->getHubUrl()), time);
		else
			UploadManager::getInstance()->unreserveSlot(ou->getUser());
	}

	return 0;
}

LRESULT ChatCtrl::onAddToFavorites(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	OnlineUserPtr ou = client->findUser(Text::fromT(selectedUser));
	if(ou)
		ou->addFav();

	return 0;
}

LRESULT ChatCtrl::onIgnore(UINT /*uMsg*/, WPARAM /*wParam*/, HWND /*lParam*/, BOOL& /*bHandled*/){
	OnlineUserPtr ou = client->findUser(Text::fromT(selectedUser));
	if(ou){
		HubFrame::ignoreList.insert(ou->getUser());
		IgnoreManager::getInstance()->storeIgnore(ou->getUser());
	}
	return 0;
}

LRESULT ChatCtrl::onUnignore(UINT /*uMsg*/, WPARAM /*wParam*/, HWND /*lParam*/, BOOL& /*bHandled*/){
	OnlineUserPtr ou = client->findUser(Text::fromT(selectedUser));
	if(ou){
		HubFrame::ignoreList.erase(ou->getUser());
		IgnoreManager::getInstance()->removeIgnore(ou->getUser());
	}
	return 0;
}

LRESULT ChatCtrl::onCopyUserInfo(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	tstring sCopy;
	
	const OnlineUserPtr ou = client->findUser(Text::fromT(selectedUser));
	if(ou) {
		sCopy = ou->getText(static_cast<uint8_t>(wID - IDC_COPY), true);
	}

	if (!sCopy.empty())
		WinUtil::setClipboard(sCopy);

	return 0;
}

void ChatCtrl::runUserCommand(UserCommand& uc) {
	ParamMap ucParams;

	if(!WinUtil::getUCParams(m_hWnd, uc, ucParams))
		return;

	client->getMyIdentity().getParams(ucParams, "my", true);
	client->getHubIdentity().getParams(ucParams, "hub", false);

	const OnlineUserPtr ou = client->findUser(Text::fromT(selectedUser));
	if(ou != NULL) {
		auto tmp = ucParams;
		ou->getIdentity().getParams(tmp, "user", true);
		client->sendUserCmd(uc, tmp);
	}
}

string ChatCtrl::escapeUnicode(tstring str) {
	TCHAR buf[8];
	memzero(buf, sizeof(buf));

	int dist = 0;
	tstring::iterator i;
	while((i = std::find_if(str.begin() + dist, str.end(), std::bind2nd(std::greater<TCHAR>(), 0x7f))) != str.end()) {
		dist = (i+1) - str.begin(); // Random Acess iterators FTW
		snwprintf(buf, sizeof(buf), _T("%hd"), int(*i));
		str.replace(i, i+1, _T("\\ud\\u") + tstring(buf) + _T("?"));
		memzero(buf, sizeof(buf));
	}
	return Text::fromT(str);
}

tstring ChatCtrl::rtfEscape(tstring str) {
	tstring::size_type i = 0;
	while((i = str.find_first_of(_T("{}\\\n"), i)) != tstring::npos) {
		switch(str[i]) {
			// no need to process \r handled elsewhere
			//case '\n': str.replace(i, 1, _T("\\line\n")); i+=6; break; hmm...
			case '\n': str.insert(i, _T("\\")); i+=2; break;  //do we need to put the /line in there? messes everything up
			default: str.insert(i, _T("\\")); i+=2;
		}
	}
	return str;
}

void ChatCtrl::scrollToEnd() {
	SCROLLINFO si = { 0 };
	POINT pt = { 0 };

	si.cbSize = sizeof(si);
	si.fMask = SIF_PAGE | SIF_RANGE | SIF_POS;
	GetScrollInfo(SB_VERT, &si);
	GetScrollPos(&pt);

	// this must be called twice to work properly :(
	PostMessage(EM_SCROLL, SB_BOTTOM, 0);
	PostMessage(EM_SCROLL, SB_BOTTOM, 0);

	SetScrollPos(&pt);
}

size_t ChatCtrl::FullTextMatch(ColorSettings* cs, CHARFORMAT2 &hlcf, const tstring &line, size_t pos, long &lineIndex) {
	//this shit needs a total cleanup, we cant highlight authors or timestamps this way and we dont need to.
	
	size_t index = tstring::npos;
	tstring searchString;

	if( cs->getMyNick() ) {
		tstring::size_type p = cs->getMatch().find(_T("$MyNI$"));
		if(p != tstring::npos) {
			searchString = cs->getMatch();
			searchString = searchString.replace(p, 8, Text::toT(client->getMyNick()));
		} 
	} else {
		searchString = cs->getMatch();
	}
	
	//we don't have any nick to search for
	//happens in pm's have to find a solution for this
	if(searchString.empty())
		return tstring::npos;

	
	//do we want to highlight the timestamps?
	if( cs->getTimestamps() ) {
		if( line[0] != _T('[') )
			return tstring::npos;
		index = 0;
	} else if( cs->getUsers() ) {
		if(timeStamps) {
			index = line.find(_T("] <"));
			// /me might cause this to happen
			if(index == tstring::npos)
				return tstring::npos;
			//compensate for "] "
			index += 2;
		} else if( line[0] == _T('<')) {
			index = 0;
		}
	}else{
		if( cs->getCaseSensitive() ) {
			index = line.find(searchString, pos);
		}else {
			index = Util::findSubString(line, searchString, pos);	
			//index = Text::toLower(line).find(Text::toLower(searchString), pos);
		}
	}
	//return if no matches where found
	if( index == tstring::npos )
		return tstring::npos;

	pos = index + searchString.length();
	
	//found the string, now make sure it matches
	//the way the user specified
	int length = 0;
		
	if( !cs->getUsers() && !cs->getTimestamps() ) {
		length = searchString.length();
		int p = 0;
			
		switch(cs->getMatchType()){
			case 0: //Begins
				p = index-1;
                if(line[p] != _T(' ') && line[p] != _T('\r') &&	line[p] != _T('\t') )
					return tstring::npos;
				break;
			case 1: //Contains
				break;
			case 2: // Ends
				p = index+length;
				if(line[p] != _T(' ') && line[p] != _T('\r') &&	line[p] != _T('\t') )
					return tstring::npos;
				break;
			case 3: // Equals
				if( !( (index == 0 || line[index-1] == _T(' ') || line[index-1] == _T('\t') || line[index-1] == _T('\r')) && 
					(line[index+length] == _T(' ') || line[index+length] == _T('\r') || 
					line[index+length] == _T('\t') || index+length == line.size()) ) )
					return tstring::npos;
				break;
		}
	}

	long begin, end;

	begin = lineIndex;
	
	if( cs->getTimestamps() ) {
		tstring::size_type pos = line.find(_T("]"));
		if( pos == tstring::npos ) 
			return tstring::npos;  //hmm no ]? this can't be right, return
		
		begin += index;
		end = begin + pos +1;
	} else if( cs->getUsers() ) {
		
		end = begin + line.find(_T(">")) +1;
		begin += index;
	} else if( cs->getWholeLine() ) {
		end = begin + line.length() -1;
	} else if( cs->getWholeWord() ) {
		int tmp;

		tmp = line.find_last_of(_T(" \t\r"), index);
		if(tmp != tstring::npos )
			begin += tmp+1;
		
		tmp = line.find_first_of(_T(" \t\r"), index);
		if(tmp != tstring::npos )
			end = lineIndex + tmp;
		else
			end = lineIndex + line.length();
	} else {
		begin += index;
		end = begin + searchString.length();
	}

	SetSel(begin, end);
		
	SetSelectionCharFormat(hlcf);


	CheckAction(cs, line);
	
	if( cs->getTimestamps() || cs->getUsers() )
		return tstring::npos;
	
	return pos;
}
size_t ChatCtrl::RegExpMatch(ColorSettings* cs, CHARFORMAT2 &hlcf, const tstring &line, long &lineIndex) {
	//TODO: Clean it up a bit
	
	long begin, end;	
	bool found = false;

	//this is not a valid regexp
	if(cs->getMatch().length() < 5)
		return tstring::npos;

	string str = (Text::fromT(cs->getMatch())).substr(4);
	try {
		const boost::wregex reg(Text::toT(str));

	
		boost::wsregex_iterator iter(line.begin(), line.end(), reg);
		boost::wsregex_iterator enditer;
		for(; iter != enditer; ++iter) {
			begin = lineIndex + iter->position();
			end = lineIndex + iter->position() + iter->length();

			if( cs->getWholeLine() ) {
				end = begin + line.length();
			} 
			SetSel(begin, end);
			SetSelectionCharFormat(hlcf);
		
			found = true;
		}
	} catch(...) {
	}

	if(!found)
		return tstring::npos;
	
	CheckAction(cs, line);
	
	return tstring::npos;

}

void ChatCtrl::CheckAction(ColorSettings* cs, const tstring& line) {
	if(cs->getPopup() && !matchedPopup) {
		matchedPopup = true;
		tstring popupTitle;
		popupTitle = _T("Highlight");
		MainFrame::getMainFrame()->ShowBalloonTip(line.c_str(), popupTitle.c_str());
	}

	//Todo maybe
//	if(cs->getTab() && isSet(TAB))
//		matchedTab = true;

//	if(cs->getLog() && !logged && !skipLog){
//		logged = true;
//		AddLogLine(line);
//	}

	if(cs->getPlaySound() && !matchedSound ){
			matchedSound = true;
			PlaySound(cs->getSoundFile().c_str(), NULL, SND_ASYNC | SND_FILENAME | SND_NOWAIT);
	}

	if(cs->getFlashWindow())
		WinUtil::FlashWindow();
}

tstring ChatCtrl::WordFromPos(const POINT& p) {
	//if(client == NULL) return Util::emptyStringT;

	int iCharPos = CharFromPos(p), /*line = LineFromChar(iCharPos),*/ len = LineLength(iCharPos) + 1;
	if(len < 3)
		return Util::emptyStringT;

	POINT p_ichar = PosFromChar(iCharPos);
	
	//check the mouse positions again, to avoid getting the word even if we are past the end of text. 
	//better way to do this?
	if(p.x > (p_ichar.x + 3)) { //+3 is close enough, dont want to be too strict about it?
		return Util::emptyStringT;
	}

	if(p.y > (p_ichar.y +  (t_height*1.5))) { //times 1.5 so dont need to be totally exact
		return Util::emptyStringT;
	}

	int begin =  0;
	int end  = 0;
	
	FINDTEXT findt;
	findt.chrg.cpMin = iCharPos;
	findt.chrg.cpMax = -1;
	findt.lpstrText = _T("\r");

	long l_Start = SendMessage(EM_FINDTEXT, 0, (LPARAM)&findt) + 1;
	long l_End = SendMessage(EM_FINDTEXT, FR_DOWN, (LPARAM)&findt);

	if(l_Start < 0)
		l_Start = 0;

	if(l_End == -1)
		l_End =  GetTextLengthEx(GTL_NUMCHARS);
		
	//long l_Start = LineIndex(line);
	//long l_End = LineIndex(line) + LineLength(iCharPos);
	
	
	tstring Line;
	len = l_End - l_Start;
	if(len < 3)
		return Util::emptyStringT;

	Line.resize(len);
	GetTextRange(l_Start, l_End, &Line[0]); //pick the current line from text for starters

	iCharPos = iCharPos - l_Start; //modify the charpos within the range of our new line.

	begin = Line.find_last_of(_T(" \t\r"), iCharPos) + 1;	
	end = Line.find_first_of(_T(" \t\r"), begin);
			if(end == tstring::npos) {
				end = Line.length();
			}
	len = end - begin;
	
	/*a hack, limit to 512, scrolling becomes sad with long words...
	links longer than 512? set ít higher or maybe just limit the cursor detecting?*/
	if((len <= 3) || (len >= 512)) 
		return Util::emptyStringT;

	tstring sText;
	sText = Line.substr(begin, end-begin);
	
	if(!sText.empty()) 
		return sText;
	
	return Util::emptyStringT;
}

LRESULT ChatCtrl::onSearch(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	WinUtil::searchAny(selectedWord);
	SetSelNone();
	return 0;
}

LRESULT ChatCtrl::onSearchTTH(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	WinUtil::searchHash(TTHValue(Text::fromT(selectedWord)));
	SetSelNone();
	return 0;
}


LRESULT ChatCtrl::onSearchSite(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	
	size_t newId = (size_t)wID - IDC_SEARCH_SITES;
	if(newId < (int)WebShortcuts::getInstance()->list.size()) {
		WebShortcut *ws = WebShortcuts::getInstance()->list[newId];
		if(ws != NULL) {
			WinUtil::SearchSite(ws, selectedWord); 
		}
	}
	SetSelNone();
	return S_OK;
}
