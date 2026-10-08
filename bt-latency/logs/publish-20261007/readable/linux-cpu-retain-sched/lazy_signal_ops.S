	.text
	.option norvc
	.balign 4
	.2byte 0x0001
	.globl s31_pie_add_u32
	.type s31_pie_add_u32, @function
s31_pie_add_u32:
	.word	0x0311001f	/* esp.vld.128.ip q0,a0,0 */
	.word	0x0311841f	/* esp.vld.128.ip q1,a1,0 */
	.word	0x06a2a31b	/* esp.vadd.u32 q2,q0,q1 */
	.word	0x8312081f	/* esp.vst.128.ip q2,a2,0 */
	ret
	.size s31_pie_add_u32, .-s31_pie_add_u32
	.balign 4
	.2byte 0x0001
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
	.section .note.GNU-stack,"",@progbits
