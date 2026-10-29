/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Isolated ESP32-S31 XespV memory/string routines.
 *
 * These symbols are intentionally prefixed: this test library does not
 * replace musl and does not require any toolchain-side libc patch.
 */
	.text
	.align 2

	.macro FUNC name
	.global \name
	.hidden \name
	.type \name,@function
\name:
	.endm

	.macro END name
	.size \name,.-\name
	.endm

/* Internal/probe primitive: out[16] = (a[16] == b[16]) lane masks. */
FUNC s31_xespv_eq16
	esp.vld.128.ip q0,a1,0
	esp.vld.128.ip q1,a2,0
	esp.vcmp.eq.u8 q2,q0,q1
	esp.vst.128.ip q2,a0,0
	ret
END s31_xespv_eq16

/* void *s31_xespv_memcpy(void *d, const void *s, size_t n) */
FUNC s31_xespv_memcpy
	mv t6,a0
	li t0,64
	bltu a2,t0,.Lmc_word
	or t0,a0,a1
	andi t0,t0,15
	bnez t0,.Lmc_word
.Lmc_64:
	esp.vld.128.ip q0,a1,16
	esp.vld.128.ip q1,a1,16
	esp.vld.128.ip q2,a1,16
	esp.vld.128.ip q3,a1,16
	esp.vst.128.ip q0,a0,16
	esp.vst.128.ip q1,a0,16
	esp.vst.128.ip q2,a0,16
	esp.vst.128.ip q3,a0,16
	addi a2,a2,-64
	li t0,64
	bgeu a2,t0,.Lmc_64
.Lmc_16:
	li t0,16
	bltu a2,t0,.Lmc_word
	esp.vld.128.ip q0,a1,16
	esp.vst.128.ip q0,a0,16
	addi a2,a2,-16
	j .Lmc_16
.Lmc_word:
	or t0,a0,a1
	andi t0,t0,3
	bnez t0,.Lmc_byte
.Lmc_word_loop:
	li t0,4
	bltu a2,t0,.Lmc_byte
	lw t1,0(a1)
	sw t1,0(a0)
	addi a0,a0,4
	addi a1,a1,4
	addi a2,a2,-4
	j .Lmc_word_loop
.Lmc_byte:
	beqz a2,.Lmc_done
	lbu t0,0(a1)
	sb t0,0(a0)
	addi a0,a0,1
	addi a1,a1,1
	addi a2,a2,-1
	j .Lmc_byte
.Lmc_done:
	mv a0,t6
	ret
END s31_xespv_memcpy

/* void *s31_xespv_memset(void *d, int c, size_t n) */
FUNC s31_xespv_memset
	mv t6,a0
	andi a1,a1,255
	li t0,64
	bltu a2,t0,.Lms_word_setup
	andi t0,a0,15
	beqz t0,.Lms_pattern
	li t1,16
	sub t0,t1,t0
.Lms_align:
	sb a1,0(a0)
	addi a0,a0,1
	addi a2,a2,-1
	addi t0,t0,-1
	bnez t0,.Lms_align
.Lms_pattern:
	li t0,0x01010101
	mul t1,a1,t0
	addi sp,sp,-16
	sw t1,0(sp)
	sw t1,4(sp)
	sw t1,8(sp)
	sw t1,12(sp)
	mv a3,sp
	esp.vld.128.ip q0,a3,0
	addi sp,sp,16
.Lms_64:
	li t0,64
	bltu a2,t0,.Lms_16
	esp.vst.128.ip q0,a0,16
	esp.vst.128.ip q0,a0,16
	esp.vst.128.ip q0,a0,16
	esp.vst.128.ip q0,a0,16
	addi a2,a2,-64
	j .Lms_64
.Lms_16:
	li t0,16
	bltu a2,t0,.Lms_word_setup
	esp.vst.128.ip q0,a0,16
	addi a2,a2,-16
	j .Lms_16
.Lms_word_setup:
	li t0,0x01010101
	mul t1,a1,t0
.Lms_word:
	li t0,4
	bltu a2,t0,.Lms_byte
	andi t0,a0,3
	bnez t0,.Lms_byte
	sw t1,0(a0)
	addi a0,a0,4
	addi a2,a2,-4
	j .Lms_word
.Lms_byte:
	beqz a2,.Lms_done
	sb a1,0(a0)
	addi a0,a0,1
	addi a2,a2,-1
	j .Lms_byte
.Lms_done:
	mv a0,t6
	ret
END s31_xespv_memset

/* void *s31_xespv_memmove(void *d, const void *s, size_t n) */
FUNC s31_xespv_memmove
	mv t6,a0
	beq a0,a1,.Lmm_done
	bltu a0,a1,.Lmm_forward
	add t0,a1,a2
	bgeu a0,t0,.Lmm_forward
	add a0,a0,a2
	add a1,a1,a2
	li t0,64
	bltu a2,t0,.Lmm_back_byte
	or t0,a0,a1
	andi t0,t0,15
	bnez t0,.Lmm_back_byte
.Lmm_back64:
	li t0,64
	bltu a2,t0,.Lmm_back16
	addi a1,a1,-64
	addi a0,a0,-64
	mv a3,a1
	mv a4,a0
	esp.vld.128.ip q0,a3,16
	esp.vld.128.ip q1,a3,16
	esp.vld.128.ip q2,a3,16
	esp.vld.128.ip q3,a3,16
	esp.vst.128.ip q0,a4,16
	esp.vst.128.ip q1,a4,16
	esp.vst.128.ip q2,a4,16
	esp.vst.128.ip q3,a4,16
	addi a2,a2,-64
	j .Lmm_back64
.Lmm_back16:
	li t0,16
	bltu a2,t0,.Lmm_back_byte
	addi a1,a1,-16
	addi a0,a0,-16
	mv a3,a1
	mv a4,a0
	esp.vld.128.ip q0,a3,0
	esp.vst.128.ip q0,a4,0
	addi a2,a2,-16
	j .Lmm_back16
.Lmm_back_byte:
	beqz a2,.Lmm_done
	addi a0,a0,-1
	addi a1,a1,-1
	lbu t0,0(a1)
	sb t0,0(a0)
	addi a2,a2,-1
	j .Lmm_back_byte
.Lmm_forward:
	mv a0,t6
	tail s31_xespv_memcpy
.Lmm_done:
	mv a0,t6
	ret
END s31_xespv_memmove

/* Build a 16-byte byte pattern in q1. Clobbers t0,t1,t3 and 16 stack bytes. */
	.macro BYTE_PATTERN reg
	andi \reg,\reg,255
	li t0,0x01010101
	mul t1,\reg,t0
	addi sp,sp,-16
	sw t1,0(sp)
	sw t1,4(sp)
	sw t1,8(sp)
	sw t1,12(sp)
	mv a3,sp
	esp.vld.128.ip q1,a3,0
	addi sp,sp,16
	.endm

/* void *s31_xespv_memchr(const void *s, int c, size_t n) */
FUNC s31_xespv_memchr
	BYTE_PATTERN a1
.Lmch_align:
	beqz a2,.Lmch_null
	andi t0,a0,15
	beqz t0,.Lmch_vec_check
	lbu t0,0(a0)
	beq t0,a1,.Lmch_done
	addi a0,a0,1
	addi a2,a2,-1
	j .Lmch_align
.Lmch_vec_check:
	li t0,16
	bltu a2,t0,.Lmch_tail
	addi sp,sp,-32
	sw s0,16(sp)
	mv s0,a0
	esp.vld.128.ip q0,a0,0
	esp.vcmp.eq.u8 q2,q0,q1
	mv a3,sp
	esp.vst.128.ip q2,a3,0
	fence rw,rw
	lw t0,0(sp)
	lw t1,4(sp)
	or t0,t0,t1
	lw t1,8(sp)
	or t0,t0,t1
	lw t1,12(sp)
	or t0,t0,t1
	mv a0,s0
	lw s0,16(sp)
	addi sp,sp,32
	bnez t0,.Lmch_hit
	addi a0,a0,16
	addi a2,a2,-16
	j .Lmch_vec_check
.Lmch_hit:
	lbu t0,0(a0)
	beq t0,a1,.Lmch_done
	addi a0,a0,1
	addi a2,a2,-1
	j .Lmch_hit
.Lmch_tail:
	beqz a2,.Lmch_null
	lbu t0,0(a0)
	beq t0,a1,.Lmch_done
	addi a0,a0,1
	addi a2,a2,-1
	j .Lmch_tail
.Lmch_null:
	li a0,0
.Lmch_done:
	ret
END s31_xespv_memchr

/* void *s31_xespv_memrchr(const void *s, int c, size_t n) */
FUNC s31_xespv_memrchr
	add a0,a0,a2
	BYTE_PATTERN a1
.Lmr_align:
	beqz a2,.Lmr_null
	andi t0,a0,15
	beqz t0,.Lmr_vec_check
	addi a0,a0,-1
	lbu t0,0(a0)
	beq t0,a1,.Lmr_done
	addi a2,a2,-1
	j .Lmr_align
.Lmr_vec_check:
	li t0,16
	bltu a2,t0,.Lmr_tail
	addi a0,a0,-16
	addi sp,sp,-32
	sw s0,16(sp)
	mv s0,a0
	esp.vld.128.ip q0,a0,0
	esp.vcmp.eq.u8 q2,q0,q1
	mv a3,sp
	esp.vst.128.ip q2,a3,0
	fence rw,rw
	lw t0,0(sp)
	lw t1,4(sp)
	or t0,t0,t1
	lw t1,8(sp)
	or t0,t0,t1
	lw t1,12(sp)
	or t0,t0,t1
	mv a0,s0
	lw s0,16(sp)
	addi sp,sp,32
	bnez t0,.Lmr_hit
	addi a2,a2,-16
	j .Lmr_vec_check
.Lmr_hit:
	addi a0,a0,16
.Lmr_hit_loop:
	addi a0,a0,-1
	lbu t0,0(a0)
	beq t0,a1,.Lmr_done
	j .Lmr_hit_loop
.Lmr_tail:
	beqz a2,.Lmr_null
	addi a0,a0,-1
	lbu t0,0(a0)
	beq t0,a1,.Lmr_done
	addi a2,a2,-1
	j .Lmr_tail
.Lmr_null:
	li a0,0
.Lmr_done:
	ret
END s31_xespv_memrchr

/* int s31_xespv_memcmp(const void *a, const void *b, size_t n) */
FUNC s31_xespv_memcmp
.Lcmp_align:
	beqz a2,.Lcmp_equal
	or t0,a0,a1
	andi t0,t0,15
	beqz t0,.Lcmp_vec
.Lcmp_byte:
	lbu t0,0(a0)
	lbu t1,0(a1)
	bne t0,t1,.Lcmp_diff
	addi a0,a0,1
	addi a1,a1,1
	addi a2,a2,-1
	bnez a2,.Lcmp_align
.Lcmp_equal:
	li a0,0
	ret
.Lcmp_vec:
	li t0,16
	bltu a2,t0,.Lcmp_byte
	addi sp,sp,-48
	sw s0,32(sp)
	sw s1,36(sp)
	sw s2,40(sp)
	sw ra,44(sp)
	mv s0,a0
	mv s1,a1
	mv s2,a2
	mv a2,a1
	mv a1,a0
	mv a0,sp
	call s31_xespv_eq16
	lw t0,0(sp)
	lw t1,4(sp)
	and t0,t0,t1
	lw t1,8(sp)
	and t0,t0,t1
	lw t1,12(sp)
	and t0,t0,t1
	mv a0,s0
	mv a1,s1
	mv a2,s2
	lw s0,32(sp)
	lw s1,36(sp)
	lw s2,40(sp)
	lw ra,44(sp)
	addi sp,sp,48
	li t1,-1
	bne t0,t1,.Lcmp_byte
	addi a0,a0,16
	addi a1,a1,16
	addi a2,a2,-16
	j .Lcmp_align
.Lcmp_diff:
	sub a0,t0,t1
	ret
END s31_xespv_memcmp

/* size_t s31_xespv_strnlen(const char *s, size_t maxlen) */
FUNC s31_xespv_strnlen
	mv t6,a0
	addi sp,sp,-16
	sw zero,0(sp)
	sw zero,4(sp)
	sw zero,8(sp)
	sw zero,12(sp)
	mv a3,sp
	esp.vld.128.ip q1,a3,0
	addi sp,sp,16
.Lsn_align:
	beqz a1,.Lsn_done
	andi t0,a0,15
	beqz t0,.Lsn_vec
	lbu t0,0(a0)
	beqz t0,.Lsn_done
	addi a0,a0,1
	addi a1,a1,-1
	j .Lsn_align
.Lsn_vec:
	li t0,16
	bltu a1,t0,.Lsn_tail
	addi sp,sp,-32
	sw s0,16(sp)
	mv s0,a0
	esp.vld.128.ip q0,a0,0
	esp.vcmp.eq.u8 q2,q0,q1
	mv a3,sp
	esp.vst.128.ip q2,a3,0
	fence rw,rw
	lw t0,0(sp)
	lw t1,4(sp)
	or t0,t0,t1
	lw t1,8(sp)
	or t0,t0,t1
	lw t1,12(sp)
	or t0,t0,t1
	mv a0,s0
	lw s0,16(sp)
	addi sp,sp,32
	bnez t0,.Lsn_tail
	addi a0,a0,16
	addi a1,a1,-16
	j .Lsn_vec
.Lsn_tail:
	beqz a1,.Lsn_done
	lbu t0,0(a0)
	beqz t0,.Lsn_done
	addi a0,a0,1
	addi a1,a1,-1
	j .Lsn_tail
.Lsn_done:
	sub a0,a0,t6
	ret
END s31_xespv_strnlen

/* size_t s31_xespv_strlen(const char *s) */
FUNC s31_xespv_strlen
	li a1,-1
	tail s31_xespv_strnlen
END s31_xespv_strlen

/* char *s31_xespv_strchrnul(const char *s, int c) */
FUNC s31_xespv_strchrnul
	BYTE_PATTERN a1
	addi sp,sp,-16
	sw zero,0(sp)
	sw zero,4(sp)
	sw zero,8(sp)
	sw zero,12(sp)
	mv a3,sp
	esp.vld.128.ip q3,a3,0
	addi sp,sp,16
.Lsc_align:
	andi t0,a0,15
	beqz t0,.Lsc_vec
.Lsc_byte:
	lbu t0,0(a0)
	beqz t0,.Lsc_done
	beq t0,a1,.Lsc_done
	addi a0,a0,1
	j .Lsc_align
.Lsc_vec:
	addi sp,sp,-48
	sw s0,32(sp)
	mv s0,a0
	esp.vld.128.ip q0,a0,0
	esp.vcmp.eq.u8 q2,q0,q1
	esp.vcmp.eq.u8 q4,q0,q3
	mv a3,sp
	esp.vst.128.ip q4,a3,16
	esp.vst.128.ip q2,a3,0
	fence rw,rw
	lw t0,0(sp)
	lw t1,4(sp)
	or t0,t0,t1
	lw t1,8(sp)
	or t0,t0,t1
	lw t1,12(sp)
	or t0,t0,t1
	lw t1,16(sp)
	or t0,t0,t1
	lw t1,20(sp)
	or t0,t0,t1
	lw t1,24(sp)
	or t0,t0,t1
	lw t1,28(sp)
	or t0,t0,t1
	mv a0,s0
	lw s0,32(sp)
	addi sp,sp,48
	bnez t0,.Lsc_byte
	addi a0,a0,16
	j .Lsc_vec
.Lsc_done:
	ret
END s31_xespv_strchrnul

/* char *s31_xespv_strchr(const char *s, int c) */
FUNC s31_xespv_strchr
	addi sp,sp,-16
	sw ra,12(sp)
	sw s0,8(sp)
	mv s0,a1
	call s31_xespv_strchrnul
	lbu t0,0(a0)
	beq t0,s0,.Lsch_done
	li a0,0
.Lsch_done:
	lw s0,8(sp)
	lw ra,12(sp)
	addi sp,sp,16
	ret
END s31_xespv_strchr

/* int s31_xespv_strcmp(const char *a, const char *b) */
FUNC s31_xespv_strcmp
	xor t0,a0,a1
	andi t0,t0,15
	bnez t0,.Lstcmp_byte
	addi sp,sp,-16
	sw zero,0(sp)
	sw zero,4(sp)
	sw zero,8(sp)
	sw zero,12(sp)
	mv a3,sp
	esp.vld.128.ip q3,a3,0
	addi sp,sp,16
.Lstcmp_align:
	andi t0,a0,15
	beqz t0,.Lstcmp_vec
.Lstcmp_byte:
	lbu t0,0(a0)
	lbu t1,0(a1)
	bne t0,t1,.Lstcmp_diff
	beqz t0,.Lstcmp_equal
	addi a0,a0,1
	addi a1,a1,1
	j .Lstcmp_align
.Lstcmp_vec:
	addi sp,sp,-48
	sw s0,32(sp)
	sw s1,36(sp)
	mv s0,a0
	mv s1,a1
	esp.vld.128.ip q0,a0,0
	esp.vld.128.ip q1,a1,0
	esp.vcmp.eq.u8 q2,q0,q1
	esp.vcmp.eq.u8 q4,q0,q3
	mv a3,sp
	esp.vst.128.ip q4,a3,16
	esp.vst.128.ip q2,a3,0
	fence rw,rw
	lw t0,0(sp)
	lw t1,4(sp)
	or t0,t0,t1
	lw t1,8(sp)
	or t0,t0,t1
	lw t1,12(sp)
	or t0,t0,t1
	lw t2,16(sp)
	lw t1,20(sp)
	and t2,t2,t1
	lw t1,24(sp)
	and t2,t2,t1
	lw t1,28(sp)
	and t2,t2,t1
	mv a0,s0
	mv a1,s1
	lw s0,32(sp)
	lw s1,36(sp)
	addi sp,sp,48
	li t1,-1
	bne t2,t1,.Lstcmp_byte
	bnez t0,.Lstcmp_byte
	addi a0,a0,16
	addi a1,a1,16
	j .Lstcmp_vec
.Lstcmp_diff:
	sub a0,t0,t1
	ret
.Lstcmp_equal:
	li a0,0
	ret
END s31_xespv_strcmp

/* char *s31_xespv_stpcpy(char *d, const char *s) */
FUNC s31_xespv_stpcpy
	xor t0,a0,a1
	andi t0,t0,15
	bnez t0,.Lstpc_byte
	addi sp,sp,-16
	sw zero,0(sp)
	sw zero,4(sp)
	sw zero,8(sp)
	sw zero,12(sp)
	mv a3,sp
	esp.vld.128.ip q1,a3,0
	addi sp,sp,16
.Lstpc_align:
	andi t0,a1,15
	beqz t0,.Lstpc_vec
.Lstpc_byte:
	lbu t0,0(a1)
	sb t0,0(a0)
	beqz t0,.Lstpc_done
	addi a0,a0,1
	addi a1,a1,1
	j .Lstpc_align
.Lstpc_vec:
	addi sp,sp,-32
	sw s0,16(sp)
	sw s1,20(sp)
	mv s0,a0
	mv s1,a1
	esp.vld.128.ip q0,a1,0
	esp.vcmp.eq.u8 q2,q0,q1
	mv a3,sp
	esp.vst.128.ip q2,a3,0
	fence rw,rw
	lw t0,0(sp)
	lw t1,4(sp)
	or t0,t0,t1
	lw t1,8(sp)
	or t0,t0,t1
	lw t1,12(sp)
	or t0,t0,t1
	mv a0,s0
	mv a1,s1
	lw s0,16(sp)
	lw s1,20(sp)
	addi sp,sp,32
	bnez t0,.Lstpc_byte
	esp.vst.128.ip q0,a0,0
	addi a0,a0,16
	addi a1,a1,16
	j .Lstpc_vec
.Lstpc_done:
	ret
END s31_xespv_stpcpy

/* char *s31_xespv_strcpy(char *d, const char *s) */
FUNC s31_xespv_strcpy
	addi sp,sp,-16
	sw ra,12(sp)
	sw s0,8(sp)
	mv s0,a0
	call s31_xespv_stpcpy
	mv a0,s0
	lw s0,8(sp)
	lw ra,12(sp)
	addi sp,sp,16
	ret
END s31_xespv_strcpy

/* void *s31_xespv_memccpy(void *d, const void *s, int c, size_t n) */
FUNC s31_xespv_memccpy
	addi sp,sp,-32
	sw ra,28(sp)
	sw s0,24(sp)
	sw s1,20(sp)
	sw s2,16(sp)
	mv s0,a0
	mv s1,a1
	mv s2,a3
	mv a0,a1
	mv a1,a2
	mv a2,a3
	call s31_xespv_memchr
	beqz a0,.Lmcc_none
	sub s2,a0,s1
	addi s2,s2,1
.Lmcc_copy:
	mv a0,s0
	mv a1,s1
	mv a2,s2
	call s31_xespv_memcpy
	add a0,s0,s2
	j .Lmcc_done
.Lmcc_none:
	beqz s2,.Lmcc_done
	mv a0,s0
	mv a1,s1
	mv a2,s2
	call s31_xespv_memcpy
	li a0,0
.Lmcc_done:
	lw s2,16(sp)
	lw s1,20(sp)
	lw s0,24(sp)
	lw ra,28(sp)
	addi sp,sp,32
	ret
END s31_xespv_memccpy

/* char *s31_xespv_stpncpy(char *d, const char *s, size_t n) */
FUNC s31_xespv_stpncpy
	addi sp,sp,-32
	sw ra,28(sp)
	sw s0,24(sp)
	sw s1,20(sp)
	sw s2,16(sp)
	sw s3,12(sp)
	mv s0,a0
	mv s1,a1
	mv s2,a2
	mv a0,a1
	mv a1,a2
	call s31_xespv_strnlen
	mv s3,a0
	beq s3,s2,.Lstpn_full
	addi t1,s3,1
	j .Lstpn_copy
.Lstpn_full:
	mv t1,s3
.Lstpn_copy:
	mv a0,s0
	mv a1,s1
	mv a2,t1
	call s31_xespv_memcpy
	beq s3,s2,.Lstpn_ret_end
	add s1,s0,s3
	addi t1,s3,1
	addi a0,s1,1
	sub a2,s2,t1
	li a1,0
	call s31_xespv_memset
	mv a0,s1
	j .Lstpn_done
.Lstpn_ret_end:
	add a0,s0,s2
.Lstpn_done:
	lw s3,12(sp)
	lw s2,16(sp)
	lw s1,20(sp)
	lw s0,24(sp)
	lw ra,28(sp)
	addi sp,sp,32
	ret
END s31_xespv_stpncpy

/* char *s31_xespv_strncpy(char *d, const char *s, size_t n) */
FUNC s31_xespv_strncpy
	addi sp,sp,-16
	sw ra,12(sp)
	sw s0,8(sp)
	mv s0,a0
	call s31_xespv_stpncpy
	mv a0,s0
	lw s0,8(sp)
	lw ra,12(sp)
	addi sp,sp,16
	ret
END s31_xespv_strncpy

/* size_t s31_xespv_strlcpy(char *d, const char *s, size_t size) */
FUNC s31_xespv_strlcpy
	addi sp,sp,-32
	sw ra,28(sp)
	sw s0,24(sp)
	sw s1,20(sp)
	sw s2,16(sp)
	sw s3,12(sp)
	sw s4,8(sp)
	mv s0,a0
	mv s1,a1
	mv s2,a2
	mv a0,a1
	call s31_xespv_strlen
	mv s3,a0
	mv s4,s3
	beqz s2,.Lsl_done
	addi t1,s2,-1
	bltu s4,t1,.Lsl_len_ok
	mv s4,t1
.Lsl_len_ok:
	mv a0,s0
	mv a1,s1
	mv a2,s4
	call s31_xespv_memcpy
	add t1,s0,s4
	sb zero,0(t1)
.Lsl_done:
	mv a0,s3
	lw s4,8(sp)
	lw s3,12(sp)
	lw s2,16(sp)
	lw s1,20(sp)
	lw s0,24(sp)
	lw ra,28(sp)
	addi sp,sp,32
	ret
END s31_xespv_strlcpy

/* Saturating unsigned byte add, matching esp.vadd.u8 hardware semantics. */
FUNC s31_xespv_add_sat_u8
	/* The 128-bit path requires all three pointers to be 16-byte aligned. */
	or t0,a0,a1
	or t0,t0,a2
	andi t0,t0,15
	bnez t0,.Lva_byte
.Lva_vec:
	li t0,16
	bltu a3,t0,.Lva_byte
	esp.vld.128.ip q0,a1,16
	esp.vld.128.ip q1,a2,16
	esp.vadd.u8 q2,q0,q1
	esp.vst.128.ip q2,a0,16
	addi a3,a3,-16
	j .Lva_vec
.Lva_byte:
	beqz a3,.Lva_done
	lbu t0,0(a1)
	lbu t1,0(a2)
	add t0,t0,t1
	sltiu t1,t0,256
	bnez t1,.Lva_store
	li t0,255
.Lva_store:
	sb t0,0(a0)
	addi a0,a0,1
	addi a1,a1,1
	addi a2,a2,1
	addi a3,a3,-1
	j .Lva_byte
.Lva_done:
	ret
END s31_xespv_add_sat_u8

	.section .note.GNU-stack,"",@progbits
