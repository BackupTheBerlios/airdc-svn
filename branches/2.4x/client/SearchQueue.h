/* 
 * Copyright (C) 2003-2006 RevConnect, http://www.revconnect.com
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

#pragma once

#include "Thread.h"
#include "Search.h"

namespace dcpp {

class SearchQueue
{
public:
	
	SearchQueue(int32_t aInterval = 0);
	~SearchQueue();

	uint64_t add(Search* s);
	Search* pop();
	
	void clear()
	{
		Lock l(cs);
		searchQueue.clear();
	}

	bool cancelSearch(void* aOwner);

	/* Interval defined by the client (settings or fav hub interval) */
	int32_t minInterval;
	uint64_t getNextSearchTick() { return lastSearchTime+nextInterval; }
	bool hasWaitingTime(uint64_t aTick);
	uint64_t lastSearchTime;
private:
	int32_t getInterval(const Search* aSearch) const;

	deque<Search*>	searchQueue;
	int32_t		nextInterval;
	CriticalSection cs;
};

}