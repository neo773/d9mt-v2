/* stubbed out: this build ships the d9mtair compile core instead of DXMT airconv, so these entry points are unreachable. */

#include <stddef.h>
#include <string.h>
#include "airconv_public.h"

AIRCONV_API int SM50Initialize( const void *pBytecode, size_t BytecodeSize, sm50_shader_t *ppShader, struct MTL_SHADER_REFLECTION *pRefl, sm50_error_t *ppError ) { return -1; }

AIRCONV_API void SM50Destroy(sm50_shader_t pShader) {}

AIRCONV_API int SM50Compile( sm50_shader_t pShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError ) { return -1; }

AIRCONV_API int SM50CompileGeometryPipelineGeometry( sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pGeometryShaderArgs, const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError ) { return -1; }

AIRCONV_API int SM50CompileGeometryPipelineVertex( sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pVertexShaderArgs, const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError ) { return -1; }

AIRCONV_API int SM50CompileTessellationPipelineDomain( sm50_shader_t pHullShader, sm50_shader_t pDomainShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pDomainShaderArgs, const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError ) { return -1; }

AIRCONV_API int SM50CompileTessellationPipelineHull( sm50_shader_t pVertexShader, sm50_shader_t pHullShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pHullShaderArgs, const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError ) { return -1; }

AIRCONV_API void SM50DestroyBitcode(sm50_bitcode_t pBitcode) {}

AIRCONV_API void SM50FreeError(sm50_error_t pError) {}

AIRCONV_API void SM50GetArgumentsInfo( sm50_shader_t pShader, struct MTL_SM50_SHADER_ARGUMENT *pConstantBuffers, struct MTL_SM50_SHADER_ARGUMENT *pArguments ) {}

AIRCONV_API void SM50GetCompiledBitcode( sm50_bitcode_t pBitcode, struct SM50_COMPILED_BITCODE *pData ) {}

AIRCONV_API size_t SM50GetErrorMessage(sm50_error_t pError, char *pBuffer, size_t BufferSize) { return 0; }

AIRCONV_API int DXSOInitialize( const void *pBytecode, size_t BytecodeSize, dxso_shader_t *ppShader ) { return -1; }

AIRCONV_API void DXSODestroy(dxso_shader_t pShader) {}

AIRCONV_API int DXSOCompile( dxso_shader_t pShader, struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, const char *FunctionName, dxso_bitcode_t *ppBitcode ) { return -1; }

AIRCONV_API void DXSOGetCompiledBitcode( dxso_bitcode_t pBitcode, struct SM50_COMPILED_BITCODE *pData ) {}

AIRCONV_API void DXSODestroyBitcode(dxso_bitcode_t pBitcode) {}
