#pragma once

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>

#if defined(__WINE__) && !defined(typeof)
#define typeof __typeof
#endif
