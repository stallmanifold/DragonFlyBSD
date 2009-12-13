/*
 * Copyright (c) 1982, 1986, 1989, 1991, 1993
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
 *	@(#)kern_exit.c	8.7 (Berkeley) 2/12/94
 * $FreeBSD: src/sys/kern/kern_exit.c,v 1.92.2.11 2003/01/13 22:51:16 dillon Exp $
 * $DragonFly: src/sys/kern/kern_exit.c,v 1.91 2008/05/18 20:02:02 nth Exp $
 */

#include "opt_compat.h"
#include "opt_ktrace.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysproto.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/ktrace.h>
#include <sys/pioctl.h>
#include <sys/tty.h>
#include <sys/wait.h>
#include <sys/vnode.h>
#include <sys/resourcevar.h>
#include <sys/signalvar.h>
#include <sys/taskqueue.h>
#include <sys/ptrace.h>
#include <sys/acct.h>		/* for acct_process() function prototype */
#include <sys/filedesc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/aio.h>
#include <sys/jail.h>
#include <sys/kern_syscall.h>
#include <sys/upcall.h>
#include <sys/caps.h>
#include <sys/unistd.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <sys/lock.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_extern.h>
#include <sys/user.h>

#include <sys/thread2.h>
#include <sys/sysref2.h>

static void reaplwps(void *context, int dummy);
static void reaplwp(struct lwp *lp);
static void killlwps(struct lwp *lp);

static MALLOC_DEFINE(M_ATEXIT, "atexit", "atexit callback");
static MALLOC_DEFINE(M_ZOMBIE, "zombie", "zombie proc status");

/*
 * callout list for things to do at exit time
 */
struct exitlist {
	exitlist_fn function;
	TAILQ_ENTRY(exitlist) next;
};

TAILQ_HEAD(exit_list_head, exitlist);
static struct exit_list_head exit_list = TAILQ_HEAD_INITIALIZER(exit_list);

/*
 * LWP reaper data
 */
struct task *deadlwp_task[MAXCPU];
struct lwplist deadlwp_list[MAXCPU];

/*
 * exit --
 *	Death of process.
 *
 * SYS_EXIT_ARGS(int rval)
 *
 * MPALMOSTSAFE
 */
int
sys_exit(struct exit_args *uap)
{
	get_mplock();
	exit1(W_EXITCODE(uap->rval, 0));
	/* NOTREACHED */
	rel_mplock();
}

/*
 * Extended exit --
 *	Death of a lwp or process with optional bells and whistles.
 *
 * MPALMOSTSAFE
 */
int
sys_extexit(struct extexit_args *uap)
{
	int action, who;
	int error;

	action = EXTEXIT_ACTION(uap->how);
	who = EXTEXIT_WHO(uap->how);

	/* Check parameters before we might perform some action */
	switch (who) {
	case EXTEXIT_PROC:
	case EXTEXIT_LWP:
		break;
	default:
		return (EINVAL);
	}

	switch (action) {
	case EXTEXIT_SIMPLE:
		break;
	case EXTEXIT_SETINT:
		error = copyout(&uap->status, uap->addr, sizeof(uap->status));
		if (error)
			return (error);
		break;
	default:
		return (EINVAL);
	}

	get_mplock();

	switch (who) {
	case EXTEXIT_LWP:
		/*
		 * Be sure only to perform a simple lwp exit if there is at
		 * least one more lwp in the proc, which will call exit1()
		 * later, otherwise the proc will be an UNDEAD and not even a
		 * SZOMB!
		 */
		if (curproc->p_nthreads > 1) {
			lwp_exit(0);
			/* NOT REACHED */
		}
		/* else last lwp in proc:  do the real thing */
		/* FALLTHROUGH */
	default:	/* to help gcc */
	case EXTEXIT_PROC:
		exit1(W_EXITCODE(uap->status, 0));
		/* NOTREACHED */
	}

	/* NOTREACHED */
	rel_mplock(); /* safety */
}

/*
 * Kill all lwps associated with the current process except the
 * current lwp.   Return an error if we race another thread trying to
 * do the same thing and lose the race.
 *
 * If forexec is non-zero the current thread and process flags are
 * cleaned up so they can be reused.
 */
int
killalllwps(int forexec)
{
	struct lwp *lp = curthread->td_lwp;
	struct proc *p = lp->lwp_proc;

	/*
	 * Interlock against P_WEXIT.  Only one of the process's thread
	 * is allowed to do the master exit.
	 */
	if (p->p_flag & P_WEXIT)
		return (EALREADY);
	p->p_flag |= P_WEXIT;

	/*
	 * Interlock with LWP_WEXIT and kill any remaining LWPs
	 */
	lp->lwp_flag |= LWP_WEXIT;
	if (p->p_nthreads > 1)
		killlwps(lp);

	/*
	 * If doing this for an exec, clean up the remaining thread
	 * (us) for continuing operation after all the other threads
	 * have been killed.
	 */
	if (forexec) {
		lp->lwp_flag &= ~LWP_WEXIT;
		p->p_flag &= ~P_WEXIT;
	}
	return(0);
}

/*
 * Kill all LWPs except the current one.  Do not try to signal
 * LWPs which have exited on their own or have already been
 * signaled.
 */
static void
killlwps(struct lwp *lp)
{
	struct proc *p = lp->lwp_proc;
	struct lwp *tlp;

	/*
	 * Kill the remaining LWPs.  We must send the signal before setting
	 * LWP_WEXIT.  The setting of WEXIT is optional but helps reduce
	 * races.  tlp must be held across the call as it might block and
	 * allow the target lwp to rip itself out from under our loop.
	 */
	FOREACH_LWP_IN_PROC(tlp, p) {
		LWPHOLD(tlp);
		if ((tlp->lwp_flag & LWP_WEXIT) == 0) {
			lwpsignal(p, tlp, SIGKILL);
			tlp->lwp_flag |= LWP_WEXIT;
		}
		LWPRELE(tlp);
	}

	/*
	 * Wait for everything to clear out.
	 */
	while (p->p_nthreads > 1) {
		tsleep(&p->p_nthreads, 0, "killlwps", 0);
	}
}

/*
 * Exit: deallocate address space and other resources, change proc state
 * to zombie, and unlink proc from allproc and parent's lists.  Save exit
 * status and rusage for wait().  Check for child processes and orphan them.
 */
void
exit1(int rv)
{
	struct thread *td = curthread;
	struct proc *p = td->td_proc;
	struct lwp *lp = td->td_lwp;
	struct proc *q, *nq;
	struct vmspace *vm;
	struct vnode *vtmp;
	struct exitlist *ep;
	int error;

	if (p->p_pid == 1) {
		kprintf("init died (signal %d, exit %d)\n",
		    WTERMSIG(rv), WEXITSTATUS(rv));
		panic("Going nowhere without my init!");
	}

	varsymset_clean(&p->p_varsymset);
	lockuninit(&p->p_varsymset.vx_lock);
	/*
	 * Kill all lwps associated with the current process, return an
	 * error if we race another thread trying to do the same thing
	 * and lose the race.
	 */
	error = killalllwps(0);
	if (error) {
		lwp_exit(0);
		/* NOT REACHED */
	}

	caps_exit(lp->lwp_thread);
	aio_proc_rundown(p);

	/* are we a task leader? */
	if (p == p->p_leader) {
        	struct kill_args killArgs;
		killArgs.signum = SIGKILL;
		q = p->p_peers;
		while(q) {
			killArgs.pid = q->p_pid;
			/*
		         * The interface for kill is better
			 * than the internal signal
			 */
			sys_kill(&killArgs);
			nq = q;
			q = q->p_peers;
		}
		while (p->p_peers) 
			tsleep((caddr_t)p, 0, "exit1", 0);
	} 

#ifdef PGINPROF
	vmsizmon();
#endif
	STOPEVENT(p, S_EXIT, rv);
	wakeup(&p->p_stype);	/* Wakeup anyone in procfs' PIOCWAIT */

	/* 
	 * Check if any loadable modules need anything done at process exit.
	 * e.g. SYSV IPC stuff
	 * XXX what if one of these generates an error?
	 */
	TAILQ_FOREACH(ep, &exit_list, next) 
		(*ep->function)(td);

	if (p->p_flag & P_PROFIL)
		stopprofclock(p);
	/*
	 * If parent is waiting for us to exit or exec,
	 * P_PPWAIT is set; we will wakeup the parent below.
	 */
	p->p_flag &= ~(P_TRACED | P_PPWAIT);
	SIGEMPTYSET(p->p_siglist);
	SIGEMPTYSET(lp->lwp_siglist);
	if (timevalisset(&p->p_realtimer.it_value))
		callout_stop(&p->p_ithandle);

	/*
	 * Reset any sigio structures pointing to us as a result of
	 * F_SETOWN with our pid.
	 */
	funsetownlst(&p->p_sigiolst);

	/*
	 * Close open files and release open-file table.
	 * This may block!
	 */
	fdfree(p, NULL);

	if(p->p_leader->p_peers) {
		q = p->p_leader;
		while(q->p_peers != p)
			q = q->p_peers;
		q->p_peers = p->p_peers;
		wakeup((caddr_t)p->p_leader);
	}

	/*
	 * XXX Shutdown SYSV semaphores
	 */
	semexit(p);

	KKASSERT(p->p_numposixlocks == 0);

	/* The next two chunks should probably be moved to vmspace_exit. */
	vm = p->p_vmspace;

	/*
	 * Release upcalls associated with this process
	 */
	if (vm->vm_upcalls)
		upc_release(vm, lp);

	/*
	 * Clean up data related to virtual kernel operation.  Clean up
	 * any vkernel context related to the current lwp now so we can
	 * destroy p_vkernel.
	 */
	if (p->p_vkernel) {
		vkernel_lwp_exit(lp);
		vkernel_exit(p);
	}

	/*
	 * Release user portion of address space.
	 * This releases references to vnodes,
	 * which could cause I/O if the file has been unlinked.
	 * Need to do this early enough that we can still sleep.
	 * Can't free the entire vmspace as the kernel stack
	 * may be mapped within that space also.
	 *
	 * Processes sharing the same vmspace may exit in one order, and
	 * get cleaned up by vmspace_exit() in a different order.  The
	 * last exiting process to reach this point releases as much of
	 * the environment as it can, and the last process cleaned up
	 * by vmspace_exit() (which decrements exitingcnt) cleans up the
	 * remainder.
	 */
	++vm->vm_exitingcnt;
	sysref_put(&vm->vm_sysref);

	if (SESS_LEADER(p)) {
		struct session *sp = p->p_session;

		if (sp->s_ttyvp) {
			/*
			 * We are the controlling process.  Signal the 
			 * foreground process group, drain the controlling
			 * terminal, and revoke access to the controlling
			 * terminal.
			 *
			 * NOTE: while waiting for the process group to exit
			 * it is possible that one of the processes in the
			 * group will revoke the tty, so the ttyclosesession()
			 * function will re-check sp->s_ttyvp.
			 */
			if (sp->s_ttyp && (sp->s_ttyp->t_session == sp)) {
				if (sp->s_ttyp->t_pgrp)
					pgsignal(sp->s_ttyp->t_pgrp, SIGHUP, 1);
				ttywait(sp->s_ttyp);
				ttyclosesession(sp, 1); /* also revoke */
			}
			/*
			 * Release the tty.  If someone has it open via
			 * /dev/tty then close it (since they no longer can
			 * once we've NULL'd it out).
			 */
			ttyclosesession(sp, 0);

			/*
			 * s_ttyp is not zero'd; we use this to indicate
			 * that the session once had a controlling terminal.
			 * (for logging and informational purposes)
			 */
		}
		sp->s_leader = NULL;
	}
	fixjobc(p, p->p_pgrp, 0);
	(void)acct_process(p);
#ifdef KTRACE
	/*
	 * release trace file
	 */
	if (p->p_tracenode)
		ktrdestroy(&p->p_tracenode);
	p->p_traceflag = 0;
#endif
	/*
	 * Release reference to text vnode
	 */
	if ((vtmp = p->p_textvp) != NULL) {
		p->p_textvp = NULL;
		vrele(vtmp);
	}

	/*
	 * Move the process to the zombie list.  This will block
	 * until the process p_lock count reaches 0.  The process will
	 * not be reaped until TDF_EXITING is set by cpu_thread_exit(),
	 * which is called from cpu_proc_exit().
	 */
	proc_move_allproc_zombie(p);

	q = LIST_FIRST(&p->p_children);
	if (q)		/* only need this if any child is S_ZOMB */
		wakeup((caddr_t) initproc);
	for (; q != 0; q = nq) {
		nq = LIST_NEXT(q, p_sibling);
		LIST_REMOVE(q, p_sibling);
		LIST_INSERT_HEAD(&initproc->p_children, q, p_sibling);
		q->p_pptr = initproc;
		q->p_sigparent = SIGCHLD;
		/*
		 * Traced processes are killed
		 * since their existence means someone is screwing up.
		 */
		if (q->p_flag & P_TRACED) {
			q->p_flag &= ~P_TRACED;
			ksignal(q, SIGKILL);
		}
	}

	/*
	 * Save exit status and final rusage info, adding in child rusage
	 * info and self times.
	 */
	p->p_xstat = rv;
	calcru_proc(p, &p->p_ru);
	ruadd(&p->p_ru, &p->p_cru);

	/*
	 * notify interested parties of our demise.
	 */
	KNOTE(&p->p_klist, NOTE_EXIT);

	/*
	 * Notify parent that we're gone.  If parent has the PS_NOCLDWAIT
	 * flag set, notify process 1 instead (and hope it will handle
	 * this situation).
	 */
	if (p->p_pptr->p_sigacts->ps_flag & PS_NOCLDWAIT) {
		struct proc *pp = p->p_pptr;
		proc_reparent(p, initproc);
		/*
		 * If this was the last child of our parent, notify
		 * parent, so in case he was wait(2)ing, he will
		 * continue.
		 */
		if (LIST_EMPTY(&pp->p_children))
			wakeup((caddr_t)pp);
	}

	if (p->p_sigparent && p->p_pptr != initproc) {
	        ksignal(p->p_pptr, p->p_sigparent);
	} else {
	        ksignal(p->p_pptr, SIGCHLD);
	}

	wakeup((caddr_t)p->p_pptr);
	/*
	 * cpu_exit is responsible for clearing curproc, since
	 * it is heavily integrated with the thread/switching sequence.
	 *
	 * Other substructures are freed from wait().
	 */
	plimit_free(p);

	/*
	 * Release the current user process designation on the process so
	 * the userland scheduler can work in someone else.
	 */
	p->p_usched->release_curproc(lp);

	/*
	 * Finally, call machine-dependent code to release as many of the
	 * lwp's resources as we can and halt execution of this thread.
	 */
	lwp_exit(1);
}

void
lwp_exit(int masterexit)
{
	struct lwp *lp = curthread->td_lwp;
	struct proc *p = lp->lwp_proc;

	/*
	 * lwp_exit() may be called without setting LWP_WEXIT, so
	 * make sure it is set here.
	 */
	lp->lwp_flag |= LWP_WEXIT;

	/*
	 * Clean up any virtualization
	 */
	if (lp->lwp_vkernel)
		vkernel_lwp_exit(lp);

	/*
	 * Nobody actually wakes us when the lock
	 * count reaches zero, so just wait one tick.
	 */
	while (lp->lwp_lock > 0)
		tsleep(lp, 0, "lwpexit", 1);

	/* Hand down resource usage to our proc */
	ruadd(&p->p_ru, &lp->lwp_ru);

	/*
	 * If we don't hold the process until the LWP is reaped wait*()
	 * may try to dispose of its vmspace before all the LWPs have
	 * actually terminated.
	 */
	PHOLD(p);

	/*
	 * We have to use the reaper for all the LWPs except the one doing
	 * the master exit.  The LWP doing the master exit can just be
	 * left on p_lwps and the process reaper will deal with it
	 * synchronously, which is much faster.
	 */
	if (masterexit == 0) {
		lwp_rb_tree_RB_REMOVE(&p->p_lwp_tree, lp);
		--p->p_nthreads;
		wakeup(&p->p_nthreads);
		LIST_INSERT_HEAD(&deadlwp_list[mycpuid], lp, u.lwp_reap_entry);
		taskqueue_enqueue(taskqueue_thread[mycpuid], deadlwp_task[mycpuid]);
	} else {
		--p->p_nthreads;
	}
	biosched_done(curthread);
	cpu_lwp_exit();
}

/*
 * Wait until a lwp is completely dead.
 *
 * If the thread is still executing, which can't be waited upon,
 * return failure.  The caller is responsible of waiting a little
 * bit and checking again.
 *
 * Suggested use:
 * while (!lwp_wait(lp))
 *	tsleep(lp, 0, "lwpwait", 1);
 */
static int
lwp_wait(struct lwp *lp)
{
	struct thread *td = lp->lwp_thread;;

	KKASSERT(lwkt_preempted_proc() != lp);

	while (lp->lwp_lock > 0)
		tsleep(lp, 0, "lwpwait1", 1);

	lwkt_wait_free(td);

	/*
	 * The lwp's thread may still be in the middle
	 * of switching away, we can't rip its stack out from
	 * under it until TDF_EXITING is set and both
	 * TDF_RUNNING and TDF_PREEMPT_LOCK are clear.
	 * TDF_PREEMPT_LOCK must be checked because TDF_RUNNING
	 * will be cleared temporarily if a thread gets
	 * preempted.
	 *
	 * YYY no wakeup occurs, so we simply return failure
	 * and let the caller deal with sleeping and calling
	 * us again.
	 */
	if ((td->td_flags & (TDF_RUNNING|TDF_PREEMPT_LOCK|TDF_EXITING)) !=
	    TDF_EXITING)
		return (0);

	return (1);
}

/*
 * Release the resources associated with a lwp.
 * The lwp must be completely dead.
 */
void
lwp_dispose(struct lwp *lp)
{
	struct thread *td = lp->lwp_thread;;

	KKASSERT(lwkt_preempted_proc() != lp);
	KKASSERT(td->td_refs == 0);
	KKASSERT((td->td_flags & (TDF_RUNNING|TDF_PREEMPT_LOCK|TDF_EXITING)) ==
		 TDF_EXITING);

	PRELE(lp->lwp_proc);
	lp->lwp_proc = NULL;
	if (td != NULL) {
		td->td_proc = NULL;
		td->td_lwp = NULL;
		lp->lwp_thread = NULL;
		lwkt_free_thread(td);
	}
	kfree(lp, M_LWP);
}

/*
 * MPSAFE
 */
int
sys_wait4(struct wait_args *uap)
{
	struct rusage rusage;
	int error, status;

	error = kern_wait(uap->pid, (uap->status ? &status : NULL),
			  uap->options, (uap->rusage ? &rusage : NULL),
			  &uap->sysmsg_result);

	if (error == 0 && uap->status)
		error = copyout(&status, uap->status, sizeof(*uap->status));
	if (error == 0 && uap->rusage)
		error = copyout(&rusage, uap->rusage, sizeof(*uap->rusage));
	return (error);
}

/*
 * wait1()
 *
 * wait_args(int pid, int *status, int options, struct rusage *rusage)
 *
 * MPALMOSTSAFE
 */
int
kern_wait(pid_t pid, int *status, int options, struct rusage *rusage, int *res)
{
	struct thread *td = curthread;
	struct lwp *lp;
	struct proc *q = td->td_proc;
	struct proc *p, *t;
	int nfound, error;

	if (pid == 0)
		pid = -q->p_pgid;
	if (options &~ (WUNTRACED|WNOHANG|WCONTINUED|WLINUXCLONE))
		return (EINVAL);
	get_mplock();
loop:
	/*
	 * Hack for backwards compatibility with badly written user code.  
	 * Or perhaps we have to do this anyway, it is unclear. XXX
	 *
	 * The problem is that if a process group is stopped and the parent
	 * is doing a wait*(..., WUNTRACED, ...), it will see the STOP
	 * of the child and then stop itself when it tries to return from the
	 * system call.  When the process group is resumed the parent will
	 * then get the STOP status even though the child has now resumed
	 * (a followup wait*() will get the CONT status).
	 *
	 * Previously the CONT would overwrite the STOP because the tstop
	 * was handled within tsleep(), and the parent would only see
	 * the CONT when both are stopped and continued together.  This litte
	 * two-line hack restores this effect.
	 */
	while (q->p_stat == SSTOP)
            tstop();

	nfound = 0;
	LIST_FOREACH(p, &q->p_children, p_sibling) {
		if (pid != WAIT_ANY &&
		    p->p_pid != pid && p->p_pgid != -pid)
			continue;

		/* This special case handles a kthread spawned by linux_clone 
		 * (see linux_misc.c).  The linux_wait4 and linux_waitpid 
		 * functions need to be able to distinguish between waiting
		 * on a process and waiting on a thread.  It is a thread if
		 * p_sigparent is not SIGCHLD, and the WLINUXCLONE option
		 * signifies we want to wait for threads and not processes.
		 */
		if ((p->p_sigparent != SIGCHLD) ^ 
		    ((options & WLINUXCLONE) != 0)) {
			continue;
		}

		nfound++;
		if (p->p_stat == SZOMB) {
			/*
			 * We may go into SZOMB with threads still present.
			 * We must wait for them to exit before we can reap
			 * the master thread, otherwise we may race reaping
			 * non-master threads.
			 */
			while (p->p_nthreads > 0) {
				tsleep(&p->p_nthreads, 0, "lwpzomb", hz);
			}

			/*
			 * Reap any LWPs left in p->p_lwps.  This is usually
			 * just the last LWP.  This must be done before
			 * we loop on p_lock since the lwps hold a ref on
			 * it as a vmspace interlock.
			 *
			 * Once that is accomplished p_nthreads had better
			 * be zero.
			 */
			while ((lp = RB_ROOT(&p->p_lwp_tree)) != NULL) {
				lwp_rb_tree_RB_REMOVE(&p->p_lwp_tree, lp);
				reaplwp(lp);
			}
			KKASSERT(p->p_nthreads == 0);

			/*
			 * Don't do anything really bad until all references
			 * to the process go away.  This may include other
			 * LWPs which are still in the process of being
			 * reaped.  We can't just pull the rug out from under
			 * them because they may still be using the VM space.
			 *
			 * Certain kernel facilities such as /proc will also
			 * put a hold on the process for short periods of
			 * time.
			 */
			while (p->p_lock)
				tsleep(p, 0, "reap3", hz);

			/* scheduling hook for heuristic */
			/* XXX no lwp available, we need a different heuristic */
			/*
			p->p_usched->heuristic_exiting(td->td_lwp, deadlp);
			*/

			/* Take care of our return values. */
			*res = p->p_pid;
			if (status)
				*status = p->p_xstat;
			if (rusage)
				*rusage = p->p_ru;
			/*
			 * If we got the child via a ptrace 'attach',
			 * we need to give it back to the old parent.
			 */
			if (p->p_oppid && (t = pfind(p->p_oppid))) {
				p->p_oppid = 0;
				proc_reparent(p, t);
				ksignal(t, SIGCHLD);
				wakeup((caddr_t)t);
				error = 0;
				goto done;
			}

			/*
			 * Unlink the proc from its process group so that
			 * the following operations won't lead to an
			 * inconsistent state for processes running down
			 * the zombie list.
			 */
			KKASSERT(p->p_lock == 0);
			proc_remove_zombie(p);
			leavepgrp(p);

			p->p_xstat = 0;
			ruadd(&q->p_cru, &p->p_ru);

			/*
			 * Decrement the count of procs running with this uid.
			 */
			chgproccnt(p->p_ucred->cr_ruidinfo, -1, 0);

			/*
			 * Free up credentials.
			 */
			crfree(p->p_ucred);
			p->p_ucred = NULL;

			/*
			 * Remove unused arguments
			 */
			if (p->p_args && --p->p_args->ar_ref == 0)
				FREE(p->p_args, M_PARGS);

			if (--p->p_sigacts->ps_refcnt == 0) {
				kfree(p->p_sigacts, M_SUBPROC);
				p->p_sigacts = NULL;
			}

			vm_waitproc(p);
			kfree(p, M_PROC);
			nprocs--;
			error = 0;
			goto done;
		}
		if (p->p_stat == SSTOP && (p->p_flag & P_WAITED) == 0 &&
		    (p->p_flag & P_TRACED || options & WUNTRACED)) {
			p->p_flag |= P_WAITED;

			*res = p->p_pid;
			if (status)
				*status = W_STOPCODE(p->p_xstat);
			/* Zero rusage so we get something consistent. */
			if (rusage)
				bzero(rusage, sizeof(rusage));
			error = 0;
			goto done;
		}
		if (options & WCONTINUED && (p->p_flag & P_CONTINUED)) {
			*res = p->p_pid;
			p->p_flag &= ~P_CONTINUED;

			if (status)
				*status = SIGCONT;
			error = 0;
			goto done;
		}
	}
	if (nfound == 0) {
		error = ECHILD;
		goto done;
	}
	if (options & WNOHANG) {
		*res = 0;
		error = 0;
		goto done;
	}
	error = tsleep((caddr_t)q, PCATCH, "wait", 0);
	if (error) {
done:
		rel_mplock();
		return (error);
	}
	goto loop;
}

/*
 * make process 'parent' the new parent of process 'child'.
 */
void
proc_reparent(struct proc *child, struct proc *parent)
{

	if (child->p_pptr == parent)
		return;

	LIST_REMOVE(child, p_sibling);
	LIST_INSERT_HEAD(&parent->p_children, child, p_sibling);
	child->p_pptr = parent;
}

/*
 * The next two functions are to handle adding/deleting items on the
 * exit callout list
 * 
 * at_exit():
 * Take the arguments given and put them onto the exit callout list,
 * However first make sure that it's not already there.
 * returns 0 on success.
 */

int
at_exit(exitlist_fn function)
{
	struct exitlist *ep;

#ifdef INVARIANTS
	/* Be noisy if the programmer has lost track of things */
	if (rm_at_exit(function)) 
		kprintf("WARNING: exit callout entry (%p) already present\n",
		    function);
#endif
	ep = kmalloc(sizeof(*ep), M_ATEXIT, M_NOWAIT);
	if (ep == NULL)
		return (ENOMEM);
	ep->function = function;
	TAILQ_INSERT_TAIL(&exit_list, ep, next);
	return (0);
}

/*
 * Scan the exit callout list for the given item and remove it.
 * Returns the number of items removed (0 or 1)
 */
int
rm_at_exit(exitlist_fn function)
{
	struct exitlist *ep;

	TAILQ_FOREACH(ep, &exit_list, next) {
		if (ep->function == function) {
			TAILQ_REMOVE(&exit_list, ep, next);
			kfree(ep, M_ATEXIT);
			return(1);
		}
	}	
	return (0);
}

/*
 * LWP reaper related code.
 */
static void
reaplwps(void *context, int dummy)
{
	struct lwplist *lwplist = context;
	struct lwp *lp;

	get_mplock();
	while ((lp = LIST_FIRST(lwplist))) {
		LIST_REMOVE(lp, u.lwp_reap_entry);
		reaplwp(lp);
	}
	rel_mplock();
}

static void
reaplwp(struct lwp *lp)
{
	while (lwp_wait(lp) == 0)
		tsleep(lp, 0, "lwpreap", 1);
	lwp_dispose(lp);
}

static void
deadlwp_init(void)
{
	int cpu;

	for (cpu = 0; cpu < ncpus; cpu++) {
		LIST_INIT(&deadlwp_list[cpu]);
		deadlwp_task[cpu] = kmalloc(sizeof(*deadlwp_task[cpu]), M_DEVBUF, M_WAITOK);
		TASK_INIT(deadlwp_task[cpu], 0, reaplwps, &deadlwp_list[cpu]);
	}
}

SYSINIT(deadlwpinit, SI_SUB_CONFIGURE, SI_ORDER_ANY, deadlwp_init, NULL);
