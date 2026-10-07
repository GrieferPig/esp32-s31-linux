
/home/grieferpig/esp32-s31-linux/out/linux/vmlinux:     file format elf32-littleriscv


Disassembly of section .head.text:

Disassembly of section .init.text:

Disassembly of section .exit.text:

Disassembly of section .text:

Disassembly of section .s31_rx_ram.text:

c081b6a8 <finish_task_switch.isra.0>:
c081b6a8:	1101                	addi	sp,sp,-32
c081b6aa:	cc22                	sw	s0,24(sp)
c081b6ac:	ca26                	sw	s1,20(sp)
c081b6ae:	c64e                	sw	s3,12(sp)
c081b6b0:	ce06                	sw	ra,28(sp)
c081b6b2:	c84a                	sw	s2,16(sp)
c081b6b4:	c452                	sw	s4,8(sp)
c081b6b6:	1000                	addi	s0,sp,32
c081b6b8:	01022783          	lw	a5,16(tp) # 10 <_edata_loc-0x5f1bbc>
c081b6bc:	c08bf737          	lui	a4,0xc08bf
c081b6c0:	7cc70713          	addi	a4,a4,1996 # c08bf7cc <__per_cpu_offset>
c081b6c4:	20e7c7b3          	sh2add	a5,a5,a4
c081b6c8:	439c                	lw	a5,0(a5)
c081b6ca:	c08ea4b7          	lui	s1,0xc08ea
c081b6ce:	e4048493          	addi	s1,s1,-448 # c08e9e40 <runqueues>
c081b6d2:	94be                	add	s1,s1,a5
c081b6d4:	00422783          	lw	a5,4(tp) # 4 <_edata_loc-0x5f1bc8>
c081b6d8:	5c04a903          	lw	s2,1472(s1)
c081b6dc:	89aa                	mv	s3,a0
c081b6de:	cb95                	beqz	a5,c081b712 <finish_task_switch.isra.0+0x6a>
c081b6e0:	c08a37b7          	lui	a5,0xc08a3
c081b6e4:	0dc7c703          	lbu	a4,220(a5) # c08a30dc <__already_done.6>
c081b6e8:	e31d                	bnez	a4,c081b70e <finish_task_switch.isra.0+0x66>
c081b6ea:	00422683          	lw	a3,4(tp) # 4 <_edata_loc-0x5f1bc8>
c081b6ee:	2e022603          	lw	a2,736(tp) # 2e0 <_edata_loc-0x5f18ec>
c081b6f2:	c0469537          	lui	a0,0xc0469
c081b6f6:	4705                	li	a4,1
c081b6f8:	40420593          	addi	a1,tp,1028 # 404 <_edata_loc-0x5f17c8>
c081b6fc:	12450513          	addi	a0,a0,292 # c0469124 <__dtb_empty_root_end+0x1bc81>
c081b700:	0ce78e23          	sb	a4,220(a5)
c081b704:	ff81d097          	auipc	ra,0xff81d
c081b708:	840080e7          	jalr	-1984(ra) # c0037f44 <__warn_printk>
c081b70c:	9002                	ebreak
c081b70e:	00022223          	sw	zero,4(tp) # 4 <_edata_loc-0x5f1bc8>
c081b712:	5c04a023          	sw	zero,1472(s1)
c081b716:	0189aa03          	lw	s4,24(s3)
c081b71a:	0310000f          	fence	rw,w
c081b71e:	8526                	mv	a0,s1
c081b720:	0209a823          	sw	zero,48(s3)
c081b724:	00000097          	auipc	ra,0x0
c081b728:	9d4080e7          	jalr	-1580(ra) # c081b0f8 <__balance_callbacks>
c081b72c:	8526                	mv	a0,s1
c081b72e:	00000097          	auipc	ra,0x0
c081b732:	898080e7          	jalr	-1896(ra) # c081afc6 <__raw_spin_unlock>
c081b736:	10016073          	csrsi	sstatus,2
c081b73a:	02090a63          	beqz	s2,c081b76e <finish_task_switch.isra.0+0xc6>
c081b73e:	28822783          	lw	a5,648(tp) # 288 <_edata_loc-0x5f1944>
c081b742:	00f91b63          	bne	s2,a5,c081b758 <finish_task_switch.isra.0+0xb0>
c081b746:	05c90793          	addi	a5,s2,92
c081b74a:	0407a7af          	amoadd.w.aq	a5,zero,(a5)
c081b74e:	0207f793          	andi	a5,a5,32
c081b752:	c399                	beqz	a5,c081b758 <finish_task_switch.isra.0+0xb0>
c081b754:	0000100f          	fence.i
c081b758:	57fd                	li	a5,-1
c081b75a:	06f927af          	amoadd.w.aqrl	a5,a5,(s2)
c081b75e:	4705                	li	a4,1
c081b760:	00e79763          	bne	a5,a4,c081b76e <finish_task_switch.isra.0+0xc6>
c081b764:	854a                	mv	a0,s2
c081b766:	ff81b097          	auipc	ra,0xff81b
c081b76a:	9b0080e7          	jalr	-1616(ra) # c0036116 <__mmdrop>
c081b76e:	08000793          	li	a5,128
c081b772:	02fa1963          	bne	s4,a5,c081b7a4 <finish_task_switch.isra.0+0xfc>
c081b776:	2249a783          	lw	a5,548(s3)
c081b77a:	47fc                	lw	a5,76(a5)
c081b77c:	c399                	beqz	a5,c081b782 <finish_task_switch.isra.0+0xda>
c081b77e:	854e                	mv	a0,s3
c081b780:	9782                	jalr	a5
c081b782:	854e                	mv	a0,s3
c081b784:	ff81b097          	auipc	ra,0xff81b
c081b788:	bfc080e7          	jalr	-1028(ra) # c0036380 <put_task_stack>
c081b78c:	4462                	lw	s0,24(sp)
c081b78e:	40f2                	lw	ra,28(sp)
c081b790:	44d2                	lw	s1,20(sp)
c081b792:	4942                	lw	s2,16(sp)
c081b794:	4a22                	lw	s4,8(sp)
c081b796:	854e                	mv	a0,s3
c081b798:	49b2                	lw	s3,12(sp)
c081b79a:	6105                	addi	sp,sp,32
c081b79c:	ff81f317          	auipc	t1,0xff81f
c081b7a0:	75030067          	jr	1872(t1) # c003aeec <put_task_struct_rcu_user>
c081b7a4:	40f2                	lw	ra,28(sp)
c081b7a6:	4462                	lw	s0,24(sp)
c081b7a8:	44d2                	lw	s1,20(sp)
c081b7aa:	4942                	lw	s2,16(sp)
c081b7ac:	49b2                	lw	s3,12(sp)
c081b7ae:	4a22                	lw	s4,8(sp)
c081b7b0:	6105                	addi	sp,sp,32
c081b7b2:	8082                	ret
