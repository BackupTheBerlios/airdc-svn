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
#include "SimpleXML.h"
#include "Streams.h"

#include "File.h"

namespace dcpp {

const string SimpleXML::utf8Header = "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\"?>\r\n";

string& SimpleXML::escape(string& aString, bool aAttrib, bool aLoading /* = false */, const string &encoding /* = "UTF-8" */) {
	string::size_type i = 0;
	const char* chars = aAttrib ? "<&>'\"" : "<&>";
	
	if(aLoading) {
		while((i = aString.find('&', i)) != string::npos) {
			if(aString.compare(i+1, 3, "lt;") == 0) {
				aString.replace(i, 4, 1, '<');
			} else if(aString.compare(i+1, 4, "amp;") == 0) {
				aString.replace(i, 5, 1, '&');
			} else if(aString.compare(i+1, 3, "gt;") == 0) {
				aString.replace(i, 4, 1, '>');
			} else if(aAttrib) {
				if(aString.compare(i+1, 5, "apos;") == 0) {
					aString.replace(i, 6, 1, '\'');
				} else if(aString.compare(i+1, 5, "quot;") == 0) {
					aString.replace(i, 6, 1, '"');
				}
			}
			i++;
		}
		i = 0;
		if( (i = aString.find('\n')) != string::npos) {
			if(i > 0 && aString[i-1] != '\r') {
				// This is a unix \n thing...convert it...
				i = 0;
				while( (i = aString.find('\n', i) ) != string::npos) {
					if(aString[i-1] != '\r')
						aString.insert(i, 1, '\r');
				
					i+=2;
				}
			}
		}
		aString = Text::toUtf8(aString, encoding);
	} else {
		while( (i = aString.find_first_of(chars, i)) != string::npos) {
			switch(aString[i]) {
			case '<': aString.replace(i, 1, "&lt;"); i+=4; break;
			case '&': aString.replace(i, 1, "&amp;"); i+=5; break;
			case '>': aString.replace(i, 1, "&gt;"); i+=4; break;
			case '\'': aString.replace(i, 1, "&apos;"); i+=6; break;
			case '"': aString.replace(i, 1, "&quot;"); i+=6; break;
			default: dcassert(0);
				}
		}
		// No need to convert back to acp since our utf8Header denotes we
		// should store it as utf8.
	}
	return aString;
}

bool SimpleXML::loadSettingFile(Util::Paths aPath, const string& aFileName, bool migrate /*true*/) {
	string fname = Util::getPath(aPath) + aFileName;

	if (migrate)
		Util::migrate(fname);

	if (!Util::fileExists(fname))
		return false;

	fromXML(File(fname, File::READ, File::OPEN).read());
	return true;
}

void SimpleXML::saveSettingFile(Util::Paths aPath, const string& aFileName) {
	string fname = Util::getPath(aPath) + aFileName;

	//try {
		File f(fname + ".tmp", File::WRITE, File::CREATE | File::TRUNCATE);
		f.write(SimpleXML::utf8Header);
		f.write(toXML());
		f.close();

		//dont overWrite with empty file.
		if(File::getSize(fname + ".tmp") > 0) {
			File::deleteFile(fname);
			File::renameFile(fname + ".tmp", fname);
		}
	//} catch(const FileException&) {
	//}
}

void SimpleXML::Tag::appendAttribString(string& tmp) {
	for(auto& i: attribs) {
		tmp.append(i.first);
		tmp.append("=\"", 2);
		if(needsEscape(i.second, true)) {
			string tmp2(i.second);
			escape(tmp2, true);
			tmp.append(tmp2);
		} else {
			tmp.append(i.second);
		}
		tmp.append("\" ", 2);
	}
	tmp.erase(tmp.size()-1);
}

/**
 * The same as the version above, but writes to a file instead...yes, this could be made
 * with streams and only one code set but streams are slow...the file f should be a buffered
 * file, otherwise things will be very slow (I assume write is not expensive and call it a lot
 */
void SimpleXML::Tag::toXML(int indent, OutputStream* f, bool /*noIndent*/ /*false*/) {
	if(children.empty() && data.empty() && !forceEndTag) {
		string tmp;
		tmp.reserve(indent + name.length() + 30);
		tmp.append(indent, '\t');
		tmp.append(1, '<');
		tmp.append(name);
		tmp.append(1, ' ');
		appendAttribString(tmp);
		tmp.append("/>\r\n", 4);
		f->write(tmp);
	} else {
		string tmp;
		tmp.append(indent, '\t');
		tmp.append(1, '<');
		tmp.append(name);
		tmp.append(1, ' ');
		appendAttribString(tmp);
		if(children.empty()) {
			tmp.append(1, '>');
			if(needsEscape(data, false)) {
				string tmp2(data);
				escape(tmp2, false);
				tmp.append(tmp2);
			} else {
				tmp.append(data);
			}
		} else {
			tmp.append(">\r\n", 3);
			f->write(tmp);
			tmp.clear();
			for(auto& i: children) {
				i->toXML(indent + 1, f);
			}
			tmp.append(indent, '\t');
		}
		tmp.append("</", 2);
		tmp.append(name);
		tmp.append(">\r\n", 3);
		f->write(tmp);
	}
}

bool SimpleXML::findChild(const string& aName) noexcept {
	dcassert(current != NULL);
	if (!current)
		return false;

	if(found && currentChild != current->children.end())
		currentChild++;
	
	while(currentChild!=current->children.end()) {
		if((*currentChild)->name == aName) {
			found = true;
			return true;
		} else
			currentChild++;
	}
	return false;
}

void SimpleXML::addTag(const string& aName, const string& aData /* = "" */) {
	if(aName.empty()) {
		throw SimpleXMLException("Empty tag names not allowed");
	}

	if(current == &root && !current->children.empty()) {
			throw SimpleXMLException("Only one root tag allowed");
	} else {
		current->children.push_back(new Tag(aName, aData, current));
		currentChild = current->children.end() - 1;
	}
}

void SimpleXML::addAttrib(const string& aName, const string& aData) {
	if(current == &root)
		throw SimpleXMLException("No tag is currently selected");

	current->attribs.emplace_back(aName, aData);
}

void SimpleXML::addChildAttrib(const string& aName, const string& aData) {
	checkChildSelected();

	(*currentChild)->attribs.emplace_back(aName, aData);
}

void SimpleXML::replaceChildAttrib(const string& aName, const string& aData) {
	checkChildSelected();

	auto i = find_if((*currentChild)->attribs.begin(), (*currentChild)->attribs.end(), CompareFirst<string,string>(aName));
	if(i != (*currentChild)->attribs.end()) {
		(*i).second = aData;
	} else {
		(*currentChild)->attribs.emplace_back(aName, aData);
	}
}

string SimpleXML::toXML() { 
	string tmp; 
	StringOutputStream os(tmp); 
	toXML(&os); 
	return tmp; 
}

void SimpleXML::toXML(OutputStream* f) { 
	if(!root.children.empty()) 
		root.children[0]->toXML(0, f); 
}

string SimpleXML::childToXML() {
	string tmp; 
	StringOutputStream os(tmp); 
	(*currentChild)->toXML(0, &os, true); 
	return tmp; 
}

void SimpleXML::fromXML(const string& aXML) {
	if(!root.children.empty()) {
		delete root.children[0];
		root.children.clear();
	}

	TagReader t(&root);
	SimpleXMLReader(&t).parse(aXML);
	
	if(root.children.size() != 1) {
		throw SimpleXMLException("Invalid XML file, missing or multiple root tags");
	}
	
	current = &root;
	resetCurrentChild();
}

} // namespace dcpp