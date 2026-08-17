#define D9MTAIR_EXPORTS
#include <windows.h>
#include "d9mtair.h"

typedef UINT64 unixlib_handle_t;

#define MemoryWineUnixFuncs 1000

extern NTSTATUS WINAPI __wine_unix_call(unixlib_handle_t handle,
                                        unsigned int code, void *args);
extern NTSTATUS WINAPI NtQueryVirtualMemory(HANDLE process, LPCVOID addr,
                                            int info_class, PVOID buffer,
                                            SIZE_T length, SIZE_T *res_len);

static unixlib_handle_t d9mtair_unix_handle;

extern IMAGE_DOS_HEADER __ImageBase;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    /* must run at attach time: wine's unixlib bookkeeping is only
     * guaranteed alive while the module load is in progress */
    NtQueryVirtualMemory(GetCurrentProcess(), &__ImageBase,
                         MemoryWineUnixFuncs, &d9mtair_unix_handle,
                         sizeof(d9mtair_unix_handle), NULL);
  }
  return TRUE;
}

int __cdecl D9MTAir_UnixCall(unsigned int code, void *params) {
  if (!d9mtair_unix_handle) {
    NTSTATUS status = NtQueryVirtualMemory(
        GetCurrentProcess(), &__ImageBase, MemoryWineUnixFuncs,
        &d9mtair_unix_handle, sizeof(d9mtair_unix_handle), NULL);
    if (status)
      return status;
  }
  return __wine_unix_call(d9mtair_unix_handle, code, params);
}
