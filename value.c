#include "value.h"
#include "runtime.h"

#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VALUE_CONTAINER_MIN_CAPACITY 8
#define VALUE_CONTAINER_MAX_LOAD_NUMERATOR 3
#define VALUE_CONTAINER_MAX_LOAD_DENOMINATOR 4

struct ValueString
{
    size_t length;
    char bytes[];
};

struct ValueArray
{
    size_t length;
    size_t capacity;
    Value *items;
};

struct ValueMatrix
{
    size_t rows;
    size_t columns;
    Value *items;
};

struct ValueQueue
{
    size_t length;
    size_t capacity;
    size_t head;
    Value *items;
};

typedef struct
{
    uint64_t hash;
    Value key;
    Value value;
    unsigned char state;
} ValueMapEntry;

struct ValueMap
{
    size_t length;
    size_t capacity;
    size_t tombstones;
    ValueMapEntry *entries;
};

typedef struct
{
    uint64_t hash;
    Value value;
    unsigned char state;
} ValueSetEntry;

struct ValueSet
{
    size_t length;
    size_t capacity;
    size_t tombstones;
    ValueSetEntry *entries;
};

struct ValueObject
{
    size_t class_id;
    size_t field_count;
    Value *fields;
};

enum
{
    VALUE_ENTRY_EMPTY,
    VALUE_ENTRY_FULL,
    VALUE_ENTRY_TOMBSTONE
};

static void hash_byte(uint64_t *hash, unsigned char byte)
{
    *hash ^= byte;
    *hash *= UINT64_C(1099511628211);
}

void value_trace(const Value *value, GCMarkObject mark, void *mark_context)
{
    if (!value || !mark)
        return;
    if (value->type == VALUE_STRING)
        mark(mark_context, value->as.string);
    else if (value->type == VALUE_ARRAY)
        mark(mark_context, value->as.array);
    else if (value->type == VALUE_MATRIX)
        mark(mark_context, value->as.matrix);
    else if (value->type == VALUE_QUEUE)
        mark(mark_context, value->as.queue);
    else if (value->type == VALUE_MAP)
        mark(mark_context, value->as.map);
    else if (value->type == VALUE_SET)
        mark(mark_context, value->as.set);
    else if (value->type == VALUE_OBJECT)
        mark(mark_context, value->as.object);
}

static void trace_array(void *object, GCMarkObject mark, void *mark_context)
{
    ValueArray *array = object;
    for (size_t i = 0; i < array->length; i++)
        value_trace(&array->items[i], mark, mark_context);
}

static void destroy_array(void *object)
{
    ValueArray *array = object;
    free(array->items);
}

static void trace_matrix(void *object, GCMarkObject mark, void *mark_context)
{
    ValueMatrix *matrix = object;
    size_t count = matrix->rows * matrix->columns;
    for (size_t i = 0; i < count; i++)
        value_trace(&matrix->items[i], mark, mark_context);
}

static void destroy_matrix(void *object)
{
    ValueMatrix *matrix = object;
    free(matrix->items);
}

static void trace_queue(void *object, GCMarkObject mark, void *mark_context)
{
    ValueQueue *queue = object;
    for (size_t i = 0; i < queue->length; i++)
        value_trace(&queue->items[(queue->head + i) % queue->capacity],
                    mark, mark_context);
}

static void destroy_queue(void *object)
{
    ValueQueue *queue = object;
    free(queue->items);
}

static void trace_map(void *object, GCMarkObject mark, void *mark_context)
{
    ValueMap *map = object;
    for (size_t i = 0; i < map->capacity; i++)
    {
        if (map->entries[i].state == VALUE_ENTRY_FULL)
        {
            value_trace(&map->entries[i].key, mark, mark_context);
            value_trace(&map->entries[i].value, mark, mark_context);
        }
    }
}

static void destroy_map(void *object)
{
    ValueMap *map = object;
    free(map->entries);
}

static void trace_set(void *object, GCMarkObject mark, void *mark_context)
{
    ValueSet *set = object;
    for (size_t i = 0; i < set->capacity; i++)
        if (set->entries[i].state == VALUE_ENTRY_FULL)
            value_trace(&set->entries[i].value, mark, mark_context);
}

static void destroy_set(void *object)
{
    ValueSet *set = object;
    free(set->entries);
}

static void trace_object(void *object, GCMarkObject mark, void *mark_context)
{
    ValueObject *instance = object;
    for (size_t i = 0; i < instance->field_count; i++)
        value_trace(&instance->fields[i], mark, mark_context);
}

static void destroy_object(void *object)
{
    ValueObject *instance = object;
    free(instance->fields);
}

static void trace_value_root(void *root, GCMarkObject mark, void *mark_context)
{
    value_trace(root, mark, mark_context);
}

static void set_error(char *error, size_t capacity, const char *message)
{
    if (error && capacity > 0)
        snprintf(error, capacity, "%s", message);
}

Value value_null(void)
{
    Value result;
    result.type = VALUE_NULL;
    result.as.integer = 0;
    return result;
}

Value value_int(int64_t integer)
{
    Value result;
    result.type = VALUE_INT;
    result.as.integer = integer;
    return result;
}

Value value_float(double floating)
{
    Value result;
    result.type = VALUE_FLOAT;
    result.as.floating = floating;
    return result;
}

Value value_bool(bool boolean)
{
    Value result;
    result.type = VALUE_BOOL;
    result.as.boolean = boolean;
    return result;
}

Value value_object(ValueObject *object)
{
    Value result;
    result.type = VALUE_OBJECT;
    result.as.object = object;
    return result;
}

Value value_native(ValueNativeFn native)
{
    Value result;
    result.type = VALUE_NATIVE;
    result.as.native = native;
    return result;
}

bool value_string(RuntimeContext *runtime, const char *bytes, size_t length, Value *result)
{
    if (!runtime_is_initialized(runtime) || !result || (!bytes && length > 0) ||
        length > SIZE_MAX - sizeof(ValueString) - 1)
        return false;

    ValueString *string = gc_allocate(runtime, sizeof(*string) + length + 1, NULL, NULL);
    if (!string)
        return false;
    string->length = length;
    if (length > 0)
        memcpy(string->bytes, bytes, length);
    string->bytes[length] = '\0';

    result->type = VALUE_STRING;
    result->as.string = string;
    return true;
}

bool value_array(RuntimeContext *runtime, Value *result)
{
    if (!runtime_is_initialized(runtime) || !result)
        return false;

    Value *items = malloc(4 * sizeof(*items));
    if (!items)
        return false;
    ValueArray *array = gc_allocate(runtime, sizeof(*array), trace_array, destroy_array);
    if (!array)
    {
        free(items);
        return false;
    }
    memset(array, 0, sizeof(*array));
    array->items = items;
    array->capacity = 4;
    result->type = VALUE_ARRAY;
    result->as.array = array;
    return true;
}

bool value_object_new(RuntimeContext *runtime, size_t class_id,
                      size_t field_count, Value *result)
{
    if (!runtime_is_initialized(runtime) || !result ||
        field_count > SIZE_MAX / sizeof(Value))
        return false;
    Value *fields = field_count ? calloc(field_count, sizeof(*fields)) : NULL;
    if (field_count && !fields)
        return false;
    ValueObject *instance = gc_allocate(runtime, sizeof(*instance),
                                        trace_object, destroy_object);
    if (!instance)
    {
        free(fields);
        return false;
    }
    instance->class_id = class_id;
    instance->field_count = field_count;
    instance->fields = fields;
    result->type = VALUE_OBJECT;
    result->as.object = instance;
    return true;
}

bool value_object_get(const Value *object, size_t field_index, Value *result)
{
    if (!object || object->type != VALUE_OBJECT || !object->as.object ||
        field_index >= object->as.object->field_count || !result)
        return false;
    return value_copy(result, &object->as.object->fields[field_index]);
}

bool value_object_set(Value *object, size_t field_index, const Value *field_value)
{
    if (!object || object->type != VALUE_OBJECT || !object->as.object ||
        field_index >= object->as.object->field_count || !field_value)
        return false;
    return value_copy(&object->as.object->fields[field_index], field_value);
}

size_t value_object_class_id(const Value *object)
{
    return object && object->type == VALUE_OBJECT && object->as.object
               ? object->as.object->class_id
               : SIZE_MAX;
}

size_t value_object_field_count(const Value *object)
{
    return object && object->type == VALUE_OBJECT && object->as.object
               ? object->as.object->field_count
               : 0;
}

const char *value_type_name(ValueType type)
{
    static const char *names[] = {
        "null", "int", "float", "bool", "string", "array", "matrix", "queue",
        "map", "set", "object", "native"};
    return (unsigned)type < sizeof(names) / sizeof(names[0]) ? names[type] : "invalid";
}

bool value_copy(Value *destination, const Value *source)
{
    if (destination == source)
        return destination != NULL;
    if (!destination || !source)
        return false;
    *destination = *source;
    return true;
}

void value_release(Value *value)
{
    if (value)
        *value = value_null();
}

bool value_root_add(RuntimeContext *runtime, Value *value)
{
    return gc_root_add(runtime, value, trace_value_root);
}

bool value_root_remove(RuntimeContext *runtime, Value *value)
{
    return gc_root_remove(runtime, value);
}

bool value_array_push(Value *array_value, const Value *item)
{
    if (!array_value || array_value->type != VALUE_ARRAY || !array_value->as.array || !item)
        return false;

    ValueArray *array = array_value->as.array;
    if (array->length == SIZE_MAX)
        return false;
    Value item_copy = *item;
    if (array->length == array->capacity)
    {
        size_t capacity = array->capacity == 0 ? 8 : array->capacity * 2;
        if (capacity < array->capacity || capacity > SIZE_MAX / sizeof(*array->items))
            return false;
        Value *items = realloc(array->items, capacity * sizeof(*items));
        if (!items)
            return false;
        array->items = items;
        array->capacity = capacity;
    }

    array->items[array->length++] = item_copy;
    return true;
}

bool value_array_pop(Value *array_value, Value *result)
{
    if (!array_value || array_value->type != VALUE_ARRAY || !array_value->as.array ||
        !result || array_value->as.array->length == 0)
        return false;
    ValueArray *array = array_value->as.array;
    *result = array->items[--array->length];
    array->items[array->length] = value_null();
    return true;
}

bool value_array_get(const Value *array_value, size_t index, Value *result)
{
    if (!array_value || array_value->type != VALUE_ARRAY || !array_value->as.array ||
        !result || index >= array_value->as.array->length)
        return false;
    return value_copy(result, &array_value->as.array->items[index]);
}

bool value_array_set(Value *array_value, size_t index, const Value *item)
{
    if (!array_value || array_value->type != VALUE_ARRAY || !array_value->as.array ||
        !item || index >= array_value->as.array->length)
        return false;
    array_value->as.array->items[index] = *item;
    return true;
}

size_t value_array_length(const Value *array_value)
{
    return array_value && array_value->type == VALUE_ARRAY && array_value->as.array
               ? array_value->as.array->length
               : 0;
}

size_t value_array_capacity(const Value *array_value)
{
    return array_value && array_value->type == VALUE_ARRAY && array_value->as.array
               ? array_value->as.array->capacity
               : 0;
}

bool value_matrix(RuntimeContext *runtime, size_t rows, size_t columns, Value *result)
{
    if (!runtime_is_initialized(runtime) || !result || rows == 0 || columns == 0 ||
        rows > SIZE_MAX / columns || rows * columns > SIZE_MAX / sizeof(Value))
        return false;
    ValueMatrix *matrix = gc_allocate(runtime, sizeof(*matrix), trace_matrix,
                                      destroy_matrix);
    if (!matrix)
        return false;
    matrix->rows = rows;
    matrix->columns = columns;
    matrix->items = malloc(rows * columns * sizeof(*matrix->items));
    if (!matrix->items)
        return false;
    for (size_t i = 0; i < rows * columns; i++)
        matrix->items[i] = value_int(0);
    result->type = VALUE_MATRIX;
    result->as.matrix = matrix;
    return true;
}

bool value_matrix_set(Value *matrix_value, size_t row, size_t column,
                      const Value *item)
{
    if (!matrix_value || matrix_value->type != VALUE_MATRIX ||
        !matrix_value->as.matrix || !item ||
        row >= matrix_value->as.matrix->rows ||
        column >= matrix_value->as.matrix->columns)
        return false;
    matrix_value->as.matrix->items[row * matrix_value->as.matrix->columns + column] =
        *item;
    return true;
}

bool value_matrix_get(const Value *matrix_value, size_t row, size_t column,
                      Value *result)
{
    if (!matrix_value || matrix_value->type != VALUE_MATRIX ||
        !matrix_value->as.matrix || !result ||
        row >= matrix_value->as.matrix->rows ||
        column >= matrix_value->as.matrix->columns)
        return false;
    return value_copy(
        result,
        &matrix_value->as.matrix->items[row * matrix_value->as.matrix->columns + column]);
}

size_t value_matrix_rows(const Value *matrix_value)
{
    return matrix_value && matrix_value->type == VALUE_MATRIX &&
                   matrix_value->as.matrix
               ? matrix_value->as.matrix->rows
               : 0;
}

size_t value_matrix_columns(const Value *matrix_value)
{
    return matrix_value && matrix_value->type == VALUE_MATRIX &&
                   matrix_value->as.matrix
               ? matrix_value->as.matrix->columns
               : 0;
}

bool value_queue(RuntimeContext *runtime, Value *result)
{
    if (!runtime_is_initialized(runtime) || !result)
        return false;
    ValueQueue *queue = gc_allocate(runtime, sizeof(*queue), trace_queue,
                                    destroy_queue);
    if (!queue)
        return false;
    memset(queue, 0, sizeof(*queue));
    queue->items = calloc(8, sizeof(*queue->items));
    if (!queue->items)
        return false;
    queue->capacity = 8;
    result->type = VALUE_QUEUE;
    result->as.queue = queue;
    return true;
}

bool value_queue_enqueue(Value *queue_value, const Value *item)
{
    if (!queue_value || queue_value->type != VALUE_QUEUE ||
        !queue_value->as.queue || !item)
        return false;
    ValueQueue *queue = queue_value->as.queue;
    if (queue->length == SIZE_MAX)
        return false;
    if (queue->length == queue->capacity)
    {
        size_t capacity = queue->capacity == 0 ? 8 : queue->capacity * 2;
        if (capacity < queue->capacity || capacity > SIZE_MAX / sizeof(*queue->items))
            return false;
        Value *items = malloc(capacity * sizeof(*items));
        if (!items)
            return false;
        for (size_t i = 0; i < queue->length; i++)
            items[i] = queue->items[(queue->head + i) % queue->capacity];
        free(queue->items);
        queue->items = items;
        queue->capacity = capacity;
        queue->head = 0;
    }
    size_t tail = (queue->head + queue->length) % queue->capacity;
    queue->items[tail] = *item;
    queue->length++;
    return true;
}

bool value_queue_peek(const Value *queue_value, Value *result)
{
    if (!queue_value || queue_value->type != VALUE_QUEUE ||
        !queue_value->as.queue || !result || queue_value->as.queue->length == 0)
        return false;
    return value_copy(result,
                      &queue_value->as.queue->items[queue_value->as.queue->head]);
}

bool value_queue_get(const Value *queue_value, size_t index, Value *result)
{
    if (!queue_value || queue_value->type != VALUE_QUEUE ||
        !queue_value->as.queue || !result ||
        index >= queue_value->as.queue->length)
        return false;
    const ValueQueue *queue = queue_value->as.queue;
    return value_copy(result, &queue->items[(queue->head + index) % queue->capacity]);
}

bool value_queue_dequeue(Value *queue_value, Value *result)
{
    if (!queue_value || queue_value->type != VALUE_QUEUE ||
        !queue_value->as.queue || !result || queue_value->as.queue->length == 0)
        return false;
    ValueQueue *queue = queue_value->as.queue;
    *result = queue->items[queue->head];
    queue->items[queue->head] = value_null();
    queue->head = (queue->head + 1) % queue->capacity;
    queue->length--;
    if (queue->length == 0)
        queue->head = 0;
    return true;
}

size_t value_queue_length(const Value *queue_value)
{
    return queue_value && queue_value->type == VALUE_QUEUE &&
                   queue_value->as.queue
               ? queue_value->as.queue->length
               : 0;
}

size_t value_queue_capacity(const Value *queue_value)
{
    return queue_value && queue_value->type == VALUE_QUEUE &&
                   queue_value->as.queue
               ? queue_value->as.queue->capacity
               : 0;
}

static bool values_equal(const Value *left, const Value *right);

static bool value_key_hash(const Value *value, uint64_t *hash)
{
    if (!value || !hash)
        return false;

    uint64_t result = UINT64_C(14695981039346656037);
    if (value->type == VALUE_NULL)
    {
        *hash = result ^ UINT64_C(0x10);
        return true;
    }
    if (value->type == VALUE_BOOL)
    {
        *hash = result ^ (value->as.boolean ? UINT64_C(0x21) : UINT64_C(0x20));
        return true;
    }
    if (value->type == VALUE_INT || value->type == VALUE_FLOAT)
    {
        double number = value->type == VALUE_INT
                            ? (double)value->as.integer
                            : value->as.floating;
        if (!isfinite(number))
            return false;
        if (number == 0.0)
            number = 0.0;
        int exponent = 0;
        double fraction = frexp(fabs(number), &exponent);
        hash_byte(&result, (unsigned char)(signbit(number) ? 1 : 0));
        unsigned int exponent_bits = (unsigned int)exponent;
        for (size_t i = 0; i < sizeof(exponent_bits); i++)
        {
            hash_byte(&result, (unsigned char)(exponent_bits & 0xffU));
            exponent_bits >>= 8;
        }
        for (int i = 0; i < DBL_MANT_DIG; i++)
        {
            fraction *= 2.0;
            if (fraction >= 1.0)
            {
                hash_byte(&result, 1);
                fraction -= 1.0;
            }
            else
            {
                hash_byte(&result, 0);
            }
        }
        *hash = result ^ UINT64_C(0x30);
        return true;
    }
    if (value->type == VALUE_STRING && value->as.string)
    {
        const unsigned char *bytes = (const unsigned char *)value->as.string->bytes;
        for (size_t i = 0; i < value->as.string->length; i++)
            hash_byte(&result, bytes[i]);
        *hash = result ^ (uint64_t)value->as.string->length ^ UINT64_C(0x40);
        return true;
    }
    return false;
}

static size_t map_find_slot(const ValueMap *map, const Value *key, uint64_t hash,
                            bool *found)
{
    size_t index = (size_t)hash & (map->capacity - 1);
    size_t first_tombstone = SIZE_MAX;
    for (size_t probed = 0; probed < map->capacity; probed++)
    {
        const ValueMapEntry *entry = &map->entries[index];
        if (entry->state == VALUE_ENTRY_EMPTY)
        {
            *found = false;
            return first_tombstone == SIZE_MAX ? index : first_tombstone;
        }
        if (entry->state == VALUE_ENTRY_TOMBSTONE)
        {
            if (first_tombstone == SIZE_MAX)
                first_tombstone = index;
        }
        else if (entry->hash == hash && values_equal(&entry->key, key))
        {
            *found = true;
            return index;
        }
        index = (index + 1) & (map->capacity - 1);
    }
    *found = false;
    return first_tombstone;
}

static size_t set_find_slot(const ValueSet *set, const Value *value, uint64_t hash,
                            bool *found)
{
    size_t index = (size_t)hash & (set->capacity - 1);
    size_t first_tombstone = SIZE_MAX;
    for (size_t probed = 0; probed < set->capacity; probed++)
    {
        const ValueSetEntry *entry = &set->entries[index];
        if (entry->state == VALUE_ENTRY_EMPTY)
        {
            *found = false;
            return first_tombstone == SIZE_MAX ? index : first_tombstone;
        }
        if (entry->state == VALUE_ENTRY_TOMBSTONE)
        {
            if (first_tombstone == SIZE_MAX)
                first_tombstone = index;
        }
        else if (entry->hash == hash && values_equal(&entry->value, value))
        {
            *found = true;
            return index;
        }
        index = (index + 1) & (set->capacity - 1);
    }
    *found = false;
    return first_tombstone;
}

static bool map_rehash(ValueMap *map, size_t capacity)
{
    if (capacity < VALUE_CONTAINER_MIN_CAPACITY ||
        (capacity & (capacity - 1)) != 0 ||
        capacity > SIZE_MAX / sizeof(*map->entries))
        return false;

    ValueMapEntry *entries = calloc(capacity, sizeof(*entries));
    if (!entries)
        return false;
    ValueMap replacement = {0, capacity, 0, entries};
    for (size_t i = 0; i < map->capacity; i++)
    {
        ValueMapEntry *old = &map->entries[i];
        if (old->state != VALUE_ENTRY_FULL)
            continue;
        bool found = false;
        size_t slot = map_find_slot(&replacement, &old->key, old->hash, &found);
        if (slot == SIZE_MAX || found)
        {
            free(entries);
            return false;
        }
        entries[slot] = *old;
        replacement.length++;
    }
    free(map->entries);
    map->entries = entries;
    map->capacity = capacity;
    map->tombstones = 0;
    return true;
}

static bool set_rehash(ValueSet *set, size_t capacity)
{
    if (capacity < VALUE_CONTAINER_MIN_CAPACITY ||
        (capacity & (capacity - 1)) != 0 ||
        capacity > SIZE_MAX / sizeof(*set->entries))
        return false;

    ValueSetEntry *entries = calloc(capacity, sizeof(*entries));
    if (!entries)
        return false;
    ValueSet replacement = {0, capacity, 0, entries};
    for (size_t i = 0; i < set->capacity; i++)
    {
        ValueSetEntry *old = &set->entries[i];
        if (old->state != VALUE_ENTRY_FULL)
            continue;
        bool found = false;
        size_t slot = set_find_slot(&replacement, &old->value, old->hash, &found);
        if (slot == SIZE_MAX || found)
        {
            free(entries);
            return false;
        }
        entries[slot] = *old;
        replacement.length++;
    }
    free(set->entries);
    set->entries = entries;
    set->capacity = capacity;
    set->tombstones = 0;
    return true;
}

static bool map_prepare_insert(ValueMap *map)
{
    size_t needed;
    if (map->length == SIZE_MAX)
        return false;
    needed = map->length + 1;
    size_t capacity = map->capacity;
    if (capacity == 0)
        return map_rehash(map, VALUE_CONTAINER_MIN_CAPACITY);

    size_t maximum = capacity - capacity / VALUE_CONTAINER_MAX_LOAD_DENOMINATOR;
    if (needed > maximum)
    {
        if (capacity > SIZE_MAX / 2)
            return false;
        capacity *= 2;
        while (needed > capacity - capacity / VALUE_CONTAINER_MAX_LOAD_DENOMINATOR)
        {
            if (capacity > SIZE_MAX / 2)
                return false;
            capacity *= 2;
        }
        return map_rehash(map, capacity);
    }
    if (map->tombstones > capacity / VALUE_CONTAINER_MAX_LOAD_DENOMINATOR)
        return map_rehash(map, capacity);
    return true;
}

static bool set_prepare_insert(ValueSet *set)
{
    size_t needed;
    if (set->length == SIZE_MAX)
        return false;
    needed = set->length + 1;
    size_t capacity = set->capacity;
    if (capacity == 0)
        return set_rehash(set, VALUE_CONTAINER_MIN_CAPACITY);

    size_t maximum = capacity - capacity / VALUE_CONTAINER_MAX_LOAD_DENOMINATOR;
    if (needed > maximum)
    {
        if (capacity > SIZE_MAX / 2)
            return false;
        capacity *= 2;
        while (needed > capacity - capacity / VALUE_CONTAINER_MAX_LOAD_DENOMINATOR)
        {
            if (capacity > SIZE_MAX / 2)
                return false;
            capacity *= 2;
        }
        return set_rehash(set, capacity);
    }
    if (set->tombstones > capacity / VALUE_CONTAINER_MAX_LOAD_DENOMINATOR)
        return set_rehash(set, capacity);
    return true;
}

bool value_map(RuntimeContext *runtime, Value *result)
{
    if (!runtime_is_initialized(runtime) || !result)
        return false;
    ValueMap *map = gc_allocate(runtime, sizeof(*map), trace_map, destroy_map);
    if (!map)
        return false;
    memset(map, 0, sizeof(*map));
    result->type = VALUE_MAP;
    result->as.map = map;
    return true;
}

bool value_map_set(Value *map_value, const Value *key, const Value *value)
{
    if (!map_value || map_value->type != VALUE_MAP || !map_value->as.map ||
        !key || !value)
        return false;
    uint64_t hash;
    if (!value_key_hash(key, &hash))
        return false;
    ValueMap *map = map_value->as.map;
    bool found = false;
    size_t slot = map->capacity == 0 ? SIZE_MAX : map_find_slot(map, key, hash, &found);
    if (found)
    {
        map->entries[slot].value = *value;
        return true;
    }
    if (!map_prepare_insert(map))
        return false;
    slot = map_find_slot(map, key, hash, &found);
    if (slot == SIZE_MAX || found)
        return false;
    if (map->entries[slot].state == VALUE_ENTRY_TOMBSTONE)
        map->tombstones--;
    map->entries[slot].hash = hash;
    map->entries[slot].key = *key;
    map->entries[slot].value = *value;
    map->entries[slot].state = VALUE_ENTRY_FULL;
    map->length++;
    return true;
}

bool value_map_get(const Value *map_value, const Value *key, Value *result)
{
    if (!map_value || map_value->type != VALUE_MAP || !map_value->as.map ||
        !key || !result || map_value->as.map->capacity == 0)
        return false;
    uint64_t hash;
    if (!value_key_hash(key, &hash))
        return false;
    bool found = false;
    size_t slot = map_find_slot(map_value->as.map, key, hash, &found);
    if (!found)
        return false;
    return value_copy(result, &map_value->as.map->entries[slot].value);
}

bool value_map_contains(const Value *map_value, const Value *key, bool *contains)
{
    if (!map_value || map_value->type != VALUE_MAP || !map_value->as.map ||
        !key || !contains)
        return false;
    uint64_t hash;
    if (!value_key_hash(key, &hash))
        return false;
    if (map_value->as.map->capacity == 0)
    {
        *contains = false;
        return true;
    }
    bool found = false;
    (void)map_find_slot(map_value->as.map, key, hash, &found);
    *contains = found;
    return true;
}

bool value_map_remove(Value *map_value, const Value *key, bool *removed)
{
    if (!map_value || map_value->type != VALUE_MAP || !map_value->as.map ||
        !key || !removed)
        return false;
    uint64_t hash;
    if (!value_key_hash(key, &hash))
        return false;
    ValueMap *map = map_value->as.map;
    *removed = false;
    if (map->capacity == 0)
        return true;
    bool found = false;
    size_t slot = map_find_slot(map, key, hash, &found);
    if (!found)
        return true;
    map->entries[slot].key = value_null();
    map->entries[slot].value = value_null();
    map->entries[slot].state = VALUE_ENTRY_TOMBSTONE;
    map->length--;
    map->tombstones++;
    *removed = true;
    return true;
}

size_t value_map_length(const Value *map_value)
{
    return map_value && map_value->type == VALUE_MAP && map_value->as.map
               ? map_value->as.map->length
               : 0;
}

bool value_set(RuntimeContext *runtime, Value *result)
{
    if (!runtime_is_initialized(runtime) || !result)
        return false;
    ValueSet *set = gc_allocate(runtime, sizeof(*set), trace_set, destroy_set);
    if (!set)
        return false;
    memset(set, 0, sizeof(*set));
    result->type = VALUE_SET;
    result->as.set = set;
    return true;
}

bool value_set_add(Value *set_value, const Value *value, bool *added)
{
    if (!set_value || set_value->type != VALUE_SET || !set_value->as.set ||
        !value || !added)
        return false;
    uint64_t hash;
    if (!value_key_hash(value, &hash))
        return false;
    ValueSet *set = set_value->as.set;
    bool found = false;
    size_t slot = set->capacity == 0 ? SIZE_MAX : set_find_slot(set, value, hash, &found);
    if (found)
    {
        *added = false;
        return true;
    }
    if (!set_prepare_insert(set))
        return false;
    slot = set_find_slot(set, value, hash, &found);
    if (slot == SIZE_MAX || found)
        return false;
    if (set->entries[slot].state == VALUE_ENTRY_TOMBSTONE)
        set->tombstones--;
    set->entries[slot].hash = hash;
    set->entries[slot].value = *value;
    set->entries[slot].state = VALUE_ENTRY_FULL;
    set->length++;
    *added = true;
    return true;
}

bool value_set_contains(const Value *set_value, const Value *value, bool *contains)
{
    if (!set_value || set_value->type != VALUE_SET || !set_value->as.set ||
        !value || !contains)
        return false;
    uint64_t hash;
    if (!value_key_hash(value, &hash))
        return false;
    if (set_value->as.set->capacity == 0)
    {
        *contains = false;
        return true;
    }
    bool found = false;
    (void)set_find_slot(set_value->as.set, value, hash, &found);
    *contains = found;
    return true;
}

bool value_set_remove(Value *set_value, const Value *value, bool *removed)
{
    if (!set_value || set_value->type != VALUE_SET || !set_value->as.set ||
        !value || !removed)
        return false;
    uint64_t hash;
    if (!value_key_hash(value, &hash))
        return false;
    ValueSet *set = set_value->as.set;
    *removed = false;
    if (set->capacity == 0)
        return true;
    bool found = false;
    size_t slot = set_find_slot(set, value, hash, &found);
    if (!found)
        return true;
    set->entries[slot].value = value_null();
    set->entries[slot].state = VALUE_ENTRY_TOMBSTONE;
    set->length--;
    set->tombstones++;
    *removed = true;
    return true;
}

size_t value_set_length(const Value *set_value)
{
    return set_value && set_value->type == VALUE_SET && set_value->as.set
               ? set_value->as.set->length
               : 0;
}

const char *value_string_bytes(const Value *string_value, size_t *length)
{
    if (!string_value || string_value->type != VALUE_STRING || !string_value->as.string)
        return NULL;
    if (length)
        *length = string_value->as.string->length;
    return string_value->as.string->bytes;
}

bool value_truthy(const Value *value)
{
    if (!value)
        return false;

    switch (value->type)
    {
    case VALUE_NULL:
        return false;
    case VALUE_BOOL:
        return value->as.boolean;
    case VALUE_INT:
        return value->as.integer != 0;
    case VALUE_FLOAT:
        return value->as.floating != 0.0 && !isnan(value->as.floating);
    case VALUE_STRING:
        return value->as.string && value->as.string->length > 0;
    case VALUE_ARRAY:
        return value->as.array && value->as.array->length > 0;
    case VALUE_MATRIX:
        return value->as.matrix && value->as.matrix->rows > 0 &&
               value->as.matrix->columns > 0;
    case VALUE_QUEUE:
        return value->as.queue && value->as.queue->length > 0;
    case VALUE_MAP:
        return value->as.map && value->as.map->length > 0;
    case VALUE_SET:
        return value->as.set && value->as.set->length > 0;
    case VALUE_OBJECT:
        return value->as.object != NULL;
    case VALUE_NATIVE:
        return value->as.native != NULL;
    }
    return false;
}

static bool number_as_double(const Value *value, double *result)
{
    if (value->type == VALUE_INT)
        *result = (double)value->as.integer;
    else if (value->type == VALUE_FLOAT)
        *result = value->as.floating;
    else
        return false;
    return true;
}

static bool format_value(const Value *value, char *buffer, size_t capacity)
{
    int written = -1;
    switch (value->type)
    {
    case VALUE_NULL:
        written = snprintf(buffer, capacity, "null");
        break;
    case VALUE_INT:
        written = snprintf(buffer, capacity, "%" PRId64, value->as.integer);
        break;
    case VALUE_FLOAT:
        written = snprintf(buffer, capacity, "%.17g", value->as.floating);
        break;
    case VALUE_BOOL:
        written = snprintf(buffer, capacity, "%s", value->as.boolean ? "true" : "false");
        break;
    case VALUE_STRING:
        if (!value->as.string)
            return false;
        if (value->as.string->length >= capacity)
            return false;
        memcpy(buffer, value->as.string->bytes, value->as.string->length + 1);
        return true;
    case VALUE_ARRAY:
        written = snprintf(buffer, capacity, "[array]");
        break;
    case VALUE_MATRIX:
        written = snprintf(buffer, capacity, "[matrix]");
        break;
    case VALUE_QUEUE:
        written = snprintf(buffer, capacity, "[queue]");
        break;
    case VALUE_MAP:
        written = snprintf(buffer, capacity, "[map]");
        break;
    case VALUE_SET:
        written = snprintf(buffer, capacity, "[set]");
        break;
    case VALUE_OBJECT:
        written = snprintf(buffer, capacity, "[object]");
        break;
    case VALUE_NATIVE:
        written = snprintf(buffer, capacity, "[native]");
        break;
    }
    return written >= 0 && (size_t)written < capacity;
}

bool value_cast(RuntimeContext *runtime, const Value *source, ValueType target, Value *result,
                char *error, size_t error_capacity)
{
    if (!source || !result)
    {
        set_error(error, error_capacity, "Invalid value cast.");
        return false;
    }
    if (source->type == target)
    {
        if (!value_copy(result, source))
        {
            set_error(error, error_capacity, "Unable to copy value.");
            return false;
        }
        return true;
    }

    if (target == VALUE_BOOL)
    {
        *result = value_bool(value_truthy(source));
        return true;
    }
    if (target == VALUE_INT)
    {
        if (source->type == VALUE_FLOAT && isfinite(source->as.floating) &&
            source->as.floating >= (double)INT64_MIN &&
            source->as.floating < -(double)INT64_MIN)
        {
            *result = value_int((int64_t)source->as.floating);
            return true;
        }
        if (source->type == VALUE_BOOL)
        {
            *result = value_int(source->as.boolean ? 1 : 0);
            return true;
        }
        if (source->type == VALUE_STRING)
        {
            char *end = NULL;
            const char *text = source->as.string->bytes;
            errno = 0;
            long long parsed = strtoll(text, &end, 10);
            if (errno != ERANGE && end != text && *end == '\0' &&
                strlen(text) == source->as.string->length)
            {
                *result = value_int((int64_t)parsed);
                return true;
            }
        }
        else if (source->type == VALUE_INT)
        {
            *result = value_int(source->as.integer);
            return true;
        }
        set_error(error, error_capacity, "Value cannot be converted to an integer.");
        return false;
    }
    if (target == VALUE_FLOAT)
    {
        double number;
        if (number_as_double(source, &number))
        {
            *result = value_float(number);
            return true;
        }
        if (source->type == VALUE_BOOL)
        {
            *result = value_float(source->as.boolean ? 1.0 : 0.0);
            return true;
        }
        if (source->type == VALUE_STRING)
        {
            char *end = NULL;
            errno = 0;
            double parsed = strtod(source->as.string->bytes, &end);
            if (errno != ERANGE && end != source->as.string->bytes &&
                *end == '\0' && strlen(source->as.string->bytes) == source->as.string->length &&
                isfinite(parsed))
            {
                *result = value_float(parsed);
                return true;
            }
        }
        set_error(error, error_capacity, "Value cannot be converted to a float.");
        return false;
    }
    if (target == VALUE_STRING)
    {
        char buffer[128];
        if (!format_value(source, buffer, sizeof(buffer)) ||
            !value_string(runtime, buffer, strlen(buffer), result))
        {
            set_error(error, error_capacity, "Unable to convert value to a string.");
            return false;
        }
        return true;
    }
    if (target == VALUE_NULL && source->type == VALUE_NULL)
    {
        *result = value_null();
        return true;
    }

    set_error(error, error_capacity, "Value cannot be converted to the requested type.");
    return false;
}

static bool append_strings(RuntimeContext *runtime, const Value *left,
                           const Value *right, Value *result)
{
    char left_buffer[128];
    char right_buffer[128];
    const char *left_bytes;
    const char *right_bytes;
    size_t left_length;
    size_t right_length;

    if (left->type == VALUE_STRING)
    {
        left_bytes = left->as.string->bytes;
        left_length = left->as.string->length;
    }
    else
    {
        if (!format_value(left, left_buffer, sizeof(left_buffer)))
            return false;
        left_bytes = left_buffer;
        left_length = strlen(left_buffer);
    }
    if (right->type == VALUE_STRING)
    {
        right_bytes = right->as.string->bytes;
        right_length = right->as.string->length;
    }
    else
    {
        if (!format_value(right, right_buffer, sizeof(right_buffer)))
            return false;
        right_bytes = right_buffer;
        right_length = strlen(right_buffer);
    }

    if (left_length > SIZE_MAX - right_length)
        return false;
    size_t length = left_length + right_length;
    char *bytes = malloc(length == 0 ? 1 : length);
    if (!bytes)
        return false;
    memcpy(bytes, left_bytes, left_length);
    memcpy(bytes + left_length, right_bytes, right_length);
    bool success = value_string(runtime, bytes, length, result);
    free(bytes);
    return success;
}

static bool values_equal(const Value *left, const Value *right)
{
    double left_number;
    double right_number;
    if (number_as_double(left, &left_number) && number_as_double(right, &right_number))
        return left_number == right_number;
    if (left->type != right->type)
        return false;
    switch (left->type)
    {
    case VALUE_NULL:
        return true;
    case VALUE_BOOL:
        return left->as.boolean == right->as.boolean;
    case VALUE_STRING:
        return left->as.string->length == right->as.string->length &&
               memcmp(left->as.string->bytes, right->as.string->bytes,
                      left->as.string->length) == 0;
    case VALUE_ARRAY:
        return left->as.array == right->as.array;
    case VALUE_MATRIX:
        return left->as.matrix == right->as.matrix;
    case VALUE_QUEUE:
        return left->as.queue == right->as.queue;
    case VALUE_MAP:
        return left->as.map == right->as.map;
    case VALUE_SET:
        return left->as.set == right->as.set;
    case VALUE_OBJECT:
        return left->as.object == right->as.object;
    case VALUE_NATIVE:
        return left->as.native == right->as.native;
    case VALUE_INT:
        return left->as.integer == right->as.integer;
    case VALUE_FLOAT:
        return left->as.floating == right->as.floating;
    }
    return false;
}

static bool checked_integer(ValueBinaryOp operation, int64_t left, int64_t right,
                            Value *result, char *error, size_t error_capacity)
{
    int64_t value;
    switch (operation)
    {
    case VALUE_OP_ADD:
        if ((right > 0 && left > INT64_MAX - right) ||
            (right < 0 && left < INT64_MIN - right))
            goto overflow;
        value = left + right;
        break;
    case VALUE_OP_SUBTRACT:
        if ((right < 0 && left > INT64_MAX + right) ||
            (right > 0 && left < INT64_MIN + right))
            goto overflow;
        value = left - right;
        break;
    case VALUE_OP_MULTIPLY:
        if (left == 0 || right == 0)
        {
            value = 0;
            break;
        }
        if ((left == -1 && right == INT64_MIN) || (right == -1 && left == INT64_MIN))
            goto overflow;
        if (left > 0 ? (right > 0 ? left > INT64_MAX / right : right < INT64_MIN / left)
                     : (right > 0 ? left < INT64_MIN / right : left < INT64_MAX / right))
            goto overflow;
        value = left * right;
        break;
    case VALUE_OP_DIVIDE:
        if (right == 0)
        {
            set_error(error, error_capacity, "Division by zero.");
            return false;
        }
        if (left == INT64_MIN && right == -1)
            goto overflow;
        value = left / right;
        break;
    case VALUE_OP_MODULO:
        if (right == 0)
        {
            set_error(error, error_capacity, "Modulo by zero.");
            return false;
        }
        value = left == INT64_MIN && right == -1 ? 0 : left % right;
        break;
    default:
        return false;
    }
    *result = value_int(value);
    return true;

overflow:
    set_error(error, error_capacity, "Integer arithmetic overflow.");
    return false;
}

bool value_binary(RuntimeContext *runtime, ValueBinaryOp operation,
                  const Value *left, const Value *right,
                  Value *result, char *error, size_t error_capacity)
{
    if (!left || !right || !result)
    {
        set_error(error, error_capacity, "Invalid operands.");
        return false;
    }

    if (operation == VALUE_OP_EQUAL || operation == VALUE_OP_NOT_EQUAL)
    {
        bool equal = values_equal(left, right);
        *result = value_bool(operation == VALUE_OP_EQUAL ? equal : !equal);
        return true;
    }
    if (operation == VALUE_OP_ADD &&
        (left->type == VALUE_STRING || right->type == VALUE_STRING))
    {
        if (left->type == VALUE_STRING && !left->as.string)
        {
            set_error(error, error_capacity, "Invalid string operand.");
            return false;
        }
        if (right->type == VALUE_STRING && !right->as.string)
        {
            set_error(error, error_capacity, "Invalid string operand.");
            return false;
        }
        if (!append_strings(runtime, left, right, result))
            set_error(error, error_capacity, "Unable to concatenate strings.");
        else
            return true;
        return false;
    }

    if (operation == VALUE_OP_LESS || operation == VALUE_OP_LESS_EQUAL ||
        operation == VALUE_OP_GREATER || operation == VALUE_OP_GREATER_EQUAL)
    {
        bool answer;
        if (left->type == VALUE_STRING && right->type == VALUE_STRING)
        {
            size_t common_length = left->as.string->length < right->as.string->length
                                       ? left->as.string->length
                                       : right->as.string->length;
            int comparison = memcmp(left->as.string->bytes, right->as.string->bytes,
                                    common_length);
            if (comparison == 0)
                comparison = left->as.string->length < right->as.string->length ? -1
                             : left->as.string->length > right->as.string->length ? 1
                                                                                  : 0;
            answer = operation == VALUE_OP_LESS ? comparison < 0
                     : operation == VALUE_OP_LESS_EQUAL ? comparison <= 0
                     : operation == VALUE_OP_GREATER ? comparison > 0
                                                     : comparison >= 0;
        }
        else
        {
            double left_number;
            double right_number;
            if (!number_as_double(left, &left_number) || !number_as_double(right, &right_number))
            {
                set_error(error, error_capacity, "Comparison requires two numbers or two strings.");
                return false;
            }
            answer = operation == VALUE_OP_LESS ? left_number < right_number
                     : operation == VALUE_OP_LESS_EQUAL ? left_number <= right_number
                     : operation == VALUE_OP_GREATER ? left_number > right_number
                                                     : left_number >= right_number;
        }
        *result = value_bool(answer);
        return true;
    }

    if (left->type == VALUE_INT && right->type == VALUE_INT &&
        operation <= VALUE_OP_MODULO)
        return checked_integer(operation, left->as.integer, right->as.integer,
                               result, error, error_capacity);

    double left_number;
    double right_number;
    if (!number_as_double(left, &left_number) || !number_as_double(right, &right_number))
    {
        set_error(error, error_capacity, "Arithmetic requires numeric operands.");
        return false;
    }
    if (operation == VALUE_OP_DIVIDE && right_number == 0.0)
    {
        set_error(error, error_capacity, "Division by zero.");
        return false;
    }
    if (operation == VALUE_OP_MODULO)
    {
        set_error(error, error_capacity, "Modulo requires integer operands.");
        return false;
    }

    double answer;
    switch (operation)
    {
    case VALUE_OP_ADD:
        answer = left_number + right_number;
        break;
    case VALUE_OP_SUBTRACT:
        answer = left_number - right_number;
        break;
    case VALUE_OP_MULTIPLY:
        answer = left_number * right_number;
        break;
    case VALUE_OP_DIVIDE:
        answer = left_number / right_number;
        break;
    default:
        set_error(error, error_capacity, "Unsupported arithmetic operation.");
        return false;
    }
    if (!isfinite(answer))
    {
        set_error(error, error_capacity, "Floating-point arithmetic produced a non-finite result.");
        return false;
    }
    *result = value_float(answer);
    return true;
}
