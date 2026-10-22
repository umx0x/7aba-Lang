#include <stdint.h>

#ifdef _WIN32
#define FFI_EXPORT __declspec(dllexport)
#else
#define FFI_EXPORT __attribute__((visibility("default")))
#endif

FFI_EXPORT int64_t sevenba_add(int64_t left, int64_t right)
{
    return left + right;
}

FFI_EXPORT double sevenba_midpoint(double left, double right)
{
    return (left + right) / 2.0;
}
