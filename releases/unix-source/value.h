#ifndef VALUE_H
#define VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gc.h"

typedef enum
{
    VALUE_NULL,
    VALUE_INT,
    VALUE_FLOAT,
    VALUE_BOOL,
    VALUE_STRING,
    VALUE_ARRAY,
    VALUE_MATRIX,
    VALUE_QUEUE,
    VALUE_MAP,
    VALUE_SET,
    VALUE_OBJECT,
    VALUE_NATIVE
} ValueType;

typedef struct Value Value;
typedef struct ValueString ValueString;
typedef struct ValueArray ValueArray;
typedef struct ValueMatrix ValueMatrix;
typedef struct ValueQueue ValueQueue;
typedef struct ValueMap ValueMap;
typedef struct ValueSet ValueSet;
typedef struct ValueObject ValueObject;

typedef bool (*ValueNativeFn)(const Value *arguments, size_t argument_count,
                              Value *result, char *error, size_t error_capacity);

/* Managed values belong to their RuntimeContext. Handles are non-owning; register
 * live roots with that same context before collecting it. */
struct Value
{
    ValueType type;
    union
    {
        int64_t integer;
        double floating;
        bool boolean;
        ValueString *string;
        ValueArray *array;
        ValueMatrix *matrix;
        ValueQueue *queue;
        ValueMap *map;
        ValueSet *set;
        ValueObject *object;
        ValueNativeFn native;
    } as;
};

typedef enum
{
    VALUE_OP_ADD,
    VALUE_OP_SUBTRACT,
    VALUE_OP_MULTIPLY,
    VALUE_OP_DIVIDE,
    VALUE_OP_MODULO,
    VALUE_OP_EQUAL,
    VALUE_OP_NOT_EQUAL,
    VALUE_OP_LESS,
    VALUE_OP_LESS_EQUAL,
    VALUE_OP_GREATER,
    VALUE_OP_GREATER_EQUAL
} ValueBinaryOp;

Value value_null(void);
Value value_int(int64_t integer);
Value value_float(double floating);
Value value_bool(bool boolean);
Value value_object(ValueObject *object);
Value value_native(ValueNativeFn native);

bool value_string(RuntimeContext *runtime, const char *bytes, size_t length, Value *result);
bool value_array(RuntimeContext *runtime, Value *result);
bool value_object_new(RuntimeContext *runtime, size_t class_id,
                     size_t field_count, Value *result);
bool value_object_get(const Value *object, size_t field_index, Value *result);
bool value_object_set(Value *object, size_t field_index, const Value *field_value);
size_t value_object_class_id(const Value *object);
size_t value_object_field_count(const Value *object);
bool value_array_push(Value *array, const Value *item);
bool value_array_pop(Value *array, Value *result);
bool value_array_get(const Value *array, size_t index, Value *result);
bool value_array_set(Value *array, size_t index, const Value *item);
size_t value_array_length(const Value *array);
size_t value_array_capacity(const Value *array);
bool value_matrix(RuntimeContext *runtime, size_t rows, size_t columns, Value *result);
bool value_matrix_set(Value *matrix, size_t row, size_t column, const Value *item);
bool value_matrix_get(const Value *matrix, size_t row, size_t column, Value *result);
size_t value_matrix_rows(const Value *matrix);
size_t value_matrix_columns(const Value *matrix);
bool value_queue(RuntimeContext *runtime, Value *result);
bool value_queue_enqueue(Value *queue, const Value *item);
bool value_queue_peek(const Value *queue, Value *result);
bool value_queue_dequeue(Value *queue, Value *result);
bool value_queue_get(const Value *queue, size_t index, Value *result);
size_t value_queue_length(const Value *queue);
size_t value_queue_capacity(const Value *queue);
/* Map/set keys are null, bool, finite numbers, or strings; integer/float keys
 * compare using the same numeric equality as value_binary(VALUE_OP_EQUAL). */
bool value_map(RuntimeContext *runtime, Value *result);
bool value_map_set(Value *map, const Value *key, const Value *value);
bool value_map_get(const Value *map, const Value *key, Value *result);
bool value_map_contains(const Value *map, const Value *key, bool *contains);
bool value_map_remove(Value *map, const Value *key, bool *removed);
size_t value_map_length(const Value *map);
bool value_set(RuntimeContext *runtime, Value *result);
bool value_set_add(Value *set, const Value *value, bool *added);
bool value_set_contains(const Value *set, const Value *value, bool *contains);
bool value_set_remove(Value *set, const Value *value, bool *removed);
size_t value_set_length(const Value *set);
const char *value_string_bytes(const Value *string, size_t *length);
const char *value_type_name(ValueType type);

bool value_copy(Value *destination, const Value *source);
void value_release(Value *value);
bool value_root_add(RuntimeContext *runtime, Value *value);
bool value_root_remove(RuntimeContext *runtime, Value *value);
void value_trace(const Value *value, GCMarkObject mark, void *mark_context);
bool value_truthy(const Value *value);
bool value_cast(RuntimeContext *runtime, const Value *source, ValueType target, Value *result,
                char *error, size_t error_capacity);
bool value_binary(RuntimeContext *runtime, ValueBinaryOp operation,
                  const Value *left, const Value *right,
                  Value *result, char *error, size_t error_capacity);

#endif
