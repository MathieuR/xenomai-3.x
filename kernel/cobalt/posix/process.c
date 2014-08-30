/*
 * Copyright (C) 2001-2014 Philippe Gerum <rpm@xenomai.org>.
 * Copyright (C) 2001-2014 The Xenomai project <http://www.xenomai.org>
 * Copyright (C) 2006 Gilles Chanteperdrix <gilles.chanteperdrix@xenomai.org>
 *
 * SMP support Copyright (C) 2004 The HYADES project <http://www.hyades-itea.org>
 * RTAI/fusion Copyright (C) 2004 The RTAI project <http://www.rtai.org>
 *
 * Xenomai is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * Xenomai is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Xenomai; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */
#include <stdarg.h>
#include <linux/unistd.h>
#include <linux/wait.h>
#include <linux/semaphore.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/file.h>
#include <linux/ptrace.h>
#include <linux/vmalloc.h>
#include <linux/completion.h>
#include <linux/kallsyms.h>
#include <linux/signal.h>
#include <linux/ipipe.h>
#include <linux/ipipe_tickdev.h>
#include <cobalt/kernel/sched.h>
#include <cobalt/kernel/heap.h>
#include <cobalt/kernel/synch.h>
#include <cobalt/kernel/clock.h>
#include <cobalt/kernel/ppd.h>
#include <cobalt/kernel/trace.h>
#include <cobalt/kernel/stat.h>
#include <cobalt/kernel/ppd.h>
#include <cobalt/kernel/vdso.h>
#include <cobalt/kernel/thread.h>
#include <cobalt/uapi/signal.h>
#include <cobalt/uapi/syscall.h>
#include <trace/events/cobalt-core.h>
#include <rtdm/fd.h>
#include <asm/xenomai/features.h>
#include <asm/xenomai/syscall.h>
#include <asm-generic/xenomai/mayday.h>
#include "../debug.h"
#include "internal.h"
#include "thread.h"
#include "sched.h"
#include "mutex.h"
#include "cond.h"
#include "mqueue.h"
#include "sem.h"
#include "signal.h"
#include "timer.h"
#include "monitor.h"
#include "clock.h"
#include "event.h"
#include "timerfd.h"
#include "io.h"

static int gid_arg = -1;
module_param_named(allowed_group, gid_arg, int, 0644);
MODULE_PARM_DESC(allowed_group, "GID of the group with access to Xenomai services");

static DEFINE_MUTEX(personality_lock);

static struct hlist_head *process_hash;
DEFINE_PRIVATE_XNLOCK(process_hash_lock);
#define PROCESS_HASH_SIZE 13

struct xnthread_personality *cobalt_personalities[NR_PERSONALITIES];

static void *mayday_page;

static struct xnsynch yield_sync;

static unsigned __attribute__((pure)) process_hash_crunch(struct mm_struct *mm)
{
	unsigned long hash = ((unsigned long)mm - PAGE_OFFSET) / sizeof(*mm);
	return hash % PROCESS_HASH_SIZE;
}

static struct cobalt_process *__process_hash_search(struct mm_struct *mm)
{
	unsigned int bucket = process_hash_crunch(mm);
	struct cobalt_process *p;

	hlist_for_each_entry(p, &process_hash[bucket], hlink)
		if (p->mm == mm)
			return p;
	
	return NULL;
}

static int process_hash_enter(struct cobalt_process *p)
{
	struct mm_struct *mm = current->mm;
	unsigned int bucket = process_hash_crunch(mm);
	int err;
	spl_t s;

	xnlock_get_irqsave(&process_hash_lock, s);
	if (__process_hash_search(mm)) {
		err = -EBUSY;
		goto out;
	}

	p->mm = mm;
	hlist_add_head(&p->hlink, &process_hash[bucket]);
	err = 0;
  out:
	xnlock_put_irqrestore(&process_hash_lock, s);
	return err;
}

static void process_hash_remove(struct cobalt_process *p)
{
	spl_t s;

	xnlock_get_irqsave(&process_hash_lock, s);
	if (p->mm)
		hlist_del(&p->hlink);
	xnlock_put_irqrestore(&process_hash_lock, s);
}

struct cobalt_process *cobalt_search_process(struct mm_struct *mm)
{
	struct cobalt_process *process;
	spl_t s;
	
	xnlock_get_irqsave(&process_hash_lock, s);
	process = __process_hash_search(mm);
	xnlock_put_irqrestore(&process_hash_lock, s);
	
	return process;
}

static void *lookup_context(int xid)
{
	struct cobalt_process *process = cobalt_current_process();
	void *priv = NULL;
	spl_t s;

	xnlock_get_irqsave(&process_hash_lock, s);
	/*
	 * First try matching the process context attached to the
	 * (usually main) thread which issued sc_cobalt_bind. If not
	 * found, try matching by mm context, which should point us
	 * back to the latter. If none match, then the current process
	 * is unbound.
	 */
	if (process == NULL && current->mm)
		process = __process_hash_search(current->mm);
	if (process)
		priv = process->priv[xid];

	xnlock_put_irqrestore(&process_hash_lock, s);

	return priv;
}

static int enter_personality(struct cobalt_process *process,
			     struct xnthread_personality *personality)
{
	if (personality->module && !try_module_get(personality->module))
		return -EAGAIN;

	__set_bit(personality->xid, &process->permap);
	atomic_inc(&personality->refcnt);

	return 0;
}

static void leave_personality(struct cobalt_process *process,
			      struct xnthread_personality *personality)
{
	__clear_bit(personality->xid, &process->permap);
	atomic_dec(&personality->refcnt);
	XENO_ASSERT(NUCLEUS, atomic_read(&personality->refcnt) >= 0);
	if (personality->module)
		module_put(personality->module);
}

static void remove_process(struct cobalt_process *process)
{
	struct xnthread_personality *personality;
	void *priv;
	int xid;

	mutex_lock(&personality_lock);

	for (xid = NR_PERSONALITIES - 1; xid >= 0; xid--) {
		if (!__test_and_clear_bit(xid, &process->permap))
			continue;
		personality = cobalt_personalities[xid];
		priv = process->priv[xid];
		/*
		 * CAUTION: process potentially refers to stale memory
		 * upon return from detach_process() for the Cobalt
		 * personality, so don't dereference it afterwards.
		 */
		if (priv) {
			process->priv[xid] = NULL;
			personality->ops.detach_process(priv);
			leave_personality(process, personality);
		}
	}

	cobalt_set_process(NULL);

	mutex_unlock(&personality_lock);
}

static void post_ppd_release(struct xnheap *h)
{
	struct cobalt_process *process;

	process = container_of(h, struct cobalt_process, sys_ppd.sem_heap);
	kfree(process);
}

static inline char *get_exe_path(struct task_struct *p)
{
	struct file *exe_file;
	char *pathname, *buf;
	struct mm_struct *mm;
	struct path path;

	/*
	 * PATH_MAX is fairly large, and in any case won't fit on the
	 * caller's stack happily; since we are mapping a shadow,
	 * which is a heavyweight operation anyway, let's pick the
	 * memory from the page allocator.
	 */
	buf = (char *)__get_free_page(GFP_TEMPORARY);
	if (buf == NULL)
		return ERR_PTR(-ENOMEM);

	mm = get_task_mm(p);
	if (mm == NULL) {
		pathname = "vmlinux";
		goto copy;	/* kernel thread */
	}

	exe_file = get_mm_exe_file(mm);
	mmput(mm);
	if (exe_file == NULL) {
		pathname = ERR_PTR(-ENOENT);
		goto out;	/* no luck. */
	}

	path = exe_file->f_path;
	path_get(&exe_file->f_path);
	fput(exe_file);
	pathname = d_path(&path, buf, PATH_MAX);
	path_put(&path);
	if (IS_ERR(pathname))
		goto out;	/* mmmh... */
copy:
	/* caution: d_path() may start writing anywhere in the buffer. */
	pathname = kstrdup(pathname, GFP_KERNEL);
out:
	free_page((unsigned long)buf);

	return pathname;
}

static inline int raise_cap(int cap)
{
	struct cred *new;

	new = prepare_creds();
	if (new == NULL)
		return -ENOMEM;

	cap_raise(new->cap_effective, cap);

	return commit_creds(new);
}

static int bind_personality(struct xnthread_personality *personality)
{
	struct cobalt_process *process;
	void *priv;
	int ret;

	/*
	 * We also check capabilities for stacking a Cobalt extension,
	 * in case the process dropped the supervisor privileges after
	 * a successful initial binding to the Cobalt interface.
	 */
	if (!capable(CAP_SYS_NICE) &&
	    (gid_arg == -1 || !in_group_p(KGIDT_INIT(gid_arg))))
		return -EPERM;
	/*
	 * Protect from the same process binding to the same interface
	 * several times.
	 */
	priv = lookup_context(personality->xid);
	if (priv)
		return 0;

	priv = personality->ops.attach_process();
	if (IS_ERR(priv))
		return PTR_ERR(priv);

	process = cobalt_current_process();
	/*
	 * We are still covered by the personality_lock, so we may
	 * safely bump the module refcount after the attach handler
	 * has returned.
	 */
	ret = enter_personality(process, personality);
	if (ret) {
		personality->ops.detach_process(priv);
		return ret;
	}

	process->priv[personality->xid] = priv;

	raise_cap(CAP_SYS_NICE);
	raise_cap(CAP_IPC_LOCK);
	raise_cap(CAP_SYS_RAWIO);

	return 0;
}

int cobalt_bind_personality(unsigned int magic)
{
	struct xnthread_personality *personality;
	int xid, ret = -ESRCH;

	mutex_lock(&personality_lock);

	for (xid = 1; xid < NR_PERSONALITIES; xid++) {
		personality = cobalt_personalities[xid];
		if (personality && personality->magic == magic) {
			ret = bind_personality(personality);
			break;
		}
	}

	mutex_unlock(&personality_lock);

	return ret ?: xid;
}

int cobalt_bind_core(void)
{
	int ret;

	mutex_lock(&personality_lock);
	ret = bind_personality(&cobalt_personality);
	mutex_unlock(&personality_lock);

	return ret;
}

/**
 * @fn int cobalt_register_personality(struct xnthread_personality *personality)
 * @internal
 * @brief Register a new interface personality.
 *
 * - personality->ops.attach_process() is called when a user-space
 *   process binds to the personality, on behalf of one of its
 *   threads. The attach_process() handler may return:
 *
 *   . an opaque pointer, representing the context of the calling
 *   process for this personality;
 *
 *   . a NULL pointer, meaning that no per-process structure should be
 *   attached to this process for this personality;
 *
 *   . ERR_PTR(negative value) indicating an error, the binding
 *   process will then abort.
 *
 * - personality->ops.detach_process() is called on behalf of an
 *   exiting user-space process which has previously attached to the
 *   personality. This handler is passed a pointer to the per-process
 *   data received earlier from the ops->attach_process() handler.
 *
 * @return the personality (extension) identifier.
 *
 * @note cobalt_get_context() is NULL when ops.detach_process() is
 * invoked for the personality the caller detaches from.
 *
 * @coretags{secondary-only}
 */
int cobalt_register_personality(struct xnthread_personality *personality)
{
	int xid;

	mutex_lock(&personality_lock);

	for (xid = 0; xid < NR_PERSONALITIES; xid++) {
		if (cobalt_personalities[xid] == NULL) {
			personality->xid = xid;
			atomic_set(&personality->refcnt, 0);
			cobalt_personalities[xid] = personality;
			goto out;
		}
	}

	xid = -EAGAIN;
out:
	mutex_unlock(&personality_lock);

	return xid;
}
EXPORT_SYMBOL_GPL(cobalt_register_personality);

/*
 * @brief Unregister an interface personality.
 *
 * @coretags{secondary-only}
 */
int cobalt_unregister_personality(int xid)
{
	struct xnthread_personality *personality;
	int ret = 0;

	if (xid < 0 || xid >= NR_PERSONALITIES)
		return -EINVAL;

	mutex_lock(&personality_lock);

	personality = cobalt_personalities[xid];
	if (atomic_read(&personality->refcnt) > 0)
		ret = -EBUSY;
	else
		cobalt_personalities[xid] = NULL;

	mutex_unlock(&personality_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(cobalt_unregister_personality);

/**
 * Stack a new personality over Cobalt for the current thread.
 *
 * This service registers the current thread as a member of the
 * additional personality identified by @a xid. If the current thread
 * is already assigned this personality, the call returns successfully
 * with no effect.
 *
 * @param xid the identifier of the additional personality.
 *
 * @return A handle to the previous personality. The caller should
 * save this handle for unstacking @a xid when applicable via a call
 * to cobalt_pop_personality().
 *
 * @coretags{secondary-only}
 */
struct xnthread_personality *
cobalt_push_personality(int xid)
{
	struct ipipe_threadinfo *p = ipipe_current_threadinfo();
	struct xnthread_personality *prev, *next;
	struct xnthread *thread = p->thread;

	secondary_mode_only();

	mutex_lock(&personality_lock);

	if (xid < 0 || xid >= NR_PERSONALITIES ||
	    p->process == NULL || !test_bit(xid, &p->process->permap)) {
		mutex_unlock(&personality_lock);
		return NULL;
	}

	next = cobalt_personalities[xid];
	prev = thread->personality;
	if (next == prev) {
		mutex_unlock(&personality_lock);
		return prev;
	}

	thread->personality = next;
	mutex_unlock(&personality_lock);
	xnthread_run_handler(thread, map_thread);

	return prev;
}
EXPORT_SYMBOL_GPL(cobalt_push_personality);

/**
 * Pop the topmost personality from the current thread.
 *
 * This service pops the topmost personality off the current thread.
 *
 * @param prev the previous personality which was returned by the
 * latest call to cobalt_push_personality() for the current thread.
 *
 * @coretags{secondary-only}
 */
void cobalt_pop_personality(struct xnthread_personality *prev)
{
	struct ipipe_threadinfo *p = ipipe_current_threadinfo();
	struct xnthread *thread = p->thread;

	secondary_mode_only();
	thread->personality = prev;
}
EXPORT_SYMBOL_GPL(cobalt_pop_personality);

/**
 * Return the per-process data attached to the calling user process.
 *
 * This service returns the per-process data attached to the calling
 * user process for the personality whose xid is @a xid.
 *
 * The per-process data was obtained from the ->attach_process()
 * handler defined for the personality @a xid refers to.
 *
 * See cobalt_register_personality() documentation for information on
 * the way to attach a per-process data to a process.
 *
 * @param xid the personality identifier.
 *
 * @return the per-process data if the current context is a user-space
 * process; @return NULL otherwise. As a special case,
 * cobalt_get_context(0) returns the current Cobalt process
 * descriptor, which is strictly identical to calling
 * cobalt_current_process().
 *
 * @coretags{task-unrestricted}
 */
void *cobalt_get_context(int xid)
{
	return lookup_context(xid);
}
EXPORT_SYMBOL_GPL(cobalt_get_context);

int cobalt_yield(xnticks_t min, xnticks_t max)
{
	xnticks_t start;
	int ret;

	start = xnclock_read_monotonic(&nkclock);
	max += start;
	min += start;

	do {
		ret = xnsynch_sleep_on(&yield_sync, max, XN_ABSOLUTE);
		if (ret & XNBREAK)
			return -EINTR;
	} while (ret == 0 && xnclock_read_monotonic(&nkclock) < min);

	return 0;
}
EXPORT_SYMBOL_GPL(cobalt_yield);

static inline void init_uthread_info(struct xnthread *thread)
{
	struct ipipe_threadinfo *p;

	p = ipipe_current_threadinfo();
	p->thread = thread;
	p->process = cobalt_search_process(current->mm);
}

static inline void clear_threadinfo(void)
{
	struct ipipe_threadinfo *p = ipipe_current_threadinfo();
	p->thread = NULL;
	p->process = NULL;
}

#ifdef CONFIG_MMU

static inline int disable_ondemand_memory(void)
{
	struct task_struct *p = current;
	siginfo_t si;

	if ((p->mm->def_flags & VM_LOCKED) == 0) {
		memset(&si, 0, sizeof(si));
		si.si_signo = SIGDEBUG;
		si.si_code = SI_QUEUE;
		si.si_int = SIGDEBUG_NOMLOCK | sigdebug_marker;
		send_sig_info(SIGDEBUG, &si, p);
		return 0;
	}

	return __ipipe_disable_ondemand_mappings(p);
}

#else /* !CONFIG_MMU */

static inline int disable_ondemand_memory(void)
{
	return 0;
}

#endif /* !CONFIG_MMU */

/**
 * @fn int cobalt_map_user(struct xnthread *thread, unsigned long __user *u_window_offset)
 * @internal
 * @brief Create a shadow thread context over a user task.
 *
 * This call maps a Xenomai thread to the current regular Linux task
 * running in userland.  The priority and scheduling class of the
 * underlying Linux task are not affected; it is assumed that the
 * interface library did set them appropriately before issuing the
 * shadow mapping request.
 *
 * @param thread The descriptor address of the new shadow thread to be
 * mapped to current. This descriptor must have been previously
 * initialized by a call to xnthread_init().
 *
 * @param u_window_offset will receive the offset of the per-thread
 * "u_window" structure in the process shared heap associated to @a
 * thread. This structure reflects thread state information visible
 * from userland through a shared memory window.
 *
 * @return 0 is returned on success. Otherwise:
 *
 * - -EINVAL is returned if the thread control block does not bear the
 * XNUSER bit.
 *
 * - -EBUSY is returned if either the current Linux task or the
 * associated shadow thread is already involved in a shadow mapping.
 *
 * @coretags{secondary-only}
 */
int cobalt_map_user(struct xnthread *thread,
		    unsigned long __user *u_window_offset)
{
	struct xnthread_user_window *u_window;
	struct xnthread_start_attr attr;
	struct xnsys_ppd *sys_ppd;
	struct xnheap *sem_heap;
	int ret;

	if (!xnthread_test_state(thread, XNUSER))
		return -EINVAL;

	if (xnthread_current() || xnthread_test_state(thread, XNMAPPED))
		return -EBUSY;

	if (!access_wok(u_window_offset, sizeof(*u_window_offset)))
		return -EFAULT;

	ret = disable_ondemand_memory();
	if (ret)
		return ret;

	sys_ppd = cobalt_ppd_get(0);
	sem_heap = &sys_ppd->sem_heap;
	u_window = xnheap_alloc(sem_heap, sizeof(*u_window));
	if (u_window == NULL)
		return -ENOMEM;

	thread->u_window = u_window;
	__xn_put_user(xnheap_mapped_offset(sem_heap, u_window), u_window_offset);
	xnthread_pin_initial(thread);

	trace_cobalt_shadow_map(thread);

	/*
	 * CAUTION: we enable the pipeline notifier only when our
	 * shadow TCB is consistent, so that we won't trigger false
	 * positive in debug code from handle_schedule_event() and
	 * friends.
	 */
	xnthread_init_shadow_tcb(thread, current);
	xnthread_suspend(thread, XNRELAX, XN_INFINITE, XN_RELATIVE, NULL);
	init_uthread_info(thread);
	xnthread_set_state(thread, XNMAPPED);
	xndebug_shadow_init(thread);
	atomic_inc(&sys_ppd->refcnt);
	/*
	 * ->map_thread() handler is invoked after the TCB is fully
	 * built, and when we know for sure that current will go
	 * through our task-exit handler, because it has a shadow
	 * extension and I-pipe notifications will soon be enabled for
	 * it.
	 */
	xnthread_run_handler(thread, map_thread);
	ipipe_enable_notifier(current);

	attr.mode = 0;
	attr.entry = NULL;
	attr.cookie = NULL;
	ret = xnthread_start(thread, &attr);
	if (ret)
		return ret;

	xnthread_sync_window(thread);

	xntrace_pid(xnthread_host_pid(thread),
		    xnthread_current_priority(thread));

	return 0;
}

static inline int handle_exception(struct ipipe_trap_data *d)
{
	struct xnthread *thread;
	struct xnsched *sched;

	sched = xnsched_current();
	thread = sched->curr;

	if (xnthread_test_state(thread, XNROOT))
		return 0;

	trace_cobalt_thread_fault(thread, d);

	if (xnarch_fault_fpu_p(d)) {
#ifdef CONFIG_XENO_HW_FPU
		/* FPU exception received in primary mode. */
		if (xnarch_handle_fpu_fault(sched->fpuholder, thread, d)) {
			sched->fpuholder = thread;
			return 1;
		}
#endif /* CONFIG_XENO_HW_FPU */
		print_symbol("invalid use of FPU in Xenomai context at %s\n",
			     xnarch_fault_pc(d));
	}

	/*
	 * If we experienced a trap on behalf of a shadow thread
	 * running in primary mode, move it to the Linux domain,
	 * leaving the kernel process the exception.
	 */
	thread->regs = xnarch_fault_regs(d);

#if XENO_DEBUG(NUCLEUS)
	if (!user_mode(d->regs)) {
		xntrace_panic_freeze();
		printk(XENO_WARN
		       "switching %s to secondary mode after exception #%u in "
		       "kernel-space at 0x%lx (pid %d)\n", thread->name,
		       xnarch_fault_trap(d),
		       xnarch_fault_pc(d),
		       xnthread_host_pid(thread));
		xntrace_panic_dump();
	} else if (xnarch_fault_notify(d)) /* Don't report debug traps */
		printk(XENO_WARN
		       "switching %s to secondary mode after exception #%u from "
		       "user-space at 0x%lx (pid %d)\n", thread->name,
		       xnarch_fault_trap(d),
		       xnarch_fault_pc(d),
		       xnthread_host_pid(thread));
#endif /* XENO_DEBUG(NUCLEUS) */

	if (xnarch_fault_pf_p(d))
		/*
		 * The page fault counter is not SMP-safe, but it's a
		 * simple indicator that something went wrong wrt
		 * memory locking anyway.
		 */
		xnstat_counter_inc(&thread->stat.pf);

	xnthread_relax(xnarch_fault_notify(d), SIGDEBUG_MIGRATE_FAULT);

	return 0;
}

static int handle_mayday_event(struct pt_regs *regs)
{
	struct xnthread *thread = xnthread_current();
	struct xnarchtcb *tcb = xnthread_archtcb(thread);
	struct xnsys_ppd *sys_ppd;

	XENO_BUGON(NUCLEUS, !xnthread_test_state(thread, XNUSER));

	/* We enter the mayday handler with hw IRQs off. */
	sys_ppd = cobalt_ppd_get(0);

	xnarch_handle_mayday(tcb, regs, sys_ppd->mayday_addr);

	return KEVENT_PROPAGATE;
}

int ipipe_trap_hook(struct ipipe_trap_data *data)
{
	if (data->exception == IPIPE_TRAP_MAYDAY)
		return handle_mayday_event(data->regs);

	/*
	 * No migration is possible on behalf of the head domain, so
	 * the following access is safe.
	 */
	__this_cpu_ptr(&xnarch_percpu_machdata)->faults[data->exception]++;

	if (handle_exception(data))
		return KEVENT_STOP;

	/*
	 * CAUTION: access faults must be propagated downstream
	 * whichever domain caused them, so that we don't spuriously
	 * raise a fatal error when some Linux fixup code is available
	 * to recover from the fault.
	 */
	return KEVENT_PROPAGATE;
}

#ifdef CONFIG_MMU

#define mayday_unmapped_area  NULL

#else /* !CONFIG_MMU */

static unsigned long mayday_unmapped_area(struct file *file,
					  unsigned long addr,
					  unsigned long len,
					  unsigned long pgoff,
					  unsigned long flags)
{
	return (unsigned long)mayday_page;
}

#endif /* !CONFIG_MMU */

static int mayday_map(struct file *filp, struct vm_area_struct *vma)
{
	vma->vm_pgoff = (unsigned long)mayday_page >> PAGE_SHIFT;
	return xnheap_remap_vm_page(vma, vma->vm_start,
				    (unsigned long)mayday_page);
}

static struct file_operations mayday_fops = {
	.mmap = mayday_map,
	.get_unmapped_area = mayday_unmapped_area
};

static unsigned long map_mayday_page(struct task_struct *p)
{
	const struct file_operations *old_fops;
	unsigned long u_addr;
	struct file *filp;

	filp = filp_open(XNHEAP_DEV_NAME, O_RDONLY, 0);
	if (IS_ERR(filp))
		return 0;

	old_fops = filp->f_op;
	filp->f_op = &mayday_fops;
	u_addr = vm_mmap(filp, 0, PAGE_SIZE, PROT_EXEC|PROT_READ, MAP_SHARED, 0);
	filp->f_op = (typeof(filp->f_op))old_fops;
	filp_close(filp, p->files);

	return IS_ERR_VALUE(u_addr) ? 0UL : u_addr;
}

static inline int mayday_init_page(void)
{
	mayday_page = vmalloc(PAGE_SIZE);
	if (mayday_page == NULL) {
		printk(XENO_ERR "can't alloc MAYDAY page\n");
		return -ENOMEM;
	}

	xnarch_setup_mayday_page(mayday_page);

	return 0;
}

static inline void mayday_cleanup_page(void)
{
	if (mayday_page)
		vfree(mayday_page);
}

#ifdef CONFIG_SMP

static int handle_setaffinity_event(struct ipipe_cpu_migration_data *d)
{
	struct task_struct *p = d->task;
	struct xnthread *thread;
	struct xnsched *sched;
	spl_t s;

	thread = xnthread_from_task(p);
	if (thread == NULL)
		return KEVENT_PROPAGATE;

	/*
	 * The CPU affinity mask is always controlled from secondary
	 * mode, therefore we progagate any change to the real-time
	 * affinity mask accordingly.
	 */
	xnlock_get_irqsave(&nklock, s);
	cpus_and(thread->affinity, p->cpus_allowed, nkaffinity);
	xnthread_run_handler_stack(thread, move_thread, d->dest_cpu);
	xnlock_put_irqrestore(&nklock, s);

	/*
	 * If kernel and real-time CPU affinity sets are disjoints,
	 * there might be problems ahead for this thread next time it
	 * moves back to primary mode, if it ends up switching to an
	 * unsupported CPU.
	 *
	 * Otherwise, check_affinity() will extend the CPU affinity if
	 * possible, fixing up the thread's affinity mask. This means
	 * that a thread might be allowed to run with a broken
	 * (i.e. fully cleared) affinity mask until it leaves primary
	 * mode then switches back to it, in SMP configurations.
	 */
	if (cpus_empty(thread->affinity))
		printk(XENO_WARN "thread %s[%d] changed CPU affinity inconsistently\n",
		       thread->name, xnthread_host_pid(thread));
	else {
		xnlock_get_irqsave(&nklock, s);
		/*
		 * Threads running in primary mode may NOT be forcibly
		 * migrated by the regular kernel to another CPU. Such
		 * migration would have to wait until the thread
		 * switches back from secondary mode at some point
		 * later, or issues a call to xnthread_migrate().
		 */
		if (!xnthread_test_state(thread, XNMIGRATE) &&
		    xnthread_test_state(thread, XNTHREAD_BLOCK_BITS)) {
			sched = xnsched_struct(d->dest_cpu);
			xnthread_migrate_passive(thread, sched);
		}
		xnlock_put_irqrestore(&nklock, s);
	}

	return KEVENT_PROPAGATE;
}

static inline void check_affinity(struct task_struct *p) /* nklocked, IRQs off */
{
	struct xnthread *thread = xnthread_from_task(p);
	struct xnsched *sched;
	int cpu = task_cpu(p);

	/*
	 * If the task moved to another CPU while in secondary mode,
	 * migrate the companion Xenomai shadow to reflect the new
	 * situation.
	 *
	 * In the weirdest case, the thread is about to switch to
	 * primary mode on a CPU Xenomai shall not use. This is
	 * hopeless, whine and kill that thread asap.
	 */
	if (!xnsched_supported_cpu(cpu)) {
		printk(XENO_WARN "thread %s[%d] switched to non-rt CPU%d, aborted.\n",
		       thread->name, xnthread_host_pid(thread), cpu);
		/*
		 * Can't call xnthread_cancel() from a migration
		 * point, that would break. Since we are on the wakeup
		 * path to hardening, just raise XNCANCELD to catch it
		 * in xnthread_harden().
		 */
		xnthread_set_info(thread, XNCANCELD);
		return;
	}

	sched = xnsched_struct(cpu);
	if (sched == thread->sched)
		return;

	/*
	 * The current thread moved to a supported real-time CPU,
	 * which is not part of its original affinity mask
	 * though. Assume user wants to extend this mask.
	 */
	if (!cpu_isset(cpu, thread->affinity))
		cpu_set(cpu, thread->affinity);

	xnthread_migrate_passive(thread, sched);
}

#else /* !CONFIG_SMP */

struct ipipe_cpu_migration_data;

static int handle_setaffinity_event(struct ipipe_cpu_migration_data *d)
{
	return KEVENT_PROPAGATE;
}

static inline void check_affinity(struct task_struct *p) { }

#endif /* CONFIG_SMP */

void ipipe_migration_hook(struct task_struct *p) /* hw IRQs off */
{
	struct xnthread *thread = xnthread_from_task(p);

	/*
	 * We fire the handler before the thread is migrated, so that
	 * thread->sched does not change between paired invocations of
	 * relax_thread/harden_thread handlers.
	 */
	xnlock_get(&nklock);
	xnthread_run_handler_stack(thread, harden_thread);
	check_affinity(p);
	xnthread_resume(thread, XNRELAX);
	xnlock_put(&nklock);

	xnsched_run();
}

#ifdef CONFIG_XENO_OPT_HOSTRT

static IPIPE_DEFINE_SPINLOCK(__hostrtlock);

static int handle_hostrt_event(struct ipipe_hostrt_data *hostrt)
{
	unsigned long flags;
	urwstate_t tmp;

	/*
	 * The locking strategy is twofold:
	 * - The spinlock protects against concurrent updates from within the
	 *   Linux kernel and against preemption by Xenomai
	 * - The unsynced R/W block is for lockless read-only access.
	 */
	spin_lock_irqsave(&__hostrtlock, flags);

	unsynced_write_block(&tmp, &nkvdso->hostrt_data.lock) {
		nkvdso->hostrt_data.live = 1;
		nkvdso->hostrt_data.cycle_last = hostrt->cycle_last;
		nkvdso->hostrt_data.mask = hostrt->mask;
		nkvdso->hostrt_data.mult = hostrt->mult;
		nkvdso->hostrt_data.shift = hostrt->shift;
		nkvdso->hostrt_data.wall_time_sec = hostrt->wall_time_sec;
		nkvdso->hostrt_data.wall_time_nsec = hostrt->wall_time_nsec;
		nkvdso->hostrt_data.wall_to_monotonic = hostrt->wall_to_monotonic;
	}

	spin_unlock_irqrestore(&__hostrtlock, flags);

	return KEVENT_PROPAGATE;
}

static inline void init_hostrt(void)
{
	unsynced_rw_init(&nkvdso->hostrt_data.lock);
	nkvdso->hostrt_data.live = 0;
}

#else /* !CONFIG_XENO_OPT_HOSTRT */

struct ipipe_hostrt_data;

static inline int handle_hostrt_event(struct ipipe_hostrt_data *hostrt)
{
	return KEVENT_PROPAGATE;
}

static inline void init_hostrt(void) { }

#endif /* !CONFIG_XENO_OPT_HOSTRT */

static inline void lock_timers(void)
{
	smp_mb__before_atomic_inc();
	atomic_inc(&nkclklk);
	smp_mb__after_atomic_inc();
}

static inline void unlock_timers(void)
{
	XENO_BUGON(NUCLEUS, atomic_read(&nkclklk) == 0);
	smp_mb__before_atomic_dec();
	atomic_dec(&nkclklk);
	smp_mb__after_atomic_dec();
}

static int handle_taskexit_event(struct task_struct *p) /* p == current */
{
	struct xnsys_ppd *sys_ppd;
	struct xnthread *thread;

	/*
	 * We are called for both kernel and user shadows over the
	 * root thread.
	 */
	secondary_mode_only();

	thread = xnthread_current();
	XENO_BUGON(NUCLEUS, thread == NULL);
	trace_cobalt_shadow_unmap(thread);

	if (xnthread_test_state(thread, XNDEBUG))
		unlock_timers();

	xnthread_run_handler_stack(thread, exit_thread);
	/* Waiters will receive EIDRM */
	xnsynch_destroy(&thread->join_synch);
	xnsched_run();

	if (xnthread_test_state(thread, XNUSER)) {
		sys_ppd = cobalt_ppd_get(0);
		xnheap_free(&sys_ppd->sem_heap, thread->u_window);
		thread->u_window = NULL;
		if (atomic_dec_and_test(&sys_ppd->refcnt))
			remove_process(cobalt_current_process());
	}

	/*
	 * __xnthread_cleanup() -> ... -> finalize_thread
	 * handler. From that point, the TCB is dropped. Be careful of
	 * not treading on stale memory within @thread.
	 */
	__xnthread_cleanup(thread);

	clear_threadinfo();

	return KEVENT_PROPAGATE;
}

static inline void signal_yield(void)
{
	spl_t s;

	if (!xnsynch_pended_p(&yield_sync))
		return;

	xnlock_get_irqsave(&nklock, s);
	if (xnsynch_pended_p(&yield_sync)) {
		xnsynch_flush(&yield_sync, 0);
		xnsched_run();
	}
	xnlock_put_irqrestore(&nklock, s);
}

static int handle_schedule_event(struct task_struct *next_task)
{
	struct task_struct *prev_task;
	struct xnthread *prev, *next;
	sigset_t pending;

	signal_yield();

	prev_task = current;
	prev = xnthread_from_task(prev_task);
	next = xnthread_from_task(next_task);
	if (next == NULL)
		goto out;

	/*
	 * Check whether we need to unlock the timers, each time a
	 * Linux task resumes from a stopped state, excluding tasks
	 * resuming shortly for entering a stopped state asap due to
	 * ptracing. To identify the latter, we need to check for
	 * SIGSTOP and SIGINT in order to encompass both the NPTL and
	 * LinuxThreads behaviours.
	 */
	if (xnthread_test_state(next, XNDEBUG)) {
		if (signal_pending(next_task)) {
			/*
			 * Do not grab the sighand lock here: it's
			 * useless, and we already own the runqueue
			 * lock, so this would expose us to deadlock
			 * situations on SMP.
			 */
			sigorsets(&pending,
				  &next_task->pending.signal,
				  &next_task->signal->shared_pending.signal);
			if (sigismember(&pending, SIGSTOP) ||
			    sigismember(&pending, SIGINT))
				goto no_ptrace;
		}
		xnthread_clear_state(next, XNDEBUG);
		unlock_timers();
	}

no_ptrace:
	if (XENO_DEBUG(NUCLEUS)) {
		if (!xnthread_test_state(next, XNRELAX)) {
			xntrace_panic_freeze();
			show_stack(xnthread_host_task(next), NULL);
			xnsys_fatal
				("hardened thread %s[%d] running in Linux domain?! "
				 "(status=0x%lx, sig=%d, prev=%s[%d])",
				 next->name, next_task->pid, xnthread_state_flags(next),
				 signal_pending(next_task), prev_task->comm, prev_task->pid);
		} else if (!(next_task->ptrace & PT_PTRACED) &&
			   /*
			    * Allow ptraced threads to run shortly in order to
			    * properly recover from a stopped state.
			    */
			   !xnthread_test_state(next, XNDORMANT)
			   && xnthread_test_state(next, XNPEND)) {
			xntrace_panic_freeze();
			show_stack(xnthread_host_task(next), NULL);
			xnsys_fatal
				("blocked thread %s[%d] rescheduled?! "
				 "(status=0x%lx, sig=%d, prev=%s[%d])",
				 next->name, next_task->pid, xnthread_state_flags(next),
				 signal_pending(next_task), prev_task->comm, prev_task->pid);
		}
	}
out:
	return KEVENT_PROPAGATE;
}

static int handle_sigwake_event(struct task_struct *p)
{
	struct xnthread *thread;
	sigset_t pending;
	spl_t s;

	thread = xnthread_from_task(p);
	if (thread == NULL)
		return KEVENT_PROPAGATE;

	xnlock_get_irqsave(&nklock, s);

	/*
	 * CAUTION: __TASK_TRACED is not set in p->state yet. This
	 * state bit will be set right after we return, when the task
	 * is woken up.
	 */
	if ((p->ptrace & PT_PTRACED) && !xnthread_test_state(thread, XNDEBUG)) {
		/* We already own the siglock. */
		sigorsets(&pending,
			  &p->pending.signal,
			  &p->signal->shared_pending.signal);

		if (sigismember(&pending, SIGTRAP) ||
		    sigismember(&pending, SIGSTOP)
		    || sigismember(&pending, SIGINT)) {
			xnthread_set_state(thread, XNDEBUG);
			lock_timers();
		}
	}

	if (xnthread_test_state(thread, XNRELAX)) {
		xnlock_put_irqrestore(&nklock, s);
		return KEVENT_PROPAGATE;
	}

	/*
	 * If kicking a shadow thread in primary mode, make sure Linux
	 * won't schedule in its mate under our feet as a result of
	 * running signal_wake_up(). The Xenomai scheduler must remain
	 * in control for now, until we explicitly relax the shadow
	 * thread to allow for processing the pending signals. Make
	 * sure we keep the additional state flags unmodified so that
	 * we don't break any undergoing ptrace.
	 */
	if (p->state & (TASK_INTERRUPTIBLE|TASK_UNINTERRUPTIBLE))
		set_task_state(p, p->state | TASK_NOWAKEUP);

	__xnthread_kick(thread);

	xnsched_run();

	xnlock_put_irqrestore(&nklock, s);

	return KEVENT_PROPAGATE;
}

static int handle_cleanup_event(struct mm_struct *mm)
{
	struct cobalt_process *old, *process;
	struct xnsys_ppd *sys_ppd;
	struct xnthread *thread;

	/*
	 * We are NOT called for exiting kernel shadows.
	 * cobalt_current_process() is cleared if we get there after
	 * handle_task_exit(), so we need to restore this context
	 * pointer temporarily.
	 */
	process = cobalt_search_process(mm);
	old = cobalt_set_process(process);
	sys_ppd = cobalt_ppd_get(0);
	if (sys_ppd != &__xnsys_global_ppd) {
		/*
		 * Detect a userland shadow running exec(), i.e. still
		 * attached to the current linux task (no prior
		 * clear_threadinfo). In this case, we emulate a task
		 * exit, since the Xenomai binding shall not survive
		 * the exec() syscall. Since the process will keep on
		 * running though, we have to disable the event
		 * notifier manually for it.
		 */
		thread = xnthread_current();
		if (thread && (current->flags & PF_EXITING) == 0) {
			handle_taskexit_event(current);
			ipipe_disable_notifier(current);
		}
		if (atomic_dec_and_test(&sys_ppd->refcnt))
			remove_process(process);
	}

	/*
	 * CAUTION: Do not override a state change caused by
	 * remove_process().
	 */
	if (cobalt_current_process() == process)
		cobalt_set_process(old);

	return KEVENT_PROPAGATE;
}

int ipipe_kevent_hook(int kevent, void *data)
{
	int ret;

	switch (kevent) {
	case IPIPE_KEVT_SCHEDULE:
		ret = handle_schedule_event(data);
		break;
	case IPIPE_KEVT_SIGWAKE:
		ret = handle_sigwake_event(data);
		break;
	case IPIPE_KEVT_EXIT:
		ret = handle_taskexit_event(data);
		break;
	case IPIPE_KEVT_CLEANUP:
		ret = handle_cleanup_event(data);
		break;
	case IPIPE_KEVT_HOSTRT:
		ret = handle_hostrt_event(data);
		break;
	case IPIPE_KEVT_SETAFFINITY:
		ret = handle_setaffinity_event(data);
		break;
	default:
		ret = KEVENT_PROPAGATE;
	}

	return ret;
}

static int attach_process(struct cobalt_process *process)
{
	struct xnsys_ppd *p = &process->sys_ppd;
	char *exe_path;
	int ret;

	ret = xnheap_init_mapped(&p->sem_heap,
				 CONFIG_XENO_OPT_SEM_HEAPSZ * 1024,
				 XNARCH_SHARED_HEAP_FLAGS);
	if (ret)
		return ret;

	xnheap_set_label(&p->sem_heap,
			 "private sem heap [%d]", current->pid);

	p->mayday_addr = map_mayday_page(current);
	if (p->mayday_addr == 0) {
		printk(XENO_WARN
		       "%s[%d] cannot map MAYDAY page\n",
		       current->comm, current->pid);
		ret = -ENOMEM;
		goto fail_mayday;
	}

	exe_path = get_exe_path(current);
	if (IS_ERR(exe_path)) {
		printk(XENO_WARN
		       "%s[%d] can't find exe path\n",
		       current->comm, current->pid);
		exe_path = NULL; /* Not lethal, but weird. */
	}
	p->exe_path = exe_path;
	xntree_init(&p->fds);
	atomic_set(&p->refcnt, 1);

	ret = process_hash_enter(process);
	if (ret)
		goto fail_hash;

	return 0;
fail_hash:
	if (p->exe_path)
		kfree(p->exe_path);
fail_mayday:
	xnheap_destroy_mapped(&p->sem_heap, post_ppd_release, NULL);

	return ret;
}

static void *cobalt_process_attach(void)
{
	struct cobalt_process *process;
	int ret;

	process = kzalloc(sizeof(*process), GFP_KERNEL);
	if (process == NULL)
		return ERR_PTR(-ENOMEM);

	ret = attach_process(process);
	if (ret) {
		kfree(process);
		return ERR_PTR(ret);
	}

	INIT_LIST_HEAD(&process->kqueues.condq);
	INIT_LIST_HEAD(&process->kqueues.mutexq);
	INIT_LIST_HEAD(&process->kqueues.semq);
	INIT_LIST_HEAD(&process->kqueues.threadq);
	INIT_LIST_HEAD(&process->kqueues.monitorq);
	INIT_LIST_HEAD(&process->kqueues.eventq);
	INIT_LIST_HEAD(&process->kqueues.schedq);
	INIT_LIST_HEAD(&process->sigwaiters);
	xntree_init(&process->usems);
	bitmap_fill(process->timers_map, CONFIG_XENO_OPT_NRTIMERS);
	cobalt_set_process(process);

	return process;
}

static void detach_process(struct cobalt_process *process)
{
	struct xnsys_ppd *p = &process->sys_ppd;

	if (p->exe_path)
		kfree(p->exe_path);

	rtdm_fd_cleanup(p);
	process_hash_remove(process);
	/*
	 * CAUTION: the process descriptor might be immediately
	 * released as a result of calling xnheap_destroy_mapped(), so
	 * we must do this last, not to tread on stale memory.
	 */
	xnheap_destroy_mapped(&p->sem_heap, post_ppd_release, NULL);
}

static void cobalt_process_detach(void *arg)
{
	struct cobalt_process *process = arg;

	cobalt_sem_usems_cleanup(process);
	cobalt_timers_cleanup(process);
	cobalt_monitorq_cleanup(&process->kqueues);
	cobalt_semq_cleanup(&process->kqueues);
	cobalt_mutexq_cleanup(&process->kqueues);
	cobalt_condq_cleanup(&process->kqueues);
	cobalt_eventq_cleanup(&process->kqueues);
	cobalt_sched_cleanup(&process->kqueues);
	detach_process(process);
	/*
	 * The cobalt_process descriptor release may be deferred until
	 * the last mapping on the private heap is gone. However, this
	 * is potentially stale memory already.
	 */
}

int cobalt_process_init(void)
{
	unsigned int i, size;
	int ret;

	size = sizeof(*process_hash) * PROCESS_HASH_SIZE;
	process_hash = kmalloc(size, GFP_KERNEL);
	if (process_hash == NULL) {
		printk(XENO_ERR "cannot allocate processes hash table\n");
		return -ENOMEM;
	}

	ret = xndebug_init();
	if (ret)
		goto fail_debug;

	/*
	 * Setup the mayday page early, before userland can mess with
	 * real-time ops.
	 */
	ret = mayday_init_page();
	if (ret)
		goto fail_mayday;

	for (i = 0; i < PROCESS_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&process_hash[i]);

	xnsynch_init(&yield_sync, XNSYNCH_FIFO, NULL);

	ret = cobalt_register_personality(&cobalt_personality);
	if (ret)
		goto fail_register;

	init_hostrt();
	ipipe_set_hooks(ipipe_root_domain, IPIPE_SYSCALL|IPIPE_KEVENT);
	ipipe_set_hooks(&xnsched_realtime_domain, IPIPE_SYSCALL|IPIPE_TRAP);

	return 0;
fail_register:
	xnsynch_destroy(&yield_sync);
	mayday_cleanup_page();
fail_mayday:
	xndebug_cleanup();
fail_debug:
	kfree(process_hash);

	return ret;
}

void cobalt_process_cleanup(void)
{
	cobalt_unregister_personality(cobalt_personality.xid);
	xnsynch_destroy(&yield_sync);
	ipipe_set_hooks(&xnsched_realtime_domain, 0);
	ipipe_set_hooks(ipipe_root_domain, 0);

	mayday_cleanup_page();
	xndebug_cleanup();

	if (process_hash) {
		kfree(process_hash);
		process_hash = NULL;
	}
}

struct xnthread_personality cobalt_personality = {
	.name = "cobalt",
	.magic = 0,
	.ops = {
		.attach_process = cobalt_process_attach,
		.detach_process = cobalt_process_detach,
		.map_thread = cobalt_thread_map,
		.exit_thread = cobalt_thread_exit,
		.finalize_thread = cobalt_thread_finalize,
	},
};
EXPORT_SYMBOL_GPL(cobalt_personality);