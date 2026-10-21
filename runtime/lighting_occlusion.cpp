// Lighting L2 (roadmap section 7): sky visibility and short-range enclosure from the BAKED per-stage occupancy
// (lighting/stgNNNN.occ, docs/lighting/OCCUPANCY-FORMAT.md, made by tools/stage_editor/lighting_bake.py) instead of
// iteration3's five replays of last frame's geometry. Off-screen and unloaded geometry now occludes, and the per-frame GPU
// cost is one trilinear lookup per volume per pixel (lighting_sky.cpp).
//
// CPU side (this file): the .occ is read and inflated on a worker thread when the stage changes. Around the camera a
// 128 x 40 x 128 volume of 32-unit cells (4096 x 1280 x 4096 units) is baked on the same worker: from each cell centre
// nine directions (up, four at 45 degrees, four at 15 degrees) march 48 .. 3072 units through merged coarse occupancy
// grids (cell size doubling with the distance = a cone), giving
//   sky  = up-weighted transmittance (how much sky the point sees),
//   near = mean transmittance over the first 200 units (corners, alleys, under things: the enclosure / AO term).
// The volume is re-baked when the camera leaves its middle; the result goes into the static storage buffer region the
// old sky maps used (kSkyDepthOffset, 5 MiB) with one queue write, and its placement is published in the same frame.
#include "lighting_occlusion.h"
#include "sys.h"
#include <aurora/gfx.hpp>
#include <aurora/lighting.hpp>
#define NOMINMAX
#include <windows.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace lighting {
namespace {
using namespace aurora::gfx;
namespace al = aurora::lighting;

constexpr uint32_t kDx = 128, kDy = 40, kDz = 128, kCells = kDx * kDy * kDz;
constexpr float kCell = 32.f;
static_assert(2 * kCells <= kOccCapacity, "both volumes must fit the old sky map region");

struct Chunk {
  uint32_t number = 0, D[3]{}, B[3]{}, M = 0, N = 0;
  V3 origin{}, lo{}, hi{};
  float s = 16;
  std::vector<uint8_t> raw;
  const uint32_t* index = nullptr;
  const uint8_t* bricks = nullptr;
  std::vector<const uint8_t*> coarse;            // level k >= 3 (nullptr below)
  std::vector<std::array<uint32_t, 3>> cdim;
  // average occupancy (0..1) of the level-k cell holding world point p (OCCUPANCY-FORMAT.md "Reader sketch")
  float avg(uint32_t k, V3 p) const {
    const float fx = (p.x - origin.x) / s, fy = (p.y - origin.y) / s, fz = (p.z - origin.z) / s;
    if (!(fx >= 0 && fy >= 0 && fz >= 0)) return 0;
    const uint32_t x = uint32_t(fx), y = uint32_t(fy), z = uint32_t(fz);
    if (x >= D[0] || y >= D[1] || z >= D[2]) return 0;
    if (k <= 2 || M <= 3) {
      const uint32_t b = index[(x >> 3) + B[0] * ((y >> 3) + B[1] * (z >> 3))];
      if (b == 0xFFFFFFFFu || b >= N) return 0;
      const uint8_t* br = bricks + 136ull * b;
      const uint32_t lx = x & 7, ly = y & 7, lz = z & 7;
      if (k == 0) { const uint32_t i = lx + 8 * ly + 64 * lz; return float((br[i >> 3] >> (i & 7)) & 1); }
      if (k == 1) return br[64 + (lx >> 1) + 4 * (ly >> 1) + 16 * (lz >> 1)] / 255.f;
      return br[128 + (lx >> 2) + 2 * (ly >> 2) + 4 * (lz >> 2)] / 255.f;
    }
    k = std::min(k, M - 1);
    const auto& d = cdim[k];
    const uint32_t cx = std::min(x >> k, d[0] - 1), cy = std::min(y >> k, d[1] - 1), cz = std::min(z >> k, d[2] - 1);
    return coarse[k][cx + d[0] * (cy + d[1] * cz)] / 255.f;
  }
};

std::string exe_dir() {
  char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
  const std::string p = exe;
  return p.substr(0, p.find_last_of("\\/"));
}
uint32_t u32at(const std::vector<uint8_t>& b, size_t o) { uint32_t v; memcpy(&v, &b[o], 4); return v; }
float f32at(const std::vector<uint8_t>& b, size_t o) { float v; memcpy(&v, &b[o], 4); return v; }

// <exe>\mods\lighting\stgNNNN.occ (a project build's bake) before <exe>\lighting\stgNNNN.occ (the base stages)
bool load(int stage, std::vector<Chunk>& out, std::string& from) {
  char name[32]; snprintf(name, sizeof name, "stg%04d.occ", stage);
  std::vector<uint8_t> f;
  for (const char* dir : {"\\mods\\lighting\\", "\\lighting\\"}) {
    from = exe_dir() + dir + name;
    FILE* fp = fopen(from.c_str(), "rb");
    if (!fp) continue;
    fseek(fp, 0, SEEK_END); const long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (n > 64 && n < (512 << 20)) { f.resize(size_t(n)); if (fread(f.data(), 1, f.size(), fp) != f.size()) f.clear(); }
    fclose(fp);
    if (!f.empty()) break;
  }
  if (f.empty()) return false;
  if (memcmp(f.data(), "SHOCC1", 6) || u32at(f, 8) != 1 || int(u32at(f, 12)) != stage) { logf("[occlusion] %s: not a version 1 file for this stage\n", from.c_str()); return false; }
  const uint32_t C = u32at(f, 16);
  if (C > 256 || 64 + 64ull * C > f.size()) return false;
  for (uint32_t i = 0; i < C; ++i) {
    const size_t r = 64 + 64ull * i;
    Chunk c;
    c.number = u32at(f, r);
    const uint32_t off = u32at(f, r + 4), stored = u32at(f, r + 8), rawsize = u32at(f, r + 60);
    c.origin = {f32at(f, r + 12), f32at(f, r + 16), f32at(f, r + 20)};
    c.s = f32at(f, r + 24);
    for (int a = 0; a < 3; ++a) c.D[a] = u32at(f, r + 28 + 4 * a);
    c.M = u32at(f, r + 40); c.N = u32at(f, r + 44);
    if (uint64_t(off) + stored > f.size() || rawsize > (256u << 20) || !(c.s > 0) || c.M == 0 || c.M > 16) { logf("[occlusion] chunk %u: bad table row\n", c.number); return false; }
    bool dims_ok = true;
    for (int a = 0; a < 3; ++a) { if (!c.D[a] || c.D[a] % 8 || c.D[a] > 8192) dims_ok = false; c.B[a] = c.D[a] / 8; }
    if (!dims_ok) return false;
    c.raw.resize(rawsize);
    uLongf got = rawsize;
    if (uncompress(c.raw.data(), &got, &f[off], stored) != Z_OK || got != rawsize) { logf("[occlusion] chunk %u: inflate failed\n", c.number); return false; }
    const uint64_t nb = uint64_t(c.B[0]) * c.B[1] * c.B[2];
    uint64_t p = 4 * nb + 136ull * c.N;
    if (p > rawsize) return false;
    c.index = reinterpret_cast<const uint32_t*>(c.raw.data());
    c.bricks = c.raw.data() + 4 * nb;
    c.coarse.assign(c.M, nullptr); c.cdim.assign(c.M, {0, 0, 0});
    for (uint32_t k = 3; k < c.M; ++k) {
      p = (p + 3) & ~3ull;
      std::array<uint32_t, 3> d;
      for (int a = 0; a < 3; ++a) d[a] = (c.D[a] + (1u << k) - 1) >> k;
      if (p + uint64_t(d[0]) * d[1] * d[2] > rawsize) { logf("[occlusion] chunk %u: coarse level %u out of the section\n", c.number, k); return false; }
      c.coarse[k] = c.raw.data() + p; c.cdim[k] = d;
      p += uint64_t(d[0]) * d[1] * d[2];
    }
    c.lo = c.origin;
    c.hi = {c.origin.x + c.D[0] * c.s, c.origin.y + c.D[1] * c.s, c.origin.z + c.D[2] * c.s};
    out.push_back(std::move(c));
  }
  return true;
}

// Occupancy of all chunks merged (max) on a world-aligned grid of cell L. A level-k cell of a thin surface shell holds
// 1 / 2^k of its voxels, so the average is scaled by 2^k: "a surface crosses this cell" reads about 1 at every level.
struct Grid {
  V3 o{}; float L = 32; int n[3]{}; std::vector<float> v;
  float at(V3 p) const {
    const int x = int(std::floor((p.x - o.x) / L)), y = int(std::floor((p.y - o.y) / L)), z = int(std::floor((p.z - o.z) / L));
    if (x < 0 || y < 0 || z < 0 || x >= n[0] || y >= n[1] || z >= n[2]) return 0;
    return v[size_t(x) + size_t(n[0]) * (size_t(y) + size_t(n[1]) * size_t(z))];
  }
};
Grid merged(const std::vector<Chunk>& chunks, V3 lo, V3 hi, float L) {
  Grid g; g.L = L;
  g.o = {std::floor(lo.x / L) * L, std::floor(lo.y / L) * L, std::floor(lo.z / L) * L};
  g.n[0] = int(std::ceil((hi.x - g.o.x) / L)); g.n[1] = int(std::ceil((hi.y - g.o.y) / L)); g.n[2] = int(std::ceil((hi.z - g.o.z) / L));
  g.v.assign(size_t(g.n[0]) * g.n[1] * g.n[2], 0.f);
  for (const auto& c : chunks) {
    const int k = std::clamp(int(std::lround(std::log2(L / c.s))), 0, int(c.M) - 1);
    const float gain = float(1u << k);
    int a[3], b[3];
    const float clo[3] = {c.lo.x, c.lo.y, c.lo.z}, chi[3] = {c.hi.x, c.hi.y, c.hi.z}, go[3] = {g.o.x, g.o.y, g.o.z};
    bool empty = false;
    for (int i = 0; i < 3; ++i) {
      a[i] = std::max(0, int(std::floor((clo[i] - go[i]) / L)));
      b[i] = std::min(g.n[i], int(std::ceil((chi[i] - go[i]) / L)));
      if (a[i] >= b[i]) empty = true;
    }
    if (empty) continue;
    for (int z = a[2]; z < b[2]; ++z)
      for (int y = a[1]; y < b[1]; ++y)
        for (int x = a[0]; x < b[0]; ++x) {
          const V3 p{g.o.x + (x + 0.5f) * L, g.o.y + (y + 0.5f) * L, g.o.z + (z + 0.5f) * L};
          float& d = g.v[size_t(x) + size_t(g.n[0]) * (size_t(y) + size_t(g.n[1]) * size_t(z))];
          d = std::max(d, std::min(1.f, c.avg(uint32_t(k), p) * gain));
        }
  }
  return g;
}

constexpr int kSteps = 7, kNearSteps = 3;
constexpr float kDist0 = 48.f;   // step i at 48 * 2^i units, read from the grid of cell 32 * 2^i
struct Dir { V3 d; float sky_w; };
const Dir kDirs[9] = {
  {{0, 1, 0}, 1.f},
  {{0.7071f, 0.7071f, 0}, 0.7071f}, {{-0.7071f, 0.7071f, 0}, 0.7071f}, {{0, 0.7071f, 0.7071f}, 0.7071f}, {{0, 0.7071f, -0.7071f}, 0.7071f},
  {{0.683f, 0.2588f, 0.683f}, 0.2588f}, {{-0.683f, 0.2588f, 0.683f}, 0.2588f}, {{0.683f, 0.2588f, -0.683f}, 0.2588f}, {{-0.683f, 0.2588f, -0.683f}, 0.2588f}};

void bake(const std::vector<Chunk>& chunks, V3 origin, float* sky, float* near_) {
  const V3 hi{origin.x + kDx * kCell, origin.y + kDy * kCell, origin.z + kDz * kCell};
  Grid g[kSteps];
  for (int i = 0; i < kSteps; ++i) {
    const float reach = kDist0 * float(1 << i) + kCell * float(1 << i);
    g[i] = merged(chunks, {origin.x - reach, origin.y - reach, origin.z - reach}, {hi.x + reach, hi.y + reach, hi.z + reach}, kCell * float(1 << i));
  }
  float wsum = 0;
  for (const auto& d : kDirs) wsum += d.sky_w;
  for (uint32_t z = 0; z < kDz; ++z)
    for (uint32_t y = 0; y < kDy; ++y)
      for (uint32_t x = 0; x < kDx; ++x) {
        const V3 c{origin.x + (x + 0.5f) * kCell, origin.y + (y + 0.5f) * kCell, origin.z + (z + 0.5f) * kCell};
        float s = 0, n = 0;
        for (const auto& d : kDirs) {
          float t = 1;
          for (int i = 0; i < kSteps; ++i) {
            t *= 1.f - 0.85f * g[i].at(c + d.d * (kDist0 * float(1 << i)));
            if (i == kNearSteps - 1) n += t;
            if (t < 0.01f) { if (i < kNearSteps - 1) n += 0; break; }
          }
          s += t * d.sky_w;
        }
        const size_t k = size_t(x) + kDx * (size_t(y) + kDy * size_t(z));
        sky[k] = s / wsum;
        near_[k] = n / 9.f;
      }
}

// ---- worker + upload ----
struct State {
  int stage = -1;                       // the stage the loaded chunks are for (worker side)
  std::vector<Chunk> chunks;            // worker side only
  std::vector<float> data;              // 2 * kCells: sky then near, written by the worker, read by the upload task
  std::atomic<int> busy{0};             // 1 while the worker runs
  std::atomic<bool> done{false};        // a finished bake waits for its upload
  std::atomic<bool> in_flight{false};   // its upload task is queued (data must not change)
  std::atomic<uint32_t> gen{0};         // a stage change discards running work
  uint32_t job_gen = 0;
  V3 job_origin{};
  bool job_ok = false;
  std::string from;
  double ms = 0;
} g;
OccVolume g_vol, g_pending;
V3 g_center{};
bool g_have_center = false;
int g_req_stage = -1;
bool g_missing = false;                 // no file for this stage (worker said so): do not ask again
EncoderTaskId g_task = InvalidEncoderTask;
struct Upload { const float* data; uint32_t count; };
// OCC_TRACE=1: one line per phase with the host time in ms (freeze hunt); OCC_NOBAKE=1: load the file, never bake;
// OCC_NOUPLOAD=1: bake, never upload (the volume stays invalid)
bool occ_trace() { static const bool t = getenv("OCC_TRACE") != nullptr; return t; }
// worker / render-worker side: real time only (never sys_time_ns: that is the GAME clock)
uint64_t real_ns() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
double host_ms() { return double(real_ns()) * 1e-6; }
void upload(const EncoderTaskContext& ctx, const wgpu::CommandEncoder&, const void* payload, size_t size, void*) {
  if (size != sizeof(Upload)) return;
  Upload u; memcpy(&u, payload, sizeof u);
  if (occ_trace()) logf("[occ-trace] %.0f upload begin\n", host_ms());
  ctx.queue.WriteBuffer(al::shadow_buffer(), uint64_t(kSkyDepthOffset) * 4, u.data, uint64_t(u.count) * 4);
  if (occ_trace()) logf("[occ-trace] %.0f upload end\n", host_ms());
  g.in_flight = false;
}

void start_job(int stage, V3 origin) {
  g.busy = 1; g.done = false;
  const uint32_t gen = g.gen;
  std::thread([stage, origin, gen] {
    const uint64_t t0 = real_ns();
    bool ok = true;
    if (occ_trace()) logf("[occ-trace] %.0f job start stage %d\n", host_ms(), stage);
    if (g.stage != stage) {
      g.chunks.clear(); g.stage = stage;
      size_t bytes = 0;
      ok = load(stage, g.chunks, g.from);
      for (const auto& c : g.chunks) bytes += c.raw.size();
      if (ok) logf("[occlusion] %s: %zu chunks, %.1f MB inflated, %.0f ms\n", g.from.c_str(), g.chunks.size(), bytes / 1048576.0, (real_ns() - t0) * 1e-6);
      else { g.chunks.clear(); logf("[occlusion] stage %d: no usable baked occupancy (lighting\\stg%04d.occ) - no sky / AO occlusion\n", stage, stage); }
    }
    static const bool nobake = getenv("OCC_NOBAKE") != nullptr;
    if (ok && !g.chunks.empty() && !nobake) {
      g.data.resize(2 * size_t(kCells));
      bake(g.chunks, origin, g.data.data(), g.data.data() + kCells);
    }
    if (nobake) ok = false;
    if (occ_trace()) logf("[occ-trace] %.0f job end (%.0f ms)\n", host_ms(), (real_ns() - t0) * 1e-6);
    g.job_gen = gen; g.job_origin = origin; g.job_ok = ok && !g.chunks.empty();
    g.ms = (real_ns() - t0) * 1e-6;
    g.done = true;
    g.busy = 0;
  }).detach();
}
}  // namespace

const OccVolume& occlusion_update(const Cam& cam, int stage, bool wanted) {
  if (stage != g_req_stage) { g_req_stage = stage; g.gen++; g_vol.valid = false; g_have_center = false; g_missing = false; }
  if (!wanted || stage <= 0) return g_vol;
  if (g_task == InvalidEncoderTask) g_task = register_encoder_task_type({.label = "occlusion upload", .callback = upload});
  if (g_task == InvalidEncoderTask) return g_vol;
  // a finished bake: discard if stale, else upload and publish (same frame: the queue write lands before this frame's draws)
  if (g.done && !g.busy && !g.in_flight) {
    g.done = false;
    if (g.job_gen != g.gen) { g_have_center = false; }
    else if (!g.job_ok) { g_missing = true; }
    else {
      static const bool noupload = getenv("OCC_NOUPLOAD") != nullptr;
      if (noupload) { g_missing = true; return g_vol; }
      g.in_flight = true;
      const Upload u{g.data.data(), 2 * kCells};
      push_encoder_task(g_task, &u, sizeof u);
      if (occ_trace()) logf("[occ-trace] %.0f upload pushed\n", host_ms());
      g_vol.valid = true; g_vol.origin = g.job_origin; g_vol.cell = kCell;
      g_vol.dims[0] = kDx; g_vol.dims[1] = kDy; g_vol.dims[2] = kDz; g_vol.count = kCells;
      static const bool log = getenv("OCC_LOG") != nullptr;
      static uint32_t logged_gen = ~0u;
      if (log || logged_gen != g.gen) logf("[occlusion] volume around (%.0f,%.0f,%.0f) baked in %.0f ms\n", g_center.x, g_center.y, g_center.z, g.ms);
      logged_gen = g.gen;
    }
  }
  if (g_missing || g.busy || g.done || g.in_flight) return g_vol;
  // (re)bake when the camera has left the middle of the volume (or none yet)
  const V3 snap{std::round(cam.p.x / 256.f) * 256.f, std::round(cam.p.y / 128.f) * 128.f, std::round(cam.p.z / 256.f) * 256.f};
  const bool away = !g_have_center || std::fabs(cam.p.x - g_center.x) > 1024 || std::fabs(cam.p.z - g_center.z) > 1024 || std::fabs(cam.p.y - g_center.y) > 320;
  if (away && std::isfinite(snap.x) && std::isfinite(snap.y) && std::isfinite(snap.z)) {
    g_center = snap; g_have_center = true;
    start_job(stage, {snap.x - kDx * kCell / 2, snap.y - kDy * kCell / 2, snap.z - kDz * kCell / 2});
  }
  return g_vol;
}
}  // namespace lighting
