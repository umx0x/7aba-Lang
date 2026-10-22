#include "stdlib.h"
#include "runtime.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct StdFile
{
    FILE *stream;
    bool readable;
    bool writable;
};

static void set_error(char *error, size_t capacity, const char *message)
{
    if (error && capacity > 0)
        snprintf(error, capacity, "%s", message);
}

static bool valid_file(const StdFile *file)
{
    return file && file->stream;
}

StdFile *stdlib_file_open(const char *path, StdFileMode mode)
{
    if (!path || !*path)
        return NULL;
    const char *mode_string;
    bool readable = false;
    bool writable = false;
    switch (mode)
    {
    case STD_FILE_READ:
        mode_string = "r";
        readable = true;
        break;
    case STD_FILE_WRITE:
        mode_string = "w";
        writable = true;
        break;
    case STD_FILE_APPEND:
        mode_string = "a";
        writable = true;
        break;
    case STD_FILE_READ_BINARY:
        mode_string = "rb";
        readable = true;
        break;
    case STD_FILE_WRITE_BINARY:
        mode_string = "wb";
        writable = true;
        break;
    case STD_FILE_APPEND_BINARY:
        mode_string = "ab";
        writable = true;
        break;
    default:
        return NULL;
    }

    FILE *stream = fopen(path, mode_string);
    if (!stream)
        return NULL;
    StdFile *file = malloc(sizeof(*file));
    if (!file)
    {
        fclose(stream);
        return NULL;
    }
    file->stream = stream;
    file->readable = readable;
    file->writable = writable;
    return file;
}

bool stdlib_file_close(StdFile *file)
{
    if (!valid_file(file))
        return false;
    FILE *stream = file->stream;
    file->stream = NULL;
    free(file);
    return fclose(stream) == 0;
}

bool stdlib_file_read(StdFile *file, void *buffer, size_t capacity, size_t *bytes_read)
{
    if (!valid_file(file) || !file->readable || (!buffer && capacity > 0) || !bytes_read)
        return false;
    *bytes_read = fread(buffer, 1, capacity, file->stream);
    return !ferror(file->stream);
}

bool stdlib_file_read_all(StdFile *file, unsigned char **bytes, size_t *length)
{
    if (!valid_file(file) || !file->readable || !bytes || !length)
        return false;
    *bytes = NULL;
    *length = 0;
    size_t capacity = 0;
    unsigned char *buffer = NULL;
    unsigned char chunk[8192];
    for (;;)
    {
        size_t count = fread(chunk, 1, sizeof(chunk), file->stream);
        if (count > 0)
        {
            if (*length > SIZE_MAX - count)
            {
                free(buffer);
                *length = 0;
                return false;
            }
            size_t required = *length + count;
            if (required > capacity)
            {
                size_t next_capacity = capacity == 0 ? sizeof(chunk) : capacity;
                while (next_capacity < required)
                {
                    if (next_capacity > SIZE_MAX / 2)
                    {
                        next_capacity = required;
                        break;
                    }
                    next_capacity *= 2;
                }
                unsigned char *next = realloc(buffer, next_capacity);
                if (!next)
                {
                    free(buffer);
                    *length = 0;
                    return false;
                }
                buffer = next;
                capacity = next_capacity;
            }
            memcpy(buffer + *length, chunk, count);
            *length += count;
        }
        if (count < sizeof(chunk))
        {
            if (ferror(file->stream))
            {
                free(buffer);
                *length = 0;
                return false;
            }
            break;
        }
    }
    *bytes = buffer;
    return true;
}

bool stdlib_file_write(StdFile *file, const void *bytes, size_t length,
                       size_t *bytes_written)
{
    if (!valid_file(file) || !file->writable || (!bytes && length > 0) || !bytes_written)
        return false;
    *bytes_written = fwrite(bytes, 1, length, file->stream);
    return *bytes_written == length && !ferror(file->stream);
}

bool stdlib_file_seek(StdFile *file, long offset, int origin)
{
    if (!valid_file(file) ||
        (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END))
        return false;
    return fseek(file->stream, offset, origin) == 0;
}

bool stdlib_file_flush(StdFile *file)
{
    return valid_file(file) && file->writable && fflush(file->stream) == 0;
}

static bool callback_value(ValueNativeFn callback, const Value *item, Value *result,
                           char *error, size_t error_capacity)
{
    if (!callback || !callback(item, 1, result, error, error_capacity))
    {
        if (error && error_capacity > 0 && error[0] == '\0')
            set_error(error, error_capacity, "Array callback failed.");
        return false;
    }
    return true;
}

bool stdlib_array_map(RuntimeContext *runtime, const Value *array,
                      ValueNativeFn callback, Value *result,
                      char *error, size_t error_capacity)
{
    if (!array || array->type != VALUE_ARRAY || !callback || !result)
    {
        set_error(error, error_capacity, "map expects an array and a callback.");
        return false;
    }
    if (!value_array(runtime, result))
    {
        set_error(error, error_capacity, "Unable to allocate mapped array.");
        return false;
    }
    for (size_t i = 0; i < value_array_length(array); i++)
    {
        Value item = value_null();
        Value mapped = value_null();
        if (!value_array_get(array, i, &item) ||
            !callback_value(callback, &item, &mapped, error, error_capacity) ||
            !value_array_push(result, &mapped))
        {
            value_release(&item);
            value_release(&mapped);
            value_release(result);
            if (!error || !error_capacity || !error[0])
                set_error(error, error_capacity, "Unable to append mapped value.");
            return false;
        }
        value_release(&item);
        value_release(&mapped);
    }
    return true;
}

bool stdlib_array_filter(RuntimeContext *runtime, const Value *array,
                         ValueNativeFn predicate, Value *result,
                         char *error, size_t error_capacity)
{
    if (!array || array->type != VALUE_ARRAY || !predicate || !result)
    {
        set_error(error, error_capacity, "filter expects an array and a predicate.");
        return false;
    }
    if (!value_array(runtime, result))
    {
        set_error(error, error_capacity, "Unable to allocate filtered array.");
        return false;
    }
    for (size_t i = 0; i < value_array_length(array); i++)
    {
        Value item = value_null();
        Value keep = value_null();
        if (!value_array_get(array, i, &item) ||
            !callback_value(predicate, &item, &keep, error, error_capacity) ||
            (value_truthy(&keep) && !value_array_push(result, &item)))
        {
            value_release(&item);
            value_release(&keep);
            value_release(result);
            if (!error || !error_capacity || !error[0])
                set_error(error, error_capacity, "Unable to append filtered value.");
            return false;
        }
        value_release(&item);
        value_release(&keep);
    }
    return true;
}

static bool compare_values(const Value *left, const Value *right, int *comparison)
{
    if (left->type == VALUE_INT && right->type == VALUE_INT)
    {
        *comparison = (left->as.integer > right->as.integer) -
                      (left->as.integer < right->as.integer);
        return true;
    }
    if ((left->type == VALUE_INT || left->type == VALUE_FLOAT) &&
        (right->type == VALUE_INT || right->type == VALUE_FLOAT))
    {
        double l = left->type == VALUE_INT ? (double)left->as.integer : left->as.floating;
        double r = right->type == VALUE_INT ? (double)right->as.integer : right->as.floating;
        *comparison = (l > r) - (l < r);
        return true;
    }
    if (left->type == VALUE_STRING && right->type == VALUE_STRING)
    {
        size_t left_length;
        size_t right_length;
        const char *l = value_string_bytes(left, &left_length);
        const char *r = value_string_bytes(right, &right_length);
        size_t common = left_length < right_length ? left_length : right_length;
        int result = memcmp(l, r, common);
        if (result == 0)
            result = (left_length > right_length) - (left_length < right_length);
        *comparison = (result > 0) - (result < 0);
        return true;
    }
    return false;
}

bool stdlib_array_sort(RuntimeContext *runtime, const Value *array,
                       StdArrayOrder order, Value *result,
                       char *error, size_t error_capacity)
{
    if (!array || array->type != VALUE_ARRAY || !result ||
        (order != STD_ARRAY_ASCENDING && order != STD_ARRAY_DESCENDING))
    {
        set_error(error, error_capacity, "sort expects an array and a valid order.");
        return false;
    }
    if (!value_array(runtime, result))
    {
        set_error(error, error_capacity, "Unable to allocate sorted array.");
        return false;
    }

    size_t length = value_array_length(array);
    for (size_t i = 0; i < length; i++)
    {
        Value item = value_null();
        if (!value_array_get(array, i, &item) || !value_array_push(result, &item))
        {
            value_release(&item);
            value_release(result);
            set_error(error, error_capacity, "Unable to copy array for sorting.");
            return false;
        }
        value_release(&item);
    }
    for (size_t i = 1; i < length; i++)
    {
        Value key = value_null();
        if (!value_array_get(result, i, &key))
        {
            value_release(result);
            set_error(error, error_capacity, "Unable to read value while sorting array.");
            return false;
        }
        size_t position = i;
        while (position > 0)
        {
            Value previous = value_null();
            int comparison;
            if (!value_array_get(result, position - 1, &previous) ||
                !compare_values(&previous, &key, &comparison))
            {
                value_release(&previous);
                value_release(&key);
                value_release(result);
                set_error(error, error_capacity, "sort supports arrays of numbers or strings.");
                return false;
            }
            value_release(&previous);
            bool move = order == STD_ARRAY_ASCENDING ? comparison > 0 : comparison < 0;
            if (!move)
                break;
            previous = value_null();
            if (!value_array_get(result, position - 1, &previous) ||
                !value_array_set(result, position, &previous))
            {
                value_release(&previous);
                value_release(&key);
                value_release(result);
                set_error(error, error_capacity, "Unable to reorder array values.");
                return false;
            }
            value_release(&previous);
            position--;
        }
        if (!value_array_set(result, position, &key))
        {
            value_release(&key);
            value_release(result);
            set_error(error, error_capacity, "Unable to place sorted value.");
            return false;
        }
        value_release(&key);
    }
    return true;
}

bool stdlib_string_trim(RuntimeContext *runtime, const Value *string,
                        Value *result, char *error, size_t error_capacity)
{
    size_t length = 0;
    const char *bytes = value_string_bytes(string, &length);
    if (!runtime || !bytes || !result)
    {
        set_error(error, error_capacity, "trim expects a string.");
        return false;
    }
    size_t start = 0;
    while (start < length &&
           (bytes[start] == ' ' || bytes[start] == '\t' ||
            bytes[start] == '\n' || bytes[start] == '\r' ||
            bytes[start] == '\v' || bytes[start] == '\f'))
        start++;
    while (length > start &&
           (bytes[length - 1] == ' ' || bytes[length - 1] == '\t' ||
            bytes[length - 1] == '\n' || bytes[length - 1] == '\r' ||
            bytes[length - 1] == '\v' || bytes[length - 1] == '\f'))
        length--;
    if (!value_string(runtime, bytes + start, length - start, result))
    {
        set_error(error, error_capacity, "Unable to allocate trimmed string.");
        return false;
    }
    return true;
}

static bool string_pair_bytes(const Value *string, const Value *other,
                              const char **bytes, size_t *length,
                              const char **other_bytes, size_t *other_length,
                              char *error, size_t error_capacity,
                              const char *function_name)
{
    *bytes = value_string_bytes(string, length);
    *other_bytes = value_string_bytes(other, other_length);
    if (!*bytes || !*other_bytes)
    {
        if (error && error_capacity)
            snprintf(error, error_capacity, "%s expects two strings.",
                     function_name);
        return false;
    }
    return true;
}

bool stdlib_string_starts_with(const Value *string, const Value *prefix,
                               bool *result, char *error,
                               size_t error_capacity)
{
    const char *bytes;
    const char *prefix_bytes;
    size_t length;
    size_t prefix_length;
    if (!result ||
        !string_pair_bytes(string, prefix, &bytes, &length, &prefix_bytes,
                           &prefix_length, error, error_capacity,
                           "starts_with"))
        return false;
    *result = prefix_length <= length &&
              memcmp(bytes, prefix_bytes, prefix_length) == 0;
    return true;
}

bool stdlib_string_ends_with(const Value *string, const Value *suffix,
                             bool *result, char *error,
                             size_t error_capacity)
{
    const char *bytes;
    const char *suffix_bytes;
    size_t length;
    size_t suffix_length;
    if (!result ||
        !string_pair_bytes(string, suffix, &bytes, &length, &suffix_bytes,
                           &suffix_length, error, error_capacity,
                           "ends_with"))
        return false;
    *result = suffix_length <= length &&
              memcmp(bytes + (length - suffix_length), suffix_bytes,
                     suffix_length) == 0;
    return true;
}

static size_t find_string_bytes(const char *bytes, size_t length,
                                const char *needle, size_t needle_length,
                                size_t start)
{
    if (needle_length == 0)
        return start <= length ? start : SIZE_MAX;
    if (start > length || needle_length > length - start)
        return SIZE_MAX;
    size_t last_start = length - needle_length;
    for (size_t i = start; i <= last_start; i++)
        if (memcmp(bytes + i, needle, needle_length) == 0)
            return i;
    return SIZE_MAX;
}

bool stdlib_string_contains(const Value *string, const Value *needle,
                            bool *result, char *error,
                            size_t error_capacity)
{
    const char *bytes;
    const char *needle_bytes;
    size_t length;
    size_t needle_length;
    if (!result ||
        !string_pair_bytes(string, needle, &bytes, &length, &needle_bytes,
                           &needle_length, error, error_capacity, "contains"))
        return false;
    *result = find_string_bytes(bytes, length, needle_bytes, needle_length,
                                0) != SIZE_MAX;
    return true;
}

bool stdlib_string_index_of(const Value *string, const Value *needle,
                            Value *result, char *error,
                            size_t error_capacity)
{
    const char *bytes;
    const char *needle_bytes;
    size_t length;
    size_t needle_length;
    if (!result ||
        !string_pair_bytes(string, needle, &bytes, &length, &needle_bytes,
                           &needle_length, error, error_capacity, "index_of"))
        return false;
    size_t position = find_string_bytes(bytes, length, needle_bytes,
                                        needle_length, 0);
    if (position == SIZE_MAX)
    {
        *result = value_int(-1);
        return true;
    }
    if (position > (size_t)INT64_MAX)
    {
        set_error(error, error_capacity, "String index exceeds integer range.");
        return false;
    }
    *result = value_int((int64_t)position);
    return true;
}

bool stdlib_string_count(const Value *string, const Value *needle,
                         Value *result, char *error,
                         size_t error_capacity)
{
    const char *bytes;
    const char *needle_bytes;
    size_t length;
    size_t needle_length;
    if (!result ||
        !string_pair_bytes(string, needle, &bytes, &length, &needle_bytes,
                           &needle_length, error, error_capacity, "count"))
        return false;

    size_t matches = 0;
    size_t position = 0;
    if (needle_length == 0)
    {
        if (length == SIZE_MAX)
        {
            set_error(error, error_capacity, "String match count is too large.");
            return false;
        }
        matches = length + 1;
    }
    else
    {
        while ((position = find_string_bytes(bytes, length, needle_bytes,
                                             needle_length,
                                             position)) != SIZE_MAX)
        {
            matches++;
            position += needle_length;
        }
    }
    if (matches > (size_t)INT64_MAX)
    {
        set_error(error, error_capacity, "String match count exceeds integer range.");
        return false;
    }
    *result = value_int((int64_t)matches);
    return true;
}

bool stdlib_string_replace(RuntimeContext *runtime, const Value *string,
                           const Value *search, const Value *replacement,
                           Value *result, char *error,
                           size_t error_capacity)
{
    size_t length;
    size_t search_length;
    size_t replacement_length;
    const char *bytes = value_string_bytes(string, &length);
    const char *search_bytes = value_string_bytes(search, &search_length);
    const char *replacement_bytes =
        value_string_bytes(replacement, &replacement_length);
    if (!runtime || !bytes || !search_bytes || !replacement_bytes || !result)
    {
        set_error(error, error_capacity,
                  "replace expects three strings and a valid runtime.");
        return false;
    }

    if (search_length == 0)
    {
        if (!value_string(runtime, bytes, length, result))
        {
            set_error(error, error_capacity, "Unable to allocate replaced string.");
            return false;
        }
        return true;
    }

    size_t match_count = 0;
    size_t position = 0;
    while ((position = find_string_bytes(bytes, length, search_bytes,
                                         search_length, position)) != SIZE_MAX)
    {
        match_count++;
        position += search_length;
    }

    size_t output_length = length;
    if (replacement_length >= search_length)
    {
        size_t growth = replacement_length - search_length;
        if (growth != 0 && match_count > (SIZE_MAX - length) / growth)
        {
            set_error(error, error_capacity, "Replaced string is too large.");
            return false;
        }
        output_length += match_count * growth;
    }
    else
    {
        output_length -= match_count * (search_length - replacement_length);
    }

    char *output = output_length > 0 ? malloc(output_length) : NULL;
    if (output_length > 0 && !output)
    {
        set_error(error, error_capacity, "Unable to allocate replaced string.");
        return false;
    }
    size_t source_offset = 0;
    size_t output_offset = 0;
    while ((position = find_string_bytes(bytes, length, search_bytes,
                                         search_length, source_offset)) != SIZE_MAX)
    {
        size_t unchanged = position - source_offset;
        if (unchanged > 0)
        {
            memcpy(output + output_offset, bytes + source_offset, unchanged);
            output_offset += unchanged;
        }
        if (replacement_length > 0)
        {
            memcpy(output + output_offset, replacement_bytes,
                   replacement_length);
            output_offset += replacement_length;
        }
        source_offset = position + search_length;
    }
    size_t tail_length = length - source_offset;
    if (tail_length > 0)
        memcpy(output + output_offset, bytes + source_offset, tail_length);

    bool success = value_string(runtime, output, output_length, result);
    free(output);
    if (!success)
        set_error(error, error_capacity, "Unable to allocate replaced string.");
    return success;
}

bool stdlib_string_repeat(RuntimeContext *runtime, const Value *string,
                          const Value *count, Value *result, char *error,
                          size_t error_capacity)
{
    size_t length;
    const char *bytes = value_string_bytes(string, &length);
    if (!runtime || !bytes || !count || count->type != VALUE_INT ||
        count->as.integer < 0 || !result)
    {
        set_error(error, error_capacity,
                  "repeat expects a string and a nonnegative integer.");
        return false;
    }
    if ((uint64_t)count->as.integer > (uint64_t)SIZE_MAX)
    {
        set_error(error, error_capacity, "Repeat count exceeds platform limits.");
        return false;
    }
    size_t repetitions = (size_t)(uint64_t)count->as.integer;
    if (repetitions != 0 && length > (SIZE_MAX - 1) / repetitions)
    {
        set_error(error, error_capacity, "Repeated string is too large.");
        return false;
    }
    size_t output_length = length * repetitions;
    char *output = output_length > 0 ? malloc(output_length) : NULL;
    if (output_length > 0 && !output)
    {
        set_error(error, error_capacity, "Unable to allocate repeated string.");
        return false;
    }
    for (size_t i = 0; i < repetitions && length > 0; i++)
        memcpy(output + i * length, bytes, length);
    bool success = value_string(runtime, output, output_length, result);
    free(output);
    if (!success)
        set_error(error, error_capacity, "Unable to allocate repeated string.");
    return success;
}

bool stdlib_math_unary(StdMathFunction function, const Value *number,
                       Value *result, char *error, size_t error_capacity)
{
    double input;
    double output;
    if (!number || !result ||
        (number->type != VALUE_INT && number->type != VALUE_FLOAT))
    {
        set_error(error, error_capacity, "Math function expects a number.");
        return false;
    }
    input = number->type == VALUE_INT ? (double)number->as.integer
                                      : number->as.floating;
    if (!isfinite(input))
    {
        set_error(error, error_capacity,
                  "Math function requires a finite number.");
        return false;
    }
    switch (function)
    {
    case STD_MATH_SQRT:
        if (input < 0.0)
        {
            set_error(error, error_capacity,
                      "sqrt requires a nonnegative number.");
            return false;
        }
        output = sqrt(input);
        break;
    case STD_MATH_SIN:
        output = sin(input);
        break;
    case STD_MATH_COS:
        output = cos(input);
        break;
    case STD_MATH_LOG:
        if (input <= 0.0)
        {
            set_error(error, error_capacity, "log requires a positive number.");
            return false;
        }
        output = log(input);
        break;
    case STD_MATH_EXP:
        output = exp(input);
        break;
    case STD_MATH_FLOOR:
        output = floor(input);
        break;
    case STD_MATH_CEIL:
        output = ceil(input);
        break;
    case STD_MATH_ROUND:
        output = round(input);
        break;
    default:
        set_error(error, error_capacity, "Unknown math function.");
        return false;
    }
    if (!isfinite(output))
    {
        set_error(error, error_capacity,
                  "Math result is outside the finite range.");
        return false;
    }
    *result = value_float(output);
    return true;
}

static int64_t signed_from_bits(uint64_t bits)
{
    if (bits <= (uint64_t)INT64_MAX)
        return (int64_t)bits;
    return -1 - (int64_t)(UINT64_MAX - bits);
}

bool stdlib_random_int(RuntimeContext *runtime, const Value *minimum,
                       const Value *maximum, Value *result,
                       char *error, size_t error_capacity)
{
    uint64_t range;
    uint64_t random_value;
    uint64_t threshold;
    if (!runtime_is_initialized(runtime) || !minimum || !maximum || !result ||
        minimum->type != VALUE_INT || maximum->type != VALUE_INT)
    {
        set_error(error, error_capacity,
                  "random_int expects two integer bounds.");
        return false;
    }
    if (minimum->as.integer > maximum->as.integer)
    {
        set_error(error, error_capacity,
                  "random_int minimum must not exceed its maximum.");
        return false;
    }
    range = (uint64_t)maximum->as.integer -
            (uint64_t)minimum->as.integer + UINT64_C(1);
    if (range == 0)
        random_value = runtime_random_next(runtime);
    else
    {
        threshold = (uint64_t)(-range) % range;
        do
        {
            random_value = runtime_random_next(runtime);
        } while (random_value < threshold);
        random_value %= range;
        random_value += (uint64_t)minimum->as.integer;
    }
    *result = value_int(signed_from_bits(random_value));
    return true;
}

bool stdlib_random_seed(RuntimeContext *runtime, const Value *seed,
                        Value *result, char *error, size_t error_capacity)
{
    if (!runtime_is_initialized(runtime) || !seed || !result ||
        seed->type != VALUE_INT)
    {
        set_error(error, error_capacity,
                  "random_seed expects one integer seed.");
        return false;
    }
    runtime_random_seed(runtime, (uint64_t)seed->as.integer);
    *result = value_null();
    return true;
}
