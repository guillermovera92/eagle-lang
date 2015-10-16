#include "ast_compiler.h"


void ac_scope_leave_struct_callback(LLVMValueRef pos, EagleTypeType *ty, void *data)
{
    CompilerBundle *cb = data;
    ac_call_destructor(cb, pos, ty);
}

LLVMValueRef ac_gen_struct_destructor_func(char *name, CompilerBundle *cb)
{
    char *buf = malloc(strlen(name) + 9);
    sprintf(buf, "__egl_x_%s", name);

    LLVMValueRef func = LLVMGetNamedFunction(cb->module, buf);
    if(func)
    {
        free(buf);
        return func;
    }

    LLVMTypeRef tys[2];
    tys[0] = LLVMPointerType(LLVMInt8Type(), 0);
    tys[1] = LLVMInt1Type();
    func = LLVMAddFunction(cb->module, buf, LLVMFunctionType(LLVMVoidType(), tys, 2, 0));

    free(buf);
    return func;
}

LLVMValueRef ac_gen_struct_constructor_func(char *name, CompilerBundle *cb, int copy)
{
    char *buf = malloc(strlen(name) + 9);
    copy ? sprintf(buf, "__egl_c_%s", name) : sprintf(buf, "__egl_i_%s", name);

    LLVMValueRef func = LLVMGetNamedFunction(cb->module, buf);
    if(func)
    {
        free(buf);
        return func;
    }

    LLVMTypeRef ty = LLVMPointerType(LLVMInt8Type(), 0);
    func = LLVMAddFunction(cb->module, buf, LLVMFunctionType(LLVMVoidType(), &ty, 1, 0));

    free(buf);
    return func;
}

void ac_make_struct_copy_constructor(AST *ast, CompilerBundle *cb)
{
    ASTStructDecl *a = (ASTStructDecl *)ast;
    LLVMValueRef func = ac_gen_struct_constructor_func(a->name, cb, 1);

    EagleTypeType *ett = ett_pointer_type(ett_struct_type(a->name));

    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(func, "entry");
    LLVMPositionBuilderAtEnd(cb->builder, entry);
    LLVMValueRef in = LLVMGetParam(func, 0);
    LLVMValueRef strct = LLVMBuildBitCast(cb->builder, in, ett_llvm_type(ett), "");


    arraylist *types = &a->types;
    int i;
    for(i = 0; i < types->count; i++)
    {
        EagleTypeType *t = arr_get(types, i);
        if(ET_IS_COUNTED(t))
        {
            LLVMValueRef gep = LLVMBuildStructGEP(cb->builder, strct, i, "");
            ac_incr_pointer(cb, &gep, t);
        }
        else if(ET_IS_WEAK(t))
        {
            LLVMValueRef gep = LLVMBuildStructGEP(cb->builder, strct, i, "");
            ac_add_weak_pointer(cb, LLVMBuildLoad(cb->builder, gep, ""), gep, t);
        }
        else if(t->type == ETStruct && ty_needs_destructor(t))
        {
            LLVMValueRef fc = ac_gen_struct_constructor_func(((EagleStructType *)t)->name, cb, 1);
            LLVMValueRef param = LLVMBuildStructGEP(cb->builder, strct, i, "");
            param = LLVMBuildBitCast(cb->builder, param, LLVMPointerType(LLVMInt8Type(), 0), "");
            LLVMBuildCall(cb->builder, fc, &param, 1, "");
        }
    }

    LLVMBuildRetVoid(cb->builder);
}

void ac_make_struct_constructor(AST *ast, CompilerBundle *cb)
{
    ASTStructDecl *a = (ASTStructDecl *)ast;
    LLVMValueRef func = ac_gen_struct_constructor_func(a->name, cb, 0);

    EagleTypeType *ett = ett_pointer_type(ett_struct_type(a->name));

    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(func, "entry");
    LLVMPositionBuilderAtEnd(cb->builder, entry);
    LLVMValueRef in = LLVMGetParam(func, 0);
    LLVMValueRef strct = LLVMBuildBitCast(cb->builder, in, ett_llvm_type(ett), "");


    arraylist *types = &a->types;
    int i;
    for(i = 0; i < types->count; i++)
    {
        EagleTypeType *t = arr_get(types, i);
        if(ET_IS_COUNTED(t) || ET_IS_WEAK(t))
        {
            LLVMValueRef gep = LLVMBuildStructGEP(cb->builder, strct, i, "");
            LLVMBuildStore(cb->builder, LLVMConstPointerNull(ett_llvm_type(t)), gep);
        }
        else if(t->type == ETStruct && ty_needs_destructor(t))
        {
            LLVMValueRef fc = ac_gen_struct_constructor_func(((EagleStructType *)t)->name, cb, 0);
            LLVMValueRef param = LLVMBuildStructGEP(cb->builder, strct, i, "");
            param = LLVMBuildBitCast(cb->builder, param, LLVMPointerType(LLVMInt8Type(), 0), "");
            LLVMBuildCall(cb->builder, fc, &param, 1, "");
        }
    }

    LLVMBuildRetVoid(cb->builder);
}

void ac_make_struct_destructor(AST *ast, CompilerBundle *cb)
{
    ASTStructDecl *a = (ASTStructDecl *)ast;
    LLVMValueRef func = ac_gen_struct_destructor_func(a->name, cb);

    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(func, "entry");
    LLVMPositionBuilderAtEnd(cb->builder, entry);

    EaglePointerType *ett = (EaglePointerType *)ett_pointer_type(ett_struct_type(a->name));
    LLVMValueRef pos = LLVMBuildAlloca(cb->builder, ett_llvm_type((EagleTypeType *)ett), "");
    LLVMValueRef cmp = LLVMBuildICmp(cb->builder, LLVMIntEQ, LLVMGetParam(func, 1), LLVMConstInt(LLVMInt1Type(), 1, 0), "");
    LLVMBasicBlockRef ifBB = LLVMAppendBasicBlock(func, "if");
    LLVMBasicBlockRef elseBB = LLVMAppendBasicBlock(func, "else");
    LLVMBasicBlockRef mergeBB = LLVMAppendBasicBlock(func, "merge");
    LLVMBuildCondBr(cb->builder, cmp, ifBB, elseBB);
    LLVMPositionBuilderAtEnd(cb->builder, ifBB);

    ett->counted = 1;
    LLVMValueRef cast = LLVMBuildBitCast(cb->builder, LLVMGetParam(func, 0), ett_llvm_type((EagleTypeType *)ett), "");
    LLVMBuildStore(cb->builder, LLVMBuildStructGEP(cb->builder, cast, 5, ""), pos);
    LLVMBuildBr(cb->builder, mergeBB);

    ett->counted = 0;
    LLVMPositionBuilderAtEnd(cb->builder, elseBB);
    cast = LLVMBuildBitCast(cb->builder, LLVMGetParam(func, 0), ett_llvm_type((EagleTypeType *)ett), "");
    LLVMBuildStore(cb->builder, cast, pos);
    LLVMBuildBr(cb->builder, mergeBB);

    LLVMPositionBuilderAtEnd(cb->builder, mergeBB);
    pos = LLVMBuildLoad(cb->builder, pos, "");

    arraylist *types = &a->types;
    int i;
    for(i = 0; i < types->count; i++)
    {
        EagleTypeType *t = arr_get(types, i);
        if(ET_IS_COUNTED(t))
        {
            LLVMValueRef gep = LLVMBuildStructGEP(cb->builder, pos, i, "");
            ac_decr_pointer(cb, &gep, t);
        }
        else if(ET_IS_WEAK(t))
        {
            LLVMValueRef gep = LLVMBuildStructGEP(cb->builder, pos, i, "");
            ac_remove_weak_pointer(cb, gep, t);
        }
        else if(t->type == ETStruct && ty_needs_destructor(t))
        {
            LLVMValueRef fc = ac_gen_struct_destructor_func(((EagleStructType *)t)->name, cb);
            LLVMValueRef params[2];
            params[0] = LLVMBuildBitCast(cb->builder, LLVMBuildStructGEP(cb->builder, pos, i, ""), 
                    LLVMPointerType(LLVMInt8Type(), 0), "");
            params[1] = LLVMConstInt(LLVMInt1Type(), 0, 0);
            LLVMBuildCall(cb->builder, fc, params, 2, "");
        }
    }

    LLVMBuildRetVoid(cb->builder);
}

void ac_add_struct_declaration(AST *ast, CompilerBundle *cb)
{
    ASTStructDecl *a = (ASTStructDecl *)ast;
    LLVMStructCreateNamed(LLVMGetGlobalContext(), a->name);
}

void ac_make_struct_definitions(AST *ast, CompilerBundle *cb)
{
    AST *old = ast;
    for(; ast; ast = ast->next)
    {
        if(ast->type != ASTRUCTDECL)
            continue;

        ASTStructDecl *a = (ASTStructDecl *)ast;

        LLVMTypeRef *tys = malloc(sizeof(LLVMTypeRef) * a->types.count);
        int i;
        for(i = 0; i < a->types.count; i++)
            tys[i] = ett_llvm_type(arr_get(&a->types, i));

        LLVMTypeRef loaded = LLVMGetTypeByName(cb->module, a->name);
        LLVMStructSetBody(loaded, tys, a->types.count, 0);
        free(tys);

        ty_add_struct_def(a->name, &a->names, &a->types);
    }

    ast = old;
    for(; ast; ast = ast->next)
    {
        if(ast->type != ASTRUCTDECL)
            continue;

        ASTStructDecl *a = (ASTStructDecl *)ast;
        if(ty_needs_destructor(ett_struct_type(a->name)))
        {
            ac_make_struct_destructor(ast, cb);
            ac_make_struct_constructor(ast, cb);
            ac_make_struct_copy_constructor(ast, cb);
        }
    }
}

void ac_call_copy_constructor(CompilerBundle *cb, LLVMValueRef pos, EagleTypeType *ty)
{
    if(ET_IS_WEAK(ty) || ET_IS_COUNTED(ty))
        pos = LLVMBuildStructGEP(cb->builder, pos, 5, "");

    EagleStructType *st = ty->type == ETPointer ? (EagleStructType *)((EaglePointerType *)ty)->to
                                                : (EagleStructType *)ty;

    pos = LLVMBuildBitCast(cb->builder, pos, LLVMPointerType(LLVMInt8Type(), 0), "");

    LLVMValueRef func = ac_gen_struct_constructor_func(st->name, cb, 1);
    LLVMBuildCall(cb->builder, func, &pos, 1, "");
}

void ac_call_constructor(CompilerBundle *cb, LLVMValueRef pos, EagleTypeType *ty)
{
    if(ET_IS_WEAK(ty) || ET_IS_COUNTED(ty))
        pos = LLVMBuildStructGEP(cb->builder, pos, 5, "");

    EagleStructType *st = ty->type == ETPointer ? (EagleStructType *)((EaglePointerType *)ty)->to
                                                : (EagleStructType *)ty;

    pos = LLVMBuildBitCast(cb->builder, pos, LLVMPointerType(LLVMInt8Type(), 0), "");

    LLVMValueRef func = ac_gen_struct_constructor_func(st->name, cb, 0);
    LLVMBuildCall(cb->builder, func, &pos, 1, "");
}

void ac_call_destructor(CompilerBundle *cb, LLVMValueRef pos, EagleTypeType *ty)
{
    if(ET_IS_WEAK(ty) || ET_IS_COUNTED(ty))
        die(-1, "WEIRD ERROR");

    EagleStructType *st = ty->type == ETPointer ? (EagleStructType *)((EaglePointerType *)ty)->to
                                                : (EagleStructType *)ty;
    pos = LLVMBuildBitCast(cb->builder, pos, LLVMPointerType(LLVMInt8Type(), 0), "");

    LLVMValueRef func = ac_gen_struct_destructor_func(st->name, cb);
    LLVMValueRef params[2];
    params[0] = pos;
    params[1] = LLVMConstInt(LLVMInt1Type(), 0, 0);
    LLVMBuildCall(cb->builder, func, params, 2, "");
}