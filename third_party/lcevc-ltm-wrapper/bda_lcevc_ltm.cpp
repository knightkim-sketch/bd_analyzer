// bda_lcevc_ltm.cpp - C wrapper around the LTM (MPEG LCEVC Test Model) decoder for bd-analyzer.
//
// The decode path mirrors LTM's DecoderApp::push_base_enhancement (src/ModelDecoderApp.cpp): parse the
// enhancement packet with Decoder::initialize_decode, set the IDR flag, wrap the external base planes
// in an lctm::Image of the signalled base format and call Decoder::decode. Intermediate surfaces are
// captured in memory through the dump hook added by patches/0001-library-build-hooks.patch.
//
// Built by scripts/setup-lcevc-ltm.sh together with the LTM decoder objects (BDA_LTM_LIBRARY defined).

#include "bda_lcevc_ltm.h"

#include "BitstreamStatistic.hpp"
#include "Decoder.hpp"
#include "Diagnostics.hpp"
#include "Image.hpp"
#include "Packet.hpp"
#include "ScanEnhancement.hpp"
#include "SignaledConfiguration.hpp"
#include "Surface.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef BDA_LTM_SRC_REV
#define BDA_LTM_SRC_REV "unknown"
#endif

// Process-wide objects that LTM's decoder sources reference and that ModelDecoderApp.cpp normally
// defines. Only Decoder.cpp's report path (disabled by BDA_LTM_LIBRARY) and BITSTREAM_DEBUG builds use
// them.
BitstreamStatistic goStat;
FILE *goBits = nullptr;
PsnrStatistic goPsnr;
uint8_t gaucMd5Digest[lctm::MAX_NUM_PLANES][16];
ReportStructure goReportStructure;
std::priority_queue<ReportStructure, std::vector<ReportStructure, std::allocator<ReportStructure>>, ReportStructureComp>
    goReportQueue;

// Target of BDA_LTM_ASSERT (bda_ltm_assert.h): LTM's assert() checks throw instead of aborting.
void bda_ltm_assert_failed(const char *expr, const char *file, int line) {
	throw std::runtime_error(lctm::format("%s:%d assertion failed: %s", file, line, expr));
}

namespace lctm {
// Defined in the patched Surface.cpp / Diagnostics.cpp.
extern void (*bda_dump_hook)(const std::string &name, const Surface &surface);
extern bool bda_log_enabled;
} // namespace lctm

using namespace lctm;

namespace {

// LTM keeps global state (dump hook, statistics, logging), so decoding is serialised.
std::mutex g_decode_mutex;

struct CapturedSurface {
	bool valid = false;
	int width = 0;
	int height = 0;
	int bytes_per_sample = 0;
	std::vector<uint8_t> data; // tightly packed rows
};

const char *const kSurfaceNames[BDA_LTM_SURF_COUNT] = {
    "dec_base_deco_P", "dec_base_pred_P", "dec_base_resi_reco_P", "dec_full_pred_P",
    "dec_full_resi_reco_P", "dec_full_temp_buff_P", "dec_full_temp_mask_P",
};

} // namespace

struct bda_ltm {
	Decoder decoder;
	bool dithering = false;
	bool dithering_fixed = false;
	bool apply_enhancement = true;
	int frame_index = 0;
	bool have_picture = false;

	SignaledConfiguration config;
	Dimensions dims;
	bool is_idr = false;

	std::vector<uint8_t> out_planes[MAX_NUM_PLANES];
	bda_ltm_planes out_desc;

	unsigned dump_mask = 0;
	CapturedSurface surfaces[BDA_LTM_SURF_COUNT][MAX_NUM_PLANES];

	std::string last_error;

	bda_ltm() { memset(&out_desc, 0, sizeof(out_desc)); }
};

namespace {

// The handle whose decode is in progress (guarded by g_decode_mutex).
bda_ltm *g_capture_target = nullptr;

void copy_surface(const Surface &s, CapturedSurface &dst) {
	const unsigned bpp = s.bpp();
	const SurfaceView<uint8_t> view(s);
	const size_t row = (size_t)s.width() * bpp;
	dst.width = (int)s.width();
	dst.height = (int)s.height();
	dst.bytes_per_sample = (int)bpp;
	dst.data.resize(row * s.height());
	for (unsigned y = 0; y < s.height(); ++y)
		memcpy(dst.data.data() + y * row, view.data(0, y), row);
	dst.valid = true;
}

void dump_hook(const std::string &name, const Surface &surface) {
	bda_ltm *h = g_capture_target;
	if (!h || surface.empty())
		return;
	for (int id = 0; id < BDA_LTM_SURF_COUNT; ++id) {
		if (!(h->dump_mask & (1u << id)))
			continue;
		const size_t n = strlen(kSurfaceNames[id]);
		if (name.size() == n + 1 && name.compare(0, n, kSurfaceNames[id]) == 0) {
			const int plane = name[n] - '0';
			if (plane >= 0 && plane < MAX_NUM_PLANES)
				copy_surface(surface, h->surfaces[id][plane]); // deep copy: LTM updates buffers in place
			return;
		}
	}
}

int fail(bda_ltm *h, int code, const std::string &msg) {
	h->last_error = msg;
	return code;
}

// Turn the caller's payload into LTM's packet (see bda_ltm_payload_format).
bool extract_packet(const uint8_t *payload, size_t size, int format, Packet &packet, bool &nal_idr) {
	if (format == BDA_LTM_PAYLOAD_RAW) {
		packet = Packet::build().contents(payload, (unsigned)size).finish();
		return true;
	}

	std::vector<uint8_t> buf;
	if (format == BDA_LTM_PAYLOAD_NAL) {
		static const uint8_t start_code[3] = {0, 0, 1};
		buf.insert(buf.end(), start_code, start_code + 3);
	}
	buf.insert(buf.end(), payload, payload + size);
	// LTM's scanner (scan_enhancement_nal) ends a NAL unit at the next start code or at the buffer end, and
	// its loop bound `data_size - 4` underflows once the extracted NAL unit is cut out of a short buffer.
	// A trailing non-LCEVC start code delimits the last NAL unit exactly as the next NAL unit of an access
	// unit would, and keeps enough bytes after the cut.
	static const uint8_t sentinel[8] = {0, 0, 1, 0, 0, 0, 0, 0};
	buf.insert(buf.end(), sentinel, sentinel + sizeof(sentinel));

	bool found = false;
	scan_enhancement(buf.data(), buf.size(), Encapsulation_None, BaseCoding_YUV, 0, false,
	                 [&](const Packet &pkt, const bool is_lcevc_idr) {
		                 if (!found) {
			                 packet = pkt;
			                 nal_idr = is_lcevc_idr;
			                 found = true;
		                 }
	                 });
	return found;
}

} // namespace

extern "C" {

int bda_ltm_api_version(void) { return BDA_LTM_API_VERSION; }

const char *bda_ltm_version_string(void) {
	static const std::string s = std::string("LTM ") + BDA_LTM_SRC_REV + " + bda wrapper API " + std::to_string(BDA_LTM_API_VERSION);
	return s.c_str();
}

bda_ltm *bda_ltm_create(void) {
	try {
		return new bda_ltm();
	} catch (...) {
		return nullptr;
	}
}

void bda_ltm_destroy(bda_ltm *h) { delete h; }

void bda_ltm_set_dithering(bda_ltm *h, int enable, int fixed_seed) {
	if (!h)
		return;
	h->dithering = enable != 0;
	h->dithering_fixed = fixed_seed != 0;
}

void bda_ltm_set_apply_enhancement(bda_ltm *h, int enable) {
	if (h)
		h->apply_enhancement = enable != 0;
}

void bda_ltm_set_logging(int enable) {
	std::lock_guard<std::mutex> lock(g_decode_mutex);
	bda_log_enabled = enable != 0;
}

int bda_ltm_decode(bda_ltm *h, const uint8_t *payload, size_t size, int format, int is_idr, const bda_ltm_planes *base,
                   bda_ltm_planes *out, unsigned dump_mask) {
	if (!h)
		return BDA_LTM_ERR_ARG;
	h->last_error.clear();
	if (!payload || size == 0 || !base)
		return fail(h, BDA_LTM_ERR_ARG, "null payload or base");
	if (format < BDA_LTM_PAYLOAD_RAW || format > BDA_LTM_PAYLOAD_ANNEXB)
		return fail(h, BDA_LTM_ERR_ARG, "unknown payload format");
	if (format == BDA_LTM_PAYLOAD_RAW && is_idr < 0)
		return fail(h, BDA_LTM_ERR_ARG, "is_idr must be 0 or 1 for BDA_LTM_PAYLOAD_RAW");

	std::lock_guard<std::mutex> lock(g_decode_mutex);
	try {
		Packet packet;
		bool nal_idr = false;
		if (!extract_packet(payload, size, format, packet, nal_idr))
			return fail(h, BDA_LTM_ERR_NO_LCEVC, "no LCEVC NAL unit (type 28/29) found");
		const bool idr = is_idr < 0 ? nal_idr : is_idr != 0;

		// Same sequence as DecoderApp::push_base_enhancement.
		Surface symbols[MAX_NUM_PLANES][MAX_NUM_LOQS][MAX_NUM_LAYERS];
		h->decoder.initialize_decode(packet, symbols);
		const SignaledConfiguration cfg = h->decoder.get_configuration();
		h->decoder.set_idr(idr);
		const Dimensions dims = h->decoder.get_dimensions();
		const GlobalConfiguration &g = cfg.global_configuration;

		if (g.transform_block_size != 2 && g.transform_block_size != 4)
			return fail(h, BDA_LTM_ERR_DECODE, "no global configuration received yet");

		const ImageDescription base_desc = ImageDescription(g.image_format, g.resolution_width, g.resolution_height)
		                                       .with_depth(g.base_depth)
		                                       .with_size(dims.base_width(), dims.base_height());

		if (base->width != (int)dims.base_width() || base->height != (int)dims.base_height() ||
		    base->bit_depth != (int)g.base_depth || base->chroma_format != (int)g.colourspace) {
			return fail(h, BDA_LTM_ERR_MISMATCH,
			            lctm::format("base %dx%d %d-bit chroma %d, signalled %ux%u %u-bit chroma %d", base->width, base->height,
			                   base->bit_depth, base->chroma_format, dims.base_width(), dims.base_height(), g.base_depth,
			                   (int)g.colourspace));
		}

		std::vector<Surface> base_planes;
		for (unsigned p = 0; p < base_desc.num_planes(); ++p) {
			if (!base->data[p])
				return fail(h, BDA_LTM_ERR_ARG, lctm::format("base plane %u is null", p));
			const unsigned w = base_desc.width(p), ht = base_desc.height(p);
			const unsigned stride = (unsigned)base->stride[p];
			if (g.base_depth == 8)
				base_planes.push_back(Surface::build_from<int8_t>().contents((const int8_t *)base->data[p], w, ht, stride).finish());
			else
				base_planes.push_back(
				    Surface::build_from<int16_t>().contents((const int16_t *)base->data[p], w, ht, stride).finish());
		}

		for (auto &per_id : h->surfaces)
			for (auto &s : per_id)
				s.valid = false;
		h->dump_mask = dump_mask & BDA_LTM_DUMP_ALL;
		g_capture_target = h;
		bda_dump_hook = h->dump_mask ? &dump_hook : nullptr;

		// Reset the hook even when LTM throws.
		struct HookGuard {
			~HookGuard() {
				bda_dump_hook = nullptr;
				g_capture_target = nullptr;
			}
		} hook_guard;
		const Image reference_image;
		const Image full = h->decoder.decode(Image("base", base_desc, (uint64_t)h->frame_index, base_planes), symbols,
		                                     reference_image, false, h->dithering, h->dithering_fixed, h->apply_enhancement);

		// Output planes
		const ImageDescription &od = full.description();
		memset(&h->out_desc, 0, sizeof(h->out_desc));
		h->out_desc.width = (int)od.width();
		h->out_desc.height = (int)od.height();
		h->out_desc.bit_depth = (int)od.bit_depth();
		h->out_desc.chroma_format = (int)od.colourspace();
		for (unsigned p = 0; p < od.num_planes() && p < MAX_NUM_PLANES; ++p) {
			const Surface &s = full.plane(p);
			const SurfaceView<uint8_t> view(s);
			const size_t row = (size_t)s.width() * s.bpp();
			h->out_planes[p].resize(row * s.height());
			for (unsigned y = 0; y < s.height(); ++y)
				memcpy(h->out_planes[p].data() + y * row, view.data(0, y), row);
			h->out_desc.data[p] = h->out_planes[p].data();
			h->out_desc.stride[p] = (ptrdiff_t)row;
			h->out_desc.plane_width[p] = (int)s.width();
			h->out_desc.plane_height[p] = (int)s.height();
		}
		if (out)
			*out = h->out_desc;

		h->config = h->decoder.get_configuration();
		h->dims = h->decoder.get_dimensions();
		h->is_idr = idr;
		h->have_picture = true;
		h->frame_index++;
		return BDA_LTM_OK;
	} catch (const std::exception &e) {
		return fail(h, BDA_LTM_ERR_DECODE, e.what());
	} catch (...) {
		return fail(h, BDA_LTM_ERR_DECODE, "unknown LTM error");
	}
}

int bda_ltm_get_surface(bda_ltm *h, int which, int plane, bda_ltm_surface *out) {
	if (!h || !out || which < 0 || which >= BDA_LTM_SURF_COUNT || plane < 0 || plane >= MAX_NUM_PLANES)
		return BDA_LTM_ERR_ARG;
	if (!h->have_picture)
		return BDA_LTM_ERR_STATE;
	const CapturedSurface &s = h->surfaces[which][plane];
	if (!s.valid)
		return BDA_LTM_NOT_AVAILABLE;
	out->width = s.width;
	out->height = s.height;
	out->bytes_per_sample = s.bytes_per_sample;
	out->data = s.data.data();
	out->stride = (ptrdiff_t)s.width * s.bytes_per_sample;
	return BDA_LTM_OK;
}

int bda_ltm_get_picture_info(bda_ltm *h, bda_ltm_picture_info *o) {
	if (!h || !o)
		return BDA_LTM_ERR_ARG;
	if (!h->have_picture)
		return BDA_LTM_ERR_STATE;
	memset(o, 0, sizeof(*o));
	const SequenceConfiguration &sq = h->config.sequence_configuration;
	const GlobalConfiguration &g = h->config.global_configuration;
	const PictureConfiguration &pc = h->config.picture_configuration;

	o->profile_idc = (int)sq.profile_idc;
	o->level_idc = (int)sq.level_idc;
	o->sublevel_idc = (int)sq.sublevel_idc;
	o->conformance_window = sq.conformance_window;
	o->conf_win_offset[0] = (int)sq.conf_win_left_offset;
	o->conf_win_offset[1] = (int)sq.conf_win_right_offset;
	o->conf_win_offset[2] = (int)sq.conf_win_top_offset;
	o->conf_win_offset[3] = (int)sq.conf_win_bottom_offset;

	o->resolution_width = (int)g.resolution_width;
	o->resolution_height = (int)g.resolution_height;
	o->enhanced_width = h->out_desc.width;
	o->enhanced_height = h->out_desc.height;
	o->base_width = (int)h->dims.base_width();
	o->base_height = (int)h->dims.base_height();
	o->base_depth = (int)g.base_depth;
	o->enhancement_depth = (int)g.enhancement_depth;
	o->chroma_format = (int)g.colourspace;
	o->num_image_planes = (int)g.num_image_planes;
	o->num_processed_planes = (int)g.num_processed_planes;
	o->num_residual_layers = (int)g.num_residual_layers;
	o->transform_block_size = (int)g.transform_block_size;
	o->scaling_mode[0] = (int)g.scaling_mode[LOQ_LEVEL_1];
	o->scaling_mode[1] = (int)g.scaling_mode[LOQ_LEVEL_2];
	o->upsample = (int)g.upsample;
	for (int i = 0; i < 4; ++i)
		o->upsampling_coefficients[i] = (int)g.upsampling_coefficients[i];
	o->predicted_residual_enabled = g.predicted_residual_enabled;
	o->temporal_enabled = (int)g.temporal_enabled;
	o->temporal_tile_intra_signalling_enabled = g.temporal_tile_intra_signalling_enabled;
	o->temporal_step_width_modifier = (int)g.temporal_step_width_modifier;
	o->level1_filtering_coeff[0] = (int)g.level_1_filtering_first_coefficient;
	o->level1_filtering_coeff[1] = (int)g.level_1_filtering_second_coefficient;
	o->level1_depth_flag = g.level1_depth_flag;
	o->chroma_step_width_multiplier = (int)g.chroma_step_width_multiplier;
	o->tile_dimensions_type = (int)g.tile_dimensions_type;
	o->tile_width = (int)g.tile_width;
	o->tile_height = (int)g.tile_height;
	o->user_data_enabled = (int)g.user_data_enabled;

	o->is_idr = h->is_idr;
	o->enhancement_enabled = pc.enhancement_enabled;
	o->temporal_refresh = pc.temporal_refresh;
	o->temporal_signalling_present = pc.temporal_signalling_present;
	o->picture_type = (int)pc.picture_type;
	o->field_type = (int)pc.field_type;
	o->step_width[0] = (int)pc.step_width_loq[LOQ_LEVEL_1];
	o->step_width[1] = (int)pc.step_width_loq[LOQ_LEVEL_2];
	o->dithering_control = pc.dithering_control;
	o->dithering_type = (int)pc.dithering_type;
	o->dithering_strength = (int)pc.dithering_strength;
	o->dequant_offset_signalled = pc.dequant_offset_signalled;
	o->dequant_offset_mode = (int)pc.dequant_offset_mode;
	o->dequant_offset = (int)pc.dequant_offset;
	o->level1_filtering_enabled = pc.level_1_filtering_enabled;
	o->quant_matrix_mode = (int)pc.quant_matrix_mode;

	uint32_t captured = 0;
	for (int id = 0; id < BDA_LTM_SURF_COUNT; ++id)
		for (int p = 0; p < MAX_NUM_PLANES; ++p)
			if (h->surfaces[id][p].valid)
				captured |= 1u << id;
	o->surfaces_captured = captured;
	o->frame_index = h->frame_index - 1;
	return BDA_LTM_OK;
}

const char *bda_ltm_last_error(bda_ltm *h) { return h ? h->last_error.c_str() : ""; }

} // extern "C"
