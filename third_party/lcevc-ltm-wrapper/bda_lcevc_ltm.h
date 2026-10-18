/*
 * bda_lcevc_ltm.h - C API of libbda-lcevc-ltm.so
 *
 * LCEVC (MPEG-5 Part 2) enhancement decoding with the MPEG LCEVC Test Model (LTM, BSD-3,
 * third_party/ltm @ 452a9869) for bd-analyzer. The base picture is decoded by the caller (any base
 * codec); this library only applies the enhancement layer and exposes the intermediate surfaces and
 * the signalled configuration for analysis.
 *
 * Built by scripts/setup-lcevc-ltm.sh. Only the functions below are exported; libstdc++/libgcc are
 * linked statically. The library is meant to be dlopen()ed: check bda_ltm_api_version() against
 * BDA_LTM_API_VERSION before using any struct (layout changes bump the version).
 *
 * Threading: one bda_ltm handle per stream. Calls on different handles may run on different threads;
 * the library serialises decoding internally because LTM keeps process-wide state.
 *
 * Determinism: dithering is OFF by default (LTM --dithering_switch 0), so the output is bit-exact
 * with `ModelDecoder --dithering_switch 0`.
 */
#ifndef BDA_LCEVC_LTM_H
#define BDA_LCEVC_LTM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define BDA_LTM_API __declspec(dllexport)
#else
#define BDA_LTM_API __attribute__((visibility("default")))
#endif

#define BDA_LTM_API_VERSION 1

typedef struct bda_ltm bda_ltm;

/* Return codes */
enum {
	BDA_LTM_OK = 0,
	BDA_LTM_NOT_AVAILABLE = 1, /* bda_ltm_get_surface: surface not produced for this picture */
	BDA_LTM_ERR_ARG = -1,      /* bad argument / unsupported format */
	BDA_LTM_ERR_NO_LCEVC = -2, /* no LCEVC NAL unit found in the payload */
	BDA_LTM_ERR_MISMATCH = -3, /* base picture does not match the signalled configuration */
	BDA_LTM_ERR_DECODE = -4,   /* LTM raised an error (see bda_ltm_last_error) */
	BDA_LTM_ERR_STATE = -5     /* no picture decoded yet */
};

/* How `payload` passed to bda_ltm_decode() is framed. */
enum bda_ltm_payload_format {
	/* LTM's internal packet: the bytes that follow the 2-byte LCEVC NAL unit header, with emulation
	 * prevention bytes (00 00 03) removed AND the final RBSP stop byte (0x80) removed. This is exactly
	 * what lctm::Decoder::initialize_decode() consumes. is_idr must be 0 or 1. */
	BDA_LTM_PAYLOAD_RAW = 0,
	/* One LCEVC NAL unit as stored in the stream, WITHOUT start code: 2-byte header
	 * (forbidden_zero=0, forbidden_one=1, nal_unit_type 28 non-IDR / 29 IDR, reserved=0x1FF) followed by
	 * the escaped payload including the stop byte. Unescaping and stop-byte removal are done by LTM's
	 * own ScanEnhancement code. is_idr < 0 takes IDR from nal_unit_type. */
	BDA_LTM_PAYLOAD_NAL = 1,
	/* Annex-B byte stream (start-code delimited, e.g. a whole access unit of a .lvc file). The first
	 * LCEVC NAL unit found by LTM's scanner is used, other NAL units are ignored. is_idr < 0 takes IDR
	 * from nal_unit_type. */
	BDA_LTM_PAYLOAD_ANNEXB = 2
};

/* Chroma format codes (same values as LTM Colourspace / chroma_format_idc). */
enum { BDA_LTM_CHROMA_400 = 0, BDA_LTM_CHROMA_420 = 1, BDA_LTM_CHROMA_422 = 2, BDA_LTM_CHROMA_444 = 3 };

/* Planar picture. Samples are uint8_t when bit_depth == 8, else uint16_t (LSB aligned).
 * Strides are in bytes. Plane order Y, U, V; U/V unused for 4:0:0. */
typedef struct bda_ltm_planes {
	int width;  /* luma width */
	int height; /* luma height */
	int bit_depth;
	int chroma_format; /* BDA_LTM_CHROMA_* */
	const void *data[3];
	ptrdiff_t stride[3];
	int plane_width[3];  /* output only (ignored on input) */
	int plane_height[3]; /* output only (ignored on input) */
} bda_ltm_planes;

/* Intermediate surfaces. Names follow LTM's --dump_surfaces files (P<n> = plane n). LTM LOQ naming:
 * LOQ-1 = enhancement sub-layer 1 (intermediate resolution), LOQ-0 = sub-layer 2 (full resolution). */
enum bda_ltm_surface_id {
	BDA_LTM_SURF_BASE_DECO = 0,       /* dec_base_deco_P*      base after bit-depth conversion (only when LOQ-1 scaling != none) */
	BDA_LTM_SURF_BASE_PRED = 1,       /* dec_base_pred_P*      base upsampled to LOQ-1 (+ predicted residual) */
	BDA_LTM_SURF_LOQ1_RESIDUAL = 2,   /* dec_base_resi_reco_P* LOQ-1 residuals after dequant/transform/deblocking */
	BDA_LTM_SURF_LOQ0_PRED = 3,       /* dec_full_pred_P*      LOQ-1 reconstruction upsampled to full resolution */
	BDA_LTM_SURF_LOQ0_RESIDUAL = 4,   /* dec_full_resi_reco_P* LOQ-0 residuals (before temporal accumulation) */
	BDA_LTM_SURF_TEMPORAL_BUFFER = 5, /* dec_full_temp_buff_P* temporal buffer after this picture's update */
	BDA_LTM_SURF_TEMPORAL_MASK = 6,   /* dec_full_temp_mask_P* per-transform-block mask (uint8: 128 = predict, 192 = intra/refresh) */
	BDA_LTM_SURF_COUNT = 7
};

#define BDA_LTM_DUMP_ALL ((1u << BDA_LTM_SURF_COUNT) - 1u)

/* A captured surface, owned by the handle and valid until the next bda_ltm_decode()/destroy.
 * bytes_per_sample == 2: int16_t in LTM's internal S15 fixed point, independent of bit depth.
 *   Picture surfaces (BASE_DECO, BASE_PRED, LOQ0_PRED): internal = (sample << (15 - depth)) - 0x4000,
 *   so sample = (internal + 0x4000 + (1 << (14 - depth))) >> (15 - depth), clamped.
 *   Residual / temporal-buffer surfaces are signed differences on the same scale (divide by
 *   1 << (15 - depth) to get sample units).
 * bytes_per_sample == 1: uint8_t (TEMPORAL_MASK). */
typedef struct bda_ltm_surface {
	int width;
	int height;
	int bytes_per_sample;
	const void *data;
	ptrdiff_t stride; /* bytes */
} bda_ltm_surface;

/* Signalled configuration of the last decoded picture (LTM SignaledConfiguration). Enum-valued fields
 * carry the LTM enum value (src/Types.hpp), noted per field. */
typedef struct bda_ltm_picture_info {
	/* sequence_config */
	int profile_idc;
	int level_idc;
	int sublevel_idc;
	int conformance_window;
	int conf_win_offset[4]; /* left, right, top, bottom (in crop units) */
	/* global_config */
	int resolution_width; /* enhanced resolution before conformance cropping */
	int resolution_height;
	int enhanced_width; /* output size after conformance cropping */
	int enhanced_height;
	int base_width;
	int base_height;
	int base_depth;
	int enhancement_depth;
	int chroma_format;        /* BDA_LTM_CHROMA_* */
	int num_image_planes;
	int num_processed_planes;
	int num_residual_layers;  /* 4 (DD) or 16 (DDS) */
	int transform_block_size; /* 2 = DD (2x2), 4 = DDS (4x4) */
	int scaling_mode[2];      /* [0] LOQ-1, [1] LOQ-0: 0 none, 1 1D (horizontal), 2 2D */
	int upsample;             /* 0 nearest, 1 linear, 2 cubic, 3 modified cubic, 4 adaptive cubic */
	int upsampling_coefficients[4];
	int predicted_residual_enabled;
	int temporal_enabled;
	int temporal_tile_intra_signalling_enabled;
	int temporal_step_width_modifier;
	int level1_filtering_coeff[2];
	int level1_depth_flag;
	int chroma_step_width_multiplier;
	int tile_dimensions_type; /* 0 none, 1 512x256, 2 1024x512, 3 custom */
	int tile_width;
	int tile_height;
	int user_data_enabled;    /* 0 none, 1 2 bits, 2 6 bits */
	/* picture_config */
	int is_idr;
	int enhancement_enabled;  /* 0 = no_enhancement_bit_flag set */
	int temporal_refresh;
	int temporal_signalling_present;
	int picture_type;         /* 0 frame, 1 field */
	int field_type;           /* 0 top, 1 bottom */
	int step_width[2];        /* [0] LOQ-1 (sub-layer 1), [1] LOQ-0 (sub-layer 2), as signalled */
	int dithering_control;
	int dithering_type;       /* 0 none, 1 uniform, 2 uniform fixed */
	int dithering_strength;
	int dequant_offset_signalled;
	int dequant_offset_mode;
	int dequant_offset;
	int level1_filtering_enabled;
	int quant_matrix_mode;
	/* decoder state */
	uint32_t surfaces_captured; /* bit i set: surface id i was captured for this picture */
	int frame_index;            /* number of pictures decoded before this one */
} bda_ltm_picture_info;

BDA_LTM_API int bda_ltm_api_version(void);
BDA_LTM_API const char *bda_ltm_version_string(void); /* e.g. "LTM 452a9869 + bda wrapper 1" */

BDA_LTM_API bda_ltm *bda_ltm_create(void);
BDA_LTM_API void bda_ltm_destroy(bda_ltm *h);

/* Options (take effect from the next bda_ltm_decode). Defaults: dithering off, fixed seed off,
 * enhancement applied. */
BDA_LTM_API void bda_ltm_set_dithering(bda_ltm *h, int enable, int fixed_seed);
BDA_LTM_API void bda_ltm_set_apply_enhancement(bda_ltm *h, int enable);
/* Process-wide: route LTM INFO/WARN log lines to stderr (default off). */
BDA_LTM_API void bda_ltm_set_logging(int enable);

/* Decode one picture: parse `payload`, apply it to `base` and return the enhanced picture.
 *   format     BDA_LTM_PAYLOAD_*
 *   is_idr     1/0, or -1 to take it from the NAL unit type (not allowed with BDA_LTM_PAYLOAD_RAW)
 *   base       caller's decoded base picture: luma size, bit depth and chroma format must match the
 *              signalled base (bda_ltm_get_picture_info after the call shows the expected values)
 *   out        filled with planes owned by the handle (valid until the next decode/destroy), may be NULL
 *   dump_mask  bit i = capture surface id i (BDA_LTM_DUMP_ALL for all), 0 = none
 * Pictures must be fed in output (display) order, the same order LTM's ModelDecoder uses.
 * After BDA_LTM_ERR_DECODE the decoder state (temporal buffer, configuration) is undefined: destroy
 * the handle and start again from an IDR / global-config picture. */
BDA_LTM_API int bda_ltm_decode(bda_ltm *h, const uint8_t *payload, size_t size, int format, int is_idr,
                               const bda_ltm_planes *base, bda_ltm_planes *out, unsigned dump_mask);

/* Captured intermediate surface of the last decoded picture. Returns BDA_LTM_NOT_AVAILABLE when the
 * surface was not requested or not produced (e.g. temporal disabled, plane not processed). */
BDA_LTM_API int bda_ltm_get_surface(bda_ltm *h, int which, int plane, bda_ltm_surface *out);

BDA_LTM_API int bda_ltm_get_picture_info(bda_ltm *h, bda_ltm_picture_info *out);

/* Message of the last failing call on this handle ("" if none). */
BDA_LTM_API const char *bda_ltm_last_error(bda_ltm *h);

#ifdef __cplusplus
}
#endif

#endif /* BDA_LCEVC_LTM_H */
