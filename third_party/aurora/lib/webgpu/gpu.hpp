#pragma once

#include <aurora/aurora.h>
#include <aurora/math.hpp>

#include "wgpu.hpp"

#include <array>
#include <cstdint>

struct SDL_Window;

namespace aurora::webgpu {
inline constexpr wgpu::TextureFormat NormalBufferFormat = wgpu::TextureFormat::RGB10A2Unorm;

struct GraphicsConfig {
  wgpu::SurfaceConfiguration surfaceConfiguration;   // the swapchain (output); FP16 scRGB when HDR is on
  wgpu::TextureFormat colorFormat;                   // ShadowRecomp: render format (EFB, copies): FP16 when HDR is on
  wgpu::TextureFormat uiFormat;                      // ShadowRecomp: 8-bit format (SDR swapchain, ImGui, its HDR layer)
  bool hdrOutput = false;                            // surface is FP16 scRGB (the present pass converts)
  wgpu::TextureFormat depthFormat;
  uint32_t msaaSamples;
  uint16_t textureAnisotropy;
  bool normalBuffer = false;
};
struct TextureWithSampler {
  wgpu::Texture texture;
  wgpu::TextureView view;
  wgpu::Extent3D size;
  wgpu::TextureFormat format;
  wgpu::Sampler sampler;
};
struct Viewport {
  float left;
  float top;
  float width;
  float height;
  float znear;
  float zfar;

  bool operator==(const Viewport& rhs) const {
    return left == rhs.left && top == rhs.top && width == rhs.width && height == rhs.height && znear == rhs.znear &&
           zfar == rhs.zfar;
  }
  bool operator!=(const Viewport& rhs) const { return !(*this == rhs); }
};

extern wgpu::Device g_device;
extern wgpu::Queue g_queue;
extern wgpu::Surface g_surface;
extern wgpu::BackendType g_backendType;
extern GraphicsConfig g_graphicsConfig;
extern TextureWithSampler g_frameBuffer;
extern TextureWithSampler g_frameBufferResolved;
extern TextureWithSampler g_depthBuffer;
extern TextureWithSampler g_normalBuffer;
extern wgpu::RenderPipeline g_CopyPipeline;
extern wgpu::RenderPipeline g_CopyPremultipliedAlphaPipeline;
extern wgpu::BindGroup g_CopyBindGroup;
extern wgpu::Instance g_instance;
extern wgpu::AdapterInfo g_adapterInfo;
extern bool g_hasCoreFeatures;
extern bool g_bcTexturesSupported;
extern bool g_astcTexturesSupported;
extern bool g_textureComponentSwizzleSupported;
extern bool g_dualSourceBlendingSupported;

bool initialize(AuroraBackend backend, bool allowCpu);
void shutdown();
void release_surface() noexcept;
bool refresh_surface(bool recreate = true);
void resize_swapchain(uint32_t width, uint32_t height, uint32_t nativeWidth, uint32_t nativeHeight, bool force = false);
// format Undefined = the render format (colorFormat)
TextureWithSampler create_render_texture(uint32_t width, uint32_t height, bool multisampled,
                                         wgpu::TextureFormat format = wgpu::TextureFormat::Undefined);
bool enable_normal_buffer();
const TextureWithSampler& present_source() noexcept;
wgpu::BindGroup create_copy_bind_group(const TextureWithSampler& source);
void set_resampler(AuroraSampler sampler) noexcept;
AuroraSampler get_resampler() noexcept;
Viewport calculate_present_viewport(uint32_t surface_width, uint32_t surface_height, uint32_t content_width,
                                    uint32_t content_height) noexcept;
const TextureWithSampler& resample_present_source(const wgpu::CommandEncoder& encoder, const Viewport& viewport);
// ShadowRecomp HDR output: write the present pass's live parameters (call before the present pass); the ImGui layer
// texture used when the output is HDR; its composite pipeline (premultiplied, fs_ui).
void update_output_uniforms();
const TextureWithSampler& ui_layer();
extern wgpu::RenderPipeline g_UiCompositePipeline;
// HDR frame statistics (aurora_hdr_request_stats): copy in the present encoder, map after submit
void hdr_stats_encode(const wgpu::CommandEncoder& encoder);
void hdr_stats_after_submit();
// exact per-frame capture (aurora_capture_frames)
void frame_capture_encode(const wgpu::CommandEncoder& encoder);
void frame_capture_after_submit();
void draw_clear(const wgpu::RenderPassEncoder& pass, bool clearColor, bool clearAlpha, bool clearDepth,
                const Vec4<float>& clearColorValue, float clearDepthValue);

size_t load_from_cache(void const* key, size_t keySize, void* value, size_t valueSize, void* userdata);
void store_to_cache(void const* key, size_t keySize, void const* value, size_t valueSize, void* userdata);
void cache_prune();
void cache_shutdown();

} // namespace aurora::webgpu
