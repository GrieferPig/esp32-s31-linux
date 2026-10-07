
/home/grieferpig/esp32-s31-linux/out/linux/vmlinux:     file format elf32-littleriscv


Disassembly of section .head.text:

Disassembly of section .init.text:

Disassembly of section .exit.text:

Disassembly of section .text:

Disassembly of section .s31_rx_ram.text:

c0824fdc <_raw_spin_unlock_irqrestore>:
c0824fdc:	1101                	addi	sp,sp,-32
c0824fde:	ce06                	sw	ra,28(sp)
c0824fe0:	c62a                	sw	a0,12(sp)
c0824fe2:	c42e                	sw	a1,8(sp)
c0824fe4:	00000097          	auipc	ra,0x0
c0824fe8:	168080e7          	jalr	360(ra) # c082514c <mmiowb_spin_unlock>
c0824fec:	4532                	lw	a0,12(sp)
c0824fee:	0a0527af          	amoswap.w.rl	a5,zero,(a0)
c0824ff2:	45a2                	lw	a1,8(sp)
c0824ff4:	8989                	andi	a1,a1,2
c0824ff6:	1005a073          	csrs	sstatus,a1
c0824ffa:	40f2                	lw	ra,28(sp)
c0824ffc:	6105                	addi	sp,sp,32
c0824ffe:	8082                	ret
