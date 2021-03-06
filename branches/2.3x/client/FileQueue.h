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

#ifndef DCPLUSPLUS_DCPP_FILE_QUEUE_H
#define DCPLUSPLUS_DCPP_FILE_QUEUE_H

#include "stdinc.h"

#include "forward.h"
#include "typedefs.h"
#include "HintedUser.h"
#include "QueueItem.h"
#include "DirectoryListing.h"

#include "boost/unordered_map.hpp"

namespace dcpp {

/** All queue items indexed by user (this is a cache for the FileQueue really...) **/
class FileQueue {
public:
	FileQueue() : targetMapInsert(queue.end()), queueSize(0) { }
	~FileQueue();

	void decreaseSize(uint64_t aSize) { queueSize -= aSize; }
	typedef vector<pair<QueueItem::SourceConstIter, const QueueItemPtr> > PFSSourceList;

	void add(QueueItemPtr qi);
	QueueItemPtr add(const string& aTarget, int64_t aSize, Flags::MaskType aFlags, QueueItem::Priority p, const string& aTempTarget, time_t aAdded, const TTHValue& root) noexcept;

	QueueItemPtr find(const string& target) noexcept;
	void find(QueueItemList& sl, int64_t aSize, const string& ext) noexcept;
	void find(StringList& sl, int64_t aSize, const string& ext) noexcept;
	void find(const TTHValue& tth, QueueItemList& ql) noexcept;
	void matchDir(const DirectoryListing::Directory* dir, QueueItemList& ql) noexcept;

	// find some PFS sources to exchange parts info
	void findPFSSources(PFSSourceList&);

	size_t getSize() noexcept { return queue.size(); }
	QueueItem::StringMap& getQueue() noexcept { return queue; }
	QueueItem::TTHMap& getTTHIndex() noexcept { return tthIndex; }
	void move(QueueItemPtr qi, const string& aTarget) noexcept;
	void remove(QueueItemPtr qi) noexcept;
	int isFileQueued(const TTHValue& aTTH, const string& aFile) noexcept;
	QueueItemPtr getQueuedFile(const TTHValue& aTTH, const string& aFile) noexcept;

	uint64_t getTotalQueueSize() noexcept { return queueSize; }
private:
	QueueItem::StringMap queue;
	QueueItem::TTHMap tthIndex;

	uint64_t queueSize;
	QueueItem::StringMap::iterator targetMapInsert;
};

} // namespace dcpp

#endif // !defined(FILE_QUEUE_H)
