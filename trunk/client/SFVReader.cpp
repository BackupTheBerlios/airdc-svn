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
#include "SFVReader.h"

#include "StringTokenizer.h"
#include "File.h"
#include "LogManager.h"
#include "FilteredFile.h"
#include "FileReader.h"
#include "ZUtils.h"
#include "Text.h"

#include <iostream>
#include <fstream>

#ifndef _WIN32
#include <dirent.h>
#include <fnmatch.h>
#endif

namespace dcpp {

DirSFVReader::DirSFVReader() : loaded(false) { }

DirSFVReader::DirSFVReader(const string& aPath) : loaded(false) {
	loadPath(aPath);
}

DirSFVReader::DirSFVReader(const string& aPath, const StringList& aSfvFiles) : loaded(false) {
	sfvFiles = aSfvFiles;
	load();
}

void DirSFVReader::loadPath(const string& aPath) {
	content.clear();
	path = aPath;
	sfvFiles = File::findFiles(path, "*.sfv");
	load();
}

bool DirSFVReader::hasFile(const string& fileName, uint32_t& crc32_) const {
	if (loaded) {
		auto p = find_if(content.begin(), content.end(), CompareFirst<string, uint32_t>(fileName));
		if (p != content.end()) {
			crc32_ = p->second;
			return true;
		}
	}
	return false;
}

bool DirSFVReader::hasFile(const string& fileName) const {
	return loaded && find_if(content.begin(), content.end(), CompareFirst<string, uint32_t>(fileName)) != content.end();
}

bool DirSFVReader::isCrcValid(const string& fileName) const {
	auto p = find_if(content.begin(), content.end(), CompareFirst<string, uint32_t>(fileName));
	if (p != content.end()) {
		CRC32Filter crc32;
		FileReader(true).read(path + fileName, [&](const void* x, size_t n) {
			return crc32(x, n), true;
		});
		return crc32.getValue() == p->second;
	}

	return true;
}

void DirSFVReader::load() noexcept {
	string line;
	boost::regex crcReg;
	crcReg.assign("(.{5,200}\\s(\\w{8})$)");

	for(auto i = sfvFiles.begin(); i != sfvFiles.end(); ++i) {

		/* Try to open the sfv */
		ifstream sfv;
		
		try {
			if (File::getSize(Text::utf8ToAcp(path)) > 1000000) {
				throw FileException();
			}

			//incase we have some extended characters in the path
			sfv.open(Text::utf8ToAcp(Util::FormatPath(*i)));

			if(!sfv.is_open()) {
				throw FileException();
			}
		} catch(const FileException&) {
			LogManager::getInstance()->message(STRING(CANT_OPEN_SFV) + *i, LogManager::LOG_ERROR);
			continue;
		}

		/* Get the filename and crc */
		while( getline( sfv, line ) ) {
			line = Text::toUtf8(line);
			//make sure that the line is valid
			if(regex_search(line, crcReg) && (line.find("\\") == string::npos) && (line.find(";") == string::npos)) {
				//only keep the filename
				size_t pos = line.rfind(" ");
				if (pos == string::npos) {
					continue;
				}

				//extra checks to ignore invalid lines
				if (line.length() < 5)
					continue;
				if (line.length() > 200) {
					//can't most likely to detect the line breaks
					LogManager::getInstance()->message(STRING(CANT_OPEN_SFV) + *i, LogManager::LOG_ERROR);
					break;
				}

				uint32_t crc32;
				sscanf(line.substr(pos+1, 8).c_str(), "%x", &crc32);

				line = Text::toLower(line.substr(0,pos));
				//quoted filename?
				if (line[0] == '\"' && line[line.length()-1] == '\"') {
					line = line.substr(1,line.length()-2);
				}

				//don't list the same file multiple times...
				if (!hasFile(line)) {
					content.push_back(make_pair(line, crc32));
				}
			}

		}
		sfv.close();
	}

	loaded = true;
	readPos = content.begin();
}

bool DirSFVReader::read(string& fileName) {
	if (readPos == content.end()) {
		readPos = content.begin();
		return false;
	}
	fileName = readPos->first;
	advance(readPos, 1);
	return true;
}

} // namespace dcpp
