
#ifdef FEAT_BORE

#include "if_bore.h"
#include <windows.h>
#include <winnt.h>
#include <ppl.h>
#include <functional>

// Circular buffer
template <typename T>
class SyncQueue
{
public:
	explicit SyncQueue(int capacity)
	{
		data_ = new T[capacity];
		capacity_ = capacity;
		queueBegin_ = 0;
		queueSize_ = 0;
		cancelSignal_ = 0;
		InitializeConditionVariable(&notEmpty_);
		InitializeConditionVariable(&notFull_);
		InitializeCriticalSection(&lock_);
	}

	~SyncQueue()
	{
		delete [] data_;
		DeleteCriticalSection(&lock_);
	}

	bool get(T& item)
	{
		EnterCriticalSection(&lock_);
		while(!queueSize_ && !cancelSignal_) 
			SleepConditionVariableCS(&notEmpty_, &lock_, INFINITE);
		if (cancelSignal_ && !queueSize_) {
			LeaveCriticalSection(&lock_);
			return false;
		}	
		item = data_[queueBegin_];
		--queueSize_;
		queueBegin_ = (queueBegin_ + 1) % capacity_;
		LeaveCriticalSection(&lock_);
		WakeConditionVariable(&notFull_);
		return true;
	}

	bool put(const T& item)
	{
		EnterCriticalSection(&lock_);
		while (queueSize_ == capacity_ && !cancelSignal_)
			SleepConditionVariableCS(&notFull_, &lock_, INFINITE);
		if (cancelSignal_) {
			LeaveCriticalSection(&lock_);
			return false;
		}	
		data_[(queueBegin_ + queueSize_) % capacity_] = item;
		++queueSize_;
		LeaveCriticalSection(&lock_);
		WakeConditionVariable(&notEmpty_);
		return true;
	}

	void donePutting()
	{
		EnterCriticalSection(&lock_);
		cancelSignal_ = 1;
		LeaveCriticalSection(&lock_);
		WakeAllConditionVariable(&notEmpty_);
		WakeAllConditionVariable(&notFull_);
	}

private:
	CONDITION_VARIABLE notEmpty_;
	CONDITION_VARIABLE notFull_;
	CRITICAL_SECTION lock_;
	LONG cancelSignal_;
	int queueBegin_;
	int queueSize_;
	int capacity_;
	T* data_;
};

enum WorkStatus 
{
	WSSuccess,
	WSSkip,
	WSNoMoreWork,
};

template <typename Tin, typename Tout>
class WorkGroup
{
public:
	typedef WorkStatus (*ProcessFunc)(const Tin* pin, Tout* pout, PVOID context);

	WorkGroup(int count, SyncQueue<Tin>* qin, SyncQueue<Tout>* qout, ProcessFunc f, PVOID fcontext)
	{
		qin_ = qin;
		qout_ = qout;
		func_ = f;
		funcContext_ = fcontext;
		count_ = count;
		thread_ = new HANDLE[count];
		DWORD tid;
		for (int i = 0; i < count; ++i)
			thread_[i] = CreateThread(0, 0, threadProc, this, 0, &tid);
	}
	
	~WorkGroup()
	{
		for (int i = 0; i < count_; ++i)
			CloseHandle(thread_[i]);
		delete [] thread_;
	}

	void join()
	{
		WaitForMultipleObjects(count_, thread_, TRUE, INFINITE);
	}

private:
	static DWORD WINAPI threadProc(PVOID p)
	{
		WorkGroup<Tin, Tout>* wg = (WorkGroup<Tin, Tout>*)p;
		for(;;)
		{
			Tin inItem;
			if (wg->qin_)
				if (!wg->qin_->get(inItem)) {
					if (wg->qout_)
						wg->qout_->donePutting();
					return 0;
				}
			Tout outItem;
			WorkStatus ws = (*wg->func_)(&inItem, &outItem, wg->funcContext_);
			if (ws == WSNoMoreWork) {
				if (wg->qout_)
					wg->qout_->donePutting();
				return 0;
			}
			if (wg->qout_ && ws == WSSuccess)
				if (!wg->qout_->put(outItem))
					return 0;
		}
	}

private:
	int count_;
	HANDLE* thread_;
	SyncQueue<Tin>* qin_;
	SyncQueue<Tout>* qout_;
	ProcessFunc func_;
	PVOID funcContext_;
};

static int bore_text_search(const char* text, int text_len, const char* what, int what_len, int* out, const int* out_end)
{
	// http://www-igm.univ-mlv.fr/~lecroq/string/index.html
#define BTSOUTPUT(j) if (p != out_end) *p++ = j; else goto done;
	const char* y = text;
	int n = text_len;
	const char* x = what;
	int m = what_len;
	int* p = out;
	int j, k, ell;

	/* Preprocessing */
	if (x[0] == x[1]) {
		k = 2;
		ell = 1;
	}
	else {
		k = 1;
		ell = 2;
	}

	/* Searching */
	j = 0;
	while (j <= n - m) {
		if (x[1] != y[j + 1])
			j += k;
		else {
			if (memcmp(x + 2, y + j + 2, m - 2) == 0 && x[0] == y[j]) {
				BTSOUTPUT(j);
			}
			j += ell;
		}
	}
done:
	return p - out;
#undef BTSOUTPUT
}

static void bore_resolve_match_location(int file_index, const char* p, u32 filesize, 
	bore_match_t* match, bore_match_t* match_end, int* offset, int offsetCount)
{
	const int* offset_end = offset + offsetCount;
	const char* pbegin = p;
	const char* linebegin = p;
	const char* fileend = p + filesize;
	int line = 1;

	while(offset < offset_end && match < match_end) {
		const char* pend = pbegin + *offset;
		while (p < pend) {
			if (*p++ == '\n') {
				++line;
				linebegin = p;
			}
		}
		const char* lineend = pend;
		while (lineend < fileend && *lineend != '\r' && *lineend != '\n')
			++lineend;
		size_t linelen = lineend - linebegin;
		if (linelen > sizeof(match->line) - 1)
			linelen = sizeof(match->line) - 1;
		match->file_index = file_index;
		match->row = line;
		match->column = p - linebegin;
		memcpy(match->line, linebegin, linelen);
		match->line[linelen] = 0;
		++match;
		++offset;
	}
}

#if 1

struct FileHandle
{
	HANDLE h;
	int fileindex;
};

struct OpenFileContext
{
	bore_t* b;
	LONG* file_index;
};

WorkStatus openFileJob(const short*, FileHandle* hout, PVOID p)
{
	OpenFileContext* c = (OpenFileContext*)p;
	WCHAR fn[BORE_MAX_PATH];
	u32* const files = (u32*)c->b->file_alloc.base;
	LONG n = InterlockedExchangeAdd(c->file_index, 1);
	if (n >= c->b->file_count)
		return WSNoMoreWork;
	int result = MultiByteToWideChar(CP_UTF8, 0, bore_str(c->b, files[n]), -1, fn, BORE_MAX_PATH);
	if (result == 0)
		return WSSkip;
	hout->fileindex = n;
	hout->h = CreateFileW(fn, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
	if (hout->h == INVALID_HANDLE_VALUE)
		return WSSkip;
	return WSSuccess;
}

struct ReadFileContext
{
	bore_t* b;
	LPCRITICAL_SECTION searchJobLock;
	bool* searchJobAlloc;
};

WorkStatus readFileJob(const FileHandle* pin, int* searchIndex, PVOID ctx)
{
	ReadFileContext* c = (ReadFileContext*)ctx;
	HANDLE f = pin->h;
	DWORD filesize = GetFileSize(f, 0);
	if (filesize == INVALID_FILE_SIZE)
		goto skip;

	int index = -1;
	do
	{
		EnterCriticalSection(c->searchJobLock);
		for (int i = 0; i < BORE_SEARCH_JOBS; ++i)
			if (c->searchJobAlloc[i] == false) {
				c->searchJobAlloc[i] = true;
				index = i;
				break;
			}
		LeaveCriticalSection(c->searchJobLock);
	} while(index == -1);

	bore_search_job_t* sj = &c->b->search[index];
	sj->fileindex = pin->fileindex;
	sj->filedata.cursor = sj->filedata.base;
	bore_alloc(&sj->filedata, filesize);
	char* p = (char*)sj->filedata.base;
	DWORD remaining = filesize;
	while(remaining) {
		DWORD readbytes;
		if(!ReadFile(f, p + filesize - remaining, remaining, &readbytes, 0))
			goto skip;
		remaining -= readbytes;
	}

	*searchIndex = index;
	
	CloseHandle(f);
	return WSSuccess;
skip:
	CloseHandle(f);
	return WSSkip;
}

struct SearchFileContext
{
	bore_t* b;
	LPCRITICAL_SECTION searchJobLock;
	bool* searchJobAlloc;
	LPCRITICAL_SECTION searchResultLock;
	bool* searchResultAlloc;
	const char* what;
	int what_len;
};

WorkStatus searchFileJob(const int* searchIndex, int* searchResultIndex, PVOID ctx)
{
	SearchFileContext* c = (SearchFileContext*)ctx;
	bore_search_job_t* sj = &c->b->search[*searchIndex];

	// Search for the text
	int match_offset[BORE_MAXMATCHPERFILE];
	int match_in_file = bore_text_search((char*)sj->filedata.base, sj->filedata.cursor - sj->filedata.base,
			c->what, c->what_len, &match_offset[0], &match_offset[BORE_MAXMATCHPERFILE]);
	
	if (0 == match_in_file)
	{
		// Done with the file buffer
		EnterCriticalSection(c->searchJobLock);
		c->searchJobAlloc[*searchIndex] = false;
		LeaveCriticalSection(c->searchJobLock);
		return WSSkip;
	}
	
	// Grab a slot for storing the result
	int index = -1;
	do
	{
		EnterCriticalSection(c->searchResultLock);
		for (int i = 0; i < BORE_SEARCH_RESULTS; ++i)
			if (c->searchResultAlloc[i] == false) {
				c->searchResultAlloc[i] = true;
				index = i;
				break;
			}
		LeaveCriticalSection(c->searchResultLock);
	} while(index == -1);

	// Fill the result with the line's text, etc.
	bore_resolve_match_location(sj->fileindex, (char*)sj->filedata.base, sj->filedata.cursor - sj->filedata.base, 
		&c->b->search_result[index].result[0], &c->b->search_result[index].result[BORE_MAXMATCHPERFILE], 
		match_offset, match_in_file);
	c->b->search_result[index].hits = match_in_file;

	// Done with the file buffer
	EnterCriticalSection(c->searchJobLock);
	c->searchJobAlloc[*searchIndex] = false;
	LeaveCriticalSection(c->searchJobLock);

	// Pass the search result slot on to the next job 
	*searchResultIndex = index;
	return WSSuccess;
}

struct WriteResultContext
{
	bore_t* b;
	LPCRITICAL_SECTION searchResultLock;
	bool* searchResultAlloc;
	int truncated;
	bore_match_t* match;
	int match_capacity;
	int match_count;
};

WorkStatus writeResultJob(const int* resultIndex, short*, PVOID ctx)
{
	WriteResultContext* c = (WriteResultContext*)ctx;
	bore_search_result_t* sr = &c->b->search_result[*resultIndex];

	int n = sr->hits + c->match_count <= c->match_capacity ? sr->hits : c->match_capacity - c->match_count;
	memcpy(&c->match[c->match_count], sr->result, sizeof(bore_match_t) * n);
	c->match_count += n;
	c->truncated = sr->hits == c->match_capacity || sr->hits == BORE_MAXMATCHPERFILE;

	// Done with the result
	EnterCriticalSection(c->searchResultLock);
	c->searchResultAlloc[*resultIndex] = false;
	LeaveCriticalSection(c->searchResultLock);

	return WSSuccess;
}

int bore_dofind(bore_t* b, int* truncated_, bore_match_t* match, int match_size, const char* what)
{
	u32* const files = (u32*)b->file_alloc.base;
	const int what_len = strlen(what);
	const int file_count = b->file_count;
	__declspec(align(BORE_CACHELINE)) LONG match_index = 0;
	__declspec(align(BORE_CACHELINE)) LONG file_index = 0;
	__declspec(align(BORE_CACHELINE)) bool searchJobAlloc[BORE_SEARCH_JOBS];
	__declspec(align(BORE_CACHELINE)) CRITICAL_SECTION searchJobLock;
	__declspec(align(BORE_CACHELINE)) bool searchResultAlloc[BORE_SEARCH_RESULTS];
	__declspec(align(BORE_CACHELINE)) CRITICAL_SECTION searchResultLock;

	memset(searchJobAlloc, 0, sizeof(searchJobAlloc));
	InitializeCriticalSection(&searchJobLock);

	memset(searchResultAlloc, 0, sizeof(searchResultAlloc));
	InitializeCriticalSection(&searchResultLock);

	static int queue1Capacity= 10;
	static int queue2Capacity= BORE_SEARCH_JOBS;
	static int queue3Capacity= BORE_SEARCH_RESULTS;
	static int wg1Capacity = 5; // File open threads
	static int wg2Capacity = 1; // Read file threads
	static int wg3Capacity = 1; // Search threads
	static int wg4Capacity = 1; // Write result thread. Must be 1

	// 1, 1, 1, 1 : 1400ms
	// 1, 1, 2, 1 : 1400ms
	// 2, 1, 1, 1 : 800ms
	// 3, 1, 1, 1 : 630ms
	// 4, 1, 1, 1 : 500ms
	// 5, 1, 1, 1 :	480ms  <---
	// 6, 1, 1, 1 :	530ms 
	// 4, 1, 4, 1 :	500ms
	// 4, 2, 1, 1 :	530ms

	SyncQueue<FileHandle> q1(queue1Capacity); // Open file handle
	SyncQueue<int> q2(queue2Capacity); // Search job index
	SyncQueue<int> q3(queue3Capacity); // Search result index

	OpenFileContext ofc;
	ofc.b = b;
	ofc.file_index = &file_index;
	WorkGroup<short, FileHandle> wg1(wg1Capacity, 0, &q1, &openFileJob, &ofc);

	ReadFileContext rfc;
	rfc.b = b;
	rfc.searchJobAlloc = searchJobAlloc;
	rfc.searchJobLock = &searchJobLock;
	WorkGroup<FileHandle, int> wg2(wg2Capacity, &q1, &q2, &readFileJob, &rfc);

	SearchFileContext sfc;
	sfc.b = b;
	sfc.searchJobAlloc = searchJobAlloc;
	sfc.searchJobLock = &searchJobLock;
	sfc.searchResultAlloc = searchResultAlloc;
	sfc.searchResultLock = &searchResultLock;
	sfc.what = what;
	sfc.what_len = strlen(what);
	WorkGroup<int, int> wg3(wg3Capacity, &q2, &q3, &searchFileJob, &sfc);

	WriteResultContext wrc;
	wrc.b = b;
	wrc.searchResultAlloc = searchResultAlloc;
	wrc.searchResultLock = &searchResultLock;
	wrc.truncated = 0;
	wrc.match = match;
	wrc.match_capacity = match_size;
	wrc.match_count = 0;
	WorkGroup<int, short> wg4(wg4Capacity, &q3, 0, &writeResultJob, &wrc);

	wg4.join();
	wg3.join();
	wg2.join();
	wg1.join();

	DeleteCriticalSection(&searchJobLock);
	DeleteCriticalSection(&searchResultLock);

	*truncated_ = wrc.truncated;
	return wrc.match_count;
}


#endif