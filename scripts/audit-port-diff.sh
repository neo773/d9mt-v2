#!/usr/bin/env bash
# audit-port-diff.sh — the port quality gate.
#
# For every ported file in driver/d3d9, diff against its DXVK 3.0 upstream
# counterpart with the EXPECTED mechanical renames normalized away. What
# remains is the real semantic delta — every surviving hunk must trace to
# docs/rewrite/01 (call-site mapping), 02 (type map), 05 (resource mapping),
# or an EXTRACTION-LOG entry. Unexplained hunks = port defects.
#
#   scripts/audit-port-diff.sh              # summary table (hunks per file)
#   scripts/audit-port-diff.sh <file.cpp>   # full normalized diff for one file
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
UP="$V2/DXVK-3.0-uptsream/src/d3d9"
PORT="$V2/driver/d3d9"

# Expected mechanical rename map (sed -E), applied to the UPSTREAM side so
# only unexpected differences survive.
normalize() {
  sed -E \
    -e 's/DxvkBufferSlice/MtlBufferSlice/g' \
    -e 's/Rc<DxvkBuffer>/Rc<MtlBuffer>/g' \
    -e 's/Rc<DxvkImageView>/Rc<MtlTextureView>/g' \
    -e 's/Rc<DxvkImage>/Rc<MtlTexture>/g' \
    -e 's/Rc<DxvkResourceAllocation>/Rc<MtlAllocation>/g' \
    -e 's/Rc<DxvkDevice>/Rc<MtlDevice>/g' \
    -e 's/Rc<DxvkInstance>/Rc<MtlInstance>/g' \
    -e 's/Rc<DxvkAdapter>/Rc<MtlAdapter>/g' \
    -e 's/Rc<DxvkShader>/Rc<MtlShaderFunction>/g' \
    -e 's/DxvkContext/MtlContext/g' \
    -e 's/DxvkImageViewKey/MtlImageViewKey/g' \
    -e 's/DxvkImageCreateInfo/MtlImageCreateInfo/g' \
    -e 's/DxvkBufferCreateInfo/MtlBufferCreateInfo/g' \
    -e 's/DxvkFormatInfo/MtlFormatInfo/g' \
    -e 's/DxvkStagingBuffer/MtlStagingBuffer/g' \
    -e 's/DxvkGpuVendor/MtlGpuVendor/g' \
    -e 's/D3D9_VK_FORMAT_MAPPING/D3D9_MTL_FORMAT_MAPPING/g' \
    -e 's/VkDeviceSize/MtlDeviceSize/g' \
    -e 's/VkExtent3D/MtlExtent3D/g' \
    -e 's/VkExtent2D/MtlExtent2D/g' \
    -e 's/VkOffset3D/MtlOffset3D/g' \
    -e 's/VkOffset2D/MtlOffset2D/g' \
    -e 's/VkImageType/MtlImageType/g' \
    -e 's/VkImageViewType/MtlTextureViewType/g' \
    -e 's/VkImageUsageFlags/MtlTextureUsage/g' \
    -e 's/VkImageAspectFlags/MtlAspectMask/g' \
    -e 's/VkFormat/MtlFormat/g' \
    -e 's/VkViewport/MtlViewport/g' \
    -e 's|#include "\.\./dxvk/dxvk_device\.h"|#include "../mtl/mtl_device.h"|' \
    -e 's|#include "\.\./dxvk/dxvk_context\.h"|#include "../mtl/mtl_context.h"|' \
    -e 's|#include "\.\./dxvk/dxvk_cs\.h"|#include "../cs/dxvk_cs.h"|'
}

single="${1:-}"

if [ -n "$single" ]; then
  diff -u <(normalize < "$UP/$single") "$PORT/$single" || true
  exit 0
fi

printf "%-36s %8s %8s\n" "file" "hunks" "lines±"
total=0
for f in "$PORT"/*.h "$PORT"/*.cpp; do
  base="$(basename "$f")"
  [ -f "$UP/$base" ] || { printf "%-36s %8s\n" "$base" "NEW"; continue; }
  d="$(diff -u <(normalize < "$UP/$base") "$f" || true)"
  hunks="$(printf '%s' "$d" | grep -c '^@@' || true)"
  churn="$(printf '%s' "$d" | grep -cE '^[+-][^+-]' || true)"
  total=$((total + hunks))
  printf "%-36s %8s %8s\n" "$base" "$hunks" "$churn"
done
echo "----"
echo "total semantic hunks: $total  (each must trace to docs/rewrite or EXTRACTION-LOG)"
