
#ifdef FEAT_BORE

#include "if_bore.h"
#include <windows.h>
#include <winnt.h>
#include <ppl.h>
#include <functional>


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

static int bore_dofind_one(bore_search_job_t* sj, int* truncated, int fileindex, const char *filename, const char* what, int what_len)
{
	WCHAR fn[BORE_MAX_PATH];
	HANDLE f;
	DWORD filesize;
	DWORD remaining;
	char* p;
	int match_offset[BORE_MAXMATCHPERFILE];
	int match_in_file = 0;
	int result = MultiByteToWideChar(CP_UTF8, 0, filename, -1, fn, BORE_MAX_PATH);
	*truncated = 0;
	if (result == 0)
		return 0;
	f = CreateFileW(fn, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
	if (f == INVALID_HANDLE_VALUE)
		goto skip;
	filesize = GetFileSize(f, 0);
	if (filesize == INVALID_FILE_SIZE)
		goto skip;
	sj->filedata.cursor = sj->filedata.base;
	bore_alloc(&sj->filedata, filesize);
	p = (char*)sj->filedata.base;
	remaining = filesize;
	while(remaining) {
		DWORD readbytes;
		if(!ReadFile(f, p + filesize - remaining, remaining, &readbytes, 0))
			goto skip;
		remaining -= readbytes;
	}
	
	CloseHandle(f);
	
	match_in_file = bore_text_search(p, filesize, what, what_len, &match_offset[0], 
		&match_offset[BORE_MAXMATCHPERFILE]);

	if (match_in_file == BORE_MAXMATCHPERFILE)
		*truncated = 1; // Wrong if there was exactly BORE_MAXMATCHPERFILE in the file, but whatever...

	bore_resolve_match_location(fileindex, p, filesize, &sj->result[0], &sj->result[BORE_MAXMATCHPERFILE], match_offset,
		match_in_file);

	return match_in_file;
skip:
	CloseHandle(f);
	return match_in_file;
}

int bore_dofind(bore_t* b, int* truncated_, bore_match_t* match, int match_size, const char* what)
{
	u32* const files = (u32*)b->file_alloc.base;
	const int what_len = strlen(what);
	const int file_count = b->file_count;
	__declspec(align(BORE_CACHELINE)) LONG match_index = 0;
	__declspec(align(BORE_CACHELINE)) LONG file_index = 0;
	__declspec(align(BORE_CACHELINE)) LONG truncated = 0;

	//Concurrency::task_group tasks;

	const LONG n = BORE_CACHELINE/sizeof(files[0]); // files per batch
	auto fn = [=](int job_index, LONG* match_index, LONG* file_index, LONG* truncated) -> std::function<void ()> {
		return [=] {
			bore_search_job_t* sj = &b->search[job_index];
			for(;;)
			{
				int begin_index = (int)InterlockedExchangeAdd(file_index, n);
				int end_index = begin_index + n;
				if (end_index > file_count) {
					if (begin_index >= file_count) {
						return;
					} else {
						end_index = file_count;
					}
				}
				for(int i = begin_index; i < end_index; ++i) {
					const char* fn = bore_str(b, files[i]);
					int truncatedSearch = 0;
					int match_count = bore_dofind_one(sj, &truncatedSearch, i, fn, what, what_len);
					if (!match_count)
						continue;
					int beginMatchIndex = (int)::InterlockedExchangeAdd(match_index, match_count);
					int endMatchIndex = beginMatchIndex + match_count;
					if (truncatedSearch || (endMatchIndex > match_size)) {
						if (beginMatchIndex <= match_size) {
							InterlockedIncrement(truncated);
							memcpy(&match[beginMatchIndex], sj->result, (match_size - beginMatchIndex) * sizeof(bore_match_t));
						}
						return;
					}
					memcpy(&match[beginMatchIndex], sj->result, (endMatchIndex - beginMatchIndex) * sizeof(bore_match_t));
				}
			}
	    };
	};

#if BORE_SEARCH_JOBS == 1
	fn(0, &match_index, &file_index, &truncated)();
#else
	Concurrency::parallel_invoke(
		fn(0, &match_index, &file_index, &truncated)
#if BORE_SEARCH_JOBS > 1
		, fn(1, &match_index, &file_index, &truncated)
#endif
#if BORE_SEARCH_JOBS > 2
		, fn(2, &match_index, &file_index, &truncated)
#endif
#if BORE_SEARCH_JOBS > 3
		, fn(3, &match_index, &file_index, &truncated)
#endif
#if BORE_SEARCH_JOBS > 4
		, fn(4, &match_index, &file_index, &truncated)
#endif
#if BORE_SEARCH_JOBS > 5
		, fn(5, &match_index, &file_index, &truncated)
#endif
#if BORE_SEARCH_JOBS > 6
		, fn(6, &match_index, &file_index, &truncated)
#endif
#if BORE_SEARCH_JOBS > 7
		, fn(7, &match_index, &file_index, &truncated)
#endif
#endif

		);
//	tasks.wait();
	*truncated_ = truncated ? 1 : 0;
	return truncated ? match_size : (int)match_index;
}

#else

int bore_dofind(bore_t* b, FILE* cf, bore_match_t* match, int match_size, const char* what)
{
	u32* files = (u32*)b->file_alloc.base;
	int found = 0;
	int truncated = 0;
	enum { BORE_MAXMATCHPERFILE = 100 };
	for(int i = 0; (i < b->file_count) && (found < match_size); ++i) {
		WCHAR fn[BORE_MAX_PATH];
		HANDLE f;
		DWORD filesize;
		DWORD remaining;
		char* p;
		int match_offset[BORE_MAXMATCHPERFILE];
		int match_in_file;
		int result = MultiByteToWideChar(CP_UTF8, 0, bore_str(b, files[i]), -1, fn, BORE_MAX_PATH);
		if (result == 0)
			continue;
		f = CreateFileW(fn, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
		if (f == INVALID_HANDLE_VALUE)
			goto skip;
		filesize = GetFileSize(f, 0);
		if (filesize == INVALID_FILE_SIZE)
			goto skip;
		b->fsearch_alloc.cursor = b->fsearch_alloc.base;
		bore_alloc(&b->fsearch_alloc, filesize);
		p = (char*)b->fsearch_alloc.base;
		remaining = filesize;
		while(remaining) {
			DWORD readbytes;
			if(!ReadFile(f, p + filesize - remaining, remaining, &readbytes, 0))
				goto skip;
			remaining -= readbytes;
		}
		
		CloseHandle(f);
		
		match_in_file = bore_text_search(p, filesize, what, strlen(what), &match_offset[0], 
			&match_offset[BORE_MAXMATCHPERFILE]);

		if (match_in_file == BORE_MAXMATCHPERFILE)
			truncated = 1; // Wrong if there was exactly BORE_MAXMATCHPERFILE in the file, but whatever...

		if ((match_size - found) < match_in_file) {
			truncated = 1;
			match_in_file = match_size - found;
		}

		bore_resolve_match_location(i, p, filesize, &match[found], &match[match_size], match_offset,
			match_in_file);

		bore_save_match_to_file(b, cf, p + filesize, &match[found], match_in_file);

		found += match_in_file;
		continue;
skip:
		CloseHandle(f);
	}

	return truncated ? -found : found;
}

#endif

#endif