// test_ltm_wrapper.cpp - verification CLI for libbda-lcevc-ltm.so (dlopen()ed, like YUView will).
//
// Decodes a sequence of LCEVC NAL units against externally decoded base pictures and compares:
//   - per-frame MD5 of the enhanced output against a reference YUV (e.g. LTM ModelDecoder -o), and
//   - the captured intermediate surfaces against LTM's --dump_surfaces files (bit-exact, frame by frame;
//     LTM appends a frame to a dump file only when the surface is produced, so the k-th capture is
//     compared with the k-th frame of the file).
//
// Usage:
//   test_ltm_wrapper --lib libbda-lcevc-ltm.so --enh enhancement.es --base base.yuv
//                    --base-size WxH [--base-depth 8] [--chroma 1] [--format annexb|nal]
//                    [--ref ref.yuv] [--out out.yuv] [--dump-dir DIR] [--frames N]
//
//   enhancement.es  Annex-B file with one LCEVC NAL unit per picture in OUTPUT order, e.g. the
//                   _temp_*__enhancement.es written by `ModelDecoder --keep_base`.
//   base.yuv        planar base pictures in output order (8-bit: 1 byte/sample, else 2 bytes LE).
// Exit status 0 only if every compared frame and surface matches.

#include "bda_lcevc_ltm.h"

#include <dlfcn.h>
#include <dirent.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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

// Split an Annex-B buffer at 00 00 01 start codes; each segment keeps its start code.
std::vector<std::pair<size_t, size_t>> split_annexb(const std::vector<uint8_t> &d) {
	std::vector<size_t> starts;
	for (size_t i = 0; i + 3 <= d.size(); ++i)
		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
			starts.push_back(i);
			i += 2;
		}
	std::vector<std::pair<size_t, size_t>> seg;
	for (size_t k = 0; k < starts.size(); ++k) {
		size_t end = k + 1 < starts.size() ? starts[k + 1] : d.size();
		if (k + 1 < starts.size() && end > starts[k] && d[end - 1] == 0)
			end--; // 4-byte start code of the next NAL
		seg.push_back({starts[k], end});
	}
	return seg;
}

#define LOAD(name) name##_fn name = (name##_fn)dlsym(lib, #name)
typedef int (*bda_ltm_api_version_fn)(void);
typedef const char *(*bda_ltm_version_string_fn)(void);
typedef bda_ltm *(*bda_ltm_create_fn)(void);
typedef void (*bda_ltm_destroy_fn)(bda_ltm *);
typedef int (*bda_ltm_decode_fn)(bda_ltm *, const uint8_t *, size_t, int, int, const bda_ltm_planes *, bda_ltm_planes *,
                                 unsigned);
typedef int (*bda_ltm_get_surface_fn)(bda_ltm *, int, int, bda_ltm_surface *);
typedef int (*bda_ltm_get_picture_info_fn)(bda_ltm *, bda_ltm_picture_info *);
typedef const char *(*bda_ltm_last_error_fn)(bda_ltm *);

const char *const kDumpPrefix[BDA_LTM_SURF_COUNT] = {"dec_base_deco",      "dec_base_pred",      "dec_base_resi_reco",
                                                     "dec_full_pred",      "dec_full_resi_reco", "dec_full_temp_buff",
                                                     "dec_full_temp_mask"};

} // namespace

int main(int argc, char **argv) {
	std::string lib_path, enh_path, base_path, ref_path, out_path, dump_dir, fmt = "annexb";
	int bw = 0, bh = 0, depth = 8, chroma = 1, max_frames = 1 << 30;
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : "";
		if (a == "--lib") lib_path = v, ++i;
		else if (a == "--enh") enh_path = v, ++i;
		else if (a == "--base") base_path = v, ++i;
		else if (a == "--ref") ref_path = v, ++i;
		else if (a == "--out") out_path = v, ++i;
		else if (a == "--dump-dir") dump_dir = v, ++i;
		else if (a == "--format") fmt = v, ++i;
		else if (a == "--base-size") { sscanf(v, "%dx%d", &bw, &bh); ++i; }
		else if (a == "--base-depth") depth = atoi(v), ++i;
		else if (a == "--chroma") chroma = atoi(v), ++i;
		else if (a == "--frames") max_frames = atoi(v), ++i;
		else {
			fprintf(stderr, "unknown option %s (see source header for usage)\n", a.c_str());
			return 2;
		}
	}
	if (lib_path.empty() || enh_path.empty() || base_path.empty() || bw <= 0 || bh <= 0) {
		fprintf(stderr, "need --lib --enh --base --base-size\n");
		return 2;
	}

	void *lib = dlopen(lib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 2;
	}
	LOAD(bda_ltm_api_version);
	LOAD(bda_ltm_version_string);
	LOAD(bda_ltm_create);
	LOAD(bda_ltm_destroy);
	LOAD(bda_ltm_decode);
	LOAD(bda_ltm_get_surface);
	LOAD(bda_ltm_get_picture_info);
	LOAD(bda_ltm_last_error);
	if (!bda_ltm_api_version || bda_ltm_api_version() != BDA_LTM_API_VERSION || !bda_ltm_decode) {
		fprintf(stderr, "API version mismatch or missing symbols\n");
		return 2;
	}
	printf("library: %s\n", bda_ltm_version_string());

	const std::vector<uint8_t> enh = read_file(enh_path);
	const auto nals = split_annexb(enh);
	FILE *base_f = fopen(base_path.c_str(), "rb");
	FILE *ref_f = ref_path.empty() ? nullptr : fopen(ref_path.c_str(), "rb");
	FILE *out_f = out_path.empty() ? nullptr : fopen(out_path.c_str(), "wb");
	if (!base_f || (!ref_path.empty() && !ref_f)) {
		fprintf(stderr, "cannot open base/ref\n");
		return 2;
	}

	// Dump files: <prefix>_P<n>_*.y in dump_dir, read frame by frame.
	std::map<std::pair<int, int>, FILE *> dump_files;
	if (!dump_dir.empty()) {
		DIR *dir = opendir(dump_dir.c_str());
		if (!dir) {
			fprintf(stderr, "cannot open %s\n", dump_dir.c_str());
			return 2;
		}
		while (dirent *e = readdir(dir)) {
			const std::string n = e->d_name;
			for (int id = 0; id < BDA_LTM_SURF_COUNT; ++id)
				for (int p = 0; p < 3; ++p) {
					const std::string pre = std::string(kDumpPrefix[id]) + "_P" + std::to_string(p) + "_";
					if (n.compare(0, pre.size(), pre) == 0)
						dump_files[{id, p}] = fopen((dump_dir + "/" + n).c_str(), "rb");
				}
		}
		closedir(dir);
	}

	const int bps = depth > 8 ? 2 : 1;
	const int cw = chroma == 3 ? bw : (bw + 1) / 2, ch = chroma == 1 ? (bh + 1) / 2 : bh;
	const size_t ysz = (size_t)bw * bh * bps, csz = chroma == 0 ? 0 : (size_t)cw * ch * bps;
	std::vector<uint8_t> base_buf(ysz + 2 * csz);

	bda_ltm *h = bda_ltm_create();
	const int format = fmt == "nal" ? BDA_LTM_PAYLOAD_NAL : BDA_LTM_PAYLOAD_ANNEXB;
	int frames = 0, md5_match = 0, md5_mismatch = 0;
	std::map<int, int> surf_match, surf_mismatch;
	int rc_all = 0;

	for (size_t k = 0; k < nals.size() && frames < max_frames; ++k) {
		if (fread(base_buf.data(), 1, base_buf.size(), base_f) != base_buf.size()) {
			fprintf(stderr, "base file ended at frame %d\n", frames);
			rc_all = 1;
			break;
		}
		bda_ltm_planes base;
		memset(&base, 0, sizeof(base));
		base.width = bw;
		base.height = bh;
		base.bit_depth = depth;
		base.chroma_format = chroma;
		base.data[0] = base_buf.data();
		base.stride[0] = (ptrdiff_t)bw * bps;
		base.data[1] = base_buf.data() + ysz;
		base.data[2] = base_buf.data() + ysz + csz;
		base.stride[1] = base.stride[2] = (ptrdiff_t)cw * bps;

		const uint8_t *p = enh.data() + nals[k].first;
		size_t sz = nals[k].second - nals[k].first;
		if (format == BDA_LTM_PAYLOAD_NAL) { // strip the start code
			const size_t sc = p[2] == 1 ? 3 : 4;
			p += sc;
			sz -= sc;
		}
		bda_ltm_planes out;
		const int rc = bda_ltm_decode(h, p, sz, format, -1, &base, &out, BDA_LTM_DUMP_ALL);
		if (rc != BDA_LTM_OK) {
			fprintf(stderr, "frame %d: decode rc=%d: %s\n", frames, rc, bda_ltm_last_error(h));
			return 1;
		}

		// Output picture, as LTM's YUVWriter writes it (planes concatenated).
		Md5 md5;
		std::vector<uint8_t> frame;
		for (int pl = 0; pl < 3 && out.data[pl]; ++pl) {
			const size_t row = (size_t)out.plane_width[pl] * (out.bit_depth > 8 ? 2 : 1);
			for (int y = 0; y < out.plane_height[pl]; ++y) {
				const uint8_t *r = (const uint8_t *)out.data[pl] + y * out.stride[pl];
				frame.insert(frame.end(), r, r + row);
			}
		}
		md5.update(frame.data(), frame.size());
		const std::string got = md5.hex();
		std::string want = "-";
		if (ref_f) {
			std::vector<uint8_t> rf(frame.size());
			if (fread(rf.data(), 1, rf.size(), ref_f) == rf.size()) {
				Md5 m2;
				m2.update(rf.data(), rf.size());
				want = m2.hex();
			}
			(got == want ? md5_match : md5_mismatch)++;
		}
		if (out_f)
			fwrite(frame.data(), 1, frame.size(), out_f);

		bda_ltm_picture_info info;
		bda_ltm_get_picture_info(h, &info);
		std::string surf_report;
		for (const auto &df : dump_files) {
			bda_ltm_surface s;
			if (bda_ltm_get_surface(h, df.first.first, df.first.second, &s) != BDA_LTM_OK)
				continue;
			const size_t bytes = (size_t)s.width * s.height * s.bytes_per_sample;
			std::vector<uint8_t> ref(bytes);
			const bool ok = fread(ref.data(), 1, bytes, df.second) == bytes && memcmp(ref.data(), s.data, bytes) == 0;
			(ok ? surf_match : surf_mismatch)[df.first.first]++;
			if (!ok)
				surf_report += std::string(" MISMATCH:") + kDumpPrefix[df.first.first] + "_P" + std::to_string(df.first.second);
		}

		printf("frame %3d idr=%d enh=%d tref=%d sw=[%d,%d] tb=%d out=%dx%d/%d md5=%s ref=%s %s%s\n", frames, info.is_idr,
		       info.enhancement_enabled, info.temporal_refresh, info.step_width[0], info.step_width[1],
		       info.transform_block_size, out.width, out.height, out.bit_depth, got.c_str(), want.c_str(),
		       ref_f ? (got == want ? "OK" : "DIFF") : "", surf_report.c_str());
		frames++;
	}

	// Every dump-file frame must have been consumed by a capture.
	int leftover = 0;
	for (const auto &df : dump_files) {
		const long pos = ftell(df.second);
		fseek(df.second, 0, SEEK_END);
		if (ftell(df.second) != pos) {
			fprintf(stderr, "dump file %s_P%d has unconsumed frames\n", kDumpPrefix[df.first.first], df.first.second);
			leftover++;
		}
	}

	int surf_bad = 0;
	for (const auto &kv : surf_mismatch)
		surf_bad += kv.second;
	printf("summary: frames=%d md5 match=%d mismatch=%d\n", frames, md5_match, md5_mismatch);
	for (int id = 0; id < BDA_LTM_SURF_COUNT; ++id)
		if (surf_match.count(id) || surf_mismatch.count(id))
			printf("  surface %-20s match=%d mismatch=%d\n", kDumpPrefix[id], surf_match[id], surf_mismatch[id]);
	bda_ltm_destroy(h);
	const bool pass = rc_all == 0 && md5_mismatch == 0 && leftover == 0 && surf_bad == 0 && frames > 0 &&
	                  (!ref_f || md5_match == frames);
	printf("%s\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
