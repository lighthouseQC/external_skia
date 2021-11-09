/*
 * Copyright 2021 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/sksl/SkSLConstantFolder.h"
#include "src/sksl/SkSLProgramSettings.h"
#include "src/sksl/ir/SkSLIndexExpression.h"
#include "src/sksl/ir/SkSLLiteral.h"
#include "src/sksl/ir/SkSLSwizzle.h"
#include "src/sksl/ir/SkSLSymbolTable.h"
#include "src/sksl/ir/SkSLTypeReference.h"

namespace SkSL {

const Type& IndexExpression::IndexType(const Context& context, const Type& type) {
    if (type.isMatrix()) {
        if (type.componentType() == *context.fTypes.fFloat) {
            switch (type.rows()) {
                case 2: return *context.fTypes.fFloat2;
                case 3: return *context.fTypes.fFloat3;
                case 4: return *context.fTypes.fFloat4;
                default: SkASSERT(false);
            }
        } else if (type.componentType() == *context.fTypes.fHalf) {
            switch (type.rows()) {
                case 2: return *context.fTypes.fHalf2;
                case 3: return *context.fTypes.fHalf3;
                case 4: return *context.fTypes.fHalf4;
                default: SkASSERT(false);
            }
        }
    }
    return type.componentType();
}

std::unique_ptr<Expression> IndexExpression::Convert(const Context& context,
                                                     SymbolTable& symbolTable,
                                                     std::unique_ptr<Expression> base,
                                                     std::unique_ptr<Expression> index) {
    // Convert an array type reference: `int[10]`.
    if (base->is<TypeReference>()) {
        const Type& baseType = base->as<TypeReference>().value();
        SKSL_INT arraySize = baseType.convertArraySize(context, std::move(index));
        if (!arraySize) {
            return nullptr;
        }
        return TypeReference::Convert(context, base->fLine,
                                      symbolTable.addArrayDimension(&baseType, arraySize));
    }
    // Convert an index expression with an expression inside of it: `arr[a * 3]`.
    const Type& baseType = base->type();
    if (!baseType.isArray() && !baseType.isMatrix() && !baseType.isVector()) {
        context.fErrors->error(base->fLine,
                               "expected array, but found '" + baseType.displayName() + "'");
        return nullptr;
    }
    if (!index->type().isInteger()) {
        index = context.fTypes.fInt->coerceExpression(std::move(index), context);
        if (!index) {
            return nullptr;
        }
    }
    // Perform compile-time bounds checking on constant-expression indices.
    const Expression* indexExpr = ConstantFolder::GetConstantValueForVariable(*index);
    if (indexExpr->isIntLiteral()) {
        SKSL_INT indexValue = indexExpr->as<Literal>().intValue();
        if (indexValue < 0 || indexValue >= baseType.columns()) {
            context.fErrors->error(base->fLine, "index " + to_string(indexValue) +
                                                " out of range for '" + baseType.displayName() +
                                                "'");
            return nullptr;
        }
    }
    return IndexExpression::Make(context, std::move(base), std::move(index));
}

std::unique_ptr<Expression> IndexExpression::Make(const Context& context,
                                                  std::unique_ptr<Expression> base,
                                                  std::unique_ptr<Expression> index) {
    const Type& baseType = base->type();
    SkASSERT(baseType.isArray() || baseType.isMatrix() || baseType.isVector());
    SkASSERT(index->type().isInteger());

    // Constant array indexes on vectors can be converted to swizzles: `v[2]` --> `v.z`.
    // Swizzling is harmless and can unlock further simplifications for some base types.
    const Expression* indexExpr = ConstantFolder::GetConstantValueForVariable(*index);
    if (indexExpr->isIntLiteral() && baseType.isVector()) {
        SKSL_INT indexValue = indexExpr->as<Literal>().intValue();
        return Swizzle::Make(context, std::move(base), ComponentArray{(int8_t)indexValue});
    }

    // TODO(skia:12472): constantArray[constantExpr] should be compile-time evaluated.

    return std::make_unique<IndexExpression>(context, std::move(base), std::move(index));
}

}  // namespace SkSL
