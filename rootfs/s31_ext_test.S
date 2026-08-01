/* SPDX-License-Identifier: GPL-2.0-only */
/* Hand-written ESP32-S31 F/HWLoop/PIE instruction tests. */

	.text
	.option norvc

	.globl s31_fpu_add
	.type s31_fpu_add, @function
s31_fpu_add:
	fmv.w.x	f0, a0
	fmv.w.x	f1, a1
	fadd.s	f0, f0, f1
	fmv.x.w	a0, f0
	ret
	.size s31_fpu_add, .-s31_fpu_add

	.globl s31_fpu_hold_yield
	.type s31_fpu_hold_yield, @function
s31_fpu_hold_yield:
	fmv.w.x	f0, a0
	li	a7, 124		/* __NR_sched_yield */
	ecall
	fmv.x.w	a0, f0
	ret
	.size s31_fpu_hold_yield, .-s31_fpu_hold_yield

	.globl s31_fpu_hold_signal
	.type s31_fpu_hold_signal, @function
s31_fpu_hold_signal:
	fmv.w.x	f0, a0
	mv	a0, a1
	li	a1, 10		/* SIGUSR1 */
	li	a7, 129		/* __NR_kill */
	ecall
	fmv.x.w	a0, f0
	ret
	.size s31_fpu_hold_signal, .-s31_fpu_hold_signal

	.globl s31_hwloop_yield
	.type s31_hwloop_yield, @function
s31_hwloop_yield:
	addi	sp, sp, -16
	sw	s0, 12(sp)
	mv	s0, a0
	li	t0, 64
	/* esp.lp.setup 0,t0,.Lhw_end (offset 20), xesploop v1.0. */
	.word	0x00a2c02b
	li	a7, 124		/* __NR_sched_yield */
	ecall
	lw	t1, 0(s0)
	addi	t1, t1, 1
.Lhw_end:
	sw	t1, 0(s0)
	lw	s0, 12(sp)
	addi	sp, sp, 16
	ret
	.size s31_hwloop_yield, .-s31_hwloop_yield

	.globl s31_hwloop_signal
	.type s31_hwloop_signal, @function
s31_hwloop_signal:
	addi	sp, sp, -16
	sw	s0, 12(sp)
	sw	s1, 8(sp)
	mv	s0, a0
	mv	s1, a1
	li	t0, 8
	/* esp.lp.setup 0,t0,.Lhwsig_end (offset 28), xesploop v1.0. */
	.word	0x00e2c02b
	mv	a0, s1
	li	a1, 10		/* SIGUSR1 */
	li	a7, 129		/* __NR_kill */
	ecall
	lw	t1, 0(s0)
	addi	t1, t1, 1
.Lhwsig_end:
	sw	t1, 0(s0)
	lw	s1, 8(sp)
	lw	s0, 12(sp)
	addi	sp, sp, 16
	ret
	.size s31_hwloop_signal, .-s31_hwloop_signal

	.globl s31_pie_add_u32
	.type s31_pie_add_u32, @function
s31_pie_add_u32:
	.word	0x0311001f	/* esp.vld.128.ip q0,a0,0 */
	.word	0x0311841f	/* esp.vld.128.ip q1,a1,0 */
	.word	0x06a2a31b	/* esp.vadd.u32 q2,q0,q1 */
	.word	0x8312081f	/* esp.vst.128.ip q2,a2,0 */
	ret
	.size s31_pie_add_u32, .-s31_pie_add_u32

	.globl s31_pie_cmp_eq_u8
	.type s31_pie_cmp_eq_u8, @function
s31_pie_cmp_eq_u8:
	.word	0x0311001f	/* esp.vld.128.ip q0,a0,0 */
	.word	0x0311841f	/* esp.vld.128.ip q1,a1,0 */
	.word	0x0740011f	/* esp.vcmp.eq.u8 q2,q0,q1 */
	.word	0x8312081f	/* esp.vst.128.ip q2,a2,0 */
	ret
	.size s31_pie_cmp_eq_u8, .-s31_pie_cmp_eq_u8

	.globl s31_pie_hold_yield
	.type s31_pie_hold_yield, @function
s31_pie_hold_yield:
	.word	0x0311001f	/* esp.vld.128.ip q0,a0,0 */
	li	a7, 124		/* __NR_sched_yield */
	ecall
	.word	0x8311801f	/* esp.vst.128.ip q0,a1,0 */
	ret
	.size s31_pie_hold_yield, .-s31_pie_hold_yield

	.globl s31_pie_hold_signal
	.type s31_pie_hold_signal, @function
s31_pie_hold_signal:
	addi	sp, sp, -16
	sw	a1, 12(sp)
	.word	0x0311001f	/* esp.vld.128.ip q0,a0,0 */
	mv	a0, a2
	li	a1, 10		/* SIGUSR1 */
	li	a7, 129		/* __NR_kill */
	ecall
	lw	a1, 12(sp)
	.word	0x8311801f	/* esp.vst.128.ip q0,a1,0 */
	addi	sp, sp, 16
	ret
	.size s31_pie_hold_signal, .-s31_pie_hold_signal

	/*
	 * Execute every RV32F instruction class and write one observable result
	 * per operation.  FLW is deliberately the first floating-point
	 * instruction: S31's EXT_ILL reason CSR cannot identify FLW/FSW, so this
	 * also exercises Linux's special first-use handling for that erratum.
	 */
	.globl s31_fpu_all
	.type s31_fpu_all, @function
s31_fpu_all:
	mv	t0, a0
	flw	f0, 0(a1)
	fsw	f0, 0(t0)
	li	t1, 0x3fc00000	/* 1.5 */
	li	t2, 0x40000000	/* 2.0 */
	li	t3, 0x3f000000	/* 0.5 */
	fmv.w.x	f0, t1
	fmv.w.x	f1, t2
	fmv.w.x	f2, t3
	fmadd.s	f3, f0, f1, f2
	fsw	f3, 4(t0)
	fmsub.s	f3, f0, f1, f2
	fsw	f3, 8(t0)
	fnmsub.s f3, f0, f1, f2
	fsw	f3, 12(t0)
	fnmadd.s f3, f0, f1, f2
	fsw	f3, 16(t0)
	fadd.s	f3, f0, f1
	fsw	f3, 20(t0)
	fsub.s	f3, f0, f1
	fsw	f3, 24(t0)
	fmul.s	f3, f0, f1
	fsw	f3, 28(t0)
	fdiv.s	f3, f0, f1
	fsw	f3, 32(t0)
	li	t3, 0x40800000	/* 4.0 */
	fmv.w.x	f2, t3
	fsqrt.s f3, f2
	fsw	f3, 36(t0)
	li	t3, 0xc0000000	/* -2.0 */
	fmv.w.x	f2, t3
	fsgnj.s f3, f0, f2
	fsw	f3, 40(t0)
	fsgnjn.s f3, f0, f2
	fsw	f3, 44(t0)
	fsgnjx.s f3, f0, f2
	fsw	f3, 48(t0)
	fmin.s	f3, f0, f2
	fsw	f3, 52(t0)
	fmax.s	f3, f0, f2
	fsw	f3, 56(t0)
	fcvt.w.s t3, f0
	sw	t3, 60(t0)
	fcvt.wu.s t3, f1
	sw	t3, 64(t0)
	fmv.x.w t3, f0
	sw	t3, 68(t0)
	feq.s	t3, f1, f1
	sw	t3, 72(t0)
	flt.s	t3, f0, f1
	sw	t3, 76(t0)
	fle.s	t3, f1, f0
	sw	t3, 80(t0)
	fclass.s t3, f2
	sw	t3, 84(t0)
	li	t3, -3
	fcvt.s.w f3, t3
	fsw	f3, 88(t0)
	li	t3, 3
	fcvt.s.wu f3, t3
	fsw	f3, 92(t0)
	li	t3, 0x3f800000
	fmv.w.x f3, t3
	fsw	f3, 96(t0)
	/* Exercise all six FCSR access pseudo-instructions, then restore FCSR. */
	frcsr	t6
	li	t3, 0
	fscsr	t3
	li	t3, 1
	fsrm	t4, t3
	frrm	t4
	sw	t4, 100(t0)
	li	t3, 0x1f
	fsflags t4, t3
	frflags t4
	sw	t4, 104(t0)
	fscsr	t6
	ret
	.size s31_fpu_all, .-s31_fpu_all

	/* esp.lp.setupi: immediate count, slot 0, two-instruction body. */
	.globl s31_hwloop_setupi
	.type s31_hwloop_setupi, @function
s31_hwloop_setupi:
	li	a0, 0
	.word	0x0052502b	/* esp.lp.setupi 0,5,.Lsetupi_end (+8) */
	addi	a0, a0, 1
.Lsetupi_end:
	nop
	ret
	.size s31_hwloop_setupi, .-s31_hwloop_setupi

	/* esp.lp.starti + endi + counti, using hardware-loop slot 1. */
	.globl s31_hwloop_split_imm
	.type s31_hwloop_split_imm, @function
s31_hwloop_split_imm:
	li	a0, 0
	.word	0x006000ab	/* esp.lp.starti 1,.Lsplit_i_start (+12) */
	.word	0x006010ab	/* esp.lp.endi   1,.Lsplit_i_end   (+12) */
	.word	0x007030ab	/* esp.lp.counti 1,7 */
.Lsplit_i_start:
	addi	a0, a0, 1
.Lsplit_i_end:
	nop
	ret
	.size s31_hwloop_split_imm, .-s31_hwloop_split_imm

	/* esp.lp.starti + endi + register count, using slot 0. */
	.globl s31_hwloop_split_reg
	.type s31_hwloop_split_reg, @function
s31_hwloop_split_reg:
	li	a0, 0
	li	t0, 9
	.word	0x0060002b	/* esp.lp.starti 0,.Lsplit_r_start (+12) */
	.word	0x0060102b	/* esp.lp.endi   0,.Lsplit_r_end   (+12) */
	.word	0x0002a02b	/* esp.lp.count  0,t0 */
.Lsplit_r_start:
	addi	a0, a0, 1
.Lsplit_r_end:
	nop
	ret
	.size s31_hwloop_split_reg, .-s31_hwloop_split_reg

	.globl s31_bitmanip_all
	.type s31_bitmanip_all, @function
s31_bitmanip_all:
	li	t0, 0x12345678
	li	t1, 0x0f0f0f0f
	sh1add	t2, t0, t1
	sw	t2, 0(a0)
	andn	t2, t0, t1
	sw	t2, 4(a0)
	bset	t2, t0, 5
	sw	t2, 8(a0)
	clmul	t2, t0, t1
	sw	t2, 12(a0)
	clmulh	t2, t0, t1
	sw	t2, 16(a0)
	clmulr	t2, t0, t1
	sw	t2, 20(a0)
	ret
	.size s31_bitmanip_all, .-s31_bitmanip_all

	/*
	 * a0 = case index, a1 = start of a 4096-byte aligned scratch buffer.
	 * Every generated case is exactly one 32-bit PIE instruction plus RET,
	 * therefore the dispatcher uses index * 8.  The generated corpus uses
	 * q0..q7, s1 as the scratch base and a0 as a zero byte offset; no case
	 * can clobber ra, sp or t0.
	 */
	.globl s31_pie_run_one
	.type s31_pie_run_one, @function
s31_pie_run_one:
	addi	sp, sp, -16
	sw	ra, 12(sp)
	sw	s0, 8(sp)
	sw	s1, 4(sp)
	slli	t0, a0, 3
	la	t1, .Lpie_cases
	add	t0, t0, t1
	mv	s0, a1
	li	t1, 2048
	add	s1, s0, t1
	li	a0, 0		/* zero byte index for every .xp form */
	jalr	ra, t0, 0
	lw	s1, 4(sp)
	lw	s0, 8(sp)
	lw	ra, 12(sp)
	addi	sp, sp, 16
	ret

	.p2align 2
.Lpie_cases:
#define PIE_CASE(opcode, name) .word opcode ; ret
#include "s31_pie_cases.inc"
#undef PIE_CASE
	.size s31_pie_run_one, .-s31_pie_run_one
