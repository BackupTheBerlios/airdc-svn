/*
 * Copyright (C) 2001-2011 Jacek Sieka, arnetheduck on gmail point com
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

#ifndef DCPLUSPLUS_DCPP_USER_QUEUE_H
#define DCPLUSPLUS_DCPP_USER_QUEUE_H

#include "forward.h"
#include "typedefs.h"
#include "HintedUser.h"
#include "QueueItem.h"

#include "boost/unordered_map.hpp"

namespace dcpp {

/** All queue items indexed by user (this is a cache for the FileQueue really...) */
class UserQueue {
public:
	void add(QueueItem* qi, bool newBundle=false);
	void add(QueueItem* qi, const HintedUser& aUser, bool newBundle=false);
	QueueItem* getNext(const UserPtr& aUser, QueueItem::Priority minPrio = QueueItem::LOWEST, int64_t wantedSize = 0, int64_t lastSpeed = 0, bool allowRemove = false, bool smallSlot=false);
	QueueItem* getNextPrioQI(const UserPtr& aUser, int64_t wantedSize = 0, int64_t lastSpeed = 0, bool smallSlot=false, bool listAll=false);
	QueueItem* getNextBundleQI(const UserPtr& aUser, Bundle::Priority minPrio = Bundle::LOWEST, int64_t wantedSize = 0, int64_t lastSpeed = 0, bool smallSlot=false);
	QueueItemList getRunning(const UserPtr& aUser);
	void addDownload(QueueItem* qi, Download* d);
	void removeDownload(QueueItem* qi, const UserPtr& d, const string& token = Util::emptyString);

	void removeQI(QueueItem* qi, bool removeRunning = true, bool removeBundle=false);
	void removeQI(QueueItem* qi, const UserPtr& aUser, bool removeRunning = true, bool addBad = false, bool removeBundle=false);
	void setQIPriority(QueueItem* qi, QueueItem::Priority p);

	void setBundlePriority(BundlePtr aBundle, Bundle::Priority p);

	boost::unordered_map<UserPtr, BundleList, User::Hash>& getBundleList()  { return userBundleQueue; }
	boost::unordered_map<UserPtr, QueueItemList, User::Hash>& getPrioList()  { return userPrioQueue; }
	boost::unordered_map<UserPtr, QueueItemList, User::Hash>& getRunning()  { return running; }

	string getLastError() { 
		string tmp = lastError;
		lastError = Util::emptyString;
		return tmp;
	}

private:
	/** Bundles by priority and user (this is where the download order is determined) */
	boost::unordered_map<UserPtr, BundleList, User::Hash> userBundleQueue;
	/** High priority QueueItems by user (this is where the download order is determined) */
	boost::unordered_map<UserPtr, QueueItemList, User::Hash> userPrioQueue;
	/** Currently running downloads, a QueueItem is always either here or in the userQueue */
	boost::unordered_map<UserPtr, QueueItemList, User::Hash> running;
	/** Last error message to sent to TransferView */
	string lastError;
};

} // namespace dcpp

#endif // !defined(USER_QUEUE_H)