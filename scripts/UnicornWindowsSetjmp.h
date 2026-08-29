#pragma once

// Native Windows x64 setjmp/longjmp performs structured stack unwinding by
// default. Unicorn's TCG executes generated code without Windows unwind tables,
// so that unwind path raises STATUS_BAD_FUNCTION_TABLE. Upstream Unicorn fixed
// the same native-MSVC failure by entering _setjmp with its hidden frame
// argument cleared. Keep that compatibility behavior local to the modern x64
// Unicorn build; do not change the vendored submodule.
#if defined(_WIN64) && defined(__clang__) && defined(_MSC_VER)
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif
int _setjmp_wrapper(jmp_buf env);
#ifdef __cplusplus
}
#endif

#ifdef setjmp
#undef setjmp
#endif
#define setjmp(env) _setjmp_wrapper(env)
#endif
