// test_jm_analyzer.cpp - verification CLI for libbda-jm-analyzer.so (dlopen()ed, like YUView will).
//
// Pushes an Annex-B H.264 stream into the library in fixed-size chunks (NAL units are split across
// pushes on purpose), pulls every output picture and checks:
//   - per-picture MD5 of the output against a reference YUV in ldecod's layout (e.g. `ldecod -o` or
//     `ffmpeg -f rawvideo`), frame by frame, and that the reference has no extra frames;
//   - macroblock bit ranges: CAVLC bit_end(n) == bit_start(n + 1) inside every slice, first MB starts at
//     the slice's data_bit_start, last MB ends in the last NAL byte (before rbsp_slice_trailing_bits);
//     CABAC: the same contiguity, first MB at data_bit_start, last MB ending < 16 bits before the NAL end;
//   - every slice has a known NAL offset and every MB of a picture with MB data was decoded.
// --dump writes all picture/slice/MB/4x4 data as text (input of the trace comparison scripts).
// --in2 decodes a second stream with a second, simultaneously alive handle, interleaving the pushes of
// both decoders chunk by chunk (process-wide JM state swapping). --repeat N decodes the stream N times
// through the same handle, flushing (end of stream) in between.
//
// Usage:
//   test_jm_analyzer --lib libbda-jm-analyzer.so --in stream.264 [--ref ref.yuv] [--out out.yuv]
//                    [--dump mb.txt] [--chunk BYTES] [--repeat N] [--in2 other.264 [--ref2 ref2.yuv]] [--log]
// Exit status 0 only if every check passes.

#include "bda_jm.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---- minimal MD5 (RFC 1321) ----
struct Md5 {
	uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
	uint64_t len = 0;
	uint8_t buf[64];
	size_t n = 0;

	static uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
	void block(const uint8_t *p) {
		static const uint32_t K[64] = {
		    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
		    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
		    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
		    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
		    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
		    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
		    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
		    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
		static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
		                          14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
		                          4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
		uint32_t m[16];
		for (int i = 0; i < 16; ++i)
			m[i] = p[i * 4] | (p[i * 4 + 1] << 8) | (p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
		uint32_t A = a, B = b, C = c, D = d;
		for (int i = 0; i < 64; ++i) {
			uint32_t F;
			int g;
			if (i < 16) {
				F = (B & C) | (~B & D);
				g = i;
			} else if (i < 32) {
				F = (D & B) | (~D & C);
				g = (5 * i + 1) % 16;
			} else if (i < 48) {
				F = B ^ C ^ D;
				g = (3 * i + 5) % 16;
			} else {
				F = C ^ (B | ~D);
				g = (7 * i) % 16;
			}
			const uint32_t t = D;
			D = C;
			C = B;
			B = B + rol(A + F + K[i] + m[g], S[i]);
			A = t;
		}
		a += A;
		b += B;
		c += C;
		d += D;
	}
	void update(const void *data, size_t size) {
		const uint8_t *p = (const uint8_t *)data;
		len += size;
		while (size) {
			const size_t k = std::min(size, 64 - n);
			memcpy(buf + n, p, k);
			n += k;
			p += k;
			size -= k;
			if (n == 64) {
				block(buf);
				n = 0;
			}
		}
	}
	std::string hex() {
		const uint64_t bits = len * 8;
		const uint8_t pad = 0x80, zero = 0;
		update(&pad, 1);
		while (n != 56)
			update(&zero, 1);
		uint8_t l[8];
		for (int i = 0; i < 8; ++i)
			l[i] = (uint8_t)(bits >> (8 * i));
		update(l, 8);
		char s[33];
		const uint32_t v[4] = {a, b, c, d};
		for (int i = 0; i < 16; ++i)
			snprintf(s + 2 * i, 3, "%02x", (v[i / 4] >> (8 * (i % 4))) & 0xff);
		return s;
	}
};

std::vector<uint8_t> read_file(const std::string &path) {
	std::vector<uint8_t> d;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
		return d;
	uint8_t tmp[1 << 16];
	size_t n;
	while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0)
		d.insert(d.end(), tmp, tmp + n);
	fclose(f);
	return d;
}

#define LOAD(name) jm_##name = (bda_jm_##name##_fn)dlsym(lib, "bda_jm_" #name)
typedef int (*bda_jm_api_version_fn)(void);
typedef const char *(*bda_jm_version_string_fn)(void);
typedef bda_jm *(*bda_jm_create_fn)(void);
typedef void (*bda_jm_destroy_fn)(bda_jm *);
typedef void (*bda_jm_set_logging_fn)(int);
typedef int (*bda_jm_push_fn)(bda_jm *, const uint8_t *, size_t);
typedef int (*bda_jm_pull_fn)(bda_jm *, bda_jm_picture *);
typedef const char *(*bda_jm_last_error_fn)(bda_jm *);
bda_jm_api_version_fn jm_api_version;
bda_jm_version_string_fn jm_version_string;
bda_jm_create_fn jm_create;
bda_jm_destroy_fn jm_destroy;
bda_jm_set_logging_fn jm_set_logging;
bda_jm_push_fn jm_push;
bda_jm_pull_fn jm_pull;
bda_jm_last_error_fn jm_last_error;

// Picture bytes in ldecod's file layout: Y rows, then Cb rows, then Cr rows (4:0:0: Y only).
std::vector<uint8_t> frame_bytes(const bda_jm_picture &p) {
	std::vector<uint8_t> f;
	for (int c = 0; c < 3 && p.planes.data[c]; ++c) {
		const size_t row = (size_t)p.planes.plane_width[c] * p.planes.bytes_per_sample;
		for (int y = 0; y < p.planes.plane_height[c]; ++y) {
			const uint8_t *r = (const uint8_t *)p.planes.data[c] + y * p.planes.stride[c];
			f.insert(f.end(), r, r + row);
		}
	}
	return f;
}

struct Stats {
	int pictures = 0, md5_match = 0, md5_mismatch = 0;
	int64_t cavlc_pairs = 0, cavlc_contig = 0, cavlc_slices = 0, cavlc_end_ok = 0;
	int64_t cabac_pairs = 0, cabac_contig = 0, cabac_slices = 0, cabac_end_ok = 0;
	int64_t mb_with_data = 0, unknown_mbs = 0, nal_offset_unknown = 0;
};

// One decoder over one stream (pushed in chunks), output checked/dumped per picture.
struct Session {
	std::string name;
	std::vector<uint8_t> data;
	size_t pos = 0;
	size_t chunk = 4096;
	bda_jm *h = nullptr;
	FILE *ref = nullptr, *out = nullptr, *dump = nullptr;
	Stats st;
	bool eos_pushed = false, done = false;
	int repeat = 1; // push the stream this many times (flush in between)

	// Check bit ranges of one picture; returns a short report.
	void check_bits(const bda_jm_picture &p) {
		if (!p.has_mb_data)
			return;
		st.mb_with_data++;
		std::vector<std::vector<int>> per_slice(p.num_slices);
		for (int i = 0; i < p.mb_width * p.mb_height; ++i) {
			const bda_jm_mb &m = p.mbs[i];
			if (m.kind == BDA_JM_MB_UNKNOWN) {
				st.unknown_mbs++;
				continue;
			}
			if (m.slice_index >= 0 && m.slice_index < p.num_slices)
				per_slice[m.slice_index].push_back(i);
		}
		for (int s = 0; s < p.num_slices; ++s) {
			const bda_jm_slice &sl = p.slices[s];
			if (sl.nal_offset < 0)
				st.nal_offset_unknown++;
			const auto &v = per_slice[s];
			for (size_t k = 0; k + 1 < v.size(); ++k) {
				const bool c = p.mbs[v[k]].bit_end == p.mbs[v[k + 1]].bit_start;
				if (sl.entropy_mode) {
					st.cabac_pairs++;
					st.cabac_contig += c;
				} else {
					st.cavlc_pairs++;
					st.cavlc_contig += c;
				}
			}
			if (sl.entropy_mode && !v.empty()) {
				// First MB covers the engine initialisation (starts at data_bit_start); the last one ends
				// within the trailing bits (rbsp_stop_one_bit + alignment, plus engine look-ahead).
				st.cabac_slices++;
				const int64_t tail = (int64_t)sl.nal_size * 8 - p.mbs[v.back()].bit_end;
				if (tail >= 0 && tail < 16 && p.mbs[v.front()].bit_start == sl.data_bit_start)
					st.cabac_end_ok++;
			}
			if (!sl.entropy_mode && !v.empty()) {
				// The last MB must end inside the last payload byte (rbsp_stop_one_bit + alignment follow).
				st.cavlc_slices++;
				const int64_t end = p.mbs[v.back()].bit_end;
				const int64_t last_byte = (int64_t)sl.nal_size - 1;
				if (end > last_byte * 8 - 8 && end <= last_byte * 8 + 7 && p.mbs[v.front()].bit_start == sl.data_bit_start)
					st.cavlc_end_ok++;
			}
		}
	}

	void dump_picture(const bda_jm_picture &p, const std::string &md5) {
		if (!dump)
			return;
		fprintf(dump, "PIC out=%lld dec=%lld poc=%d frame_num=%d idr=%d ref=%d types=%u struct=%d size=%dx%d crop=%d,%d,%d,%d mbdata=%d mbw=%d mbh=%d slices=%d md5=%s\n",
		        (long long)p.output_index, (long long)p.decode_index, p.poc, p.frame_num, p.is_idr, p.is_reference,
		        p.slice_types, p.structure, p.planes.width, p.planes.height, p.crop_left, p.crop_right, p.crop_top,
		        p.crop_bottom, p.has_mb_data, p.mb_width, p.mb_height, p.num_slices, md5.c_str());
		if (!p.has_mb_data)
			return;
		for (int s = 0; s < p.num_slices; ++s) {
			const bda_jm_slice &sl = p.slices[s];
			fprintf(dump, "SLICE %d type=%d cabac=%d first_mb=%d num_mbs=%d qp=%d nal_type=%d nal_ref_idc=%d nal_offset=%lld nal_size=%d nal_index=%lld data_bit_start=%lld\n",
			        s, sl.slice_type, sl.entropy_mode, sl.first_mb, sl.num_mbs, sl.slice_qp, sl.nal_unit_type,
			        sl.nal_ref_idc, (long long)sl.nal_offset, sl.nal_size, (long long)sl.nal_index, (long long)sl.data_bit_start);
		}
		const int bw = p.mb_width * 4;
		for (int i = 0; i < p.mb_width * p.mb_height; ++i) {
			const bda_jm_mb &m = p.mbs[i];
			fprintf(dump, "MB %d x=%d y=%d slice=%d stype=%d cabac=%d kind=%d mb_type=%d skip=%d intra=%d t8=%d qp=%d cbp=%d i16=%d ichroma=%d",
			        i, m.mb_x, m.mb_y, m.slice_index, m.slice_type, m.entropy_mode, m.kind, m.mb_type, m.skip, m.is_intra,
			        m.transform_8x8, m.qp, m.cbp, m.intra16x16_mode, m.intra_chroma_mode);
			fprintf(dump, " i4=");
			for (int b = 0; b < 16; ++b)
				fprintf(dump, "%s%d", b ? "," : "", m.intra4x4_mode[b]);
			fprintf(dump, " i8=%d,%d,%d,%d sub=%d,%d,%d,%d pred=%d,%d,%d,%d pw=%d,%d,%d,%d ph=%d,%d,%d,%d part=%dx%d bits=%lld-%lld\n",
			        m.intra8x8_mode[0], m.intra8x8_mode[1], m.intra8x8_mode[2], m.intra8x8_mode[3], m.sub_mb_type[0],
			        m.sub_mb_type[1], m.sub_mb_type[2], m.sub_mb_type[3], m.part_pred[0], m.part_pred[1], m.part_pred[2],
			        m.part_pred[3], m.part_w[0], m.part_w[1], m.part_w[2], m.part_w[3], m.part_h[0], m.part_h[1],
			        m.part_h[2], m.part_h[3], m.mb_part_w, m.mb_part_h, (long long)m.bit_start, (long long)m.bit_end);
			fprintf(dump, "BLK %d", i);
			for (int b = 0; b < 16; ++b) {
				const bda_jm_block &k = p.blocks[(size_t)(m.mb_y * 4 + (b >> 2)) * bw + m.mb_x * 4 + (b & 3)];
				fprintf(dump, " %d:%d,%d:%d,%d:%d,%d:%d,%d", b, k.ref_idx[0], k.ref_idx[1], k.mv[0][0], k.mv[0][1],
				        k.mv[1][0], k.mv[1][1], k.ref_poc[0], k.ref_poc[1]);
			}
			fprintf(dump, "\n");
		}
	}

	void on_picture(const bda_jm_picture &p) {
		const std::vector<uint8_t> f = frame_bytes(p);
		Md5 md5;
		md5.update(f.data(), f.size());
		const std::string got = md5.hex();
		std::string want = "-";
		if (ref) {
			std::vector<uint8_t> rf(f.size());
			if (fread(rf.data(), 1, rf.size(), ref) == rf.size()) {
				Md5 m2;
				m2.update(rf.data(), rf.size());
				want = m2.hex();
			}
			(got == want ? st.md5_match : st.md5_mismatch)++;
		}
		if (out)
			fwrite(f.data(), 1, f.size(), out);
		check_bits(p);
		dump_picture(p, got);
		printf("%s pic out=%3lld dec=%3lld poc=%4d fn=%3d idr=%d types=%02x %dx%d/%d mb=%d md5=%s %s\n", name.c_str(),
		       (long long)p.output_index, (long long)p.decode_index, p.poc, p.frame_num, p.is_idr, p.slice_types,
		       p.planes.width, p.planes.height, p.planes.bit_depth_luma, p.has_mb_data, got.c_str(),
		       ref ? (got == want ? "OK" : "DIFF") : "");
		st.pictures++;
	}

	// One step: push a chunk (or end of stream) and drain. Returns false when finished or failed.
	bool step() {
		if (done)
			return false;
		if (pos < data.size()) {
			const size_t n = std::min(chunk, data.size() - pos);
			if (jm_push(h, data.data() + pos, n) != BDA_JM_OK) {
				fprintf(stderr, "%s push failed: %s\n", name.c_str(), jm_last_error(h));
				done = true;
				return false;
			}
			pos += n;
		} else if (!eos_pushed) {
			jm_push(h, nullptr, 0);
			eos_pushed = true;
		}
		for (;;) {
			bda_jm_picture p;
			const int rc = jm_pull(h, &p);
			if (rc == BDA_JM_OK) {
				on_picture(p);
				continue;
			}
			if (rc == BDA_JM_NEED_DATA)
				return true;
			if (rc == BDA_JM_EOS) {
				if (--repeat > 0) { // decode the stream again after the flush
					pos = 0;
					eos_pushed = false;
					return true;
				}
				done = true;
				return false;
			}
			fprintf(stderr, "%s pull rc=%d: %s\n", name.c_str(), rc, jm_last_error(h));
			done = true;
			return false;
		}
	}
};

} // namespace

int main(int argc, char **argv) {
	std::string lib_path, in_path, ref_path, out_path, dump_path, in2_path, ref2_path;
	size_t chunk = 4096;
	int repeat = 1;
	bool logging = false;
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : "";
		if (a == "--lib") lib_path = v, ++i;
		else if (a == "--in") in_path = v, ++i;
		else if (a == "--ref") ref_path = v, ++i;
		else if (a == "--out") out_path = v, ++i;
		else if (a == "--dump") dump_path = v, ++i;
		else if (a == "--chunk") chunk = (size_t)atol(v), ++i;
		else if (a == "--repeat") repeat = atoi(v), ++i;
		else if (a == "--in2") in2_path = v, ++i;
		else if (a == "--ref2") ref2_path = v, ++i;
		else if (a == "--log") logging = true;
		else {
			fprintf(stderr, "unknown option %s (see source header for usage)\n", a.c_str());
			return 2;
		}
	}
	if (lib_path.empty() || in_path.empty()) {
		fprintf(stderr, "need --lib --in\n");
		return 2;
	}
	void *lib = dlopen(lib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 2;
	}
	LOAD(api_version);
	LOAD(version_string);
	LOAD(create);
	LOAD(destroy);
	LOAD(set_logging);
	LOAD(push);
	LOAD(pull);
	LOAD(last_error);
	if (!jm_api_version || jm_api_version() != BDA_JM_API_VERSION || !jm_pull || !jm_push) {
		fprintf(stderr, "API version mismatch or missing symbols\n");
		return 2;
	}
	printf("library: %s\n", jm_version_string());
	jm_set_logging(logging);

	std::vector<Session> ss(in2_path.empty() ? 1 : 2);
	const std::string ins[2] = {in_path, in2_path}, refs[2] = {ref_path, ref2_path};
	for (size_t k = 0; k < ss.size(); ++k) {
		Session &s = ss[k];
		s.name = ss.size() > 1 ? (k ? "B" : "A") : "-";
		s.data = read_file(ins[k]);
		s.chunk = chunk ? chunk : 1;
		s.repeat = repeat;
		s.h = jm_create(); // both handles stay alive for the whole run
		FILE *probe = fopen(ins[k].c_str(), "rb"); // an empty stream is a valid test input
		if (!probe || !s.h) {
			fprintf(stderr, "cannot read %s or create decoder\n", ins[k].c_str());
			return 2;
		}
		fclose(probe);
		if (!refs[k].empty() && !(s.ref = fopen(refs[k].c_str(), "rb"))) {
			fprintf(stderr, "cannot open %s\n", refs[k].c_str());
			return 2;
		}
	}
	if (!out_path.empty())
		ss[0].out = fopen(out_path.c_str(), "wb");
	if (!dump_path.empty())
		ss[0].dump = fopen(dump_path.c_str(), "w");

	// Interleave the sessions chunk by chunk.
	bool any = true;
	while (any) {
		any = false;
		for (auto &s : ss)
			any |= s.step();
	}

	bool pass = true;
	for (auto &s : ss) {
		const Stats &t = s.st;
		printf("summary %s: pictures=%d md5 match=%d mismatch=%d | mbdata pictures=%lld unknown_mbs=%lld | CAVLC contiguous %lld/%lld, slice ends ok %lld/%lld | CABAC contiguous %lld/%lld, slice ends ok %lld/%lld | nal_offset unknown=%lld\n",
		       s.name.c_str(), t.pictures, t.md5_match, t.md5_mismatch, (long long)t.mb_with_data, (long long)t.unknown_mbs,
		       (long long)t.cavlc_contig, (long long)t.cavlc_pairs, (long long)t.cavlc_end_ok, (long long)t.cavlc_slices,
		       (long long)t.cabac_contig, (long long)t.cabac_pairs, (long long)t.cabac_end_ok, (long long)t.cabac_slices,
		       (long long)t.nal_offset_unknown);
		pass &= (t.pictures > 0 || s.data.empty()) && t.md5_mismatch == 0 && (!s.ref || t.md5_match == t.pictures) &&
		        t.cavlc_contig == t.cavlc_pairs && t.cavlc_end_ok == t.cavlc_slices && t.cabac_contig == t.cabac_pairs &&
		        t.cabac_end_ok == t.cabac_slices && t.nal_offset_unknown == 0 &&
		        t.unknown_mbs == 0 && s.done;
		if (s.ref) {
			// The reference must not have more frames than were decoded.
			const long p = ftell(s.ref);
			fseek(s.ref, 0, SEEK_END);
			if (ftell(s.ref) != p) {
				printf("summary %s: reference has more frames than decoded\n", s.name.c_str());
				pass = false;
			}
		}
		jm_destroy(s.h);
	}
	if (ss[0].out)
		fclose(ss[0].out);
	if (ss[0].dump)
		fclose(ss[0].dump);
	printf("%s\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
