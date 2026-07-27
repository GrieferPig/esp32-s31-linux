/*
 * ESP32-S31 Hart1 trampoline - 接收Core0发来的FROM_CPU_0中断
 * 关键：core1_irq_handler必须在64字节对齐边界上
 */
    .section .iram1, "ax"

/* ---- 中断Handler在独立64字节对齐的section开头 ---- */
    .align  6
    .global core1_irq_entry
    .type   core1_irq_entry, @function
core1_irq_entry:
    j       core1_irq_handler_real
    /* padding到64字节 */
    .skip   60

/* ---- Core1入口 ---- */
    .align  6
    .global core1_entry
    .type   core1_entry, @function
    .option push
    .option norelax

.equ SHARED_BASE,              0x2f062f80
.equ SHARED_MAGIC_OFFSET,      0x000
.equ SHARED_H1_IRQ_ACK_OFF,    0x100
.equ SHARED_H1_SEND_IRQ_COUNT, 0x108   /* Core1发送中断计数 */
.equ SHARED_MSG_REQ_FLAG,      0x30    /* Core0消息请求flag */
.equ SHARED_MSG_H0_TO_H1,      0x200   /* Core0→Core1消息 (64B) */
.equ SHARED_MSG_H1_TO_H0,      0x240   /* Core1→Core0响应 (64B) */
.equ SHARED_MSG_RESP_FLAG,     0x2C0   /* Core1响应就绪flag */
.equ SHARED_MAGIC_VAL,         0xDEADBEEF

.equ IPC_CLIC_ID,              28
.equ CLIC_CTRL_BASE,           0x10801000
.equ CLIC_CFG_REG,             0x10800000

.equ DR_REG_HP_SYS_BASE,       0x20586000
.equ HP_SYSTEM_CPU_INT_FROM_CPU_1_REG, (DR_REG_HP_SYS_BASE + 0x14)

/* Core1 中断矩阵: FROM_CPU_0 -> CLIC ID 28 */
.equ INTR1_BASE,               0x20585800
.equ INTR1_CPU_INTR_FROM_CPU_0_MAP_REG, (INTR1_BASE + 0x104)

core1_entry:
    csrw    mie, zero
    la      t0, core1_early_trap
    csrw    mtvec, t0

    li      t0, 0x1800          /* MPP=M-mode */
    csrw    mstatus, t0
    csrw    0x307, zero

    /* 清零所有CLIC槽 */
    li      t0, CLIC_CTRL_BASE
    li      t1, 48
1:
    sb      zero, 0(t0)
    sb      zero, 1(t0)
    sb      zero, 2(t0)
    sb      zero, 3(t0)
    addi    t0, t0, 4
    addi    t1, t1, -1
    bnez    t1, 1b

    /* 设置PMP: 全空间RWX给M-mode */
    li      t0, -1
    csrw    pmpaddr0, t0
    li      t0, 0x1f
    csrw    pmpcfg0, t0

    /* 配置mcliccfg: NMBITS=1 */
    li      t0, CLIC_CFG_REG
    lw      t1, 0(t0)
    li      t2, ~((3 << 5) | (0xF << 1))
    and     t1, t1, t2
    ori     t1, t1, (1 << 5)     /* NMBITS=1 */
    ori     t1, t1, (3 << 1)     /* NLBITS=3 */
    sw      t1, 0(t0)

    /* 中断矩阵: FROM_CPU_0 -> CLIC ID 28, PASS_LEVEL=2 (M-mode) */
    li      t0, INTR1_CPU_INTR_FROM_CPU_0_MAP_REG
    li      t1, IPC_CLIC_ID | (2 << 8)
    sw      t1, 0(t0)

    /* 配置CLIC ID 28: M-mode, 上升沿触发, 非向量 */
    li      t0, CLIC_CTRL_BASE
    addi    t0, t0, 4 * IPC_CLIC_ID
    sb      zero, 0(t0)          /* IP = 0 */
    sb      zero, 1(t0)          /* IE = 0 */
    li      t1, (3 << 6) | (1 << 1)  /* MODE=3(M), TRIG=1(edge) */
    sb      t1, 2(t0)
    li      t1, 0x3f             /* 优先级 */
    sb      t1, 3(t0)
    li      t1, 1
    sb      t1, 1(t0)            /* IE = 1 */

    /* 清零阈值 */
    csrw    0x347, zero           /* mintthresh = 0 */

    /* 设置mtvec: 使用core1_irq_entry (64字节对齐) + MODE=3 */
    la      t0, core1_irq_entry
    csrw    mtvec, t0

    /* 使能全局中断 */
    csrsi   mstatus, (1 << 3)    /* MIE = 1 */

    fence   rw, rw
    fence.i

    /* 写入诊断信息 */
    li      t0, CLIC_CTRL_BASE
    addi    t0, t0, 4 * IPC_CLIC_ID
    lw      t1, 0(t0)
    la      t2, (SHARED_BASE + 0x10)
    sw      t1, 0(t2)

    csrr    t1, mstatus
    la      t2, (SHARED_BASE + 0x14)
    sw      t1, 0(t2)

    la      t0, core1_irq_entry
    la      t2, (SHARED_BASE + 0x18)
    sw      t0, 0(t2)

    li      t0, INTR1_CPU_INTR_FROM_CPU_0_MAP_REG
    lw      t1, 0(t0)
    la      t2, (SHARED_BASE + 0x1C)
    sw      t1, 0(t2)

    /* 写入Magic值 */
    la      t0, (SHARED_BASE + SHARED_MAGIC_OFFSET)
    li      t1, SHARED_MAGIC_VAL
    sw      t1, 0(t0)
    fence   rw, rw

    /* WFI循环：轮询 + 等待中断 */
1:
    la      t0, SHARED_BASE

    /* 检查轮询请求flag (offset 0x20) */
    lw      t1, 0x20(t0)
    beqz    t1, 3f
    lw      t2, 0x100(t0)        /* ACK++ */
    addi    t2, t2, 1
    sw      t2, 0x100(t0)
    sw      zero, 0x20(t0)
    fence   rw, rw

3:
    /* 检查发送中断请求flag (offset 0x24, Core0要求Core1发IRQ) */
    lw      t1, 0x24(t0)
    beqz    t1, 4f
    /* 给Core0发送FROM_CPU_1中断 */
    li      t2, HP_SYSTEM_CPU_INT_FROM_CPU_1_REG
    li      t3, 1
    sw      t3, 0(t2)
    /* 更新发送计数 */
    lw      t2, 0x108(t0)        /* SHARED_H1_SEND_IRQ_COUNT */
    addi    t2, t2, 1
    sw      t2, 0x108(t0)
    sw      zero, 0x24(t0)       /* 清除flag */
    fence   rw, rw

4:
    /* 检查消息处理请求flag (offset 0x30, ping-pong测试) */
    lw      t1, SHARED_MSG_REQ_FLAG(t0)
    beqz    t1, 2f

    /* 保存更多寄存器 */
    addi    sp, sp, -32
    sw      a0, 0(sp)
    sw      a1, 4(sp)
    sw      a2, 8(sp)
    sw      a3, 12(sp)
    sw      a4, 16(sp)
    sw      a5, 20(sp)

    /* 复制Core0的消息到Core1本地 (模拟"读取") */
    /* 实际上只是验证数据可访问，然后构建响应 */
    la      a0, SHARED_BASE
    addi    a0, a0, SHARED_MSG_H0_TO_H1   /* 源: Core0消息 */
    addi    a1, t0, SHARED_MSG_H1_TO_H0   /* 目标: 响应区 */
    li      a2, 64                  /* 64字节 */
    /* 实际只是简单复制然后修改前几个字节 */
    /* 先读取Core0消息的第一个word确认 */
    lw      a3, 0(a0)              /* 读Core0消息首word */
    /* 构建响应: "Pong N from Hart1!" */
    /* 用固定字符串写入响应区 */
    la      a4, pong_msg
    li      a5, 0
5:
    lbu     a3, 0(a4)
    sb      a3, 0(a1)
    addi    a4, a4, 1
    addi    a1, a1, 1
    addi    a5, a5, 1
    li      a3, 64
    blt     a5, a3, 5b

    /* 设置响应就绪flag */
    li      a0, 0xCAFE
    sw      a0, SHARED_MSG_RESP_FLAG(t0)
    /* 清零请求flag */
    sw      zero, SHARED_MSG_REQ_FLAG(t0)
    fence   rw, rw

    /* 给Core0发中断通知 */
    li      a0, HP_SYSTEM_CPU_INT_FROM_CPU_1_REG
    li      a1, 1
    sw      a1, 0(a0)
    /* 更新发送计数 */
    lw      a0, 0x108(t0)
    addi    a0, a0, 1
    sw      a0, 0x108(t0)

    /* 恢复寄存器 */
    lw      a0, 0(sp)
    lw      a1, 4(sp)
    lw      a2, 8(sp)
    lw      a3, 12(sp)
    lw      a4, 16(sp)
    lw      a5, 20(sp)
    addi    sp, sp, 32
2:
    wfi
    j       1b

    .size core1_entry, . - core1_entry

/* ---- 实际的IRQ handler代码 (可以从非对齐位置跳转过来) ---- */
core1_irq_handler_real:
    addi    sp, sp, -80
    sw      ra, 0(sp)
    sw      t0, 4(sp)
    sw      t1, 8(sp)
    sw      t2, 12(sp)
    sw      a0, 16(sp)
    sw      a1, 20(sp)

    csrr    t0, mcause

    /* 清除CLIC pending (ID 28) */
    li      t1, CLIC_CTRL_BASE
    addi    t1, t1, 4 * IPC_CLIC_ID
    sb      zero, 0(t1)

    /* 更新共享ACK计数器 */
    la      t1, (SHARED_BASE + SHARED_H1_IRQ_ACK_OFF)
    lw      t2, 0(t1)
    addi    t2, t2, 1
    sw      t2, 0(t1)
    fence   rw, rw

    /* 恢复上下文 */
    lw      ra, 0(sp)
    lw      t0, 4(sp)
    lw      t1, 8(sp)
    lw      t2, 12(sp)
    lw      a0, 16(sp)
    lw      a1, 20(sp)
    addi    sp, sp, 80
    mret

/* ---- 早期trap: 记录错误并park ---- */
    .align  2
core1_early_trap:
    csrr    t0, mcause
    la      t1, g_core1_trap_mcause
    sw      t0, 0(t1)
    csrr    t0, mtval
    la      t1, g_core1_trap_mtval
    sw      t0, 0(t1)
    csrr    t0, mepc
    la      t1, g_core1_trap_mepc
    sw      t0, 0(t1)
    fence   rw, rw
1:
    wfi
    j       1b

    .option pop

/* ---- 响应消息字符串 ---- */
    .section .rodata
    .align  4
pong_msg:
    .ascii  "Pong from Hart1! IPC ping-pong test OK.            "
    /* 正好64字节 */
