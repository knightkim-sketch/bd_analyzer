// bda_jm.cpp - C wrapper around the JM 19.0 H.264/AVC reference decoder for bd-analyzer.
//
// JM is a pull decoder: DecodeOneFrame() reads NAL units from a file until a picture is complete and
// writes output pictures to a file from deep inside its DPB code. The wrapper keeps that control flow
// and runs JM on its own stack (ucontext coroutine, one per handle):
//   - annexb.c getChunk() asks bda_jm_hook_read() for bytes; with nothing pending the coroutine yields
//     back to bda_jm_pull() (BDA_JM_NEED_DATA) and continues there after the next bda_jm_push().
//   - output.c write_out_picture() calls bda_jm_hook_output(), which copies the picture and yields, so
//     bda_jm_pull() returns one picture at a time in output order.
//   - image.c exit_picture() calls bda_jm_hook_picture_done() before the picture enters the DPB; the
//     macroblock data captured while decoding is attached to the StorablePicture and travels with it
//     to the output hook (B pictures are output long after the next pictures were decoded).
//   - error()/exit()/assert() unwind to the coroutine entry with longjmp (bda_jm_lib.h).
// JM's process-wide state (p_Dec, read_new_slice's pending NAL unit) is swapped per handle on every
// call under one mutex, so several handles can coexist.
//
// Built by scripts/setup-jm-analyzer.sh together with the patched JM decoder objects (BDA_JM_LIBRARY).

#include "bda_jm.h"

extern "C" {
#include "global.h"
#include "annexb.h"
#include "biaridecod.h"
#include "config_common.h"
#include "h264decoder.h"
#include "mbuffer.h"
#include "nalu.h"
}

#include <setjmp.h>
#include <sys/mman.h>
#include <ucontext.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#ifndef BDA_JM_SRC_REV
#define BDA_JM_SRC_REV "unknown"
#endif

// Defined in JM's configfile.c / configfile.h (decoder parameter table and its target struct).
extern "C" {
extern InputParameters cfgparams;
extern Mapping Map[];
NALU_t *bda_jm_pending_nalu = nullptr;
}

namespace {

const size_t kStackSize = 16u << 20; // JM keeps sizeable arrays on the stack; pages are committed lazily

// NAL unit record: where the NAL unit sits in the pushed stream and where its emulation prevention
// bytes were, so RBSP bit positions can be mapped back to escaped NAL bit positions.
struct NalRec {
	int64_t offset = -1;
	int size = 0;
	int64_t index = -1;
	int type = 0;
	int ref_idc = 0;
	// For every emulation prevention byte: RBSP byte index (JM streamBuffer coordinates, i.e. counted
	// after the 1-byte NAL header) of the byte that follows it. Sorted.
	std::vector<int> epb_before;
	// Filled while decoding the slice
	int64_t data_bit_start = -1;
	int num_mbs = 0;

	int64_t nal_bit(int64_t rbsp_bit) const {
		const int64_t byte = rbsp_bit >> 3;
		const int64_t n = std::upper_bound(epb_before.begin(), epb_before.end(), (int)byte) - epb_before.begin();
		return (byte + 1 + n) * 8 + (rbsp_bit & 7);
	}
};

struct Snapshot {
	std::vector<bda_jm_mb> mbs;
	std::vector<bda_jm_block> blocks;
	std::vector<bda_jm_slice> slices;
	unsigned slice_types = 0;
	int64_t decode_index = -1;
	int mb_width = 0;
	int mb_height = 0;
	bool has_mb = false; // mbs/blocks filled (progressive non-MBAFF frame)
	bool is_reference = false; // nal_ref_idc != 0 (used_for_reference at decode time)
};

struct OutPic {
	bda_jm_picture pic;
	std::vector<uint8_t> planes[3];
	std::unique_ptr<Snapshot> snap;
};

enum YieldReason { Y_NONE, Y_NEED_DATA, Y_OUTPUT, Y_EOS, Y_FATAL };

std::mutex g_mutex;
bool g_logging = false;

} // namespace

struct bda_jm {
	// JM process-wide state of this decoder (swapped in by Enter)
	DecoderParams *dec = nullptr;
	NALU_t *pending = nullptr;

	// Coroutine
	ucontext_t caller_ctx;
	ucontext_t co_ctx;
	void *stack = nullptr;
	bool co_started = false;
	bool co_finished = false; // coroutine function returned (fatal error)
	YieldReason reason = Y_NONE;
	jmp_buf *jmp = nullptr; // longjmp target of fatal errors (coroutine entry or a guarded call)
	jmp_buf co_jmp;
	bool jm_open = false;

	// Input
	std::vector<uint8_t> in;  // in[0] is stream byte in_base
	int64_t in_base = 0;
	int64_t delivered = 0;    // stream bytes handed to JM
	bool end_of_stream = false;
	bool waiting_data = false; // coroutine suspended in hook_read
	bool at_eos = false;       // coroutine suspended after FinitDecoder

	// Per-stream counters and current picture
	NalRec cur_nal;
	int64_t nal_count = 0;
	std::vector<bda_jm_mb> cur_mbs;
	int64_t decode_count = 0;
	int64_t output_count = 0;

	// Output
	std::deque<std::unique_ptr<OutPic>> queue;
	std::unique_ptr<OutPic> current;

	bool dead = false;
	std::string last_error;
	std::string last_msg;

	bda_jm() {
		memset(&caller_ctx, 0, sizeof(caller_ctx));
		memset(&co_ctx, 0, sizeof(co_ctx));
	}
};

namespace {

bda_jm *g_cur = nullptr; // handle whose JM state is swapped in (guarded by g_mutex)

void enter(bda_jm *h) {
	g_cur = h;
	p_Dec = h->dec;
	bda_jm_pending_nalu = h->pending;
}

void leave(bda_jm *h) {
	h->dec = p_Dec;
	h->pending = bda_jm_pending_nalu;
	p_Dec = nullptr;
	bda_jm_pending_nalu = nullptr;
	g_cur = nullptr;
}

// longjmp to the handle's guarded entry point. Callers must not have live C++ objects with destructors
// in the frames being skipped (all JM frames are C).
[[noreturn]] void unwind(const char *msg) {
	bda_jm *h = g_cur;
	if (!h || !h->jmp)
		abort(); // JM code running outside any guarded call: cannot happen
	if (h->last_error.empty()) {
		try {
			h->last_error = msg;
		} catch (...) {
		}
	}
	longjmp(*h->jmp, 1);
}

void yield(bda_jm *h, YieldReason r) {
	h->reason = r;
	swapcontext(&h->co_ctx, &h->caller_ctx);
}

void log_text(const char *s) {
	bda_jm *h = g_cur;
	if (h) {
		try { // called from C frames: no exception may escape
			std::string t(s);
			while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' '))
				t.pop_back();
			size_t b = t.find_first_not_of("\r\n ");
			if (b != std::string::npos)
				h->last_msg = t.substr(b);
		} catch (...) {
		}
	}
	if (g_logging) {
		fputs("[jm] ", stderr);
		fputs(s, stderr);
		if (!*s || s[strlen(s) - 1] != '\n')
			fputc('\n', stderr);
	}
}

// Default decoder parameters exactly as ldecod sets them before parsing a config file (InitParams on
// JM's parameter table), without touching stdout or the current directory.
void default_params(InputParameters *inp) {
	memset(&cfgparams, 0, sizeof(cfgparams));
	InitParams(Map);
	*inp = cfgparams;
	inp->FileFormat = PAR_OF_ANNEXB;
	inp->silent = 1;
	inp->bDisplayDecParams = 0;
	inp->iDecFrmNum = 0;
	inp->DecodeAllLayers = 0;
	// infile/outfile/reffile stay empty: no files are opened (p_out = p_ref = -1)
}

[[noreturn]] void fatalf(const char *fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	unwind(buf);
}

void co_main_body(bda_jm *h) {
	InputParameters inp;
	default_params(&inp);
	if (OpenDecoder(&inp) != DEC_OPEN_NOERR)
		fatalf("OpenDecoder failed");
	h->jm_open = true;
	// JM never calls init_annex_b(), so IsFirstByteStreamNALU starts at 0 and ldecod rejects
	// leading_zero_8bits (more than one zero byte before the first start code). Allow them, as the
	// syntax does, for the first NAL unit of every sequence (also after a flush, below).
	p_Dec->p_Vid->annex_b->IsFirstByteStreamNALU = 1;
	for (;;) {
		DecodedPicList *list = nullptr;
		const int r = DecodeOneFrame(&list);
		if (r == DEC_EOS) {
			FinitDecoder(&list);
			p_Dec->p_Vid->annex_b->IsFirstByteStreamNALU = 1; // see above; reset_annex_b() keeps the flag
			h->at_eos = true;
			yield(h, Y_EOS);
			// Resumed after new data following the flush: JM continues with the next sequence.
			continue;
		}
		if (r != DEC_SUCCEED)
			fatalf("DecodeOneFrame returned 0x%x", r);
	}
}

void co_entry(unsigned lo, unsigned hi) {
	bda_jm *h = (bda_jm *)(((uintptr_t)hi << 32) | (uintptr_t)lo);
	h->jmp = &h->co_jmp;
	if (setjmp(h->co_jmp) == 0)
		co_main_body(h);
	// Fatal error: the coroutine ends; uc_link returns to the caller's last swapcontext().
	h->dead = true;
	h->co_finished = true;
	h->reason = Y_FATAL;
}

// Run the coroutine until it yields. Called with g_mutex held and the handle entered.
bool resume(bda_jm *h) {
	if (!h->co_started) {
		h->stack = mmap(nullptr, kStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK,
		                -1, 0);
		if (h->stack == MAP_FAILED) {
			h->stack = nullptr;
			return false;
		}
		mprotect(h->stack, 4096, PROT_NONE); // guard page: overflow faults instead of corrupting memory
		getcontext(&h->co_ctx);
		h->co_ctx.uc_stack.ss_sp = h->stack;
		h->co_ctx.uc_stack.ss_size = kStackSize;
		h->co_ctx.uc_link = &h->caller_ctx;
		const uintptr_t p = (uintptr_t)h;
		makecontext(&h->co_ctx, (void (*)())co_entry, 2, (unsigned)(p & 0xffffffffu), (unsigned)(p >> 32));
		h->co_started = true;
	}
	h->reason = Y_NONE;
	swapcontext(&h->caller_ctx, &h->co_ctx);
	return true;
}

int rbsp_pos(Slice *s) {
	if (s->active_pps && s->active_pps->entropy_coding_mode_flag)
		return arideco_bits_read(&s->partArr[0].de_cabac);
	return s->partArr[0].bitstream->frame_bitoffset;
}

NalRec *slice_nal(Slice *s) { return (NalRec *)s->bda_nal; }

// Normalised kind and spec mb_type from JM's pre-interpretation code (see interpret_mb_mode_*).
void classify(Macroblock *mb, int slice_type, bda_jm_mb &o) {
	const int code = mb->bda_mb_code;
	int intra_code = -1; // I-table mb_type for intra MBs
	o.mb_type = -1;
	switch (slice_type) {
	case I_SLICE:
		o.mb_type = (int16_t)code;
		intra_code = code;
		break;
	case SI_SLICE:
		o.mb_type = (int16_t)code;
		if (code == 0)
			o.kind = BDA_JM_MB_SI;
		else
			intra_code = code - 1;
		break;
	case P_SLICE:
	case SP_SLICE:
		if (mb->skip_flag || code == 0) {
			o.kind = BDA_JM_MB_P_SKIP;
			o.skip = 1;
		} else if (code <= 5) {
			o.mb_type = (int16_t)(code - 1);
			static const uint8_t k[5] = {BDA_JM_MB_P_L0_16X16, BDA_JM_MB_P_L0_L0_16X8, BDA_JM_MB_P_L0_L0_8X16,
			                             BDA_JM_MB_P_8X8, BDA_JM_MB_P_8X8REF0};
			o.kind = k[code - 1];
		} else {
			o.mb_type = (int16_t)(code - 1);
			intra_code = code - 6;
		}
		break;
	case B_SLICE:
		if (mb->skip_flag) {
			o.kind = BDA_JM_MB_B_SKIP;
			o.skip = 1;
		} else if (code <= 22) {
			o.mb_type = (int16_t)code;
			o.kind = (uint8_t)(BDA_JM_MB_B_DIRECT_16X16 + code);
		} else {
			o.mb_type = (int16_t)code;
			intra_code = code - 23;
		}
		break;
	}
	if (intra_code >= 0) {
		o.is_intra = 1;
		if (intra_code == 0)
			o.kind = mb->luma_transform_size_8x8_flag ? BDA_JM_MB_I8X8 : BDA_JM_MB_I4X4;
		else if (intra_code == 25)
			o.kind = BDA_JM_MB_IPCM;
		else
			o.kind = BDA_JM_MB_I16X16;
	} else if (o.kind == BDA_JM_MB_SI) {
		o.is_intra = 1;
	}
}

// Partition layout from JM's interpreted b8mode/b8pdir (b8mode: 0 direct, 1 16x16, 2 16x8, 3 8x16,
// 4 8x8, 5 8x4, 6 4x8, 7 4x4; b8pdir: 0 L0, 1 L1, 2 Bi).
void partitions(Macroblock *mb, bool direct_8x8_inference, bda_jm_mb &o) {
	for (int i = 0; i < 4; ++i) {
		o.part_pred[i] = BDA_JM_PRED_INTRA;
		o.part_w[i] = o.part_h[i] = 0;
	}
	o.mb_part_w = o.mb_part_h = 0;
	if (o.is_intra)
		return;
	const bool b8 = o.kind == BDA_JM_MB_P_8X8 || o.kind == BDA_JM_MB_P_8X8REF0 || o.kind == BDA_JM_MB_B_8X8;
	const bool direct_mb = o.kind == BDA_JM_MB_B_SKIP || o.kind == BDA_JM_MB_B_DIRECT_16X16;
	if (o.kind == BDA_JM_MB_P_SKIP || o.kind == BDA_JM_MB_P_L0_16X16 || (o.kind >= BDA_JM_MB_B_L0_16X16 && o.kind <= BDA_JM_MB_B_BI_16X16)) {
		o.mb_part_w = o.mb_part_h = 16;
	} else if (o.kind == BDA_JM_MB_P_L0_L0_16X8 || (o.kind >= BDA_JM_MB_B_L0_L0_16X8 && o.kind <= BDA_JM_MB_B_BI_BI_8X16 && (o.kind - BDA_JM_MB_B_L0_L0_16X8) % 2 == 0)) {
		o.mb_part_w = 16;
		o.mb_part_h = 8;
	} else if (o.kind == BDA_JM_MB_P_L0_L0_8X16 || (o.kind >= BDA_JM_MB_B_L0_L0_16X8 && o.kind <= BDA_JM_MB_B_BI_BI_8X16)) {
		o.mb_part_w = 8;
		o.mb_part_h = 16;
	} else {
		o.mb_part_w = o.mb_part_h = 8; // P_8x8, B_8x8, and direct MBs (predicted per 8x8 or 4x4)
	}
	for (int i = 0; i < 4; ++i) {
		if (direct_mb) {
			o.part_pred[i] = BDA_JM_PRED_DIRECT;
			o.part_w[i] = o.part_h[i] = direct_8x8_inference ? 8 : 4;
		} else if (b8) {
			const int m = mb->b8mode[i];
			if (m == 0) {
				o.part_pred[i] = BDA_JM_PRED_DIRECT;
				o.part_w[i] = o.part_h[i] = direct_8x8_inference ? 8 : 4;
			} else {
				o.part_pred[i] = mb->b8pdir[i];
				o.part_w[i] = (m == 4 || m == 5) ? 8 : 4;
				o.part_h[i] = (m == 4 || m == 6) ? 8 : 4;
			}
		} else {
			o.part_pred[i] = o.kind == BDA_JM_MB_P_SKIP ? BDA_JM_PRED_L0 : mb->b8pdir[i];
			o.part_w[i] = o.part_h[i] = 16;
		}
	}
}

void copy_plane(imgpel **img, int x0, int y0, int w, int h, int bytes, std::vector<uint8_t> &dst) {
	dst.resize((size_t)w * h * bytes);
	uint8_t *d = dst.data();
	for (int y = 0; y < h; ++y) {
		const imgpel *s = img[y0 + y] + x0;
		if (bytes == 1) {
			for (int x = 0; x < w; ++x)
				*d++ = (uint8_t)s[x];
		} else {
			memcpy(d, s, (size_t)w * 2); // imgpel is uint16 (IMGTYPE 1), little endian like ldecod writes it
			d += (size_t)w * 2;
		}
	}
}

} // namespace

// ---------------------------------------------------------------------------------------------------
// Hooks called from JM (bda_jm_lib.h)
// ---------------------------------------------------------------------------------------------------
extern "C" {

int bda_jm_printf(const char *fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	log_text(buf);
	return n;
}

int bda_jm_fprintf(FILE *f, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int n;
	if (f == stdout || f == stderr) {
		char buf[1024];
		n = vsnprintf(buf, sizeof(buf), fmt, ap);
		log_text(buf);
	} else {
		n = vfprintf(f, fmt, ap);
	}
	va_end(ap);
	return n;
}

void bda_jm_exit(int code) {
	bda_jm *h = g_cur;
	char buf[640];
	snprintf(buf, sizeof(buf), "%s (JM exit code %d)", h && !h->last_msg.empty() ? h->last_msg.c_str() : "JM terminated",
	         code);
	unwind(buf);
}

void bda_jm_assert_failed(const char *expr, const char *file, int line) {
	char buf[512];
	snprintf(buf, sizeof(buf), "%s:%d assertion failed: %s", file, line, expr);
	unwind(buf);
}

void bda_jm_hook_fatal(const char *text, int code) {
	char buf[640];
	snprintf(buf, sizeof(buf), "%s (JM error %d)", text, code);
	unwind(buf);
}

int bda_jm_hook_read(unsigned char *buf, int size) {
	bda_jm *h = g_cur;
	for (;;) {
		const int64_t avail = h->in_base + (int64_t)h->in.size() - h->delivered;
		if (avail > 0) {
			const int n = (int)std::min<int64_t>(avail, size);
			memcpy(buf, h->in.data() + (h->delivered - h->in_base), n);
			h->delivered += n;
			return n;
		}
		if (h->end_of_stream)
			return 0;
		h->waiting_data = true;
		yield(h, Y_NEED_DATA);
		// g_cur is this handle again when resumed
	}
}

void bda_jm_hook_nalu(struct video_par *p_Vid, struct nalu_t *nalu) {
	bda_jm *h = g_cur;
	bool oom = false;
	try {
		NalRec &r = h->cur_nal;
		r = NalRec();
		r.index = h->nal_count++;
		r.size = nalu->len;
		r.type = nalu->nal_unit_type;
		r.ref_idc = nalu->nal_reference_idc;

		// Emulation prevention bytes, scanned exactly like EBSPtoRBSP(buf, len, 1).
		const byte *b = nalu->buf;
		int zeros = 0, j = 1;
		for (int i = 1; i < nalu->len; ++i) {
			if (zeros == 2 && b[i] == 0x03) {
				r.epb_before.push_back(j - 1);
				zeros = 0;
				if (i == nalu->len - 1)
					break; // trailing cabac_zero_word 0x000003: the 03 is dropped
				++i;
			}
			zeros = b[i] == 0 ? zeros + 1 : 0;
			++j;
		}

		// Stream position: get_annex_b_NALU() has consumed the NAL unit, its trailing zero bytes and the
		// next start code (nextstartcodebytes, 0 at end of stream). NAL units never end with 0x00.
		const ANNEXB_t *ab = p_Vid->annex_b;
		const int64_t consumed = h->delivered - ab->bytesinbuffer;
		int64_t end = consumed - ab->nextstartcodebytes;
		while (end > h->in_base && h->in[end - 1 - h->in_base] == 0)
			--end;
		const int64_t start = end - nalu->len;
		if (start >= h->in_base && memcmp(h->in.data() + (start - h->in_base), nalu->buf, nalu->len) == 0)
			r.offset = start;

		// Drop input that JM has consumed (keep a margin for the next NAL unit's leading bytes).
		const int64_t keep_from = consumed - 16;
		if (keep_from - h->in_base > (1 << 20)) {
			h->in.erase(h->in.begin(), h->in.begin() + (keep_from - h->in_base));
			h->in_base = keep_from;
		}
	} catch (const std::bad_alloc &) {
		oom = true;
	}
	if (oom)
		bda_jm_hook_fatal("out of memory", 100);
}

void bda_jm_hook_slice_nal(struct slice *s) {
	bool oom = false;
	try {
		delete slice_nal(s);
		s->bda_nal = new NalRec(g_cur->cur_nal);
	} catch (const std::bad_alloc &) {
		oom = true;
	}
	if (oom)
		bda_jm_hook_fatal("out of memory", 100);
}

void bda_jm_hook_free_slice(struct slice *s) {
	delete slice_nal(s);
	s->bda_nal = nullptr;
}

void bda_jm_hook_mb_begin(struct slice *s, struct macroblock_dec *mb) {
	bda_jm *h = g_cur;
	bool oom = false;
	try {
		VideoParameters *p_Vid = mb->p_Vid;
		const size_t n = (size_t)p_Vid->PicSizeInMbs;
		if (h->cur_mbs.size() != n) {
			bda_jm_mb blank;
			memset(&blank, 0, sizeof(blank));
			blank.kind = BDA_JM_MB_UNKNOWN;
			h->cur_mbs.assign(n, blank);
		}
	} catch (const std::bad_alloc &) {
		oom = true;
	}
	if (oom)
		bda_jm_hook_fatal("out of memory", 100);
	if (mb->mbAddrX < 0 || (size_t)mb->mbAddrX >= h->cur_mbs.size())
		return;
	mb->bda_mb_code = -1;
	memset(mb->bda_sub_mb_type, -1, sizeof(mb->bda_sub_mb_type));
	int pos = rbsp_pos(s); // RBSP position, mapped in hook_mb_end
	if (s->num_dec_mb == 0 && s->active_pps && s->active_pps->entropy_coding_mode_flag)
		pos -= 9; // arideco_start_decoding() has read 9 bits into codIOffset: they belong to slice_data()
	h->cur_mbs[mb->mbAddrX].bit_start = pos;
}

void bda_jm_hook_mb_read(struct slice *s, struct macroblock_dec *mb) {
	bda_jm *h = g_cur;
	if (mb->mbAddrX < 0 || (size_t)mb->mbAddrX >= h->cur_mbs.size())
		return;
	VideoParameters *p_Vid = mb->p_Vid;
	bda_jm_mb &o = h->cur_mbs[mb->mbAddrX];
	const int64_t bit_start = o.bit_start;
	memset(&o, 0, sizeof(o));
	o.bit_start = bit_start;
	o.mb_x = (int16_t)mb->mb.x;
	o.mb_y = (int16_t)mb->mb.y;
	o.slice_index = mb->slice_nr;
	o.slice_type = (uint8_t)s->slice_type;
	o.entropy_mode = s->active_pps->entropy_coding_mode_flag ? 1 : 0;
	o.kind = BDA_JM_MB_UNKNOWN;
	classify(mb, s->slice_type, o);
	o.transform_8x8 = (!o.skip && mb->luma_transform_size_8x8_flag) ? 1 : 0;
	o.cbp = (int16_t)(o.skip ? 0 : o.kind == BDA_JM_MB_IPCM ? 47 : (mb->cbp & 0x3f));
	o.intra16x16_mode = o.kind == BDA_JM_MB_I16X16 ? (int8_t)mb->i16mode : -1;
	o.intra_chroma_mode = (o.is_intra && o.kind != BDA_JM_MB_IPCM && (p_Vid->active_sps->chroma_format_idc == YUV420 ||
	                                                                     p_Vid->active_sps->chroma_format_idc == YUV422))
	                          ? (int8_t)mb->c_ipred_mode
	                          : -1;
	const bool nxn = o.kind == BDA_JM_MB_I4X4 || o.kind == BDA_JM_MB_I8X8 || o.kind == BDA_JM_MB_SI;
	for (int b = 0; b < 16; ++b)
		o.intra4x4_mode[b] = nxn ? (int8_t)s->ipredmode[mb->block_y + (b >> 2)][mb->block_x + (b & 3)] : -1;
	for (int b = 0; b < 4; ++b) {
		o.intra8x8_mode[b] = o.kind == BDA_JM_MB_I8X8 ? o.intra4x4_mode[(b >> 1) * 8 + (b & 1) * 2] : -1;
		const bool sub = o.kind == BDA_JM_MB_P_8X8 || o.kind == BDA_JM_MB_P_8X8REF0 || o.kind == BDA_JM_MB_B_8X8;
		o.sub_mb_type[b] = sub ? mb->bda_sub_mb_type[b] : -1;
	}
	partitions(mb, p_Vid->active_sps->direct_8x8_inference_flag != 0, o);
}

void bda_jm_hook_mb_end(struct slice *s, struct macroblock_dec *mb) {
	bda_jm *h = g_cur;
	if (mb->mbAddrX < 0 || (size_t)mb->mbAddrX >= h->cur_mbs.size())
		return;
	bda_jm_mb &o = h->cur_mbs[mb->mbAddrX];
	NalRec *r = slice_nal(s);
	const int64_t end = rbsp_pos(s);
	o.qp = (int16_t)mb->qp; // after decoding: JM sets QPY = 0 for I_PCM there
	if (r) {
		if (r->data_bit_start < 0)
			r->data_bit_start = r->nal_bit(o.bit_start);
		r->num_mbs++;
		o.bit_start = r->nal_bit(o.bit_start);
		o.bit_end = r->nal_bit(end);
	} else {
		o.bit_end = end;
	}
}

void bda_jm_hook_picture_done(struct video_par *p_Vid, struct storable_picture *p) {
	bda_jm *h = g_cur;
	const int64_t index = h->decode_count++;
	// MB data only for progressive non-MBAFF frames; other pictures keep decode index and slice info.
	const bool mb_ok = p->structure == FRAME && !p->mb_aff_frame_flag && !p_Vid->separate_colour_plane_flag &&
	                   h->cur_mbs.size() == (size_t)p->PicSizeInMbs;
	bool oom = false;
	try {
		std::unique_ptr<Snapshot> snap(new Snapshot);
		snap->decode_index = index;
		snap->has_mb = mb_ok;
		snap->is_reference = p->used_for_reference != 0;
		if (mb_ok) {
			snap->mb_width = (int)p->PicWidthInMbs;
			snap->mb_height = (int)(p->PicSizeInMbs / p->PicWidthInMbs);
			snap->mbs = h->cur_mbs;
		}

		for (int i = 0; i < p_Vid->iSliceNumOfCurrPic; ++i) {
			Slice *s = p_Vid->ppSliceList[i];
			bda_jm_slice sl;
			memset(&sl, 0, sizeof(sl));
			sl.slice_type = s->slice_type;
			sl.entropy_mode = s->active_pps && s->active_pps->entropy_coding_mode_flag ? 1 : 0;
			sl.first_mb = s->start_mb_nr;
			sl.slice_qp = 26 + (s->active_pps ? s->active_pps->pic_init_qp_minus26 : 0) + s->slice_qp_delta;
			sl.nal_offset = -1;
			sl.nal_index = -1;
			sl.data_bit_start = -1;
			if (const NalRec *r = slice_nal(s)) {
				sl.num_mbs = r->num_mbs;
				sl.nal_unit_type = r->type;
				sl.nal_ref_idc = r->ref_idc;
				sl.nal_size = r->size;
				sl.nal_offset = r->offset;
				sl.nal_index = r->index;
				sl.data_bit_start = r->data_bit_start;
			}
			snap->slice_types |= 1u << (s->slice_type % 5);
			snap->slices.push_back(sl);
		}

		const int bw = snap->mb_width * 4, bh = mb_ok ? snap->mb_height * 4 : 0;
		snap->blocks.resize((size_t)bw * bh);
		for (int y = 0; y < bh; ++y) {
			for (int x = 0; x < bw; ++x) {
				const PicMotionParams &mi = p->mv_info[y][x];
				bda_jm_block &b = snap->blocks[(size_t)y * bw + x];
				for (int l = 0; l < 2; ++l) {
					b.ref_idx[l] = mi.ref_idx[l];
					if (mi.ref_idx[l] >= 0) {
						b.mv[l][0] = mi.mv[l].mv_x;
						b.mv[l][1] = mi.mv[l].mv_y;
						b.ref_poc[l] = mi.ref_pic[l] ? mi.ref_pic[l]->poc : 0;
					} else {
						b.ref_idx[l] = -1;
						b.mv[l][0] = b.mv[l][1] = 0;
						b.ref_poc[l] = 0;
					}
				}
			}
		}
		delete (Snapshot *)p->bda_snapshot;
		p->bda_snapshot = snap.release();
	} catch (const std::bad_alloc &) {
		oom = true;
	}
	// MBs not decoded in the next picture must not show this picture's values
	bda_jm_mb blank;
	memset(&blank, 0, sizeof(blank));
	blank.kind = BDA_JM_MB_UNKNOWN;
	std::fill(h->cur_mbs.begin(), h->cur_mbs.end(), blank);
	if (oom)
		bda_jm_hook_fatal("out of memory", 100);
}

void bda_jm_hook_free_picture(struct storable_picture *p) {
	delete (Snapshot *)p->bda_snapshot;
	p->bda_snapshot = nullptr;
}

void bda_jm_hook_output(struct video_par *p_Vid, struct storable_picture *p) {
	bda_jm *h = g_cur;
	static const int SubWidthC[4] = {1, 2, 2, 1};
	static const int SubHeightC[4] = {1, 2, 1, 1};
	bool oom = false;
	try {
		std::unique_ptr<OutPic> o(new OutPic);
		bda_jm_picture &pic = o->pic;
		memset(&pic, 0, sizeof(pic));

		// Cropping and sample size exactly as write_out_picture() computes them.
		const int cf = p->chroma_format_idc;
		const int fmo = 2 - p->frame_mbs_only_flag;
		int cl = 0, cr = 0, ct = 0, cb = 0;
		if (p->frame_cropping_flag) {
			cl = SubWidthC[cf] * p->frame_crop_left_offset;
			cr = SubWidthC[cf] * p->frame_crop_right_offset;
			ct = SubHeightC[cf] * fmo * p->frame_crop_top_offset;
			cb = SubHeightC[cf] * fmo * p->frame_crop_bottom_offset;
		}
		const int bytes = (p_Vid->pic_unit_bitsize_on_disk + 7) >> 3;
		bda_jm_planes &pl = pic.planes;
		pl.width = p->size_x - cl - cr;
		pl.height = p->size_y - ct - cb;
		pl.bit_depth_luma = p_Vid->bitdepth_luma;
		pl.bit_depth_chroma = p_Vid->bitdepth_chroma;
		pl.bytes_per_sample = bytes;
		pl.chroma_format = cf;
		copy_plane(p->imgY, cl, ct, pl.width, pl.height, bytes, o->planes[0]);
		pl.plane_width[0] = pl.width;
		pl.plane_height[0] = pl.height;
		if (cf != YUV400) {
			const int ccl = p->frame_cropping_flag ? p->frame_crop_left_offset : 0;
			const int ccr = p->frame_cropping_flag ? p->frame_crop_right_offset : 0;
			const int cct = p->frame_cropping_flag ? fmo * p->frame_crop_top_offset : 0;
			const int ccb = p->frame_cropping_flag ? fmo * p->frame_crop_bottom_offset : 0;
			const int cw = p->size_x_cr - ccl - ccr, ch = p->size_y_cr - cct - ccb;
			for (int c = 1; c <= 2; ++c) {
				copy_plane(p->imgUV[c - 1], ccl, cct, cw, ch, bytes, o->planes[c]);
				pl.plane_width[c] = cw;
				pl.plane_height[c] = ch;
			}
		}
		for (int c = 0; c < 3; ++c) {
			pl.data[c] = o->planes[c].empty() ? nullptr : o->planes[c].data();
			pl.stride[c] = (ptrdiff_t)pl.plane_width[c] * bytes;
		}

		pic.poc = p->frame_poc;
		pic.frame_num = (int)p->frame_num;
		pic.is_idr = p->idr_flag ? 1 : 0;
		pic.is_reference = p->used_for_reference ? 1 : 0;
		pic.structure = p->coded_frame ? BDA_JM_STRUCT_FRAME : BDA_JM_STRUCT_FIELD_PAIR;
		pic.output_index = h->output_count++;
		pic.coded_width = p->size_x;
		pic.coded_height = p->size_y;
		pic.crop_left = cl;
		pic.crop_right = cr;
		pic.crop_top = ct;
		pic.crop_bottom = cb;
		pic.concealed = p->concealed_pic ? 1 : 0;
		pic.decode_index = -1;
		pic.slice_types = 1u << (p->slice_type % 5);

		o->snap.reset((Snapshot *)p->bda_snapshot);
		p->bda_snapshot = nullptr;
		if (const Snapshot *s = o->snap.get()) {
			pic.decode_index = s->decode_index;
			pic.is_reference = s->is_reference ? 1 : 0;
			pic.slice_types = s->slice_types;
			pic.num_slices = (int)s->slices.size();
			pic.slices = s->slices.data();
			if (s->has_mb) {
				pic.has_mb_data = 1;
				pic.mb_width = s->mb_width;
				pic.mb_height = s->mb_height;
				pic.mbs = s->mbs.data();
				pic.blocks = s->blocks.data();
			}
		}
		h->queue.push_back(std::move(o));
	} catch (const std::bad_alloc &) {
		oom = true;
	}
	if (oom)
		bda_jm_hook_fatal("out of memory", 100);
	yield(h, Y_OUTPUT);
}

} // extern "C"

// ---------------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------------
extern "C" {

int bda_jm_api_version(void) { return BDA_JM_API_VERSION; }

const char *bda_jm_version_string(void) {
	static const std::string s = std::string("JM ") + VERSION + " (" + BDA_JM_SRC_REV + ") + bda wrapper API " +
	                             std::to_string(BDA_JM_API_VERSION);
	return s.c_str();
}

bda_jm *bda_jm_create(void) {
	try {
		return new bda_jm();
	} catch (...) {
		return nullptr;
	}
}

void bda_jm_destroy(bda_jm *h) {
	if (!h)
		return;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		enter(h);
		if (h->jm_open) {
			// Release JM's memory. The suspended coroutine stack is simply discarded. After a fatal error
			// the state may be inconsistent; a second error inside CloseDecoder only leaks.
			jmp_buf guard;
			h->jmp = &guard;
			if (setjmp(guard) == 0)
				CloseDecoder();
			h->jmp = nullptr;
		}
		leave(h);
	}
	if (h->stack)
		munmap(h->stack, kStackSize);
	delete h;
}

void bda_jm_set_logging(int enable) {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_logging = enable != 0;
}

int bda_jm_push(bda_jm *h, const uint8_t *data, size_t size) {
	if (!h || (size && !data))
		return BDA_JM_ERR_ARG;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (h->dead)
		return BDA_JM_ERR_STATE;
	if (size == 0) {
		h->end_of_stream = true;
		return BDA_JM_OK;
	}
	if (h->end_of_stream) {
		if (!h->at_eos)
			return BDA_JM_ERR_STATE; // end of stream signalled but not flushed yet
		h->end_of_stream = false;    // new sequence after the flush
	}
	try {
		h->in.insert(h->in.end(), data, data + size);
	} catch (const std::bad_alloc &) {
		return BDA_JM_ERR_NOMEM;
	}
	return BDA_JM_OK;
}

int bda_jm_pull(bda_jm *h, bda_jm_picture *out) {
	if (!h || !out)
		return BDA_JM_ERR_ARG;
	std::lock_guard<std::mutex> lock(g_mutex);
	h->current.reset();
	for (;;) {
		if (!h->queue.empty()) {
			h->current = std::move(h->queue.front());
			h->queue.pop_front();
			*out = h->current->pic;
			return BDA_JM_OK;
		}
		if (h->dead)
			return BDA_JM_ERR_DECODE;
		const bool have_data = h->in_base + (int64_t)h->in.size() > h->delivered;
		if (h->at_eos) {
			if (!have_data)
				return BDA_JM_EOS;
			h->at_eos = false; // new sequence pushed after the flush
		} else if (h->co_started && h->waiting_data && !have_data && !h->end_of_stream) {
			return BDA_JM_NEED_DATA;
		} else if (!h->co_started && !have_data && !h->end_of_stream) {
			return BDA_JM_NEED_DATA;
		}
		if (!h->co_started && !have_data && h->end_of_stream && h->delivered == 0) {
			// Empty stream: nothing to decode (JM cannot run without any picture).
			h->at_eos = true;
			return BDA_JM_EOS;
		}
		h->waiting_data = false;
		enter(h);
		const bool ok = resume(h);
		leave(h);
		if (!ok) {
			h->dead = true;
			h->last_error = "cannot allocate the decoder stack";
			return BDA_JM_ERR_NOMEM;
		}
		// Y_OUTPUT / Y_NEED_DATA / Y_EOS / Y_FATAL are all resolved at the top of the loop
	}
}

const char *bda_jm_last_error(bda_jm *h) { return h ? h->last_error.c_str() : ""; }

} // extern "C"
