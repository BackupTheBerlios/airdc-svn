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

#include "stdinc.h"
#include "QueueManager.h"

#include "AutoSearchManager.h"
#include "AirUtil.h"
#include "Bundle.h"
#include "ClientManager.h"
#include "ConnectionManager.h"
#include "DirectoryListing.h"
#include "Download.h"
#include "DownloadManager.h"
#include "FileReader.h"
#include "format.h"
#include "HashManager.h"
#include "LogManager.h"
#include "ResourceManager.h"
#include "ScopedFunctor.h"
#include "SearchManager.h"
#include "ShareScannerManager.h"
#include "ShareManager.h"
#include "SimpleXMLReader.h"
#include "StringTokenizer.h"
#include "Transfer.h"
#include "UploadManager.h"
#include "UserConnection.h"
#include "version.h"
#include "Wildcards.h"
#include "SearchResult.h"
#include "DirectoryListingManager.h"

#include <mmsystem.h>
#include <limits>

#if !defined(_WIN32) && !defined(PATH_MAX) // Extra PATH_MAX check for Mac OS X
#include <sys/syslimits.h>
#endif

#ifdef ff
#undef ff
#endif

namespace dcpp {

using boost::range::for_each;

#ifdef ATOMIC_FLAG_INIT
atomic_flag QueueManager::FileMover::active = ATOMIC_FLAG_INIT;
#else
atomic_flag QueueManager::FileMover::active;
#endif

struct MoverTask : public Task {
	MoverTask(const string& aSource, const string& aTarget, QueueItemPtr aQI) : target(aTarget), source(aSource), qi(aQI) { }

	string target, source;
	QueueItemPtr qi;
};

void QueueManager::FileMover::moveFile(const string& source, const string& target, QueueItemPtr q) {
	tasks.add(MOVE_FILE, unique_ptr<Task>(new MoverTask(source, target, q)));
	if(!active.test_and_set()) {
		start();
		setThreadPriority(Thread::LOW);
	}
}

void QueueManager::FileMover::removeDir(const string& aDir) {
	tasks.add(REMOVE_DIR, unique_ptr<StringTask>(new StringTask(aDir)));
	if(!active.test_and_set()) {
		start();
		setThreadPriority(Thread::LOW);
	}
}

int QueueManager::FileMover::run() {
	for(;;) {
		TaskQueue::TaskPair t;
		if (!tasks.getFront(t)) {
			active.clear();
			return 0;
		}

		if (t.first == MOVE_FILE) {
			auto mv = static_cast<MoverTask*>(t.second);
			moveFile_(mv->source, mv->target, mv->qi);
		} else if (t.first == REMOVE_DIR) {
			auto dir = static_cast<StringTask*>(t.second);
			AirUtil::removeIfEmpty(dir->str);
		}

		tasks.pop_front();
	}
}

void QueueManager::Rechecker::add(const string& file) {
	Lock l(cs);
	files.push_back(file);
	if(!active) {
		active = true;
		start();
	}
}

int QueueManager::Rechecker::run() {
	while(true) {
		string file;
		{
			Lock l(cs);
			auto i = files.begin();
			if(i == files.end()) {
				active = false;
				return 0;
			}
			file = *i;
			files.erase(i);
		}

		QueueItemPtr q;
		int64_t tempSize;
		TTHValue tth;

		{
			RLock l(qm->cs);

			q = qm->fileQueue.findFile(file);
			if(!q || q->isSet(QueueItem::FLAG_USER_LIST))
				continue;

			qm->fire(QueueManagerListener::RecheckStarted(), q->getTarget());
			dcdebug("Rechecking %s\n", file.c_str());

			tempSize = File::getSize(q->getTempTarget());

			if(tempSize == -1) {
				qm->fire(QueueManagerListener::RecheckNoFile(), q->getTarget());
				continue;
			}

			if(tempSize < 64*1024) {
				qm->fire(QueueManagerListener::RecheckFileTooSmall(), q->getTarget());
				continue;
			}

			if(tempSize != q->getSize()) {
				File(q->getTempTarget(), File::WRITE, File::OPEN).setSize(q->getSize());
			}

			if(q->isRunning()) {
				qm->fire(QueueManagerListener::RecheckDownloadsRunning(), q->getTarget());
				continue;
			}

			tth = q->getTTH();
		}

		TigerTree tt;
		bool gotTree = HashManager::getInstance()->getTree(tth, tt);

		string tempTarget;

		{
			RLock l(qm->cs);

			// get q again in case it has been (re)moved
			q = qm->fileQueue.findFile(file);
			if(!q)
				continue;

			if(!gotTree) {
				qm->fire(QueueManagerListener::RecheckNoTree(), q->getTarget());
				continue;
			}

			//Clear segments
			q->resetDownloaded();

			tempTarget = q->getTempTarget();
		}

		TigerTree ttFile(tt.getBlockSize());

		try {
			FileReader().read(tempTarget, [&](const void* x, size_t n) {
				return ttFile.update(x, n), true;
			});
		} catch(const FileException & e) {
			dcdebug("Error while reading file: %s\n", e.what());
		}

		{
			RLock l(qm->cs);
			// get q again in case it has been (re)moved
			q = qm->fileQueue.findFile(file);
		}

		if(!q)
			continue;

		ttFile.finalize();

		if(ttFile.getRoot() == tth) {
			//If no bad blocks then the file probably got stuck in the temp folder for some reason
			qm->moveStuckFile(q);
			continue;
		}

		size_t pos = 0;

		{
			WLock l(qm->cs);
			boost::for_each(tt.getLeaves(), ttFile.getLeaves(), [&](const TTHValue& our, const TTHValue& file) {
				if(our == file) {
					q->addFinishedSegment(Segment(pos, tt.getBlockSize()));
				}

				pos += tt.getBlockSize();
			});
		}

		qm->rechecked(q);
	}
	return 0;
}

QueueManager::QueueManager() : 
	lastSave(0),
	lastAutoPrio(0),
	rechecker(this),
	udp(Socket::TYPE_UDP)
{ 
	//add listeners in loadQueue
	File::ensureDirectory(Util::getListPath());
	File::ensureDirectory(Util::getBundlePath());
}

QueueManager::~QueueManager() noexcept { 
	SearchManager::getInstance()->removeListener(this);
	TimerManager::getInstance()->removeListener(this); 
	ClientManager::getInstance()->removeListener(this);
	HashManager::getInstance()->removeListener(this);

	saveQueue(true);

	if(!SETTING(KEEP_LISTS)) {
		string path = Util::getListPath();

		std::sort(protectedFileLists.begin(), protectedFileLists.end());

		StringList filelists = File::findFiles(path, "*.xml.bz2");
		std::sort(filelists.begin(), filelists.end());
		std::for_each(filelists.begin(), std::set_difference(filelists.begin(), filelists.end(),
			protectedFileLists.begin(), protectedFileLists.end(), filelists.begin()), &File::deleteFile);

		filelists = File::findFiles(path, "*.DcLst");
		std::sort(filelists.begin(), filelists.end());
		std::for_each(filelists.begin(), std::set_difference(filelists.begin(), filelists.end(),
			protectedFileLists.begin(), protectedFileLists.end(), filelists.begin()), &File::deleteFile);
	}
}

void QueueManager::getBloom(HashBloom& bloom) const {
	RLock l(cs);
	fileQueue.getBloom(bloom);
}

size_t QueueManager::getQueuedFiles() const noexcept {
	RLock l(cs);
	return bundleQueue.getTotalFiles();
}

bool QueueManager::getSearchInfo(const string& aTarget, TTHValue& tth_, int64_t size_) noexcept {
	RLock l(cs);
	QueueItemPtr qi = fileQueue.findFile(aTarget);
	if(qi) {
		tth_ = qi->getTTH();
		size_ = qi->getSize();
		return true;
	}
	return false;
}

struct PartsInfoReqParam{
	PartsInfo	parts;
	string		tth;
	string		myNick;
	string		hubIpPort;
	string		ip;
	string		udpPort;
};

void QueueManager::on(TimerManagerListener::Minute, uint64_t aTick) noexcept {
	BundlePtr bundle;
	vector<const PartsInfoReqParam*> params;

	{
		RLock l(cs);

		//find max 10 pfs sources to exchange parts
		//the source basis interval is 5 minutes
		FileQueue::PFSSourceList sl;
		fileQueue.findPFSSources(sl);

		for(auto& i: sl) {
			QueueItem::PartialSource::Ptr source = i.first->getPartialSource();
			const QueueItemPtr qi = i.second;

			PartsInfoReqParam* param = new PartsInfoReqParam;
			
			int64_t blockSize = HashManager::getInstance()->getBlockSize(qi->getTTH());
			if(blockSize == 0)
				blockSize = qi->getSize();			
			qi->getPartialInfo(param->parts, blockSize);
			
			param->tth = qi->getTTH().toBase32();
			param->ip  = source->getIp();
			param->udpPort = source->getUdpPort();
			param->myNick = source->getMyNick();
			param->hubIpPort = source->getHubIpPort();

			params.push_back(param);

			source->setPendingQueryCount((uint8_t)(source->getPendingQueryCount() + 1));
			source->setNextQueryTime(aTick + 300000);		// 5 minutes
		}

		if (SETTING(AUTO_SEARCH) && SETTING(AUTO_ADD_SOURCE))
			bundle = bundleQueue.findSearchBundle(aTick); //may modify the recent search queue
	}

	if(bundle) {
		searchBundle(bundle, false);
	}

	// Request parts info from partial file sharing sources
	for(auto& param: params){
		//dcassert(param->udpPort > 0);
		
		try {
			AdcCommand cmd = SearchManager::getInstance()->toPSR(true, param->myNick, param->hubIpPort, param->tth, param->parts);
			udp.writeTo(param->ip, param->udpPort, cmd.toString(ClientManager::getInstance()->getMyCID()));
		} catch(...) {
			dcdebug("Partial search caught error\n");		
		}
		
		delete param;
	}
}

bool QueueManager::hasDownloadedBytes(const string& aTarget) throw(QueueException) {
	RLock l(cs);
	auto q = fileQueue.findFile(aTarget);
	if (!q)
		throw QueueException(STRING(TARGET_REMOVED));

	return q->getDownloadedBytes() > 0;
}

void QueueManager::addList(const HintedUser& aUser, Flags::MaskType aFlags, const string& aInitialDir /* = Util::emptyString */) throw(QueueException, FileException) {
	addFile(aInitialDir, -1, TTHValue(), aUser, Util::emptyString, (Flags::MaskType)(QueueItem::FLAG_USER_LIST | aFlags));
}

string QueueManager::getListPath(const HintedUser& user) {
	StringList nicks = ClientManager::getInstance()->getNicks(user);
	string nick = nicks.empty() ? Util::emptyString : Util::cleanPathChars(nicks[0]) + ".";
	return checkTarget(Util::getListPath() + nick + user.user->getCID().toBase32(), /*checkExistence*/ false);
}

bool QueueManager::replaceFinishedItem(QueueItemPtr q) {
	if (!Util::fileExists(q->getTarget()) && q->getBundle() && q->isSet(QueueItem::FLAG_MOVED)) {
		bundleQueue.removeFinishedItem(q);
		fileQueue.remove(q);
		return true;
	}
	return false;
}

void QueueManager::setMatchers() {
	auto sl = SETTING(SKIPLIST_DOWNLOAD);
	skipList.pattern = SETTING(SKIPLIST_DOWNLOAD);
	skipList.setMethod(SETTING(DOWNLOAD_SKIPLIST_USE_REGEXP) ? StringMatch::REGEX : StringMatch::WILDCARD);
	skipList.prepare();

	highPrioFiles.pattern = SETTING(HIGH_PRIO_FILES);
	highPrioFiles.setMethod(SETTING(HIGHEST_PRIORITY_USE_REGEXP) ? StringMatch::REGEX : StringMatch::WILDCARD);
	highPrioFiles.prepare();
}

void QueueManager::addFile(const string& aTarget, int64_t aSize, const TTHValue& root, const HintedUser& aUser, const string& aRemotePath, 
	Flags::MaskType aFlags /* = 0 */, bool addBad /* = true */, QueueItem::Priority aPrio, BundlePtr aBundle /*NULL*/, ProfileToken aAutoSearch /*0*/) throw(QueueException, FileException)
{
	bool wantConnection = true;

	// Check that we're not downloading from ourselves...
	if(aUser == ClientManager::getInstance()->getMe()) {
		throw QueueException(STRING(NO_DOWNLOADS_FROM_SELF));
	}

	if (aUser.user && !aUser.user->isSet(User::NMDC) && !aUser.user->isSet(User::TLS) && SETTING(TLS_MODE) == SettingsManager::TLS_FORCED) {
		throw QueueException(Util::toString(ClientManager::getInstance()->getNicks(aUser)) + ": " + STRING(SOURCE_NO_ENCRYPTION));
	}
	
	string target;
	string tempTarget;
	if((aFlags & QueueItem::FLAG_USER_LIST)) {
		if((aFlags & QueueItem::FLAG_PARTIAL_LIST) && !aTarget.empty()) {
			StringList nicks = ClientManager::getInstance()->getNicks(aUser);
			if (nicks.empty())
				throw QueueException(STRING(INVALID_TARGET_FILE));
			target = Util::getListPath() + nicks[0] + ".partial[" + Util::cleanPathChars(aTarget) + "]";
		} else {
			target = getListPath(aUser);
		}
		tempTarget = aTarget;
	} else {
		target = aTarget;
		if (!(aFlags & QueueItem::FLAG_CLIENT_VIEW) && !(aFlags & QueueItem::FLAG_OPEN)) {
			if (!aBundle)
				target = Util::formatTime(aTarget, time(NULL));

			if (SETTING(DONT_DL_ALREADY_SHARED) && ShareManager::getInstance()->isFileShared(root, Util::getFileName(aTarget))) {
				// Check if we're not downloading something already in our share
				LogManager::getInstance()->message(STRING(FILE_ALREADY_SHARED) + " " + aTarget, LogManager::LOG_INFO);
				throw QueueException(STRING(TTH_ALREADY_SHARED));
			}

			if(skipList.match(Util::getFileName(aTarget))) {
				LogManager::getInstance()->message(STRING(DOWNLOAD_SKIPLIST_MATCH) + ": " + aTarget, LogManager::LOG_INFO);
				throw QueueException(STRING(DOWNLOAD_SKIPLIST_MATCH));
			}

			if (highPrioFiles.match(Util::getFileName(aTarget))) {
				aPrio = SETTING(PRIO_LIST_HIGHEST) ? QueueItem::HIGHEST : QueueItem::HIGH;
			}
		} else if (aFlags & QueueItem::FLAG_TEXT && aSize > 1*1024*1024) { // 1MB
			auto msg = STRING_F(VIEWED_FILE_TOO_BIG, aTarget % Util::formatBytes(aSize));
			LogManager::getInstance()->message(msg, LogManager::LOG_ERROR);
			throw QueueException(msg);
		}
		
		//we can check the existence and throw even with FTPlogger support, if the file exists already the directory must exist too.
		target = checkTarget(target, /*checkExistence*/ true);

		if(SETTING(USE_FTP_LOGGER)) {
			AirUtil::fileEvent(target);
		} 
	}

	// Check if it's a zero-byte file, if so, create and return...
	if(aSize == 0) {
		if(!SETTING(SKIP_ZERO_BYTE)) {
			File::ensureDirectory(target);
			File f(target, File::WRITE, File::CREATE);
		}
		return;
	}

	bool bundleFinished = false;
	QueueItemPtr q = nullptr;
	{
		WLock l(cs);
		q = fileQueue.findFile(target);
		if(q) {
			if(q->isFinished()) {
				/* The target file doesn't exist, add our item. Also recheck the existance in case of finished files being moved on the same time. */
				dcassert(q->getBundle());
				if (replaceFinishedItem(q)) {
					q = nullptr;
				} else {
					throw QueueException(STRING(FILE_ALREADY_FINISHED));
				}
			} else {
				/* try to add the source for the existing item */
				if(q->getSize() != aSize) {
					throw QueueException(STRING(FILE_WITH_DIFFERENT_SIZE));
				}
				if(!(root == q->getTTH())) {
					throw QueueException(STRING(FILE_WITH_DIFFERENT_TTH));
				}

				q->setFlag(aFlags);
				wantConnection = aUser.user && addSource(q, aUser, (Flags::MaskType)(addBad ? QueueItem::Source::FLAG_MASK : 0), aRemotePath, aBundle);
				goto connect;
			}
		}

		if(!(aFlags & QueueItem::FLAG_USER_LIST) && !(aFlags & QueueItem::FLAG_CLIENT_VIEW) && !(aFlags & QueueItem::FLAG_OPEN)) {
			if (SETTING(DONT_DL_ALREADY_QUEUED) && !SettingsManager::lanMode) {
				q = fileQueue.getQueuedFile(root, Util::getFileName(aTarget));
				if (q) {
					if (q->isFinished()) {
						/* The target file doesn't exist, add it. Also recheck the existance in case of finished files being moved on the same time. */
						dcassert(q->getBundle());
						if (replaceFinishedItem(q)) {
							q = nullptr;
						}
					} else if (q->isSet(QueueItem::FLAG_CLIENT_VIEW)) {
						q = nullptr;
					} else { 
						if(!q->isSource(aUser)) {
							try {
								if (addSource(q, aUser, addBad ? QueueItem::Source::FLAG_MASK : 0, aRemotePath)) {
									wantConnection = true;
									goto connect;
								}
							} catch(const Exception&) {
								//...
							}
						}
					}

					if (q) {
						string tmp = STRING_F(FILE_ALREADY_QUEUED, Util::getFileName(target).c_str() % q->getTarget().c_str());
						LogManager::getInstance()->message(tmp, LogManager::LOG_ERROR);
						throw QueueException(tmp);
					}
				}
			}
		} else {
			aPrio = QueueItem::HIGHEST;
		}

		q = fileQueue.add( target, aSize, aFlags, aPrio, tempTarget, GET_TIME(), SettingsManager::lanMode ? AirUtil::getTTH(Util::getFileName(target), aSize) : root);

		/* Bundles */
		if (aBundle) {
			if (aBundle->getPriority() == Bundle::PAUSED && q->getPriority() == QueueItem::HIGHEST) {
				q->setPriority(QueueItem::HIGH);
			}
			bundleQueue.addBundleItem(q, aBundle);
		} else if (!(aFlags & QueueItem::FLAG_USER_LIST) && !(aFlags & QueueItem::FLAG_CLIENT_VIEW) && !(aFlags & QueueItem::FLAG_OPEN)) {
			aBundle = bundleQueue.getMergeBundle(q->getTarget());
			if (aBundle) {
				//finished bundle but failed hashing/scanning?
				bundleFinished = aBundle->isFinished();

				bundleQueue.addBundleItem(q, aBundle);
				if (aAutoSearch > 0)
					aBundle->addAutoSearch(aAutoSearch);
				aBundle->setDirty();
			} else {
				ProfileTokenSet as;
				if (aAutoSearch > 0)
					as.insert(aAutoSearch);
				aBundle = new Bundle(q, as);
			}
		}
		/* Bundles end */

		try {
			wantConnection = aUser.user && addSource(q, aUser, (Flags::MaskType)(addBad ? QueueItem::Source::FLAG_MASK : 0), aRemotePath, aBundle);
		} catch(const Exception&) {
			//...
		}
	}

	if (aBundle) {
		if (aBundle->isSet(Bundle::FLAG_NEW)) {
			if (aBundle->isFileBundle()) {
				addBundle(aBundle);
			}
			/* Connect in addBundle */
			return;
		} else {
			if (!bundleFinished) {
				/* Merged into an existing dir bundle */
				LogManager::getInstance()->message(STRING_F(BUNDLE_ITEM_ADDED, q->getTarget().c_str() % aBundle->getName().c_str()), LogManager::LOG_INFO);

				addBundleUpdate(aBundle);
			} else {
				readdBundle(aBundle);
			}

			fire(QueueManagerListener::Added(), q);
		}
	} else {
		fire(QueueManagerListener::Added(), q);
	}
connect:
	bool smallSlot = (q->isSet(QueueItem::FLAG_PARTIAL_LIST) || (q->getSize() <= 65792 && !q->isSet(QueueItem::FLAG_USER_LIST) && q->isSet(QueueItem::FLAG_CLIENT_VIEW)));
	if(!aUser.user || !aUser.user->isOnline())
		return;

	if(wantConnection || smallSlot) {
		ConnectionManager::getInstance()->getDownloadConnection(aUser, smallSlot);
	}
}

void QueueManager::readdQISource(const string& target, const HintedUser& aUser) throw(QueueException) {
	bool wantConnection = false;
	{
		WLock l(cs);
		QueueItemPtr q = fileQueue.findFile(target);
		if(q && q->isBadSource(aUser)) {
			wantConnection = addSource(q, aUser, QueueItem::Source::FLAG_MASK, Util::emptyString); //FIX
		}
	}
	if(wantConnection && aUser.user->isOnline())
		ConnectionManager::getInstance()->getDownloadConnection(aUser);
}

void QueueManager::readdBundleSource(BundlePtr aBundle, const HintedUser& aUser) {
	bool wantConnection = false;
	{
		WLock l(cs);
		auto& ql = aBundle->getQueueItems();
		for(auto& q: ql) {
			dcassert(!q->isSource(aUser));
			if(q && q->isBadSource(aUser.user)) {
				try {
					if (addSource(q, aUser, QueueItem::Source::FLAG_MASK, Util::emptyString)) { //FIX
						wantConnection = true;
					}
				} catch(...) {
					LogManager::getInstance()->message("Failed to add the source for " + q->getTarget(), LogManager::LOG_WARNING);
				}
			}
		}
	}

	if(wantConnection && aUser.user->isOnline())
		ConnectionManager::getInstance()->getDownloadConnection(aUser);
}

string QueueManager::checkTarget(const string& aTarget, bool checkExistence, BundlePtr aBundle) throw(QueueException, FileException) {
#ifdef _WIN32
	if(aTarget.length() > UNC_MAX_PATH) {
		throw QueueException(STRING(TARGET_FILENAME_TOO_LONG));
	}
	// Check that target starts with a drive or is an UNC path
	if( (aTarget[1] != ':' || aTarget[2] != '\\') &&
		(aTarget[0] != '\\' && aTarget[1] != '\\') ) {
		throw QueueException(STRING(INVALID_TARGET_FILE));
	}
#else
	if(aTarget.length() > PATH_MAX) {
		throw QueueException(STRING(TARGET_FILENAME_TOO_LONG));
	}
	// Check that target contains at least one directory...we don't want headless files...
	if(aTarget[0] != '/') {
		throw QueueException(STRING(INVALID_TARGET_FILE));
	}
#endif

	string target = Util::validateFileName(aTarget);

	// Check that the file doesn't already exist...
	int64_t size = File::getSize(target);
	if(checkExistence && size != -1) {
		if (aBundle) {
			/* TODO: add for recheck */
			aBundle->increaseSize(size);
			aBundle->addFinishedSegment(size);
		}
		throw FileException(target + ": " + STRING(TARGET_FILE_EXISTS));
	}
	return target;	
}

/** Add a source to an existing queue item */
bool QueueManager::addSource(QueueItemPtr& qi, const HintedUser& aUser, Flags::MaskType addBad, const string& /*aRemotePath*/, bool newBundle /*false*/, bool checkTLS /*true*/) throw(QueueException, FileException) {
	if (!aUser.user) //atleast magnet links can cause this to happen.
		throw QueueException("Can't find Source user to add For Target: " + qi->getTargetFileName());

	if (checkTLS && !aUser.user->isSet(User::NMDC) && !aUser.user->isSet(User::TLS) && SETTING(TLS_MODE) == SettingsManager::TLS_FORCED) {
		throw QueueException(STRING(SOURCE_NO_ENCRYPTION));
	}

	if(qi->isFinished()) //no need to add source to finished item.
		throw QueueException("Already Finished: " + Util::getFileName(qi->getTarget()));
	
	bool wantConnection = qi->startDown();
	dcassert(qi->getBundle() || qi->getPriority() == QueueItem::HIGHEST);

	if(qi->isSource(aUser)) {
		if(qi->isSet(QueueItem::FLAG_USER_LIST)) {
			return wantConnection;
		}
		throw QueueException(STRING(DUPLICATE_SOURCE) + ": " + Util::getFileName(qi->getTarget()));
	}

	bool isBad = false;
	if(qi->isBadSourceExcept(aUser, addBad, isBad)) {
		throw QueueException(STRING(DUPLICATE_SOURCE) + ": " + Util::getFileName(qi->getTarget()));
	}

	//qi->addSource(aUser, SettingsManager::lanMode ? aRemotePath : Util::emptyString);
	qi->addSource(aUser);
	userQueue.addQI(qi, aUser, newBundle, isBad);

	if ((!SETTING(SOURCEFILE).empty()) && (!SETTING(SOUNDS_DISABLED)))
		PlaySound(Text::toT(SETTING(SOURCEFILE)).c_str(), NULL, SND_FILENAME | SND_ASYNC);
	
	if (!newBundle) {
		fire(QueueManagerListener::SourcesUpdated(), qi);
	}
	if (qi->getBundle()) {
		qi->getBundle()->setDirty();
	}

	return wantConnection;
	
}

QueueItem::Priority QueueManager::hasDownload(const UserPtr& aUser, const OrderedStringSet& onlineHubs, bool smallSlot) noexcept {
	RLock l(cs);
	QueueItemPtr qi = userQueue.getNext(aUser, onlineHubs, QueueItem::LOWEST, 0, 0, smallSlot);
	if(qi) {
		return qi->getPriority() == QueueItem::HIGHEST ? QueueItem::HIGHEST : (QueueItem::Priority)qi->getBundle()->getPriority();
	}
	return QueueItem::PAUSED;
}

QueueItem::Priority QueueManager::hasDownload(const UserPtr& aUser, string& hubHint, bool smallSlot, string& bundleToken) noexcept {
	auto hubs = ClientManager::getInstance()->getHubSet(aUser->getCID());
	if (hubs.empty())
		return QueueItem::PAUSED;


	RLock l(cs);
	QueueItemPtr qi = userQueue.getNext(aUser, hubs, QueueItem::LOWEST, 0, 0, smallSlot);
	if(qi) {
		if (qi->getBundle()) {
			bundleToken = qi->getBundle()->getToken();
		} 

		if (hubs.find(hubHint) == hubs.end()) {
			//we can't connect via a hub that is offline...
			hubHint = *hubs.begin();
		}
		
		qi->getSource(aUser)->updateHubUrl(hubs, hubHint, qi->isSet(QueueItem::FLAG_USER_LIST));

		return qi->getPriority() == QueueItem::HIGHEST ? QueueItem::HIGHEST : (QueueItem::Priority)qi->getBundle()->getPriority();
	}
	return QueueItem::PAUSED;
}

void QueueManager::matchListing(const DirectoryListing& dl, int& matches, int& newFiles, BundleList& bundles) {
	bool wantConnection = false;
	QueueItem::StringItemList ql;
	//uint64_t start = GET_TICK();
	{
		RLock l(cs);
		fileQueue.matchListing(dl, ql);
	}

	{
		WLock l(cs);
		for(auto& sqp: ql) {
			try {
				if (addSource(sqp.second, dl.getHintedUser(), QueueItem::Source::FLAG_FILE_NOT_AVAILABLE, sqp.first)) {
					wantConnection = true;
				}
				newFiles++;
			} catch(const Exception&) {
				//...
			}
			if (sqp.second->getBundle() && find(bundles.begin(), bundles.end(), sqp.second->getBundle()) == bundles.end()) {
				bundles.push_back(sqp.second->getBundle());
			}
		}
	}
	//uint64_t end = GET_TICK();
	//LogManager::getInstance()->message("List matched in " + Util::toString(end-start) + " ms WITHOUT buildMap");

	matches = (int)ql.size();
	if(wantConnection)
		ConnectionManager::getInstance()->getDownloadConnection(dl.getHintedUser());
}

bool QueueManager::getQueueInfo(const HintedUser& aUser, string& aTarget, int64_t& aSize, int& aFlags, string& bundleToken) noexcept {
	OrderedStringSet hubs;
	hubs.insert(aUser.hint);

	RLock l(cs);
	QueueItemPtr qi = userQueue.getNext(aUser, hubs);
	if(!qi)
		return false;

	aTarget = qi->getTarget();
	aSize = qi->getSize();
	aFlags = qi->getFlags();
	if (qi->getBundle()) {
		bundleToken = qi->getBundle()->getToken();
	}

	return true;
}

void QueueManager::onSlowDisconnect(const string& aToken) {
	RLock l(cs);
	auto b = bundleQueue.findBundle(aToken);
	if(b) {
		if(b->isSet(Bundle::FLAG_AUTODROP)) {
			b->unsetFlag(Bundle::FLAG_AUTODROP);
		} else {
			b->setFlag(Bundle::FLAG_AUTODROP);
		}
	}
}

bool QueueManager::getAutoDrop(const string& aToken) {
	RLock l(cs);
	auto b = bundleQueue.findBundle(aToken);
	if(b) {
		return b->isSet(Bundle::FLAG_AUTODROP);
	}
	return false;
}

string QueueManager::getTempTarget(const string& aTarget) {
	RLock l(cs);
	auto qi = fileQueue.findFile(aTarget);
	if(qi) {
		return qi->getTempTarget();
	}
	return Util::emptyString;
}

StringList QueueManager::getTargets(const TTHValue& tth) {
	QueueItemList ql;
	StringList sl;

	{
		RLock l(cs);
		fileQueue.findFiles(tth, ql);
	}

	for(auto& q: ql)
		sl.push_back(q->getTarget());

	return sl;
}

void QueueManager::readLockedOperation(const function<void (const QueueItem::StringMap&)>& currentQueue) {
	RLock l(cs);
	if(currentQueue) currentQueue(fileQueue.getQueue());
}

Download* QueueManager::getDownload(UserConnection& aSource, const OrderedStringSet& onlineHubs, string& aMessage, string& newUrl, bool smallSlot) noexcept {
	QueueItemPtr q = nullptr;
	const UserPtr& u = aSource.getUser();
	{
		WLock l(cs);
		dcdebug("Getting download for %s...", u->getCID().toBase32().c_str());

		q = userQueue.getNext(aSource.getUser(), onlineHubs, QueueItem::LOWEST, aSource.getChunkSize(), aSource.getSpeed(), smallSlot);
		if (q) {
			auto source = q->getSource(aSource.getUser());

			//update the hub hint
			newUrl = aSource.getHubUrl();
			source->updateHubUrl(onlineHubs, newUrl, q->isSet(QueueItem::FLAG_USER_LIST));
			
			//check partial sources
			if(source->isSet(QueueItem::Source::FLAG_PARTIAL)) {
				int64_t blockSize = HashManager::getInstance()->getBlockSize(q->getTTH());
				if(blockSize == 0)
					blockSize = q->getSize();
					
				Segment segment = q->getNextSegment(blockSize, aSource.getChunkSize(), aSource.getSpeed(), source->getPartialSource(), false);
				if(segment.getStart() != -1 && segment.getSize() == 0) {
					// no other partial chunk from this user, remove him from queue
					userQueue.removeQI(q, u);
					q->removeSource(u, QueueItem::Source::FLAG_NO_NEED_PARTS);
					aMessage = STRING(NO_NEEDED_PART);
					return nullptr;
				}
			}
		} else {
			aMessage = userQueue.getLastError();
			dcdebug("none\n");
			return nullptr;
		}

		// Check that the file we will be downloading to exists
		if(q->getDownloadedBytes() > 0) {
			if(!Util::fileExists(q->getTempTarget())) {
				// Temp target gone?
				q->resetDownloaded();
			}
		}

		Download* d = new Download(aSource, *q);
		userQueue.addDownload(q, d);

		fire(QueueManagerListener::SourcesUpdated(), q);
		dcdebug("found %s\n", q->getTarget().c_str());
		return d;
	}
}

void QueueManager::moveFile(const string& source, const string& target, QueueItemPtr q /*nullptr, only add when download finishes!*/, bool forceThreading /*false*/) {
	File::ensureDirectory(target);
	if(forceThreading || File::getSize(source) > MOVER_LIMIT) {
		mover.moveFile(source, target, q);
	} else {
		moveFile_(source, target, q);
	}
}

void QueueManager::moveFile_(const string& source, const string& target, QueueItemPtr& qi) {
	try {
		UploadManager::getInstance()->abortUpload(source);
		File::renameFile(source, target);
		if (Util::fileExists(source)) {
			//UploadManager::getInstance()->abortUpload(source);
			//File::deleteFile(source);
			//if (Util::fileExists(source)) {
				LogManager::getInstance()->message("Failed to delete the file: " + source, LogManager::LOG_INFO);
			//}
		}
	} catch(const FileException& /*e1*/) {
		// Try to just rename it to the correct name at least
		string newTarget = Util::getFilePath(source) + Util::getFileName(target);
		try {
			File::renameFile(source, newTarget);
			LogManager::getInstance()->message(source + " " + STRING(RENAMED_TO) + " " + newTarget, LogManager::LOG_WARNING);
		} catch(const FileException& e2) {
			LogManager::getInstance()->message(STRING(UNABLE_TO_RENAME) + " " + source + ": " + e2.getError(), LogManager::LOG_ERROR);
		}
	}
	if(SETTING(USE_FTP_LOGGER))
		AirUtil::fileEvent(target, true);

	if (qi && qi->getBundle()) {
		getInstance()->handleMovedBundleItem(qi);
	}
}

void QueueManager::handleMovedBundleItem(QueueItemPtr& qi) {
	BundlePtr b = qi->getBundle();

	HintedUserList notified;

	{
		RLock l (cs);

		//collect the users that don't have this file yet
		for (auto& fn: qi->getBundle()->getFinishedNotifications()) {
			if (!qi->isSource(fn.first.user)) {
				notified.push_back(fn.first);
			}
		}
	}

	//send the notifications
	for(auto& u: notified) {
		AdcCommand cmd(AdcCommand::CMD_PBD, AdcCommand::TYPE_UDP);

		cmd.addParam("UP1");
		cmd.addParam("HI", u.hint);
		cmd.addParam("TH", qi->getTTH().toBase32());
		ClientManager::getInstance()->send(cmd, u.user->getCID(), false, true);
	}

	bool hasNotifications = false;
	{
		RLock l (cs);
		//flag this file as moved
		auto s = find_if(b->getFinishedFiles(), [qi](QueueItemPtr aQI) { return aQI->getTarget() == qi->getTarget(); });
		if (s != b->getFinishedFiles().end()) {
			qi->setFlag(QueueItem::FLAG_MOVED);
		} else if (b->getFinishedFiles().empty() && b->getQueueItems().empty()) {
			//the bundle was removed while the file was being moved?
			return;
		}

		//check if there are queued or non-moved files remaining
		if (!b->allowHash()) 
			return;

		hasNotifications = !b->getFinishedNotifications().empty();
	}

	if (hasNotifications) {
		//the bundle has finished downloading so we don't need any partial bundle sharing notifications

		Bundle::FinishedNotifyList fnl;
		{
			WLock l(cs);
			b->clearFinishedNotifications(fnl);
		}

		for(auto& ubp: fnl)
			sendRemovePBD(ubp.first, ubp.second);
	}

	if (!SETTING(SCAN_DL_BUNDLES) || b->isFileBundle()) {
		LogManager::getInstance()->message(STRING_F(DL_BUNDLE_FINISHED, b->getName().c_str()), LogManager::LOG_INFO);
	} else if (!scanBundle(b)) {
		return;
	} 

	onBundleRemoved(b, true);
	if (SETTING(ADD_FINISHED_INSTANTLY)) {
		hashBundle(b);
	} else {
		LogManager::getInstance()->message(CSTRING(INSTANT_SHARING_DISABLED), LogManager::LOG_INFO);
	}
}

bool QueueManager::scanBundle(BundlePtr& aBundle) {
	if (!SETTING(SCAN_DL_BUNDLES))
		return true;

	bool hasMissing=false, hasExtras=false;
	ShareScannerManager::getInstance()->scanBundle(aBundle, hasMissing, hasExtras);
	if (hasMissing || hasExtras) {
		aBundle->setFlag(Bundle::FLAG_SHARING_FAILED);
		onBundleStatusChanged(aBundle, hasExtras ? AutoSearch::STATUS_FAILED_EXTRAS : AutoSearch::STATUS_FAILED_MISSING);
		return false;
	}
	return true;
}

void QueueManager::hashBundle(BundlePtr& aBundle) {
	if(ShareManager::getInstance()->allowAddDir(aBundle->getTarget())) {
		aBundle->setFlag(Bundle::FLAG_HASH);
		QueueItemList hash;
		QueueItemList removed;

		{
			RLock l(cs);
			for (auto& qi: aBundle->getFinishedFiles()) {
				if (ShareManager::getInstance()->checkSharedName(qi->getTarget(), false, false, qi->getSize()) && Util::fileExists(qi->getTarget())) {
					qi->unsetFlag(QueueItem::FLAG_HASHED);
					hash.push_back(qi);
				} else {
					removed.push_back(qi);
				}
			}
		}

		if (!removed.empty()) {
			WLock l (cs);
			for(auto& q: removed) {
				//erase failed items
				bundleQueue.removeFinishedItem(q);
				fileQueue.remove(q);
			}
		}

		int64_t hashSize = aBundle->getSize();

		{
			HashManager::HashPauser pauser;
			for(auto& q: hash) {
				try {
					// Schedule for hashing, it'll be added automatically later on...
					if (!HashManager::getInstance()->checkTTH(q->getTarget(), q->getSize(), AirUtil::getLastWrite(q->getTarget()))) {
						//..
					} else {
						//fine, it's there already..
						q->setFlag(QueueItem::FLAG_HASHED);
						hashSize -= q->getSize();
					}
				} catch(const Exception&) {
					//...
				}
			}
		}

		if (hashSize > 0) {
			LogManager::getInstance()->message(STRING_F(BUNDLE_ADDED_FOR_HASH, aBundle->getName() % Util::formatBytes(hashSize)), LogManager::LOG_INFO);
		} else {
			//all files have been hashed already?
			checkBundleHashed(aBundle);
		}
	} else if (SETTING(ADD_FINISHED_INSTANTLY)) {
		LogManager::getInstance()->message(STRING_F(NOT_IN_SHARED_DIR, aBundle->getTarget().c_str()), LogManager::LOG_INFO);
	} else {
		LogManager::getInstance()->message(CSTRING(INSTANT_SHARING_DISABLED), LogManager::LOG_INFO);
	}
}

void QueueManager::onFileHashed(const string& fname, const TTHValue& root, bool failed) {
	QueueItemPtr q;
	{
		RLock l(cs);

		//prefer the exact path match
		q = fileQueue.findFile(fname);
		if (!q) {
			//also remove bundles that haven't been removed in a shared directories... remove this when the bundles are shown correctly in GUI

			auto tpi = make_pair(fileQueue.getTTHIndex().begin(), fileQueue.getTTHIndex().end());
			if (!failed) {
				//we have the tth so we can limit the range
				tpi = fileQueue.getTTHIndex().equal_range(const_cast<TTHValue*>(&root));
			}

			if (tpi.first != tpi.second) {
				int64_t size = 0;
				if (failed) {
					size = File::getSize(fname);
				}

				auto file = Util::getFileName(fname);
				auto p = find_if(tpi | map_values, [&](QueueItemPtr aQI) { return (!failed || size == aQI->getSize()) && aQI->getBundle() && aQI->getBundle()->isSet(Bundle::FLAG_HASH) && 
					aQI->getTargetFileName() == file && aQI->isFinished() && !aQI->isSet(QueueItem::FLAG_HASHED); });

				if (p.base() != tpi.second) {
					q = *p;
				}
			}
		}
	}

	if (!q) {
		if (!failed) {
			fire(QueueManagerListener::FileHashed(), fname, root);
		}
		return;
	}

	if (!q->getBundle())
		return;


	q->setFlag(QueueItem::FLAG_HASHED);
	if (failed) {
		q->getBundle()->setFlag(Bundle::FLAG_SHARING_FAILED);
	} else if (!q->getBundle()->isSet(Bundle::FLAG_HASH)) {
		//instant sharing disabled/the folder wasn't shared when the bundle finished
		fire(QueueManagerListener::FileHashed(), fname, root);
	}

	checkBundleHashed(q->getBundle());
}

void QueueManager::checkBundleHashed(BundlePtr b) {
	bool fireHashed = false;
	{
		RLock l(cs);
		if (!b->getQueueItems().empty() || boost::find_if(b->getFinishedFiles(), [](QueueItemPtr q) { return !q->isSet(QueueItem::FLAG_HASHED); }) != b->getFinishedFiles().end())
			return;


		//don't fire anything if nothing has been hashed (the folder has probably been removed...)
		if (!b->getFinishedFiles().empty()) {
			if (!b->getQueueItems().empty()) {
				//new items have been added while it was being hashed
				b->unsetFlag(Bundle::FLAG_HASH);
				return;
			}

			if (b->isSet(Bundle::FLAG_HASH)) {
				b->unsetFlag(Bundle::FLAG_HASH);
				if (!b->isSet(Bundle::FLAG_SHARING_FAILED)) {
					fireHashed = true;
				} else {
					LogManager::getInstance()->message(STRING_F(BUNDLE_HASH_FAILED, b->getTarget().c_str()), LogManager::LOG_ERROR);
					return;
				}
			} else {
				//instant sharing disabled/the folder wasn't shared when the bundle finished
			}
		}
	}

	if (fireHashed) {
		if (!b->isFileBundle()) {
			fire(QueueManagerListener::BundleHashed(), b->getTarget());
		} else {
			fire(QueueManagerListener::FileHashed(), b->getFinishedFiles().front()->getTarget(), b->getFinishedFiles().front()->getTTH());
		}
	}

	{
		WLock l(cs);
		for(auto i = b->getFinishedFiles().begin(); i != b->getFinishedFiles().end(); ) {
			fileQueue.remove(*i);
			i = b->getFinishedFiles().erase(i);
		}

		//for_each(b->getFinishedFiles(), [&] (QueueItemPtr qi) { fileQueue.remove(qi); } );
		bundleQueue.removeBundle(b);
	}
}

void QueueManager::moveStuckFile(QueueItemPtr& qi) {

	moveFile(qi->getTempTarget(), qi->getTarget());

	if(qi->isFinished()) {
		WLock l(cs);
		userQueue.removeQI(qi);
	}

	string target = qi->getTarget();

	if(!SETTING(KEEP_FINISHED_FILES)) {
		fire(QueueManagerListener::Removed(), qi, true);
		fileQueue.remove(qi);
		removeBundleItem(qi, true);
	 } else {
		qi->addFinishedSegment(Segment(0, qi->getSize()));
		fire(QueueManagerListener::StatusUpdated(), qi);
	}

	fire(QueueManagerListener::RecheckAlreadyFinished(), target);
}

void QueueManager::rechecked(QueueItemPtr& qi) {
	fire(QueueManagerListener::RecheckDone(), qi->getTarget());
	fire(QueueManagerListener::StatusUpdated(), qi);
	if (qi->getBundle()) {
		qi->getBundle()->setDirty();
	}
}

void QueueManager::putDownload(Download* aDownload, bool finished, bool noAccess /*false*/, bool rotateQueue /*false*/) noexcept {
	HintedUserList getConn;
 	string fl_fname;
	int fl_flag = 0;
	QueueItemPtr q = nullptr;
	bool removeFinished = false;

	// Make sure the download gets killed
	unique_ptr<Download> d(aDownload);
	aDownload = nullptr;

	d->close();

	{
		WLock l(cs);
		q = fileQueue.findFile(d->getPath());
		if(!q) {
			// Target has been removed, clean up the mess
			auto hasTempTarget = !d->getTempTarget().empty();
			auto isFullList = d->getType() == Transfer::TYPE_FULL_LIST;
			auto isFile = d->getType() == Transfer::TYPE_FILE && d->getTempTarget() != d->getPath();

			if(hasTempTarget && (isFullList || isFile)) {
				File::deleteFile(d->getTempTarget());
			}

			return;
		}

		if (q->isSet(QueueItem::FLAG_FINISHED)) {
			return;
		}

		if(!finished) {
			if(d->getType() == Transfer::TYPE_FULL_LIST && !d->getTempTarget().empty()) {
				// No use keeping an unfinished file list...
				File::deleteFile(d->getTempTarget());
			}

			if(d->getType() != Transfer::TYPE_TREE && q->getDownloadedBytes() == 0) {
				if(d->getType() == Transfer::TYPE_FILE)
					File::deleteFile(d->getTempTarget());
				q->setTempTarget(Util::emptyString);
			}

			if(d->getType() == Transfer::TYPE_FILE) {
				// mark partially downloaded chunk, but align it to block size
				int64_t downloaded = d->getPos();
				downloaded -= downloaded % d->getTigerTree().getBlockSize();

				if(downloaded > 0) {
					q->addFinishedSegment(Segment(d->getStartPos(), downloaded));
				}

				if (rotateQueue && q->getBundle()) {
					q->getBundle()->rotateUserQueue(q, d->getUser());
				}
			}

			if (noAccess) {
				q->blockSourceHub(d->getHintedUser());
			}

			if(q->getPriority() != QueueItem::PAUSED) {
				q->getOnlineUsers(getConn);
			}

			userQueue.removeDownload(q, d->getToken());
			fire(QueueManagerListener::StatusUpdated(), q);
		} else { // Finished
			if(d->getType() == Transfer::TYPE_PARTIAL_LIST) {
				if( (q->isSet(QueueItem::FLAG_DIRECTORY_DOWNLOAD)) ||
					(q->isSet(QueueItem::FLAG_MATCH_QUEUE)) ||
					(q->isSet(QueueItem::FLAG_VIEW_NFO)))
				{					
					fl_fname = d->getPFS();
					fl_flag = (q->isSet(QueueItem::FLAG_DIRECTORY_DOWNLOAD) ? (QueueItem::FLAG_DIRECTORY_DOWNLOAD) : 0)
						| (q->isSet(QueueItem::FLAG_PARTIAL_LIST) ? (QueueItem::FLAG_PARTIAL_LIST) : 0)
						| (q->isSet(QueueItem::FLAG_MATCH_QUEUE) ? QueueItem::FLAG_MATCH_QUEUE : 0) | QueueItem::FLAG_TEXT
						| (q->isSet(QueueItem::FLAG_VIEW_NFO) ? QueueItem::FLAG_VIEW_NFO : 0);
				} else {
					fire(QueueManagerListener::PartialList(), d->getHintedUser(), d->getPFS(), q->getTempTarget());
				}
				userQueue.removeQI(q);
				fire(QueueManagerListener::Removed(), q, true);
				fileQueue.remove(q);
			} else if(d->getType() == Transfer::TYPE_TREE) {
				//add it in hashmanager outside the lock
				userQueue.removeDownload(q, d->getToken());
				fire(QueueManagerListener::StatusUpdated(), q);
			} else if(d->getType() == Transfer::TYPE_FULL_LIST) {
				if(d->isSet(Download::FLAG_XML_BZ_LIST)) {
					q->setFlag(QueueItem::FLAG_XML_BZLIST);
				} else {
					q->unsetFlag(QueueItem::FLAG_XML_BZLIST);
				}

				auto dir = q->getTempTarget(); // We cheated and stored the initial display directory here (when opening lists from search)
				q->addFinishedSegment(Segment(0, q->getSize()));

				// Now, let's see if this was a directory download filelist...
				if( (q->isSet(QueueItem::FLAG_DIRECTORY_DOWNLOAD)) ||
					(q->isSet(QueueItem::FLAG_MATCH_QUEUE)) )
				{
					fl_fname = q->getListName();
					fl_flag = (q->isSet(QueueItem::FLAG_DIRECTORY_DOWNLOAD) ? QueueItem::FLAG_DIRECTORY_DOWNLOAD : 0)
						| (q->isSet(QueueItem::FLAG_MATCH_QUEUE) ? QueueItem::FLAG_MATCH_QUEUE : 0);
				}

				fire(QueueManagerListener::Finished(), q, dir, d->getHintedUser(), d->getAverageSpeed());
				userQueue.removeQI(q);

				fire(QueueManagerListener::Removed(), q, true);
				fileQueue.remove(q);
			} else if(d->getType() == Transfer::TYPE_FILE) {
				d->setOverlapped(false);
				q->addFinishedSegment(d->getSegment());

				if(q->isFinished()) {
					// Disconnect all possible overlapped downloads
					for(auto aD: q->getDownloads()) {
						if(compare(aD->getToken(), d->getToken()) != 0)
							aD->getUserConnection().disconnect();
					}

					removeFinished = true;
					q->setFlag(QueueItem::FLAG_FINISHED);
					userQueue.removeQI(q);

					if(SETTING(KEEP_FINISHED_FILES)) {
						fire(QueueManagerListener::StatusUpdated(), q);
					} else {
						fire(QueueManagerListener::Removed(), q, true);
						if (!d->getBundle())
							fileQueue.remove(q);
					}
				} else {
					userQueue.removeDownload(q, d->getToken());
					fire(QueueManagerListener::StatusUpdated(), q);
				}
			} else {
				dcassert(0);
			}
		}
	}
	
	if (d->getType() == Transfer::TYPE_TREE && finished) {
		// Got a full tree, now add it to the HashManager
		dcassert(d->getTreeValid());
		HashManager::getInstance()->addTree(d->getTigerTree());
	}

	if (removeFinished && q->isSet(QueueItem::FLAG_OPEN)) {
		Util::openFile(q->getTarget());
	}

	if (removeFinished) {
		if (q->getBundle()) {
			removeBundleItem(q, true);
		}

		if(SETTING(LOG_DOWNLOADS)) {
			ParamMap params;
			d->getParams(d->getUserConnection(), params);
			LOG(LogManager::DOWNLOAD, params);
		}

		// Check if we need to move the file
		if(!d->getTempTarget().empty() && (Util::stricmp(d->getPath().c_str(), d->getTempTarget().c_str()) != 0) ) {
			moveFile(d->getTempTarget(), d->getPath(), q);
		}

		fire(QueueManagerListener::Finished(), q, Util::emptyString, d->getHintedUser(), d->getAverageSpeed());
	}

	for(auto& u: getConn) {
		if (u.user != d->getUser())
			ConnectionManager::getInstance()->getDownloadConnection(u);
	}

	if(!fl_fname.empty()) {
		if (d->isSet(Download::FLAG_TTHLIST)) {	 
			matchTTHList(fl_fname, d->getHintedUser(), fl_flag);	 
		} else {	 
			DirectoryListingManager::getInstance()->processList(fl_fname, d->getHintedUser(), d->getTempTarget(), fl_flag); 
		}
	}
}

void QueueManager::setSegments(const string& aTarget, uint8_t aSegments) {
	RLock l (cs);
	auto qi = fileQueue.findFile(aTarget);
	if (qi) {
		qi->setMaxSegments(aSegments);
	}
}

void QueueManager::matchTTHList(const string& name, const HintedUser& user, int flags) {	 
	dcdebug("matchTTHList");
	if(flags & QueueItem::FLAG_MATCH_QUEUE) {
		bool wantConnection = false;
		int matches = 0;
		 
		typedef vector<TTHValue> TTHList;
		TTHList tthList;
 	 
		size_t start = 0;
		while (start+39 < name.length()) {
			tthList.emplace_back(name.substr(start, 39));
			start = start+40;
		}
 	 
		if(tthList.empty())
			return;

		QueueItemList ql;
 		{	 
			RLock l(cs);
			for (auto& tth: tthList) {
				fileQueue.findFiles(tth, ql);
			}
		}

		if (!ql.empty()) {
			WLock l (cs);
			for(auto& qi: ql) {
				try {
					if (addSource(qi, user, QueueItem::Source::FLAG_FILE_NOT_AVAILABLE, Util::emptyString)) {
						wantConnection = true;
					}
				} catch(...) {
					// Ignore...
				}
				matches++;
			}
		}

		if((matches > 0) && wantConnection)
			ConnectionManager::getInstance()->getDownloadConnection(user); 
	}
}

void QueueManager::recheck(const string& aTarget) {
	rechecker.add(aTarget);
}

void QueueManager::remove(const string aTarget) noexcept {
	QueueItemPtr qi = NULL;
	{
		RLock l(cs);
		qi = fileQueue.findFile(aTarget);
	}
	if (qi) {
		removeQI(qi);
	}
}

void QueueManager::removeQI(QueueItemPtr& q, bool moved /*false*/) noexcept {
	UserConnectionList x;
	dcassert(q);

	// For partial-share
	UploadManager::getInstance()->abortUpload(q->getTempTarget());
	UserPtr u = nullptr;

	{
		WLock l(cs);
		if(q->isSet(QueueItem::FLAG_DIRECTORY_DOWNLOAD)) {
			u = q->getSources()[0].getUser();
		}

		if(q->isRunning()) {
			for(auto d: q->getDownloads()) 
				x.push_back(&d->getUserConnection());
		} else if(!q->getTempTarget().empty() && q->getTempTarget() != q->getTarget()) {
			File::deleteFile(q->getTempTarget());
		}

		if(!q->isFinished()) {
			userQueue.removeQI(q);
		}

		if (!moved) {
			fire(QueueManagerListener::Removed(), q, true);
		}
		fileQueue.remove(q);
	}

	if (u)
		DirectoryListingManager::getInstance()->removeDirectoryDownload(u, q->getTempTarget(), q->isSet(QueueItem::FLAG_PARTIAL_LIST));

	removeBundleItem(q, false, moved);
	for(auto& u: x)
		u->disconnect(true);
}

void QueueManager::removeSource(const string& aTarget, const UserPtr& aUser, Flags::MaskType reason, bool removeConn /* = true */) noexcept {
	QueueItemPtr qi = NULL;
	{
		RLock l(cs);
		qi = fileQueue.findFile(aTarget);
	}
	if (qi)
		removeSource(qi, aUser, reason, removeConn);
}

void QueueManager::removeSource(QueueItemPtr& q, const UserPtr& aUser, Flags::MaskType reason, bool removeConn /* = true */) noexcept {
	bool isRunning = false;
	bool removeCompletely = false;
	{
		WLock l(cs);
		if(!q->isSource(aUser))
			return;

		if(q->isFinished())
			return;
	
		if(q->isSet(QueueItem::FLAG_USER_LIST)) {
			removeCompletely = true;
			goto endCheck;
		}

		if(reason == QueueItem::Source::FLAG_NO_TREE) {
			q->getSource(aUser)->setFlag(reason);
			return;
		}

		if(q->isRunning()) {
			isRunning = true;
		}
		userQueue.removeQI(q, aUser, false, true, true);
		q->removeSource(aUser, reason);
		
		fire(QueueManagerListener::SourcesUpdated(), q);

		BundlePtr b = q->getBundle();
		if (b) {
			b->setDirty();
		}
	}
endCheck:
	if(isRunning && removeConn) {
		DownloadManager::getInstance()->abortDownload(q->getTarget(), aUser);
	}

	if(removeCompletely) {
		removeQI(q);
	}
}

void QueueManager::removeSource(const UserPtr& aUser, Flags::MaskType reason) noexcept {
	// @todo remove from finished items
	QueueItemList ql;

	{
		RLock l(cs);
		userQueue.getUserQIs(aUser, ql);
	}

	for(auto& qi: ql) 
		removeSource(qi, aUser, reason);
}

void QueueManager::setBundlePriority(const string& bundleToken, Bundle::Priority p) noexcept {
	BundlePtr bundle = nullptr;
	{
		RLock l(cs);
		bundle = bundleQueue.findBundle(bundleToken);
	}

	setBundlePriority(bundle, p, false);
}

void QueueManager::setBundlePriority(BundlePtr& aBundle, Bundle::Priority p, bool isAuto, bool isQIChange /*false*/) noexcept {
	QueueItemPtr fileBundleQI = nullptr;
	Bundle::Priority oldPrio = aBundle->getPriority();
	//LogManager::getInstance()->message("Changing priority to: " + Util::toString(p));
	if (oldPrio == p) {
		//LogManager::getInstance()->message("Prio not changed: " + Util::toString(oldPrio));
		return;
	}

	{
		WLock l(cs);
		bundleQueue.removeSearchPrio(aBundle);
		userQueue.setBundlePriority(aBundle, p);
		bundleQueue.addSearchPrio(aBundle);
		bundleQueue.recalculateSearchTimes(aBundle->isRecent(), true);
		if (!isAuto) {
			aBundle->setAutoPriority(false);
		}

		fire(QueueManagerListener::BundlePriority(), aBundle);
		if (aBundle->isFileBundle() && !isQIChange) {
			fileBundleQI = aBundle->getQueueItems().front();
		}
	}

	aBundle->setDirty();

	if(p == Bundle::PAUSED) {
		DownloadManager::getInstance()->disconnectBundle(aBundle);
	} else if (oldPrio == Bundle::PAUSED || oldPrio == Bundle::LOWEST) {
		connectBundleSources(aBundle);
	}

	if (fileBundleQI) {
		setQIPriority(fileBundleQI, (QueueItem::Priority)p, isAuto, true);
		if (aBundle->getAutoPriority() != fileBundleQI->getAutoPriority()) {
			setQIAutoPriority(fileBundleQI->getTarget(), aBundle->getAutoPriority(), true);
		}
	}

	//LogManager::getInstance()->message("Prio changed to: " + Util::toString(bundle->getPriority()));
}

void QueueManager::setBundleAutoPriority(const string& bundleToken, bool isQIChange /*false*/) noexcept {
	BundlePtr b = nullptr;
	{
		RLock l(cs);
		b = bundleQueue.findBundle(bundleToken);
		if (b) {
			b->setAutoPriority(!b->getAutoPriority());
			b->setDirty();
		}
	}

	if (b) {
		if (!isQIChange && b->isFileBundle()) {
			QueueItemPtr qi = nullptr;
			{
				RLock l (cs);
				qi = b->getQueueItems().front();
			}

			if (qi->getAutoPriority() != b->getAutoPriority()) {
				setQIAutoPriority(qi->getTarget(), b->getAutoPriority(), true);
			}
		}

		if (SETTING(AUTOPRIO_TYPE) == SettingsManager::PRIO_BALANCED) {
			calculateBundlePriorities(false);
			if (b->getPriority() == Bundle::PAUSED) {
				//failed to count auto priorities, but we don't want it to stay paused
				setBundlePriority(b, Bundle::LOW, true);
			}
		} else if (SETTING(AUTOPRIO_TYPE) == SettingsManager::PRIO_PROGRESS) {
			setBundlePriority(b, b->calculateProgressPriority(), true);
		}
	}
}

void QueueManager::removeBundleSource(const string& bundleToken, const UserPtr& aUser) noexcept {
	BundlePtr bundle = nullptr;
	{
		RLock l(cs);
		bundle = bundleQueue.findBundle(bundleToken);
	}
	removeBundleSource(bundle, aUser);
}

void QueueManager::removeBundleSource(BundlePtr aBundle, const UserPtr& aUser) noexcept {
	if (aBundle) {
		QueueItemList ql;
		{
			RLock l(cs);
			aBundle->getItems(aUser, ql);

			//we don't want notifications from this user anymore
			auto p = boost::find_if(aBundle->getFinishedNotifications(), [&aUser](const Bundle::UserBundlePair& ubp) { return ubp.first.user == aUser; });
			if (p != aBundle->getFinishedNotifications().end()) {
				sendRemovePBD(p->first, p->second);
			}
		}

		for(auto& qi: ql) {
			removeSource(qi, aUser, QueueItem::Source::FLAG_REMOVED);
		}
	}
}

void QueueManager::sendRemovePBD(const HintedUser& aUser, const string& aRemoteToken) {
	AdcCommand cmd(AdcCommand::CMD_PBD, AdcCommand::TYPE_UDP);

	cmd.addParam("HI", aUser.hint);
	cmd.addParam("BU", aRemoteToken);
	cmd.addParam("RM1");
	ClientManager::getInstance()->send(cmd, aUser.user->getCID(), false, true);
}

void QueueManager::setQIPriority(const string& aTarget, QueueItem::Priority p) noexcept {
	QueueItemPtr q = nullptr;
	{
		RLock l(cs);
		q = fileQueue.findFile(aTarget);
	}
	setQIPriority(q, p);
}

void QueueManager::setQIPriority(QueueItemPtr& q, QueueItem::Priority p, bool isAP /*false*/, bool isBundleChange /*false*/) noexcept {
	HintedUserList getConn;
	bool running = false;
	if (!q || !q->getBundle()) {
		//items without a bundle should always use the highest prio
		return;
	}

	BundlePtr b = q->getBundle();
	{
		WLock l(cs);
		if(q && q->getPriority() != p && !q->isFinished() ) {
			if((q->getPriority() == QueueItem::PAUSED && b->getPriority() != Bundle::PAUSED) || p == QueueItem::HIGHEST) {
				// Problem, we have to request connections to all these users...
				q->getOnlineUsers(getConn);
			}

			running = q->isRunning();
			userQueue.setQIPriority(q, p);
			fire(QueueManagerListener::StatusUpdated(), q);
		}
	}

	if (b->isFileBundle() && !isBundleChange) {
		setBundlePriority(b, (Bundle::Priority)p, isAP, true);
	} else {
		b->setDirty();
	}

	if(p == QueueItem::PAUSED && running) {
		DownloadManager::getInstance()->abortDownload(q->getTarget());
	} else {
		for(auto& u: getConn)
			ConnectionManager::getInstance()->getDownloadConnection(u);
	}
}

void QueueManager::setQIAutoPriority(const string& aTarget, bool ap, bool isBundleChange /*false*/) noexcept {
	vector<pair<QueueItemPtr, QueueItem::Priority>> priorities;
	string bundleToken;

	{
		RLock l(cs);
		QueueItemPtr q = fileQueue.findFile(aTarget);
		if (!q)
			return;
		if (!q->getBundle())
			return;
		if (q->getAutoPriority() == ap)
			return;

		q->setAutoPriority(ap);
		if (!isBundleChange && q->getBundle()->isFileBundle()) {
			BundlePtr bundle = q->getBundle();
			if (q->getAutoPriority() != bundle->getAutoPriority()) {
				bundleToken = bundle->getToken();
			}
		}
		if(ap && !isBundleChange) {
			if (SETTING(AUTOPRIO_TYPE) == SettingsManager::PRIO_PROGRESS) {
				priorities.emplace_back(q, q->calculateAutoPriority());
			} else if (q->getPriority() == QueueItem::PAUSED) {
				priorities.emplace_back(q, QueueItem::LOW);
			}
		}
		dcassert(q->getBundle());
		q->getBundle()->setDirty();
		fire(QueueManagerListener::StatusUpdated(), q);
	}

	for(auto& qp: priorities) {
		setQIPriority(qp.first, (QueueItem::Priority)qp.second);
	}

	if (!bundleToken.empty()) {
		setBundleAutoPriority(bundleToken, true);
	}
}

void QueueManager::saveQueue(bool force) noexcept {
	RLock l(cs);	
	bundleQueue.saveQueue(force);

	// Put this here to avoid very many saves tries when disk is full...
	lastSave = GET_TICK();
}

class QueueLoader : public SimpleXMLReader::CallBack {
public:
	QueueLoader() : curFile(NULL), inDownloads(false), inBundle(false), inFile(false) { }
	~QueueLoader() { }
	void startTag(const string& name, StringPairList& attribs, bool simple);
	void endTag(const string& name);
	void resetBundle() {
		curFile = nullptr;
		curBundle = nullptr;
		inFile = false;
		inBundle = false;
		inDownloads = false;
		curToken.clear();
		target.clear();
	}
private:
	string target;

	QueueItemPtr curFile;
	BundlePtr curBundle;
	bool inDownloads;
	bool inBundle;
	bool inFile;
	string curToken;
};

void QueueManager::loadQueue() noexcept {
	setMatchers();


	
	//migrate old bundles
	Util::migrate(Util::getPath(Util::PATH_BUNDLES), "Bundle*");

	QueueLoader loader;
	StringList fileList = File::findFiles(Util::getPath(Util::PATH_BUNDLES), "Bundle*");
	for (auto& path: fileList) {
		if (Util::getFileExt(path) == ".xml") {
			try {
				File f(path, File::READ, File::OPEN, false);
				SimpleXMLReader(&loader).parse(f);
			} catch(const Exception& e) {
				LogManager::getInstance()->message(STRING_F(BUNDLE_LOAD_FAILED, path % e.getError().c_str()), LogManager::LOG_ERROR);
				File::deleteFile(path);
				loader.resetBundle();
			}
		}
	}

	try {
		//load the old queue file and delete it
		auto path = Util::getPath(Util::PATH_USER_CONFIG) + "Queue.xml";
		Util::migrate(path);

		File f(path, File::READ, File::OPEN);
		SimpleXMLReader(&loader).parse(f);
		f.close();
		File::copyFile(Util::getPath(Util::PATH_USER_CONFIG) + "Queue.xml", Util::getPath(Util::PATH_USER_CONFIG) + "Queue.xml.bak");
		File::deleteFile(Util::getPath(Util::PATH_USER_CONFIG) + "Queue.xml");
	} catch(const Exception&) {
		// ...
	}

	TimerManager::getInstance()->addListener(this); 
	SearchManager::getInstance()->addListener(this);
	ClientManager::getInstance()->addListener(this);
	HashManager::getInstance()->addListener(this);
}

static const string sFile = "File";
static const string sBundle = "Bundle";
static const string sName = "Name";
static const string sToken = "Token";
static const string sDownload = "Download";
static const string sTempTarget = "TempTarget";
static const string sTarget = "Target";
static const string sSize = "Size";
static const string sDownloaded = "Downloaded";
static const string sPriority = "Priority";
static const string sSource = "Source";
static const string sNick = "Nick";
static const string sDirectory = "Directory";
static const string sAdded = "Added";
static const string sDate = "Date";
static const string sTTH = "TTH";
static const string sCID = "CID";
static const string sHubHint = "HubHint";
static const string sRemotePath = "RemotePath";
static const string sSegment = "Segment";
static const string sStart = "Start";
static const string sAutoPriority = "AutoPriority";
static const string sMaxSegments = "MaxSegments";
static const string sBundleToken = "BundleToken";
static const string sFinished = "Finished";
static const string sAutoSearch = "AutoSearch";



void QueueLoader::startTag(const string& name, StringPairList& attribs, bool simple) {
	QueueManager* qm = QueueManager::getInstance();
	if(!inDownloads && name == "Downloads") {
		inDownloads = true;
	} else if (!inFile && name == sFile) {
		curToken = getAttrib(attribs, sToken, 1);
		inFile = true;		
	} else if (!inBundle && name == sBundle) {
		const string& bundleTarget = getAttrib(attribs, sTarget, 0);
		const string& token = getAttrib(attribs, sToken, 1);
		if(token.empty())
			throw Exception("Missing bundle token");

		time_t dirDate = static_cast<time_t>(Util::toInt(getAttrib(attribs, sDate, 2)));
		time_t added = static_cast<time_t>(Util::toInt(getAttrib(attribs, sAdded, 3)));
		const string& prio = getAttrib(attribs, sPriority, 4);
		if(added == 0) {
			added = GET_TIME();
		}

		if (ConnectionManager::getInstance()->tokens.addToken(token))
			curBundle = new Bundle(bundleTarget, added, !prio.empty() ? (Bundle::Priority)Util::toInt(prio) : Bundle::DEFAULT, ProfileTokenSet(), dirDate, token, false);
		else
			throw Exception("Duplicate bundle token");

		inBundle = true;		
	} else if(inDownloads || inBundle || inFile) {
		if(!curFile && name == sDownload) {
			int64_t size = Util::toInt64(getAttrib(attribs, sSize, 1));
			if(size == 0)
				return;
			try {
				const string& tgt = getAttrib(attribs, sTarget, 0);
				// @todo do something better about existing files
				target = QueueManager::checkTarget(tgt, true, curBundle);
				if(target.empty())
					return;
			} catch(const Exception&) {
				return;
			}

			if (curBundle && inBundle && !AirUtil::isParentOrExact(curBundle->getTarget(), target)) {
				//the file isn't inside the main bundle dir, can't add this
				return;
			}

			QueueItem::Priority p = (QueueItem::Priority)Util::toInt(getAttrib(attribs, sPriority, 3));
			time_t added = static_cast<time_t>(Util::toInt(getAttrib(attribs, sAdded, 4)));
			const string& tthRoot = getAttrib(attribs, sTTH, 5);
			if(tthRoot.empty())
				return;

			string tempTarget = getAttrib(attribs, sTempTarget, 5);
			uint8_t maxSegments = (uint8_t)Util::toInt(getAttrib(attribs, sMaxSegments, 5));

			if(added == 0)
				added = GET_TIME();

			QueueItemPtr qi = qm->fileQueue.findFile(target);

			if(!qi) {
				if (Util::toInt(getAttrib(attribs, sAutoPriority, 6)) == 1) {
					p = QueueItem::DEFAULT;
				}

				qi = qm->fileQueue.add(target, size, 0, p, tempTarget, added, TTHValue(tthRoot));
				qi->setMaxSegments(max((uint8_t)1, maxSegments));

				//bundles
				if (curBundle && inBundle) {
					//LogManager::getInstance()->message("itemtoken exists: " + bundleToken);
					qm->bundleQueue.addBundleItem(qi, curBundle);
				} else if (inDownloads) {
					//assign bundles for the items in the old queue file
					curBundle = new Bundle(qi);
				} else if (inFile && !curToken.empty()) {
					if (ConnectionManager::getInstance()->tokens.addToken(curToken)) {
						curBundle = new Bundle(qi, ProfileTokenSet(), curToken, false);
					} else {
						qm->fileQueue.remove(qi);
						throw Exception("Duplicate token");
					}
				}
			}
			if(!simple)
				curFile = qi;
		} else if(curFile && name == sSegment) {
			int64_t start = Util::toInt64(getAttrib(attribs, sStart, 0));
			int64_t size = Util::toInt64(getAttrib(attribs, sSize, 1));
			
			if(size > 0 && start >= 0 && (start + size) <= curFile->getSize()) {
				curFile->addFinishedSegment(Segment(start, size));
				if (curFile->getAutoPriority() && SETTING(AUTOPRIO_TYPE) == SettingsManager::PRIO_PROGRESS) {
					curFile->setPriority(curFile->calculateAutoPriority());
				}
			}
		} else if(curFile && name == sSource) {
			const string& cid = getAttrib(attribs, sCID, 0);
			if(cid.length() != 39) {
				// Skip loading this source - sorry old users
				return;
			}
			UserPtr user = ClientManager::getInstance()->getUser(CID(cid));
			ClientManager::getInstance()->updateNick(user, getAttrib(attribs, sNick, 1));

			try {
				const string& hubHint = getAttrib(attribs, sHubHint, 1);
				HintedUser hintedUser(user, hubHint);
				if (SettingsManager::lanMode) {
					const string& remotePath = getAttrib(attribs, sRemotePath, 2);
					if (remotePath.empty())
						return;

					qm->addSource(curFile, hintedUser, 0, remotePath) && user->isOnline();
				} else {
					qm->addSource(curFile, hintedUser, 0, Util::emptyString, true, false) && user->isOnline();
				}
			} catch(const Exception&) {
				return;
			}
		} else if(inBundle && curBundle && name == sFinished) {
			//LogManager::getInstance()->message("FOUND FINISHED TTH");
			const string& tth = getAttrib(attribs, sTTH, 0);
			const string& target = getAttrib(attribs, sTarget, 0);
			int64_t size = Util::toInt64(getAttrib(attribs, sSize, 1));
			time_t added = static_cast<time_t>(Util::toInt(getAttrib(attribs, sAdded, 4)));
			if(size == 0 || tth.empty() || target.empty() || added == 0)
				return;
			if(!Util::fileExists(target))
				return;
			qm->addFinishedItem(TTHValue(tth), curBundle, target, size, added);
		} else if(inBundle || inFile && name == sAutoSearch) {
			const string& autoSearch = getAttrib(attribs, sToken, 0);
			curBundle->addAutoSearch(Util::toInt(autoSearch));
		} else {
			//LogManager::getInstance()->message("QUEUE LOADING ERROR");
		}
	}
}

void QueueLoader::endTag(const string& name) {
	
	if(inDownloads || inBundle || inFile) {
		if(name == "Downloads") {
			inDownloads = false;
		} else if(name == sBundle) {
			ScopedFunctor([this] { curBundle = nullptr; });
			inBundle = false;
			if (curBundle->getQueueItems().empty()) {
				throw Exception(STRING_F(NO_FILES_WERE_LOADED, curBundle->getTarget()));
			} else {
				QueueManager::getInstance()->addBundle(curBundle, true);
			}
		} else if(name == sFile) {
			curToken = Util::emptyString;
			inFile = false;
			if (!curBundle || curBundle->getQueueItems().empty())
				throw Exception(STRING(NO_FILES_FROM_FILE));
		} else if(name == sDownload) {
			if (curBundle && curBundle->isFileBundle()) {
				/* Only for file bundles and when migrating an old queue */
				QueueManager::getInstance()->addBundle(curBundle, true);
			}
			curFile = nullptr;
		}
	}
}

string QueueManager::getBundlePath(const string& aBundleToken) const {
	auto b = bundleQueue.findBundle(aBundleToken);
	return b ? b->getTarget() : "Unknown";
}

void QueueManager::addFinishedItem(const TTHValue& tth, BundlePtr& aBundle, const string& aTarget, int64_t aSize, time_t aFinished) {
	//LogManager::getInstance()->message("ADD FINISHED TTH: " + tth.toBase32());
	if (fileQueue.findFile(aTarget)) {
		return;
	}
	QueueItemPtr qi = new QueueItem(aTarget, aSize, QueueItem::DEFAULT, QueueItem::FLAG_NORMAL, aFinished, tth, Util::emptyString);
	qi->addFinishedSegment(Segment(0, aSize)); //make it complete

	bundleQueue.addFinishedItem(qi, aBundle);
	fileQueue.add(qi);
	//LogManager::getInstance()->message("added finished tth, totalsize: " + Util::toString(aBundle->getFinishedFiles().size()));
}

void QueueManager::noDeleteFileList(const string& path) {
	if(!SETTING(KEEP_LISTS)) {
		protectedFileLists.push_back(path);
	}
}

// SearchManagerListener
void QueueManager::on(SearchManagerListener::SR, const SearchResultPtr& sr) noexcept {
	if (!SETTING(AUTO_ADD_SOURCE)) {
		return;
	}

	bool wantConnection = false;
	bool addSources = false;
	BundlePtr b = nullptr;
	QueueItemPtr qi = nullptr;

	{
		QueueItemList matches;

		RLock l(cs);
		if (SettingsManager::lanMode)
			fileQueue.findFiles(AirUtil::getTTH(sr->getFileName(), sr->getSize()), matches);
		else
			fileQueue.findFiles(sr->getTTH(), matches);

		for(auto& q: matches) {
			qi = q;
			// Size compare to avoid popular spoof
			if(qi->getSize() == sr->getSize() && !qi->isSource(sr->getUser()) && qi->getBundle()) {
				b = qi->getBundle();
				if (b->isFinished()) {
					continue;
				}

				if(qi->isFinished() && b->isSource(sr->getUser())) {
					continue;
				}

				if((b->countOnlineUsers() < (size_t)SETTING(MAX_AUTO_MATCH_SOURCES))) {
					addSources = true;
				} 
			}
			break;
		}
	}

	if (addSources && b->isFileBundle()) {
		/* No reason to match anything with file bundles */
		WLock l(cs);
		try {	 
			wantConnection = addSource(qi, HintedUser(sr->getUser(), sr->getHubURL()), QueueItem::Source::FLAG_FILE_NOT_AVAILABLE, sr->getFile());
		} catch(...) {
			// Ignore...
		}
	} else if(addSources) {
		string path = b->getMatchPath(sr->getFile(), qi->getTarget(), sr->getUser()->isSet(User::NMDC));
		if (!path.empty()) {
			if (sr->getUser()->isSet(User::NMDC)) {
				//A NMDC directory bundle, just add the sources without matching
				QueueItemList ql;
				int newFiles = 0;
				{
					WLock l(cs);
					b->getDirQIs(path, ql);
					for (auto& q: ql) {
						try {	 
							if (addSource(q, HintedUser(sr->getUser(), sr->getHubURL()), QueueItem::Source::FLAG_FILE_NOT_AVAILABLE, Util::emptyString)) { // no SettingsManager::lanMode in NMDC...
								wantConnection = true;
							}
							newFiles++;
						} catch(...) {
							// Ignore...
						}
					}
				}
				if (SETTING(REPORT_ADDED_SOURCES) && newFiles > 0) {
					LogManager::getInstance()->message(Util::toString(ClientManager::getInstance()->getNicks(HintedUser(sr->getUser(), sr->getHubURL()))) + ": " + 
						STRING_F(MATCH_SOURCE_ADDED, newFiles % b->getName().c_str()), LogManager::LOG_INFO);
				}
			} else {
				//An ADC directory bundle, match recursive partial list
				try {
					addList(HintedUser(sr->getUser(), sr->getHubURL()), QueueItem::FLAG_MATCH_QUEUE | QueueItem::FLAG_RECURSIVE_LIST |(path.empty() ? 0 : QueueItem::FLAG_PARTIAL_LIST), path);
				} catch(...) { }
			}
		} else if (SETTING(ALLOW_MATCH_FULL_LIST)) {
			//failed, use full filelist
			try {
				addList(HintedUser(sr->getUser(), sr->getHubURL()), QueueItem::FLAG_MATCH_QUEUE);
			} catch(const Exception&) {
				// ...
			}
		}
	}

	if(wantConnection) {
		ConnectionManager::getInstance()->getDownloadConnection(HintedUser(sr->getUser(), sr->getHubURL()));
	}
}

// ClientManagerListener
void QueueManager::on(ClientManagerListener::UserConnected, const OnlineUser& aUser, bool /*wasOffline*/) noexcept {
	bool hasDown = false;

	{
		QueueItemList ql;
		{
			RLock l(cs);
			userQueue.getUserQIs(aUser.getUser(), ql);
		}

		for(auto& q: ql) {
			fire(QueueManagerListener::StatusUpdated(), q);
			if(!hasDown && q->startDown() && !q->isHubBlocked(aUser.getUser(), aUser.getHubUrl()))
				hasDown = true;
		}
	}

	if(hasDown) { 
		ConnectionManager::getInstance()->getDownloadConnection(HintedUser(aUser.getUser(), aUser.getHubUrl()));
	}
}

void QueueManager::on(ClientManagerListener::UserDisconnected, const UserPtr& aUser, bool wentOffline) noexcept {
	if (!wentOffline)
		return;

	QueueItemList ql;
	{
		RLock l(cs);
		userQueue.getUserQIs(aUser, ql);
	}

	for(auto& q: ql)
		fire(QueueManagerListener::StatusUpdated(), q);
}

void QueueManager::runAltSearch() {
	auto b = bundleQueue.findSearchBundle(GET_TICK(), true);
	if (b) {
		searchBundle(b, false);
	} else {
		LogManager::getInstance()->message("No bundles to search for!", LogManager::LOG_INFO);
	}
}

void QueueManager::on(TimerManagerListener::Second, uint64_t aTick) noexcept {
	if((lastSave + 10000) < aTick) {
		saveQueue(false);
	}

	Bundle::PrioList qiPriorities;
	vector<pair<BundlePtr, Bundle::Priority>> bundlePriorities;
	auto prioType = SETTING(AUTOPRIO_TYPE);
	bool calculate = aTick >= getLastAutoPrio() + (SETTING(AUTOPRIO_INTERVAL)*1000);

	{
		RLock l(cs);
		for (auto& b: bundleQueue.getBundles() | map_values) {
			if (b->isFinished()) {
				continue;
			}

			if (calculate && prioType == SettingsManager::PRIO_PROGRESS && b->getAutoPriority()) {
				auto p2 = b->calculateProgressPriority();
				if(b->getPriority() != p2) {
					bundlePriorities.emplace_back(b, p2);
				}
			}

			for(auto& q: b->getQueueItems()) {
				if(q->isRunning()) {
					fire(QueueManagerListener::StatusUpdated(), q);
					if (calculate && SETTING(QI_AUTOPRIO) && q->getAutoPriority() && prioType == SettingsManager::PRIO_PROGRESS) {
						auto p1 = q->getPriority();
						if(p1 != QueueItem::PAUSED) {
							auto p2 = q->calculateAutoPriority();
							if(p1 != p2)
								qiPriorities.emplace_back(q, (int8_t)p2);
						}
					}
				}
			}
		}
	}

	if (calculate && prioType != SettingsManager::PRIO_DISABLED) {
		if (prioType == SettingsManager::PRIO_BALANCED) {
			//LogManager::getInstance()->message("Calculate autoprio (balanced)");
			calculateBundlePriorities(false);
			setLastAutoPrio(aTick);
		} else {
			//LogManager::getInstance()->message("Calculate autoprio (progress)");
			for(auto& bp: bundlePriorities)
				setBundlePriority(bp.first, bp.second, true);

			for(auto& qp: qiPriorities)
				setQIPriority(qp.first, (QueueItem::Priority)qp.second);
		}
	}
}

template<class T>
static void calculateBalancedPriorities(vector<pair<T, uint8_t>>& priorities, multimap<T, pair<int64_t, double>>& speedSourceMap, bool verbose) {
	if (speedSourceMap.empty())
		return;

	//scale the priorization maps
	double factorSpeed=0, factorSource=0;
	double max = max_element(speedSourceMap.begin(), speedSourceMap.end())->second.first;
	if (max > 0) {
		factorSpeed = 100 / max;
	}

	max = max_element(speedSourceMap.begin(), speedSourceMap.end())->second.second;
	if (max > 0) {
		factorSource = 100 / max;
	}

	multimap<int, T> finalMap;
	int uniqueValues = 0;
	for (auto& i: speedSourceMap) {
		auto points = (i.second.first * factorSpeed) + (i.second.second * factorSource);
		if (finalMap.find(points) == finalMap.end()) {
			uniqueValues++;
		}
		finalMap.emplace(points, i.first);
	}

	int prioGroup = 1;
	if (uniqueValues <= 1) {
		if (verbose) {
			LogManager::getInstance()->message("Not enough items with unique points to perform the priotization!", LogManager::LOG_INFO);
		}
		return;
	} else if (uniqueValues > 2) {
		prioGroup = uniqueValues / 3;
	}

	if (verbose) {
		LogManager::getInstance()->message("Unique values: " + Util::toString(uniqueValues) + " prioGroup size: " + Util::toString(prioGroup), LogManager::LOG_INFO);
	}


	//priority to set (4-2, high-low)
	int8_t prio = 4;

	//counters for analyzing identical points
	int lastPoints = 999;
	int prioSet=0;

	for (auto& i: finalMap) {
		if (lastPoints==i.first) {
			if (verbose) {
				LogManager::getInstance()->message(i.second->getTarget() + " points: " + Util::toString(i.first) + " setting prio " + AirUtil::getPrioText(prio), LogManager::LOG_INFO);
			}

			if(i.second->getPriority() != prio)
				priorities.emplace_back(i.second, prio);

			//don't increase the prio if two items have identical points
			if (prioSet < prioGroup) {
				prioSet++;
			}
		} else {
			if (prioSet == prioGroup && prio != 2) {
				prio--;
				prioSet=0;
			} 

			if (verbose) {
				LogManager::getInstance()->message(i.second->getTarget() + " points: " + Util::toString(i.first) + " setting prio " + AirUtil::getPrioText(prio), LogManager::LOG_INFO);
			}

			if(i.second->getPriority() != prio)
				priorities.emplace_back(i.second, prio);

			prioSet++;
			lastPoints = i.first;
		}
	}
}

void QueueManager::calculateBundlePriorities(bool verbose) {
	multimap<BundlePtr, pair<int64_t, double>> bundleSpeedSourceMap;

	/* Speed and source maps for files in each bundle */
	vector<multimap<QueueItemPtr, pair<int64_t, double>>> qiMaps;

	{
		RLock l (cs);
		for (auto& b: bundleQueue.getBundles() | map_values) {
			if (!b->isFinished()) {
				if (b->getAutoPriority()) {
					bundleSpeedSourceMap.emplace(b, b->getPrioInfo());
				}

				if (SETTING(QI_AUTOPRIO)) {
					qiMaps.push_back(b->getQIBalanceMaps());
				}
			}
		}
	}

	vector<pair<BundlePtr, uint8_t>> bundlePriorities;
	calculateBalancedPriorities<BundlePtr>(bundlePriorities, bundleSpeedSourceMap, verbose);

	for(auto& p: bundlePriorities) {
		setBundlePriority(p.first, (Bundle::Priority)p.second, true);
	}


	if (SETTING(QI_AUTOPRIO)) {

		vector<pair<QueueItemPtr, uint8_t>> qiPriorities;
		for(auto& s: qiMaps) {
			calculateBalancedPriorities<QueueItemPtr>(qiPriorities, s, verbose);
		}

		for(auto& p: qiPriorities) {
			setQIPriority(p.first, (QueueItem::Priority)p.second, true);
		}
	}
}

bool QueueManager::dropSource(Download* d) {
	BundlePtr b = d->getBundle();
	size_t onlineUsers = 0;

	if(b->getRunning() >= SETTING(DISCONNECT_MIN_SOURCES)) {
		int iHighSpeed = SETTING(DISCONNECT_FILE_SPEED);
		{
			RLock l (cs);
			onlineUsers = b->countOnlineUsers();
		}

		if((iHighSpeed == 0 || b->getSpeed() > iHighSpeed * 1024) && onlineUsers > 2) {
			d->setFlag(Download::FLAG_SLOWUSER);

			if(d->getAverageSpeed() < SETTING(REMOVE_SPEED)*1024) {
				return true;
			} else {
				d->getUserConnection().disconnect();
			}
		}
	}

	return false;
}

bool QueueManager::handlePartialResult(const HintedUser& aUser, const TTHValue& tth, const QueueItem::PartialSource& partialSource, PartsInfo& outPartialInfo) {
	bool wantConnection = false;
	dcassert(outPartialInfo.empty());
	QueueItemPtr qi = nullptr;

	// Locate target QueueItem in download queue
	{
		QueueItemList ql;

		RLock l(cs);
		fileQueue.findFiles(tth, ql);
		
		if(ql.empty()){
			dcdebug("Not found in download queue\n");
			return false;
		}
		
		qi = ql.front();

		// don't add sources to finished files
		// this could happen when "Keep finished files in queue" is enabled
		if(qi->isFinished())
			return false;
	}

	// Check min size
	if(qi->getSize() < PARTIAL_SHARE_MIN_SIZE){
		dcassert(0);
		return false;
	}

	// Get my parts info
	int64_t blockSize = HashManager::getInstance()->getBlockSize(qi->getTTH());
	if(blockSize == 0)
		blockSize = qi->getSize();

	{
		WLock l(cs);
		qi->getPartialInfo(outPartialInfo, blockSize);
		
		// Any parts for me?
		wantConnection = qi->isNeededPart(partialSource.getPartialInfo(), blockSize);

		// If this user isn't a source and has no parts needed, ignore it
		auto si = qi->getSource(aUser);
		if(si == qi->getSources().end()){
			si = qi->getBadSource(aUser);

			if(si != qi->getBadSources().end() && si->isSet(QueueItem::Source::FLAG_TTH_INCONSISTENCY))
				return false;

			if(!wantConnection) {
				if(si == qi->getBadSources().end())
					return false;
			} else {
				// add this user as partial file sharing source
				qi->addSource(aUser);
				si = qi->getSource(aUser);
				si->setFlag(QueueItem::Source::FLAG_PARTIAL);

				QueueItem::PartialSource* ps = new QueueItem::PartialSource(partialSource.getMyNick(),
					partialSource.getHubIpPort(), partialSource.getIp(), partialSource.getUdpPort());
				si->setPartialSource(ps);

				userQueue.addQI(qi, aUser);
				dcassert(si != qi->getSources().end());
				fire(QueueManagerListener::SourcesUpdated(), qi);
			}
		}

		// Update source's parts info
		if(si->getPartialSource()) {
			si->getPartialSource()->setPartialInfo(partialSource.getPartialInfo());
		}
	}
	
	// Connect to this user
	if(wantConnection && aUser.user->isOnline())
		ConnectionManager::getInstance()->getDownloadConnection(aUser);

	return true;
}

BundlePtr QueueManager::findBundle(const TTHValue& tth) {
	QueueItemList ql;
	{
		RLock l(cs);
		fileQueue.findFiles(tth, ql);
	}

	if (!ql.empty()) {
		return ql.front()->getBundle();
	}
	return nullptr;
}

bool QueueManager::handlePartialSearch(const UserPtr& aUser, const TTHValue& tth, PartsInfo& _outPartsInfo, string& _bundle, bool& _reply, bool& _add) {
	QueueItemPtr qi = nullptr;
	{
		QueueItemList ql;

		RLock l(cs);
		// Locate target QueueItem in download queue
		fileQueue.findFiles(tth, ql);
		if (ql.empty()) {
			return false;
		}

		qi = ql.front();

		//don't share files download from private chat
		if (qi->isSet(QueueItem::FLAG_PRIVATE))
			return false;

		BundlePtr b = qi->getBundle();
		if (b) {
			_bundle = b->getToken();

			//should we notify the other user about finished item?
			_reply = !b->getQueueItems().empty() && !b->isFinishedNotified(aUser);

			//do we have finished files that the other guy could download?
			_add = !b->getFinishedFiles().empty();
		}

		// do we have a file to send?
		if (!qi->hasPartialSharingTarget())
			return false;
	}

	if(qi->getSize() < PARTIAL_SHARE_MIN_SIZE){
		return false;  
	}


	int64_t blockSize = HashManager::getInstance()->getBlockSize(qi->getTTH());
	if(blockSize == 0)
		blockSize = qi->getSize();

	RLock l(cs);
	qi->getPartialInfo(_outPartsInfo, blockSize);

	return !_outPartsInfo.empty();
}

tstring QueueManager::getDirPath(const string& aDirName) const {
	RLock l(cs);
	auto dbp = bundleQueue.findRemoteDir(aDirName);
	return Text::toT(dbp.first);
}

void QueueManager::getUnfinishedPaths(StringList& retBundles) {
	RLock l(cs);
	for(auto& b: bundleQueue.getBundles() | map_values) {
		if (!b->isFileBundle() && !b->isFinished())
			retBundles.push_back(b->getTarget());
	}
}

void QueueManager::getForbiddenPaths(StringList& retBundles, const StringList& sharePaths) {
	BundleList hash;
	{
		RLock l(cs);
		for (auto& b: bundleQueue.getBundles() | map_values) {
			if (b->isFileBundle())
				continue;

			//check the path just to avoid hashing/scanning bundles from dirs that aren't being refreshed
			if (boost::find_if(sharePaths, [b](const string& p) { return AirUtil::isParentOrExact(p, b->getTarget()); }) == sharePaths.end()) {
				continue;
			}

			if(b->isFinished() && (b->isSet(Bundle::FLAG_SHARING_FAILED) || b->allowHash())) {
				hash.push_back(b);
			}

			retBundles.push_back(Text::toLower(b->getTarget()));
		}
	}

	for (auto& b: hash) {
		if(b->isSet(Bundle::FLAG_SHARING_FAILED) && !scanBundle(b)) {
			continue;
		}

		b->unsetFlag(Bundle::FLAG_SHARING_FAILED);
		hashBundle(b); 
	}

	sort(retBundles.begin(), retBundles.end());
}

void QueueManager::onBundleStatusChanged(BundlePtr& aBundle, AutoSearch::Status aStatus) {
	ProfileTokenSet searches;

	{
		RLock l(cs);
		searches = aBundle->getAutoSearches();
	}

	auto found = AutoSearchManager::getInstance()->onBundleStatus(aBundle, searches, aStatus);

	if (aStatus == AutoSearch::STATUS_FAILED_MISSING && !found && SETTING(AUTO_COMPLETE_BUNDLES)) {
		auto token = Util::randInt(10);

		{
			WLock l(cs);
			aBundle->addAutoSearch(token);
		}

		AutoSearchManager::getInstance()->addFailedBundle(aBundle, token); 
	}
}

void QueueManager::onBundleRemoved(BundlePtr& aBundle, bool finished) {
	ProfileTokenSet searches;

	{
		RLock l(cs);
		if (aBundle->getAutoSearches().empty())
			return;

		searches = aBundle->getAutoSearches();
	}

	AutoSearchManager::getInstance()->onRemoveBundle(aBundle, searches, finished);
}

void QueueManager::shareBundle(const string& aName) {
	BundlePtr b = nullptr;
	{
		RLock l (cs);
		b = bundleQueue.findRemoteDir(aName).second;
	}

	if (b) {
		b->unsetFlag(Bundle::FLAG_SHARING_FAILED);
		onBundleRemoved(b, true);
		hashBundle(b); 
		LogManager::getInstance()->message("The bundle " + aName + " has been added for hashing", LogManager::LOG_INFO);
	} else {
		LogManager::getInstance()->message("The bundle " + aName + " wasn't found", LogManager::LOG_WARNING);
	}
}

bool QueueManager::isChunkDownloaded(const TTHValue& tth, int64_t startPos, int64_t& bytes, int64_t& fileSize_, string& target) {
	QueueItemList ql;

	RLock l(cs);
	fileQueue.findFiles(tth, ql);

	if(ql.empty()) return false;


	QueueItemPtr qi = ql.front();
	if (!qi->hasPartialSharingTarget())
		return false;

	fileSize_ = qi->getSize();
	target = qi->isFinished() ? qi->getTarget() : qi->getTempTarget();

	return qi->isChunkDownloaded(startPos, bytes);
}

bool QueueManager::addBundle(BundlePtr& aBundle, bool loading) {
	if (aBundle->getQueueItems().empty()) {
		return false;
	}

	BundlePtr oldBundle = NULL;
	{
		RLock l(cs);
		oldBundle = bundleQueue.getMergeBundle(aBundle->getTarget());
	}

	if (oldBundle) {
		mergeBundle(oldBundle, aBundle);
		return false;
	} else if (!aBundle->isFileBundle()) {
		//check that there are no file bundles inside the bundle that will be created and merge them in that case
		mergeFileBundles(aBundle);
	}

	{
		WLock l(cs);
		bundleQueue.addBundle(aBundle);
		fire(QueueManagerListener::BundleAdded(), aBundle);
		aBundle->updateSearchMode();
	}

	onBundleStatusChanged(aBundle, AutoSearch::STATUS_QUEUED_OK);
	if (loading)
		return true;

	if (SETTING(AUTO_SEARCH) && SETTING(AUTO_ADD_SOURCE) && aBundle->getPriority() != Bundle::PAUSED) {
		aBundle->setFlag(Bundle::FLAG_SCHEDULE_SEARCH);
		addBundleUpdate(aBundle);
	}

	LogManager::getInstance()->message(STRING_F(BUNDLE_CREATED, aBundle->getName().c_str() % aBundle->getQueueItems().size()) + 
		" (" + CSTRING_F(TOTAL_SIZE, Util::formatBytes(aBundle->getSize()).c_str()) + ")", LogManager::LOG_INFO);

	connectBundleSources(aBundle);
	return true;
}

void QueueManager::connectBundleSources(BundlePtr& aBundle) {
	if (aBundle->getPriority() == Bundle::PAUSED)
		return;

	HintedUserList x;
	{
		RLock l(cs);
		aBundle->getSources(x);
	}

	for(auto& u: x) { 
		if(u.user && u.user->isOnline())
			ConnectionManager::getInstance()->getDownloadConnection(u, false); 
	}
}

void QueueManager::readdBundle(BundlePtr& aBundle) {
	aBundle->unsetFlag(Bundle::FLAG_SHARING_FAILED);

	{
		WLock l(cs);
		//check that the finished files still exist
		for(auto i = aBundle->getFinishedFiles().begin(); i != aBundle->getFinishedFiles().end();) {
			QueueItemPtr q = *i;
			if (!Util::fileExists(q->getTarget())) {
				bundleQueue.removeFinishedItem(q);
				fileQueue.remove(q);
			} else {
				++i;
			}
		}
		bundleQueue.addSearchPrio(aBundle);
	}

	onBundleStatusChanged(aBundle, AutoSearch::STATUS_QUEUED_OK);
	LogManager::getInstance()->message(STRING_F(BUNDLE_READDED, aBundle->getName().c_str()), LogManager::LOG_INFO);
}

void QueueManager::mergeFileBundles(BundlePtr& targetBundle) {
	BundleList bl;
	{
		RLock l(cs);
		bundleQueue.getSubBundles(targetBundle->getTarget(), bl);
	}

	for(auto& sourceBundle: bl) {
		fire(QueueManagerListener::BundleMoved(), sourceBundle); 
		mergeBundle(targetBundle, sourceBundle);
	}
}

void QueueManager::mergeBundle(BundlePtr& targetBundle, BundlePtr& sourceBundle) {

	//finished bundle but failed hashing/scanning?
	bool finished = targetBundle->isFinished();

	HintedUserList x;
	//new bundle? we need to connect to sources then
	if (sourceBundle->isSet(Bundle::FLAG_NEW)) {
		for(auto& st: sourceBundle->getSources())
			x.push_back(get<Bundle::SOURCE_USER>(st));
	}

	int added = 0;

	//the target bundle is a sub directory of the source bundle?
	bool changeTarget = AirUtil::isSub(targetBundle->getTarget(), sourceBundle->getTarget());
	if (changeTarget) {
		string oldTarget = targetBundle->getTarget();
		added = changeBundleTarget(targetBundle, sourceBundle->getTarget());
		RLock l (cs);
		fire(QueueManagerListener::BundleMerged(), targetBundle, oldTarget);

		auto as = targetBundle->getAutoSearches();
		as.insert(sourceBundle->getAutoSearches().begin(), sourceBundle->getAutoSearches().end());
		targetBundle->setAutoSearches(as);
	}

	onBundleStatusChanged(targetBundle, AutoSearch::STATUS_QUEUED_OK);

	{
		WLock l (cs);
		added = (int)sourceBundle->getQueueItems().size();
		moveBundleItems(sourceBundle, targetBundle, true);
	}
	
	if (finished) {
		readdBundle(targetBundle);
	} else if (!changeTarget) {
		targetBundle->setFlag(Bundle::FLAG_UPDATE_SIZE);
		addBundleUpdate(targetBundle);
		targetBundle->setDirty();
	}

	if (targetBundle->getPriority() != Bundle::PAUSED) {
		for(auto& u: x) {
			if(u.user && u.user->isOnline())
				ConnectionManager::getInstance()->getDownloadConnection(u, false); 
		}
	}


	/* Report */
	if (sourceBundle->isFileBundle()) {
		LogManager::getInstance()->message(STRING_F(FILEBUNDLE_MERGED, sourceBundle->getName().c_str() % targetBundle->getName().c_str()), LogManager::LOG_INFO);
	} else if (changeTarget) {
		string tmp = STRING_F(BUNDLE_CREATED, targetBundle->getName().c_str() % targetBundle->getQueueItems().size()) + 
			" (" + CSTRING_F(TOTAL_SIZE, Util::formatBytes(targetBundle->getSize()).c_str()) + ")";

		if (added > 0)
			tmp += str(boost::format(", " + STRING(EXISTING_BUNDLES_MERGED)) % (added+1));

		LogManager::getInstance()->message(tmp, LogManager::LOG_INFO);
	} else if (targetBundle->getTarget() == sourceBundle->getTarget()) {
		LogManager::getInstance()->message(STRING_F(X_BUNDLE_ITEMS_ADDED, added % targetBundle->getName().c_str()), LogManager::LOG_INFO);
	} else {
		LogManager::getInstance()->message(STRING_F(BUNDLE_MERGED, sourceBundle->getName().c_str() % targetBundle->getName().c_str() % added), LogManager::LOG_INFO);
	}
}

void QueueManager::moveBundleItems(BundlePtr& sourceBundle, BundlePtr& targetBundle, bool fireAdded) {
	for (auto j = sourceBundle->getQueueItems().begin(); j != sourceBundle->getQueueItems().end();) {
		moveBundleItem(*j, targetBundle, fireAdded);
		j = sourceBundle->getQueueItems().begin();
	}

	bundleQueue.removeBundle(sourceBundle);
}

int QueueManager::changeBundleTarget(BundlePtr& aBundle, const string& newTarget) {
	/* In this case we also need check if there are directory bundles inside the subdirectories */
	BundleList mBundles;
	{
		WLock l(cs);
		bundleQueue.moveBundle(aBundle, newTarget); //set the new target
		bundleQueue.getSubBundles(newTarget, mBundles);
	}

	{
		WLock l(cs);
		for(auto& b: mBundles) {
			fire(QueueManagerListener::BundleRemoved(), b);
			for (auto j = b->getFinishedFiles().begin(); j != b->getFinishedFiles().end();) {
				QueueItemPtr q = *j;
				bundleQueue.removeFinishedItem(q);
				bundleQueue.addFinishedItem(q, aBundle);
				j = b->getFinishedFiles().begin();
			}

			moveBundleItems(b, aBundle, false);
		}
	}

	aBundle->setFlag(Bundle::FLAG_UPDATE_SIZE);
	aBundle->setFlag(Bundle::FLAG_UPDATE_NAME);
	addBundleUpdate(aBundle);
	aBundle->setDirty();

	return (int)mBundles.size();
}

int QueueManager::getDirItemCount(const BundlePtr& aBundle, const string& aDir) const noexcept { 
	RLock l(cs);
	QueueItemList ql;
	aBundle->getDirQIs(aDir, ql);
	return (int)ql.size();
}

uint8_t QueueManager::isDirQueued(const string& aDir) const {
	RLock l(cs);
	auto bdp = bundleQueue.findRemoteDir(aDir);
	if (bdp.second) {
		auto s = bdp.second->getPathInfo(bdp.first);
		if (s.first == 0) //no queued items
			return 2;
		else
			return 1;
	}
	return 0;
}



int QueueManager::getBundleItemCount(const BundlePtr& aBundle) const noexcept {
	RLock l(cs); 
	return aBundle->getQueueItems().size(); 
}

int QueueManager::getFinishedItemCount(const BundlePtr& aBundle) const noexcept { 
	RLock l(cs); 
	return (int)aBundle->getFinishedFiles().size(); 
}

void QueueManager::setBundlePriorities(const string& aSource, const BundleList& sourceBundles, Bundle::Priority p, bool autoPrio) {
	if (sourceBundles.empty()) {
		return;
	}

	BundlePtr bundle;

	if (sourceBundles.size() == 1 && AirUtil::isSub(aSource, sourceBundles.front()->getTarget())) {
		//we aren't removing the whole bundle
		bundle = sourceBundles.front();
		QueueItemList ql;
		{
			RLock l(cs);
			bundle->getDirQIs(aSource, ql);
		}

		for (auto& q: ql) {
			if (autoPrio) {
				setQIAutoPriority(q->getTarget(), q->getAutoPriority());
			} else {
				setQIPriority(q, (QueueItem::Priority)p);
			}
		}
	} else {
		for(auto b: sourceBundles) {
			if (autoPrio) {
				setBundleAutoPriority(b->getToken());
			} else {
				setBundlePriority(b, (Bundle::Priority)p);
			}
		}
	}
}

void QueueManager::removeDir(const string aSource, const BundleList& sourceBundles, bool removeFinished) {
	if (sourceBundles.empty()) {
		return;
	}

	if (sourceBundles.size() == 1 && AirUtil::isSub(aSource, sourceBundles.front()->getTarget())) {
		//we aren't removing the whole bundle
		BundlePtr bundle = sourceBundles.front();
		QueueItemList ql;
		{
			WLock l(cs);
			bundle->getDirQIs(aSource, ql);
			for (auto i = bundle->getFinishedFiles().begin(); i != bundle->getFinishedFiles().end();) {
				QueueItemPtr qi = *i;
				if (AirUtil::isSub(qi->getTarget(), aSource)) {
					UploadManager::getInstance()->abortUpload((*i)->getTarget());
					if (removeFinished) {
						File::deleteFile(qi->getTarget());
					}
					fileQueue.remove(qi);
					bundleQueue.removeFinishedItem(qi);
				} else {
					i++;
				}
			}
		}

		AirUtil::removeIfEmpty(aSource);
		for(auto& qi: ql) 
			removeQI(qi, false);
	} else {
		for(auto b: sourceBundles) 
			removeBundle(b, false, removeFinished);
	}
}

void QueueManager::moveBundle(const string& aSource, const string& aTarget, BundlePtr sourceBundle, bool moveFinished) {

	string sourceBundleTarget = sourceBundle->getTarget();
	bool hasMergeBundle = false;
	BundlePtr newBundle = NULL;
	auto newBundleTarget = AirUtil::convertMovePath(sourceBundle->getTarget(), aSource, aTarget);

	//handle finished items
	{
		WLock l(cs);
		//can we merge this with an existing bundle?
		newBundle = bundleQueue.getMergeBundle(newBundleTarget);
		if (newBundle && newBundle != sourceBundle) {
			hasMergeBundle = true;
		} else {
			newBundle = sourceBundle;
		}

		//handle finished items
		for (auto i = sourceBundle->getFinishedFiles().begin(); i != sourceBundle->getFinishedFiles().end();) {
			QueueItemPtr qi = *i;
			if (moveFinished) {
				string targetPath = AirUtil::convertMovePath(qi->getTarget(), aSource, aTarget);
				if (!fileQueue.findFile(targetPath)) {
					if(!Util::fileExists(targetPath)) {
						qi->unsetFlag(QueueItem::FLAG_MOVED);
						moveFile(qi->getTarget(), targetPath, qi, true);
						if (hasMergeBundle) {
							bundleQueue.removeFinishedItem(qi);
							fileQueue.move(qi, targetPath);
							bundleQueue.addFinishedItem(qi, newBundle);
						} else {
							//keep in the current bundle
							fileQueue.move(qi, targetPath);
							i++;
						}
						continue;
					} else {
						/* TODO: add for recheck */
					}
				}
			}
			fileQueue.remove(qi);
			bundleQueue.removeFinishedItem(qi);
		}

		//we may not be able to remove the directory instantly if we have finished files to move
		mover.removeDir(sourceBundle->getTarget());
	}

	//convert the QIs
	QueueItemList ql;
	{
		RLock l(cs);
		fire(QueueManagerListener::BundleMoved(), sourceBundle);
		ql = sourceBundle->getQueueItems();
	}

	for (auto i = ql.begin(); i != ql.end();) {
		if (!moveBundleFile(*i, AirUtil::convertMovePath((*i)->getTarget(), aSource, aTarget), false)) {
			i = ql.erase(i);
		} else {
			++i;
		}
	}

	if (ql.empty()) {
		//may happen if all queueitems are being merged with existing ones
		removeBundle(sourceBundle, false, false, true);
		return;
	}

	if (hasMergeBundle) {
		mergeBundle(newBundle, sourceBundle);
	} else {
		//nothing to merge to, move the old bundle
		int merged = changeBundleTarget(sourceBundle, newBundleTarget);

		{
			RLock l(cs);
			fire(QueueManagerListener::BundleAdded(), sourceBundle);
		}

		string tmp = STRING_F(BUNDLE_MOVED, sourceBundle->getName().c_str() % sourceBundle->getTarget().c_str());
		if (merged > 0)
			tmp += str(boost::format(" (" + STRING(EXISTING_BUNDLES_MERGED) + ")") % merged);

		LogManager::getInstance()->message(tmp, LogManager::LOG_INFO);
	}
}

void QueueManager::splitBundle(const string& aSource, const string& aTarget, BundlePtr sourceBundle, bool moveFinished) {
	//first pick the items that we need to move
	QueueItemList ql;
	BundlePtr newBundle = NULL;
	{
		RLock l(cs);
		sourceBundle->getDirQIs(aSource, ql);
		newBundle = bundleQueue.getMergeBundle(aTarget);
	}

	//create a temp bundle for split items
	BundlePtr tempBundle = BundlePtr(new Bundle(aTarget, GET_TIME(), sourceBundle->getPriority(), sourceBundle->getAutoSearches(), sourceBundle->getDirDate()));

	//can we merge the split folder?
	bool hasMergeBundle = newBundle;

	{
		WLock l(cs);

		//handle finished items
		for (auto i = sourceBundle->getFinishedFiles().begin(); i != sourceBundle->getFinishedFiles().end();) {
			QueueItemPtr qi = *i;
			if (AirUtil::isSub(qi->getTarget(), aSource)) {
				if (moveFinished) {
					string targetPath = AirUtil::convertMovePath(qi->getTarget(), aSource, aTarget);
					if (!fileQueue.findFile(targetPath)) {
						if(!Util::fileExists(targetPath)) {
							qi->unsetFlag(QueueItem::FLAG_MOVED);
							moveFile(qi->getTarget(), targetPath, qi, true);
							if (newBundle == sourceBundle) {
								fileQueue.move(qi, targetPath);
								i++;
							} else {
								bundleQueue.removeFinishedItem(qi);
								fileQueue.move(qi, targetPath);
								bundleQueue.addFinishedItem(qi, hasMergeBundle ? newBundle : tempBundle);
							}
							continue;
						} else {
							/* TODO: add for recheck */
						}
					}
				}
				fileQueue.remove(qi);
				bundleQueue.removeFinishedItem(qi);
			} else {
				i++;
			}
		}

		//we may not be able to remove the directory instantly if we have finished files to move
		mover.removeDir(sourceBundle->getTarget());

		fire(QueueManagerListener::BundleMoved(), sourceBundle);
	}

	//convert the QIs
	for (auto i = ql.begin(); i != ql.end();) {
		if (!moveBundleFile(*i, AirUtil::convertMovePath((*i)->getTarget(), aSource, aTarget), false)) {
			i = ql.erase(i);
		} else {
			i++;
		}
	}

	if (newBundle != sourceBundle) {
		WLock l(cs);
		moveBundleItems(ql, tempBundle, false);
	}

	{
		RLock l(cs);
		if (!sourceBundle->getQueueItems().empty()) {
			fire(QueueManagerListener::BundleAdded(), sourceBundle);
		}
	}

	//merge or add the temp bundle

	if (newBundle == sourceBundle) {
		return;
	} else if (hasMergeBundle) {
		//merge the temp bundle
		mergeBundle(newBundle, tempBundle);
	} else {
		addBundle(tempBundle);
	}
}

void QueueManager::moveFileBundle(BundlePtr& aBundle, const string& aTarget) noexcept {
	QueueItemPtr qi = nullptr;
	BundlePtr targetBundle = nullptr;
	{
		RLock l(cs);
		qi = aBundle->getQueueItems().front();
		fire(QueueManagerListener::BundleMoved(), aBundle);
		targetBundle = bundleQueue.getMergeBundle(aTarget);
	}

	if (!moveBundleFile(qi, aTarget, false)) {
		return;
	}

	if (targetBundle) {
		mergeBundle(targetBundle, aBundle);
		fire(QueueManagerListener::Added(), qi);
	} else {
		LogManager::getInstance()->message(STRING_F(FILEBUNDLE_MOVED, aBundle->getName().c_str() % aBundle->getTarget().c_str()), LogManager::LOG_INFO);

		{
			RLock l(cs);
			aBundle->setTarget(qi->getTarget());
			fire(QueueManagerListener::BundleAdded(), aBundle);
		}

		/* update the bundle path in transferview */
		fire(QueueManagerListener::BundleTarget(), aBundle);

		aBundle->setDirty();
	}
}

bool QueueManager::moveBundleFile(QueueItemPtr& qs, const string& aTarget, bool movingSingleItems) noexcept {
	string target = Util::validateFileName(aTarget);

	if(qs->getTarget() == target) {
		//LogManager::getInstance()->message("MOVE FILE, TARGET SAME");
		return false;
	}
	dcassert(qs->getBundle());

	// Let's see if the target exists...then things get complicated...
	QueueItemPtr qt = nullptr;
	{
		RLock l(cs);
		qt = fileQueue.findFile(target);
	}

	if(!qt) {
		//Does the file exist already on the disk?
		if(Util::fileExists(target)) {
			removeQI(qs, !movingSingleItems);
			return false;
		}
		// Good, update the target and move in the queue...
		{
			WLock l(cs);
			if(qs->isRunning()) {
				DownloadManager::getInstance()->setTarget(qs->getTarget(), aTarget);
			}

			string oldTarget = qs->getTarget();
			fileQueue.move(qs, target);
			if (movingSingleItems)
				fire(QueueManagerListener::Moved(), qs, oldTarget);
		}
		return true;
	}

	// Don't move to target of different size
	if(qs->getSize() != qt->getSize() || qs->getTTH() != qt->getTTH())
		return false;

	{
		WLock l(cs);
		if (qt->isFinished()) {
			dcassert(qt->getBundle());
			if (replaceFinishedItem(qt)) {
				fileQueue.move(qs, target);
				return true;
			}
		} else {
			for(auto& s: qs->getSources()) {
				try {
					//addSource(qt, s.getUser(), QueueItem::Source::FLAG_MASK, s.getRemotePath());
					addSource(qt, s.getUser(), QueueItem::Source::FLAG_MASK, Util::emptyString);
				} catch(const Exception&) {
					//..
				}
			}
		}
	}
	removeQI(qs, !movingSingleItems);
	return false;
}

void QueueManager::moveFiles(const StringPairList& sourceTargetList) noexcept {
	QueueItemList ql;
	for(auto& sp: sourceTargetList) {
		QueueItemPtr qs = nullptr;
		{
			RLock l(cs);
			qs = fileQueue.findFile(sp.first);
		}
		if(qs) {
			BundlePtr b = qs->getBundle();
			dcassert(b);
			if (b) {
				if (b->isFileBundle()) {
					//the path has been converted already
					moveFileBundle(b, sp.second);
				} else if (moveBundleFile(qs, sp.second, true)) {
					ql.push_back(qs);
				}
			}
		}
	}

	if (!ql.empty()) {
		//all files should be part of the same directory bundle
		QueueItemPtr qi = ql.front();
		BundlePtr sourceBundle = qi->getBundle();
		BundlePtr targetBundle = nullptr;

		{
			RLock l(cs);
			targetBundle = bundleQueue.getMergeBundle(qi->getTarget());
		}

		if (targetBundle) {
			bool finished = false;
			//are we moving items inside the same bundle?
			if (targetBundle == sourceBundle) {
				for(auto& qi: ql)
					fire(QueueManagerListener::Added(), qi);
				return;
			}

			{
				WLock l(cs);
				finished = targetBundle->isFinished();
				moveBundleItems(ql, targetBundle, !finished);
			}

			if (finished) {
				readdBundle(targetBundle);

				RLock l (cs);
				fire(QueueManagerListener::BundleAdded(), targetBundle);
			}
		} else {
			//split into file bundles
			BundleList newBundles;
			{
				WLock l(cs);
				for(auto& qi: ql) {
					bundleQueue.removeBundleItem(qi, false);
					userQueue.removeQI(qi, false); //we definately don't want to remove downloads because the QI will stay the same
					targetBundle = BundlePtr(new Bundle(qi, sourceBundle->getAutoSearches()));
					targetBundle->setPriority(sourceBundle->getPriority());
					targetBundle->setAutoPriority(sourceBundle->getAutoPriority());

					if (qi->isRunning()) {
						//now we need to move the download(s) to correct bundle... the target has been changed earlier, if needed
						DownloadManager::getInstance()->changeBundle(sourceBundle, targetBundle, qi->getTarget());
					}

					/* ADDING */
					qi->setPriority((QueueItem::Priority)sourceBundle->getPriority());
					qi->setAutoPriority(sourceBundle->getAutoPriority());
					userQueue.addQI(qi);
					newBundles.push_back(targetBundle);
				}
			}

			for(auto& b: newBundles) 
				addBundle(b, false);
		}

		{
			RLock l(cs);
			if (!sourceBundle->getQueueItems().empty()) {
				return;
			}
		}

		//the bundle is empty
		removeBundle(sourceBundle, false, false, true);
	}
}

void QueueManager::moveBundleItems(const QueueItemList& ql, BundlePtr& targetBundle, bool fireAdded) {
	/* NO FILEBUNDLES SHOULD COME HERE */
	if (!ql.empty() && !ql.front()->getBundle()->isFileBundle()) {
		BundlePtr sourceBundle = ql.front()->getBundle();
		bool empty = false;

		{
			WLock l(cs);
			for(auto qi: ql) 
				moveBundleItem(qi, targetBundle, fireAdded);
			empty = sourceBundle->getQueueItems().empty();
		}	

		if (empty) {
			removeBundle(sourceBundle, false, false, true);
		} else {
			targetBundle->setFlag(Bundle::FLAG_UPDATE_SIZE);
			addBundleUpdate(targetBundle);
			targetBundle->setDirty();
			if (fireAdded) {
				sourceBundle->setFlag(Bundle::FLAG_UPDATE_SIZE);
				addBundleUpdate(sourceBundle);
				sourceBundle->setDirty();
			}
		}
	}
}

void QueueManager::moveBundleItem(QueueItemPtr qi, BundlePtr& targetBundle, bool fireAdded) {
	BundlePtr sourceBundle = qi->getBundle();
	bundleQueue.removeBundleItem(qi, false);
	userQueue.removeQI(qi, false); //we definately don't want to remove downloads because the QI will stay the same

	if (qi->isRunning()) {
		//now we need to move the download(s) to correct bundle... the target has been changed earlier, if needed
		DownloadManager::getInstance()->changeBundle(sourceBundle, targetBundle, qi->getTarget());
	}

	qi->setBundle(nullptr);

	/* ADDING */
	bundleQueue.addBundleItem(qi, targetBundle);
	userQueue.addQI(qi);
	if (fireAdded) {
		fire(QueueManagerListener::Added(), qi);
	}
}

void QueueManager::addBundleUpdate(const BundlePtr& aBundle) {
	delayEvents.addEvent(aBundle->getToken(), [this, aBundle] { handleBundleUpdate(aBundle->getToken()); }, aBundle->isSet(Bundle::FLAG_SCHEDULE_SEARCH) ? 10000 : 1000);
}

void QueueManager::handleBundleUpdate(const string& bundleToken) {
	//LogManager::getInstance()->message("QueueManager::sendBundleUpdate");
	BundlePtr b = nullptr;
	{
		RLock l(cs);
		b = bundleQueue.findBundle(bundleToken);
	}

	if (b) {
		if (b->isSet(Bundle::FLAG_UPDATE_SIZE) || b->isSet(Bundle::FLAG_UPDATE_NAME)) {
			if (b->isSet(Bundle::FLAG_UPDATE_SIZE)) {
				fire(QueueManagerListener::BundleSize(), b);
			} 
			if (b->isSet(Bundle::FLAG_UPDATE_NAME)) {
				fire(QueueManagerListener::BundleTarget(), b);
			}
			DownloadManager::getInstance()->sendSizeNameUpdate(b);
		}
		
		if (b->isSet(Bundle::FLAG_SCHEDULE_SEARCH)) {
			searchBundle(b, false);
		}
	}
}

void QueueManager::removeBundleItem(QueueItemPtr& qi, bool finished, bool moved /*false*/) {
	BundlePtr bundle = qi->getBundle();
	if (!bundle) {
		return;
	}
	bool emptyBundle = false;

	{
		WLock l(cs);
		bundleQueue.removeBundleItem(qi, finished);
		if (finished) {
			fileQueue.decreaseSize(qi->getSize());
		}

		emptyBundle = bundle->getQueueItems().empty();
	}

	if (emptyBundle) {
		removeBundle(bundle, finished, false, moved);
	} else {
		bundle->setDirty();
	}
}

void QueueManager::removeBundle(BundlePtr& aBundle, bool finished, bool removeFinished, bool moved /*false*/) {
	if (aBundle->isSet(Bundle::FLAG_NEW)) {
		return;
	}

	if (finished) {
		aBundle->finishBundle();
		fire(QueueManagerListener::BundleFinished(), aBundle);
	} else if (!moved) {
		//LogManager::getInstance()->message("The Bundle " + aBundle->getName() + " has been removed");
		DownloadManager::getInstance()->disconnectBundle(aBundle);
		{
			WLock l(cs);
			for (auto i = aBundle->getFinishedFiles().begin(); i != aBundle->getFinishedFiles().end();) {
				QueueItemPtr q = *i;
				UploadManager::getInstance()->abortUpload(q->getTarget());
				fileQueue.remove(q);
				if (removeFinished) {
					File::deleteFile(q->getTarget());
				}

				i = aBundle->getFinishedFiles().erase(i);
			}

			for (auto i = aBundle->getQueueItems().begin(); i != aBundle->getQueueItems().end();) {
				QueueItemPtr qi = *i;
				UploadManager::getInstance()->abortUpload(qi->getTarget());

				if(!qi->isRunning() && !qi->getTempTarget().empty() && qi->getTempTarget() != qi->getTarget()) {
					File::deleteFile(qi->getTempTarget());
				}

				if(!qi->isFinished()) {
					userQueue.removeQI(qi, true, false);
					fire(QueueManagerListener::Removed(), qi, false);
				}

				fileQueue.remove(qi);
				i = aBundle->getQueueItems().erase(i);
			}

			fire(QueueManagerListener::BundleRemoved(), aBundle);

			LogManager::getInstance()->message(STRING_F(BUNDLE_X_REMOVED, aBundle->getName()), LogManager::LOG_INFO);
		}

		onBundleRemoved(aBundle, false);
		if (!aBundle->isFileBundle()) {
			AirUtil::removeIfEmpty(aBundle->getTarget());
		}
	}

	{
		WLock l(cs);
		if (!finished) {
			bundleQueue.removeBundle(aBundle);
		} else {
			aBundle->deleteBundleFile();
			bundleQueue.removeSearchPrio(aBundle);
		}
	}
}

MemoryInputStream* QueueManager::generateTTHList(const string& bundleToken, bool isInSharingHub) {
	if(!isInSharingHub)
		throw QueueException(UserConnection::FILE_NOT_AVAILABLE);

	string tths;
	StringOutputStream tthList(tths);
	{
		RLock l(cs);
		BundlePtr b = bundleQueue.findBundle(bundleToken);
		if (b) {
			//write finished items
			string tmp2;
			for(auto& q: b->getFinishedFiles()) {
				if (q->isSet(QueueItem::FLAG_MOVED)) {
					tmp2.clear();
					tthList.write(q->getTTH().toBase32(tmp2) + " ");
				}
			}
		}
	}

	if (tths.size() == 0) {
		throw QueueException(UserConnection::FILE_NOT_AVAILABLE);
	} else {
		return new MemoryInputStream(tths);
	}
}

void QueueManager::addBundleTTHList(const HintedUser& aUser, const string& remoteBundle, const TTHValue& tth) {
	//LogManager::getInstance()->message("ADD TTHLIST");
	if (findBundle(tth)) {
		addList(aUser, QueueItem::FLAG_TTHLIST_BUNDLE | QueueItem::FLAG_PARTIAL_LIST | QueueItem::FLAG_MATCH_QUEUE, remoteBundle);
	}
}

bool QueueManager::checkPBDReply(HintedUser& aUser, const TTHValue& aTTH, string& _bundleToken, bool& _notify, bool& _add, const string& remoteBundle) {
	BundlePtr bundle = findBundle(aTTH);
	if (bundle) {
		WLock l(cs);
		//LogManager::getInstance()->message("checkPBDReply: BUNDLE FOUND");
		_bundleToken = bundle->getToken();
		_add = !bundle->getFinishedFiles().empty();

		if (!bundle->getQueueItems().empty()) {
			bundle->addFinishedNotify(aUser, remoteBundle);
			_notify = true;
		}
		return true;
	}
	//LogManager::getInstance()->message("checkPBDReply: CHECKNOTIFY FAIL");
	return false;
}

void QueueManager::addFinishedNotify(HintedUser& aUser, const TTHValue& aTTH, const string& remoteBundle) {
	BundlePtr bundle = findBundle(aTTH);
	if (bundle) {
		WLock l(cs);
		//LogManager::getInstance()->message("addFinishedNotify: BUNDLE FOUND");
		if (!bundle->getQueueItems().empty()) {
			bundle->addFinishedNotify(aUser, remoteBundle);
		}
	}
	//LogManager::getInstance()->message("addFinishedNotify: BUNDLE NOT FOUND");
}

void QueueManager::removeBundleNotify(const UserPtr& aUser, const string& bundleToken) {
	WLock l(cs);
	BundlePtr bundle = bundleQueue.findBundle(bundleToken);
	if (bundle) {
		bundle->removeFinishedNotify(aUser);
	}
}

void QueueManager::updatePBD(const HintedUser& aUser, const TTHValue& aTTH) {
	//LogManager::getInstance()->message("UPDATEPBD");
	bool wantConnection = false;
	QueueItemList qiList;
	{
		WLock l(cs);
		fileQueue.findFiles(aTTH, qiList);
		for(auto& q: qiList) {
			try {
				//LogManager::getInstance()->message("ADDSOURCE");
				if (addSource(q, aUser, QueueItem::Source::FLAG_FILE_NOT_AVAILABLE, Util::emptyString)) {
					wantConnection = true;
				}
			} catch(...) {
				// Ignore...
			}
		}
	}

	if(wantConnection && aUser.user->isOnline())
		ConnectionManager::getInstance()->getDownloadConnection(aUser);
}

void QueueManager::searchBundle(BundlePtr& aBundle, bool manual) {
	if (!SETTING(AUTO_ADD_SOURCE)) {
		LogManager::getInstance()->message(STRING(AUTO_ADD_SOURCES_DISABLED), LogManager::LOG_WARNING);
		return;
	}

	map<string, QueueItemPtr> searches;
	int64_t nextSearch = 0;
	{
		RLock l(cs);
		bool isScheduled = aBundle->isSet(Bundle::FLAG_SCHEDULE_SEARCH);

		aBundle->unsetFlag(Bundle::FLAG_SCHEDULE_SEARCH);
		if (!manual)
			nextSearch = (bundleQueue.recalculateSearchTimes(aBundle->isRecent(), false) - GET_TICK()) / (60*1000);

		if (isScheduled && !aBundle->allowAutoSearch())
			return;

		aBundle->getSearchItems(searches, manual);
	}

	if (searches.size() <= 5) {
		aBundle->setSimpleMatching(true);
		for(auto& sqp: searches)
			sqp.second->searchAlternates();
	} else {
		//use an alternative matching, choose random items to search for
		aBundle->setSimpleMatching(false);
		int k = 0;
		while (k < 5) {
			auto pos = searches.begin();
			auto rand = Util::rand(searches.size());
			advance(pos, rand);
			//LogManager::getInstance()->message("QueueManager::searchBundle, ALT searchString: " + pos->second);
			pos->second->searchAlternates();
			searches.erase(pos);
			k++;
		}
	}

	int searchCount = (int)searches.size() <= 4 ? (int)searches.size() : 4;
	if (manual) {
		LogManager::getInstance()->message(STRING_F(BUNDLE_ALT_SEARCH, aBundle->getName().c_str() % searchCount), LogManager::LOG_INFO);
	} else if(SETTING(REPORT_ALTERNATES)) {
		//if (aBundle->getSimpleMatching()) {
			if (aBundle->isRecent()) {
				LogManager::getInstance()->message(str(boost::format(STRING(BUNDLE_ALT_SEARCH_RECENT) + 
				" " + (STRING(NEXT_RECENT_SEARCH_IN))) % 
					aBundle->getName().c_str() % 
					searchCount % 
					nextSearch), LogManager::LOG_INFO);
			} else {
				LogManager::getInstance()->message(str(boost::format(STRING(BUNDLE_ALT_SEARCH) + 
					" " + (STRING(NEXT_SEARCH_IN))) % 
					aBundle->getName().c_str() % 
					searchCount % 
					nextSearch), LogManager::LOG_INFO);
			}
		/*} else {
			if (!aBundle->isRecent()) {
				LogManager::getInstance()->message(STRING(ALTERNATES_SEND) + " " + aBundle->getName() + ", not using partial lists, next search in " + Util::toString(nextSearch) + " minutes", LogManager::LOG_INFO);
			} else {
				LogManager::getInstance()->message(STRING(ALTERNATES_SEND) + " " + aBundle->getName() + ", not using partial lists, next recent search in " + Util::toString(nextSearch) + " minutes", LogManager::LOG_INFO);
			}
		}*/
	}
}

void QueueManager::onUseSeqOrder(BundlePtr& b) {
	if (b) {
		WLock l (cs);
		b->setSeqOrder(true);
		auto ql = b->getQueueItems();
		for (auto& q: ql) {
			if (q->getPriority() != QueueItem::PAUSED) {
				userQueue.removeQI(q, false, false);
				userQueue.addQI(q, true);
			}
		}
	}
}

} // namespace dcpp