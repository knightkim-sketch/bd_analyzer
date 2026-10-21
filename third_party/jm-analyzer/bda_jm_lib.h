/* bda_jm_lib.h - force-included into every JM source compiled for libbda-jm-analyzer.so.
 *
 * - Console output: JM prints progress, warnings and statistics with printf()/fprintf(stdout|stderr).
 *   The host application owns stdout, so these calls are routed to bda_jm_printf/bda_jm_fprintf, which
 *   drop the text (or forward it to stderr when bda_jm_set_logging(1)) and keep the last message as the
 *   error text. fprintf() to real files is passed through.
 * - Termination: exit() (error(), no_mem_exit(), FMO errors) and failed assert() checks would end the
 *   host process. They unwind to the wrapper's decoder entry point instead (longjmp); the handle is then
 *   dead (BDA_JM_ERR_DECODE). scripts/setup-jm-analyzer.sh rewrites assert(...) to BDA_JM_ASSERT(...)
 *   in the build copy; the check itself is kept, so valid streams decode exactly as with ldecod.
 * - Hooks called from the patched JM sources (patches/0001-library-hooks.patch, all guarded by
 *   BDA_JM_LIBRARY), implemented in bda_jm.cpp.
 */
#ifndef BDA_JM_LIB_H
#define BDA_JM_LIB_H

#ifdef BDA_JM_LIBRARY

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

int bda_jm_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int bda_jm_fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void bda_jm_exit(int code) __attribute__((noreturn));
void bda_jm_assert_failed(const char *expr, const char *file, int line) __attribute__((noreturn));

struct video_par;
struct slice;
struct macroblock_dec;
struct storable_picture;
struct nalu_t;

/* annexb.c getChunk(): copy up to size pushed bytes; suspends the decoder while no data is pending;
 * returns 0 at end of stream. */
int bda_jm_hook_read(unsigned char *buf, int size);
/* nalu.c read_next_nalu(): NAL unit read (still escaped) */
void bda_jm_hook_nalu(struct video_par *p_Vid, struct nalu_t *nalu);
/* image.c read_new_slice(): the NAL unit just read becomes this slice's data */
void bda_jm_hook_slice_nal(struct slice *currSlice);
/* ldecod.c free_slice() */
void bda_jm_hook_free_slice(struct slice *currSlice);
/* image.c decode_one_slice(): before read_one_macroblock, after it, after exit_macroblock */
void bda_jm_hook_mb_begin(struct slice *currSlice, struct macroblock_dec *currMB);
void bda_jm_hook_mb_read(struct slice *currSlice, struct macroblock_dec *currMB);
void bda_jm_hook_mb_end(struct slice *currSlice, struct macroblock_dec *currMB);
/* image.c exit_picture(): picture fully decoded, before store_picture_in_dpb() */
void bda_jm_hook_picture_done(struct video_par *p_Vid, struct storable_picture *p);
/* output.c write_out_picture(): picture output in output order */
void bda_jm_hook_output(struct video_par *p_Vid, struct storable_picture *p);
/* mbuffer.c free_storable_picture() */
void bda_jm_hook_free_picture(struct storable_picture *p);
/* ldecod.c error() */
void bda_jm_hook_fatal(const char *text, int code) __attribute__((noreturn));

/* image.c read_new_slice()'s function-static pending NAL unit, made swappable per decoder */
extern struct nalu_t *bda_jm_pending_nalu;

#ifdef __cplusplus
}
#endif

#define printf bda_jm_printf
#define fprintf bda_jm_fprintf
#define exit bda_jm_exit
#define BDA_JM_ASSERT(expr) ((expr) ? (void)0 : bda_jm_assert_failed(#expr, __FILE__, __LINE__))

#endif /* BDA_JM_LIBRARY */

#endif /* BDA_JM_LIB_H */
