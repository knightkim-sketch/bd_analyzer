// bda_ltm_parallel.h - row-parallel loops for the LTM decoder built into libbda-lcevc-ltm.so.
//
// patches/0002-parallel-decode.patch calls bda_ltm_parallel_for() from LTM's per-pel loops (surface
// generation, upsampling) and from the per-layer entropy decoding. Every call site only reads shared
// data and writes its own rows (or its own layer), so the result is bit-exact with the serial loop.
//
// The pool is created on first use with one thread per core (at most 16). BDA_LTM_THREADS=<n> in the
// environment overrides that; BDA_LTM_THREADS=1 runs every loop on the calling thread.
#pragma once

#include <functional>

// Run fn(begin, end) over [0, count) split into contiguous chunks of at least min_chunk items, on the
// pool and the calling thread, and return when all chunks are done. The first exception thrown by a
// chunk is rethrown here. A call from inside fn runs serially.
void bda_ltm_parallel_for(unsigned count, unsigned min_chunk, const std::function<void(unsigned, unsigned)> &fn);

// Number of threads a loop may use (pool + caller).
unsigned bda_ltm_parallel_threads();

// Rows per chunk so that a chunk covers at least ~8K samples of a row `width` samples wide.
inline unsigned bda_ltm_min_rows(unsigned width) { return width ? (8192 + width - 1) / width : 1; }
