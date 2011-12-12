/* vi:set ts=8 sts=4 sw=4:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Bore by Jonas Kjellstrom
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
TODO:




*/

#include <stdio.h>
#include <string.h>
#include "vim.h"
#include "regexp.h"

#if defined(FEAT_BORE)

#include "roxml.h"
#include <windows.h>

typedef unsigned char u8;
typedef unsigned int u32;

#define BORE_MAX_SMALL_PATH 256
#define BORE_MAX_PATH 1024

static int bore_canonicalize (const char* src, char* dst, DWORD* attr);

typedef struct bore_alloc_t {
	u8* base;
	u8* end;
	u8* cursor;
} bore_alloc_t;

void bore_prealloc(bore_alloc_t* p, size_t size)
{
	p->base = (u8*)alloc(size);
	p->end  = p->base + size;
	p->cursor = p->base;
}

void* bore_alloc(bore_alloc_t* p, size_t size)
{
	void* mem;
	if (p->cursor + size > p->end) {
		// resize
		size_t capacity = p->end - p->base;
		size_t newcapacity = capacity * 2;
		size_t currentsize = p->cursor - p->base;
		size_t newsize = currentsize + size;
		u8* basenew;
		if (newsize > newcapacity)
			newcapacity = newsize * 2;
		basenew = alloc(newcapacity);
		memcpy(basenew, p->base, currentsize);
		vim_free(p->base);
		p->cursor = basenew + currentsize;
		p->base = basenew;
		p->end = p->base + newcapacity;
	}
	mem = p->cursor;
	p->cursor += size;
	return mem;
}

void bore_alloc_trim(bore_alloc_t* p, size_t size)
{
	p->cursor -= size;
}

void bore_alloc_free(bore_alloc_t* p)
{
	if (p->base)
		vim_free(p->base);
}

typedef struct bore_proj_t {
	u32 project_name;
	u32 project_path;
} bore_proj_t;

typedef struct bore_t {
	u32 sln_path; // abs path of solution
	u32 sln_dir;  // abs dir of solution

	// array of bore_proj_t (all projects in the solution)
	int proj_count;
	bore_alloc_t proj_alloc; 

	// array of files in the solution
	int file_count;
	bore_alloc_t file_alloc;

	bore_alloc_t data_alloc; // bulk data (filenames, strings, etc)

	bore_alloc_t fsearch_alloc; // scratch pad for reading a complete file for searching
} bore_t;

static bore_t* g_bore = 0;

static void bore_free(bore_t* b)
{
	if (!b) return;
	bore_alloc_free(&b->file_alloc);
	bore_alloc_free(&b->data_alloc);
	bore_alloc_free(&b->proj_alloc);
	bore_alloc_free(&b->fsearch_alloc);
	vim_free(b);
}

static char* bore_str(bore_t* b, u32 offset)
{
	return (char*)(b->data_alloc.base + offset);
}

static u32 bore_strndup(bore_t* b, const char* s, int len)
{
	char* p = (char*)bore_alloc(&b->data_alloc, len + 1);
	memcpy(p, s, len);
	p[len] = 0;
	return p - b->data_alloc.base;
}

static void bore_append_vcxproj_files(bore_t* b, node_t** result, int result_count,
	char* filename_buf, char* filename_part, int path_part_len)
{
	int i, file_index;
	u32* files = (u32*)bore_alloc(&b->file_alloc, sizeof(u32)*result_count);

	for(i = 0, file_index = 0; i < result_count; ++i) {
		char buf[BORE_MAX_PATH];
		DWORD attr;
		roxml_get_content(result[i], filename_part, BORE_MAX_PATH - path_part_len, 0);
		if (FAIL != bore_canonicalize(filename_buf, buf, &attr)) {
			if (!(FILE_ATTRIBUTE_DIRECTORY & attr))
				files[file_index++] = bore_strndup(b, buf, strlen(buf));
		}
	}
	b->file_count += file_index;
	bore_alloc_trim(&b->file_alloc, sizeof(u32)*(result_count - file_index));
}

static void bore_load_vcxproj_filters(bore_t* b, const char* path)
{
	node_t* root;
	node_t** result;
	int result_count;
	char filename_buf[BORE_MAX_PATH];
	char* filename_part;
	int path_part_len;
	
	root = roxml_load_doc((char*)path);
	if (!root)
		return;

	strcpy(filename_buf, path);
	filename_part = vim_strrchr(filename_buf, '\\') + 1;
	path_part_len = filename_part - filename_buf;

	//result = roxml_xpath(root, "//ClInclude/@Include", &result_count);
	result = roxml_xpath(root, "//@Include", &result_count);
	bore_append_vcxproj_files(b, result, result_count, filename_buf, filename_part, path_part_len);
	roxml_release(result);

	roxml_close(root);
}

static int bore_extract_projects_from_sln(bore_t* b, const char* sln_path)
{
	regmatch_T regmatch;
	FILE* f;
	char buf[BORE_MAX_PATH];
	char buf2[BORE_MAX_PATH];
	int result = FAIL;
	int sln_dir_len = vim_strrchr((char*)sln_path, '\\') - sln_path + 1;

	regmatch.regprog = vim_regcomp("^Project(\"{.\\{-}}\") = \"\\(.\\{-}\\)\", \"\\(.\\{-}\\)\"", RE_MAGIC + RE_STRING);
	regmatch.rm_ic = 0;

	f = fopen(sln_path, "rb");
	if (!f) {
		goto done;
	}

	while (0 == vim_fgets(buf, sizeof(buf), f)) {
	    if (vim_regexec_nl(&regmatch, buf, (colnr_T)0)) {
			bore_proj_t* proj = (bore_proj_t*)bore_alloc(&b->proj_alloc, sizeof(bore_proj_t));
			++b->proj_count;
			proj->project_name = bore_strndup(b, regmatch.startp[1], regmatch.endp[1] - regmatch.startp[1]);

			vim_strncpy(buf2, (char*)sln_path, sln_dir_len);
			vim_strncpy(buf2 + sln_dir_len, regmatch.startp[2], regmatch.endp[2] - regmatch.startp[2]);

			if (FAIL != bore_canonicalize(buf2, buf, 0))
				proj->project_path = bore_strndup(b, buf, strlen(buf));
			else
				proj->project_path = 0;
		}
	}

	fclose(f);
	vim_free(regmatch.regprog);

	result = OK;

done:
	return result;
}

static int bore_extract_files_from_projects(bore_t* b)
{
	bore_proj_t* proj = (bore_proj_t*)b->proj_alloc.base;
	int i;
	char path[BORE_MAX_PATH];

	for (i = 0; i < b->proj_count; ++i) {
		if (proj[i].project_path) {
			sprintf(path, "%s.filters", bore_str(b, proj[i].project_path));
			bore_load_vcxproj_filters(b, path);
		}
	}
	return OK;
}

static int bore_sort_filename(void* ctx, const void* vx, const void* vy)
{
	bore_t* b = (bore_t*)ctx;
	u32 x = *(u32*)vx;
	u32 y = *(u32*)vy;
	return stricmp(bore_str(b, x), bore_str(b, y));
}

static int bore_sort_and_cleanup_files(bore_t* b)
{
	u32* files = (u32*)b->file_alloc.base;

	// sort
	qsort_s(files, b->file_count, sizeof(u32), bore_sort_filename, b);

	// uniq
	{
		u32* pr = files + 1;
		u32* pw = files + 1;
		u32* pend = files + b->file_count;
		int n;
		while(pr != pend) {
			if (0 != stricmp(bore_str(b, *pr), bore_str(b, *(pr-1)))) {
				*pw++ = *pr++;
			} else {
				++pr;
			}
		}
		n = pw - files;

		// resize file-array
		bore_alloc_trim(&b->file_alloc, sizeof(u32)*(b->file_count - n));
		b->file_count = n;
	}
	
	return OK;
}

static void bore_load_sln(const char* path)
{
	char buf[BORE_MAX_PATH];
	char* pc;
	bore_t* b = (bore_t*)alloc(sizeof(bore_t));
	memset(b, 0, sizeof(bore_t));

	bore_free(g_bore);
	g_bore = 0;

	bore_prealloc(&b->data_alloc, 8*1024*1024);
	bore_prealloc(&b->file_alloc, sizeof(u32)*64*1024);
	bore_prealloc(&b->proj_alloc, sizeof(bore_proj_t)*256);
	bore_prealloc(&b->fsearch_alloc, 1*1024*1024);

	// Allocate something small, so that we can use offset 0 as NULL
	bore_alloc(&b->data_alloc, 1);

	if (FAIL == bore_canonicalize((char*)path, buf, 0))
		goto fail;

	b->sln_path = bore_strndup(b, buf, strlen(buf));
	b->sln_dir = bore_strndup(b, buf, strlen(buf));
	pc = vim_strrchr(bore_str(b, b->sln_dir), '\\');
	if (pc)
		*++pc = 0;
	
	if (FAIL == bore_extract_projects_from_sln(b, buf))
		goto fail;

	if (FAIL == bore_extract_files_from_projects(b))
		goto fail;

	if (FAIL == bore_sort_and_cleanup_files(b))
		goto fail;

	g_bore = b;
	return;

fail:
	bore_free(b);
	EMSG2(_("Could not open solution file %s"), buf);
	return;
}

static void bore_print_sln()
{
	if (g_bore) {
		int i;
		char status[BORE_MAX_PATH];
		sprintf(status, "%s, %d projects, %d files", bore_str(g_bore, g_bore->sln_path),
			g_bore->proj_count, g_bore->file_count);
		MSG(_(status));


		for (i = 0; i < g_bore->file_count; ++i) {
			char* fn = bore_str(g_bore, ((u32*)(g_bore->file_alloc.base))[i]);
			ml_append(i, fn, strlen(fn)+1, 0);
		}

		update_screen(VALID);
	}
}

static int bore_canonicalize(const char* src, char* dst, DWORD* attr)
{
	WCHAR wbuf[BORE_MAX_PATH];
	WCHAR wbuf2[BORE_MAX_PATH];
	DWORD fnresult;
	int result = MultiByteToWideChar(CP_UTF8, 0, src, -1, wbuf, BORE_MAX_PATH);
	if (result <= 0) 
		return FAIL;
	fnresult = GetFullPathNameW(wbuf, BORE_MAX_PATH, wbuf2, 0);
	if (!fnresult)
		return FAIL;
	if (attr)
		*attr = GetFileAttributesW(wbuf);
	result = WideCharToMultiByte(CP_UTF8, 0, wbuf2, -1, dst, BORE_MAX_PATH, 0, 0);
	if (!result)
		return FAIL;
	return OK;
}

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

static void bore_find(bore_t* b, char* what)
{
	int i;
	u32* files = (u32*)b->file_alloc.base;
	int found = 0;

	for(i = 0; i < b->file_count; ++i) {
		WCHAR fn[BORE_MAX_PATH];
		HANDLE f;
		DWORD filesize;
		DWORD remaining;
		char* p;
		int match_positions[100];
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
		p = b->fsearch_alloc.base;
		remaining = filesize;
		while(remaining) {
			DWORD readbytes;
			if(!ReadFile(f, p + filesize - remaining, remaining, &readbytes, 0))
				goto skip;
			remaining -= readbytes;
		}
		found += bore_text_search(p, filesize, what, strlen(what), match_positions, 
			match_positions + sizeof(match_positions)/sizeof(match_positions[0]));
		CloseHandle(f);
		continue;
skip:
		CloseHandle(f);
	}

	{
		char buf[100];
		sprintf(buf, "Matches: %d", found);
		MSG(_(buf));
	}
}

#endif

/* Only do the following when the feature is enabled.  Needed for "make
 * depend". */
#if defined(FEAT_BORE) || defined(PROTO)

void ex_boresln __ARGS((exarg_T *eap))
{
    if (*eap->arg == NUL) {
		bore_print_sln();
    } else {
		DWORD start = GetTickCount();
		DWORD elapsed;
		char mess[100];
		bore_load_sln((char*)eap->arg);
		elapsed = GetTickCount() - start;
		sprintf(mess, "Elapsed time: %u ms", elapsed);
		MSG(_(mess));
	}
}

void ex_borefind __ARGS((exarg_T *eap))
{
	if (!g_bore) {
		EMSG(_("Load a solution first with boresln"));
    } else {
		DWORD start = GetTickCount();
		DWORD elapsed;
		char mess[100];
		bore_find(g_bore, (char*)eap->arg);
		elapsed = GetTickCount() - start;
		sprintf(mess, "Elapsed time: %u ms", elapsed);
		MSG(_(mess));
	}
}

#endif
