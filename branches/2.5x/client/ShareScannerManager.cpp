/*
 * Copyright (C) 2011-2013 AirDC++ Project
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

#include "ShareScannerManager.h"
#include "HashManager.h"
#include "TimerManager.h"

#include "AutoSearchManager.h"
#include "FileReader.h"
#include "LogManager.h"
#include "ShareManager.h"
#include "StringTokenizer.h"
#include "FilteredFile.h"
#include "File.h"
#include "Wildcards.h"
#include "QueueManager.h"
#include "format.h"

#include <cstdlib>
#include <iostream>
#include <fstream>
#include <cstring>

#ifndef _WIN32
#include <dirent.h>
#include <fnmatch.h>
#endif

#include "concurrency.h"

namespace dcpp {

#ifdef ATOMIC_FLAG_INIT
atomic_flag ShareScannerManager::scanning = ATOMIC_FLAG_INIT;
#else
atomic_flag ShareScannerManager::scanning;
#endif

ShareScannerManager::ShareScannerManager() {
	releaseReg.assign(AirUtil::getReleaseRegBasic());
	simpleReleaseReg.assign("(([A-Z0-9]\\S{3,})-([A-Za-z0-9_]{2,}))");
	emptyDirReg.assign("(\\S*(((nfo|dir).?fix)|nfo.only)\\S*)", boost::regex_constants::icase);
	rarReg.assign("(.+\\.((r\\w{2})|(0\\d{2})))");
	rarMp3Reg.assign("(.+\\.((r\\w{2})|(0\\d{2})|(mp3)|(flac)))");
	audioBookReg.assign(".+(-|\\()AUDIOBOOK(-|\\)).+", boost::regex_constants::icase);
	flacReg.assign(".+(-|\\()(LOSSLESS|FLAC)((-|\\)).+)?", boost::regex_constants::icase);
	zipReg.assign("(.+\\.zip)");
	longReleaseReg.assign(AirUtil::getReleaseRegLong(false));
	mvidReg.assign("(.+\\.(m2v|avi|mkv|mp(e)?g))");
	proofImageReg.assign("(.*(jp(e)?g|png))", boost::regex_constants::icase);
	subDirReg.assign("((((DVD)|(CD)|(DIS(K|C))).?([0-9](0-9)?))|(Sample)|(Cover(s)?)|(.{0,5}Sub(s)?))", boost::regex_constants::icase);
	extraRegs[AUDIOBOOK].assign("(.+\\.(jp(e)?g|png|m3u|cue|zip|sfv|nfo))");
	extraRegs[FLAC].assign("(.+\\.(jp(e)?g|png|m3u|cue|log|sfv|nfo))");
	extraRegs[NORMAL].assign("(.+\\.(jp(e)?g|png|m3u|cue|diz|sfv|nfo))");
	zipFolderReg.assign("(.+\\.(jp(e)?g|png|diz|zip|nfo|sfv))");
	subReg.assign("(.{0,8}[Ss]ub(s|pack)?)");
}

ShareScannerManager::~ShareScannerManager() { 
	Stop();
	join();
}

int ShareScannerManager::scan(StringList paths, bool sfv /*false*/) {
	stop = false;
	//initiate the thread always here for now.
	if(scanning.test_and_set()){
		LogManager::getInstance()->message(STRING(SCAN_RUNNING), LogManager::LOG_INFO);
		return 1;
	}
	isCheckSFV = false;
	isDirScan = false;

	if(sfv) {
		isCheckSFV = true;
		rootPaths = paths;
	} else if(!paths.empty())  {
		isDirScan = true;
		rootPaths = paths;
	} else {
		ShareManager::getInstance()->getParentPaths(rootPaths);
	}

	start();
	

	if(sfv) {
		LogManager::getInstance()->message(STRING(CRC_STARTED), LogManager::LOG_INFO);
		crcOk = 0;
		crcInvalid = 0;
		checkFailed = 0;
	} else {
		LogManager::getInstance()->message(STRING(SCAN_STARTED), LogManager::LOG_INFO);
	}
	return 0;
}

void ShareScannerManager::Stop() {
	stop = true;
}

int ShareScannerManager::run() {
	if (isCheckSFV) {

		/* Get the total size and dirs */
		scanFolderSize = 0;
		SFVScanList sfvDirPaths;
		StringList sfvFilePaths;
		for(auto& path: rootPaths) {
			if(path[path.size() -1] == PATH_SEPARATOR) {
				prepareSFVScanDir(path, sfvDirPaths);
			} else {
				prepareSFVScanFile(path, sfvFilePaths);
			}
		}

		/* Scan root files */
		if (!sfvFilePaths.empty()) {
			DirSFVReader sfv(Util::getFilePath(rootPaths.front()));
			for(auto& path: sfvFilePaths) {
				if (stop)
					break;

				checkFileSFV(path, sfv, false);
			}
		}

		/* Scan all directories */
		for(auto& i: sfvDirPaths) {
			if (stop)
				break;

			auto files = findFiles(i.first, "*", false, false);
			for(auto s = files.begin(); s != files.end(); ++s) {
				if (stop)
					break;

				checkFileSFV(*s, i.second, true);
			}
		}


		/* Report */
		if (stop) {
			LogManager::getInstance()->message(STRING(CRC_STOPPED), LogManager::LOG_INFO);
		} else {
			LogManager::getInstance()->message(STRING_F(CRC_FINISHED, crcOk % crcInvalid % checkFailed), LogManager::LOG_INFO);
		}
	} else {
		/* Scan for missing files */
		QueueManager::getInstance()->getUnfinishedPaths(bundleDirs);
		sort(bundleDirs.begin(), bundleDirs.end());

		ScanInfoList scanners;
		for(auto& dir: rootPaths) {
			scanners.emplace_back(dir, ScanInfo::TYPE_COLLECT_LOG, true);
		}

		parallel_for_each(scanners.begin(), scanners.end(), [&](ScanInfo& s) {
			DWORD attrib = GetFileAttributes(Text::toT(s.rootPath).c_str());
			if(attrib != INVALID_FILE_ATTRIBUTES && attrib != FILE_ATTRIBUTE_HIDDEN && attrib != FILE_ATTRIBUTE_SYSTEM && attrib != FILE_ATTRIBUTE_OFFLINE) {
				if (!matchSkipList(Util::getLastDir(s.rootPath)) && !std::binary_search(bundleDirs.begin(), bundleDirs.end(), s.rootPath)) {
					scanDir(s.rootPath, s);
					if(SETTING(CHECK_DUPES) && isDirScan)
						findDupes(s.rootPath, s);

					find(s.rootPath, Text::toLower(s.rootPath), s);
				}
			}
		});

		if(!stop) {
			//merge the results
			ScanInfo total(Util::emptyString, ScanInfo::TYPE_COLLECT_LOG, true);
			for(auto& s: scanners) {
				s.merge(total);
			}

			ScanType scanType = isDirScan ? TYPE_PARTIAL : TYPE_FULL;
			string report;
			if (scanType == TYPE_FULL) {
				report = CSTRING(SCAN_SHARE_FINISHED);
			} else if (scanType == TYPE_PARTIAL) {
				report = CSTRING(SCAN_FOLDER_FINISHED);
			}

			if (!total.scanMessage.empty()) {
				report += " ";
				report += CSTRING(SCAN_PROBLEMS_FOUND);
				report += ":  ";
				report += total.getResults();
				report += ". " + STRING(SCAN_RESULT_NOTE);

				if (SETTING(LOG_SHARE_SCANS)) {
					auto path = Util::validateFileName(Util::formatTime(SETTING(LOG_DIRECTORY) + SETTING(LOG_SHARE_SCAN_PATH), time(NULL)));
					File::ensureDirectory(path);

					File f(path, File::WRITE, File::OPEN | File::CREATE);
					f.setEndPos(0);
					f.write(total.scanMessage);
				}

				char buf[255];
				time_t time = GET_TIME();
				tm* _tm = localtime(&time);
				strftime(buf, 254, "%c", _tm);

				fire(ScannerManagerListener::ScanFinished(), total.scanMessage, STRING_F(SCANNING_RESULTS_ON, string(buf)));
			} else {
				report += ", ";
				report += CSTRING(SCAN_NO_PROBLEMS);
			}

			LogManager::getInstance()->message(report, LogManager::LOG_INFO);
		}
		bundleDirs.clear();
		dupeDirs.clear();
	}
	
	scanning.clear();
	rootPaths.clear();
	return 0;
}

void ShareScannerManager::ScanInfo::merge(ScanInfo& collect) const {
	collect.missingFiles += missingFiles;
	collect.missingSFV += missingSFV;
	collect.missingNFO += missingNFO;
	collect.extrasFound += extrasFound;
	collect.noReleaseFiles += noReleaseFiles;
	collect.emptyFolders += emptyFolders;
	collect.dupesFound += dupesFound;

	collect.scanMessage += scanMessage;
}

bool ShareScannerManager::matchSkipList(const string& dir) {
	if (SETTING(CHECK_USE_SKIPLIST)) {
		return ShareManager::getInstance()->matchSkipList(dir);
	}
	return false;
}

void ShareScannerManager::find(const string& aPath, const string& aPathLower, ScanInfo& aScan) {
	if(aPath.empty() || stop)
		return;

	string dir;
	string dirLower;
	
	for(FileFindIter i(aPath + "*", true); i != FileFindIter(); ++i) {
		try {
			if(i->isDirectory() && strcmp(i->getFileName().c_str(), ".") != 0 && strcmp(i->getFileName().c_str(), "..") != 0){
				if (matchSkipList(i->getFileName())) {
					continue;
				}
				dir = aPath + i->getFileName() + PATH_SEPARATOR;
				dirLower = aPathLower + Text::toLower(i->getFileName()) + PATH_SEPARATOR;
				
				if (aScan.isManualShareScan && std::binary_search(bundleDirs.begin(), bundleDirs.end(), dirLower)) {
					continue;
				}

				if(!i->isHidden()) {
					scanDir(dir, aScan);
					if(SETTING(CHECK_DUPES) && aScan.isManualShareScan)
						findDupes(dir, aScan);

					find(dir, dirLower, aScan);
				}
			}
		} catch(const FileException&) { } 
	}
}


void ShareScannerManager::findDupes(const string& path, ScanInfo& aScan) throw(FileException) {
	if(path.empty())
		return;
	
	string dirName = Util::getLastDir(path);

	//only match release names here
	if (!regex_match(dirName, releaseReg))
		return;
	
	{
		WLock l(cs);
		auto dupes = dupeDirs.equal_range(Text::toLower(dirName));
		if (dupes.first != dupes.second) {
			aScan.dupesFound++;

			//list all dupes here
			for(auto k = dupes.first; k != dupes.second; ++k) {
				reportMessage(STRING_F(X_IS_SAME_THAN, path % k->second), aScan, false);
			}
		}

		dupeDirs.emplace(dirName, path);
	}
}

StringList ShareScannerManager::findFiles(const string& path, const string& pattern, bool dirs /*false*/, bool aMatchSkipList) {
	StringList ret;

	WIN32_FIND_DATA data;
	HANDLE hFind;

	hFind = ::FindFirstFile(Text::toT(Util::FormatPath(path + pattern)).c_str(), &data);
	if(hFind != INVALID_HANDLE_VALUE) {
		do {
			if (!(data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) && !(data.dwFileAttributes &FILE_ATTRIBUTE_SYSTEM) && !(data.dwFileAttributes &FILE_ATTRIBUTE_SYSTEM)) {
				if (aMatchSkipList && matchSkipList(Text::fromT(data.cFileName))) {
					continue;
				}
				if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
					if (dirs && Text::fromT(data.cFileName)[0] != '.') {
						ret.push_back(Text::fromT(data.cFileName));
					}
				} else if (!dirs) {
					if (SETTING(CHECK_IGNORE_ZERO_BYTE)) {
						if (File::getSize(path + Text::fromT(data.cFileName)) <= 0) {
							continue;
						}
					}
					ret.push_back(Text::toLower(Text::fromT(data.cFileName)));
				}
			}
		} while(::FindNextFile(hFind, &data));

		::FindClose(hFind);
	}
	return ret;
}

void ShareScannerManager::scanDir(const string& aPath, ScanInfo& aScan) throw(FileException) {
	if(aPath.empty())
		return;

	StringList sfvFileList, fileList = findFiles(aPath, "*", false, true);

	if (fileList.empty()) {
		//check if there are folders
		StringList folderList = findFiles(aPath, "*", true, true);
		if (folderList.empty()) {
			if (SETTING(CHECK_EMPTY_DIRS)) {
				reportMessage(STRING(DIR_EMPTY) + " " + aPath, aScan);
				aScan.emptyFolders++;
			}
			return;
		}
	}

	int nfoFiles=0, sfvFiles=0;
	bool isSample=false, isRelease=false, isZipRls=false, found=false, extrasInFolder = false;

	string dirName = Util::getLastDir(aPath);

	// Find NFO and SFV files
	for(auto& fileName: fileList) {
		if (Util::getFileExt(fileName) == ".nfo") {
			nfoFiles++;
		} else if (Util::getFileExt(fileName) == ".sfv") {
			sfvFileList.push_back(aPath + fileName);
			sfvFiles++;
		}
	}

	/* No release files at all? */
	if (!fileList.empty() && ((nfoFiles + sfvFiles) == (int)fileList.size()) && (SETTING(CHECK_EMPTY_RELEASES))) {
		if (!regex_match(dirName, emptyDirReg)) {
			StringList folderList = findFiles(aPath, "*", true, true);
			if (folderList.empty()) {
				reportMessage(STRING(RELEASE_FILES_MISSING) + " " + aPath, aScan);
				aScan.noReleaseFiles++;
				return;
			}
		}
	}

	if(SETTING(CHECK_NFO) || SETTING(CHECK_SFV) || SETTING(CHECK_EXTRA_FILES) || SETTING(CHECK_EXTRA_SFV_NFO)) {
		//Check for multiple NFO or SFV files
		if (SETTING(CHECK_EXTRA_SFV_NFO)) {
			if (nfoFiles > 1) {
				reportMessage(STRING(MULTIPLE_NFO) + " " + aPath, aScan);
				aScan.extrasFound++;
				extrasInFolder = true;
			}
			if (sfvFiles > 1) {
				reportMessage(STRING(MULTIPLE_SFV) + " " + aPath, aScan);
				if (!extrasInFolder) {
					extrasInFolder = true;
					aScan.extrasFound++;
				}
			}
		}

		//Check if it's a sample folder
		isSample = (strcmp(Text::toLower(dirName).c_str(), "sample") == 0);

		if (nfoFiles == 0 || sfvFiles == 0 || isSample || SETTING(CHECK_EXTRA_FILES)) {
			//Check if it's a RAR/Music release folder
			isRelease = AirUtil::listRegexMatch(fileList, (SETTING(CHECK_MP3_DIR) ? rarMp3Reg : rarReg));

			if (!isRelease) {
				//Check if it's a zip release folder
				if (regex_match(dirName, simpleReleaseReg)) {
					isZipRls = AirUtil::listRegexMatch(fileList, zipReg);
				}

				//Check if it's a Mvid release folder
				if (!isZipRls && regex_match(dirName, longReleaseReg)) {
					isRelease = AirUtil::listRegexMatch(fileList, mvidReg);
				}

				//Report extra files in a zip folder
				if (isZipRls && SETTING(CHECK_EXTRA_FILES) && sfvFiles == 0) {
					AirUtil::listRegexSubtract(fileList, zipFolderReg);
					if (!fileList.empty()) {
						reportMessage(STRING_F(EXTRA_FILES_RLSDIR_X, aPath.c_str() % Util::toString(", ", fileList)), aScan);
						aScan.extrasFound++;
					}
				}
			}

			//Report extra files in sample folder
			if (SETTING(CHECK_EXTRA_FILES) && isSample) {
				found = false;
				if (fileList.size() > 1) {
					//check that all files have the same extension.. otherwise there are extras
					string extension;
					for(auto& fileName: fileList) {
						//ignore image files
						if (boost::regex_match(Util::getFileExt(fileName), proofImageReg))
							continue;
						
						string loopExt = Util::getFileExt(fileName);
						if (!extension.empty() && loopExt != extension) {
							found = true;
							break;
						}
						extension = loopExt;
					}
				}

				if (nfoFiles > 0 || sfvFiles > 0 || isRelease || found) {
					reportMessage(STRING_F(EXTRA_FILES_SAMPLEDIR_X, aPath), aScan);
					aScan.extrasFound++;
				}
			}

			if (isSample)
				return;

			//Report missing NFO
			if (SETTING(CHECK_NFO) && nfoFiles == 0 && regex_match(dirName, simpleReleaseReg)) {
				found = false;
				if (fileList.empty()) {
					found = true;
					StringList folderList = findFiles(aPath, "*", true, true);
					//check if there are multiple disks and nfo inside them
					for(auto& dirName: folderList) {
						if (regex_match(dirName, subDirReg)) {
							found = false;
							StringList filesListSub = findFiles(aPath + dirName + "\\", "*.nfo", false, true);
							if (!filesListSub.empty()) {
								found = true;
								break;
							}
						}
					}
				}

				if (!found) {
					reportMessage(STRING(NFO_MISSING) + aPath, aScan);
					aScan.missingNFO++;
				}
			}

			//Report missing SFV
			if (sfvFiles == 0 && isRelease) {
				//avoid extra matches
				if (!regex_match(dirName,subReg) && SETTING(CHECK_SFV)) {
					reportMessage(STRING(SFV_MISSING) + aPath, aScan);
					aScan.missingSFV++;
				}
				return;
			}
		}
	}

	if (sfvFiles == 0)
		return;


	/* Check for missing files */
	bool hasValidSFV = false;

	int releaseFiles=0, loopMissing=0;

	DirSFVReader sfv(aPath, sfvFileList);
	sfv.read([&](const string& fileName) {
		hasValidSFV = true;
		releaseFiles++;

		auto s = std::find(fileList.begin(), fileList.end(), fileName);
		if(s == fileList.end()) { 
			loopMissing++;
			if (SETTING(CHECK_MISSING))
				reportMessage(STRING(FILE_MISSING) + " " + aPath + fileName, aScan);
		} else {
			fileList.erase(s);
		}
	});

	if (SETTING(CHECK_MISSING))
		aScan.missingFiles += loopMissing;

	/* Extras in folder? */
	releaseFiles = releaseFiles - loopMissing;

	if(SETTING(CHECK_EXTRA_FILES) && ((int)fileList.size() > nfoFiles + sfvFiles) && hasValidSFV) {
		//Find allowed extra files from the release folder
		int8_t extrasType = NORMAL;
		if (regex_match(dirName, audioBookReg)) {
			extrasType = AUDIOBOOK;
		} else if (regex_match(dirName, flacReg)) {
			extrasType = FLAC;
		}

		AirUtil::listRegexSubtract(fileList, extraRegs[extrasType]);
		if (!fileList.empty()) {
			reportMessage(CSTRING_F(EXTRA_FILES_RLSDIR_X, aPath % Util::toString(", ", fileList)), aScan);
			if (!extrasInFolder)
				aScan.extrasFound++;
		}
	}
}

void ShareScannerManager::prepareSFVScanDir(const string& aPath, SFVScanList& dirs) throw(FileException) {
	DirSFVReader sfv(aPath);

	/* Get the size and see if all files in the sfv exists */
	if (sfv.hasSFV()) {
		sfv.read([&](const string& fileName) {
			if (Util::fileExists(aPath + fileName)) {
				scanFolderSize = scanFolderSize + File::getSize(aPath + fileName);
			} else {
				LogManager::getInstance()->message(STRING(FILE_MISSING) + " " + aPath + fileName, LogManager::LOG_WARNING);
				checkFailed++;
			}
		});
		dirs.emplace_back(aPath, sfv);
	}

	/* Recursively scan subfolders */
	for(FileFindIter i(aPath + "*", true); i != FileFindIter(); ++i) {
		try {
			if (!i->isHidden()) {
				if (i->isDirectory()) {
					prepareSFVScanDir(aPath + i->getFileName() + PATH_SEPARATOR, dirs);
				}
			}
		} catch(const FileException&) { } 
	}
}

void ShareScannerManager::prepareSFVScanFile(const string& aPath, StringList& files) {
	if (Util::fileExists(aPath)) {
		scanFolderSize += File::getSize(aPath);
		files.push_back(Text::toLower(Util::getFileName(aPath)));
	} else {
		LogManager::getInstance()->message(STRING(FILE_MISSING) + " " + aPath, LogManager::LOG_WARNING);
		checkFailed++;
	}
}

void ShareScannerManager::checkFileSFV(const string& aFileName, DirSFVReader& sfv, bool isDirScan) throw(FileException) {
 
	uint64_t checkStart = 0;
	uint64_t checkEnd = 0;

	if(sfv.hasFile(aFileName)) {
		bool crcMatch = false;
		try {
			checkStart = GET_TICK();
			crcMatch = sfv.isCrcValid(aFileName);
			checkEnd = GET_TICK();
		} catch(const FileException& ) {
			// Couldn't read the file to get the CRC(!!!)
			LogManager::getInstance()->message(STRING(CRC_FILE_ERROR) + sfv.getPath() + aFileName, LogManager::LOG_ERROR);
		}

		int64_t size = File::getSize(sfv.getPath() + aFileName);
		int64_t speed = 0;
		if(checkEnd > checkStart) {
			speed = size * _LL(1000) / (checkEnd - checkStart);
		}

		string message;

		if(crcMatch) {
			message = STRING(CRC_OK);
			crcOk++;
		} else {
			message = STRING(CRC_FAILED);
			crcInvalid++;
		}

		message += sfv.getPath() + aFileName + " (" + Util::formatBytes(speed) + "/s)";

		scanFolderSize = scanFolderSize - size;
		message += ", " + STRING(CRC_REMAINING) + Util::formatBytes(scanFolderSize);
		LogManager::getInstance()->message(message, (crcMatch ? LogManager::LOG_INFO : LogManager::LOG_ERROR));


	} else if (!isDirScan || regex_match(aFileName, rarMp3Reg)) {
		LogManager::getInstance()->message(STRING(NO_CRC32) + " " + sfv.getPath() + aFileName, LogManager::LOG_WARNING);
		checkFailed++;
	}
}

Bundle::Status ShareScannerManager::onScanBundle(const BundlePtr& aBundle, string& error_) noexcept {
	if (SETTING(SCAN_DL_BUNDLES) && !aBundle->isFileBundle()) {
		ScanInfo scanner(aBundle->getName(), ScanInfo::TYPE_SYSLOG, false);

		scanDir(aBundle->getTarget(), scanner);
		find(aBundle->getTarget(), Text::toLower(aBundle->getTarget()), scanner);

		bool hasMissing = scanner.hasMissing();
		bool hasExtras = scanner.hasExtras();

		if (!aBundle->isFailed() || hasMissing || hasExtras) {
			string logMsg;
			if (aBundle->isFailed()) {
				logMsg = STRING_F(SCAN_FAILED_BUNDLE_FINISHED, aBundle->getName());
			} else {
				logMsg = STRING_F(SCAN_BUNDLE_FINISHED, aBundle->getName());
			}

			if (hasMissing || hasExtras) {
				if (!aBundle->isFailed()) {
					logMsg += " ";
					logMsg += CSTRING(SCAN_PROBLEMS_FOUND);
					logMsg += ":  ";
				}

				logMsg += scanner.getResults();
				if (SETTING(ADD_FINISHED_INSTANTLY)) {
					logMsg += ". " + STRING_F(FORCE_HASH_NOTIFICATION, aBundle->getTarget());
				}

				error_ = STRING_F(SCANNING_FAILED_X, scanner.getResults());
			} else {
				logMsg += ", ";
				logMsg += CSTRING(SCAN_NO_PROBLEMS);
			}

			LogManager::getInstance()->message(logMsg, (hasMissing || hasExtras) ? LogManager::LOG_ERROR : LogManager::LOG_INFO);
			if (hasMissing && !hasExtras)
				return Bundle::STATUS_FAILED_MISSING;
			if (hasExtras)
				return Bundle::STATUS_SHARING_FAILED;
		}
	}

	return Bundle::STATUS_FINISHED;
}

bool ShareScannerManager::onScanSharedDir(const string& aDir, string& error_, bool report) {
	if (!SETTING(SCAN_MONITORED_FOLDERS))
		return true;

	ScanInfo scanner(aDir, report ? ScanInfo::TYPE_SYSLOG : ScanInfo::TYPE_NOREPORT, false);

	scanDir(aDir, scanner);
	find(aDir, Text::toLower(aDir), scanner);

	if (scanner.hasMissing() || scanner.hasExtras()) {
		error_ = scanner.getResults();
		return false;
	}

	return true;
}

void ShareScannerManager::reportMessage(const string& aMessage, ScanInfo& aScan, bool warning /*true*/) {
	if (aScan.reportType == ScanInfo::TYPE_SYSLOG) {
		LogManager::getInstance()->message(aMessage, warning ? LogManager::LOG_WARNING : LogManager::LOG_INFO);
	} else if (aScan.reportType == ScanInfo::TYPE_COLLECT_LOG) {
		aScan.scanMessage += aMessage + "\r\n";
	}
}

bool ShareScannerManager::ScanInfo::hasMissing() const {
	return (missingFiles > 0 || missingNFO > 0 || missingSFV > 0 || noReleaseFiles > 0);
}

bool ShareScannerManager::ScanInfo::hasExtras() const {
	return extrasFound > 0;
}

string ShareScannerManager::ScanInfo::getResults() const {
	string tmp;
	bool first = true;

	auto checkFirst = [&] {
		if (!first) {
			tmp += ", ";
		}
		first = false;
	};

	if (missingFiles > 0) {
		checkFirst();
		tmp += STRING_F(X_MISSING_RELEASE_FILES, missingFiles);
	}

	if (missingSFV > 0) {
		checkFirst();
		tmp += STRING_F(X_MISSING_SFV_FILES, missingSFV);
	}

	if (missingNFO > 0) {
		checkFirst();
		tmp += STRING_F(X_MISSING_NFO_FILES, missingNFO);
	}

	if (extrasFound > 0) {
		checkFirst();
		tmp += STRING_F(X_FOLDERS_EXTRAS, extrasFound);
	}

	if (noReleaseFiles > 0) {
		checkFirst();
		tmp += STRING_F(X_NO_RELEASE_FILES, noReleaseFiles);
	}

	if (emptyFolders > 0) {
		checkFirst();
		tmp += STRING_F(X_EMPTY_FOLDERS, emptyFolders);
	}

	if (dupesFound > 0) {
		checkFirst();
		tmp += STRING_F(X_DUPE_FOLDERS, dupesFound);
	}

	return tmp;
}

} // namespace dcpp
