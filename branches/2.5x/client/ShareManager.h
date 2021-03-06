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

#ifndef DCPLUSPLUS_DCPP_SHARE_MANAGER_H
#define DCPLUSPLUS_DCPP_SHARE_MANAGER_H

#include <string>
#include "TimerManager.h"
#include "SettingsManager.h"
#include "QueueManagerListener.h"

#include "AdcSearch.h"
#include "BloomFilter.h"
#include "Exception.h"
#include "Flags.h"
#include "HashBloom.h"
#include "HashedFile.h"
#include "LogManager.h"
#include "MerkleTree.h"
#include "Pointer.h"
#include "SearchManager.h"
#include "Singleton.h"
#include "ShareProfile.h"
#include "SortedVector.h"
#include "StringMatch.h"
#include "StringSearch.h"
#include "TaskQueue.h"
#include "Thread.h"

#include "DirectoryMonitor.h"
#include "DirectoryMonitorListener.h"
#include "DualString.h"

namespace dcpp {

STANDARD_EXCEPTION(ShareException);

class SimpleXML;
class Client;
class File;
class OutputStream;
class MemoryInputStream;
//struct ShareLoader;
class AdcSearch;
class Worker;
class TaskQueue;

class ShareDirInfo;
typedef boost::intrusive_ptr<ShareDirInfo> ShareDirInfoPtr;

class ShareDirInfo : public FastAlloc<ShareDirInfo>, public intrusive_ptr_base<ShareDirInfo> {
public:
	enum DiffState { 
		DIFF_NORMAL,
		DIFF_ADDED,
		DIFF_REMOVED
	};

	enum State { 
		STATE_NORMAL,
		STATE_ADDED,
		STATE_REMOVED
	};

	ShareDirInfo(const string& aVname, ProfileToken aProfile, const string& aPath, bool aIncoming=false) : vname(aVname), profile(aProfile), path(aPath), incoming(aIncoming), 
		found(false), diffState(DIFF_NORMAL), state(STATE_NORMAL), size(0) {}

	~ShareDirInfo() {}

	string vname;
	ProfileToken profile;
	string path;
	bool incoming;
	bool found; //used when detecting removed dirs with using dir tree
	int64_t size;

	DiffState diffState;
	State state;

	/*struct Hash {
		size_t operator()(const ShareDirInfo* x) const { return hash<string>()(x->path) + x->profile; }
	};

	typedef unordered_set<ShareDirInfoPtr, Hash> set;*/
	typedef vector<ShareDirInfoPtr> List;
	typedef unordered_map<int, List> Map;
};

class ShareProfile;
class FileList;

class ShareManager : public Singleton<ShareManager>, private Thread, private SettingsManagerListener, private TimerManagerListener, private QueueManagerListener, private DirectoryMonitorListener
{
public:
	/**
	 * @param aDirectory Physical directory location
	 * @param aName Virtual name
	 */

	void setSkipList();

	bool matchSkipList(const string& aStr) { return skipList.match(aStr); }
	bool checkSharedName(const string& fullPath, const string& fullPathLower, bool dir, bool report = true, int64_t size = 0);
	void validatePath(const string& realPath, const string& virtualName);

	string toVirtual(const TTHValue& tth, ProfileToken aProfile) const;
	pair<int64_t, string> getFileListInfo(const string& virtualFile, ProfileToken aProfile);
	void toRealWithSize(const string& virtualFile, const ProfileTokenSet& aProfiles, const HintedUser& aUser, string& path_, int64_t& size_, bool& noAccess_);
	TTHValue getListTTH(const string& virtualFile, ProfileToken aProfile) const;
	
	enum RefreshType {
		TYPE_MANUAL,
		TYPE_SCHEDULED,
		TYPE_STARTUP,
		TYPE_MONITORING,
		TYPE_BUNDLE
	};

	int refresh(bool incoming, RefreshType aType, function<void (float)> progressF = nullptr);
	int refresh(const string& aDir);

	bool isRefreshing() {	return refreshRunning; }
	
	//need to be called from inside a lock.
	void setProfilesDirty(ProfileTokenSet aProfiles, bool forceXmlRefresh=false);

	void startup(function<void (const string&)> stepF, function<void (float)> progressF);
	void shutdown(function<void (float)> progressF);
	void abortRefresh();

	void changeExcludedDirs(const ProfileTokenStringList& aAdd, const ProfileTokenStringList& aRemove);
	void rebuildTotalExcludes();

	void search(SearchResultList& l, const string& aString, int aSearchType, int64_t aSize, int aFileType, StringList::size_type maxResults, bool aHideShare) noexcept;
	void search(SearchResultList& l, AdcSearch& aSearch, StringList::size_type maxResults, ProfileToken aProfile, const CID& cid, const string& aDir);

	bool isDirShared(const string& aDir) const;
	uint8_t isDirShared(const string& aPath, int64_t aSize) const;
	bool isFileShared(const TTHValue& aTTH, const string& fileName) const;
	bool isFileShared(const string& aFileName, int64_t aSize) const;
	bool isFileShared(const TTHValue& aTTH, const string& fileName, ProfileToken aProfile) const;

	bool allowAddDir(const string& dir);
	string getDirPath(const string& directory);

	bool loadCache(function<void (float)> progressF);

	vector<pair<string, StringList>> getGroupedDirectories() const noexcept;
	static bool checkType(const string& aString, int aType);
	MemoryInputStream* generatePartialList(const string& dir, bool recurse, ProfileToken aProfile) const;
	MemoryInputStream* generateTTHList(const string& dir, bool recurse, ProfileToken aProfile) const;
	MemoryInputStream* getTree(const string& virtualFile, ProfileToken aProfile) const;

	void saveXmlList(bool verbose=false, function<void (float)> progressF = nullptr);	//for filelist caching

	AdcCommand getFileInfo(const string& aFile, ProfileToken aProfile);

	int64_t getTotalShareSize(ProfileToken aProfile) const noexcept;
	int64_t getShareSize(const string& realPath, ProfileToken aProfile) const noexcept;
	void getProfileInfo(ProfileToken aProfile, int64_t& size, size_t& files) const;
	
	void getBloom(HashBloom& bloom) const;

	static SearchManager::TypeModes getType(const string& fileName) noexcept;

	string validateVirtual(const string& /*aVirt*/) const noexcept;
	void addHits(uint32_t aHits) {
		hits += aHits;
	}

	string generateOwnList(ProfileToken aProfile);

	bool isTTHShared(const TTHValue& tth) const;

	void getRealPaths(const string& path, StringList& ret, ProfileToken aProfile) const;

	string getRealPath(const TTHValue& root) const;
	string getRealPath(const string& aFileName, int64_t aSize) const;

	enum { 
		REFRESH_STARTED = 0,
		REFRESH_PATH_NOT_FOUND = 1,
		REFRESH_IN_PROGRESS = 2,
		REFRESH_ALREADY_QUEUED = 3
	};

	GETSET(bool, monitorDebug, MonitorDebug);
	GETSET(size_t, hits, Hits);
	GETSET(int64_t, sharedSize, SharedSize);

	//tempShares
	struct TempShareInfo {
		TempShareInfo(const string& aKey, const string& aPath, int64_t aSize) : key(aKey), path(aPath), size(aSize) { }
		
		string key; //CID or hubUrl
		string path; //filepath
		int64_t size; //filesize
	};

	typedef unordered_multimap<TTHValue, TempShareInfo> TempShareMap;
	TempShareMap tempShares;
	void addTempShare(const string& aKey, const TTHValue& tth, const string& filePath, int64_t aSize, ProfileToken aProfile);

	// GUI only
	bool hasTempShares() { return !tempShares.empty(); }
	TempShareMap& getTempShares() { return tempShares; }

	void removeTempShare(const string& aKey, const TTHValue& tth);
	bool isTempShared(const string& aKey, const TTHValue& tth);
	//tempShares end

	typedef vector<ShareProfilePtr> ShareProfileList;

	void getShares(ShareDirInfo::Map& aDirs);

	enum Tasks {
		ASYNC,
		ADD_DIR,
		REFRESH_ALL,
		REFRESH_DIRS,
		REFRESH_INCOMING,
		ADD_BUNDLE
	};

	ShareProfilePtr getShareProfile(ProfileToken aProfile, bool allowFallback=false) const;
	void getParentPaths(StringList& aDirs) const;

	void addDirectories(const ShareDirInfo::List& aNewDirs);
	void removeDirectories(const ShareDirInfo::List& removeDirs);
	void changeDirectories(const ShareDirInfo::List& renameDirs);

	void addProfiles(const ShareProfile::set& aProfiles);
	void removeProfiles(ProfileTokenList aProfiles);

	bool isRealPathShared(const string& aPath);
	ShareProfilePtr getProfile(ProfileToken aProfile) const;

	/* Only for gui use purposes, no locking */
	const ShareProfileList& getProfiles() { return shareProfiles; }
	void getExcludes(ProfileToken aProfile, StringList& excludes);

	string getStats() const;
	mutable SharedMutex cs;

	int addRefreshTask(uint8_t aTaskType, StringList& dirs, RefreshType aRefreshType, const string& displayName=Util::emptyString, function<void (float)> progressF = nullptr) noexcept;
	struct ShareLoader;

	void rebuildMonitoring();
	void handleChangedFiles();
private:
	unique_ptr<DirectoryMonitor> monitor;

	uint64_t totalSearches;
	typedef BloomFilter<5> ShareBloom;

	class ProfileDirectory : public intrusive_ptr_base<ProfileDirectory>, boost::noncopyable, public Flags {
		public:
			typedef boost::intrusive_ptr<ProfileDirectory> Ptr;

			ProfileDirectory(const string& aRootPath, const string& aVname, ProfileToken aProfile, bool incoming = false);
			ProfileDirectory(const string& aRootPath, ProfileToken aProfile);

			GETSET(string, path, Path);

			//lists the profiles where this directory is set as root and virtual names
			GETSET(ProfileTokenStringMap, rootProfiles, RootProfiles);
			GETSET(ProfileTokenSet, excludedProfiles, ExcludedProfiles);
			GETSET(bool, cacheDirty, CacheDirty);

			~ProfileDirectory() { }

			enum InfoFlags {
				FLAG_ROOT				= 0x01,
				FLAG_EXCLUDE_TOTAL		= 0x02,
				FLAG_EXCLUDE_PROFILE	= 0x04,
				FLAG_INCOMING			= 0x08
			};

			bool hasExcludes() const { return !excludedProfiles.empty(); }
			bool hasRoots() const { return !rootProfiles.empty(); }

			bool hasRootProfile(ProfileToken aProfile) const;
			bool hasRootProfile(const ProfileTokenSet& aProfiles) const;
			bool isExcluded(ProfileToken aProfile) const;
			bool isExcluded(const ProfileTokenSet& aProfiles) const;
			void addRootProfile(const string& aName, ProfileToken aProfile);
			void addExclude(ProfileToken aProfile);
			bool removeRootProfile(ProfileToken aProfile);
			bool removeExcludedProfile(ProfileToken aProfile);
			string getName(ProfileToken aProfile) const;

			string getCacheXmlPath() const;
	};

	unique_ptr<ShareBloom> bloom;

	struct FileListDir;
	class Directory : public intrusive_ptr_base<Directory>, boost::noncopyable {
	public:
		typedef boost::intrusive_ptr<Directory> Ptr;
		typedef unordered_map<string, Ptr, noCaseStringHash, noCaseStringEq> Map;
		typedef Map::iterator MapIter;

		struct NameLower {
			const string& operator()(const Ptr& a) const { return a->name.getLower(); }
		};

		class File {
		public:
			/*struct FileLess {
				bool operator()(const File& a, const File& b) const { return strcmp(a.name.getLower().c_str(), b.name.getLower().c_str()) < 0; }
			};*/

			struct NameLower {
				const string& operator()(const File* a) const { return a->name.getLower(); }
			};

			//typedef set<File, FileLess> Set;
			typedef SortedVector<File*, std::vector, string, Compare, NameLower> Set;

			File(DualString&& aName, const Directory::Ptr& aParent, HashedFile& aFileInfo);
			~File();

			/*bool operator==(const File& rhs) const {
				return name.getLower().compare(rhs.name.getLower()) == 0 && parent == rhs.getParent();
			}*/
		
			string getADCPath(ProfileToken aProfile) const { return parent->getADCPath(aProfile) + name.getNormal(); }
			string getFullName(ProfileToken aProfile) const { return parent->getFullName(aProfile) + name.getNormal(); }
			string getRealPath(bool validate = true) const { return parent->getRealPath(name.getNormal(), validate); }
			bool hasProfile(ProfileToken aProfile) const { return parent->hasProfile(aProfile); }

			void toXml(OutputStream& xmlFile, string& indent, string& tmp2, bool addDate) const;
			void addSR(SearchResultList& aResults, ProfileToken aProfile, bool addParent) const;

			GETSET(int64_t, size, Size);
			GETSET(Directory*, parent, Parent);
			GETSET(uint64_t, lastWrite, LastWrite);
			GETSET(TTHValue, tth, TTH);

			DualString name;
		private:
			File(const File* src);
		};

		//typedef set<Directory::Ptr, DirLess> Set;

		typedef SortedVector<Ptr, std::vector, string, Compare, NameLower> Set;
		Set directories;
		File::Set files;

		static Ptr create(DualString&& aRealName, const Ptr& aParent, uint64_t aLastWrite, ProfileDirectory::Ptr aRoot = nullptr);

		struct HasRootProfile {
			HasRootProfile(ProfileToken aT) : t(aT) { }
			bool operator()(const Ptr& d) const {
				return d->getProfileDir()->hasRootProfile(t);
			}
			ProfileToken t;
		private:
			HasRootProfile& operator=(const HasRootProfile&);
		};

		struct IsParent {
			bool operator()(const Ptr& d) const {
				return !d->getParent();
			}
		};

		bool hasType(uint32_t type) const noexcept {
			return ( (type == SearchManager::TYPE_ANY) || (fileTypes & (1 << type)) );
		}
		void addType(uint32_t type) noexcept;

		string getADCPath(ProfileToken aProfile) const noexcept;
		string getVirtualName(ProfileToken aProfile) const noexcept;
		string getFullName(ProfileToken aProfile) const noexcept; 
		string getRealPath(bool checkExistance) const { return getRealPath(Util::emptyString, checkExistance); };

		bool hasProfile(const ProfileTokenSet& aProfiles) const noexcept;
		bool hasProfile(ProfileToken aProfiles) const noexcept;

		void getResultInfo(ProfileToken aProfile, int64_t& size_, size_t& files_, size_t& folders_) const noexcept;
		int64_t getSize(ProfileToken aProfile) const noexcept;
		int64_t getTotalSize() const noexcept;
		void getProfileInfo(ProfileToken aProfile, int64_t& totalSize, size_t& filesCount) const;

		void search(SearchResultList& aResults, StringSearch::List& aStrings, int aSearchType, int64_t aSize, int aFileType, StringList::size_type maxResults) const noexcept;
		void search(SearchResultList& aResults, AdcSearch& aStrings, StringList::size_type maxResults, ProfileToken aProfile) const noexcept;

		void toFileList(FileListDir* aListDir, ProfileToken aProfile, bool isFullList);
		void toXml(SimpleXML& aXml, bool fullList, ProfileToken aProfile) const;
		void toTTHList(OutputStream& tthList, string& tmp2, bool recursive) const;

		//for file list caching
		void toXmlList(OutputStream& xmlFile, string&& path, string& indent, string& tmp);
		void filesToXmlList(OutputStream& xmlFile, string& indent, string& tmp2) const;

		GETSET(uint64_t, lastWrite, LastWrite);
		GETSET(Directory*, parent, Parent);
		GETSET(ProfileDirectory::Ptr, profileDir, ProfileDir);

		Directory(DualString&& aRealName, const Ptr& aParent, uint64_t aLastWrite, ProfileDirectory::Ptr root = nullptr);
		~Directory();

		void copyRootProfiles(ProfileTokenSet& aProfiles, bool setCacheDirty) const;
		bool isRootLevel(ProfileToken aProfile) const;
		bool isLevelExcluded(ProfileToken aProfile) const;
		bool isLevelExcluded(const ProfileTokenSet& aProfiles) const;
		int64_t size;

		void addBloom(ShareBloom& aBloom) const;

		void getStats(uint64_t& totalAge_, size_t& totalDirs_, int64_t& totalSize_, size_t& totalFiles, size_t& lowerCaseFiles, size_t& totalStrLen_) const;
		DualString name;

		void getRenameInfoList(const string& aPath, RenameList& aRename);
	private:
		friend void intrusive_ptr_release(intrusive_ptr_base<Directory>*);
		/** Set of flags that say which SearchManager::TYPE_* a directory contains */
		uint32_t fileTypes;

		string getRealPath(const string& path, bool checkExistance) const;
	};

	struct FileListDir {
		typedef unordered_map<string, FileListDir*, noCaseStringHash, noCaseStringEq> ListDirectoryMap;
		vector<Directory::Ptr> shareDirs;

		FileListDir(const string& aName, int64_t aSize, int aDate);
		~FileListDir();

		string name;
		int64_t size;
		uint64_t date;
		ListDirectoryMap listDirs;

		void toXml(OutputStream& xmlFile, string& indent, string& tmp2, bool fullList) const;
		void filesToXml(OutputStream& xmlFile, string& indent, string& tmp2, bool addDate) const;
	};

	void addAsyncTask(AsyncF aF);
	Directory::Ptr getDirByName(const string& directory) const;

	/* Directory items mapped to realpath*/
	typedef map<string, Directory::Ptr> DirMap;

	void addRoot(const string& aPath, Directory::Ptr& aDir);
	DirMap::const_iterator findRoot(const string& aPath) const;

	friend class Singleton<ShareManager>;

	typedef unordered_multimap<TTHValue*, const Directory::File*> HashFileMap;
	HashFileMap tthIndex;
	
	ShareManager();
	~ShareManager();

	struct TaskData {
		virtual ~TaskData() { }
	};

	struct RefreshTask : public TaskData {
		RefreshTask(int refreshOptions_) : refreshOptions(refreshOptions_) { }
		int refreshOptions;
	};

	bool addDirResult(const string& aPath, SearchResultList& aResults, ProfileToken aProfile, AdcSearch& srch) const;
	typedef unordered_map<string, ProfileDirectory::Ptr, noCaseStringHash, noCaseStringEq> ProfileDirMap;
	ProfileDirMap profileDirs;

	ProfileDirMap getSubProfileDirs(const string& aPath);

	TaskQueue tasks;

	FileList* generateXmlList(ProfileToken aProfile, bool forced = false);
	FileList* getFileList(ProfileToken aProfile) const;

	volatile bool aShutdown;

	static boost::regex rxxReg;
	
	static atomic_flag refreshing;
	bool refreshRunning;

	uint64_t lastFullUpdate;
	uint64_t lastIncomingUpdate;
	uint64_t lastSave;
	uint64_t findLastWrite(const string& aName) const;
	
	//caching the share size so we dont need to loop tthindex everytime
	bool xml_saving;

	mutable SharedMutex dirNames; // Bundledirs, releasedirs and excluded dirs

	int refreshOptions;

	/*
	multimap to allow multiple same key values, needed to return from some functions.
	*/
	typedef unordered_multimap<string*, Directory::Ptr, noCaseStringHash, noCaseStringEq> DirMultiMap; 

	//list to return multiple directory item pointers
	typedef std::vector<Directory::Ptr> DirectoryList;

	/** Map real name to virtual name - multiple real names may be mapped to a single virtual one */
	DirMap rootPaths;
	DirMultiMap dirNameMap;

	class RefreshInfo {
	public:
		RefreshInfo(const string& aPath, const Directory::Ptr& aOldRoot, uint64_t aLastWrite);
		~RefreshInfo();

		Directory::Ptr oldRoot;
		Directory::Ptr root;
		int64_t hashSize;
		int64_t addedSize;
		ProfileDirMap subProfiles;
		DirMultiMap dirNameMapNew;
		HashFileMap tthIndexNew;
		DirMap rootPathsNew;

		string path;

		/*struct FileCount {
			int64_t operator()(const RefreshInfo& ri) const {
				return ri.root->getProfileDir() ? ri.root->getProfileDir()->getRootProfiles();
			}
		};*/
	private:
		RefreshInfo(const RefreshInfo&);
		RefreshInfo& operator=(const RefreshInfo&);
	};

	typedef std::list<RefreshInfo> RefreshInfoList;

	//void mergeRefreshChanges(RefreshInfoList& aList, DirMultiMap& aDirNameMap, DirMap& aRootPaths, HashFileMap& aTTHIndex, int64_t& totalHash, int64_t& totalAdded, ProfileTokenSet* dirtyProfiles);
	template<typename T>
	void mergeRefreshChanges(T& aList, DirMultiMap& aDirNameMap, DirMap& aRootPaths, HashFileMap& aTTHIndex, int64_t& totalHash, int64_t& totalAdded, ProfileTokenSet* dirtyProfiles) {
		for (auto& ri: aList) {
			aDirNameMap.insert(ri.dirNameMapNew.begin(), ri.dirNameMapNew.end());
			aRootPaths.insert(ri.rootPathsNew.begin(), ri.rootPathsNew.end());
			aTTHIndex.insert(ri.tthIndexNew.begin(), ri.tthIndexNew.end());

			totalHash += ri.hashSize;
			totalAdded += ri.addedSize;

			if (dirtyProfiles)
				ri.root->copyRootProfiles(*dirtyProfiles, true);
		}
	}

	void buildTree(string& aPath, string& aPathLower, const Directory::Ptr& aDir, const ProfileDirMap& aSubRoots, DirMultiMap& aDirs, DirMap& newShares, int64_t& hashSize, int64_t& addedSize, HashFileMap& tthIndexNew, ShareBloom& aBloom);
	bool checkHidden(const string& aName) const;
	void addFile(const string& aName, Directory::Ptr& aDir, HashedFile& fi, ProfileTokenSet& dirtyProfiles_);

	//void rebuildIndices();
	static void updateIndices(Directory::Ptr& aDirectory, ShareBloom& aBloom, int64_t& sharedSize, HashFileMap& tthIndex, DirMultiMap& aDirNames);
	static void updateIndices(Directory& dir, const Directory::File* f, ShareBloom& aBloom, int64_t& sharedSize, HashFileMap& tthIndex);
	void cleanIndices(Directory& dir);
	void removeDirName(Directory& dir);
	void cleanIndices(Directory& dir, const Directory::File* f);

	void onFileHashed(const string& fname, HashedFile& fileInfo);
	
	StringList bundleDirs;

	void getByVirtual(const string& virtualName, ProfileToken aProfiles, DirectoryList& dirs) const noexcept;
	void getByVirtual(const string& virtualName, const ProfileTokenSet& aProfiles, DirectoryList& dirs) const noexcept;
	//void findVirtuals(const string& virtualPath, ProfileToken aProfiles, DirectoryList& dirs) const;

	template<class T>
	void findVirtuals(const string& virtualPath, const T& aProfile, DirectoryList& dirs) const {

		DirectoryList virtuals; //since we are mapping by realpath, we can have more than 1 same virtualnames
		if(virtualPath.empty() || virtualPath[0] != '/') {
			throw ShareException(UserConnection::FILE_NOT_AVAILABLE);
		}

		string::size_type start = virtualPath.find('/', 1);
		if(start == string::npos || start == 1) {
			throw ShareException(UserConnection::FILE_NOT_AVAILABLE);
		}

		getByVirtual( virtualPath.substr(1, start-1), aProfile, virtuals);
		if(virtuals.empty()) {
			throw ShareException(UserConnection::FILE_NOT_AVAILABLE);
		}

		Directory::Ptr d;
		for(auto k = virtuals.begin(); k != virtuals.end(); k++) {
			string::size_type i = start; // always start from the begin.
			string::size_type j = i + 1;
			d = *k;

			while((i = virtualPath.find('/', j)) != string::npos) {
				auto mi = d->directories.find(Text::toLower(virtualPath.substr(j, i - j)));
				j = i + 1;
				if(mi != d->directories.end() && !(*mi)->isLevelExcluded(aProfile)) {   //if we found something, look for more.
					d = *mi;
				} else {
					d = nullptr;   //make the pointer null so we can check if found something or not.
					break;
				}
			}

			if(d) 
				dirs.push_back(d);
		}

		if(dirs.empty()) {
			//if we are here it means we didnt find anything, throw.
			throw ShareException(UserConnection::FILE_NOT_AVAILABLE);
		}
	}

	/*template<class T>
	Directory::Ptr findDirectory(const string& virtualPath, const T& aProfile, const Directory::Ptr& aDir) const noexcept {
		string::size_type i = 0; // always start from the begin.
		string::size_type j = 1;

		Directory::Ptr d = aDir;
		while((i = virtualPath.find('/', j)) != string::npos) {
			auto mi = d->directories.find(virtualPath.substr(j, i - j));
			j = i + 1;
			if(mi != d->directories.end() && !(*mi)->isLevelExcluded(aProfile)) {   //if we found something, look for more.
				d = *mi;
			} else {
				return nullptr;
			}
		}

		return d;
	}*/

	string findRealRoot(const string& virtualRoot, const string& virtualLeaf) const;

	Directory::Ptr findDirectory(const string& fname, bool allowAdd, bool report, bool checkExcludes=true);

	virtual int run();

	void runTasks(function<void (float)> progressF = nullptr);

	// QueueManagerListener
	virtual void on(QueueManagerListener::BundleAdded, const BundlePtr& aBundle) noexcept;
	virtual void on(QueueManagerListener::BundleStatusChanged, const BundlePtr& aBundle) noexcept;
	virtual void on(QueueManagerListener::FileHashed, const string& aPath, HashedFile& aFileInfo) noexcept { onFileHashed(aPath, aFileInfo); }

	// SettingsManagerListener
	void on(SettingsManagerListener::Save, SimpleXML& xml) noexcept {
		save(xml);
	}
	void on(SettingsManagerListener::Load, SimpleXML& xml) noexcept {
		load(xml);
	}
	
	// TimerManagerListener
	void on(TimerManagerListener::Second, uint64_t tick) noexcept;
	void on(TimerManagerListener::Minute, uint64_t tick) noexcept;


	//DirectoryMonitorListener
	virtual void on(DirectoryMonitorListener::FileCreated, const string& aPath) noexcept;
	virtual void on(DirectoryMonitorListener::FileModified, const string& aPath) noexcept;
	virtual void on(DirectoryMonitorListener::FileRenamed, const string& aOldPath, const string& aNewPath) noexcept;
	virtual void on(DirectoryMonitorListener::FileDeleted, const string& aPath) noexcept;
	virtual void on(DirectoryMonitorListener::Overflow, const string& aPath) noexcept;
	virtual void on(DirectoryMonitorListener::DirectoryFailed, const string& aPath, const string& aError) noexcept;

	//Directory::Ptr ShareManager::removeFileOrDirectory(Directory::Ptr& aDir, const string& aName) throw(ShareException);

	void load(SimpleXML& aXml);
	void loadProfile(SimpleXML& aXml, const string& aName, ProfileToken aToken);
	void save(SimpleXML& aXml);

	void reportTaskStatus(uint8_t aTask, const StringList& aDirectories, bool finished, int64_t aHashSize, const string& displayName, RefreshType aRefreshType);
	
	ShareProfileList shareProfiles;

	StringMatch skipList;
	string winDir;

	void addMonitoring(const StringList& aPaths);
	void removeMonitoring(const StringList& aPaths);

	struct DirModifyInfo {
		typedef deque<DirModifyInfo> List;
		enum ActionType {
			ACTION_NONE,
			ACTION_MODIFIED,
			ACTION_DELETED
		};

		//DirModifyInfo(ActionType aAction) : lastFileActivity(GET_TICK()), lastReportedError(0), dirAction(aAction) { }
		DirModifyInfo(const string& aFile, bool isDirectory, ActionType aAction);

		void addFile(const string& aFile, ActionType aAction);

		unordered_map<string, ActionType> files;
		time_t lastFileActivity;
		time_t lastReportedError;

		ActionType dirAction;
		string volume;
		string path;

		void setPath(const string& aPath);
	};

	typedef set<string, Util::PathSortOrderBool> PathSet;
	struct FileAddInfo {
		FileAddInfo(string&& aName, uint64_t aLastWrite, int64_t aSize) : name(aName), lastWrite(aLastWrite), size(aSize) { }

		string name;
		uint64_t lastWrite;
		int64_t size;
	};

	DirModifyInfo::List fileModifications;

	void onFileModified(const string& aPath, bool created);
	void addModifyInfo(const string& aPath, bool isDirectory, DirModifyInfo::ActionType aAction);
	bool handleDeletedFile(const string& aPath, bool isDirectory, ProfileTokenSet& dirtyProfiles_);

	// Removes all notifications for the selected path
	void removeNotifications(const string& aPath);
	DirModifyInfo::List::iterator findModifyInfo(const string& aFile);
	void handleChangedFiles(uint64_t aTick, bool forced=false);
}; //sharemanager end

} // namespace dcpp

#endif // !defined(SHARE_MANAGER_H)
