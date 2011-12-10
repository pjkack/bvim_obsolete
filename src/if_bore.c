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

#include <stdio.h>
#include <string.h>
#include "vim.h"
#include "regexp.h"

#if defined(FEAT_BORE)

#include "libroxml-2.1.2/inc/roxml.h"

typedef unsigned char u8;
typedef unsigned int u32;

typedef struct bore_alloc_t {
	u8* base;
	u8* end;
	u8* cursor;
} bore_alloc_t;

void* bore_alloc(bore_alloc_t*p, size_t size)
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
} bore_t;

static bore_t* g_bore = 0;

const char* bore_str(bore_t* b, u32 offset)
{
	return (char*)(b->data_alloc.base + offset);
}

u32 bore_strndup(bore_t* b, const char* s, int len)
{
	char* p = (char*)bore_alloc(&b->data_alloc, len + 1);
	memcpy(p, s, len);
	p[len] = 0;
	return p - b->data_alloc.base;
}

void bore_load_vcxproj_filters(bore_t* b, const char* path)
{
	node_t* root;
	node_t** result;
	u32* files;
	int result_count;
	int i;
	
	root = roxml_load_doc((char*)path);
	if (!root)
		return;

	result = roxml_xpath(root, "//@Include", &result_count);

	files = (u32*)bore_alloc(&b->file_alloc, sizeof(u32)*result_count);
	b->file_count += result_count;

	for(i = 0; i < result_count; ++i) {
		char buf[1024];
		int size;
		roxml_get_content(result[i], buf, 1024, &size);
		files[i] = bore_strndup(b, buf, size-1);
	}

	roxml_release(result);
	roxml_close(root);
}

int bore_extract_projects_from_sln(bore_t* b, const char* sln_path)
{
	regmatch_T regmatch;
	FILE* f;
	char buf[1024];
	int result = -1;

	regmatch.regprog = vim_regcomp("^Project(\"{.\\{-}}\") = \"\\(.\\{-}\\)\", \"\\(.\\{-}\\)\"", RE_MAGIC + RE_STRING);
	regmatch.rm_ic = 0;

	f = fopen(sln_path, "rb");
	if (!f) {
		goto done;
	}

	while (0 == vim_fgets(buf, 1024, f)) {
	    if (vim_regexec_nl(&regmatch, buf, (colnr_T)0)) {
			bore_proj_t* proj = (bore_proj_t*)bore_alloc(&b->proj_alloc, sizeof(bore_proj_t));
			++b->proj_count;
			proj->project_name = bore_strndup(b, regmatch.startp[1], regmatch.endp[1] - regmatch.startp[1]);
			proj->project_path = bore_strndup(b, regmatch.startp[2], regmatch.endp[2] - regmatch.startp[2]);
		}
	}

	fclose(f);
	vim_free(regmatch.regprog);

	result = 0;

done:
	return result;
}

int bore_extract_files_from_projects(bore_t* b)
{
	bore_proj_t* proj = (bore_proj_t*)b->proj_alloc.base;
	int i;
	char path[1024];

	for (i = 0; i < b->proj_count; ++i) {
		sprintf(path, "%s%s.filters", bore_str(b, b->sln_dir), bore_str(b, proj[i].project_path));
		bore_load_vcxproj_filters(b, path);
	}
	return 0;
}

void bore_free(bore_t* b)
{
	if (!b) return;
	bore_alloc_free(&b->file_alloc);
	bore_alloc_free(&b->data_alloc);
	vim_free(b);
}

void bore_load_sln(const char* path)
{
	char buf[1024];
	char* pc;
	bore_t* b = (bore_t*)alloc(sizeof(bore_t));
	memset(b, 0, sizeof(bore_t));

	bore_free(g_bore);
	g_bore = 0;

	// Allocate something small, so that we can use offset 0 as NULL
	bore_alloc(&b->data_alloc, 1);

	if (FAIL == vim_FullName((char*)path, buf, 1024, 1))
		goto fail;

	b->sln_path = bore_strndup(b, buf, strlen(buf));
	b->sln_dir = bore_strndup(b, buf, strlen(buf));
	pc = vim_strrchr(bore_str(b, b->sln_dir), '/');
	if (pc)
		*++pc = 0;
	
	if (bore_extract_projects_from_sln(b, path) < 0)
		goto fail;

	if (bore_extract_files_from_projects(b))
		goto fail;

	g_bore = b;
	return;

fail:
	bore_free(b);
	EMSG2(_("Could not open solution file "), path);
	return;
}

void bore_print_sln()
{
	if (g_bore) {
		char status[1024];
		sprintf(status, "%s, %d projects, %d files", bore_str(g_bore, g_bore->sln_path),
			g_bore->proj_count, g_bore->file_count);
		MSG(_(status));
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
		bore_load_sln((char *)eap->arg);
	}
}

#endif
