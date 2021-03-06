/*
 * Copyright (c) 2015-2016 Sam Horlbeck Olsen
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "ast_compiler.h"

LLVMValueRef ac_const_value(AST *ast, CompilerBundle *cb)
{
    ASTValue *a = (ASTValue *)ast;
    switch(a->etype)
    {
        case ETInt1:
            a->resultantType = ett_base_type(ETInt1);
            return LLVMConstInt(LLVMInt1TypeInContext(utl_get_current_context()), a->value.i, 1);
        case ETInt8:
            a->resultantType = ett_base_type(ETInt8);
            return LLVMConstInt(LLVMInt8TypeInContext(utl_get_current_context()), a->value.i, 1);
        case ETInt32:
            a->resultantType = ett_base_type(ETInt32);
            return LLVMConstInt(LLVMInt32TypeInContext(utl_get_current_context()), a->value.i, 1);
        case ETDouble:
            a->resultantType = ett_base_type(ETDouble);
            return LLVMConstReal(LLVMDoubleTypeInContext(utl_get_current_context()), a->value.d);
        case ETCString:
        {
            a->resultantType = ett_pointer_type(ett_base_type(ETInt8));
            LLVMValueRef conststr = LLVMConstStringInContext(utl_get_current_context(), a->value.id, strlen(a->value.id), 0);
            
            LLVMValueRef globstr = LLVMAddGlobal(cb->module, LLVMTypeOf(conststr), ".str");
            LLVMSetInitializer(globstr, conststr);
            LLVMSetThreadLocal(globstr, 0);
            LLVMSetLinkage(globstr, LLVMPrivateLinkage);
            LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(utl_get_current_context()), 0, 1);
            return LLVMConstGEP(globstr, &zero, 1);
            //LLVMValueRef ptr = LLVMBuildGlobalStringPtr(cb->builder, a->value.id, "str");
            
        }
        case ETNil:
            a->resultantType = ett_pointer_type(ett_base_type(ETAny));
            return LLVMConstPointerNull(ett_llvm_type(a->resultantType));
        default:
            die(ALN, "Unknown value type.");
            return NULL;
    }
}

LLVMValueRef ac_convert_const(LLVMValueRef val, EagleComplexType *to, EagleComplexType *from)
{
    switch(from->type)
    {
        case ETInt1:
        case ETInt8:
        case ETInt16:
        case ETInt32:
        case ETInt64:
        case ETUInt8:
        case ETUInt16:
        case ETUInt32:
        case ETUInt64:
        {
            switch(to->type)
            {
                case ETInt1:
                    return LLVMConstICmp(LLVMIntNE, val, LLVMConstInt(ett_llvm_type(from), 0, 0));
                case ETInt8:
                case ETInt16:
                case ETInt32:
                case ETInt64:
                    return LLVMConstIntCast(val, ett_llvm_type(to), 1);
                case ETUInt8:
                case ETUInt16:
                case ETUInt32:
                case ETUInt64:
                    return LLVMConstIntCast(val, ett_llvm_type(to), 0);
                case ETDouble:
                    return LLVMConstSIToFP(val, ett_llvm_type(to));
                default:
                    return NULL;
            }
        }
        case ETDouble:
        {
            switch(to->type)
            {
                case ETDouble:
                    return val;
                case ETInt1:
                    return LLVMConstFCmp(LLVMRealONE, val, LLVMConstReal(0, 0));
                case ETInt8:
                case ETInt16:
                case ETInt32:
                case ETInt64:
                    return LLVMConstFPToSI(val, ett_llvm_type(to));
                case ETUInt8:
                case ETUInt16:
                case ETUInt32:
                case ETUInt64:
                    return LLVMConstFPToUI(val, ett_llvm_type(to));
                default:
                    return NULL;
            }
        }
        default:
            return NULL;
    }
}

