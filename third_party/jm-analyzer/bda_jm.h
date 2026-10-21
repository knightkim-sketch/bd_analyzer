/*
 * bda_jm.h - C API of libbda-jm-analyzer.so
 *
 * H.264/AVC analysis decoding with the JM reference decoder (JM 19.0, third_party/jm @ ef2362d1) for
 * bd-analyzer. Besides the output pictures, the library exposes per-macroblock syntax/decision data
 * (mb_type, sub_mb_type, intra modes, QP, CBP, transform size, motion vectors, reference indices) and
 * the bit range of every macroblock inside its slice NAL unit.
 *
 * Built by scripts/setup-jm-analyzer.sh. Only the functions below are exported; libstdc++/libgcc are
 * linked statically. The library is meant to be dlopen()ed: check bda_jm_api_version() against
 * BDA_JM_API_VERSION before using any struct (layout changes bump the version).
 *
 * Usage
 *   h = bda_jm_create();
 *   for each chunk of the Annex-B stream:   bda_jm_push(h, data, size);
 *                                           while (bda_jm_pull(h, &pic) == BDA_JM_OK) use(pic);
 *   at the end:                             bda_jm_push(h, NULL, 0);           (end of stream)
 *                                           while (bda_jm_pull(h, &pic) == BDA_JM_OK) use(pic);
 *   bda_jm_destroy(h);
 *
 * Decoding model: JM pulls bytes from the pushed data on its own schedule. It needs to see the start of
 * the next NAL unit (or end of stream) to finish a NAL unit, and the first slice of the next picture
 * (or end of stream) to finish a picture, so pictures come out one or more pushes later than the data
 * that carried them; the DPB then delays them further (output order, like ldecod writes them). Each
 * bda_jm_pull() decodes until the next output picture or until the pushed data is exhausted.
 *
 * Instances: JM keeps its decoder in a process-wide variable (p_Dec) plus one function-static NAL
 * pointer. The library swaps that state per handle on every call and serialises all calls with a
 * process-wide mutex, so several handles may be alive at the same time (e.g. a loading and a caching
 * decoder) and may be used from different threads; they just never decode concurrently.
 *
 * Determinism: output samples are bit-exact with `ldecod -i stream -o out.yuv` (JM 19.0, same
 * compiler flags), except that pictures are always delivered as Y/Cb/Cr planes (ldecod reorders planes
 * when the VUI signals matrix_coefficients == 0 (RGB) and applies tone-mapping SEI only then).
 */
#ifndef BDA_JM_H
#define BDA_JM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define BDA_JM_API __declspec(dllexport)
#else
#define BDA_JM_API __attribute__((visibility("default")))
#endif

#define BDA_JM_API_VERSION 1

typedef struct bda_jm bda_jm;

/* Return codes */
enum {
	BDA_JM_OK = 0,
	BDA_JM_NEED_DATA = 1,         /* bda_jm_pull: all pushed data consumed, push more (or end of stream) */
	BDA_JM_EOS = 2,               /* bda_jm_pull: end of stream reached and every picture delivered */
	BDA_JM_ERR_ARG = -1,          /* bad argument */
	BDA_JM_ERR_DECODE = -2,       /* JM reported a fatal error (see bda_jm_last_error); handle is dead */
	BDA_JM_ERR_STATE = -3,        /* call not valid in the current state (e.g. push after a fatal error) */
	BDA_JM_ERR_UNSUPPORTED = -4,  /* reserved: feature not supported by this build */
	BDA_JM_ERR_NOMEM = -5
};

/* Slice types (values of slice_type % 5, Table 7-6) */
enum { BDA_JM_SLICE_P = 0, BDA_JM_SLICE_B = 1, BDA_JM_SLICE_I = 2, BDA_JM_SLICE_SP = 3, BDA_JM_SLICE_SI = 4 };

/* Chroma format (chroma_format_idc) */
enum { BDA_JM_CHROMA_400 = 0, BDA_JM_CHROMA_420 = 1, BDA_JM_CHROMA_422 = 2, BDA_JM_CHROMA_444 = 3 };

/* Picture structure of an output picture */
enum {
	BDA_JM_STRUCT_FRAME = 0,      /* progressive frame (MB data available unless MBAFF) */
	BDA_JM_STRUCT_FIELD_PAIR = 1  /* frame assembled from field pictures, or an unpaired field padded to a
	                                 frame by JM (no MB data) */
};

/* Normalised macroblock kind. B kinds are BDA_JM_MB_B_DIRECT_16X16 + B mb_type (Table 7-14). */
enum bda_jm_mb_kind {
	BDA_JM_MB_I4X4 = 0,       /* I_NxN, transform_size_8x8_flag = 0 */
	BDA_JM_MB_I8X8 = 1,       /* I_NxN, transform_size_8x8_flag = 1 */
	BDA_JM_MB_I16X16 = 2,
	BDA_JM_MB_IPCM = 3,
	BDA_JM_MB_SI = 4,
	BDA_JM_MB_P_L0_16X16 = 10,
	BDA_JM_MB_P_L0_L0_16X8 = 11,
	BDA_JM_MB_P_L0_L0_8X16 = 12,
	BDA_JM_MB_P_8X8 = 13,
	BDA_JM_MB_P_8X8REF0 = 14,
	BDA_JM_MB_P_SKIP = 15,
	BDA_JM_MB_B_DIRECT_16X16 = 20,
	BDA_JM_MB_B_L0_16X16 = 21,
	BDA_JM_MB_B_L1_16X16 = 22,
	BDA_JM_MB_B_BI_16X16 = 23,
	BDA_JM_MB_B_L0_L0_16X8 = 24,
	BDA_JM_MB_B_L0_L0_8X16 = 25,
	BDA_JM_MB_B_L1_L1_16X8 = 26,
	BDA_JM_MB_B_L1_L1_8X16 = 27,
	BDA_JM_MB_B_L0_L1_16X8 = 28,
	BDA_JM_MB_B_L0_L1_8X16 = 29,
	BDA_JM_MB_B_L1_L0_16X8 = 30,
	BDA_JM_MB_B_L1_L0_8X16 = 31,
	BDA_JM_MB_B_L0_BI_16X8 = 32,
	BDA_JM_MB_B_L0_BI_8X16 = 33,
	BDA_JM_MB_B_L1_BI_16X8 = 34,
	BDA_JM_MB_B_L1_BI_8X16 = 35,
	BDA_JM_MB_B_BI_L0_16X8 = 36,
	BDA_JM_MB_B_BI_L0_8X16 = 37,
	BDA_JM_MB_B_BI_L1_16X8 = 38,
	BDA_JM_MB_B_BI_L1_8X16 = 39,
	BDA_JM_MB_B_BI_BI_16X8 = 40,
	BDA_JM_MB_B_BI_BI_8X16 = 41,
	BDA_JM_MB_B_8X8 = 42,
	BDA_JM_MB_B_SKIP = 43,
	BDA_JM_MB_UNKNOWN = 255   /* not decoded (lost/concealed) */
};

/* Prediction direction of a partition */
enum { BDA_JM_PRED_L0 = 0, BDA_JM_PRED_L1 = 1, BDA_JM_PRED_BI = 2, BDA_JM_PRED_DIRECT = 3, BDA_JM_PRED_INTRA = -1 };

/* One macroblock (raster order, mb_y * mb_width + mb_x). */
typedef struct bda_jm_mb {
	int16_t mb_x, mb_y;      /* macroblock column / row */
	int16_t slice_index;     /* index into bda_jm_picture.slices */
	int16_t mb_type;         /* mb_type value of the table of the slice type: I Table 7-11 (0..25),
	                            P/SP Table 7-13 (0..4 inter, 5..30 intra = I value + 5), B Table 7-14
	                            (0..22 inter, 23..48 intra = I value + 23), SI Table 7-12 (0 SI, 1..26 =
	                            I value + 1); -1 for P_Skip / B_Skip (inferred, no mb_type coded) */
	uint8_t kind;            /* enum bda_jm_mb_kind */
	uint8_t slice_type;      /* BDA_JM_SLICE_* of the slice */
	uint8_t entropy_mode;    /* 0 CAVLC, 1 CABAC */
	uint8_t is_intra;
	uint8_t skip;            /* P_Skip / B_Skip */
	uint8_t transform_8x8;   /* transform_size_8x8_flag */
	int16_t qp;              /* QPY (QP'Y - QpBdOffsetY; negative values possible above 8 bit) */
	int16_t cbp;             /* coded_block_pattern: bits 0..3 luma 8x8 blocks, (cbp >> 4) chroma 0..2.
	                            Intra16x16: derived from mb_type (luma 0 or 15). I_PCM: 47. */
	int8_t intra16x16_mode;  /* Intra16x16PredMode 0..3, -1 otherwise */
	int8_t intra_chroma_mode;/* intra_chroma_pred_mode 0..3, -1 for inter MBs and 4:0:0 / 4:4:4 */
	int8_t intra4x4_mode[16];/* Intra4x4PredMode 0..8 per 4x4 block in RASTER order inside the MB
	                            (index = blk_y * 4 + blk_x, not the spec's zig-zag luma4x4BlkIdx). For I8x8
	                            each entry holds the mode of its 8x8 block. -1 for other MB kinds. */
	int8_t intra8x8_mode[4]; /* Intra8x8PredMode per 8x8 block, raster order (index = y8 * 2 + x8); I8x8 only */
	int8_t sub_mb_type[4];   /* sub_mb_type per 8x8 block (raster order) for P_8x8 / P_8x8ref0 (Table 7-17)
	                            and B_8x8 (Table 7-18); -1 otherwise */
	int8_t part_pred[4];     /* BDA_JM_PRED_* per 8x8 block (raster order); DIRECT for B_Skip,
	                            B_Direct_16x16 and B_Direct_8x8 sub-blocks */
	uint8_t part_w[4];       /* prediction block size inside each 8x8 block in luma samples (16, 8 or 4;
	                            16 = the 8x8 block is part of a 16x16 / 16x8 / 8x16 partition, see mb_part_*) */
	uint8_t part_h[4];
	uint8_t mb_part_w;       /* macroblock partition size: 16x16, 16x8, 8x16 or 8x8; 0 for intra */
	uint8_t mb_part_h;
	uint8_t reserved[2];
	/* Bit range of the macroblock in its slice NAL unit: bit positions counted from the first bit of the
	 * NAL unit header byte, over the escaped NAL bytes (emulation prevention bytes included). Combine with
	 * bda_jm_slice.nal_offset to get absolute stream positions: byte = nal_offset + bit / 8.
	 *   CAVLC: exact bitstream reader position before the MB's first syntax element (an mb_skip_run that
	 *          precedes the MB belongs to it) and after its last one. bit_end(n) == bit_start(n + 1) inside
	 *          a slice; the last MB ends at the rbsp_slice_trailing_bits.
	 *   CABAC: position of the arithmetic decoding engine (bits read into codIOffset: 9 at initialisation,
	 *          +1 per renormalisation bit), i.e. the bits the decoder has consumed; an MB's range covers
	 *          its bins including the end_of_slice_flag that follows it, and the first MB of a slice also
	 *          covers the 9 initialisation bits, so the ranges of a slice are contiguous from
	 *          data_bit_start to the end of the last MB. Arithmetic coding does not align symbols with
	 *          bits, so the split between neighbouring MBs is approximate by up to ~9 bits (the engine's
	 *          9-bit register); the slice's last MB ends within a few bits of rbsp_slice_trailing_bits.
	 * An emulation prevention byte falling inside a range is counted in it; a range that ends exactly in
	 * front of one includes it. Data-partitioned slices (Extended profile, untested): positions refer to
	 * partition A only, residual bits in partitions B/C are not included. */
	int64_t bit_start;
	int64_t bit_end;
} bda_jm_mb;

/* Motion of one 4x4 luma block (raster order, (mb_y * 4 + y) * (mb_width * 4) + mb_x * 4 + x). */
typedef struct bda_jm_block {
	int8_t ref_idx[2];  /* refIdxL0 / refIdxL1, -1 = list not used (intra or single-list prediction) */
	int16_t mv[2][2];   /* [list][0 = x, 1 = y], quarter luma samples; 0 when ref_idx < 0 */
	int32_t ref_poc[2]; /* POC of the referenced picture; 0 when ref_idx < 0 */
} bda_jm_block;

/* One slice of the picture (decoding order). */
typedef struct bda_jm_slice {
	int slice_type;     /* BDA_JM_SLICE_* */
	int entropy_mode;   /* 0 CAVLC, 1 CABAC */
	int first_mb;       /* first_mb_in_slice */
	int num_mbs;        /* macroblocks decoded in this slice */
	int slice_qp;       /* SliceQPY */
	int nal_unit_type;
	int nal_ref_idc;
	int nal_size;       /* bytes of the NAL unit: header + escaped payload, without start code and
	                       trailing_zero_8bits */
	int64_t nal_offset; /* byte offset of the NAL unit header in the pushed byte stream (all bytes pushed
	                       into this handle since bda_jm_create, counted from 0); -1 if unknown */
	int64_t nal_index;  /* index of the NAL unit in the stream (counted from 0, all NAL unit types) */
	int64_t data_bit_start; /* NAL bit position where slice_data() starts (after the slice header and,
	                           for CABAC, cabac_alignment_one_bit); == bit_start of the slice's first MB */
} bda_jm_slice;

/* Planar picture. Samples are uint8_t when bytes_per_sample == 1, else uint16_t (native little endian,
 * LSB aligned); 2 bytes are used for every plane when either bit depth exceeds 8 (as ldecod writes
 * them). Plane order Y, Cb, Cr; Cb/Cr NULL for 4:0:0 (ldecod writes 128-valued chroma planes there). */
typedef struct bda_jm_planes {
	int width;  /* luma width after the SPS cropping window */
	int height; /* luma height after cropping */
	int bit_depth_luma;
	int bit_depth_chroma;
	int bytes_per_sample;
	int chroma_format;  /* BDA_JM_CHROMA_* */
	const void *data[3];
	ptrdiff_t stride[3]; /* bytes */
	int plane_width[3];
	int plane_height[3];
} bda_jm_planes;

typedef struct bda_jm_picture {
	bda_jm_planes planes;
	int poc;               /* PicOrderCnt of the frame */
	int frame_num;
	int is_idr;
	int is_reference;      /* nal_ref_idc != 0 (reference status when the picture was decoded) */
	unsigned slice_types;  /* bit (1 << BDA_JM_SLICE_*) set for every slice type in the picture */
	int structure;         /* BDA_JM_STRUCT_* */
	int64_t decode_index;  /* picture id: position in decoding order (0, 1, ...) among all coded pictures
	                          (each field picture counts once); -1 when JM assembled the output frame from
	                          field pictures or produced it without decoding it (e.g. error concealment) */
	int64_t output_index;  /* position in output order (0, 1, ...) */
	int coded_width;       /* luma size before cropping (multiples of 16) */
	int coded_height;
	int crop_left, crop_right, crop_top, crop_bottom; /* luma samples removed by the cropping window */
	int concealed;         /* JM error concealment touched this picture */

	/* Macroblock data (progressive, non-MBAFF frames only; has_mb_data == 0 otherwise: MBAFF frames,
	 * field pictures, frames assembled from fields and 4:4:4 with separate_colour_plane_flag are not
	 * supported yet, mbs/blocks are NULL). MBs that were not decoded (lost slices) have
	 * kind == BDA_JM_MB_UNKNOWN and all other fields 0.
	 * slices/num_slices are filled for every coded frame (also MBAFF), 0 for frames assembled from
	 * fields. All pointers are valid until the next bda_jm_pull() / bda_jm_destroy(). */
	int has_mb_data;
	int mb_width;          /* PicWidthInMbs */
	int mb_height;         /* FrameHeightInMbs */
	const bda_jm_mb *mbs;          /* mb_width * mb_height */
	const bda_jm_block *blocks;    /* (mb_width * 4) * (mb_height * 4) */
	int num_slices;
	const bda_jm_slice *slices;
} bda_jm_picture;

BDA_JM_API int bda_jm_api_version(void);
BDA_JM_API const char *bda_jm_version_string(void); /* e.g. "JM 19.0 (ef2362d1) + bda wrapper API 1" */

/* Create a decoder. Returns NULL only when out of memory. */
BDA_JM_API bda_jm *bda_jm_create(void);
BDA_JM_API void bda_jm_destroy(bda_jm *h);

/* Process-wide: route JM's console messages (warnings, statistics) to stderr (default off). */
BDA_JM_API void bda_jm_set_logging(int enable);

/* Append Annex-B data: any number of complete or partial NAL units with start codes (the data is
 * consumed as a byte stream; a NAL unit may be split across pushes). size == 0 signals end of stream:
 * the remaining pictures are flushed by the following bda_jm_pull() calls. After the flush (pull returned
 * BDA_JM_EOS) new data may be pushed; it must start a new coded video sequence (IDR). The data is copied.
 * Returns BDA_JM_OK, BDA_JM_ERR_NOMEM, or BDA_JM_ERR_STATE after a fatal error or when data is pushed
 * after the end of stream but before the flush has completed (pull until BDA_JM_EOS first). */
BDA_JM_API int bda_jm_push(bda_jm *h, const uint8_t *data, size_t size);

/* Decode until the next output picture.
 *   BDA_JM_OK          *out filled; owned by the handle, valid until the next pull / destroy
 *   BDA_JM_NEED_DATA   push more data (or end of stream)
 *   BDA_JM_EOS         end of stream pushed and all pictures delivered
 *   BDA_JM_ERR_DECODE  fatal JM error; the handle only accepts bda_jm_destroy() afterwards
 *   BDA_JM_ERR_NOMEM   the decoder stack could not be allocated (handle is dead)
 * After a fatal error a small amount of memory held by the interrupted JM call may leak (62 bytes
 * measured with valgrind on a corrupted stream). */
BDA_JM_API int bda_jm_pull(bda_jm *h, bda_jm_picture *out);

/* Message of the last fatal error on this handle ("" if none). */
BDA_JM_API const char *bda_jm_last_error(bda_jm *h);

#ifdef __cplusplus
}
#endif

#endif /* BDA_JM_H */
