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
#include "if_bore.h"

#if defined(FEAT_BORE)
 
#include "roxml.h"
#include <windows.h>


static int bore_canonicalize (const char* src, char* dst, DWORD* attr);

void bore_prealloc(bore_alloc_t* p, size_t size)
{
	p->base = (u8*)alloc(size + BORE_CACHELINE);
	p->offset = BORE_CACHELINE - ((size_t)(p->base) & (BORE_CACHELINE - 1));
	p->base += p->offset;
	assert(((size_t)(p->base) & (BORE_CACHELINE - 1)) == 0);
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
		size_t offsetnew;
		u8* basenew;
		if (newsize > newcapacity)
			newcapacity = newsize * 2;
		basenew = alloc(newcapacity + BORE_CACHELINE);
		offsetnew = BORE_CACHELINE - ((size_t)basenew & (BORE_CACHELINE - 1));
		basenew += offsetnew;
		assert(((size_t)basenew & (BORE_CACHELINE - 1)) == 0);
		memcpy(basenew, p->base, currentsize);
		vim_free(p->base - p->offset);
		p->cursor = basenew + currentsize;
		p->base = basenew;
		p->offset = offsetnew;
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
		vim_free(p->base - p->offset);
}

static bore_t* g_bore = 0;

static void bore_free(bore_t* b)
{
	int i;
	if (!b) return;
	vim_free(b->filelist_tmp_file);
	bore_alloc_free(&b->file_alloc);
	bore_alloc_free(&b->data_alloc);
	bore_alloc_free(&b->proj_alloc);
	for (i = 0; i < BORE_SEARCH_JOBS; ++i) {
		bore_alloc_free(&b->search[i].filedata);
	}
	vim_free(b);
}

char* bore_str(bore_t* b, u32 offset)
{
	return (char*)(b->data_alloc.base + offset);
}

static u32 bore_strndup(bore_t* b, const char* s, int len)
{
	char* p = (char*)bore_alloc(&b->data_alloc, len + 1);
	memcpy(p, s, len);
	p[len] = 0;
	return p - (char*)b->data_alloc.base;
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
	filename_part = (char*)vim_strrchr((char_u*)filename_buf, '\\') + 1;
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
	int sln_dir_len = (char*)vim_strrchr((char_u*)sln_path, '\\') - sln_path + 1;

	regmatch.regprog = vim_regcomp((char_u*)"^Project(\"{.\\{-}}\") = \"\\(.\\{-}\\)\", \"\\(.\\{-}\\)\"", RE_MAGIC + RE_STRING);
	regmatch.rm_ic = 0;

	f = fopen(sln_path, "rb");
	if (!f) {
		goto done;
	}

	while (0 == vim_fgets((char_u*)buf, sizeof(buf), f)) {
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
			bore_load_vcxproj_filters(b, bore_str(b, proj[i].project_path));
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
		while(pr < pend) {
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

static int bore_write_filelist_to_tempfile(bore_t* b)
{
	FILE* f;
	int i;
	const char *slndir;
	int slndirlen;
	b->filelist_tmp_file = vim_tempname('b');
	if (!b->filelist_tmp_file)
		return FAIL;
	f = fopen(b->filelist_tmp_file, "w");
	if (!f)
		return FAIL;
	slndir = bore_str(b, b->sln_dir);
	slndirlen = strlen(slndir);
	for(i = 0; i < b->file_count; ++i) {
		const char *fn = bore_str(b, ((u32*)b->file_alloc.base)[i]);
		if (strncmp(fn, slndir, slndirlen) == 0)
			fprintf(f, "%s\n", fn + slndirlen);
		else
			fprintf(f, "%s\n", fn);
	}
	fclose(f);
	return OK;
}

static void bore_load_ini(bore_ini_t* ini, const char* dirpath)
{
	ini->borebuf_height = 30;
}

static void bore_load_sln(const char* path)
{
	char buf[BORE_MAX_PATH];
	int i;
	char* pc;
	bore_t* b = (bore_t*)alloc(sizeof(bore_t));
	memset(b, 0, sizeof(bore_t));

	bore_free(g_bore);
	g_bore = 0;

	bore_prealloc(&b->data_alloc, 8*1024*1024);
	bore_prealloc(&b->file_alloc, sizeof(u32)*64*1024);
	bore_prealloc(&b->proj_alloc, sizeof(bore_proj_t)*256);

	for (i = 0; i < BORE_SEARCH_JOBS; ++i) {
		bore_prealloc(&b->search[i].filedata, 1*1024*1024);
	}

	// Allocate something small, so that we can use offset 0 as NULL
	bore_alloc(&b->data_alloc, 1);

	if (FAIL == bore_canonicalize((char*)path, buf, 0))
		goto fail;

	b->sln_path = bore_strndup(b, buf, strlen(buf));
	b->sln_dir = bore_strndup(b, buf, strlen(buf));
	pc = vim_strrchr(bore_str(b, b->sln_dir), '\\');
	if (pc)
		*++pc = 0;
	
	bore_load_ini(&b->ini, bore_str(b, b->sln_dir));

	if (FAIL == bore_extract_projects_from_sln(b, buf))
		goto fail;

	if (FAIL == bore_extract_files_from_projects(b))
		goto fail;

	if (FAIL == bore_sort_and_cleanup_files(b))
		goto fail;

	if (FAIL == bore_write_filelist_to_tempfile(b))
		goto fail;

	sprintf(buf, "let g:bore_filelist_file=\'%s\'", b->filelist_tmp_file);
    do_cmdline_cmd(buf);

	sprintf(buf, "cd %s", bore_str(b, b->sln_dir));
    do_cmdline_cmd(buf);
	
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
//		int i;
		char status[BORE_MAX_PATH];
		sprintf(status, "%s, %d projects, %d files", bore_str(g_bore, g_bore->sln_path),
			g_bore->proj_count, g_bore->file_count);
		MSG(_(status));


		//for (i = 0; i < g_bore->file_count; ++i) {
		//	char* fn = bore_str(g_bore, ((u32*)(g_bore->file_alloc.base))[i]);
		//	ml_append(i		//	, fn, strlen(fn)+1, 0);
		//}

		//update_screen(VALID);
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

static void bore_display_search_result(bore_t* b, const char* filename)
{
	exarg_T eap;
	char* title[] = {"Bore Find", 0};

	memset(&eap, 0, sizeof(eap));
	eap.cmdidx = CMD_cfile;
	eap.arg = (char*)filename;
	eap.cmdlinep = title;
	ex_cfile(&eap);
	
	memset(&eap, 0, sizeof(eap));
	eap.cmdidx = CMD_cwindow;
	ex_copen(&eap);
}

static void bore_save_match_to_file(bore_t* b, FILE* cf, const bore_match_t* match, int match_count)
{
	const char *slndir = bore_str(b, b->sln_dir);
	int slndirlen = strlen(slndir);
	int i;
	for (i = 0; i < match_count; ++i, ++match) {
		const char* fn = bore_str(b, ((u32*)(b->file_alloc.base))[match->file_index]);
		if (strncmp(fn, slndir, slndirlen) == 0)
			fn += slndirlen;
		fprintf(cf, "%s:%d:%d:%s\n", fn, match->row, match->column + 1, match->line);
	}
}

static int bore_find(bore_t* b, char* what)
{
	enum { MaxMatch = 1000 };
	u32* files = (u32*)b->file_alloc.base;
	int found = 0;
	char_u *tmp = vim_tempname('f');
	FILE* cf = 0;
	bore_match_t* match = 0;
	int truncated = 0;

	match = (bore_match_t*)alloc(MaxMatch * sizeof(bore_match_t));

	found = bore_dofind(b, &truncated, match, MaxMatch, what);

	cf = mch_fopen((char *)tmp, "wb");
	if (cf == NULL) {
	    EMSG2(_(e_notopen), tmp);
		goto fail;
	}
	bore_save_match_to_file(b, cf, match, found);

	fclose(cf);

	bore_display_search_result(b, tmp);
	mch_remove(tmp);
fail:
	vim_free(tmp);
	vim_free(match);
	if (cf) fclose(cf);
	return truncated ? -found : found;
}

// Display filename in the borebuf.
// Window height is at least minheight (if possible)
// mappings is a null-terminated array of strings with buffer mappings of the form "<key> <command>"
static void bore_show_borebuf(const char* filename, int minheight, const char** mappings)
{
	//char_u	*arg;
	char_u  maparg[512];
	int		n;
#ifdef FEAT_WINDOWS
	win_T	*wp;
#endif
//	char_u	*p;
	int		empty_fnum = 0;
	int		alt_fnum = 0;
	buf_T	*buf;
	FILE    *filelist_fd = 0;

	if (!g_bore || !g_bore->filelist_tmp_file) {
		EMSG(_("Load a solution first with boresln"));
		return;
	}

	//if (eap != NULL)
	//{
	//	/*
	//	* A ":boreopen" command ends at the first LF, or at a '|' that is
	//	* followed by some text.  Set nextcmd to the following command.
	//	*/
	//	for (arg = eap->arg; *arg; ++arg)
	//	{
	//		if (*arg == '\n' || *arg == '\r'
	//			|| (*arg == '|' && arg[1] != NUL && arg[1] != '|'))
	//		{
	//			*arg++ = NUL;
	//			eap->nextcmd = arg;
	//			break;
	//		}
	//	}
	//	arg = eap->arg;

	//	if (eap->skip)	    /* not executing commands */
	//		return;
	//}
	//else
	//	arg = (char_u *)"";

	///* remove trailing blanks */
	//p = arg + STRLEN(arg) - 1;
	//while (p > arg && vim_iswhite(*p) && p[-1] != '\\')
	//	*p-- = NUL;

#ifdef FEAT_GUI
	need_mouse_correct = TRUE;
#endif

	/*
	* Re-use an existing bore window or open a new one.
	*/
	if (!curwin->w_buffer->b_borebuf)
	{
#ifdef FEAT_WINDOWS
		for (wp = firstwin; wp != NULL; wp = wp->w_next)
			if (wp->w_buffer != NULL && wp->w_buffer->b_borebuf)
				break;
		if (wp != NULL && wp->w_buffer->b_nwindows > 0)
			win_enter(wp, TRUE);
		else
#endif
		{
#ifdef FEAT_WINDOWS
			/* Split off help window; put it at far top if no position
			* specified, the current window is vertically split and
			* narrow. */
			n = WSP_HELP;
# ifdef FEAT_VERTSPLIT
			if (cmdmod.split == 0 && curwin->w_width != Columns
				&& curwin->w_width < 80)
				n |= WSP_TOP;
# endif
			if (win_split(0, n) == FAIL)
				goto erret;
#else
			/* use current window */
			if (!can_abandon(curbuf, FALSE))
				goto erret;
#endif

#ifdef FEAT_WINDOWS
			if (curwin->w_height < minheight)
				win_setheight(minheight);
#endif

			alt_fnum = curbuf->b_fnum;
			// Piggyback on the help window which has the properties we want for borebuf too.
			// (readonly, can't insert text, etc)
			(void)do_ecmd(0, (char*)filename, NULL, NULL, ECMD_LASTL, ECMD_HIDE + ECMD_SET_HELP,
#ifdef FEAT_WINDOWS
				NULL  /* buffer is still open, don't store info */
#else
				curwin
#endif
				);

			if (!cmdmod.keepalt)
				curwin->w_alt_fnum = alt_fnum;
			empty_fnum = curbuf->b_fnum;

			// This is the borebuf
			curwin->w_buffer->b_borebuf = 1;
		}
	}

	// Press enter to open the file on the current line
	while(*mappings) {
		sprintf(maparg, "<buffer> %s", *mappings);
		if (0 != do_map(0, maparg, NORMAL, FALSE))
			goto erret;
		++mappings;
	}

	if (!p_im)
		restart_edit = 0;	    /* don't want insert mode in help file */

	/* Delete the empty buffer if we're not using it.  Careful: autocommands
	* may have jumped to another window, check that the buffer is not in a
	* window. */
	if (empty_fnum != 0 && curbuf->b_fnum != empty_fnum)
	{
		buf = buflist_findnr(empty_fnum);
		if (buf != NULL && buf->b_nwindows == 0)
			wipe_buffer(buf, TRUE);
	}

	/* keep the previous alternate file */
	if (alt_fnum != 0 && curwin->w_alt_fnum == empty_fnum && !cmdmod.keepalt)
		curwin->w_alt_fnum = alt_fnum;

	return;
erret:
	EMSG(_("Could not open borebuf"));
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
		bore_print_sln();
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
		int found = bore_find(g_bore, (char*)eap->arg);
		elapsed = GetTickCount() - start;
		sprintf(mess, "Matching lines: %d%s Elapsed time: %u ms", found > 0 ? found : -found, found < 0 ? " (truncated)" : "", elapsed);
		MSG(_(mess));
	}
}

void ex_boreopen __ARGS((exarg_T *eap))
{
	if (!g_bore)
		EMSG(_("Load a solution first with boresln"));
	else {
		const char* mappings[] = {"<CR> :ZZBoreopenselection<CR>", 0};
		bore_show_borebuf(g_bore->filelist_tmp_file, g_bore->ini.borebuf_height, mappings);
	}
}

// Internal functions

// Open the file on the current row in the current buffer 
void ex_Boreopenselection __ARGS((exarg_T *eap))
{
	char_u* fn;
	exarg_T ea;
	if (!g_bore)
		return;
	fn = vim_strsave(ml_get_curline());
	if (!fn)
		return;

	win_close(curwin, TRUE);

	memset(&ea, 0, sizeof(ea));
	ea.arg = fn;
	ea.cmdidx = CMD_edit;
	do_exedit(&ea, NULL);
	vim_free(fn);
}

#endif
