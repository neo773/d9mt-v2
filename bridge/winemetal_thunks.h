
#ifndef __WINEMETAL_THUNKS_H
#define __WINEMETAL_THUNKS_H

#include "winemetal.h"

#pragma pack(push, 8)

struct unixcall_generic_obj_ret {
  obj_handle_t ret;
};

struct unixcall_generic_obj_noret {
  obj_handle_t handle;
};

struct unixcall_generic_obj_obj_ret {
  obj_handle_t handle;
  obj_handle_t ret;
};

struct unixcall_generic_obj_uint64_obj_ret {
  obj_handle_t handle;
  uint64_t arg;
  obj_handle_t ret;
};

struct unixcall_generic_obj_uint64_ret {
  obj_handle_t handle;
  uint64_t ret;
};

struct unixcall_generic_obj_uint64_noret {
  obj_handle_t handle;
  uint64_t arg;
};

struct unixcall_apitrace_command_buffer_begin {
  obj_handle_t command_buffer;
  uint64_t frame_id;
};

struct unixcall_apitrace_present {
  obj_handle_t command_buffer;
  obj_handle_t drawable;
  uint64_t frame_index;
  uint32_t sync_interval;
  uint32_t flags;
};

struct unixcall_generic_obj_ptr_noret {
  obj_handle_t handle;
  struct WMTMemoryPointer arg;
};

struct unixcall_generic_obj_constptr_noret {
  obj_handle_t handle;
  struct WMTConstMemoryPointer arg;
};

struct unixcall_generic_obj_obj_uint64_noret {
  obj_handle_t handle;
  obj_handle_t arg0;
  uint64_t arg1;
};

struct unixcall_generic_obj_uint64_uint64_ret {
  obj_handle_t handle;
  uint64_t arg;
  uint64_t ret;
};

struct unixcall_nsstring_getcstring {
  obj_handle_t str;
  uint64_t buffer_ptr;
  uint64_t max_length;
  enum WMTStringEncoding encoding;
  uint32_t ret;
};

struct unixcall_mtldevice_newbuffer {
  obj_handle_t device;
  struct WMTMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtldevice_newsamplerstate {
  obj_handle_t device;
  struct WMTMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtldevice_newdepthstencilstate {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtldevice_newtexture {
  obj_handle_t device;
  struct WMTMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtldevice_sparsetilesize {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  struct WMTMemoryPointer tile_size;
  uint32_t ret;
};

struct unixcall_mtldevice_newplacementheap {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtldevice_updatesparsetexturemappings {
  obj_handle_t device;
  obj_handle_t texture;
  obj_handle_t heap;
  struct WMTConstMemoryPointer operations;
  uint64_t operation_count;
  uint32_t ret;
};

struct unixcall_mtlbuffer_newtexture {
  obj_handle_t buffer;
  struct WMTMemoryPointer info;
  uint64_t offset;
  uint64_t bytes_per_row;
  obj_handle_t ret;
};

struct unixcall_mtltexture_newtextureview {
  obj_handle_t texture;
  enum WMTPixelFormat format;
  enum WMTTextureType texture_type;
  uint16_t level_start;
  uint16_t level_count;
  uint16_t slice_start;
  uint16_t slice_count;
  struct WMTTextureSwizzleChannels swizzle;
  obj_handle_t ret;
  uint64_t gpu_resource_id;
};

struct unixcall_mtldevice_newlibrary {
  obj_handle_t device;
  obj_handle_t data;
  obj_handle_t ret_error;
  obj_handle_t ret_library;
};

struct unixcall_mtldevice_newcomputepso {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret_error;
  obj_handle_t ret_pso;
};

struct unixcall_mtldevice_newrenderpso {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret_error;
  obj_handle_t ret_pso;
};

/* d9mt addition: all members 8-byte / arch-invariant, so the 64-bit thunk is
   byte-compatible for wow64 (see DXSODestroy note pattern). */
struct unixcall_mtldevice_newrenderpso_vtx {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  struct WMTConstMemoryPointer vertex_info;
  obj_handle_t ret_error;
  obj_handle_t ret_pso;
};

struct unixcall_mtldevice_newmeshrenderpso {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret_error;
  obj_handle_t ret_pso;
};

struct unixcall_generic_obj_cmd_noret {
  obj_handle_t encoder;
  struct WMTConstMemoryPointer cmd_head;
};

struct unixcall_mtltexture_replaceregion {
  obj_handle_t texture;
  struct WMTOrigin origin;
  struct WMTSize size;
  uint64_t level;
  uint64_t slice;
  struct WMTMemoryPointer data;
  uint64_t bytes_per_row;
  uint64_t bytes_per_image;
};

struct unixcall_generic_obj_obj_noret {
  obj_handle_t handle;
  obj_handle_t arg;
};

struct unixcall_generic_obj_obj_obj_noret {
  obj_handle_t handle;
  obj_handle_t arg0;
  obj_handle_t arg1;
};

struct unixcall_generic_obj_obj_obj_uint64_ret {
  obj_handle_t handle;
  obj_handle_t arg0;
  obj_handle_t arg1;
  uint64_t ret;
};

struct unixcall_generic_obj_obj_double_noret {
  obj_handle_t handle;
  obj_handle_t arg0;
  double arg1;
};

struct unixcall_mtlcapturemanager_startcapture {
  obj_handle_t capture_manager;
  struct WMTMemoryPointer info;
  bool ret;
};

struct unixcall_mtldevice_newfxtemporalscaler {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtldevice_newfxspatialscaler {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtlcommandbuffer_temporal_scale {
  obj_handle_t cmdbuf;
  obj_handle_t scaler;
  obj_handle_t color;
  obj_handle_t output;
  obj_handle_t depth;
  obj_handle_t motion;
  obj_handle_t exposure;
  obj_handle_t fence;
  struct WMTConstMemoryPointer props;
};

struct unixcall_mtlcommandbuffer_spatial_scale {
  obj_handle_t cmdbuf;
  obj_handle_t scaler;
  obj_handle_t color;
  obj_handle_t output;
  obj_handle_t fence;
};

struct unixcall_nsstring_string {
  struct WMTConstMemoryPointer buffer_ptr;
  enum WMTStringEncoding encoding;
  obj_handle_t ret;
};

struct unixcall_create_metal_view_from_hwnd {
  uint64_t hwnd;
  obj_handle_t device;
  obj_handle_t ret_view;
  obj_handle_t ret_layer;
};

struct unixcall_enumerate {
  obj_handle_t enumerable;
  uint64_t start;
  uint64_t buffer_size;
  struct WMTMemoryPointer buffer;
  uint64_t ret_read;
};

struct unixcall_mtllibrary_newfunction_with_constants {
  obj_handle_t library;
  struct WMTConstMemoryPointer name;
  uint64_t num_constants;
  obj_handle_t ret;
  obj_handle_t ret_error;
  struct WMTFunctionConstant constants[8];
  uint64_t bool_values;
};

struct unixcall_query_display_setting {
  uint64_t display_id;
  enum WMTColorSpace colorspace;
  struct WMTMemoryPointer hdr_metadata;
  bool ret;
};

struct unixcall_update_display_setting {
  uint64_t display_id;
  enum WMTColorSpace colorspace;
  struct WMTConstMemoryPointer hdr_metadata;
};

struct unixcall_query_display_setting_for_layer {
  obj_handle_t layer;
  uint64_t version;
  enum WMTColorSpace colorspace;
  struct WMTMemoryPointer hdr_metadata;
  struct WMTEDRValue edr_value;
};

struct unixcall_mtlbuffer_updatecontents {
  obj_handle_t buffer;
  uint64_t offset;
  struct WMTConstMemoryPointer data;
  uint64_t length;
};

struct unixcall_mtlsharedevent_setevent {
  obj_handle_t shared_event;
  obj_handle_t event_handle;
  obj_handle_t shared_event_listener;
  uint64_t value;
};

struct unixcall_mtlsharedevent_createmachport {
  obj_handle_t event;
  uint32_t ret_mach_port;
};

struct unixcall_mtldevice_newsharedeventwithmachport {
  obj_handle_t device;
  uint32_t mach_port;
  obj_handle_t ret_event;
};

struct unixcall_get_os_version {
  uint64_t ret_major;
  uint64_t ret_minor;
  uint64_t ret_patch;
};

struct unixcall_mtldevice_newbinaryarchive {
  obj_handle_t device;
  struct WMTConstMemoryPointer url;
  obj_handle_t ret_archive;
  obj_handle_t ret_error;
};

struct unixcall_mtlbinaryarchive_serialize {
  obj_handle_t archive;
  struct WMTConstMemoryPointer url;
  obj_handle_t ret_error;
};

struct unixcall_cache_alloc_init {
  struct WMTConstMemoryPointer path;
  uint64_t version;
  obj_handle_t ret_cache;
};

struct unixcall_cache_get {
  obj_handle_t cache;
  struct WMTConstMemoryPointer key;
  uint64_t key_length;
  obj_handle_t ret_data;
};

struct unixcall_cache_set {
  obj_handle_t cache;
  struct WMTConstMemoryPointer key;
  uint64_t key_length;
  obj_handle_t value_data;
};

struct unixcall_setmetalcachepath {
  struct WMTConstMemoryPointer path;
  uint64_t ret_success;
};

struct unixcall_bootstrap {
  char name[128];
  uint32_t mach_port;
  uint32_t reserved;
};

struct unixcall_mtlsharedevent_waituntilsignaledvalue {
  obj_handle_t event;
  uint64_t value;
  uint64_t timeout_ms;
  bool ret_timeout;
};

struct unixcall_mtlcountersamplebuffer_newtimestampbuffer {
  obj_handle_t device;
  uint32_t sample_count;
  uint32_t shared;
  obj_handle_t ret;
};

struct unixcall_mtlcountersamplebuffer_resolvecounterrange {
  obj_handle_t sample_buffer;
  uint32_t start;
  uint32_t len;
  struct WMTMemoryPointer data_out;
  uint64_t data_length;
};

struct unixcall_mtlcommandbuffer_blitcommandencoderwithsamplebuffers {
  obj_handle_t cmdbuf;
  struct WMTMemoryPointer attachments;
  uint64_t num_attachments;
  obj_handle_t ret;
};

/* ===================================================================== *
 * d9mt addition: Metal 4 surface param structs (dispatch codes 157..200).
 * Every member is 8-byte / arch-invariant, so each 64-bit unix thunk is
 * wow64 byte-compatible (shared thunk, no thunk32 twin). Device factories
 * and single-handle ops reuse the generic unixcall_* structs above:
 *   newMTL4CommandQueue/newCommandAllocator/newMTL4CommandBuffer/
 *   newMTL4Compiler + computeCommandEncoder  -> unixcall_generic_obj_obj_ret
 *   renderCommandEncoder                     -> unixcall_generic_obj_uint64_obj_ret
 *   reset/endCommandBuffer/RS commit/req/removeAll -> unixcall_generic_obj_noret
 *   beginCommandBuffer/useResidencySet/setPSO/setDepthStencil/
 *   RS addAllocation/generateMipmaps/queue wait+signal drawable
 *                                            -> unixcall_generic_obj_obj_noret
 *   setCullMode/setFrontFacingWinding/setStencilRef -> unixcall_generic_obj_uint64_noret
 *   queue signalEvent/waitForEvent           -> unixcall_generic_obj_obj_uint64_noret
 * ===================================================================== */

struct unixcall_mtl4_new_from_desc {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtl4_queue_commit {
  obj_handle_t queue;
  struct WMTConstMemoryPointer cmdbufs;
  uint64_t count;
};

struct unixcall_mtl4_argtable_setaddress {
  obj_handle_t table;
  uint64_t gpu_address;
  uint64_t stride;
  uint64_t index;
};

struct unixcall_mtl4_argtable_setresource {
  obj_handle_t table;
  uint64_t gpu_resource_id;
  uint64_t index;
};

struct unixcall_mtl4_setargumenttable {
  obj_handle_t encoder;
  obj_handle_t table;
  uint64_t stages;
};

struct unixcall_mtl4_setviewport {
  obj_handle_t encoder;
  struct WMTViewport viewport;
};

struct unixcall_mtl4_setscissorrect {
  obj_handle_t encoder;
  struct WMTScissorRect rect;
};

struct unixcall_mtl4_setdepthbias {
  obj_handle_t encoder;
  float bias;
  float slope_scale;
  float clamp;
  uint32_t padding;
};

struct unixcall_mtl4_setblendcolor {
  obj_handle_t encoder;
  float red;
  float green;
  float blue;
  float alpha;
};

struct unixcall_mtl4_setvisibilitymode {
  obj_handle_t encoder;
  uint64_t mode;
  uint64_t offset;
};

struct unixcall_mtl4_draw {
  obj_handle_t encoder;
  uint64_t primitive;
  uint64_t vertex_start;
  uint64_t vertex_count;
  uint64_t instance_count;
  uint64_t base_instance;
};

struct unixcall_mtl4_draw_indexed {
  obj_handle_t encoder;
  uint64_t primitive;
  uint64_t index_count;
  uint64_t index_type;
  uint64_t index_buffer;
  uint64_t index_buffer_length;
  uint64_t instance_count;
  int64_t base_vertex;
  uint64_t base_instance;
};

struct unixcall_mtl4_copy_buf_buf {
  obj_handle_t encoder;
  obj_handle_t src;
  obj_handle_t dst;
  uint64_t src_offset;
  uint64_t dst_offset;
  uint64_t size;
};

struct unixcall_mtl4_copy_buf_tex {
  obj_handle_t encoder;
  obj_handle_t src_buffer;
  obj_handle_t dst_texture;
  uint64_t src_offset;
  uint64_t src_bytes_per_row;
  uint64_t src_bytes_per_image;
  struct WMTSize src_size;
  uint64_t dst_slice;
  uint64_t dst_level;
  struct WMTOrigin dst_origin;
};

struct unixcall_mtl4_copy_tex_tex {
  obj_handle_t encoder;
  obj_handle_t src;
  obj_handle_t dst;
  uint64_t src_slice;
  uint64_t src_level;
  struct WMTOrigin src_origin;
  struct WMTSize src_size;
  uint64_t dst_slice;
  uint64_t dst_level;
  struct WMTOrigin dst_origin;
};

struct unixcall_mtl4_copy_tex_buf {
  obj_handle_t encoder;
  obj_handle_t src_texture;
  obj_handle_t dst_buffer;
  uint64_t src_slice;
  uint64_t src_level;
  struct WMTOrigin src_origin;
  struct WMTSize src_size;
  uint64_t dst_offset;
  uint64_t dst_bytes_per_row;
  uint64_t dst_bytes_per_image;
};

struct unixcall_mtl4_fillbuffer {
  obj_handle_t encoder;
  obj_handle_t buffer;
  uint64_t offset;
  uint64_t length;
  uint32_t value;
  uint32_t padding;
};

struct unixcall_mtl4_newrenderpso {
  obj_handle_t compiler;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret_error;
  obj_handle_t ret_pso;
};

#pragma pack(pop)

#endif
