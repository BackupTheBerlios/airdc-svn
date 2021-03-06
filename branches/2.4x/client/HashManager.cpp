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
#include "HashManager.h"

#include "Util.h"
#include "ResourceManager.h"
#include "SimpleXML.h"
#include "LogManager.h"
#include "File.h"
#include "FileReader.h"
#include "ZUtils.h"
#include "ShareManager.h"
#include "AirUtil.h"

namespace dcpp {

#ifdef ATOMIC_FLAG_INIT
atomic_flag HashManager::HashStore::saving = ATOMIC_FLAG_INIT;
#else
atomic_flag HashManager::HashStore::saving;
#endif

using boost::range::find_if;

SharedMutex HashManager::Hasher::hcs;

#define HASH_FILE_VERSION_STRING "2"
static const uint32_t HASH_FILE_VERSION = 2;
const int64_t HashManager::MIN_BLOCK_SIZE = 64 * 1024;

HashManager::HashManager(): lastSave(0), pausers(0), aShutdown(false) {
	TimerManager::getInstance()->addListener(this);
}

HashManager::~HashManager() {
	TimerManager::getInstance()->removeListener(this);
}

bool HashManager::checkTTH(const string& aFileName, int64_t aSize, uint32_t aTimeStamp) {
	if (!store.checkTTH(Text::toLower(aFileName), aSize, aTimeStamp)) {
		hashFile(aFileName, aSize);
		return false;
	}
	return true;
}

TTHValue HashManager::getTTH(const string& aFileName, int64_t aSize) {
	const TTHValue* tth = store.getTTH(Text::toLower(aFileName));
	if (!tth) {
		hashFile(aFileName, aSize);
		throw HashException();
	}
	return *tth;
}

bool HashManager::getTree(const TTHValue& root, TigerTree& tt) {
	return store.getTree(root, tt);
}

size_t HashManager::getBlockSize(const TTHValue& root) {
	return store.getBlockSize(root);
}

int64_t HashManager::Hasher::getTimeLeft() const {
	return lastSpeed > 0 ? (totalBytesLeft / lastSpeed) : 0;
}

void HashManager::hashFile(const string& fileName, int64_t size) {
	if(aShutdown) //we cant allow adding more hashers if we are shutting down, it will result in infinite loop
		return;

	TCHAR* buf = new TCHAR[fileName.length()];
	GetVolumePathName(Text::toT(fileName).c_str(), buf, fileName.length());
	auto vol = Text::fromT(buf);
	delete[] buf;
	dcassert(!vol.empty());

	Hasher* h = nullptr;

	WLock l(Hasher::hcs);
	if (hashers.size() == 1 && !hashers.front()->hasDevices()) {
		//always use the first hasher if it's idle
		h = hashers.front();
	} else {
		auto getLeastLoaded = [](const HasherList& hl) {
			return min_element(hl.begin(), hl.end(), [](const Hasher* h1, const Hasher* h2) { return h1->getBytesLeft() < h2->getBytesLeft(); });
		};

		if (SETTING(HASHERS_PER_VOLUME) == 1) {
			//do we have files for this volume queued already? always use the same one in that case
			auto p = find_if(hashers, [&vol](const Hasher* aHasher) { return aHasher->hasDevice(vol); });
			if (p != hashers.end()) {
				h = *p;
			} else if (hashers.size() >= SETTING(MAX_HASHING_THREADS)) {
				// can't create new ones
				h = *getLeastLoaded(hashers);
			}
		} else {
			//get the hashers with this volume
			HasherList volHashers;
			copy_if(hashers.begin(), hashers.end(), back_inserter(volHashers), [&vol](const Hasher* aHasher) { return aHasher->hasDevice(vol); });

			if (volHashers.empty() && hashers.size() >= SETTING(MAX_HASHING_THREADS)) {
				//we just need choose from all hashers
				h = *getLeastLoaded(hashers);
			} else {
				auto minLoaded = getLeastLoaded(volHashers);

				//don't create new hashers if the file is less than 10 megabytes and there's a hasher with less than 200MB queued, or the maximum number of threads have been reached for this volume
				if (hashers.size() >= SETTING(MAX_HASHING_THREADS) || volHashers.size() >= SETTING(HASHERS_PER_VOLUME) || (size <= 10*1024*1024 && !volHashers.empty() && (*minLoaded)->getBytesLeft() <= 200*1024*1024)) {
					//use the least loaded hasher that already has this volume
					h = *minLoaded;
				}
			}
		}

		if (!h) {
			//add a new one
			int id = 0;
			for (auto i: hashers) {
				if (i->hasherID != id)
					break;
				id++;
			}

			LogManager::getInstance()->message(STRING_F(HASHER_X_CREATED, id), LogManager::LOG_INFO);
			h = new Hasher(pausers > 0, id);
			hashers.push_back(h);
		}
	}

	//queue the file for hashing
	h->hashFile(fileName, size, vol);
}

void HashManager::getFileTTH(const string& aFile, int64_t aSize, bool addStore, TTHValue& tth_, int64_t& sizeLeft_, const bool& aCancel, std::function<void (int64_t, const string&)> updateF/*nullptr*/) {
	if (!store.checkTTH(Text::toLower(aFile), aSize, AirUtil::getLastWrite(aFile))) {
		File f(aFile, File::READ, File::OPEN);
		int64_t bs = max(TigerTree::calcBlockSize(aSize, 10), MIN_BLOCK_SIZE);
		uint64_t timestamp = f.getLastModified();
		TigerTree tt(bs);

		auto start = GET_TICK();
		int64_t tickHashed = 0;

		FileReader fr(true);
		fr.read(aFile, [&](const void* buf, size_t n) -> bool {
			tt.update(buf, n);

			if (updateF) {
				tickHashed += n;

				uint64_t end = GET_TICK();
				if (end - start > 1000) {
					sizeLeft_ -= tickHashed;
					auto lastSpeed = tickHashed * 1000 / (end - start);

					updateF(lastSpeed > 0 ? (sizeLeft_ / lastSpeed) : 0, aFile);

					tickHashed = 0;
					start = end;
				}
			}
			return !aCancel;
		});

		f.close();
		tt.finalize();
		tth_ = tt.getRoot();

		if (addStore && !aCancel)
			store.addFile(Text::toLower(aFile), timestamp, tt, true);
	} else {
		tth_ = *store.getTTH(Text::toLower(aFile));
	}
}

void HashManager::hashDone(const string& aFileName, uint64_t aTimeStamp, const TigerTree& tth, int64_t speed, int64_t /*size*/, int hasherID /*0*/) {
	try {
		store.addFile(Text::toLower(aFileName), aTimeStamp, tth, true);
	} catch (const Exception& e) {
		log(STRING(HASHING_FAILED) + " " + e.getError(), hasherID, true, true);
		return;
	}
	
	if(SETTING(LOG_HASHING)) {
		string fn = aFileName;
		if (count(fn.begin(), fn.end(), PATH_SEPARATOR) >= 2) {
			string::size_type i = fn.rfind(PATH_SEPARATOR);
			i = fn.rfind(PATH_SEPARATOR, i - 1);
			fn.erase(0, i);
			fn.insert(0, "...");
		}
	
		if (speed > 0) {
			log(STRING(HASHING_FINISHED) + " " + fn + " (" + Util::formatBytes(speed) + "/s)", hasherID, false, true);
		} else {
			log(STRING(HASHING_FINISHED) + " " + fn, hasherID, false, true);
		}
	}
}

void HashManager::HashStore::addFile(const string&& aFileLower, uint64_t aTimeStamp, const TigerTree& tth, bool aUsed) {
	addTree(tth);

	WLock l(cs);
	auto& fileList = fileIndex[Util::getFilePath(aFileLower)];
	auto j = find(fileList.begin(), fileList.end(), Util::getFileName(aFileLower));
	if (j != fileList.end()) {
		fileList.erase(j);
	}

	fileList.emplace_back(Util::getFileName(aFileLower), tth.getRoot(), aTimeStamp, aUsed);
	dirty = true;
}

void HashManager::HashStore::addTree(const TigerTree& tt) noexcept {
	WLock l(cs);
	if (treeIndex.find(tt.getRoot()) == treeIndex.end()) {
		try {
			File f(getDataFile(), File::READ | File::WRITE, File::OPEN | File::SHARED);
			int64_t index = saveTree(f, tt);
			treeIndex.emplace(tt.getRoot(), TreeInfo(tt.getFileSize(), index, tt.getBlockSize()));
			dirty = true;
		} catch (const FileException& e) {
			LogManager::getInstance()->message(STRING(ERROR_SAVING_HASH) + " " + e.getError(), LogManager::LOG_ERROR);
		}
	}
}

int64_t HashManager::HashStore::saveTree(File& f, const TigerTree& tt) {
	if (tt.getLeaves().size() == 1)
		return SMALL_TREE;

	f.setPos(0);
	int64_t pos = 0;
	size_t n = sizeof(pos);
	if (f.read(&pos, n) != sizeof(pos))
		throw HashException(STRING(HASH_READ_FAILED));

	// Check if we should grow the file, we grow by a meg at a time...
	int64_t datsz = f.getSize();
	if ((pos + (int64_t) (tt.getLeaves().size() * TTHValue::BYTES)) >= datsz) {
		f.setPos(datsz + 1024 * 1024);
		f.setEOF();
	}
	f.setPos(pos); dcassert(tt.getLeaves().size() > 1);
	f.write(tt.getLeaves()[0].data, (tt.getLeaves().size() * TTHValue::BYTES));
	int64_t p2 = f.getPos();
	f.setPos(0);
	f.write(&p2, sizeof(p2));
	return pos;
}

bool HashManager::HashStore::loadTree(File& f, const TreeInfo& ti, const TTHValue& root, TigerTree& tt, bool rebuilding /*false*/) {
	if (ti.getIndex() == SMALL_TREE) {
		tt = TigerTree(ti.getSize(), ti.getBlockSize(), root);
		return true;
	}
	try {
		f.setPos(ti.getIndex());
		size_t datalen = TigerTree::calcBlocks(ti.getSize(), ti.getBlockSize()) * TTHValue::BYTES;
		boost::scoped_array<uint8_t> buf(new uint8_t[datalen]);
		f.read(&buf[0], datalen);
		tt = TigerTree(ti.getSize(), ti.getBlockSize(), &buf[0]);
		if (!(tt.getRoot() == root))
			throw HashException(STRING(INVALID_TREE));
	} catch (const Exception& e) {
		if (!rebuilding)
			LogManager::getInstance()->message(STRING_F(TREE_LOAD_FAILED, root.toBase32() % e.getError()), LogManager::LOG_ERROR);
		return false;
	}

	return true;
}

bool HashManager::HashStore::getTree(const TTHValue& root, TigerTree& tt) {
	RLock l(cs);
	auto i = treeIndex.find(root);
	if (i == treeIndex.end())
		return false;
	try {
		File f(getDataFile(), File::READ, File::OPEN | File::SHARED | File::RANDOM_ACCESS);
		return loadTree(f, i->second, root, tt);
	} catch (const Exception&) {
		return false;
	}
}

size_t HashManager::HashStore::getBlockSize(const TTHValue& root) const {
	RLock l(cs);
	auto i = treeIndex.find(root);
	return i == treeIndex.end() ? 0 : static_cast<size_t>(i->second.getBlockSize());
}

bool HashManager::HashStore::checkTTH(const string&& aFileLower, int64_t aSize, uint32_t aTimeStamp) {
	RLock l(cs);
	auto i = fileIndex.find(Util::getFilePath(aFileLower));
	if (i != fileIndex.end()) {
		auto j = find(i->second.begin(), i->second.end(), Util::getFileName(aFileLower));
		if (j != i->second.end()) {
			auto& fi = *j;
			auto ti = treeIndex.find(fi.getRoot());
			if (ti == treeIndex.end() || ti->second.getSize() != aSize || fi.getTimeStamp() != aTimeStamp) {
				i->second.erase(j);
				dirty = true;
				return false;
			}
			return true;
		}
	} 
	return false;
}

const TTHValue* HashManager::HashStore::getTTH(const string&& aFileLower) {
	RLock l(cs);
	auto i = fileIndex.find(Util::getFilePath(aFileLower));
	if (i != fileIndex.end()) {
		auto j = find(i->second.begin(), i->second.end(), Util::getFileName(aFileLower));
		if (j != i->second.end()) {
			j->setUsed(true);
			return &(j->getRoot());
		}
	}
	return nullptr;
}

void HashManager::HashStore::rebuild() {
	try {
		decltype(fileIndex) newFileIndex;
		decltype(treeIndex) newTreeIndex;

		size_t initialTreeIndexSize = 0, finalTreeIndexSize=0;
		int failedTrees = 0;
		int unusedFiles=0; 
		int64_t failedSize = 0;

		{
			RLock l(cs);
			initialTreeIndexSize = treeIndex.size();
			for (auto& i: fileIndex) {
				for (auto& j: i.second) {
					if (!j.getUsed())
						continue;

					auto k = treeIndex.find(j.getRoot());
					if (k != treeIndex.end()) {
						newTreeIndex[j.getRoot()] = k->second;
					}
				}
			}
		} //unlock

		string tmpName = getDataFile() + ".tmp";
		string origName = getDataFile();

		createDataFile(tmpName);

		{
			File in(origName, File::READ, File::OPEN | File::SHARED | File::RANDOM_ACCESS);
			File out(tmpName, File::READ | File::WRITE, File::OPEN | File::RANDOM_ACCESS);

			{
				RLock l(cs);
				for (auto i = newTreeIndex.begin(); i != newTreeIndex.end();) {
					TigerTree tree;
					bool loaded = loadTree(in, i->second, i->first, tree, true);
					if (loaded) {
						i->second.setIndex(saveTree(out, tree));
						++i;
					} else {
						failedTrees++;
						failedSize += i->second.getSize();
						newTreeIndex.erase(i++);
					}
				}

				for (auto& i: fileIndex) {
		#ifndef _MSC_VER
					decltype(fileIndex)::mapped_type newFileList;
		#else
					/// @todo remove this workaround when VS has a proper decltype...
					decltype(fileIndex.begin()->second) newFileList;
		#endif

					for (auto& j: i.second) {
						if (newTreeIndex.find(j.getRoot()) != newTreeIndex.end()) {
							newFileList.push_back(j);
						} else {
							unusedFiles++;
						}
					}

					if(!newFileList.empty()) {
						newFileIndex[i.first] = move(newFileList);
					}
				}

				finalTreeIndexSize = newTreeIndex.size();
			}
		}
	
		{
			WLock l(cs); //need to lock, we wont be able to rename if the file is in use...
			File::deleteFile(origName);
			File::renameFile(tmpName, origName);
			treeIndex = newTreeIndex;
			fileIndex = newFileIndex;
		}

		dirty = true;
		save();

		string msg;
		if (finalTreeIndexSize < initialTreeIndexSize) {
			msg = STRING_F(HASH_REBUILT_UNUSED, (initialTreeIndexSize-finalTreeIndexSize) % unusedFiles);
		} else {
			msg = STRING(HASH_REBUILT_NO_UNUSED);
		}

		if (failedTrees > 0) {
			msg += ". ";
			msg += STRING_F(REBUILD_FAILED_ENTRIES, failedTrees % Util::formatBytes(failedSize));
		}

		LogManager::getInstance()->message(msg, failedTrees > 0 ? LogManager::LOG_ERROR : LogManager::LOG_INFO);
	} catch (const Exception& e) {
		LogManager::getInstance()->message(STRING(HASHING_FAILED) + " " + e.getError(), LogManager::LOG_ERROR);
	}
}

void HashManager::HashStore::save() {
	if(saving.test_and_set())
		return;

	if (dirty && !SettingsManager::lanMode) {
		try {
			File ff(getIndexFile() + ".tmp", File::WRITE, File::CREATE | File::TRUNCATE, false);
			BufferedOutputStream<false> f(&ff);

			string tmp;
			string b32tmp;

			f.write(SimpleXML::utf8Header);
			f.write(LIT("<HashStore Version=\"" HASH_FILE_VERSION_STRING "\">\r\n"));

			f.write(LIT("\t<Trees>\r\n"));
			{
				RLock l(cs);
				dirty = false;
				for (auto& i: treeIndex) {
					const TreeInfo& ti = i.second;
					f.write(LIT("\t\t<Hash Type=\"TTH\" Index=\""));
					f.write(Util::toString(ti.getIndex()));
					f.write(LIT("\" BlockSize=\""));
					f.write(Util::toString(ti.getBlockSize()));
					f.write(LIT("\" Size=\""));
					f.write(Util::toString(ti.getSize()));
					f.write(LIT("\" Root=\""));
					b32tmp.clear();
					f.write(i.first.toBase32(b32tmp));
					f.write(LIT("\"/>\r\n"));
				}
		
				f.write(LIT("\t</Trees>\r\n\t<Files>\r\n"));

				for (auto& i: fileIndex) {
					const string& dir = i.first;
					for (auto& fi: i.second) {
						f.write(LIT("\t\t<File Name=\""));
						f.write(SimpleXML::escape(dir + fi.getFileName(), tmp, true));
						f.write(LIT("\" TimeStamp=\""));
						f.write(Util::toString(fi.getTimeStamp()));
						f.write(LIT("\" Root=\""));
						b32tmp.clear();
						f.write(fi.getRoot().toBase32(b32tmp));
						f.write(LIT("\"/>\r\n"));
					}
				}
			} //unlock
			f.write(LIT("\t</Files>\r\n</HashStore>"));
			f.flush();
			ff.close();
			File::deleteFile( getIndexFile());
			File::renameFile(getIndexFile() + ".tmp", getIndexFile());
		} catch (const FileException& e) {
			LogManager::getInstance()->message(STRING(ERROR_SAVING_HASH) + " " + e.getError(), LogManager::LOG_ERROR);
		}
	}
	saving.clear();
}

string HashManager::HashStore::getIndexFile() { return Util::getPath(Util::PATH_USER_CONFIG) + "HashIndex.xml"; }
string HashManager::HashStore::getDataFile() { return Util::getPath(Util::PATH_USER_CONFIG) + "HashData.dat"; }

class HashLoader: public SimpleXMLReader::CallBack {
public:
	HashLoader(HashManager::HashStore& s) : 
		store(s), size(0), timeStamp(0), version(HASH_FILE_VERSION), inTrees(false), inFiles(false), inHashStore(false) {
	}
	void startTag(const string& name, StringPairList& attribs, bool simple);
	void endTag(const string& name);
	
private:
	HashManager::HashStore& store;

	string file;
	int64_t size;
	uint32_t timeStamp;
	int version;

	bool inTrees;
	bool inFiles;
	bool inHashStore;
};

void HashManager::HashStore::load() {
	if (SettingsManager::lanMode)
		return;
	try {
		Util::migrate(getIndexFile());

		HashLoader loader(*this);
		File f(getIndexFile(), File::READ, File::OPEN);
		SimpleXMLReader(&loader).parse(f);
	} catch (const Exception&) {
		// ...
	}
}

static const string sHashStore = "HashStore";
static const string sversion = "version";		// Oops, v1 was like this
static const string sVersion = "Version";
static const string sTrees = "Trees";
static const string sFiles = "Files";
static const string sFile = "File";
static const string sName = "Name";
static const string sSize = "Size";
static const string sHash = "Hash";
static const string sType = "Type";
static const string sTTH = "TTH";
static const string sIndex = "Index";
static const string sBlockSize = "BlockSize";
static const string sTimeStamp = "TimeStamp";
static const string sRoot = "Root";

void HashLoader::startTag(const string& name, StringPairList& attribs, bool simple) {
	if (!inHashStore && name == sHashStore) {
		version = Util::toInt(getAttrib(attribs, sVersion, 0));
		if (version == 0) {
			version = Util::toInt(getAttrib(attribs, sversion, 0));
		}
		inHashStore = !simple;
	} else if (inHashStore && (version == 2 || version == 3)) {
		if (inTrees && name == sHash) {
			const string& type = getAttrib(attribs, sType, 0);
			int64_t index = Util::toInt64(getAttrib(attribs, sIndex, 1));
			int64_t blockSize = Util::toInt64(getAttrib(attribs, sBlockSize, 2));
			int64_t size = Util::toInt64(getAttrib(attribs, sSize, 3));
			const string& root = getAttrib(attribs, sRoot, 4);
			if (!root.empty() && type == sTTH && (index >= 8 || index == HashManager::SMALL_TREE) && blockSize >= 1024) {
				store.treeIndex[TTHValue(root)] = HashManager::HashStore::TreeInfo(size, index, blockSize);
			}
		} else if (inFiles && name == sFile) {
			file = getAttrib(attribs, sName, 0);
			timeStamp = Util::toUInt32(getAttrib(attribs, sTimeStamp, 1));
			const string& root = getAttrib(attribs, sRoot, 2);

			if (!file.empty() && size >= 0 && timeStamp > 0 && !root.empty()) {
				string fileLower = Text::toLower(file);
				store.fileIndex[Util::getFilePath(fileLower)].emplace_back(Util::getFileName(fileLower), TTHValue(root), timeStamp, false);
			}
		} else if (name == sTrees) {
			inTrees = !simple;
		} else if (name == sFiles) {
			inFiles = !simple;
		}
	}
}

void HashLoader::endTag(const string& name) {
	if (name == sFile) {
		file.clear();
	}
}

HashManager::HashStore::HashStore() :
	dirty(false) {

	Util::migrate(getDataFile());

	if (File::getSize(getDataFile()) <= static_cast<int64_t> (sizeof(int64_t))) {
		try {
			createDataFile( getDataFile());
		} catch (const FileException&) {
			// ?
		}
	}
}

/**
 * Creates the data files for storing hash values.
 * The data file is very simple in its format. The first 8 bytes
 * are filled with an int64_t (little endian) of the next write position
 * in the file counting from the start (so that file can be grown in chunks).
 * We start with a 1 mb file, and then grow it as needed to avoid fragmentation.
 * To find data inside the file, use the corresponding index file.
 * Since file is never deleted, space will eventually be wasted, so a rebuild
 * should occasionally be done.
 */
void HashManager::HashStore::createDataFile(const string& name) {
	try {
		File dat(name, File::WRITE, File::CREATE | File::TRUNCATE);
		dat.setPos(1024 * 1024);
		dat.setEOF();
		dat.setPos(0);
		int64_t start = sizeof(start);
		dat.write(&start, sizeof(start));

	} catch (const FileException& e) {
		LogManager::getInstance()->message(STRING(ERROR_CREATING_HASH_DATA_FILE) + " " + e.getError(), LogManager::LOG_ERROR);
	}
}

void HashManager::Hasher::hashFile(const string& fileName, int64_t size, const string& devID) {
	auto wi = move(WorkItem(fileName, size, devID));
	auto hqr = equal_range(w.begin(), w.end(), wi, HashSortOrder());
	if (hqr.first == hqr.second) {
		//it doesn't exist yet
		devices[devID]++; 
		w.insert(hqr.first, move(wi));
		totalBytesLeft += size;
		s.signal();
	} else {
		//in case the size has changed...
		wi.fileSize = size;
	}
}

bool HashManager::Hasher::pause() {
	paused = true;
	return paused;
}

void HashManager::Hasher::resume() {
	paused = false;
	t_resume();
}

bool HashManager::Hasher::isPaused() const {
	return paused;
}

void HashManager::Hasher::removeDevice(const string& aID) {
	dcassert(!aID.empty());
	auto dp = devices.find(aID);
	if (dp != devices.end()) {
		dp->second--;
		if (dp->second == 0)
			devices.erase(dp);
	}
}

void HashManager::Hasher::stopHashing(const string& baseDir) {
	for (auto i = w.begin(); i != w.end();) {
		if (strnicmp(baseDir, i->filePath, baseDir.length()) == 0) {
			totalBytesLeft -= i->fileSize;
			removeDevice(i->devID);
			i = w.erase(i);
		} else {
			++i;
		}
	}
}

void HashManager::stopHashing(const string& baseDir) {
	WLock l(Hasher::hcs);
	for (auto h: hashers)
		h->stopHashing(baseDir); 
}

void HashManager::setPriority(Thread::Priority p) {
	RLock l(Hasher::hcs);
	for (auto h: hashers)
		h->setThreadPriority(p); 
}

void HashManager::getStats(string& curFile, int64_t& bytesLeft, size_t& filesLeft, int64_t& speed, int& hasherCount) {
	RLock l(Hasher::hcs);
	hasherCount = hashers.size();
	for (auto i: hashers)
		i->getStats(curFile, bytesLeft, filesLeft, speed);
}

void HashManager::rebuild() {
	hashers.front()->scheduleRebuild(); 
}

void HashManager::startup() {
	hashers.push_back(new Hasher(false, 0));
	store.load(); 
}

void HashManager::stop() {
	WLock l(Hasher::hcs);
	for (auto h: hashers)
		h->clear();
}

void HashManager::Hasher::shutdown() { 
	stop = true; 
	if(paused) 
		resume(); 
	s.signal(); 
}

void HashManager::Hasher::scheduleRebuild() { 
	rebuild = true; 
	s.signal(); 
	if(paused) 
		t_resume(); 
}

void HashManager::shutdown() {
	aShutdown = true;
	{
		WLock l(Hasher::hcs);
		for (auto h: hashers) {
			h->clear();
			h->shutdown();
		}
	}

	// Wait for the hashers to shut down
	while(true) {
		{
			RLock l(Hasher::hcs);
			if(hashers.empty()) {
				break;
			}
		}
		Thread::sleep(50);
	}

	store.save(); 
}

void HashManager::Hasher::clear() {
	w.clear();
	devices.clear();
	totalBytesLeft = 0;
}

void HashManager::Hasher::getStats(string& curFile, int64_t& bytesLeft, size_t& filesLeft, int64_t& speed) {
	curFile = currentFile;
	filesLeft += w.size();
	if (running)
		filesLeft++;
	bytesLeft += totalBytesLeft;
	speed += lastSpeed;
}

void HashManager::Hasher::instantPause() {
	if(paused) {
		t_suspend();
	}
}

HashManager::Hasher::Hasher(bool isPaused, int aHasherID) : stop(false), running(false), paused(isPaused), rebuild(false), saveData(false), totalBytesLeft(0), lastSpeed(0), sizeHashed(0), hashTime(0), dirsHashed(0),
	filesHashed(0), dirFilesHashed(0), dirSizeHashed(0), dirHashTime(0), hasherID(aHasherID) { 

	start();
	if (isPaused)
		t_suspend();
}

void HashManager::log(const string& aMessage, int hasherID, bool isError, bool lock) {
	ConditionalRLock l(Hasher::hcs, lock);
	LogManager::getInstance()->message((hashers.size() > 1 ? "[" + STRING_F(HASHER_X, hasherID) + "] " + ": " : Util::emptyString) + aMessage, isError ? LogManager::LOG_ERROR : LogManager::LOG_INFO);
}

int HashManager::Hasher::run() {
	setThreadPriority(Thread::IDLE);

	string fname;
	for(;;) {
		s.wait();
		instantPause(); //suspend the thread...
		if(stop) {
			WLock l(hcs);
			HashManager::getInstance()->removeHasher(this);
			break;
		}

		if(saveData) {
			HashManager::getInstance()->SaveData();
			saveData = false;
			continue;
		}

		if(rebuild) {
			HashManager::getInstance()->doRebuild();
			rebuild = false;
			continue;
		}
		
		bool failed = true;
		bool dirChanged = false;
		string curDevID;
		{
			WLock l(hcs);
			if(!w.empty()) {
				auto& wi = w.front();
				dirChanged = compare(Util::getFilePath(wi.filePath), Util::getFilePath(fname)) != 0;
				currentFile = fname = move(wi.filePath);
				curDevID = move(wi.devID);
				dcassert(!curDevID.empty());
				w.pop_front();
			} else {
				fname.clear();
			}
		}
		running = true;

		TTHValue tth;
		if(!fname.empty()) {
			try {
				if (initialDir.empty()) {
					initialDir = Util::getFilePath(fname);
				}

				if (dirChanged)
					sfv.loadPath(Util::getFilePath(fname));
				uint64_t start = GET_TICK();
				File f(fname, File::READ, File::OPEN);
				int64_t size = f.getSize();
				int64_t bs = max(TigerTree::calcBlockSize(size, 10), MIN_BLOCK_SIZE);
				uint64_t timestamp = f.getLastModified();
				int64_t sizeLeft = size;
				TigerTree tt(bs);

				uint32_t fileCRC = 0;
				CRC32Filter crc32;
				CRC32Filter* xcrc32 = 0;

				if (sfv.hasFile(Text::toLower(Util::getFileName(fname)), fileCRC))
					xcrc32 = &crc32;

				uint64_t lastRead = GET_TICK();
 
                FileReader fr(true);
				fr.read(fname, [&](const void* buf, size_t n) -> bool {
					uint64_t now = GET_TICK();
					if(SETTING(MAX_HASH_SPEED)> 0) {
						uint64_t minTime = n * 1000LL / (SETTING(MAX_HASH_SPEED) * 1024LL * 1024LL);
 
						if(lastRead + minTime > now) {
							Thread::sleep(minTime - (now - lastRead));
						}
						lastRead = lastRead + minTime;
					} else {
						lastRead = GET_TICK();
					}
					tt.update(buf, n);
				
					if(xcrc32)
						(*xcrc32)(buf, n);

					sizeLeft -= n;

					{
						WLock l(hcs);
						if(totalBytesLeft > 0)
							totalBytesLeft -= n;
						if(now > start)
							lastSpeed = (size - sizeLeft)*1000 / (now -start);

					}

					return !stop;
				});

				f.close();
				tt.finalize();

				failed = xcrc32 && xcrc32->getValue() != fileCRC;

				uint64_t end = GET_TICK();
				int64_t averageSpeed = 0;

				if (!failed) {
					sizeHashed += size;
					dirSizeHashed += size;

					dirFilesHashed++;
					filesHashed++;
				}

				if(end > start) {
					hashTime += (end - start);
					dirHashTime += (end - start);
					averageSpeed = size * 1000 / (end - start);
				}

				if(failed) {
					HashManager::getInstance()->log(STRING(ERROR_HASHING) + fname + ": " + STRING(ERROR_HASHING_CRC32), hasherID, true, true);
					HashManager::getInstance()->fire(HashManagerListener::HashFailed(), fname);
				} else {
					HashManager::getInstance()->hashDone(fname, timestamp, tt, averageSpeed, size, hasherID);
					tth = tt.getRoot();
				}
			} catch(const FileException& e) {
				HashManager::getInstance()->log(STRING(ERROR_HASHING) + " " + fname + ": " + e.getError(), hasherID, true, true);
				HashManager::getInstance()->fire(HashManagerListener::HashFailed(), fname);
			}
		
		}

		auto onDirHashed = [&] () -> void {
			if ((SETTING(HASHERS_PER_VOLUME) == 1 || w.empty()) && (dirFilesHashed > 1 || !failed)) {
				if (dirFilesHashed == 1) {
					HashManager::getInstance()->log(STRING_F(HASHING_FINISHED_FILE, currentFile % 
						Util::formatBytes(dirSizeHashed) % 
						Util::formatTime(dirHashTime / 1000, true) % 
						(Util::formatBytes(dirHashTime > 0 ? ((dirSizeHashed * 1000) / dirHashTime) : 0) + "/s" )), hasherID, false, false);
				} else {
					HashManager::getInstance()->log(STRING_F(HASHING_FINISHED_DIR, Util::getFilePath(initialDir) % 
						dirFilesHashed %
						Util::formatBytes(dirSizeHashed) % 
						Util::formatTime(dirHashTime / 1000, true) % 
						(Util::formatBytes(dirHashTime > 0 ? ((dirSizeHashed * 1000) / dirHashTime) : 0) + "/s" )), hasherID, false, false);
				}
			}

			dirsHashed++;
			dirHashTime = 0;
			dirSizeHashed = 0;
			dirFilesHashed = 0;
			initialDir.clear();
		};

		bool deleteThis = false;
		{
			WLock l(hcs);
			if (!fname.empty())
				removeDevice(curDevID);

			if (w.empty()) {
				if (sizeHashed > 0) {
					if (dirsHashed == 0) {
						onDirHashed();
						//LogManager::getInstance()->message(STRING(HASHING_FINISHED_TOTAL_PLAIN), LogManager::LOG_INFO);
					} else {
						onDirHashed();
						HashManager::getInstance()->log(STRING_F(HASHING_FINISHED_TOTAL, filesHashed % Util::formatBytes(sizeHashed) % dirsHashed % 
							Util::formatTime(hashTime / 1000, true) % 
							(Util::formatBytes(hashTime > 0 ? ((sizeHashed * 1000) / hashTime) : 0)  + "/s" )), hasherID, false, false);
					}
				}
				hashTime = 0;
				sizeHashed = 0;
				dirsHashed = 0;
				filesHashed = 0;
				deleteThis = hasherID != 0;
			} else if (!AirUtil::isParentOrExact(initialDir, w.front().filePath)) {
				onDirHashed();
			}

			currentFile.clear();
		}

		if (!failed && !fname.empty())
			HashManager::getInstance()->fire(HashManagerListener::TTHDone(), fname, tth);

		if (deleteThis) {
			//check again if we have added new items while this was unlocked

			WLock l(hcs);
			if (w.empty()) {
				//Nothing more to has, delete this hasher
				HashManager::getInstance()->removeHasher(this);
				break;
			}
		}

		running = false;
	}

	delete this;
	return 0;
}

void HashManager::removeHasher(Hasher* aHasher) {
	hashers.erase(remove(hashers.begin(), hashers.end(), aHasher), hashers.end());
}

bool HashManager::Hasher::HashSortOrder::operator()(const WorkItem& left, const WorkItem& right) const {
	// Case-sensitive (faster), it is rather unlikely that case changes, and if it does it's harmless.
	auto comp = compare(Util::getFilePath(left.filePath), Util::getFilePath(right.filePath));
	if (comp == 0) {
		return compare(left.filePath, right.filePath) < 0;
	}
	return comp < 0;
}

HashManager::HashPauser::HashPauser() {
	HashManager::getInstance()->pauseHashing();
}

HashManager::HashPauser::~HashPauser() {
	HashManager::getInstance()->resumeHashing();
}

bool HashManager::pauseHashing() {
	pausers++;
	if (pausers == 1) {
		RLock l (Hasher::hcs);
		for (auto h: hashers)
			h->pause();
		return isHashingPaused(false);
	}
	return true;
}

void HashManager::resumeHashing(bool forced) {
	if (forced )
		pausers = 0;
	else if (pausers > 0)
		pausers--;

	if (pausers == 0) {
		RLock l(Hasher::hcs);
		for(auto h: hashers)
			h->resume();
	}
}

bool HashManager::isHashingPaused(bool lock /*true*/) const {
	ConditionalRLock l(Hasher::hcs, lock);
	return all_of(hashers.begin(), hashers.end(), [](const Hasher* h) { return h->isPaused(); });
}

void HashManager::on(TimerManagerListener::Minute, uint64_t) noexcept {
	if(GET_TICK() - lastSave > 15*60*1000 && store.isDirty()) { 
		lastSave = GET_TICK();

		RLock l(Hasher::hcs);
		hashers.front()->save();
	}
}

} // namespace dcpp