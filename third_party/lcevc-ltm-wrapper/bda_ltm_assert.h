// bda_ltm_assert.h - force-included into every LTM source compiled for libbda-lcevc-ltm.so.
//
// scripts/setup-lcevc-ltm.sh rewrites assert(...) to BDA_LTM_ASSERT(...) in the build copy of LTM.
// A failed check throws std::runtime_error (caught by bda_ltm_decode) instead of aborting the host.
#pragma once

[[noreturn]] void bda_ltm_assert_failed(const char *expr, const char *file, int line);

#define BDA_LTM_ASSERT(expr) ((expr) ? (void)0 : bda_ltm_assert_failed(#expr, __FILE__, __LINE__))
