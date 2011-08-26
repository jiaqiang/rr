#define _GNU_SOURCE

#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <error.h>
#include <sched.h>
#include <fcntl.h>
#include <signal.h>

#include <linux/perf_event.h>
#include <linux/futex.h>

#include <sys/user.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "write_trace.h"
#include "rec_process_event.h"
#include "rec_sched.h"
#include "recorder.h"

#include "../share/hpc.h"
#include "../share/ipc.h"
#include "../share/sys.h"
#include "../share/util.h"
#include "../share/trace.h"



#define GET_EVENT(status)	 		((0xFF0000 & status) >> 16)


/**
 * Single steps to the next event that must be recorded. This can either be a system call, or reading the time
 * stamp counter (for now)
 */
void goto_next_event_singlestep(struct context* context)
{
	pid_t tid = context->child_tid;

	while (1) {
		int inst_size;
		char* inst =  get_inst(tid, 0, &inst_size);
		if ((strncmp(inst, "sysenter", 7) == 0) || (strncmp(inst, "int", 3) == 0)) {
			record_inst_done(context);
			free(inst);
			printf("breaking out\n");
			break;
		}
		record_inst(context, inst);
		free(inst);
		if (context->pending_sig != 0) {
			printf("pending sig: %d\n", context->pending_sig);
		}

		sys_ptrace_singlestep(tid, context->pending_sig);
		sys_waitpid(tid, &(context->status));

		if (
		WSTOPSIG(context->status) == SIGSEGV) {
			break;
		}
	}

	assert(GET_EVENT(context->status)==0);
}

static void cont_nonblock(struct context* context)
{
	//printf("%d:state: %x  event: %d pending_sig: %d\n", context->tid, context->exec_state, context->event, context->pending_sig);
	sys_ptrace_syscall_sig(context->child_tid, context->pending_sig);
	context->pending_sig = 0;
}

static int wait_nonblock(struct context* context)
{
	int ret = sys_waitpid_nonblock(context->child_tid, &(context->status));

	if (ret) {
		context->event = read_child_orig_eax(context->child_tid);
		handle_signal(context);
	}

	return ret;
}

void cont_block(struct context *ctx)
{
	goto_next_event(ctx);
	handle_signal(ctx);
}

static int needs_finish(struct context* context)
{
	int event = context->event;

	/* int futex(int *uaddr, int op, int val, const struct timespec *timeout, int *uaddr2, int val3); */
	if (event == SYS_futex) {
		int op = read_child_ecx(context->child_tid) & FUTEX_CMD_MASK;
		if (op == FUTEX_WAKE || op == FUTEX_WAKE_OP || op == FUTEX_WAKE_PRIVATE) {
			return 1;
		}
	}

	return 0;
}

static void handle_syscall_exit(struct context *ctx)
{
	int event = GET_EVENT(ctx->status);

	switch (event) {

	case PTRACE_EVENT_CLONE:
	case PTRACE_EVENT_FORK:
	case PTRACE_EVENT_VFORK:
	{
		/* get new tid, register at the scheduler and setup HPC */
		int new_tid = sys_ptrace_getmsg(ctx->child_tid);

		/* ensure that clone was successful */
		int ret = read_child_eax(ctx->child_tid);
		if (ret == -1) {
			printf("error in clone system call -- bailing out\n");
			sys_exit();
		}

		/* wait until the new thread is ready */
		sys_waitpid(new_tid, &ctx->status);

		rec_sched_register_thread(ctx->child_tid, new_tid);
		sys_ptrace_setup(new_tid);

		/* execute an additional ptrace_sysc((0xFF0000 & status) >> 16);all, since we setup trace like that
		 * do not execute the additional ptrace for vfork, since vfork blocks the
		 * parent thread and we will never continue */
		if (event != PTRACE_EVENT_VFORK) {
			cont_block(ctx);
			assert(signal_pending(ctx->status) == 0);
		} else {
			rec_sched_set_exec_state(new_tid, EXEC_STATE_IN_SYSCALL);
			record_event(ctx, 0);
			ctx->exec_state = EXEC_STATE_IN_SYSCALL;
			return;
		}
		break;

	}

	case PTRACE_EVENT_EXEC:
	{
		cont_block(ctx);
		assert(signal_pending(ctx->status) == 0);
		break;
	}

	case PTRACE_EVENT_VFORK_DONE:
	case PTRACE_EVENT_EXIT:
	{
		ctx->event = USR_EXIT;
		record_event(ctx, 1);
		sched_deregister_thread(ctx);
		return;
	}

	} /* end switch */

	rec_process_syscall(ctx);
	record_event(ctx, 1);
	ctx->exec_state = EXEC_STATE_START;
}

void start_recording()
{
	struct context *ctx = NULL;

	/* record the initial status of the register file */
	ctx = get_active_thread(ctx);
	ctx->event = -1000;
	record_event(ctx, 0);

	while (rec_sched_get_num_threads() > 0) {
		/* get a thread that is ready to be executed */
		ctx = get_active_thread(ctx);

		/* the child process will either be interrupted by: (1) a signal, or (2) at
		 * the entry of the system call */
		debug_print("%d: state %d\n",ctx->child_tid,ctx->exec_state);



		/* simple state machine to guarantee process in the application */
		switch (ctx->exec_state) {

		case EXEC_STATE_START:
		{
			//goto_next_event_singlestep(context);
			cont_nonblock(ctx);

			while (1) {

				int ret = wait_nonblock(ctx);

				if (ret) {
					/* state might be overwritten if a signal occurs */
					if (ctx->event == SIG_SEGV_RDTSC || ctx->event == USR_SCHED) {
						ctx->allow_ctx_switch = 1;
					} else if(ctx->pending_sig) {
						ctx->allow_ctx_switch = 0;
					} else if (ctx->event > 0) {
						ctx->exec_state = EXEC_STATE_ENTRY_SYSCALL;
						ctx->allow_ctx_switch = needs_finish(ctx);
						/* this is a wired state -- no idea why it works */
					} else if (ctx->event == SYS_restart_syscall) {
						ctx->exec_state = EXEC_STATE_START;
						assert(1==0);
					}
					record_event(ctx, 0);
					break;
				}
			}

			break;
		}

		case EXEC_STATE_ENTRY_SYSCALL:
		{
			assert(ctx->pending_sig == 0);

			if (read_child_eax(ctx->child_tid) != -38) {
				assert(1==0);
				ctx->exec_state = EXEC_STATE_START;
				break;
			}

			/* continue and execute the system call */
			cont_nonblock(ctx);
			ctx->exec_state = EXEC_STATE_IN_SYSCALL;
			break;
		}

		case EXEC_STATE_IN_SYSCALL:
		{

			int ret = wait_nonblock(ctx);
			if (ret) {
				/* we received a signal while in the system call and send it right away*/
				/* we have already sent the signal and process sigreturn */
				if (ctx->event == SYS_sigreturn) {
					assert(1==0);
					assert(ctx->pending_sig == 0);
					ctx->exec_state = EXEC_STATE_IN_SYSCALL_SIG;
				}

				if (ctx->pending_sig) {
					printf("received signal in system call: %d  event: %d\n", ctx->pending_sig, ctx->event);
					assert(1==0);
					ctx->exec_state = EXEC_STATE_ENTRY_SYSCALL;
				}

				assert(signal_pending(ctx->status) == 0);
				handle_syscall_exit(ctx);
				ctx->allow_ctx_switch = 0;

			}
			break;
		}

		/*case EXEC_STATE_IN_SYSCALL_SIG:
		{
			assert(1==0);
			cont_nonblock(ctx);
			ctx->exec_state = EXEC_STATE_IN_SYSCALL_SIG_SND;
			break;
		}

		case EXEC_STATE_IN_SYSCALL_SIG_SND:
		{
			assert(1==0);
			int ret = wait_nonblock(ctx);

			if (ret) {
				cont_block(ctx);

				printf("and now: %ld\n", read_child_orig_eax(ctx->child_tid));
				ctx->exec_state = EXEC_STATE_START;
				record_event(ctx, 0);

			}

			break;
		}*/

		default:
		errx(1, "Unknown execution state: %x -- bailing out\n", ctx->exec_state);
		}
	}
}