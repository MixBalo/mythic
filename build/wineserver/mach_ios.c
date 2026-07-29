/*
 * Server-side debugger support using Mach primitives
 *
 * Copyright (C) 1999, 2006 Alexandre Julliard
 * Copyright (C) 2006 Ken Thomases for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef HAVE_SYS_SYSCTL_H
#include <sys/sysctl.h>
#endif

#include "ntstatus.h"
#include "winternl.h"

#include "file.h"
#include "process.h"
#include "thread.h"
#include "request.h"

#ifdef USE_MACH

#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/thread_act.h>
#include <mach/mach_vm.h>
#include <servers/bootstrap.h>

static mach_port_t server_mach_port;

void sigchld_callback(void)
{
    assert(0);  /* should never be called on MacOS */
}

static void mach_set_error(kern_return_t mach_error)
{
    switch (mach_error)
    {
        case KERN_SUCCESS:              break;
        case KERN_INVALID_ARGUMENT:     set_error(STATUS_INVALID_PARAMETER); break;
        case KERN_NO_SPACE:             set_error(STATUS_NO_MEMORY); break;
        case KERN_PROTECTION_FAILURE:   set_error(STATUS_ACCESS_DENIED); break;
        case KERN_INVALID_ADDRESS:      set_error(STATUS_ACCESS_VIOLATION); break;
        default:                        set_error(STATUS_UNSUCCESSFUL); break;
    }
}

static mach_port_t get_process_port( struct process *process )
{
    /* NOTE (task #32): NOT changed to mach_task_self() on iOS. The cross-thread
     * context capture (ios_fill_thread_context) uses mach_task_self() directly,
     * so it doesn't need this. Making this return our task would additionally
     * ACTIVATE read/write_process_memory (they early-out on !process_port),
     * which regressed Steam boot into a guest SEGV + loader-lock deadlock —
     * some caller depends on the old ACCESS_DENIED no-op. Leave as-is. */
    return process->trace_data;
}

static int is_rosetta( void )
{
    static int rosetta_status, did_check = 0;
    if (!did_check)
    {
        /* returns 0 for native process or on error, 1 for translated */
        int ret = 0;
        size_t size = sizeof(ret);
        if (sysctlbyname( "sysctl.proc_translated", &ret, &size, NULL, 0 ) == -1)
            rosetta_status = 0;
        else
            rosetta_status = ret;

        did_check = 1;
    }

    return rosetta_status;
}

extern kern_return_t bootstrap_register2( mach_port_t bp, name_t service_name, mach_port_t sp, uint64_t flags );

/* initialize the process control mechanism */
void init_tracing_mechanism(void)
{
#ifdef WINE_IOS
    /* On iOS, skip bootstrap_register2 - no launchd access */
    mach_port_allocate( mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &server_mach_port );
#else
    mach_port_t bp;

    if (task_get_bootstrap_port( mach_task_self(), &bp ) != KERN_SUCCESS)
        fatal_error( "Can't find bootstrap port\n" );
    if (mach_port_allocate( mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &server_mach_port ) != KERN_SUCCESS)
        fatal_error( "Can't allocate port\n" );
    if  (mach_port_insert_right( mach_task_self(),
                                 server_mach_port,
                                 server_mach_port,
                                 MACH_MSG_TYPE_MAKE_SEND ) != KERN_SUCCESS)
            fatal_error( "Error inserting rights\n" );
    if (bootstrap_register2( bp, server_dir, server_mach_port, 0 ) != KERN_SUCCESS)
        fatal_error( "Can't check in server_mach_port\n" );
    mach_port_deallocate( mach_task_self(), bp );
#endif
}

/* initialize the per-process tracing mechanism */
void init_process_tracing( struct process *process )
{
    int pid, ret;
    struct
    {
        mach_msg_header_t           header;
        mach_msg_body_t             body;
        mach_msg_port_descriptor_t  task_port;
        mach_msg_trailer_t          trailer; /* only present on receive */
    } msg;

    for (;;)
    {
        ret = mach_msg( &msg.header, MACH_RCV_MSG|MACH_RCV_TIMEOUT, 0, sizeof(msg),
                        server_mach_port, 0, 0 );
        if (ret)
        {
            if (ret != MACH_RCV_TIMED_OUT && debug_level)
                fprintf( stderr, "warning: mach port receive failed with %x\n", ret );
            return;
        }

        /* if anything in the message is invalid, ignore it */
        if (msg.header.msgh_size != offsetof(typeof(msg), trailer)) continue;
        if (msg.body.msgh_descriptor_count != 1) continue;
        if (msg.task_port.type != MACH_MSG_PORT_DESCRIPTOR) continue;
        if (msg.task_port.disposition != MACH_MSG_TYPE_PORT_SEND) continue;
        if (msg.task_port.name == MACH_PORT_NULL) continue;
        if (msg.task_port.name == MACH_PORT_DEAD) continue;

        if (!pid_for_task( msg.task_port.name, &pid ))
        {
            struct thread *thread = get_thread_from_pid( pid );

            if (thread && !thread->process->trace_data)
                thread->process->trace_data = msg.task_port.name;
            else
                mach_port_deallocate( mach_task_self(), msg.task_port.name );
        }
    }
    /* On Mach thread priorities depend on having the process port available, so
     * reapply all thread priorities here after process tracing is initialized */
    set_process_base_priority( process, process->base_priority );
}

/* terminate the per-process tracing mechanism */
void finish_process_tracing( struct process *process )
{
    if (process->trace_data)
    {
        mach_port_deallocate( mach_task_self(), process->trace_data );
        process->trace_data = 0;
    }
}

/* initialize registers in new thread if necessary */
void init_thread_context( struct thread *thread )
{
}

/* retrieve the thread x86 registers */
void get_thread_context( struct thread *thread, struct context_data *context, unsigned int flags )
{
#if defined(__i386__) || defined(__x86_64__)
    x86_debug_state_t state;
    mach_msg_type_number_t count = sizeof(state) / sizeof(int);
    mach_msg_type_name_t type;
    mach_port_t port, process_port = get_process_port( thread->process );
    kern_return_t ret;
    unsigned long dr[8];

    /* all other regs are handled on the client side */
    assert( flags == SERVER_CTX_DEBUG_REGISTERS );

    if (is_rosetta())
    {
        /* getting debug registers of a translated process is not supported cross-process, return all zeroes */
        memset( &context->debug, 0, sizeof(context->debug) );
        context->flags |= SERVER_CTX_DEBUG_REGISTERS;
        return;
    }

    if (thread->unix_pid == -1 || !process_port ||
        mach_port_extract_right( process_port, thread->unix_tid,
                                 MACH_MSG_TYPE_COPY_SEND, &port, &type ))
    {
        set_error( STATUS_ACCESS_DENIED );
        return;
    }

    ret = thread_get_state( port, x86_DEBUG_STATE, (thread_state_t)&state, &count );
    if (!ret)
    {
        assert( state.dsh.flavor == x86_DEBUG_STATE32 ||
                state.dsh.flavor == x86_DEBUG_STATE64 );

        if (state.dsh.flavor == x86_DEBUG_STATE64)
        {
            dr[0] = state.uds.ds64.__dr0;
            dr[1] = state.uds.ds64.__dr1;
            dr[2] = state.uds.ds64.__dr2;
            dr[3] = state.uds.ds64.__dr3;
            dr[6] = state.uds.ds64.__dr6;
            dr[7] = state.uds.ds64.__dr7;
        }
        else
        {
            dr[0] = state.uds.ds32.__dr0;
            dr[1] = state.uds.ds32.__dr1;
            dr[2] = state.uds.ds32.__dr2;
            dr[3] = state.uds.ds32.__dr3;
            dr[6] = state.uds.ds32.__dr6;
            dr[7] = state.uds.ds32.__dr7;
        }

        switch (context->machine)
        {
        case IMAGE_FILE_MACHINE_I386:
            context->debug.i386_regs.dr0 = dr[0];
            context->debug.i386_regs.dr1 = dr[1];
            context->debug.i386_regs.dr2 = dr[2];
            context->debug.i386_regs.dr3 = dr[3];
            context->debug.i386_regs.dr6 = dr[6];
            context->debug.i386_regs.dr7 = dr[7];
            break;
        case IMAGE_FILE_MACHINE_AMD64:
            context->debug.x86_64_regs.dr0 = dr[0];
            context->debug.x86_64_regs.dr1 = dr[1];
            context->debug.x86_64_regs.dr2 = dr[2];
            context->debug.x86_64_regs.dr3 = dr[3];
            context->debug.x86_64_regs.dr6 = dr[6];
            context->debug.x86_64_regs.dr7 = dr[7];
            break;
        default:
            set_error( STATUS_INVALID_PARAMETER );
            goto done;
        }
        context->flags |= SERVER_CTX_DEBUG_REGISTERS;
    }
    else
        mach_set_error( ret );
done:
    mach_port_deallocate( mach_task_self(), port );
#endif
}

/* set the thread x86 registers */
void set_thread_context( struct thread *thread, const struct context_data *context, unsigned int flags )
{
#if defined(__i386__) || defined(__x86_64__)
    x86_debug_state_t state;
    mach_msg_type_number_t count = sizeof(state) / sizeof(int);
    mach_msg_type_name_t type;
    mach_port_t port, process_port = get_process_port( thread->process );
    unsigned long dr[8];
    kern_return_t ret;

    /* all other regs are handled on the client side */
    assert( flags == SERVER_CTX_DEBUG_REGISTERS );

    if (is_rosetta())
    {
        /* Setting debug registers of a translated process is not supported cross-process
         * (and even in-process, setting debug registers never has the desired effect).
         */
        set_error( STATUS_UNSUCCESSFUL );
        return;
    }

    if (thread->unix_pid == -1 || !process_port ||
        mach_port_extract_right( process_port, thread->unix_tid,
                                 MACH_MSG_TYPE_COPY_SEND, &port, &type ))
    {
        set_error( STATUS_ACCESS_DENIED );
        return;
    }

    /* get the debug state to determine which flavor to use */
    ret = thread_get_state(port, x86_DEBUG_STATE, (thread_state_t)&state, &count);
    if (ret)
    {
        mach_set_error( ret );
        goto done;
    }
    assert( state.dsh.flavor == x86_DEBUG_STATE32 ||
            state.dsh.flavor == x86_DEBUG_STATE64 );

    switch (context->machine)
    {
        case IMAGE_FILE_MACHINE_I386:
            dr[0] = context->debug.i386_regs.dr0;
            dr[1] = context->debug.i386_regs.dr1;
            dr[2] = context->debug.i386_regs.dr2;
            dr[3] = context->debug.i386_regs.dr3;
            dr[6] = context->debug.i386_regs.dr6;
            dr[7] = context->debug.i386_regs.dr7;
            break;
        case IMAGE_FILE_MACHINE_AMD64:
            dr[0] = context->debug.x86_64_regs.dr0;
            dr[1] = context->debug.x86_64_regs.dr1;
            dr[2] = context->debug.x86_64_regs.dr2;
            dr[3] = context->debug.x86_64_regs.dr3;
            dr[6] = context->debug.x86_64_regs.dr6;
            dr[7] = context->debug.x86_64_regs.dr7;
            break;
        default:
            set_error( STATUS_INVALID_PARAMETER );
            goto done;
    }

    /* Mac OS doesn't allow setting the global breakpoint flags */
    dr[7] = (dr[7] & ~0xaa) | ((dr[7] & 0xaa) >> 1);

    if (state.dsh.flavor == x86_DEBUG_STATE64)
    {
        state.dsh.count = sizeof(state.uds.ds64) / sizeof(int);
        state.uds.ds64.__dr0 = dr[0];
        state.uds.ds64.__dr1 = dr[1];
        state.uds.ds64.__dr2 = dr[2];
        state.uds.ds64.__dr3 = dr[3];
        state.uds.ds64.__dr4 = 0;
        state.uds.ds64.__dr5 = 0;
        state.uds.ds64.__dr6 = dr[6];
        state.uds.ds64.__dr7 = dr[7];
    }
    else
    {
        state.dsh.count = sizeof(state.uds.ds32) / sizeof(int);
        state.uds.ds32.__dr0 = dr[0];
        state.uds.ds32.__dr1 = dr[1];
        state.uds.ds32.__dr2 = dr[2];
        state.uds.ds32.__dr3 = dr[3];
        state.uds.ds32.__dr4 = 0;
        state.uds.ds32.__dr5 = 0;
        state.uds.ds32.__dr6 = dr[6];
        state.uds.ds32.__dr7 = dr[7];
    }
    ret = thread_set_state( port, x86_DEBUG_STATE, (thread_state_t)&state, count );
    if (ret)
        mach_set_error( ret );
done:
    mach_port_deallocate( mach_task_self(), port );
#endif
}

extern int __pthread_kill( mach_port_t, int );

int send_thread_signal( struct thread *thread, int sig )
{
    int ret = -1;
    mach_port_t process_port = get_process_port( thread->process );

    if (thread->unix_pid != -1 && process_port)
    {
        mach_msg_type_name_t type;
        mach_port_t port;

        if (!mach_port_extract_right( process_port, thread->unix_tid,
                                      MACH_MSG_TYPE_COPY_SEND, &port, &type ))
        {
            ret = __pthread_kill( port, sig );
            mach_port_deallocate( mach_task_self(), port );
        }
        else errno = ESRCH;

        if (ret == -1 && errno == ESRCH) /* thread got killed */
        {
            thread->unix_pid = -1;
            thread->unix_tid = -1;
        }
    }
    if (debug_level && ret != -1)
        fprintf( stderr, "%04x: *sent signal* signal=%d\n", thread->id, sig );
    return (ret != -1);
}

#ifdef WINE_IOS
/* iOS cross-thread context capture (task #32).
 *
 * POSIX signal suspend (SIGUSR1 via __pthread_kill) does NOT deliver on iOS —
 * __pthread_kill returns success but usr1_handler never runs, so the upstream
 * "target thread fills its own context in wait_suspend" mechanism is dead and
 * SuspendThread+GetThreadContext hung forever (Steam's watchdog wedged boot).
 *
 * Instead the SERVER captures the target's context via native Mach:
 *   - thread_suspend() halts the target for a coherent snapshot,
 *   - thread_get_state(ARM_THREAD_STATE64) gives the native ARM64 regs,
 *   - the guest x86-64 regs are read from the target's last-synced CPU-area
 *     context (TEB->ChpeV2CpuAreaInfo->ContextAmd64, an AMD64 CONTEXT laid out
 *     binary-compatibly), reachable by a same-task read,
 *   - thread_resume() lets it run again (momentary halt — no lasting suspend,
 *     so no lock-holder deadlock window).
 * Fills both context_data sides; thread.c's stop_thread sets status + signals
 * the context sync so the client's GetThreadContext returns without PENDING.
 *
 * The guest RIP is FEX's last block-boundary sync value — exactly what Steam's
 * hang-detection watchdog samples; a progressing thread shows an advancing RIP.
 * Returns 1 if at least the native context was captured. */

/* TEB / CPU-area / AMD64 CONTEXT field offsets (see winternl.h / winnt.h). */
#define IOS_TEB_CHPE_CPUAREA_OFF   0x1788   /* TEB.ChpeV2CpuAreaInfo */
#define IOS_CPUAREA_CTX64_OFF      0x18     /* CHPE_V2_CPU_AREA_INFO.ContextAmd64 */
#define IOS_A64_SEGCS   0x38
#define IOS_A64_SEGDS   0x3a
#define IOS_A64_SEGES   0x3c
#define IOS_A64_SEGFS   0x3e
#define IOS_A64_SEGGS   0x40
#define IOS_A64_SEGSS   0x42
#define IOS_A64_EFLAGS  0x44
#define IOS_A64_RAX     0x78
#define IOS_A64_RCX     0x80
#define IOS_A64_RDX     0x88
#define IOS_A64_RBX     0x90
#define IOS_A64_RSP     0x98
#define IOS_A64_RBP     0xa0
#define IOS_A64_RSI     0xa8
#define IOS_A64_RDI     0xb0
#define IOS_A64_R8      0xb8
#define IOS_A64_R9      0xc0
#define IOS_A64_R10     0xc8
#define IOS_A64_R11     0xd0
#define IOS_A64_R12     0xd8
#define IOS_A64_R13     0xe0
#define IOS_A64_R14     0xe8
#define IOS_A64_R15     0xf0
#define IOS_A64_RIP     0xf8
#define IOS_A64_FLTSAVE 0x100
#define IOS_A64_CTXLEN  0x4d0            /* full AMD64 CONTEXT */

static int ios_safe_read( uint64_t addr, void *buf, unsigned int size )
{
    mach_vm_size_t got = 0;
    if (!addr) return 0;
    if (mach_vm_read_overwrite( mach_task_self(), (mach_vm_address_t)addr,
                                (mach_vm_size_t)size, (mach_vm_address_t)buf, &got ))
        return 0;
    return got == size;
}

int ios_fill_thread_context( struct thread *thread,
                             struct context_data *native,
                             struct context_data *wow )
{
    mach_msg_type_name_t type;
    mach_port_t port;
    arm_thread_state64_t arm;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    kern_return_t kr;
    int have_native = 0;

    if (thread->unix_pid == -1 || thread->unix_tid == (unsigned int)-1) return 0;
    if (mach_port_extract_right( mach_task_self(), thread->unix_tid,
                                 MACH_MSG_TYPE_COPY_SEND, &port, &type ))
        return 0;

    if (thread_suspend( port )) { mach_port_deallocate( mach_task_self(), port ); return 0; }

    /* --- native ARM64 context --- */
    kr = thread_get_state( port, ARM_THREAD_STATE64, (thread_state_t)&arm, &count );
    if (!kr)
    {
        unsigned int i;
        native->flags |= SERVER_CTX_CONTROL | SERVER_CTX_INTEGER;
        native->ctl.arm64_regs.sp     = arm.__sp;
        native->ctl.arm64_regs.pc     = arm.__pc;
        native->ctl.arm64_regs.pstate = arm.__cpsr;
        for (i = 0; i < 29; i++) native->integer.arm64_regs.x[i] = arm.__x[i];
        native->integer.arm64_regs.x[29] = arm.__fp;
        native->integer.arm64_regs.x[30] = arm.__lr;
        have_native = 1;
    }

    /* --- guest x86-64 context from TEB->ChpeV2CpuAreaInfo->ContextAmd64 --- */
    if (wow)
    {
        uint64_t cpuarea = 0, ctx64 = 0;
        unsigned char ctx[IOS_A64_CTXLEN];
        /* Tag the WOW side with the guest machine unconditionally so the
         * get_thread_context handler's native/WOW split always routes an
         * AMD64 request correctly, even if the CPU-area read fails for an
         * early / pure-native thread (then flags stay 0 → empty x64 ctx). */
        wow->machine = thread->process->machine;   /* IMAGE_FILE_MACHINE_AMD64 */
        if (thread->teb &&
            ios_safe_read( (uint64_t)thread->teb + IOS_TEB_CHPE_CPUAREA_OFF, &cpuarea, 8 ) && cpuarea &&
            ios_safe_read( cpuarea + IOS_CPUAREA_CTX64_OFF, &ctx64, 8 ) && ctx64 &&
            ios_safe_read( ctx64, ctx, sizeof(ctx) ))
        {
            wow->flags |= SERVER_CTX_CONTROL | SERVER_CTX_INTEGER | SERVER_CTX_SEGMENTS |
                          SERVER_CTX_FLOATING_POINT;
            wow->ctl.x86_64_regs.rip   = *(uint64_t *)(ctx + IOS_A64_RIP);
            wow->ctl.x86_64_regs.rsp   = *(uint64_t *)(ctx + IOS_A64_RSP);
            wow->ctl.x86_64_regs.cs    = *(uint16_t *)(ctx + IOS_A64_SEGCS);
            wow->ctl.x86_64_regs.ss    = *(uint16_t *)(ctx + IOS_A64_SEGSS);
            wow->ctl.x86_64_regs.flags = *(uint32_t *)(ctx + IOS_A64_EFLAGS);
            /* AMD64 CONTEXT memory order (Rax,Rcx,Rdx,Rbx,Rsp,Rbp,Rsi,Rdi,R8..)
             * differs from context_data.integer field order (rax,rbx,rcx,rdx,
             * rbp,rsi,rdi,r8..; no rsp) — map each register explicitly. */
            wow->integer.x86_64_regs.rax = *(uint64_t *)(ctx + IOS_A64_RAX);
            wow->integer.x86_64_regs.rbx = *(uint64_t *)(ctx + IOS_A64_RBX);
            wow->integer.x86_64_regs.rcx = *(uint64_t *)(ctx + IOS_A64_RCX);
            wow->integer.x86_64_regs.rdx = *(uint64_t *)(ctx + IOS_A64_RDX);
            wow->integer.x86_64_regs.rbp = *(uint64_t *)(ctx + IOS_A64_RBP);
            wow->integer.x86_64_regs.rsi = *(uint64_t *)(ctx + IOS_A64_RSI);
            wow->integer.x86_64_regs.rdi = *(uint64_t *)(ctx + IOS_A64_RDI);
            wow->integer.x86_64_regs.r8  = *(uint64_t *)(ctx + IOS_A64_R8);
            wow->integer.x86_64_regs.r9  = *(uint64_t *)(ctx + IOS_A64_R9);
            wow->integer.x86_64_regs.r10 = *(uint64_t *)(ctx + IOS_A64_R10);
            wow->integer.x86_64_regs.r11 = *(uint64_t *)(ctx + IOS_A64_R11);
            wow->integer.x86_64_regs.r12 = *(uint64_t *)(ctx + IOS_A64_R12);
            wow->integer.x86_64_regs.r13 = *(uint64_t *)(ctx + IOS_A64_R13);
            wow->integer.x86_64_regs.r14 = *(uint64_t *)(ctx + IOS_A64_R14);
            wow->integer.x86_64_regs.r15 = *(uint64_t *)(ctx + IOS_A64_R15);
            wow->seg.x86_64_regs.ds = *(uint16_t *)(ctx + IOS_A64_SEGDS);
            wow->seg.x86_64_regs.es = *(uint16_t *)(ctx + IOS_A64_SEGES);
            wow->seg.x86_64_regs.fs = *(uint16_t *)(ctx + IOS_A64_SEGFS);
            wow->seg.x86_64_regs.gs = *(uint16_t *)(ctx + IOS_A64_SEGGS);
            memcpy( wow->fp.x86_64_regs.fpregs, ctx + IOS_A64_FLTSAVE,
                    sizeof(wow->fp.x86_64_regs.fpregs) );
        }
    }

    thread_resume( port );
    mach_port_deallocate( mach_task_self(), port );

    return have_native;
}
#endif  /* WINE_IOS */

/* read data from a process memory space */
int read_process_memory( struct process *process, client_ptr_t ptr, data_size_t size, char *dest )
{
    kern_return_t ret;
    mach_vm_size_t bytes_read;
    mach_port_t process_port = get_process_port( process );

    if (!process_port)
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }
    if ((mach_vm_address_t)ptr != ptr)
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }

    ret = mach_vm_read_overwrite( process_port, (mach_vm_address_t)ptr, (mach_vm_size_t)size, (mach_vm_address_t)dest, &bytes_read );
    mach_set_error( ret );
    return (ret == KERN_SUCCESS);
}

/* write data to a process memory space */
int write_process_memory( struct process *process, client_ptr_t ptr, data_size_t size, const char *src,
                          data_size_t *written )
{
    kern_return_t ret;
    mach_port_t process_port = get_process_port( process );
    mach_vm_offset_t data;

    if (written) *written = 0;

    if (!process_port)
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }
    if ((mach_vm_address_t)ptr != ptr)
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }
    if (posix_memalign( (void **)&data, get_page_size(), size ))
    {
        set_error( STATUS_NO_MEMORY );
        return 0;
    }

    memcpy( (void *)data, src, size );

    ret = mach_vm_write( process_port, (mach_vm_address_t)ptr, data, (mach_msg_type_number_t)size );

    /*
     * On arm64 macOS, enabling execute permission for a memory region automatically disables write
     * permission for that region. This can also happen under Rosetta sometimes.
     * In that case mach_vm_write returns KERN_INVALID_ADDRESS.
     */

    if (ret == KERN_INVALID_ADDRESS)
    {
        mach_vm_address_t current_address = (mach_vm_address_t)ptr;
        mach_vm_address_t region_address = current_address;
        mach_vm_size_t region_size, write_size;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object_name;
        data_size_t remaining_size = size;

        ret = mach_vm_region( process_port, &region_address, &region_size, VM_REGION_BASIC_INFO_64,
                     (vm_region_info_t)&info, &info_count, &object_name );
        if (ret != KERN_SUCCESS)
            goto out;

        /*
         * Actually check that everything is sane before suspending.
         * KERN_INVALID_ADDRESS can also be returned when address is illegal or
         * specifies a non-allocated region.
         */
        if (region_address > current_address ||
            region_address + region_size <= current_address)
        {
            ret = KERN_INVALID_ADDRESS;
            goto out;
        }

        /*
         * FIXME: Rosetta can turn RWX pages into R-X pages during execution.
         * For now we will just have to ignore failures due to the wrong
         * protection here.
         */
        if (!is_rosetta() && !(info.protection & VM_PROT_WRITE))
        {
            ret = KERN_PROTECTION_FAILURE;
            goto out;
        }

        /* The following operations should seem atomic from the perspective of the
         * target process. */
        if ((ret = task_suspend( process_port )) != KERN_SUCCESS)
            goto out;

        /* Iterate over all applicable memory regions until the write is completed. */
        while (remaining_size)
        {
            region_address = current_address;
            info_count = VM_REGION_BASIC_INFO_COUNT_64;
            ret = mach_vm_region( process_port, &region_address, &region_size, VM_REGION_BASIC_INFO_64,
                     (vm_region_info_t)&info, &info_count, &object_name );
            if (ret != KERN_SUCCESS) break;

            if (region_address > current_address ||
                region_address + region_size <= current_address)
            {
                ret = KERN_INVALID_ADDRESS;
                break;
            }

            /* FIXME: See the above Rosetta remark. */
            if (!is_rosetta() && !(info.protection & VM_PROT_WRITE))
            {
                ret = KERN_PROTECTION_FAILURE;
                break;
            }

            write_size = region_size - (current_address - region_address);
            if (write_size > remaining_size) write_size = remaining_size;

            ret = mach_vm_protect( process_port, current_address, write_size, 0,
                    VM_PROT_READ | VM_PROT_WRITE );
            if (ret != KERN_SUCCESS) break;

            ret = mach_vm_write( process_port, current_address,
                    data + (current_address - (mach_vm_address_t)ptr), write_size );
            if (ret != KERN_SUCCESS) break;

            ret = mach_vm_protect( process_port, current_address, write_size, 0,
                    info.protection );
            if (ret != KERN_SUCCESS) break;

            if (written) *written += write_size;
            current_address       += write_size;
            remaining_size        -= write_size;
        }

        task_resume( process_port );
    }

out:
    free( (void *)data );
    mach_set_error( ret );
    if (ret == KERN_SUCCESS && written) *written = size;
    return (ret == KERN_SUCCESS);
}

#endif  /* USE_MACH */
