/*
 * Copyright (c) 1998 Michael Smith.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: src/lib/libstand/stand.h,v 1.18.2.8 2002/06/17 11:22:39 sobomax Exp $
 * From	$NetBSD: stand.h,v 1.22 1997/06/26 19:17:40 drochner Exp $	
 */

/*-
 * Copyright (c) 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)stand.h	8.1 (Berkeley) 6/11/93
 */

#include <machine/stdarg.h>	/* __va_list */
#include <sys/types.h>
#include <sys/cdefs.h>
#include <sys/stat.h>
#include <sys/dirent.h>
#include <string.h>

#define CHK(fmt, args...)	printf("%s(%d): " fmt "\n", __func__, __LINE__ , ##args)
#define PCHK(fmt, args...)	{printf("%s(%d): " fmt "\n", __func__, __LINE__ , ##args); getchar();}

/* Avoid unwanted userlandish components */
#define _KERNEL
#include <sys/errno.h>
#undef _KERNEL

/* special stand error codes */
#define	EADAPT	(ELAST+1)	/* bad adaptor */
#define	ECTLR	(ELAST+2)	/* bad controller */
#define	EUNIT	(ELAST+3)	/* bad unit */
#define ESLICE	(ELAST+4)	/* bad slice */
#define	EPART	(ELAST+5)	/* bad partition */
#define	ERDLAB	(ELAST+6)	/* can't read disk label */
#define	EUNLAB	(ELAST+7)	/* unlabeled disk */
#define	EOFFSET	(ELAST+8)	/* relative seek not supported */
#define	ESALAST	(ELAST+8)	/* */

struct open_file;

/*
 * This structure is used to define file system operations in a file system
 * independent way.
 *
 * XXX note that filesystem providers should export a pointer to their fs_ops
 *     struct, so that consumers can reference this and thus include the
 *     filesystems that they require.
 */
struct fs_ops {
    const char	*fs_name;
    int		(*fo_open)(const char *, struct open_file *);
    int		(*fo_close)(struct open_file *);
    int		(*fo_read)(struct open_file *, void *,
			   size_t, size_t *);
    int		(*fo_write)(struct open_file *, void *,
			    size_t, size_t *);
    off_t	(*fo_seek)(struct open_file *, off_t, int);
    int		(*fo_stat)(struct open_file *, struct stat *);
    int		(*fo_readdir)(struct open_file *, struct dirent *);
};

/*
 * libstand-supplied filesystems
 */
extern struct fs_ops ufs_fsops;
extern struct fs_ops hammer_fsops;
extern struct fs_ops tftp_fsops;
extern struct fs_ops nfs_fsops;
extern struct fs_ops cd9660_fsops;
extern struct fs_ops gzipfs_fsops;
extern struct fs_ops zipfs_fsops;
extern struct fs_ops bzipfs_fsops;
extern struct fs_ops dosfs_fsops;
extern struct fs_ops ext2fs_fsops;
extern struct fs_ops splitfs_fsops;

/* where values for lseek(2) */
#define	SEEK_SET	0	/* set file offset to offset */
#define	SEEK_CUR	1	/* set file offset to current plus offset */
#define	SEEK_END	2	/* set file offset to EOF plus offset */

/* 
 * Device switch
 */
struct devsw {
    const char	dv_name[8];
    int		dv_type;		/* opaque type constant, arch-dependant */
    int		(*dv_init)(void);	/* early probe call */
    int		(*dv_strategy)(void *, int, daddr_t, size_t, char *, size_t *);
    int		(*dv_open)(struct open_file *, ...);
    int		(*dv_close)(struct open_file *);
    int		(*dv_ioctl)(struct open_file *, u_long, void *);
    void	(*dv_print)(int);	/* print device information */
    void	(*dv_cleanup)(void);
};

/*
 * libstand-supplied device switch
 */
extern struct devsw netdev;

extern int errno;
extern int no_io_error;

struct open_file {
    int			f_flags;	/* see F_* below */
    struct devsw	*f_dev;		/* pointer to device operations */
    void		*f_devdata;	/* device specific data */
    struct fs_ops	*f_ops;		/* pointer to file system operations */
    void		*f_fsdata;	/* file system specific data */
    off_t		f_offset;	/* current file offset */
    char		*f_rabuf;	/* readahead buffer pointer */
    size_t		f_ralen;	/* valid data in readahead buffer */
    off_t		f_raoffset;	/* consumer offset in readahead buffer */
#define SOPEN_RASIZE	512
};

#define	SOPEN_MAX	8
extern struct open_file files[];

/* f_flags values */
#define	F_READ		0x0001	/* file opened for reading */
#define	F_WRITE		0x0002	/* file opened for writing */
#define	F_RAW		0x0004	/* raw device open - no file system */
#define F_DEVDESC	0x0008	/* generic devdesc, else specific */

#define isascii(c)	(((c) & ~0x7F) == 0)

static __inline int
isupper(int c)
{
    return c >= 'A' && c <= 'Z';
}

static __inline int
islower(int c)
{
    return c >= 'a' && c <= 'z';
}

static __inline int
isspace(int c)
{
    return c == ' ' || (c >= 0x9 && c <= 0xd);
}

static __inline int
isdigit(int c)
{
    return c >= '0' && c <= '9';
}

static __inline int
isxdigit(int c)
{
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static __inline int
isalpha(int c)
{
    return isupper(c) || islower(c);
}

static __inline int
isalnum(int c)
{
    return isalpha(c) || isdigit(c);
}

static __inline int
toupper(int c)
{
    return islower(c) ? c - 'a' + 'A' : c;
}

static __inline int
tolower(int c)
{
    return isupper(c) ? c - 'A' + 'a' : c;
}

/* sbrk emulation */
extern void	setheap(void *, void *);
extern char	*getheap(size_t *sizep);
extern char	*sbrk(intptr_t);

/* Matt Dillon's zalloc/zmalloc */
extern void	*malloc(size_t);
extern void	free(void *);
/*#define free(p)	{CHK("free %p", p); free(p);} */ /* use for catching guard violations */
extern void	*calloc(size_t, size_t);
extern void	*realloc(void *, size_t);
extern void	*reallocf(void *, size_t);
extern void	mallocstats(void);

extern int	printf(const char *, ...) __printflike(1, 2);
extern void	vprintf(const char *, __va_list) __printflike(1, 0);
extern int	sprintf(char *, const char *, ...) __printflike(2, 3);
extern void	vsprintf(char *, const char *, __va_list) __printflike(2, 0);
int		snprintf(char *, size_t, const char *, ...) __printflike(3, 4);
int		vsnprintf(char *, size_t, const char *, __va_list)
		    __printflike(3, 0);

extern void	twiddle(void);

extern void	ngets(char *, int);
#define gets(x)	ngets((x), 0)
extern char 	*fgets(char *, int, int);
extern int	fgetstr(char *, int, int);

extern int	open(const char *, int);
#define	O_RDONLY	0x0
#define O_WRONLY	0x1			/* writing not (yet?) supported */
#define O_RDWR		0x2
extern int	close(int);
extern void	closeall(void);
extern ssize_t	read(int, void *, size_t);
extern ssize_t	write(int, void *, size_t);
extern struct	dirent *readdirfd(int);

extern void	srandom(u_long);
extern u_long	random(void);

/* imports from stdlib, locally modified */
extern long	strtol(const char *, char **, int);
extern char	*optarg;			/* getopt(3) external variables */
extern int	optind, opterr, optopt, optreset;
extern int	getopt(int, char * const [], const char *);

/* ether.c */
extern char    *ether_sprintf(u_char *);

/* pager.c */
extern void	pager_open(void);
extern void	pager_close(void);
extern int	pager_output(const char *);
extern int	pager_file(const char *);

/* No signal state to preserve */
#define setjmp	_setjmp
#define longjmp	_longjmp

/* environment.c */
#define EV_DYNAMIC	(1<<0)		/* value was dynamically allocated, free if changed/unset */
#define EV_VOLATILE	(1<<1)		/* value is volatile, make a copy of it */
#define EV_NOHOOK	(1<<2)		/* don't call hook when setting */

struct env_var;
typedef char	*(ev_format_t)(struct env_var *);
typedef int	(ev_sethook_t)(struct env_var *, int, const void *);
typedef int	(ev_unsethook_t)(struct env_var *);

struct env_var
{
    char		*ev_name;
    int			ev_flags;
    void		*ev_value;
    ev_sethook_t	*ev_sethook;
    ev_unsethook_t	*ev_unsethook;
    struct env_var	*ev_next, *ev_prev;
};
extern struct env_var	*environ;

extern struct env_var	*env_getenv(const char *);
extern int		env_setenv(const char *, int, const void *, ev_sethook_t,
				   ev_unsethook_t);
extern char		*getenv(const char *);
extern int		setenv(const char *, const char *, int);
extern int		putenv(const char *);
extern int		unsetenv(const char *);

extern ev_sethook_t	env_noset;		/* refuse set operation */
extern ev_unsethook_t	env_nounset;		/* refuse unset operation */

/* BCD conversions (undocumented) */
extern u_char const	bcd2bin_data[];
extern u_char const	bin2bcd_data[];
extern char const	hex2ascii_data[];

#define	bcd2bin(bcd)	(bcd2bin_data[bcd])
#define	bin2bcd(bin)	(bin2bcd_data[bin])
#define	hex2ascii(hex)	(hex2ascii_data[hex])

/* min/max (undocumented) */
static __inline int imax(int a, int b) { return (a > b ? a : b); }
static __inline int imin(int a, int b) { return (a < b ? a : b); }
static __inline long lmax(long a, long b) { return (a > b ? a : b); }
static __inline long lmin(long a, long b) { return (a < b ? a : b); }
static __inline u_int max(u_int a, u_int b) { return (a > b ? a : b); }
static __inline u_int min(u_int a, u_int b) { return (a < b ? a : b); }
static __inline quad_t qmax(quad_t a, quad_t b) { return (a > b ? a : b); }
static __inline quad_t qmin(quad_t a, quad_t b) { return (a < b ? a : b); }
static __inline u_long ulmax(u_long a, u_long b) { return (a > b ? a : b); }
static __inline u_long ulmin(u_long a, u_long b) { return (a < b ? a : b); }
static __inline size_t szmax(size_t a, size_t b) { return (a > b ? a : b); }
static __inline size_t szmin(size_t a, size_t b) { return (a < b ? a : b); }

/* swaps (undocumented, useful?) */
#ifdef __i386__
extern u_int32_t	bswap32(u_int32_t);
extern u_int64_t	bswap64(u_int64_t);
#endif

/* null functions for device/filesystem switches (undocumented) */
extern int	nodev(void);
extern int	noioctl(struct open_file *, u_long, void *);
extern void	nullsys(void);

extern int	null_open(const char *, struct open_file *);
extern int	null_close(struct open_file *);
extern int	null_read(struct open_file *, void *, size_t, size_t *);
extern int	null_write(struct open_file *, void *, size_t, size_t *);
extern off_t	null_seek(struct open_file *, off_t, int);
extern int	null_stat(struct open_file *, struct stat *);
extern int	null_readdir(struct open_file *, struct dirent *);


/* 
 * Machine dependent functions and data, must be provided or stubbed by 
 * the consumer 
 */
extern int		getchar(void);
extern int		ischar(void);
extern void		putchar(int);
extern int		devopen(struct open_file *, const char *, const char **);
extern int		devclose(struct open_file *);
extern void		devreplace(struct open_file *, void *devdata);
extern void		panic(const char *, ...) __dead2 __printflike(1, 2);
extern struct fs_ops	*file_system[];
extern struct devsw	*devsw[];

#if 0

static inline void *
malloc_debug(size_t size, const char *file, int line)
{
    void *p;
    printf("%s:%d malloc(%ld)", file, line, size);
    p = malloc(size);
    printf("=%p\n", p);
    return p;
}

static inline void
free_debug(void *p, const char *file, int line)
{
    printf("%s:%d free(%p)\n", file, line, p);
    free(p);
}

#define malloc(x)	malloc_debug(x, __FILE__, __LINE__)
#define free(x)		free_debug(x, __FILE__, __LINE__)

#endif
