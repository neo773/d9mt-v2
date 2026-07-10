/* d9mt addition: DXMT airconv/DXSO entry points referenced by the upstream
 * dispatch tables, stubbed out — this build ships the d9mtair compile core
 * (D3D9 SM3 -> AIR) instead of DXMT airconv. Callers get a clean failure. */

#include <stddef.h>
#include <string.h>
#include "airconv_public.h"

AIRCONV_API int SM50Initialize( const void *pBytecode, size_t BytecodeSize, sm50_shader_t *ppShader, struct MTL_SHADER_REFLECTION *pRefl, sm50_error_t *ppError ) { return -1; }

AIRCONV_API void SM50Destroy(sm50_shader_t pShader) { /* unavailable in d9mt build */ }

AIRCONV_API int SM50Compile( sm50_shader_t pShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError ) { return -1; }

AIRCONV_API int SM50CompileGeometryPipelineGeometry( sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pGeometryShaderArgs, const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError ) { return -1; }

AIRCONV_API int SM50CompileGeometryPipelineVertex( sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pVertexShaderArgs, const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError ) { return -1; }

AIRCONV_API int SM50CompileTessellationPipelineDomain( sm50_shader_t pHullShader, sm50_shader_t pDomainShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pDomainShaderArgs, const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError ) { return -1; }

AIRCONV_API int SM50CompileTessellationPipelineHull( sm50_shader_t pVertexShader, sm50_shader_t pHullShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pHullShaderArgs, const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError ) { return -1; }

AIRCONV_API void SM50DestroyBitcode(sm50_bitcode_t pBitcode) { /* unavailable in d9mt build */ }

AIRCONV_API void SM50FreeError(sm50_error_t pError) { /* unavailable in d9mt build */ }

AIRCONV_API void SM50GetArgumentsInfo( sm50_shader_t pShader, struct MTL_SM50_SHADER_ARGUMENT *pConstantBuffers, struct MTL_SM50_SHADER_ARGUMENT *pArguments ) { /* unavailable in d9mt build */ }

AIRCONV_API void SM50GetCompiledBitcode( sm50_bitcode_t pBitcode, struct SM50_COMPILED_BITCODE *pData ) { /* unavailable in d9mt build */ }

AIRCONV_API size_t SM50GetErrorMessage(sm50_error_t pError, char *pBuffer, size_t BufferSize) { return 0; }

AIRCONV_API int DXSOInitialize( const void *pBytecode, size_t BytecodeSize, dxso_shader_t *ppShader ) { return -1; }

AIRCONV_API void DXSODestroy(dxso_shader_t pShader) { /* unavailable in d9mt build */ }

AIRCONV_API int DXSOCompile( dxso_shader_t pShader, struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, const char *FunctionName, dxso_bitcode_t *ppBitcode ) { return -1; }

AIRCONV_API void DXSOGetCompiledBitcode( dxso_bitcode_t pBitcode, struct SM50_COMPILED_BITCODE *pData ) { /* unavailable in d9mt build */ }

AIRCONV_API void DXSODestroyBitcode(dxso_bitcode_t pBitcode) { /* unavailable in d9mt build */ }
