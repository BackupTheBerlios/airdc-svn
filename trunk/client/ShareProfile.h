/*
 * Copyright (C) 2012 AirDC++ Project
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

#ifndef DCPLUSPLUS_DCPP_SHAREPROFILE_H_
#define DCPLUSPLUS_DCPP_SHAREPROFILE_H_

#include <string>

#include "forward.h"
#include "Pointer.h"
#include "GetSet.h"
#include "Util.h"
#include "HashValue.h"
#include "TigerHash.h"
#include "File.h"

namespace dcpp {

using std::string;


/*
A Class that holds info on Hub spesific Filelist,
a Full FileList that contains all like it did before is constructed with sharemanager instance, and then updated like before,
this means that we should allways have FileListALL, other lists are just extra.
Now this would be really simple if just used recursive Locks in sharemanager, to protect everything at once.
BUT i dont want freezes and lockups so lets make it a bit more complex :) 
..*/
class FileList {
	public:
		FileList(const string& aProfile);

		GETSET(int64_t, xmlListLen, XmlListLen);
		GETSET(TTHValue, xmlRoot, XmlRoot);
		GETSET(int64_t, bzXmlListLen, BzXmlListLen);
		GETSET(TTHValue, bzXmlRoot, BzXmlRoot);
		GETSET(uint64_t, lastXmlUpdate, LastXmlUpdate);
		GETSET(string, profile, Profile);
		GETSET(bool, xmlDirty, XmlDirty);
		GETSET(bool, forceXmlRefresh, ForceXmlRefresh); /// bypass the 15-minutes guard

		unique_ptr<File> bzXmlRef;
		void increaseN() { listN++; };
		string getFileName();
		string getDefaultFileName();
		string getNFileName();

		bool isDirty(bool force=false);
		void unsetDirty();
		void saveList(SimpleXML& aXml);
	private:
		int listN;
		bool isSavedSuccessfully;
};

class ShareProfile : public intrusive_ptr_base<ShareProfile> {
public:
	bool operator==(const ShareProfilePtr rhs) const {
		return rhs->getToken() == token;
	}

	struct Hash {
		size_t operator()(const ShareProfilePtr x) const { return hash<string>()(x->getToken()); }
	};

	//GETSET(ShareManager::DirMap, shares, Shares);
	GETSET(string, token, Token);
	GETSET(string, name, Name);
	GETSET(FileList*, profileList, ProfileList);
	ShareProfile(const string& aName, const string& aToken = Util::toString(Util::rand()));
	~ShareProfile();

	FileList* generateProfileList();
	typedef unordered_set<ShareProfilePtr, Hash> set;
};

inline bool operator==(ShareProfilePtr ptr, const string& aToken) { return compare(ptr->getToken(), aToken) == 0; }

}

#endif /* DCPLUSPLUS_DCPP_SHAREPROFILE_H_ */