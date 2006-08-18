/*-
 * CPDUP.C
 *
 * CPDUP <options> source destination
 *
 * (c) Copyright 1997-1999 by Matthew Dillon and Dima Ruban.  Permission to
 *     use and distribute based on the FreeBSD copyright.  Supplied as-is,
 *     USE WITH EXTREME CAUTION.
 *
 * This program attempts to duplicate the source onto the destination as 
 * exactly as possible, retaining modify times, flags, perms, uid, and gid.
 * It can duplicate devices, files (including hardlinks), softlinks, 
 * directories, and so forth.  It is recursive by default!  The duplication
 * is inclusive of removal of files/directories on the destination that do
 * not exist on the source.  This program supports a per-directory exception
 * file called .cpignore, or a user-specified exception file.
 *
 * Safety features:
 *
 *	- does not cross partition boundries on source
 *	- asks for confirmation on deletions unless -i0 is specified
 *	- refuses to replace a destination directory with a source file
 *	  unless -s0 is specified.
 *	- terminates on error
 *
 * Copying features:
 *
 *	- does not copy file if mtime, flags, perms, and size match unless
 *	  forced
 *
 *	- copies to temporary and renames-over the original, allowing
 *	  you to update live systems
 *
 *	- copies uid, gid, mtime, perms, flags, softlinks, devices, hardlinks,
 *	  and recurses through directories.
 *
 *	- accesses a per-directory exclusion file, .cpignore, containing 
 *	  standard wildcarded ( ? / * style, NOT regex) exclusions.
 *
 *	- tries to play permissions and flags smart in regards to overwriting 
 *	  schg files and doing related stuff.
 *
 *	- Can do MD5 consistancy checks
 *
 *	- Is able to do incremental mirroring/backups via hardlinks from
 *	  the 'previous' version (supplied with -H path).
 *
 * $DragonFly: src/bin/cpdup/cpdup.c,v 1.15 2006/08/18 01:13:51 dillon Exp $
 */

/*-
 * Example: cc -O cpdup.c -o cpdup -lmd
 *
 * ".MD5.CHECKSUMS" contains md5 checksumms for the current directory.
 * This file is stored on the source.
 */

#include "cpdup.h"
#include "hclink.h"
#include "hcproto.h"

#define HSIZE	16384
#define HMASK	(HSIZE-1)
#define HLSIZE	8192
#define HLMASK	(HLSIZE - 1)

#ifndef _ST_FLAGS_PRESENT_
#define st_flags	st_mode
#endif

typedef struct Node {
    struct Node *no_Next;
    struct Node *no_HNext;
    int  no_Value;
    char no_Name[4];
} Node;

typedef struct List {
    Node	li_Node;
    Node	*li_Hash[HSIZE];
} List;

struct hlink {
    ino_t ino;
    ino_t dino;
    struct hlink *next;
    struct hlink *prev;
    nlink_t nlinked;
    char name[0];
};

struct hlink *hltable[HLSIZE];

void RemoveRecur(const char *dpath, dev_t devNo);
void InitList(List *list);
void ResetList(List *list);
int AddList(List *list, const char *name, int n);
static struct hlink *hltlookup(struct stat *);
static struct hlink *hltadd(struct stat *, const char *);
static char *checkHLPath(struct stat *st, const char *spath, const char *dpath);
static int shash(const char *s);
static void hltdelete(struct hlink *);
int YesNo(const char *path);
static int xrename(const char *src, const char *dst, u_long flags);
static int xlink(const char *src, const char *dst, u_long flags);
int WildCmp(const char *s1, const char *s2);
int DoCopy(const char *spath, const char *dpath, dev_t sdevNo, dev_t ddevNo);

int AskConfirmation = 1;
int SafetyOpt = 1;
int ForceOpt;
int VerboseOpt;
int QuietOpt;
int NoRemoveOpt;
int UseMD5Opt;
int UseFSMIDOpt;
int SummaryOpt;
int SlaveOpt;
int EnableDirectoryRetries;
int DstBaseLen;
char IOBuf1[65536];
char IOBuf2[65536];
const char *UseCpFile;
const char *UseHLPath;
const char *MD5CacheFile;
const char *FSMIDCacheFile;

int64_t CountSourceBytes;
int64_t CountSourceItems;
int64_t CountCopiedItems;
int64_t CountReadBytes;
int64_t CountWriteBytes;
int64_t CountRemovedItems;

struct HostConf SrcHost;
struct HostConf DstHost;

int
main(int ac, char **av)
{
    int i;
    char *src = NULL;
    char *dst = NULL;
    char *ptr;
    struct timeval start;

    signal(SIGPIPE, SIG_IGN);

    gettimeofday(&start, NULL);
    for (i = 1; i < ac; ++i) {
	int v = 1;

	ptr = av[i];
	if (*ptr != '-') { 
	    if (src == NULL) {
		src = ptr;
	    } else if (dst == NULL) {
		dst = ptr;
	    } else {
		fatal("too many arguments");
		/* not reached */
	    }
	    continue;
	}
	ptr += 2;

	if (*ptr)
	    v = strtol(ptr, NULL, 0);

	switch(ptr[-1]) {
	case 'v':
	    VerboseOpt = 1;
	    while (*ptr == 'v') {
		++VerboseOpt;
		++ptr;
	    }
	    if (*ptr >= '0' && *ptr <= '9')
		VerboseOpt = strtol(ptr, NULL, 0);
	    break;
	case 'I':
	    SummaryOpt = v;
	    break;
	case 'o':
	    NoRemoveOpt = v;
	    break;
	case 'x':
	    UseCpFile = ".cpignore";
	    break;
	case 'X':
	    UseCpFile = (*ptr) ? ptr : av[++i];
	    break;
	case 'H':
	    UseHLPath = (*ptr) ? ptr : av[++i];
	    break;
	case 'S':
	    SlaveOpt = v;
	    break;
	case 'f':
	    ForceOpt = v;
	    break;
	case 'i':
	    AskConfirmation = v;
	    break;
	case 's':
	    SafetyOpt = v;
	    break;
	case 'q':
	    QuietOpt = v;
	    break;
	case 'k':
	    UseFSMIDOpt = v;
	    FSMIDCacheFile = ".FSMID.CHECK";
	    break;
	case 'K':
	    UseFSMIDOpt = v;
	    FSMIDCacheFile = av[++i];
	    break;
	case 'M':
	    UseMD5Opt = v;
	    MD5CacheFile = av[++i];
	    break;
	case 'm':
	    UseMD5Opt = v;
	    MD5CacheFile = ".MD5.CHECKSUMS";
	    break;
	case 'u':
	    setvbuf(stdout, NULL, _IOLBF, 0);
	    break;
	default:
	    fatal("illegal option: %s\n", ptr - 2);
	    /* not reached */
	    break;
	}
    }

    /*
     * If we are told to go into slave mode, run the HC protocol
     */
    if (SlaveOpt) {
	hc_slave(0, 1);
	exit(0);
    }

    /*
     * Extract the source and/or/neither target [user@]host and
     * make any required connections.
     */
    if (src && (ptr = strchr(src, ':')) != NULL) {
	asprintf(&SrcHost.host, "%*.*s", ptr - src, ptr - src, src);
	src = ptr + 1;
	if (UseCpFile) {
	    fprintf(stderr, "The cpignore options are not currently supported for remote sources\n");
	    exit(1);
	}
	if (UseMD5Opt) {
	    fprintf(stderr, "The MD5 options are not currently supported for remote sources\n");
	    exit(1);
	}
	if (hc_connect(&SrcHost) < 0)
	    fprintf(stderr, "Unable to connect to %s\n", SrcHost.host);
    }
    if (dst && (ptr = strchr(dst, ':')) != NULL) {
	asprintf(&DstHost.host, "%*.*s", ptr - dst, ptr - dst, dst);
	dst = ptr + 1;
	if (UseFSMIDOpt) {
	    fprintf(stderr, "The FSMID options are not currently supported for remote targets\n");
	    exit(1);
	}
	if (hc_connect(&DstHost) < 0)
	    fprintf(stderr, "Unable to connect to %s\n", DstHost.host);
    }

    /*
     * dst may be NULL only if -m option is specified,
     * which forces an update of the MD5 checksums
     */
    if (dst == NULL && UseMD5Opt == 0) {
	fatal(NULL);
	/* not reached */
    }
    if (dst) {
	DstBaseLen = strlen(dst);
	i = DoCopy(src, dst, (dev_t)-1, (dev_t)-1);
    } else {
	i = DoCopy(src, NULL, (dev_t)-1, (dev_t)-1);
    }
#ifndef NOMD5
    md5_flush();
#endif
    fsmid_flush();

    if (SummaryOpt && i == 0) {
	long duration;
	struct timeval end;

	gettimeofday(&end, NULL);
	CountSourceBytes += sizeof(struct stat) * CountSourceItems;
	CountReadBytes += sizeof(struct stat) * CountSourceItems;
	CountWriteBytes +=  sizeof(struct stat) * CountCopiedItems;
	CountWriteBytes +=  sizeof(struct stat) * CountRemovedItems;

	duration = end.tv_sec - start.tv_sec;
	duration *= 1000000;
	duration += end.tv_usec - start.tv_usec;
	if (duration == 0) duration = 1;
	logstd("cpdup completed successfully\n");
	logstd("%lld bytes source %lld bytes read %lld bytes written (%.1fX speedup)\n",
	    (long long)CountSourceBytes,
	    (long long)CountReadBytes,
	    (long long)CountWriteBytes,
	    ((double)CountSourceBytes * 2.0) / ((double)(CountReadBytes + CountWriteBytes)));
 	logstd("%lld source items %lld items copied %lld things deleted\n",
	    (long long)CountSourceItems,
	    (long long)CountCopiedItems,
	    (long long)CountRemovedItems);
	logstd("%.1f seconds %5d Kbytes/sec synced %5d Kbytes/sec scanned\n",
	    (float)duration / (float)1000000,
	    (long)((long)1000000 * (CountReadBytes + CountWriteBytes) / duration  / 1024.0),
	    (long)((long)1000000 * CountSourceBytes / duration / 1024.0));
    }
    exit((i == 0) ? 0 : 1);
}

static struct hlink *
hltlookup(struct stat *stp)
{
    struct hlink *hl;
    int n;

    n = stp->st_ino & HLMASK;

    for (hl = hltable[n]; hl; hl = hl->next)
        if (hl->ino == stp->st_ino)
              return hl;

    return NULL;
}

static struct hlink *
hltadd(struct stat *stp, const char *path)
{
    struct hlink *new;
    int plen = strlen(path);
    int n;

    new = malloc(offsetof(struct hlink, name[plen + 1]));
    if (new == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(EXIT_FAILURE);
    }

    /* initialize and link the new element into the table */
    new->ino = stp->st_ino;
    new->dino = 0;
    bcopy(path, new->name, plen + 1);
    new->nlinked = 1;
    new->prev = NULL;
    n = stp->st_ino & HLMASK;
    new->next = hltable[n];
    if (hltable[n])
        hltable[n]->prev = new;
    hltable[n] = new;

    return new;
}

static void
hltdelete(struct hlink *hl)
{
    if (hl->prev) {
        if (hl->next)
            hl->next->prev = hl->prev;
        hl->prev->next = hl->next;
    } else {
        if (hl->next)
            hl->next->prev = NULL;

        hltable[hl->ino & HLMASK] = hl->next;
    }

    free(hl);
}

/*
 * If UseHLPath is defined check to see if the file in question is
 * the same as the source file, and if it is return a pointer to the
 * -H path based file for hardlinking.  Else return NULL.
 */
static char *
checkHLPath(struct stat *st1, const char *spath, const char *dpath)
{
    struct stat sthl;
    char *hpath;
    int fd1;
    int fd2;
    int good;

    asprintf(&hpath, "%s%s", UseHLPath, dpath + DstBaseLen);

    /*
     * stat info matches ?
     */
    if (hc_stat(&DstHost, hpath, &sthl) < 0 ||
	st1->st_size != sthl.st_size ||
	st1->st_uid != sthl.st_uid ||
	st1->st_gid != sthl.st_gid ||
	st1->st_mtime != sthl.st_mtime
    ) {
	free(hpath);
	return(NULL);
    }

    /*
     * If ForceOpt is set we have to compare the files
     */
    if (ForceOpt) {
	fd1 = hc_open(&SrcHost, spath, O_RDONLY, 0);
	fd2 = hc_open(&DstHost, hpath, O_RDONLY, 0);
	good = 0;

	if (fd1 >= 0 && fd2 >= 0) {
	    int n;

	    while ((n = hc_read(&SrcHost, fd1, IOBuf1, sizeof(IOBuf1))) > 0) {
		if (hc_read(&DstHost, fd2, IOBuf2, sizeof(IOBuf2)) != n)
		    break;
		if (bcmp(IOBuf1, IOBuf2, n) != 0)
		    break;
	    }
	    if (n == 0)
		good = 1;
	}
	if (fd1 >= 0)
	    hc_close(&SrcHost, fd1);
	if (fd2 >= 0)
	    hc_close(&DstHost, fd2);
	if (good == 0) {
	    free(hpath);
	    hpath = NULL;
	}
    }
    return(hpath);
}

int
DoCopy(const char *spath, const char *dpath, dev_t sdevNo, dev_t ddevNo)
{
    struct stat st1;
    struct stat st2;
    int r, mres, fres, st2Valid;
    struct hlink *hln;
    List list;
    u_int64_t size;

    InitList(&list);
    r = mres = fres = st2Valid = 0;
    size = 0;
    hln = NULL;

    if (hc_lstat(&SrcHost, spath, &st1) != 0)
	return(0);
    st2.st_mode = 0;	/* in case lstat fails */
    st2.st_flags = 0;	/* in case lstat fails */
    if (dpath && hc_lstat(&DstHost, dpath, &st2) == 0)
	st2Valid = 1;

    if (S_ISREG(st1.st_mode)) {
	size = st1.st_blocks * 512;
	if (st1.st_size % 512) 
	    size += st1.st_size % 512 - 512;
    }

    /*
     * Handle hardlinks
     */

    if (S_ISREG(st1.st_mode) && st1.st_nlink > 1 && dpath) {
        if ((hln = hltlookup(&st1)) != NULL) {
            hln->nlinked++;

            if (st2Valid) {
                if (st2.st_ino == hln->dino) {
		    /*
		     * hard link is already correct, nothing to do
		     */
		    if (VerboseOpt >= 3)
			logstd("%-32s nochange\n", (dpath) ? dpath : spath);
                    if (hln->nlinked == st1.st_nlink)
                        hltdelete(hln);
		    CountSourceItems++;
                    return 0;
                } else {
		    /*
		     * hard link is not correct, attempt to unlink it
		     */
                    if (hc_remove(&DstHost, dpath) < 0) {
			logerr("%-32s hardlink: unable to unlink: %s\n", 
			    ((dpath) ? dpath : spath), strerror(errno));
                        hltdelete(hln);
			return (r + 1);
		    }
                }
            }

            if (xlink(hln->name, dpath, st1.st_flags) < 0) {
		int tryrelink = (errno == EMLINK);
		logerr("%-32s hardlink: unable to link to %s: %s\n",
		    (dpath ? dpath : spath), hln->name, strerror(errno)
		);
                hltdelete(hln);
                hln = NULL;
		if (tryrelink) {
		    logerr("%-20s hardlink: will attempt to copy normally\n");
		    goto relink;
		}
		++r;
            } else {
                if (hln->nlinked == st1.st_nlink) {
                    hltdelete(hln);
		    hln = NULL;
		}
                if (r == 0) {
		    if (VerboseOpt) {
			logstd("%-32s hardlink: %s\n", 
			    (dpath ? dpath : spath),
			    (st2Valid ? "relinked" : "linked")
			);
		    }
		    CountSourceItems++;
		    CountCopiedItems++;
                    return 0;
		}
            }
        } else {
	    /*
	     * first instance of hardlink must be copied normally
	     */
relink:
            hln = hltadd(&st1, dpath);
	}
    }

    /*
     * Do we need to copy the file/dir/link/whatever?  Early termination
     * if we do not.  Always redo links.  Directories are always traversed
     * except when the FSMID options are used.
     *
     * NOTE: st2Valid is true only if dpath != NULL *and* dpath stats good.
     */

    if (
	st2Valid
	&& st1.st_mode == st2.st_mode
#ifdef _ST_FLAGS_PRESENT_
	&& st1.st_flags == st2.st_flags
#endif
    ) {
	if (S_ISLNK(st1.st_mode) || S_ISDIR(st1.st_mode)) {
	    /*
	     * If FSMID tracking is turned on we can avoid recursing through
	     * an entire directory subtree if the FSMID matches.
	     */
#ifdef _ST_FSMID_PRESENT_
	    if (ForceOpt == 0 &&
		(UseFSMIDOpt && (fres = fsmid_check(st1.st_fsmid, dpath)) == 0)
	    ) {
		if (VerboseOpt >= 3) {
		    if (UseFSMIDOpt)
			logstd("%-32s fsmid-nochange\n", (dpath ? dpath : spath));
		    else
			logstd("%-32s nochange\n", (dpath ? dpath : spath));
		}
		return(0);
	    }
#endif
	} else {
	    if (ForceOpt == 0 &&
		st1.st_size == st2.st_size &&
		st1.st_uid == st2.st_uid &&
		st1.st_gid == st2.st_gid &&
		st1.st_mtime == st2.st_mtime
#ifndef NOMD5
		&& (UseMD5Opt == 0 || (mres = md5_check(spath, dpath)) == 0)
#endif
#ifdef _ST_FSMID_PRESENT_
		&& (UseFSMIDOpt == 0 || (fres = fsmid_check(st1.st_fsmid, dpath)) == 0)
#endif
	    ) {
                if (hln)
                    hln->dino = st2.st_ino;
		if (VerboseOpt >= 3) {
#ifndef NOMD5
		    if (UseMD5Opt)
			logstd("%-32s md5-nochange\n", (dpath ? dpath : spath));
		    else
#endif
		    if (UseFSMIDOpt)
			logstd("%-32s fsmid-nochange\n", (dpath ? dpath : spath));
		    else
			logstd("%-32s nochange\n", (dpath ? dpath : spath));
		}
		CountSourceBytes += size;
		CountSourceItems++;

		return(0);
	    }
	}
    }
    if (st2Valid && !S_ISDIR(st1.st_mode) && S_ISDIR(st2.st_mode)) {
	if (SafetyOpt) {
	    logerr("%-32s SAFETY - refusing to copy file over directory\n",
		(dpath ? dpath : spath)
	    );
	    ++r;		/* XXX */
	    return(0);	/* continue with the cpdup anyway */
	}
	if (QuietOpt == 0 || AskConfirmation) {
	    logstd("%-32s WARNING: non-directory source will blow away\n"
		   "%-32s preexisting dest directory, continuing anyway!\n",
		   ((dpath) ? dpath : spath), "");
	}
	if (dpath)
	    RemoveRecur(dpath, ddevNo);
    }

    /*
     * The various comparisons failed, copy it.
     */
    if (S_ISDIR(st1.st_mode)) {
	DIR *dir;

	if (fres < 0)
	    logerr("%-32s/ fsmid-CHECK-FAILED\n", (dpath) ? dpath : spath);
	if ((dir = hc_opendir(&SrcHost, spath)) != NULL) {
	    struct dirent *den;
	    int noLoop = 0;

	    if (dpath) {
		if (S_ISDIR(st2.st_mode) == 0) {
		    hc_remove(&DstHost, dpath);
		    if (hc_mkdir(&DstHost, dpath, st1.st_mode | 0700) != 0) {
			logerr("%s: mkdir failed: %s\n", 
			    (dpath ? dpath : spath), strerror(errno));
			r = 1;
			noLoop = 1;
		    }
		    /*
		     * Matt: why don't you check error codes here?
		     */
		    hc_lstat(&DstHost, dpath, &st2);
		    hc_chown(&DstHost, dpath, st1.st_uid, st1.st_gid);
		    CountCopiedItems++;
		} else {
		    /*
		     * Directory must be scanable by root for cpdup to
		     * work.  We'll fix it later if the directory isn't
		     * supposed to be readable ( which is why we fixup
		     * st2.st_mode to match what we did ).
		     */
		    if ((st2.st_mode & 0700) != 0700) {
			hc_chmod(&DstHost, dpath, st2.st_mode | 0700);
			st2.st_mode |= 0700;
		    }
		    if (VerboseOpt >= 2)
			logstd("%s\n", dpath ? dpath : spath);
		}
	    }

	    if ((int)sdevNo >= 0 && st1.st_dev != sdevNo) {
		noLoop = 1;
	    } else {
		sdevNo = st1.st_dev;
	    }

	    if ((int)ddevNo >= 0 && st2.st_dev != ddevNo) {
		noLoop = 1;
	    } else {
		ddevNo = st2.st_dev;
	    }

	    /*
	     * scan .cpignore file for files/directories 
	     * to ignore.
	     */

	    if (UseCpFile) {
		FILE *fi;
		char buf[8192];
		char *fpath;

		if (UseCpFile[0] == '/') {
		    fpath = mprintf("%s", UseCpFile);
		} else {
		    fpath = mprintf("%s/%s", spath, UseCpFile);
		}
		AddList(&list, strrchr(fpath, '/') + 1, 1);
		if ((fi = fopen(fpath, "r")) != NULL) {
		    while (fgets(buf, sizeof(buf), fi) != NULL) {
			int l = strlen(buf);
			CountReadBytes += l;
			if (l && buf[l-1] == '\n')
			    buf[--l] = 0;
			if (buf[0] && buf[0] != '#')
			    AddList(&list, buf, 1);
		    }
		    fclose(fi);
		}
		free(fpath);
	    }

	    /*
	     * Automatically exclude MD5CacheFile that we create on the
	     * source from the copy to the destination.
	     *
	     * Automatically exclude a FSMIDCacheFile on the source that
	     * would otherwise overwrite the one we maintain on the target.
	     */
	    if (UseMD5Opt)
		AddList(&list, MD5CacheFile, 1);
	    if (UseFSMIDOpt)
		AddList(&list, FSMIDCacheFile, 1);

	    while (noLoop == 0 && (den = hc_readdir(&SrcHost, dir)) != NULL) {
		/*
		 * ignore . and ..
		 */
		char *nspath;
		char *ndpath = NULL;

		if (strcmp(den->d_name, ".") == 0 ||
		    strcmp(den->d_name, "..") == 0
		) {
		    continue;
		}
		/*
		 * ignore if on .cpignore list
		 */
		if (AddList(&list, den->d_name, 0) == 1) {
		    continue;
		}
		nspath = mprintf("%s/%s", spath, den->d_name);
		if (dpath)
		    ndpath = mprintf("%s/%s", dpath, den->d_name);
		r += DoCopy(
		    nspath,
		    ndpath,
		    sdevNo,
		    ddevNo
		);
		free(nspath);
		if (ndpath)
		    free(ndpath);
	    }

	    hc_closedir(&SrcHost, dir);

	    /*
	     * Remove files/directories from destination that do not appear
	     * in the source.
	     */
	    if (dpath && (dir = hc_opendir(&DstHost, dpath)) != NULL) {
		while (noLoop == 0 && (den = hc_readdir(&DstHost, dir)) != NULL) {
		    /*
		     * ignore . or ..
		     */
		    if (strcmp(den->d_name, ".") == 0 ||
			strcmp(den->d_name, "..") == 0
		    ) {
			continue;
		    }
		    /*
		     * If object does not exist in source or .cpignore
		     * then recursively remove it.
		     */
		    if (AddList(&list, den->d_name, 3) == 3) {
			char *ndpath;

			ndpath = mprintf("%s/%s", dpath, den->d_name);
			RemoveRecur(ndpath, ddevNo);
			free(ndpath);
		    }
		}
		hc_closedir(&DstHost, dir);
	    }

	    if (dpath) {
		if (ForceOpt ||
		    st2Valid == 0 || 
		    st1.st_uid != st2.st_uid ||
		    st1.st_gid != st2.st_gid
		) {
		    hc_chown(&DstHost, dpath, st1.st_uid, st1.st_gid);
		}
		if (st2Valid == 0 || st1.st_mode != st2.st_mode) {
		    hc_chmod(&DstHost, dpath, st1.st_mode);
		}
#ifdef _ST_FLAGS_PRESENT_
		if (st2Valid == 0 || st1.st_flags != st2.st_flags) {
		    hc_chflags(&DstHost, dpath, st1.st_flags);
		}
#endif
	    }
	}
    } else if (dpath == NULL) {
	/*
	 * If dpath is NULL, we are just updating the MD5
	 */
#ifndef NOMD5
	if (UseMD5Opt && S_ISREG(st1.st_mode)) {
	    mres = md5_check(spath, NULL);

	    if (VerboseOpt > 1) {
		if (mres < 0)
		    logstd("%-32s md5-update\n", (dpath) ? dpath : spath);
		else
		    logstd("%-32s md5-ok\n", (dpath) ? dpath : spath);
	    } else if (!QuietOpt && mres < 0) {
		logstd("%-32s md5-update\n", (dpath) ? dpath : spath);
	    }
	}
#endif
    } else if (S_ISREG(st1.st_mode)) {
	char *path;
	char *hpath;
	int fd1;
	int fd2;

	path = mprintf("%s.tmp", dpath);

	/*
	 * Handle check failure message.
	 */
#ifndef NOMD5
	if (mres < 0)
	    logerr("%-32s md5-CHECK-FAILED\n", (dpath) ? dpath : spath);
	else 
#endif
	if (fres < 0)
	    logerr("%-32s fsmid-CHECK-FAILED\n", (dpath) ? dpath : spath);

	/*
	 * Not quite ready to do the copy yet.  If UseHLPath is defined,
	 * see if we can hardlink instead.
	 */

	if (UseHLPath && (hpath = checkHLPath(&st1, spath, dpath)) != NULL) {
		if (hc_link(&DstHost, hpath, dpath) == 0) {
			if (VerboseOpt) {
			    logstd("%-32s hardlinked(-H)\n",
				   (dpath ? dpath : spath));
			}
			free(hpath);
			goto skip_copy;
		}
		/*
		 * Shucks, we may have hit a filesystem hard linking limit,
		 * we have to copy instead.
		 */
		free(hpath);
	}

	if ((fd1 = hc_open(&SrcHost, spath, O_RDONLY, 0)) >= 0) {
	    if ((fd2 = hc_open(&DstHost, path, O_WRONLY|O_CREAT|O_EXCL, 0600)) < 0) {
		/*
		 * There could be a .tmp file from a previously interrupted
		 * run, delete and retry.  Fail if we still can't get at it.
		 */
#ifdef _ST_FLAGS_PRESENT_
		hc_chflags(&DstHost, path, 0);
#endif
		hc_remove(&DstHost, path);
		fd2 = hc_open(&DstHost, path, O_WRONLY|O_CREAT|O_EXCL|O_TRUNC, 0600);
	    }
	    if (fd2 >= 0) {
		const char *op;
		int n;

		/*
		 * Matt: What about holes?
		 */
		op = "read";
		while ((n = hc_read(&SrcHost, fd1, IOBuf1, sizeof(IOBuf1))) > 0) {
		    op = "write";
		    if (hc_write(&DstHost, fd2, IOBuf1, n) != n)
			break;
		    op = "read";
		}
		hc_close(&DstHost, fd2);
		if (n == 0) {
		    struct timeval tv[2];

		    bzero(tv, sizeof(tv));
		    tv[0].tv_sec = st1.st_mtime;
		    tv[1].tv_sec = st1.st_mtime;

		    hc_utimes(&DstHost, path, tv);
		    hc_chown(&DstHost, path, st1.st_uid, st1.st_gid);
		    hc_chmod(&DstHost, path, st1.st_mode);
		    if (xrename(path, dpath, st2.st_flags) != 0) {
			logerr("%-32s rename-after-copy failed: %s\n",
			    (dpath ? dpath : spath), strerror(errno)
			);
			++r;
		    } else {
			if (VerboseOpt)
			    logstd("%-32s copy-ok\n", (dpath ? dpath : spath));
#ifdef _ST_FLAGS_PRESENT_
			if (st1.st_flags)
			    hc_chflags(&DstHost, dpath, st1.st_flags);
#endif
		    }
		    CountReadBytes += size;
		    CountWriteBytes += size;
		    CountSourceBytes += size;
		    CountSourceItems++;
		    CountCopiedItems++;
		} else {
		    logerr("%-32s %s failed: %s\n",
			(dpath ? dpath : spath), op, strerror(errno)
		    );
		    hc_remove(&DstHost, path);
		    ++r;
		}
	    } else {
		logerr("%-32s create (uid %d, euid %d) failed: %s\n",
		    (dpath ? dpath : spath), getuid(), geteuid(),
		    strerror(errno)
		);
		++r;
	    }
	    hc_close(&SrcHost, fd1);
	} else {
	    logerr("%-32s copy: open failed: %s\n",
		(dpath ? dpath : spath),
		strerror(errno)
	    );
	    ++r;
	}
skip_copy:
	free(path);

        if (hln) {
            if (!r && hc_stat(&DstHost, dpath, &st2) == 0)
                hln->dino = st2.st_ino;
            else
                hltdelete(hln);
        }
    } else if (S_ISLNK(st1.st_mode)) {
	char link1[1024];
	char link2[1024];
	char path[2048];
	int n1;
	int n2;

	snprintf(path, sizeof(path), "%s.tmp", dpath);
	n1 = hc_readlink(&SrcHost, spath, link1, sizeof(link1) - 1);
	n2 = hc_readlink(&DstHost, dpath, link2, sizeof(link2) - 1);
	if (n1 >= 0) {
	    if (ForceOpt || n1 != n2 || bcmp(link1, link2, n1) != 0) {
		hc_umask(&DstHost, ~st1.st_mode);
		hc_remove(&DstHost, path);
		link1[n1] = 0;
		if (hc_symlink(&DstHost, link1, path) < 0) {
                      logerr("%-32s symlink (%s->%s) failed: %s\n",
			  (dpath ? dpath : spath), link1, path,
			  strerror(errno)
		      );
		      ++r;
		} else {
		    hc_lchown(&DstHost, path, st1.st_uid, st1.st_gid);
		    /*
		     * there is no lchmod() or lchflags(), we 
		     * cannot chmod or chflags a softlink.
		     */
		    if (xrename(path, dpath, st2.st_flags) != 0) {
			logerr("%-32s rename softlink (%s->%s) failed: %s\n",
			    (dpath ? dpath : spath),
			    path, dpath, strerror(errno));
		    } else if (VerboseOpt) {
			logstd("%-32s softlink-ok\n", (dpath ? dpath : spath));
		    }
		    hc_umask(&DstHost, 000);
		    CountWriteBytes += n1;
		    CountCopiedItems++;
	  	}
	    } else {
		if (VerboseOpt >= 3)
		    logstd("%-32s nochange\n", (dpath ? dpath : spath));
	    }
	    CountSourceBytes += n1;
	    CountReadBytes += n1;
	    if (n2 > 0) CountReadBytes += n2;
	    CountSourceItems++;
	} else {
	    r = 1;
	    logerr("%-32s softlink-failed\n", (dpath ? dpath : spath));
	}
    } else if (S_ISCHR(st1.st_mode) || S_ISBLK(st1.st_mode)) {
	char path[2048];

	if (ForceOpt ||
	    st2Valid == 0 || 
	    st1.st_mode != st2.st_mode || 
	    st1.st_rdev != st2.st_rdev ||
	    st1.st_uid != st2.st_uid ||
	    st1.st_gid != st2.st_gid
	) {
	    snprintf(path, sizeof(path), "%s.tmp", dpath);

	    hc_remove(&DstHost, path);
	    if (mknod(path, st1.st_mode, st1.st_rdev) == 0) {
		hc_chmod(&DstHost, path, st1.st_mode);
		hc_chown(&DstHost, path, st1.st_uid, st1.st_gid);
		hc_remove(&DstHost, dpath);
		if (xrename(path, dpath, st2.st_flags) != 0) {
		    logerr("%-32s dev-rename-after-create failed: %s\n",
			(dpath ? dpath : spath),
			strerror(errno)
		    );
		} else if (VerboseOpt) {
		    logstd("%-32s dev-ok\n", (dpath ? dpath : spath));
		}
		CountCopiedItems++;
	    } else {
		r = 1;
		logerr("%-32s dev failed: %s\n", 
		    (dpath ? dpath : spath), strerror(errno)
		);
	    }
	} else {
	    if (VerboseOpt >= 3)
		logstd("%-32s nochange\n", (dpath ? dpath : spath));
	}
	CountSourceItems++;
    }
    ResetList(&list);
    return (r);
}

/*
 * RemoveRecur()
 */

void
RemoveRecur(const char *dpath, dev_t devNo)
{
    struct stat st;

    if (hc_lstat(&DstHost, dpath, &st) == 0) {
	if ((int)devNo < 0)
	    devNo = st.st_dev;
	if (st.st_dev == devNo) {
	    if (S_ISDIR(st.st_mode)) {
		DIR *dir;

		if ((dir = hc_opendir(&DstHost, dpath)) != NULL) {
		    struct dirent *den;
		    while ((den = hc_readdir(&DstHost, dir)) != NULL) {
			char *ndpath;

			if (strcmp(den->d_name, ".") == 0)
			    continue;
			if (strcmp(den->d_name, "..") == 0)
			    continue;
			ndpath = mprintf("%s/%s", dpath, den->d_name);
			RemoveRecur(ndpath, devNo);
			free(ndpath);
		    }
		    hc_closedir(&DstHost, dir);
		}
		if (AskConfirmation && NoRemoveOpt == 0) {
		    if (YesNo(dpath)) {
			if (hc_rmdir(&DstHost, dpath) < 0) {
			    logerr("%-32s rmdir failed: %s\n",
				dpath, strerror(errno)
			    );
			}
			CountRemovedItems++;
		    }
		} else {
		    if (NoRemoveOpt) {
			if (VerboseOpt)
			    logstd("%-32s not-removed\n", dpath);
		    } else if (hc_rmdir(&DstHost, dpath) == 0) {
			if (VerboseOpt)
			    logstd("%-32s rmdir-ok\n", dpath);
			CountRemovedItems++;
		    } else {
			logerr("%-32s rmdir failed: %s\n",
			    dpath, strerror(errno)
			);
		    }
		}
	    } else {
		if (AskConfirmation && NoRemoveOpt == 0) {
		    if (YesNo(dpath)) {
			if (hc_remove(&DstHost, dpath) < 0) {
			    logerr("%-32s remove failed: %s\n",
				dpath, strerror(errno)
			    );
			}
			CountRemovedItems++;
		    }
		} else {
		    if (NoRemoveOpt) {
			if (VerboseOpt)
			    logstd("%-32s not-removed\n", dpath);
		    } else if (hc_remove(&DstHost, dpath) == 0) {
			if (VerboseOpt)
			    logstd("%-32s remove-ok\n", dpath);
			CountRemovedItems++;
		    } else {
			logerr("%-32s remove failed: %s\n",
			    dpath, strerror(errno)
			);
		    }
		}
	    }
	}
    }
}

void
InitList(List *list)
{
    bzero(list, sizeof(List));
    list->li_Node.no_Next = &list->li_Node;
}

void 
ResetList(List *list)
{
    Node *node;

    while ((node = list->li_Node.no_Next) != &list->li_Node) {
	list->li_Node.no_Next = node->no_Next;
	free(node);
    }
    InitList(list);
}

int
AddList(List *list, const char *name, int n)
{
    Node *node;
    int hv;

    hv = shash(name);

    /*
     * Scan against wildcards.  Only a node value of 1 can be a wildcard
     * ( usually scanned from .cpignore )
     */

    for (node = list->li_Hash[0]; node; node = node->no_HNext) {
	if (strcmp(name, node->no_Name) == 0 ||
	    (n != 1 && node->no_Value == 1 && WildCmp(node->no_Name, name) == 0)
	) {
	    return(node->no_Value);
	}
    }

    /*
     * Look for exact match
     */

    for (node = list->li_Hash[hv]; node; node = node->no_HNext) {
	if (strcmp(name, node->no_Name) == 0) {
	    return(node->no_Value);
	}
    }
    node = malloc(sizeof(Node) + strlen(name) + 1);
    if (node == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(EXIT_FAILURE);
    }

    node->no_Next = list->li_Node.no_Next;
    list->li_Node.no_Next = node;

    node->no_HNext = list->li_Hash[hv];
    list->li_Hash[hv] = node;

    strcpy(node->no_Name, name);
    node->no_Value = n;

    return(n);
}

static int
shash(const char *s)
{
    int hv;

    hv = 0xA4FB3255;

    while (*s) {
	if (*s == '*' || *s == '?' || 
	    *s == '{' || *s == '}' || 
	    *s == '[' || *s == ']' ||
	    *s == '|'
	) {
	    return(0);
	}
	hv = (hv << 5) ^ *s ^ (hv >> 23);
	++s;
    }
    return(((hv >> 16) ^ hv) & HMASK);
}

/*
 * WildCmp() - compare wild string to sane string
 *
 *	Return 0 on success, -1 on failure.
 */

int
WildCmp(const char *w, const char *s)
{
    /*
     * skip fixed portion
     */
  
    for (;;) {
	switch(*w) {
	case '*':
	    if (w[1] == 0)	/* optimize wild* case */
		return(0);
	    {
		int i;
		int l = strlen(s);

		for (i = 0; i <= l; ++i) {
		    if (WildCmp(w + 1, s + i) == 0)
			return(0);
		}
	    }
	    return(-1);
	case '?':
	    if (*s == 0)
		return(-1);
	    ++w;
	    ++s;
	    break;
	default:
	    if (*w != *s)
		return(-1);
	    if (*w == 0)	/* terminator */
		return(0);
	    ++w;
	    ++s;
	    break;
	}
    }
    /* not reached */
    return(-1);
}

int
YesNo(const char *path)
{
    int ch, first;

    fprintf(stderr, "remove %s (Yes/No) [No]? ", path);
    fflush(stderr);

    first = ch = getchar();
    while (ch != '\n' && ch != EOF)
	ch = getchar();
    return ((first == 'y' || first == 'Y'));
}

/*
 * xrename() - rename with override
 *
 *	If the rename fails, attempt to override st_flags on the 
 *	destination and rename again.  If that fails too, try to
 *	set the flags back the way they were and give up.
 */

static int
xrename(const char *src, const char *dst, u_long flags)
{
    int r;

    r = 0;

    if ((r = hc_rename(&DstHost, src, dst)) < 0) {
#ifdef _ST_FLAGS_PRESENT_
	hc_chflags(&DstHost, dst, 0);
	if ((r = hc_rename(&DstHost, src, dst)) < 0)
		hc_chflags(&DstHost, dst, flags);
#endif
    }
    return(r);
}

static int
xlink(const char *src, const char *dst, u_long flags)
{
    int r;
#ifdef _ST_FLAGS_PRESENT_
    int e;
#endif

    r = 0;

    if ((r = hc_link(&DstHost, src, dst)) < 0) {
#ifdef _ST_FLAGS_PRESENT_
	hc_chflags(&DstHost, src, 0);
	r = hc_link(&DstHost, src, dst);
	e = errno;
	hc_chflags(&DstHost, src, flags);
	errno = e;
#endif
    }
    return(r);
}

