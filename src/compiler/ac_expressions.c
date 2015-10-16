#include "ast_compiler.h"

LLVMValueRef ac_compile_value(AST *ast, CompilerBundle *cb)
{
    ASTValue *a = (ASTValue *)ast;
    switch(a->etype)
    {
        case ETInt1:
            a->resultantType = ett_base_type(ETInt1);
            return LLVMConstInt(LLVMInt1Type(), a->value.i, 1);
        case ETInt32:
            a->resultantType = ett_base_type(ETInt32);
            return LLVMConstInt(LLVMInt32Type(), a->value.i, 1);
        case ETDouble:
            a->resultantType = ett_base_type(ETDouble);
            return LLVMConstReal(LLVMDoubleType(), a->value.d);
        case ETCString:
            a->resultantType = ett_pointer_type(ett_base_type(ETInt8));
            return LLVMBuildGlobalStringPtr(cb->builder, a->value.id, "str");
        case ETNil:
            a->resultantType = ett_pointer_type(ett_base_type(ETAny));
            return LLVMConstPointerNull(ett_llvm_type(a->resultantType));
        default:
            die(ALN, "Unknown value type.");
            return NULL;
    }
}

LLVMValueRef ac_compile_identifier(AST *ast, CompilerBundle *cb)
{
    ASTValue *a = (ASTValue *)ast;
    VarBundle *b = vs_get(cb->varScope, a->value.id);

    if(!b) // We are dealing with a local variable
        die(ALN, "Undeclared Identifier (%s)", a->value.id);
    if(b->type->type == ETAuto)
        die(ALN, "Trying to read variable of unknown type (%s)", a->value.id);

    if(b->type->type == ETFunction)
    {
        a->resultantType = b->type;
        return b->value;
    }

    if(ET_IS_CLOSED(b->type))
    {
        a->resultantType = ((EaglePointerType *)b->type)->to;
        LLVMValueRef pos = LLVMBuildStructGEP(cb->builder, LLVMBuildLoad(cb->builder, b->value, ""), 5, "");

        if(a->resultantType->type == ETArray || a->resultantType->type == ETStruct || a->resultantType->type == ETClass)
            return pos;

        return LLVMBuildLoad(cb->builder, pos, "loadtmp");
    }

    a->resultantType = b->type;

    if(b->type->type == ETArray && !ET_IS_GEN_ARR(b->type))
        return b->value;

    if(b->type->type == ETStruct || b->type->type == ETClass)
        return b->value;
    /*
    if((b->type->type == ETPointer && ((EaglePointerType *)b->type)->to->type == ETStruct) && (!ET_IS_COUNTED(b->type) && !ET_IS_WEAK(b->type)))
        return b->value;
        */

    return LLVMBuildLoad(cb->builder, b->value, "loadtmp");
}

LLVMValueRef ac_compile_var_decl_ext(EagleTypeType *type, char *ident, CompilerBundle *cb)
{

    LLVMBasicBlockRef curblock = LLVMGetInsertBlock(cb->builder);
    LLVMPositionBuilderAtEnd(cb->builder, cb->currentFunctionEntry);

    LLVMValueRef begin = LLVMGetFirstInstruction(cb->currentFunctionEntry);
    if(begin)
        LLVMPositionBuilderBefore(cb->builder, begin);
    LLVMValueRef pos = LLVMBuildAlloca(cb->builder, ett_llvm_type(type), ident);

    LLVMPositionBuilderAtEnd(cb->builder, curblock);

    VarBundle *b = vs_get(cb->varScope, ident);
    if(b && !b->value)
        b->value = pos;

    if(ET_IS_COUNTED(type))
    {
        LLVMBuildStore(cb->builder, LLVMConstPointerNull(ett_llvm_type(type)), pos);

        vs_add_callback(cb->varScope, ident, ac_scope_leave_callback, cb);
    }
    else if(ET_IS_WEAK(type))
    {
        LLVMBuildStore(cb->builder, LLVMConstPointerNull(ett_llvm_type(type)), pos);
        vs_add_callback(cb->varScope, ident, ac_scope_leave_weak_callback, cb);
    }
    else if(type->type == ETStruct && ty_needs_destructor(type))
    {
        ac_call_constructor(cb, pos, type);
        vs_add_callback(cb->varScope, ident, ac_scope_leave_struct_callback, cb);
    }

    if(type->type == ETArray && ett_array_has_counted(type))
    {
        ac_nil_fill_array(cb, pos, ett_array_count(type));
        vs_add_callback(cb->varScope, ident, ac_scope_leave_array_callback, cb);
    }

    return pos;
}

LLVMValueRef ac_compile_var_decl(AST *ast, CompilerBundle *cb)
{
    ASTVarDecl *a = (ASTVarDecl *)ast;
    ASTTypeDecl *type = (ASTTypeDecl *)a->atype;
    ast->resultantType = type->etype;

    if(type->etype->type == ETAuto)
    {
        vs_put(cb->varScope, a->ident, NULL, type->etype);
        return NULL;
    }

    vs_put(cb->varScope, a->ident, NULL, type->etype);
    LLVMValueRef pos = ac_compile_var_decl_ext(type->etype, a->ident, cb);

    return pos;
}

LLVMValueRef ac_compile_struct_member(AST *ast, CompilerBundle *cb, int keepPointer)
{
    ASTStructMemberGet *a = (ASTStructMemberGet *)ast;
    LLVMValueRef left = ac_dispatch_expression(a->left, cb);


    EagleTypeType *ty = a->left->resultantType;

    if(ty->type != ETStruct && ty->type != ETClass)
        die(ALN, "Attempting to access member of non-struct type (%s).", a->ident);
    if(ty->type == ETPointer && ((EaglePointerType *)ty)->to->type != ETStruct && ((EaglePointerType *)ty)->to->type != ETClass)
        die(ALN, "Attempting to access member of non-struct pointer type (%s).", a->ident);

    // Only save the value of the instance if we have a class and a method.
    if((ty->type == ETClass || ty->type == ETStruct) && ty_method_lookup(((EagleStructType *)ty)->name, a->ident))
        a->leftCompiled = a->left->type == AUNARY ? ((ASTUnary *)a->left)->savedWrapped : left;
        //a->leftCompiled = a->left->type == AUNARY ? ((ASTUnary *)a->left)->savedWrapped : left;

    int index;
    EagleTypeType *type;
    ty_struct_member_index(ty, a->ident, &index, &type);

    if(index < -1)
        die(ALN, "Internal compiler error. Struct not loaded but found.");
    if(index < 0)
        die(ALN, "Struct \"%s\" has no member \"%s\".", ((EagleStructType *)ty)->name, a->ident);

    ast->resultantType = type;

    LLVMValueRef gep = LLVMBuildStructGEP(cb->builder, left, index, a->ident);
    if(keepPointer || type->type == ETStruct || type->type == ETClass)
        return gep;
    return LLVMBuildLoad(cb->builder, gep, "");
}

LLVMValueRef ac_compile_malloc_counted_raw(LLVMTypeRef rt, LLVMTypeRef *out, CompilerBundle *cb)
{
    LLVMTypeRef ptmp[2];
    ptmp[0] = LLVMPointerType(LLVMInt8Type(), 0);
    ptmp[1] = LLVMInt1Type();
    
    LLVMTypeRef tys[6];
    tys[0] = LLVMInt64Type();
    tys[1] = LLVMInt16Type();
    tys[2] = LLVMInt16Type();
    tys[3] = LLVMPointerType(LLVMInt8Type(), 0);
    tys[4] = LLVMPointerType(LLVMFunctionType(LLVMVoidType(), ptmp, 2, 0), 0);
    tys[5] = rt;
    LLVMTypeRef tt = LLVMStructType(tys, 6, 0);
    tt = ty_get_counted(tt);
    
    LLVMValueRef mal = LLVMBuildMalloc(cb->builder, tt, "new");

    *out = tt;

    ac_prepare_pointer(cb, mal, NULL);

    return mal;
}

LLVMValueRef ac_compile_malloc_counted(EagleTypeType *type, EagleTypeType **res, LLVMValueRef ib, CompilerBundle *cb)
{
    LLVMTypeRef ptmp[2];
    ptmp[0] = LLVMPointerType(LLVMInt8Type(), 0);
    ptmp[1] = LLVMInt1Type();
    
    LLVMTypeRef tys[6];
    tys[0] = LLVMInt64Type();
    tys[1] = LLVMInt16Type();
    tys[2] = LLVMInt16Type();
    tys[3] = LLVMPointerType(LLVMInt8Type(), 0);
    tys[4] = LLVMPointerType(LLVMFunctionType(LLVMVoidType(), ptmp, 2, 0), 0);
    tys[5] = ett_llvm_type(type);
    LLVMTypeRef tt = LLVMStructType(tys, 6, 0);

    //LLVMDumpType(ett_llvm_type(type));
    tt = ty_get_counted(tt);

    //LLVMDumpType(tt);
    LLVMValueRef mal;
    if(ib)
        mal = EGLBuildMalloc(cb->builder, tt, ib, "new");
    else
        mal = LLVMBuildMalloc(cb->builder, tt, "new");
   
    EaglePointerType *pt = (EaglePointerType *)ett_pointer_type(type);
    pt->counted = 1;
    EagleTypeType *resultantType = (EagleTypeType *)pt;
    if(res)
        *res = resultantType;

    ac_prepare_pointer(cb, mal, resultantType);
    if((type->type == ETStruct && ty_needs_destructor(type)) || type->type == ETClass)
    {
        LLVMValueRef pos = LLVMBuildStructGEP(cb->builder, mal, 5, "");
        ac_call_constructor(cb, pos, type);
        pos = LLVMBuildStructGEP(cb->builder, mal, 4, "");
        EagleStructType *st = (EagleStructType *)type;
        LLVMBuildStore(cb->builder, ac_gen_struct_destructor_func(st->name, cb), pos);
    }
    
    // We need to specially handle counted counted types
    if(ET_IS_COUNTED(type))
    {
        LLVMValueRef pos = LLVMBuildStructGEP(cb->builder, mal, 4, "");
        LLVMBuildStore(cb->builder, LLVMGetNamedFunction(cb->module, "__egl_counted_destructor"), pos);

        pos = LLVMBuildStructGEP(cb->builder, mal, 5, "");
        LLVMBuildStore(cb->builder, LLVMConstPointerNull(ett_llvm_type(type)), pos);
    }
    /*
    LLVMValueRef pos = LLVMBuildStructGEP(cb->builder, mal, 0, "ctp");
    LLVMBuildStore(cb->builder, LLVMConstInt(LLVMInt64Type(), 0, 0), pos);
    */


    return mal;
}

LLVMValueRef ac_compile_new_decl(AST *ast, CompilerBundle *cb)
{
    ASTBinary *a = (ASTBinary *)ast;
    ASTTypeDecl *type = (ASTTypeDecl *)a->left;
    
    LLVMValueRef val = ac_compile_malloc_counted(type->etype, &ast->resultantType, NULL, cb);
    hst_put(&cb->transients, ast, val, ahhd, ahed);

    if(a->right)
    {
        LLVMValueRef init = ac_dispatch_expression(a->right, cb);
        if(!ett_are_same(a->right->resultantType, type->etype))
            init = ac_build_conversion(cb->builder, init, a->right->resultantType, type->etype);
        LLVMValueRef pos = LLVMBuildStructGEP(cb->builder, val, 5, "");
        LLVMBuildStore(cb->builder, init, pos);
    }

    return val;
}

LLVMValueRef ac_compile_cast(AST *ast, CompilerBundle *cb)
{
    ASTCast *a = (ASTCast *)ast;
    ASTTypeDecl *ty = (ASTTypeDecl *)a->etype;

    LLVMValueRef val = ac_dispatch_expression(a->val, cb);

    EagleTypeType *to = ty->etype;
    EagleTypeType *from = a->val->resultantType;

    ast->resultantType = to;

    if(ett_is_numeric(to) && ett_is_numeric(from))
    {
        return ac_build_conversion(cb->builder, val, from, to);
    }

    if(to->type == ETPointer && from->type == ETPointer)
    {
        return LLVMBuildBitCast(cb->builder, val, ett_llvm_type(to), "casttmp");
    }

    if(to->type == ETPointer)
    {
        if(!ET_IS_INT(from->type))
            die(ALN, "Cannot cast non-integer type to pointer.");
        return LLVMBuildIntToPtr(cb->builder, val, ett_llvm_type(to), "casttmp");
    }

    if(from->type == ETPointer)
    {
        if(!ET_IS_INT(to->type))
            die(ALN, "Pointers may only be cast to other pointers or integers.");
        return LLVMBuildPtrToInt(cb->builder, val, ett_llvm_type(to), "casttmp");
    }

    die(ALN, "Unknown type conversion requested.");

    return NULL;
}

LLVMValueRef ac_compile_index(AST *ast, int keepPointer, CompilerBundle *cb)
{
    ASTBinary *a = (ASTBinary *)ast;
    AST *left = a->left;
    AST *right = a->right;

    LLVMValueRef l = ac_dispatch_expression(left, cb);
    LLVMValueRef r = ac_dispatch_expression(right, cb);

    EagleTypeType *lt = left->resultantType;
    EagleTypeType *rt = right->resultantType;

    if(lt->type != ETPointer && lt->type != ETArray)
        die(LN(left), "Only pointer types may be indexed.");
    if(lt->type == ETPointer && ett_pointer_depth(lt) == 1 && ett_get_base_type(lt) == ETAny)
        die(LN(left), "Trying to dereference any-pointer.");
    if(!ett_is_numeric(rt))
        die(LN(right), "Arrays can only be indexed by a number.");

    if(ET_IS_REAL(rt->type))
        r = ac_build_conversion(cb->builder, r, rt, ett_base_type(ETInt64));

    if(lt->type == ETPointer)
        ast->resultantType = ((EaglePointerType *)lt)->to;
    else
        ast->resultantType = ((EagleArrayType *)lt)->of;

    LLVMValueRef gep;
    if(lt->type == ETArray && ((EagleArrayType *)lt)->ct >= 0)
    {
        LLVMValueRef zero = LLVMConstInt(LLVMInt32Type(), 0, 0);
        LLVMValueRef pts[] = {zero, r};
        gep = LLVMBuildInBoundsGEP(cb->builder, l, pts, 2, "idx");
    }
    else
    {
        gep = LLVMBuildInBoundsGEP(cb->builder, l, &r, 1, "idx");
    }

    if(keepPointer || (lt->type == ETArray && ((EagleArrayType *)lt)->of->type == ETArray))
        return gep;

    return LLVMBuildLoad(cb->builder, gep, "dereftmp");
}

// Short circuited logical or
LLVMValueRef ac_compile_logical_or(AST *ast, CompilerBundle *cb)
{
    ASTBinary *a = (ASTBinary *)ast;

    arraylist trees = arr_create(10);
    arraylist values = arr_create(10);
    arraylist blocks = arr_create(10);

    for(; a->type == ABINARY && ((ASTBinary *)a)->op == '|'; a = (ASTBinary *)a->left)
        arr_append(&trees, a->right);

    arr_append(&trees, a);

    LLVMBasicBlockRef mergeBB = LLVMAppendBasicBlock(cb->currentFunction, "merge");

    int i;
    for(i = trees.count - 1; i > 0; i--)
    {
        LLVMValueRef val = ac_dispatch_expression(trees.items[i], cb);
        LLVMValueRef cmp = ac_compile_test(trees.items[i], val, cb);

        LLVMBasicBlockRef nextBB = LLVMAppendBasicBlock(cb->currentFunction, "next");

        arr_append(&values, LLVMConstInt(LLVMInt1Type(), 1, 0));
        arr_append(&blocks, LLVMGetInsertBlock(cb->builder));

        hst_for_each(&cb->transients, ac_decr_transients, cb);
        hst_for_each(&cb->loadedTransients, ac_decr_loaded_transients, cb);
        
        hst_free(&cb->transients);
        hst_free(&cb->loadedTransients);

        cb->transients = hst_create();
        cb->loadedTransients = hst_create();

        LLVMBuildCondBr(cb->builder, cmp, mergeBB, nextBB);

        LLVMPositionBuilderAtEnd(cb->builder, nextBB);
    }

    LLVMValueRef val = ac_dispatch_expression(trees.items[0], cb);
    LLVMValueRef cmp = ac_compile_test(trees.items[0], val, cb);

    hst_for_each(&cb->transients, ac_decr_transients, cb);
    hst_for_each(&cb->loadedTransients, ac_decr_loaded_transients, cb);
    
    hst_free(&cb->transients);
    hst_free(&cb->loadedTransients);

    cb->transients = hst_create();
    cb->loadedTransients = hst_create();

    LLVMBuildBr(cb->builder, mergeBB);

    arr_append(&values, cmp);
    arr_append(&blocks, LLVMGetInsertBlock(cb->builder));

    LLVMMoveBasicBlockAfter(mergeBB, LLVMGetInsertBlock(cb->builder));
    LLVMPositionBuilderAtEnd(cb->builder, mergeBB);

    LLVMValueRef phi = LLVMBuildPhi(cb->builder, LLVMInt1Type(), "phi");
    LLVMAddIncoming(phi, (LLVMValueRef *)values.items, (LLVMBasicBlockRef *)blocks.items, values.count);

    ast->resultantType = ett_base_type(ETInt1);

    arr_free(&trees);
    arr_free(&blocks);
    arr_free(&values);

    return phi;
}

// Short circuited logical and
LLVMValueRef ac_compile_logical_and(AST *ast, CompilerBundle *cb)
{
    ASTBinary *a = (ASTBinary *)ast;

    arraylist trees = arr_create(10);
    arraylist values = arr_create(10);
    arraylist blocks = arr_create(10);

    for(; a->type == ABINARY && ((ASTBinary *)a)->op == '&'; a = (ASTBinary *)a->left)
        arr_append(&trees, a->right);

    arr_append(&trees, a);

    LLVMBasicBlockRef mergeBB = LLVMAppendBasicBlock(cb->currentFunction, "merge");

    int i;
    for(i = trees.count - 1; i > 0; i--)
    {
        LLVMValueRef val = ac_dispatch_expression(trees.items[i], cb);
        LLVMValueRef cmp = ac_compile_test(trees.items[i], val, cb);

        // Any allocations made in the phi block must be destroyed there.
        // ==============================================================
        hst_for_each(&cb->transients, ac_decr_transients, cb);
        hst_for_each(&cb->loadedTransients, ac_decr_loaded_transients, cb);
        
        hst_free(&cb->transients);
        hst_free(&cb->loadedTransients);

        cb->transients = hst_create();
        cb->loadedTransients = hst_create();
        // ==============================================================

        LLVMBasicBlockRef nextBB = LLVMAppendBasicBlock(cb->currentFunction, "next");
        LLVMBuildCondBr(cb->builder, cmp, nextBB, mergeBB);

        arr_append(&values, LLVMConstInt(LLVMInt1Type(), 0, 0));
        arr_append(&blocks, LLVMGetInsertBlock(cb->builder));

        LLVMPositionBuilderAtEnd(cb->builder, nextBB);
    }

    LLVMValueRef val = ac_dispatch_expression(trees.items[0], cb);
    LLVMValueRef cmp = ac_compile_test(trees.items[0], val, cb);

    // ==============================================================
    hst_for_each(&cb->transients, ac_decr_transients, cb);
    hst_for_each(&cb->loadedTransients, ac_decr_loaded_transients, cb);
    
    hst_free(&cb->transients);
    hst_free(&cb->loadedTransients);

    cb->transients = hst_create();
    cb->loadedTransients = hst_create();
    // ==============================================================
        
    LLVMBuildBr(cb->builder, mergeBB);

    arr_append(&values, cmp);
    arr_append(&blocks, LLVMGetInsertBlock(cb->builder));

    LLVMMoveBasicBlockAfter(mergeBB, LLVMGetInsertBlock(cb->builder));
    LLVMPositionBuilderAtEnd(cb->builder, mergeBB);

    LLVMValueRef phi = LLVMBuildPhi(cb->builder, LLVMInt1Type(), "phi");
    LLVMAddIncoming(phi, (LLVMValueRef *)values.items, (LLVMBasicBlockRef *)blocks.items, values.count);

    ast->resultantType = ett_base_type(ETInt1);

    arr_free(&trees);
    arr_free(&blocks);
    arr_free(&values);

    return phi;
}

LLVMValueRef ac_generic_binary(ASTBinary *a, LLVMValueRef l, LLVMValueRef r, char save_left, EagleTypeType *fromtype, EagleTypeType *totype, CompilerBundle *cb)
{
    if(
    (!save_left && (fromtype->type == ETPointer || totype->type == ETPointer)) ||
    (save_left && totype->type == ETPointer)
    )
    {
        if(a->op == 'e')
        {
            l = LLVMBuildPtrToInt(cb->builder, l, LLVMInt64Type(), "");
            r = LLVMBuildPtrToInt(cb->builder, r, LLVMInt64Type(), "");

            a->resultantType = ett_base_type(ETInt1);
            return LLVMBuildICmp(cb->builder, LLVMIntEQ, l, r, "");
        }

        EagleTypeType *lt = totype;
        EagleTypeType *rt = fromtype;
        if(a->op != '+' && a->op != '-' && a->op != 'e' && a->op !='P')
            die(a->lineno, "Operation '%c' not valid for pointer types.", a->op);
        if(lt->type == ETPointer && !ET_IS_INT(rt->type))
            die(a->lineno, "Pointer arithmetic is only valid with integer and non-any pointer types.");
        if(rt->type == ETPointer && !ET_IS_INT(lt->type))
            die(a->lineno, "Pointer arithmetic is only valid with integer and non-any pointer types.");
        
        if(lt->type == ETPointer && ett_get_base_type(lt) == ETAny && ett_pointer_depth(lt) == 1)
            die(a->lineno, "Pointer arithmetic results in dereferencing any pointer.");
        if(rt->type == ETPointer && ett_get_base_type(rt) == ETAny && ett_pointer_depth(rt) == 1)
            die(a->lineno, "Pointer arithmetic results in dereferencing any pointer.");

        LLVMValueRef indexer = lt->type == ETPointer ? r : l;
        LLVMValueRef ptr = lt->type == ETPointer ? l : r;

        if(a->op == '-')
            indexer = LLVMBuildNeg(cb->builder, indexer, "neg");

        EaglePointerType *pt = lt->type == ETPointer ?
            (EaglePointerType *)lt : (EaglePointerType *)rt;

        a->resultantType = (EagleTypeType *)pt;

        LLVMValueRef gep = LLVMBuildInBoundsGEP(cb->builder, ptr, &indexer, 1, "arith");
        return LLVMBuildBitCast(cb->builder, gep, ett_llvm_type((EagleTypeType *)pt), "cast");
    }

    switch(a->op)
    {
        case '+':
        case 'P':
            return ac_make_add(l, r, cb->builder, totype->type);
        case '-':
        case 'M':
            return ac_make_sub(l, r, cb->builder, totype->type);
        case '*':
        case 'T':
            return ac_make_mul(l, r, cb->builder, totype->type);
        case '/':
        case 'D':
            return ac_make_div(l, r, cb->builder, totype->type);
        default:
            die(a->lineno, "Invalid binary operation (%c).", a->op);
            return NULL;
    }
}

LLVMValueRef ac_compile_binary(AST *ast, CompilerBundle *cb)
{
    ASTBinary *a = (ASTBinary *)ast;
    if(a->op == '=')
        return ac_build_store(ast, cb, 0);
    else if(a->op == '[')
        return ac_compile_index(ast, 0, cb);
    else if(a->op == '&')
        return ac_compile_logical_and(ast, cb);
    else if(a->op == '|')
        return ac_compile_logical_or(ast, cb);
    else if(a->op == 'P' || a->op == 'M' || a->op == 'T' || a->op == 'D')
        return ac_build_store(ast, cb, 1);

    LLVMValueRef l = ac_dispatch_expression(a->left, cb);
    LLVMValueRef r = ac_dispatch_expression(a->right, cb);

    if(a->left->resultantType->type == ETPointer || a->right->resultantType->type == ETPointer)
        return ac_generic_binary(a, l, r, 0, a->right->resultantType, a->left->resultantType, cb);


    EagleType promo = et_promotion(a->left->resultantType->type, a->right->resultantType->type);
    a->resultantType = ett_base_type(promo);

    if(a->left->resultantType->type != promo)
    {
        l = ac_build_conversion(cb->builder, l, a->left->resultantType, ett_base_type(promo));
    }
    else if(a->right->resultantType->type != promo)
    {
        r = ac_build_conversion(cb->builder, r, a->right->resultantType, ett_base_type(promo));
    }

    switch(a->op)
    {
        case 'e':
        case 'n':
        case 'g':
        case 'l':
        case 'G':
        case 'L':
            a->resultantType = ett_base_type(ETInt1);
            return ac_make_comp(l, r, cb->builder, promo, a->op);
        default:
            return ac_generic_binary(a, l, r, 0, a->resultantType, a->resultantType, cb);
    }
}

LLVMValueRef ac_compile_get_address(AST *of, CompilerBundle *cb)
{
    switch(of->type)
    {
        case AIDENT:
        {
            ASTValue *o = (ASTValue *)of;
            VarBundle *b = vs_get(cb->varScope, o->value.id);

            if(!b)
                die(LN(of), "Undeclared identifier (%s)", o->value.id);
            
            of->resultantType = b->type;

            return b->value;
        }
        case ABINARY:
        {
            ASTBinary *o = (ASTBinary *)of;
            if(o->op != '[')
                die(LN(of), "Address may not be taken of this operator.");

            LLVMValueRef val = ac_compile_index(of, 1, cb);
            return val;
        }
        case ASTRUCTMEMBER:
        {
            return ac_compile_struct_member(of, cb, 1);
        }
        default:
            die(LN(of), "Address may not be taken of this expression.");
    }

    return NULL;
}

LLVMValueRef ac_compile_unary(AST *ast, CompilerBundle *cb)
{
    ASTUnary *a = (ASTUnary *)ast;

    if(a->op == '&')
    {
        LLVMValueRef out = ac_compile_get_address(a->val, cb);
        a->resultantType = ett_pointer_type(a->val->resultantType);
        return out;
    }

    if(a->op == 't')
    {
        //ASTValue *v = (ASTValue *)a->val;
        //ac_replace_with_counted(cb, v->value.id);
        return NULL;
    }

    LLVMValueRef v;
    if(a->op != 's' && a->val)
        v = ac_dispatch_expression(a->val, cb);

    switch(a->op)
    {
        case 'p':
            {
                LLVMValueRef fmt = NULL;
                switch(a->val->resultantType->type)
                {
                    case ETDouble:
                        fmt = LLVMBuildGlobalStringPtr(cb->builder, "%lf\n", "prfLF");
                        break;
                    case ETInt1:
                        fmt = LLVMBuildGlobalStringPtr(cb->builder, "(Bool) %d\n", "prfB");
                        break;
                    case ETInt8:
                    case ETInt32:
                        fmt = LLVMBuildGlobalStringPtr(cb->builder, "%d\n", "prfI");
                        break;
                    case ETInt64:
                        fmt = LLVMBuildGlobalStringPtr(cb->builder, "%ld\n", "prfLI");
                        break;
                    case ETArray:
                    case ETPointer:
                        fmt = LLVMBuildGlobalStringPtr(cb->builder, 
                                (((EaglePointerType *)a->val->resultantType)->to->type == ETInt8 ? "%s\n" : "%p\n"), "prfPTR");
                        break;
                    default:
                        die(ALN, "The requested type may not be printed.");
                        break;
                }

                LLVMValueRef func = LLVMGetNamedFunction(cb->module, "printf");
                LLVMValueRef args[] = {fmt, v};
                LLVMBuildCall(cb->builder, func, args, 2, "putsout");
                return NULL;
            }
        case '*':
            {
                if(a->val->resultantType->type != ETPointer)
                    die(ALN, "Only pointers may be dereferenced.");
                if(IS_ANY_PTR(a->val->resultantType))
                    die(ALN, "Any pointers may not be dereferenced without cast.");

                a->savedWrapped = v;
                ac_unwrap_pointer(cb, &v, a->val->resultantType, 0);

                EaglePointerType *pt = (EaglePointerType *)a->val->resultantType;
                a->resultantType = pt->to;

                LLVMValueRef r = v;
                if(a->resultantType->type != ETStruct && a->resultantType->type != ETClass)
                    r = LLVMBuildLoad(cb->builder, v, "dereftmp");
                return r;
            }
            /*
        case 'c':
            {
                if(a->val->resultantType->type != ETArray)
                    die(ALN, "countof operator only valid for arrays.");

                LLVMValueRef r = LLVMBuildStructGEP(cb->builder, v, 0, "ctp");
                a->resultantType = ett_base_type(ETInt64);
                return LLVMBuildLoad(cb->builder, r, "dereftmp");
            }
            */
        /*
        case 'b':
            LLVMBuildBr(cb->builder, cb->currentLoopExit);
            break;
        case 'c':
            LLVMBuildBr(cb->builder, cb->currentLoopEntry);
            break;
            */
        case 'u':
            {
                if(!ET_IS_COUNTED(a->val->resultantType) && !ET_IS_WEAK(a->val->resultantType))
                    die(ALN, "Only pointers in the counted regime may be unwrapped.");

                a->resultantType = ett_pointer_type(((EaglePointerType *)a->val->resultantType)->to);
                return LLVMBuildStructGEP(cb->builder, v, 5, "unwrap");
            }
        case 's':
            {
                ASTTypeDecl *aty = (ASTTypeDecl *)a->val;
                LLVMTypeRef ty = ett_llvm_type(aty->etype);

                unsigned long size = LLVMABISizeOfType(cb->td, ty);
                a->resultantType = ett_base_type(ETInt64);

                return LLVMConstInt(LLVMInt64Type(), size, 0);
            }
        case '!':
            // TODO: Broken
            a->resultantType = ett_base_type(ETInt1);
            return ac_compile_ntest(a->val, v, cb);
        default:
            die(ALN, "Invalid unary operator (%c).", a->op);
            break;
    }

    return NULL;
}

LLVMValueRef ac_compile_function_call(AST *ast, CompilerBundle *cb)
{
    ASTFuncCall *a = (ASTFuncCall *)ast;

    LLVMValueRef func = ac_dispatch_expression(a->callee, cb);

    LLVMValueRef instanceOfClass = NULL;
    if(a->callee->type == ASTRUCTMEMBER)
    {
        ASTStructMemberGet *asmg = (ASTStructMemberGet *)a->callee;
        instanceOfClass = asmg->leftCompiled;
        asmg->leftCompiled = NULL;
    }

    EagleTypeType *orig = a->callee->resultantType;

    EagleFunctionType *ett;
    if(orig->type == ETFunction)
        ett = (EagleFunctionType *)orig;
    else
        ett = (EagleFunctionType *)((EaglePointerType *)orig)->to;

    a->resultantType = ett->retType;

    int start = instanceOfClass ? 1 : 0;
    int offset = instanceOfClass ? 0 : 1;

    AST *p;
    int i;
    for(p = a->params, i = start; p; p = p->next, i++);
    int ct = i;

    LLVMValueRef args[ct + offset];
    for(p = a->params, i = start; p; p = p->next, i++)
    {
        LLVMValueRef val = ac_dispatch_expression(p, cb);
        EagleTypeType *rt = p->resultantType;

        if(i < ett->pct)
        {
            if(!ett_are_same(rt, ett->params[i]))
                val = ac_build_conversion(cb->builder, val, rt, ett->params[i]);
        }

        hst_remove_key(&cb->transients, p, ahhd, ahed);
        // hst_remove_key(&cb->loadedTransients, p, ahhd, ahed);
        args[i + offset] = val;
    }

    LLVMValueRef out;
    if(ET_IS_CLOSURE(ett))
    {
        // func = LLVMBuildLoad(cb->builder, func, "");
        if(!ET_IS_RECURSE(ett))
            ac_unwrap_pointer(cb, &func, NULL, 0);

        if(instanceOfClass)
            die(ALN, "Internal compiler error!");
        // if(ET_HAS_CLOASED(ett))
        args[0] = LLVMBuildLoad(cb->builder, LLVMBuildStructGEP(cb->builder, func, 1, ""), "");
        // else
        //     args[0] = LLVMConstPointerNull(LLVMPointerType(LLVMInt8Type(), 0));


        func = LLVMBuildStructGEP(cb->builder, func, 0, "");
        func = LLVMBuildLoad(cb->builder, func, "");
        func = LLVMBuildBitCast(cb->builder, func, LLVMPointerType(ett_closure_type((EagleTypeType *)ett), 0), "");
        
        out = LLVMBuildCall(cb->builder, func, args, ct + 1, "");
    }
    else if(instanceOfClass)
    {
        args[0] = LLVMBuildBitCast(cb->builder, instanceOfClass, LLVMPointerType(LLVMInt8Type(), 0), "");

        out = LLVMBuildCall(cb->builder, func, args, ct, ett->retType->type == ETVoid ? "" : "callout");
    }
    else
        out = LLVMBuildCall(cb->builder, func, args + 1, ct, ett->retType->type == ETVoid ? "" : "callout");

    if(ET_IS_COUNTED(ett->retType) || (ett->retType->type == ETStruct && ty_needs_destructor(ett->retType)))
    {
        hst_put(&cb->loadedTransients, ast, out, ahhd, ahed);
    }

    return out;
}

LLVMValueRef ac_build_store(AST *ast, CompilerBundle *cb, char update)
{
    ASTBinary *a = (ASTBinary *)ast;
    EagleTypeType *totype;
    LLVMValueRef pos;

    VarBundle *storageBundle = NULL;
    char *storageIdent = NULL;

    if(a->left->type == AIDENT)
    {
        ASTValue *l = (ASTValue *)a->left;
        VarBundle *b = vs_get(cb->varScope, l->value.id);

        if(ET_IS_CLOSED(b->type))
        {
            totype = ((EaglePointerType *)b->type)->to;
            pos = LLVMBuildLoad(cb->builder, b->value, "");
            pos = LLVMBuildStructGEP(cb->builder, pos, 5, "");
        }
        else
        {
            totype = b->type;
            pos = b->value;
        }

        storageIdent = l->value.id;
        storageBundle = b;
    }
    else if(a->left->type == AUNARY && ((ASTUnary *)a->left)->op == '*')
    {
        ASTUnary *l = (ASTUnary *)a->left;
        
        pos = ac_dispatch_expression(l->val, cb);
        if(l->val->resultantType->type != ETPointer)
        {
            fprintf(stderr, "Error: Only pointers may be dereferenced.\n");
            exit(1);
        }
        if(IS_ANY_PTR(l->val->resultantType))
        {
            fprintf(stderr, "Error: Any pointers may not be dereferenced without cast.\n");
            exit(1);
        }

        totype = ((EaglePointerType *)l->val->resultantType)->to;
        ac_unwrap_pointer(cb, &pos, l->val->resultantType, 0);
    }
    else if(a->left->type == AVARDECL)
    {
        pos = ac_dispatch_expression(a->left, cb);
        totype = a->left->resultantType;

        if(!pos)
        {
            storageIdent = ((ASTVarDecl *)a->left)->ident;
            storageBundle = vs_get(cb->varScope, storageIdent);
        }
    }
    else if(a->left->type == ABINARY && ((ASTBinary *)a->left)->op == '[')
    {
        pos = ac_compile_index(a->left, 1, cb);
        totype = a->left->resultantType;
    }
    else if(a->left->type == ASTRUCTMEMBER)
    {
        pos = ac_compile_struct_member(a->left, cb, 1);
        totype = a->left->resultantType;
    }
    else
    {
        die(ALN, "Left hand side may not be assigned to.");
        return NULL;
    }
    /*
    else
    {
        ASTVarDecl *l = (ASTVarDecl *)a->left;
        
        ASTTypeDecl *type = (ASTTypeDecl *)l->atype;
        totype = type->etype;
        pos = ac_compile_var_decl(a->left, cb);
    }
    */

    LLVMValueRef r = ac_dispatch_expression(a->right, cb);
    EagleTypeType *fromtype = a->right->resultantType;

    if(totype->type == ETAuto)
    {
        totype = fromtype;
        if(!storageBundle || !storageIdent)
            die(ALN, "Internal compiler error!\nstorageBundle = %p; storageIdent = %p;", storageBundle, storageIdent);

        pos = ac_compile_var_decl_ext(totype, storageIdent, cb);
        storageBundle->type = totype;
    }

    a->resultantType = totype;

    if(!ett_are_same(fromtype, totype) && (!update || (update && totype->type != ETPointer)))
        r = ac_build_conversion(cb->builder, r, fromtype, totype);

    int transient = 0;
    LLVMValueRef ptrPos = NULL;
    if(ET_IS_COUNTED(a->resultantType))
    {
        ptrPos = pos;
        ac_decr_pointer(cb, &pos, totype);

        /*if(hst_remove_key(&cb->transients, a->right, ahhd, ahed))
            transient = 1;*/
        hst_remove_key(&cb->transients, a->right, ahhd, ahed);
        if(hst_remove_key(&cb->loadedTransients, a->right, ahhd, ahed))
            transient = 1;
        //ac_unwrap_pointer(cb, &pos, totype, 1);
    }
    else if(ET_IS_WEAK(a->resultantType))
    {
        ac_remove_weak_pointer(cb, pos, totype);
        ac_add_weak_pointer(cb, r, pos, totype);
    }
    else if(a->resultantType->type == ETStruct && ty_needs_destructor(a->resultantType))
    {
        ac_call_destructor(cb, pos, a->resultantType);
        r = LLVMBuildLoad(cb->builder, r, "");
        if(hst_remove_key(&cb->loadedTransients, a->right, ahhd, ahed))
            transient = 1;
        //ac_call_constr(cb, r, a->resultantType);
    }

    if(update)
    {
        LLVMValueRef cur = LLVMBuildLoad(cb->builder, pos, "");
        r = ac_generic_binary(a, cur, r, 1, fromtype, totype, cb);
        // r = ac_make_add(cur, r, cb->builder, totype->type);
    }

    LLVMBuildStore(cb->builder, r, pos);
    
    if(ET_IS_COUNTED(a->resultantType) && !transient)
        ac_incr_pointer(cb, &ptrPos, totype);
    else if(a->resultantType->type == ETStruct && ty_needs_destructor(a->resultantType) && !transient)
        ac_call_copy_constructor(cb, pos, a->resultantType);

    return LLVMBuildLoad(cb->builder, pos, "loadtmp");
}
