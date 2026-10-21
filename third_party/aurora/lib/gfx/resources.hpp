#pragma once

#include "types.hpp"

namespace aurora::gfx {
inline constexpr bool UseTextureBuffer = true;
inline constexpr uint64_t UniformBufferSize = 33554432; // 32 MiB (was 24; #154: ~18 KB per drawn ring-like object)
inline constexpr uint64_t VertexBufferSize = 12582912;  // 12 MiB (was 5; #154: ~4 KB per object)
inline constexpr uint64_t IndexBufferSize = 16777216;   // 16 MiB (was 2: ~220 drawn rings overflowed it and aborted the game, #154; ~8 KB per object)
inline constexpr uint64_t StorageBufferSize = 16777216; // 16 MiB (was 8; #154: ~8 KB per object, overflowed at ~1000 rings)
inline constexpr uint64_t TextureUploadSize = 25165824; // 24 MiB
inline constexpr uint64_t ShadowBufferSize = 67108864 + 5 * 512 * 512 * 4 + 16777216;  // Sun (64 MiB), the sky volumes (5 MiB), then the screen-space AO (16 MiB, #113).

namespace detail {
struct Resources {
  wgpu::Buffer vertexBuffer;
  wgpu::Buffer uniformBuffer;
  wgpu::Buffer indexBuffer;
  wgpu::Buffer storageBuffer;
  wgpu::Buffer shadowBuffer;   // [ShadowRecomp lighting] sun cascade depth, read by lit shaders (static group binding 2)
  wgpu::BindGroupLayout staticBindGroupLayout;
  wgpu::BindGroup staticBindGroup;
  wgpu::BindGroupLayout uniformBindGroupLayout;
  wgpu::BindGroup uniformBindGroup;
  wgpu::Limits limits;
  AuroraStats stats{};
};

Resources& resources() noexcept;
} // namespace detail
} // namespace aurora::gfx
