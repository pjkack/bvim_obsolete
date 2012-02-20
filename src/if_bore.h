#pragma once
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif

#define BORE_MAX_SMALL_PATH 256
#define BORE_MAX_PATH 1024
#define BORE_SEARCH_JOBS 8
#define BORE_SEARCH_RESULTS 8
#define BORE_CACHELINE 64 
#define BORE_MAXMATCHPERFILE 100

typedef unsigned char u8;
typedef unsigned int u32;

typedef struct bore_alloc_t {
	u8* base; // cacheline aligned
	u8* end;
	u8* cursor;
	size_t offset; // for alignment
} bore_alloc_t;

typedef struct bore_proj_t {
	u32 project_name;
	u32 project_path;
} bore_proj_t;

typedef struct bore_match_t {
	u32 file_index;
	u32 row;
	u32 column;
	char line[1024 - 12];
} bore_match_t;

typedef struct bore_ini_t {
	int borebuf_height; // Default height of borebuf window
} bore_ini_t;

typedef struct __declspec(align(BORE_CACHELINE)) bore_search_job_t {
	bore_alloc_t filedata;
	int fileindex;
} bore_search_job_t;

typedef struct __declspec(align(BORE_CACHELINE)) bore_search_result_t { 
	int hits;
	bore_match_t result[BORE_MAXMATCHPERFILE];  
} bore_search_result_t;

typedef struct bore_toggle_entry_t {
	u32 basename_hash;
	u32 extension_hash;
	int extension_index;
	u32 fileindex;
} bore_toggle_entry_t;

typedef struct bore_t {
	u32 sln_path; // abs path of solution
	u32 sln_dir;  // abs dir of solution

	char* filelist_tmp_file; // name of temporary filelist file

	// array of bore_proj_t (all projects in the solution)
	int proj_count;
	bore_alloc_t proj_alloc; 

	// array of files in the solution
	int file_count;
	bore_alloc_t file_alloc;

	// array of bore_toggle_entry_t;
	int toggle_entry_count;
	bore_alloc_t toggle_index_alloc;

	bore_alloc_t data_alloc; // bulk data (filenames, strings, etc)

	// context used for searching
	bore_search_job_t search[BORE_SEARCH_JOBS];
	bore_search_result_t search_result[BORE_SEARCH_RESULTS];

	bore_ini_t ini;
} bore_t;

void bore_prealloc(bore_alloc_t* p, size_t size);
void* bore_alloc(bore_alloc_t* p, size_t size);
void bore_alloc_trim(bore_alloc_t* p, size_t size);
void bore_alloc_free(bore_alloc_t* p);

char* bore_str(bore_t* b, u32 offset);

int bore_dofind(bore_t* b, int* truncated, bore_match_t* match, int match_size, const char* what);

#ifdef __cplusplus
}
#endif
