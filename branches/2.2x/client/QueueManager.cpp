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

#include "stdinc.h"
#include "QueueManager.h"

#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm/for_each.hpp>

#include "ClientManager.h"
#include "ConnectionManager.h"
#include "DirectoryListing.h"
#include "Download.h"
#include "DownloadManager.h"
#include "HashManager.h"
#include "LogManager.h"
#include "ResourceManager.h"
#include "SearchManager.h"
#include "ShareManager.h"
#include "SimpleXML.h"
#include "StringTokenizer.h"
#include "Transfer.h"
#include "UploadManager.h"
#include "UserConnection.h"
#include "version.h"
#include "Wildcards.h"
#include "SearchResult.h"
#include "SharedFileStream.h"
#include "MerkleCheckOutputStream.h"

#include <limits>

#if !defined(_WIN32) && !defined(PATH_MAX) // Extra PATH_MAX check for Mac OS X
#include <sys/syslimits.h>
#endif

#ifdef ff
#undef ff
#endif

namespace dcpp {

using boost::adaptors::map_values;
using boost::range::for_each;

class DirectoryItem {
public:
	DirectoryItem() : priority(QueueItem::DEFAULT) { }
	DirectoryItem(const UserPtr& aUser, const string& aName, const string& aTarget, 
		QueueItem::Priority p) : name(aName), target(aTarget), priority(p), user(aUser) { }
	~DirectoryItem() { }
	
	UserPtr& getUser() { return user; }
	void setUser(const UserPtr& aUser) { user = aUser; }
	
	GETSET(string, name, Name);
	GETSET(string, target, Target);
	GETSET(QueueItem::Priority, priority, Priority);
private:
	UserPtr user;
};

QueueManager::FileQueue::~FileQueue() {
	for(auto i = queue.begin(); i != queue.end(); ++i)
		i->second->dec();
}

QueueItem* QueueManager::FileQueue::add(const string& aTarget, int64_t aSize, 
						  Flags::MaskType aFlags, QueueItem::Priority p, const string& aTempTarget, 
						  time_t aAdded, const TTHValue& root) throw(QueueException, FileException)
{
//remember the default state so high_prio files are matched
//even if priority is set with priority by size setting
bool Default = false;

if(p == QueueItem::DEFAULT)
Default = true;

	if(p == QueueItem::DEFAULT) {
		if(aSize <= SETTING(PRIO_HIGHEST_SIZE)*1024) {
			p = QueueItem::HIGHEST;
		} else if(aSize <= SETTING(PRIO_HIGH_SIZE)*1024) {
			p = QueueItem::HIGH;
		} else if(aSize <= SETTING(PRIO_NORMAL_SIZE)*1024) {
			p = QueueItem::NORMAL;
		} else if(aSize <= SETTING(PRIO_LOW_SIZE)*1024) {
			p = QueueItem::LOW;
		} else if(SETTING(PRIO_LOWEST)) {
			p = QueueItem::LOWEST;
		}
	}

	if((p != QueueItem::HIGHEST) && ( Default )){
		if(!SETTING(HIGH_PRIO_FILES).empty()){
		int pos = aTarget.rfind("\\")+1;
		
		if(BOOLSETTING(HIGHEST_PRIORITY_USE_REGEXP)){
			/*PME regexp;
			regexp.Init(Text::utf8ToAcp(SETTING(HIGH_PRIO_FILES)));
			if((regexp.IsValid()) && (regexp.match(Text::utf8ToAcp(aTarget.substr(pos))))) {
				p = QueueItem::HIGH;
			}*/
			string str1 = SETTING(HIGH_PRIO_FILES);
			string str2 = aTarget.substr(pos);
			try {
				boost::regex reg(str1);
				if(boost::regex_search(str2.begin(), str2.end(), reg)){
					p = QueueItem::HIGH;
					Default = false;
				};
			} catch(...) {
			}
		}else{
			if(Wildcard::patternMatch(Text::utf8ToAcp(aTarget.substr(pos)), Text::utf8ToAcp(SETTING(HIGH_PRIO_FILES)), '|')) {
				p = QueueItem::HIGH;
				Default = false;
			}
		}
	}
}



	QueueItem* qi = new QueueItem(aTarget, aSize, p, aFlags, aAdded, root);

	if(qi->isSet(QueueItem::FLAG_USER_LIST)) {
		qi->setPriority(QueueItem::HIGHEST);
	} else {
		qi->setMaxSegments(getMaxSegments(qi->getSize()));
		
		if(p == QueueItem::DEFAULT) {
			if(BOOLSETTING(AUTO_PRIORITY_DEFAULT)) {
				qi->setAutoPriority(true);
				qi->setPriority(QueueItem::LOW);
			} else {
				qi->setPriority(QueueItem::NORMAL);
			}
		}
	}

	qi->setTempTarget(aTempTarget);
	if(!Util::fileExists(aTempTarget) && Util::fileExists(aTempTarget + ".antifrag")) {
		// load old antifrag file
		File::renameFile(aTempTarget + ".antifrag", qi->getTempTarget());
    }
	        	
	dcassert(find(aTarget) == NULL);
	add(qi);
	return qi;
}

void QueueManager::FileQueue::add(QueueItem* qi) {
	if(lastInsert == queue.end())
		lastInsert = queue.insert(make_pair(const_cast<string*>(&qi->getTarget()), qi)).first;
	else
		lastInsert = queue.insert(lastInsert, make_pair(const_cast<string*>(&qi->getTarget()), qi));

//	queue.insert(make_pair(const_cast<string*>(&qi->getTarget()), qi));
	//string dir = Util::getDir(qi->getTarget(), true, true);
	//releaselist.insert(make_pair(const_cast<string*>(&qi->getTarget()), qi->getTTH()));
}

void QueueManager::FileQueue::remove(QueueItem* qi) {
	if(lastInsert != queue.end() && stricmp(*lastInsert->first, qi->getTarget()) == 0)
		++lastInsert;
	queue.erase(const_cast<string*>(&qi->getTarget()));
	//delete qi;

	//queue.erase(const_cast<string*>(&qi->getTarget()));
	qi->dec();
}

QueueItem* QueueManager::FileQueue::find(const string& target) {
	auto i = queue.find(const_cast<string*>(&target));
	return (i == queue.end()) ? NULL : i->second;
}

void QueueManager::FileQueue::find(QueueItemList& sl, int64_t aSize, const string& suffix) {
	for(auto i = queue.begin(); i != queue.end(); ++i) {
		if(i->second->getSize() == aSize) {
			const string& t = i->second->getTarget();
			if(suffix.empty() || (suffix.length() < t.length() &&
				stricmp(suffix.c_str(), t.c_str() + (t.length() - suffix.length())) == 0) )
				sl.push_back(i->second);
		}
	}
}

QueueManager::QueueItemList QueueManager::FileQueue::find(const TTHValue& tth) {
		QueueItemList ql;
	for(auto i = queue.begin(); i != queue.end(); ++i) {
		QueueItem* qi = i->second;
		if(qi->getTTH() == tth) {
			ql.push_back(qi);
		}
	}
	return ql;
}

static QueueItem* findCandidate(QueueItem* cand, QueueItem::StringMap::iterator start, QueueItem::StringMap::iterator end, const StringList& recent) {

	for(auto i = start; i != end; ++i) {
		QueueItem* q = i->second;

		// We prefer to search for things that are not running...
		if((cand != NULL) && q->isRunning())
			continue;
		// No finished files
		if(q->isFinished())
			continue;
		// No user lists
		if(q->isSet(QueueItem::FLAG_USER_LIST))
			continue;
        // No paused downloads
		if(q->getPriority() == QueueItem::PAUSED)
			continue;
		// No files that already have more than AUTO_SEARCH_LIMIT online sources
		if(q->countOnlineUsers() >= (size_t)SETTING(AUTO_SEARCH_LIMIT))
			continue;
		// Did we search for it recently?
        if(find(recent.begin(), recent.end(), q->getTarget()) != recent.end())
			continue;

		cand = q;

		if(cand->isWaiting())
			break;
	}
	return cand;
}

QueueItem* QueueManager::FileQueue::findAutoSearch(StringList& recent){
	// We pick a start position at random, hoping that we will find something to search for...
	 auto start = (QueueItem::StringMap::size_type)Util::rand((uint32_t)queue.size());

	auto i = queue.begin();
	advance(i, start);

	QueueItem* cand = findCandidate(NULL, i, queue.end(), recent);

	if(cand == NULL || cand->isRunning()) {
		cand = findCandidate(cand, queue.begin(), i, recent);  
	}


	return cand;
}

void QueueManager::FileQueue::move(QueueItem* qi, const string& aTarget) {
	if(lastInsert != queue.end() && stricmp(*lastInsert->first, qi->getTarget()) == 0)
		lastInsert = queue.end();
	queue.erase(const_cast<string*>(&qi->getTarget()));
	qi->setTarget(aTarget);
	add(qi);
}

void QueueManager::UserQueue::add(QueueItem* qi) {
	for(QueueItem::SourceConstIter i = qi->getSources().begin(); i != qi->getSources().end(); ++i) {
		add(qi, i->getUser());
	}
}

void QueueManager::UserQueue::add(QueueItem* qi, const UserPtr& aUser) {
	auto& l = userQueue[qi->getPriority()][aUser];

	if(qi->getDownloadedBytes() > 0 ) {
		l.push_front(qi);
	} else {
		l.push_back(qi);
	}
}

QueueItem* QueueManager::UserQueue::getNext(const UserPtr& aUser, QueueItem::Priority minPrio, int64_t wantedSize, int64_t lastSpeed, bool allowRemove, bool smallSlot) {
	int p = QueueItem::LAST - 1;
	lastError = Util::emptyString;

	do {
		auto i = userQueue[p].find(aUser);
		if(i != userQueue[p].end()) {
			dcassert(!i->second.empty());
			for(auto j = i->second.begin(); j != i->second.end(); ++j) {
				QueueItem* qi = *j;
				
				/*user is not a source anymore?? removed but still in userqueue?
				item just finished, dont go further?, pick another one.*/
				if(!qi->isSource(aUser) || qi->isFinished()) 
					continue; 

				QueueItem::SourceConstIter source = qi->getSource(aUser);

				if(smallSlot && !qi->isSet(QueueItem::FLAG_PARTIAL_LIST) && qi->getSize() > 65792) {
					//don't even think of stealing our priority channel
					continue;
				}

				//don't try to get a file that we are currently downloading
				if(!qi->isWaiting()) {
					bool found=false;
					for(DownloadList::iterator i = qi->getDownloads().begin(); i != qi->getDownloads().end(); ++i) {
						//LogManager::getInstance()->message("USER: " + aUser->getCID().toBase32() + " comparing: " + Util::toString(ClientManager::getInstance()->getNicks((*i)->getHintedUser())));
						if((*i)->getUser() == aUser) {
							//LogManager::getInstance()->message("FOUND USER: " + aUser->getCID().toBase32() + " comparing: " + Util::toString(ClientManager::getInstance()->getNicks((*i)->getHintedUser())));
							found=true;
							break;
						}
					}
					if (found) {
						lastError = STRING(NO_FILES_AVAILABLE);
						continue;
					}
				}

				if(source->isSet(QueueItem::Source::FLAG_PARTIAL)) {
					// check partial source
					int64_t blockSize = HashManager::getInstance()->getBlockSize(qi->getTTH());
					if(blockSize == 0)
						blockSize = qi->getSize();
					
					Segment segment = qi->getNextSegment(blockSize, wantedSize, lastSpeed, source->getPartialSource());
					if(allowRemove && segment.getStart() != -1 && segment.getSize() == 0) {
						// no other partial chunk from this user, remove him from queue
						remove(qi, aUser);
						qi->removeSource(aUser, QueueItem::Source::FLAG_NO_NEED_PARTS);
						lastError = STRING(NO_NEEDED_PART);
						p++;
						break;
					}
				}

				if(qi->isWaiting()) {
					return qi;
				}
				
				// No segmented downloading when getting the tree
				if(qi->getDownloads()[0]->getType() == Transfer::TYPE_TREE) {
					continue;
				}
				if(!qi->isSet(QueueItem::FLAG_USER_LIST)) {
					int64_t blockSize = HashManager::getInstance()->getBlockSize(qi->getTTH());
					if(blockSize == 0)
						blockSize = qi->getSize();

					Segment segment = qi->getNextSegment(blockSize, wantedSize, lastSpeed, source->getPartialSource());
					if(segment.getSize() == 0) {
						lastError = (segment.getStart() == -1 || qi->getSize() < (SETTING(MIN_SEGMENT_SIZE)*1024)) ? STRING(NO_FILES_AVAILABLE) : STRING(NO_FREE_BLOCK);
						//LogManager::getInstance()->message("NO SEGMENT: " + aUser->getCID().toBase32());
						dcdebug("No segment for %s in %s, block " I64_FMT "\n", aUser->getCID().toBase32().c_str(), qi->getTarget().c_str(), blockSize);
						continue;
					}
				}
				return qi;
			}
		}
		p--;
	} while(p >= minPrio);

	return NULL;
}

void QueueManager::UserQueue::addDownload(QueueItem* qi, Download* d) {
	qi->getDownloads().push_back(d);

	// Only one download per user...
	//dcassert(running.find(d->getUser()) == running.end());
	running[d->getUser()] = qi;
}

void QueueManager::UserQueue::removeDownload(QueueItem* qi, const UserPtr& user) {
	running.erase(user);

	for(DownloadList::iterator i = qi->getDownloads().begin(); i != qi->getDownloads().end(); ++i) {
		if((*i)->getUser() == user) {
			qi->getDownloads().erase(i);
			break;
		}
	}
}

void QueueManager::UserQueue::setPriority(QueueItem* qi, QueueItem::Priority p) {
	remove(qi, false);
	qi->setPriority(p);
	add(qi);
}

QueueItem* QueueManager::UserQueue::getRunning(const UserPtr& aUser) {
	auto i = running.find(aUser);
	return (i == running.end()) ? 0 : i->second;
}

void QueueManager::UserQueue::remove(QueueItem* qi, bool removeRunning) {
	for(QueueItem::SourceConstIter i = qi->getSources().begin(); i != qi->getSources().end(); ++i) {
		remove(qi, i->getUser(), removeRunning);
	}
}

void QueueManager::UserQueue::remove(QueueItem* qi, const UserPtr& aUser, bool removeRunning) {
	if(removeRunning && qi == getRunning(aUser)) {
		removeDownload(qi, aUser);
	}

	dcassert(qi->isSource(aUser));
	auto& ulm = userQueue[qi->getPriority()];
	auto j = ulm.find(aUser);
	dcassert(j != ulm.end());
	auto& l = j->second;
	auto i = find(l.begin(), l.end(), qi);
	dcassert(i != l.end());
	l.erase(i);

	if(l.empty()) {
		ulm.erase(j);
	}
}

void QueueManager::FileMover::moveFile(const string& source, const string& target) {
	Lock l(cs);
	files.push_back(make_pair(source, target));
	if(!active) {
		active = true;
		start();
	}
}

int QueueManager::FileMover::run() {
	for(;;) {
		FilePair next;
		{
			Lock l(cs);
			if(files.empty()) {
				active = false;
				return 0;
			}
			next = files.back();
			files.pop_back();
		}
		moveFile_(next.first, next.second);
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
			StringIter i = files.begin();
			if(i == files.end()) {
				active = false;
				return 0;
			}
			file = *i;
			files.erase(i);
		}

		QueueItem* q;
		int64_t tempSize;
		TTHValue tth;

		{
			Lock l(qm->cs);

			q = qm->fileQueue.find(file);
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
			Lock l(qm->cs);

			// get q again in case it has been (re)moved
			q = qm->fileQueue.find(file);
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

		//Merklecheck
		int64_t startPos=0;
		DummyOutputStream dummy;
		int64_t blockSize = tt.getBlockSize();
		bool hasBadBlocks = false;

		vector<uint8_t> buf((size_t)min((int64_t)1024*1024, blockSize));

		typedef pair<int64_t, int64_t> SizePair;
		typedef vector<SizePair> Sizes;
		Sizes sizes;

		{
			File inFile(tempTarget, File::READ, File::OPEN);

			while(startPos < tempSize) {
				try {
					MerkleCheckOutputStream<TigerTree, false> check(tt, &dummy, startPos);

					inFile.setPos(startPos);
					int64_t bytesLeft = min((tempSize - startPos),blockSize); //Take care of the last incomplete block
					int64_t segmentSize = bytesLeft;
					while(bytesLeft > 0) {
						size_t n = (size_t)min((int64_t)buf.size(), bytesLeft);
						size_t nr = inFile.read(&buf[0], n);
						check.write(&buf[0], nr);
						bytesLeft -= nr;
						if(bytesLeft > 0 && nr == 0) {
							// Huh??
							throw Exception();
						}
					}
					check.flush();

					sizes.push_back(make_pair(startPos, segmentSize));
				} catch(const Exception&) {
					hasBadBlocks = true;
					dcdebug("Found bad block at " I64_FMT "\n", startPos);
				}
				startPos += blockSize;
			}
		}

		Lock l(qm->cs);

		// get q again in case it has been (re)moved
		q = qm->fileQueue.find(file);
		if(!q)
			continue;

		//If no bad blocks then the file probably got stuck in the temp folder for some reason
		if(!hasBadBlocks) {
			qm->moveStuckFile(q);
			continue;
		}

		for(Sizes::const_iterator i = sizes.begin(); i != sizes.end(); ++i)
			q->addSegment(Segment(i->first, i->second));

		qm->rechecked(q);
	}
	return 0;
}

QueueManager::QueueManager() : 
	lastSave(0), 
	queueFile(Util::getPath(Util::PATH_USER_CONFIG) + "Queue.xml"),
	rechecker(this),
	dirty(true), 
	nextSearch(0)
{ 
	TimerManager::getInstance()->addListener(this); 
	SearchManager::getInstance()->addListener(this);
	ClientManager::getInstance()->addListener(this);

	regexp.Init("[Rr0-9][Aa0-9][Rr0-9]");

	File::ensureDirectory(Util::getListPath());
}

QueueManager::~QueueManager() noexcept { 
	SearchManager::getInstance()->removeListener(this);
	TimerManager::getInstance()->removeListener(this); 
	ClientManager::getInstance()->removeListener(this);

	saveQueue();

	if(!BOOLSETTING(KEEP_LISTS)) {
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

bool QueueManager::getTTH(const string& name, TTHValue& tth) noexcept {
	Lock l(cs);
	QueueItem* qi = fileQueue.find(name);
	if(qi) {
		tth = qi->getTTH();
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
    uint16_t	udpPort;
};

void QueueManager::on(TimerManagerListener::Minute, uint64_t aTick) noexcept {
	string searchString;
	vector<const PartsInfoReqParam*> params;

	{
		Lock l(cs);

		//find max 10 pfs sources to exchange parts
		//the source basis interval is 5 minutes
		PFSSourceList sl;
		fileQueue.findPFSSources(sl);

		for(PFSSourceList::const_iterator i = sl.begin(); i != sl.end(); i++){
			QueueItem::PartialSource::Ptr source = (*i->first).getPartialSource();
			const QueueItem* qi = i->second;

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

			source->setPendingQueryCount(source->getPendingQueryCount() + 1);
			source->setNextQueryTime(aTick + 300000);		// 5 minutes

		}

		if(BOOLSETTING(AUTO_SEARCH) && (aTick >= nextSearch) && (fileQueue.getSize() > 0)) {
			// We keep 30 recent searches to avoid duplicate searches
			while((recent.size() >= fileQueue.getSize()) || (recent.size() > 30)) {
				recent.erase(recent.begin());
			}

			QueueItem* qi;
			qi = fileQueue.findAutoSearch(recent);
			//while((qi = fileQueue.findAutoSearch(recent)) == NULL && !recent.empty()) {
				//recent.pop_front();
			//}
			if(qi != NULL) {
				searchString = qi->getTTH().toBase32();
				recent.push_back(qi->getTarget());
				if(BOOLSETTING(REPORT_ALTERNATES))
					LogManager::getInstance()->message(STRING(ALTERNATES_SEND) + " " + Util::getFileName(qi->getTargetFileName()));		
			}
			nextSearch = aTick + (SETTING(SEARCH_TIME) * 60000); //this is also the time for next check, set it here so we dont need to start checking every minute
		}
	}

	// Request parts info from partial file sharing sources
	for(vector<const PartsInfoReqParam*>::const_iterator i = params.begin(); i != params.end(); i++){
		const PartsInfoReqParam* param = *i;
		dcassert(param->udpPort > 0);
		
		try {
			AdcCommand cmd = SearchManager::getInstance()->toPSR(true, param->myNick, param->hubIpPort, param->tth, param->parts);
			Socket s;
			s.writeTo(param->ip, param->udpPort, cmd.toString(ClientManager::getInstance()->getMyCID()));
		} catch(...) {
			dcdebug("Partial search caught error\n");		
		}
		
		delete param;
	}

	if(!searchString.empty()) {
		SearchManager::getInstance()->search(searchString, 0, SearchManager::TYPE_TTH, SearchManager::SIZE_DONTCARE, "auto");
	}
}
 

void QueueManager::addList(const HintedUser& aUser, Flags::MaskType aFlags, const string& aInitialDir /* = Util::emptyString */) throw(QueueException, FileException) {
	add(aInitialDir, -1, TTHValue(), aUser, (Flags::MaskType)(QueueItem::FLAG_USER_LIST | aFlags));
}

string QueueManager::getListPath(const HintedUser& user) {
	StringList nicks = ClientManager::getInstance()->getNicks(user);
	string nick = nicks.empty() ? Util::emptyString : Util::cleanPathChars(nicks[0]) + ".";
	return checkTarget(Util::getListPath() + nick + user.user->getCID().toBase32(), /*checkExistence*/ false);
}

//fuldc ftp logger support
void QueueManager::fileEvent(const string& tgt, bool file /*false*/) {
	string target = tgt;
if(file) {
		if(File::getSize(target) != -1) {
			StringPair sp = SettingsManager::getInstance()->getFileEvent(SettingsManager::ON_FILE_COMPLETE);
			if(sp.first.length() > 0) {
				STARTUPINFO si = { sizeof(si), 0 };
				PROCESS_INFORMATION pi = { 0 };
				StringMap params;
				params["file"] = target;
				wstring cmdLine = Text::toT(Util::formatParams(sp.second, params, false));
				wstring cmd = Text::toT(sp.first);

				AutoArray<TCHAR> cmdLineBuf(cmdLine.length() + 1);
				_tcscpy(cmdLineBuf, cmdLine.c_str());

				AutoArray<TCHAR> cmdBuf(cmd.length() + 1);
				_tcscpy(cmdBuf, cmd.c_str());

				if(::CreateProcess(cmdBuf, cmdLineBuf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
					::CloseHandle(pi.hThread);
					::CloseHandle(pi.hProcess);
				}
			}
		}
} else {
	if(File::createDirectory(target)) {
		StringPair sp = SettingsManager::getInstance()->getFileEvent(SettingsManager::ON_DIR_CREATED);
		if(sp.first.length() > 0) {
			STARTUPINFO si = { sizeof(si), 0 };
			PROCESS_INFORMATION pi = { 0 };
			StringMap params;
			params["dir"] = target;
			wstring cmdLine = Text::toT(Util::formatParams(sp.second, params, true));
			wstring cmd = Text::toT(sp.first);

			AutoArray<TCHAR> cmdLineBuf(cmdLine.length() + 1);
			_tcscpy(cmdLineBuf, cmdLine.c_str());

			AutoArray<TCHAR> cmdBuf(cmd.length() + 1);
			_tcscpy(cmdBuf, cmd.c_str());

			if(::CreateProcess(cmdBuf, cmdLineBuf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
				//wait for the process to finish executing
				if(WAIT_OBJECT_0 == WaitForSingleObject(pi.hProcess, INFINITE)) {
					DWORD code = 0;
					//retrieve the error code to check if we should stop this download.
					if(0 != GetExitCodeProcess(pi.hProcess, &code)) {
						if(code != 0) { //assume 0 is the only valid return code, everything else is an error
							string::size_type end = target.find_last_of("\\/");
							if(end != string::npos) {
								tstring tmp = Text::toT(target.substr(0, end));
								RemoveDirectory(tmp.c_str());

								//the directory we removed might be a sub directory of
								//the real one, check to see if that's the case.
								end = tmp.find_last_of(_T("\\/"));
								if(end != string::npos) {
									tstring dir = tmp.substr(end+1);
									if( strnicmp(dir, _T("sample"), 6) == 0 ||
										strnicmp(dir, _T("subs"), 4) == 0 ||
										strnicmp(dir, _T("cover"), 5) == 0 ||
										strnicmp(dir, _T("cd"), 2) == 0) {
											RemoveDirectory(tmp.substr(0, end).c_str());
									}
								}
								
								::CloseHandle(pi.hThread);
								::CloseHandle(pi.hProcess);

								throw QueueException("An external sfv tool stopped the download of this file");
							}
						}
					}
				}
				
				::CloseHandle(pi.hThread);
				::CloseHandle(pi.hProcess);
				}
			}
		}
	}
}
void QueueManager::add(const string& aTarget, int64_t aSize, const TTHValue& root, const HintedUser& aUser,
					   Flags::MaskType aFlags /* = 0 */, bool addBad /* = true */) throw(QueueException, FileException)
{
	bool wantConnection = true;
	bool newItem = false;

	// Check that we're not downloading from ourselves...
	if(aUser == ClientManager::getInstance()->getMe()) {
		throw QueueException(STRING(NO_DOWNLOADS_FROM_SELF));
	}

	// Check if we're not downloading something already in our share
	if (BOOLSETTING(DONT_DL_ALREADY_SHARED)){
		if (!(aFlags & QueueItem::FLAG_CLIENT_VIEW) && !(aFlags & QueueItem::FLAG_USER_LIST) && !(aFlags & QueueItem::FLAG_PARTIAL_LIST)) {
			if(ShareManager::getInstance()->isTTHShared(root)){
				LogManager::getInstance()->message(STRING(FILE_ALREADY_SHARED) + " " + aTarget );
				throw QueueException(STRING(TTH_ALREADY_SHARED));
			}
		}
	}
    
	string target;
	string tempTarget;
	if((aFlags & QueueItem::FLAG_USER_LIST) == QueueItem::FLAG_USER_LIST) {
		if((aFlags & QueueItem::FLAG_PARTIAL_LIST) && !aTarget.empty()) {
			StringList nicks = ClientManager::getInstance()->getNicks(aUser);
			string nick = nicks.empty() ? Util::emptyString : Util::cleanPathChars(nicks[0]) + ".";
			target = Util::getListPath() + nick + Util::toString(Util::rand());
		} else {
			target = getListPath(aUser);
		}
		tempTarget = aTarget;
	} else {
		if(BOOLSETTING(USE_FTP_LOGGER)) {
			target = checkTarget(aTarget, /*checkExistence*/ false);
			fileEvent(target);
		} else
			target = checkTarget(aTarget, /*checkExistence*/ true);
	
	}


	// Check if it's a zero-byte file, if so, create and return...
	if(aSize == 0) {
		if(!BOOLSETTING(SKIP_ZERO_BYTE)) {
			File::ensureDirectory(target);
			File f(target, File::WRITE, File::CREATE);
		}
		return;
	}
	

	
	if( !SETTING(SKIPLIST_DOWNLOAD).empty() ){
		int pos = aTarget.rfind("\\")+1;
		
		if(BOOLSETTING(DOWNLOAD_SKIPLIST_USE_REGEXP)){
			string str1 = SETTING(SKIPLIST_DOWNLOAD);
			string str2 = aTarget.substr(pos);
			try {
				boost::regex reg(str1);
				if(boost::regex_search(str2.begin(), str2.end(), reg)){
					return;
				};
			} catch(...) {
			}
			/*PME regexp;
			regexp.Init(Text::utf8ToAcp(SETTING(SKIPLIST_DOWNLOAD)));
			if((regexp.IsValid()) && (regexp.match(Text::utf8ToAcp(aTarget.substr(pos))))) {
				return;
			}*/
		}else{
			if(Wildcard::patternMatch(Text::utf8ToAcp(aTarget.substr(pos)), Text::utf8ToAcp(SETTING(SKIPLIST_DOWNLOAD)), '|') ){
				return;
			}
		}
	}

	{
		Lock l(cs);

		// This will be pretty slow on large queues...
		if(BOOLSETTING(DONT_DL_ALREADY_QUEUED) && !(aFlags & QueueItem::FLAG_USER_LIST)) {
			QueueItemList ql = fileQueue.find(root);
			if (ql.size() > 0) {
				// Found one or more existing queue items, lets see if we can add the source to them
				bool sourceAdded = false;
				for(auto i = ql.begin(); i != ql.end(); ++i) {
					if(!(*i)->isSource(aUser)) {
						try {
							wantConnection = addSource(*i, aUser, addBad ? QueueItem::Source::FLAG_MASK : 0);
							sourceAdded = true;
						} catch(const Exception&) {
						//...
						}
					}
				}

				if(!sourceAdded) {
					LogManager::getInstance()->message(STRING(FILE_WITH_SAME_TTH) + " " + aTarget );
					throw QueueException(STRING(FILE_WITH_SAME_TTH));
				}
				goto connect;
			}
		}

		QueueItem* q = fileQueue.find(target);	
		if(q == NULL) {
			q = fileQueue.add(target, aSize, aFlags, QueueItem::DEFAULT, tempTarget, GET_TIME(), root);
			
			if(!(aFlags & QueueItem::FLAG_USER_LIST))
					updateDirSize(q->getTarget(), q->getSize(), true);
			
			fire(QueueManagerListener::Added(), q);

			newItem = true;
		} else {
			if(q->getSize() != aSize) {
				throw QueueException(STRING(FILE_WITH_DIFFERENT_SIZE));
			}
			if(!(root == q->getTTH())) {
				throw QueueException(STRING(FILE_WITH_DIFFERENT_TTH));
			}

			if(q->isFinished()) {
				throw QueueException(STRING(FILE_ALREADY_FINISHED));
			}

			q->setFlag(aFlags);
		}
		try {
		wantConnection = aUser.user && addSource(q, aUser, (Flags::MaskType)(addBad ? QueueItem::Source::FLAG_MASK : 0));
		} catch(const Exception&) {
			//...
			}
		setDirty();
	}
connect:
	bool smallSlot=false;
	if (newItem && (((aFlags & QueueItem::FLAG_PARTIAL_LIST) || (aSize <= 65792 && !(aFlags & QueueItem::FLAG_USER_LIST))) && (aFlags & QueueItem::FLAG_CLIENT_VIEW))) {
			smallSlot=true;
	}
	if(!aUser.user) //atleast magnet links can cause this to happen.
		return;

	if(wantConnection && aUser.user->isOnline() || aUser.user->isOnline() && smallSlot) {
		ConnectionManager::getInstance()->getDownloadConnection(aUser, smallSlot);
	}

}

void QueueManager::readd(const string& target, const HintedUser& aUser) throw(QueueException) {
	bool wantConnection = false;
	{
		Lock l(cs);
		QueueItem* q = fileQueue.find(target);
		if(q && q->isBadSource(aUser)) {
			wantConnection = addSource(q, aUser, QueueItem::Source::FLAG_MASK);
		}
	}
	if(wantConnection && aUser.user->isOnline())
		ConnectionManager::getInstance()->getDownloadConnection(aUser);
}

void QueueManager::setDirty() {
	if(!dirty) {
		dirty = true;
		lastSave = GET_TICK();
	}
}

string QueueManager::checkTarget(const string& aTarget, bool checkExistence) throw(QueueException, FileException) {
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
	if(checkExistence && File::getSize(target) != -1) {
		throw FileException(target + ": " + STRING(TARGET_FILE_EXISTS));
	}
	return target;	
}

/** Add a source to an existing queue item */
bool QueueManager::addSource(QueueItem* qi, const HintedUser& aUser, Flags::MaskType addBad) throw(QueueException, FileException) {
	
	if (!aUser.user) //atleast magnet links can cause this to happen.
	throw QueueException("Can't find Source user to add For Target: " + Util::getFileName(qi->getTarget()));

	if(qi->isFinished()) //no need to add source to finished item.
		throw QueueException("Already Finished: " + Util::getFileName(qi->getTarget()));
	
	bool wantConnection = (qi->getPriority() != QueueItem::PAUSED) && !userQueue.getRunning(aUser);

	if(qi->isSource(aUser)) {
		if(qi->isSet(QueueItem::FLAG_USER_LIST)) {
			return wantConnection;
		}
		throw QueueException(STRING(DUPLICATE_SOURCE) + ": " + Util::getFileName(qi->getTarget()));
	}

	if(qi->isBadSourceExcept(aUser, addBad)) {
		throw QueueException(STRING(DUPLICATE_SOURCE) + ": " + Util::getFileName(qi->getTarget()));
	}
	{ 
		//Lock l(cs);

		qi->addSource(aUser);


	if(qi->isFinished()) {
		wantConnection = false;
	} else {
		userQueue.add(qi, aUser);
		if ((!SETTING(SOURCEFILE).empty()) && (!BOOLSETTING(SOUNDS_DISABLED)))
			PlaySound(Text::toT(SETTING(SOURCEFILE)).c_str(), NULL, SND_FILENAME | SND_ASYNC);
		}
	}

	fire(QueueManagerListener::SourcesUpdated(), qi);
	setDirty();

	return wantConnection;
	
}

void QueueManager::addDirectory(const string& aDir, const HintedUser& aUser, const string& aTarget, QueueItem::Priority p /* = QueueItem::DEFAULT */, bool useFullList) noexcept {
	bool adc=true;
	if (aUser.user->isSet(User::NMDC) || useFullList)
		adc=false;
	bool needList;
	{
		Lock l(cs);
		
		auto dp = directories.equal_range(aUser);
		
		for(auto i = dp.first; i != dp.second; ++i) {
			if(stricmp(aTarget.c_str(), i->second->getName().c_str()) == 0)
				return;
		}
		
		// Unique directory, fine...
		directories.insert(make_pair(aUser, new DirectoryItem(aUser, aDir, aTarget, p)));
		needList = (dp.first == dp.second);
		setDirty();
	}
	if(needList || adc) {
		try {
			if (!adc) {
				addList(aUser, QueueItem::FLAG_DIRECTORY_DOWNLOAD, aDir);
			} else {
				addList(aUser, QueueItem::FLAG_DIRECTORY_DOWNLOAD | QueueItem::FLAG_PARTIAL_LIST | QueueItem::FLAG_RECURSIVE_LIST, aDir);
			}
		} catch(const Exception&) {
			// Ignore, we don't really care...
		}
	}
}

QueueItem::Priority QueueManager::hasDownload(const UserPtr& aUser, bool smallSlot) noexcept {
	Lock l(cs);
	QueueItem* qi = userQueue.getNext(aUser, QueueItem::LOWEST, 0, 0, false, smallSlot);
	if(!qi) {
		return QueueItem::PAUSED;
	}
	return qi->getPriority();
}

namespace {

typedef std::unordered_map<TTHValue, const DirectoryListing::File*> TTHMap;

// *** WARNING *** 
// Lock(cs) makes sure that there's only one thread accessing this
static TTHMap tthMap;

void buildMap(const DirectoryListing::Directory* dir) noexcept {
	for(DirectoryListing::Directory::List::const_iterator j = dir->directories.begin(); j != dir->directories.end(); ++j) {
		if(!(*j)->getAdls())
			buildMap(*j);
	}

	for(DirectoryListing::File::List::const_iterator i = dir->files.begin(); i != dir->files.end(); ++i) {
		const DirectoryListing::File* df = *i;
		tthMap.insert(make_pair(df->getTTH(), df));
	}
}
}

int QueueManager::matchListing(const DirectoryListing& dl, bool partialList) noexcept {
	int matches = 0;
	bool wantConnection = false;
	{
		Lock l(cs);
		tthMap.clear();
		buildMap(dl.getRoot());

		if (partialList) {
			//LogManager::getInstance()->message("MATCHING PARTIAL LIST");
 			for (auto s = tthMap.begin(); s != tthMap.end(); ++s) {
				QueueItemList ql = fileQueue.find(s->first);
				if (!ql.empty()) {
					for (auto i = ql.begin(); i != ql.end(); ++i) {
						QueueItem* qi = (*i);
						if(qi->isFinished())
							continue;
						if(qi->isSet(QueueItem::FLAG_USER_LIST))
							continue;

						try {	 
							wantConnection = addSource(qi, dl.getHintedUser(), QueueItem::Source::FLAG_FILE_NOT_AVAILABLE);
						} catch(...) {
							// Ignore...
						}
						matches++;
					}
				}
			}
		} else {

		for(auto i = fileQueue.getQueue().begin(); i != fileQueue.getQueue().end(); ++i) {
			QueueItem* qi = i->second;
			if(qi->isFinished())
				continue;
			if(qi->isSet(QueueItem::FLAG_USER_LIST))
				continue;
			TTHMap::iterator j = tthMap.find(qi->getTTH());
			if(j != tthMap.end() && j->second->getSize() == qi->getSize()) {
				try {
					wantConnection = addSource(qi, dl.getHintedUser(), QueueItem::Source::FLAG_FILE_NOT_AVAILABLE);    
					} catch(const Exception&) {
					//...
					}
				matches++;
			}
		}
	}
	tthMap.clear();
	}
	if((matches > 0) && wantConnection)
		ConnectionManager::getInstance()->getDownloadConnection(dl.getHintedUser());
		return matches;
}

void QueueManager::move(const string& aSource, const string& aTarget) noexcept {
	string target = Util::validateFileName(aTarget);
	if(aSource == target)
		return;

	bool delSource = false;

	Lock l(cs);
	QueueItem* qs = fileQueue.find(aSource);
	if(qs) {
		// Don't move running downloads
		if(qs->isRunning()) {
			return;
		}
		// Don't move file lists
		if(qs->isSet(QueueItem::FLAG_USER_LIST))
			return;

		// Let's see if the target exists...then things get complicated...
		QueueItem* qt = fileQueue.find(target);
		if(qt == NULL || stricmp(aSource, target) == 0) {
			// Good, update the target and move in the queue...
			fire(QueueManagerListener::Moved(), qs, aSource);
			fileQueue.move(qs, target);
			updateDirSize(aSource, qs->getSize(), false);
			updateDirSize(target, qs->getSize(), true);
			fire(QueueManagerListener::Added(), qs);
			setDirty();
		} else {
			// Don't move to target of different size
			if(qs->getSize() != qt->getSize() || qs->getTTH() != qt->getTTH())
				return;

			for(QueueItem::SourceConstIter i = qs->getSources().begin(); i != qs->getSources().end(); ++i) {
				try {
					addSource(qt, i->getUser(), QueueItem::Source::FLAG_MASK);
				} catch(const Exception&) {
					//..
				}
			}
			delSource = true;
		}
	}

	if(delSource) {
		remove(aSource);
		updateDirSize(aSource, qs->getSize(), false);
	}
}

bool QueueManager::getQueueInfo(const UserPtr& aUser, string& aTarget, int64_t& aSize, int& aFlags) noexcept {
    Lock l(cs);
    QueueItem* qi = userQueue.getNext(aUser);
	if(qi == NULL)
		return false;

	aTarget = qi->getTarget();
	aSize = qi->getSize();
	aFlags = qi->getFlags();

	return true;
}

uint8_t QueueManager::FileQueue::getMaxSegments(int64_t filesize) const {
	uint8_t MaxSegments = 1;

	if(BOOLSETTING(SEGMENTS_MANUAL)) {
		MaxSegments = min((uint8_t)SETTING(NUMBER_OF_SEGMENTS), (uint8_t)10);
	} else {
		if((filesize >= 2*1048576) && (filesize < 15*1048576)) {
			MaxSegments = 2;
		} else if((filesize >= (int64_t)15*1048576) && (filesize < (int64_t)30*1048576)) {
			MaxSegments = 3;
		} else if((filesize >= (int64_t)30*1048576) && (filesize < (int64_t)60*1048576)) {
			MaxSegments = 4;
		} else if((filesize >= (int64_t)60*1048576) && (filesize < (int64_t)120*1048576)) {
			MaxSegments = 5;
		} else if((filesize >= (int64_t)120*1048576) && (filesize < (int64_t)240*1048576)) {
			MaxSegments = 6;
		} else if((filesize >= (int64_t)240*1048576) && (filesize < (int64_t)480*1048576)) {
			MaxSegments = 7;
		} else if((filesize >= (int64_t)480*1048576) && (filesize < (int64_t)960*1048576)) {
			MaxSegments = 8;
		} else if((filesize >= (int64_t)960*1048576) && (filesize < (int64_t)1920*1048576)) {
			MaxSegments = 9;
		} else if(filesize >= (int64_t)1920*1048576) {
			MaxSegments = 10;
		}
	}

#ifdef _DEBUG
	return 88;
#else
	return MaxSegments;
#endif
}

StringList QueueManager::getTargets(const TTHValue& tth) {
	Lock l(cs);
	QueueItemList ql = fileQueue.find(tth);
	StringList sl;
	for(auto i = ql.begin(); i != ql.end(); ++i) {
		sl.push_back((*i)->getTarget());
	}
	return sl;
}

Download* QueueManager::getDownload(UserConnection& aSource, string& aMessage, bool smallSlot) noexcept {
	Lock l(cs);

	const UserPtr& u = aSource.getUser();
	dcdebug("Getting download for %s...", u->getCID().toBase32().c_str());

	QueueItem* q = userQueue.getNext(u, QueueItem::LOWEST, aSource.getChunkSize(), aSource.getSpeed(), true, smallSlot);

	if(!q) {
		aMessage = userQueue.getLastError();
		dcdebug("none\n");
		return 0;
	}

	// Check that the file we will be downloading to exists
	if(q->getDownloadedBytes() > 0) {
		if(!Util::fileExists(q->getTempTarget())) {
			// Temp target gone?
			q->resetDownloaded();
		}
	}

	bool partial = q->isSet(QueueItem::FLAG_PARTIAL_LIST);
	
	Download* d = new Download(aSource, *q, partial ? q->getTempTarget() : q->getTarget());
	if (partial) {
		d->setTempTarget(q->getTarget());
	}
	userQueue.addDownload(q, d);	
	fire(QueueManagerListener::SourcesUpdated(), q);
	dcdebug("found %s\n", q->getTarget().c_str());
	return d;
}

namespace {
class TreeOutputStream : public OutputStream {
public:
	TreeOutputStream(TigerTree& aTree) : tree(aTree), bufPos(0) {
	}

	virtual size_t write(const void* xbuf, size_t len) throw(Exception) {
		size_t pos = 0;
		uint8_t* b = (uint8_t*)xbuf;
		while(pos < len) {
			size_t left = len - pos;
			if(bufPos == 0 && left >= TigerTree::BYTES) {
				tree.getLeaves().push_back(TTHValue(b + pos));
				pos += TigerTree::BYTES;
			} else {
				size_t bytes = min(TigerTree::BYTES - bufPos, left);
				memcpy(buf + bufPos, b + pos, bytes);
				bufPos += bytes;
				pos += bytes;
				if(bufPos == TigerTree::BYTES) {
					tree.getLeaves().push_back(TTHValue(buf));
					bufPos = 0;
				}
			}
		}
		return len;
	}

	virtual size_t flush() throw(Exception) {
		return 0;
	}
private:
	TigerTree& tree;
	uint8_t buf[TigerTree::BYTES];
	size_t bufPos;
};

}

void QueueManager::setFile(Download* d) {
	if(d->getType() == Transfer::TYPE_FILE) {
		Lock l(cs);

		QueueItem* qi = fileQueue.find(d->getPath());
		if(!qi) {
			throw QueueException(STRING(TARGET_REMOVED));
		}
		
		if(d->getOverlapped()) {
			d->setOverlapped(false);

			bool found = false;
			// ok, we got a fast slot, so it's possible to disconnect original user now
			for(DownloadList::const_iterator i = qi->getDownloads().begin(); i != qi->getDownloads().end(); ++i) {
				if((*i) != d && (*i)->getSegment().contains(d->getSegment())) {

					// overlapping has no sense if segment is going to finish
					if((*i)->getSecondsLeft() < 10)
						break;					

					found = true;

					// disconnect slow chunk
					(*i)->getUserConnection().disconnect();
					break;
				}
			}

			if(!found) {
				// slow chunk already finished ???
				throw QueueException(STRING(DOWNLOAD_FINISHED_IDLE));
			}
		}
		
		string target = d->getDownloadTarget();
		
		if(qi->getDownloadedBytes() > 0) {
			if(!Util::fileExists(qi->getTempTarget())) {
				// When trying the download the next time, the resume pos will be reset
				throw QueueException(STRING(TARGET_REMOVED));
			}
		} else {
			File::ensureDirectory(target);
		}

		// open stream for both writing and reading, because UploadManager can request reading from it
		SharedFileStream* f = new SharedFileStream(target, File::RW, File::OPEN | File::CREATE | File::NO_CACHE_HINT);

		// Only use antifrag if we don't have a previous non-antifrag part
		if(BOOLSETTING(ANTI_FRAG) && f->getSize() != qi->getSize()) {
			f->setSize(d->getTigerTree().getFileSize());
		}
		
		f->setPos(d->getSegment().getStart());
		d->setFile(f);
	} else if(d->getType() == Transfer::TYPE_FULL_LIST) {
		{
			Lock l(cs);

			QueueItem* qi = fileQueue.find(d->getPath());
			if(!qi) {
				throw QueueException(STRING(TARGET_REMOVED));
			}
		
			// set filelist's size
			qi->setSize(d->getSize());
		}

		string target = d->getPath();
		File::ensureDirectory(target);

		if(d->isSet(Download::FLAG_XML_BZ_LIST)) {
			target += ".xml.bz2";
		} else {
			target += ".xml";
		}
		d->setFile(new File(target, File::WRITE, File::OPEN | File::TRUNCATE | File::CREATE));
	} else if(d->getType() == Transfer::TYPE_PARTIAL_LIST) {
		d->setFile(new StringOutputStream(d->getPFS()));
	} else if(d->getType() == Transfer::TYPE_TREE) {
		d->setFile(new TreeOutputStream(d->getTigerTree()));		
	}
}

void QueueManager::moveFile(const string& source, const string& target) {
	File::ensureDirectory(target);
	if(File::getSize(source) > MOVER_LIMIT) {
		mover.moveFile(source, target);
	} else {
		moveFile_(source, target);
	}
}

void QueueManager::moveFile_(const string& source, const string& target) {
	try {
		File::renameFile(source, target);
		getInstance()->fire(QueueManagerListener::FileMoved(), target);
	} catch(const FileException& /*e1*/) {
		// Try to just rename it to the correct name at least
		string newTarget = Util::getFilePath(source) + Util::getFileName(target);
		try {
			File::renameFile(source, newTarget);
			LogManager::getInstance()->message(source + " " + STRING(RENAMED_TO) + " " + newTarget);
		} catch(const FileException& e2) {
			LogManager::getInstance()->message(STRING(UNABLE_TO_RENAME) + " " + source + ": " + e2.getError());
		}
	}
	if(BOOLSETTING(USE_FTP_LOGGER))
		getInstance()->fileEvent(target, true);

}


void QueueManager::moveStuckFile(QueueItem* qi) {
	moveFile(qi->getTempTarget(), qi->getTarget());

	if(qi->isFinished()) {
		userQueue.remove(qi);
	}

	string target = qi->getTarget();

	if(!BOOLSETTING(KEEP_FINISHED_FILES)) {
		fire(QueueManagerListener::Removed(), qi);
		fileQueue.remove(qi);
	 } else {
		qi->addSegment(Segment(0, qi->getSize()));
		fire(QueueManagerListener::StatusUpdated(), qi);
	}

	fire(QueueManagerListener::RecheckAlreadyFinished(), target);
}

void QueueManager::rechecked(QueueItem* qi) {
	fire(QueueManagerListener::RecheckDone(), qi->getTarget());
	fire(QueueManagerListener::StatusUpdated(), qi);

	setDirty();
}

void QueueManager::putDownload(Download* aDownload, bool finished, bool reportFinish) noexcept {
	HintedUserList getConn;
 	string fl_fname;
	string fl_path = Util::emptyString;
	HintedUser fl_user = aDownload->getHintedUser();
	Flags::MaskType fl_flag = 0;
	bool downloadList = false;

	{
		Lock l(cs);

		delete aDownload->getFile();
		aDownload->setFile(0);

		if(aDownload->getType() == Transfer::TYPE_PARTIAL_LIST) {
			QueueItem* q;
			if (!aDownload->getPath().empty()) {
				q = fileQueue.find(aDownload->getTempTarget());
			} else {
				//root directory in the partial list
				q = fileQueue.find(getListPath(aDownload->getHintedUser()));
			}
			if(q) {
				if(!aDownload->getPFS().empty()) {
					if( (q->isSet(QueueItem::FLAG_DIRECTORY_DOWNLOAD) && directories.find(aDownload->getUser()) != directories.end()) ||
						(q->isSet(QueueItem::FLAG_MATCH_QUEUE)) ||
						(q->isSet(QueueItem::FLAG_VIEW_NFO)))
					{
						dcassert(finished);
											
						fl_fname = aDownload->getPFS();
						fl_user = aDownload->getHintedUser();
						fl_path = aDownload->getPath();
						fl_flag = (q->isSet(QueueItem::FLAG_DIRECTORY_DOWNLOAD) ? (QueueItem::FLAG_DIRECTORY_DOWNLOAD) : 0)
							| (q->isSet(QueueItem::FLAG_PARTIAL_LIST) ? (QueueItem::FLAG_PARTIAL_LIST) : 0)
							| (q->isSet(QueueItem::FLAG_MATCH_QUEUE) ? QueueItem::FLAG_MATCH_QUEUE : 0) | QueueItem::FLAG_TEXT
							| (q->isSet(QueueItem::FLAG_VIEW_NFO) ? QueueItem::FLAG_VIEW_NFO : 0);
					} else {
						fire(QueueManagerListener::PartialList(), aDownload->getHintedUser(), aDownload->getPFS());
					}
				} else {
					// partial filelist probably failed, redownload full list
					dcassert(!finished);
					if (!q->isSet(QueueItem::FLAG_VIEW_NFO) || !aDownload->getUserConnection().isSet(UserConnection::FLAG_SMALL_SLOT))
						downloadList = true;
					fl_flag = q->getFlags() & ~QueueItem::FLAG_PARTIAL_LIST;	
				}
					
				fire(QueueManagerListener::Removed(), q);

				userQueue.remove(q);
				fileQueue.remove(q);
			}
		} else {
			QueueItem* q = fileQueue.find(aDownload->getPath());

			if(q) {
				if(aDownload->getType() == Transfer::TYPE_FULL_LIST) {
					if(aDownload->isSet(Download::FLAG_XML_BZ_LIST)) {
						q->setFlag(QueueItem::FLAG_XML_BZLIST);
					} else {
						q->unsetFlag(QueueItem::FLAG_XML_BZLIST);
					}
				}

				if(finished) {
					if(aDownload->getType() == Transfer::TYPE_TREE) {
						// Got a full tree, now add it to the HashManager
						dcassert(aDownload->getTreeValid());
						HashManager::getInstance()->addTree(aDownload->getTigerTree());

						userQueue.removeDownload(q, aDownload->getUser());
						fire(QueueManagerListener::StatusUpdated(), q);
					} else {
						// Now, let's see if this was a directory download filelist...
						if( (q->isSet(QueueItem::FLAG_DIRECTORY_DOWNLOAD) && directories.find(aDownload->getHintedUser()) != directories.end()) ||
							(q->isSet(QueueItem::FLAG_MATCH_QUEUE)) ||
							(q->isSet(QueueItem::FLAG_VIEW_NFO))) 
						{
							fl_fname = q->getListName();
							fl_user = aDownload->getHintedUser();
							fl_flag = (q->isSet(QueueItem::FLAG_DIRECTORY_DOWNLOAD) ? QueueItem::FLAG_DIRECTORY_DOWNLOAD : 0)
								| (q->isSet(QueueItem::FLAG_MATCH_QUEUE) ? QueueItem::FLAG_MATCH_QUEUE : 0)
								| (q->isSet(QueueItem::FLAG_VIEW_NFO) ? QueueItem::FLAG_VIEW_NFO : 0);
						} 

						string dir;

						if(aDownload->getType() == Transfer::TYPE_FULL_LIST) {
							dir = q->getTempTarget();
							q->addSegment(Segment(0, q->getSize()));
						} else if(aDownload->getType() == Transfer::TYPE_FILE) {
							aDownload->setOverlapped(false);
							q->addSegment(aDownload->getSegment());							
						}
						
						if(aDownload->getType() != Transfer::TYPE_FILE || q->isFinished()) {

							if(aDownload->getType() == Transfer::TYPE_FILE) {
								// For partial-share, abort upload first to move file correctly
								UploadManager::getInstance()->abortUpload(q->getTempTarget());

								// Disconnect all possible overlapped downloads
								for(DownloadList::const_iterator i = q->getDownloads().begin(); i != q->getDownloads().end(); ++i) {
									if(*i != aDownload)
										(*i)->getUserConnection().disconnect();
								}
							}
						
							// Check if we need to move the file
							if( aDownload->getType() == Transfer::TYPE_FILE && !aDownload->getTempTarget().empty() && (stricmp(aDownload->getPath().c_str(), aDownload->getTempTarget().c_str()) != 0) ) {
								moveFile(aDownload->getTempTarget(), aDownload->getPath());
							}

							if(BOOLSETTING(LOG_DOWNLOADS) && (BOOLSETTING(LOG_FILELIST_TRANSFERS) || aDownload->getType() == Transfer::TYPE_FILE)) {
								StringMap params;
								aDownload->getParams(aDownload->getUserConnection(), params);
								LOG(LogManager::DOWNLOAD, params);
							}
							
							fire(QueueManagerListener::Finished(), q, dir, aDownload);
													
							if(aDownload->getType() != Transfer::TYPE_FULL_LIST && aDownload->getType() != Transfer::TYPE_PARTIAL_LIST) 
								updateDirSize(q->getTarget(), q->getSize(), false);

							userQueue.remove(q);
			
							if(!BOOLSETTING(KEEP_FINISHED_FILES) || aDownload->getType() == Transfer::TYPE_FULL_LIST) {
								fire(QueueManagerListener::Removed(), q);						
								fileQueue.remove(q);
							} else {
								fire(QueueManagerListener::StatusUpdated(), q);
							}
						} else {

							userQueue.removeDownload(q, aDownload->getUser());
							if(aDownload->getType() != Transfer::TYPE_FILE || (reportFinish && q->isWaiting())) {
								fire(QueueManagerListener::StatusUpdated(), q);
							}
						}

						setDirty();
					}
				} else {
					if(aDownload->getType() != Transfer::TYPE_TREE) {
						if(q->getDownloadedBytes() == 0) {
							q->setTempTarget(Util::emptyString);
						}
						if(q->isSet(QueueItem::FLAG_USER_LIST)) {
							// Blah...no use keeping an unfinished file list...
							try{
							File::deleteFile(q->getListName());
							}catch(...) {}
						}
						if(aDownload->getType() == Transfer::TYPE_FILE) {
							// mark partially downloaded chunk, but align it to block size
							int64_t downloaded = aDownload->getPos();
							downloaded -= downloaded % aDownload->getTigerTree().getBlockSize();

							if(downloaded > 0) {
								// since download is not finished, it should never happen that downloaded size is same as segment size
								dcassert(downloaded < aDownload->getSize());
								
								q->addSegment(Segment(aDownload->getStartPos(), downloaded));
								setDirty();
							}
						}
					}

					if(q->getPriority() != QueueItem::PAUSED) {
						q->getOnlineUsers(getConn);
					}
	
					userQueue.removeDownload(q, aDownload->getUser());
					fire(QueueManagerListener::StatusUpdated(), q);

					if(aDownload->isSet(Download::FLAG_OVERLAP)) {
						// overlapping segment disconnected, unoverlap original segment
						for(DownloadList::const_iterator i = q->getDownloads().begin(); i != q->getDownloads().end(); ++i) {
							if((*i)->getSegment().contains(aDownload->getSegment())) {
								(*i)->setOverlapped(false);
								break;
							}
						}
					}
 				}
 			} else if(aDownload->getType() != Transfer::TYPE_TREE) {
				string path = aDownload->getPath();
				if(aDownload->getType() == Transfer::TYPE_FULL_LIST) {
					// delete unfinished lists manually removed from queue
					if(aDownload->isSet(Download::FLAG_XML_BZ_LIST)) {
						path += ".xml.bz2";
					} else {
						path += ".xml";
					}
					try{
					File::deleteFile(path);
					}catch(...) {}
				} else if(!aDownload->getTempTarget().empty() && aDownload->getTempTarget() != path) {
					try{
 					File::deleteFile(aDownload->getTempTarget());
					}catch(...) {}
 				}
 			}
		}
		delete aDownload;
	}

	for(HintedUserList::const_iterator i = getConn.begin(); i != getConn.end(); ++i) {
		ConnectionManager::getInstance()->getDownloadConnection(*i);
	}

	if(!fl_fname.empty() && fl_user.user) {
		processList(fl_fname, fl_user, fl_path, fl_flag);
	}

	// partial file list failed, redownload full list
	if(fl_user.user->isOnline() && downloadList) {
		try {
			addList(fl_user, fl_flag);
		} catch(const Exception&) {}
	}
}

void QueueManager::processList(const string& name, const HintedUser& user, const string path, int flags) {
	DirectoryListing dirList(user);
	try {
		if(flags & QueueItem::FLAG_TEXT) {
			MemoryInputStream mis(name);
			dirList.loadXML(mis, true, false);
		} else {
			dirList.loadFile(name, false);
		}
	} catch(const Exception&) {
		LogManager::getInstance()->message(STRING(UNABLE_TO_OPEN_FILELIST) + " " + name);
		return;
	}

	if(flags & QueueItem::FLAG_DIRECTORY_DOWNLOAD) {
		if (!path.empty()) {
			DirectoryItem* d = NULL;
			{
				Lock l(cs);
				auto dp = directories.equal_range(user);
				for(auto i = dp.first; i != dp.second; ++i) {
					if(stricmp(path.c_str(), i->second->getName().c_str()) == 0) {
						d = i->second;
						directories.erase(i);
						break;
					}
				}
			}
			if(d != NULL){
				/* fix deadlock!!  cant call to download inside a lock, the lock time becomes too long and goes too deep,
				threads in Queuemanager and Connectionmanager will lock each other. */
				dirList.download(d->getName(), d->getTarget(), false, d->getPriority(), true);
				delete d;
			}

		} else {
			vector<DirectoryItemPtr> dl;
			{
				Lock l(cs);
				auto dpf = directories.equal_range(user) | map_values;
				dl.assign(boost::begin(dpf), boost::end(dpf));
				directories.erase(user);
			}
			for(auto i = dl.begin(); i != dl.end(); ++i) {
				DirectoryItem* di = *i;
				dirList.download(di->getName(), di->getTarget(), false, di->getPriority());
				delete di;
			}
		}
	}
	if(flags & QueueItem::FLAG_MATCH_QUEUE) {
		const size_t BUF_SIZE = STRING(MATCHED_FILES).size() + 16;
		string tmp;
		tmp.resize(BUF_SIZE);
		snprintf(&tmp[0], tmp.size(), CSTRING(MATCHED_FILES), matchListing(dirList, (flags & QueueItem::FLAG_PARTIAL_LIST ? true : false)));
		if(flags & QueueItem::FLAG_PARTIAL_LIST) {
			//no report
		} else {
			LogManager::getInstance()->message(Util::toString(ClientManager::getInstance()->getNicks(user)) + ": " + tmp);
		}
	}
	if((flags & QueueItem::FLAG_VIEW_NFO) && (flags & QueueItem::FLAG_PARTIAL_LIST)) {
		findNfo(dirList.getRoot(), dirList);
	}
}

bool QueueManager::findNfo(const DirectoryListing::Directory* dl, const DirectoryListing& dir) noexcept {

	for(DirectoryListing::Directory::List::const_iterator j = dl->directories.begin(); j != dl->directories.end(); ++j) {
		if(!(*j)->getAdls())
			findNfo(*j, dir);
	}


	if (!dl->files.empty()) {
		boost::wregex reg;
		reg.assign(_T("(.+\\.nfo)"), boost::regex_constants::icase);
		for(DirectoryListing::File::List::const_iterator i = dl->files.begin(); i != dl->files.end(); ++i) {
			const DirectoryListing::File* df = *i;
			if (regex_match(Text::toT(df->getName()), reg)) {
				QueueManager::getInstance()->add(Util::getTempPath() + df->getName(), df->getSize(), df->getTTH(), dir.getHintedUser(), QueueItem::FLAG_CLIENT_VIEW | QueueItem::FLAG_TEXT);
				return true;
			}
		}
		//can be reported because this is the only folder containing files in partial list
		LogManager::getInstance()->message(Util::toString(ClientManager::getInstance()->getNicks(dir.getHintedUser())) + ": " + STRING(NO_NFO_FOUND));
	}
	
	return false;
}

void QueueManager::recheck(const string& aTarget) {
	rechecker.add(aTarget);
}

void QueueManager::remove(const string& aTarget) noexcept {
	UserConnectionList x;

	{
		Lock l(cs);

		QueueItem* q = fileQueue.find(aTarget);
		if(!q)
			return;

		if(q->isSet(QueueItem::FLAG_DIRECTORY_DOWNLOAD)) {
			dcassert(q->getSources().size() == 1);
			auto dp = directories.equal_range(q->getSources()[0].getUser());
			for(auto i = dp.first; i != dp.second; ++i) {
				delete i->second;
			}
			directories.erase(q->getSources()[0].getUser());
		}

		// For partial-share
		UploadManager::getInstance()->abortUpload(q->getTempTarget());

		if(q->isRunning()) {
			for(DownloadList::iterator i = q->getDownloads().begin(); i != q->getDownloads().end(); ++i) {
				UserConnection* uc = &(*i)->getUserConnection();
				x.push_back(uc);
			}
		} else if(!q->getTempTarget().empty() && q->getTempTarget() != q->getTarget()) {
			File::deleteFile(q->getTempTarget());
		}

		fire(QueueManagerListener::Removed(), q);

		if(!q->isFinished()) {
			userQueue.remove(q);
		}
		
		if(!q->isSet(QueueItem::FLAG_USER_LIST)) 
		updateDirSize(q->getTarget(), q->getSize(), false);
		
		fileQueue.remove(q);

		setDirty();
	}

	for(UserConnectionList::const_iterator i = x.begin(); i != x.end(); ++i) {
		(*i)->disconnect(true);
	}
}

void QueueManager::removeSource(const string& aTarget, const UserPtr& aUser, Flags::MaskType reason, bool removeConn /* = true */) noexcept {
	bool isRunning = false;
	bool removeCompletely = false;
	{
		Lock l(cs);
		QueueItem* q = fileQueue.find(aTarget);
		if(!q)
			return;

		if(!q->isSource(aUser))
			return;
	
		if(q->isSet(QueueItem::FLAG_USER_LIST)) {
			removeCompletely = true;
			goto endCheck;
		}

		if(reason == QueueItem::Source::FLAG_NO_TREE) {
			q->getSource(aUser)->setFlag(reason);
			return;
		}

		if(q->isRunning() && userQueue.getRunning(aUser) == q) {
			isRunning = true;
			userQueue.removeDownload(q, aUser);
			fire(QueueManagerListener::StatusUpdated(), q);

		}
		if(!q->isFinished()) {
			userQueue.remove(q, aUser);
		}
		q->removeSource(aUser, reason);
		
		fire(QueueManagerListener::SourcesUpdated(), q);
		setDirty();
	}
endCheck:
	if(isRunning && removeConn) {
		ConnectionManager::getInstance()->disconnect(aUser, true);
	}
	if(removeCompletely) {
		remove(aTarget);
	}	
}

void QueueManager::removeSource(const UserPtr& aUser, Flags::MaskType reason) noexcept {
	// @todo remove from finished items
	bool isRunning = false;
	string removeRunning;
	{
		Lock l(cs);
		QueueItem* qi = NULL;
		while( (qi = userQueue.getNext(aUser, QueueItem::PAUSED)) != NULL) {
			if(qi->isSet(QueueItem::FLAG_USER_LIST)) {
				remove(qi->getTarget());
			} else {
				userQueue.remove(qi, aUser);
				qi->removeSource(aUser, reason);
				fire(QueueManagerListener::SourcesUpdated(), qi);
				setDirty();
			}
		}
		
		qi = userQueue.getRunning(aUser);
		if(qi) {
			if(qi->isSet(QueueItem::FLAG_USER_LIST)) {
				removeRunning = qi->getTarget();
			} else {
				userQueue.removeDownload(qi, aUser);
				userQueue.remove(qi, aUser);
				isRunning = true;
				qi->removeSource(aUser, reason);
				fire(QueueManagerListener::StatusUpdated(), qi);
				fire(QueueManagerListener::SourcesUpdated(), qi);
				setDirty();
			}
		}
	}

	if(isRunning) {
		ConnectionManager::getInstance()->disconnect(aUser, true);
	}
	if(!removeRunning.empty()) {
		remove(removeRunning);
	}	
}

void QueueManager::setPriority(const string& aTarget, QueueItem::Priority p) noexcept {
	HintedUserList getConn;
	bool running = false;

	{
		Lock l(cs);
	
		QueueItem* q = fileQueue.find(aTarget);
		if( (q != NULL) && (q->getPriority() != p) && !q->isFinished() ) {
			running = q->isRunning();

			if(q->getPriority() == QueueItem::PAUSED || p == QueueItem::HIGHEST) {
				// Problem, we have to request connections to all these users...
				q->getOnlineUsers(getConn);
			}
			userQueue.setPriority(q, p);
			setDirty();
			fire(QueueManagerListener::StatusUpdated(), q);
		}
	}

	if(p == QueueItem::PAUSED) {
		if(running)
			DownloadManager::getInstance()->abortDownload(aTarget);
	} else {
		for(HintedUserList::const_iterator i = getConn.begin(); i != getConn.end(); ++i) {
			ConnectionManager::getInstance()->getDownloadConnection(*i);
		}
	}
}

void QueueManager::setAutoPriority(const string& aTarget, bool ap) noexcept {
	vector<pair<string, QueueItem::Priority>> priorities;

	{
		Lock l(cs);
	
		QueueItem* q = fileQueue.find(aTarget);
		if( (q != NULL) && (q->getAutoPriority() != ap) ) {
			q->setAutoPriority(ap);
			if(ap) {
				priorities.push_back(make_pair(q->getTarget(), q->calculateAutoPriority()));
			}
			setDirty();
			fire(QueueManagerListener::StatusUpdated(), q);
		}
	}

	for(vector<pair<string, QueueItem::Priority>>::const_iterator p = priorities.begin(); p != priorities.end(); p++) {
		setPriority((*p).first, (*p).second);
	}
}

void QueueManager::saveQueue(bool force) noexcept {
	if(!dirty && !force)
		return;
		
	

try {
		Lock l(cs);	
		
		File ff(getQueueFile() + ".tmp", File::WRITE, File::CREATE | File::TRUNCATE);
		BufferedOutputStream<false> f(&ff);
		
		f.write(SimpleXML::utf8Header);
		f.write(LIT("<Downloads Version=\"" VERSIONSTRING "\">\r\n"));
		string tmp;
		string b32tmp;
		for(auto i = fileQueue.getQueue().begin(); i != fileQueue.getQueue().end(); ++i) {
			QueueItem* qi = i->second;
			if(!qi->isSet(QueueItem::FLAG_USER_LIST)) {
				f.write(LIT("\t<Download Target=\""));
				f.write(SimpleXML::escape(qi->getTarget(), tmp, true));
				f.write(LIT("\" Size=\""));
				f.write(Util::toString(qi->getSize()));
				f.write(LIT("\" Priority=\""));
				f.write(Util::toString((int)qi->getPriority()));
				f.write(LIT("\" Added=\""));
				f.write(Util::toString(qi->getAdded()));
				b32tmp.clear();
				f.write(LIT("\" TTH=\""));
				f.write(qi->getTTH().toBase32(b32tmp));
				if(!qi->getDone().empty()) {
					f.write(LIT("\" TempTarget=\""));
					f.write(SimpleXML::escape(qi->getTempTarget(), tmp, true));
				}
				f.write(LIT("\" AutoPriority=\""));
				f.write(Util::toString(qi->getAutoPriority()));
				f.write(LIT("\" MaxSegments=\""));
				f.write(Util::toString(qi->getMaxSegments()));

				f.write(LIT("\">\r\n"));

				for(QueueItem::SegmentSet::const_iterator i = qi->getDone().begin(); i != qi->getDone().end(); ++i) {
					f.write(LIT("\t\t<Segment Start=\""));
					f.write(Util::toString(i->getStart()));
					f.write(LIT("\" Size=\""));
					f.write(Util::toString(i->getSize()));
					f.write(LIT("\"/>\r\n"));
				}

				for(QueueItem::SourceConstIter j = qi->sources.begin(); j != qi->sources.end(); ++j) {
					if(j->isSet(QueueItem::Source::FLAG_PARTIAL)) continue;
					
					const CID& cid = j->getUser().user->getCID();
					const string& hint = j->getUser().hint;

					f.write(LIT("\t\t<Source CID=\""));
					f.write(cid.toBase32());
					f.write(LIT("\" Nick=\""));
					f.write(SimpleXML::escape(ClientManager::getInstance()->getNicks(cid, hint)[0], tmp, true));
					if(!hint.empty()) {
						f.write(LIT("\" HubHint=\""));
						f.write(hint);
					}
					f.write(LIT("\"/>\r\n"));
				}

				f.write(LIT("\t</Download>\r\n"));
			}
		}
		
		f.write("</Downloads>\r\n");
		f.flush();
		ff.close();

		File::deleteFile(getQueueFile() + ".bak");
		CopyFile(Text::toT(getQueueFile()).c_str(), Text::toT(getQueueFile() + ".bak").c_str(), FALSE);
		File::deleteFile(getQueueFile());
		File::renameFile(getQueueFile() + ".tmp", getQueueFile());

		dirty = false;
	} catch(...) {
		// ...
	}
	// Put this here to avoid very many saves tries when disk is full...
	lastSave = GET_TICK();
}

class QueueLoader : public SimpleXMLReader::CallBack {
public:
	QueueLoader() : cur(NULL), inDownloads(false) { }
	~QueueLoader() { }
	void startTag(const string& name, StringPairList& attribs, bool simple);
	void endTag(const string& name, const string& data);
private:
	string target;

	QueueItem* cur;
	bool inDownloads;
};

void QueueManager::loadQueue() noexcept {
	try {
		QueueLoader l;
		Util::migrate(getQueueFile());

		File f(getQueueFile(), File::READ, File::OPEN);
		SimpleXMLReader(&l).parse(f);
		dirty = false;
	} catch(const Exception&) {
		// ...
	}
}

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
static const string sTTH = "TTH";
static const string sCID = "CID";
static const string sHubHint = "HubHint";
static const string sSegment = "Segment";
static const string sStart = "Start";
static const string sAutoPriority = "AutoPriority";
static const string sMaxSegments = "MaxSegments";



void QueueLoader::startTag(const string& name, StringPairList& attribs, bool simple) {
	QueueManager* qm = QueueManager::getInstance();	
	if(!inDownloads && name == "Downloads") {
		inDownloads = true;
	} else if(inDownloads) {
		if(cur == NULL && name == sDownload) {
			int64_t size = Util::toInt64(getAttrib(attribs, sSize, 1));
			if(size == 0)
				return;
			try {
				const string& tgt = getAttrib(attribs, sTarget, 0);
				// @todo do something better about existing files
				target = QueueManager::checkTarget(tgt,  /*checkExistence*/ false);
				if(target.empty())
					return;
			} catch(const Exception&) {
				return;
			}
			QueueItem::Priority p = (QueueItem::Priority)Util::toInt(getAttrib(attribs, sPriority, 3));
			time_t added = static_cast<time_t>(Util::toInt(getAttrib(attribs, sAdded, 4)));
			const string& tthRoot = getAttrib(attribs, sTTH, 5);
			if(tthRoot.empty())
				return;

			string tempTarget = getAttrib(attribs, sTempTarget, 5);
			uint8_t maxSegments = (uint8_t)Util::toInt(getAttrib(attribs, sMaxSegments, 5));
			int64_t downloaded = Util::toInt64(getAttrib(attribs, sDownloaded, 5));
			if (downloaded > size || downloaded < 0)
				downloaded = 0;

			if(added == 0)
				added = GET_TIME();

			QueueItem* qi = qm->fileQueue.find(target);

			if(qi == NULL) {
				qi = qm->fileQueue.add(target, size, 0, p, tempTarget, added, TTHValue(tthRoot));
				if(downloaded > 0) {
					qi->addSegment(Segment(0, downloaded));
					qi->setPriority(qi->calculateAutoPriority());
				}

				bool ap = Util::toInt(getAttrib(attribs, sAutoPriority, 6)) == 1;
				qi->setAutoPriority(ap);
				qi->setMaxSegments(max((uint8_t)1, maxSegments));
				qm->updateDirSize(target, size, true);
				qm->fire(QueueManagerListener::Added(), qi);
			}
			if(!simple)
				cur = qi;
		} else if(cur && name == sSegment) {
			int64_t start = Util::toInt64(getAttrib(attribs, sStart, 0));
			int64_t size = Util::toInt64(getAttrib(attribs, sSize, 1));
			
			if(size > 0 && start >= 0 && (start + size) <= cur->getSize()) {
				cur->addSegment(Segment(start, size));
				cur->setPriority(cur->calculateAutoPriority());
			}
		} else if(cur && name == sSource) {
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
				if(qm->addSource(cur, hintedUser, 0) && user->isOnline())
					ConnectionManager::getInstance()->getDownloadConnection(hintedUser);
			} catch(const Exception&) {
				return;
			}
		}
	}
}

void QueueLoader::endTag(const string& name, const string&) {
	
	if(inDownloads) {
		if(name == sDownload) {
			cur = NULL;
		} else if(name == "Downloads") {
			inDownloads = false;
		}
	}
}


void QueueManager::noDeleteFileList(const string& path) {
	if(!BOOLSETTING(KEEP_LISTS)) {
		protectedFileLists.push_back(path);
	}
}

// SearchManagerListener
void QueueManager::on(SearchManagerListener::SR, const SearchResultPtr& sr) noexcept {
	bool added = false;
	bool wantConnection = false;
	bool matchPartialADC = false;
	bool matchPartialNMDC = false;
	size_t users = 0;

	{
		Lock l(cs);
		QueueItemList  matches = fileQueue.find(sr->getTTH());

		for(auto i = matches.begin(); i != matches.end(); ++i) {
			QueueItem* qi = *i;

			// Size compare to avoid popular spoof
			if(qi->getSize() == sr->getSize() && !qi->isSource(sr->getUser())) {
				try {
					
					if(qi->isFinished())
						break;  // don't add sources to already finished files

					users = qi->countOnlineUsers(); 

					if(BOOLSETTING(AUTO_ADD_SOURCE)) {
					
						bool nmdcUser = sr->getUser()->isSet(User::NMDC);
						/* match with partial list allways but decide if we are in nmdc hub here. 
						( dont want to change the settings, would just break what user has set already, 
						alltho would make a bit more sense to have just 1 match queue option, but since many have that disabled totally and we prefer to match partials in adchubs... ) */
						if(BOOLSETTING(AUTO_SEARCH_AUTO_MATCH) && (users < (size_t)SETTING(MAX_AUTO_MATCH_SOURCES))) {
							if(nmdcUser)							
								matchPartialNMDC = true;
							else
								matchPartialADC = true;
						//if we are in adc hub match with recursive partial list
						} else if(BOOLSETTING(PARTIAL_MATCH_ADC) && !nmdcUser) {
								matchPartialADC = true;
							//if its a rar release add the sources to all files.
						} else if (regexp.match(sr->getFile(), sr->getFile().length()-4) > 0) {
								wantConnection = addAlternates(sr->getFile(), HintedUser(sr->getUser(), sr->getHubURL()));
							// this is how sdc has it, dont add sources and receive wantconnection if we are about to match queue.
						} else {
								wantConnection = addSource(qi, HintedUser(sr->getUser(), sr->getHubURL()), 0);
						}
					}

					added = true;

					} catch(const Exception&) {
					//...
					}
				break;
			}
		}
	}

	//moved outside lock range.
	if(added && matchPartialADC) {
		try {
			string path = Util::getDir(Util::getFilePath(sr->getFile()), true, false);
			addList(HintedUser(sr->getUser(), sr->getHubURL()), QueueItem::FLAG_MATCH_QUEUE | QueueItem::FLAG_RECURSIVE_LIST |(path.empty() ? 0 : QueueItem::FLAG_PARTIAL_LIST), path);
		}catch(...) { }
	}

	if(added && matchPartialNMDC) {
		try {
			string path = Util::getFilePath(sr->getFile());
			addList(HintedUser(sr->getUser(), sr->getHubURL()), QueueItem::FLAG_MATCH_QUEUE |(path.empty() ? 0 : QueueItem::FLAG_PARTIAL_LIST), path);
		} catch(const Exception&) {
			// ...
		}
	}
	if(added && sr->getUser()->isOnline() && wantConnection) {
		ConnectionManager::getInstance()->getDownloadConnection(HintedUser(sr->getUser(), sr->getHubURL()));
	}

}

bool QueueManager::addAlternates(const string& aFile, const dcpp::HintedUser& aUser) {
	string path, file;
	string::size_type pos, pos2;
	bool wantConnection = false;

	try {
		
		pos = aFile.find_last_of(".");
		if(pos == string::npos)
			return false;
		
		pos2 = aFile.find_last_of("\\");
		if(pos2 == string::npos)
			return false;

		file = aFile.substr(pos2+1, pos - pos2);
		path = aFile.substr(0, pos2);

		if(file.empty() || path.empty())
			return false;

		QueueItem::StringMap::const_iterator i;
		QueueItem::StringMap queue = fileQueue.getQueue();
		//iterate through the entire queue and add the user as source
		//where the filenames match
		for(i = queue.begin(); i != queue.end(); ++i) {
			if( i->first->find(file) != string::npos) {
				if(!i->second->isSource(aUser)) {
					try{
					wantConnection = addSource(i->second, aUser, 0);
					}catch(...) { //catch here so it can continue the loop, and add other sources
						//errors can spam syslog here
					}
				}	
			}
		}
		} catch(const Exception&) {
			}

	return wantConnection;
}
// ClientManagerListener
void QueueManager::on(ClientManagerListener::UserConnected, const UserPtr& aUser) noexcept {
	bool hasDown = false;
	{
		Lock l(cs);
		for(int i = 0; i < QueueItem::LAST; ++i) {
			auto j = userQueue.getList(i).find(aUser);
			if(j != userQueue.getList(i).end()) {
				for(auto m = j->second.begin(); m != j->second.end(); ++m)
					fire(QueueManagerListener::StatusUpdated(), *m);
				if(i != QueueItem::PAUSED)
					hasDown = true;
			}
		}
	}

	if(hasDown && aUser->isOnline()) { //even tho user just connected, check if he is still online and that we really have a user.
		// the user just came on, so there's only 1 possible hub, no need for a hint
		ConnectionManager::getInstance()->getDownloadConnection(HintedUser(aUser, Util::emptyString));
	}
}

void QueueManager::on(ClientManagerListener::UserDisconnected, const UserPtr& aUser) noexcept {
	Lock l(cs);
	for(int i = 0; i < QueueItem::LAST; ++i) {
		auto j = userQueue.getList(i).find(aUser);
		if(j != userQueue.getList(i).end()) {
			for(auto m = j->second.begin(); m != j->second.end(); ++m)
				fire(QueueManagerListener::StatusUpdated(), *m);
		}
	}
}

void QueueManager::on(TimerManagerListener::Second, uint64_t aTick) noexcept {
	if(dirty && ((lastSave + 10000) < aTick)) {
		saveQueue();
	}


	vector<pair<string, QueueItem::Priority>> priorities;

	{
		Lock l(cs);

		QueueItemList um = getRunningFiles();
		for(auto j = um.begin(); j != um.end(); ++j) {
			QueueItem* q = *j;

			if(q->getAutoPriority()) {
				QueueItem::Priority p1 = q->getPriority();
				if(p1 != QueueItem::PAUSED) {
					QueueItem::Priority p2 = q->calculateAutoPriority();
					if(p1 != p2)
						priorities.push_back(make_pair(q->getTarget(), p2));
				}
			}
			fire(QueueManagerListener::StatusUpdated(), q);
		}
	}

	for(vector<pair<string, QueueItem::Priority>>::const_iterator p = priorities.begin(); p != priorities.end(); p++) {
		setPriority((*p).first, (*p).second);
	}


}

bool QueueManager::dropSource(Download* d) {
	size_t activeSegments = 0, onlineUsers;
	uint64_t overallSpeed;

	{
	    Lock l(cs);

		QueueItem* q = userQueue.getRunning(d->getUser());

		if(!q)
			return false;

   		dcassert(q->isSource(d->getUser()));

		if(!q->isSet(QueueItem::FLAG_AUTODROP))
			return false;

		for(DownloadList::const_iterator i = q->getDownloads().begin(); i != q->getDownloads().end(); i++) {
			if((*i)->getStart() > 0) {
				activeSegments++;
			}

			// more segments won't change anything
			if(activeSegments > 2)
				break;
		}

		onlineUsers = q->countOnlineUsers();
		overallSpeed = q->getAverageSpeed();
	}

	if(!SETTING(DROP_MULTISOURCE_ONLY) || (activeSegments >= 2)) {
		size_t iHighSpeed = SETTING(DISCONNECT_FILE_SPEED);

		if((iHighSpeed == 0 || overallSpeed > iHighSpeed * 1024) && onlineUsers > 2) {
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

	{
		Lock l(cs);

		// Locate target QueueItem in download queue
		QueueItemList ql = fileQueue.find(tth);
		
		if(ql.empty()){
			dcdebug("Not found in download queue\n");
			return false;
		}
		
		QueueItemPtr qi = ql.front();

		// don't add sources to finished files
		// this could happen when "Keep finished files in queue" is enabled
		if(qi->isFinished())
			return false;

		// Check min size
		if(qi->getSize() < PARTIAL_SHARE_MIN_SIZE){
			dcassert(0);
			return false;
		}

		// Get my parts info
		int64_t blockSize = HashManager::getInstance()->getBlockSize(qi->getTTH());
		if(blockSize == 0)
			blockSize = qi->getSize();
		qi->getPartialInfo(outPartialInfo, blockSize);
		
		// Any parts for me?
		wantConnection = qi->isNeededPart(partialSource.getPartialInfo(), blockSize);

		// If this user isn't a source and has no parts needed, ignore it
		QueueItem::SourceIter si = qi->getSource(aUser);
		if(si == qi->getSources().end()){
			si = qi->getBadSource(aUser);

			if(si != qi->getBadSources().end() && si->isSet(QueueItem::Source::FLAG_TTH_INCONSISTENCY))
				return false;

			if(!wantConnection){
				if(si == qi->getBadSources().end())
					return false;
			}else{
				// add this user as partial file sharing source
				qi->addSource(aUser);
				si = qi->getSource(aUser);
				si->setFlag(QueueItem::Source::FLAG_PARTIAL);

				QueueItem::PartialSource* ps = new QueueItem::PartialSource(partialSource.getMyNick(),
					partialSource.getHubIpPort(), partialSource.getIp(), partialSource.getUdpPort());
				si->setPartialSource(ps);

				userQueue.add(qi, aUser);
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

bool QueueManager::handlePartialSearch(const TTHValue& tth, PartsInfo& _outPartsInfo) {
	{
		Lock l(cs);

		// Locate target QueueItem in download queue
		QueueItemList ql = fileQueue.find(tth);

		if(ql.empty()){
			return false;
		}

		QueueItemPtr qi = ql.front();
		if(qi->getSize() < PARTIAL_SHARE_MIN_SIZE){
			return false;
		}

		int64_t blockSize = HashManager::getInstance()->getBlockSize(qi->getTTH());
		if(blockSize == 0)
			blockSize = qi->getSize();
		qi->getPartialInfo(_outPartsInfo, blockSize);
	}

	return !_outPartsInfo.empty();
}

// compare nextQueryTime, get the oldest ones
void QueueManager::FileQueue::findPFSSources(PFSSourceList& sl) 
{
	typedef multimap<time_t, pair<QueueItem::SourceConstIter, const QueueItem*> > Buffer;
	Buffer buffer;
	uint64_t now = GET_TICK();

	for(auto i = queue.begin(); i != queue.end(); ++i) {
		QueueItem* q = i->second;

		if(q->getSize() < PARTIAL_SHARE_MIN_SIZE) continue;

		// don't share when file does not exist
 	    if(!Util::fileExists(q->isFinished() ? q->getTarget() : q->getTempTarget()))
			continue;

		const QueueItem::SourceList& sources = q->getSources();
		const QueueItem::SourceList& badSources = q->getBadSources();

		for(QueueItem::SourceConstIter j = sources.begin(); j != sources.end(); ++j) {
			if(	(*j).isSet(QueueItem::Source::FLAG_PARTIAL) && (*j).getPartialSource()->getNextQueryTime() <= now &&
				(*j).getPartialSource()->getPendingQueryCount() < 10 && (*j).getPartialSource()->getUdpPort() > 0)
			{
				buffer.insert(make_pair((*j).getPartialSource()->getNextQueryTime(), make_pair(j, q)));
			}
		}

		for(QueueItem::SourceConstIter j = badSources.begin(); j != badSources.end(); ++j) {
			if(	(*j).isSet(QueueItem::Source::FLAG_TTH_INCONSISTENCY) == false && (*j).isSet(QueueItem::Source::FLAG_PARTIAL) &&
				(*j).getPartialSource()->getNextQueryTime() <= now && (*j).getPartialSource()->getPendingQueryCount() < 10 &&
				(*j).getPartialSource()->getUdpPort() > 0)
			{
				buffer.insert(make_pair((*j).getPartialSource()->getNextQueryTime(), make_pair(j, q)));
			}
		}
	}

	// copy to results
	dcassert(sl.empty());
	const int32_t maxElements = 10;
	sl.reserve(maxElements);
	for(Buffer::iterator i = buffer.begin(); i != buffer.end() && sl.size() < maxElements; i++){
		sl.push_back(i->second);
	}
}

// Total Time Left /* ttlf */ 
/*
int64_t QueueManager::FileQueue::getTotalSize(const string & path){

	int qpos,pos = path.rfind("\\");
	string targetPath = path.substr(0, pos);
	string queueTargetPath;
	int64_t totalSize=0;
	
	for(auto i = queue.begin(); i != queue.end(); ++i) {
		QueueItem* q = i->second;
		queueTargetPath = q->getTarget();
		qpos = queueTargetPath.rfind("\\");
		queueTargetPath = queueTargetPath.substr(0, qpos);

		//if (queueTargetPath.compare(targetPath) == 0)
			if (stricmp(queueTargetPath, targetPath) == 0) {
				totalSize += q->getSize();
			}
	}
	
	return totalSize;
}
*/
uint64_t QueueManager::FileQueue::getTotalQueueSize(){

	uint64_t totalsize = 0;

	for(auto i = queue.begin(); i != queue.end(); ++i) {
		totalsize += i->second->getSize();
	}

	if(totalsize < 0)
		totalsize = 0;

	return totalsize;
}

void QueueManager::updateDirSize(const string& path, const int64_t& size, bool add/*true*/){
	if(path.find("FileLists") != string::npos)
		return;

	int pos = path.rfind("\\");
	string tmp = path.substr(0, pos);
	StringIntMap::iterator i = dirSizeMap.find(tmp);

	if(add) {
		if( i == dirSizeMap.end() ) 
			dirSizeMap.insert(make_pair(tmp, size));
		else
			i->second += size;
	} else {
		if(i != dirSizeMap.end()) {
			i->second -= size;
			if(i->second <= 0)
				dirSizeMap.erase(i);
		}
	}
}
int64_t QueueManager::getDirSize(const string& path){
	if(path.find("FileLists") != string::npos)
		return 0;

	int pos = path.rfind("\\");

	string tmp = path.substr(0, pos);

	StringIntMap::iterator i = dirSizeMap.find(tmp);

	if(i == dirSizeMap.end())
		return 0;
	else
		return i->second;
}

} // namespace dcpp

/**
 * @file
 * $Id: QueueManager.cpp 568 2011-07-24 18:28:43Z bigmuscle $
 */
