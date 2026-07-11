/* d9mt addition: PE-side marshalling for the AIR compile thunks.
 * Style mirrors airconv_thunks.c: fill params, WINE_UNIX_CALL by code. */

/* export, not import — must precede winemetal.h (mirrors winemetal_thunks.c) */
#ifdef _WIN32
#define WINEMETAL_API __declspec(dllexport)
#endif

#include "wineunixlib.h"
#include "d9mtair_thunks.h"

WINEMETAL_API uint32_t
D9MTAirVersion(struct d9mt_air_version_params *params) {
  NTSTATUS status = WINE_UNIX_CALL(154, params);
  return status == 0 ? params->abi_version : 0u;
}

WINEMETAL_API uint32_t
D9MTAirCompile(struct d9mt_air_compile_params *params) {
  WINE_UNIX_CALL(155, params);
  return params->ret_status;
}

WINEMETAL_API void
D9MTAirFree(struct d9mt_air_free_params *params) {
  WINE_UNIX_CALL(156, params);
}
