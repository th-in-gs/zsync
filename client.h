/*
 *   zsync - client side rsync over http
 *   Copyright (C) 2004,2005,2007,2009 Colin Phipps <cph@moria.org.uk>
 *   Modifications Copyright (C) 2009 James Montgomerie <jamie@th.ingsmadeoutofotherthin.gs>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the Artistic License v2 (see the accompanying 
 *   file COPYING for the full license terms), or, at your option, any later 
 *   version of the same license.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   COPYING file for details.
 */

/* zsync client library header */

#include <stdbool.h>
#include <stdio.h>

struct zsync_http_routines {
    // Takes a URL, referrer (updated on a redirect), optional filename to save to.
    // Return a handle to the file, opened and positioned at the beginning.
    // Returned referrer will be freed by the caller.
    // This pointer ('http_get' in the zsync_http_routines structure) may be 
    // NULL if the zsync control file specified in the zsync_client call is
    // already local.
    FILE*(*http_get)(const char *orig_url, char **track_referrer, const char *tfname); 
    
    // Prepare to fetch some ranges from a URL.  
    // Returns a status blob referred to by the calls below..
    void*(*range_fetch_start)(const char *url, const char* referrer);

    // Add ranges to this request.
    // First argument is the status blob, from range_fetch_start.
    // Second argument is an array of offsets.
    // Third argument in number of ranges (pairs of offsets) in the offsets list.
    void(*range_fetch_addranges)(void *rf, off_t* ranges, int nranges);
    
    // Called repeatedly to get the data for a set of range fetches.
    // First argument is the status blob from range_fetch_start.
    // Second argument will be set to the offset the received data starts at.
    // Third argument is a buffer to receive data in.
    // Fourth argument is the data buffer length.
    // Return the total bytes read, 0 for EOF, -1 for error (like 'read').
    int(*get_range_block)(void *rf, off_t *offset, unsigned char *data, size_t dlen);
    
    // Returns the total bytes retreived in this request.
    // Takes a status blob.
    off_t(*range_fetch_bytes_down)(const void *rf);
    
    // Called after a set of range fetches is complete.
    // Takes a status blob (which should become invalid after this call).
    void(*range_fetch_end)(void *rf);
};

struct zsync_progress_routines {
    // Takes a URL, returns a status blob used when further tracking progress 
    // for this URL.
    void*(*start_progress)(const char *url);
    
    // Takes a status blob returned by start_progress, a percentage and the 
    // total bytes retreived so far.
    void(*do_progress)(void* p, float pcnt, long long newdl);
    
    // Takes a status blob, and a done parameter: 0 for error, 1 for 
    // okay-but-incomplete, 2 for completed 
    void(*end_progress)(void* p, int done);    
};

#define zs_ok 0
#define zs_read_control_file_err 1
#define zs_download_local_err 2
#define zs_download_receive_err 3
#define zs_move_received_file_err 4
#define zs_backup_old_file_err 5
typedef int zs_return;

/* progress may be NULL if quiet is true */
zs_return zsync_client(const char *control_file_location, 
                       const char *keep_control_file_path, 
                       const char *output_file_path, 
                       const char *referrer,
                       char **seedfiles,
                       const int nseedfiles,
                       bool quiet,
                       struct zsync_http_routines *http,
                       struct zsync_progress_routines *progress);
