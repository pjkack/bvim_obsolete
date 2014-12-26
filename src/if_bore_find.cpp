
#ifdef FEAT_BORE

#include "if_bore.h"
#include <windows.h>
#include <winnt.h>

//#define BORE_CVPROFILE

#ifdef BORE_CVPROFILE
#pragma comment(lib, "Advapi32.lib")
#include <cvmarkers.h>
//#include <C:\vs2013\Common7\IDE\Extensions\iq325bsi.llt\SDK\Native\Inc\cvmarkers.h>
PCV_PROVIDER g_provider;
PCV_MARKERSERIES g_series1;
int g_cvInitialized;

#define BORE_CVBEGINSPAN(str) CvEnterSpanA(g_series1, &span, str)
#define BORE_CVENDSPAN() do { CvLeaveSpan(span); span = 0; } while(0)
#define BORE_CVINITSPAN PCV_SPAN span = 0
#define BORE_CVDEINITSPAN do { if (span) CvLeaveSpan(span); } while(0)
#else
#define BORE_CVBEGINSPAN(str)
#define BORE_CVENDSPAN()
#define BORE_CVINITSPAN
#define BORE_CVDEINITSPAN
#endif

#define BTSOUTPUT(j) if (p != out_end) *p++ = j; else goto done;
struct ExactStringSearch
{
	virtual int search(const char* text, int text_len, const char* what, int what_len, int* out, const int* out_end) const = 0;
};

struct QuickSearch : public ExactStringSearch
{
	QuickSearch(const char *what, int what_len)
	{
		/* Preprocessing */
		preQsBc((const unsigned char*)what, what_len, m_qsBc);
	}

	virtual int search (const char* text, int text_len, const char* what, int what_len, int* out, const int* out_end) const
	{
		int j;
		const unsigned char* x = (const unsigned char*)what;
		int m = what_len;
		const unsigned char* y = (const unsigned char*)text;
		int n = text_len;
		int* p = out;

		j = 0;
		while (j <= n - m) {
			if (memcmp(x, y + j, m) == 0)
				BTSOUTPUT(j);
			j += m_qsBc[y[j + m]];               /* shift */
		}

	done:
		return p - out;
	}

private:
	enum { ASIZE = 256 };
	int m_qsBc[ASIZE];
	void preQsBc(const unsigned char *x, int m, int qsBc[]) {
		int i;

		for (i = 0; i < ASIZE; ++i)
			qsBc[i] = m + 1;
		for (i = 0; i < m; ++i)
			qsBc[x[i]] = m - i;
	}
};

struct OldSearch : public ExactStringSearch
{
	virtual int search(const char* text, int text_len, const char* what, int what_len, int* out, const int* out_end) const
	{
		// http://www-igm.univ-mlv.fr/~lecroq/string/index.html
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
	}
};

#undef BTSOUTPUT

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

struct SearchContext 
{
	bore_t* b;
	LONG* remainingFileCount;
	bore_alloc_t filedata;
	const ExactStringSearch* search;
	const char* what;
	int what_len;
	bore_match_t* match;
	LONG matchSize;
	LONG* matchCount;
	int wasTruncated;
};

static void SearchOneFile(struct SearchContext* searchContext, const char* filename, int fileIndex)
{
	HANDLE fileHandle = INVALID_HANDLE_VALUE;
	bore_search_result_t search_result = {0};
	BORE_CVINITSPAN;

	{
		BORE_CVBEGINSPAN("opn");
		WCHAR fn[BORE_MAX_PATH];
		int result = MultiByteToWideChar(CP_UTF8, 0, filename, -1, fn, BORE_MAX_PATH);
		if (result == 0)
		{
			goto skip;
		}

		fileHandle = CreateFileW(fn, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
		if (fileHandle == INVALID_HANDLE_VALUE)
		{
			goto skip;
		}
		BORE_CVENDSPAN();
	}

	{
		BORE_CVBEGINSPAN("rd");

		DWORD filesize = GetFileSize(fileHandle, 0);
		if (filesize == INVALID_FILE_SIZE)
			goto skip;

		searchContext->filedata.cursor = searchContext->filedata.base;
		bore_alloc(&searchContext->filedata, filesize);

		char* p = (char*)searchContext->filedata.base;
		DWORD remaining = filesize;
		while(remaining) {
			DWORD readbytes;
			if(!ReadFile(fileHandle, p + filesize - remaining, remaining, &readbytes, 0))
				goto skip;
			remaining -= readbytes;
		}

		BORE_CVENDSPAN();
	}


	{
		BORE_CVBEGINSPAN("srch");

		// Search for the text
		int match_offset[BORE_MAXMATCHPERFILE];
		int match_in_file = searchContext->search->search(
			(char*)searchContext->filedata.base, 
			searchContext->filedata.cursor - searchContext->filedata.base,
			searchContext->what, 
			searchContext->what_len, 
			&match_offset[0], 
			&match_offset[BORE_MAXMATCHPERFILE]);

		// Fill the result with the line's text, etc.
		bore_resolve_match_location(
			fileIndex, 
			(char*)searchContext->filedata.base, 
			searchContext->filedata.cursor - searchContext->filedata.base, 
			&search_result.result[0], 
			&search_result.result[BORE_MAXMATCHPERFILE], 
			match_offset, 
			match_in_file);

		if (match_in_file == BORE_MAXMATCHPERFILE)
			searchContext->wasTruncated = 1;

		search_result.hits = match_in_file;

		BORE_CVENDSPAN();
	}

	{
		BORE_CVBEGINSPAN("wr");

		LONG startIndex = InterlockedExchangeAdd(searchContext->matchCount, search_result.hits);

		int n = search_result.hits;
		if (startIndex + n >= searchContext->matchSize)
		{
			searchContext->wasTruncated = 2; // Out of space. Signal quit.
			n = searchContext->matchSize - startIndex;
		}

		if (n > 0)
			memcpy(&searchContext->match[startIndex], search_result.result, sizeof(bore_match_t) * n);
	
		BORE_CVENDSPAN();
	}

skip:
	if (fileHandle != INVALID_HANDLE_VALUE) 
	{
		CloseHandle(fileHandle);
	}
	BORE_CVDEINITSPAN;
}

static DWORD WINAPI SearchWorker(struct SearchContext* searchContext)
{
	for (;;)
	{
		LONG fileIndex = InterlockedDecrement(searchContext->remainingFileCount);
		if (fileIndex < 0)
			break;

		u32* const files = (u32*)searchContext->b->file_alloc.base;
		SearchOneFile(searchContext, bore_str(searchContext->b, files[fileIndex]), fileIndex);

		if (searchContext->wasTruncated > 1)
			break;

	}
	return 0;
}

int bore_dofind(bore_t* b, int threadCount, int* truncated_, bore_match_t* match, int match_size, const char* what)
{
#ifdef BORE_CVPROFILE
	if (!g_cvInitialized)
	{
		GUID guid = { 0x551695cb, 0x80ac, 0x4c14, 0x98, 0x58, 0xec, 0xb9, 0x43, 0x48, 0xd4, 0x3e };
		CvInitProvider(&guid, &g_provider);
		CvCreateMarkerSeriesA(g_provider, "bore_find", &g_series1);
		g_cvInitialized = 1;
	}
#endif	

	u32* const files = (u32*)b->file_alloc.base;
	const int what_len = strlen(what);
	LONG file_count = b->file_count;
	*truncated_ = 0;	
	
	QuickSearch search(what, what_len);

	if (threadCount < 1)
	{
		threadCount = 1;
	}
	else if (threadCount > 32) 
	{
		threadCount = 32;
	}
	
	HANDLE threads[32] = {0};
	SearchContext searchContexts[32] = {0};
	LONG match_count = 0;
	for (int i = 0; i < threadCount; ++i) 
	{
		searchContexts[i].b = b;
		searchContexts[i].remainingFileCount = &file_count;
		bore_prealloc(&searchContexts[i].filedata, 100000);
		searchContexts[i].search = &search;
		searchContexts[i].what = what;
		searchContexts[i].what_len = what_len;
		searchContexts[i].match = match;
		searchContexts[i].matchSize = match_size;
		searchContexts[i].matchCount = &match_count;
	}

	for (int i = 0; i < threadCount - 1; ++i)
		threads[i] = CreateThread(0, 0, (LPTHREAD_START_ROUTINE)SearchWorker, &searchContexts[i], 0, 0);

	SearchWorker(&searchContexts[threadCount - 1]);

	WaitForMultipleObjects(threadCount - 1, threads, TRUE, INFINITE);

	for (int i = 0; i < threadCount; ++i)
	{
		if (searchContexts[i].wasTruncated > *truncated_)
			*truncated_ = searchContexts[i].wasTruncated;
	}

	if (*truncated_ > 1)
		match_count = match_size;

	for (int i = 0; i < threadCount; ++i) 
	{
		bore_alloc_free(&searchContexts[i].filedata);
	}

	return match_count;
}


#endif
