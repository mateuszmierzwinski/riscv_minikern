/* The floating-point registers, saved and restored.
 *
 * **Why this is not optional.** `f0`..`f31` and `fcsr` are a thread's state
 * exactly as `x1`..`x31` are. A kernel that switches threads without them
 * lets one thread's floating-point values survive into another's, and a
 * kernel that delivers a signal without them lets the handler clobber
 * whatever the interrupted code was holding.
 *
 * The symptoms are nothing to do with arithmetic. What this project saw,
 * under a 36-run stress loop: a Go program panicking with "sync: WaitGroup
 * misuse" on code that misuses nothing, and "panic while printing panic
 * value" while reporting a different, deliberate panic — because the
 * runtime's own code had its live values replaced underneath it. Roughly one
 * run in twelve, and nothing smaller than a stress loop finds it.
 *
 * The layout is the ABI's `union __riscv_fp_state` for the D extension:
 * thirty-two doubles, then `fcsr` as a 32-bit word. It is what a signal
 * frame's `sc_fpregs` is, so the same routine fills a frame user space will
 * read and a thread's private save area.
 */
    .section .text

    .globl fp_save
/* fp_save(area = a0) */
fp_save:
    fsd     f0, 0(a0)
    fsd     f1, 8(a0)
    fsd     f2, 16(a0)
    fsd     f3, 24(a0)
    fsd     f4, 32(a0)
    fsd     f5, 40(a0)
    fsd     f6, 48(a0)
    fsd     f7, 56(a0)
    fsd     f8, 64(a0)
    fsd     f9, 72(a0)
    fsd     f10, 80(a0)
    fsd     f11, 88(a0)
    fsd     f12, 96(a0)
    fsd     f13, 104(a0)
    fsd     f14, 112(a0)
    fsd     f15, 120(a0)
    fsd     f16, 128(a0)
    fsd     f17, 136(a0)
    fsd     f18, 144(a0)
    fsd     f19, 152(a0)
    fsd     f20, 160(a0)
    fsd     f21, 168(a0)
    fsd     f22, 176(a0)
    fsd     f23, 184(a0)
    fsd     f24, 192(a0)
    fsd     f25, 200(a0)
    fsd     f26, 208(a0)
    fsd     f27, 216(a0)
    fsd     f28, 224(a0)
    fsd     f29, 232(a0)
    fsd     f30, 240(a0)
    fsd     f31, 248(a0)
    frcsr   t0
    sw      t0, 256(a0)
    ret

    .globl fp_restore
/* fp_restore(area = a0) */
fp_restore:
    fld     f0, 0(a0)
    fld     f1, 8(a0)
    fld     f2, 16(a0)
    fld     f3, 24(a0)
    fld     f4, 32(a0)
    fld     f5, 40(a0)
    fld     f6, 48(a0)
    fld     f7, 56(a0)
    fld     f8, 64(a0)
    fld     f9, 72(a0)
    fld     f10, 80(a0)
    fld     f11, 88(a0)
    fld     f12, 96(a0)
    fld     f13, 104(a0)
    fld     f14, 112(a0)
    fld     f15, 120(a0)
    fld     f16, 128(a0)
    fld     f17, 136(a0)
    fld     f18, 144(a0)
    fld     f19, 152(a0)
    fld     f20, 160(a0)
    fld     f21, 168(a0)
    fld     f22, 176(a0)
    fld     f23, 184(a0)
    fld     f24, 192(a0)
    fld     f25, 200(a0)
    fld     f26, 208(a0)
    fld     f27, 216(a0)
    fld     f28, 224(a0)
    fld     f29, 232(a0)
    fld     f30, 240(a0)
    fld     f31, 248(a0)
    lw      t0, 256(a0)
    fscsr   t0
    ret
