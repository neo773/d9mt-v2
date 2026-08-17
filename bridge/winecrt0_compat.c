#include <windows.h>
typedef LONG NTSTATUS;

typedef UINT64 unixlib_handle_t;

#define MemoryWineUnixFuncs 1000

extern NTSTATUS WINAPI __wine_unix_call(unixlib_handle_t handle,
                                        unsigned int code, void *args);
extern NTSTATUS WINAPI NtQueryVirtualMemory(HANDLE process, LPCVOID addr,
                                            int info_class, PVOID info,
                                            SIZE_T size, SIZE_T *ret_size);

extern IMAGE_DOS_HEADER __ImageBase;

unixlib_handle_t __wine_unixlib_handle = 0;

static NTSTATUS WINAPI
compat_dispatcher(unixlib_handle_t handle, unsigned int code, void *args) {
  return __wine_unix_call(handle, code, args);
}

NTSTATUS (WINAPI *__wine_unix_call_dispatcher)(unixlib_handle_t,
                                               unsigned int, void *) =
    compat_dispatcher;

NTSTATUS WINAPI
__wine_init_unix_call(void) {
  SIZE_T ret_size = 0;
  return NtQueryVirtualMemory(GetCurrentProcess(), &__ImageBase,
                              MemoryWineUnixFuncs, &__wine_unixlib_handle,
                              sizeof(__wine_unixlib_handle), &ret_size);
}
