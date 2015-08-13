#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "ast_compiler.h"
#include "variable_manager.h"
#include "core/llvm_headers.h"

#define IS_ANY_PTR(t) (t->type == ETPointer && ((EaglePointerType *)t)->to->type == ETAny)
#define ALN (ast->lineno)
#define LN(a) (a->lineno)

typedef struct {
    LLVMModuleRef module;
    LLVMBuilderRef builder;

    EagleFunctionType *currentFunctionType;
    LLVMBasicBlockRef currentFunctionEntry;
    LLVMValueRef currentFunction;
    VarScope *currentFunctionScope;

    VarScopeStack *varScope;
    hashtable transients;
    hashtable loadedTransients;
} CompilerBundle;

static inline LLVMValueRef ac_build_conversion(LLVMBuilderRef builder, LLVMValueRef val, EagleTypeType *from, EagleTypeType *to);
static inline LLVMValueRef ac_make_add(LLVMValueRef left, LLVMValueRef right, LLVMBuilderRef builder, EagleType type);
static inline LLVMValueRef ac_make_sub(LLVMValueRef left, LLVMValueRef right, LLVMBuilderRef builder, EagleType type);
static inline LLVMValueRef ac_make_mul(LLVMValueRef left, LLVMValueRef right, LLVMBuilderRef builder, EagleType type);
static inline LLVMValueRef ac_make_div(LLVMValueRef left, LLVMValueRef right, LLVMBuilderRef builder, EagleType type);
static inline LLVMValueRef ac_make_comp(LLVMValueRef left, LLVMValueRef right, LLVMBuilderRef builder, EagleType type, char comp);
static inline void ac_unwrap_pointer(CompilerBundle *cb, LLVMValueRef *ptr, EagleTypeType *ty, int keepptr);
static inline void ac_incr_pointer(CompilerBundle *cb, LLVMValueRef *ptr, EagleTypeType *ty);
static inline void ac_incr_val_pointer(CompilerBundle *cb, LLVMValueRef *ptr, EagleTypeType *ty);
static inline void ac_check_pointer(CompilerBundle *cb, LLVMValueRef *ptr, EagleTypeType *ty);
static inline void ac_decr_pointer(CompilerBundle *cb, LLVMValueRef *ptr, EagleTypeType *ty);
static inline void ac_decr_val_pointer(CompilerBundle *cb, LLVMValueRef *ptr, EagleTypeType *ty);
static inline void ac_decr_in_array(CompilerBundle *cb, LLVMValueRef arr, int ct);
static inline void ac_nil_fill_array(CompilerBundle *cb, LLVMValueRef arr, int ct);

LLVMValueRef ac_dispatch_expression(AST *ast, CompilerBundle *cb);
void ac_dispatch_statement(AST *ast, CompilerBundle *cb);
void ac_dispatch_declaration(AST *ast, CompilerBundle *cb);

void ac_compile_function(AST *ast, CompilerBundle *cb);
int ac_compile_block(AST *ast, LLVMBasicBlockRef block, CompilerBundle *cb);
void ac_compile_if(AST *ast, CompilerBundle *cb, LLVMBasicBlockRef mergeBB);
LLVMValueRef ac_compile_index(AST *ast, int keepPointer, CompilerBundle *cb);
LLVMValueRef ac_compile_function_call(AST *ast, CompilerBundle *cb);

long ahhd(void *k, void *d)
{
    return (long)k;
}

int ahed(void *k, void *d)
{
    return k == d;
}

void die(int lineno, const char *fmt, ...)
{
    size_t len = strlen(fmt);
    char format[len + 9];
    sprintf(format, "Error: %s\n", fmt);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\t-> Line %d\n", lineno);
    va_end(args);

    exit(0);
}

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

    if(b->type->type == ETFunction)
    {
        a->resultantType = b->type;
        return b->value;
    }

    a->resultantType = b->type;

    if(b->type->type == ETArray && !ET_IS_GEN_ARR(b->type))
        return b->value;
    return LLVMBuildLoad(cb->builder, b->value, "loadtmp");
}

LLVMValueRef ac_compile_arrvar_decl(ASTVarDecl *a, CompilerBundle *cb)
{
    LLVMBasicBlockRef curblock = LLVMGetInsertBlock(cb->builder);
    LLVMPositionBuilderAtEnd(cb->builder, cb->currentFunctionEntry);

    ASTTypeDecl *type = (ASTTypeDecl *)a->atype;
    LLVMValueRef begin = LLVMGetFirstInstruction(cb->currentFunctionEntry);
    if(begin)
        LLVMPositionBuilderBefore(cb->builder, begin);
    LLVMValueRef pos = LLVMBuildArrayAlloca(cb->builder, ett_llvm_type(type->etype), ac_dispatch_expression(a->arrct, cb), a->ident);
    vs_put(cb->varScope, a->ident, pos, type->etype);
    a->resultantType = type->etype;

    LLVMPositionBuilderAtEnd(cb->builder, curblock);

    return pos;
}

void ac_scope_leave_callback(LLVMValueRef pos, EagleTypeType *ty, void *data)
{
    CompilerBundle *cb = data;
    ac_decr_pointer(cb, &pos, ty);
}

void ac_scope_leave_array_callback(LLVMValueRef pos, EagleTypeType *ty, void *data)
{
    CompilerBundle *cb = data;
    ac_decr_in_array(cb, pos, ett_array_count(ty));
}

LLVMValueRef ac_compile_var_decl(AST *ast, CompilerBundle *cb)
{
    ASTVarDecl *a = (ASTVarDecl *)ast;

    /*
    if(a->arrct)
        return ac_compile_arrvar_decl(a, cb);
        */

    LLVMBasicBlockRef curblock = LLVMGetInsertBlock(cb->builder);
    LLVMPositionBuilderAtEnd(cb->builder, cb->currentFunctionEntry);

    ASTTypeDecl *type = (ASTTypeDecl *)a->atype;

    LLVMValueRef begin = LLVMGetFirstInstruction(cb->currentFunctionEntry);
    if(begin)
        LLVMPositionBuilderBefore(cb->builder, begin);
    LLVMValueRef pos = LLVMBuildAlloca(cb->builder, ett_llvm_type(type->etype), a->ident);

    /*
    if(type->etype->type == ETArray)
    {
        EagleArrayType *at = (EagleArrayType *)type->etype;
        if(at->ct > 0)
        {
            LLVMValueRef ctp = LLVMBuildStructGEP(cb->builder, pos, 0, "ctp");
            LLVMBuildStore(cb->builder, LLVMConstInt(LLVMInt64Type(), at->ct, 1), ctp);
        }
    }
    */

    vs_put(cb->varScope, a->ident, pos, type->etype);

    if(ET_IS_COUNTED(type->etype))
    {
        LLVMBuildStore(cb->builder, LLVMConstPointerNull(ett_llvm_type(type->etype)), pos);
        vs_add_callback(cb->varScope, a->ident, ac_scope_leave_callback, cb);
    }

    if(type->etype->type == ETArray && ett_array_has_counted(type->etype))
    {
        ac_nil_fill_array(cb, pos, ett_array_count(type->etype));
        vs_add_callback(cb->varScope, a->ident, ac_scope_leave_array_callback, cb);
    }

    ast->resultantType = type->etype;

    LLVMPositionBuilderAtEnd(cb->builder, curblock);

    return pos;
}

LLVMValueRef ac_compile_new_decl(AST *ast, CompilerBundle *cb)
{
    ASTUnary *a = (ASTUnary *)ast;
    ASTTypeDecl *type = (ASTTypeDecl *)a->val;
    
    LLVMTypeRef tys[2];
    tys[0] = LLVMInt64Type();
    tys[1] = ett_llvm_type(type->etype);
    LLVMTypeRef tt = LLVMStructType(tys, 2, 0);

    LLVMValueRef mal = LLVMBuildMalloc(cb->builder, tt, "new");
    EaglePointerType *pt = (EaglePointerType *)ett_pointer_type(type->etype);
    pt->counted = 1;
    ast->resultantType = (EagleTypeType *)pt;

    LLVMValueRef pos = LLVMBuildStructGEP(cb->builder, mal, 0, "ctp");
    LLVMBuildStore(cb->builder, LLVMConstInt(LLVMInt64Type(), 0, 0), pos);

    hst_put(&cb->transients, ast, mal, ahhd, ahed);

    return mal;
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

LLVMValueRef ac_build_store(AST *ast, CompilerBundle *cb)
{
    ASTBinary *a = (ASTBinary *)ast;
    EagleTypeType *totype;
    LLVMValueRef pos;

    if(a->left->type == AIDENT)
    {
        ASTValue *l = (ASTValue *)a->left;
        VarBundle *b = vs_get(cb->varScope, l->value.id);

        totype = b->type;
        pos = b->value;
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
    }
    else if(a->left->type == ABINARY && ((ASTBinary *)a->left)->op == '[')
    {
        pos = ac_compile_index(a->left, 1, cb);
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

    a->resultantType = totype;

    if(!ett_are_same(fromtype, totype))
        r = ac_build_conversion(cb->builder, r, fromtype, totype);

    int transient = 0;
    LLVMValueRef ptrPos = NULL;
    if(a->resultantType->type == ETPointer)
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

    LLVMBuildStore(cb->builder, r, pos);
    
    if(a->resultantType->type == ETPointer && !transient)
        ac_incr_pointer(cb, &ptrPos, totype);

    return LLVMBuildLoad(cb->builder, pos, "loadtmp");
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

LLVMValueRef ac_compile_binary(AST *ast, CompilerBundle *cb)
{
    ASTBinary *a = (ASTBinary *)ast;
    if(a->op == '=')
        return ac_build_store(ast, cb);
    else if(a->op == '[')
        return ac_compile_index(ast, 0, cb);

    LLVMValueRef l = ac_dispatch_expression(a->left, cb);
    LLVMValueRef r = ac_dispatch_expression(a->right, cb);

    if(a->left->resultantType->type == ETPointer || a->right->resultantType->type == ETPointer)
    {
        EagleTypeType *lt = a->left->resultantType;
        EagleTypeType *rt = a->right->resultantType;
        if(a->op != '+' && a->op != '-')
            die(ALN, "Operation '%c' not valid for pointer types.", a->op);
        if(lt->type == ETPointer && !ET_IS_INT(rt->type))
            die(ALN, "Pointer arithmetic is only valid with integer and non-any pointer types.");
        if(rt->type == ETPointer && !ET_IS_INT(lt->type))
            die(ALN, "Pointer arithmetic is only valid with integer and non-any pointer types.");
        
        if(lt->type == ETPointer && ett_get_base_type(lt) == ETAny && ett_pointer_depth(lt) == 1)
            die(ALN, "Pointer arithmetic results in dereferencing any pointer.");
        if(rt->type == ETPointer && ett_get_base_type(rt) == ETAny && ett_pointer_depth(rt) == 1)
            die(ALN, "Pointer arithmetic results in dereferencing any pointer.");

        LLVMValueRef indexer = lt->type == ETPointer ? r : l;
        LLVMValueRef ptr = lt->type == ETPointer ? l : r;

        if(a->op == '-')
            indexer = LLVMBuildNeg(cb->builder, indexer, "neg");

        EaglePointerType *pt = lt->type == ETPointer ?
            (EaglePointerType *)lt : (EaglePointerType *)rt;

        ast->resultantType = (EagleTypeType *)pt;

        LLVMValueRef gep = LLVMBuildInBoundsGEP(cb->builder, ptr, &indexer, 1, "arith");
        return LLVMBuildBitCast(cb->builder, gep, ett_llvm_type((EagleTypeType *)pt), "cast");
    }

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
        case '+':
            return ac_make_add(l, r, cb->builder, promo);
        case '-':
            return ac_make_sub(l, r, cb->builder, promo);
        case '*':
            return ac_make_mul(l, r, cb->builder, promo);
        case '/':
            return ac_make_div(l, r, cb->builder, promo);
        case 'e':
        case 'n':
        case 'g':
        case 'l':
        case 'G':
        case 'L':
            ast->resultantType = ett_base_type(ETInt1);
            return ac_make_comp(l, r, cb->builder, promo, a->op);
        default:
            die(ALN, "Invalid binary operation (%c).", a->op);
            return NULL;
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

    LLVMValueRef v = ac_dispatch_expression(a->val, cb);

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
                    case ETPointer:
                        fmt = LLVMBuildGlobalStringPtr(cb->builder, "%p\n", "prfPTR");
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

                ac_unwrap_pointer(cb, &v, a->val->resultantType, 0);

                LLVMValueRef r = LLVMBuildLoad(cb->builder, v, "dereftmp");
                EaglePointerType *pt = (EaglePointerType *)a->val->resultantType;
                a->resultantType = pt->to;
                return r;
            }
        case 'c':
            {
                if(a->val->resultantType->type != ETArray)
                    die(ALN, "countof operator only valid for arrays.");

                LLVMValueRef r = LLVMBuildStructGEP(cb->builder, v, 0, "ctp");
                a->resultantType = ett_base_type(ETInt64);
                return LLVMBuildLoad(cb->builder, r, "dereftmp");
            }
        case '!':
            // TODO: Broken
            return LLVMBuildNot(cb->builder, v, "nottmp");
        default:
            die(ALN, "Invalid unary operator (%c).", a->op);
            break;
    }

    return NULL;
}

LLVMValueRef ac_dispatch_expression(AST *ast, CompilerBundle *cb)
{
    LLVMValueRef val = NULL;
    switch(ast->type)
    {
        case AVALUE:
            val = ac_compile_value(ast, cb);
            break;
        case ABINARY:
            val = ac_compile_binary(ast, cb);
            break;
        case AUNARY:
            val = ac_compile_unary(ast, cb);
            break;
        case AVARDECL:
            val = ac_compile_var_decl(ast, cb);
            break;
        case AIDENT:
            val = ac_compile_identifier(ast, cb);
            break;
        case AFUNCCALL:
            val = ac_compile_function_call(ast, cb);
            break;
        case ACAST:
            val = ac_compile_cast(ast, cb);
            break;
        case AALLOC:
            val = ac_compile_new_decl(ast, cb);
            break;
        default:
            die(ALN, "Invalid expression type.");
            return NULL;
    }

    if(!ast->resultantType)
        die(ALN, "Internal Error. AST Resultant Type for expression not set.");
    return val;
}

void ac_decr_loaded_transients(void *key, void *val, void *data)
{
    CompilerBundle *cb = data;
    AST *ast = key;
    LLVMValueRef pos = val;

    ac_decr_val_pointer(cb, &pos, ast->resultantType);
}

void ac_decr_transients(void *key, void *val, void *data)
{
    CompilerBundle *cb = data;
    AST *ast = key;
    LLVMValueRef pos = val;

    ac_check_pointer(cb, &pos, ast->resultantType);
}

void ac_dispatch_statement(AST *ast, CompilerBundle *cb)
{
    switch(ast->type)
    {
        case AVALUE:
            ac_compile_value(ast, cb);
            break;
        case ABINARY:
            ac_compile_binary(ast, cb);
            break;
        case AUNARY:
            ac_compile_unary(ast, cb);
            break;
        case AVARDECL:
            ac_compile_var_decl(ast, cb);
            break;
        case AIDENT:
            ac_compile_identifier(ast, cb);
            break;
        case AFUNCCALL:
            ac_compile_function_call(ast, cb);
            break;
        case AIF:
            ac_compile_if(ast, cb, NULL);
            break;
        case ACAST:
            ac_compile_cast(ast, cb);
            break;
        case AALLOC:
            ac_compile_new_decl(ast, cb);
            break;
        default:
            die(ALN, "Invalid statement type.");
    }

    hst_for_each(&cb->transients, ac_decr_transients, cb);
    hst_for_each(&cb->loadedTransients, ac_decr_loaded_transients, cb);
    
    hst_free(&cb->transients);
    hst_free(&cb->loadedTransients);

    cb->transients = hst_create();
    cb->loadedTransients = hst_create();
}

void ac_dispatch_declaration(AST *ast, CompilerBundle *cb)
{
    switch(ast->type)
    {
        case AFUNCDECL:
            ac_compile_function(ast, cb);
            break;
        default:
            die(ALN, "Invalid declaration type.");
            return;
    }
}

void ac_compile_if(AST *ast, CompilerBundle *cb, LLVMBasicBlockRef mergeBB)
{
    ASTIfBlock *a = (ASTIfBlock *)ast;
    LLVMValueRef val = ac_dispatch_expression(a->test, cb);

    LLVMValueRef cmp = NULL;
    if(a->test->resultantType->type == ETInt1)
        cmp = LLVMBuildICmp(cb->builder, LLVMIntNE, val, LLVMConstInt(LLVMInt1Type(), 0, 0), "cmp");
    else if(a->test->resultantType->type == ETInt8)
        cmp = LLVMBuildICmp(cb->builder, LLVMIntNE, val, LLVMConstInt(LLVMInt8Type(), 0, 0), "cmp");
    else if(a->test->resultantType->type == ETInt32)
        cmp = LLVMBuildICmp(cb->builder, LLVMIntNE, val, LLVMConstInt(LLVMInt32Type(), 0, 0), "cmp");
    else if(a->test->resultantType->type == ETInt64)
        cmp = LLVMBuildICmp(cb->builder, LLVMIntNE, val, LLVMConstInt(LLVMInt64Type(), 0, 0), "cmp");
    else if(a->test->resultantType->type == ETDouble)
        cmp = LLVMBuildFCmp(cb->builder, LLVMRealONE, val, LLVMConstReal(LLVMDoubleType(), 0.0), "cmp");
    else if(a->test->resultantType->type == ETPointer)
        cmp = LLVMBuildICmp(cb->builder, LLVMIntNE, val, LLVMConstPointerNull(ett_llvm_type(a->test->resultantType)), "cmp");
    else
        die(ALN, "Cannot test against given type.");

    int multiBlock = a->ifNext && ((ASTIfBlock *)a->ifNext)->test;
    int threeBlock = !!a->ifNext && !multiBlock;

    LLVMBasicBlockRef ifBB = LLVMAppendBasicBlock(cb->currentFunction, "if");
    LLVMBasicBlockRef elseBB = threeBlock || multiBlock ? LLVMAppendBasicBlock(cb->currentFunction, "else") : NULL;
    if(!mergeBB)
        mergeBB = LLVMAppendBasicBlock(cb->currentFunction, "merge");

    LLVMBuildCondBr(cb->builder, cmp, ifBB, elseBB ? elseBB : mergeBB);
    LLVMPositionBuilderAtEnd(cb->builder, ifBB);

    vs_push(cb->varScope);
    if(!ac_compile_block(a->block, ifBB, cb))
    {
        vs_run_callbacks_through(cb->varScope, cb->varScope->scope);
        LLVMBuildBr(cb->builder, mergeBB);
    }
    vs_pop(cb->varScope);

    if(threeBlock)
    {
        ASTIfBlock *el = (ASTIfBlock *)a->ifNext;
        LLVMPositionBuilderAtEnd(cb->builder, elseBB);

        vs_push(cb->varScope);
        if(!ac_compile_block(el->block, elseBB, cb))
        {
            vs_run_callbacks_through(cb->varScope, cb->varScope->scope);
            LLVMBuildBr(cb->builder, mergeBB);
        }
        vs_pop(cb->varScope);
    }
    else if(multiBlock)
    {
        AST *el = a->ifNext;
        LLVMPositionBuilderAtEnd(cb->builder, elseBB);
        ac_compile_if(el, cb, mergeBB);

        return;
    }

    LLVMBasicBlockRef last = LLVMGetLastBasicBlock(cb->currentFunction);
    LLVMMoveBasicBlockAfter(mergeBB, last);

    LLVMPositionBuilderAtEnd(cb->builder, mergeBB);
}

int ac_compile_block(AST *ast, LLVMBasicBlockRef block, CompilerBundle *cb)
{
    for(; ast; ast = ast->next)
    {
        if(ast->type == AUNARY && ((ASTUnary *)ast)->op == 'r') // Handle the special return case
        {
            LLVMValueRef val = ac_dispatch_expression(((ASTUnary *)ast)->val, cb);
            EagleTypeType *t = ((ASTUnary *)ast)->val->resultantType;
            EagleTypeType *o = cb->currentFunctionType->retType;

            if(!ett_are_same(t, o))
                val = ac_build_conversion(cb->builder, val, t, o);

            if(ET_IS_COUNTED(o))
                ac_incr_val_pointer(cb, &val, o);

            vs_run_callbacks_through(cb->varScope, cb->currentFunctionScope);

            LLVMBuildRet(cb->builder, val);

            return 1;
        }
        
        ac_dispatch_statement(ast, cb);
    }

    return 0;
}

LLVMValueRef ac_compile_function_call(AST *ast, CompilerBundle *cb)
{
    ASTFuncCall *a = (ASTFuncCall *)ast;

    LLVMValueRef func = ac_dispatch_expression(a->callee, cb);
    
    EagleFunctionType *ett = (EagleFunctionType *)a->callee->resultantType;

    a->resultantType = ett->retType;

    AST *p;
    int i;
    for(p = a->params, i = 0; p; p = p->next, i++);
    int ct = i;

    LLVMValueRef args[ct];
    for(p = a->params, i = 0; p; p = p->next, i++)
    {
        LLVMValueRef val = ac_dispatch_expression(p, cb);
        EagleTypeType *rt = p->resultantType;
        if(!ett_are_same(rt, ett->params[i]))
            val = ac_build_conversion(cb->builder, val, rt, ett->params[i]);

        hst_remove_key(&cb->transients, p, ahhd, ahed);
        // hst_remove_key(&cb->loadedTransients, p, ahhd, ahed);
        args[i] = val;
    }

    LLVMValueRef out = LLVMBuildCall(cb->builder, func, args, ct, ett->retType->type == ETVoid ? "" : "callout");
    if(ET_IS_COUNTED(ett->retType))
    {
        hst_put(&cb->loadedTransients, ast, out, ahhd, ahed);
    }

    return out;
}

void ac_compile_function(AST *ast, CompilerBundle *cb)
{
    ASTFuncDecl *a = (ASTFuncDecl *)ast;

    if(!a->body) // This is an extern definition
        return;
    
    int i;
    AST *p = a->params;
    for(i = 0; p; p = p->next, i++);

    int ct = i;

    EagleTypeType *eparam_types[ct];
    for(i = 0, p = a->params; i < ct; p = p->next, i++)
    {
        ASTTypeDecl *type = (ASTTypeDecl *)((ASTVarDecl *)p)->atype;
        eparam_types[i] = type->etype;
    }

    ASTTypeDecl *retType = (ASTTypeDecl *)a->retType;

    LLVMValueRef func = NULL;

    VarBundle *vb = vs_get(cb->varScope, a->ident);
    func = vb->value;
    cb->currentFunctionType = (EagleFunctionType *)vb->type;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(func, "entry");
    LLVMPositionBuilderAtEnd(cb->builder, entry);
    
    vs_push(cb->varScope);

    cb->currentFunctionEntry = entry;
    cb->currentFunction = func;
    cb->currentFunctionScope = cb->varScope->scope;

    if(a->params)
    {
        int i;
        AST *p = a->params;
        for(i = 0; p; p = p->next, i++)
        {
            LLVMValueRef pos = ac_compile_var_decl(p, cb);
            LLVMBuildStore(cb->builder, LLVMGetParam(func, i), pos);
            if(ET_IS_COUNTED(eparam_types[i]))
                ac_incr_pointer(cb, &pos, eparam_types[i]);
        }
    }

    if(!ac_compile_block(a->body, entry, cb) && retType->etype->type != ETVoid)
        die(ALN, "Function must return a value.");

    if(retType->etype->type == ETVoid)
    {
        vs_run_callbacks_through(cb->varScope, cb->varScope->scope);
        LLVMBuildRetVoid(cb->builder);
    }
    vs_pop(cb->varScope);

    cb->currentFunctionEntry = NULL;
}

void ac_prepare_module(LLVMModuleRef module)
{
    LLVMTypeRef param_types[] = {LLVMPointerType(LLVMInt8Type(), 0)};
    LLVMTypeRef func_type = LLVMFunctionType(LLVMInt32Type(), param_types, 1, 1);
    LLVMAddFunction(module, "printf", func_type);

    LLVMTypeRef param_types_rc[] = {LLVMPointerType(LLVMInt64Type(), 0)};
    LLVMTypeRef func_type_rc = LLVMFunctionType(LLVMVoidType(), param_types_rc, 1, 0);
    LLVMAddFunction(module, "__egl_incr_ptr", func_type_rc);
    LLVMAddFunction(module, "__egl_decr_ptr", func_type_rc);
    LLVMAddFunction(module, "__egl_check_ptr", func_type_rc);

    LLVMTypeRef param_types_arr1[] = {LLVMPointerType(LLVMInt8Type(), 0), LLVMInt64Type()};
    func_type_rc = LLVMFunctionType(LLVMVoidType(), param_types_arr1, 2, 0);
    LLVMAddFunction(module, "__egl_array_fill_nil", func_type_rc);

    param_types_arr1[0] = LLVMPointerType(LLVMPointerType(LLVMInt8Type(), 0), 0);
    func_type_rc = LLVMFunctionType(LLVMVoidType(), param_types_arr1, 2, 0);
    LLVMAddFunction(module, "__egl_array_decr_ptrs", func_type_rc);
}

void ac_add_early_declarations(AST *ast, CompilerBundle *cb)
{
    if(ast->type != AFUNCDECL)
        return;

    ASTFuncDecl *a = (ASTFuncDecl *)ast;
    int i, ct;
    AST *p;
    for(p = a->params, i = 0; p; p = p->next, i++);
    ct = i;

    LLVMTypeRef param_types[ct];
    EagleTypeType *eparam_types[ct];
    for(i = 0, p = a->params; p; p = p->next, i++)
    {
        ASTTypeDecl *type = (ASTTypeDecl *)((ASTVarDecl *)p)->atype;

        param_types[i] = ett_llvm_type(type->etype);
        eparam_types[i] = type->etype;
    }

    ASTTypeDecl *retType = (ASTTypeDecl *)a->retType;
    LLVMTypeRef func_type = LLVMFunctionType(ett_llvm_type(retType->etype), param_types, ct, 0);
    LLVMValueRef func = LLVMAddFunction(cb->module, a->ident, func_type);

    vs_put(cb->varScope, a->ident, func, ett_function_type(retType->etype, eparam_types, ct));
}

LLVMModuleRef ac_compile(AST *ast)
{
    CompilerBundle cb;
    cb.module = LLVMModuleCreateWithName("main-module");
    cb.builder = LLVMCreateBuilder();
    cb.transients = hst_create();
    cb.loadedTransients = hst_create();
    
    VarScopeStack vs = vs_make();
    cb.varScope = &vs;

    vs_push(cb.varScope);

    ac_prepare_module(cb.module);

    AST *old = ast;
    for(; ast; ast = ast->next)
        ac_add_early_declarations(ast, &cb);

    ast = old;

    for(; ast; ast = ast->next)
    {
        ac_dispatch_declaration(ast, &cb);
    }

    vs_pop(cb.varScope);

    LLVMDisposeBuilder(cb.builder);
    vs_free(cb.varScope);
    return cb.module;
}

LLVMValueRef ac_build_conversion(LLVMBuilderRef builder, LLVMValueRef val, EagleTypeType *from, EagleTypeType *to)
{
    switch(from->type)
    {
        case ETPointer:
        {
            if(to->type != ETPointer)
                die(-1, "Non-pointer type may not be converted to pointer type.");

            if(ett_get_base_type(to) == ETAny)
                return LLVMBuildBitCast(builder, val, ett_llvm_type(to), "ptrtmp");
            if(ett_get_base_type(from) == ETAny && ett_pointer_depth(from) == 1)
                return LLVMBuildBitCast(builder, val, ett_llvm_type(to), "ptrtmp");

            if(ett_pointer_depth(to) != ett_pointer_depth(from))
                die(-1, "Implicit pointer conversion invalid. Cannot conver pointer of depth %d to depth %d.", ett_pointer_depth(to), ett_pointer_depth(from));
            if(ett_get_base_type(to) != ett_get_base_type(from))
                die(-1, "Implicit pointer conversion invalid; pointer types are incompatible.");

            return LLVMBuildBitCast(builder, val, ett_llvm_type(to), "ptrtmp");
        }
        case ETArray:
        {
            ett_debug_print(to);
            if(to->type != ETPointer && to->type != ETArray)
                die(-1, "Arrays may only be converted to equivalent pointers.");

            //LLVMValueRef zero = LLVMConstInt(LLVMInt32Type(), 0, 0);
            return LLVMBuildBitCast(builder, val, ett_llvm_type(to), "ptrtmp");
        }
        case ETInt1:
        case ETInt8:
        case ETInt32:
        case ETInt64:
            switch(to->type)
            {
                case ETInt1:
                    return LLVMBuildICmp(builder, LLVMIntNE, val, LLVMConstInt(ett_llvm_type(from), 0, 0), "cmp");
                case ETInt8:
                case ETInt32:
                case ETInt64:
                    return LLVMBuildIntCast(builder, val, ett_llvm_type(to), "conv");
                case ETDouble:
                    return LLVMBuildSIToFP(builder, val, LLVMDoubleType(), "conv");
                default:
                    die(-1, "Invalid implicit conversion.");
                    break;
            }
        case ETDouble:
            switch(to->type)
            {
                case ETDouble:
                    return val;
                case ETInt1:
                    return LLVMBuildFCmp(builder, LLVMRealONE, val, LLVMConstReal(0, 0), "cmp");
                case ETInt8:
                case ETInt32:
                case ETInt64:
                    return LLVMBuildFPToSI(builder, val, ett_llvm_type(to), "conv");
                default:
                    die(-1, "Invalid implicit conversion from double.");
                    break;
            }
            break;
        default:
            die(-1, "Invalid implicit conversion.");
            break;
    }

    return NULL;
}

LLVMValueRef ac_make_add(LLVMValueRef left, LLVMValueRef right, LLVMBuilderRef builder, EagleType type)
{
    switch(type)
    {
        case ETDouble:
            return LLVMBuildFAdd(builder, left, right, "addtmp");
        case ETInt8:
        case ETInt32:
        case ETInt64:
            return LLVMBuildAdd(builder, left, right, "addtmp");
        default:
            die(-1, "The given types may not be summed.");
            return NULL;
    }
}

LLVMValueRef ac_make_sub(LLVMValueRef left, LLVMValueRef right, LLVMBuilderRef builder, EagleType type)
{
    switch(type)
    {
        case ETDouble:
            return LLVMBuildFSub(builder, left, right, "subtmp");
        case ETInt8:
        case ETInt32:
        case ETInt64:
            return LLVMBuildSub(builder, left, right, "subtmp");
        default:
            die(-1, "The given types may not be subtracted.");
            return NULL;
    }
}

LLVMValueRef ac_make_mul(LLVMValueRef left, LLVMValueRef right, LLVMBuilderRef builder, EagleType type)
{
    switch(type)
    {
        case ETDouble:
            return LLVMBuildFMul(builder, left, right, "multmp");
        case ETInt8:
        case ETInt32:
        case ETInt64:
            return LLVMBuildMul(builder, left, right, "multmp");
        default:
            die(-1, "The given types may not be multiplied.");
            return NULL;
    }
}

LLVMValueRef ac_make_div(LLVMValueRef left, LLVMValueRef right, LLVMBuilderRef builder, EagleType type)
{
    switch(type)
    {
        case ETDouble:
            return LLVMBuildFDiv(builder, left, right, "divtmp");
        case ETInt8:
        case ETInt32:
        case ETInt64:
            return LLVMBuildSDiv(builder, left, right, "divtmp");
        default:
            die(-1, "The given types may not be divided.");
            return NULL;
    }
}

LLVMValueRef ac_make_comp(LLVMValueRef left, LLVMValueRef right, LLVMBuilderRef builder, EagleType type, char comp)
{
    LLVMIntPredicate ip;
    LLVMRealPredicate rp;

    switch(comp)
    {
        case 'e':
            ip = LLVMIntEQ;
            rp = LLVMRealOEQ;
            break;
        case 'n':
            ip = LLVMIntNE;
            rp = LLVMRealONE;
            break;
        case 'g':
            ip = LLVMIntSGT;
            rp = LLVMRealOGT;
            break;
        case 'l':
            ip = LLVMIntSLT;
            rp = LLVMRealOLT;
            break;
        case 'G':
            ip = LLVMIntSGE;
            rp = LLVMRealOGE;
            break;
        case 'L':
            ip = LLVMIntSLE;
            rp = LLVMRealOLE;
            break;
        default:
            ip = LLVMIntEQ;
            rp = LLVMRealOEQ;
            break;
    }

    switch(type)
    {
        case ETDouble:
            return LLVMBuildFCmp(builder, rp, left, right, "eqtmp");
        case ETInt1:
        case ETInt8:
        case ETInt32:
        case ETInt64:
            return LLVMBuildICmp(builder, ip, left, right, "eqtmp");
        default:
            die(-1, "The given types may not be compared.");
            return NULL;
    }
}

void ac_unwrap_pointer(CompilerBundle *cb, LLVMValueRef *ptr, EagleTypeType *ty, int keepptr)
{
    EaglePointerType *pt = (EaglePointerType *)ty;
    if(!pt->counted)
        return;

    LLVMValueRef tptr = *ptr;//LLVMBuildLoad(cb->builder, *ptr, "tptr");

    LLVMValueRef pos = LLVMBuildStructGEP(cb->builder, tptr, 1, "unwrap");

    *ptr = pos;
}

void ac_incr_val_pointer(CompilerBundle *cb, LLVMValueRef *ptr, EagleTypeType *ty)
{
    LLVMBuilderRef builder = cb->builder;
    EaglePointerType *pt = (EaglePointerType *)ty;
    if(!pt->counted)
        return;

    LLVMValueRef tptr = *ptr;
    tptr = LLVMBuildBitCast(builder, tptr, LLVMPointerType(LLVMInt64Type(), 0), "cast");
    
    LLVMValueRef func = LLVMGetNamedFunction(cb->module, "__egl_incr_ptr");
    LLVMBuildCall(builder, func, &tptr, 1, ""); 
}

void ac_incr_pointer(CompilerBundle *cb, LLVMValueRef *ptr, EagleTypeType *ty)
{
    LLVMBuilderRef builder = cb->builder;
    EaglePointerType *pt = (EaglePointerType *)ty;
    if(!pt->counted)
        return;

    LLVMValueRef tptr = LLVMBuildLoad(builder, *ptr, "tptr");
    tptr = LLVMBuildBitCast(builder, tptr, LLVMPointerType(LLVMInt64Type(), 0), "cast");
    
    LLVMValueRef func = LLVMGetNamedFunction(cb->module, "__egl_incr_ptr");
    LLVMBuildCall(builder, func, &tptr, 1, ""); 
    /*
    LLVMBasicBlockRef nb = LLVMAppendBasicBlock(cb->currentFunction, "nil");
    LLVMBasicBlockRef mb = LLVMAppendBasicBlock(cb->currentFunction, "mergeIncr");

    LLVMValueRef ptrCheck = LLVMBuildICmp(cb->builder, LLVMIntNE, tptr, LLVMConstPointerNull(ett_llvm_type(ty)), "cmp");
    LLVMBuildCondBr(builder, ptrCheck, nb, mb);

    LLVMPositionBuilderAtEnd(builder, nb);

    LLVMValueRef pos = LLVMBuildStructGEP(builder, tptr, 0, "ct");
    LLVMValueRef ct = LLVMBuildLoad(builder, pos, "tct");
    LLVMValueRef incr = LLVMBuildAdd(builder, ct, LLVMConstInt(LLVMInt64Type(), 1, 0), "sum");
    LLVMBuildStore(builder, incr, pos);

    LLVMBuildBr(builder, mb);
    LLVMPositionBuilderAtEnd(builder, mb);
    */
}

void ac_check_pointer(CompilerBundle *cb, LLVMValueRef *ptr, EagleTypeType *ty)
{
    EaglePointerType *pt = (EaglePointerType *)ty;
    if(!pt->counted)
        return;

    LLVMValueRef tptr = LLVMBuildBitCast(cb->builder, *ptr, LLVMPointerType(LLVMInt64Type(), 0), "");
    LLVMValueRef func = LLVMGetNamedFunction(cb->module, "__egl_check_ptr");
    LLVMBuildCall(cb->builder, func, &tptr, 1, "");
}

void ac_decr_val_pointer(CompilerBundle *cb, LLVMValueRef *ptr, EagleTypeType *ty)
{
    LLVMBuilderRef builder = cb->builder;
    EaglePointerType *pt = (EaglePointerType *)ty;
    if(!pt->counted)
        return;

    LLVMValueRef tptr = *ptr;
    tptr = LLVMBuildBitCast(builder, tptr, LLVMPointerType(LLVMInt64Type(), 0), "cast");
    
    LLVMValueRef func = LLVMGetNamedFunction(cb->module, "__egl_decr_ptr");
    LLVMBuildCall(builder, func, &tptr, 1, ""); 
}

void ac_nil_fill_array(CompilerBundle *cb, LLVMValueRef arr, int ct)
{
    LLVMBuilderRef builder = cb->builder;
    arr = LLVMBuildBitCast(builder, arr, LLVMPointerType(LLVMInt8Type(), 0), "");

    LLVMValueRef func = LLVMGetNamedFunction(cb->module, "__egl_array_fill_nil");
    LLVMValueRef vals[] = {arr, LLVMConstInt(LLVMInt64Type(), ct, 0)};
    LLVMBuildCall(builder, func, vals, 2, "");
}

void ac_decr_in_array(CompilerBundle *cb, LLVMValueRef arr, int ct)
{
    LLVMBuilderRef builder = cb->builder;
    arr = LLVMBuildBitCast(builder, arr, LLVMPointerType(LLVMPointerType(LLVMInt8Type(), 0), 0), "");

    LLVMValueRef func = LLVMGetNamedFunction(cb->module, "__egl_array_decr_ptrs");
    LLVMValueRef vals[] = {arr, LLVMConstInt(LLVMInt64Type(), ct, 0)};
    LLVMBuildCall(builder, func, vals, 2, "");
}

void ac_decr_pointer(CompilerBundle *cb, LLVMValueRef *ptr, EagleTypeType *ty)
{
    LLVMBuilderRef builder = cb->builder;
    EaglePointerType *pt = (EaglePointerType *)ty;
    if(!pt->counted)
        return;

    LLVMValueRef tptr = LLVMBuildLoad(builder, *ptr, "tptr");
    tptr = LLVMBuildBitCast(builder, tptr, LLVMPointerType(LLVMInt64Type(), 0), "cast");
    
    LLVMValueRef func = LLVMGetNamedFunction(cb->module, "__egl_decr_ptr");
    LLVMBuildCall(builder, func, &tptr, 1, ""); 

    /*
    LLVMValueRef tptr = needsloading ? LLVMBuildLoad(builder, *ptr, "tptr") : *ptr;
    LLVMBasicBlockRef nb = LLVMAppendBasicBlock(cb->currentFunction, "nil");
    LLVMBasicBlockRef ib = LLVMAppendBasicBlock(cb->currentFunction, "if");
    LLVMBasicBlockRef mb = LLVMAppendBasicBlock(cb->currentFunction, "mergeDecr");

    LLVMValueRef ptrCheck = LLVMBuildICmp(cb->builder, LLVMIntNE, tptr, LLVMConstPointerNull(ett_llvm_type(ty)), "cmp");
    LLVMBuildCondBr(builder, ptrCheck, nb, mb);

    LLVMPositionBuilderAtEnd(builder, nb);

    LLVMValueRef pos = LLVMBuildStructGEP(builder, tptr, 0, "ct");
    LLVMValueRef ct = LLVMBuildLoad(builder, pos, "tct");
    LLVMValueRef decr = LLVMBuildSub(builder, ct, LLVMConstInt(LLVMInt64Type(), 1, 0), "sub");
    LLVMBuildStore(builder, decr, pos);


    LLVMValueRef cmp = LLVMBuildICmp(builder, LLVMIntEQ, decr, LLVMConstInt(LLVMInt64Type(), 0, 0), "icmp");
    LLVMBuildCondBr(builder, cmp, ib, mb);

    LLVMPositionBuilderAtEnd(builder, ib);
    LLVMBuildFree(builder, tptr);

    if(needsloading)
        LLVMBuildStore(builder, LLVMConstPointerNull(ett_llvm_type(ty)), *ptr);
    LLVMBuildBr(builder, mb);

    LLVMPositionBuilderAtEnd(builder, mb);
    */
}

