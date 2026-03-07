#ifndef URBC_COMMON_H
#define URBC_COMMON_H 1

#include "urb.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define URBC_VERSION_MAJOR 1
#define URBC_VERSION_MINOR 0
#define URBC_ERROR_CAP 256

#if defined(_MSC_VER)
#define URBC_THREAD_LOCAL __declspec(thread)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define URBC_THREAD_LOCAL _Thread_local
#else
#define URBC_THREAD_LOCAL __thread
#endif

typedef enum UrbcStatus {
    URBC_OK = 0,
    URBC_ERR_ARGUMENT,
    URBC_ERR_FORMAT,
    URBC_ERR_RANGE,
    URBC_ERR_ALLOC,
    URBC_ERR_UNIMPLEMENTED,
    URBC_ERR_RUNTIME,
} UrbcStatus;

const char *urbc_status_name(UrbcStatus status);

#ifdef __cplusplus
}
#endif

#endif
