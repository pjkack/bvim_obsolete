/* vi:set ts=8 sts=4 sw=4 et:
 *
 * VIM - Vi IMproved    by Bram Moolenaar
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

//#define BORE_VIMPROFILE

#ifdef BORE_VIMPROFILE
#define BORE_VIMPROFILE_INIT proftime_T ptime
#define BORE_VIMPROFILE_START profile_start(&ptime)
#define BORE_VIMPROFILE_STOP(str) do { \
    char mess[100]; \
    profile_end(&ptime); \
    vim_snprintf(mess, 100, "%s %s", profile_msg(&ptime), str); \
    MSG(_(mess)); \
} while(0)
#else
#define BORE_VIMPROFILE_INIT
#define BORE_VIMPROFILE_START
#define BORE_VIMPROFILE_STOP(str)
#endif

#if defined(FEAT_BORE)

#include "roxml.h"
#include <windows.h>

static int bore_canonicalize (const char* src, char* dst, DWORD* attr);
static u32 bore_string_hash(const char* s);
static u32 bore_string_hash_n(const char* s, int n);

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
    bore_alloc_free(&b->file_ext_alloc);
    bore_alloc_free(&b->toggle_index_alloc);
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

static int bore_is_excluded_extension(const char* ext)
{
    // TODO-jkjellstrom: Use extension hash when supported?
    if (ext)
    {
        if (0 == stricmp(ext, ".dll") || 0 == stricmp(ext, ".vcxproj") || 0 == stricmp(ext, ".exe"))
            return 1;
    }

    return 0;
}

static int bore_append_solution_files(bore_t* b, const char* sln_path)
{
    int result = FAIL;
    FILE* f = fopen(sln_path, "rb");
    int file_index = 0;
    int state = 0;
    u32* files = 0;
    char buf[BORE_MAX_PATH];
    char fn[BORE_MAX_PATH];
    if (!f) {
        goto done;
    }

    // There cannot be more than solutionLineCount files in the sln-files. The files are specified one on each row.
    files = (u32*)bore_alloc(&b->file_alloc, sizeof(u32)*b->solutionLineCount);

    while (0 == vim_fgets((char_u*)buf, sizeof(buf), f)) {
        if (state == 0) {
            if (strstr(buf, "ProjectSection(SolutionItems) = preProject"))
                state = 1;
        } else {
            if (strstr(buf, "EndProjectSection")) {
                state = 0;
            } else {
                char* ends = strstr(buf, " = ");
                if (ends) {
                    DWORD attr = 0;
                    *ends = 0;
                    if (FAIL != bore_canonicalize(&buf[2], fn, &attr)) {
                        if (!(FILE_ATTRIBUTE_DIRECTORY & attr) && !bore_is_excluded_extension(vim_strrchr(fn, '.')))
                            files[file_index++] = bore_strndup(b, fn, strlen(fn));
                    }

                } else {
                    state = 0;
                }
            }
        }
    }

    b->file_count += file_index;
    bore_alloc_trim(&b->file_alloc, sizeof(u32)*(b->solutionLineCount - file_index));

    fclose(f);

    result = OK;
done:
    return result;
}

static void bore_append_vcxproj_files(bore_t* b, node_t** result, int result_count,
        char* filename_buf, char* filename_part, int path_part_len)
{
    int i, file_index;
    u32* files = (u32*)bore_alloc(&b->file_alloc, sizeof(u32)*result_count);

    for(i = 0, file_index = 0; i < result_count; ++i) {
        char buf[BORE_MAX_PATH];
        const char* fn;
        DWORD attr;
        int len;
        const char* ext;
        int skipFile = 0;

        roxml_get_content(result[i], filename_part, BORE_MAX_PATH - path_part_len, 0);
        len = strlen(filename_part);
        /* roxml sometimes returns paths with trailing " */
        while(len > 0 && filename_part[len - 1] == '\"') {
            --len;
            filename_part[len] = 0;
        }
        fn = (strlen(filename_part) >=2 && filename_part[1] == ':') ? filename_part : filename_buf;

        ext = vim_strrchr(filename_part, '.');
        skipFile = bore_is_excluded_extension(ext);
        if (!skipFile && FAIL != bore_canonicalize(fn, buf, &attr)) {
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

    b->solutionLineCount = 0;
    while (0 == vim_fgets((char_u*)buf, sizeof(buf), f)) {
        b->solutionLineCount++;
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

    if (b->file_count == 0)
        return OK;

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

static int bore_build_extension_list(bore_t* b)
{
    u32* files = (u32*)b->file_alloc.base;
    bore_alloc(&b->file_ext_alloc, b->file_count * sizeof(u32));
    u32* ext_hash = (u32*)b->file_ext_alloc.base;
    u32 i;
    for (i = 0; i < (u32)b->file_count; ++i) {
        char* path = bore_str(b, files[i]);
        u32 path_len = (u32)strlen(path);
        char* ext = vim_strrchr(path, '.');

        ext = ext ? ext + 1 : path + path_len;
        ext_hash[i] = bore_string_hash(ext);
    }

    return OK;
}

static int bore_sort_toggle_entry(const void* vx, const void* vy)
{
    const bore_toggle_entry_t* x = (const bore_toggle_entry_t*)vx;
    const bore_toggle_entry_t* y = (const bore_toggle_entry_t*)vy;
    u32 basename_diff = x->basename_hash - y->basename_hash;
    if (0 == basename_diff)
        return x->extension_index - y->extension_index;
    else
        return basename_diff;
}

static int bore_build_toggle_index(bore_t* b)
{
    u32* files = (u32*)b->file_alloc.base;
    u32 i;
    u32 seq[] = {
        bore_string_hash("cpp"),
        bore_string_hash("cxx"),
        bore_string_hash("c"),
        bore_string_hash("inl"),
        bore_string_hash("hpp"),
        bore_string_hash("hxx"),
        bore_string_hash("h"),
        bore_string_hash("asm"),
        bore_string_hash("s"),
        bore_string_hash("ddf"), 
    };
    bore_prealloc(&b->toggle_index_alloc, b->file_count * sizeof(bore_toggle_entry_t));
    b->toggle_entry_count = 0;
    for (i = 0; i < (u32)b->file_count; ++i) {
        char* path = bore_str(b, files[i]);
        u32 path_len = (u32)strlen(path);
        char* ext = vim_strrchr(path, '.');
        char* basename = vim_strrchr(path, '\\');
        bore_toggle_entry_t* e;
        int j;
        u32 ext_hash;
        int ext_index = -1;

        ext = ext ? ext + 1 : path + path_len;
        basename = basename ? basename + 1 : path;

        ext_hash = bore_string_hash(ext);

        for (j = 0; j < sizeof(seq)/sizeof(seq[0]); ++j)
            if (seq[j] == ext_hash) {
                ext_index = j;
                break;
            }

        if (ext_index >= 0) {
            e = (bore_toggle_entry_t*)bore_alloc(&b->toggle_index_alloc, 
                    sizeof(bore_toggle_entry_t));
            e->fileindex = i;
            e->extension_hash = ext_hash;
            e->extension_index = ext_index;
            e->basename_hash = bore_string_hash_n(basename, (int)(ext - basename));
            b->toggle_entry_count++;
        }
    }
    qsort(b->toggle_index_alloc.base, b->toggle_entry_count, sizeof(bore_toggle_entry_t), 
            bore_sort_toggle_entry);

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

struct bore_async_execute_context_t
{
    HANDLE wait_thread;
    PROCESS_INFORMATION spawned_process;
    char result_filename[MAX_PATH];
    HANDLE result_handle;
};

static struct bore_async_execute_context_t g_bore_async_execute_context;

static void bore_load_sln(const char* path)
{
    g_bore_async_execute_context.wait_thread = INVALID_HANDLE_VALUE;
    g_bore_async_execute_context.result_handle = INVALID_HANDLE_VALUE;

    char buf[BORE_MAX_PATH];
    int i;
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

    {
        char* sln_dir_str = bore_str(b, b->sln_dir);
        char* pc = vim_strrchr(sln_dir_str, '\\');
        if (pc) {
            pc[1] = 0; // Keep trailing backslash

            // Special case. If the solution file is in a local folder, then assume 
            // code paths start one level up from that
            while (--pc > sln_dir_str) {
                if (*pc == '\\') {
                    if (stricmp(pc, "\\Local\\") == 0) {
                        pc[1] = 0; // Keep trailing backslash
                    }
                    break;
                }
            }
        }
    }

    sprintf(buf, "cd %s", bore_str(b, b->sln_dir));
    ++msg_silent;
    do_cmdline_cmd(buf);
    --msg_silent;

    bore_load_ini(&b->ini, bore_str(b, b->sln_dir));

    BORE_VIMPROFILE_INIT;

    BORE_VIMPROFILE_START;
    if (FAIL == bore_extract_projects_from_sln(b, bore_str(b, b->sln_path)))
        goto fail;
    BORE_VIMPROFILE_STOP("bore_extract_projects_from_sln");

    BORE_VIMPROFILE_START;
    if (FAIL == bore_append_solution_files(b, bore_str(b, b->sln_path)))
        goto fail;
    BORE_VIMPROFILE_STOP("bore_append_solution_files");

    BORE_VIMPROFILE_START;
    if (FAIL == bore_extract_files_from_projects(b))
        goto fail;
    BORE_VIMPROFILE_STOP("bore_extract_files_from_projects");

    BORE_VIMPROFILE_START;
    if (FAIL == bore_sort_and_cleanup_files(b))
        goto fail;
    BORE_VIMPROFILE_STOP("bore_sort_and_cleanup_files");

    BORE_VIMPROFILE_START;
    if (FAIL == bore_build_extension_list(b))
        goto fail;
    BORE_VIMPROFILE_STOP("bore_build_extension_list");

    BORE_VIMPROFILE_START;
    if (FAIL == bore_build_toggle_index(b))
        goto fail;
    BORE_VIMPROFILE_STOP("bore_build_toggle_index");

    BORE_VIMPROFILE_START;
    if (FAIL == bore_write_filelist_to_tempfile(b))
        goto fail;
    BORE_VIMPROFILE_STOP("bore_write_filelist_to_tempfile");

    sprintf(buf, "let g:bore_base_dir=\'%s\'", bore_str(b, b->sln_dir));
    do_cmdline_cmd(buf);

    sprintf(buf, "let g:bore_filelist_file=\'%s\'", b->filelist_tmp_file);
    do_cmdline_cmd(buf);

    g_bore = b;
    return;

fail:
    bore_free(b);
    EMSG2(_("Could not open solution file %s"), buf);
    return;
}

static void bore_print_sln(DWORD elapsed)
{
    if (g_bore) {
        char status[BORE_MAX_PATH];
        if (elapsed)
        {
            sprintf(status, "%s, %d projects, %d files (%u ms)", bore_str(g_bore, g_bore->sln_path),
                    g_bore->proj_count, g_bore->file_count, elapsed);
        }
        else
        {
            sprintf(status, "%s, %d projects, %d files", bore_str(g_bore, g_bore->sln_path),
                    g_bore->proj_count, g_bore->file_count);
        }           
        MSG(_(status));
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
    if (attr) {
        *attr = GetFileAttributesW(wbuf);
        if (*attr == INVALID_FILE_ATTRIBUTES)
            return FAIL;
    }
    result = WideCharToMultiByte(CP_UTF8, 0, wbuf2, -1, dst, BORE_MAX_PATH, 0, 0);
    if (!result)
        return FAIL;
    return OK;
}

static u32 bore_string_hash(const char *str)
{
    return bore_string_hash_n(str, -1);
}

static u32 bore_string_hash_n(const char *str, int n)
{
    u32 h;
    u8 *p = (u8*)str;
    u8 *pend = p + n;

    h = 0;
    for (; p != pend && *p != '\0'; p++)
        h = 33 * h + tolower(*p);
    return h + (h >> 5);
}

static void bore_display_search_result(bore_t* b, const char* filename, char* what, int found)
{
    exarg_T eap;
    char* title = (char*)alloc(100);
    vim_snprintf(title, 100, "borefind \"%s\", %d lines%s", what, found > 0 ? found : -found, found < 0 ? " (truncated)" : "");

    memset(&eap, 0, sizeof(eap));
    eap.cmdidx = CMD_cgetfile;
    eap.arg = (char*)filename;
    eap.cmdlinep = &title;
    ex_cfile(&eap);

    memset(&eap, 0, sizeof(eap));
    eap.cmdidx = CMD_cwindow;
    ex_copen(&eap);

    vim_free(title);
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

static int bore_find(bore_t* b, char* what, char* what_ext)
{
    enum { MaxMatch = 1000 };
    u32* files = (u32*)b->file_alloc.base;
    int found = 0;
    char_u *tmp = vim_tempname('f');
    FILE* cf = 0;
    bore_match_t* match = 0;
    int truncated = 0;

    match = (bore_match_t*)alloc(MaxMatch * sizeof(bore_match_t));

    int threadCount = 4;
    const char_u* threadCountStr = get_var_value((char_u *)"g:bore_search_thread_count");
    if (threadCountStr)
    {
        threadCount = atoi(threadCountStr);
    }

    bore_search_t search;
    search.what = what;
    search.what_len = strlen(what);
    search.ext_count = 0;

    // parse comma separated list of file extensions into list of hashes
    if (what_ext)
    {
        int len = 0;
        char* ext = what_ext;
        char* c;
        for (c = ext; search.ext_count < BORE_MAX_SEARCH_EXTENSIONS; ++c)
        {
            if (*c == ',' || *c == '\0')
            {
                search.ext[search.ext_count++] = bore_string_hash_n(ext, len);
                ext = c + 1;
                len = 0;
            }
            else
            {
                ++len;
            }

            if (*c == '\0')
                break;
        }
    }

    found = bore_dofind(b, threadCount, &truncated, match, MaxMatch, &search);
    if (0 == found)
        goto fail;

    cf = mch_fopen((char *)tmp, "wb");
    if (cf == NULL) {
        EMSG2(_(e_notopen), tmp);
        goto fail;
    }
    bore_save_match_to_file(b, cf, match, found);

    fclose(cf);

    bore_display_search_result(b, tmp, what, truncated ? -found : found);
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
    //char_u    *arg;
    char_u  maparg[512];
    int    n;
#ifdef FEAT_WINDOWS
    win_T    *wp;
#endif
    //    char_u        *p;
    int    empty_fnum = 0;
    int    alt_fnum = 0;
    buf_T    *buf;
    FILE    *filelist_fd = 0;

    if (!g_bore || !g_bore->filelist_tmp_file) {
        EMSG(_("Load a solution first with boresln"));
        return;
    }

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
        restart_edit = 0;           /* don't want insert mode in help file */

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

void bore_async_execute_completed()
{
    CloseHandle(g_bore_async_execute_context.wait_thread);
    CloseHandle(g_bore_async_execute_context.spawned_process.hThread);
    CloseHandle(g_bore_async_execute_context.spawned_process.hProcess);
    CloseHandle(g_bore_async_execute_context.result_handle);

    exarg_T eap;
    char* title = (char*)alloc(100);
    vim_snprintf(title, 100, "borebuild");

    memset(&eap, 0, sizeof(eap));
    eap.cmdidx = CMD_cgetfile;
    eap.arg = (char*)g_bore_async_execute_context.result_filename;
    eap.cmdlinep = &title;
    ex_cfile(&eap);

    memset(&eap, 0, sizeof(eap));
    eap.cmdidx = CMD_cwindow;
    ex_copen(&eap);

    vim_free(title);

    g_bore_async_execute_context.wait_thread = INVALID_HANDLE_VALUE;
    g_bore_async_execute_context.spawned_process.hThread = INVALID_HANDLE_VALUE;
    g_bore_async_execute_context.spawned_process.hProcess = INVALID_HANDLE_VALUE;
    g_bore_async_execute_context.result_handle = INVALID_HANDLE_VALUE;

    update_screen(VALID);
}

static DWORD WINAPI bore_async_execute_wait_thread(LPVOID param)
{
    extern HWND s_hwnd;
    DWORD result = WaitForSingleObject(g_bore_async_execute_context.spawned_process.hProcess, INFINITE);
    assert(sizeof(WPARAM) == sizeof(&bore_async_execute_completed));
    WPARAM wparam = (WPARAM)&bore_async_execute_completed;
    LPARAM lparam = (result == WAIT_OBJECT_0);
    PostMessage(s_hwnd, WM_USER + 1234, wparam, lparam);
    return 0;
}

static void bore_async_execute(const char* cmdline)
{
    if (g_bore_async_execute_context.wait_thread != INVALID_HANDLE_VALUE) {
        EMSG(_("bore_async_execute: Busy. Cannot launch another process."));
        return;
    }

    autowrite_all();

    SECURITY_ATTRIBUTES sa_attr = {0};
    sa_attr.nLength = sizeof(sa_attr);
    sa_attr.bInheritHandle = TRUE;
    sa_attr.lpSecurityDescriptor = NULL;

    if (g_bore_async_execute_context.result_filename[0] == 0) {
        char temp_path[MAX_PATH];

        DWORD result = GetTempPathA(MAX_PATH, temp_path);

        if (result == 0 || result > MAX_PATH) {
            EMSG(_("bore_async_execute: Could get temp path"));
            goto fail;
        }

        result = GetTempFileNameA(
                temp_path, 
                "bore_build", 
                0, 
                g_bore_async_execute_context.result_filename);

        if (result == 0) {
            EMSG(_("bore_async_execute: Could get temp filename"));
            goto fail;
        }
    }
    
    // TODO-jkjellstrom: Doesn't seem to be possible to copen a file we still have a handle open to.
    //                   Temp files will remain after closing the session right now.
    g_bore_async_execute_context.result_handle = CreateFileA(
            g_bore_async_execute_context.result_filename, 
            GENERIC_WRITE|GENERIC_READ, 
            FILE_SHARE_READ, 
            &sa_attr, 
            CREATE_ALWAYS, 
            //FILE_ATTRIBUTE_NORMAL|FILE_FLAG_DELETE_ON_CLOSE, 
            FILE_ATTRIBUTE_NORMAL, 
            NULL);

    if (g_bore_async_execute_context.result_handle == INVALID_HANDLE_VALUE) {
        EMSG(_("bore_async_execute: Could not create temp result file"));
        goto fail;
    }

    //SetFilePointer(g_bore_async_execute_context.result_handle, 0, 0, FILE_BEGIN);
    //SetEndOfFile(g_bore_async_execute_context.result_handle);

    STARTUPINFO startup_info = {0};
    char cmd[1024];

    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup_info.hStdOutput = g_bore_async_execute_context.result_handle;
    startup_info.hStdError = g_bore_async_execute_context.result_handle;

    if (-1 == _snprintf_s(cmd, sizeof(cmd), sizeof(cmd), "cmd.exe /c \"%s\"", cmdline)) {
        EMSG(_("bore_async_execute: Command line is too long"));
        goto fail;
    }

    memset(
            &g_bore_async_execute_context.spawned_process, 
            0, 
            sizeof(g_bore_async_execute_context.spawned_process));

    BOOL process_created = CreateProcess(
            NULL, 
            cmd, 
            NULL, 
            &sa_attr, 
            TRUE, 
            CREATE_NO_WINDOW, 
            NULL, 
            NULL, 
            &startup_info, 
            &g_bore_async_execute_context.spawned_process);

    if (!process_created) {
        EMSG(_("bore_async_execute: Failed to spawn process"));
        goto fail;
    }

    DWORD thread_id = 0;

    g_bore_async_execute_context.wait_thread = CreateThread(
            NULL, 
            4096, 
            bore_async_execute_wait_thread, 
            &g_bore_async_execute_context, 
            0, 
            &thread_id);

    if (g_bore_async_execute_context.wait_thread == INVALID_HANDLE_VALUE) {
        CloseHandle(g_bore_async_execute_context.spawned_process.hThread);
        CloseHandle(g_bore_async_execute_context.spawned_process.hProcess);
        EMSG(_("bore_async_execute: Failed to spawn wait thread"));
        goto fail;
    }

    return;

fail:
    CloseHandle(g_bore_async_execute_context.wait_thread);
    CloseHandle(g_bore_async_execute_context.spawned_process.hThread);
    CloseHandle(g_bore_async_execute_context.spawned_process.hProcess);
    CloseHandle(g_bore_async_execute_context.result_handle);
    g_bore_async_execute_context.wait_thread = INVALID_HANDLE_VALUE;
}


#endif

/* Only do the following when the feature is enabled.  Needed for "make
 * depend". */
#if defined(FEAT_BORE) || defined(PROTO)

void ex_boresln __ARGS((exarg_T *eap))
{
    if (*eap->arg == NUL) {
        bore_print_sln(0);
    } else {
        DWORD start = GetTickCount();
        DWORD elapsed;
        bore_load_sln((char*)eap->arg);
        elapsed = GetTickCount() - start;
        bore_print_sln(elapsed);
    }
}

void borefind_parse_options(char* arg, char** what, char** what_ext)
{
    // Usage: [option(s)] what
    //   -e ext1,ext2,...,ext12
    //      filters the search based on a list of file extensions
    //   - 
    //   -u
    //      an empty (or any unknown) option will force the remainder to be treated as the search string

    char* opt = NUL;
    *what = arg;
    *what_ext = NUL;

    for (; *arg; ++arg)
    {
        if (NUL == opt)
        {
            if ('-' == *arg)
            {
                // found new option marker
                ++arg;
                if (*arg == 'e' && arg[1] == ' ')
                {
                    // found extension option argument start, loop until next space
                    opt = arg;
                    *what_ext = &opt[2];
                    arg += 2;
                }
                else
                {
                    // empty or unknown option, treat the rest as the search string
                    *what = arg + 1;
                    break;
                }
            }
            else
            {
                // no option found, treat the rest as the search string
                *what = arg;
                break;
            }
        }
        else if (' ' == *arg)
        {
            // end current option argument string and search for next option
            opt = NUL;
            *arg = '\0';
        }
    }
}

void ex_borefind __ARGS((exarg_T *eap))
{
    if (!g_bore) {
        EMSG(_("Load a solution first with boresln"));
    }
    else {
        DWORD start = GetTickCount();
        DWORD elapsed;
        char mess[100];
        char* what;
        char* what_ext;

        borefind_parse_options((char*)eap->arg, &what, &what_ext);
        int found = bore_find(g_bore, what, what_ext);
        elapsed = GetTickCount() - start;
        if (found)
        {
            vim_snprintf(mess, 100, "Matching lines: %d%s Elapsed time: %u ms", found > 0 ? found : -found, found < 0 ? " (truncated)" : "", elapsed);
            MSG(_(mess));
        }
        else
        {
            vim_snprintf(mess, 100, "No matching lines for \"%s\": Elapsed time: %u ms", what, elapsed);
            EMSG(_(mess));
        }
    }
}

void ex_boreopen __ARGS((exarg_T *eap))
{
    if (!g_bore)
        EMSG(_("Load a solution first with boresln"));
    else {
        const char* mappings[] = {
            "<CR> :ZZBoreopenselection<CR>", 
            "<2-LeftMouse> :ZZBoreopenselection<CR>",
            0};
        bore_show_borebuf(g_bore->filelist_tmp_file, g_bore->ini.borebuf_height, mappings);
    }
}

int bore_toggle_entry_score(const char* buffer_name, const char* candidate)
{
    int score = 0;
    while(*buffer_name && *candidate && (tolower(*buffer_name) == tolower(*candidate))) {
        ++buffer_name;
        ++candidate;
        ++score;
    }
    return score;
}

void bore_open_file_buffer(char_u* fn)
{ 
    buf_T* buf;

    buf = buflist_findname_exp(fn);
    if (NULL == buf)
        goto edit;

    if (NULL != buf->b_ml.ml_mfp && NULL != buf_jump_open_tab(buf))
        goto verify;

    set_curbuf(buf, DOBUF_GOTO);

verify:
    if (buf != curbuf)
edit:
    {
        exarg_T ea;
        memset(&ea, 0, sizeof(ea));
        ea.arg = fn;
        ea.cmdidx = CMD_edit;
        do_exedit(&ea, NULL);
    }
}

void ex_boretoggle __ARGS((exarg_T *eap))
{
    if (!g_bore)
        EMSG(_("Load a solution first with boresln"));
    else {
        char path[BORE_MAX_PATH];
        const char* ext;
        const char* basename;
        int path_len;
        u32 basename_hash;
        u32 ext_hash;
        const bore_toggle_entry_t* e_begin = (const bore_toggle_entry_t*)g_bore->toggle_index_alloc.base;
        const bore_toggle_entry_t* e = e_begin;
        const bore_toggle_entry_t* e_end = e + g_bore->toggle_entry_count;
        const bore_toggle_entry_t* e_buf;
        const bore_toggle_entry_t* e_best;
        int e_best_score;

        if (FAIL == bore_canonicalize(curbuf->b_fname, path, 0))
            return;

        path_len = strlen(path);

        ext = vim_strrchr(path, '.');
        ext = ext ? ext + 1 : path + path_len;
        ext_hash = bore_string_hash(ext);

        basename = vim_strrchr(path, '\\');
        basename = basename ? basename + 1 : path;
        basename_hash = bore_string_hash_n(basename, ext - basename);

        // find first entry with identical basename
        e = e_begin;
        for (; e != e_end; ++e) {
            if (e->basename_hash == basename_hash)
                break;
        }

        if (e == e_end)
            return;

        e_begin = e; // first match is now the new beginning

        // Find the entry of this buffer's file
        for (; e != e_end && e->basename_hash == basename_hash; ++e)
            if (0 == stricmp(bore_str(g_bore, ((u32*)(g_bore->file_alloc.base))[e->fileindex]) , path))
                break;

        if (e == e_end || e->basename_hash != basename_hash)
            return;

        // Find what ext to toggle to
        e_buf = e;
        for(;;) {
            ++e;
            if (e == e_end || e->basename_hash != basename_hash)
                e = e_begin;
            if (e == e_buf)
                return; // no match
            if (e->extension_index != e_buf->extension_index) {
                break;
            }
        }

        // Find the best matching ext
        e_best = e;
        e_best_score = bore_toggle_entry_score(path, bore_str(g_bore, ((u32*)(g_bore->file_alloc.base))[e->fileindex]));
        ++e;
        for(; e != e_end && e_best->extension_index == e->extension_index; ++e)
        {
            int score = bore_toggle_entry_score(path, bore_str(g_bore, ((u32*)(g_bore->file_alloc.base))[e->fileindex]));
            if (score > e_best_score) {
                e_best_score = score;
                e_best = e;
            }
        }

        {
            const char *slndir = bore_str(g_bore, g_bore->sln_dir);
            int slndirlen = strlen(slndir);
            char *fn = bore_str(g_bore, ((u32*)g_bore->file_alloc.base)[e_best->fileindex]);
            if (strncmp(fn, slndir, slndirlen) == 0)
                fn += slndirlen;
            bore_open_file_buffer(fn);
        }
    }
}

void ex_borebuild __ARGS((exarg_T *eap))
{
    if (!g_bore) {
        EMSG(_("Load a solution first with boresln"));
    } else {
        //bore_async_execute("dir");
        bore_async_execute("msbuild.exe /nologo vim_vs2010.vcxproj /t:Build /p:Configuration=Debug;Platform=Win32 /verbosity:minimal");
    }
}


// Internal functions

// Open the file on the current row in the current buffer 
void ex_Boreopenselection __ARGS((exarg_T *eap))
{
    char_u* fn;
    if (!g_bore)
        return;
    fn = vim_strsave(ml_get_curline());
    if (!fn)
        return;

    win_close(curwin, TRUE);

    bore_open_file_buffer(fn);
    vim_free(fn);
}

#endif
