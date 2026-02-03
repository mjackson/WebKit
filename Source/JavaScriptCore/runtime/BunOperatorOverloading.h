#pragma once

#include <cstdint>

namespace JSC {
class JSGlobalObject;
typedef int64_t EncodedJSValue;
}

enum BunBinaryOp : uint8_t {
    BunBinaryOp_Add,
    BunBinaryOp_Sub,
    BunBinaryOp_Mul,
    BunBinaryOp_Div,
    BunBinaryOp_Remainder,
    BunBinaryOp_Pow,
    BunBinaryOp_BitwiseAnd,
    BunBinaryOp_BitwiseOr,
    BunBinaryOp_BitwiseXor,
    BunBinaryOp_LShift,
    BunBinaryOp_RShift,
};

enum BunUnaryOp : uint8_t {
    BunUnaryOp_Negate,
    BunUnaryOp_BitwiseNot,
};

enum BunCompareOp : uint8_t {
    BunCompareOp_LessThan,
    BunCompareOp_LessThanOrEqual,
    BunCompareOp_Equal,
    BunCompareOp_StrictEqual,
};

extern "C" JSC::EncodedJSValue Bun__tryBinaryOp(JSC::JSGlobalObject*, JSC::EncodedJSValue, JSC::EncodedJSValue, BunBinaryOp);
extern "C" JSC::EncodedJSValue Bun__tryUnaryOp(JSC::JSGlobalObject*, JSC::EncodedJSValue, BunUnaryOp);
// Returns: 0 = not handled, 1 = true, -1 = false
extern "C" int32_t Bun__tryCompareOp(JSC::JSGlobalObject*, JSC::EncodedJSValue, JSC::EncodedJSValue, BunCompareOp);
