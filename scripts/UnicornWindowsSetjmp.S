// Native Windows x64 _setjmp has a hidden frame/unwind argument in RDX.
// Passing zero selects the no-unwind longjmp path required by Unicorn's JIT.
// This is the same mechanism merged upstream in unicorn-engine/unicorn#1331.

.text
.globl _setjmp_wrapper
.p2align 4, 0x90
_setjmp_wrapper:
    xorq %rdx, %rdx
    jmp _setjmp
