/*-
 * Copyright (c) 1986, 1988, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
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
 *	@(#)kern_shutdown.c	8.3 (Berkeley) 1/21/94
 * $FreeBSD: src/sys/kern/kern_shutdown.c,v 1.72.2.12 2002/02/21 19:15:10 dillon Exp $
 * $DragonFly: src/sys/kern/kern_shutdown.c,v 1.62 2008/01/05 13:23:48 corecode Exp $
 */

#include "opt_ddb.h"
#include "opt_ddb_trace.h"
#include "opt_hw_wdog.h"
#include "opt_panic.h"
#include "opt_show_busybufs.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/eventhandler.h>
#include <sys/buf.h>
#include <sys/diskslice.h>
#include <sys/reboot.h>
#include <sys/proc.h>
#include <sys/priv.h>
#include <sys/fcntl.h>		/* FREAD	*/
#include <sys/stat.h>		/* S_IFCHR	*/
#include <sys/vnode.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/queue.h>
#include <sys/sysctl.h>
#include <sys/vkernel.h>
#include <sys/conf.h>
#include <sys/sysproto.h>
#include <sys/device.h>
#include <sys/cons.h>
#include <sys/shm.h>
#include <sys/kern_syscall.h>
#include <vm/vm_map.h>
#include <vm/pmap.h>

#include <sys/thread2.h>
#include <sys/buf2.h>

#include <machine/pcb.h>
#include <machine/clock.h>
#include <machine/md_var.h>
#include <machine/smp.h>		/* smp_active_mask, cpuid */
#include <machine/vmparam.h>

#include <sys/signalvar.h>

#ifndef PANIC_REBOOT_WAIT_TIME
#define PANIC_REBOOT_WAIT_TIME 15 /* default to 15 seconds */
#endif

/*
 * Note that stdarg.h and the ANSI style va_start macro is used for both
 * ANSI and traditional C compilers.  We use the machine version to stay
 * within the confines of the kernel header files.
 */
#include <machine/stdarg.h>

#ifdef DDB
#include <ddb/ddb.h>
#ifdef DDB_UNATTENDED
int debugger_on_panic = 0;
#else
int debugger_on_panic = 1;
#endif
SYSCTL_INT(_debug, OID_AUTO, debugger_on_panic, CTLFLAG_RW,
	&debugger_on_panic, 0, "Run debugger on kernel panic");

#ifdef DDB_TRACE
int trace_on_panic = 1;
#else
int trace_on_panic = 0;
#endif
SYSCTL_INT(_debug, OID_AUTO, trace_on_panic, CTLFLAG_RW,
	&trace_on_panic, 0, "Print stack trace on kernel panic");
#endif

static int sync_on_panic = 1;
SYSCTL_INT(_kern, OID_AUTO, sync_on_panic, CTLFLAG_RW,
	&sync_on_panic, 0, "Do a sync before rebooting from a panic");

SYSCTL_NODE(_kern, OID_AUTO, shutdown, CTLFLAG_RW, 0, "Shutdown environment");

#ifdef	HW_WDOG
/*
 * If there is a hardware watchdog, point this at the function needed to
 * hold it off.
 * It's needed when the kernel needs to do some lengthy operations.
 * e.g. in wd.c when dumping core.. It's most annoying to have
 * your precious core-dump only half written because the wdog kicked in.
 */
watchdog_tickle_fn wdog_tickler = NULL;
#endif	/* HW_WDOG */

/*
 * Variable panicstr contains argument to first call to panic; used as flag
 * to indicate that the kernel has already called panic.
 */
const char *panicstr;

int dumping;				/* system is dumping */
#ifdef SMP
u_int panic_cpu_interlock;		/* panic interlock */
globaldata_t panic_cpu_gd;		/* which cpu took the panic */
#endif

int bootverbose = 0;			/* note: assignment to force non-bss */
SYSCTL_INT(_debug, OID_AUTO, bootverbose, CTLFLAG_RW,
	   &bootverbose, 0, "Verbose kernel messages");

int cold = 1;				/* note: assignment to force non-bss */
int dumplo;				/* OBSOLETE - savecore compat */
u_int64_t dumplo64;

static void boot (int) __dead2;
static void dumpsys (void);
static int setdumpdev (cdev_t dev);
static void poweroff_wait (void *, int);
static void print_uptime (void);
static void shutdown_halt (void *junk, int howto);
static void shutdown_panic (void *junk, int howto);
static void shutdown_reset (void *junk, int howto);
static int shutdown_busycount1(struct buf *bp, void *info);
static int shutdown_busycount2(struct buf *bp, void *info);
static void shutdown_cleanup_proc(struct proc *p);

/* register various local shutdown events */
static void 
shutdown_conf(void *unused)
{
	EVENTHANDLER_REGISTER(shutdown_final, poweroff_wait, NULL, SHUTDOWN_PRI_FIRST);
	EVENTHANDLER_REGISTER(shutdown_final, shutdown_halt, NULL, SHUTDOWN_PRI_LAST + 100);
	EVENTHANDLER_REGISTER(shutdown_final, shutdown_panic, NULL, SHUTDOWN_PRI_LAST + 100);
	EVENTHANDLER_REGISTER(shutdown_final, shutdown_reset, NULL, SHUTDOWN_PRI_LAST + 200);
}

SYSINIT(shutdown_conf, SI_BOOT2_MACHDEP, SI_ORDER_ANY, shutdown_conf, NULL)

/* ARGSUSED */

/*
 * The system call that results in a reboot
 */
int
sys_reboot(struct reboot_args *uap)
{
	struct thread *td = curthread;
	int error;

	if ((error = priv_check(td, PRIV_REBOOT)))
		return (error);

	boot(uap->opt);
	return (0);
}

/*
 * Called by events that want to shut down.. e.g  <CTL><ALT><DEL> on a PC
 */
static int shutdown_howto = 0;

void
shutdown_nice(int howto)
{
	shutdown_howto = howto;
	
	/* Send a signal to init(8) and have it shutdown the world */
	if (initproc != NULL) {
		ksignal(initproc, SIGINT);
	} else {
		/* No init(8) running, so simply reboot */
		boot(RB_NOSYNC);
	}
	return;
}
static int	waittime = -1;
static struct thread *dumpthread;
static struct pcb dumppcb;

static void
print_uptime(void)
{
	int f;
	struct timespec ts;

	getnanouptime(&ts);
	kprintf("Uptime: ");
	f = 0;
	if (ts.tv_sec >= 86400) {
		kprintf("%ldd", ts.tv_sec / 86400);
		ts.tv_sec %= 86400;
		f = 1;
	}
	if (f || ts.tv_sec >= 3600) {
		kprintf("%ldh", ts.tv_sec / 3600);
		ts.tv_sec %= 3600;
		f = 1;
	}
	if (f || ts.tv_sec >= 60) {
		kprintf("%ldm", ts.tv_sec / 60);
		ts.tv_sec %= 60;
		f = 1;
	}
	kprintf("%lds\n", ts.tv_sec);
}

/*
 *  Go through the rigmarole of shutting down..
 * this used to be in machdep.c but I'll be dammned if I could see
 * anything machine dependant in it.
 */
static void
boot(int howto)
{
	/*
	 * Get rid of any user scheduler baggage and then give
	 * us a high priority.
	 */
	if (curthread->td_release)
		curthread->td_release(curthread);
	lwkt_setpri_self(TDPRI_MAX);

	/* collect extra flags that shutdown_nice might have set */
	howto |= shutdown_howto;

#ifdef SMP
	/*
	 * We really want to shutdown on the BSP.  Subsystems such as ACPI
	 * can't power-down the box otherwise.
	 */
	if (smp_active_mask > 1) {
		kprintf("boot() called on cpu#%d\n", mycpu->gd_cpuid);
	}
	if (panicstr == NULL && mycpu->gd_cpuid != 0) {
		kprintf("Switching to cpu #0 for shutdown\n");
		lwkt_setcpu_self(globaldata_find(0));
	}
#endif
	/*
	 * Do any callouts that should be done BEFORE syncing the filesystems.
	 */
	EVENTHANDLER_INVOKE(shutdown_pre_sync, howto);

	/*
	 * Try to get rid of any remaining FS references.  The calling
	 * process, proc0, and init may still hold references.  The
	 * VFS cache subsystem may still hold a root reference to root.
	 *
	 * XXX this needs work.  We really need to SIGSTOP all remaining
	 * processes in order to avoid blowups due to proc0's filesystem
	 * references going away.  For now just make sure that the init
	 * process is stopped.
	 */
	if (panicstr == NULL) {
		shutdown_cleanup_proc(curproc);
		shutdown_cleanup_proc(&proc0);
		if (initproc) {
			if (initproc != curproc) {
				ksignal(initproc, SIGSTOP);
				tsleep(boot, 0, "shutdn", hz / 20);
			}
			shutdown_cleanup_proc(initproc);
		}
		vfs_cache_setroot(NULL, NULL);
	}

	/* 
	 * Now sync filesystems
	 */
	if (!cold && (howto & RB_NOSYNC) == 0 && waittime < 0) {
		int iter, nbusy, pbusy;

		waittime = 0;
		kprintf("\nsyncing disks... ");

		sys_sync(NULL);	/* YYY was sync(&proc0, NULL). why proc0 ? */

		/*
		 * With soft updates, some buffers that are
		 * written will be remarked as dirty until other
		 * buffers are written.
		 */
		for (iter = pbusy = 0; iter < 20; iter++) {
			nbusy = scan_all_buffers(shutdown_busycount1, NULL);
			if (nbusy == 0)
				break;
			kprintf("%d ", nbusy);
			if (nbusy < pbusy)
				iter = 0;
			pbusy = nbusy;
			/*
			 * XXX:
			 * Process soft update work queue if buffers don't sync
			 * after 6 iterations by permitting the syncer to run.
			 */
			if (iter > 5)
				bio_ops_sync(NULL);
 
			sys_sync(NULL); /* YYY was sync(&proc0, NULL). why proc0 ? */
			tsleep(boot, 0, "shutdn", hz * iter / 20 + 1);
		}
		kprintf("\n");
		/*
		 * Count only busy local buffers to prevent forcing 
		 * a fsck if we're just a client of a wedged NFS server
		 */
		nbusy = scan_all_buffers(shutdown_busycount2, NULL);
		if (nbusy) {
			/*
			 * Failed to sync all blocks. Indicate this and don't
			 * unmount filesystems (thus forcing an fsck on reboot).
			 */
			kprintf("giving up on %d buffers\n", nbusy);
#ifdef DDB
			Debugger("busy buffer problem");
#endif /* DDB */
			tsleep(boot, 0, "shutdn", hz * 5 + 1);
		} else {
			kprintf("done\n");
			/*
			 * Unmount filesystems
			 */
			if (panicstr == NULL)
				vfs_unmountall();
		}
		tsleep(boot, 0, "shutdn", hz / 10 + 1);
	}

	print_uptime();

	/*
	 * Dump before doing post_sync shutdown ops
	 */
	crit_enter();
	if ((howto & (RB_HALT|RB_DUMP)) == RB_DUMP && !cold)
		dumpsys();

	/*
	 * Ok, now do things that assume all filesystem activity has
	 * been completed.  This will also call the device shutdown
	 * methods.
	 */
	EVENTHANDLER_INVOKE(shutdown_post_sync, howto);

	/* Now that we're going to really halt the system... */
	EVENTHANDLER_INVOKE(shutdown_final, howto);

	for(;;) ;	/* safety against shutdown_reset not working */
	/* NOTREACHED */
}

static int
shutdown_busycount1(struct buf *bp, void *info)
{
	if ((bp->b_flags & B_INVAL) == 0 && BUF_REFCNT(bp) > 0)
		return(1);
	if ((bp->b_flags & (B_DELWRI | B_INVAL)) == B_DELWRI)
		return (1);
	return (0);
}

static int
shutdown_busycount2(struct buf *bp, void *info)
{
	if (((bp->b_flags & B_INVAL) == 0 && BUF_REFCNT(bp)) ||
	    ((bp->b_flags & (B_DELWRI|B_INVAL)) == B_DELWRI)) {
		/*
		 * Only count buffers undergoing write I/O
		 * on the related vnode.
		 */
		if (bp->b_vp == NULL || 
		    bio_track_active(&bp->b_vp->v_track_write) == 0) {
			return (0);
		}
#if defined(SHOW_BUSYBUFS) || defined(DIAGNOSTIC)
		kprintf(
	    "%p dev:?, flags:%08x, loffset:%lld, doffset:%lld\n",
		    bp, 
		    bp->b_flags, bp->b_loffset,
		    bp->b_bio2.bio_offset);
#endif
		return(1);
	}
	return(0);
}

/*
 * If the shutdown was a clean halt, behave accordingly.
 */
static void
shutdown_halt(void *junk, int howto)
{
	if (howto & RB_HALT) {
		kprintf("\n");
		kprintf("The operating system has halted.\n");
#ifdef _KERNEL_VIRTUAL
		cpu_halt();
#else
		kprintf("Please press any key to reboot.\n\n");
		switch (cngetc()) {
		case -1:		/* No console, just die */
			cpu_halt();
			/* NOTREACHED */
		default:
			howto &= ~RB_HALT;
			break;
		}
#endif
	}
}

/*
 * Check to see if the system paniced, pause and then reboot
 * according to the specified delay.
 */
static void
shutdown_panic(void *junk, int howto)
{
	int loop;

	if (howto & RB_DUMP) {
		if (PANIC_REBOOT_WAIT_TIME != 0) {
			if (PANIC_REBOOT_WAIT_TIME != -1) {
				kprintf("Automatic reboot in %d seconds - "
				       "press a key on the console to abort\n",
					PANIC_REBOOT_WAIT_TIME);
				for (loop = PANIC_REBOOT_WAIT_TIME * 10;
				     loop > 0; --loop) {
					DELAY(1000 * 100); /* 1/10th second */
					/* Did user type a key? */
					if (cncheckc() != -1)
						break;
				}
				if (!loop)
					return;
			}
		} else { /* zero time specified - reboot NOW */
			return;
		}
		kprintf("--> Press a key on the console to reboot,\n");
		kprintf("--> or switch off the system now.\n");
		cngetc();
	}
}

/*
 * Everything done, now reset
 */
static void
shutdown_reset(void *junk, int howto)
{
	kprintf("Rebooting...\n");
	DELAY(1000000);	/* wait 1 sec for kprintf's to complete and be read */
	/* cpu_boot(howto); */ /* doesn't do anything at the moment */
	cpu_reset();
	/* NOTREACHED */ /* assuming reset worked */
}

/*
 * Try to remove FS references in the specified process.  This function
 * is used during shutdown
 */
static
void
shutdown_cleanup_proc(struct proc *p)
{
	struct filedesc *fdp;
	struct vmspace *vm;

	if (p == NULL)
		return;
	if ((fdp = p->p_fd) != NULL) {
		kern_closefrom(0);
		if (fdp->fd_cdir) {
			cache_drop(&fdp->fd_ncdir);
			vrele(fdp->fd_cdir);
			fdp->fd_cdir = NULL;
		}
		if (fdp->fd_rdir) {
			cache_drop(&fdp->fd_nrdir);
			vrele(fdp->fd_rdir);
			fdp->fd_rdir = NULL;
		}
		if (fdp->fd_jdir) {
			cache_drop(&fdp->fd_njdir);
			vrele(fdp->fd_jdir);
			fdp->fd_jdir = NULL;
		}
	}
	if (p->p_vkernel)
		vkernel_exit(p);
	if (p->p_textvp) {
		vrele(p->p_textvp);
		p->p_textvp = NULL;
	}
	vm = p->p_vmspace;
	if (vm != NULL) {
		pmap_remove_pages(vmspace_pmap(vm),
				  VM_MIN_USER_ADDRESS,
				  VM_MAX_USER_ADDRESS);
		vm_map_remove(&vm->vm_map,
			      VM_MIN_USER_ADDRESS,
			      VM_MAX_USER_ADDRESS);
	}
}

/*
 * Magic number for savecore
 *
 * exported (symorder) and used at least by savecore(8)
 *
 * Mark it as used so that gcc doesn't optimize it away.
 */
__attribute__((__used__))
	static u_long const dumpmag = 0x8fca0101UL;	

static int	dumpsize = 0;		/* also for savecore */

static int	dodump = 1;

SYSCTL_INT(_machdep, OID_AUTO, do_dump, CTLFLAG_RW, &dodump, 0,
    "Try to perform coredump on kernel panic");

static int
setdumpdev(cdev_t dev)
{
	struct partinfo pinfo;
	u_int64_t newdumplo;
	int error;
	int doopen;

	if (dev == NULL) {
		dumpdev = dev;
		return (0);
	}
	bzero(&pinfo, sizeof(pinfo));

	/*
	 * We have to open the device before we can perform ioctls on it,
	 * or the slice/label data may not be present.  Device opens are
	 * usually tracked by specfs, but the dump device can be set in
	 * early boot and may not be open so this is somewhat of a hack.
	 */
	doopen = (dev->si_sysref.refcnt == 1);
	if (doopen) {
		error = dev_dopen(dev, FREAD, S_IFCHR, proc0.p_ucred);
		if (error)
			return (error);
	}
	error = dev_dioctl(dev, DIOCGPART, (void *)&pinfo, 0, proc0.p_ucred);
	if (doopen)
		dev_dclose(dev, FREAD, S_IFCHR);
	if (error || pinfo.media_blocks == 0 || pinfo.media_blksize == 0)
		return (ENXIO);

	newdumplo = pinfo.media_blocks - 
		    ((u_int64_t)Maxmem * PAGE_SIZE / DEV_BSIZE);
	if ((int64_t)newdumplo < (int64_t)pinfo.reserved_blocks)
		return (ENOSPC);
	dumpdev = dev;
	dumplo64 = newdumplo;
	return (0);
}


/* ARGSUSED */
static void dump_conf (void *dummy);
static void
dump_conf(void *dummy)
{
	char *path;
	cdev_t dev;

	path = kmalloc(MNAMELEN, M_TEMP, M_WAITOK);
	if (TUNABLE_STR_FETCH("dumpdev", path, MNAMELEN) != 0) {
		dev = kgetdiskbyname(path);
		if (dev != NULL)
			dumpdev = dev;
	}
	kfree(path, M_TEMP);
	if (setdumpdev(dumpdev) != 0)
		dumpdev = NULL;
}

SYSINIT(dump_conf, SI_SUB_DUMP_CONF, SI_ORDER_FIRST, dump_conf, NULL)

static int
sysctl_kern_dumpdev(SYSCTL_HANDLER_ARGS)
{
	int error;
	udev_t ndumpdev;

	ndumpdev = dev2udev(dumpdev);
	error = sysctl_handle_opaque(oidp, &ndumpdev, sizeof ndumpdev, req);
	if (error == 0 && req->newptr != NULL)
		error = setdumpdev(udev2dev(ndumpdev, 0));
	return (error);
}

SYSCTL_PROC(_kern, KERN_DUMPDEV, dumpdev, CTLTYPE_OPAQUE|CTLFLAG_RW,
	0, sizeof dumpdev, sysctl_kern_dumpdev, "T,udev_t", "");

/*
 * Doadump comes here after turning off memory management and
 * getting on the dump stack, either when called above, or by
 * the auto-restart code.
 */
static void
dumpsys(void)
{
	int	error;

	savectx(&dumppcb);
	dumpthread = curthread;
	if (dumping++) {
		kprintf("Dump already in progress, bailing...\n");
		return;
	}
	if (!dodump)
		return;
	if (dumpdev == NULL)
		return;
	dumpsize = Maxmem;
	kprintf("\ndumping to dev %s, blockno %lld\n",
		devtoname(dumpdev),
		(long long)dumplo64);
	kprintf("dump ");
	error = dev_ddump(dumpdev);
	if (error == 0) {
		kprintf("succeeded\n");
		return;
	}
	kprintf("failed, reason: ");
	switch (error) {
	case ENOSYS:
	case ENODEV:
		kprintf("device doesn't support a dump routine\n");
		break;

	case ENXIO:
		kprintf("device bad\n");
		break;

	case EFAULT:
		kprintf("device not ready\n");
		break;

	case EINVAL:
		kprintf("area improper\n");
		break;

	case EIO:
		kprintf("i/o error\n");
		break;

	case EINTR:
		kprintf("aborted from console\n");
		break;

	default:
		kprintf("unknown, error = %d\n", error);
		break;
	}
}

int
dumpstatus(vm_offset_t addr, off_t count)
{
	int c;

	if (addr % (1024 * 1024) == 0) {
#ifdef HW_WDOG
		if (wdog_tickler)
			(*wdog_tickler)();
#endif   
		kprintf("%ld ", (long)(count / (1024 * 1024)));
	}

	if ((c = cncheckc()) == 0x03)
		return -1;
	else if (c != -1)
		kprintf("[CTRL-C to abort] ");
	
	return 0;
}

/*
 * Panic is called on unresolvable fatal errors.  It prints "panic: mesg",
 * and then reboots.  If we are called twice, then we avoid trying to sync
 * the disks as this often leads to recursive panics.
 */
void
panic(const char *fmt, ...)
{
	int bootopt, newpanic;
	__va_list ap;
	static char buf[256];

#ifdef SMP
	/*
	 * If a panic occurs on multiple cpus before the first is able to
	 * halt the other cpus, only one cpu is allowed to take the panic.
	 * Attempt to be verbose about this situation but if the kprintf() 
	 * itself panics don't let us overrun the kernel stack.
	 *
	 * Be very nasty about descheduling our thread at the lowest
	 * level possible in an attempt to freeze the thread without
	 * inducing further panics.
	 *
	 * Bumping gd_trap_nesting_level will also bypass assertions in
	 * lwkt_switch() and allow us to switch away even if we are a
	 * FAST interrupt or IPI.
	 */
	if (atomic_poll_acquire_int(&panic_cpu_interlock)) {
		panic_cpu_gd = mycpu;
	} else if (panic_cpu_gd != mycpu) {
		crit_enter();
		++mycpu->gd_trap_nesting_level;
		if (mycpu->gd_trap_nesting_level < 25) {
			kprintf("SECONDARY PANIC ON CPU %d THREAD %p\n",
				mycpu->gd_cpuid, curthread);
		}
		curthread->td_release = NULL;	/* be a grinch */
		for (;;) {
			lwkt_deschedule_self(curthread);
			lwkt_switch();
		}
		/* NOT REACHED */
		/* --mycpu->gd_trap_nesting_level */
		/* crit_exit() */
	}
#endif
	bootopt = RB_AUTOBOOT | RB_DUMP;
	if (sync_on_panic == 0)
		bootopt |= RB_NOSYNC;
	newpanic = 0;
	if (panicstr)
		bootopt |= RB_NOSYNC;
	else {
		panicstr = fmt;
		newpanic = 1;
	}

	__va_start(ap, fmt);
	kvsnprintf(buf, sizeof(buf), fmt, ap);
	if (panicstr == fmt)
		panicstr = buf;
	__va_end(ap);
	kprintf("panic: %s\n", buf);
#ifdef SMP
	/* two separate prints in case of an unmapped page and trap */
	kprintf("mp_lock = %08x; ", mp_lock);
	kprintf("cpuid = %d\n", mycpu->gd_cpuid);
#endif

#if defined(DDB)
	if (newpanic && trace_on_panic)
		print_backtrace();
	if (debugger_on_panic)
		Debugger("panic");
#endif
	boot(bootopt);
}

/*
 * Support for poweroff delay.
 */
#ifndef POWEROFF_DELAY
# define POWEROFF_DELAY 5000
#endif
static int poweroff_delay = POWEROFF_DELAY;

SYSCTL_INT(_kern_shutdown, OID_AUTO, poweroff_delay, CTLFLAG_RW,
	&poweroff_delay, 0, "");

static void 
poweroff_wait(void *junk, int howto)
{
	if(!(howto & RB_POWEROFF) || poweroff_delay <= 0)
		return;
	DELAY(poweroff_delay * 1000);
}

/*
 * Some system processes (e.g. syncer) need to be stopped at appropriate
 * points in their main loops prior to a system shutdown, so that they
 * won't interfere with the shutdown process (e.g. by holding a disk buf
 * to cause sync to fail).  For each of these system processes, register
 * shutdown_kproc() as a handler for one of shutdown events.
 */
static int kproc_shutdown_wait = 60;
SYSCTL_INT(_kern_shutdown, OID_AUTO, kproc_shutdown_wait, CTLFLAG_RW,
    &kproc_shutdown_wait, 0, "");

void
shutdown_kproc(void *arg, int howto)
{
	struct thread *td;
	struct proc *p;
	int error;

	if (panicstr)
		return;

	td = (struct thread *)arg;
	if ((p = td->td_proc) != NULL) {
	    kprintf("Waiting (max %d seconds) for system process `%s' to stop...",
		kproc_shutdown_wait, p->p_comm);
	} else {
	    kprintf("Waiting (max %d seconds) for system thread %s to stop...",
		kproc_shutdown_wait, td->td_comm);
	}
	error = suspend_kproc(td, kproc_shutdown_wait * hz);

	if (error == EWOULDBLOCK)
		kprintf("timed out\n");
	else
		kprintf("stopped\n");
}
