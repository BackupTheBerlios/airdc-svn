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

#ifndef DCPLUSPLUS_DCPP_QUEUE_MANAGER_LISTENER_H
#define DCPLUSPLUS_DCPP_QUEUE_MANAGER_LISTENER_H

#include "forward.h"
#include "noexcept.h"

namespace dcpp {

class QueueManagerListener {
public:
	virtual ~QueueManagerListener() { }
	template<int I>	struct X { enum { TYPE = I };  };

	typedef X<0> Added;
	typedef X<1> Finished;
	typedef X<2> Removed;
	typedef X<3> Moved;
	typedef X<4> SourcesUpdated;
	typedef X<5> StatusUpdated;
	typedef X<6> PartialList;

	typedef X<8> RecheckStarted;
	typedef X<9> RecheckNoFile;
	typedef X<10> RecheckFileTooSmall;
	typedef X<11> RecheckDownloadsRunning;
	typedef X<12> RecheckNoTree;
	typedef X<13> RecheckAlreadyFinished;
	typedef X<14> RecheckDone;
	
	typedef X<15> BundleSources;

	typedef X<16> BundleFinished;
	typedef X<17> BundleMerged;
	typedef X<18> BundleRemoved;
	typedef X<19> BundleMoved;
	typedef X<20> BundleSize;
	typedef X<21> BundleTarget;
	typedef X<22> BundleUser;
	typedef X<23> BundlePriority;
	typedef X<24> BundleAdded;
	typedef X<25> BundleHashed;

	typedef X<26> FileHashed;

	virtual void on(Added, QueueItemPtr) noexcept { }
	virtual void on(Finished, const QueueItemPtr, const string&, const HintedUser&, int64_t) noexcept { }
	virtual void on(Removed, const QueueItemPtr) noexcept { }
	virtual void on(Moved, const QueueItemPtr, const string&) noexcept { }
	virtual void on(SourcesUpdated, const QueueItemPtr) noexcept { }
	virtual void on(StatusUpdated, const QueueItemPtr) noexcept { }
	virtual void on(PartialList, const HintedUser&, const string&) noexcept { }

	virtual void on(BundleSources, const BundlePtr) noexcept { }
	virtual void on(BundleFinished, const BundlePtr) noexcept { }
	virtual void on(BundleRemoved, const BundlePtr) noexcept { }
	virtual void on(BundleMoved, const BundlePtr) noexcept { }
	virtual void on(BundleMerged, const BundlePtr, const string&) noexcept { }
	virtual void on(BundleSize, const BundlePtr) noexcept { }
	virtual void on(BundleTarget, const BundlePtr) noexcept { }
	virtual void on(BundlePriority, const BundlePtr) noexcept { }
	virtual void on(BundleAdded, const BundlePtr) noexcept { }
	virtual void on(BundleHashed, const string&) noexcept { }
	virtual void on(FileHashed, const string& /* fileName */, const TTHValue& /* root */) noexcept { }
	
	virtual void on(RecheckStarted, const string&) noexcept { }
	virtual void on(RecheckNoFile, const string&) noexcept { }
	virtual void on(RecheckFileTooSmall, const string&) noexcept { }
	virtual void on(RecheckDownloadsRunning, const string&) noexcept { }
	virtual void on(RecheckNoTree, const string&) noexcept { }
	virtual void on(RecheckAlreadyFinished, const string&) noexcept { }
	virtual void on(RecheckDone, const string&) noexcept { }
};

} // namespace dcpp

#endif // !defined(QUEUE_MANAGER_LISTENER_H)

/**
 * @file
 * $Id: QueueManagerListener.h 568 2011-07-24 18:28:43Z bigmuscle $
 */
