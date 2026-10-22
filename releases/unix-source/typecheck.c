#include "typecheck.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
    bool known;
    int value;
} IntegerConstant;

static TypecheckResult make_result(TypecheckStatus status, TypecheckType type,
                                   AstSpan span, const char *message)
{
    TypecheckResult result;
    result.status = status;
    result.type = type;
    result.span = span;
    result.message[0] = '\0';
    if (message)
    {
        (void)snprintf(result.message, sizeof(result.message), "%s", message);
    }
    return result;
}

static TypecheckResult error_result(const AstNode *node, const char *message)
{
    return make_result(TYPECHECK_ERROR, TYPECHECK_UNKNOWN_TYPE,
                       node ? node->span : (AstSpan){0, 0, 0, 0}, message);
}

static TypecheckResult unknown_result(const AstNode *node, const char *message)
{
    return make_result(TYPECHECK_UNKNOWN, TYPECHECK_UNKNOWN_TYPE,
                       node ? node->span : (AstSpan){0, 0, 0, 0}, message);
}

static TypecheckResult known_result(const AstNode *node, TypecheckType type)
{
    return make_result(TYPECHECK_OK, type, node->span, NULL);
}

static TypecheckResult infer(const AstNode *node);

static IntegerConstant constant_integer(const AstNode *node)
{
    IntegerConstant unknown = {false, 0};
    if (!node)
        return unknown;
    if (node->kind == AST_INTEGER)
    {
        IntegerConstant value = {true, node->as.integer};
        return value;
    }
    if (node->kind == AST_UNARY &&
        node->as.unary.operation == AST_OP_NEGATE)
    {
        IntegerConstant operand = constant_integer(node->as.unary.operand);
        if (operand.known && operand.value != INT_MIN)
        {
            operand.value = -operand.value;
            return operand;
        }
    }
    if (node->kind == AST_BINARY)
    {
        IntegerConstant left = constant_integer(node->as.binary.left);
        IntegerConstant right = constant_integer(node->as.binary.right);
        int value;
        if (!left.known || !right.known)
            return unknown;
        switch (node->as.binary.operation)
        {
        case AST_OP_ADD:
            if ((right.value > 0 && left.value > INT_MAX - right.value) ||
                (right.value < 0 && left.value < INT_MIN - right.value))
                return unknown;
            value = left.value + right.value;
            break;
        case AST_OP_SUBTRACT:
            if ((right.value < 0 && left.value > INT_MAX + right.value) ||
                (right.value > 0 && left.value < INT_MIN + right.value))
                return unknown;
            value = left.value - right.value;
            break;
        case AST_OP_MULTIPLY:
            if (left.value == 0 || right.value == 0)
                value = 0;
            else if ((left.value == -1 && right.value == INT_MIN) ||
                     (right.value == -1 && left.value == INT_MIN))
                return unknown;
            else
            {
                if (left.value > 0)
                {
                    if ((right.value > 0 && left.value > INT_MAX / right.value) ||
                        (right.value < 0 && right.value < INT_MIN / left.value))
                        return unknown;
                }
                else if ((right.value > 0 && left.value < INT_MIN / right.value) ||
                         (right.value < 0 && left.value < INT_MAX / right.value))
                    return unknown;
                value = left.value * right.value;
            }
            break;
        case AST_OP_DIVIDE:
            if (right.value == 0 ||
                (left.value == INT_MIN && right.value == -1))
                return unknown;
            value = left.value / right.value;
            break;
        default:
            return unknown;
        }
        IntegerConstant result = {true, value};
        return result;
    }
    return unknown;
}

static bool checked_add(int left, int right)
{
    return !((right > 0 && left > INT_MAX - right) ||
             (right < 0 && left < INT_MIN - right));
}

static bool checked_subtract(int left, int right)
{
    return !((right < 0 && left > INT_MAX + right) ||
             (right > 0 && left < INT_MIN + right));
}

static bool checked_multiply(int left, int right)
{
    if (left == 0 || right == 0)
        return true;
    if ((left == -1 && right == INT_MIN) ||
        (right == -1 && left == INT_MIN))
        return false;
    if (left > 0)
    {
        if (right > 0)
            return left <= INT_MAX / right;
        return right >= INT_MIN / left;
    }
    if (right > 0)
        return left >= INT_MIN / right;
    return left >= INT_MAX / right;
}

static bool has_name(const AstNode *node, const char *name)
{
    size_t length = strlen(name);
    return node->as.call.name_length == length &&
           memcmp(node->as.call.name, name, length) == 0;
}

static TypecheckResult infer_call(const AstNode *node)
{
    const char *name = node->as.call.name;
    size_t length = node->as.call.name_length;
    size_t expected;
    size_t i;
    TypecheckType argument_types[3];
    TypecheckType return_type;

    if (!name || length == 0 || node->as.call.argument_count > 3)
        return error_result(node, "Malformed or unsupported builtin call.");
    if (length == 3 && memcmp(name, "abs", 3) == 0)
    {
        expected = 1;
        return_type = TYPECHECK_INT;
    }
    else if ((length == 3 && memcmp(name, "min", 3) == 0) ||
             (length == 3 && memcmp(name, "max", 3) == 0))
    {
        expected = 2;
        return_type = TYPECHECK_INT;
    }
    else if (length == 3 && memcmp(name, "len", 3) == 0)
    {
        expected = 1;
        return_type = TYPECHECK_INT;
    }
    else if ((length == 5 && memcmp(name, "lower", 5) == 0) ||
             (length == 5 && memcmp(name, "upper", 5) == 0))
    {
        expected = 1;
        return_type = TYPECHECK_STRING;
    }
    else if (length == 6 && memcmp(name, "substr", 6) == 0)
    {
        expected = 3;
        return_type = TYPECHECK_STRING;
    }
    else if (length == 4 && memcmp(name, "trim", 4) == 0)
    {
        expected = 1;
        return_type = TYPECHECK_STRING;
    }
    else if ((length == 11 && memcmp(name, "starts_with", 11) == 0) ||
             (length == 9 && memcmp(name, "ends_with", 9) == 0))
    {
        expected = 2;
        return_type = TYPECHECK_BOOL;
    }
    else if (length == 8 && memcmp(name, "contains", 8) == 0)
    {
        expected = 2;
        return_type = TYPECHECK_BOOL;
    }
    else if (length == 8 && memcmp(name, "index_of", 8) == 0)
    {
        expected = 2;
        return_type = TYPECHECK_INT;
    }
    else if (length == 5 && memcmp(name, "count", 5) == 0)
    {
        expected = 2;
        return_type = TYPECHECK_INT;
    }
    else if (length == 7 && memcmp(name, "replace", 7) == 0)
    {
        expected = 3;
        return_type = TYPECHECK_STRING;
    }
    else if (length == 6 && memcmp(name, "repeat", 6) == 0)
    {
        expected = 2;
        return_type = TYPECHECK_STRING;
    }
    else if ((length == 4 && memcmp(name, "sqrt", 4) == 0) ||
             (length == 3 && memcmp(name, "sin", 3) == 0) ||
             (length == 3 && memcmp(name, "cos", 3) == 0) ||
             (length == 3 && memcmp(name, "log", 3) == 0) ||
             (length == 3 && memcmp(name, "exp", 3) == 0) ||
             (length == 5 && memcmp(name, "floor", 5) == 0) ||
             (length == 4 && memcmp(name, "ceil", 4) == 0) ||
             (length == 5 && memcmp(name, "round", 5) == 0))
    {
        expected = 1;
        return_type = TYPECHECK_FLOAT;
    }
    else if (length == 2 && memcmp(name, "pi", 2) == 0)
    {
        expected = 0;
        return_type = TYPECHECK_FLOAT;
    }
    else if (length == 10 && memcmp(name, "random_int", 10) == 0)
    {
        expected = 2;
        return_type = TYPECHECK_INT;
    }
    else if (length == 11 && memcmp(name, "random_seed", 11) == 0)
    {
        expected = 1;
        return_type = TYPECHECK_NULL;
    }
    else
        return error_result(node, "Call is not a recognized builtin.");

    if (node->as.call.argument_count != expected)
        return error_result(node, "Builtin called with incorrect arity.");
    if (expected > sizeof(argument_types) / sizeof(argument_types[0]))
        return error_result(node, "Builtin arity exceeds analyzer capacity.");

    for (i = 0; i < expected; i++)
    {
        TypecheckResult argument = infer(node->as.call.arguments[i]);
        if (argument.status != TYPECHECK_OK)
            return argument;
        argument_types[i] = argument.type;
    }

    if (has_name(node, "abs") || has_name(node, "min") ||
        has_name(node, "max"))
    {
        for (i = 0; i < expected; i++)
            if (argument_types[i] != TYPECHECK_INT &&
                argument_types[i] != TYPECHECK_FLOAT)
                return error_result(node, "Builtin requires numeric argument(s).");
        if (has_name(node, "abs"))
            return_type = argument_types[0];
        else if (argument_types[0] != argument_types[1])
            return unknown_result(
                node, "min/max result type depends on the selected argument.");
        else
            return_type = argument_types[0];
    }
    else if (has_name(node, "len"))
    {
        if (argument_types[0] != TYPECHECK_STRING &&
            argument_types[0] != TYPECHECK_ARRAY &&
            argument_types[0] != TYPECHECK_MAP &&
            argument_types[0] != TYPECHECK_SET)
            return error_result(node, "len requires a string or collection.");
    }
    else if (has_name(node, "lower") || has_name(node, "upper") ||
             has_name(node, "trim"))
    {
        if (argument_types[0] != TYPECHECK_STRING)
            return error_result(node, "String builtin requires a string.");
    }
    else if (has_name(node, "starts_with") || has_name(node, "ends_with"))
    {
        if (argument_types[0] != TYPECHECK_STRING ||
            argument_types[1] != TYPECHECK_STRING)
            return error_result(node, "String predicate requires two strings.");
    }
    else if (has_name(node, "contains") || has_name(node, "index_of") ||
             has_name(node, "count"))
    {
        if (argument_types[0] != TYPECHECK_STRING ||
            argument_types[1] != TYPECHECK_STRING)
            return error_result(node, "String search requires two strings.");
    }
    else if (has_name(node, "replace"))
    {
        if (argument_types[0] != TYPECHECK_STRING ||
            argument_types[1] != TYPECHECK_STRING ||
            argument_types[2] != TYPECHECK_STRING)
            return error_result(node, "replace requires three strings.");
    }
    else if (has_name(node, "repeat"))
    {
        if (argument_types[0] != TYPECHECK_STRING ||
            argument_types[1] != TYPECHECK_INT)
            return error_result(node, "repeat requires (string, int).");
    }
    else if (has_name(node, "random_int"))
    {
        if (argument_types[0] != TYPECHECK_INT ||
            argument_types[1] != TYPECHECK_INT)
            return error_result(node, "random_int requires two integers.");
    }
    else if (has_name(node, "random_seed"))
    {
        if (argument_types[0] != TYPECHECK_INT)
            return error_result(node, "random_seed requires an integer.");
    }
    else if (has_name(node, "sqrt") || has_name(node, "sin") ||
             has_name(node, "cos") || has_name(node, "log") ||
             has_name(node, "exp") || has_name(node, "floor") ||
             has_name(node, "ceil") || has_name(node, "round"))
    {
        if (argument_types[0] != TYPECHECK_INT &&
            argument_types[0] != TYPECHECK_FLOAT)
            return error_result(node, "Math function requires a number.");
    }
    else if (has_name(node, "substr"))
    {
        if (argument_types[0] != TYPECHECK_STRING ||
            argument_types[1] != TYPECHECK_INT ||
            argument_types[2] != TYPECHECK_INT)
            return error_result(node, "substr requires (string, int, int).");
    }

    if (has_name(node, "abs"))
    {
        IntegerConstant argument = constant_integer(node->as.call.arguments[0]);
        if (argument.known && argument.value == INT_MIN)
            return error_result(node, "abs argument overflows the integer range.");
    }
    return known_result(node, return_type);
}

static TypecheckResult infer_binary(const AstNode *node)
{
    TypecheckResult left = infer(node->as.binary.left);
    TypecheckResult right = infer(node->as.binary.right);
    AstOperator operation = node->as.binary.operation;
    TypecheckType ltype;
    TypecheckType rtype;

    if (left.status == TYPECHECK_ERROR)
        return left;
    if (right.status == TYPECHECK_ERROR)
        return right;
    if (left.status == TYPECHECK_UNKNOWN || right.status == TYPECHECK_UNKNOWN)
        return unknown_result(node, "Binary expression has an unknown operand type.");
    ltype = left.type;
    rtype = right.type;

    if (operation == AST_OP_EQUAL || operation == AST_OP_NOT_EQUAL)
        return known_result(node, TYPECHECK_BOOL);
    if (operation == AST_OP_IN)
    {
        if (rtype == TYPECHECK_SET || rtype == TYPECHECK_MAP)
        {
            if (ltype != TYPECHECK_INT && ltype != TYPECHECK_FLOAT &&
                ltype != TYPECHECK_BOOL && ltype != TYPECHECK_STRING &&
                ltype != TYPECHECK_NULL)
                return error_result(node,
                                    "Map and set membership requires a hashable scalar left operand.");
            return known_result(node, TYPECHECK_BOOL);
        }
        if (rtype == TYPECHECK_STRING && ltype == TYPECHECK_STRING)
            return known_result(node, TYPECHECK_BOOL);
        if (rtype == TYPECHECK_STRING)
            return error_result(node, "String membership requires a string left operand.");
        return error_result(node, "Membership requires a string, map, or set right operand.");
    }
    if (operation == AST_OP_LESS || operation == AST_OP_LESS_EQUAL ||
        operation == AST_OP_GREATER || operation == AST_OP_GREATER_EQUAL)
    {
        if (((ltype == TYPECHECK_INT || ltype == TYPECHECK_FLOAT) &&
             (rtype == TYPECHECK_INT || rtype == TYPECHECK_FLOAT)) ||
            (ltype == TYPECHECK_STRING && rtype == TYPECHECK_STRING))
            return known_result(node, TYPECHECK_BOOL);
        return error_result(node,
                            "Ordered comparison requires two numbers or two strings.");
    }
    if (operation == AST_OP_ADD &&
        (ltype == TYPECHECK_STRING || rtype == TYPECHECK_STRING))
        return known_result(node, TYPECHECK_STRING);
    if ((ltype != TYPECHECK_INT && ltype != TYPECHECK_FLOAT) ||
        (rtype != TYPECHECK_INT && rtype != TYPECHECK_FLOAT))
        return error_result(node, "Arithmetic requires two numbers unless '+' has a string operand.");

    if (ltype == TYPECHECK_INT && rtype == TYPECHECK_INT)
    {
        IntegerConstant lvalue = constant_integer(node->as.binary.left);
        IntegerConstant rvalue = constant_integer(node->as.binary.right);
        if (operation == AST_OP_ADD && lvalue.known && rvalue.known &&
            !checked_add(lvalue.value, rvalue.value))
            return error_result(node, "Integer addition overflows.");
        if (operation == AST_OP_SUBTRACT && lvalue.known && rvalue.known &&
            !checked_subtract(lvalue.value, rvalue.value))
            return error_result(node, "Integer subtraction overflows.");
        if (operation == AST_OP_MULTIPLY && lvalue.known && rvalue.known &&
            !checked_multiply(lvalue.value, rvalue.value))
            return error_result(node, "Integer multiplication overflows.");
        if (operation == AST_OP_DIVIDE && rvalue.known &&
            rvalue.value == 0)
            return error_result(node, "Integer division by zero.");
        if (operation == AST_OP_DIVIDE && lvalue.known && rvalue.known &&
            lvalue.value == INT_MIN && rvalue.value == -1)
            return error_result(node, "Integer division overflows.");
    }
    if (operation == AST_OP_ADD || operation == AST_OP_SUBTRACT ||
        operation == AST_OP_MULTIPLY || operation == AST_OP_DIVIDE)
        return known_result(node, ltype == TYPECHECK_FLOAT ||
                                      rtype == TYPECHECK_FLOAT
                                  ? TYPECHECK_FLOAT
                                  : TYPECHECK_INT);
    return error_result(node, "Unsupported binary operator.");
}

static TypecheckResult infer(const AstNode *node)
{
    size_t i;
    if (!node)
        return error_result(NULL, "Missing expression node.");

    switch (node->kind)
    {
    case AST_INTEGER:
        return known_result(node, TYPECHECK_INT);
    case AST_STRING:
        if (!node->as.string.bytes && node->as.string.length != 0)
            return error_result(node, "Malformed string literal.");
        return known_result(node, TYPECHECK_STRING);
    case AST_BOOLEAN:
        return known_result(node, TYPECHECK_BOOL);
    case AST_NULL:
        return known_result(node, TYPECHECK_NULL);
    case AST_NAME:
        if (!node->as.name.name || node->as.name.length == 0)
            return error_result(node, "Malformed name expression.");
        return unknown_result(node, "Name has no available type information.");
    case AST_GROUP:
    {
        TypecheckResult result = infer(node->as.grouped);
        result.span = node->span;
        return result;
    }
    case AST_UNARY:
    {
        TypecheckResult operand = infer(node->as.unary.operand);
        IntegerConstant constant;
        if (operand.status != TYPECHECK_OK)
            return operand;
        if (node->as.unary.operation != AST_OP_NEGATE)
            return error_result(node, "Unsupported unary operator.");
        if (operand.type != TYPECHECK_INT && operand.type != TYPECHECK_FLOAT)
            return error_result(node, "Unary negation requires a number.");
        constant = constant_integer(node->as.unary.operand);
        if (operand.type == TYPECHECK_INT && constant.known &&
            constant.value == INT_MIN)
            return error_result(node, "Integer negation overflows.");
        return known_result(node, operand.type);
    }
    case AST_BINARY:
        if (!node->as.binary.left || !node->as.binary.right)
            return error_result(node, "Malformed binary expression.");
        return infer_binary(node);
    case AST_INDEX:
    {
        TypecheckResult collection = infer(node->as.index.collection);
        TypecheckResult index = infer(node->as.index.index);
        if (collection.status == TYPECHECK_ERROR)
            return collection;
        if (index.status == TYPECHECK_ERROR)
            return index;
        return unknown_result(node, "Index expression result type is not represented.");
    }
    case AST_MEMBER:
    {
        TypecheckResult object = infer(node->as.member.object);
        if (object.status == TYPECHECK_ERROR)
            return object;
        return unknown_result(node, "Member result type is not represented.");
    }
    case AST_CALL:
        if (node->as.call.argument_count > 0 && !node->as.call.arguments)
            return error_result(node, "Malformed builtin argument list.");
        return infer_call(node);
    case AST_MAP:
        if (node->as.map.count > 0 && !node->as.map.entries)
            return error_result(node, "Malformed map entry list.");
        for (i = 0; i < node->as.map.count; i++)
        {
            TypecheckResult key = infer(node->as.map.entries[i].key);
            TypecheckResult value = infer(node->as.map.entries[i].value);
            if (key.status == TYPECHECK_ERROR)
                return key;
            if (value.status == TYPECHECK_ERROR)
                return value;
        }
        return known_result(node, TYPECHECK_MAP);
    case AST_SET:
        if (node->as.set.count > 0 && !node->as.set.items)
            return error_result(node, "Malformed set item list.");
        for (i = 0; i < node->as.set.count; i++)
        {
            TypecheckResult item = infer(node->as.set.items[i]);
            if (item.status == TYPECHECK_ERROR)
                return item;
        }
        return known_result(node, TYPECHECK_SET);
    }
    return error_result(node, "Unsupported AST node kind.");
}

TypecheckResult typecheck_expression(const AstNode *node)
{
    return infer(node);
}

const char *typecheck_type_name(TypecheckType type)
{
    switch (type)
    {
    case TYPECHECK_INT:
        return "Int";
    case TYPECHECK_FLOAT:
        return "Float";
    case TYPECHECK_BOOL:
        return "Bool";
    case TYPECHECK_STRING:
        return "String";
    case TYPECHECK_NULL:
        return "Null";
    case TYPECHECK_ARRAY:
        return "Array";
    case TYPECHECK_MAP:
        return "Map";
    case TYPECHECK_SET:
        return "Set";
    case TYPECHECK_UNKNOWN_TYPE:
        return "Unknown";
    }
    return "Invalid";
}
