#ifndef __D9MTAIR_THUNKS_H
#define __D9MTAIR_THUNKS_H

#include "winemetal.h"
#include "d9mtair.h"

WINEMETAL_API uint32_t D9MTAirVersion(struct d9mt_air_version_params *params);
WINEMETAL_API uint32_t D9MTAirCompile(struct d9mt_air_compile_params *params);
WINEMETAL_API void     D9MTAirFree(struct d9mt_air_free_params *params);

#endif
