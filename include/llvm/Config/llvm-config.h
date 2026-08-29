// Generated-configuration compatibility header for the vendored LLVM 7.1
// public headers used by ipaSim's modern loader build.
//
// This is not a behavioral stub and it does not emulate LLVM. The modern core
// only consumes LLVM's public Mach-O data structure declarations, but those
// headers include llvm/Config/llvm-config.h, which normally exists only after a
// full LLVM CMake configure. Keeping the exact vendored version/Windows-host
// configuration here avoids rebuilding the historical compiler just to obtain
// manifest constants.

#ifndef LLVM_CONFIG_H
#define LLVM_CONFIG_H

#define LLVM_DEFAULT_TARGET_TRIPLE "x86_64-pc-windows-msvc"
#define LLVM_ENABLE_THREADS 1
#define LLVM_HAS_ATOMICS 1
#define LLVM_HOST_TRIPLE "x86_64-pc-windows-msvc"
#define LLVM_NATIVE_ARCH X86
#define LLVM_NATIVE_ASMPARSER LLVMInitializeX86AsmParser
#define LLVM_NATIVE_ASMPRINTER LLVMInitializeX86AsmPrinter
#define LLVM_NATIVE_DISASSEMBLER LLVMInitializeX86Disassembler
#define LLVM_NATIVE_TARGET LLVMInitializeX86Target
#define LLVM_NATIVE_TARGETINFO LLVMInitializeX86TargetInfo
#define LLVM_NATIVE_TARGETMC LLVMInitializeX86TargetMC
#define LLVM_USE_INTEL_JITEVENTS 0
#define LLVM_USE_OPROFILE 0
#define LLVM_USE_PERF 0
#define LLVM_VERSION_MAJOR 7
#define LLVM_VERSION_MINOR 1
#define LLVM_VERSION_PATCH 0
#define LLVM_VERSION_STRING "7.1.0"
#define LLVM_FORCE_ENABLE_STATS 0

#endif // LLVM_CONFIG_H
