#include "gpu.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <aurora/aurora.h>
#include <aurora/webgpu.hpp>
#include <aurora/gfx.h>
#include <magic_enum.hpp>
#include <webgpu/webgpu_cpp.h>

#include "../gx/gx.hpp"
#include "../gfx/frame.hpp"
#include "../gfx/recording.hpp"
#include "../gfx/render_worker.hpp"
#include "../gfx/tex_copy_conv.hpp"
#include "../internal.hpp"
#include "../window.hpp"
#include "gpu_prof.hpp"

#ifdef WEBGPU_DAWN
#include "../dawn/TracyPlatform.hpp"
#include <dawn/native/DawnNative.h>
#endif

namespace aurora::gx {
void clear_copy_texture_cache() noexcept;
} // namespace aurora::gx
namespace aurora::gfx {
void clear_offscreen_cache();
} // namespace aurora::gfx

namespace aurora::webgpu {
static Module Log("aurora::gpu");

wgpu::Device g_device;
wgpu::Queue g_queue;
wgpu::Surface g_surface;
wgpu::BackendType g_backendType;
GraphicsConfig g_graphicsConfig;
TextureWithSampler g_frameBuffer;
TextureWithSampler g_frameBufferResolved;
TextureWithSampler g_depthBuffer;
TextureWithSampler g_normalBuffer;

// EFB -> XFB copy pipeline
static wgpu::BindGroupLayout g_CopyBindGroupLayout;
wgpu::RenderPipeline g_CopyPipeline;
wgpu::RenderPipeline g_CopyPremultipliedAlphaPipeline;
wgpu::BindGroup g_CopyBindGroup;
// ShadowRecomp: HDR output (see the copy shader's OutputParams)
struct OutputUniformBlock {
  uint32_t hdr = 0;
  float paperWhite = 2.5f, peak = 12.5f, highlight = 0.f, saturation = 1.f, vibrance = 0.f, contrast = 1.f;
  float uiWhite = 2.5f, black = 0.f;
  uint32_t test = 0, pad0 = 0, pad1 = 0;
  // #147 colour pass (col_on 0 = skipped)
  uint32_t colOn = 0;
  float colBlack = 0.f, colWhite = 1.f, colContrast = 1.f, colSaturation = 1.f, colVibrance = 0.f, colGamma = 1.f, colExposure = 1.f;
  float colR = 1.f, colG = 1.f, colB = 1.f, colPad = 0.f;
};
static wgpu::Buffer g_OutputUniformBuffer;
wgpu::RenderPipeline g_UiCompositePipeline;               // the ImGui layer onto the HDR swapchain
static wgpu::TextureFormat g_copyPipelineFormat = wgpu::TextureFormat::Undefined;
static std::mutex g_hdrMutex;
static AuroraHdrSettings g_hdrSettings{};                // last aurora_set_hdr
static bool g_hdrFp16Offered = false;
static TextureWithSampler g_uiLayer;                     // ImGui renders here (colorFormat) when the output is HDR
static AuroraSampler g_Resampler = SAMPLER_BILINEAR;
static wgpu::BindGroupLayout g_ResampleBindGroupLayout;
static wgpu::RenderPipeline g_ResamplePipeline;
static wgpu::Buffer g_ResampleUniformBuffer;
static TextureWithSampler g_resampledFrameBuffer;

static wgpu::Adapter g_adapter;
wgpu::Instance g_instance;
wgpu::AdapterInfo g_adapterInfo;
static wgpu::SurfaceCapabilities g_surfaceCapabilities;
bool g_hasCoreFeatures = false;
bool g_dualSourceBlendingSupported = false;
bool g_bcTexturesSupported = false;
bool g_astcTexturesSupported = false;
bool g_textureComponentSwizzleSupported = false;
static std::atomic_bool g_initialized = false;
static std::atomic_bool g_vsyncEnabled = true;

namespace {

AuroraLogLevel wgpu_log_level(wgpu::LoggingType type) {
  switch (type) {
  case wgpu::LoggingType::Verbose:
    return LOG_DEBUG;
  case wgpu::LoggingType::Info:
    return LOG_INFO;
  case wgpu::LoggingType::Warning:
    return LOG_WARNING;
  case wgpu::LoggingType::Error:
    return LOG_ERROR;
  default:
    return LOG_FATAL;
  }
}

void wgpu_log(wgpu::LoggingType type, wgpu::StringView message) {
  Log.report(wgpu_log_level(type), "WebGPU message: {}", message);
}

struct ResampleUniformBlock {
  uint32_t samplerMode = 0;
  float frameWidth = 0.f;
  float frameHeight = 0.f;
};

constexpr std::string_view resampleShaderSource = R"(
struct Uniforms {
    sampler_mode: u32,
    frame_width: f32,
    frame_height: f32,
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var s: sampler;
@group(0) @binding(2) var t: texture_2d<f32>;

var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(-1.0, 1.0),
    vec2(-1.0, -3.0),
    vec2(3.0, 1.0),
);
var<private> uvs: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(0.0, 0.0),
    vec2(0.0, 2.0),
    vec2(2.0, 0.0),
);

@vertex
fn vs_main(@builtin(vertex_index) vtxIdx: u32) -> VertexOutput {
    var out: VertexOutput;
    out.position = vec4<f32>(pos[vtxIdx], 0.0, 1.0);
    out.uv = uvs[vtxIdx];
    return out;
}

// BEGIN AREA SAMPLER REGION

fn sample_by_pixel(pixel: vec2<i32>) -> vec4<f32> {
    let source_dims = textureDimensions(t);
    let max_coord = vec2<i32>(source_dims) - vec2<i32>(1, 1);
    let coord = clamp(pixel, vec2<i32>(0, 0), max_coord);
    return textureLoad(t, coord, 0);
}

fn sample_area(frag_position: vec4<f32>) -> vec4<f32> {
    let source_size = vec2<f32>(textureDimensions(t));
    let target_size = max(vec2<f32>(uniforms.frame_width, uniforms.frame_height), vec2<f32>(1.0, 1.0));

    let source_min = clamp((frag_position.xy - vec2<f32>(0.5, 0.5)) / target_size,
                           vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 1.0)) * source_size;
    let source_max = clamp((frag_position.xy + vec2<f32>(0.5, 0.5)) / target_size,
                           vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 1.0)) * source_size;

    let first_pixel = vec2<i32>(floor(source_min));
    let last_pixel = vec2<i32>(ceil(source_max));
    let max_iterations: i32 = 16;

    var avg_color = vec4<f32>(0.0, 0.0, 0.0, 0.0);
    var total_weight = 0.0;

    for (var iy: i32 = 0; iy < max_iterations; iy = iy + 1) {
        let source_y = first_pixel.y + iy;
        if (source_y < last_pixel.y) {
            let y0 = f32(source_y);
            let weight_y = max(min(source_max.y, y0 + 1.0) - max(source_min.y, y0), 0.0);

            for (var ix: i32 = 0; ix < max_iterations; ix = ix + 1) {
                let source_x = first_pixel.x + ix;
                if (source_x < last_pixel.x) {
                    let x0 = f32(source_x);
                    let weight_x = max(min(source_max.x, x0 + 1.0) - max(source_min.x, x0), 0.0);
                    let weight = weight_x * weight_y;
                    avg_color += weight * sample_by_pixel(vec2<i32>(source_x, source_y));
                    total_weight += weight;
                }
            }
        }
    }

    return avg_color / max(total_weight, 0.000001);
}

// END AREA SAMPLER REGION

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    var color = textureSample(t, s, in.uv);
    if (uniforms.sampler_mode == 1u) {
        color = sample_area(in.position);
    }
    return vec4(color.rgb, 1.0);
}
)"sv;

wgpu::TextureFormat to_linear(wgpu::TextureFormat format) {
  if (format == wgpu::TextureFormat::RGBA8UnormSrgb) {
    return wgpu::TextureFormat::RGBA8Unorm;
  }
  if (format == wgpu::TextureFormat::BGRA8UnormSrgb) {
    return wgpu::TextureFormat::BGRA8Unorm;
  }
  return format;
}

wgpu::TextureFormat best_surface_format() {
  if (g_surfaceCapabilities.formatCount == 0) {
    return wgpu::TextureFormat::Undefined;
  }
  for (size_t i = 0; i < g_surfaceCapabilities.formatCount; ++i) {
    const auto format = to_linear(g_surfaceCapabilities.formats[i]);
    if (format == wgpu::TextureFormat::RGBA8Unorm || format == wgpu::TextureFormat::BGRA8Unorm) {
      return format;
    }
  }
  return g_surfaceCapabilities.formats[0];
}

uint32_t sampler_mode(AuroraSampler sampler) noexcept {
  switch (sampler) {
  case SAMPLER_AREA:
    return 1;
  case SAMPLER_BILINEAR:
  default:
    return 0;
  }
}

uint32_t viewport_extent(float value) noexcept {
  return std::max(1u, static_cast<uint32_t>(std::lround(std::max(value, 1.f))));
}

} // namespace

bool vsync_enabled() noexcept { return g_vsyncEnabled.load(std::memory_order_acquire); }

wgpu::PresentMode select_present_mode(const wgpu::SurfaceCapabilities& capabilities) noexcept {
  const auto supports = [&capabilities](const wgpu::PresentMode candidate) {
    for (size_t i = 0; i < capabilities.presentModeCount; ++i) {
      if (capabilities.presentModes[i] == candidate) {
        return true;
      }
    }
    return false;
  };
  if (vsync_enabled()) {
    if (supports(wgpu::PresentMode::FifoRelaxed)) {
      return wgpu::PresentMode::FifoRelaxed;
    }
  } else {
    // Dawn only disables CAMetalLayer displaySyncEnabled for Immediate on Metal
    if (g_backendType != wgpu::BackendType::Metal && supports(wgpu::PresentMode::Mailbox)) {
      return wgpu::PresentMode::Mailbox;
    }
    if (supports(wgpu::PresentMode::Immediate)) {
      return wgpu::PresentMode::Immediate;
    }
  }
  return wgpu::PresentMode::Fifo;
}

TextureWithSampler create_render_texture(uint32_t width, uint32_t height, bool multisampled,
                                         wgpu::TextureFormat format) {
  const wgpu::Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  if (format == wgpu::TextureFormat::Undefined) format = g_graphicsConfig.colorFormat;
  uint32_t sampleCount = 1;
  if (multisampled) {
    sampleCount = g_graphicsConfig.msaaSamples;
  }
  if (width == 0 || height == 0) {
    Log.fatal("Invalid render texture size! {}x{}, multisampled {}, format {}", width, height,
              static_cast<uint32_t>(format), multisampled);
  }
  const wgpu::TextureDescriptor textureDescriptor{
      .label = "Render texture",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc |
               wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = format,
      .mipLevelCount = 1,
      .sampleCount = sampleCount,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);

  constexpr wgpu::TextureViewDescriptor viewDescriptor{
      .label = "Render texture view",
      .dimension = wgpu::TextureViewDimension::e2D,
  };
  auto view = texture.CreateView(&viewDescriptor);

  constexpr wgpu::SamplerDescriptor samplerDescriptor{
      .label = "Render sampler",
      .addressModeU = wgpu::AddressMode::ClampToEdge,
      .addressModeV = wgpu::AddressMode::ClampToEdge,
      .addressModeW = wgpu::AddressMode::ClampToEdge,
      .magFilter = wgpu::FilterMode::Linear,
      .minFilter = wgpu::FilterMode::Linear,
      .mipmapFilter = wgpu::MipmapFilterMode::Linear,
      .lodMinClamp = 0.f,
      .lodMaxClamp = 1000.f,
      .maxAnisotropy = 1,
  };
  auto sampler = g_device.CreateSampler(&samplerDescriptor);

  return {
      .texture = std::move(texture),
      .view = std::move(view),
      .size = size,
      .format = format,
      .sampler = std::move(sampler),
  };
}

const TextureWithSampler& present_source() noexcept {
  return g_graphicsConfig.msaaSamples > 1 ? g_frameBufferResolved : g_frameBuffer;
}

void set_resampler(AuroraSampler sampler) noexcept {
  switch (sampler) {
  case SAMPLER_AREA:
  case SAMPLER_BILINEAR:
    g_Resampler = sampler;
    return;
  default:
    g_Resampler = SAMPLER_BILINEAR;
    return;
  }
}

AuroraSampler get_resampler() noexcept { return g_Resampler; }

Viewport calculate_present_viewport(uint32_t surface_width, uint32_t surface_height, uint32_t content_width,
                                    uint32_t content_height) noexcept {
  if (surface_width == 0 || surface_height == 0 || content_width == 0 || content_height == 0) {
    return {};
  }

  uint32_t viewport_width = surface_width;
  uint32_t viewport_height = std::min<uint32_t>(
      surface_height, std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(static_cast<double>(viewport_width) *
                                                                               static_cast<double>(content_height) /
                                                                               static_cast<double>(content_width)))));
  if (viewport_height == surface_height) {
    viewport_width = std::min<uint32_t>(
        surface_width, std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(static_cast<double>(viewport_height) *
                                                                                static_cast<double>(content_width) /
                                                                                static_cast<double>(content_height)))));
  }

  return {
      .left = static_cast<float>((surface_width - viewport_width) / 2),
      .top = static_cast<float>((surface_height - viewport_height) / 2),
      .width = static_cast<float>(viewport_width),
      .height = static_cast<float>(viewport_height),
      .znear = 0.f,
      .zfar = 1.f,
  };
}

static TextureWithSampler create_depth_texture(uint32_t width, uint32_t height) {
  const wgpu::Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  const auto format = g_graphicsConfig.depthFormat;
  const wgpu::TextureDescriptor textureDescriptor{
      .label = "Depth texture",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = format,
      .mipLevelCount = 1,
      .sampleCount = g_graphicsConfig.msaaSamples,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);

  const wgpu::TextureViewDescriptor viewDescriptor{
      .label = "Depth texture view",
      .dimension = wgpu::TextureViewDimension::e2D,
  };
  auto view = texture.CreateView(&viewDescriptor);

  const wgpu::SamplerDescriptor samplerDescriptor{
      .label = "Depth sampler",
      .addressModeU = wgpu::AddressMode::ClampToEdge,
      .addressModeV = wgpu::AddressMode::ClampToEdge,
      .addressModeW = wgpu::AddressMode::ClampToEdge,
      .magFilter = wgpu::FilterMode::Linear,
      .minFilter = wgpu::FilterMode::Linear,
      .mipmapFilter = wgpu::MipmapFilterMode::Linear,
      .lodMinClamp = 0.f,
      .lodMaxClamp = 1000.f,
      .maxAnisotropy = 1,
  };
  auto sampler = g_device.CreateSampler(&samplerDescriptor);

  return {
      .texture = std::move(texture),
      .view = std::move(view),
      .size = size,
      .format = format,
      .sampler = std::move(sampler),
  };
}

static TextureWithSampler create_normal_texture(uint32_t width, uint32_t height) {
  const wgpu::Extent3D size{width, height, 1};
  const wgpu::TextureDescriptor desc{
      .label = "Scene normals",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc,
      .size = size,
      .format = NormalBufferFormat,
  };
  auto texture = g_device.CreateTexture(&desc);
  auto view = texture.CreateView();
  return {.texture = std::move(texture), .view = std::move(view), .size = size, .format = NormalBufferFormat};
}

bool enable_normal_buffer() {
  if (g_graphicsConfig.normalBuffer) {
    return true;
  }
  if (!g_hasCoreFeatures || g_graphicsConfig.msaaSamples != 1 || !g_device || g_frameBuffer.size.width == 0 ||
      g_frameBuffer.size.height == 0) {
    return false;
  }
  g_normalBuffer = create_normal_texture(g_frameBuffer.size.width, g_frameBuffer.size.height);
  g_graphicsConfig.normalBuffer = true;
  return true;
}

void create_copy_pipeline() {
  wgpu::ShaderSourceWGSL sourceDescriptor{};
  sourceDescriptor.code = R"""(
@group(0) @binding(0)
var efb_sampler: sampler;
@group(0) @binding(1)
var efb_texture: texture_2d<f32>;

struct VertexOutput {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(-1.0, 1.0),
    vec2(-1.0, -3.0),
    vec2(3.0, 1.0),
);
var<private> uvs: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(0.0, 0.0),
    vec2(0.0, 2.0),
    vec2(2.0, 0.0),
);

@vertex
fn vs_main(@builtin(vertex_index) vtxIdx: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4<f32>(pos[vtxIdx], 0.0, 1.0);
    out.uv = uvs[vtxIdx];
    return out;
}

// ShadowRecomp output transform. Live parameters (OutputUniformBlock, written every present from aurora_set_hdr).
// SDR (hdr == 0): identity, the original picture. HDR (FP16 scRGB swapchain, 1.0 = 80 nits): the game's sRGB picture
// is linearised, graded (contrast / saturation / vibrance), placed at paper white, its top end stretched toward the
// display peak (highlight expansion = inverse tone mapping) and soft-clipped so nothing exceeds the peak.
struct OutputParams {
    hdr: u32,
    paper_white: f32,   // nits / 80
    peak: f32,          // nits / 80
    highlight: f32,     // 0..1 how far SDR white is pushed toward the peak
    saturation: f32,    // 1 = original
    vibrance: f32,      // 0 = off; boosts dull colours more than saturated ones
    contrast: f32,      // 1 = original (around mid grey)
    ui_white: f32,      // nits / 80 for the PC menus layer
    black: f32,         // black level lift/crush in linear (0 = original)
    test: u32,          // 1 = calibration pattern instead of the game
    pad0: u32,
    pad1: u32,
    col_on: u32,        // #147 colour pass: 0 = skipped (all neutral)
    col_black: f32,     // lift (>0) / crush (<0), linear
    col_white: f32,     // white level gain
    col_contrast: f32,
    col_saturation: f32,
    col_vibrance: f32,
    col_gamma: f32,     // >1 brightens the mid tones
    col_exposure: f32,  // linear multiplier (2^stops)
    col_r: f32,
    col_g: f32,
    col_b: f32,
    col_pad: f32,
};
@group(0) @binding(2)
var<uniform> out_params: OutputParams;

const LUMA = vec3<f32>(0.2126, 0.7152, 0.0722);

fn srgb_to_linear(c: vec3<f32>) -> vec3<f32> {
    return select(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, c <= vec3(0.04045));
}

// the sRGB curve continued above 1.0: with the FP16 render path, additive light piles up past white
fn srgb_to_linear_ext(c: vec3<f32>) -> vec3<f32> {
    let x = max(c, vec3(0.0));
    return select(pow((x + 0.055) / 1.055, vec3(2.4)), x / 12.92, x <= vec3(0.04045));
}

fn grade(lin_in: vec3<f32>) -> vec3<f32> {
    var lin = max(lin_in + vec3(out_params.black) * max(vec3(1.0) - lin_in, vec3(0.0)), vec3(0.0));
    let y = dot(lin, LUMA);
    if (y > 0.0 && out_params.contrast != 1.0) {
        lin = lin * (0.18 * pow(y / 0.18, out_params.contrast) / y);
    }
    let y2 = dot(lin, LUMA);
    lin = mix(vec3(y2), lin, out_params.saturation);
    let mx = max(lin.r, max(lin.g, lin.b));
    let mn = min(lin.r, min(lin.g, lin.b));
    let sat = select(0.0, (mx - mn) / mx, mx > 0.0);
    lin = mix(vec3(y2), lin, 1.0 + out_params.vibrance * (1.0 - sat));
    return max(lin, vec3(0.0));
}

// #147 the player's colour pass in linear light (SDR: 0..1, HDR: may exceed 1): exposure, R/G/B, black / white level,
// gamma, contrast, saturation, vibrance. Skipped entirely when neutral (col_on == 0), so neutral is the original picture.
fn colour_pass(lin_in: vec3<f32>) -> vec3<f32> {
    var lin = max(lin_in, vec3(0.0)) * out_params.col_exposure * vec3(out_params.col_r, out_params.col_g, out_params.col_b);
    let w = out_params.col_white;
    lin = max(lin + vec3(out_params.col_black) * max(vec3(1.0) - lin, vec3(0.0)), vec3(0.0)) * w;
    if (out_params.col_gamma != 1.0) {
        lin = pow(lin, vec3(1.0 / out_params.col_gamma));
    }
    let y = dot(lin, LUMA);
    if (y > 0.0 && out_params.col_contrast != 1.0) {
        lin = lin * (0.18 * pow(y / 0.18, out_params.col_contrast) / y);
    }
    let y2 = dot(lin, LUMA);
    lin = mix(vec3(y2), lin, out_params.col_saturation);
    let mx = max(lin.r, max(lin.g, lin.b));
    let mn = min(lin.r, min(lin.g, lin.b));
    let sat = select(0.0, (mx - mn) / mx, mx > 0.0);
    lin = mix(vec3(y2), lin, max(1.0 + out_params.col_vibrance * (1.0 - sat), 0.0));
    return max(lin, vec3(0.0));
}

fn linear_to_srgb(c: vec3<f32>) -> vec3<f32> {
    let x = max(c, vec3(0.0));
    return select(1.055 * pow(x, vec3(1.0 / 2.4)) - 0.055, x * 12.92, x <= vec3(0.0031308));
}

// per channel: unchanged up to 80% of the peak, then an exponential shoulder that reaches the peak asymptotically
fn soft_clip(c: vec3<f32>, p: f32) -> vec3<f32> {
    let k = 0.8 * p;
    let r = max(p - k, 0.0001);
    let over = k + r * (vec3(1.0) - exp(-(c - vec3(k)) / r));
    return select(c, over, c > vec3(k));
}

fn hdr_game(c: vec3<f32>) -> vec3<f32> {
    if (out_params.hdr == 0u) {
        if (out_params.col_on == 0u) {
            return c;
        }
        return linear_to_srgb(colour_pass(srgb_to_linear_ext(c)));
    }
    var lin0 = srgb_to_linear_ext(c);
    if (out_params.col_on != 0u) {
        lin0 = colour_pass(lin0);
    }
    let lin = grade(lin0);   // may exceed 1.0 (overbright from the FP16 EFB)
    let y = dot(lin, LUMA);
    let t = smoothstep(0.45, 1.0, y);
    let max_gain = max(out_params.peak / out_params.paper_white, 1.0);
    let gain = 1.0 + (max_gain - 1.0) * out_params.highlight * t * t;
    return soft_clip(lin * out_params.paper_white * gain, out_params.peak);
}

// HDR calibration pattern (test == 1): left half a grey ramp 0..1 SDR (top) and HDR steps up to the peak (bottom);
// right half: saturated colour bars and a peak-white box. Tells the user what paper white / peak look like.
fn calibration(uv: vec2<f32>) -> vec3<f32> {
    let pw = out_params.paper_white;
    if (uv.x < 0.5) {
        let x = uv.x * 2.0;
        if (uv.y < 0.5) {
            return vec3(pow(x, 2.2)) * pw;                                  // SDR ramp at paper white
        }
        let step_ = floor(x * 10.0) / 9.0;
        return vec3(mix(pw, out_params.peak, step_));                       // 10 steps paper white -> peak
    }
    let x = (uv.x - 0.5) * 2.0;
    if (uv.y < 0.5) {
        let i = u32(floor(x * 6.0));
        var bars = array<vec3<f32>, 6>(vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0),
                                       vec3(0.0, 1.0, 1.0), vec3(1.0, 0.0, 1.0), vec3(1.0, 1.0, 0.0));
        return grade(bars[min(i, 5u)]) * pw;
    }
    let box = abs(uv - vec2(0.75, 0.75)) < vec2(0.1, 0.12);
    return select(vec3(0.0), vec3(out_params.peak), box.x && box.y);         // peak box on black
}

@fragment
fn fs_opaque(in: VertexOutput) -> @location(0) vec4<f32> {
    if (out_params.hdr != 0u && out_params.test != 0u) {
        return vec4(calibration(in.uv), 1.0);
    }
    let color = textureSample(efb_texture, efb_sampler, in.uv);
    return vec4(hdr_game(color.rgb), 1.0);
}

@fragment
fn fs_premultiplied_alpha(in: VertexOutput) -> @location(0) vec4<f32> {
    let color = textureSample(efb_texture, efb_sampler, in.uv);
    if ((out_params.hdr == 0u && out_params.col_on == 0u) || color.a <= 0.0) {
        return color;
    }
    return vec4(hdr_game(color.rgb / color.a) * color.a, color.a);
}

// the PC menus (ImGui) layer, premultiplied, at its own brightness, no grading
@fragment
fn fs_ui(in: VertexOutput) -> @location(0) vec4<f32> {
    let color = textureSample(efb_texture, efb_sampler, in.uv);
    if (out_params.hdr == 0u || color.a <= 0.0) {
        return color;
    }
    return vec4(srgb_to_linear(clamp(color.rgb / color.a, vec3(0.0), vec3(1.0))) * out_params.ui_white * color.a, color.a);
}
)""";
  const wgpu::ShaderModuleDescriptor moduleDescriptor{
      .nextInChain = &sourceDescriptor,
      .label = "XFB Copy Module",
  };
  auto module = g_device.CreateShaderModule(&moduleDescriptor);
  const std::array bindGroupLayoutEntries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler =
              wgpu::SamplerBindingLayout{
                  .type = wgpu::SamplerBindingType::Filtering,
              },
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture =
              wgpu::TextureBindingLayout{
                  .sampleType = wgpu::TextureSampleType::Float,
                  .viewDimension = wgpu::TextureViewDimension::e2D,
              },
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 2,
          .visibility = wgpu::ShaderStage::Fragment,
          .buffer =
              wgpu::BufferBindingLayout{
                  .type = wgpu::BufferBindingType::Uniform,
              },
      },
  };
  const wgpu::BindGroupLayoutDescriptor bindGroupLayoutDescriptor{
      .entryCount = bindGroupLayoutEntries.size(),
      .entries = bindGroupLayoutEntries.data(),
  };
  g_CopyBindGroupLayout = g_device.CreateBindGroupLayout(&bindGroupLayoutDescriptor);
  if (!g_OutputUniformBuffer) {
    const wgpu::BufferDescriptor uniformBufferDescriptor{
        .label = "Present Output Uniform Buffer",
        .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::Uniform,
        .size = AURORA_ALIGN(sizeof(OutputUniformBlock), 16),
    };
    g_OutputUniformBuffer = g_device.CreateBuffer(&uniformBufferDescriptor);
  }
  const wgpu::PipelineLayoutDescriptor layoutDescriptor{
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &g_CopyBindGroupLayout,
  };
  auto pipelineLayout = g_device.CreatePipelineLayout(&layoutDescriptor);

  const auto make_copy_pipeline = [&](const char* label, const char* fragmentEntryPoint,
                                      const wgpu::BlendState* blend) {
    const std::array colorTargets{wgpu::ColorTargetState{
        .format = g_graphicsConfig.surfaceConfiguration.format,
        .blend = blend,
        .writeMask = wgpu::ColorWriteMask::All,
    }};
    const wgpu::FragmentState fragmentState{
        .module = module,
        .entryPoint = fragmentEntryPoint,
        .targetCount = colorTargets.size(),
        .targets = colorTargets.data(),
    };
    const wgpu::RenderPipelineDescriptor pipelineDescriptor{
        .label = label,
        .layout = pipelineLayout,
        .vertex =
            wgpu::VertexState{
                .module = module,
                .entryPoint = "vs_main",
            },
        .primitive =
            wgpu::PrimitiveState{
                .topology = wgpu::PrimitiveTopology::TriangleList,
            },
        .multisample =
            wgpu::MultisampleState{
                .count = 1,
                .mask = UINT32_MAX,
            },
        .fragment = &fragmentState,
    };
    return g_device.CreateRenderPipeline(&pipelineDescriptor);
  };
  g_CopyPipeline = make_copy_pipeline("XFB Copy Pipeline", "fs_opaque", nullptr);

  const wgpu::BlendState premultipliedAlphaBlend{
      .color =
          {
              .operation = wgpu::BlendOperation::Add,
              .srcFactor = wgpu::BlendFactor::One,
              .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha,
          },
      .alpha =
          {
              .operation = wgpu::BlendOperation::Add,
              .srcFactor = wgpu::BlendFactor::One,
              .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha,
          },
  };
  g_CopyPremultipliedAlphaPipeline =
      make_copy_pipeline("XFB Premultiplied Alpha Copy Pipeline", "fs_premultiplied_alpha", &premultipliedAlphaBlend);
  g_UiCompositePipeline = make_copy_pipeline("UI Layer Composite Pipeline", "fs_ui", &premultipliedAlphaBlend);
  g_copyPipelineFormat = g_graphicsConfig.surfaceConfiguration.format;
}

void create_resample_pipeline() {
  wgpu::ShaderSourceWGSL sourceDescriptor{};
  sourceDescriptor.code = resampleShaderSource;
  const wgpu::ShaderModuleDescriptor moduleDescriptor{
      .nextInChain = &sourceDescriptor,
      .label = "Present Resample Module",
  };
  auto module = g_device.CreateShaderModule(&moduleDescriptor);
  const std::array colorTargets{wgpu::ColorTargetState{
      .format = g_graphicsConfig.colorFormat,   // renders into g_resampledFrameBuffer (create_render_texture)
      .writeMask = wgpu::ColorWriteMask::All,
  }};
  const wgpu::FragmentState fragmentState{
      .module = module,
      .entryPoint = "fs_main",
      .targetCount = colorTargets.size(),
      .targets = colorTargets.data(),
  };
  const std::array bindGroupLayoutEntries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Fragment,
          .buffer =
              wgpu::BufferBindingLayout{
                  .type = wgpu::BufferBindingType::Uniform,
              },
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler =
              wgpu::SamplerBindingLayout{
                  .type = wgpu::SamplerBindingType::Filtering,
              },
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 2,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture =
              wgpu::TextureBindingLayout{
                  .sampleType = wgpu::TextureSampleType::Float,
                  .viewDimension = wgpu::TextureViewDimension::e2D,
              },
      },
  };
  const wgpu::BindGroupLayoutDescriptor bindGroupLayoutDescriptor{
      .entryCount = bindGroupLayoutEntries.size(),
      .entries = bindGroupLayoutEntries.data(),
  };
  g_ResampleBindGroupLayout = g_device.CreateBindGroupLayout(&bindGroupLayoutDescriptor);
  const wgpu::PipelineLayoutDescriptor layoutDescriptor{
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &g_ResampleBindGroupLayout,
  };
  auto pipelineLayout = g_device.CreatePipelineLayout(&layoutDescriptor);
  const wgpu::RenderPipelineDescriptor pipelineDescriptor{
      .label = "Present Resample Pipeline",
      .layout = pipelineLayout,
      .vertex =
          wgpu::VertexState{
              .module = module,
              .entryPoint = "vs_main",
          },
      .primitive =
          wgpu::PrimitiveState{
              .topology = wgpu::PrimitiveTopology::TriangleList,
          },
      .multisample =
          wgpu::MultisampleState{
              .count = 1,
              .mask = UINT32_MAX,
          },
      .fragment = &fragmentState,
  };
  g_ResamplePipeline = g_device.CreateRenderPipeline(&pipelineDescriptor);

  const wgpu::BufferDescriptor uniformBufferDescriptor{
      .label = "Present Resample Uniform Buffer",
      .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::Uniform,
      .size = AURORA_ALIGN(sizeof(ResampleUniformBlock), 16),
  };
  g_ResampleUniformBuffer = g_device.CreateBuffer(&uniformBufferDescriptor);
}

wgpu::BindGroup create_copy_bind_group(const TextureWithSampler& source) {
  const std::array bindGroupEntries{
      wgpu::BindGroupEntry{
          .binding = 0,
          .sampler = source.sampler,
      },
      wgpu::BindGroupEntry{
          .binding = 1,
          .textureView = source.view,
      },
      wgpu::BindGroupEntry{
          .binding = 2,
          .buffer = g_OutputUniformBuffer,
          .size = AURORA_ALIGN(sizeof(OutputUniformBlock), 16),
      },
  };
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .layout = g_CopyBindGroupLayout,
      .entryCount = bindGroupEntries.size(),
      .entries = bindGroupEntries.data(),
  };
  return g_device.CreateBindGroup(&bindGroupDescriptor);
}

const TextureWithSampler& resample_present_source(const wgpu::CommandEncoder& encoder, const Viewport& viewport) {
  const auto& source = present_source();
  const uint32_t width = viewport_extent(viewport.width);
  const uint32_t height = viewport_extent(viewport.height);
  if (!g_resampledFrameBuffer.view || g_resampledFrameBuffer.size.width != width ||
      g_resampledFrameBuffer.size.height != height || g_resampledFrameBuffer.format != source.format) {
    g_resampledFrameBuffer = create_render_texture(width, height, false);
  }

  const ResampleUniformBlock uniform{
      .samplerMode = sampler_mode(g_Resampler),
      .frameWidth = static_cast<float>(width),
      .frameHeight = static_cast<float>(height),
  };
  AURORA_ASSERT(gfx::render_worker::is_worker_thread(), "Present resample queue write must run on the render worker");
  g_queue.WriteBuffer(g_ResampleUniformBuffer, 0, &uniform, sizeof(uniform));

  const std::array bindGroupEntries{
      wgpu::BindGroupEntry{
          .binding = 0,
          .buffer = g_ResampleUniformBuffer,
          .size = AURORA_ALIGN(sizeof(ResampleUniformBlock), 16),
      },
      wgpu::BindGroupEntry{
          .binding = 1,
          .sampler = source.sampler,
      },
      wgpu::BindGroupEntry{
          .binding = 2,
          .textureView = source.view,
      },
  };
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .layout = g_ResampleBindGroupLayout,
      .entryCount = bindGroupEntries.size(),
      .entries = bindGroupEntries.data(),
  };
  const auto bindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);

  const std::array attachments{
      wgpu::RenderPassColorAttachment{
          .view = g_resampledFrameBuffer.view,
          .loadOp = wgpu::LoadOp::Clear,
          .storeOp = wgpu::StoreOp::Store,
      },
  };
  const wgpu::RenderPassDescriptor renderPassDescriptor{
      .label = "Present resample render pass",
      .colorAttachmentCount = attachments.size(),
      .colorAttachments = attachments.data(),
      .timestampWrites = gpu_prof::pass_writes("Present resample"),
  };
  const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
  pass.SetPipeline(g_ResamplePipeline);
  pass.SetBindGroup(0, bindGroup, 0, nullptr);
  pass.SetViewport(0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f);
  pass.Draw(3);
  pass.End();

  return g_resampledFrameBuffer;
}

static wgpu::BackendType to_wgpu_backend(AuroraBackend backend) {
  switch (backend) {
  case BACKEND_WEBGPU:
    return wgpu::BackendType::WebGPU;
  case BACKEND_D3D11:
    return wgpu::BackendType::D3D11;
  case BACKEND_D3D12:
    return wgpu::BackendType::D3D12;
  case BACKEND_METAL:
    return wgpu::BackendType::Metal;
  case BACKEND_VULKAN:
    return wgpu::BackendType::Vulkan;
  case BACKEND_OPENGL:
    return wgpu::BackendType::OpenGL;
  case BACKEND_OPENGLES:
    return wgpu::BackendType::OpenGLES;
  default:
    return wgpu::BackendType::Null;
  }
}

static void release_surface_locked() noexcept { g_surface = {}; }

static bool create_surface() {
  SDL_Window* window = window::get_sdl_window();
  if (window == nullptr) {
    Log.error("Failed to create surface: no window");
    return false;
  }
  window::SurfaceLock surfaceLock;
  release_surface_locked();
  g_surface = create_window_surface(g_instance, window, "Surface");
  if (!g_surface) {
    Log.error("Failed to create surface");
    return false;
  }
  return true;
}

bool initialize(AuroraBackend auroraBackend, bool allowCpu) {
  if (!g_instance) {
    Log.info("Creating WebGPU instance");
    const std::array requiredInstanceFeatures{
        wgpu::InstanceFeatureName::TimedWaitAny,
    };
    wgpu::InstanceDescriptor instanceDescriptor{
        .requiredFeatureCount = requiredInstanceFeatures.size(),
        .requiredFeatures = requiredInstanceFeatures.data(),
    };
#ifdef WEBGPU_DAWN
    constexpr std::array instanceToggles{
        "allow_unsafe_apis",
    };
    wgpu::DawnTogglesDescriptor instanceTogglesDescriptor{wgpu::DawnTogglesDescriptor::Init{
        .enabledToggleCount = instanceToggles.size(),
        .enabledToggles = instanceToggles.data(),
    }};
    dawn::native::DawnInstanceDescriptor dawnInstanceDescriptor{};
    dawnInstanceDescriptor.nextInChain = &instanceTogglesDescriptor;
    dawnInstanceDescriptor.backendValidationLevel = dawn::native::BackendValidationLevel::Disabled;
    dawnInstanceDescriptor.SetLoggingCallback(wgpu_log);
#ifdef TRACY_ENABLE
    dawnInstanceDescriptor.platform = tracy_dawn_platform();
#endif
    instanceDescriptor.nextInChain = &dawnInstanceDescriptor;
#endif
    g_instance = wgpu::CreateInstance(&instanceDescriptor);
    if (!g_instance) {
      Log.error("Failed to create WebGPU instance");
      return false;
    }
  }
  const wgpu::BackendType backend = to_wgpu_backend(auroraBackend);
  Log.info("Attempting to initialize {}", magic_enum::enum_name(backend));
#if 0
  // D3D12's debug layer is very slow
  g_dawnInstance->EnableBackendValidation(backend != WGPUBackendType::D3D12);
#endif

  if (!create_surface()) {
    return false;
  }
  {
    const wgpu::RequestAdapterOptions options{
        .featureLevel = wgpu::FeatureLevel::Compatibility,
        .powerPreference = wgpu::PowerPreference::HighPerformance,
        .backendType = backend,
        .compatibleSurface = g_surface,
    };
    Log.info("Requesting adapter\n  Feature level: {}\n  Power preference: {}\n  Backend: {}\n  Compatible surface: {}",
             magic_enum::enum_name(options.featureLevel), magic_enum::enum_name(options.powerPreference),
             magic_enum::enum_name(options.backendType), static_cast<bool>(options.compatibleSurface));
    bool requestAdapterCallbackCompleted = false;
    wgpu::RequestAdapterStatus requestAdapterStatus = wgpu::RequestAdapterStatus::CallbackCancelled;
    std::string requestAdapterMessage;
    const auto future = g_instance.RequestAdapter(
        &options, wgpu::CallbackMode::WaitAnyOnly,
        [&](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView message) {
          requestAdapterCallbackCompleted = true;
          requestAdapterStatus = status;
          requestAdapterMessage = std::string{std::string_view{message}};
          if (status == wgpu::RequestAdapterStatus::Success) {
            g_adapter = std::move(adapter);
          } else {
            Log.warn("Adapter request failed: {}: {}", magic_enum::enum_name(status), message);
          }
        });
    const auto status = g_instance.WaitAny(future, 5000000000);
    if (status != wgpu::WaitStatus::Success) {
      if (requestAdapterCallbackCompleted) {
        Log.error("Failed to create adapter: wait status {}, request status {}, message: {}",
                  magic_enum::enum_name(status), magic_enum::enum_name(requestAdapterStatus), requestAdapterMessage);
      } else {
        Log.error("Failed to create adapter: wait status {}, request callback did not complete",
                  magic_enum::enum_name(status));
      }
      return false;
    }
    if (!g_adapter) {
      if (requestAdapterCallbackCompleted) {
        Log.error("Failed to create adapter: request status {}, message: {}",
                  magic_enum::enum_name(requestAdapterStatus), requestAdapterMessage);
      } else {
        Log.error("Failed to create adapter: request callback did not complete");
      }
      return false;
    }
  }
  g_adapter.GetInfo(&g_adapterInfo);
  auto adapterName = g_adapterInfo.device;
  if (adapterName.IsUndefined()) {
    adapterName = wgpu::StringView("Unknown");
  }
  if (!allowCpu && g_adapterInfo.adapterType == wgpu::AdapterType::CPU && backend != wgpu::BackendType::Null) {
    Log.warn("Ignoring CPU adapter: {}", adapterName);
    g_adapterInfo = {};
    g_adapter = {};
    return false;
  }
  auto description = g_adapterInfo.description;
  if (description.IsUndefined()) {
    description = wgpu::StringView("Unknown");
  }
  g_backendType = g_adapterInfo.backendType;
  const auto backendName = magic_enum::enum_name(g_backendType);
  Log.info("Graphics adapter information\n  API: {}\n  Device: {} ({})\n  Driver: {}", backendName, adapterName,
           magic_enum::enum_name(g_adapterInfo.adapterType), description);

  {
    wgpu::Limits supportedLimits{};
    g_adapter.GetLimits(&supportedLimits);
    wgpu::CompatibilityModeLimits compatibilityModeLimits{wgpu::CompatibilityModeLimits::Init{
        .maxStorageBuffersInVertexStage = 2,
        .maxStorageBuffersInFragmentStage = 2,
    }};
    const wgpu::Limits requiredLimits{
        .nextInChain = &compatibilityModeLimits,
        // Use "best" supported limits
        .maxTextureDimension1D = supportedLimits.maxTextureDimension1D == 0 ? WGPU_LIMIT_U32_UNDEFINED
                                                                            : supportedLimits.maxTextureDimension1D,
        .maxTextureDimension2D = supportedLimits.maxTextureDimension2D == 0 ? WGPU_LIMIT_U32_UNDEFINED
                                                                            : supportedLimits.maxTextureDimension2D,
        .maxTextureDimension3D = supportedLimits.maxTextureDimension3D == 0 ? WGPU_LIMIT_U32_UNDEFINED
                                                                            : supportedLimits.maxTextureDimension3D,
        .maxTextureArrayLayers = supportedLimits.maxTextureArrayLayers == 0 ? WGPU_LIMIT_U32_UNDEFINED
                                                                            : supportedLimits.maxTextureArrayLayers,
        .maxStorageBuffersPerShaderStage = 2,
        .minUniformBufferOffsetAlignment =
            supportedLimits.minUniformBufferOffsetAlignment < 64 ? 64 : supportedLimits.minUniformBufferOffsetAlignment,
        .minStorageBufferOffsetAlignment =
            supportedLimits.minStorageBufferOffsetAlignment < 16 ? 16 : supportedLimits.minStorageBufferOffsetAlignment,
        .maxImmediateSize = sizeof(gx::DrawImmediateData),
    };
    Log.info(
        "Using limits:"
        "\n  maxTextureDimension1D: {}"
        "\n  maxTextureDimension2D: {}"
        "\n  maxTextureDimension3D: {}"
        "\n  maxTextureArrayLayers: {}"
        "\n  maxStorageBuffersPerShaderStage: {}"
        "\n  minUniformBufferOffsetAlignment: {}"
        "\n  minStorageBufferOffsetAlignment: {}"
        "\n  maxImmediateSize: {}",
        requiredLimits.maxTextureDimension1D, requiredLimits.maxTextureDimension2D,
        requiredLimits.maxTextureDimension3D, requiredLimits.maxTextureArrayLayers,
        requiredLimits.maxStorageBuffersPerShaderStage, requiredLimits.minUniformBufferOffsetAlignment,
        requiredLimits.minStorageBufferOffsetAlignment, requiredLimits.maxImmediateSize);
    std::vector<wgpu::FeatureName> requiredFeatures;
    g_hasCoreFeatures = false;
    g_bcTexturesSupported = false;
    g_astcTexturesSupported = false;
    g_textureComponentSwizzleSupported = false;
    g_dualSourceBlendingSupported = false;
    wgpu::SupportedFeatures supportedFeatures;
    g_adapter.GetFeatures(&supportedFeatures);
    for (size_t i = 0; i < supportedFeatures.featureCount; ++i) {
      const auto feature = supportedFeatures.features[i];
      if (feature == wgpu::FeatureName::CoreFeaturesAndLimits || feature == wgpu::FeatureName::TextureCompressionBC ||
          feature == wgpu::FeatureName::TextureCompressionASTC ||
          feature == wgpu::FeatureName::TextureComponentSwizzle || feature == wgpu::FeatureName::DualSourceBlending) {
        if (feature == wgpu::FeatureName::CoreFeaturesAndLimits) {
          g_hasCoreFeatures = true;
        } else if (feature == wgpu::FeatureName::TextureCompressionBC) {
          g_bcTexturesSupported = true;
        } else if (feature == wgpu::FeatureName::TextureCompressionASTC) {
          g_astcTexturesSupported = true;
        } else if (feature == wgpu::FeatureName::TextureComponentSwizzle) {
          g_textureComponentSwizzleSupported = true;
        } else if (feature == wgpu::FeatureName::DualSourceBlending) {
          g_dualSourceBlendingSupported = true;
        }
        requiredFeatures.push_back(feature);
      }
#ifdef TRACY_ENABLE
      if (feature == wgpu::FeatureName::TimestampQuery) {
        requiredFeatures.push_back(feature);
      }
#else
      // ShadowRecomp: GPU_TIMESTAMPS=1 (tests) enables timestamp queries without Tracy (#148 AO cost)
      if (feature == wgpu::FeatureName::TimestampQuery && getenv("GPU_TIMESTAMPS")) {
        requiredFeatures.push_back(feature);
      }
#endif
    }
    std::string featureList;
    for (auto featureName : requiredFeatures) {
      featureList += "\n  ";
      featureList += magic_enum::enum_name(featureName);
    }
    Log.info("Enabling features: {}", featureList);
#ifdef WEBGPU_DAWN
    wgpu::DawnCacheDeviceDescriptor cacheDescriptor({.nextInChain = nullptr});
    cacheDescriptor.SetDawnLoadCacheDataCallback(
        [](std::span<const std::byte> key, std::span<std::byte> value) noexcept -> size_t {
          return load_from_cache(key.data(), key.size(), value.data(), value.size(), nullptr);
        });
    cacheDescriptor.SetDawnStoreCacheDataCallback(
        [](std::span<const std::byte> key, std::span<const std::byte> value) noexcept {
          store_to_cache(key.data(), key.size(), value.data(), value.size(), nullptr);
        });

    constexpr std::array enableToggles{
#if _WIN32
        "use_dxc",
#ifndef NDEBUG
        "emit_hlsl_debug_symbols",
#endif
#endif
#ifdef NDEBUG
        "skip_validation",
        "disable_robustness",
#endif
#ifndef ANDROID
        "use_user_defined_labels_in_backend",
#endif
        "disable_symbol_renaming",
        "enable_immediate_error_handling",
        "gl_allow_context_on_multi_threads",
    };
    constexpr std::array disableToggles{
        "timestamp_quantization",
    };
    wgpu::DawnTogglesDescriptor togglesDescriptor(wgpu::DawnTogglesDescriptor::Init{
        .nextInChain = &cacheDescriptor,
        .enabledToggleCount = enableToggles.size(),
        .enabledToggles = enableToggles.data(),
        .disabledToggleCount = disableToggles.size(),
        .disabledToggles = disableToggles.data(),
    });
#endif
    wgpu::DeviceDescriptor deviceDescriptor({
#ifdef WEBGPU_DAWN
        .nextInChain = &togglesDescriptor,
#endif
        .requiredFeatureCount = requiredFeatures.size(),
        .requiredFeatures = requiredFeatures.data(),
        .requiredLimits = &requiredLimits,
    });
    deviceDescriptor.SetUncapturedErrorCallback(
        [](const wgpu::Device& device, wgpu::ErrorType type, wgpu::StringView message) {
          if (g_initialized) {
            FATAL("WebGPU error {}: {}", underlying(type), message);
          } else {
            Log.warn("WebGPU error {}: {}", underlying(type), message);
          }
        });
    deviceDescriptor.SetDeviceLostCallback(
        wgpu::CallbackMode::AllowSpontaneous,
        [](const wgpu::Device& device, wgpu::DeviceLostReason reason, wgpu::StringView message) {
          if (g_initialized) {
            FATAL("Device lost: {}", message);
          } else {
            Log.warn("Device lost: {}", message);
          }
        });
    const auto future =
        g_adapter.RequestDevice(&deviceDescriptor, wgpu::CallbackMode::WaitAnyOnly,
                                [](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView message) {
                                  if (status == wgpu::RequestDeviceStatus::Success) {
                                    g_device = std::move(device);
                                  } else {
                                    Log.warn("Device request failed: {}", message);
                                  }
                                });
    const auto status = g_instance.WaitAny(future, 5000000000);
    if (status != wgpu::WaitStatus::Success) {
      Log.error("Failed to create device: {}", magic_enum::enum_name(status));
      return false;
    }
    if (!g_device) {
      return false;
    }
    g_device.SetLoggingCallback(wgpu_log);
  }
  g_queue = g_device.GetQueue();

  const wgpu::Status status = g_surface.GetCapabilities(g_adapter, &g_surfaceCapabilities);
  if (status != wgpu::Status::Success) {
    Log.error("Failed to get surface capabilities: {}", magic_enum::enum_name(status));
    return false;
  }
  if (g_surfaceCapabilities.formatCount == 0) {
    Log.error("Surface has no formats");
    return false;
  }
  if (g_surfaceCapabilities.presentModeCount == 0) {
    Log.error("Surface has no present modes");
    return false;
  }
  // ShadowRecomp: the game renders in an 8-bit format (colorFormat); the swapchain is the same in SDR, or FP16 scRGB
  // when HDR output is requested and offered (DXGI composes an FP16 flip-model swapchain as scRGB by default).
  const auto renderFormat = best_surface_format();
  auto surfaceFormat = renderFormat;
  {
    std::string formats;
    bool fp16 = false;
    for (size_t i = 0; i < g_surfaceCapabilities.formatCount; ++i) {
      formats += std::string(magic_enum::enum_name(g_surfaceCapabilities.formats[i])) + " ";
      fp16 |= g_surfaceCapabilities.formats[i] == wgpu::TextureFormat::RGBA16Float;
    }
    Log.info("Surface formats offered: {}", formats);
    g_hdrFp16Offered = fp16;
    {
      std::lock_guard lk(g_hdrMutex);
      if (!g_hdrSettings.paperWhiteNits) {   // aurora_set_hdr not called yet: start from the config
        g_hdrSettings.enabled = g_config.hdr;
        g_hdrSettings.paperWhiteNits = g_config.hdrPaperWhiteNits > 0.f ? g_config.hdrPaperWhiteNits : 200.f;
        g_hdrSettings.peakNits = g_config.hdrPeakNits > 0.f ? g_config.hdrPeakNits : 1000.f;
        g_hdrSettings.saturation = 1.f;
        g_hdrSettings.contrast = 1.f;
        g_hdrSettings.uiWhiteNits = g_hdrSettings.paperWhiteNits;
      }
      if (g_hdrSettings.enabled) {
        if (fp16) surfaceFormat = wgpu::TextureFormat::RGBA16Float;
        else Log.warn("HDR requested but the surface offers no RGBA16Float: staying SDR");
      }
    }
  }
  g_vsyncEnabled.store(g_config.vsync, std::memory_order_release);
  auto presentMode = select_present_mode(g_surfaceCapabilities);
  Log.info("Using surface format {} (render format {}), present mode {}", magic_enum::enum_name(surfaceFormat),
           magic_enum::enum_name(renderFormat), magic_enum::enum_name(presentMode));
  const auto size = window::get_window_size();
  g_graphicsConfig = GraphicsConfig{
      .surfaceConfiguration =
          wgpu::SurfaceConfiguration{
              .format = surfaceFormat,
              .usage = wgpu::TextureUsage::RenderAttachment,
              .width = size.native_fb_width,
              .height = size.native_fb_height,
              .presentMode = presentMode,
          },
      .colorFormat = surfaceFormat == wgpu::TextureFormat::RGBA16Float ? surfaceFormat : renderFormat,
      .uiFormat = renderFormat,
      .hdrOutput = surfaceFormat == wgpu::TextureFormat::RGBA16Float && renderFormat != surfaceFormat,
      .depthFormat = wgpu::TextureFormat::Depth32Float,
      .msaaSamples = g_config.msaa,
      .textureAnisotropy = g_config.maxTextureAnisotropy,
  };
  create_copy_pipeline();
  create_resample_pipeline();
  gpu_prof::initialize();
  resize_swapchain(size.fb_width, size.fb_height, size.native_fb_width, size.native_fb_height, true);
  g_initialized = true;
  return true;
}

void shutdown() {
  g_initialized = false;
  gfx::gpu_synchronize();
  gpu_prof::shutdown();
  g_CopyBindGroupLayout = {};
  g_CopyPipeline = {};
  g_CopyPremultipliedAlphaPipeline = {};
  g_UiCompositePipeline = {};
  g_uiLayer = {};
  g_OutputUniformBuffer = {};
  g_copyPipelineFormat = wgpu::TextureFormat::Undefined;
  g_CopyBindGroup = {};
  g_ResampleBindGroupLayout = {};
  g_ResamplePipeline = {};
  g_ResampleUniformBuffer = {};
  g_resampledFrameBuffer = {};
  g_frameBuffer = {};
  g_frameBufferResolved = {};
  g_depthBuffer = {};
  g_normalBuffer = {};
  g_graphicsConfig.normalBuffer = false;
  g_queue = {};
  g_surface = {};
  g_device = {};
  g_adapter = {};
  g_instance = {};
  cache_shutdown();
}

void release_surface() noexcept {
  gfx::gpu_synchronize();
  {
    window::SurfaceLock surfaceLock;
    release_surface_locked();
  }
}

static void resize_swapchain_internal(uint32_t width, uint32_t height, uint32_t nativeWidth, uint32_t nativeHeight,
                                      bool force) {
  if (!g_surface || !g_device || width == 0 || height == 0 || nativeHeight == 0 || nativeWidth == 0) {
    return;
  }
  const bool sizeChanged = g_graphicsConfig.surfaceConfiguration.width != nativeWidth ||
                           g_graphicsConfig.surfaceConfiguration.height != nativeHeight ||
                           g_frameBuffer.size.width != width || g_frameBuffer.size.height != height;
  if (!force && !sizeChanged) {
    return;
  }
  if (sizeChanged) {
    gx::clear_copy_texture_cache();
    gfx::clear_caches();
  }
  g_graphicsConfig.surfaceConfiguration.width = nativeWidth;
  g_graphicsConfig.surfaceConfiguration.height = nativeHeight;
  {   // ShadowRecomp: HDR switched on/off while running (aurora_set_hdr -> RefreshSurface -> here, GPU idle)
    bool want;
    {
      std::lock_guard lk(g_hdrMutex);
      want = g_hdrSettings.enabled && g_hdrFp16Offered;
    }
    // HDR = FP16 everywhere the game draws (EFB, EFB copies, conversions, resample) and an FP16 scRGB swapchain:
    // additive light (glows, explosions, bloom, sun) keeps its real intensity above 1.0 instead of clipping at white.
    const auto fmt = want ? wgpu::TextureFormat::RGBA16Float : g_graphicsConfig.uiFormat;
    if (fmt != g_graphicsConfig.surfaceConfiguration.format) {
      Log.info("Output format {} -> {} (HDR {})", magic_enum::enum_name(g_graphicsConfig.surfaceConfiguration.format),
               magic_enum::enum_name(fmt), want ? "on" : "off");
      g_graphicsConfig.surfaceConfiguration.format = fmt;
    }
    // HDR_EFB8 (test): the game still draws into an 8-bit EFB (GameCube clamping per blend), only the output is HDR
    static const bool efb8 = getenv("HDR_EFB8") != nullptr;
    const auto rfmt = efb8 ? g_graphicsConfig.uiFormat : fmt;
    if (g_graphicsConfig.colorFormat != rfmt) {
      Log.info("Render format {} -> {}", magic_enum::enum_name(g_graphicsConfig.colorFormat), magic_enum::enum_name(rfmt));
      g_graphicsConfig.colorFormat = rfmt;
      gx::clear_copy_texture_cache();      // copy targets are format-bound (re-created on the next copy)
      gfx::clear_caches();
      gfx::tex_copy_conv::shutdown();      // its blit pipeline targets the render format
      gfx::tex_copy_conv::initialize();
      create_resample_pipeline();
      g_resampledFrameBuffer = {};
    }
    g_graphicsConfig.hdrOutput = want;
    if (g_copyPipelineFormat != fmt) create_copy_pipeline();   // its targets are format-bound
    g_uiLayer = {};
  }
  auto surfaceConfiguration = g_graphicsConfig.surfaceConfiguration;
  surfaceConfiguration.device = g_device;
  {
    window::SurfaceLock surfaceLock;
    g_surface.Configure(&surfaceConfiguration);
  }
  g_frameBuffer = create_render_texture(width, height, true);
  g_frameBufferResolved = create_render_texture(width, height, false);
  g_depthBuffer = create_depth_texture(width, height);
  if (g_graphicsConfig.normalBuffer) {
    g_normalBuffer = create_normal_texture(width, height);
  }
  g_CopyBindGroup = create_copy_bind_group(present_source());
}

bool refresh_surface(bool recreate) {
  gfx::gpu_synchronize();
  if (!g_instance || !g_device) {
    return false;
  }
  if (!window::is_presentable()) {
    {
      window::SurfaceLock surfaceLock;
      release_surface_locked();
    }
    return false;
  }
  if ((!g_surface || recreate) && !create_surface()) {
    return false;
  }
  uint32_t width = g_graphicsConfig.surfaceConfiguration.width;
  uint32_t height = g_graphicsConfig.surfaceConfiguration.height;
  uint32_t nativeWidth = width;
  uint32_t nativeHeight = height;
  if (window::get_sdl_window() != nullptr) {
    const auto size = window::get_window_size();
    width = size.fb_width;
    height = size.fb_height;
    nativeWidth = size.native_fb_width;
    nativeHeight = size.native_fb_height;
  }
  if (width != 0 && height != 0) {
    resize_swapchain_internal(width, height, nativeWidth, nativeHeight, true);
  }
  return true;
}

void resize_swapchain(uint32_t width, uint32_t height, uint32_t nativeWidth, uint32_t nativeHeight, bool force) {
  gfx::gpu_synchronize();
  resize_swapchain_internal(width, height, nativeWidth, nativeHeight, force);
}

// ShadowRecomp: the present pass's live parameters, written before every present (cheap: 48 bytes)
void update_output_uniforms() {
  if (!g_OutputUniformBuffer) return;
  AuroraHdrSettings s;
  {
    std::lock_guard lk(g_hdrMutex);
    s = g_hdrSettings;
  }
  OutputUniformBlock u;
  u.hdr = g_graphicsConfig.hdrOutput ? 1u : 0u;
  u.paperWhite = std::max(s.paperWhiteNits, 1.f) / 80.f;
  u.peak = std::max(s.peakNits, s.paperWhiteNits) / 80.f;
  u.highlight = std::clamp(s.highlight, 0.f, 1.f);
  u.saturation = s.saturation;
  u.vibrance = s.vibrance;
  u.contrast = s.contrast > 0.f ? s.contrast : 1.f;
  u.uiWhite = std::max(s.uiWhiteNits > 0.f ? s.uiWhiteNits : s.paperWhiteNits, 1.f) / 80.f;
  u.black = s.blackLevel;
  u.test = s.testPattern ? 1u : 0u;
  {   // #147 colour pass; a zeroed field = its neutral value
    auto nz = [](float v, float d) { return v > 0.f ? v : d; };
    u.colBlack = s.colBlack;
    u.colWhite = nz(s.colWhite, 1.f);
    u.colContrast = nz(s.colContrast, 1.f);
    u.colSaturation = s.colWhite > 0.f ? std::max(s.colSaturation, 0.f) : 1.f;   // zeroed struct: neutral
    u.colVibrance = s.colVibrance;
    u.colR = nz(s.colGain[0], 1.f);
    u.colG = nz(s.colGain[1], 1.f);
    u.colB = nz(s.colGain[2], 1.f);
    u.colGamma = nz(s.colGamma, 1.f);
    u.colExposure = std::exp2(s.colExposure);
    u.colOn = (u.colBlack != 0.f || u.colWhite != 1.f || u.colContrast != 1.f || u.colSaturation != 1.f || u.colVibrance != 0.f ||
               u.colR != 1.f || u.colG != 1.f || u.colB != 1.f || u.colGamma != 1.f || u.colExposure != 1.f) ? 1u : 0u;
  }
  g_queue.WriteBuffer(g_OutputUniformBuffer, 0, &u, sizeof(u));
}

// The PC menus (ImGui) layer for an HDR output: same size as the swapchain, the 8-bit render format, cleared to
// transparent each frame and composited by the present pass (fs_ui) at the UI brightness.
const TextureWithSampler& ui_layer() {
  const uint32_t w = g_graphicsConfig.surfaceConfiguration.width, h = g_graphicsConfig.surfaceConfiguration.height;
  if (!g_uiLayer.view || g_uiLayer.size.width != w || g_uiLayer.size.height != h) {
    g_uiLayer = create_render_texture(w, h, false, g_graphicsConfig.uiFormat);
  }
  return g_uiLayer;
}
} // namespace aurora::webgpu

void aurora_set_hdr(const AuroraHdrSettings* settings) {
  using namespace aurora::webgpu;
  bool formatChange;
  {
    std::lock_guard lk(g_hdrMutex);
    g_hdrSettings = *settings;
    const auto want = settings->enabled && g_hdrFp16Offered ? wgpu::TextureFormat::RGBA16Float : g_graphicsConfig.uiFormat;
    formatChange = g_initialized && want != g_graphicsConfig.surfaceConfiguration.format;
  }
  if (formatChange) aurora::window::push_custom_event(aurora::window::CustomEvent::RefreshSurface);
}
bool aurora_hdr_available() { return aurora::webgpu::g_hdrFp16Offered; }
bool aurora_hdr_active() { return aurora::webgpu::g_graphicsConfig.hdrOutput; }

void aurora_enable_vsync(const bool enabled) {
  aurora::webgpu::g_vsyncEnabled.store(enabled, std::memory_order_release);
  aurora::webgpu::g_graphicsConfig.surfaceConfiguration.presentMode =
      aurora::webgpu::select_present_mode(aurora::webgpu::g_surfaceCapabilities);
  aurora::window::push_custom_event(aurora::window::CustomEvent::RefreshSurface);
}

// ShadowRecomp: HDR frame statistics (testing without an HDR display). aurora_hdr_request_stats() reads the next
// presented frame back (the EFB = present source, FP16 or 8-bit) and logs how much of it is overbright.
namespace aurora::webgpu {
namespace {
std::atomic_bool g_statRequest = false;
wgpu::Buffer g_statBuffer;
uint32_t g_statW = 0, g_statH = 0, g_statRow = 0;
wgpu::TextureFormat g_statFmt = wgpu::TextureFormat::Undefined;
bool g_statSubmitted = false;

float half_to_float(uint16_t h) {
  const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
  float v;
  if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
  else if (e == 31) v = m ? 0.f : 65504.f;
  else v = std::ldexp(static_cast<float>(m | 1024), static_cast<int>(e) - 25);
  return s ? -v : v;
}

void stat_mapped(wgpu::MapAsyncStatus status) {
  if (status != wgpu::MapAsyncStatus::Success) {
    Log.warn("HDR stats: readback failed");
    g_statBuffer = {};
    return;
  }
  const auto* base = static_cast<const uint8_t*>(g_statBuffer.GetConstMappedRange(0, uint64_t(g_statRow) * g_statH));
  const bool fp16 = g_statFmt == wgpu::TextureFormat::RGBA16Float;
  const bool bgra = g_statFmt == wgpu::TextureFormat::BGRA8Unorm;
  double sum = 0;
  float maxc = 0, maxy = 0;
  uint64_t over1 = 0, over2 = 0, at1 = 0, n = 0;
  for (uint32_t y = 0; y < g_statH; ++y) {
    const uint8_t* row = base + uint64_t(y) * g_statRow;
    for (uint32_t x = 0; x < g_statW; ++x) {
      float r, g, b;
      if (fp16) {
        const auto* p = reinterpret_cast<const uint16_t*>(row) + x * 4;
        r = half_to_float(p[0]); g = half_to_float(p[1]); b = half_to_float(p[2]);
      } else {
        const uint8_t* p = row + x * 4;
        r = p[bgra ? 2 : 0] / 255.f; g = p[1] / 255.f; b = p[bgra ? 0 : 2] / 255.f;
      }
      const float mc = std::max(r, std::max(g, b));
      const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
      maxc = std::max(maxc, mc);
      maxy = std::max(maxy, lum);
      sum += lum;
      over1 += mc > 1.001f;
      over2 += mc > 2.f;
      at1 += mc >= 0.999f;
      ++n;
    }
  }
  g_statBuffer.Unmap();
  g_statBuffer = {};
  Log.info("HDR stats {} {}x{}: max channel {:.3f}, max luma {:.3f}, mean luma {:.4f}, >1.0: {:.3f}%, >2.0: {:.3f}%, "
           ">=1.0: {:.3f}% (values are the game's gamma-encoded EFB; 1.0 = SDR white)",
           magic_enum::enum_name(g_statFmt), g_statW, g_statH, maxc, maxy, n ? sum / n : 0.0,
           n ? 100.0 * over1 / n : 0.0, n ? 100.0 * over2 / n : 0.0, n ? 100.0 * at1 / n : 0.0);
}
} // namespace

void hdr_stats_encode(const wgpu::CommandEncoder& encoder) {
  if (!g_statRequest.load() || g_statBuffer) return;
  const auto& src = present_source();
  const bool fp16 = src.format == wgpu::TextureFormat::RGBA16Float;
  if (!src.texture || (!fp16 && src.format != wgpu::TextureFormat::BGRA8Unorm && src.format != wgpu::TextureFormat::RGBA8Unorm)) {
    g_statRequest = false;
    Log.warn("HDR stats: unsupported source format {}", magic_enum::enum_name(src.format));
    return;
  }
  g_statRequest = false;
  g_statW = src.size.width;
  g_statH = src.size.height;
  g_statFmt = src.format;
  g_statRow = AURORA_ALIGN(g_statW * (fp16 ? 8u : 4u), 256u);
  const wgpu::BufferDescriptor desc{
      .label = "HDR stats readback",
      .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
      .size = uint64_t(g_statRow) * g_statH,
  };
  g_statBuffer = g_device.CreateBuffer(&desc);
  const wgpu::TexelCopyTextureInfo from{.texture = src.texture};
  const wgpu::TexelCopyBufferInfo to{.layout = {.bytesPerRow = g_statRow, .rowsPerImage = g_statH}, .buffer = g_statBuffer};
  const wgpu::Extent3D size{g_statW, g_statH, 1};
  encoder.CopyTextureToBuffer(&from, &to, &size);
  g_statSubmitted = true;
}

void hdr_stats_after_submit() {
  if (!g_statSubmitted || !g_statBuffer) return;
  g_statSubmitted = false;
  g_statBuffer.MapAsync(wgpu::MapMode::Read, 0, uint64_t(g_statRow) * g_statH, wgpu::CallbackMode::AllowSpontaneous,
                        [](wgpu::MapAsyncStatus status, wgpu::StringView) { stat_mapped(status); });
}
} // namespace aurora::webgpu

void aurora_hdr_request_stats() { aurora::webgpu::g_statRequest = true; }

// ShadowRecomp: exact per-frame capture (testing temporal bugs - flicker, popping). aurora_capture_frames(n, rect,
// prefix) reads back a region of the next n presented frames (the EFB = present source) and writes them as PPM files
// <prefix>NNN.ppm. Each frame gets its own readback buffer, so the frames are consecutive.
namespace aurora::webgpu {
namespace {
struct FrameCap {
  wgpu::Buffer buf;
  uint32_t w = 0, h = 0, row = 0, index = 0;
  wgpu::TextureFormat fmt = wgpu::TextureFormat::Undefined;
};
std::mutex g_capMutex;
int g_capLeft = 0, g_capIndex = 0;
uint32_t g_capX = 0, g_capY = 0, g_capW = 0, g_capH = 0;
std::string g_capPrefix;
std::vector<FrameCap> g_capEncoded;
float cap_half(uint16_t h) {
  const uint32_t e = (h >> 10) & 31, m = h & 1023;
  float v = e == 0 ? std::ldexp(static_cast<float>(m), -24) : e == 31 ? 65504.f : std::ldexp(static_cast<float>(m | 1024), int(e) - 25);
  return (h >> 15) ? -v : v;
}
void cap_write(const FrameCap& f) {
  const auto* base = static_cast<const uint8_t*>(f.buf.GetConstMappedRange(0, uint64_t(f.row) * f.h));
  char name[512];
  snprintf(name, sizeof name, "%s%03u.ppm", g_capPrefix.c_str(), f.index);
  if (FILE* out = fopen(name, "wb")) {
    fprintf(out, "P6\n%u %u\n255\n", f.w, f.h);
    std::vector<uint8_t> line(size_t(f.w) * 3);
    for (uint32_t y = 0; y < f.h; ++y) {
      const uint8_t* r = base + uint64_t(y) * f.row;
      for (uint32_t x = 0; x < f.w; ++x) {
        uint8_t c[3];
        if (f.fmt == wgpu::TextureFormat::RGBA16Float) {
          const auto* p = reinterpret_cast<const uint16_t*>(r) + x * 4;
          for (int k = 0; k < 3; ++k) c[k] = uint8_t(std::clamp(cap_half(p[k]), 0.f, 1.f) * 255.f + 0.5f);
        } else if (f.fmt == wgpu::TextureFormat::BGRA8Unorm) {
          c[0] = r[x * 4 + 2]; c[1] = r[x * 4 + 1]; c[2] = r[x * 4];
        } else {
          c[0] = r[x * 4]; c[1] = r[x * 4 + 1]; c[2] = r[x * 4 + 2];
        }
        memcpy(&line[x * 3], c, 3);
      }
      fwrite(line.data(), 1, line.size(), out);
    }
    fclose(out);
  }
  if (f.fmt == wgpu::TextureFormat::RGBA16Float) {   // the raw FP16 values too (.pfm, rows bottom-up): HDR measurements
    snprintf(name, sizeof name, "%s%03u.pfm", g_capPrefix.c_str(), f.index);
    if (FILE* out = fopen(name, "wb")) {
      fprintf(out, "PF\n%u %u\n-1.0\n", f.w, f.h);
      std::vector<float> line(size_t(f.w) * 3);
      for (uint32_t y = f.h; y-- > 0;) {
        const auto* p = reinterpret_cast<const uint16_t*>(base + uint64_t(y) * f.row);
        for (uint32_t x = 0; x < f.w * 3; ++x) line[x] = cap_half(p[(x / 3) * 4 + x % 3]);
        fwrite(line.data(), sizeof(float), line.size(), out);
      }
      fclose(out);
    }
  }
  f.buf.Unmap();
}
} // namespace

void frame_capture_encode(const wgpu::CommandEncoder& encoder) {
  std::lock_guard lk(g_capMutex);
  if (g_capLeft <= 0) return;
  const auto& src = present_source();
  if (!src.texture) return;
  const bool fp16 = src.format == wgpu::TextureFormat::RGBA16Float;
  FrameCap f;
  f.fmt = src.format;
  f.w = std::min(g_capW, src.size.width > g_capX ? src.size.width - g_capX : 0u);
  f.h = std::min(g_capH, src.size.height > g_capY ? src.size.height - g_capY : 0u);
  if (!f.w || !f.h) { g_capLeft = 0; return; }
  f.row = AURORA_ALIGN(f.w * (fp16 ? 8u : 4u), 256u);
  f.index = g_capIndex++;
  const wgpu::BufferDescriptor desc{
      .label = "Frame capture readback",
      .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
      .size = uint64_t(f.row) * f.h,
  };
  f.buf = g_device.CreateBuffer(&desc);
  const wgpu::TexelCopyTextureInfo from{.texture = src.texture, .origin = {g_capX, g_capY, 0}};
  const wgpu::TexelCopyBufferInfo to{.layout = {.bytesPerRow = f.row, .rowsPerImage = f.h}, .buffer = f.buf};
  const wgpu::Extent3D size{f.w, f.h, 1};
  encoder.CopyTextureToBuffer(&from, &to, &size);
  g_capEncoded.push_back(std::move(f));
  --g_capLeft;
}

void frame_capture_after_submit() {
  std::vector<FrameCap> todo;
  {
    std::lock_guard lk(g_capMutex);
    todo.swap(g_capEncoded);
  }
  for (auto& f : todo) {
    auto* held = new FrameCap(std::move(f));
    held->buf.MapAsync(wgpu::MapMode::Read, 0, uint64_t(held->row) * held->h, wgpu::CallbackMode::AllowSpontaneous,
                       [held](wgpu::MapAsyncStatus status, wgpu::StringView) {
                         if (status == wgpu::MapAsyncStatus::Success) cap_write(*held);
                         delete held;
                       });
  }
}
} // namespace aurora::webgpu

void aurora_capture_frames(int count, int x, int y, int w, int h, const char* prefix) {
  using namespace aurora::webgpu;
  std::lock_guard lk(g_capMutex);
  g_capLeft = count;
  g_capIndex = 0;
  g_capX = uint32_t(std::max(x, 0));
  g_capY = uint32_t(std::max(y, 0));
  g_capW = uint32_t(std::max(w, 1));
  g_capH = uint32_t(std::max(h, 1));
  g_capPrefix = prefix ? prefix : "frame_";
}
