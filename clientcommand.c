
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

/* zsync command-line client program */

#include "client.h"

#include "zsglobal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/types.h>

#include "progress.h"
#include "http.h"
 
/* A ptrlist is a very simple structure for storing lists of pointers. This is
 * the only function in its API. The structure (not actually a struct) consists
 * of a (pointer to a) void*[] and an int giving the number of entries.
 *
 * ptrlist = append_ptrlist(&entries, ptrlist, new_entry)
 * Like realloc(2), this returns the new location of the ptrlist array; the
 * number of entries is passed by reference and updated in place. The new entry
 * is appended to the list.
 */
static void **append_ptrlist(int *n, void **p, void *a) {
    if (!a)
        return p;
    p = realloc(p, (*n + 1) * sizeof *p);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    p[*n] = a;
    (*n)++;
    return p;
}

/****************************************************************************
 *
 * Main program */
int main(int argc, char **argv) {
    char **seedfiles = NULL;
    int nseedfiles = 0;
    char *filename = NULL;
    char *zfname = NULL;
    char *referrer = NULL;
    int no_progress = 0;
    
    {   /* Option parsing */
        int opt;
        
        while ((opt = getopt(argc, argv, "A:k:o:i:Vsqu:")) != -1) {
            switch (opt) {
                case 'A':           /* Authentication options for remote server */
                    {               /* Scan string as hostname=username:password */
                        char *p = strdup(optarg);
                        char *q = strchr(p, '=');
                        char *r = q ? strchr(q, ':') : NULL;
                        
                        if (!q || !r) {
                            fprintf(stderr,
                                    "-A takes hostname=username:password\n");
                            return 1;
                        }
                        else {
                            *q++ = *r++ = 0;
                            add_auth(p, q, r);
                        }
                    }
                    break;
                case 'k':
                    free(zfname);
                    zfname = strdup(optarg);
                    break;
                case 'o':
                    free(filename);
                    filename = strdup(optarg);
                    break;
                case 'i':
                    seedfiles = (char **)append_ptrlist(&nseedfiles, (void **)seedfiles, optarg);
                    break;
                case 'V':
                    printf(PACKAGE " v" VERSION " (compiled " __DATE__ " " __TIME__
                           ")\n" "By Colin Phipps <cph@moria.org.uk>\n"
                           "Published under the Artistic License v2, see the COPYING file for details.\n");
                    return 0;
                case 's':
                case 'q':
                    no_progress = 1;
                    break;
                case 'u':
                    free(referrer);
                    referrer = strdup(optarg);
                    break;
            }
        }
    }
    
    /* Last and only non-option parameter must be the path/URL of the .zsync */
    if (optind == argc) {
        fprintf(stderr,
                "No .zsync file specified.\nUsage: zsync http://example.com/some/filename.zsync\n");
        return 3;
    }
    else if (optind < argc - 1) {
        fprintf(stderr,
                "Usage: zsync http://example.com/some/filename.zsync\n");
        return 3;
    }
    
    /* No progress display except on terminal */
    if (!isatty(0))
        no_progress = 1;
    
    {   /* Get proxy setting from the environment */
        char *pr = getenv("http_proxy");
        
        if (pr != NULL)
            set_proxy_from_string(pr);
    }
    
    struct zsync_http_routines http_routines = 
    {
        http_get,
        range_fetch_start,
        range_fetch_addranges,
        get_range_block,
        range_fetch_bytes_down,
        range_fetch_end
    };
    
    struct zsync_progress_routines progress_routines = 
    {
        start_progress,
        do_progress,
        end_progress
    };
    
    no_http_progress = no_progress;
    
    return zsync_client(argv[optind], zfname, filename, referrer, seedfiles, nseedfiles, no_progress, &http_routines, &progress_routines);
}
