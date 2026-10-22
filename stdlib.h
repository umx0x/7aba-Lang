#ifndef STDLIB_H
#define STDLIB_H

#include "value.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct StdFile StdFile;
typedef enum
{
    STD_FILE_READ,
    STD_FILE_WRITE,
    STD_FILE_APPEND,
    STD_FILE_READ_BINARY,
    STD_FILE_WRITE_BINARY,
    STD_FILE_APPEND_BINARY
} StdFileMode;

typedef enum
{
    STD_ARRAY_ASCENDING,
    STD_ARRAY_DESCENDING
} StdArrayOrder;

typedef enum
{
    STD_MATH_SQRT,
    STD_MATH_SIN,
    STD_MATH_COS,
    STD_MATH_LOG,
    STD_MATH_EXP,
    STD_MATH_FLOOR,
    STD_MATH_CEIL,
    STD_MATH_ROUND
} StdMathFunction;

StdFile *stdlib_file_open(const char *path, StdFileMode mode);
bool stdlib_file_close(StdFile *file);
bool stdlib_file_read(StdFile *file, void *buffer, size_t capacity, size_t *bytes_read);
bool stdlib_file_read_all(StdFile *file, unsigned char **bytes, size_t *length);
bool stdlib_file_write(StdFile *file, const void *bytes, size_t length,
                       size_t *bytes_written);
bool stdlib_file_seek(StdFile *file, long offset, int origin);
bool stdlib_file_flush(StdFile *file);

bool stdlib_array_map(RuntimeContext *runtime, const Value *array,
                      ValueNativeFn callback, Value *result,
                      char *error, size_t error_capacity);
bool stdlib_array_filter(RuntimeContext *runtime, const Value *array,
                         ValueNativeFn predicate, Value *result,
                         char *error, size_t error_capacity);
bool stdlib_array_sort(RuntimeContext *runtime, const Value *array,
                       StdArrayOrder order, Value *result,
                       char *error, size_t error_capacity);

/* String helpers operate on bytes. trim recognizes the C locale's ASCII
 * whitespace bytes; starts_with and ends_with compare exact byte sequences. */
bool stdlib_string_trim(RuntimeContext *runtime, const Value *string,
                        Value *result, char *error, size_t error_capacity);
bool stdlib_string_starts_with(const Value *string, const Value *prefix,
                               bool *result, char *error,
                               size_t error_capacity);
bool stdlib_string_ends_with(const Value *string, const Value *suffix,
                             bool *result, char *error,
                             size_t error_capacity);
bool stdlib_string_contains(const Value *string, const Value *needle,
                            bool *result, char *error,
                            size_t error_capacity);
bool stdlib_string_index_of(const Value *string, const Value *needle,
                            Value *result, char *error,
                            size_t error_capacity);
bool stdlib_string_count(const Value *string, const Value *needle,
                         Value *result, char *error,
                         size_t error_capacity);
bool stdlib_string_replace(RuntimeContext *runtime, const Value *string,
                           const Value *search, const Value *replacement,
                           Value *result, char *error,
                           size_t error_capacity);
bool stdlib_string_repeat(RuntimeContext *runtime, const Value *string,
                          const Value *count, Value *result, char *error,
                          size_t error_capacity);
bool stdlib_math_unary(StdMathFunction function, const Value *number,
                       Value *result, char *error, size_t error_capacity);
bool stdlib_random_int(RuntimeContext *runtime, const Value *minimum,
                       const Value *maximum, Value *result,
                       char *error, size_t error_capacity);
bool stdlib_random_seed(RuntimeContext *runtime, const Value *seed,
                        Value *result, char *error, size_t error_capacity);

#endif
