
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

/* zsync client library */

#include "client.h"

#include "zsglobal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <utime.h>
#include <stdbool.h>

#ifdef WITH_DMALLOC
# include <dmalloc.h>
#endif

#include "libzsync/zsync.h"

#include "url.h"

struct zsync_client_state 
{
    struct zsync_http_routines *http_routines;
    struct zsync_progress_routines *progress_routines;
    bool quiet;
    unsigned random_seed;
    char *referrer;
    long long http_down;
};

/* FILE* f = open_zcat_pipe(file_str)
 * Returns a (popen) filehandle which when read returns the un-gzipped content
 * of the given file. Or NULL on error; or the filehandle may fail to read. It
 * is up to the caller to call pclose() on the handle and check the return
 * value of that.
 */
static FILE* open_zcat_pipe(const char* fname, bool quiet)
{
    /* Get buffer to build command line */
    char *cmd = malloc(6 + strlen(fname) * 2);
    if (!cmd)
        return NULL;

    strcpy(cmd, "zcat ");
    {   /* Add filename to commandline, escaping any characters that the shell
         * might consider special. */
        int i, j;

        for (i = 0, j = 5; fname[i]; i++) {
            if (!isalnum(fname[i]))
                cmd[j++] = '\\';
            cmd[j++] = fname[i];
        }
        cmd[j] = 0;
    }

    if (!quiet)
        fprintf(stderr, "reading seed %s: ", cmd);
    {   /* Finally, open the subshell for reading, and return the handle */
        FILE* f = popen(cmd, "r");
        free(cmd);
        return f;
    }
}

/* read_seed_file(zsync, filename_str)
 * Reads the given file (decompressing it if appropriate) and applies the rsync
 * checksum algorithm to it, so any data that is contained in the target file
 * is written to the in-progress target. So use this function to supply local
 * source files which are believed to have data in common with the target.
 */
static void read_seed_file(struct zsync_client_state *cs, struct zsync_state *z, const char *fname) {
    /* If we should decompress this file */
    if (zsync_hint_decompress(z) && strlen(fname) > 3
        && !strcmp(fname + strlen(fname) - 3, ".gz")) {
        /* Open for reading */
        FILE *f = open_zcat_pipe(fname, cs->quiet);
        if (!f) {
            perror("popen");
            fprintf(stderr, "not using seed file %s\n", fname);
        }
        else {

            /* Give the contents to libzsync to read and find any useful
             * content */
            zsync_submit_source_file(z, f, !cs->quiet);

            /* Close and check for errors */
            if (pclose(f) != 0) {
                perror("close");
            }
        }
    }
    else {
        /* Simple uncompressed file - open it */
        FILE *f = fopen(fname, "r");
        if (!f) {
            perror("open");
            fprintf(stderr, "not using seed file %s\n", fname);
        }
        else {

            /* Give the contents to libzsync to read, to find any content that
             * is part of the target file. */
            if (!cs->quiet)
                fprintf(stderr, "reading seed file %s: ", fname);
            zsync_submit_source_file(z, f, !cs->quiet);

            /* And close */
            if (fclose(f) != 0) {
                perror("close");
            }
        }
    }

    {   /* And print how far we've progressed towards the target file */
        long long done, total;

        zsync_progress(z, &done, &total);
        if (!cs->quiet)
            fprintf(stderr, "\rRead %s. Target %02.1f%% complete.      \n",
                    fname, (100.0f * done) / total);
    }
}

/* zs = read_zsync_control_file(location_str, filename)
 * Reads a zsync control file from either a URL or filename specified in
 * location_str. This is treated as a URL if no local file exists of that name
 * and it starts with a URL scheme ; only http URLs are supported.
 * Second parameter is a filename in which to locally save the content of the
 * .zsync _if it is retrieved from a URL_; can be NULL in which case no local
 * copy is made.
 */
static struct zsync_state *read_zsync_control_file(struct zsync_client_state *cs, const char *p, const char *fn, zs_return *error) {
    FILE *f;
    struct zsync_state *zs;
    char *lastpath = NULL;

    /* Try opening as a local path */
    f = fopen(p, "r");
    if (!f) {
        /* No such local file - if not a URL either, report error */
        if (!is_url_absolute(p)) {
            perror(p);
            *error = zs_download_local_err;
            return;
        }

        /* Try URL fetch */
        if(cs->http_routines->http_get) {
            f = cs->http_routines->http_get(p, &lastpath, fn);
        }
        if (!f) {
            fprintf(stderr, "could not read control file from URL %s\n", p);
            *error = zs_download_receive_err;
            return;
        }
        free(cs->referrer);
        cs->referrer = lastpath;
    }

    /* Read the .zsync */
    if ((zs = zsync_begin(f)) == NULL) {
        *error = zs_read_control_file_err;
    }

    /* And close it */
    if (fclose(f) != 0) {
        perror("fclose");
        *error = zs_download_local_err;
    }
    return zs;
}

/* str = get_filename_prefix(path_str)
 * Returns a (malloced) string of the alphanumeric leading segment of the
 * filename in the given file path.
 */
static char *get_filename_prefix(const char *p) {
    char *s = strdup(p);
    char *t = strrchr(s, '/');
    char *u;

    if (t)
        *t++ = 0;
    else
        t = s;
    u = t;
    while (isalnum(*u)) {
        u++;
    }
    *u = 0;
    if (*t > 0)
        t = strdup(t);
    else
        t = NULL;
    free(s);
    return t;
}

/* filename_str = get_filename(zs, source_filename_str)
 * Returns a (malloced string with a) suitable filename for a zsync download,
 * using the given zsync state and source filename strings as hints. */
static char *get_filename(const struct zsync_state *zs, const char *source_name) {
    char *p = zsync_filename(zs);
    char *filename = NULL;

    if (p) {
        if (strchr(p, '/')) {
            fprintf(stderr,
                    "Rejected filename specified in %s, contained path component.\n",
                    source_name);
            free(p);
        }
        else {
            char *t = get_filename_prefix(source_name);

            if (t && !memcmp(p, t, strlen(t)))
                filename = p;
            else
                free(p);

            if (t && !filename) {
                fprintf(stderr,
                        "Rejected filename specified in %s - prefix %s differed from filename %s.\n",
                        source_name, t, p);
            }
            free(t);
        }
    }
    if (!filename) {
        filename = get_filename_prefix(source_name);
        if (!filename)
            filename = strdup("zsync-download");
    }
    return filename;
}

/* prog = calc_zsync_progress(zs)
 * Returns the progress ratio 0..1 (none...done) for the given zsync_state */
static float calc_zsync_progress(const struct zsync_state *zs) {
    long long zgot, ztot;

    zsync_progress(zs, &zgot, &ztot);
    return (100.0f * zgot / ztot);
}

/* fetch_remaining_blocks_http(zs, url, type)
 * For the given zsync_state, using the given URL (which is a copy of the
 * actual content of the target file is type == 0, or a compressed copy of it
 * if type == 1), retrieve the parts of the target that are currently missing. 
 * Returns true if this URL was useful, false if we crashed and burned.
 */
#define BUFFERSIZE 8192

static int fetch_remaining_blocks_http(struct zsync_client_state *cs, 
                                       struct zsync_state *z, 
                                       const char *url,
                                       int type) {
    int ret = 0;
    void *rf;
    unsigned char *buf;
    struct zsync_receiver *zr;

    /* URL might be relative - we need an absolute URL to do a fetch */
    char *u = make_url_absolute(cs->referrer, url);
    if (!u) {
        fprintf(stderr,
                "URL '%s' from the .zsync file is relative, but I don't know the referrer URL (you probably downloaded the .zsync separately and gave it to me as a file). I need to know the referring URL (the URL of the .zsync) in order to locate the download. You can specify this with -u (or edit the URL line(s) in the .zsync file you have).\n",
                url);
        return -1;
    }

    /* Start a range fetch and a zsync receiver */
    rf = cs->http_routines->range_fetch_start(u, cs->referrer);
    if (!rf) {
        free(u);
        return -1;
    }
    zr = zsync_begin_receive(z, type);
    if (!zr) {
        cs->http_routines->range_fetch_end(rf);
        free(u);
        return -1;
    }

    if (!cs->quiet)
        fprintf(stderr, "downloading from %s:", u);

    /* Create a read buffer */
    buf = malloc(BUFFERSIZE);
    if (!buf) {
        zsync_end_receive(zr);
        cs->http_routines->range_fetch_end(rf);
        free(u);
        return -1;
    }

    {   /* Get a set of byte ranges that we need to complete the target */
        int nrange;
        off_t *zbyterange = zsync_needed_byte_ranges(z, &nrange, type);
        if (!zbyterange)
            return 1;
        if (nrange == 0)
            return 0;

        /* And give that to the range fetcher */
        cs->http_routines->range_fetch_addranges(rf, zbyterange, nrange);
        free(zbyterange);
    }

    {
        int len;
        off_t zoffset;
        void *p = NULL;

        /* Set up progress display to run during the fetch */
        if (!cs->quiet) {
            p = cs->progress_routines->start_progress(u);
            fputc('\n', stderr);
            do_progress(p, calc_zsync_progress(z), cs->http_routines->range_fetch_bytes_down(rf));
        }

        /* Loop while we're receiving data, until we're done or there is an error */
        while (!ret
               && (len = get_range_block(rf, &zoffset, buf, BUFFERSIZE)) > 0) {
            /* Pass received data to the zsync receiver, which writes it to the
             * appropriate location in the target file */
            if (zsync_receive_data(zr, buf, zoffset, len) != 0)
                ret = 1;

            /* Maintain progress display */
            if (!cs->quiet)
                cs->progress_routines->do_progress(p, calc_zsync_progress(z),
                                                   cs->http_routines->range_fetch_bytes_down(rf));

            // Needed in case next call returns len=0 and we need to signal where the EOF was.
            zoffset += len;     
        }

        /* If error, we need to flag that to our caller */
        if (len < 0)
            ret = -1;
        else    /* Else, let the zsync receiver know that we're at EOF; there
                 *could be data in its buffer that it can use or needs to process */
            zsync_receive_data(zr, NULL, zoffset, 0);

        if (!cs->quiet)
            cs->progress_routines->end_progress(p, zsync_status(z) >= 2 ? 2 : len == 0 ? 1 : 0);
    }

    /* Clean up */
    free(buf);
    cs->http_down += cs->http_routines->range_fetch_bytes_down(rf);
    zsync_end_receive(zr);
    cs->http_routines->range_fetch_end(rf);
    free(u);
    return ret;
}

/* fetch_remaining_blocks(zs, random_seed)
 * Using the URLs in the supplied zsync state, downloads data to complete the
 * target file. 
 * random_seed is a seed used by the random number generator that controls
 * the order URLs are fetched from.
 */
static int fetch_remaining_blocks(struct zsync_client_state *cs, struct zsync_state *zs) {
    int n, utype;
    const char *const *url = zsync_get_urls(zs, &n, &utype);
    int *status;        /* keep status for each URL - 0 means no error */
    int ok_urls = n;

    if (!url) {
        fprintf(stderr, "no URLs available from zsync?");
        return 1;
    }
    status = calloc(n, sizeof *status);

    /* Keep going until we're done or have no useful URLs left */
    while (zsync_status(zs) < 2 && ok_urls) {
        /* Still need data; pick a URL to use. */
        int try = rand_r(&(cs->random_seed)) % n;

        if (!status[try]) {
            const char *tryurl = url[try];

            /* Try fetching data from this URL */
            int rc = fetch_remaining_blocks_http(cs, zs, tryurl, utype);
            if (rc != 0) {
                fprintf(stderr, "failed to retrieve from %s\n", tryurl);
                status[try] = 1;
                ok_urls--;
            }
        }
    }
    free(status);
    return 0;
}

static int set_mtime(const char* filename, time_t mtime) {
    struct stat s;
    struct utimbuf u;

    /* Get the access time, which I don't want to modify. */
    if (stat(filename, &s) != 0) {
        perror("stat");
        return -1;
    }
    
    /* Set the modification time. */
    u.actime = s.st_atime;
    u.modtime = mtime;
    if (utime(filename, &u) != 0) {
        perror("utime");
        return -1;
    }
    return 0;
}

zs_return zsync_client(const char *control_file_location, 
                       const char *keep_control_file_path, 
                       const char *output_file_path, 
                       const char *referrer,
                       char **seedfiles,
                       const int nseedfiles,
                       bool quiet,
                       struct zsync_http_routines *http_routines,
                       struct zsync_progress_routines *progress_routines) {
    zs_return ret = zs_ok;
    
    struct zsync_client_state cs = { 0 };
    struct zsync_state *zs = NULL;
    char *temp_file = NULL;
    long long local_used = 0;

    cs.http_routines = http_routines;
    cs.progress_routines = progress_routines;
    cs.quiet = quiet;

    /* Initialise the random seed used throughout */
    cs.random_seed = (unsigned)getpid() ^ (unsigned)time(NULL);
    
    if(referrer) {
        cs.referrer = strdup(referrer);
    }
    
    /* STEP 1: Read the zsync control file */
    zs_return error = zs_ok;
    zs = read_zsync_control_file(&cs, control_file_location, keep_control_file_path, &ret);
    if(ret != zs_ok) {
        goto bail;
    } else if (zs == NULL) {
        ret = zs_read_control_file_err;
        goto bail;
    }
    

    /* Get eventual filename for output, and filename to write to while working */
    if (!output_file_path)
        output_file_path = get_filename(zs, control_file_location);
    temp_file = malloc(strlen(output_file_path) + 6);
    strcpy(temp_file, output_file_path);
    strcat(temp_file, ".part");

    {   /* STEP 2: read available local data and fill in what we know in the
         *target file */
        int i;

        /* Try any seed files supplied by the command line */
        for (i = 0; i < nseedfiles; i++) {
            read_seed_file(&cs, zs, seedfiles[i]);
        }
        /* If the target file already exists, we're probably updating that file
         * - so it's a seed file */
        if (!access(output_file_path, R_OK)) {
            read_seed_file(&cs, zs, output_file_path);
        }
        /* If the .part file exists, it's probably an interrupted earlier
         * effort; a normal HTTP client would 'resume' from where it got to,
         * but zsync can't (because we don't know this data corresponds to the
         * current version on the remote) and doesn't need to, because we can
         * treat it like any other local source of data. Use it now. */
        if (!access(temp_file, R_OK)) {
            read_seed_file(&cs, zs, temp_file);
        }

        /* Show how far that got us */
        zsync_progress(zs, &local_used, NULL);

        /* People that don't understand zsync might use it wrongly and end up
         * downloading everything. Although not essential, let's hint to them
         * that they probably messed up. */
        if (!local_used) {
            if (!cs.quiet)
                fputs
                    ("No relevent local data found - I will be downloading the whole file. If that's not what you want, CTRL-C out. You should specify the local file is the old version of the file to download with -i (you might have to decompress it with gzip -d first). Or perhaps you just have no data that helps download the file\n",
                     stderr);
        }
    }

    /* libzsync has been writing to a randomely-named temp file so far -
     * because we didn't want to overwrite the .part from previous runs. Now
     * we've read any previous .part, we can replace it with our new
     * in-progress run (which should be a superset of the old .part - unless
     * the content changed, in which case it still contains anything relevant
     * from the old .part). */
    if (zsync_rename_file(zs, temp_file) != 0) {
        perror("rename");
        ret = zs_read_control_file_err;
        goto bail;
    }

    /* STEP 3: fetch remaining blocks via the URLs from the .zsync */
    if (fetch_remaining_blocks(&cs, zs) != 0) {
        fprintf(stderr,
                "failed to retrieve all remaining blocks - no valid download URLs remain. Incomplete transfer left in %s.\n(If this is the download filename with .part appended, zsync will automatically pick this up and reuse the data it has already done if you retry in this dir.)\n",
                temp_file);
        ret = zs_download_receive_err;
        goto bail;
    }

    {   /* STEP 4: verify download */
        int r;

        if (!cs.quiet)
            printf("verifying download...");
        r = zsync_complete(zs);
        switch (r) {
        case -1:
            fprintf(stderr, "Aborting, download available in %s\n", temp_file);
            ret = zs_download_local_err;
            goto bail;
            break;
        case 0:
            if (!cs.quiet)
                printf("no recognised checksum found\n");
            break;
        case 1:
            if (!cs.quiet)
                printf("checksum matches OK\n");
            break;
        }
    }

    free(temp_file);

    /* Get any mtime that we is suggested to set for the file, and then shut
     * down the zsync_state as we are done on the file transfer. Getting the
     * current name of the file at the same time. */
    time_t mtime = zsync_mtime(zs);
    temp_file = zsync_end(zs);

    /* STEP 5: Move completed .part file into place as the final target */
    if (output_file_path) {
        char *oldfile_backup = malloc(strlen(output_file_path) + 8);
        int ok = 1;

        strcpy(oldfile_backup, output_file_path);
        strcat(oldfile_backup, ".zs-old");

        if (!access(output_file_path, F_OK)) {
            /* backup of old file */
            unlink(oldfile_backup);     /* Don't care if this fails - the link below will catch any failure */
            if (link(output_file_path, oldfile_backup) != 0) {
                perror("link");
                fprintf(stderr,
                        "Unable to back up old file %s - completed download left in %s\n",
                        output_file_path, temp_file);
                ok = 0;         /* Prevent overwrite of old file below */
                ret = zs_backup_old_file_err;
            }
        }
        if (ok) {
            /* Rename the file to the desired name */
            if (rename(temp_file, output_file_path) == 0) {
                /* final, final thing - set the mtime on the file if we have one */
                if (mtime != -1) set_mtime(output_file_path, mtime);
            }
            else {
                perror("rename");
                fprintf(stderr,
                        "Unable to back up old file %s - completed download left in %s\n",
                        output_file_path, temp_file);
                ret = zs_move_received_file_err;
            }
        }
        free(oldfile_backup);
    }
    else {
        printf
            ("No filename specified for download - completed download left in %s\n",
             temp_file);
    }

bail:
    /* Final stats and cleanup */
    if (!cs.quiet)
        printf("used %lld local, fetched %lld\n", local_used, cs.http_down);
    
    free(cs.referrer);
    free(temp_file);
    
    return ret;
}



