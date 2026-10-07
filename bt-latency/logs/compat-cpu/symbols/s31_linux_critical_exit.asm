
/home/grieferpig/esp32-s31-linux/out/linux/drivers/platform/esp32s31-radio.ko:     file format elf32-littleriscv


Disassembly of section .init.text:

Disassembly of section .exit.text:

Disassembly of section .text:

000058aa <s31_linux_critical_exit>:
    58aa:	1141                	addi	sp,sp,-16
    58ac:	c422                	sw	s0,8(sp)
    58ae:	c226                	sw	s1,4(sp)
    58b0:	c606                	sw	ra,12(sp)
    58b2:	842a                	mv	s0,a0
    58b4:	00000097          	auipc	ra,0x0
    58b8:	000080e7          	jalr	ra # 58b4 <s31_linux_critical_exit+0xa>
    58bc:	84aa                	mv	s1,a0
    58be:	00000517          	auipc	a0,0x0
    58c2:	00050513          	mv	a0,a0
    58c6:	00000097          	auipc	ra,0x0
    58ca:	000080e7          	jalr	ra # 58c6 <s31_linux_critical_exit+0x1c>
    58ce:	c491                	beqz	s1,58da <s31_linux_critical_exit+0x30>
    58d0:	5cc0                	lw	s0,60(s1)
    58d2:	020484a3          	sb	zero,41(s1)
    58d6:	0204ae23          	sw	zero,60(s1)
    58da:	8809                	andi	s0,s0,2
    58dc:	10042073          	csrs	sstatus,s0
    58e0:	4422                	lw	s0,8(sp)
    58e2:	40b2                	lw	ra,12(sp)
    58e4:	4492                	lw	s1,4(sp)
    58e6:	0141                	addi	sp,sp,16
    58e8:	00000317          	auipc	t1,0x0
    58ec:	00030067          	jr	t1 # 58e8 <s31_linux_critical_exit+0x3e>

Disassembly of section .text.unlikely:
