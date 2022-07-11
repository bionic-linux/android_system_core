/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//  The ctx_t is the integral context of a thread, these comments apply to
//  all architextures.
//
//  The thread integer register context is divided into two parts, the first
//  part is what is informally referred to as a "half" context, it is not the
//  full context, the full context only has to be saved when a thread is
//  interrupted (and preempted) in an arbitrary instruction location.  When a
//  thread is preempted voluntarily, for example when waiting to acquire a
//  mutex, or waiting for a condition to occur.
//
//  A zero least significant bit of ctx_fpctx indicates that only the half
//  context needs to be loaded when switching into the thread. The simplest
//  case is when a half context when no floating point is to be loaded, both
//  conditions are tested with a single compare against zero of ctx_fpctx.

#define	SIZEOF_UREG_T	8

#ifdef LWT_ARM64 //{
#ifdef LWT_C //{

#define CACHE_LINE_SIZE_L2      6
#define CACHE_LINE_SIZE         64

//  On ARM64 (without SVE) there are 32 128 bit SIMD registers, the 32 bit
//  floating point registers are held inside of them.  A non-SIMD version of
//  the floating point context could be used by threads that don't use SIMD,
//  if it is possible to disable SIMD while allowing FP to still be used
//  and if it is practical to do so.  TODO review SIMD / FP separation.
//
//  TODO: add support for SVE context.

#define	FPCTX_NREG	 32

typedef struct {
	uregx2_t	 fpctx_regs[FPCTX_NREG];
} aligned_cache_line fpctx_t;

#ifndef LWT_CTX_ARRAY //{

//  The ctx_t register context is a subset prefix of the sigcontext structure,
//  it excludes the fault_address at the start of it, and uses that space to
//  store the low 32 bits of fpsr and fpcr (both are 64 bits, their upper 32
//  bits are reserved, they both have plenty of reserved bit in their lower
//  32 bits too).  The Linux kernel sigreturn contains a fpsimd_context, that
//  structure stores both fpcr and fpsr as 32 bit quantities.

//  The sigreturn optional data area (where fpsimd_context would be stored),
//  __reserved array at the end of sigreturn is not part of ctx_t.  The data
//  within it, is represented in other ways.  The __reserved area is already
//  too small for large SVE vectors, hardwiring a fixed sized reserved area
//  is a mistake for the LWT implementation.

//  The context is split into two areas, the "half" context and the "rest" of
//  the context, together they make a "full" context. To keep ctx_t a prefix of
//  sigcontext, the "rest" area is first.  The address of the ctx_t is chosen
//  so that its first three ureg_t are the last three ureg_t of a cache line.
//  This makes the subsequent 32 ureg_t into 4 groups, each cache aligned.

typedef struct {

	//  This is the "rest" of the context.

	ureg_t		 ctx_fpcr_fpsr;	// low 32 bits of fpcr and fpsr
	ureg_t		 ctx_x0;	// [x0, x1]
	ureg_t		 ctx_x1;

	ureg_t		 ctx_x2;	// [x2, x3]
	ureg_t		 ctx_x3;
	ureg_t		 ctx_x4;	// [x4, x5]
	ureg_t		 ctx_x5;
	ureg_t		 ctx_x6;	// [x6, x7]
	ureg_t		 ctx_x7;
	ureg_t		 ctx_x8;	// [x8, x9]
	ureg_t		 ctx_x9;

	//  This part is the "half" context.
	//
	//  The first 8 are floating point registers that are callee saved.
	//  In a "half" context d8-d15 are stored in the location for x10-x17
	//  which is not used in a "half" context.  In a half context only the
	//  low 64 bits of v8-v15 need to be saved, those low halves are saved
	//  more efficiently in a single cache line by storing them here.
	//
	//  Last 16 are general purpose registers that are callee saved,
	//  keeping these 16 together puts them in two cache lines.

	ureg_t		 ctx_x10;	// [x10, x11] or [d8, d9]
	ureg_t		 ctx_x11;
	ureg_t		 ctx_x12;	// [x12, x13] or [d10, d11]
	ureg_t		 ctx_x13;
	ureg_t		 ctx_x14;	// [x14, x15] or [d12, d13]
	ureg_t		 ctx_x15;
	ureg_t		 ctx_x16;	// [x16, x17] or [d14, d15]
	ureg_t		 ctx_x17;

	ureg_t		 ctx_x18;	// [x18, x19]
	ureg_t		 ctx_x19;
	ureg_t		 ctx_x20;	// [x20, x21]
	ureg_t		 ctx_x21;
	ureg_t		 ctx_x22;	// [x22, x23]
	ureg_t		 ctx_x23;
	ureg_t		 ctx_x24;	// [x24, x25]
	ureg_t		 ctx_x25;

	ureg_t		 ctx_x26;	// [x26, x27]
	ureg_t		 ctx_x27;
	ureg_t		 ctx_x28;	// [x28, x29]
	ureg_t		 ctx_x29;
        ureg_t		 ctx_x30;	// [x30, sp]
	ureg_t		 ctx_sp;
        ureg_t		 ctx_pc;	// [pc, pstate]
	ureg_t		 ctx_pstate;

} ctx_t;

#else //}{ LWT_CTX_ARRAY

//  Use this version instead of offsets generated by lwt_genassym.c.
//
//  Don't remove the version above, keep both in sync, one will be removed
//  eventually.  The version above, used with lwt_genassym.c, makes debugging
//  much easier.
//
//  TODO: this is a regression against how things should be, remove this
//  when compiling a program, generating a header file, and using it in
//  the Android build to build other files is understood.

#define	CTX_NREGS	 35
typedef struct {
	ureg_t		 ctx_regs[CTX_NREGS];
} ctx_t;

#define	ctx_fpcr_fpsr		ctx_regs[CTX_FPCR_FPSR_IX]
#define	ctx_x0			ctx_regs[CTX_X0_IX]
#define	ctx_x1			ctx_regs[CTX_X1_IX]

#define	ctx_x2			ctx_regs[CTX_X2_IX]
#define	ctx_x3			ctx_regs[CTX_X3_IX]
#define	ctx_x4			ctx_regs[CTX_X4_IX]
#define	ctx_x5			ctx_regs[CTX_X5_IX]
#define	ctx_x6			ctx_regs[CTX_X6_IX]
#define	ctx_x7			ctx_regs[CTX_X7_IX]
#define	ctx_x8			ctx_regs[CTX_X8_IX]
#define	ctx_x9			ctx_regs[CTX_X9_IX]

#define	ctx_x10			ctx_regs[CTX_X10_IX]
#define	ctx_x11			ctx_regs[CTX_X11_IX]
#define	ctx_x12			ctx_regs[CTX_X12_IX]
#define	ctx_x13			ctx_regs[CTX_X13_IX]
#define	ctx_x14			ctx_regs[CTX_X14_IX]
#define	ctx_x15			ctx_regs[CTX_X15_IX]
#define	ctx_x16			ctx_regs[CTX_X16_IX]
#define	ctx_x17			ctx_regs[CTX_X17_IX]

#define	ctx_x18			ctx_regs[CTX_X18_IX]
#define	ctx_x19			ctx_regs[CTX_X19_IX]
#define	ctx_x20			ctx_regs[CTX_X20_IX]
#define	ctx_x21			ctx_regs[CTX_X21_IX]
#define	ctx_x22			ctx_regs[CTX_X22_IX]
#define	ctx_x23			ctx_regs[CTX_X23_IX]
#define	ctx_x24			ctx_regs[CTX_X24_IX]
#define	ctx_x25			ctx_regs[CTX_X25_IX]

#define	ctx_x26			ctx_regs[CTX_X26_IX]
#define	ctx_x27			ctx_regs[CTX_X27_IX]
#define	ctx_x28			ctx_regs[CTX_X28_IX]
#define	ctx_x29			ctx_regs[CTX_X29_IX]
#define	ctx_x30			ctx_regs[CTX_X30_IX]
#define	ctx_sp			ctx_regs[CTX_SP_IX]
#define	ctx_pc			ctx_regs[CTX_PC_IX]
#define	ctx_pstate		ctx_regs[CTX_PSTATE_IX]

#endif //} LWT_CTX_ARRAY
#endif //} LWT_C

#define CTX_FPCR_FPSR_IX	0
#define CTX_X0_IX		1
#define CTX_X1_IX		2

#define CTX_X2_IX		3
#define CTX_X3_IX		4
#define CTX_X4_IX		5
#define CTX_X5_IX		6
#define CTX_X6_IX		7
#define CTX_X7_IX		8
#define CTX_X8_IX		9
#define CTX_X9_IX		10

#define CTX_X10_IX		11
#define CTX_X11_IX		12
#define CTX_X12_IX		13
#define CTX_X13_IX		14
#define CTX_X14_IX		15
#define CTX_X15_IX		16
#define CTX_X16_IX		17
#define CTX_X17_IX		18

#define CTX_X18_IX		19
#define CTX_X19_IX		20
#define CTX_X20_IX		21
#define CTX_X21_IX		22
#define CTX_X22_IX		23
#define CTX_X23_IX		24
#define CTX_X24_IX		25
#define CTX_X25_IX		26

#define CTX_X26_IX		27
#define CTX_X27_IX		28
#define CTX_X28_IX		29
#define CTX_X29_IX		30
#define CTX_X30_IX		31
#define CTX_SP_IX		32
#define CTX_PC_IX		33
#define CTX_PSTATE_IX		34

#ifdef LWT_C //{

#define	ctx_thr_start_arg0	ctx_x19
#define	ctx_thr_start_func	ctx_x20
#define	ctx_thr_start_pc	ctx_x21

#else //}{ !LWT_C

#define	reg_thr_start_arg0	x19
#define	reg_thr_start_func	x20
#define	reg_thr_start_pc	x21

#define	ctx_d8		ctx_x10
#define	ctx_d9		ctx_x11
#define	ctx_d10		ctx_x12
#define	ctx_d11		ctx_x13
#define	ctx_d12		ctx_x14
#define	ctx_d13		ctx_x15
#define	ctx_d14		ctx_x16
#define	ctx_d15		ctx_x17

#ifdef LWT_CTX_ARRAY //{

#define	ctx_fpcr_fpsr		(SIZEOF_UREG_T * CTX_FPCR_FPSR_IX)
#define	ctx_x0			(SIZEOF_UREG_T * CTX_X0_IX)
#define	ctx_x1			(SIZEOF_UREG_T * CTX_X1_IX)

#define	ctx_x2			(SIZEOF_UREG_T * CTX_X2_IX)
#define	ctx_x3			(SIZEOF_UREG_T * CTX_X3_IX)
#define	ctx_x4			(SIZEOF_UREG_T * CTX_X4_IX)
#define	ctx_x5			(SIZEOF_UREG_T * CTX_X5_IX)
#define	ctx_x6			(SIZEOF_UREG_T * CTX_X6_IX)
#define	ctx_x7			(SIZEOF_UREG_T * CTX_X7_IX)
#define	ctx_x8			(SIZEOF_UREG_T * CTX_X8_IX)
#define	ctx_x9			(SIZEOF_UREG_T * CTX_X9_IX)

#define	ctx_x10			(SIZEOF_UREG_T * CTX_X10_IX)
#define	ctx_x11			(SIZEOF_UREG_T * CTX_X11_IX)
#define	ctx_x12			(SIZEOF_UREG_T * CTX_X12_IX)
#define	ctx_x13			(SIZEOF_UREG_T * CTX_X13_IX)
#define	ctx_x14			(SIZEOF_UREG_T * CTX_X14_IX)
#define	ctx_x15			(SIZEOF_UREG_T * CTX_X15_IX)
#define	ctx_x16			(SIZEOF_UREG_T * CTX_X16_IX)
#define	ctx_x17			(SIZEOF_UREG_T * CTX_X17_IX)

#define	ctx_x18			(SIZEOF_UREG_T * CTX_X18_IX)
#define	ctx_x19			(SIZEOF_UREG_T * CTX_X19_IX)
#define	ctx_x20			(SIZEOF_UREG_T * CTX_X20_IX)
#define	ctx_x21			(SIZEOF_UREG_T * CTX_X21_IX)
#define	ctx_x22			(SIZEOF_UREG_T * CTX_X22_IX)
#define	ctx_x23			(SIZEOF_UREG_T * CTX_X23_IX)
#define	ctx_x24			(SIZEOF_UREG_T * CTX_X24_IX)
#define	ctx_x25			(SIZEOF_UREG_T * CTX_X25_IX)

#define	ctx_x26			(SIZEOF_UREG_T * CTX_X26_IX)
#define	ctx_x27			(SIZEOF_UREG_T * CTX_X27_IX)
#define	ctx_x28			(SIZEOF_UREG_T * CTX_X28_IX)
#define	ctx_x29			(SIZEOF_UREG_T * CTX_X29_IX)
#define	ctx_x30			(SIZEOF_UREG_T * CTX_X30_IX)
#define	ctx_sp			(SIZEOF_UREG_T * CTX_SP_IX)
#define	ctx_pc			(SIZEOF_UREG_T * CTX_PC_IX)
#define	ctx_pstate		(SIZEOF_UREG_T * CTX_PSTATE_IX)

#endif //} LWT_CTX_ARRAY
#endif //} !LWT_C
#endif //} LWT_ARM64

#ifdef LWT_X64 //{
#ifdef LWT_C //{

//  X64 floating point is too barroque, best is to exactly follow libc

#include <sys/ucontext.h>

#define CACHE_LINE_SIZE_L2      6
#define CACHE_LINE_SIZE         64

typedef struct {
	struct _libc_fpstate fpstate;
} fpctx_t;

#ifndef LWT_CTX_ARRAY //{

typedef struct {
	//  This part is the "half" context.
	//
	//  First 8 are callee saved, keeping these 8 together puts them
	//  in two cache lines. The ctx_fpctx is among those 8 so that FP
	//  context, if any can be restored without touching the rest of this
	//  structure.
	//
	//  Unlike ARM64, there is no extra space for ctx_rdi (first argument)
	//  in this cache line, newly created threads use a trampoline function
	//  (__lwt_thr_start) to adjust their context, the argument is found in
	//  ctx_rbp and the actual function address in ctx_rbx also known as
	//  ctx_thr_start_pc and ctx_thr_start_arg0 in portable code.

	fpctx_t		*ctx_fpctx;
	ureg_t		 ctx_pc;
	ureg_t		 ctx_sp;
	ureg_t		 ctx_rbp;
	ureg_t		 ctx_rbx;
	ureg_t		 ctx_r12;
	ureg_t		 ctx_r13;
	ureg_t		 ctx_r14;
	ureg_t		 ctx_r15;	// one past the end of the cache line

	//  This is the rest of the context.

	ureg_t		 ctx_flags;
	ureg_t		 ctx_rax;
	ureg_t		 ctx_rcx;
	ureg_t		 ctx_rdx;
	ureg_t		 ctx_rdi;
	ureg_t		 ctx_rsi;
	ureg_t		 ctx_r8;
	ureg_t		 ctx_r9;
	ureg_t		 ctx_r10;
	ureg_t		 ctx_r11;
} ctx_t;

#else //}{ !LWT_CTX_ARRAY 

//  Use this version instead of offsets generated by lwt_genassym.c.
//
//  Don't remove the version above, keep both in sync, one will be removed
//  eventually.  The version above, used with lwt_genassym.c, makes debugging
//  much easier.
//
//  TODO: this is a regression against how things should be, remove this
//  when compiling a program, generating a header file, and using it in
//  the Android build to build other files is understood.

#define	CTX_NREGS	 19
typedef struct {
	ureg_t		 ctx_regs[CTX_NREGS];
} ctx_t;

#define	ctx_fpctx	 ctx_regs[CTX_FPCTX_IX]
#define	ctx_pc		 ctx_regs[CTX_PC_IX]
#define	ctx_sp		 ctx_regs[CTX_SP_IX]
#define	ctx_rbp		 ctx_regs[CTX_RBP_IX]
#define	ctx_rbx		 ctx_regs[CTX_RBX_IX]
#define	ctx_r12		 ctx_regs[CTX_R12_IX]
#define	ctx_r13		 ctx_regs[CTX_R13_IX]
#define	ctx_r14		 ctx_regs[CTX_R14_IX]
#define	ctx_r15		 ctx_regs[CTX_R15_IX]

#define	ctx_flags	 ctx_regs[CTX_FLAGS_IX]
#define	ctx_rax		 ctx_regs[CTX_RAX_IX]
#define	ctx_rcx		 ctx_regs[CTX_RCX_IX]
#define	ctx_rdx		 ctx_regs[CTX_RDX_IX]
#define	ctx_rdi		 ctx_regs[CTX_RDI_IX]
#define	ctx_rsi		 ctx_regs[CTX_RSI_IX]
#define	ctx_r8		 ctx_regs[CTX_R8_IX]
#define	ctx_r9		 ctx_regs[CTX_R9_IX]
#define	ctx_r10		 ctx_regs[CTX_R10_IX]
#define	ctx_r11		 ctx_regs[CTX_R11_IX]

#endif //} !LWT_CTX_ARRAY 
#endif //} LWT_C

#define	CTX_FPCTX_IX	 0
#define	CTX_PC_IX	 1
#define	CTX_SP_IX	 2
#define	CTX_RBP_IX	 3
#define	CTX_RBX_IX	 4
#define	CTX_R12_IX	 5
#define	CTX_R13_IX	 6
#define	CTX_R14_IX	 7
#define	CTX_R15_IX	 8

#define	CTX_FLAGS_IX	 9
#define	CTX_RAX_IX	 10
#define	CTX_RCX_IX	 11
#define	CTX_RDX_IX	 12
#define	CTX_RDI_IX	 13
#define	CTX_RSI_IX	 14
#define	CTX_R8_IX	 15
#define	CTX_R9_IX	 16
#define	CTX_R10_IX	 17
#define	CTX_R11_IX	 18

#ifdef LWT_C //{

#define	ctx_thr_start_arg0	ctx_rbp
#define	ctx_thr_start_func	ctx_rbx
#define	ctx_thr_start_pc	ctx_r12

#else //}{ !LWT_C

#define	reg_thr_start_arg0	rbp
#define	reg_thr_start_func	rbx
#define	reg_thr_start_pc	r12

#ifdef LWT_CTX_ARRAY //{

#define	ctx_fpctx	 (SIZEOF_UREG_T * CTX_FPCTX_IX)
#define	ctx_pc		 (SIZEOF_UREG_T * CTX_PC_IX)
#define	ctx_sp		 (SIZEOF_UREG_T * CTX_SP_IX)
#define	ctx_rbp		 (SIZEOF_UREG_T * CTX_RBP_IX)
#define	ctx_rbx		 (SIZEOF_UREG_T * CTX_RBX_IX)
#define	ctx_r12		 (SIZEOF_UREG_T * CTX_R12_IX)
#define	ctx_r13		 (SIZEOF_UREG_T * CTX_R13_IX)
#define	ctx_r14		 (SIZEOF_UREG_T * CTX_R14_IX)
#define	ctx_r15		 (SIZEOF_UREG_T * CTX_R15_IX)

#define	ctx_flags	 (SIZEOF_UREG_T * CTX_FLAGS_IX)
#define	ctx_rax		 (SIZEOF_UREG_T * CTX_RAX_IX)
#define	ctx_rcx		 (SIZEOF_UREG_T * CTX_RCX_IX)
#define	ctx_rdx		 (SIZEOF_UREG_T * CTX_RDX_IX)
#define	ctx_rdi		 (SIZEOF_UREG_T * CTX_RDI_IX)
#define	ctx_rsi		 (SIZEOF_UREG_T * CTX_RSI_IX)
#define	ctx_r8		 (SIZEOF_UREG_T * CTX_R8_IX)
#define	ctx_r9		 (SIZEOF_UREG_T * CTX_R9_IX)
#define	ctx_r10		 (SIZEOF_UREG_T * CTX_R10_IX)
#define	ctx_r11		 (SIZEOF_UREG_T * CTX_R11_IX)

#endif //} LWT_CTX_ARRAY
#endif //} !LWT_C
#endif //} LWT_X64

#ifdef LWT_C //{

//  uptr atomic operations

#if 0
uptr_t uptr_load_acq(uptr_atom_t *m);

inline_only void uptr_store_rel(uptr_atom_t *m, uptr_t v)
{
	__asm__ volatile(
		"stlr	%1, %0"
		: "=m"(*m)
		: "r"(v), "r"(m));
}

inline_only void uptr_store_zero_rel(uptr_atom_t *m)
{
	__asm__ volatile(
		"stlr	xzr, %0"
		: "=m"(*m)
		: "r"(m));
}

inline_only uptr_t uptr_comp_and_swap_acq(uptr_t old, uptr_t new,
					  uptr_atom_t *m)
{
	__asm__ volatile(
		"casa	%0, %2, %1"
		: "+&r"(old), "+Q"(*(uptr_t *)m)
		: "r"(new), "r"(m));
	return old;
}

inline_only uptr_t uptr_comp_and_swap_acq_rel(uptr_t old, uptr_t new,
					      uptr_atom_t *m)
{
	__asm__ volatile(
		"casal	%0, %2, %1"
		: "+&r"(old), "+Q"(*(uptr_t *)m)
		: "r"(new), "r"(m));
	return old;
}
#endif

inline_only bool uregx2_equal(uregx2_t a, uregx2_t b)
{
	return ((a.low ^ b.low) | (a.high ^ b.high)) == 0;
}

inline_only uregx2_t uregx2_load(uregx2_t *m) {
	return *m;
}


//  uregx2_t atomic operations

#ifdef LWT_ARM64 //{
inline_only uregx2_t uregx2_comp_and_swap_acq_rel(uregx2_t old, uregx2_t new,
						  uregx2_t *m)
{
	//  Choosing the registers to be register pairs starting at
	//  even register numbers is not properly done by gcc-11

	register ureg_t old_low  __asm__("x0") = old.low;
	register ureg_t old_high __asm__("x1") = old.high;
	register ureg_t new_low  __asm__("x2") = new.low;
	register ureg_t new_high __asm__("x3") = new.high;

	//  Without volatile the compiler sometimes wants to use register
	//  displament addressing: [xN,offset] which is invalid for caspal,
	//  this ensures that the pointer m is in register p.

	register uregx2_t *volatile p = m;

	__asm__ volatile("caspal	%0, %1, %3, %4, %2" // "+Q" vs "+m" ?
			 : "+&r"(old_low), "+&r"(old_high), "+m"(*(uregx2_t *)p)
			 : "r"(new_low), "r"(new_high), "r"(p));
	return (uregx2_t) {.low = old_low, .high = old_high};
}

#ifndef LWT_CPU_PTHREAD_KEY //{
void cpu_current_set(cpu_t *cpu)
{
#if 0
	// TODO: use x18 until tpidrro_el0 can be set from the kernel :-(
	*((volatile int *)11) = 0xDEADBEEF;
#else
	__asm__ ("mov	x18, %0" : "=r"(cpu));
#endif
}
inline_only cpu_t *cpu_current(void)
{
	register ureg_t cpureg;
#if 0
	__asm__("mrs	%0, tpidrro_el0" : "=r"(cpureg));
#else
	__asm__ ("mov	%0, x18" : "=r"(cpureg));
#endif
	return (cpu_t *) cpureg;
}
#endif //}
#endif //}

#ifdef LWT_X64 //{
inline_only uregx2_t uregx2_comp_and_swap_acq_rel(uregx2_t old, uregx2_t new,
						  uregx2_t *m)
{
	register ureg_t old_low  __asm__("rax") = old.low;
	register ureg_t old_high __asm__("rdx") = old.high;
	register ureg_t new_low  __asm__("rbx") = new.low;
	register ureg_t new_high __asm__("rcx") = new.high;
	__asm__ volatile("lock; cmpxchg16b	%2"
			 : "+&r"(old_low), "+&r"(old_high), "+m"(*(uregx2_t *)m)
			 : "r"(new_low), "r"(new_high));
	return (uregx2_t) {.low = old_low, .high = old_high};
}

inline_only void cpu_current_set_x64(cpu_t **cpu)
{
	register ureg_t gsb  __asm__("rdi") = (ureg_t) cpu;
	__asm__ volatile("wrgsbase      %0" : : "r"(gsb) : "memory");

}

inline_only cpu_t *cpu_current()
{
	register ureg_t reg  __asm__("rax");
	__asm__ volatile("mov %%gs:0, %0" : "=r"(reg));
	return (cpu_t *) reg;
}

// cpu_t *cpu_current(void);
#endif //}

#endif //}
