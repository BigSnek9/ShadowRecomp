#ifndef AURORA_GFX_H
#define AURORA_GFX_H

#ifdef __cplusplus
#include <cstdint>

extern "C" {
#else
#include "stdint.h"
#endif

#if !defined(NDEBUG) && !defined(AURORA_GFX_DEBUG_GROUPS)
#define AURORA_GFX_DEBUG_GROUPS
#endif

void push_debug_group(const char* label);
void pop_debug_group();

typedef struct {
  uint32_t queuedPipelines;
  uint32_t createdPipelines;
  uint32_t drawCallCount;
  uint32_t mergedDrawCallCount;
  uint32_t lastVertSize;
  uint32_t lastUniformSize;
  uint32_t lastIndexSize;
  uint32_t lastStorageSize;
  uint32_t lastTextureUploadSize;
} AuroraStats;

const AuroraStats* aurora_get_stats();
float aurora_get_fps();

void aurora_enable_vsync(bool enabled);

/* ShadowRecomp: HDR output, changeable at any time. `enabled` switches the swapchain between the render format and
 * FP16 scRGB (applied at the next surface refresh, a frame or two later); the other fields are read every present.
 * Nits are absolute (scRGB 1.0 = 80 nits). Neutral grading: saturation 1, vibrance 0, contrast 1, blackLevel 0. */
typedef struct {
  bool enabled;
  bool testPattern;       /* calibration pattern instead of the game (while adjusting) */
  float paperWhiteNits;   /* the game's white */
  float peakNits;         /* display peak: highlight expansion target and soft-clip limit */
  float highlight;        /* 0..1: how far the brightest parts are pushed toward the peak */
  float saturation;
  float vibrance;
  float contrast;
  float blackLevel;       /* -0.05..0.05: crush (<0) or lift (>0) the darkest tones */
  float uiWhiteNits;      /* PC menus brightness (0 = paper white) */
  /* #147 colour pass, SDR and HDR, before the HDR grade. Neutral (= skipped, the original picture): black 0, white 1,
   * contrast 1, saturation 1, vibrance 0, gains 1, gamma 1, exposure 0 (stops). A zeroed struct counts as neutral. */
  float colBlack, colWhite, colContrast, colSaturation, colVibrance, colGain[3], colGamma, colExposure;
} AuroraHdrSettings;
void aurora_set_hdr(const AuroraHdrSettings* settings);
bool aurora_hdr_available(void); /* the window surface offers FP16 (Windows: also needs Windows HDR on to look HDR) */
bool aurora_hdr_active(void);    /* the swapchain is FP16 scRGB right now */
void aurora_hdr_request_stats(void);
/* testing: write a region of the next `count` presented frames to <prefix>NNN.ppm (consecutive frames) */
void aurora_capture_frames(int count, int x, int y, int w, int h, const char* prefix);
void aurora_hdr_request_stats_unused(void); /* testing: log overbright statistics of the next presented frame */

#ifdef __cplusplus
}
#endif

#endif
