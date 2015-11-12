#ifndef CPP_H
#define CPP_H

#include <llvm-c/Core.h>

#ifdef __cplusplus
extern "C" {
#endif

LLVMValueRef EGLBuildMalloc(LLVMBuilderRef B, LLVMTypeRef Ty, LLVMValueRef Before, const char *Name);
void EGLGenerateAssembly(LLVMModuleRef module);

#ifdef __cplusplus
}
#endif

#endif
