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

#ifndef DCPLUSPLUS_DCPP_QUEUE_MANAGER_H
#define DCPLUSPLUS_DCPP_QUEUE_MANAGER_H

# pragma warning(disable: 4512) // assignment operator could not be generated (bimap)

#include "TimerManager.h"
#include "ClientManager.h"

#include "Exception.h"
#include "User.h"
#include "File.h"
#include "QueueItem.h"
#include "Singleton.h"
#include "DirectoryListing.h"
#include "MerkleTree.h"
#include "Socket.h"

#include "QueueManagerListener.h"
#include "SearchManagerListener.h"
#include "ClientManagerListener.h"
#include "DownloadManagerListener.h"
#include "LogManager.h"
#include "HashManager.h"
#include "TargetUtil.h"
#include "StringMatch.h"
#include "TaskQueue.h"

#include "BundleQueue.h"
#include "FileQueue.h"
#include "UserQueue.h"
#include "DelayedEvents.h"
#include "HashBloom.h"
#include "HashedFile.h"

#include "boost/bimap.hpp"
#include <boost/bimap/unordered_multiset_of.hpp>

#include "concurrency.h"


namespace dcpp {

namespace bimaps = boost::bimaps;

STANDARD_EXCEPTION(QueueException);
STANDARD_EXCEPTION(DupeException);

class UserConnection;

class ConnectionQueueItem;
class QueueLoader;

class QueueManager : public Singleton<QueueManager>, public Speaker<QueueManagerListener>, private TimerManagerListener, 
	private SearchManagerListener, private ClientManagerListener, private HashManagerListener
{
public:
	void getBloom(HashBloom& bloom) const;
	size_t getQueuedBundleFiles() const noexcept;
	bool hasDownloadedBytes(const string& aTarget) throw(QueueException);
	uint64_t getTotalQueueSize() const { return fileQueue.getTotalQueueSize(); }

	/** Add a user's filelist to the queue. */
	void addList(const HintedUser& HintedUser, Flags::MaskType aFlags, const string& aInitialDir = Util::emptyString, BundlePtr aBundle=nullptr) throw(QueueException, FileException);

	/** Add an item that is opened in the client or with an external program */
	void addOpenedItem(const string& aFileName, int64_t aSize, const TTHValue& aTTH, const HintedUser& aUse, bool isClientView);

	/** Readd a source that was removed */
	void readdQISource(const string& target, const HintedUser& aUser) throw(QueueException);
	void readdBundleSource(BundlePtr aBundle, const HintedUser& aUser);
	void onUseSeqOrder(BundlePtr& aBundle);

	/** Add a directory to the queue (downloads filelist and matches the directory). */
	void matchListing(const DirectoryListing& dl, int& matches, int& newFiles, BundleList& bundles);

	void removeFile(const string aTarget) noexcept;
	void removeFileSource(const string& aTarget, const UserPtr& aUser, Flags::MaskType reason, bool removeConn = true) noexcept;
	void removeSource(const UserPtr& aUser, Flags::MaskType reason) noexcept;

	void recheck(const string& aTarget);

	void setQIPriority(const string& aTarget, QueueItemBase::Priority p) noexcept;
	void setQIPriority(QueueItemPtr& qi, QueueItemBase::Priority p, bool isAP=false) noexcept;
	void setQIAutoPriority(const string& aTarget) noexcept;

	StringList getTargets(const TTHValue& tth);
	void readLockedOperation(const function<void (const QueueItem::StringMap&)>& currentQueue);

	void onSlowDisconnect(const string& aToken);

	string getTempTarget(const string& aTarget);
	void setSegments(const string& aTarget, uint8_t aSegments);

	bool isFinished(const QueueItemPtr& qi) const { RLock l(cs); return qi->isFinished(); }
	bool isWaiting(const QueueItemPtr& qi) const { RLock l(cs); return qi->isWaiting(); }
	uint64_t getDownloadedBytes(const QueueItemPtr& qi) const { RLock l(cs); return qi->getDownloadedBytes(); }

	QueueItem::SourceList getSources(const QueueItemPtr& qi) const { RLock l(cs); return qi->getSources(); }
	QueueItem::SourceList getBadSources(const QueueItemPtr& qi) const { RLock l(cs); return qi->getBadSources(); }

	Bundle::SourceList getBundleSources(const BundlePtr& b) const { RLock l(cs); return b->getSources(); }
	Bundle::SourceList getBadBundleSources(const BundlePtr& b) const { RLock l(cs); return b->getBadSources(); }

	size_t getSourcesCount(const QueueItemPtr& qi) const { RLock l(cs); return qi->getSources().size(); }
	void getChunksVisualisation(const QueueItemPtr& qi, vector<Segment>& running, vector<Segment>& downloaded, vector<Segment>& done) const { RLock l(cs); qi->getChunksVisualisation(running, downloaded, done); }

	bool getQueueInfo(const HintedUser& aUser, string& aTarget, int64_t& aSize, int& aFlags, string& bundleToken) noexcept;
	Download* getDownload(UserConnection& aSource, const OrderedStringSet& onlineHubs, string& aMessage, string& newUrl, bool smallSlot) noexcept;
	void putDownload(Download* aDownload, bool finished, bool noAccess=false, bool rotateQueue=false) noexcept;
	
	/** @return The highest priority download the user has, PAUSED may also mean no downloads */
	QueueItemBase::Priority hasDownload(const UserPtr& aUser, const OrderedStringSet& onlineHubs, bool smallSlot) noexcept;
	/** The same thing but only used before any connect requests */
	QueueItemBase::Priority hasDownload(const UserPtr& aUser, string& hubUrl, bool smallSlot, string& bundleToken, bool& allowUrlChange) noexcept;
	
	void loadQueue(function<void (float)> progressF) noexcept;
	void saveQueue(bool force) noexcept;

	void noDeleteFileList(const string& path);

	//merging, adding, deletion
	BundlePtr createDirectoryBundle(const string& aTarget, const HintedUser& aUser, BundleFileList& aFiles, QueueItemBase::Priority aPrio, time_t aDate, string& errorMsg_) noexcept;
	BundlePtr createFileBundle(const string& aTarget, int64_t aSize, const TTHValue& aTTH, const HintedUser& aUser, time_t aDate, Flags::MaskType aFlags = 0, QueueItemBase::Priority aPrio = QueueItem::DEFAULT) throw(QueueException, FileException);
	void moveBundleDir(const string& aSource, const string& aTarget, BundlePtr sourceBundle, bool moveFinished);
	void moveFileBundle(BundlePtr& aBundle, const string& aTarget) noexcept;
	void removeBundle(BundlePtr& aBundle, bool finished, bool removeFinished, bool moved = false);


	BundlePtr findBundle(const string& bundleToken) { RLock l (cs); return bundleQueue.findBundle(bundleToken); }
	BundlePtr findBundle(const TTHValue& tth);

	/* Partial bundle sharing */
	bool checkPBDReply(HintedUser& aUser, const TTHValue& aTTH, string& _bundleToken, bool& _notify, bool& _add, const string& remoteBundle);
	void addFinishedNotify(HintedUser& aUser, const TTHValue& aTTH, const string& remoteBundle);
	void updatePBD(const HintedUser& aUser, const TTHValue& aTTH);
	void removeBundleNotify(const UserPtr& aUser, const string& bundleToken);
	void sendRemovePBD(const HintedUser& aUser, const string& aRemoteToken);
	bool getSearchInfo(const string& aTarget, TTHValue& tth_, int64_t size_) noexcept;
	bool handlePartialSearch(const UserPtr& aUser, const TTHValue& tth, PartsInfo& _outPartsInfo, string& _bundle, bool& _reply, bool& _add);
	bool handlePartialResult(const HintedUser& aUser, const TTHValue& tth, const QueueItem::PartialSource& partialSource, PartsInfo& outPartialInfo);
	void addBundleTTHList(const HintedUser& aUser, const string& bundle, const TTHValue& tth);
	MemoryInputStream* generateTTHList(const string& bundleToken, bool isInSharingHub);


	/* Priorities */
	void setBundlePriority(const string& bundleToken, QueueItemBase::Priority p) noexcept;
	void setBundlePriority(BundlePtr& aBundle, QueueItemBase::Priority p, bool isAuto=false) noexcept;
	void setBundleAutoPriority(const string& bundleToken) noexcept;
	void calculateBundlePriorities(bool verbose);

	void removeBundleSource(const string& bundleToken, const UserPtr& aUser, Flags::MaskType reason) noexcept;
	void removeBundleSource(BundlePtr aBundle, const UserPtr& aUser, Flags::MaskType reason) noexcept;

	void handleSlowDisconnect(const UserPtr& aUser, const string& aTarget, const BundlePtr& aBundle);

	/** Move the target location of a queued item. Running items are silently ignored */
	void moveFiles(const StringPairList& sourceTargetList) noexcept;
	void removeDir(const string aSource, const BundleList& sourceBundles, bool removeFinished);

	void searchBundle(BundlePtr& aBundle, bool manual);

	/* Info collecting */
	void getBundleInfo(const string& aSource, BundleList& retBundles, int& finishedFiles, int& fileBundles) { 
		RLock l (cs); 
		bundleQueue.getInfo(aSource, retBundles, finishedFiles, fileBundles); 
	}
	int getBundleItemCount(const BundlePtr& aBundle) const noexcept;
	int getFinishedItemCount(const BundlePtr& aBundle) const noexcept;
	void getDirItems(const BundlePtr& aBundle, const string& aDir, QueueItemList& aItems) const noexcept;
	void getSourceInfo(const UserPtr& aUser, Bundle::SourceBundleList& aSources, Bundle::SourceBundleList& aBad) const;

	int isFileQueued(const TTHValue& aTTH, const string& aFile) { RLock l (cs); return fileQueue.isFileQueued(aTTH, aFile); }
	
	bool dropSource(Download* d);

	bool isChunkDownloaded(const TTHValue& tth, int64_t startPos, int64_t& bytes, int64_t& fileSize_, string& tempTarget);
	string getBundlePath(const string& aBundleToken) const;
	uint8_t isDirQueued(const string& aDir) const;
	tstring getDirPath(const string& aDir) const;

	void getDiskInfo(TargetUtil::TargetInfoMap& dirMap, const TargetUtil::VolumeSet& volumes) const { RLock l (cs); bundleQueue.getDiskInfo(dirMap, volumes); }
	void getUnfinishedPaths(StringList& bundles);
	void getForbiddenPaths(StringList& bundlePaths, const StringList& sharePaths);
	
	GETSET(uint64_t, lastSave, LastSave);
	GETSET(uint64_t, lastAutoPrio, LastAutoPrio);

	class FileMover : public Thread {
	public:
		enum Tasks {
			MOVE_FILE,
			REMOVE_DIR,
			SHUTDOWN
		};

		FileMover();
		virtual ~FileMover();

		void moveFile(const string& source, const string& target, QueueItemPtr aBundle);
		void removeDir(const string& aDir);
		void shutdown();
		virtual int run();
	private:

		Semaphore s;
		TaskQueue tasks;
	} mover;

	class Rechecker : public Thread {

		public:
			explicit Rechecker(QueueManager* qm_) : qm(qm_), active(false) { }
			virtual ~Rechecker() { join(); }

			void add(const string& file);
			virtual int run();

		private:
			QueueManager* qm;
			bool active;

			StringList files;
			CriticalSection cs;
	} rechecker;

	void shareBundle(const string& aName);
	void runAltSearch();

	RLock lockRead() { return RLock(cs); }

	void setMatchers();
	void shutdown();
private:
	friend class QueueLoader;
	friend class Singleton<QueueManager>;
	
	QueueManager();
	~QueueManager();
	
	mutable SharedMutex cs;

	Socket udp;

	/** QueueItems by target and TTH */
	FileQueue fileQueue;

	/** Bundles by target */
	BundleQueue bundleQueue;

	/** QueueItems by user */
	UserQueue userQueue;

	/** File lists not to delete */
	StringList protectedFileLists;

	task_group tasks;

	void connectBundleSources(BundlePtr& aBundle);

	int changeBundleTarget(BundlePtr& aBundle, const string& newTarget);
	void removeBundleItem(QueueItemPtr& qi, bool finished, bool moved = false);
	void moveBundleItem(QueueItemPtr qi, BundlePtr& targetBundle, bool fireAdded); //don't use reference here!
	void moveBundleItems(const QueueItemList& ql, BundlePtr& targetBundle);
	void addLoadedBundle(BundlePtr& aBundle);
	bool addBundle(BundlePtr& aBundle, const string& aTarget, int filesAdded, bool moving = false);
	void readdBundle(BundlePtr& aBundle);

	bool changeTarget(QueueItemPtr& qs, const string& aTarget, bool movingSingleItems) noexcept;
	void removeQI(QueueItemPtr& qi, bool noFiring = false) noexcept;

	void handleBundleUpdate(const string& bundleToken);

	/** Get a bundle for adding new items in queue (a new one or existing)  */
	BundlePtr getBundle(const string& aTarget, QueueItemBase::Priority aPrio, time_t aDate, bool isFileBundle);

	/** Add a file to the queue (returns the item and whether it didn't exist before) */
	bool addFile(const string& aTarget, int64_t aSize, const TTHValue& root, const HintedUser& aUser, Flags::MaskType aFlags, bool addBad, QueueItemBase::Priority aPrio, bool& wantConnection, bool& smallSlot, BundlePtr& aBundle) throw(QueueException, FileException);

	/** Check that we can download from this user */
	void checkSource(const UserPtr& aUser) const throw(QueueException);

	/** Check that we can download from this user */
	void validateBundleFile(const string& aBundleDir, string& aBundleFile, const TTHValue& aTTH, QueueItemBase::Priority& aPrio) const throw(QueueException);

	/** Sanity check for the target filename */
	//static string checkTargetPath(const string& aTarget) throw(QueueException, FileException);
	static string checkTarget(const string& toValidate, const string& aParentDir=Util::emptyString) throw(QueueException, FileException);
	static string formatBundleTarget(const string& aPath, time_t aRemoteDate);

	/** Add a source to an existing queue item */
	bool addSource(QueueItemPtr& qi, const HintedUser& aUser, Flags::MaskType addBad, bool newBundle=false, bool checkTLS=true) throw(QueueException, FileException);

	/** Add a source to an existing queue item */
	void mergeFinishedItems(const string& aSource, const string& aTarget, BundlePtr& aSourceBundle, BundlePtr& aTargetBundle, bool moveFiles);
	 
	void matchTTHList(const string& name, const HintedUser& user, int flags);

	void addBundleUpdate(const BundlePtr& aBundle);

	void addFinishedItem(const TTHValue& tth, BundlePtr& aBundle, const string& aTarget, time_t aSize, int64_t aFinished);

	void load(const SimpleXML& aXml);

	static void moveFile_(const string& source, const string& target, QueueItemPtr& q);

	void handleMovedBundleItem(QueueItemPtr& q);
	void checkBundleFinished(BundlePtr& aBundle, bool isPrivate);

	unordered_map<string, SearchResultList> searchResults;
	void pickMatch(QueueItemPtr qi);
	void matchBundle(QueueItemPtr& aQI, const SearchResultPtr& aResult);

	void moveStuckFile(QueueItemPtr& qi);
	void rechecked(QueueItemPtr& qi);
	void onFileHashed(const string& aPath, HashedFile& aFileInfo, bool failed);
	void hashBundle(BundlePtr& aBundle);
	bool scanBundle(BundlePtr& aBundle);
	void checkBundleHashed(BundlePtr& aBundle);
	void setBundleStatus(BundlePtr& aBundle, Bundle::Status newStatus);
	void removeFinishedBundle(BundlePtr& aBundle);

	/* Returns true if an item can be replaces */
	bool replaceItem(QueueItemPtr& qi, int64_t aSize, const TTHValue& aTTH);

	void removeFileSource(QueueItemPtr& qi, const UserPtr& aUser, Flags::MaskType reason, bool removeConn = true) noexcept;

	string getListPath(const HintedUser& user);

	StringMatch highPrioFiles;
	StringMatch skipList;

	// TimerManagerListener
	void on(TimerManagerListener::Second, uint64_t aTick) noexcept;
	void on(TimerManagerListener::Minute, uint64_t aTick) noexcept;
	
	// SearchManagerListener
	void on(SearchManagerListener::SR, const SearchResultPtr&) noexcept;
	
	// HashManagerListener
	void on(HashManagerListener::TTHDone, const string& aPath, HashedFile& fi) noexcept { onFileHashed(aPath, fi, false); }
	void on(HashManagerListener::HashFailed, const string& aPath, HashedFile& fi) noexcept { onFileHashed(aPath, fi, true); }

	// ClientManagerListener
	void on(ClientManagerListener::UserConnected, const OnlineUser& aUser, bool wasOffline) noexcept;
	void on(ClientManagerListener::UserDisconnected, const UserPtr& aUser, bool wentOffline) noexcept;

	DelayedEvents<string> delayEvents;

	typedef boost::bimap<bimaps::unordered_multiset_of<string>, bimaps::unordered_multiset_of<string>> StringMultiBiMap;
	StringMultiBiMap matchLists;
};

} // namespace dcpp

#endif // !defined(QUEUE_MANAGER_H)