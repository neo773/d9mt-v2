#include <stdatomic.h>
#include <dlfcn.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#import <Cocoa/Cocoa.h>
#import <ColorSync/ColorSync.h>
#import <CoreFoundation/CFRunLoop.h>
#import <Metal/Metal.h>
#import <Metal/MTL4CommandQueue.h>
/* d9mt addition: full Metal 4 surface (codes 157..200). Explicit MTL4 imports
 * so the bridge builds even where <Metal/Metal.h> does not umbrella every
 * MTL4 header on this SDK. */
#import <Metal/MTL4CommandAllocator.h>
#import <Metal/MTL4CommandBuffer.h>
#import <Metal/MTL4ArgumentTable.h>
#import <Metal/MTL4RenderCommandEncoder.h>
#import <Metal/MTL4ComputeCommandEncoder.h>
#import <Metal/MTL4RenderPass.h>
#import <Metal/MTL4Compiler.h>
#import <Metal/MTL4RenderPipeline.h>
#import <Metal/MTL4PipelineState.h>
#import <Metal/MTL4LibraryFunctionDescriptor.h>
#import <Metal/MTLResidencySet.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>
#include "objc/objc-runtime.h"
#include <bootstrap.h>
#include <mach/mach_port.h>
#include <pthread.h>
#define WINEMETAL_API
#include "../winemetal_thunks.h"
#include "../airconv_thunks.h"
#include "../../driver/mtl/mtl_wire.h"

#if DXMT_APITRACE_METAL
#include "apitrace/metal_capi.h"
#endif

typedef int NTSTATUS;
#define STATUS_SUCCESS 0
#define STATUS_UNSUCCESSFUL 0xC0000001

#if DXMT_APITRACE_METAL
typedef NS_ENUM(uint32_t, DXMTApitraceEncoderKind) {
  DXMTApitraceEncoderKindRender = 1,
  DXMTApitraceEncoderKindCompute = 2,
  DXMTApitraceEncoderKindBlit = 3,
};

@interface DXMTApitraceCommandBufferState : NSObject
@property(nonatomic, assign) uint64_t beginSequence;
@property(nonatomic, retain) NSMutableData *blitCopyTextureOps;
@property(nonatomic, retain) NSMutableData *blitWaitFences;
@property(nonatomic, retain) NSMutableData *blitUpdateFences;
@end

@implementation DXMTApitraceCommandBufferState
-(instancetype)init {
  self = [super init];
  if (self) {
    _blitCopyTextureOps = [[NSMutableData alloc] init];
    _blitWaitFences = [[NSMutableData alloc] init];
    _blitUpdateFences = [[NSMutableData alloc] init];
  }
  return self;
}
@end

@interface DXMTApitraceEncoderState : NSObject
@property(nonatomic, assign) uint64_t beginSequence;
@property(nonatomic, assign) uint64_t commandBuffer;
@property(nonatomic, assign) uint64_t d3dSequence;
@property(nonatomic, assign) DXMTApitraceEncoderKind kind;
@property(nonatomic, assign) BOOL metalTraceBegan;
@property(nonatomic, retain) NSMutableArray *fenceOps;
@property(nonatomic, retain) NSMutableArray *blitOps;
@property(nonatomic, retain) NSMutableData *blitWaitFences;
@property(nonatomic, retain) NSMutableData *blitUpdateFences;
@end

@implementation DXMTApitraceEncoderState
-(instancetype)init {
  self = [super init];
  if (self) {
    _fenceOps = [[NSMutableArray alloc] init];
    _blitOps = [[NSMutableArray alloc] init];
    _blitWaitFences = [[NSMutableData alloc] init];
    _blitUpdateFences = [[NSMutableData alloc] init];
  }
  return self;
}
@end

static pthread_mutex_t dxmt_apitrace_lock = PTHREAD_MUTEX_INITIALIZER;
static apitrace_metal_session_t *dxmt_apitrace_session = NULL;
static NSMutableDictionary *dxmt_apitrace_command_buffers = nil;
static NSMutableDictionary *dxmt_apitrace_encoders = nil;
static NSMutableDictionary *dxmt_apitrace_functions = nil;
static __thread uint64_t dxmt_apitrace_current_d3d_sequence = 0;
static uint64_t dxmt_apitrace_blit_payload_count = 0;
static uint64_t dxmt_apitrace_blit_payload_ops = 0;
static uint64_t dxmt_apitrace_blit_payload_wait_fences = 0;
static uint64_t dxmt_apitrace_blit_payload_update_fences = 0;
static uint64_t dxmt_apitrace_blit_payload_bytes = 0;
static uint64_t dxmt_apitrace_blit_payload_build_ns = 0;
static uint64_t dxmt_apitrace_blit_payload_max_build_ns = 0;
static const NSUInteger dxmt_apitrace_command_buffer_blit_ops_flush_threshold = 256;

static uint32_t
dxmt_apitrace_command_buffer_copy_texture_count(DXMTApitraceCommandBufferState *state) {
  return state ? (uint32_t)(state.blitCopyTextureOps.length / sizeof(apitrace_metal_copy_texture_op_t)) : 0;
}

static bool
dxmt_apitrace_truthy_env_value(const char *value) {
  if (!value || !value[0])
    return false;
  return !strcmp(value, "1") || !strcmp(value, "true") || !strcmp(value, "yes") || !strcmp(value, "trace");
}

static bool
dxmt_apitrace_runtime_enabled(void) {
  const char *enabled = getenv("DXMT_APITRACE_ENBALED");
  if (!enabled || !enabled[0])
    enabled = getenv("DXMT_APITRACE_ENABLED");
  return dxmt_apitrace_truthy_env_value(enabled);
}

static apitrace_metal_session_t *
dxmt_apitrace_ensure_session_locked(void);

static const char *
dxmt_apitrace_bundle_root(void) {
  const char *bundle_root = getenv("APITRACE_TRACE_BUNDLE");
  if (bundle_root && bundle_root[0])
    return bundle_root;

  static bool warned = false;
  if (!warned) {
    warned = true;
    fprintf(stderr,
            "warn:  DXMT apitrace: APITRACE_TRACE_BUNDLE not set; PE side must "
            "initialize bundle root before opening unix session\n");
  }
  return NULL;
}

static bool
dxmt_apitrace_verbose_enabled(void) {
  static bool initialized = false;
  static bool enabled = false;
  if (!initialized) {
    const char *value = getenv("APITRACE_METAL_VERBOSE");
    enabled = dxmt_apitrace_truthy_env_value(value);
    initialized = true;
  }
  return enabled;
}

static void
dxmt_apitrace_log(const char *message, uint64_t arg0, uint64_t arg1) {
  if (!dxmt_apitrace_verbose_enabled())
    return;
  fprintf(stderr, "info:  DXMT apitrace: %s arg0=%llu arg1=%llu\n",
          message,
          (unsigned long long)arg0,
          (unsigned long long)arg1);
}

static uint64_t
dxmt_apitrace_monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void
dxmt_apitrace_record_blit_payload_stats(uint64_t build_ns, NSUInteger ops_count, NSUInteger wait_count, NSUInteger update_count, NSUInteger payload_bytes) {
  ++dxmt_apitrace_blit_payload_count;
  dxmt_apitrace_blit_payload_ops += (uint64_t)ops_count;
  dxmt_apitrace_blit_payload_wait_fences += (uint64_t)wait_count;
  dxmt_apitrace_blit_payload_update_fences += (uint64_t)update_count;
  dxmt_apitrace_blit_payload_bytes += (uint64_t)payload_bytes;
  dxmt_apitrace_blit_payload_build_ns += build_ns;
  if (build_ns > dxmt_apitrace_blit_payload_max_build_ns)
    dxmt_apitrace_blit_payload_max_build_ns = build_ns;
}

static void
dxmt_apitrace_log_blit_payload_stats(void) {
  if (!dxmt_apitrace_blit_payload_count)
    return;
  fprintf(stderr,
          "info:  DXMT apitrace blit payload stats: count=%llu ops=%llu wait_fences=%llu update_fences=%llu bytes=%llu build_ns=%llu max_build_ns=%llu\n",
          (unsigned long long)dxmt_apitrace_blit_payload_count,
          (unsigned long long)dxmt_apitrace_blit_payload_ops,
          (unsigned long long)dxmt_apitrace_blit_payload_wait_fences,
          (unsigned long long)dxmt_apitrace_blit_payload_update_fences,
          (unsigned long long)dxmt_apitrace_blit_payload_bytes,
          (unsigned long long)dxmt_apitrace_blit_payload_build_ns,
          (unsigned long long)dxmt_apitrace_blit_payload_max_build_ns);
}

static uint64_t
dxmt_apitrace_texture_region_bytes_size(const struct WMTSize *size, uint64_t bytes_per_row, uint64_t bytes_per_image) {
  if (!size || !size->width || !size->height || !size->depth || !bytes_per_row)
    return 0;
  if (size->depth > 1) {
    if (!bytes_per_image)
      return 0;
    return bytes_per_image * size->depth;
  }
  return bytes_per_row * size->height;
}

static NSNumber *
dxmt_apitrace_key(obj_handle_t handle) {
  return [NSNumber numberWithUnsignedLongLong:(unsigned long long)handle];
}

static NSDictionary *
dxmt_apitrace_function_state_locked(obj_handle_t handle) {
  if (!handle || !dxmt_apitrace_functions)
    return nil;
  return [dxmt_apitrace_functions objectForKey:dxmt_apitrace_key(handle)];
}

static size_t
dxmt_apitrace_constant_data_size(enum WMTDataType type) {
  switch (type) {
  case WMTDataTypeFloat:
  case WMTDataTypeInt:
  case WMTDataTypeUInt:
  case WMTDataTypeBool:
    return 4;
  case WMTDataTypeFloat2:
  case WMTDataTypeInt2:
  case WMTDataTypeUInt2:
    return 8;
  case WMTDataTypeFloat3:
  case WMTDataTypeInt3:
  case WMTDataTypeUInt3:
    return 12;
  case WMTDataTypeFloat4:
  case WMTDataTypeInt4:
  case WMTDataTypeUInt4:
    return 16;
  default:
    return 0;
  }
}

static NSArray *
dxmt_apitrace_function_constants_json(const struct WMTFunctionConstant *constants, uint64_t num_constants, uint64_t bool_values) {
  NSMutableArray *values = [NSMutableArray arrayWithCapacity:num_constants];
  for (uint64_t i = 0; i < num_constants; ++i) {
    const struct WMTFunctionConstant *constant = &constants[i];
    NSMutableDictionary *entry = [NSMutableDictionary dictionary];
    entry[@"index"] = @(constant->index);
    entry[@"type"] = @(constant->type);
    if (constant->type == WMTDataTypeBool) {
      entry[@"bool_value"] = @((bool_values & (UINT64_C(1) << i)) != 0);
    } else {
      const size_t size = dxmt_apitrace_constant_data_size(constant->type);
      const uint8_t *bytes = constant->data.ptr;
      NSMutableArray *byte_values = [NSMutableArray arrayWithCapacity:size];
      for (size_t byte_index = 0; byte_index < size; ++byte_index) {
        [byte_values addObject:@(bytes ? bytes[byte_index] : 0)];
      }
      entry[@"bytes"] = byte_values;
    }
    [values addObject:entry];
  }
  return values;
}

static void
dxmt_apitrace_track_function_locked(obj_handle_t function, obj_handle_t library, NSString *name) {
  if (!function || !library || !name)
    return;
  if (!dxmt_apitrace_functions)
    dxmt_apitrace_functions = [[NSMutableDictionary alloc] init];
  NSDictionary *state = @{
    @"library_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)library],
    @"function_name" : name,
  };
  [dxmt_apitrace_functions setObject:state forKey:dxmt_apitrace_key(function)];
  if (dxmt_apitrace_ensure_session_locked()) {
    NSData *payload_data = [NSJSONSerialization dataWithJSONObject:state options:0 error:nil];
    NSString *payload = payload_data ? [[NSString alloc] initWithData:payload_data encoding:NSUTF8StringEncoding] : nil;
    if (payload) {
      apitrace_metal_object_metadata(dxmt_apitrace_session, function, payload.UTF8String);
      [payload release];
    }
  }
}

static void
dxmt_apitrace_track_function_with_constants_locked(
    obj_handle_t function, obj_handle_t library, NSString *name, const struct WMTFunctionConstant *constants,
    uint64_t num_constants, uint64_t bool_values) {
  if (!function || !library || !name)
    return;
  if (!dxmt_apitrace_functions)
    dxmt_apitrace_functions = [[NSMutableDictionary alloc] init];
  NSDictionary *state = @{
    @"library_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)library],
    @"function_name" : name,
    @"function_constants" : dxmt_apitrace_function_constants_json(constants, num_constants, bool_values),
  };
  [dxmt_apitrace_functions setObject:state forKey:dxmt_apitrace_key(function)];
  if (dxmt_apitrace_ensure_session_locked()) {
    NSData *payload_data = [NSJSONSerialization dataWithJSONObject:state options:0 error:nil];
    NSString *payload = payload_data ? [[NSString alloc] initWithData:payload_data encoding:NSUTF8StringEncoding] : nil;
    if (payload) {
      apitrace_metal_object_metadata(dxmt_apitrace_session, function, payload.UTF8String);
      [payload release];
    }
  }
}

static apitrace_metal_session_t *
dxmt_apitrace_ensure_session_locked(void) {
  if (dxmt_apitrace_session)
    return dxmt_apitrace_session;

  if (!dxmt_apitrace_runtime_enabled())
    return NULL;

  const char *bundle_root = dxmt_apitrace_bundle_root();
  if (!bundle_root)
    return NULL;
  dxmt_apitrace_session = apitrace_metal_session_open(bundle_root);
  if (!dxmt_apitrace_session)
    return NULL;

  if (!dxmt_apitrace_command_buffers)
    dxmt_apitrace_command_buffers = [[NSMutableDictionary alloc] init];
  if (!dxmt_apitrace_encoders)
    dxmt_apitrace_encoders = [[NSMutableDictionary alloc] init];
  if (!dxmt_apitrace_functions)
    dxmt_apitrace_functions = [[NSMutableDictionary alloc] init];

  dxmt_apitrace_log("session opened", 0, 0);
  return dxmt_apitrace_session;
}

static void
dxmt_apitrace_close_session_locked(void) {
  if (dxmt_apitrace_session) {
    dxmt_apitrace_log_blit_payload_stats();
    apitrace_metal_session_close(dxmt_apitrace_session);
    dxmt_apitrace_session = NULL;
  }
  [dxmt_apitrace_command_buffers removeAllObjects];
  [dxmt_apitrace_encoders removeAllObjects];
  [dxmt_apitrace_functions removeAllObjects];
  dxmt_apitrace_current_d3d_sequence = 0;
}

static NSString *
dxmt_apitrace_json_string(id object) {
  if (!object)
    return @"{}";

  NSError *error = nil;
  NSData *data = [NSJSONSerialization dataWithJSONObject:object options:0 error:&error];
  if (!data || error)
    return @"{}";
  NSString *json = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
  return [json autorelease];
}

static NSNumber *
dxmt_apitrace_optional_handle(obj_handle_t handle) {
  return handle ? [NSNumber numberWithUnsignedLongLong:(unsigned long long)handle] : (NSNumber *)[NSNull null];
}

static NSString *
dxmt_apitrace_copy_texture_json(
    obj_handle_t source_texture,
    obj_handle_t destination_texture,
    const struct WMTOrigin *source_origin,
    const struct WMTSize *source_size,
    uint32_t source_slice,
    uint32_t source_level,
    const struct WMTOrigin *destination_origin,
    uint32_t destination_slice,
    uint32_t destination_level);

static void
dxmt_apitrace_append_fence_op(DXMTApitraceEncoderState *state, const char *op, obj_handle_t fence, uint32_t stages) {
  if (!state || !op)
    return;
  [state.fenceOps addObject:@[
    [NSString stringWithUTF8String:op],
    @(fence),
    @(stages),
  ]];
}

static void
dxmt_apitrace_flush_fence_ops(apitrace_metal_session_t *session, obj_handle_t encoder, DXMTApitraceEncoderState *state) {
  if (!session || !state || !state.fenceOps.count)
    return;
  NSString *payload = dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_fence_ops",
    @"ops" : state.fenceOps,
  });
  apitrace_metal_record_fence_ops(session, encoder, payload.UTF8String);
  [state.fenceOps removeAllObjects];
}

static void
dxmt_apitrace_flush_blit_ops(apitrace_metal_session_t *session, obj_handle_t encoder, DXMTApitraceEncoderState *state) {
  if (!session || !state ||
      (!state.blitOps.count && !state.blitWaitFences.length && !state.blitUpdateFences.length))
    return;
  const uint64_t build_start_ns = dxmt_apitrace_monotonic_ns();
  const NSUInteger ops_count = state.blitOps.count;
  const NSUInteger wait_count = state.blitWaitFences.length / sizeof(uint64_t);
  const NSUInteger update_count = state.blitUpdateFences.length / sizeof(uint64_t);
  if (!ops_count) {
    dxmt_apitrace_record_blit_payload_stats(
        dxmt_apitrace_monotonic_ns() - build_start_ns,
        ops_count,
        wait_count,
        update_count,
        0);
    apitrace_metal_blit_fence_batch(
        session,
        encoder,
        (const uint64_t *)state.blitWaitFences.bytes,
        (uint32_t)wait_count,
        (const uint64_t *)state.blitUpdateFences.bytes,
        (uint32_t)update_count);
    [state.blitWaitFences setLength:0];
    [state.blitUpdateFences setLength:0];
    return;
  }
  NSString *ops_json = state.blitOps.count ? dxmt_apitrace_json_string(state.blitOps) : @"[]";
  NSMutableString *payload = [NSMutableString stringWithFormat:@"{\"kind\":\"dxmt_blit_batch\",\"ops\":%@", ops_json];
  if (state.blitWaitFences.length || state.blitUpdateFences.length) {
    [payload appendString:@",\"fence_ops\":{\"schema\":\"blit-fence-v2\",\"stages\":0"];
    const uint64_t *wait_fences = (const uint64_t *)state.blitWaitFences.bytes;
    [payload appendString:@",\"wait_fences\":["];
    for (NSUInteger i = 0; i < wait_count; ++i) {
      if (i)
        [payload appendString:@","];
      [payload appendFormat:@"%llu", (unsigned long long)wait_fences[i]];
    }
    [payload appendString:@"]"];
    const uint64_t *update_fences = (const uint64_t *)state.blitUpdateFences.bytes;
    [payload appendString:@",\"update_fences\":["];
    for (NSUInteger i = 0; i < update_count; ++i) {
      if (i)
        [payload appendString:@","];
      [payload appendFormat:@"%llu", (unsigned long long)update_fences[i]];
    }
    [payload appendString:@"]}"];
  }
  [payload appendString:@"}"];
  dxmt_apitrace_record_blit_payload_stats(
      dxmt_apitrace_monotonic_ns() - build_start_ns,
      ops_count,
      wait_count,
      update_count,
      [payload lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
  apitrace_metal_blit_batch(session, encoder, payload.UTF8String);
  [state.blitOps removeAllObjects];
  [state.blitWaitFences setLength:0];
  [state.blitUpdateFences setLength:0];
}

static DXMTApitraceCommandBufferState *
dxmt_apitrace_command_buffer_state_for_id(obj_handle_t command_buffer) {
  if (!command_buffer || !dxmt_apitrace_command_buffers)
    return nil;
  return [dxmt_apitrace_command_buffers objectForKey:dxmt_apitrace_key(command_buffer)];
}

static void
dxmt_apitrace_flush_command_buffer_blit_batch(apitrace_metal_session_t *session, obj_handle_t command_buffer) {
  DXMTApitraceCommandBufferState *state = dxmt_apitrace_command_buffer_state_for_id(command_buffer);
  if (!session || !state || (!state.blitCopyTextureOps.length && !state.blitWaitFences.length && !state.blitUpdateFences.length))
    return;

  const uint32_t ops_count = dxmt_apitrace_command_buffer_copy_texture_count(state);
  const NSUInteger wait_count = state.blitWaitFences.length / sizeof(uint64_t);
  const NSUInteger update_count = state.blitUpdateFences.length / sizeof(uint64_t);
  apitrace_metal_blit_encoder_copy_texture_ops_batch(
      session,
      command_buffer,
      command_buffer,
      (const apitrace_metal_copy_texture_op_t *)state.blitCopyTextureOps.bytes,
      ops_count,
      (const uint64_t *)state.blitWaitFences.bytes,
      (uint32_t)wait_count,
      (const uint64_t *)state.blitUpdateFences.bytes,
      (uint32_t)update_count);
  dxmt_apitrace_record_blit_payload_stats(0, ops_count, wait_count, update_count, 0);
  [state.blitCopyTextureOps setLength:0];
  [state.blitWaitFences setLength:0];
  [state.blitUpdateFences setLength:0];
}

static void
dxmt_apitrace_accumulate_command_buffer_blit_fences(obj_handle_t command_buffer, DXMTApitraceEncoderState *encoder_state) {
  DXMTApitraceCommandBufferState *command_state = dxmt_apitrace_command_buffer_state_for_id(command_buffer);
  if (!command_state || !encoder_state)
    return;
  if (encoder_state.blitWaitFences.length)
    [command_state.blitWaitFences appendData:encoder_state.blitWaitFences];
  if (encoder_state.blitUpdateFences.length)
    [command_state.blitUpdateFences appendData:encoder_state.blitUpdateFences];
  [encoder_state.blitWaitFences setLength:0];
  [encoder_state.blitUpdateFences setLength:0];
}

static void
dxmt_apitrace_accumulate_command_buffer_blit_copy_texture(
    apitrace_metal_session_t *session,
    obj_handle_t command_buffer,
    const struct wmtcmd_blit_copy_from_texture_to_texture *cmd) {
  DXMTApitraceCommandBufferState *state = dxmt_apitrace_command_buffer_state_for_id(command_buffer);
  if (!state || !cmd)
    return;
  if (dxmt_apitrace_command_buffer_copy_texture_count(state) >= dxmt_apitrace_command_buffer_blit_ops_flush_threshold)
    dxmt_apitrace_flush_command_buffer_blit_batch(session, command_buffer);
  apitrace_metal_copy_texture_op_t op = {
    .source_texture_id = cmd->src,
    .destination_texture_id = cmd->dst,
    .source_origin_x = cmd->src_origin.x,
    .source_origin_y = cmd->src_origin.y,
    .source_origin_z = cmd->src_origin.z,
    .source_size_width = cmd->src_size.width,
    .source_size_height = cmd->src_size.height,
    .source_size_depth = cmd->src_size.depth,
    .source_slice = cmd->src_slice,
    .source_level = cmd->src_level,
    .destination_origin_x = cmd->dst_origin.x,
    .destination_origin_y = cmd->dst_origin.y,
    .destination_origin_z = cmd->dst_origin.z,
    .destination_slice = cmd->dst_slice,
    .destination_level = cmd->dst_level,
  };
  [state.blitCopyTextureOps appendBytes:&op length:sizeof(op)];
}

static void
dxmt_apitrace_begin_blit_encoder_if_needed(apitrace_metal_session_t *session, obj_handle_t encoder, DXMTApitraceEncoderState *state) {
  if (!session || !state || state.kind != DXMTApitraceEncoderKindBlit || state.metalTraceBegan)
    return;
  state.beginSequence = apitrace_metal_blit_encoder_begin(session, encoder, state.commandBuffer, "{}");
  state.metalTraceBegan = YES;
}

static void
dxmt_apitrace_emit_blit_encoder_batch(apitrace_metal_session_t *session, obj_handle_t encoder, DXMTApitraceEncoderState *state) {
  if (!session || !state ||
      (!state.blitOps.count && !state.blitWaitFences.length && !state.blitUpdateFences.length))
    return;
  const uint64_t build_start_ns = dxmt_apitrace_monotonic_ns();
  const NSUInteger ops_count = state.blitOps.count;
  const NSUInteger wait_count = state.blitWaitFences.length / sizeof(uint64_t);
  const NSUInteger update_count = state.blitUpdateFences.length / sizeof(uint64_t);
  if (!ops_count) {
    const uint64_t *wait_fences = (const uint64_t *)state.blitWaitFences.bytes;
    const uint64_t *update_fences = (const uint64_t *)state.blitUpdateFences.bytes;
    dxmt_apitrace_record_blit_payload_stats(
        dxmt_apitrace_monotonic_ns() - build_start_ns,
        ops_count,
        wait_count,
        update_count,
        0);
    apitrace_metal_blit_encoder_fence_batch(
        session,
        encoder,
        state.commandBuffer,
        wait_fences,
        (uint32_t)wait_count,
        update_fences,
        (uint32_t)update_count);
    [state.blitWaitFences setLength:0];
    [state.blitUpdateFences setLength:0];
    return;
  }
  NSString *ops_json = state.blitOps.count ? dxmt_apitrace_json_string(state.blitOps) : @"[]";
  NSMutableString *payload = [NSMutableString stringWithFormat:
      @"{\"command_buffer_id\":%llu,\"kind\":\"dxmt_blit_encoder_batch\",\"ops\":%@",
      (unsigned long long)state.commandBuffer,
      ops_json];
  if (state.blitWaitFences.length || state.blitUpdateFences.length) {
    [payload appendString:@",\"fence_ops\":{\"schema\":\"blit-fence-v2\",\"stages\":0"];
    const uint64_t *wait_fences = (const uint64_t *)state.blitWaitFences.bytes;
    [payload appendString:@",\"wait_fences\":["];
    for (NSUInteger i = 0; i < wait_count; ++i) {
      if (i)
        [payload appendString:@","];
      [payload appendFormat:@"%llu", (unsigned long long)wait_fences[i]];
    }
    [payload appendString:@"]"];
    const uint64_t *update_fences = (const uint64_t *)state.blitUpdateFences.bytes;
    [payload appendString:@",\"update_fences\":["];
    for (NSUInteger i = 0; i < update_count; ++i) {
      if (i)
        [payload appendString:@","];
      [payload appendFormat:@"%llu", (unsigned long long)update_fences[i]];
    }
    [payload appendString:@"]}"];
  }
  [payload appendString:@"}"];
  dxmt_apitrace_record_blit_payload_stats(
      dxmt_apitrace_monotonic_ns() - build_start_ns,
      ops_count,
      wait_count,
      update_count,
      [payload lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
  apitrace_metal_blit_encoder_batch_with_command_buffer(session, encoder, state.commandBuffer, payload.UTF8String);
  [state.blitOps removeAllObjects];
  [state.blitWaitFences setLength:0];
  [state.blitUpdateFences setLength:0];
}

static NSString *
dxmt_apitrace_texture_descriptor_json(const struct WMTTextureInfo *info) {
  NSMutableDictionary *dict = [NSMutableDictionary dictionary];
  dict[@"type"] = @(info->type);
  dict[@"pixel_format"] = @(info->pixel_format);
  dict[@"width"] = @(info->width);
  dict[@"height"] = @(info->height);
  dict[@"depth"] = @(info->depth);
  dict[@"array_length"] = @(info->array_length);
  dict[@"mipmap_level_count"] = @(info->mipmap_level_count);
  dict[@"sample_count"] = @(info->sample_count);
  dict[@"usage"] = @(info->usage);
  dict[@"options"] = @(info->options);
  dict[@"gpu_resource_id"] = @(info->gpu_resource_id);
  return dxmt_apitrace_json_string(dict);
}

static NSString *
dxmt_apitrace_render_pipeline_json(const struct WMTRenderPipelineInfo *info) {
  NSMutableDictionary *dict = [NSMutableDictionary dictionary];
  NSDictionary *vertex_state = dxmt_apitrace_function_state_locked(info->vertex_function);
  NSDictionary *fragment_state = dxmt_apitrace_function_state_locked(info->fragment_function);
  dict[@"rasterization_enabled"] = @(info->rasterization_enabled);
  dict[@"raster_sample_count"] = @(info->raster_sample_count);
  dict[@"depth_pixel_format"] = @(info->depth_pixel_format);
  dict[@"stencil_pixel_format"] = @(info->stencil_pixel_format);
  dict[@"color_pixel_format"] = @(info->colors[0].pixel_format);
  dict[@"alpha_to_coverage_enabled"] = @(info->alpha_to_coverage_enabled);
  dict[@"logic_operation_enabled"] = @(info->logic_operation_enabled);
  dict[@"logic_operation"] = @(info->logic_operation);
  dict[@"input_primitive_topology"] = @(info->input_primitive_topology);
  dict[@"immutable_vertex_buffers"] = @(info->immutable_vertex_buffers);
  dict[@"immutable_fragment_buffers"] = @(info->immutable_fragment_buffers);
  NSMutableArray *colors = [NSMutableArray arrayWithCapacity:8];
  for (unsigned i = 0; i < 8; i++) {
    const struct WMTColorAttachmentBlendInfo *color = &info->colors[i];
    [colors addObject:@{
      @"pixel_format" : @(color->pixel_format),
      @"blending_enabled" : @(color->blending_enabled),
      @"write_mask" : @(color->write_mask),
      @"rgb_blend_operation" : @(color->rgb_blend_operation),
      @"alpha_blend_operation" : @(color->alpha_blend_operation),
      @"src_rgb_blend_factor" : @(color->src_rgb_blend_factor),
      @"dst_rgb_blend_factor" : @(color->dst_rgb_blend_factor),
      @"src_alpha_blend_factor" : @(color->src_alpha_blend_factor),
      @"dst_alpha_blend_factor" : @(color->dst_alpha_blend_factor),
    }];
  }
  dict[@"colors"] = colors;
  if (vertex_state) {
    dict[@"vertex_library_id"] = vertex_state[@"library_id"];
    dict[@"vertex_function"] = vertex_state[@"function_name"];
  }
  if (fragment_state) {
    dict[@"fragment_library_id"] = fragment_state[@"library_id"];
    dict[@"fragment_function"] = fragment_state[@"function_name"];
    if (fragment_state[@"function_constants"])
      dict[@"fragment_function_constants"] = fragment_state[@"function_constants"];
  }
  return dxmt_apitrace_json_string(dict);
}

static NSString *
dxmt_apitrace_mesh_render_pipeline_json(const struct WMTMeshRenderPipelineInfo *info) {
  NSMutableDictionary *dict = [NSMutableDictionary dictionary];
  NSDictionary *object_state = dxmt_apitrace_function_state_locked(info->object_function);
  NSDictionary *mesh_state = dxmt_apitrace_function_state_locked(info->mesh_function);
  NSDictionary *fragment_state = dxmt_apitrace_function_state_locked(info->fragment_function);
  dict[@"pipeline_kind"] = @"mesh_render";
  dict[@"rasterization_enabled"] = @(info->rasterization_enabled);
  dict[@"raster_sample_count"] = @(info->raster_sample_count);
  dict[@"depth_pixel_format"] = @(info->depth_pixel_format);
  dict[@"stencil_pixel_format"] = @(info->stencil_pixel_format);
  dict[@"color_pixel_format"] = @(info->colors[0].pixel_format);
  dict[@"alpha_to_coverage_enabled"] = @(info->alpha_to_coverage_enabled);
  dict[@"logic_operation_enabled"] = @(info->logic_operation_enabled);
  dict[@"logic_operation"] = @(info->logic_operation);
  dict[@"immutable_object_buffers"] = @(info->immutable_object_buffers);
  dict[@"immutable_mesh_buffers"] = @(info->immutable_mesh_buffers);
  dict[@"immutable_fragment_buffers"] = @(info->immutable_fragment_buffers);
  dict[@"payload_memory_length"] = @(info->payload_memory_length);
  dict[@"mesh_tgsize_is_multiple_of_sgwidth"] = @(info->mesh_tgsize_is_multiple_of_sgwidth);
  dict[@"object_tgsize_is_multiple_of_sgwidth"] = @(info->object_tgsize_is_multiple_of_sgwidth);
  NSMutableArray *colors = [NSMutableArray arrayWithCapacity:8];
  for (unsigned i = 0; i < 8; i++) {
    const struct WMTColorAttachmentBlendInfo *color = &info->colors[i];
    [colors addObject:@{
      @"pixel_format" : @(color->pixel_format),
      @"blending_enabled" : @(color->blending_enabled),
      @"write_mask" : @(color->write_mask),
      @"rgb_blend_operation" : @(color->rgb_blend_operation),
      @"alpha_blend_operation" : @(color->alpha_blend_operation),
      @"src_rgb_blend_factor" : @(color->src_rgb_blend_factor),
      @"dst_rgb_blend_factor" : @(color->dst_rgb_blend_factor),
      @"src_alpha_blend_factor" : @(color->src_alpha_blend_factor),
      @"dst_alpha_blend_factor" : @(color->dst_alpha_blend_factor),
    }];
  }
  dict[@"colors"] = colors;
  if (object_state) {
    dict[@"object_library_id"] = object_state[@"library_id"];
    dict[@"object_function"] = object_state[@"function_name"];
    if (object_state[@"function_constants"])
      dict[@"object_function_constants"] = object_state[@"function_constants"];
  }
  if (mesh_state) {
    dict[@"mesh_library_id"] = mesh_state[@"library_id"];
    dict[@"mesh_function"] = mesh_state[@"function_name"];
    if (mesh_state[@"function_constants"])
      dict[@"mesh_function_constants"] = mesh_state[@"function_constants"];
  }
  if (fragment_state) {
    dict[@"fragment_library_id"] = fragment_state[@"library_id"];
    dict[@"fragment_function"] = fragment_state[@"function_name"];
    if (fragment_state[@"function_constants"])
      dict[@"fragment_function_constants"] = fragment_state[@"function_constants"];
  }
  return dxmt_apitrace_json_string(dict);
}

static NSString *
dxmt_apitrace_tile_render_pipeline_json(const struct WMTTileRenderPipelineInfo *info) {
  NSMutableDictionary *dict = [NSMutableDictionary dictionary];
  NSDictionary *tile_state = dxmt_apitrace_function_state_locked(info->tile_function);
  dict[@"pipeline_kind"] = @"tile_render";
  dict[@"raster_sample_count"] = @(info->raster_sample_count);
  dict[@"tgsize_matches_tile_size"] = @(info->tgsize_matches_tile_size);
  dict[@"immutable_tile_buffers"] = @(info->immutable_tile_buffers);
  NSMutableArray *colors = [NSMutableArray arrayWithCapacity:8];
  for (unsigned i = 0; i < 8; i++) {
    [colors addObject:@{@"pixel_format" : @(info->color_formats[i])}];
  }
  dict[@"colors"] = colors;
  if (tile_state) {
    dict[@"tile_library_id"] = tile_state[@"library_id"];
    dict[@"tile_function"] = tile_state[@"function_name"];
    if (tile_state[@"function_constants"])
      dict[@"tile_function_constants"] = tile_state[@"function_constants"];
  }
  return dxmt_apitrace_json_string(dict);
}

static NSDictionary *
dxmt_apitrace_stencil_json(const struct WMTStencilInfo *info) {
  return @{
    @"enabled" : @(info->enabled),
    @"depth_stencil_pass_op" : @(info->depth_stencil_pass_op),
    @"stencil_fail_op" : @(info->stencil_fail_op),
    @"depth_fail_op" : @(info->depth_fail_op),
    @"stencil_compare_function" : @(info->stencil_compare_function),
    @"write_mask" : @(info->write_mask),
    @"read_mask" : @(info->read_mask),
  };
}

static NSString *
dxmt_apitrace_depth_stencil_json(const struct WMTDepthStencilInfo *info, obj_handle_t dsso) {
  return dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_depth_stencil_state",
    @"depth_stencil_state_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)dsso],
    @"depth_compare_function" : @(info->depth_compare_function),
    @"depth_write_enabled" : @(info->depth_write_enabled),
    @"front_stencil" : dxmt_apitrace_stencil_json(&info->front_stencil),
    @"back_stencil" : dxmt_apitrace_stencil_json(&info->back_stencil),
  });
}

static NSString *
dxmt_apitrace_set_rasterizer_state_json(const struct wmtcmd_render_setrasterizerstate *cmd) {
  return dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_set_rasterizer_state",
    @"fill_mode" : @(cmd->fill_mode),
    @"cull_mode" : @(cmd->cull_mode),
    @"depth_clip_mode" : @(cmd->depth_clip_mode),
    @"winding" : @(cmd->winding),
    @"depth_bias" : @(cmd->depth_bias),
    @"slope_scale" : @(cmd->scole_scale),
    @"depth_bias_clamp" : @(cmd->depth_bias_clamp),
  });
}

static NSString *
dxmt_apitrace_set_depth_stencil_state_json(const struct wmtcmd_render_setdsso *cmd) {
  return dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_set_depth_stencil_state",
    @"depth_stencil_state_id" : dxmt_apitrace_optional_handle(cmd->dsso),
    @"stencil_ref" : @(cmd->stencil_ref),
  });
}

static NSString *
dxmt_apitrace_set_blend_factor_json(const struct wmtcmd_render_setblendcolor *cmd) {
  return dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_set_blend_factor",
    @"red" : @(cmd->red),
    @"green" : @(cmd->green),
    @"blue" : @(cmd->blue),
    @"alpha" : @(cmd->alpha),
    @"stencil_ref" : @(cmd->stencil_ref),
  });
}

static NSArray *
dxmt_apitrace_viewports_json(const struct WMTViewport *viewports, uint8_t count) {
  NSMutableArray *values = [NSMutableArray arrayWithCapacity:count];
  for (uint8_t i = 0; i < count; ++i) {
    const struct WMTViewport *viewport = &viewports[i];
    [values addObject:@[
      @(viewport->originX),
      @(viewport->originY),
      @(viewport->width),
      @(viewport->height),
      @(viewport->znear),
      @(viewport->zfar),
    ]];
  }
  return values;
}

static NSArray *
dxmt_apitrace_scissor_rects_json(const struct WMTScissorRect *rects, uint8_t count) {
  NSMutableArray *values = [NSMutableArray arrayWithCapacity:count];
  for (uint8_t i = 0; i < count; ++i) {
    const struct WMTScissorRect *rect = &rects[i];
    [values addObject:@[
      @(rect->x),
      @(rect->y),
      @(rect->width),
      @(rect->height),
    ]];
  }
  return values;
}

static NSString *
dxmt_apitrace_set_viewports_json(const struct WMTViewport *viewports, uint8_t count) {
  return dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_set_viewports",
    @"viewports" : dxmt_apitrace_viewports_json(viewports, count),
  });
}

static NSString *
dxmt_apitrace_set_scissor_rects_json(const struct WMTScissorRect *rects, uint8_t count) {
  return dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_set_scissor_rects",
    @"rects" : dxmt_apitrace_scissor_rects_json(rects, count),
  });
}

static NSString *
dxmt_apitrace_compute_pipeline_json(const struct WMTComputePipelineInfo *info) {
  NSMutableDictionary *dict = [NSMutableDictionary dictionary];
  NSDictionary *function_state = dxmt_apitrace_function_state_locked(info->compute_function);
  dict[@"immutable_buffers"] = @(info->immutable_buffers);
  dict[@"tgsize_is_multiple_of_sgwidth"] = @(info->tgsize_is_multiple_of_sgwidth);
  dict[@"fail_on_binary_archive_miss"] = @(info->fail_on_binary_archive_miss);
  dict[@"num_binary_archives_for_lookup"] = @(info->num_binary_archives_for_lookup);
  if (function_state) {
    dict[@"library_id"] = function_state[@"library_id"];
    dict[@"function"] = function_state[@"function_name"];
  }
  return dxmt_apitrace_json_string(dict);
}

static NSString *
dxmt_apitrace_render_pass_json(const struct WMTRenderPassInfo *info) {
  NSMutableDictionary *dict = [NSMutableDictionary dictionary];
  NSMutableArray *colors = [NSMutableArray array];
  for (unsigned i = 0; i < 8; i++) {
    if (!info->colors[i].texture && !info->colors[i].resolve_texture)
      continue;
    NSMutableDictionary *color = [NSMutableDictionary dictionary];
    color[@"slot"] = @(i);
    color[@"texture"] = dxmt_apitrace_optional_handle(info->colors[i].texture);
    color[@"resolve_texture"] = dxmt_apitrace_optional_handle(info->colors[i].resolve_texture);
    color[@"load_action"] = @(info->colors[i].load_action);
    color[@"store_action"] = @(info->colors[i].store_action);
    color[@"level"] = @(info->colors[i].level);
    color[@"slice"] = @(info->colors[i].slice);
    color[@"depth_plane"] = @(info->colors[i].depth_plane);
    color[@"clear_color"] = @[ @(info->colors[i].clear_color.r),
                               @(info->colors[i].clear_color.g),
                               @(info->colors[i].clear_color.b),
                               @(info->colors[i].clear_color.a) ];
    [colors addObject:color];
  }
  dict[@"colors"] = colors;
  if ([colors count] > 0) {
    NSDictionary *first_color = [colors objectAtIndex:0];
    id texture = first_color[@"texture"];
    if (texture && texture != [NSNull null])
      dict[@"color_texture_id"] = texture;
    dict[@"load_action"] = first_color[@"load_action"];
    dict[@"store_action"] = first_color[@"store_action"];
    dict[@"clear_color"] = first_color[@"clear_color"];
  }
  dict[@"default_raster_sample_count"] = @(info->default_raster_sample_count);
  dict[@"render_target_array_length"] = @(info->render_target_array_length);
  dict[@"tile_width"] = @(info->tile_width);
  dict[@"tile_height"] = @(info->tile_height);
  dict[@"render_target_width"] = @(info->render_target_width);
  dict[@"render_target_height"] = @(info->render_target_height);
  if (info->depth.texture) {
    dict[@"depth"] = @{
      @"texture" : dxmt_apitrace_optional_handle(info->depth.texture),
      @"load_action" : @(info->depth.load_action),
      @"store_action" : @(info->depth.store_action),
      @"level" : @(info->depth.level),
      @"slice" : @(info->depth.slice),
      @"depth_plane" : @(info->depth.depth_plane),
      @"clear_depth" : @(info->depth.clear_depth),
    };
  }
  if (info->stencil.texture) {
    dict[@"stencil"] = @{
      @"texture" : dxmt_apitrace_optional_handle(info->stencil.texture),
      @"load_action" : @(info->stencil.load_action),
      @"store_action" : @(info->stencil.store_action),
      @"level" : @(info->stencil.level),
      @"slice" : @(info->stencil.slice),
      @"depth_plane" : @(info->stencil.depth_plane),
      @"clear_stencil" : @(info->stencil.clear_stencil),
    };
  }
  if (info->visibility_buffer)
    dict[@"visibility_buffer"] = dxmt_apitrace_optional_handle(info->visibility_buffer);
  return dxmt_apitrace_json_string(dict);
}

static NSString *
dxmt_apitrace_copy_texture_json(
    obj_handle_t source_texture,
    obj_handle_t destination_texture,
    const struct WMTOrigin *source_origin,
    const struct WMTSize *source_size,
    uint32_t source_slice,
    uint32_t source_level,
    const struct WMTOrigin *destination_origin,
    uint32_t destination_slice,
    uint32_t destination_level) {
  NSMutableDictionary *dict = [NSMutableDictionary dictionary];
  dict[@"source_texture"] = dxmt_apitrace_optional_handle(source_texture);
  dict[@"destination_texture"] = dxmt_apitrace_optional_handle(destination_texture);
  if (source_origin) {
    dict[@"source_origin"] = @[ @(source_origin->x), @(source_origin->y), @(source_origin->z) ];
  }
  if (source_size) {
    dict[@"source_size"] = @[ @(source_size->width), @(source_size->height), @(source_size->depth) ];
  }
  dict[@"source_slice"] = @(source_slice);
  dict[@"source_level"] = @(source_level);
  if (destination_origin) {
    dict[@"destination_origin"] = @[ @(destination_origin->x), @(destination_origin->y), @(destination_origin->z) ];
  }
  dict[@"destination_slice"] = @(destination_slice);
  dict[@"destination_level"] = @(destination_level);
  return dxmt_apitrace_json_string(dict);
}

static NSString *
dxmt_apitrace_copy_buffer_to_texture_json(
    obj_handle_t source_buffer,
    uint64_t source_offset,
    uint32_t source_bytes_per_row,
    uint32_t source_bytes_per_image,
    const struct WMTSize *source_size,
    obj_handle_t destination_texture,
    uint32_t destination_slice,
    uint32_t destination_level,
    const struct WMTOrigin *destination_origin) {
  NSMutableDictionary *dict = [NSMutableDictionary dictionary];
  dict[@"kind"] = @"dxmt_copy_buffer_to_texture";
  dict[@"source_buffer"] = dxmt_apitrace_optional_handle(source_buffer);
  dict[@"source_offset"] = @(source_offset);
  dict[@"source_bytes_per_row"] = @(source_bytes_per_row);
  dict[@"source_bytes_per_image"] = @(source_bytes_per_image);
  if (source_size) {
    dict[@"source_size"] = @[ @(source_size->width), @(source_size->height), @(source_size->depth) ];
  }
  dict[@"destination_texture"] = dxmt_apitrace_optional_handle(destination_texture);
  dict[@"destination_slice"] = @(destination_slice);
  dict[@"destination_level"] = @(destination_level);
  if (destination_origin) {
    dict[@"destination_origin"] = @[ @(destination_origin->x), @(destination_origin->y), @(destination_origin->z) ];
  }
  return dxmt_apitrace_json_string(dict);
}

static NSString *
dxmt_apitrace_sampler_descriptor_json(const struct WMTSamplerInfo *info, obj_handle_t sampler) {
  return dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_sampler_gpu_resource_id",
    @"sampler_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)sampler],
    @"gpu_resource_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)info->gpu_resource_id],
    @"border_color" : @(info->border_color),
    @"r_address_mode" : @(info->r_address_mode),
    @"s_address_mode" : @(info->s_address_mode),
    @"t_address_mode" : @(info->t_address_mode),
    @"mag_filter" : @(info->mag_filter),
    @"min_filter" : @(info->min_filter),
    @"mip_filter" : @(info->mip_filter),
    @"compare_function" : @(info->compare_function),
    @"lod_max_clamp" : @(info->lod_max_clamp),
    @"lod_min_clamp" : @(info->lod_min_clamp),
    @"max_anisotropy" : @(info->max_anisotroy),
    @"lod_average" : @(info->lod_average),
    @"normalized_coordinates" : @(info->normalized_coords),
    @"support_argument_buffers" : @(info->support_argument_buffers),
  });
}

static NSString *
dxmt_apitrace_texture_view_json(const struct unixcall_mtltexture_newtextureview *params) {
  return dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_texture_view",
    @"texture_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)params->ret],
    @"source_texture_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)params->texture],
    @"gpu_resource_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)params->gpu_resource_id],
    @"pixel_format" : @(params->format),
    @"texture_type" : @(params->texture_type),
    @"level_start" : @(params->level_start),
    @"level_count" : @(params->level_count),
    @"slice_start" : @(params->slice_start),
    @"slice_count" : @(params->slice_count),
    @"swizzle" : @[ @(params->swizzle.r), @(params->swizzle.g), @(params->swizzle.b), @(params->swizzle.a) ],
  });
}

static NSString *
dxmt_apitrace_memory_barrier_json(uint32_t scope, uint32_t stages_before, uint32_t stages_after) {
  return dxmt_apitrace_json_string(@{
    @"scope" : @(scope),
    @"stages_before" : @(stages_before),
    @"stages_after" : @(stages_after),
  });
}

static NSString *
dxmt_apitrace_dispatch_threads_json(const struct WMTSize *size, const struct WMTSize *threadgroup_size) {
  return dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_dispatch_threads",
    @"width" : @(size ? size->width : 1),
    @"height" : @(size ? size->height : 1),
    @"depth" : @(size ? size->depth : 1),
    @"threads_per_group_width" : @(threadgroup_size ? threadgroup_size->width : 1),
    @"threads_per_group_height" : @(threadgroup_size ? threadgroup_size->height : 1),
    @"threads_per_group_depth" : @(threadgroup_size ? threadgroup_size->depth : 1),
  });
}

static NSString *
dxmt_apitrace_bytes_json(const void *bytes, size_t length) {
  NSMutableArray *values = [NSMutableArray arrayWithCapacity:length];
  const uint8_t *raw = bytes;
  for (size_t i = 0; i < length; ++i) {
    [values addObject:@(raw ? raw[i] : 0)];
  }
  return dxmt_apitrace_json_string(@{ @"length" : @(length), @"bytes" : values });
}

static void
dxmt_apitrace_snapshot_buffer(apitrace_metal_session_t *session, obj_handle_t buffer_handle, NSMutableSet *seen) {
  if (!session || !buffer_handle)
    return;
  NSNumber *key = dxmt_apitrace_key(buffer_handle);
  if (seen && [seen containsObject:key])
    return;
  if (seen)
    [seen addObject:key];

  id<MTLBuffer> buffer = (id<MTLBuffer>)buffer_handle;
  if ([buffer storageMode] == MTLStorageModePrivate || ![buffer contents])
    return;

  NSString *address_payload = dxmt_apitrace_json_string(@{
    @"kind" : @"dxmt_buffer_gpu_address",
    @"buffer_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)buffer_handle],
    @"gpu_address" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)[buffer gpuAddress]],
  });
  apitrace_metal_object_metadata(session, buffer_handle, address_payload.UTF8String);
}

static void
dxmt_apitrace_record_render_commands(apitrace_metal_session_t *session, obj_handle_t encoder, const struct wmtcmd_base *head) {
  DXMTApitraceEncoderState *state = [dxmt_apitrace_encoders objectForKey:dxmt_apitrace_key(encoder)];
  for (const struct wmtcmd_base *base = head; base; base = (const struct wmtcmd_base *)base->next.ptr) {
    switch ((enum WMTRenderCommandType)base->type) {
    case WMTRenderCommandSetPSO: {
      const struct wmtcmd_render_setpso *cmd = (const struct wmtcmd_render_setpso *)base;
      apitrace_metal_set_render_pipeline_state(session, encoder, cmd->pso);
      break;
    }
    case WMTRenderCommandSetVertexBuffer: {
      const struct wmtcmd_render_setbuffer *cmd = (const struct wmtcmd_render_setbuffer *)base;
      apitrace_metal_set_vertex_buffer(session, encoder, cmd->buffer, cmd->offset, cmd->index);
      break;
    }
    case WMTRenderCommandSetFragmentBuffer: {
      const struct wmtcmd_render_setbuffer *cmd = (const struct wmtcmd_render_setbuffer *)base;
      apitrace_metal_set_fragment_buffer(session, encoder, cmd->buffer, cmd->offset, cmd->index);
      break;
    }
    case WMTRenderCommandSetVertexBufferOffset: {
      const struct wmtcmd_render_setbufferoffset *cmd = (const struct wmtcmd_render_setbufferoffset *)base;
      apitrace_metal_set_vertex_buffer_offset(session, encoder, cmd->offset, cmd->index);
      break;
    }
    case WMTRenderCommandSetFragmentBufferOffset: {
      const struct wmtcmd_render_setbufferoffset *cmd = (const struct wmtcmd_render_setbufferoffset *)base;
      apitrace_metal_set_fragment_buffer_offset(session, encoder, cmd->offset, cmd->index);
      break;
    }
    case WMTRenderCommandSetFragmentTexture: {
      const struct wmtcmd_render_settexture *cmd = (const struct wmtcmd_render_settexture *)base;
      apitrace_metal_set_fragment_texture(session, encoder, cmd->texture, cmd->index);
      break;
    }
    case WMTRenderCommandSetFragmentBytes: {
      const struct wmtcmd_render_setbytes *cmd = (const struct wmtcmd_render_setbytes *)base;
      NSString *payload = dxmt_apitrace_bytes_json(cmd->bytes.ptr, cmd->length);
      apitrace_metal_set_fragment_bytes(session, encoder, cmd->index, payload.UTF8String);
      break;
    }
    case WMTRenderCommandSetRasterizerState: {
      const struct wmtcmd_render_setrasterizerstate *cmd = (const struct wmtcmd_render_setrasterizerstate *)base;
      NSString *payload = dxmt_apitrace_set_rasterizer_state_json(cmd);
      apitrace_metal_encoder_state(session, encoder, payload.UTF8String);
      break;
    }
    case WMTRenderCommandSetDSSO: {
      const struct wmtcmd_render_setdsso *cmd = (const struct wmtcmd_render_setdsso *)base;
      NSString *payload = dxmt_apitrace_set_depth_stencil_state_json(cmd);
      apitrace_metal_encoder_state(session, encoder, payload.UTF8String);
      break;
    }
    case WMTRenderCommandSetBlendFactorAndStencilRef: {
      const struct wmtcmd_render_setblendcolor *cmd = (const struct wmtcmd_render_setblendcolor *)base;
      NSString *payload = dxmt_apitrace_set_blend_factor_json(cmd);
      apitrace_metal_encoder_state(session, encoder, payload.UTF8String);
      break;
    }
    case WMTRenderCommandSetViewports: {
      const struct wmtcmd_render_setviewports *cmd = (const struct wmtcmd_render_setviewports *)base;
      NSString *payload = dxmt_apitrace_set_viewports_json(cmd->viewports.ptr, cmd->viewport_count);
      apitrace_metal_encoder_state(session, encoder, payload.UTF8String);
      break;
    }
    case WMTRenderCommandSetScissorRects: {
      const struct wmtcmd_render_setscissorrects *cmd = (const struct wmtcmd_render_setscissorrects *)base;
      NSString *payload = dxmt_apitrace_set_scissor_rects_json(cmd->scissor_rects.ptr, cmd->rect_count);
      apitrace_metal_encoder_state(session, encoder, payload.UTF8String);
      break;
    }
    case WMTRenderCommandSetViewport: {
      const struct wmtcmd_render_setviewport *cmd = (const struct wmtcmd_render_setviewport *)base;
      NSString *payload = dxmt_apitrace_set_viewports_json(&cmd->viewport, 1);
      apitrace_metal_encoder_state(session, encoder, payload.UTF8String);
      break;
    }
    case WMTRenderCommandSetScissorRect: {
      const struct wmtcmd_render_setscissorrect *cmd = (const struct wmtcmd_render_setscissorrect *)base;
      NSString *payload = dxmt_apitrace_set_scissor_rects_json(&cmd->scissor_rect, 1);
      apitrace_metal_encoder_state(session, encoder, payload.UTF8String);
      break;
    }
    case WMTRenderCommandUseResource: {
      const struct wmtcmd_render_useresource *cmd = (const struct wmtcmd_render_useresource *)base;
      if ([(id)cmd->resource conformsToProtocol:@protocol(MTLBuffer)])
        dxmt_apitrace_snapshot_buffer(session, cmd->resource, NULL);
      apitrace_metal_use_resource(session, encoder, cmd->resource, cmd->usage, cmd->stages);
      break;
    }
    case WMTRenderCommandDraw: {
      const struct wmtcmd_render_draw *cmd = (const struct wmtcmd_render_draw *)base;
      apitrace_metal_draw_primitives(
          session, encoder, cmd->primitive_type, (uint32_t)cmd->vertex_start, (uint32_t)cmd->vertex_count,
          cmd->instance_count, cmd->base_instance);
      break;
    }
    case WMTRenderCommandDrawIndexed: {
      const struct wmtcmd_render_draw_indexed *cmd = (const struct wmtcmd_render_draw_indexed *)base;
      apitrace_metal_draw_indexed_primitives(
          session, encoder, cmd->primitive_type, (uint32_t)cmd->index_count, cmd->index_type,
          cmd->index_buffer, cmd->index_buffer_offset, cmd->instance_count, cmd->base_vertex, cmd->base_instance);
      break;
    }
    case WMTRenderCommandDrawIndirect: {
      const struct wmtcmd_render_draw_indirect *cmd = (const struct wmtcmd_render_draw_indirect *)base;
      apitrace_metal_draw_primitives_indirect(
          session, encoder, cmd->primitive_type, cmd->indirect_args_buffer, cmd->indirect_args_offset);
      break;
    }
    case WMTRenderCommandDrawIndexedIndirect: {
      const struct wmtcmd_render_draw_indexed_indirect *cmd = (const struct wmtcmd_render_draw_indexed_indirect *)base;
      apitrace_metal_draw_indexed_primitives_indirect(
          session, encoder, cmd->primitive_type, cmd->index_type, cmd->index_buffer, cmd->index_buffer_offset,
          cmd->indirect_args_buffer, cmd->indirect_args_offset);
      break;
    }
    case WMTRenderCommandWaitForFence:
    case WMTRenderCommandUpdateFence: {
      const struct wmtcmd_render_fence_op *cmd = (const struct wmtcmd_render_fence_op *)base;
      if (base->type == WMTRenderCommandWaitForFence)
        dxmt_apitrace_append_fence_op(state, "wait", cmd->fence, cmd->stages);
      else
        dxmt_apitrace_append_fence_op(state, "update", cmd->fence, cmd->stages);
      break;
    }
    case WMTRenderCommandMemoryBarrier: {
      const struct wmtcmd_render_memory_barrier *cmd = (const struct wmtcmd_render_memory_barrier *)base;
      NSString *payload = dxmt_apitrace_memory_barrier_json(cmd->scope, cmd->stages_before, cmd->stages_after);
      apitrace_metal_memory_barrier(session, encoder, payload.UTF8String);
      break;
    }
    default:
      break;
    }
  }
}

static void
dxmt_apitrace_record_compute_commands(apitrace_metal_session_t *session, obj_handle_t encoder, const struct wmtcmd_base *head) {
  DXMTApitraceEncoderState *state = [dxmt_apitrace_encoders objectForKey:dxmt_apitrace_key(encoder)];
  struct WMTSize threadgroup_size = {1, 1, 1};
  for (const struct wmtcmd_base *base = head; base; base = (const struct wmtcmd_base *)base->next.ptr) {
    switch ((enum WMTComputeCommandType)base->type) {
    case WMTComputeCommandSetPSO: {
      const struct wmtcmd_compute_setpso *cmd = (const struct wmtcmd_compute_setpso *)base;
      threadgroup_size = cmd->threadgroup_size;
      apitrace_metal_set_compute_pipeline_state(session, encoder, cmd->pso);
      break;
    }
    case WMTComputeCommandSetBuffer: {
      const struct wmtcmd_compute_setbuffer *cmd = (const struct wmtcmd_compute_setbuffer *)base;
      apitrace_metal_set_compute_buffer(session, encoder, cmd->buffer, cmd->offset, cmd->index);
      break;
    }
    case WMTComputeCommandSetBufferOffset: {
      const struct wmtcmd_compute_setbufferoffset *cmd = (const struct wmtcmd_compute_setbufferoffset *)base;
      apitrace_metal_set_compute_buffer_offset(session, encoder, cmd->offset, cmd->index);
      break;
    }
    case WMTComputeCommandSetBytes: {
      const struct wmtcmd_compute_setbytes *cmd = (const struct wmtcmd_compute_setbytes *)base;
      NSMutableDictionary *dict = [NSMutableDictionary dictionary];
      dict[@"kind"] = @"dxmt_set_compute_bytes";
      dict[@"index"] = @(cmd->index);
      dict[@"length"] = @(cmd->length);
      NSMutableArray *bytes = [NSMutableArray arrayWithCapacity:cmd->length];
      const uint8_t *raw = cmd->bytes.ptr;
      for (uint64_t i = 0; i < cmd->length; ++i)
        [bytes addObject:@(raw ? raw[i] : 0)];
      dict[@"bytes"] = bytes;
      NSString *payload = dxmt_apitrace_json_string(dict);
      apitrace_metal_set_compute_bytes(session, encoder, payload.UTF8String);
      break;
    }
    case WMTComputeCommandSetTexture: {
      const struct wmtcmd_compute_settexture *cmd = (const struct wmtcmd_compute_settexture *)base;
      apitrace_metal_set_compute_texture(session, encoder, cmd->texture, cmd->index);
      break;
    }
    case WMTComputeCommandUseResource: {
      const struct wmtcmd_compute_useresource *cmd = (const struct wmtcmd_compute_useresource *)base;
      if ([(id)cmd->resource conformsToProtocol:@protocol(MTLBuffer)])
        dxmt_apitrace_snapshot_buffer(session, cmd->resource, NULL);
      apitrace_metal_use_resource(session, encoder, cmd->resource, cmd->usage, 0);
      break;
    }
    case WMTComputeCommandDispatch: {
      const struct wmtcmd_compute_dispatch *cmd = (const struct wmtcmd_compute_dispatch *)base;
      apitrace_metal_dispatch_threadgroups(
          session, encoder, (uint32_t)cmd->size.width, (uint32_t)cmd->size.height, (uint32_t)cmd->size.depth,
          (uint32_t)threadgroup_size.width, (uint32_t)threadgroup_size.height, (uint32_t)threadgroup_size.depth);
      break;
    }
    case WMTComputeCommandDispatchThreads: {
      const struct wmtcmd_compute_dispatch *cmd = (const struct wmtcmd_compute_dispatch *)base;
      NSString *payload = dxmt_apitrace_dispatch_threads_json(&cmd->size, &threadgroup_size);
      apitrace_metal_dispatch_threads(session, encoder, payload.UTF8String);
      break;
    }
    case WMTComputeCommandDispatchIndirect: {
      const struct wmtcmd_compute_dispatch_indirect *cmd = (const struct wmtcmd_compute_dispatch_indirect *)base;
      apitrace_metal_dispatch_threadgroups_indirect(
          session, encoder, cmd->indirect_args_buffer, cmd->indirect_args_offset,
          (uint32_t)threadgroup_size.width, (uint32_t)threadgroup_size.height, (uint32_t)threadgroup_size.depth);
      break;
    }
    case WMTComputeCommandWaitForFence:
    case WMTComputeCommandUpdateFence: {
      const struct wmtcmd_compute_fence_op *cmd = (const struct wmtcmd_compute_fence_op *)base;
      if (base->type == WMTComputeCommandWaitForFence)
        dxmt_apitrace_append_fence_op(state, "wait", cmd->fence, 0);
      else
        dxmt_apitrace_append_fence_op(state, "update", cmd->fence, 0);
      break;
    }
    case WMTComputeCommandMemoryBarrier: {
      const struct wmtcmd_compute_memory_barrier *cmd = (const struct wmtcmd_compute_memory_barrier *)base;
      NSString *payload = dxmt_apitrace_memory_barrier_json(cmd->scope, 0, 0);
      apitrace_metal_memory_barrier(session, encoder, payload.UTF8String);
      break;
    }
    default:
      break;
    }
  }
}

static void
dxmt_apitrace_record_blit_commands(apitrace_metal_session_t *session, obj_handle_t encoder, const struct wmtcmd_base *head) {
  DXMTApitraceEncoderState *state = [dxmt_apitrace_encoders objectForKey:dxmt_apitrace_key(encoder)];
  for (const struct wmtcmd_base *base = head; base; base = (const struct wmtcmd_base *)base->next.ptr) {
    switch ((enum WMTBlitCommandType)base->type) {
    case WMTBlitCommandCopyFromBufferToBuffer: {
      dxmt_apitrace_begin_blit_encoder_if_needed(session, encoder, state);
      dxmt_apitrace_flush_blit_ops(session, encoder, state);
      dxmt_apitrace_flush_command_buffer_blit_batch(session, state.commandBuffer);
      const struct wmtcmd_blit_copy_from_buffer_to_buffer *cmd = (const struct wmtcmd_blit_copy_from_buffer_to_buffer *)base;
      apitrace_metal_copy_buffer(
          session, encoder, cmd->src, cmd->src_offset, cmd->dst, cmd->dst_offset, cmd->copy_length);
      break;
    }
    case WMTBlitCommandCopyFromBufferToTexture: {
      dxmt_apitrace_begin_blit_encoder_if_needed(session, encoder, state);
      dxmt_apitrace_flush_blit_ops(session, encoder, state);
      dxmt_apitrace_flush_command_buffer_blit_batch(session, state.commandBuffer);
      const struct wmtcmd_blit_copy_from_buffer_to_texture *cmd = (const struct wmtcmd_blit_copy_from_buffer_to_texture *)base;
      NSString *payload = dxmt_apitrace_copy_buffer_to_texture_json(
          cmd->src, cmd->src_offset, cmd->bytes_per_row, cmd->bytes_per_image, &cmd->size, cmd->dst, cmd->slice,
          cmd->level, &cmd->origin);
      apitrace_metal_copy_buffer_to_texture(session, encoder, cmd->src, cmd->dst, payload.UTF8String);
      break;
    }
    case WMTBlitCommandCopyFromTextureToTexture: {
      const struct wmtcmd_blit_copy_from_texture_to_texture *cmd = (const struct wmtcmd_blit_copy_from_texture_to_texture *)base;
      if (!state.blitOps.count) {
        dxmt_apitrace_accumulate_command_buffer_blit_fences(state.commandBuffer, state);
        dxmt_apitrace_accumulate_command_buffer_blit_copy_texture(session, state.commandBuffer, cmd);
      } else {
        [state.blitOps addObject:@{
          @"op" : @"copy_texture",
          @"source_texture_id" : @(cmd->src),
          @"destination_texture_id" : @(cmd->dst),
          @"payload" : dxmt_apitrace_copy_texture_json(
              cmd->src, cmd->dst, &cmd->src_origin, &cmd->src_size, cmd->src_slice, cmd->src_level,
              &cmd->dst_origin, cmd->dst_slice, cmd->dst_level),
        }];
      }
      break;
    }
    case WMTBlitCommandFillBuffer: {
      const struct wmtcmd_blit_fillbuffer *cmd = (const struct wmtcmd_blit_fillbuffer *)base;
      [state.blitOps addObject:@{
        @"op" : @"fill_buffer",
        @"buffer_id" : @(cmd->buffer),
        @"range_start" : @(cmd->offset),
        @"range_length" : @(cmd->length),
        @"value" : @(cmd->value),
      }];
      break;
    }
    case WMTBlitCommandWaitForFence:
    case WMTBlitCommandUpdateFence: {
      const struct wmtcmd_blit_fence_op *cmd = (const struct wmtcmd_blit_fence_op *)base;
      uint64_t fence = cmd->fence;
      if (base->type == WMTBlitCommandWaitForFence)
        [state.blitWaitFences appendBytes:&fence length:sizeof(fence)];
      else
        [state.blitUpdateFences appendBytes:&fence length:sizeof(fence)];
      break;
    }
    case WMTBlitCommandCopyFromTextureToBuffer:
    case WMTBlitCommandGenerateMipmaps:
    case WMTBlitCommandResolveCounters:
      dxmt_apitrace_begin_blit_encoder_if_needed(session, encoder, state);
      dxmt_apitrace_flush_blit_ops(session, encoder, state);
      dxmt_apitrace_flush_command_buffer_blit_batch(session, state.commandBuffer);
      break;
    default:
      break;
    }
  }
}

static void
dxmt_apitrace_begin_encoder_locked(
    apitrace_metal_session_t *session,
    DXMTApitraceEncoderKind kind,
    obj_handle_t encoder,
    obj_handle_t command_buffer,
    const struct WMTRenderPassInfo *render_pass_info) {
  if (!session || !dxmt_apitrace_current_d3d_sequence)
    return;
  if (![dxmt_apitrace_command_buffers objectForKey:dxmt_apitrace_key(command_buffer)])
    return;

  uint64_t begin_sequence = 0;
  BOOL metal_trace_began = YES;
  switch (kind) {
  case DXMTApitraceEncoderKindRender: {
    dxmt_apitrace_flush_command_buffer_blit_batch(session, command_buffer);
    NSString *payload = dxmt_apitrace_render_pass_json(render_pass_info);
    begin_sequence = apitrace_metal_render_encoder_begin(session, encoder, command_buffer, payload.UTF8String);
    break;
  }
  case DXMTApitraceEncoderKindCompute:
    dxmt_apitrace_flush_command_buffer_blit_batch(session, command_buffer);
    begin_sequence = apitrace_metal_compute_encoder_begin(session, encoder, command_buffer, "{}");
    break;
  case DXMTApitraceEncoderKindBlit:
    metal_trace_began = NO;
    break;
  }

  DXMTApitraceEncoderState *state = [[DXMTApitraceEncoderState alloc] init];
  state.beginSequence = begin_sequence;
  state.commandBuffer = command_buffer;
  state.d3dSequence = dxmt_apitrace_current_d3d_sequence;
  state.kind = kind;
  state.metalTraceBegan = metal_trace_began;
  [dxmt_apitrace_encoders setObject:state forKey:dxmt_apitrace_key(encoder)];
  [state release];
}

static void
dxmt_apitrace_end_encoder_locked(apitrace_metal_session_t *session, obj_handle_t encoder) {
  DXMTApitraceEncoderState *state = [dxmt_apitrace_encoders objectForKey:dxmt_apitrace_key(encoder)];
  if (!session || !state)
    return;

  if (state.kind == DXMTApitraceEncoderKindBlit) {
    if (state.metalTraceBegan) {
      dxmt_apitrace_flush_blit_ops(session, encoder, state);
    } else {
      if (state.blitOps.count) {
        dxmt_apitrace_flush_command_buffer_blit_batch(session, state.commandBuffer);
        dxmt_apitrace_emit_blit_encoder_batch(session, encoder, state);
      } else {
        dxmt_apitrace_accumulate_command_buffer_blit_fences(state.commandBuffer, state);
      }
    }
  }
  dxmt_apitrace_flush_fence_ops(session, encoder, state);

  switch (state.kind) {
  case DXMTApitraceEncoderKindRender:
    apitrace_metal_render_encoder_end(session, encoder);
    break;
  case DXMTApitraceEncoderKindCompute:
    apitrace_metal_compute_encoder_end(session, encoder);
    break;
  case DXMTApitraceEncoderKindBlit:
    if (state.metalTraceBegan)
      apitrace_metal_blit_encoder_end(session, encoder);
    break;
  }

  [dxmt_apitrace_encoders removeObjectForKey:dxmt_apitrace_key(encoder)];
}
#endif

static void
dxmt_set_layer_background_black(CAMetalLayer *layer) {
  CGColorRef black = CGColorGetConstantColor(kCGColorBlack);
  layer.backgroundColor = black;
  layer.opaque = YES;

  NSView *view = [layer.delegate isKindOfClass:NSView.class] ? (NSView *)layer.delegate : nil;
  for (NSView *ancestor = view; ancestor; ancestor = ancestor.superview) {
    CALayer *ancestor_layer = ancestor.layer;
    if (ancestor_layer) {
      ancestor_layer.backgroundColor = black;
      ancestor_layer.opaque = YES;
    }
  }

  NSWindow *window = view.window;
  if (window) {
    window.backgroundColor = NSColor.blackColor;
    window.opaque = YES;

    NSView *content_view = window.contentView;
    CALayer *content_layer = content_view.layer;
    if (content_layer) {
      content_layer.backgroundColor = black;
      content_layer.opaque = YES;
    }
  }
}

void
execute_on_main(dispatch_block_t block) {
  if ([NSThread isMainThread]) {
    block();
  } else {
    dispatch_sync(dispatch_get_main_queue(), block);
  }
}

static NTSTATUS
_NSObject_retain(NSObject **obj) {
  [*obj retain];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSObject_release(NSObject **obj) {
  [*obj release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSArray_object(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(NSArray *)params->handle objectAtIndex:params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSArray_count(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(NSArray *)params->handle count];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCopyAllDevices(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = (obj_handle_t)MTLCopyAllDevices();
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_recommendedMaxWorkingSetSize(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle recommendedMaxWorkingSetSize];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_currentAllocatedSize(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle currentAllocatedSize];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_name(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[[(id<MTLDevice>)params->handle name] retain];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_getCString(void *obj) {
  struct unixcall_nsstring_getcstring *params = obj;
  params->ret = (uint32_t)[(NSString *)params->str getCString:(char *)params->buffer_ptr
                                                    maxLength:params->max_length
                                                     encoding:params->encoding];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newCommandQueue(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newCommandQueueWithMaxCommandBufferCount:params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSAutoreleasePool_alloc_init(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = (obj_handle_t)[[NSAutoreleasePool alloc] init];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandQueue_commandBuffer(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[[(id<MTLCommandQueue>)params->handle commandBuffer] retain];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_commit(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(id<MTLCommandBuffer>)params->handle commit];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_waitUntilCompleted(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(id<MTLCommandBuffer>)params->handle waitUntilCompleted];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_status(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLCommandBuffer>)params->handle status];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newSharedEvent(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newSharedEvent];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLSharedEvent_signaledValue(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLSharedEvent>)params->handle signaledValue];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeSignalEvent(void *obj) {
  struct unixcall_generic_obj_obj_uint64_noret *params = obj;
  [(id<MTLCommandBuffer>)params->handle encodeSignalEvent:(id<MTLSharedEvent>)params->arg0 value:params->arg1];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newBuffer(void *obj) {
  struct unixcall_mtldevice_newbuffer *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTBufferInfo *info = params->info.ptr;
  id<MTLBuffer> buffer;
  const void *initial_bytes = info->memory.ptr;
  if (info->memory.ptr) {
    buffer = [device newBufferWithBytesNoCopy:info->memory.ptr
                                       length:info->length
                                      options:(enum MTLResourceOptions)info->options
                                  deallocator:NULL];
  } else {
    buffer = [device newBufferWithLength:info->length options:(enum MTLResourceOptions)info->options];
    info->memory.ptr = [buffer storageMode] == MTLStorageModePrivate ? NULL : [buffer contents];
  }
  params->ret = (obj_handle_t)buffer;
  info->gpu_address = [buffer gpuAddress];
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *address_payload = dxmt_apitrace_json_string(@{
        @"kind" : @"dxmt_buffer_gpu_address",
        @"buffer_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)params->ret],
        @"gpu_address" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)[buffer gpuAddress]],
      });
      apitrace_metal_object_metadata(dxmt_apitrace_session, params->ret, address_payload.UTF8String);
      apitrace_metal_register_buffer(
          dxmt_apitrace_session,
          params->ret,
          info->length,
          (uint32_t)[buffer storageMode],
          initial_bytes,
          initial_bytes ? info->length : 0);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newSamplerState(void *obj) {
  struct unixcall_mtldevice_newsamplerstate *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTSamplerInfo *info = params->info.ptr;

  MTLSamplerDescriptor *sampler_desc = [[MTLSamplerDescriptor alloc] init];
  sampler_desc.borderColor = (MTLSamplerBorderColor)info->border_color;
  sampler_desc.rAddressMode = (MTLSamplerAddressMode)info->r_address_mode;
  sampler_desc.sAddressMode = (MTLSamplerAddressMode)info->s_address_mode;
  sampler_desc.tAddressMode = (MTLSamplerAddressMode)info->t_address_mode;
  sampler_desc.magFilter = (MTLSamplerMinMagFilter)info->mag_filter;
  sampler_desc.minFilter = (MTLSamplerMinMagFilter)info->min_filter;
  sampler_desc.mipFilter = (MTLSamplerMipFilter)info->mip_filter;
  sampler_desc.compareFunction = (MTLCompareFunction)info->compare_function;
  sampler_desc.lodMaxClamp = info->lod_max_clamp;
  sampler_desc.lodMinClamp = info->lod_min_clamp;
  sampler_desc.maxAnisotropy = info->max_anisotroy;
  sampler_desc.lodAverage = info->lod_average;
  sampler_desc.normalizedCoordinates = info->normalized_coords;
  sampler_desc.supportArgumentBuffers = info->support_argument_buffers;

  id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sampler_desc];
  info->gpu_resource_id = info->support_argument_buffers ? [sampler gpuResourceID]._impl : 0;
  params->ret = (obj_handle_t)sampler;
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && info->gpu_resource_id) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *payload = dxmt_apitrace_sampler_descriptor_json(info, params->ret);
      apitrace_metal_object_metadata(dxmt_apitrace_session, params->ret, payload.UTF8String);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  [sampler_desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newDepthStencilState(void *obj) {
  struct unixcall_mtldevice_newdepthstencilstate *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  const struct WMTDepthStencilInfo *info = params->info.ptr;

  MTLDepthStencilDescriptor *desc = [[MTLDepthStencilDescriptor alloc] init];
  desc.depthCompareFunction = (MTLCompareFunction)info->depth_compare_function;
  desc.depthWriteEnabled = info->depth_write_enabled;

  if (info->front_stencil.enabled) {
    desc.frontFaceStencil.depthStencilPassOperation = (MTLStencilOperation)info->front_stencil.depth_stencil_pass_op;
    desc.frontFaceStencil.depthFailureOperation = (MTLStencilOperation)info->front_stencil.depth_fail_op;
    desc.frontFaceStencil.stencilFailureOperation = (MTLStencilOperation)info->front_stencil.stencil_fail_op;
    desc.frontFaceStencil.stencilCompareFunction = (MTLCompareFunction)info->front_stencil.stencil_compare_function;
    desc.frontFaceStencil.writeMask = info->front_stencil.write_mask;
    desc.frontFaceStencil.readMask = info->front_stencil.read_mask;
  }

  if (info->back_stencil.enabled) {
    desc.backFaceStencil.depthStencilPassOperation = (MTLStencilOperation)info->back_stencil.depth_stencil_pass_op;
    desc.backFaceStencil.depthFailureOperation = (MTLStencilOperation)info->back_stencil.depth_fail_op;
    desc.backFaceStencil.stencilFailureOperation = (MTLStencilOperation)info->back_stencil.stencil_fail_op;
    desc.backFaceStencil.stencilCompareFunction = (MTLCompareFunction)info->back_stencil.stencil_compare_function;
    desc.backFaceStencil.writeMask = info->back_stencil.write_mask;
    desc.backFaceStencil.readMask = info->back_stencil.read_mask;
  }

  params->ret = (obj_handle_t)[device newDepthStencilStateWithDescriptor:desc];
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && params->ret) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *payload = dxmt_apitrace_depth_stencil_json(info, params->ret);
      apitrace_metal_object_metadata(dxmt_apitrace_session, params->ret, payload.UTF8String);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  [desc release];
  return STATUS_SUCCESS;
}

MTLPixelFormat to_metal_pixel_format(enum WMTPixelFormat format) {
  return (MTLPixelFormat)ORIGINAL_FORMAT(format);
}

void
fill_texture_descriptor(MTLTextureDescriptor *desc, struct WMTTextureInfo *info) {
  desc.textureType = (MTLTextureType)info->type;
  desc.pixelFormat = to_metal_pixel_format(info->pixel_format);
  desc.width = info->width;
  desc.height = info->height;
  desc.depth = info->depth;
  desc.arrayLength = info->array_length;
  desc.mipmapLevelCount = info->mipmap_level_count;
  desc.sampleCount = info->sample_count;
  desc.usage = (MTLTextureUsage)info->usage;
  desc.resourceOptions = (MTLResourceOptions)info->options;
  if (info->reserved & WMTTextureInfoFlagPlacementSparse) {
    if (@available(macOS 26.0, *)) {
      desc.placementSparsePageSize = MTLSparsePageSize64;
    }
  }
  // No descriptor-baked swizzle. Per-bind format/swizzle aliasing
  // happens through newTextureView (see _MTLTexture_newTextureView +
  // to_metal_swizzle below) so the base texture stays a clean
  // identity-swizzle Metal object.
};

void
extract_texture_descriptor(id<MTLTexture> desc, struct WMTTextureInfo *info) {
  info->type = desc.textureType;
  info->pixel_format = desc.pixelFormat;
  info->width = desc.width;
  info->height = desc.height;
  info->depth = desc.depth;
  info->array_length = desc.arrayLength;
  info->mipmap_level_count = desc.mipmapLevelCount;
  info->sample_count = desc.sampleCount;
  info->usage = desc.usage;
  info->options = (enum WMTResourceOptions)desc.resourceOptions;
  info->reserved = 0;
};

static NTSTATUS
_MTLDevice_newTexture(void *obj) {
  struct unixcall_mtldevice_newtexture *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTTextureInfo *info = params->info.ptr;
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);

  id<MTLTexture> ret = [device newTextureWithDescriptor:desc];
  params->ret = (obj_handle_t)ret;
  info->gpu_resource_id = ret ? [ret gpuResourceID]._impl : 0;
  info->mach_port = 0;
#if DXMT_APITRACE_METAL
  if (ret && dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *resource_payload = dxmt_apitrace_json_string(@{
        @"kind" : @"dxmt_texture_gpu_resource_id",
        @"texture_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)params->ret],
        @"gpu_resource_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)info->gpu_resource_id],
      });
      apitrace_metal_object_metadata(dxmt_apitrace_session, params->ret, resource_payload.UTF8String);
      NSString *descriptor_json = dxmt_apitrace_texture_descriptor_json(info);
      apitrace_metal_register_texture(dxmt_apitrace_session, params->ret, descriptor_json.UTF8String);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsPlacementSparse(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->handle;
  params->ret = 0;
  if (@available(macOS 26.4, *)) {
    params->ret = device.supportsPlacementSparse ? 1 : 0;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_sparseTileSize(void *obj) {
  struct unixcall_mtldevice_sparsetilesize *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  const struct WMTTextureInfo *info = params->info.ptr;
  struct WMTSparseTileSize *tile_size = params->tile_size.ptr;
  params->ret = 0;
  if (!info || !tile_size)
    return STATUS_SUCCESS;

  if (@available(macOS 26.0, *)) {
    const MTLPixelFormat format = to_metal_pixel_format(info->pixel_format);
    MTLSize size = [device sparseTileSizeWithTextureType:(MTLTextureType)info->type
                                             pixelFormat:format
                                             sampleCount:info->sample_count
                                          sparsePageSize:MTLSparsePageSize64];
    tile_size->width = size.width;
    tile_size->height = size.height;
    tile_size->depth = size.depth;
    tile_size->bytes = [device sparseTileSizeInBytesForSparsePageSize:MTLSparsePageSize64];
    params->ret = tile_size->width && tile_size->height && tile_size->depth && tile_size->bytes;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newPlacementHeap(void *obj) {
  struct unixcall_mtldevice_newplacementheap *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  const struct WMTPlacementHeapInfo *info = params->info.ptr;
  params->ret = 0;
  if (!info || !info->size)
    return STATUS_SUCCESS;

  if (@available(macOS 26.0, *)) {
    MTLHeapDescriptor *desc = [[MTLHeapDescriptor alloc] init];
    desc.type = MTLHeapTypePlacement;
    desc.size = info->size;
    desc.resourceOptions = (MTLResourceOptions)info->options;
    desc.maxCompatiblePlacementSparsePageSize = MTLSparsePageSize64;
    params->ret = (obj_handle_t)[device newHeapWithDescriptor:desc];
    [desc release];
  }
  return STATUS_SUCCESS;
}

static MTLSparseTextureMappingMode
to_metal_sparse_texture_mapping_mode(enum WMTSparseTextureMappingMode mode) {
  switch (mode) {
  case WMTSparseTextureMappingModeUnmap:
    return MTLSparseTextureMappingModeUnmap;
  case WMTSparseTextureMappingModeMap:
  default:
    return MTLSparseTextureMappingModeMap;
  }
}

static NTSTATUS
_MTLDevice_updateSparseTextureMappings(void *obj) {
  struct unixcall_mtldevice_updatesparsetexturemappings *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  id<MTLTexture> texture = (id<MTLTexture>)params->texture;
  id<MTLHeap> heap = (id<MTLHeap>)params->heap;
  const struct WMTSparseTextureMappingOperation *operations = params->operations.ptr;
  params->ret = 0;
  if (!texture || !operations || !params->operation_count)
    return STATUS_SUCCESS;

  if (@available(macOS 26.0, *)) {
    id<MTL4CommandQueue> queue = [device newMTL4CommandQueue];
    id<MTLSharedEvent> event = [device newSharedEvent];
    if (!queue || !event) {
      [queue release];
      [event release];
      return STATUS_SUCCESS;
    }

    MTL4UpdateSparseTextureMappingOperation *mtl_ops =
        calloc((size_t)params->operation_count, sizeof(*mtl_ops));
    if (!mtl_ops) {
      [queue release];
      [event release];
      return STATUS_SUCCESS;
    }

    for (uint64_t i = 0; i < params->operation_count; ++i) {
      const struct WMTSparseTextureMappingOperation *src = &operations[i];
      mtl_ops[i].mode = to_metal_sparse_texture_mapping_mode(src->mode);
      mtl_ops[i].textureRegion = MTLRegionMake3D(src->x, src->y, src->z,
                                                 src->width, src->height,
                                                 src->depth);
      mtl_ops[i].textureLevel = src->level;
      mtl_ops[i].textureSlice = src->slice;
      mtl_ops[i].heapOffset = src->heap_offset;
    }

    [queue updateTextureMappings:texture
                            heap:heap
                      operations:mtl_ops
                           count:(NSUInteger)params->operation_count];
    [queue signalEvent:event value:1];
    params->ret = [event waitUntilSignaledValue:1 timeoutMS:UINT64_MAX] ? 1 : 0;

    free(mtl_ops);
    [queue release];
    [event release];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBuffer_newTexture(void *obj) {
  struct unixcall_mtlbuffer_newtexture *params = obj;
  id<MTLBuffer> buffer = (id<MTLBuffer>)params->buffer;
  struct WMTTextureInfo *info = params->info.ptr;
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);

  id<MTLTexture> ret = [buffer newTextureWithDescriptor:desc offset:params->offset bytesPerRow:params->bytes_per_row];
  params->ret = (obj_handle_t)ret;
  info->gpu_resource_id = [ret gpuResourceID]._impl;
  info->mach_port = 0;
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *resource_payload = dxmt_apitrace_json_string(@{
        @"kind" : @"dxmt_texture_gpu_resource_id",
        @"texture_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)params->ret],
        @"gpu_resource_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)info->gpu_resource_id],
      });
      apitrace_metal_object_metadata(dxmt_apitrace_session, params->ret, resource_payload.UTF8String);
      NSString *descriptor_json = dxmt_apitrace_texture_descriptor_json(info);
      apitrace_metal_register_texture(dxmt_apitrace_session, params->ret, descriptor_json.UTF8String);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif

  [desc release];
  return STATUS_SUCCESS;
}

static inline MTLTextureSwizzleChannels
to_metal_swizzle(struct WMTTextureSwizzleChannels swizzle, enum WMTPixelFormat format) {
  if (format & WMTPixelFormatRGB1Swizzle) {
    return MTLTextureSwizzleChannelsMake(
        (MTLTextureSwizzle)swizzle.r, (MTLTextureSwizzle)swizzle.g, (MTLTextureSwizzle)swizzle.b, MTLTextureSwizzleOne
    );
  }
  if (format & WMTPixelFormatR001Swizzle) {
    return MTLTextureSwizzleChannelsMake(
        (MTLTextureSwizzle)swizzle.r, MTLTextureSwizzleZero, MTLTextureSwizzleZero, MTLTextureSwizzleOne
    );
  }
  if (format & WMTPixelFormat0R01Swizzle) {
    return MTLTextureSwizzleChannelsMake(
        MTLTextureSwizzleOne, (MTLTextureSwizzle)swizzle.r, MTLTextureSwizzleOne, MTLTextureSwizzleOne
    );
  }
  if (format & WMTPixelFormatGBARSwizzle) {
    return MTLTextureSwizzleChannelsMake(
        (MTLTextureSwizzle)swizzle.g, (MTLTextureSwizzle)swizzle.b, (MTLTextureSwizzle)swizzle.a,
        (MTLTextureSwizzle)swizzle.r
    );
  }
  return MTLTextureSwizzleChannelsMake(
      (MTLTextureSwizzle)swizzle.r, (MTLTextureSwizzle)swizzle.g, (MTLTextureSwizzle)swizzle.b,
      (MTLTextureSwizzle)swizzle.a
  );
}

static NTSTATUS
_MTLTexture_newTextureView(void *obj) {
  struct unixcall_mtltexture_newtextureview *params = obj;
  id<MTLTexture> texture = (id<MTLTexture>)params->texture;

  id<MTLTexture> ret = [texture
      newTextureViewWithPixelFormat:to_metal_pixel_format(params->format)
                        textureType:(MTLTextureType)params->texture_type
                             levels:NSMakeRange(params->level_start, params->level_count)
                             slices:NSMakeRange(params->slice_start, params->slice_count)
                            swizzle:to_metal_swizzle(params->swizzle, params->format)];
  params->ret = (obj_handle_t)ret;
  params->gpu_resource_id = [ret gpuResourceID]._impl;
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && params->ret) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *resource_payload = dxmt_apitrace_json_string(@{
        @"kind" : @"dxmt_texture_gpu_resource_id",
        @"texture_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)params->ret],
        @"gpu_resource_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)params->gpu_resource_id],
      });
      apitrace_metal_object_metadata(dxmt_apitrace_session, params->ret, resource_payload.UTF8String);
      NSString *view_payload = dxmt_apitrace_texture_view_json(params);
      apitrace_metal_object_metadata(dxmt_apitrace_session, params->ret, view_payload.UTF8String);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_minimumLinearTextureAlignmentForPixelFormat(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle minimumLinearTextureAlignmentForPixelFormat:to_metal_pixel_format(params->arg)];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newLibrary(void *obj) {
  struct unixcall_mtldevice_newlibrary *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  NSError *err = NULL;
  params->ret_library = (obj_handle_t)[device newLibraryWithData:(dispatch_data_t)params->data error:&err];
  params->ret_error = (obj_handle_t)[err retain];   /* +1 across the boundary; PE releases */
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && !err && params->ret_library) {
    const void *bytes = NULL;
    size_t size = 0;
    dispatch_data_t mapped = dispatch_data_create_map((dispatch_data_t)params->data, &bytes, &size);
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      apitrace_metal_register_library(dxmt_apitrace_session, params->ret_library, bytes, size);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
    (void)mapped;
  }
#endif
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLLibrary_newFunction(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  id<MTLLibrary> library = (id<MTLLibrary>)params->handle;
  NSString *name = [[NSString alloc] initWithCString:(char *)params->arg encoding:NSUTF8StringEncoding];
  params->ret = (obj_handle_t)[library newFunctionWithName:name];
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && params->ret) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    dxmt_apitrace_track_function_locked(params->ret, params->handle, name);
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  [name release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_lengthOfBytesUsingEncoding(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  params->ret = (uint64_t)[(NSString *)params->handle lengthOfBytesUsingEncoding:(NSStringEncoding)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSObject_description(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[[(NSObject *)params->handle description] retain];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newComputePipelineState(void *obj) {
  struct unixcall_mtldevice_newcomputepso *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  const struct WMTComputePipelineInfo *info = params->info.ptr;
  MTLComputePipelineDescriptor *descriptor = [[MTLComputePipelineDescriptor alloc] init];
  NSError *err = NULL;
  descriptor.computeFunction = (id<MTLFunction>)info->compute_function;
  descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth = info->tgsize_is_multiple_of_sgwidth;
  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_buffers & (1 << i))
      descriptor.buffers[i].mutability = MTLMutabilityImmutable;
  }
  if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
    descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                    count:info->num_binary_archives_for_lookup];
  MTLPipelineOption options =
      info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  params->ret_pso =
      (obj_handle_t)[device newComputePipelineStateWithDescriptor:descriptor options:options reflection:nil error:&err];
  params->ret_error = (obj_handle_t)[err retain];   /* +1 across the boundary; PE releases */
  if (!err && info->binary_archive_for_serialization) {
    [(id<MTLBinaryArchive>)info->binary_archive_for_serialization addComputePipelineFunctionsWithDescriptor:descriptor
                                                                                                      error:&err];
  }
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && params->ret_pso) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *descriptor_json = dxmt_apitrace_compute_pipeline_json(info);
      apitrace_metal_register_compute_pipeline(
          dxmt_apitrace_session,
          params->ret_pso,
          descriptor_json.UTF8String,
          info->compute_function);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_blitCommandEncoder(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[[(id<MTLCommandBuffer>)params->handle blitCommandEncoder] retain];
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    dxmt_apitrace_begin_encoder_locked(
        dxmt_apitrace_ensure_session_locked(),
        DXMTApitraceEncoderKindBlit,
        params->ret,
        params->handle,
        NULL);
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_computeCommandEncoder(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  params->ret = (obj_handle_t)[[(id<MTLCommandBuffer>)params->handle computeCommandEncoderWithDispatchType:params->arg ? MTLDispatchTypeConcurrent : MTLDispatchTypeSerial] retain];
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    dxmt_apitrace_begin_encoder_locked(
        dxmt_apitrace_ensure_session_locked(),
        DXMTApitraceEncoderKindCompute,
        params->ret,
        params->handle,
        NULL);
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  return STATUS_SUCCESS;
}

// Current render pass's render-target extent, captured when the encoder is
// created and read when a scissor rect is applied to clamp it into bounds.
// The encode thread creates an encoder then replays that pass's commands in
// order before the next, so a file-static is sufficient (no interleaving).
static uint32_t g_dxmt_pass_rt_w = 0;
static uint32_t g_dxmt_pass_rt_h = 0;

static NTSTATUS
_MTLCommandBuffer_renderCommandEncoder(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  struct WMTRenderPassInfo *info = (struct WMTRenderPassInfo *)params->arg;
  MTLRenderPassDescriptor *descriptor = [[MTLRenderPassDescriptor alloc] init];
  for (unsigned i = 0; i < 8; i++) {
    // Skip color slots with no texture. D3D9 (and D3D11) permit binding
    // render targets at non-contiguous indices (e.g. RT0 + RT2 with RT1
    // unbound); the caller reports render_target_count as highest_bound+1,
    // so the unbound gap slot reaches here with a nil texture. Leaving it
    // at the MTLRenderPassDescriptor default (nil texture, DontCare load
    // and store) keeps it an inert/unused attachment that Metal ignores.
    // Configuring it from the blanket WMTRenderPassInfo defaults instead
    // stamps storeAction=Store onto a textureless slot, which makes
    // MTL_DEBUG_LAYER_VALIDATE_LOAD_ACTIONS treat it as a live attachment
    // with an undefined load and assert. Matches the texture-guarded
    // depth/stencil blocks below.
    if (!info->colors[i].texture)
      continue;
    descriptor.colorAttachments[i].clearColor = MTLClearColorMake(
        info->colors[i].clear_color.r, info->colors[i].clear_color.g, info->colors[i].clear_color.b,
        info->colors[i].clear_color.a
    );
    descriptor.colorAttachments[i].level = info->colors[i].level;
    descriptor.colorAttachments[i].slice = info->colors[i].slice;
    descriptor.colorAttachments[i].depthPlane = info->colors[i].depth_plane;
    descriptor.colorAttachments[i].texture = (id<MTLTexture>)info->colors[i].texture;
    descriptor.colorAttachments[i].loadAction = (MTLLoadAction)info->colors[i].load_action;
    descriptor.colorAttachments[i].storeAction = (MTLStoreAction)info->colors[i].store_action;
    descriptor.colorAttachments[i].resolveTexture = (id<MTLTexture>)info->colors[i].resolve_texture;
    descriptor.colorAttachments[i].resolveLevel = info->colors[i].resolve_level;
    descriptor.colorAttachments[i].resolveSlice = info->colors[i].resolve_slice;
    descriptor.colorAttachments[i].resolveDepthPlane = info->colors[i].resolve_depth_plane;
  }

  if (info->depth.texture) {
    descriptor.depthAttachment.clearDepth = info->depth.clear_depth;
    descriptor.depthAttachment.depthPlane = info->depth.depth_plane;
    descriptor.depthAttachment.level = info->depth.level;
    descriptor.depthAttachment.slice = info->depth.slice;
    descriptor.depthAttachment.texture = (id<MTLTexture>)info->depth.texture;
    descriptor.depthAttachment.loadAction = (MTLLoadAction)info->depth.load_action;
    descriptor.depthAttachment.storeAction = (MTLStoreAction)info->depth.store_action;
  }

  if (info->stencil.texture) {
    descriptor.stencilAttachment.clearStencil = info->stencil.clear_stencil;
    descriptor.stencilAttachment.depthPlane = info->stencil.depth_plane;
    descriptor.stencilAttachment.level = info->stencil.level;
    descriptor.stencilAttachment.slice = info->stencil.slice;
    descriptor.stencilAttachment.texture = (id<MTLTexture>)info->stencil.texture;
    descriptor.stencilAttachment.loadAction = (MTLLoadAction)info->stencil.load_action;
    descriptor.stencilAttachment.storeAction = (MTLStoreAction)info->stencil.store_action;
  }

  descriptor.defaultRasterSampleCount = info->default_raster_sample_count;
  descriptor.renderTargetArrayLength = info->render_target_array_length;
  descriptor.renderTargetHeight = info->render_target_height;
  descriptor.renderTargetWidth = info->render_target_width;
  descriptor.visibilityResultBuffer = (id<MTLBuffer>)info->visibility_buffer;

  // Capture this pass's render-target extent so scissor commands can be clamped
  // to it at apply time (see WMTRenderCommandSetScissorRect). D3D9 blit / resolve
  // / mip-downsample paths can hand Metal a scissor sized for the source while
  // the destination is a smaller mip level; Vulkan auto-clamps so DXVK never
  // needs this, but Metal's debug layer aborts on an out-of-bounds scissor.
  {
    id<MTLTexture> c0 = (id<MTLTexture>)info->colors[0].texture;
    id<MTLTexture> dt = (id<MTLTexture>)info->depth.texture;
    unsigned eff_w = c0 ? (unsigned)([c0 width] >> info->colors[0].level)
                        : (dt ? (unsigned)([dt width] >> info->depth.level) : 0);
    unsigned eff_h = c0 ? (unsigned)([c0 height] >> info->colors[0].level)
                        : (dt ? (unsigned)([dt height] >> info->depth.level) : 0);
    g_dxmt_pass_rt_w = info->render_target_width ? info->render_target_width : eff_w;
    g_dxmt_pass_rt_h = info->render_target_height ? info->render_target_height : eff_h;
  }

  if (info->tile_height && info->tile_width) {
    descriptor.tileWidth = info->tile_width;
    descriptor.tileHeight = info->tile_height;
  }

  params->ret = (obj_handle_t)[[(id<MTLCommandBuffer>)params->handle renderCommandEncoderWithDescriptor:descriptor] retain];
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    dxmt_apitrace_begin_encoder_locked(
        dxmt_apitrace_ensure_session_locked(),
        DXMTApitraceEncoderKindRender,
        params->ret,
        params->handle,
        info);
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif

  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandEncoder_endEncoding(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(id<MTLCommandEncoder>)params->handle endEncoding];
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    dxmt_apitrace_end_encoder_locked(dxmt_apitrace_session, params->handle);
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  return STATUS_SUCCESS;
}

#ifndef DXMT_NO_PRIVATE_API

typedef NS_ENUM(NSUInteger, MTLLogicOperation) {
  MTLLogicOperationClear,
  MTLLogicOperationSet,
  MTLLogicOperationCopy,
  MTLLogicOperationCopyInverted,
  MTLLogicOperationNoop,
  MTLLogicOperationInvert,
  MTLLogicOperationAnd,
  MTLLogicOperationNand,
  MTLLogicOperationOr,
  MTLLogicOperationNor,
  MTLLogicOperationXor,
  MTLLogicOperationEquivalence,
  MTLLogicOperationAndReverse,
  MTLLogicOperationAndInverted,
  MTLLogicOperationOrReverse,
  MTLLogicOperationOrInverted,
};

@interface
MTLRenderPipelineDescriptor ()

- (void)setLogicOperationEnabled:(BOOL)enable;
- (void)setLogicOperation:(MTLLogicOperation)op;

@end

@interface
MTLMeshRenderPipelineDescriptor ()

- (void)setLogicOperationEnabled:(BOOL)enable;
- (void)setLogicOperation:(MTLLogicOperation)op;

@end

#endif

/* d9mt addition: shared between the classic entry and the
 * vertex-descriptor-capable entry below. Body unchanged from upstream. */
static MTLRenderPipelineDescriptor *
_wm_render_pipeline_descriptor(const struct WMTRenderPipelineInfo *info) {
  MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];

  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].pixelFormat = to_metal_pixel_format(info->colors[i].pixel_format);
    descriptor.colorAttachments[i].blendingEnabled = info->colors[i].blending_enabled;
    descriptor.colorAttachments[i].writeMask = (MTLColorWriteMask)info->colors[i].write_mask;

    descriptor.colorAttachments[i].alphaBlendOperation = (MTLBlendOperation)info->colors[i].alpha_blend_operation;
    descriptor.colorAttachments[i].rgbBlendOperation = (MTLBlendOperation)info->colors[i].rgb_blend_operation;

    descriptor.colorAttachments[i].sourceRGBBlendFactor = (MTLBlendFactor)info->colors[i].src_rgb_blend_factor;
    descriptor.colorAttachments[i].sourceAlphaBlendFactor = (MTLBlendFactor)info->colors[i].src_alpha_blend_factor;
    descriptor.colorAttachments[i].destinationRGBBlendFactor = (MTLBlendFactor)info->colors[i].dst_rgb_blend_factor;
    descriptor.colorAttachments[i].destinationAlphaBlendFactor = (MTLBlendFactor)info->colors[i].dst_alpha_blend_factor;
  }

  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_fragment_buffers & (1 << i))
      descriptor.fragmentBuffers[i].mutability = MTLMutabilityImmutable;
    if (info->immutable_vertex_buffers & (1 << i))
      descriptor.vertexBuffers[i].mutability = MTLMutabilityImmutable;
  }

#ifndef DXMT_NO_PRIVATE_API
  [descriptor setLogicOperationEnabled:info->logic_operation_enabled];
  [descriptor setLogicOperation:(MTLLogicOperation)info->logic_operation];
#endif
  descriptor.depthAttachmentPixelFormat = to_metal_pixel_format(info->depth_pixel_format);
  descriptor.stencilAttachmentPixelFormat = to_metal_pixel_format(info->stencil_pixel_format);
  descriptor.alphaToCoverageEnabled = info->alpha_to_coverage_enabled;
  descriptor.rasterizationEnabled = info->rasterization_enabled;
  descriptor.rasterSampleCount = info->raster_sample_count;
  descriptor.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)info->input_primitive_topology;
  descriptor.tessellationPartitionMode = (MTLTessellationPartitionMode)info->tessellation_partition_mode;
  descriptor.tessellationFactorStepFunction = (MTLTessellationFactorStepFunction)info->tessellation_factor_step;
  descriptor.tessellationOutputWindingOrder = (MTLWinding)info->tessellation_output_winding_order;
  descriptor.maxTessellationFactor = info->max_tessellation_factor;

  descriptor.vertexFunction = (id<MTLFunction>)info->vertex_function;
  descriptor.fragmentFunction = (id<MTLFunction>)info->fragment_function;

  if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
    descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                    count:info->num_binary_archives_for_lookup];
  return descriptor;
}

static NTSTATUS
_MTLDevice_newRenderPipelineState(void *obj) {
  struct unixcall_mtldevice_newrenderpso *params = obj;
  const struct WMTRenderPipelineInfo *info = params->info.ptr;
  MTLRenderPipelineDescriptor *descriptor = _wm_render_pipeline_descriptor(info);
  NSError *err = NULL;
  MTLPipelineOption options =
      info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newRenderPipelineStateWithDescriptor:descriptor
                                                                                              options:options
                                                                                           reflection:nil
                                                                                                error:&err];
  params->ret_error = (obj_handle_t)[err retain];   /* +1 across the boundary; PE releases */
  const BOOL pso_created = params->ret_pso != 0;
  if (pso_created && info->binary_archive_for_serialization) {
    [(id<MTLBinaryArchive>)info->binary_archive_for_serialization addRenderPipelineFunctionsWithDescriptor:descriptor
                                                                                                     error:&err];
  }
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && pso_created) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *descriptor_json = dxmt_apitrace_render_pipeline_json(info);
      apitrace_metal_register_render_pipeline(
          dxmt_apitrace_session,
          params->ret_pso,
          descriptor_json.UTF8String,
          info->vertex_function,
          info->fragment_function);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  [descriptor release];
  return STATUS_SUCCESS;
}

static id<MTLFunction>
DXMTGetNullFragmentFunction(id<MTLDevice> device) {
  static NSMutableDictionary *functions_by_device = nil;

  @synchronized([MTLMeshRenderPipelineDescriptor class]) {
    if (!functions_by_device)
      functions_by_device = [[NSMutableDictionary alloc] init];

    NSValue *key = [[NSValue alloc] initWithBytes:&device objCType:@encode(void *)];
    id<MTLFunction> function = [functions_by_device objectForKey:key];
    if (function) {
      [key release];
      return function;
    }

    NSString *source = @"using namespace metal;\n"
                       "fragment void dxmt_null_fragment() {}\n";
    NSError *err = NULL;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&err];
    if (!library) {
      fprintf(stderr,
              "err:   DXMT failed to create null fragment library: %s\n",
              [[err localizedDescription] UTF8String]);
      [key release];
      return nil;
    }

    function = [library newFunctionWithName:@"dxmt_null_fragment"];
    if (!function) {
      fprintf(stderr,
              "err:   DXMT failed to create null fragment function\n");
      [library release];
      [key release];
      return nil;
    }

    [functions_by_device setObject:function forKey:key];
    [function release];
    [library release];
    function = [functions_by_device objectForKey:key];
    [key release];
    return function;
  }
}

static NTSTATUS
_MTLDevice_newRenderPipelineStateWithVertexDescriptor(void *obj) {
  struct unixcall_mtldevice_newrenderpso_vtx *params = obj;
  const struct WMTRenderPipelineInfo *info = params->info.ptr;
  const struct WMTRenderPipelineVertexInfo *vertex_info = params->vertex_info.ptr;
  MTLRenderPipelineDescriptor *descriptor = _wm_render_pipeline_descriptor(info);

  if (vertex_info && (vertex_info->num_attributes || vertex_info->num_layouts)) {
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    for (unsigned i = 0; i < vertex_info->num_attributes && i < 31; i++) {
      const struct WMTVertexAttributeInfo *attr = &vertex_info->attributes[i];
      vd.attributes[i].format = (MTLVertexFormat)attr->format;
      vd.attributes[i].offset = attr->offset;
      vd.attributes[i].bufferIndex = attr->buffer_index;
    }
    for (unsigned i = 0; i < vertex_info->num_layouts && i < 31; i++) {
      const struct WMTVertexBufferLayoutInfo *layout = &vertex_info->layouts[i];
      if (!layout->stride && layout->step_function == WMTVertexStepFunctionPerVertex)
        continue;
      vd.layouts[i].stride = layout->stride;
      vd.layouts[i].stepFunction = (MTLVertexStepFunction)layout->step_function;
      vd.layouts[i].stepRate =
          layout->step_function == WMTVertexStepFunctionConstant ? 0 : layout->step_rate;
    }
    descriptor.vertexDescriptor = vd;
  }

  NSError *err = NULL;
  params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newRenderPipelineStateWithDescriptor:descriptor
                                                                                              options:MTLPipelineOptionNone
                                                                                           reflection:nil
                                                                                                error:&err];
  params->ret_error = (obj_handle_t)[err retain];   /* +1 across the boundary; PE releases */
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newMeshRenderPipelineState(void *obj) {
  struct unixcall_mtldevice_newmeshrenderpso *params = obj;
  const struct WMTMeshRenderPipelineInfo *info = params->info.ptr;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  MTLMeshRenderPipelineDescriptor *descriptor = [[MTLMeshRenderPipelineDescriptor alloc] init];

  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].pixelFormat = to_metal_pixel_format(info->colors[i].pixel_format);
    descriptor.colorAttachments[i].blendingEnabled = info->colors[i].blending_enabled;
    descriptor.colorAttachments[i].writeMask = (MTLColorWriteMask)info->colors[i].write_mask;

    descriptor.colorAttachments[i].alphaBlendOperation = (MTLBlendOperation)info->colors[i].alpha_blend_operation;
    descriptor.colorAttachments[i].rgbBlendOperation = (MTLBlendOperation)info->colors[i].rgb_blend_operation;

    descriptor.colorAttachments[i].sourceRGBBlendFactor = (MTLBlendFactor)info->colors[i].src_rgb_blend_factor;
    descriptor.colorAttachments[i].sourceAlphaBlendFactor = (MTLBlendFactor)info->colors[i].src_alpha_blend_factor;
    descriptor.colorAttachments[i].destinationRGBBlendFactor = (MTLBlendFactor)info->colors[i].dst_rgb_blend_factor;
    descriptor.colorAttachments[i].destinationAlphaBlendFactor = (MTLBlendFactor)info->colors[i].dst_alpha_blend_factor;
  }

  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_fragment_buffers & (1 << i))
      descriptor.fragmentBuffers[i].mutability = MTLMutabilityImmutable;
    if (info->immutable_mesh_buffers & (1 << i))
      descriptor.meshBuffers[i].mutability = MTLMutabilityImmutable;
    if (info->immutable_object_buffers & (1 << i))
      descriptor.objectBuffers[i].mutability = MTLMutabilityImmutable;
  }

#ifndef DXMT_NO_PRIVATE_API
  [descriptor setLogicOperationEnabled:info->logic_operation_enabled];
  [descriptor setLogicOperation:(MTLLogicOperation)info->logic_operation];
#endif
  descriptor.depthAttachmentPixelFormat = to_metal_pixel_format(info->depth_pixel_format);
  descriptor.stencilAttachmentPixelFormat = to_metal_pixel_format(info->stencil_pixel_format);
  descriptor.alphaToCoverageEnabled = info->alpha_to_coverage_enabled;
  descriptor.rasterizationEnabled = info->rasterization_enabled;
  descriptor.rasterSampleCount = info->raster_sample_count;

  descriptor.objectFunction = (id<MTLFunction>)info->object_function;
  descriptor.meshFunction = (id<MTLFunction>)info->mesh_function;
  id<MTLFunction> fragment_function = (id<MTLFunction>)info->fragment_function;
  if (info->rasterization_enabled && !fragment_function)
    fragment_function = DXMTGetNullFragmentFunction(device);
  descriptor.fragmentFunction = fragment_function;
  descriptor.payloadMemoryLength = info->payload_memory_length;

  if (info->rasterization_enabled && !info->fragment_function) {
    fprintf(stderr,
            "warn:  DXMT diagnostic: Metal mesh render descriptor has rasterization enabled but no D3D pixel shader; "
            "using_null_fragment=%u; "
            "object=0x%llx mesh=0x%llx depth=%u stencil=%u samples=%u payload=%u rt=[%u,%u,%u,%u,%u,%u,%u,%u]\n",
            fragment_function ? 1 : 0,
            (unsigned long long)info->object_function,
            (unsigned long long)info->mesh_function,
            info->depth_pixel_format,
            info->stencil_pixel_format,
            info->raster_sample_count,
            info->payload_memory_length,
            info->colors[0].pixel_format,
            info->colors[1].pixel_format,
            info->colors[2].pixel_format,
            info->colors[3].pixel_format,
            info->colors[4].pixel_format,
            info->colors[5].pixel_format,
            info->colors[6].pixel_format,
            info->colors[7].pixel_format);
  }

  descriptor.meshThreadgroupSizeIsMultipleOfThreadExecutionWidth = info->mesh_tgsize_is_multiple_of_sgwidth;
  descriptor.objectThreadgroupSizeIsMultipleOfThreadExecutionWidth = info->object_tgsize_is_multiple_of_sgwidth;

  MTLPipelineOption options = MTLPipelineOptionNone;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
      descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                      count:info->num_binary_archives_for_lookup];
    options = info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  }
#endif
  NSError *err = NULL;
  params->ret_pso = (obj_handle_t)[device newRenderPipelineStateWithMeshDescriptor:descriptor
                                                                           options:options
                                                                        reflection:nil
                                                                             error:&err];
  params->ret_error = (obj_handle_t)[err retain];   /* +1 across the boundary; PE releases */
  const BOOL pso_created = params->ret_pso != 0;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (pso_created && info->binary_archive_for_serialization) {
      [(id<MTLBinaryArchive>)info->binary_archive_for_serialization
          addMeshRenderPipelineFunctionsWithDescriptor:descriptor
                                                 error:&err];
    }
  }
#endif
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && pso_created) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *descriptor_json = dxmt_apitrace_mesh_render_pipeline_json(info);
      apitrace_metal_register_mesh_render_pipeline(
          dxmt_apitrace_session,
          params->ret_pso,
          descriptor_json.UTF8String,
          info->object_function,
          info->mesh_function,
          info->fragment_function);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBlitCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  id<MTLBlitCommandEncoder> encoder = (id<MTLBlitCommandEncoder>)params->encoder;
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    DXMTApitraceEncoderState *state = [dxmt_apitrace_encoders objectForKey:dxmt_apitrace_key(params->encoder)];
    if (dxmt_apitrace_session && state) {
      dxmt_apitrace_record_blit_commands(dxmt_apitrace_session, params->encoder, next);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  while (next) {
    switch ((enum WMTBlitCommandType)next->type) {
    default:
      assert(!next->type && "unhandled blit command type");
      break;
    case WMTBlitCommandCopyFromBufferToBuffer: {
      struct wmtcmd_blit_copy_from_buffer_to_buffer *body = (struct wmtcmd_blit_copy_from_buffer_to_buffer *)next;
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
                     toBuffer:(id<MTLBuffer>)body->dst
            destinationOffset:body->dst_offset
                         size:body->copy_length];
      break;
    }
    case WMTBlitCommandCopyFromBufferToTexture: {
      struct wmtcmd_blit_copy_from_buffer_to_texture *body = (struct wmtcmd_blit_copy_from_buffer_to_texture *)next;
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
            sourceBytesPerRow:body->bytes_per_row
          sourceBytesPerImage:body->bytes_per_image
                   sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                    toTexture:(id<MTLTexture>)body->dst
             destinationSlice:body->slice
             destinationLevel:body->level
            destinationOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)];
      break;
    }
    case WMTBlitCommandCopyFromTextureToBuffer: {
      struct wmtcmd_blit_copy_from_texture_to_buffer *body = (struct wmtcmd_blit_copy_from_texture_to_buffer *)next;
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                       sourceSlice:body->slice
                       sourceLevel:body->level
                      sourceOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)
                        sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                          toBuffer:(id<MTLBuffer>)body->dst
                 destinationOffset:body->offset
            destinationBytesPerRow:body->bytes_per_row
          destinationBytesPerImage:body->bytes_per_image];
      break;
    }
    case WMTBlitCommandCopyFromTextureToTexture: {
      struct wmtcmd_blit_copy_from_texture_to_texture *body = (struct wmtcmd_blit_copy_from_texture_to_texture *)next;
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                   sourceSlice:body->src_slice
                   sourceLevel:body->src_level
                  sourceOrigin:MTLOriginMake(body->src_origin.x, body->src_origin.y, body->src_origin.z)
                    sourceSize:MTLSizeMake(body->src_size.width, body->src_size.height, body->src_size.depth)
                     toTexture:(id<MTLTexture>)body->dst
              destinationSlice:body->dst_slice
              destinationLevel:body->dst_level
             destinationOrigin:MTLOriginMake(body->dst_origin.x, body->dst_origin.y, body->dst_origin.z)];
      break;
    }
    case WMTBlitCommandGenerateMipmaps: {
      struct wmtcmd_blit_generate_mipmaps *body = (struct wmtcmd_blit_generate_mipmaps *)next;
      [encoder generateMipmapsForTexture:(id<MTLTexture>)body->texture];
      break;
    }
    case WMTBlitCommandUpdateFence: {
      struct wmtcmd_blit_fence_op *body = (struct wmtcmd_blit_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence];
      break;
    }
    case WMTBlitCommandWaitForFence: {
      struct wmtcmd_blit_fence_op *body = (struct wmtcmd_blit_fence_op *)next;
      [encoder waitForFence:(id<MTLFence>)body->fence];
      break;
    }
    case WMTBlitCommandFillBuffer: {
      struct wmtcmd_blit_fillbuffer *body = (struct wmtcmd_blit_fillbuffer *)next;
      [encoder fillBuffer:(id<MTLBuffer>)body->buffer range:NSMakeRange(body->offset, body->length) value:body->value];
      break;
    }
    case WMTBlitCommandResolveCounters: {
      struct wmtcmd_blit_resolvecounters *body = (struct wmtcmd_blit_resolvecounters *)next;
      [encoder resolveCounters:(id<MTLCounterSampleBuffer>)body->sample_buffer
                       inRange:NSMakeRange(body->start, body->len)
             destinationBuffer:(id<MTLBuffer>)body->dst_buffer
             destinationOffset:body->dst_offset];
      break;
    }
    }

    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLComputeCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  id<MTLComputeCommandEncoder> encoder = (id<MTLComputeCommandEncoder>)params->encoder;
  MTLSize threadgroup_size = {0, 0, 0};
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    DXMTApitraceEncoderState *state = [dxmt_apitrace_encoders objectForKey:dxmt_apitrace_key(params->encoder)];
    if (dxmt_apitrace_session && state) {
      dxmt_apitrace_record_compute_commands(dxmt_apitrace_session, params->encoder, next);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  while (next) {
    switch ((enum WMTComputeCommandType)next->type) {
    default:
      assert(!next->type && "unhandled compute command type");
      break;
    case WMTComputeCommandDispatch: {
      struct wmtcmd_compute_dispatch *body = (struct wmtcmd_compute_dispatch *)next;
      [encoder dispatchThreadgroups:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
              threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandDispatchThreads: {
      struct wmtcmd_compute_dispatch *body = (struct wmtcmd_compute_dispatch *)next;
      [encoder dispatchThreads:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
          threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandDispatchIndirect: {
      struct wmtcmd_compute_dispatch_indirect *body = (struct wmtcmd_compute_dispatch_indirect *)next;
      [encoder dispatchThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                                 indirectBufferOffset:body->indirect_args_offset
                                threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandSetPSO: {
      struct wmtcmd_compute_setpso *body = (struct wmtcmd_compute_setpso *)next;
      [encoder setComputePipelineState:(id<MTLComputePipelineState>)body->pso];
      threadgroup_size.width = body->threadgroup_size.width;
      threadgroup_size.height = body->threadgroup_size.height;
      threadgroup_size.depth = body->threadgroup_size.depth;
      break;
    }
    case WMTComputeCommandSetBuffer: {
      struct wmtcmd_compute_setbuffer *body = (struct wmtcmd_compute_setbuffer *)next;
      [encoder setBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTComputeCommandSetBufferOffset: {
      struct wmtcmd_compute_setbufferoffset *body = (struct wmtcmd_compute_setbufferoffset *)next;
      [encoder setBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTComputeCommandSetArgumentBuffer: {
      struct wmtcmd_compute_setargumentbuffer *body = (struct wmtcmd_compute_setargumentbuffer *)next;
      [encoder setBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTComputeCommandSetArgumentBufferOffset: {
      struct wmtcmd_compute_setargumentbufferoffset *body =
          (struct wmtcmd_compute_setargumentbufferoffset *)next;
      [encoder setBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTComputeCommandUseResource: {
      struct wmtcmd_compute_useresource *body = (struct wmtcmd_compute_useresource *)next;
      [encoder useResource:(id<MTLResource>)body->resource usage:(MTLResourceUsage)body->usage];
      break;
    }
    case WMTComputeCommandSetBytes: {
      struct wmtcmd_compute_setbytes *body = (struct wmtcmd_compute_setbytes *)next;
      [encoder setBytes:body->bytes.ptr length:body->length atIndex:body->index];
      break;
    }
    case WMTComputeCommandSetTexture: {
      struct wmtcmd_compute_settexture *body = (struct wmtcmd_compute_settexture *)next;
      [encoder setTexture:(id<MTLTexture>)body->texture atIndex:body->index];
      break;
    }
    case WMTComputeCommandUpdateFence: {
      struct wmtcmd_compute_fence_op *body = (struct wmtcmd_compute_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence];
      break;
    }
    case WMTComputeCommandWaitForFence: {
      struct wmtcmd_compute_fence_op *body = (struct wmtcmd_compute_fence_op *)next;
      [encoder waitForFence:(id<MTLFence>)body->fence];
      break;
    }
    case WMTComputeCommandMemoryBarrier: {
      struct wmtcmd_compute_memory_barrier *body = (struct wmtcmd_compute_memory_barrier *)next;
      [encoder memoryBarrierWithScope:(MTLBarrierScope)body->scope];
      break;
    }
    }

    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLRenderCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  id<MTLRenderCommandEncoder> encoder = (id<MTLRenderCommandEncoder>)params->encoder;
  id<MTLRenderPipelineState> current_pso = nil;
  id<MTLDepthStencilState> current_dsso = nil;
  uint8_t current_stencil_ref = 0;
  bool current_stencil_ref_valid = false;
  bool current_rasterizer_state_valid = false;
  struct wmtcmd_render_setrasterizerstate current_rasterizer_state = {0};
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled()) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    DXMTApitraceEncoderState *state = [dxmt_apitrace_encoders objectForKey:dxmt_apitrace_key(params->encoder)];
    if (dxmt_apitrace_session && state) {
      dxmt_apitrace_record_render_commands(dxmt_apitrace_session, params->encoder, next);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  while (next) {
    switch ((enum WMTRenderCommandType)next->type) {
    default:
      assert(!next->type && "unhandled render command type");
      break;
    case WMTRenderCommandNop:
      break;
    case WMTRenderCommandUseResource: {
      struct wmtcmd_render_useresource *body = (struct wmtcmd_render_useresource *)next;
      [encoder useResource:(id<MTLResource>)body->resource
                     usage:(MTLResourceUsage)body->usage
                    stages:(MTLRenderStages)body->stages];
      break;
    }
    case WMTRenderCommandSetVertexBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      [encoder setVertexBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetVertexBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      [encoder setVertexBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetFragmentBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      [encoder setFragmentBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetFragmentBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      [encoder setFragmentBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetArgumentBuffer: {
      struct wmtcmd_render_setargumentbuffer *body = (struct wmtcmd_render_setargumentbuffer *)next;
      if (body->stages & WMTRenderStageVertex)
        [encoder setVertexBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      if (body->stages & WMTRenderStageFragment)
        [encoder setFragmentBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      if (body->stages & WMTRenderStageObject)
        [encoder setObjectBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      if (body->stages & WMTRenderStageMesh)
        [encoder setMeshBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetArgumentBufferOffset: {
      struct wmtcmd_render_setargumentbufferoffset *body =
          (struct wmtcmd_render_setargumentbufferoffset *)next;
      if (body->stages & WMTRenderStageVertex)
        [encoder setVertexBufferOffset:body->offset atIndex:body->index];
      if (body->stages & WMTRenderStageFragment)
        [encoder setFragmentBufferOffset:body->offset atIndex:body->index];
      if (body->stages & WMTRenderStageObject)
        [encoder setObjectBufferOffset:body->offset atIndex:body->index];
      if (body->stages & WMTRenderStageMesh)
        [encoder setMeshBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetMeshBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      [encoder setMeshBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetMeshBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      [encoder setMeshBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetObjectBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetObjectBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      [encoder setObjectBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetFragmentBytes: {
      struct wmtcmd_render_setbytes *body = (struct wmtcmd_render_setbytes *)next;
      [encoder setFragmentBytes:body->bytes.ptr length:body->length atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetFragmentTexture: {
      struct wmtcmd_render_settexture *body = (struct wmtcmd_render_settexture *)next;
      [encoder setFragmentTexture:(id<MTLTexture>)body->texture atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetRasterizerState: {
      struct wmtcmd_render_setrasterizerstate *body = (struct wmtcmd_render_setrasterizerstate *)next;
      if (!current_rasterizer_state_valid ||
          current_rasterizer_state.fill_mode != body->fill_mode ||
          current_rasterizer_state.cull_mode != body->cull_mode ||
          current_rasterizer_state.depth_clip_mode != body->depth_clip_mode ||
          current_rasterizer_state.winding != body->winding ||
          current_rasterizer_state.depth_bias != body->depth_bias ||
          current_rasterizer_state.scole_scale != body->scole_scale ||
          current_rasterizer_state.depth_bias_clamp != body->depth_bias_clamp) {
        [encoder setTriangleFillMode:(MTLTriangleFillMode)body->fill_mode];
        [encoder setCullMode:(MTLCullMode)body->cull_mode];
        [encoder setDepthClipMode:(MTLDepthClipMode)body->depth_clip_mode];
        [encoder setDepthBias:body->depth_bias slopeScale:body->scole_scale clamp:body->depth_bias_clamp];
        [encoder setFrontFacingWinding:(MTLWinding)body->winding];
        current_rasterizer_state = *body;
        current_rasterizer_state_valid = true;
      }
      break;
    }
    case WMTRenderCommandSetViewports: {
      struct wmtcmd_render_setviewports *body = (struct wmtcmd_render_setviewports *)next;
      [encoder setViewports:(const MTLViewport *)body->viewports.ptr count:body->viewport_count];
      break;
    }
    case WMTRenderCommandSetScissorRects: {
      struct wmtcmd_render_setscissorrects *body = (struct wmtcmd_render_setscissorrects *)next;
      [encoder setScissorRects:(const MTLScissorRect *)body->scissor_rects.ptr count:body->rect_count];
      break;
    }
    case WMTRenderCommandSetPSO: {
      struct wmtcmd_render_setpso *body = (struct wmtcmd_render_setpso *)next;
      id<MTLRenderPipelineState> pso = (id<MTLRenderPipelineState>)body->pso;
      if (current_pso != pso) {
        [encoder setRenderPipelineState:pso];
        current_pso = pso;
      }
      break;
    }
    case WMTRenderCommandSetDSSO: {
      struct wmtcmd_render_setdsso *body = (struct wmtcmd_render_setdsso *)next;
      id<MTLDepthStencilState> dsso = (id<MTLDepthStencilState>)body->dsso;
      if (current_dsso != dsso) {
        [encoder setDepthStencilState:dsso];
        current_dsso = dsso;
      }
      if (!current_stencil_ref_valid || current_stencil_ref != body->stencil_ref) {
        [encoder setStencilReferenceValue:body->stencil_ref];
        current_stencil_ref = body->stencil_ref;
        current_stencil_ref_valid = true;
      }
      break;
    }
    case WMTRenderCommandSetBlendFactorAndStencilRef: {
      struct wmtcmd_render_setblendcolor *body = (struct wmtcmd_render_setblendcolor *)next;
      [encoder setBlendColorRed:body->red green:body->green blue:body->blue alpha:body->alpha];
      if (!current_stencil_ref_valid || current_stencil_ref != body->stencil_ref) {
        [encoder setStencilReferenceValue:body->stencil_ref];
        current_stencil_ref = body->stencil_ref;
        current_stencil_ref_valid = true;
      }
      break;
    }
    case WMTRenderCommandSetVisibilityMode: {
      struct wmtcmd_render_setvisibilitymode *body = (struct wmtcmd_render_setvisibilitymode *)next;
      [encoder setVisibilityResultMode:(MTLVisibilityResultMode)body->mode offset:body->offset];
      break;
    }
    case WMTRenderCommandDraw: {
      struct wmtcmd_render_draw *body = (struct wmtcmd_render_draw *)next;
      [encoder drawPrimitives:(MTLPrimitiveType)body->primitive_type
                  vertexStart:body->vertex_start
                  vertexCount:body->vertex_count
                instanceCount:body->instance_count
                 baseInstance:body->base_instance];
      break;
    }
    case WMTRenderCommandDrawIndexed: {
      struct wmtcmd_render_draw_indexed *body = (struct wmtcmd_render_draw_indexed *)next;
      [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type
                          indexCount:body->index_count
                           indexType:(MTLIndexType)body->index_type
                         indexBuffer:(id<MTLBuffer>)body->index_buffer
                   indexBufferOffset:body->index_buffer_offset
                       instanceCount:body->instance_count
                          baseVertex:body->base_vertex
                        baseInstance:body->base_instance];
      break;
    }
    case WMTRenderCommandDrawIndirect: {
      struct wmtcmd_render_draw_indirect *body = (struct wmtcmd_render_draw_indirect *)next;
      [encoder drawPrimitives:(MTLPrimitiveType)body->primitive_type
                indirectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
          indirectBufferOffset:body->indirect_args_offset];
      break;
    }
    case WMTRenderCommandDrawIndexedIndirect: {
      struct wmtcmd_render_draw_indexed_indirect *body = (struct wmtcmd_render_draw_indexed_indirect *)next;
      [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type
                           indexType:(MTLIndexType)body->index_type
                         indexBuffer:(id<MTLBuffer>)body->index_buffer
                   indexBufferOffset:body->index_buffer_offset
                      indirectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                indirectBufferOffset:body->indirect_args_offset];
      break;
    }
    case WMTRenderCommandDrawMeshThreadgroups: {
      struct wmtcmd_render_draw_meshthreadgroups *body = (struct wmtcmd_render_draw_meshthreadgroups *)next;
      [encoder drawMeshThreadgroups:MTLSizeMake(
                                        body->threadgroup_per_grid.width, body->threadgroup_per_grid.height,
                                        body->threadgroup_per_grid.depth
                                    )
          threadsPerObjectThreadgroup:MTLSizeMake(
                                          body->object_threadgroup_size.width, body->object_threadgroup_size.height,
                                          body->object_threadgroup_size.depth
                                      )
            threadsPerMeshThreadgroup:MTLSizeMake(
                                          body->mesh_threadgroup_size.width, body->mesh_threadgroup_size.height,
                                          body->mesh_threadgroup_size.depth
                                      )];
      break;
    }
    case WMTRenderCommandDrawMeshThreadgroupsIndirect: {
      struct wmtcmd_render_draw_meshthreadgroups_indirect *body =
          (struct wmtcmd_render_draw_meshthreadgroups_indirect *)next;
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                                 indirectBufferOffset:body->indirect_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(
                                                          body->object_threadgroup_size.width,
                                                          body->object_threadgroup_size.height,
                                                          body->object_threadgroup_size.depth
                                                      )
                            threadsPerMeshThreadgroup:MTLSizeMake(
                                                          body->mesh_threadgroup_size.width,
                                                          body->mesh_threadgroup_size.height,
                                                          body->mesh_threadgroup_size.depth
                                                      )];
      break;
    }
    case WMTRenderCommandMemoryBarrier: {
      struct wmtcmd_render_memory_barrier *body = (struct wmtcmd_render_memory_barrier *)next;
      [encoder memoryBarrierWithScope:(MTLBarrierScope)body->scope
                          afterStages:(MTLRenderStages)body->stages_after
                         beforeStages:(MTLRenderStages)body->stages_before];
      break;
    }
    case WMTRenderCommandDXMTGeometryDraw: {
      struct wmtcmd_render_dxmt_geometry_draw *body = (struct wmtcmd_render_dxmt_geometry_draw *)next;
      [encoder setObjectBufferOffset:body->draw_arguments_offset atIndex:21];
      [encoder drawMeshThreadgroups:MTLSizeMake(body->warp_count, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndexed: {
      struct wmtcmd_render_dxmt_geometry_draw_indexed *body = (struct wmtcmd_render_dxmt_geometry_draw_indexed *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->index_buffer offset:body->index_buffer_offset atIndex:20];
      [encoder setObjectBufferOffset:body->draw_arguments_offset atIndex:21];
      [encoder drawMeshThreadgroups:MTLSizeMake(body->warp_count, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndirect: {
      struct wmtcmd_render_dxmt_geometry_draw_indirect *body = (struct wmtcmd_render_dxmt_geometry_draw_indirect *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->indirect_args_buffer offset:body->indirect_args_offset atIndex:21];
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->dispatch_args_buffer
                                 indirectBufferOffset:body->dispatch_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->imm_draw_arguments offset:0 atIndex:21];
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndexedIndirect: {
      struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect *body =
          (struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->index_buffer offset:body->index_buffer_offset atIndex:20];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->indirect_args_buffer offset:body->indirect_args_offset atIndex:21];
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->dispatch_args_buffer
                                 indirectBufferOffset:body->dispatch_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->imm_draw_arguments offset:0 atIndex:21];
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDraw: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw *body = (struct wmtcmd_render_dxmt_tessellation_mesh_draw *)next;
      [encoder setObjectBufferOffset:body->draw_arguments_offset atIndex:21];
      [encoder drawMeshThreadgroups:MTLSizeMake(body->patch_per_mesh_instance, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDrawIndexed: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed *body = (struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->index_buffer offset:body->index_buffer_offset atIndex:20];
      [encoder setObjectBufferOffset:body->draw_arguments_offset atIndex:21];
      [encoder drawMeshThreadgroups:MTLSizeMake(body->patch_per_mesh_instance, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }

    case WMTRenderCommandDXMTTessellationMeshDrawIndirect: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect *body = (struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->indirect_args_buffer offset:body->indirect_args_offset atIndex:21];
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->dispatch_args_buffer
                                 indirectBufferOffset:body->dispatch_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->imm_draw_arguments offset:0 atIndex:21];
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect *body =
          (struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->index_buffer offset:body->index_buffer_offset atIndex:20];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->indirect_args_buffer offset:body->indirect_args_offset atIndex:21];
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->dispatch_args_buffer
                                 indirectBufferOffset:body->dispatch_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->imm_draw_arguments offset:0 atIndex:21];
      break;
    }
    case WMTRenderCommandUpdateFence: {
      struct wmtcmd_render_fence_op *body = (struct wmtcmd_render_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence afterStages:(MTLRenderStages)body->stages];
      break;
    }
    case WMTRenderCommandWaitForFence: {
      struct wmtcmd_render_fence_op *body = (struct wmtcmd_render_fence_op *)next;
      [encoder waitForFence:(id<MTLFence>)body->fence beforeStages:(MTLRenderStages)body->stages];
      break;
    }
    case WMTRenderCommandSetViewport: {
      struct wmtcmd_render_setviewport *body = (struct wmtcmd_render_setviewport *)next;
      union {
        struct WMTViewport src;
        MTLViewport dst;
      } u = {.src = body->viewport};
      [encoder setViewport:u.dst];
      break;
    }
    case WMTRenderCommandSetScissorRect: {
      struct wmtcmd_render_setscissorrect *body = (struct wmtcmd_render_setscissorrect *)next;
      struct WMTScissorRect r = body->scissor_rect;
      // Clamp into the render-pass bounds. Metal treats an out-of-bounds
      // scissor as undefined and the debug layer aborts; the d3d9 blit / mip
      // paths can exceed a smaller mip-level target. MoltenVK clips every
      // scissor at this same point (MVKCommandEncoder::clipToRenderArea).
      // No-op for in-bounds rects.
      if (g_dxmt_pass_rt_w && g_dxmt_pass_rt_h) {
        if (r.x > g_dxmt_pass_rt_w)
          r.x = g_dxmt_pass_rt_w;
        if (r.y > g_dxmt_pass_rt_h)
          r.y = g_dxmt_pass_rt_h;
        if (r.x + r.width > g_dxmt_pass_rt_w)
          r.width = g_dxmt_pass_rt_w - r.x;
        if (r.y + r.height > g_dxmt_pass_rt_h)
          r.height = g_dxmt_pass_rt_h - r.y;
      }
      union {
        struct WMTScissorRect src;
        MTLScissorRect dst;
      } u = {.src = r};
      [encoder setScissorRect:u.dst];
      break;
    }
    case WMTRenderCommandDispatchThreadsPerTile: {
      struct wmtcmd_render_dispatch_threads_per_tile *body = (struct wmtcmd_render_dispatch_threads_per_tile *)next;
      [encoder dispatchThreadsPerTile:MTLSizeMake(body->width, body->height, 1)];
      break;
    }
    case WMTRenderCommandSetFragmentSamplerState: {
      struct wmtcmd_render_setsamplerstate *body = (struct wmtcmd_render_setsamplerstate *)next;
      [encoder setFragmentSamplerState:(id<MTLSamplerState>)body->sampler atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetVertexTexture: {
      struct wmtcmd_render_settexture *body = (struct wmtcmd_render_settexture *)next;
      [encoder setVertexTexture:(id<MTLTexture>)body->texture atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetVertexSamplerState: {
      struct wmtcmd_render_setsamplerstate *body = (struct wmtcmd_render_setsamplerstate *)next;
      [encoder setVertexSamplerState:(id<MTLSamplerState>)body->sampler atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetBlendColor: {
      struct wmtcmd_render_setblendcolor_only *body = (struct wmtcmd_render_setblendcolor_only *)next;
      [encoder setBlendColorRed:body->red green:body->green blue:body->blue alpha:body->alpha];
      break;
    }
    }
    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_pixelFormat(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle pixelFormat];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_usage(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle usage];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_width(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle width];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_height(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle height];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_depth(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle depth];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_arrayLength(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle arrayLength];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_mipmapLevelCount(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle mipmapLevelCount];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_replaceRegion(void *obj) {
  struct unixcall_mtltexture_replaceregion *params = obj;
  [(id<MTLTexture>)params->texture replaceRegion:MTLRegionMake3D(
                                                     params->origin.x, params->origin.y, params->origin.z,
                                                     params->size.width, params->size.height, params->size.depth
                                                 )
                                     mipmapLevel:params->level
                                           slice:params->slice
                                       withBytes:params->data.ptr
                                     bytesPerRow:params->bytes_per_row
                                   bytesPerImage:params->bytes_per_image];
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && params->data.ptr) {
    const uint64_t bytes_size =
        dxmt_apitrace_texture_region_bytes_size(&params->size, params->bytes_per_row, params->bytes_per_image);
    if (bytes_size) {
      pthread_mutex_lock(&dxmt_apitrace_lock);
      if (dxmt_apitrace_ensure_session_locked()) {
        apitrace_metal_replace_texture_region(
            dxmt_apitrace_session,
            params->texture,
            params->origin.x,
            params->origin.y,
            params->origin.z,
            params->size.width,
            params->size.height,
            params->size.depth,
            (uint32_t)params->level,
            (uint32_t)params->slice,
            params->bytes_per_row,
            params->bytes_per_image,
            params->data.ptr,
            bytes_size);
      }
      pthread_mutex_unlock(&dxmt_apitrace_lock);
    }
  }
#endif
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBuffer_didModifyRange(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  [(id<MTLBuffer>)params->handle didModifyRange:NSMakeRange(params->arg, params->ret)];
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && [(id<MTLBuffer>)params->handle storageMode] != MTLStorageModePrivate) {
    const uint64_t offset = params->arg;
    const uint64_t length = params->ret;
    const uint64_t buffer_length = [(id<MTLBuffer>)params->handle length];
    const void *contents = [(id<MTLBuffer>)params->handle contents];
    if (contents && length && offset <= buffer_length && length <= buffer_length - offset) {
      pthread_mutex_lock(&dxmt_apitrace_lock);
      if (dxmt_apitrace_ensure_session_locked()) {
        apitrace_metal_update_buffer_contents(
            dxmt_apitrace_session,
            params->handle,
            offset,
            length,
            (uint32_t)[(id<MTLBuffer>)params->handle storageMode],
            (const uint8_t *)contents + offset,
            length);
      }
      pthread_mutex_unlock(&dxmt_apitrace_lock);
    }
  }
#endif
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_presentDrawable(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  [(id<MTLCommandBuffer>)params->handle presentDrawable:(id<MTLDrawable>)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_presentDrawableAfterMinimumDuration(void *obj) {
  struct unixcall_generic_obj_obj_double_noret *params = obj;
  [(id<MTLCommandBuffer>)params->handle presentDrawable:(id<MTLDrawable>)params->arg0
                                   afterMinimumDuration:params->arg1];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsFamily(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle supportsFamily:(MTLGPUFamily)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsBCTextureCompression(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle supportsBCTextureCompression];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsTextureSampleCount(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle supportsTextureSampleCount:params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_hasUnifiedMemory(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle hasUnifiedMemory];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCaptureManager_sharedCaptureManager(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = (obj_handle_t)[MTLCaptureManager sharedCaptureManager];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCaptureManager_startCapture(void *obj) {
  struct unixcall_mtlcapturemanager_startcapture *params = obj;
  MTLCaptureDescriptor *desc = [[MTLCaptureDescriptor alloc] init];
  const struct WMTCaptureInfo *info = params->info.ptr;
  desc.destination = (MTLCaptureDestination)info->destination;
  desc.captureObject = (id)info->capture_object;
  NSString *path_str = [[NSString alloc] initWithCString:info->output_url.ptr encoding:NSUTF8StringEncoding];
  NSURL *url = [[NSURL alloc] initFileURLWithPath:path_str];
  desc.outputURL = url;
  [(MTLCaptureManager *)params->capture_manager startCaptureWithDescriptor:desc error:nil];
  [url release];
  [path_str release];
  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCaptureManager_stopCapture(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(MTLCaptureManager *)params->handle stopCapture];
  return STATUS_SUCCESS;
}

#include <signal.h>

void
temp_handler(int signum) {
  fprintf(stderr, "received signal %d in temp_handler(), and it may cause problem!\n", signum);
}

static const int SIGNALS[] = {
    SIGHUP,
    SIGINT,
    SIGTERM,
    SIGUSR2,
    SIGILL,
    SIGTRAP,
    SIGABRT,
    SIGFPE,
    SIGBUS,
    SIGSEGV,
    SIGQUIT
#ifdef SIGSYS
    ,
    SIGSYS
#endif
#ifdef SIGXCPU
    ,
    SIGXCPU
#endif
#ifdef SIGXFSZ
    ,
    SIGXFSZ
#endif
#ifdef SIGEMT
    ,
    SIGEMT
#endif
    ,
    SIGUSR1
#ifdef SIGINFO
    ,
    SIGINFO
#endif
};

static NTSTATUS
_MTLDevice_newTemporalScaler(void *obj) {
  struct unixcall_mtldevice_newfxtemporalscaler *params = obj;
  MTLFXTemporalScalerDescriptor *desc = [[MTLFXTemporalScalerDescriptor alloc] init];
  const struct WMTFXTemporalScalerInfo *info = params->info.ptr;
  desc.colorTextureFormat = to_metal_pixel_format(info->color_format);
  desc.outputTextureFormat = to_metal_pixel_format(info->output_format);
  desc.depthTextureFormat = to_metal_pixel_format(info->depth_format);
  desc.motionTextureFormat = to_metal_pixel_format(info->motion_format);
  desc.inputWidth = info->input_width;
  desc.inputHeight = info->input_height;
  desc.outputWidth = info->output_width;
  desc.outputHeight = info->output_height;
  desc.inputContentMaxScale = info->input_content_max_scale;
  desc.inputContentMinScale = info->input_content_min_scale;
  desc.inputContentPropertiesEnabled = info->input_content_properties_enabled;
  #if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    desc.requiresSynchronousInitialization = info->requires_synchronous_initialization;
  }
  #endif
  desc.autoExposureEnabled = info->auto_exposure;

  struct sigaction old_action[sizeof(SIGNALS) / sizeof(int)], new_action;
  if (@available(macOS 16, *)) {} else {
    new_action.sa_handler = temp_handler;
    sigemptyset(&new_action.sa_mask);
    new_action.sa_flags = 0;
    for (unsigned int i = 0; i < sizeof(SIGNALS) / sizeof(int); i++)
      sigaction(SIGNALS[i], &new_action, &old_action[i]);
  }

  params->ret = (obj_handle_t)[desc newTemporalScalerWithDevice:(id<MTLDevice>)params->device];

  if (@available(macOS 16, *)) {} else {
    for (unsigned int i = 0; i < sizeof(SIGNALS) / sizeof(int); i++)
      sigaction(SIGNALS[i], &old_action[i], NULL);
  }

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newSpatialScaler(void *obj) {
  struct unixcall_mtldevice_newfxspatialscaler *params = obj;
  MTLFXSpatialScalerDescriptor *desc = [[MTLFXSpatialScalerDescriptor alloc] init];
  const struct WMTFXSpatialScalerInfo *info = params->info.ptr;
  desc.colorTextureFormat = to_metal_pixel_format(info->color_format);
  desc.outputTextureFormat = to_metal_pixel_format(info->output_format);
  desc.inputWidth = info->input_width;
  desc.inputHeight = info->input_height;
  desc.outputWidth = info->output_width;
  desc.outputHeight = info->output_height;
  params->ret = (obj_handle_t)[desc newSpatialScalerWithDevice:(id<MTLDevice>)params->device];
  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeTemporalScale(void *obj) {
  struct unixcall_mtlcommandbuffer_temporal_scale *params = obj;
  id<MTLCommandBuffer> cmdbuf = (id<MTLCommandBuffer>)params->cmdbuf;
  id<MTLFXTemporalScaler> scaler = (id<MTLFXTemporalScaler>)params->scaler;
  scaler.colorTexture = (id<MTLTexture>)params->color;
  scaler.outputTexture = (id<MTLTexture>)params->output;
  scaler.depthTexture = (id<MTLTexture>)params->depth;
  scaler.motionTexture = (id<MTLTexture>)params->motion;
  scaler.exposureTexture = (id<MTLTexture>)params->exposure;
  scaler.fence = (id<MTLFence>)params->fence;
  const struct WMTFXTemporalScalerProps *props = params->props.ptr;
  scaler.inputContentWidth = props->input_content_width;
  scaler.inputContentHeight = props->input_content_height;
  scaler.reset = props->reset;
  scaler.depthReversed = props->depth_reversed;
  scaler.motionVectorScaleX = props->motion_vector_scale_x;
  scaler.motionVectorScaleY = props->motion_vector_scale_y;
  scaler.jitterOffsetX = props->jitter_offset_x;
  scaler.jitterOffsetY = props->jitter_offset_y;
  scaler.preExposure = props->pre_exposure;
  [scaler encodeToCommandBuffer:cmdbuf];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeSpatialScale(void *obj) {
  struct unixcall_mtlcommandbuffer_spatial_scale *params = obj;
  id<MTLCommandBuffer> cmdbuf = (id<MTLCommandBuffer>)params->cmdbuf;
  id<MTLFXSpatialScaler> scaler = (id<MTLFXSpatialScaler>)params->scaler;
  scaler.colorTexture = (id<MTLTexture>)params->color;
  scaler.outputTexture = (id<MTLTexture>)params->output;
  scaler.fence = (id<MTLFence>)params->fence;
  [scaler encodeToCommandBuffer:cmdbuf];
  return STATUS_SUCCESS;
}

/* Metal 4 spatial scaler: descriptor shared with the MTL3 path, but the
 * scaler object is created against an MTL4Compiler and encodes into an
 * MTL4CommandBuffer. */
static NTSTATUS
_MTLDevice_newMTL4SpatialScaler(void *obj) {
  struct unixcall_mtldevice_new_mtl4fxspatialscaler *params = obj;
  MTLFXSpatialScalerDescriptor *desc = [[MTLFXSpatialScalerDescriptor alloc] init];
  const struct WMTFXSpatialScalerInfo *info = params->info.ptr;
  desc.colorTextureFormat = to_metal_pixel_format(info->color_format);
  desc.outputTextureFormat = to_metal_pixel_format(info->output_format);
  desc.inputWidth = info->input_width;
  desc.inputHeight = info->input_height;
  desc.outputWidth = info->output_width;
  desc.outputHeight = info->output_height;
  params->ret = (obj_handle_t)[desc newMTL4SpatialScalerWithDevice:(id<MTLDevice>)params->device
                                                          compiler:(id<MTL4Compiler>)params->compiler];
  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandBuffer_encodeSpatialScale(void *obj) {
  struct unixcall_mtl4commandbuffer_spatial_scale *params = obj;
  id<MTL4CommandBuffer> cmdbuf = (id<MTL4CommandBuffer>)params->cmdbuf;
  id<MTL4FXSpatialScaler> scaler = (id<MTL4FXSpatialScaler>)params->scaler;
  scaler.colorTexture = (id<MTLTexture>)params->color;
  scaler.outputTexture = (id<MTLTexture>)params->output;
  [scaler encodeToCommandBuffer:cmdbuf];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_string(void *obj) {
  struct unixcall_nsstring_string *params = obj;
  NSString *str = [NSString stringWithCString:params->buffer_ptr.ptr encoding:(NSStringEncoding)params->encoding];
  params->ret = (obj_handle_t)str;
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_alloc_init(void *obj) {
  struct unixcall_nsstring_string *params = obj;
  NSString *str = [[NSString alloc] initWithCString:params->buffer_ptr.ptr encoding:(NSStringEncoding)params->encoding];
  params->ret = (obj_handle_t)str;
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_instance(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret =
      (obj_handle_t)((id(*)(id, SEL))objc_msgSend)(objc_lookUpClass("_CADeveloperHUDProperties"), @selector(instance));
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_addLabel(void *obj) {
  struct unixcall_generic_obj_obj_obj_uint64_ret *params = obj;
  params->ret = ((bool (*)(id, SEL, id, id)
  )objc_msgSend)((id)params->handle, @selector(addLabel:after:), (id)params->arg0, (id)params->arg1);
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_updateLabel(void *obj) {
  struct unixcall_generic_obj_obj_obj_noret *params = obj;
  ((void (*)(id, SEL, id, id)
  )objc_msgSend)((id)params->handle, @selector(updateLabel:value:), (id)params->arg0, (id)params->arg1);
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_remove(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  ((void (*)(id, SEL, id))objc_msgSend)((id)params->handle, @selector(remove:), (id)params->arg);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalDrawable_texture(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  id<MTLTexture> texture = [(id<CAMetalDrawable>)params->handle texture];
  params->ret = (obj_handle_t)texture;
  if (getenv("D9MT_WSILOG"))
    fprintf(stderr, "D9MT-WSI[unix] drawable_texture drawable=%p -> tex=%p\n",
            (void *)params->handle, (void *)texture);
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && texture) {
    struct WMTTextureInfo info;
    extract_texture_descriptor(texture, &info);
    info.gpu_resource_id = [texture gpuResourceID]._impl;
    info.mach_port = 0;
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *resource_payload = dxmt_apitrace_json_string(@{
        @"kind" : @"dxmt_texture_gpu_resource_id",
        @"texture_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)params->ret],
        @"gpu_resource_id" : [NSNumber numberWithUnsignedLongLong:(unsigned long long)info.gpu_resource_id],
      });
      apitrace_metal_object_metadata(dxmt_apitrace_session, params->ret, resource_payload.UTF8String);
      NSString *descriptor_json = dxmt_apitrace_texture_descriptor_json(&info);
      apitrace_metal_register_drawable_texture(
          dxmt_apitrace_session,
          params->ret,
          params->handle,
          descriptor_json.UTF8String);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  return STATUS_SUCCESS;
}

/* D9MT_WSILOG=1: trace the unix-side present path to stderr (counts + handles)
 * so we can tell whether PE-side calls actually arrive here. */
static int d9mt_wsilog_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *v = getenv("D9MT_WSILOG");
    cached = (v && v[0] == '1') ? 1 : 0;
  }
  return cached;
}
#define D9MT_WSILOG(fmt, ...) do { \
    if (d9mt_wsilog_enabled()) \
      fprintf(stderr, "D9MT-WSI[unix] " fmt "\n", ##__VA_ARGS__); \
  } while (0)

static NTSTATUS
_MetalLayer_nextDrawable(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  /* nextDrawable returns an AUTORELEASED drawable; the handle crosses the
   * PE boundary and is presented later from another thread — retain here,
   * the driver releases it right after MetalDrawable_present. */
  params->ret = (obj_handle_t)[[(CAMetalLayer *)params->handle nextDrawable] retain];
  static _Atomic unsigned n_next;
  unsigned k = n_next++;
  if (k < 16 || (k % 100) == 0)
    D9MT_WSILOG("nextDrawable #%u layer=%p -> %p", k,
                (void *)params->handle, (void *)params->ret);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsFXSpatialScaler(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [MTLFXSpatialScalerDescriptor supportsDevice:(id<MTLDevice>)params->handle];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsFXTemporalScaler(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [MTLFXTemporalScalerDescriptor supportsDevice:(id<MTLDevice>)params->handle];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_setProps(void *obj) {
  struct unixcall_generic_obj_constptr_noret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  /* By-value copy: the block runs AFTER this call returns and the caller's
   * params struct is gone. */
  const struct WMTLayerProps props = *(const struct WMTLayerProps *)params->arg.ptr;
  /* ASYNC, never dispatch_sync: a synchronous hop to the main thread
   * deadlocks during display-mode changes — wine's main thread is blocked
   * SendMessage'ing WM_DISPLAYCHANGE to the app thread, which sits in
   * Reset waiting for THIS call (device lock held). Layer props are
   * configuration, not per-frame state; applying them a beat later is
   * harmless. Manual retain: no ARC in this TU, the block must keep the
   * layer alive until it runs. */
  [layer retain];
  void (^apply)(void) = ^{
    layer.device = (id<MTLDevice>)props.device;
    layer.opaque = props.opaque;
    dxmt_set_layer_background_black(layer);
    layer.framebufferOnly = props.framebuffer_only;
    /* Zero means "leave untouched": layer geometry (contentsScale,
     * drawableSize) is owned by the window system's view layout; callers
     * that only configure device/format must not clobber it. */
    if (props.contents_scale > 0.0)
      layer.contentsScale = props.contents_scale;
    layer.displaySyncEnabled = props.display_sync_enabled;
    if (props.drawable_width > 0.0 && props.drawable_height > 0.0)
      layer.drawableSize = CGSizeMake(props.drawable_width, props.drawable_height);
    layer.pixelFormat = to_metal_pixel_format(props.pixel_format);
    if (props.maximum_drawable_count != 0)
      layer.maximumDrawableCount = props.maximum_drawable_count;
    [layer release];
  };
  if ([NSThread isMainThread])
    apply();
  else
    dispatch_async(dispatch_get_main_queue(), apply);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_getProps(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  struct WMTLayerProps *props = params->arg.ptr;
  props->device = (obj_handle_t)layer.device;
  props->opaque = layer.opaque;
  props->framebuffer_only = layer.framebufferOnly;
  props->contents_scale = layer.contentsScale;
  props->display_sync_enabled = layer.displaySyncEnabled;
  props->drawable_height = layer.drawableSize.height;
  props->drawable_width = layer.drawableSize.width;
  props->pixel_format = layer.pixelFormat;
  props->maximum_drawable_count = (uint32_t)layer.maximumDrawableCount;
  return STATUS_SUCCESS;
}

typedef struct macdrv_opaque_metal_device *macdrv_metal_device;
typedef struct macdrv_opaque_metal_view *macdrv_metal_view;
typedef struct macdrv_opaque_metal_layer *macdrv_metal_layer;
typedef struct macdrv_opaque_view *macdrv_view;
typedef struct macdrv_opaque_window *macdrv_window;
typedef struct macdrv_opaque_window_data *macdrv_window_data;
typedef struct opaque_window_surface *window_surface;
typedef struct opaque_HWND *HWND;
struct macdrv_win_data {
  HWND hwnd; /* hwnd that this private data belongs to */
  macdrv_window cocoa_window;
  macdrv_view cocoa_view;
  macdrv_view client_cocoa_view;
};

struct macdrv_functions_t {
  void (*macdrv_init_display_devices)(BOOL);
  struct macdrv_win_data *(*get_win_data)(HWND hwnd);
  void (*release_win_data)(struct macdrv_win_data *data);
  macdrv_window (*macdrv_get_cocoa_window)(HWND hwnd, BOOL require_on_screen);
  macdrv_metal_device (*macdrv_create_metal_device)(void);
  void (*macdrv_release_metal_device)(macdrv_metal_device d);
  macdrv_metal_view (*macdrv_view_create_metal_view)(macdrv_view v, macdrv_metal_device d);
  macdrv_metal_layer (*macdrv_view_get_metal_layer)(macdrv_metal_view v);
  void (*macdrv_view_release_metal_view)(macdrv_metal_view v);
  void (*on_main_thread)(dispatch_block_t b);
};

static NTSTATUS
_CreateMetalViewFromHWND(void *obj) {
  struct unixcall_create_metal_view_from_hwnd *params = obj;

  struct macdrv_win_data *(*pfn_get_win_data)(HWND hwnd) = NULL;
  void (*pfn_release_win_data)(struct macdrv_win_data *data) = NULL;
  macdrv_metal_view (*pfn_macdrv_view_create_metal_view)(macdrv_view v, macdrv_metal_device d) = NULL;
  macdrv_metal_layer (*pfn_macdrv_view_get_metal_layer)(macdrv_metal_view v) = NULL;

  struct macdrv_functions_t *macdrv_functions;
  if ((macdrv_functions = dlsym(RTLD_DEFAULT, "macdrv_functions"))) {
    pfn_get_win_data = macdrv_functions->get_win_data;
    pfn_release_win_data = macdrv_functions->release_win_data;
    pfn_macdrv_view_create_metal_view = macdrv_functions->macdrv_view_create_metal_view;
    pfn_macdrv_view_get_metal_layer = macdrv_functions->macdrv_view_get_metal_layer;
  } else {
    pfn_get_win_data = dlsym(RTLD_DEFAULT, "get_win_data");
    pfn_release_win_data = dlsym(RTLD_DEFAULT, "release_win_data");
    pfn_macdrv_view_create_metal_view = dlsym(RTLD_DEFAULT, "macdrv_view_create_metal_view");
    pfn_macdrv_view_get_metal_layer = dlsym(RTLD_DEFAULT, "macdrv_view_get_metal_layer");
  }

  if (pfn_get_win_data && pfn_release_win_data && pfn_macdrv_view_create_metal_view &&
      pfn_macdrv_view_get_metal_layer) {
    struct macdrv_win_data *win_data = pfn_get_win_data((HWND)params->hwnd);
    macdrv_metal_view view =
        pfn_macdrv_view_create_metal_view(win_data->client_cocoa_view, (macdrv_metal_device)params->device);
    params->ret_view = (obj_handle_t)view;
    if (view) {
      params->ret_layer = (obj_handle_t)pfn_macdrv_view_get_metal_layer(view);
      execute_on_main(^{
        dxmt_set_layer_background_black((CAMetalLayer *)params->ret_layer);
      });
    }
    pfn_release_win_data(win_data);
  }

  return STATUS_SUCCESS;
}

static NTSTATUS
_ReleaseMetalView(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;

  void (*pfn_macdrv_view_release_metal_view)(macdrv_metal_view v) = NULL;

  struct macdrv_functions_t *macdrv_functions;
  if ((macdrv_functions = dlsym(RTLD_DEFAULT, "macdrv_functions"))) {
    pfn_macdrv_view_release_metal_view = macdrv_functions->macdrv_view_release_metal_view;
  } else {
    pfn_macdrv_view_release_metal_view = dlsym(RTLD_DEFAULT, "macdrv_view_release_metal_view");
  }

  if (pfn_macdrv_view_release_metal_view)
    pfn_macdrv_view_release_metal_view((macdrv_metal_view)params->handle);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50Initialize(void *args) {
  struct sm50_initialize_params *params = args;

  params->ret =
      SM50Initialize(params->bytecode, params->bytecode_size, params->shader, params->reflection, params->error);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50Destroy(void *args) {
  struct sm50_destroy_params *params = args;

  SM50Destroy(params->shader);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_DXSOInitialize(void *args) {
  struct dxso_initialize_params *params = args;

  params->ret = DXSOInitialize(params->bytecode, params->bytecode_size, params->shader);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_DXSODestroy(void *args) {
  struct dxso_destroy_params *params = args;

  DXSODestroy(params->shader);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_DXSOCompile(void *args) {
  struct dxso_compile_params *params = args;

  params->ret = DXSOCompile(params->shader, params->args, params->func_name, params->bitcode);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_DXSOGetCompiledBitcode(void *args) {
  struct dxso_get_compiled_bitcode_params *params = args;

  DXSOGetCompiledBitcode(params->bitcode, params->data_out);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_DXSODestroyBitcode(void *args) {
  struct dxso_destroy_bitcode_params *params = args;

  DXSODestroyBitcode(params->bitcode);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50Compile(void *args) {
  struct sm50_compile_params *params = args;

  params->ret = SM50Compile(params->shader, params->args, params->func_name, params->bitcode, params->error);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50GetCompiledBitcode(void *args) {
  struct sm50_get_compiled_bitcode_params *params = args;

  SM50GetCompiledBitcode(params->bitcode, params->data_out);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50DestroyBitcode(void *args) {
  struct sm50_destroy_bitcode_params *params = args;

  SM50DestroyBitcode(params->bitcode);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50GetErrorMessage(void *args) {
  struct sm50_get_error_message_params *params = args;

  params->ret_size = SM50GetErrorMessage(params->error, params->buffer, params->buffer_size);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50FreeError(void *args) {
  struct sm50_free_error_params *params = args;

  SM50FreeError(params->error);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileTessellationPipelineHull(void *args) {
  struct sm50_compile_tessellation_pipeline_hull_params *params = args;

  params->ret = SM50CompileTessellationPipelineHull(
      params->vertex, params->hull, params->hull_args, params->func_name, params->bitcode, params->error
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileTessellationPipelineDomain(void *args) {
  struct sm50_compile_tessellation_pipeline_domain_params *params = args;

  params->ret = SM50CompileTessellationPipelineDomain(
      params->hull, params->domain, params->domain_args, params->func_name, params->bitcode, params->error
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileGeometryPipelineVertex(void *args) {
  struct sm50_compile_geometry_pipeline_vertex_params *params = args;

  params->ret = SM50CompileGeometryPipelineVertex(
      params->vertex, params->geometry, params->vertex_args, params->func_name, params->bitcode, params->error
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileGeometryPipelineGeometry(void *args) {
  struct sm50_compile_geometry_pipeline_geometry_params *params = args;

  params->ret = SM50CompileGeometryPipelineGeometry(
      params->vertex, params->geometry, params->geometry_args, params->func_name, params->bitcode, params->error
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandEncoder_setLabel(void *args) {
  struct unixcall_generic_obj_obj_noret *params = args;
  [(id<MTLCommandEncoder>)params->handle setLabel:(NSString *)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_setShouldMaximizeConcurrentCompilation(void *args) {
  struct unixcall_generic_obj_uint64_noret *params = args;
  [(id<MTLDevice>)params->handle setShouldMaximizeConcurrentCompilation:(BOOL)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50GetArgumentsInfo(void *args) {
  struct sm50_get_arguments_info_params *params = args;
  SM50GetArgumentsInfo(params->shader, params->constant_buffers, params->arguments);
  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_AirconvUnsupportedInitialize(void *args) {
  struct sm50_initialize_params *params = args;
  if (params->shader)
    *params->shader = 0;
  if (params->error)
    *params->error = 0;
  params->ret = -1;

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_AirconvUnsupportedDestroy(void *args) {
  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_AirconvUnsupportedCompile(void *args) {
  struct sm50_compile_params *params = args;
  if (params->bitcode)
    *params->bitcode = 0;
  if (params->error)
    *params->error = 0;
  params->ret = -1;

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_AirconvUnsupportedGetArgumentsInfo(void *args) {
  return STATUS_SUCCESS;
}

static inline void *
UInt32ToPtr(uint32_t v) {
  return (void *)(uint64_t)v;
}

#ifndef DXMT_NATIVE

static NTSTATUS
thunk32_SM50Initialize(void *args) {
  struct sm50_initialize_params32 *params = args;

  params->ret = SM50Initialize(
      UInt32ToPtr(params->bytecode), params->bytecode_size, UInt32ToPtr(params->shader),
      UInt32ToPtr(params->reflection), UInt32ToPtr(params->error)
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_DXSOInitialize(void *args) {
  struct dxso_initialize_params32 *params = args;

  params->ret = DXSOInitialize(
      UInt32ToPtr(params->bytecode), params->bytecode_size, UInt32ToPtr(params->shader)
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_DXSOGetCompiledBitcode(void *args) {
  struct dxso_get_compiled_bitcode_params32 *params = args;

  DXSOGetCompiledBitcode(params->bitcode, UInt32ToPtr(params->data_out));

  return STATUS_SUCCESS;
}

struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t num_output_slots;
  uint32_t num_elements;
  uint32_t strides[4];
  uint32_t elements;
};

struct SM50_SHADER_COMMON_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_SHADER_METAL_VERSION metal_version;
  enum SM50_SHADER_FLAG flag;
};

struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
};

struct SM50_SHADER_IA_INPUT_LAYOUT_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_INDEX_BUFFER_FORMAT index_buffer_format;
  uint32_t slot_mask;
  uint32_t num_elements;
  uint32_t elements;
};

struct SM50_SHADER_PSO_PIXEL_SHADER_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t sample_mask;
  bool dual_source_blending;
  bool disable_depth_output;
  uint32_t unorm_output_reg_mask;
  uint64_t demote_msaa_srv_mask_lo;
  uint64_t demote_msaa_srv_mask_hi;
};

_Static_assert(sizeof(struct SM50_SHADER_PSO_PIXEL_SHADER_DATA32) == 40, "SM50 pixel PSO args32 size mismatch");
_Static_assert(offsetof(struct SM50_SHADER_PSO_PIXEL_SHADER_DATA32, demote_msaa_srv_mask_lo) == 24,
               "SM50 pixel PSO args32 demote mask lo offset mismatch");
_Static_assert(offsetof(struct SM50_SHADER_PSO_PIXEL_SHADER_DATA32, demote_msaa_srv_mask_hi) == 32,
               "SM50 pixel PSO args32 demote mask hi offset mismatch");

struct SM50_SHADER_GS_PASS_THROUGH_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  union {
    struct MTL_GEOMETRY_SHADER_PASS_THROUGH Data;
    uint32_t DataEncoded;
  };
  bool RasterizationDisabled;
};

struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  bool strip_topology;
};

struct SM50_SHADER_PSO_TESSELLATOR_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t max_potential_tess_factor;
};

void
sm50_compilation_argument32_convert(
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *first_arg, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32
) {
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *last_arg = first_arg;

  first_arg->type = SM50_SHADER_ARGUMENT_TYPE_MAX;
  first_arg->next = NULL;

  while (args32) {
    switch (args32->type) {
    case SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT: {
      struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA32 *src = (void *)args32;
      struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA *data =
          malloc(sizeof(struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->num_output_slots = src->num_output_slots;
      data->num_elements = src->num_elements;
      data->strides[0] = src->strides[0];
      data->strides[1] = src->strides[1];
      data->strides[2] = src->strides[2];
      data->strides[3] = src->strides[3];
      data->elements = UInt32ToPtr(src->elements);
      break;
    }
    case SM50_SHADER_COMMON: {
      struct SM50_SHADER_COMMON_DATA32 *src = (void *)args32;
      struct SM50_SHADER_COMMON_DATA *data = malloc(sizeof(struct SM50_SHADER_COMMON_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->metal_version = src->metal_version;
      data->flags = src->flag;
      break;
    }
    case SM50_SHADER_PSO_PIXEL_SHADER: {
      struct SM50_SHADER_PSO_PIXEL_SHADER_DATA32 *src = (void *)args32;
      struct SM50_SHADER_PSO_PIXEL_SHADER_DATA *data = malloc(sizeof(struct SM50_SHADER_PSO_PIXEL_SHADER_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->unorm_output_reg_mask = src->unorm_output_reg_mask;
      data->disable_depth_output = src->disable_depth_output;
      data->sample_mask = src->sample_mask;
      data->dual_source_blending = src->dual_source_blending;
      data->demote_msaa_srv_mask_lo = src->demote_msaa_srv_mask_lo;
      data->demote_msaa_srv_mask_hi = src->demote_msaa_srv_mask_hi;
      break;
    }
    case SM50_SHADER_IA_INPUT_LAYOUT: {
      struct SM50_SHADER_IA_INPUT_LAYOUT_DATA32 *src = (void *)args32;
      struct SM50_SHADER_IA_INPUT_LAYOUT_DATA *data = malloc(sizeof(struct SM50_SHADER_IA_INPUT_LAYOUT_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->slot_mask = src->slot_mask;
      data->index_buffer_format = src->index_buffer_format;
      data->num_elements = src->num_elements;
      data->elements = UInt32ToPtr(src->elements);
      break;
    }
    case SM50_SHADER_GS_PASS_THROUGH: {
      struct SM50_SHADER_GS_PASS_THROUGH_DATA32 *src = (void *)args32;
      struct SM50_SHADER_GS_PASS_THROUGH_DATA *data = malloc(sizeof(struct SM50_SHADER_GS_PASS_THROUGH_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->Data = src->Data;
      data->RasterizationDisabled = src->RasterizationDisabled;
      break;
    }
    case SM50_SHADER_PSO_GEOMETRY_SHADER: {
      struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA32 *src = (void *)args32;
      struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA *data = malloc(sizeof(struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->strip_topology = src->strip_topology;
      break;
    }
    case SM50_SHADER_PSO_TESSELLATOR: {
      struct SM50_SHADER_PSO_TESSELLATOR_DATA32 *src = (void *)args32;
      struct SM50_SHADER_PSO_TESSELLATOR_DATA *data = malloc(sizeof(struct SM50_SHADER_PSO_TESSELLATOR_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->max_potential_tess_factor = src->max_potential_tess_factor;
      break;
    }
    case SM50_SHADER_ARGUMENT_TYPE_MAX:
      break;
    }
    args32 = UInt32ToPtr(args32->next);
  }
}

void
sm50_compilation_argument32_free(struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *first_arg) {
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *arg = first_arg->next;

  while (arg) {
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *next = arg->next;
    free(arg);
    arg = next;
  }
}

/* DXSO compilation argument chain: same shape as SM50's 32-bit
   chain conversion (sm50_compilation_argument32_convert). DXSO has
   its own enum + struct family so the d3d9 caller picks the right
   types at compile time, but the wire-form is byte-identical to
   SM50's so we reuse the same UInt32ToPtr unpack pattern. The IA
   layout's `elements` pointer points at app-side memory: already
   in the wow64 32-bit address space: so it round-trips through
   UInt32ToPtr without further translation. */
struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA32 {
  uint32_t next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
};

struct DXSO_SHADER_IA_INPUT_LAYOUT_DATA32 {
  uint32_t next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum DXSO_INDEX_BUFFER_FORMAT index_buffer_format;
  uint32_t slot_mask;
  uint32_t num_elements;
  uint32_t elements;
};

struct DXSO_SHADER_PSO_PIXEL_SHADER_DATA32 {
  uint32_t next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t alpha_test_func;
  uint32_t alpha_test_ref;
  uint32_t dual_source_blending;
};

struct DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA32 {
  uint32_t next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint8_t kinds[16];
};

struct DXSO_SHADER_PS_POINT_SPRITE_DATA32 {
  uint32_t next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
};

struct DXSO_SHADER_PS_FOG_DATA32 {
  uint32_t next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t mode;
};

struct DXSO_SHADER_VS_POINT_SIZE_DATA32 {
  uint32_t next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
  float value;
};

struct DXSO_SHADER_PS_BUMP_ENV_DATA32 {
  uint32_t next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
  float mat[8][4];
  float lscale[8];
  float loffset[8];
};


void
dxso_compilation_argument32_convert(
    struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA *first_arg, struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA32 *args32
) {
  struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA *last_arg = first_arg;

  first_arg->type = DXSO_SHADER_ARGUMENT_TYPE_MAX;
  first_arg->next = NULL;

  while (args32) {
    switch (args32->type) {
    case DXSO_SHADER_IA_INPUT_LAYOUT: {
      struct DXSO_SHADER_IA_INPUT_LAYOUT_DATA32 *src = (void *)args32;
      struct DXSO_SHADER_IA_INPUT_LAYOUT_DATA *data = malloc(sizeof(struct DXSO_SHADER_IA_INPUT_LAYOUT_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->index_buffer_format = src->index_buffer_format;
      data->slot_mask = src->slot_mask;
      data->num_elements = src->num_elements;
      data->elements = UInt32ToPtr(src->elements);
      break;
    }
    case DXSO_SHADER_PSO_PIXEL_SHADER: {
      struct DXSO_SHADER_PSO_PIXEL_SHADER_DATA32 *src = (void *)args32;
      struct DXSO_SHADER_PSO_PIXEL_SHADER_DATA *data = malloc(sizeof(struct DXSO_SHADER_PSO_PIXEL_SHADER_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->alpha_test_func = src->alpha_test_func;
      data->alpha_test_ref = src->alpha_test_ref;
      data->dual_source_blending = src->dual_source_blending;
      break;
    }
    case DXSO_SHADER_PS_SAMPLER_LAYOUT: {
      struct DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA32 *src = (void *)args32;
      struct DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA *data = malloc(sizeof(struct DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      memcpy(data->kinds, src->kinds, sizeof(data->kinds));
      break;
    }
    case DXSO_SHADER_PS_POINT_SPRITE: {
      struct DXSO_SHADER_PS_POINT_SPRITE_DATA *data = malloc(sizeof(struct DXSO_SHADER_PS_POINT_SPRITE_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = DXSO_SHADER_PS_POINT_SPRITE;
      break;
    }
    case DXSO_SHADER_VS_POINT_SIZE: {
      struct DXSO_SHADER_VS_POINT_SIZE_DATA32 *src = (void *)args32;
      struct DXSO_SHADER_VS_POINT_SIZE_DATA *data = malloc(sizeof(struct DXSO_SHADER_VS_POINT_SIZE_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = DXSO_SHADER_VS_POINT_SIZE;
      data->value = src->value;
      break;
    }
    case DXSO_SHADER_PS_BUMP_ENV: {
      struct DXSO_SHADER_PS_BUMP_ENV_DATA32 *src = (void *)args32;
      struct DXSO_SHADER_PS_BUMP_ENV_DATA *data = malloc(sizeof(struct DXSO_SHADER_PS_BUMP_ENV_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = DXSO_SHADER_PS_BUMP_ENV;
      memcpy(data->mat, src->mat, sizeof(data->mat));
      memcpy(data->lscale, src->lscale, sizeof(data->lscale));
      memcpy(data->loffset, src->loffset, sizeof(data->loffset));
      break;
    }
    case DXSO_SHADER_PS_FOG: {
      struct DXSO_SHADER_PS_FOG_DATA32 *src = (void *)args32;
      struct DXSO_SHADER_PS_FOG_DATA *data = malloc(sizeof(struct DXSO_SHADER_PS_FOG_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = DXSO_SHADER_PS_FOG;
      data->mode = src->mode;
      break;
    }
    case DXSO_SHADER_ARGUMENT_TYPE_MAX:
      break;
    }
    args32 = UInt32ToPtr(args32->next);
  }
}

void
dxso_compilation_argument32_free(struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA *first_arg) {
  struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA *arg = first_arg->next;

  while (arg) {
    struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA *next = arg->next;
    free(arg);
    arg = next;
  }
}

static NTSTATUS
thunk32_DXSOCompile(void *args) {
  struct dxso_compile_params32 *params = args;
  struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->args);
  dxso_compilation_argument32_convert(&first_arg, args32);

  params->ret = DXSOCompile(
      params->shader, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode)
  );

  dxso_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50Compile(void *args) {
  struct sm50_compile_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50Compile(
      params->shader, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50GetCompiledBitcode(void *args) {
  struct sm50_get_compiled_bitcode_params32 *params = args;

  SM50GetCompiledBitcode(params->bitcode, UInt32ToPtr(params->data_out));

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50GetErrorMessage(void *args) {
  struct sm50_get_error_message_params32 *params = args;

  params->ret_size = SM50GetErrorMessage(params->error, UInt32ToPtr(params->buffer), params->buffer_size);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileTessellationPipelineHull(void *args) {
  struct sm50_compile_tessellation_pipeline_hull_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->hull_args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50CompileTessellationPipelineHull(
      params->vertex, params->hull, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileTessellationPipelineDomain(void *args) {
  struct sm50_compile_tessellation_pipeline_domain_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->domain_args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50CompileTessellationPipelineDomain(
      params->hull, params->domain, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileGeometryPipelineVertex(void *args) {
  struct sm50_compile_geometry_pipeline_vertex_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->vertex_args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50CompileGeometryPipelineVertex(
      params->vertex, params->geometry, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileGeometryPipelineGeometry(void *args) {
  struct sm50_compile_geometry_pipeline_geometry_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->geometry_args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50CompileGeometryPipelineGeometry(
      params->vertex, params->geometry, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50GetArgumentsInfo(void *args) {
  struct sm50_get_arguments_info_params32 *params = args;

  SM50GetArgumentsInfo(params->shader, UInt32ToPtr(params->constant_buffers), UInt32ToPtr(params->arguments));

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_AirconvUnsupportedInitialize(void *args) {
  struct sm50_initialize_params32 *params = args;
  params->ret = -1;

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_AirconvUnsupportedCompile(void *args) {
  struct sm50_compile_params32 *params = args;
  params->ret = -1;

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_AirconvUnsupportedGetArgumentsInfo(void *args) {
  return STATUS_SUCCESS;
}
#endif /* DXMT_NATIVE */

static NTSTATUS
_MTLCommandBuffer_error(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[[(id<MTLCommandBuffer>)params->handle error] retain];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_logs(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[[(id<MTLCommandBuffer>)params->handle logs] retain];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLLogContainer_enumerate(void *obj) {
  struct unixcall_enumerate *params = obj;
  uint64_t count = 0;
  uint64_t read = 0;
  id *buffer = params->buffer.ptr;
  for (id _ in (id<MTLLogContainer>)params->enumerable) {
    if (count >= params->start) {
      if (count < params->start + params->buffer_size) {
        buffer[count - params->start] = _;
        read++;
      } else {
        break;
      }
    }
    count++;
  }
  params->ret_read = read;
  return STATUS_SUCCESS;
}

CFStringRef
GetColorSpaceName(enum WMTColorSpace colorspace) {
  switch (colorspace) {
  case WMTColorSpaceSRGB:
    return kCGColorSpaceSRGB;
  case WMTColorSpaceSRGBLinear:
  case WMTColorSpaceHDR_scRGB:
    return kCGColorSpaceExtendedLinearSRGB;
  case WMTColorSpaceBT2020:
    return kCGColorSpaceITUR_2020_sRGBGamma;
  case WMTColorSpaceHDR_PQ:
    return kCGColorSpaceITUR_2100_PQ;
  default:
    return nil;
  }
}

static NTSTATUS
_CGColorSpace_checkColorSpaceSupported(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = false;
  CFStringRef name = GetColorSpaceName((enum WMTColorSpace)params->handle);
  if (!name)
    return STATUS_SUCCESS;
  CGColorSpaceRef ref = CGColorSpaceCreateWithName(name);
  if (!ref)
    return STATUS_SUCCESS;
  CGColorSpaceRelease(ref);
  params->ret = true;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_setColorSpace(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  enum WMTColorSpace colorspace = params->arg;
  CFStringRef name = GetColorSpaceName(colorspace);
  params->ret = false;
  if (!name)
    return STATUS_SUCCESS;
  CGColorSpaceRef ref = CGColorSpaceCreateWithName(name);
  if (!ref)
    return STATUS_SUCCESS;
  execute_on_main(^{
    layer.colorspace = ref;
    layer.wantsExtendedDynamicRangeContent = WMT_COLORSPACE_IS_HDR(colorspace);
    CGColorSpaceRelease(ref);
  });
  params->ret = true;
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTGetPrimaryDisplayId(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = CGMainDisplayID();
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTGetSecondaryDisplayId(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = kCGNullDirectDisplay;

  uint32_t count = 0;
  CGGetOnlineDisplayList(0, NULL, &count);

  if (count == 0)
    return STATUS_SUCCESS;

  CGDirectDisplayID main_display = CGMainDisplayID();
  CGDirectDisplayID displays[count];
  CGGetOnlineDisplayList(count, displays, &count);

  for (uint32_t i = 0; i < count; i++) {
    CGDirectDisplayID id = displays[i];
    if (id == main_display)
      continue;
    if (CGDisplayMirrorsDisplay(id) != kCGNullDirectDisplay)
      continue;
    params->ret = id;
    break;
  }

  return STATUS_SUCCESS;
}

typedef struct icc_XYZ_t {
  uint32_t sig;      // 0x205a5958
  uint32_t reserved; // 0
  int32_t x;
  int32_t y;
  int32_t z;
} icc_XYZ_t;

bool
GetChromaticity_xy(ColorSyncProfileRef profile, CFStringRef tag, float *out_x, float *out_y) {
  CFDataRef tag_data = ColorSyncProfileCopyTag(profile, tag);
  if (!tag_data)
    return false;
  if (CFDataGetLength(tag_data) != sizeof(icc_XYZ_t))
    return false;
  icc_XYZ_t *data = (icc_XYZ_t *)CFDataGetBytePtr(tag_data);
  if (data->sig != 0x205a5958)
    return false;
  double X = (int32_t)__builtin_bswap32(data->x) / 65536.0;
  double Y = (int32_t)__builtin_bswap32(data->y) / 65536.0;
  double Z = (int32_t)__builtin_bswap32(data->z) / 65536.0;
  *out_x = X / (X + Y + Z);
  *out_y = Y / (X + Y + Z);
  return true;
}

bool
GetDisplayColorGamut(ColorSyncProfileRef profile, struct WMTDisplayDescription *desc_out) {
  return GetChromaticity_xy(
             profile, kColorSyncSigMediaWhitePointTag, &desc_out->white_points[0], &desc_out->white_points[1]
         ) &&
         GetChromaticity_xy(
             profile, kColorSyncSigRedColorantTag, &desc_out->red_primaries[0], &desc_out->red_primaries[1]
         ) &&
         GetChromaticity_xy(
             profile, kColorSyncSigGreenColorantTag, &desc_out->green_primaries[0], &desc_out->green_primaries[1]
         ) &&
         GetChromaticity_xy(
             profile, kColorSyncSigBlueColorantTag, &desc_out->blue_primaries[0], &desc_out->blue_primaries[1]
         );
}

NSScreen *
GetNSScreenForDisplayID(CGDirectDisplayID display_id) {
  for (NSScreen *screen in [NSScreen screens]) {
    CGDirectDisplayID id = [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];
    if (id == display_id) {
      return screen;
    }
  }
  return nil;
}

static NTSTATUS
_WMTGetDisplayDescription(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  CGDirectDisplayID display_id = params->handle;
  struct WMTDisplayDescription *desc_out = params->arg.ptr;
  ColorSyncProfileRef profile = ColorSyncProfileCreateWithDisplayID(display_id);
  if (!profile || !GetDisplayColorGamut(profile, desc_out))
    GetDisplayColorGamut(ColorSyncProfileCreateWithName(kColorSyncGenericRGBProfile), desc_out);
  NSScreen *screen = GetNSScreenForDisplayID(display_id);
  if (screen) {
    desc_out->maximum_edr_color_component_value = [screen maximumExtendedDynamicRangeColorComponentValue];
    desc_out->maximum_reference_edr_color_component_value =
        [screen maximumReferenceExtendedDynamicRangeColorComponentValue];
    desc_out->maximum_potential_edr_color_component_value =
        [screen maximumPotentialExtendedDynamicRangeColorComponentValue];
  } else {
    desc_out->maximum_edr_color_component_value = 1.0;
    desc_out->maximum_reference_edr_color_component_value = 0.0;
    desc_out->maximum_potential_edr_color_component_value = 1.0;
  }
  return STATUS_SUCCESS;
}

struct DisplaySetting {
  uint64_t version;
  enum WMTColorSpace colorspace;
  struct WMTHDRMetadata hdr_metadata;
};

struct DisplaySetting g_display_settings[2] = {{0, 0, {}}, {0, 0, {}}};

static NTSTATUS
_MetalLayer_getEDRValue(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  struct WMTEDRValue *value = params->arg.ptr;
  value->maximum_edr_color_component_value = 1.0;
  value->maximum_potential_edr_color_component_value = 1.0;

  if (!layer)
    return STATUS_SUCCESS;

  execute_on_main(^{
    if (![layer isKindOfClass:CAMetalLayer.class] ||
        ![layer.delegate isKindOfClass:NSView.class])
      return;

    NSView *view = (NSView *)layer.delegate;
    if (!view.window || !view.window.screen)
      return;

    NSScreen *screen = view.window.screen;
    value->maximum_edr_color_component_value =
        layer.wantsExtendedDynamicRangeContent ? screen.maximumExtendedDynamicRangeColorComponentValue : 1.0;
    value->maximum_potential_edr_color_component_value =
        screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
  });

  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLLibrary_newFunctionWithConstants(void *obj) {
  struct unixcall_mtllibrary_newfunction_with_constants *params = obj;
  id<MTLLibrary> library = (id<MTLLibrary>)params->library;
  NSString *name = [[NSString alloc] initWithCString:(char *)params->name.ptr encoding:NSUTF8StringEncoding];
  NSError *err = NULL;
  MTLFunctionConstantValues *values = [[MTLFunctionConstantValues alloc] init];
  for (uint64_t i = 0; i < params->num_constants; i++) {
    const struct WMTFunctionConstant *constant = &params->constants[i];
    if (constant->type == WMTDataTypeBool) {
      bool value = (params->bool_values & (UINT64_C(1) << i)) != 0;
      [values setConstantValue:&value type:(MTLDataType)constant->type atIndex:constant->index];
    } else {
      [values setConstantValue:constant->data.ptr type:(MTLDataType)constant->type atIndex:constant->index];
    }
  }

  params->ret = (obj_handle_t)[library newFunctionWithName:name constantValues:values error:&err];
  params->ret_error = (obj_handle_t)[err retain];   /* +1 across the boundary; PE releases */
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && params->ret && !err) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    dxmt_apitrace_track_function_with_constants_locked(
        params->ret, params->library, name, params->constants, params->num_constants, params->bool_values);
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  [name release];
  [values release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTQueryDisplaySetting(void *obj) {
  struct unixcall_query_display_setting *params = obj;
  CGDirectDisplayID display_id = params->display_id;
  struct WMTHDRMetadata *value = params->hdr_metadata.ptr;
  params->ret = false;
  struct DisplaySetting *setting = &g_display_settings[display_id == CGMainDisplayID()];
  if (setting->version) {
    *value = setting->hdr_metadata;
    params->colorspace = setting->colorspace;
    params->ret = true;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTUpdateDisplaySetting(void *obj) {
  struct unixcall_update_display_setting *params = obj;
  CGDirectDisplayID display_id = params->display_id;
  const struct WMTHDRMetadata *value = params->hdr_metadata.ptr;
  struct DisplaySetting *setting = &g_display_settings[display_id == CGMainDisplayID()];
  if (value) {
    setting->hdr_metadata = *value;
    setting->colorspace = params->colorspace;
    setting->version++;
  } else {
    setting->version = 0;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTQueryDisplaySettingForLayer(void *obj) {
  struct unixcall_query_display_setting_for_layer *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->layer;
  struct WMTHDRMetadata *hdr_metadata_out = params->hdr_metadata.ptr;

  params->version = 0;
  params->edr_value.maximum_edr_color_component_value = 1.0;
  params->edr_value.maximum_potential_edr_color_component_value = 1.0;
  if (!layer)
    return STATUS_SUCCESS;

  execute_on_main(^{
    if (![layer isKindOfClass:CAMetalLayer.class] ||
        ![layer.delegate isKindOfClass:NSView.class])
      return;

    NSView *view = (NSView *)layer.delegate;
    if (!view.window || !view.window.screen)
      return;

    NSScreen *screen = view.window.screen;
    CGDirectDisplayID id = [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];

    struct DisplaySetting *setting = &g_display_settings[id == CGMainDisplayID()];
    *hdr_metadata_out = setting->hdr_metadata;
    params->version = setting->version;
    params->colorspace = setting->colorspace;
    params->edr_value.maximum_edr_color_component_value =
        layer.wantsExtendedDynamicRangeContent ? screen.maximumExtendedDynamicRangeColorComponentValue : 1.0;
    params->edr_value.maximum_potential_edr_color_component_value =
        screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
  });
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeWaitForEvent(void *obj) {
  struct unixcall_generic_obj_obj_uint64_noret *params = obj;
  [(id<MTLCommandBuffer>)params->handle encodeWaitForEvent:(id<MTLSharedEvent>)params->arg0 value:params->arg1];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLSharedEvent_signalValue(void *obj) {
  struct unixcall_generic_obj_uint64_noret *params = obj;
  [(id<MTLSharedEvent>)params->handle setSignaledValue:params->arg];
  return STATUS_SUCCESS;
}

#ifndef DXMT_NATIVE

typedef struct {
  _Atomic(CFRunLoopRef) runloop_ref;
  MTLSharedEventListener *shared_listener;
} *shared_event_listener_t;

extern NTSTATUS NtSetEvent(void *handle, void *prev_state);

static NTSTATUS
_MTLSharedEvent_setWin32EventAtValue(void *obj) {
  struct unixcall_mtlsharedevent_setevent *params = obj;
  void *nt_event_handle = (shared_event_listener_t)params->event_handle;
  shared_event_listener_t q = (shared_event_listener_t)params->shared_event_listener;
  [(id<MTLSharedEvent>)params->shared_event
      notifyListener:q->shared_listener
             atValue:params->value
               block:^(id<MTLSharedEvent> _e, uint64_t _v) {
                 // NOTE: must ensure no more notification comes after listener been destroyed.
                 while (!atomic_load_explicit(&q->runloop_ref, memory_order_acquire)) {
#if defined(__x86_64__)
                   _mm_pause();
#elif defined(__aarch64__)
          __asm__ __volatile__("yield");
#endif
                 }
                 CFRunLoopPerformBlock(q->runloop_ref, kCFRunLoopCommonModes, ^{
                   NtSetEvent(nt_event_handle, NULL);
                 });
                 CFRunLoopWakeUp(q->runloop_ref);
               }];
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_start(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  shared_event_listener_t q = (shared_event_listener_t)params->handle;
  CFRunLoopRef uninited = NULL;
  if (q && atomic_compare_exchange_strong(&q->runloop_ref, &uninited, CFRunLoopGetCurrent())) {
    /* Add a dummy source so the runloop stays running */
    CFRunLoopSourceContext source_context = {0};
    CFRunLoopSourceRef source = CFRunLoopSourceCreate(NULL, 0, &source_context);
    CFRunLoopAddSource(q->runloop_ref, source, kCFRunLoopCommonModes);
    CFRunLoopRun();
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_create(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  shared_event_listener_t q = malloc(sizeof(*q));
  if (q) {
    q->runloop_ref = NULL;
    q->shared_listener = [[MTLSharedEventListener alloc] init];
  }
  params->ret = (obj_handle_t)q;
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_destroy(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  shared_event_listener_t q = (shared_event_listener_t)params->handle;
  if (q && q->runloop_ref) {
    CFRunLoopStop(q->runloop_ref);
    q->runloop_ref = NULL;
    [q->shared_listener release];
    q->shared_listener = nil;
    free(q);
  }
  return STATUS_SUCCESS;
}

#else
static NTSTATUS
_MTLSharedEvent_setWin32EventAtValue(void *obj) {
  // nop
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_start(void *obj) {
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_create(void *obj) {
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_destroy(void *obj) {
  return STATUS_SUCCESS;
}

#endif

static NTSTATUS
_MTLDevice_newFence(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newFence];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newEvent(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newEvent];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBuffer_updateContents(void *obj) {
  struct unixcall_mtlbuffer_updatecontents *params = obj;
  memcpy((void *)((char *)[(id<MTLBuffer>)params->buffer contents] + params->offset), params->data.ptr, params->length);
  if ([(id<MTLBuffer>)params->buffer storageMode] == MTLStorageModeManaged)
    [(id<MTLBuffer>)params->buffer didModifyRange:NSMakeRange(params->offset, params->length)];
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && [(id<MTLBuffer>)params->buffer storageMode] != MTLStorageModePrivate) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      apitrace_metal_update_buffer_contents(
          dxmt_apitrace_session,
          params->buffer,
          params->offset,
          params->length,
          (uint32_t)[(id<MTLBuffer>)params->buffer storageMode],
          (const uint8_t *)[(id<MTLBuffer>)params->buffer contents] + params->offset,
          params->length);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTGetOSVersion(void *obj) {
  struct unixcall_get_os_version *params = obj;
  NSOperatingSystemVersion version = [NSProcessInfo processInfo].operatingSystemVersion;
  params->ret_major = version.majorVersion;
  params->ret_minor = version.minorVersion;
  params->ret_patch = version.patchVersion;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newBinaryArchive(void *obj) {
  struct unixcall_mtldevice_newbinaryarchive *params = obj;
  NSString *path_str = NULL;
  NSURL *url = NULL;
  MTLBinaryArchiveDescriptor *desc = [[MTLBinaryArchiveDescriptor alloc] init];
  if (params->url.ptr != NULL) {
    path_str = [[NSString alloc] initWithCString:params->url.ptr encoding:NSUTF8StringEncoding];
    url = [[NSURL alloc] initFileURLWithPath:path_str];
    desc.url = url;
  }
  NSError *err = NULL;
  params->ret_archive = (obj_handle_t)[(id<MTLDevice>)params->device newBinaryArchiveWithDescriptor:desc error:&err];
  params->ret_error = (obj_handle_t)[err retain];   /* +1 across the boundary; PE releases */
  [desc release];
  if (url)
    [url release];
  if (path_str)
    [path_str release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBinaryArchive_serialize(void *obj) {
  struct unixcall_mtlbinaryarchive_serialize *params = obj;
  NSString *path_str = [[NSString alloc] initWithCString:params->url.ptr encoding:NSUTF8StringEncoding];
  NSURL *url = [[NSURL alloc] initFileURLWithPath:path_str];
  NSError *err = NULL;
  [(id<MTLBinaryArchive>)params->archive serializeToURL:url error:&err];
  params->ret_error = (obj_handle_t)[err retain];   /* +1 across the boundary; PE releases */
  [url release];
  [path_str release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_DispatchData_alloc_init(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  // dispatch_data_create with destructor=NULL is documented as
  // "DISPATCH_DATA_DESTRUCTOR_DEFAULT" and copies the bytes, but the
  // copy semantics across libdispatch versions and wine's loader
  // surface have given us flaky AGX XPC failures at PSO link time on
  // the d3d9 path. Explicitly malloc + memcpy + DISPATCH_DATA_DESTRUCTOR_FREE
  // makes ownership unambiguous: the dispatch_data owns the buffer
  // outright and frees via free(3) on last release.
  void *copy = malloc(params->arg);
  memcpy(copy, (void *)params->handle, params->arg);
  params->ret = (obj_handle_t)dispatch_data_create(
      copy, params->arg, NULL, DISPATCH_DATA_DESTRUCTOR_FREE);
  return STATUS_SUCCESS;
}

@interface MTLSharedTextureHandle ()

- (MTLSharedTextureHandle *)initWithMachPort:(mach_port_t)port;
- (mach_port_t)createMachPort;

@end

static NTSTATUS
_MTLDevice_newSharedTexture(void *obj) {
  struct unixcall_mtldevice_newtexture *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTTextureInfo *info = params->info.ptr;

  if (info->mach_port) {
    MTLSharedTextureHandle *handle = [[MTLSharedTextureHandle alloc] initWithMachPort:info->mach_port];
    id<MTLTexture> ret = [device newSharedTextureWithHandle:handle];
    extract_texture_descriptor(ret, info);
    params->ret = (obj_handle_t)ret;
    info->gpu_resource_id = [ret gpuResourceID]._impl;
    [handle release];
  } else {
    MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
    fill_texture_descriptor(desc, info);
    id<MTLTexture> ret = [device newSharedTextureWithDescriptor:desc];
    MTLSharedTextureHandle *handle = [ret newSharedTextureHandle];
    params->ret = (obj_handle_t)ret;
    info->gpu_resource_id = [ret gpuResourceID]._impl;
    info->mach_port = [handle createMachPort]; // implicitly add ref to underlying IOSurface
    [handle release];
    [desc release];
  }

  return STATUS_SUCCESS;
}

/* Private API to register a mach port with the bootstrap server */
extern kern_return_t bootstrap_register2(mach_port_t bp, name_t service_name, mach_port_t sp, int flags);

static NTSTATUS
_WMTBootstrapRegister(void *obj) {
  struct unixcall_bootstrap *params = obj;
  mach_port_t rp = params->mach_port;
  mach_port_t bp;

  if (task_get_bootstrap_port(mach_task_self(), &bp) != KERN_SUCCESS)
    return STATUS_UNSUCCESSFUL;
  NTSTATUS ret = bootstrap_register2(bp, params->name, rp, 0) != KERN_SUCCESS ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
  mach_port_deallocate(mach_task_self(), bp);
  return ret;
}

static NTSTATUS
_WMTBootstrapLookUp(void *obj) {
  struct unixcall_bootstrap *params = obj;
  mach_port_t rp = 0;
  mach_port_t bp;

  if (task_get_bootstrap_port(mach_task_self(), &bp) != KERN_SUCCESS)
    return STATUS_UNSUCCESSFUL;
  NTSTATUS ret = bootstrap_look_up(bp, params->name, &rp) != KERN_SUCCESS ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
  mach_port_deallocate(mach_task_self(), bp);
  params->mach_port = rp;
  return ret;
}

@protocol MTLDeviceSPI <MTLDevice>

- (id<MTLSharedEvent>)newSharedEventWithMachPort:(mach_port_t)machPort;

@end

@interface MTLSharedEventHandle ()

- (mach_port_t)eventPort;

@end

static NTSTATUS
_MTLSharedEvent_createMachPort(void *obj) {
  struct unixcall_mtlsharedevent_createmachport *params = obj;
  id<MTLSharedEvent> event = (id<MTLSharedEvent>)params->event;
  MTLSharedEventHandle *handle = [event newSharedEventHandle];
  mach_port_t port = [handle eventPort];
  
  // The eventPort method returns a send right that's owned by the handle.
  // We need to add our own send right since we're keeping the port but releasing the handle.
  // This increments the send right count so the port remains valid.
  mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1);
  
  params->ret_mach_port = port;
  [handle release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newSharedEventWithMachPort(void *obj) {
  struct unixcall_mtldevice_newsharedeventwithmachport *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  id<MTLDeviceSPI> deviceSPI = (id<MTLDeviceSPI>)device;
  params->ret_event = (obj_handle_t)[deviceSPI newSharedEventWithMachPort:params->mach_port];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_registryID(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle registryID];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLSharedEvent_waitUntilSignaledValue(void *obj) {
  struct unixcall_mtlsharedevent_waituntilsignaledvalue *params = obj;
  if (getenv("D9MT_WSILOG"))
    fprintf(stderr, "D9MT-WSI[unix] waitUntilSignaledValue value=%llu tmo=%llu ENTER (cur=%llu)\n",
            (unsigned long long)params->value, (unsigned long long)params->timeout_ms,
            (unsigned long long)[(id<MTLSharedEvent>)params->event signaledValue]);
  bool timeout = [(id<MTLSharedEvent>)params->event waitUntilSignaledValue:params->value timeoutMS:params->timeout_ms];
  params->ret_timeout = timeout;
  if (getenv("D9MT_WSILOG"))
    fprintf(stderr, "D9MT-WSI[unix] waitUntilSignaledValue value=%llu EXIT signaled=%d\n",
            (unsigned long long)params->value, (int)timeout);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCounterSampleBuffer_newTimestampBuffer(void *obj) {
  struct unixcall_mtlcountersamplebuffer_newtimestampbuffer *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;

  MTLCounterSampleBufferDescriptor *desc = [[MTLCounterSampleBufferDescriptor alloc] init];
  NSArray<id<MTLCounterSet>> *counter_sets = [device counterSets];
  id<MTLCounterSet> timestamp_counter_set = nil;
  for (id<MTLCounterSet> counterSet in counter_sets) {
    if ([counterSet.name isEqualToString:MTLCommonCounterSetTimestamp]) {
      timestamp_counter_set = counterSet;
      break;
    }
  }
  desc.counterSet = timestamp_counter_set;
  desc.sampleCount = params->sample_count;
  desc.storageMode = params->shared ? MTLStorageModeShared : MTLStorageModePrivate;

  NSError *error = nil;
  params->ret = (obj_handle_t)[device newCounterSampleBufferWithDescriptor:desc error:&error];

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCounterSampleBuffer_resolveCounterRange(void *obj) {
  struct unixcall_mtlcountersamplebuffer_resolvecounterrange *params = obj;
  id<MTLCounterSampleBuffer> sample_buffer = (id<MTLCounterSampleBuffer>)params->sample_buffer;

  NSData *data = [sample_buffer resolveCounterRange:NSMakeRange(params->start, params->len)];
  if (data && params->data_out.ptr) {
    [data getBytes:params->data_out.ptr length:params->data_length];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_blitCommandEncoderWithSampleBuffers(void *obj) {
  struct unixcall_mtlcommandbuffer_blitcommandencoderwithsamplebuffers *params = obj;
  id<MTLCommandBuffer> cmdbuf = (id<MTLCommandBuffer>)params->cmdbuf;
  struct WMTSampleBufferAttachmentInfo *attachments = params->attachments.ptr;

  MTLBlitPassDescriptor *blit_desc = [[MTLBlitPassDescriptor alloc] init];
  for (uint64_t i = 0; i < params->num_attachments; i++) {
    MTLBlitPassSampleBufferAttachmentDescriptor *desc = blit_desc.sampleBufferAttachments[i];
    desc.sampleBuffer = (id<MTLCounterSampleBuffer>)attachments[i].sample_buffer;
    desc.startOfEncoderSampleIndex = attachments[i].start_of_encoder_sample_index;
    desc.endOfEncoderSampleIndex = attachments[i].end_of_encoder_sample_index;
  }

  params->ret = (obj_handle_t)[[cmdbuf blitCommandEncoderWithDescriptor:blit_desc] retain];

  [blit_desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_property(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  id<MTLCommandBuffer> cmdbuf = (id<MTLCommandBuffer>)params->handle;
  double ns = 1000000000;
  switch (params->arg) {
  case WMTCommandBufferPropertyKernelStartTime:
    params->ret = (uint64_t)([cmdbuf kernelStartTime] * ns);
    break;
  case WMTCommandBufferPropertyKernelEndTime:
    params->ret = (uint64_t)([cmdbuf kernelEndTime] * ns);
    break;
  case WMTCommandBufferPropertyGPUStartTime:
    params->ret = (uint64_t)([cmdbuf GPUStartTime] * ns);
    break;
  case WMTCommandBufferPropertyGPUEndTime:
    params->ret = (uint64_t)([cmdbuf GPUEndTime] * ns);
    break;
  default:
    params->ret = 0;
    break;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newTileRenderPipelineState(void *obj) {
  struct unixcall_mtldevice_newrenderpso *params = obj;
  const struct WMTTileRenderPipelineInfo *info = params->info.ptr;
  MTLTileRenderPipelineDescriptor *descriptor = [[MTLTileRenderPipelineDescriptor alloc] init];

  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].pixelFormat = to_metal_pixel_format(info->color_formats[i]);
  }

  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_tile_buffers & (1 << i))
      descriptor.tileBuffers[i].mutability = MTLMutabilityImmutable;
  }

  descriptor.rasterSampleCount = info->raster_sample_count;
  descriptor.threadgroupSizeMatchesTileSize = info->tgsize_matches_tile_size;


  descriptor.tileFunction = (id<MTLFunction>)info->tile_function;

  MTLPipelineOption options = MTLPipelineOptionNone;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
      descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                      count:info->num_binary_archives_for_lookup];
    options = info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  }
#endif
  NSError *err = NULL;
  params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newRenderPipelineStateWithTileDescriptor:descriptor
                                                                                                  options:options
                                                                                               reflection:nil
                                                                                                    error:&err];
  params->ret_error = (obj_handle_t)[err retain];   /* +1 across the boundary; PE releases */
  const BOOL pso_created = params->ret_pso != 0;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (pso_created && info->binary_archive_for_serialization) {
      [(id<MTLBinaryArchive>)info->binary_archive_for_serialization
          addTileRenderPipelineFunctionsWithDescriptor:descriptor
                                                 error:&err];
    }
  }
#endif
#if DXMT_APITRACE_METAL
  if (dxmt_apitrace_runtime_enabled() && pso_created) {
    pthread_mutex_lock(&dxmt_apitrace_lock);
    if (dxmt_apitrace_ensure_session_locked()) {
      NSString *descriptor_json = dxmt_apitrace_tile_render_pipeline_json(info);
      apitrace_metal_register_tile_render_pipeline(
          dxmt_apitrace_session,
          params->ret_pso,
          descriptor_json.UTF8String,
          info->tile_function);
    }
    pthread_mutex_unlock(&dxmt_apitrace_lock);
  }
#endif
  [descriptor release];
  return STATUS_SUCCESS;
}

#if DXMT_APITRACE_METAL
static NTSTATUS
_WMTApitraceSessionEnsureOpen(void *obj) {
  (void)obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  (void)dxmt_apitrace_ensure_session_locked();
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTApitraceSessionClose(void *obj) {
  (void)obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  dxmt_apitrace_close_session_locked();
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTApitraceSessionFlush(void *obj) {
  (void)obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  if (dxmt_apitrace_session)
    apitrace_metal_session_flush(dxmt_apitrace_session);
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTApitraceSessionSealCheckpoint(void *obj) {
  (void)obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  if (dxmt_apitrace_session)
    apitrace_metal_session_seal_checkpoint(dxmt_apitrace_session);
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTApitraceSetCurrentD3DSequence(void *obj) {
  struct unixcall_generic_obj_uint64_noret *params = obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  dxmt_apitrace_current_d3d_sequence = params->arg;
  if (dxmt_apitrace_ensure_session_locked()) {
    apitrace_metal_set_current_d3d_sequence(dxmt_apitrace_session, params->arg);
  }
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTApitraceCommandBufferBegin(void *obj) {
  struct unixcall_apitrace_command_buffer_begin *params = obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  if (dxmt_apitrace_ensure_session_locked()) {
    uint64_t begin_sequence =
        apitrace_metal_command_buffer_begin(dxmt_apitrace_session, params->command_buffer, params->frame_id, "");
    DXMTApitraceCommandBufferState *state = [[DXMTApitraceCommandBufferState alloc] init];
    state.beginSequence = begin_sequence;
    [dxmt_apitrace_command_buffers setObject:state forKey:dxmt_apitrace_key(params->command_buffer)];
    [state release];
  }
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTApitraceCommandBufferCommit(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  DXMTApitraceCommandBufferState *state = [dxmt_apitrace_command_buffers objectForKey:dxmt_apitrace_key(params->handle)];
  if (dxmt_apitrace_session && state) {
    dxmt_apitrace_flush_command_buffer_blit_batch(dxmt_apitrace_session, params->handle);
    apitrace_metal_command_buffer_commit(dxmt_apitrace_session, params->handle);
    [dxmt_apitrace_command_buffers removeObjectForKey:dxmt_apitrace_key(params->handle)];
    apitrace_metal_set_current_d3d_sequence(dxmt_apitrace_session, 0);
    dxmt_apitrace_current_d3d_sequence = 0;
  }
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTApitracePresentDrawable(void *obj) {
  struct unixcall_apitrace_present *params = obj;
  if (!dxmt_apitrace_runtime_enabled())
    return STATUS_SUCCESS;
  pthread_mutex_lock(&dxmt_apitrace_lock);
  if (dxmt_apitrace_ensure_session_locked()) {
    id<MTLDrawable> drawable = (id<MTLDrawable>)params->drawable;
    id<MTLTexture> texture = [drawable respondsToSelector:@selector(texture)] ? [(id<MTLDrawable>)drawable texture] : nil;
    uint32_t width = texture ? (uint32_t)texture.width : 0;
    uint32_t height = texture ? (uint32_t)texture.height : 0;
    apitrace_metal_present_drawable(
        dxmt_apitrace_session,
        params->command_buffer,
        params->drawable,
        (uint64_t)texture,
        params->frame_index,
        width,
        height,
        params->sync_interval,
        params->flags);
  }
  pthread_mutex_unlock(&dxmt_apitrace_lock);
  return STATUS_SUCCESS;
}
#else
static NTSTATUS _WMTApitraceSessionEnsureOpen(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMTApitraceSessionClose(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMTApitraceSessionFlush(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMTApitraceSessionSealCheckpoint(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMTApitraceSetCurrentD3DSequence(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMTApitraceCommandBufferBegin(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMTApitraceCommandBufferCommit(void *obj) { (void)obj; return STATUS_SUCCESS; }
static NTSTATUS _WMTApitracePresentDrawable(void *obj) { (void)obj; return STATUS_SUCCESS; }
#endif

/*
 * Definition from cache.c
 */

NTSTATUS _CacheReader_alloc_init(void *obj);
NTSTATUS _CacheReader_get(void *obj);
NTSTATUS _CacheWriter_alloc_init(void *obj);
NTSTATUS _CacheWriter_set(void *obj);
NTSTATUS _WMTSetMetalShaderCachePath(void *obj);

/* d9mt addition: AIR shader-compile thunks (D3D9 SM3 -> AIR), backed by the
 * d9mtair compile core (AirBuilder + LLVM), linked into this dylib. Params are
 * all 8-byte/arch-invariant, so the 64-bit thunks are wow64 byte-compatible
 * (see DXSODestroy note pattern). */
struct d9mt_air_version_params;
struct d9mt_air_compile_params;
struct d9mt_air_free_params;
extern int d9mtair_version(struct d9mt_air_version_params *p);
extern int d9mtair_compile(struct d9mt_air_compile_params *p);
extern int d9mtair_free(struct d9mt_air_free_params *p);

static NTSTATUS
thunk_D9MTAirVersion(void *args) {
  return d9mtair_version(args) == 0 ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static NTSTATUS
thunk_D9MTAirCompile(void *args) {
  /* status travels inside the param struct; the call itself succeeds */
  d9mtair_compile(args);
  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_D9MTAirFree(void *args) {
  d9mtair_free(args);
  return STATUS_SUCCESS;
}

/* ===================================================================== *
 * d9mt addition: the full Metal 4 surface (dispatch codes 157..200).
 * Obj-C (MRC, like the rest of this file). Handles are id<...> casts.
 * ===================================================================== */

/* --- device factories (157..162) ------------------------------------- */
static NTSTATUS
_MTLDevice_newMTL4CommandQueue(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newMTL4CommandQueue];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newCommandAllocator(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newCommandAllocator];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newMTL4CommandBuffer(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newCommandBuffer];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newArgumentTable(void *obj) {
  struct unixcall_mtl4_new_from_desc *params = obj;
  const struct WMTArgTableInfo *info = params->info.ptr;
  MTL4ArgumentTableDescriptor *desc = [[MTL4ArgumentTableDescriptor alloc] init];
  desc.maxBufferBindCount = info->max_buffers;
  desc.maxTextureBindCount = info->max_textures;
  desc.maxSamplerStateBindCount = info->max_samplers;
  desc.initializeBindings = info->initialize_bindings ? YES : NO;
  desc.supportAttributeStrides = info->support_attribute_strides ? YES : NO;
  NSError *err = NULL;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->device newArgumentTableWithDescriptor:desc error:&err];
  if (!params->ret)
    fprintf(stderr, "winemetal: newArgumentTable FAILED: %s\n",
            err ? err.localizedDescription.UTF8String : "(no error)");
  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newMTL4Compiler(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  MTL4CompilerDescriptor *desc = [[MTL4CompilerDescriptor alloc] init];
  NSError *err = NULL;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newCompilerWithDescriptor:desc error:&err];
  if (!params->ret)
    fprintf(stderr, "winemetal: newMTL4Compiler FAILED: %s\n",
            err ? err.localizedDescription.UTF8String : "(no error)");
  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newResidencySet(void *obj) {
  struct unixcall_mtl4_new_from_desc *params = obj;
  const struct WMTResidencySetInfo *info = params->info.ptr;
  MTLResidencySetDescriptor *desc = [[MTLResidencySetDescriptor alloc] init];
  desc.initialCapacity = info->initial_capacity;
  NSError *err = NULL;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->device newResidencySetWithDescriptor:desc error:&err];
  if (!params->ret)
    fprintf(stderr, "winemetal: newResidencySet FAILED: %s\n",
            err ? err.localizedDescription.UTF8String : "(no error)");
  [desc release];
  return STATUS_SUCCESS;
}

/* --- command allocator (163) ----------------------------------------- */
static NTSTATUS
_MTL4CommandAllocator_reset(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(id<MTL4CommandAllocator>)params->handle reset];
  return STATUS_SUCCESS;
}

/* --- command buffer (164..166) --------------------------------------- */
static NTSTATUS
_MTL4CommandBuffer_beginCommandBuffer(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  [(id<MTL4CommandBuffer>)params->handle beginCommandBufferWithAllocator:(id<MTL4CommandAllocator>)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandBuffer_endCommandBuffer(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(id<MTL4CommandBuffer>)params->handle endCommandBuffer];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandBuffer_useResidencySet(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  [(id<MTL4CommandBuffer>)params->handle useResidencySet:(id<MTLResidencySet>)params->arg];
  return STATUS_SUCCESS;
}

/* --- command queue (167..171) ---------------------------------------- */
static NTSTATUS
_MTL4CommandQueue_commit(void *obj) {
  struct unixcall_mtl4_queue_commit *params = obj;
  const obj_handle_t *handles = params->cmdbufs.ptr;
  NSUInteger count = (NSUInteger)params->count;
  /* obj_handle_t is a uint64 id-bitpattern; MTL4 commit wants a contiguous
   * id<MTL4CommandBuffer>[] — reinterpret the handle array in place. */
  if (getenv("D9MT_WSILOG")) {
    static _Atomic unsigned n_commit;
    unsigned k = n_commit++;
    if (k < 24 || (k % 100) == 0)
      fprintf(stderr, "D9MT-WSI[unix] queue_commit #%u count=%lu\n", k, (unsigned long)count);
  }
  [(id<MTL4CommandQueue>)params->queue commit:(const id<MTL4CommandBuffer> _Nonnull *)handles count:count];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandQueue_waitForDrawable(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  static _Atomic unsigned n_wait;
  unsigned k = n_wait++;
  if (k < 16 || (k % 100) == 0)
    D9MT_WSILOG("waitForDrawable #%u drawable=%p", k, (void *)params->arg);
  [(id<MTL4CommandQueue>)params->handle waitForDrawable:(id<MTLDrawable>)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandQueue_signalDrawable(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  static _Atomic unsigned n_sig;
  unsigned k = n_sig++;
  if (k < 16 || (k % 100) == 0)
    D9MT_WSILOG("signalDrawable #%u drawable=%p", k, (void *)params->arg);
  [(id<MTL4CommandQueue>)params->handle signalDrawable:(id<MTLDrawable>)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandQueue_signalEvent(void *obj) {
  struct unixcall_generic_obj_obj_uint64_noret *params = obj;
  [(id<MTL4CommandQueue>)params->handle signalEvent:(id<MTLEvent>)params->arg0 value:params->arg1];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandQueue_waitForEvent(void *obj) {
  struct unixcall_generic_obj_obj_uint64_noret *params = obj;
  [(id<MTL4CommandQueue>)params->handle waitForEvent:(id<MTLEvent>)params->arg0 value:params->arg1];
  return STATUS_SUCCESS;
}

/* --- argument table (172..175) --------------------------------------- */
static NTSTATUS
_MTL4ArgumentTable_setAddress(void *obj) {
  struct unixcall_mtl4_argtable_setaddress *params = obj;
  [(id<MTL4ArgumentTable>)params->table setAddress:(MTLGPUAddress)params->gpu_address atIndex:(NSUInteger)params->index];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4ArgumentTable_setAddressWithStride(void *obj) {
  struct unixcall_mtl4_argtable_setaddress *params = obj;
  [(id<MTL4ArgumentTable>)params->table setAddress:(MTLGPUAddress)params->gpu_address
                                   attributeStride:(NSUInteger)params->stride
                                           atIndex:(NSUInteger)params->index];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4ArgumentTable_setTexture(void *obj) {
  struct unixcall_mtl4_argtable_setresource *params = obj;
  MTLResourceID rid = { params->gpu_resource_id };
  [(id<MTL4ArgumentTable>)params->table setTexture:rid atIndex:(NSUInteger)params->index];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4ArgumentTable_setSamplerState(void *obj) {
  struct unixcall_mtl4_argtable_setresource *params = obj;
  MTLResourceID rid = { params->gpu_resource_id };
  [(id<MTL4ArgumentTable>)params->table setSamplerState:rid atIndex:(NSUInteger)params->index];
  return STATUS_SUCCESS;
}

/* --- render command encoder (176..188) ------------------------------- */
static MTL4RenderPassDescriptor *
_wm_mtl4_render_pass_descriptor(const struct WMTRenderPassInfo *info) {
  MTL4RenderPassDescriptor *descriptor = [[MTL4RenderPassDescriptor alloc] init];
  for (unsigned i = 0; i < 8; i++) {
    if (!info->colors[i].texture)
      continue;
    descriptor.colorAttachments[i].clearColor = MTLClearColorMake(
        info->colors[i].clear_color.r, info->colors[i].clear_color.g, info->colors[i].clear_color.b,
        info->colors[i].clear_color.a
    );
    descriptor.colorAttachments[i].level = info->colors[i].level;
    descriptor.colorAttachments[i].slice = info->colors[i].slice;
    descriptor.colorAttachments[i].depthPlane = info->colors[i].depth_plane;
    descriptor.colorAttachments[i].texture = (id<MTLTexture>)info->colors[i].texture;
    descriptor.colorAttachments[i].loadAction = (MTLLoadAction)info->colors[i].load_action;
    descriptor.colorAttachments[i].storeAction = (MTLStoreAction)info->colors[i].store_action;
    descriptor.colorAttachments[i].resolveTexture = (id<MTLTexture>)info->colors[i].resolve_texture;
    descriptor.colorAttachments[i].resolveLevel = info->colors[i].resolve_level;
    descriptor.colorAttachments[i].resolveSlice = info->colors[i].resolve_slice;
    descriptor.colorAttachments[i].resolveDepthPlane = info->colors[i].resolve_depth_plane;
  }
  if (info->depth.texture) {
    descriptor.depthAttachment.clearDepth = info->depth.clear_depth;
    descriptor.depthAttachment.depthPlane = info->depth.depth_plane;
    descriptor.depthAttachment.level = info->depth.level;
    descriptor.depthAttachment.slice = info->depth.slice;
    descriptor.depthAttachment.texture = (id<MTLTexture>)info->depth.texture;
    descriptor.depthAttachment.loadAction = (MTLLoadAction)info->depth.load_action;
    descriptor.depthAttachment.storeAction = (MTLStoreAction)info->depth.store_action;
  }
  if (info->stencil.texture) {
    descriptor.stencilAttachment.clearStencil = info->stencil.clear_stencil;
    descriptor.stencilAttachment.depthPlane = info->stencil.depth_plane;
    descriptor.stencilAttachment.level = info->stencil.level;
    descriptor.stencilAttachment.slice = info->stencil.slice;
    descriptor.stencilAttachment.texture = (id<MTLTexture>)info->stencil.texture;
    descriptor.stencilAttachment.loadAction = (MTLLoadAction)info->stencil.load_action;
    descriptor.stencilAttachment.storeAction = (MTLStoreAction)info->stencil.store_action;
  }
  descriptor.defaultRasterSampleCount = info->default_raster_sample_count;
  descriptor.renderTargetArrayLength = info->render_target_array_length;
  descriptor.renderTargetHeight = info->render_target_height;
  descriptor.renderTargetWidth = info->render_target_width;
  descriptor.visibilityResultBuffer = (id<MTLBuffer>)info->visibility_buffer;
  if (info->tile_height && info->tile_width) {
    descriptor.tileWidth = info->tile_width;
    descriptor.tileHeight = info->tile_height;
  }
  return descriptor;
}

static NTSTATUS
_MTL4CommandBuffer_renderCommandEncoder(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  const struct WMTRenderPassInfo *info = (const struct WMTRenderPassInfo *)params->arg;
  MTL4RenderPassDescriptor *descriptor = _wm_mtl4_render_pass_descriptor(info);
  if (getenv("D9MT_WSILOG"))
    fprintf(stderr, "D9MT-WSI[unix] renderPass c0tex=%p load=%u clear=(%.2f,%.2f,%.2f,%.2f)\n",
            (void *)info->colors[0].texture, (unsigned)info->colors[0].load_action,
            info->colors[0].clear_color.r, info->colors[0].clear_color.g,
            info->colors[0].clear_color.b, info->colors[0].clear_color.a);
  params->ret = (obj_handle_t)[[(id<MTL4CommandBuffer>)params->handle renderCommandEncoderWithDescriptor:descriptor] retain];
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4RenderCommandEncoder_setArgumentTable(void *obj) {
  struct unixcall_mtl4_setargumenttable *params = obj;
  [(id<MTL4RenderCommandEncoder>)params->encoder setArgumentTable:(id<MTL4ArgumentTable>)params->table
                                                        atStages:(MTLRenderStages)params->stages];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4RenderCommandEncoder_setRenderPipelineState(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  [(id<MTL4RenderCommandEncoder>)params->handle setRenderPipelineState:(id<MTLRenderPipelineState>)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4RenderCommandEncoder_setViewport(void *obj) {
  struct unixcall_mtl4_setviewport *params = obj;
  [(id<MTL4RenderCommandEncoder>)params->encoder setViewport:*(MTLViewport *)&params->viewport];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4RenderCommandEncoder_setScissorRect(void *obj) {
  struct unixcall_mtl4_setscissorrect *params = obj;
  [(id<MTL4RenderCommandEncoder>)params->encoder setScissorRect:*(MTLScissorRect *)&params->rect];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4RenderCommandEncoder_setDepthStencilState(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  [(id<MTL4RenderCommandEncoder>)params->handle setDepthStencilState:(id<MTLDepthStencilState>)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4RenderCommandEncoder_setCullMode(void *obj) {
  struct unixcall_generic_obj_uint64_noret *params = obj;
  [(id<MTL4RenderCommandEncoder>)params->handle setCullMode:(MTLCullMode)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4RenderCommandEncoder_setFrontFacingWinding(void *obj) {
  struct unixcall_generic_obj_uint64_noret *params = obj;
  [(id<MTL4RenderCommandEncoder>)params->handle setFrontFacingWinding:(MTLWinding)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4RenderCommandEncoder_setDepthBias(void *obj) {
  struct unixcall_mtl4_setdepthbias *params = obj;
  [(id<MTL4RenderCommandEncoder>)params->encoder setDepthBias:params->bias
                                                  slopeScale:params->slope_scale
                                                       clamp:params->clamp];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4RenderCommandEncoder_setBlendColor(void *obj) {
  struct unixcall_mtl4_setblendcolor *params = obj;
  [(id<MTL4RenderCommandEncoder>)params->encoder setBlendColorRed:params->red
                                                          green:params->green
                                                           blue:params->blue
                                                          alpha:params->alpha];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4RenderCommandEncoder_setStencilReferenceValue(void *obj) {
  struct unixcall_generic_obj_uint64_noret *params = obj;
  [(id<MTL4RenderCommandEncoder>)params->handle setStencilReferenceValue:(uint32_t)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4RenderCommandEncoder_drawPrimitives(void *obj) {
  struct unixcall_mtl4_draw *params = obj;
  [(id<MTL4RenderCommandEncoder>)params->encoder drawPrimitives:(MTLPrimitiveType)params->primitive
                                                   vertexStart:(NSUInteger)params->vertex_start
                                                   vertexCount:(NSUInteger)params->vertex_count
                                                 instanceCount:(NSUInteger)params->instance_count
                                                  baseInstance:(NSUInteger)params->base_instance];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4RenderCommandEncoder_drawIndexedPrimitives(void *obj) {
  struct unixcall_mtl4_draw_indexed *params = obj;
  [(id<MTL4RenderCommandEncoder>)params->encoder drawIndexedPrimitives:(MTLPrimitiveType)params->primitive
                                                           indexCount:(NSUInteger)params->index_count
                                                            indexType:(MTLIndexType)params->index_type
                                                          indexBuffer:(MTLGPUAddress)params->index_buffer
                                                    indexBufferLength:(NSUInteger)params->index_buffer_length
                                                        instanceCount:(NSUInteger)params->instance_count
                                                           baseVertex:(NSInteger)params->base_vertex
                                                         baseInstance:(NSUInteger)params->base_instance];
  return STATUS_SUCCESS;
}

/* --- compute command encoder (189..195) ------------------------------ */
static NTSTATUS
_MTL4CommandBuffer_computeCommandEncoder(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[[(id<MTL4CommandBuffer>)params->handle computeCommandEncoder] retain];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4ComputeCommandEncoder_copyFromBufferToBuffer(void *obj) {
  struct unixcall_mtl4_copy_buf_buf *params = obj;
  [(id<MTL4ComputeCommandEncoder>)params->encoder copyFromBuffer:(id<MTLBuffer>)params->src
                                                   sourceOffset:(NSUInteger)params->src_offset
                                                       toBuffer:(id<MTLBuffer>)params->dst
                                              destinationOffset:(NSUInteger)params->dst_offset
                                                           size:(NSUInteger)params->size];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4ComputeCommandEncoder_copyFromBufferToTexture(void *obj) {
  struct unixcall_mtl4_copy_buf_tex *params = obj;
  MTLSize src_size = { (NSUInteger)params->src_size.width, (NSUInteger)params->src_size.height, (NSUInteger)params->src_size.depth };
  MTLOrigin dst_origin = { (NSUInteger)params->dst_origin.x, (NSUInteger)params->dst_origin.y, (NSUInteger)params->dst_origin.z };
  [(id<MTL4ComputeCommandEncoder>)params->encoder copyFromBuffer:(id<MTLBuffer>)params->src_buffer
                                                   sourceOffset:(NSUInteger)params->src_offset
                                              sourceBytesPerRow:(NSUInteger)params->src_bytes_per_row
                                            sourceBytesPerImage:(NSUInteger)params->src_bytes_per_image
                                                     sourceSize:src_size
                                                      toTexture:(id<MTLTexture>)params->dst_texture
                                               destinationSlice:(NSUInteger)params->dst_slice
                                               destinationLevel:(NSUInteger)params->dst_level
                                              destinationOrigin:dst_origin];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4ComputeCommandEncoder_copyFromTextureToTexture(void *obj) {
  struct unixcall_mtl4_copy_tex_tex *params = obj;
  MTLOrigin src_origin = { (NSUInteger)params->src_origin.x, (NSUInteger)params->src_origin.y, (NSUInteger)params->src_origin.z };
  MTLSize src_size = { (NSUInteger)params->src_size.width, (NSUInteger)params->src_size.height, (NSUInteger)params->src_size.depth };
  MTLOrigin dst_origin = { (NSUInteger)params->dst_origin.x, (NSUInteger)params->dst_origin.y, (NSUInteger)params->dst_origin.z };
  if (getenv("D9MT_WSILOG"))
    fprintf(stderr, "D9MT-WSI[unix] copyTexTex enc=%p src=%p dst=%p %lux%lu ENTER\n",
            (void *)params->encoder, (void *)params->src, (void *)params->dst,
            (unsigned long)src_size.width, (unsigned long)src_size.height);
  [(id<MTL4ComputeCommandEncoder>)params->encoder copyFromTexture:(id<MTLTexture>)params->src
                                                     sourceSlice:(NSUInteger)params->src_slice
                                                     sourceLevel:(NSUInteger)params->src_level
                                                    sourceOrigin:src_origin
                                                      sourceSize:src_size
                                                       toTexture:(id<MTLTexture>)params->dst
                                                destinationSlice:(NSUInteger)params->dst_slice
                                                destinationLevel:(NSUInteger)params->dst_level
                                               destinationOrigin:dst_origin];
  if (getenv("D9MT_WSILOG"))
    fprintf(stderr, "D9MT-WSI[unix] copyTexTex EXIT\n");
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4ComputeCommandEncoder_copyFromTextureToBuffer(void *obj) {
  struct unixcall_mtl4_copy_tex_buf *params = obj;
  MTLOrigin src_origin = { (NSUInteger)params->src_origin.x, (NSUInteger)params->src_origin.y, (NSUInteger)params->src_origin.z };
  MTLSize src_size = { (NSUInteger)params->src_size.width, (NSUInteger)params->src_size.height, (NSUInteger)params->src_size.depth };
  if (getenv("D9MT_WSILOG"))
    fprintf(stderr, "D9MT-WSI[unix] copyTexBuf tex=%p %lux%lu -> buf=%p off=%llu bpr=%llu ENTER\n",
            (void *)params->src_texture,
            (unsigned long)src_size.width, (unsigned long)src_size.height,
            (void *)params->dst_buffer,
            (unsigned long long)params->dst_offset,
            (unsigned long long)params->dst_bytes_per_row);
  [(id<MTL4ComputeCommandEncoder>)params->encoder copyFromTexture:(id<MTLTexture>)params->src_texture
                                                     sourceSlice:(NSUInteger)params->src_slice
                                                     sourceLevel:(NSUInteger)params->src_level
                                                    sourceOrigin:src_origin
                                                      sourceSize:src_size
                                                        toBuffer:(id<MTLBuffer>)params->dst_buffer
                                               destinationOffset:(NSUInteger)params->dst_offset
                                          destinationBytesPerRow:(NSUInteger)params->dst_bytes_per_row
                                        destinationBytesPerImage:(NSUInteger)params->dst_bytes_per_image];
  if (getenv("D9MT_WSILOG"))
    fprintf(stderr, "D9MT-WSI[unix] copyTexBuf EXIT\n");
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4ComputeCommandEncoder_fillBuffer(void *obj) {
  struct unixcall_mtl4_fillbuffer *params = obj;
  NSRange range = NSMakeRange((NSUInteger)params->offset, (NSUInteger)params->length);
  [(id<MTL4ComputeCommandEncoder>)params->encoder fillBuffer:(id<MTLBuffer>)params->buffer
                                                     range:range
                                                     value:(uint8_t)params->value];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4ComputeCommandEncoder_generateMipmaps(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  [(id<MTL4ComputeCommandEncoder>)params->handle generateMipmapsForTexture:(id<MTLTexture>)params->arg];
  return STATUS_SUCCESS;
}

/* --- compiler + render pipeline (196) -------------------------------- */
static NTSTATUS
_MTL4Compiler_newRenderPipelineState(void *obj) {
  struct unixcall_mtl4_newrenderpso *params = obj;
  const struct WMTMtl4RenderPipelineInfo *info = params->info.ptr;
  MTL4RenderPipelineDescriptor *descriptor = [[MTL4RenderPipelineDescriptor alloc] init];

  MTL4LibraryFunctionDescriptor *vfd = [[MTL4LibraryFunctionDescriptor alloc] init];
  vfd.library = (id<MTLLibrary>)info->vertex_library;
  vfd.name = [NSString stringWithUTF8String:(const char *)info->vertex_entry_name.ptr];
  descriptor.vertexFunctionDescriptor = vfd;
  [vfd release];

  if (info->fragment_library && info->fragment_entry_name.ptr) {
    MTL4LibraryFunctionDescriptor *ffd = [[MTL4LibraryFunctionDescriptor alloc] init];
    ffd.library = (id<MTLLibrary>)info->fragment_library;
    ffd.name = [NSString stringWithUTF8String:(const char *)info->fragment_entry_name.ptr];
    descriptor.fragmentFunctionDescriptor = ffd;
    [ffd release];
  }

  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].pixelFormat = to_metal_pixel_format(info->colors[i].pixel_format);
    descriptor.colorAttachments[i].blendingState =
        info->colors[i].blending_enabled ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
    descriptor.colorAttachments[i].writeMask = (MTLColorWriteMask)info->colors[i].write_mask;
    descriptor.colorAttachments[i].rgbBlendOperation = (MTLBlendOperation)info->colors[i].rgb_blend_operation;
    descriptor.colorAttachments[i].alphaBlendOperation = (MTLBlendOperation)info->colors[i].alpha_blend_operation;
    descriptor.colorAttachments[i].sourceRGBBlendFactor = (MTLBlendFactor)info->colors[i].src_rgb_blend_factor;
    descriptor.colorAttachments[i].sourceAlphaBlendFactor = (MTLBlendFactor)info->colors[i].src_alpha_blend_factor;
    descriptor.colorAttachments[i].destinationRGBBlendFactor = (MTLBlendFactor)info->colors[i].dst_rgb_blend_factor;
    descriptor.colorAttachments[i].destinationAlphaBlendFactor = (MTLBlendFactor)info->colors[i].dst_alpha_blend_factor;
  }

  const struct WMTRenderPipelineVertexInfo *vertex_info = &info->vertex;
  if (vertex_info->num_attributes || vertex_info->num_layouts) {
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    for (unsigned i = 0; i < vertex_info->num_attributes && i < 31; i++) {
      const struct WMTVertexAttributeInfo *attr = &vertex_info->attributes[i];
      vd.attributes[i].format = (MTLVertexFormat)attr->format;
      vd.attributes[i].offset = attr->offset;
      vd.attributes[i].bufferIndex = attr->buffer_index;
    }
    for (unsigned i = 0; i < vertex_info->num_layouts && i < 31; i++) {
      const struct WMTVertexBufferLayoutInfo *layout = &vertex_info->layouts[i];
      if (!layout->stride && layout->step_function == WMTVertexStepFunctionPerVertex)
        continue;
      vd.layouts[i].stride = layout->stride;
      vd.layouts[i].stepFunction = (MTLVertexStepFunction)layout->step_function;
      vd.layouts[i].stepRate = layout->step_function == WMTVertexStepFunctionConstant ? 0 : layout->step_rate;
    }
    descriptor.vertexDescriptor = vd;
  }

  descriptor.rasterSampleCount = info->raster_sample_count ? info->raster_sample_count : 1;
  descriptor.alphaToCoverageState =
      info->alpha_to_coverage_enabled ? MTL4AlphaToCoverageStateEnabled : MTL4AlphaToCoverageStateDisabled;
  descriptor.rasterizationEnabled = info->rasterization_enabled ? YES : NO;
  descriptor.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)info->input_primitive_topology;

  NSError *err = NULL;
  params->ret_pso = (obj_handle_t)[(id<MTL4Compiler>)params->compiler newRenderPipelineStateWithDescriptor:descriptor
                                                                                     compilerTaskOptions:nil
                                                                                                   error:&err];
  params->ret_error = (obj_handle_t)[err retain];   /* +1 across the boundary; PE releases */
  [descriptor release];
  return STATUS_SUCCESS;
}

/* --- residency set (197..200) ---------------------------------------- */
static NTSTATUS
_MTLResidencySet_addAllocation(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  [(id<MTLResidencySet>)params->handle addAllocation:(id<MTLAllocation>)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLResidencySet_removeAllocation(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  [(id<MTLResidencySet>)params->handle removeAllocation:(id<MTLAllocation>)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLResidencySet_commit(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(id<MTLResidencySet>)params->handle commit];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLResidencySet_requestResidency(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(id<MTLResidencySet>)params->handle requestResidency];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLResidencySet_removeAllAllocations(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(id<MTLResidencySet>)params->handle removeAllAllocations];
  return STATUS_SUCCESS;
}

/* --- drawable present (201) ------------------------------------------ */
static NTSTATUS
_MetalDrawable_present(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  static _Atomic unsigned n_pres;
  unsigned k = n_pres++;
  if (k < 16 || (k % 100) == 0)
    D9MT_WSILOG("present #%u drawable=%p", k, (void *)params->handle);
  [(id<MTLDrawable>)params->handle present];
  return STATUS_SUCCESS;
}

/* --- render encoder visibility mode (202) ---------------------------- */
static NTSTATUS
_MTL4RenderCommandEncoder_setVisibilityResultMode(void *obj) {
  struct unixcall_mtl4_setvisibilitymode *params = obj;
  [(id<MTL4RenderCommandEncoder>)params->encoder setVisibilityResultMode:(MTLVisibilityResultMode)params->mode
                                                                 offset:(NSUInteger)params->offset];
  return STATUS_SUCCESS;
}

/* --- cross-encoder producer barrier (203) --------------------------- *
 * -[MTL4CommandEncoder barrierAfterStages:beforeQueueStages:visibilityOptions:]
 * is declared on the MTL4CommandEncoder protocol both the render and compute
 * encoders adopt, so one thunk orders either encoder's writes ahead of later
 * queue stages (SOURCE_OF_TRUTH §4c). */
static NTSTATUS
_MTL4CommandEncoder_barrier(void *obj) {
  struct unixcall_mtl4_barrier *params = obj;
  [(id<MTL4CommandEncoder>)params->encoder
      barrierAfterStages:(MTLStages)params->after_stages
       beforeQueueStages:(MTLStages)params->before_stages
       visibilityOptions:(MTL4VisibilityOptions)params->visibility];
  return STATUS_SUCCESS;
}

/* --- drawable residency + queue-level attach (205..207) -------------- *
 * The layer's residencySet is read-only and layer-owned: return it
 * BORROWED (no retain — attaching it to the queue retains it unix-side). */
static NTSTATUS
_MetalLayer_residencySet(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(CAMetalLayer *)params->handle residencySet];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandQueue_addResidencySet(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  [(id<MTL4CommandQueue>)params->handle addResidencySet:(id<MTLResidencySet>)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandQueue_removeResidencySet(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  [(id<MTL4CommandQueue>)params->handle removeResidencySet:(id<MTLResidencySet>)params->arg];
  return STATUS_SUCCESS;
}

/* --- MTL4 render command stream (208) --------------------------------- *
 * One crossing per batch: replays the PE-built wmtcmd_mtl4_* chain natively
 * (the Metal-4 counterpart of _MTLRenderCommandEncoder_encodeCommands
 * above). Argument-table commands carry their own table handle —
 * MTL4ArgumentTable is not an encoder object — so table writes and the
 * draws that read them stay in one ordered stream. Every payload is a
 * record-time snapshot; nothing is resolved here. The pooled dispatch
 * wrapper's autoreleasepool covers the whole replay. */
static NTSTATUS
_MTL4RenderCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  id<MTL4RenderCommandEncoder> encoder = (id<MTL4RenderCommandEncoder>)params->encoder;
  while (next) {
    switch ((enum WMTMtl4RenderCommandType)next->type) {
    default:
      assert(!next->type && "unhandled mtl4 render command type");
      break;
    case WMTMtl4RenderCommandNop:
      break;
    case WMTMtl4RenderCommandSetPSO: {
      struct wmtcmd_mtl4_setpso *body = (struct wmtcmd_mtl4_setpso *)next;
      [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)body->pso];
      break;
    }
    case WMTMtl4RenderCommandSetArgumentTable: {
      struct wmtcmd_mtl4_setargtable *body = (struct wmtcmd_mtl4_setargtable *)next;
      [encoder setArgumentTable:(id<MTL4ArgumentTable>)body->table
                       atStages:(MTLRenderStages)body->stages];
      break;
    }
    case WMTMtl4RenderCommandTableSetAddress: {
      struct wmtcmd_mtl4_table_setaddress *body = (struct wmtcmd_mtl4_table_setaddress *)next;
      [(id<MTL4ArgumentTable>)body->table setAddress:(MTLGPUAddress)body->gpu_address
                                             atIndex:(NSUInteger)body->index];
      break;
    }
    case WMTMtl4RenderCommandTableSetTexture: {
      struct wmtcmd_mtl4_table_setresource *body = (struct wmtcmd_mtl4_table_setresource *)next;
      MTLResourceID rid = { body->gpu_resource_id };
      [(id<MTL4ArgumentTable>)body->table setTexture:rid atIndex:(NSUInteger)body->index];
      break;
    }
    case WMTMtl4RenderCommandTableSetSamplerState: {
      struct wmtcmd_mtl4_table_setresource *body = (struct wmtcmd_mtl4_table_setresource *)next;
      MTLResourceID rid = { body->gpu_resource_id };
      [(id<MTL4ArgumentTable>)body->table setSamplerState:rid atIndex:(NSUInteger)body->index];
      break;
    }
    case WMTMtl4RenderCommandSetViewport: {
      struct wmtcmd_mtl4_setviewport *body = (struct wmtcmd_mtl4_setviewport *)next;
      [encoder setViewport:*(MTLViewport *)&body->viewport];
      break;
    }
    case WMTMtl4RenderCommandSetScissorRect: {
      struct wmtcmd_mtl4_setscissorrect *body = (struct wmtcmd_mtl4_setscissorrect *)next;
      [encoder setScissorRect:*(MTLScissorRect *)&body->rect];
      break;
    }
    case WMTMtl4RenderCommandSetDepthStencilState: {
      struct wmtcmd_mtl4_setdsso *body = (struct wmtcmd_mtl4_setdsso *)next;
      [encoder setDepthStencilState:(id<MTLDepthStencilState>)body->dsso];
      break;
    }
    case WMTMtl4RenderCommandSetCullMode: {
      struct wmtcmd_mtl4_setcullmode *body = (struct wmtcmd_mtl4_setcullmode *)next;
      [encoder setCullMode:(MTLCullMode)body->mode];
      break;
    }
    case WMTMtl4RenderCommandSetFrontFacingWinding: {
      struct wmtcmd_mtl4_setwinding *body = (struct wmtcmd_mtl4_setwinding *)next;
      [encoder setFrontFacingWinding:(MTLWinding)body->winding];
      break;
    }
    case WMTMtl4RenderCommandSetDepthBias: {
      struct wmtcmd_mtl4_setdepthbias *body = (struct wmtcmd_mtl4_setdepthbias *)next;
      [encoder setDepthBias:body->bias slopeScale:body->slope_scale clamp:body->clamp];
      break;
    }
    case WMTMtl4RenderCommandSetBlendColor: {
      struct wmtcmd_mtl4_setblendcolor *body = (struct wmtcmd_mtl4_setblendcolor *)next;
      [encoder setBlendColorRed:body->red green:body->green blue:body->blue alpha:body->alpha];
      break;
    }
    case WMTMtl4RenderCommandSetStencilReferenceValue: {
      struct wmtcmd_mtl4_setstencilref *body = (struct wmtcmd_mtl4_setstencilref *)next;
      [encoder setStencilReferenceValue:(uint32_t)body->reference];
      break;
    }
    case WMTMtl4RenderCommandSetVisibilityResultMode: {
      struct wmtcmd_mtl4_setvisibilitymode *body = (struct wmtcmd_mtl4_setvisibilitymode *)next;
      [encoder setVisibilityResultMode:(MTLVisibilityResultMode)body->mode
                                offset:(NSUInteger)body->offset];
      break;
    }
    case WMTMtl4RenderCommandDraw: {
      struct wmtcmd_mtl4_draw *body = (struct wmtcmd_mtl4_draw *)next;
      [encoder drawPrimitives:(MTLPrimitiveType)body->primitive
                  vertexStart:(NSUInteger)body->vertex_start
                  vertexCount:(NSUInteger)body->vertex_count
                instanceCount:(NSUInteger)body->instance_count
                 baseInstance:(NSUInteger)body->base_instance];
      break;
    }
    case WMTMtl4RenderCommandDrawIndexed: {
      struct wmtcmd_mtl4_draw_indexed *body = (struct wmtcmd_mtl4_draw_indexed *)next;
      [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive
                          indexCount:(NSUInteger)body->index_count
                           indexType:(MTLIndexType)body->index_type
                         indexBuffer:(MTLGPUAddress)body->index_buffer
                   indexBufferLength:(NSUInteger)body->index_buffer_length
                       instanceCount:(NSUInteger)body->instance_count
                          baseVertex:(NSInteger)body->base_vertex
                        baseInstance:(NSUInteger)body->base_instance];
      break;
    }
    case WMTMtl4RenderCommandBarrier: {
      struct wmtcmd_mtl4_barrier *body = (struct wmtcmd_mtl4_barrier *)next;
      [encoder barrierAfterStages:(MTLStages)body->after_stages
                beforeQueueStages:(MTLStages)body->before_stages
                visibilityOptions:(MTL4VisibilityOptions)body->visibility];
      break;
    }
    }

    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

/* --- d9mt wire transport client (render-server Phase S1) ----------------- *
 * docs/RENDER_SERVER_SPEC.md. The PE side serializes every hot MtlContext
 * call into an SPSC ring (layout: driver/mtl/mtl_wire.h). Since Phase S1 the
 * ring no longer lives on the heap with an in-process consumer thread:
 * WireRing_attach relocates it into a MAP_SHARED file and posix_spawns the
 * native arm64 helper `d9mt-serverd` (server/), which consumes the records
 * cross-process and publishes progress/heartbeat/error through a second
 * shared `status` file (server/d9mt_status.h).
 *
 * Address-space contract: the PE producer is 32-bit-capable (i686 d3d9.dll),
 * so the ring must stay at a PE-visible VA. The PE side VirtualAllocs the
 * ring region and passes it in; this side maps the ring FILE over that exact
 * region with mmap(MAP_SHARED|MAP_FIXED) — same pointer, now file-backed.
 * The existing producer code writes into shared memory unchanged.
 *
 * Shared dir: $TMPDIR/d9mt-<pid>/ with files `ring` (192 B control block +
 * capacity) and `status` (136 B), created here, unlinked at detach.
 *
 * MANDATORY FALLBACK: if the server binary is missing, spawn fails, or
 * epoch!=1 within 3 s, ONE warn line is logged, the files are unlinked, the
 * region is restored to plain anonymous memory and the attach reports
 * WMTWireAttachLocal: a ring with NO consumer, ever. The PE producer's
 * ring-full degradation (stalls counter -> permanent forward-only) keeps the
 * driver working exactly as before. */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "../../server/d9mt_status.h"

extern char **environ;

static struct d9mt_wire_ring *wire_ring; /* PE VA: shm-mapped (server) or PE anon (local) */
static uint64_t wire_ring_total;         /* control block + capacity, bytes */
static struct d9mt_status *wire_status;  /* unix-side mapping of the status file */
static struct d9mt_wire_heap *wire_heap; /* PE VA: S3 content heap (NULL = none) */
static uint64_t wire_heap_total;         /* header + data, bytes */
static pid_t wire_server_pid;
static uint32_t wire_mode; /* enum WMTWireAttachMode */
static char wire_dir[PATH_MAX];
static char wire_ring_path[PATH_MAX];
static char wire_status_path[PATH_MAX];
static char wire_heap_path[PATH_MAX];

static uint64_t
wire_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void
wire_init_header(struct d9mt_wire_ring *ring, uint64_t capacity) {
  memset(ring, 0, sizeof(*ring));
  ring->magic = D9MT_WIRE_RING_MAGIC;
  ring->capacity = capacity;
  ring->data_offset = sizeof(*ring);
}

/* Server binary resolution order (RENDER_SERVER_SPEC + task brief):
 *   1. $D9MT_SERVERD_PATH (dev trees)
 *   2. next to winemetal.so (dladdr on this function)
 *   3. /usr/local/lib/d9mt/d9mt-serverd
 * Returns the source tag of the first executable candidate, NULL if none. */
static const char *
wire_find_serverd(char out[PATH_MAX]) {
  const char *env = getenv("D9MT_SERVERD_PATH");
  if (env && *env) {
    snprintf(out, PATH_MAX, "%s", env);
    if (access(out, X_OK) == 0)
      return "env D9MT_SERVERD_PATH";
  }
  Dl_info info;
  if (dladdr((void *)&wire_find_serverd, &info) && info.dli_fname) {
    snprintf(out, PATH_MAX, "%s", info.dli_fname);
    char *slash = strrchr(out, '/');
    if (slash) {
      snprintf(slash + 1, (size_t)(PATH_MAX - (slash + 1 - out)), "d9mt-serverd");
      if (access(out, X_OK) == 0)
        return "next to winemetal.so";
    }
  }
  snprintf(out, PATH_MAX, "/usr/local/lib/d9mt/d9mt-serverd");
  if (access(out, X_OK) == 0)
    return "/usr/local/lib/d9mt";
  return NULL;
}

/* S3 v3: boot-time orphan sweep. A client that dies without detach leaks
 * its shm dir forever (nobody unlinks on a crash — the S1 flag). At attach
 * time, unlink every $TMPDIR/d9mt-<pid>[-seq] dir whose creator pid is DEAD
 * (kill(pid,0) => ESRCH). Skips our own pid (live re-attach dirs of THIS
 * process were already unlinked by detach), non-numeric names (d9mt-test-*)
 * and anything whose liveness is ambiguous (EPERM). NOT the full
 * client-death watchdog — just the cheap boot cleanup. */
static void
wire_sweep_orphans(const char *tmp) {
  DIR *d = opendir(tmp);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, "d9mt-", 5) != 0)
      continue;
    const char *p = e->d_name + 5;
    if (!isdigit((unsigned char)*p))
      continue;
    char *end = NULL;
    long pid = strtol(p, &end, 10);
    if (pid <= 0 || (end && *end != 0 && *end != '-'))
      continue;
    if ((pid_t)pid == getpid())
      continue;
    if (kill((pid_t)pid, 0) == 0 || errno != ESRCH)
      continue; /* alive (or unknowable) — leave it */
    static const char *files[] = {"ring", "status", "heap"};
    char path[PATH_MAX];
    for (unsigned i = 0; i < 3; i++) {
      snprintf(path, sizeof(path), "%s/%s/%s", tmp, e->d_name, files[i]);
      unlink(path);
    }
    snprintf(path, sizeof(path), "%s/%s", tmp, e->d_name);
    if (rmdir(path) == 0)
      fprintf(stderr, "[wire] swept orphaned shm dir %s (pid %ld dead)\n",
              e->d_name, pid);
  }
  closedir(d);
}

/* Create + size <dir>/<name>, optionally map it. When fixed_at is non-NULL
 * the file is mapped MAP_FIXED over that region (the PE ring VA); otherwise
 * anywhere. Returns the mapping (or NULL), path copied into out_path. */
static void *
wire_create_map(const char *name, size_t bytes, void *fixed_at, char out_path[PATH_MAX]) {
  snprintf(out_path, PATH_MAX, "%s/%s", wire_dir, name);
  int fd = open(out_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    return NULL;
  if (ftruncate(fd, (off_t)bytes) != 0) {
    close(fd);
    return NULL;
  }
  int flags = MAP_SHARED | (fixed_at ? MAP_FIXED : 0);
  void *p = mmap(fixed_at, bytes, PROT_READ | PROT_WRITE, flags, fd, 0);
  close(fd); /* the mapping keeps the file alive */
  return p == MAP_FAILED ? NULL : p;
}

/* Undo a shm setup: unlink files, drop the status mapping, restore plain
 * anonymous memory over the PE ring (and heap) regions — never UNMAP those
 * VAs, the PE side owns them (fallback keeps producing into them; detach
 * VirtualFrees them right after). S3 v4: the detach path passes base too,
 * so wine's VM manager only ever frees/reuses plain anon memory — a
 * file-backed range surviving into the NEXT VirtualAlloc is the re-attach
 * hazard class. The file references drop at the anon restore. */
static void
wire_teardown_shm(void *base, uint64_t total) {
  if (wire_status) {
    munmap(wire_status, sizeof(struct d9mt_status));
    wire_status = NULL;
  }
  if (wire_ring_path[0])
    unlink(wire_ring_path);
  if (wire_status_path[0])
    unlink(wire_status_path);
  if (wire_heap_path[0])
    unlink(wire_heap_path);
  if (wire_dir[0])
    rmdir(wire_dir);
  wire_ring_path[0] = wire_status_path[0] = wire_heap_path[0] = wire_dir[0] = 0;
  if (base) {
    mmap(base, total, PROT_READ | PROT_WRITE,
         MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (wire_heap)
      mmap(wire_heap, wire_heap_total, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
  }
  wire_heap = NULL;
  wire_heap_total = 0;
}

static NTSTATUS
_WireRing_attach(void *obj) {
  struct unixcall_wirering_attach *params = obj;
  uint8_t *base = params->base.ptr;
  params->mode = WMTWireAttachFailed;

  const long page = sysconf(_SC_PAGESIZE);
  const uint64_t total = params->bytes;
  const uint64_t cap = total > sizeof(struct d9mt_wire_ring)
                     ? total - sizeof(struct d9mt_wire_ring) : 0;
  if (wire_ring || !base || !cap || (cap & (cap - 1u)) ||
      ((uintptr_t)base & (uintptr_t)(page - 1))) {
    fprintf(stderr, "[wire] attach REJECTED (already attached or bad region)\n");
    return STATUS_SUCCESS; /* transport disabled; PE frees the region */
  }

  char exe[PATH_MAX];
  const char *src = wire_find_serverd(exe);
  if (!src) {
    fprintf(stderr, "[wire] WARN: no d9mt-serverd (checked $D9MT_SERVERD_PATH, "
                    "next to winemetal.so, /usr/local/lib/d9mt) — local ring, "
                    "no consumer\n");
    goto local;
  }

  unsigned seq;
  {
    /* S3 v3: attach sequence in the dir name — device churn (throwaway
     * boot devices) re-attaches with a FRESH dir + fresh server; a stale
     * dir from an unfinished teardown can never collide. */
    static unsigned wire_attach_seq; /* unix side, process lifetime */
    seq = wire_attach_seq++;
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp)
      tmp = "/tmp";
    size_t len = strlen(tmp);
    wire_sweep_orphans(tmp);
    snprintf(wire_dir, sizeof(wire_dir), "%s%sd9mt-%d-%u", tmp,
             (len && tmp[len - 1] == '/') ? "" : "/", (int)getpid(), seq);
  }
  /* S3 v4: every attach attempt names itself up front — a future failure
   * anywhere below leaves a paired begin/WARN trail, never a silent half. */
  fprintf(stderr, "[wire] attach #%u: dir=%s ring=%llu B heap=%llu B\n", seq,
          wire_dir, (unsigned long long)cap,
          (unsigned long long)params->heap_bytes);
  if (mkdir(wire_dir, 0700) != 0 && errno != EEXIST) {
    fprintf(stderr, "[wire] WARN: mkdir(%s): %s — local ring, no consumer\n",
            wire_dir, strerror(errno));
    wire_dir[0] = 0;
    goto local;
  }

  /* Relocate the ring: map the file over the PE region (same VA). */
  if (wire_create_map("ring", (size_t)total, base, wire_ring_path) != base) {
    fprintf(stderr, "[wire] WARN: ring shm map failed (%s) — local ring, no consumer\n",
            strerror(errno));
    wire_teardown_shm(base, total);
    goto local;
  }
  wire_status = wire_create_map("status", sizeof(struct d9mt_status), NULL,
                                wire_status_path);
  if (!wire_status) {
    fprintf(stderr, "[wire] WARN: status shm map failed (%s) — local ring, no consumer\n",
            strerror(errno));
    wire_teardown_shm(base, total);
    goto local;
  }
  /* S3 v4: the status file was just O_TRUNC-created — a fresh mapping MUST
   * read epoch=0. A nonzero epoch here means the mapping resolves to STALE
   * memory (the wrong-VA / leftover-mapping failure class): the handshake
   * below would then "succeed" against a dead server's last words. Fail
   * loudly instead. */
  {
    uint64_t pre = atomic_load_explicit(&wire_status->epoch, memory_order_acquire);
    if (pre != 0) {
      fprintf(stderr, "[wire] WARN: fresh status file reads epoch=%llu "
                      "(stale mapping) — local ring, no consumer\n",
              (unsigned long long)pre);
      wire_teardown_shm(base, total);
      goto local;
    }
  }
  wire_init_header((struct d9mt_wire_ring *)base, cap);

  /* S3 shared heap (optional second region, same MAP_FIXED relocation).
   * Failure is NOT fatal to the attach: the transport runs ring-only and
   * the PE side simply never emits upload records (heap_ok = 0). Mapped
   * BEFORE the spawn so the server finds the file at boot. */
  {
    uint8_t *hbase = params->heap_base.ptr;
    const uint64_t htotal = params->heap_bytes;
    const uint64_t hcap = htotal > D9MT_WIRE_HEAP_DATA_OFFSET
                        ? htotal - D9MT_WIRE_HEAP_DATA_OFFSET : 0;
    if (hbase && hcap && !(hcap & (hcap - 1u)) &&
        !((uintptr_t)hbase & (uintptr_t)(page - 1))) {
      if (wire_create_map("heap", (size_t)htotal, hbase, wire_heap_path) == (void *)hbase) {
        struct d9mt_wire_heap *h = (struct d9mt_wire_heap *)hbase;
        /* fresh O_TRUNC file: cursors already zero; header last */
        h->capacity = hcap;
        h->data_offset = D9MT_WIRE_HEAP_DATA_OFFSET;
        h->magic = D9MT_WIRE_HEAP_MAGIC;
        wire_heap = h;
        wire_heap_total = htotal;
      } else {
        fprintf(stderr, "[wire] WARN: heap shm map failed (%s) — content uploads disabled\n",
                strerror(errno));
        if (wire_heap_path[0])
          unlink(wire_heap_path);
        wire_heap_path[0] = 0;
      }
    } else if (hbase || htotal) {
      fprintf(stderr, "[wire] WARN: bad heap region (base=%p bytes=%llu) — content uploads disabled\n",
              (void *)hbase, (unsigned long long)htotal);
    }
  }

  {
    pid_t pid = 0;
    char *argv[3] = {exe, wire_dir, NULL};
    int rc = posix_spawn(&pid, exe, NULL, NULL, argv, environ);
    if (rc != 0) {
      fprintf(stderr, "[wire] WARN: posix_spawn(%s): %s — local ring, no consumer\n",
              exe, strerror(rc));
      wire_teardown_shm(base, total);
      goto local;
    }

    /* Handshake: epoch=1 is release-stored LAST by the server. 3 s budget,
     * early-out if the child dies first (S3 v4: named separately from the
     * timeout — no more silent halves). */
    int ready = 0;
    int died = 0, died_status = 0;
    for (uint32_t i = 0; i < 3000u; i++) {
      if (atomic_load_explicit(&wire_status->epoch, memory_order_acquire) == 1) {
        ready = 1;
        break;
      }
      int st;
      if (waitpid(pid, &st, WNOHANG) == pid) {
        pid = 0;
        died = 1;
        died_status = st;
        break;
      }
      usleep(1000);
    }
    if (!ready) {
      uint32_t err = atomic_load_explicit(&wire_status->error, memory_order_acquire);
      if (died)
        fprintf(stderr, "[wire] WARN: attach #%u: d9mt-serverd DIED before ready "
                        "(wait status 0x%x, server err=%u) — local ring, no consumer\n",
                seq, died_status, err);
      else
        fprintf(stderr, "[wire] WARN: attach #%u: d9mt-serverd not ready in 3s "
                        "(pid=%d err=%u) — local ring, no consumer\n",
                seq, (int)pid, err);
      if (pid) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
      }
      wire_teardown_shm(base, total);
      goto local;
    }
    /* S3 v4: epoch alone is not proof of life — require the heartbeat to
     * ADVANCE (~100 Hz once the consumer loop runs; 1 s budget). A stale
     * epoch read through any wrong mapping has a FROZEN heartbeat and fails
     * here instead of "attaching" to a dead ring. */
    {
      const uint64_t hb0 =
          atomic_load_explicit(&wire_status->heartbeat, memory_order_acquire);
      int alive = 0;
      for (uint32_t i = 0; i < 1000u; i++) {
        if (atomic_load_explicit(&wire_status->heartbeat, memory_order_acquire) != hb0) {
          alive = 1;
          break;
        }
        usleep(1000);
      }
      if (!alive) {
        fprintf(stderr, "[wire] WARN: attach #%u: epoch=1 but heartbeat FROZEN "
                        "at %llu for 1s (dead/phantom server) — local ring, no consumer\n",
                seq, (unsigned long long)hb0);
        if (pid) {
          kill(pid, SIGKILL);
          waitpid(pid, NULL, 0);
        }
        wire_teardown_shm(base, total);
        goto local;
      }
    }
    wire_server_pid = pid;
  }

  wire_ring = (struct d9mt_wire_ring *)base;
  wire_ring_total = total;
  wire_mode = WMTWireAttachServer;
  params->mode = WMTWireAttachServer;
  params->heap_ok = wire_heap != NULL;
  fprintf(stderr, "[wire] render server attached: pid=%d dir=%s exe=%s (via %s) "
                  "capacity=%llu bytes heap=%llu bytes\n",
          (int)wire_server_pid, wire_dir, exe, src, (unsigned long long)cap,
          (unsigned long long)(wire_heap ? wire_heap->capacity : 0));
  return STATUS_SUCCESS;

local:
  /* Mandatory fallback: plain memory, NO consumer. The producer degrades on
   * ring-full (stalls -> permanent forward-only); driver behaves as today. */
  wire_init_header((struct d9mt_wire_ring *)base, cap);
  wire_ring = (struct d9mt_wire_ring *)base;
  wire_ring_total = total;
  wire_mode = WMTWireAttachLocal;
  params->mode = WMTWireAttachLocal;
  return STATUS_SUCCESS;
}

/* Cross-process the kick has no channel yet (the server parks on a 1 ms
 * poll — server/README.md S1 caveats), and the local fallback has no
 * consumer at all: no-op either way. Kept registered so the producer's
 * empty->nonempty protocol is unchanged; TODO(S2): real kick channel. */
static NTSTATUS
_WireRing_kick(void *obj) {
  (void)obj;
  return STATUS_SUCCESS;
}

static NTSTATUS
_WireRing_stats(void *obj) {
  struct unixcall_wirering_stats *params = obj;
  struct WMTWireStats *out = params->out.ptr;
  if (!out)
    return STATUS_SUCCESS;
  memset(out, 0, sizeof(*out));
  if (!wire_ring)
    return STATUS_SUCCESS;
  out->records = atomic_load_explicit(&wire_ring->records, memory_order_relaxed);
  out->bytes = atomic_load_explicit(&wire_ring->bytes, memory_order_relaxed);
  out->stalls = atomic_load_explicit(&wire_ring->stalls, memory_order_relaxed);
  out->decode_errors = atomic_load_explicit(&wire_ring->decode_errors, memory_order_relaxed);
  if (wire_status) {
    out->heartbeat = atomic_load_explicit(&wire_status->heartbeat, memory_order_relaxed);
    out->server_error = atomic_load_explicit(&wire_status->error, memory_order_relaxed);
    /* status v2 (render-server S2): zero on a pre-S2 server (version==0) */
    uint32_t version = atomic_load_explicit(&wire_status->version, memory_order_acquire);
    if (version >= 2u) {
      out->server_frames = atomic_load_explicit(&wire_status->frames, memory_order_relaxed);
      out->server_frame_serial =
          atomic_load_explicit(&wire_status->frame_serial, memory_order_relaxed);
      out->server_exec_errors =
          atomic_load_explicit(&wire_status->exec_errors, memory_order_relaxed);
    }
    /* status v3 (render-server S3) */
    if (version >= 3u) {
      out->server_skipped_pso = atomic_load_explicit(
          &wire_status->draws_skipped_pending_pso, memory_order_relaxed);
      out->server_uploads =
          atomic_load_explicit(&wire_status->uploads_executed, memory_order_relaxed);
      out->server_exec_opcode =
          atomic_load_explicit(&wire_status->exec_opcode, memory_order_relaxed);
      out->server_creates_rejected =
          atomic_load_explicit(&wire_status->creates_rejected, memory_order_relaxed);
    }
  }
  if (wire_heap) {
    out->heap_consumed = atomic_load_explicit(&wire_heap->consumed, memory_order_relaxed);
    out->heap_dropped = atomic_load_explicit(&wire_heap->dropped, memory_order_relaxed);
  }
  return STATUS_SUCCESS;
}

/* Teardown (PE dtor wrote the Detach record + shutdown flag first):
 *   1. <=500 ms: wait for the server to drain (consumer_exited, or
 *      status.progress == ring.head),
 *   2. SIGTERM if still alive, reap with the remaining budget,
 *   3. SIGKILL as last resort. Total bound < 1 s.
 * Then unlink the shm files + dir. The ring VA itself is PE-owned
 * (VirtualAlloc) and is released by the PE side AFTER this call — its munmap
 * drops the last reference to the already-unlinked ring file. */
static NTSTATUS
_WireRing_detach(void *obj) {
  (void)obj;
  if (!wire_ring)
    return STATUS_SUCCESS;
  if (wire_mode == WMTWireAttachServer) {
    const uint64_t t0 = wire_now_ms();
    while (wire_now_ms() - t0 < 500u) {
      if (atomic_load_explicit(&wire_ring->consumer_exited, memory_order_acquire))
        break;
      if (wire_status &&
          atomic_load_explicit(&wire_status->progress, memory_order_acquire) ==
          atomic_load_explicit(&wire_ring->head, memory_order_acquire))
        break;
      usleep(1000);
    }
    if (wire_server_pid) {
      int st = 0;
      pid_t r = waitpid(wire_server_pid, &st, WNOHANG);
      if (r == 0) {
        kill(wire_server_pid, SIGTERM);
        while (wire_now_ms() - t0 < 950u) {
          r = waitpid(wire_server_pid, &st, WNOHANG);
          if (r != 0)
            break;
          usleep(1000);
        }
        if (r == 0) {
          fprintf(stderr, "[wire] detach: serverd unresponsive, SIGKILL\n");
          kill(wire_server_pid, SIGKILL);
          usleep(1000);
          waitpid(wire_server_pid, &st, WNOHANG);
        }
      }
      wire_server_pid = 0;
    }
    fprintf(stderr, "[wire] detached: records=%llu bytes=%llu decode_errors=%llu\n",
            (unsigned long long)atomic_load_explicit(&wire_ring->records, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&wire_ring->bytes, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&wire_ring->decode_errors, memory_order_relaxed));
    /* S3 v4: restore ANONYMOUS memory over the ring+heap VAs BEFORE the PE
     * side VirtualFrees them. Wine's VM manager created those regions as
     * anon and does not know this side swapped in MAP_SHARED file backing;
     * letting VirtualFree/re-VirtualAlloc operate on a file-backed range is
     * exactly the re-attach hazard class (a "fresh" region that still
     * resolves to the DEAD ring's file). After this the PE frees plain
     * memory — the file references drop here. */
    wire_teardown_shm(wire_ring, wire_ring_total);
  }
  wire_ring = NULL;
  wire_ring_total = 0;
  wire_mode = WMTWireAttachFailed;
  return STATUS_SUCCESS;
}

/* S3 v5 session-boundary ack: bounded wait for the server's epoch bump
 * (executed session record). Diagnostic only — record order in the ring
 * already serializes the reset ahead of any later record. */
static NTSTATUS
_WireRing_quiesce(void *obj) {
  (void)obj;
  if (!wire_status || wire_mode != WMTWireAttachServer)
    return STATUS_SUCCESS;
  const uint64_t e0 = atomic_load_explicit(&wire_status->epoch, memory_order_acquire);
  for (uint32_t i = 0; i < 700u; i++) {
    uint64_t e = atomic_load_explicit(&wire_status->epoch, memory_order_acquire);
    if (e > e0) {
      fprintf(stderr, "[wire] session record acked (epoch %llu -> %llu)\n",
              (unsigned long long)e0, (unsigned long long)e);
      return STATUS_SUCCESS;
    }
    usleep(1000);
  }
  fprintf(stderr, "[wire] WARN: session record not acked in 700ms (epoch=%llu; "
                  "server behind or dead — ring order still serializes the reset)\n",
          (unsigned long long)e0);
  return STATUS_SUCCESS;
}

/* --- S3 cutover: server IOSurface present source ------------------------- *
 * The render server renders GTA's stream into an IOSurface pool and publishes
 * {present_slot, frame_serial (GPU-done), iosurface_id[], iosurface_serial[]}.
 * The client wraps the published IOSurface as an MTLTexture on ITS OWN device
 * (IOSurface backing is shared cross-device/cross-process) and blits it into
 * the drawable. IOSurfaceLookup is ~4us and the newTextureWithDescriptor:
 * iosurface: wrap ~10us, so the wraps are cached per {IOSurfaceID, generation}
 * — the pool generation (iosurface_pool.m g_generation) bumps only on resize,
 * so exactly the 3 rotating slot surfaces stay hot; a resize adds a few more.
 *
 * OWNERSHIP / RESIDENCY (the run-11 hang fix): a texture used as a copy/blit
 * SOURCE must be RESIDENT on the client's MTL4 command queue, or the GPU faults
 * and the present's completion event never signals (the CS thread parks
 * forever on the wine sync object — exactly the observed deadlock). Driver
 * textures get this via MtlResidencyTracker::allocCreated at creation; an
 * externally-wrapped IOSurface texture does NOT, so the PE side adds each
 * freshly-minted wrap to the residency set (signalled by out_minted). Because
 * the residency set references these textures for the queue's lifetime, the
 * cache NEVER releases them (a release while still referenced would fault):
 * entries evicted from the fixed table are intentionally leaked (they stay
 * alive + resident). The leak is bounded — one small MTLTexture wrapper per
 * distinct {id,generation} beyond the table window, i.e. per resize, NOT per
 * frame (the 3 steady surfaces never evict). The IOSurface pixels themselves
 * are owned by the server pool, never by these wrappers. Not ARC (explicit
 * retain/release, see _MTLDevice_newSharedTexture). */
#define WIRE_SRV_TEX_CACHE 8u
static struct {
  IOSurfaceID    id;
  uint64_t       gen;
  id<MTLTexture> tex;   /* owned +1, intentionally never released (see above) */
} wire_srv_tex_cache[WIRE_SRV_TEX_CACHE];
static uint32_t wire_srv_tex_next = 0;   /* round-robin eviction cursor */

/* Returns the cached-or-freshly-wrapped texture; *minted set to 1 iff a new
 * texture was created this call (the PE side must then make it resident). */
static id<MTLTexture>
wire_server_lookup_texture(id<MTLDevice> device, IOSurfaceID sid, uint64_t gen,
                           uint32_t *minted) {
  *minted = 0;
  for (uint32_t i = 0; i < WIRE_SRV_TEX_CACHE; i++) {
    if (wire_srv_tex_cache[i].tex &&
        wire_srv_tex_cache[i].id == sid && wire_srv_tex_cache[i].gen == gen)
      return wire_srv_tex_cache[i].tex;   /* hot path: same surface generation */
  }

  IOSurfaceRef surf = IOSurfaceLookup(sid);
  if (!surf)
    return nil;                          /* id not resolvable this process yet */

  size_t w = IOSurfaceGetWidth(surf);
  size_t h = IOSurfaceGetHeight(surf);
  if (w == 0 || h == 0) {                /* malformed surface — never present it */
    CFRelease(surf);
    return nil;
  }
  /* The server pool is BGRA8Unorm (matches the D3D9 X8R8G8B8 backbuffer path).
   * A non-BGRA server pool would need format negotiation; documented in
   * HANDOFF-CUTOVER.md as the single-format assumption. */
  MTLTextureDescriptor *desc =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:w
                                                        height:h
                                                     mipmapped:NO];
  /* ShaderRead: the scaled-present path samples the source in a render pass
   * (blitTextureView). RenderTarget: keep it a valid copy destination too. */
  desc.usage       = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
  desc.storageMode = MTLStorageModeShared;   /* IOSurface-backed */
  id<MTLTexture> tex = [device newTextureWithDescriptor:desc iosurface:surf plane:0];
  CFRelease(surf);                            /* the texture retains the surface */
  /* desc came from the texture2DDescriptor... class factory = AUTORELEASED;
   * the per-unixcall pool owns it. Releasing it here double-frees at pool
   * drain (c0000005 in unixcall 216 — the cutover boot hang). */
  if (!tex)
    return nil;

  uint32_t slot = wire_srv_tex_next;
  wire_srv_tex_next = (wire_srv_tex_next + 1u) % WIRE_SRV_TEX_CACHE;
  /* Evicted entry is intentionally NOT released — it may still sit in the
   * client residency set; releasing it would fault the GPU. Bounded leak. */
  wire_srv_tex_cache[slot].id  = sid;
  wire_srv_tex_cache[slot].gen = gen;
  wire_srv_tex_cache[slot].tex = tex;         /* own the +1 from newTexture */
  *minted = 1;
  return tex;
}

static NTSTATUS
_WireServerFrame_acquire(void *obj) {
  struct unixcall_wire_server_frame *p = obj;
  p->out_usable = 0;
  p->out_texture = 0;
  p->out_minted = 0;
  p->out_gpu_resource_id = 0;
  if (!wire_status)
    return STATUS_SUCCESS;

  uint32_t version = atomic_load_explicit(&wire_status->version, memory_order_acquire);
  if (version < 2u)
    return STATUS_SUCCESS;                     /* pre-S2 server: no frames */

  uint64_t frames = atomic_load_explicit(&wire_status->frames, memory_order_relaxed);
  if (frames == 0u)
    return STATUS_SUCCESS;                      /* nothing presented yet */

  uint64_t fs   = atomic_load_explicit(&wire_status->frame_serial, memory_order_relaxed);
  uint32_t slot = atomic_load_explicit(&wire_status->present_slot, memory_order_relaxed);

  /* Pace gate: the server's completed serial must be within `lag_frames` of
   * the present the client is about to do. `local - fs > lag` = too stale ->
   * fall back to the always-ready local backbuffer. Server-ahead is fine
   * (present the freshest completed frame). */
  if (p->local_serial > fs && (p->local_serial - fs) > p->lag_frames)
    return STATUS_SUCCESS;

  if (slot >= D9MT_STATUS_SLOTS)
    return STATUS_SUCCESS;
  IOSurfaceID sid = atomic_load_explicit(&wire_status->iosurface_id[slot], memory_order_relaxed);
  uint64_t    gen = atomic_load_explicit(&wire_status->iosurface_serial[slot], memory_order_relaxed);
  if (sid == 0)
    return STATUS_SUCCESS;                      /* empty slot */

  uint32_t minted = 0;
  id<MTLTexture> tex = wire_server_lookup_texture((id<MTLDevice>)p->device, sid, gen, &minted);
  if (!tex)
    return STATUS_SUCCESS;                       /* lookup / wrap failed */

  /* The wrapped texture carries the IOSurface's true dimensions; a zero extent
   * would drive a zero/out-of-bounds present copy -> GPU fault. Never return a
   * zero-extent texture as usable (the PE guard double-checks). */
  uint32_t tw = (uint32_t)[tex width];
  uint32_t th = (uint32_t)[tex height];
  if (tw == 0 || th == 0)
    return STATUS_SUCCESS;

  p->out_texture      = (obj_handle_t)tex;
  p->out_frame_serial = fs;
  p->out_width        = tw;
  p->out_height       = th;
  p->out_format       = (uint32_t)MTLPixelFormatBGRA8Unorm;
  p->out_slot         = slot;
  p->out_minted       = minted;
  /* The SCALED present blit samples this texture through an MTL4 argument
   * table, which binds by MTLResourceID — a 0 id is legal-but-black (argument
   * tables are not validated). Ship the real id; the PE side refuses to
   * present a frame without one. */
  p->out_gpu_resource_id = [tex gpuResourceID]._impl;
  p->out_usable       = 1;
  return STATUS_SUCCESS;
}

/* --- autorelease-pool dispatch wrappers --------------------------------- *
 * Wine threads have no NSAutoreleasePool, so every autoreleased object a
 * Metal/CA call produces on this side would leak for the process lifetime
 * (objc "MISSING POOLS ... just leaking"). Each unix call therefore runs
 * inside its own pool. Returned handles survive the drain by convention:
 * creation calls return +1 references and property getters return objects
 * owned by longer-lived parents (drawable.texture, layer.residencySet).
 * NSObject_retain/release stay unwrapped: pure refcount ops, no autorelease,
 * and they are the hottest entries. */
#define D9MT_POOLED(fn) \
  static NTSTATUS fn##_pooled(void *obj) { \
    NTSTATUS status; \
    @autoreleasepool { status = fn(obj); } \
    return status; \
  }
D9MT_POOLED(_CGColorSpace_checkColorSpaceSupported)
D9MT_POOLED(_CacheReader_alloc_init)
D9MT_POOLED(_CacheReader_get)
D9MT_POOLED(_CacheWriter_alloc_init)
D9MT_POOLED(_CacheWriter_set)
D9MT_POOLED(_CreateMetalViewFromHWND)
D9MT_POOLED(_DeveloperHUDProperties_addLabel)
D9MT_POOLED(_DeveloperHUDProperties_instance)
D9MT_POOLED(_DeveloperHUDProperties_remove)
D9MT_POOLED(_DeveloperHUDProperties_updateLabel)
D9MT_POOLED(_DispatchData_alloc_init)
D9MT_POOLED(_MTL4ArgumentTable_setAddress)
D9MT_POOLED(_MTL4ArgumentTable_setAddressWithStride)
D9MT_POOLED(_MTL4ArgumentTable_setSamplerState)
D9MT_POOLED(_MTL4ArgumentTable_setTexture)
D9MT_POOLED(_MTL4CommandAllocator_reset)
D9MT_POOLED(_MTL4CommandBuffer_beginCommandBuffer)
D9MT_POOLED(_MTL4CommandBuffer_computeCommandEncoder)
D9MT_POOLED(_MTL4CommandBuffer_endCommandBuffer)
D9MT_POOLED(_MTL4CommandBuffer_renderCommandEncoder)
D9MT_POOLED(_MTL4CommandBuffer_useResidencySet)
D9MT_POOLED(_MTL4CommandEncoder_barrier)
D9MT_POOLED(_MTL4CommandQueue_addResidencySet)
D9MT_POOLED(_MTL4CommandQueue_commit)
D9MT_POOLED(_MTL4CommandQueue_removeResidencySet)
D9MT_POOLED(_MTL4CommandQueue_signalDrawable)
D9MT_POOLED(_MTL4CommandQueue_signalEvent)
D9MT_POOLED(_MTL4CommandQueue_waitForDrawable)
D9MT_POOLED(_MTL4CommandQueue_waitForEvent)
D9MT_POOLED(_MTL4Compiler_newRenderPipelineState)
D9MT_POOLED(_MTL4ComputeCommandEncoder_copyFromBufferToBuffer)
D9MT_POOLED(_MTL4ComputeCommandEncoder_copyFromBufferToTexture)
D9MT_POOLED(_MTL4ComputeCommandEncoder_copyFromTextureToBuffer)
D9MT_POOLED(_MTL4ComputeCommandEncoder_copyFromTextureToTexture)
D9MT_POOLED(_MTL4ComputeCommandEncoder_fillBuffer)
D9MT_POOLED(_MTL4ComputeCommandEncoder_generateMipmaps)
D9MT_POOLED(_MTL4RenderCommandEncoder_drawIndexedPrimitives)
D9MT_POOLED(_MTL4RenderCommandEncoder_drawPrimitives)
D9MT_POOLED(_MTL4RenderCommandEncoder_encodeCommands)
D9MT_POOLED(_MTL4RenderCommandEncoder_setArgumentTable)
D9MT_POOLED(_MTL4RenderCommandEncoder_setBlendColor)
D9MT_POOLED(_MTL4RenderCommandEncoder_setCullMode)
D9MT_POOLED(_MTL4RenderCommandEncoder_setDepthBias)
D9MT_POOLED(_MTL4RenderCommandEncoder_setDepthStencilState)
D9MT_POOLED(_MTL4RenderCommandEncoder_setFrontFacingWinding)
D9MT_POOLED(_MTL4RenderCommandEncoder_setRenderPipelineState)
D9MT_POOLED(_MTL4RenderCommandEncoder_setScissorRect)
D9MT_POOLED(_MTL4RenderCommandEncoder_setStencilReferenceValue)
D9MT_POOLED(_MTL4RenderCommandEncoder_setViewport)
D9MT_POOLED(_MTL4RenderCommandEncoder_setVisibilityResultMode)
D9MT_POOLED(_MTLBinaryArchive_serialize)
D9MT_POOLED(_MTLBlitCommandEncoder_encodeCommands)
D9MT_POOLED(_MTLBuffer_didModifyRange)
D9MT_POOLED(_MTLBuffer_newTexture)
D9MT_POOLED(_MTLBuffer_updateContents)
D9MT_POOLED(_MTLCaptureManager_sharedCaptureManager)
D9MT_POOLED(_MTLCaptureManager_startCapture)
D9MT_POOLED(_MTLCaptureManager_stopCapture)
D9MT_POOLED(_MTLCommandBuffer_blitCommandEncoder)
D9MT_POOLED(_MTLCommandBuffer_blitCommandEncoderWithSampleBuffers)
D9MT_POOLED(_MTLCommandBuffer_commit)
D9MT_POOLED(_MTLCommandBuffer_computeCommandEncoder)
D9MT_POOLED(_MTLCommandBuffer_encodeSignalEvent)
D9MT_POOLED(_MTLCommandBuffer_encodeSpatialScale)
D9MT_POOLED(_MTLCommandBuffer_encodeTemporalScale)
D9MT_POOLED(_MTLCommandBuffer_encodeWaitForEvent)
D9MT_POOLED(_MTLCommandBuffer_error)
D9MT_POOLED(_MTLCommandBuffer_logs)
D9MT_POOLED(_MTLCommandBuffer_presentDrawable)
D9MT_POOLED(_MTLCommandBuffer_presentDrawableAfterMinimumDuration)
D9MT_POOLED(_MTLCommandBuffer_property)
D9MT_POOLED(_MTLCommandBuffer_renderCommandEncoder)
D9MT_POOLED(_MTLCommandBuffer_status)
D9MT_POOLED(_MTLCommandBuffer_waitUntilCompleted)
D9MT_POOLED(_MTLCommandEncoder_endEncoding)
D9MT_POOLED(_MTLCommandEncoder_setLabel)
D9MT_POOLED(_MTLCommandQueue_commandBuffer)
D9MT_POOLED(_MTLComputeCommandEncoder_encodeCommands)
D9MT_POOLED(_MTLCopyAllDevices)
D9MT_POOLED(_MTLCounterSampleBuffer_newTimestampBuffer)
D9MT_POOLED(_MTLCounterSampleBuffer_resolveCounterRange)
D9MT_POOLED(_MTLDevice_currentAllocatedSize)
D9MT_POOLED(_MTLDevice_hasUnifiedMemory)
D9MT_POOLED(_MTLDevice_minimumLinearTextureAlignmentForPixelFormat)
D9MT_POOLED(_MTLDevice_name)
D9MT_POOLED(_MTLDevice_newArgumentTable)
D9MT_POOLED(_MTLDevice_newBinaryArchive)
D9MT_POOLED(_MTLDevice_newBuffer)
D9MT_POOLED(_MTLDevice_newCommandAllocator)
D9MT_POOLED(_MTLDevice_newCommandQueue)
D9MT_POOLED(_MTLDevice_newComputePipelineState)
D9MT_POOLED(_MTLDevice_newDepthStencilState)
D9MT_POOLED(_MTLDevice_newEvent)
D9MT_POOLED(_MTLDevice_newFence)
D9MT_POOLED(_MTLDevice_newLibrary)
D9MT_POOLED(_MTLDevice_newMTL4CommandBuffer)
D9MT_POOLED(_MTLDevice_newMTL4CommandQueue)
D9MT_POOLED(_MTLDevice_newMTL4Compiler)
D9MT_POOLED(_MTLDevice_newMeshRenderPipelineState)
D9MT_POOLED(_MTLDevice_newPlacementHeap)
D9MT_POOLED(_MTLDevice_newRenderPipelineState)
D9MT_POOLED(_MTLDevice_newRenderPipelineStateWithVertexDescriptor)
D9MT_POOLED(_MTLDevice_newResidencySet)
D9MT_POOLED(_MTLDevice_newSamplerState)
D9MT_POOLED(_MTLDevice_newSharedEvent)
D9MT_POOLED(_MTLDevice_newSharedEventWithMachPort)
D9MT_POOLED(_MTLDevice_newSharedTexture)
D9MT_POOLED(_MTLDevice_newSpatialScaler)
D9MT_POOLED(_MTLDevice_newTemporalScaler)
D9MT_POOLED(_MTLDevice_newTexture)
D9MT_POOLED(_MTLDevice_newTileRenderPipelineState)
D9MT_POOLED(_MTLDevice_recommendedMaxWorkingSetSize)
D9MT_POOLED(_MTLDevice_registryID)
D9MT_POOLED(_MTLDevice_setShouldMaximizeConcurrentCompilation)
D9MT_POOLED(_MTLDevice_sparseTileSize)
D9MT_POOLED(_MTLDevice_supportsBCTextureCompression)
D9MT_POOLED(_MTLDevice_supportsFXSpatialScaler)
D9MT_POOLED(_MTLDevice_supportsFXTemporalScaler)
D9MT_POOLED(_MTLDevice_newMTL4SpatialScaler)
D9MT_POOLED(_MTL4CommandBuffer_encodeSpatialScale)
/* wire transport: attach/stats pooled for consistency; kick and detach stay
 * raw (pure POSIX, no ObjC — see their comments). */
D9MT_POOLED(_WireRing_attach)
D9MT_POOLED(_WireRing_stats)
D9MT_POOLED(_WireServerFrame_acquire)
D9MT_POOLED(_MTLDevice_supportsFamily)
D9MT_POOLED(_MTLDevice_supportsPlacementSparse)
D9MT_POOLED(_MTLDevice_supportsTextureSampleCount)
D9MT_POOLED(_MTLDevice_updateSparseTextureMappings)
D9MT_POOLED(_MTLLibrary_newFunction)
D9MT_POOLED(_MTLLibrary_newFunctionWithConstants)
D9MT_POOLED(_MTLLogContainer_enumerate)
D9MT_POOLED(_MTLRenderCommandEncoder_encodeCommands)
D9MT_POOLED(_MTLResidencySet_addAllocation)
D9MT_POOLED(_MTLResidencySet_commit)
D9MT_POOLED(_MTLResidencySet_removeAllAllocations)
D9MT_POOLED(_MTLResidencySet_removeAllocation)
D9MT_POOLED(_MTLResidencySet_requestResidency)
D9MT_POOLED(_MTLSharedEvent_createMachPort)
D9MT_POOLED(_MTLSharedEvent_setWin32EventAtValue)
D9MT_POOLED(_MTLSharedEvent_signalValue)
D9MT_POOLED(_MTLSharedEvent_signaledValue)
D9MT_POOLED(_MTLSharedEvent_waitUntilSignaledValue)
D9MT_POOLED(_MTLTexture_arrayLength)
D9MT_POOLED(_MTLTexture_depth)
D9MT_POOLED(_MTLTexture_height)
D9MT_POOLED(_MTLTexture_mipmapLevelCount)
D9MT_POOLED(_MTLTexture_newTextureView)
D9MT_POOLED(_MTLTexture_pixelFormat)
D9MT_POOLED(_MTLTexture_replaceRegion)
D9MT_POOLED(_MTLTexture_usage)
D9MT_POOLED(_MTLTexture_width)
D9MT_POOLED(_MetalDrawable_present)
D9MT_POOLED(_MetalDrawable_texture)
D9MT_POOLED(_MetalLayer_getEDRValue)
D9MT_POOLED(_MetalLayer_getProps)
D9MT_POOLED(_MetalLayer_nextDrawable)
D9MT_POOLED(_MetalLayer_residencySet)
D9MT_POOLED(_MetalLayer_setColorSpace)
D9MT_POOLED(_MetalLayer_setProps)
D9MT_POOLED(_NSArray_count)
D9MT_POOLED(_NSArray_object)
D9MT_POOLED(_NSObject_description)
D9MT_POOLED(_NSString_alloc_init)
D9MT_POOLED(_NSString_getCString)
D9MT_POOLED(_NSString_lengthOfBytesUsingEncoding)
D9MT_POOLED(_NSString_string)
D9MT_POOLED(_ReleaseMetalView)
D9MT_POOLED(_SharedEventListener_create)
D9MT_POOLED(_SharedEventListener_destroy)
D9MT_POOLED(_SharedEventListener_start)
D9MT_POOLED(_WMTApitraceCommandBufferBegin)
D9MT_POOLED(_WMTApitraceCommandBufferCommit)
D9MT_POOLED(_WMTApitracePresentDrawable)
D9MT_POOLED(_WMTApitraceSessionClose)
D9MT_POOLED(_WMTApitraceSessionEnsureOpen)
D9MT_POOLED(_WMTApitraceSessionFlush)
D9MT_POOLED(_WMTApitraceSessionSealCheckpoint)
D9MT_POOLED(_WMTApitraceSetCurrentD3DSequence)
D9MT_POOLED(_WMTBootstrapLookUp)
D9MT_POOLED(_WMTBootstrapRegister)
D9MT_POOLED(_WMTGetDisplayDescription)
D9MT_POOLED(_WMTGetOSVersion)
D9MT_POOLED(_WMTGetPrimaryDisplayId)
D9MT_POOLED(_WMTGetSecondaryDisplayId)
D9MT_POOLED(_WMTQueryDisplaySetting)
D9MT_POOLED(_WMTQueryDisplaySettingForLayer)
D9MT_POOLED(_WMTSetMetalShaderCachePath)
D9MT_POOLED(_WMTUpdateDisplaySetting)
D9MT_POOLED(thunk32_AirconvUnsupportedCompile)
D9MT_POOLED(thunk32_AirconvUnsupportedGetArgumentsInfo)
D9MT_POOLED(thunk32_AirconvUnsupportedInitialize)
D9MT_POOLED(thunk32_DXSOCompile)
D9MT_POOLED(thunk32_DXSOGetCompiledBitcode)
D9MT_POOLED(thunk32_DXSOInitialize)
D9MT_POOLED(thunk32_SM50Compile)
D9MT_POOLED(thunk32_SM50CompileGeometryPipelineGeometry)
D9MT_POOLED(thunk32_SM50CompileGeometryPipelineVertex)
D9MT_POOLED(thunk32_SM50CompileTessellationPipelineDomain)
D9MT_POOLED(thunk32_SM50CompileTessellationPipelineHull)
D9MT_POOLED(thunk32_SM50GetArgumentsInfo)
D9MT_POOLED(thunk32_SM50GetCompiledBitcode)
D9MT_POOLED(thunk32_SM50GetErrorMessage)
D9MT_POOLED(thunk32_SM50Initialize)
D9MT_POOLED(thunk_AirconvUnsupportedCompile)
D9MT_POOLED(thunk_AirconvUnsupportedDestroy)
D9MT_POOLED(thunk_AirconvUnsupportedGetArgumentsInfo)
D9MT_POOLED(thunk_AirconvUnsupportedInitialize)
D9MT_POOLED(thunk_D9MTAirCompile)
D9MT_POOLED(thunk_D9MTAirFree)
D9MT_POOLED(thunk_D9MTAirVersion)
D9MT_POOLED(thunk_DXSOCompile)
D9MT_POOLED(thunk_DXSODestroy)
D9MT_POOLED(thunk_DXSODestroyBitcode)
D9MT_POOLED(thunk_DXSOGetCompiledBitcode)
D9MT_POOLED(thunk_DXSOInitialize)
D9MT_POOLED(thunk_SM50Compile)
D9MT_POOLED(thunk_SM50CompileGeometryPipelineGeometry)
D9MT_POOLED(thunk_SM50CompileGeometryPipelineVertex)
D9MT_POOLED(thunk_SM50CompileTessellationPipelineDomain)
D9MT_POOLED(thunk_SM50CompileTessellationPipelineHull)
D9MT_POOLED(thunk_SM50Destroy)
D9MT_POOLED(thunk_SM50DestroyBitcode)
D9MT_POOLED(thunk_SM50FreeError)
D9MT_POOLED(thunk_SM50GetArgumentsInfo)
D9MT_POOLED(thunk_SM50GetCompiledBitcode)
D9MT_POOLED(thunk_SM50GetErrorMessage)
D9MT_POOLED(thunk_SM50Initialize)
#undef D9MT_POOLED

const void *__wine_unix_call_funcs[] = {
    &_NSObject_retain,
    &_NSObject_release,
    &_NSArray_object_pooled,
    &_NSArray_count_pooled,
    &_MTLCopyAllDevices_pooled,
    &_MTLDevice_recommendedMaxWorkingSetSize_pooled,
    &_MTLDevice_currentAllocatedSize_pooled,
    &_MTLDevice_name_pooled,
    &_NSString_getCString_pooled,
    &_MTLDevice_newCommandQueue_pooled,
    &_NSAutoreleasePool_alloc_init,
    &_MTLCommandQueue_commandBuffer_pooled,
    &_MTLCommandBuffer_commit_pooled,
    &_MTLCommandBuffer_waitUntilCompleted_pooled,
    &_MTLCommandBuffer_status_pooled,
    &_MTLDevice_newSharedEvent_pooled,
    &_MTLSharedEvent_signaledValue_pooled,
    &_MTLCommandBuffer_encodeSignalEvent_pooled,
    &_MTLDevice_newBuffer_pooled,
    &_MTLDevice_newSamplerState_pooled,
    &_MTLDevice_newDepthStencilState_pooled,
    &_MTLDevice_newTexture_pooled,
    &_MTLBuffer_newTexture_pooled,
    &_MTLTexture_newTextureView_pooled,
    &_MTLDevice_minimumLinearTextureAlignmentForPixelFormat_pooled,
    &_MTLDevice_newLibrary_pooled,
    &_MTLLibrary_newFunction_pooled,
    &_NSString_lengthOfBytesUsingEncoding_pooled,
    &_NSObject_description_pooled,
    &_MTLDevice_newComputePipelineState_pooled,
    &_MTLCommandBuffer_blitCommandEncoder_pooled,
    &_MTLCommandBuffer_computeCommandEncoder_pooled,
    &_MTLCommandBuffer_renderCommandEncoder_pooled,
    &_MTLCommandEncoder_endEncoding_pooled,
    &_MTLDevice_newRenderPipelineState_pooled,
    &_MTLDevice_newMeshRenderPipelineState_pooled,
    &_MTLBlitCommandEncoder_encodeCommands_pooled,
    &_MTLComputeCommandEncoder_encodeCommands_pooled,
    &_MTLRenderCommandEncoder_encodeCommands_pooled,
    &_MTLTexture_pixelFormat_pooled,
    &_MTLTexture_width_pooled,
    &_MTLTexture_height_pooled,
    &_MTLTexture_depth_pooled,
    &_MTLTexture_arrayLength_pooled,
    &_MTLTexture_mipmapLevelCount_pooled,
    &_MTLTexture_replaceRegion_pooled,
    &_MTLBuffer_didModifyRange_pooled,
    &_MTLCommandBuffer_presentDrawable_pooled,
    &_MTLCommandBuffer_presentDrawableAfterMinimumDuration_pooled,
    &_MTLDevice_supportsFamily_pooled,
    &_MTLDevice_supportsBCTextureCompression_pooled,
    &_MTLDevice_supportsTextureSampleCount_pooled,
    &_MTLDevice_hasUnifiedMemory_pooled,
    &_MTLCaptureManager_sharedCaptureManager_pooled,
    &_MTLCaptureManager_startCapture_pooled,
    &_MTLCaptureManager_stopCapture_pooled,
    &_MTLDevice_newTemporalScaler_pooled,
    &_MTLDevice_newSpatialScaler_pooled,
    &_MTLCommandBuffer_encodeTemporalScale_pooled,
    &_MTLCommandBuffer_encodeSpatialScale_pooled,
    &_NSString_string_pooled,
    &_NSString_alloc_init_pooled,
    &_DeveloperHUDProperties_instance_pooled,
    &_DeveloperHUDProperties_addLabel_pooled,
    &_DeveloperHUDProperties_updateLabel_pooled,
    &_DeveloperHUDProperties_remove_pooled,
    &_MetalDrawable_texture_pooled,
    &_MetalLayer_nextDrawable_pooled,
    &_MTLDevice_supportsFXSpatialScaler_pooled,
    &_MTLDevice_supportsFXTemporalScaler_pooled,
    &_MetalLayer_setProps_pooled,
    &_MetalLayer_getProps_pooled,
    &_CreateMetalViewFromHWND_pooled,
    &_ReleaseMetalView_pooled,
    &thunk_SM50Initialize_pooled,
    &thunk_SM50Destroy_pooled,
    &thunk_SM50Compile_pooled,
    &thunk_SM50GetCompiledBitcode_pooled,
    &thunk_SM50DestroyBitcode_pooled,
    &thunk_SM50GetErrorMessage_pooled,
    &thunk_SM50FreeError_pooled,
    &thunk_SM50CompileGeometryPipelineVertex_pooled,
    &thunk_SM50CompileGeometryPipelineGeometry_pooled,
    &_MTLTexture_usage_pooled,
    &thunk_SM50CompileTessellationPipelineHull_pooled,
    &thunk_SM50CompileTessellationPipelineDomain_pooled,
    &_MTLCommandEncoder_setLabel_pooled,
    &_MTLDevice_setShouldMaximizeConcurrentCompilation_pooled,
    &thunk_SM50GetArgumentsInfo_pooled,
    &thunk_AirconvUnsupportedInitialize_pooled,
    &thunk_AirconvUnsupportedDestroy_pooled,
    &thunk_AirconvUnsupportedCompile_pooled,
    &thunk_AirconvUnsupportedGetArgumentsInfo_pooled,
    &_MTLCommandBuffer_error_pooled,
    &_MTLCommandBuffer_logs_pooled,
    &_MTLLogContainer_enumerate_pooled,
    &_CGColorSpace_checkColorSpaceSupported_pooled,
    &_MetalLayer_setColorSpace_pooled,
    &_WMTGetPrimaryDisplayId_pooled,
    &_WMTGetSecondaryDisplayId_pooled,
    &_WMTGetDisplayDescription_pooled,
    &_MetalLayer_getEDRValue_pooled,
    &_MTLLibrary_newFunctionWithConstants_pooled,
    &_WMTQueryDisplaySetting_pooled,
    &_WMTUpdateDisplaySetting_pooled,
    &_WMTQueryDisplaySettingForLayer_pooled,
    &_MTLCommandBuffer_encodeWaitForEvent_pooled,
    &_MTLSharedEvent_signalValue_pooled,
    &_MTLSharedEvent_setWin32EventAtValue_pooled,
    &_MTLDevice_newFence_pooled,
    &_MTLDevice_newEvent_pooled,
    &_MTLBuffer_updateContents_pooled,
    &_SharedEventListener_create_pooled,
    &_SharedEventListener_start_pooled,
    &_SharedEventListener_destroy_pooled,
    &_WMTGetOSVersion_pooled,
    &_MTLDevice_newBinaryArchive_pooled,
    &_MTLBinaryArchive_serialize_pooled,
    &_DispatchData_alloc_init_pooled,
    &_CacheReader_alloc_init_pooled,
    &_CacheReader_get_pooled,
    &_CacheWriter_alloc_init_pooled,
    &_CacheWriter_set_pooled,
    &_WMTSetMetalShaderCachePath_pooled,
    &_MTLDevice_newSharedTexture_pooled,
    &_WMTBootstrapRegister_pooled,
    &_WMTBootstrapLookUp_pooled,
    &_MTLSharedEvent_createMachPort_pooled,
    &_MTLDevice_newSharedEventWithMachPort_pooled,
    &_MTLDevice_registryID_pooled,
    &_MTLSharedEvent_waitUntilSignaledValue_pooled,
    &_MTLCounterSampleBuffer_newTimestampBuffer_pooled,
    &_MTLCounterSampleBuffer_resolveCounterRange_pooled,
    &_MTLCommandBuffer_blitCommandEncoderWithSampleBuffers_pooled,
    &_MTLCommandBuffer_property_pooled,
    &_MTLDevice_newTileRenderPipelineState_pooled,
    // Upstream apitrace block: indices 136..142 (UNIX_CALL hard-codes
    // these in winemetal_thunks.c WMTApitrace*).
    &_WMTApitraceSessionEnsureOpen_pooled,
    &_WMTApitraceSessionClose_pooled,
    &_WMTApitraceSetCurrentD3DSequence_pooled,
    &_WMTApitraceCommandBufferBegin_pooled,
    &_WMTApitraceCommandBufferCommit_pooled,
    &_WMTApitracePresentDrawable_pooled,
    &_WMTApitraceSessionSealCheckpoint_pooled,
    &_MTLDevice_supportsPlacementSparse_pooled,
    &_MTLDevice_sparseTileSize_pooled,
    &_MTLDevice_newPlacementHeap_pooled,
    &_MTLDevice_updateSparseTextureMappings_pooled,
    &_WMTApitraceSessionFlush_pooled,
    // DXSO block: indices 148..152, must match unix_dxso_* in
    // airconv_thunks.h. It sits immediately after upstream's apitrace +
    // sparse entries; if upstream inserts more above this line, move both
    // this block and the airconv_thunks.h anchor forward in lock-step.
    &thunk_DXSOInitialize_pooled,
    &thunk_DXSODestroy_pooled,
    &thunk_DXSOCompile_pooled,
    &thunk_DXSOGetCompiledBitcode_pooled,
    &thunk_DXSODestroyBitcode_pooled,
    /* d9mt additions: codes 153..156, lock-step in both tables.
       All param layouts are 8-byte/arch-invariant: shared thunks. */
    &_MTLDevice_newRenderPipelineStateWithVertexDescriptor_pooled,
    &thunk_D9MTAirVersion_pooled,
    &thunk_D9MTAirCompile_pooled,
    &thunk_D9MTAirFree_pooled,
    /* d9mt additions: full Metal 4 surface, codes 157..200, lock-step in both
       tables. All param layouts are 8-byte/arch-invariant: shared thunks. */
    &_MTLDevice_newMTL4CommandQueue_pooled,                        /* 157 */
    &_MTLDevice_newCommandAllocator_pooled,                        /* 158 */
    &_MTLDevice_newMTL4CommandBuffer_pooled,                       /* 159 */
    &_MTLDevice_newArgumentTable_pooled,                           /* 160 */
    &_MTLDevice_newMTL4Compiler_pooled,                            /* 161 */
    &_MTLDevice_newResidencySet_pooled,                            /* 162 */
    &_MTL4CommandAllocator_reset_pooled,                           /* 163 */
    &_MTL4CommandBuffer_beginCommandBuffer_pooled,                 /* 164 */
    &_MTL4CommandBuffer_endCommandBuffer_pooled,                   /* 165 */
    &_MTL4CommandBuffer_useResidencySet_pooled,                    /* 166 */
    &_MTL4CommandQueue_commit_pooled,                              /* 167 */
    &_MTL4CommandQueue_waitForDrawable_pooled,                     /* 168 */
    &_MTL4CommandQueue_signalDrawable_pooled,                      /* 169 */
    &_MTL4CommandQueue_signalEvent_pooled,                         /* 170 */
    &_MTL4CommandQueue_waitForEvent_pooled,                        /* 171 */
    &_MTL4ArgumentTable_setAddress_pooled,                         /* 172 */
    &_MTL4ArgumentTable_setAddressWithStride_pooled,               /* 173 */
    &_MTL4ArgumentTable_setTexture_pooled,                         /* 174 */
    &_MTL4ArgumentTable_setSamplerState_pooled,                    /* 175 */
    &_MTL4CommandBuffer_renderCommandEncoder_pooled,               /* 176 */
    &_MTL4RenderCommandEncoder_setArgumentTable_pooled,            /* 177 */
    &_MTL4RenderCommandEncoder_setRenderPipelineState_pooled,      /* 178 */
    &_MTL4RenderCommandEncoder_setViewport_pooled,                 /* 179 */
    &_MTL4RenderCommandEncoder_setScissorRect_pooled,              /* 180 */
    &_MTL4RenderCommandEncoder_setDepthStencilState_pooled,        /* 181 */
    &_MTL4RenderCommandEncoder_setCullMode_pooled,                 /* 182 */
    &_MTL4RenderCommandEncoder_setFrontFacingWinding_pooled,       /* 183 */
    &_MTL4RenderCommandEncoder_setDepthBias_pooled,                /* 184 */
    &_MTL4RenderCommandEncoder_setBlendColor_pooled,               /* 185 */
    &_MTL4RenderCommandEncoder_setStencilReferenceValue_pooled,    /* 186 */
    &_MTL4RenderCommandEncoder_drawPrimitives_pooled,              /* 187 */
    &_MTL4RenderCommandEncoder_drawIndexedPrimitives_pooled,       /* 188 */
    &_MTL4CommandBuffer_computeCommandEncoder_pooled,              /* 189 */
    &_MTL4ComputeCommandEncoder_copyFromBufferToBuffer_pooled,     /* 190 */
    &_MTL4ComputeCommandEncoder_copyFromBufferToTexture_pooled,    /* 191 */
    &_MTL4ComputeCommandEncoder_copyFromTextureToTexture_pooled,   /* 192 */
    &_MTL4ComputeCommandEncoder_copyFromTextureToBuffer_pooled,    /* 193 */
    &_MTL4ComputeCommandEncoder_fillBuffer_pooled,                 /* 194 */
    &_MTL4ComputeCommandEncoder_generateMipmaps_pooled,            /* 195 */
    &_MTL4Compiler_newRenderPipelineState_pooled,                  /* 196 */
    &_MTLResidencySet_addAllocation_pooled,                        /* 197 */
    &_MTLResidencySet_commit_pooled,                               /* 198 */
    &_MTLResidencySet_requestResidency_pooled,                     /* 199 */
    &_MTLResidencySet_removeAllAllocations_pooled,                 /* 200 */
    &_MetalDrawable_present_pooled,                                /* 201 */
    &_MTL4RenderCommandEncoder_setVisibilityResultMode_pooled,     /* 202 */
    &_MTL4CommandEncoder_barrier_pooled,                           /* 203 */
    &_MTLResidencySet_removeAllocation_pooled,                     /* 204 */
    &_MetalLayer_residencySet_pooled,                              /* 205 */
    &_MTL4CommandQueue_addResidencySet_pooled,                     /* 206 */
    &_MTL4CommandQueue_removeResidencySet_pooled,                  /* 207 */
    &_MTL4RenderCommandEncoder_encodeCommands_pooled,              /* 208 */
    &_MTLDevice_newMTL4SpatialScaler_pooled,                       /* 209 */
    &_MTL4CommandBuffer_encodeSpatialScale_pooled,                 /* 210 */
    /* d9mt wire transport, lock-step in both tables */
    &_WireRing_attach_pooled,                                      /* 211 */
    &_WireRing_kick,                                               /* 212 */
    &_WireRing_stats_pooled,                                       /* 213 */
    &_WireRing_detach,                                             /* 214 */
    &_WireRing_quiesce,                                            /* 215 */
    &_WireServerFrame_acquire_pooled,                              /* 216 */

};

#ifndef DXMT_NATIVE
const void *__wine_unix_call_wow64_funcs[] = {
    &_NSObject_retain,
    &_NSObject_release,
    &_NSArray_object_pooled,
    &_NSArray_count_pooled,
    &_MTLCopyAllDevices_pooled,
    &_MTLDevice_recommendedMaxWorkingSetSize_pooled,
    &_MTLDevice_currentAllocatedSize_pooled,
    &_MTLDevice_name_pooled,
    &_NSString_getCString_pooled,
    &_MTLDevice_newCommandQueue_pooled,
    &_NSAutoreleasePool_alloc_init,
    &_MTLCommandQueue_commandBuffer_pooled,
    &_MTLCommandBuffer_commit_pooled,
    &_MTLCommandBuffer_waitUntilCompleted_pooled,
    &_MTLCommandBuffer_status_pooled,
    &_MTLDevice_newSharedEvent_pooled,
    &_MTLSharedEvent_signaledValue_pooled,
    &_MTLCommandBuffer_encodeSignalEvent_pooled,
    &_MTLDevice_newBuffer_pooled,
    &_MTLDevice_newSamplerState_pooled,
    &_MTLDevice_newDepthStencilState_pooled,
    &_MTLDevice_newTexture_pooled,
    &_MTLBuffer_newTexture_pooled,
    &_MTLTexture_newTextureView_pooled,
    &_MTLDevice_minimumLinearTextureAlignmentForPixelFormat_pooled,
    &_MTLDevice_newLibrary_pooled,
    &_MTLLibrary_newFunction_pooled,
    &_NSString_lengthOfBytesUsingEncoding_pooled,
    &_NSObject_description_pooled,
    &_MTLDevice_newComputePipelineState_pooled,
    &_MTLCommandBuffer_blitCommandEncoder_pooled,
    &_MTLCommandBuffer_computeCommandEncoder_pooled,
    &_MTLCommandBuffer_renderCommandEncoder_pooled,
    &_MTLCommandEncoder_endEncoding_pooled,
    &_MTLDevice_newRenderPipelineState_pooled,
    &_MTLDevice_newMeshRenderPipelineState_pooled,
    &_MTLBlitCommandEncoder_encodeCommands_pooled,
    &_MTLComputeCommandEncoder_encodeCommands_pooled,
    &_MTLRenderCommandEncoder_encodeCommands_pooled,
    &_MTLTexture_pixelFormat_pooled,
    &_MTLTexture_width_pooled,
    &_MTLTexture_height_pooled,
    &_MTLTexture_depth_pooled,
    &_MTLTexture_arrayLength_pooled,
    &_MTLTexture_mipmapLevelCount_pooled,
    &_MTLTexture_replaceRegion_pooled,
    &_MTLBuffer_didModifyRange_pooled,
    &_MTLCommandBuffer_presentDrawable_pooled,
    &_MTLCommandBuffer_presentDrawableAfterMinimumDuration_pooled,
    &_MTLDevice_supportsFamily_pooled,
    &_MTLDevice_supportsBCTextureCompression_pooled,
    &_MTLDevice_supportsTextureSampleCount_pooled,
    &_MTLDevice_hasUnifiedMemory_pooled,
    &_MTLCaptureManager_sharedCaptureManager_pooled,
    &_MTLCaptureManager_startCapture_pooled,
    &_MTLCaptureManager_stopCapture_pooled,
    &_MTLDevice_newTemporalScaler_pooled,
    &_MTLDevice_newSpatialScaler_pooled,
    &_MTLCommandBuffer_encodeTemporalScale_pooled,
    &_MTLCommandBuffer_encodeSpatialScale_pooled,
    &_NSString_string_pooled,
    &_NSString_alloc_init_pooled,
    &_DeveloperHUDProperties_instance_pooled,
    &_DeveloperHUDProperties_addLabel_pooled,
    &_DeveloperHUDProperties_updateLabel_pooled,
    &_DeveloperHUDProperties_remove_pooled,
    &_MetalDrawable_texture_pooled,
    &_MetalLayer_nextDrawable_pooled,
    &_MTLDevice_supportsFXSpatialScaler_pooled,
    &_MTLDevice_supportsFXTemporalScaler_pooled,
    &_MetalLayer_setProps_pooled,
    &_MetalLayer_getProps_pooled,
    &_CreateMetalViewFromHWND_pooled,
    &_ReleaseMetalView_pooled,
    &thunk32_SM50Initialize_pooled,
    &thunk_SM50Destroy_pooled,
    &thunk32_SM50Compile_pooled,
    &thunk32_SM50GetCompiledBitcode_pooled,
    &thunk_SM50DestroyBitcode_pooled,
    &thunk32_SM50GetErrorMessage_pooled,
    &thunk_SM50FreeError_pooled,
    &thunk32_SM50CompileGeometryPipelineVertex_pooled,
    &thunk32_SM50CompileGeometryPipelineGeometry_pooled,
    &_MTLTexture_usage_pooled,
    &thunk32_SM50CompileTessellationPipelineHull_pooled,
    &thunk32_SM50CompileTessellationPipelineDomain_pooled,
    &_MTLCommandEncoder_setLabel_pooled,
    &_MTLDevice_setShouldMaximizeConcurrentCompilation_pooled,
    &thunk32_SM50GetArgumentsInfo_pooled,
    &thunk32_AirconvUnsupportedInitialize_pooled,
    &thunk_AirconvUnsupportedDestroy_pooled,
    &thunk32_AirconvUnsupportedCompile_pooled,
    &thunk32_AirconvUnsupportedGetArgumentsInfo_pooled,
    &_MTLCommandBuffer_error_pooled,
    &_MTLCommandBuffer_logs_pooled,
    &_MTLLogContainer_enumerate_pooled,
    &_CGColorSpace_checkColorSpaceSupported_pooled,
    &_MetalLayer_setColorSpace_pooled,
    &_WMTGetPrimaryDisplayId_pooled,
    &_WMTGetSecondaryDisplayId_pooled,
    &_WMTGetDisplayDescription_pooled,
    &_MetalLayer_getEDRValue_pooled,
    &_MTLLibrary_newFunctionWithConstants_pooled,
    &_WMTQueryDisplaySetting_pooled,
    &_WMTUpdateDisplaySetting_pooled,
    &_WMTQueryDisplaySettingForLayer_pooled,
    &_MTLCommandBuffer_encodeWaitForEvent_pooled,
    &_MTLSharedEvent_signalValue_pooled,
    &_MTLSharedEvent_setWin32EventAtValue_pooled,
    &_MTLDevice_newFence_pooled,
    &_MTLDevice_newEvent_pooled,
    &_MTLBuffer_updateContents_pooled,
    &_SharedEventListener_create_pooled,
    &_SharedEventListener_start_pooled,
    &_SharedEventListener_destroy_pooled,
    &_WMTGetOSVersion_pooled,
    &_MTLDevice_newBinaryArchive_pooled,
    &_MTLBinaryArchive_serialize_pooled,
    &_DispatchData_alloc_init_pooled,
    &_CacheReader_alloc_init_pooled,
    &_CacheReader_get_pooled,
    &_CacheWriter_alloc_init_pooled,
    &_CacheWriter_set_pooled,
    &_WMTSetMetalShaderCachePath_pooled,
    &_MTLDevice_newSharedTexture_pooled,
    &_WMTBootstrapRegister_pooled,
    &_WMTBootstrapLookUp_pooled,
    &_MTLSharedEvent_createMachPort_pooled,
    &_MTLDevice_newSharedEventWithMachPort_pooled,
    &_MTLDevice_registryID_pooled,
    &_MTLSharedEvent_waitUntilSignaledValue_pooled,
    &_MTLCounterSampleBuffer_newTimestampBuffer_pooled,
    &_MTLCounterSampleBuffer_resolveCounterRange_pooled,
    &_MTLCommandBuffer_blitCommandEncoderWithSampleBuffers_pooled,
    &_MTLCommandBuffer_property_pooled,
    &_MTLDevice_newTileRenderPipelineState_pooled,
    // Upstream apitrace block: indices 136..142, lock-step with the
    // 64-bit table above.
    &_WMTApitraceSessionEnsureOpen_pooled,
    &_WMTApitraceSessionClose_pooled,
    &_WMTApitraceSetCurrentD3DSequence_pooled,
    &_WMTApitraceCommandBufferBegin_pooled,
    &_WMTApitraceCommandBufferCommit_pooled,
    &_WMTApitracePresentDrawable_pooled,
    &_WMTApitraceSessionSealCheckpoint_pooled,
    &_MTLDevice_supportsPlacementSparse_pooled,
    &_MTLDevice_sparseTileSize_pooled,
    &_MTLDevice_newPlacementHeap_pooled,
    &_MTLDevice_updateSparseTextureMappings_pooled,
    &_WMTApitraceSessionFlush_pooled,
    // DXSO block: indices 148..152, lock-step with __wine_unix_call_funcs
    // above and with unix_dxso_* in airconv_thunks.h. Sits immediately
    // after the apitrace + sparse entries (136..147).
    &thunk32_DXSOInitialize_pooled,
    /* dxso_destroy_params holds only sm50_ptr64_t (always 8 bytes), so
       the 64-bit thunk is byte-compatible: no thunk32 needed. */
    &thunk_DXSODestroy_pooled,
    &thunk32_DXSOCompile_pooled,
    &thunk32_DXSOGetCompiledBitcode_pooled,
    /* dxso_destroy_bitcode_params is sm50_ptr64_t-only, see DXSODestroy
       comment above. */
    &thunk_DXSODestroyBitcode_pooled,
    /* d9mt additions: codes 153..156, lock-step in both tables.
       All param layouts are 8-byte/arch-invariant: shared thunks. */
    &_MTLDevice_newRenderPipelineStateWithVertexDescriptor_pooled,
    &thunk_D9MTAirVersion_pooled,
    &thunk_D9MTAirCompile_pooled,
    &thunk_D9MTAirFree_pooled,
    /* d9mt additions: full Metal 4 surface, codes 157..200, lock-step with
       __wine_unix_call_funcs above. Params are 8-byte/arch-invariant, so the
       64-bit thunks are byte-compatible for wow64: shared, no thunk32 twins. */
    &_MTLDevice_newMTL4CommandQueue_pooled,                        /* 157 */
    &_MTLDevice_newCommandAllocator_pooled,                        /* 158 */
    &_MTLDevice_newMTL4CommandBuffer_pooled,                       /* 159 */
    &_MTLDevice_newArgumentTable_pooled,                           /* 160 */
    &_MTLDevice_newMTL4Compiler_pooled,                            /* 161 */
    &_MTLDevice_newResidencySet_pooled,                            /* 162 */
    &_MTL4CommandAllocator_reset_pooled,                           /* 163 */
    &_MTL4CommandBuffer_beginCommandBuffer_pooled,                 /* 164 */
    &_MTL4CommandBuffer_endCommandBuffer_pooled,                   /* 165 */
    &_MTL4CommandBuffer_useResidencySet_pooled,                    /* 166 */
    &_MTL4CommandQueue_commit_pooled,                              /* 167 */
    &_MTL4CommandQueue_waitForDrawable_pooled,                     /* 168 */
    &_MTL4CommandQueue_signalDrawable_pooled,                      /* 169 */
    &_MTL4CommandQueue_signalEvent_pooled,                         /* 170 */
    &_MTL4CommandQueue_waitForEvent_pooled,                        /* 171 */
    &_MTL4ArgumentTable_setAddress_pooled,                         /* 172 */
    &_MTL4ArgumentTable_setAddressWithStride_pooled,               /* 173 */
    &_MTL4ArgumentTable_setTexture_pooled,                         /* 174 */
    &_MTL4ArgumentTable_setSamplerState_pooled,                    /* 175 */
    &_MTL4CommandBuffer_renderCommandEncoder_pooled,               /* 176 */
    &_MTL4RenderCommandEncoder_setArgumentTable_pooled,            /* 177 */
    &_MTL4RenderCommandEncoder_setRenderPipelineState_pooled,      /* 178 */
    &_MTL4RenderCommandEncoder_setViewport_pooled,                 /* 179 */
    &_MTL4RenderCommandEncoder_setScissorRect_pooled,              /* 180 */
    &_MTL4RenderCommandEncoder_setDepthStencilState_pooled,        /* 181 */
    &_MTL4RenderCommandEncoder_setCullMode_pooled,                 /* 182 */
    &_MTL4RenderCommandEncoder_setFrontFacingWinding_pooled,       /* 183 */
    &_MTL4RenderCommandEncoder_setDepthBias_pooled,                /* 184 */
    &_MTL4RenderCommandEncoder_setBlendColor_pooled,               /* 185 */
    &_MTL4RenderCommandEncoder_setStencilReferenceValue_pooled,    /* 186 */
    &_MTL4RenderCommandEncoder_drawPrimitives_pooled,              /* 187 */
    &_MTL4RenderCommandEncoder_drawIndexedPrimitives_pooled,       /* 188 */
    &_MTL4CommandBuffer_computeCommandEncoder_pooled,              /* 189 */
    &_MTL4ComputeCommandEncoder_copyFromBufferToBuffer_pooled,     /* 190 */
    &_MTL4ComputeCommandEncoder_copyFromBufferToTexture_pooled,    /* 191 */
    &_MTL4ComputeCommandEncoder_copyFromTextureToTexture_pooled,   /* 192 */
    &_MTL4ComputeCommandEncoder_copyFromTextureToBuffer_pooled,    /* 193 */
    &_MTL4ComputeCommandEncoder_fillBuffer_pooled,                 /* 194 */
    &_MTL4ComputeCommandEncoder_generateMipmaps_pooled,            /* 195 */
    &_MTL4Compiler_newRenderPipelineState_pooled,                  /* 196 */
    &_MTLResidencySet_addAllocation_pooled,                        /* 197 */
    &_MTLResidencySet_commit_pooled,                               /* 198 */
    &_MTLResidencySet_requestResidency_pooled,                     /* 199 */
    &_MTLResidencySet_removeAllAllocations_pooled,                 /* 200 */
    &_MetalDrawable_present_pooled,                                /* 201 */
    &_MTL4RenderCommandEncoder_setVisibilityResultMode_pooled,     /* 202 */
    &_MTL4CommandEncoder_barrier_pooled,                           /* 203 */
    &_MTLResidencySet_removeAllocation_pooled,                     /* 204 */
    &_MetalLayer_residencySet_pooled,                              /* 205 */
    &_MTL4CommandQueue_addResidencySet_pooled,                     /* 206 */
    &_MTL4CommandQueue_removeResidencySet_pooled,                  /* 207 */
    &_MTL4RenderCommandEncoder_encodeCommands_pooled,              /* 208 */
    &_MTLDevice_newMTL4SpatialScaler_pooled,                       /* 209 */
    &_MTL4CommandBuffer_encodeSpatialScale_pooled,                 /* 210 */
    /* d9mt wire transport, lock-step in both tables */
    &_WireRing_attach_pooled,                                      /* 211 */
    &_WireRing_kick,                                               /* 212 */
    &_WireRing_stats_pooled,                                       /* 213 */
    &_WireRing_detach,                                             /* 214 */
    &_WireRing_quiesce,                                            /* 215 */
    &_WireServerFrame_acquire_pooled,                              /* 216 */

};
#endif
