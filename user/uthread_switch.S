        .text

/*
 * thread_switch(old_context, new_context)
 *
 *   a0 = &old_thread->context     (要把当前 callee-save 寄存器存到这里)
 *   a1 = &new_thread->context     (要从这里把寄存器恢复出来)
 *
 * 总体步骤：
 *   1) 把 a0 指向的 struct thread_context 看作"寄存器存储区"，
 *      按 struct thread_context 的字段顺序把当前寄存器 sd 进去；
 *   2) 从 a1 指向的存储区按同样顺序 ld 出来；
 *   3) ret —— ret 指令会把 ra 装到 pc，所以新线程会"返回"到它
 *      上次 thread_switch 离开时的下一条指令（也就是新线程的
 *      "resume point"）。
 *
 * struct thread_context 的字段顺序（参见 user/uthread.c）：
 *   uint64 ra;     // offset 0
 *   uint64 sp;     // offset 8
 *   uint64 s0;     // offset 16
 *   uint64 s1;     // offset 24
 *   uint64 s2;     // offset 32
 *   uint64 s3;     // offset 40
 *   uint64 s4;     // offset 48
 *   uint64 s5;     // offset 56
 *   uint64 s6;     // offset 64
 *   uint64 s7;     // offset 72
 *   uint64 s8;     // offset 80
 *   uint64 s9;     // offset 88
 *   uint64 s10;    // offset 96
 *   uint64 s11;    // offset 104
 *
 * 注意：RISC-V 调用约定里，callee 必须保留 ra 和 s0..s11，
 * 其它 (a*, t*, gp, tp) 是 caller-saved，由调用者 thread_schedule
 * 自己负责。因此这里只保存这 14 个寄存器就足够了。
 */

        .globl thread_switch
thread_switch:
        # === 把旧线程的 callee-save 寄存器存到 *a0 ===
        # struct thread_context 在内存里的偏移量：
        #   ra  = 0,   sp  = 8,   s0  = 16, s1  = 24, s2  = 32, s3  = 40
        #   s4  = 48,  s5  = 56,  s6  = 64, s7  = 72, s8  = 80, s9  = 88
        #   s10 = 96,  s11 = 104
        # sd  = store doubleword (8 字节) 到内存
        sd ra, 0(a0)        # ra 存到 offset 0
        sd sp, 8(a0)        # sp 存到 offset 8
        sd s0, 16(a0)       # s0 存到 offset 16
        sd s1, 24(a0)       # s1
        sd s2, 32(a0)       # s2
        sd s3, 40(a0)       # s3
        sd s4, 48(a0)       # s4
        sd s5, 56(a0)       # s5
        sd s6, 64(a0)       # s6
        sd s7, 72(a0)       # s7
        sd s8, 80(a0)       # s8
        sd s9, 88(a0)       # s9
        sd s10, 96(a0)      # s10
        sd s11, 104(a0)     # s11

        # === 从 *a1 恢复新线程的 callee-save 寄存器 ===
        # ld = load doubleword，从内存读到寄存器
        ld ra, 0(a1)        # ra 从 offset 0 读
        ld sp, 8(a1)        # sp
        ld s0, 16(a1)
        ld s1, 24(a1)
        ld s2, 32(a1)
        ld s3, 40(a1)
        ld s4, 48(a1)
        ld s5, 56(a1)
        ld s6, 64(a1)
        ld s7, 72(a1)
        ld s8, 80(a1)
        ld s9, 88(a1)
        ld s10, 96(a1)
        ld s11, 104(a1)

        # ret 指令：把 ra 装到 pc，即跳到新线程上次离开时的位置
        ret
