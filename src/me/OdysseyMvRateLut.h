// MV rate lookup tables lifted verbatim from odyssey-encoder.
//
// Source: odyssey-encoder-algoritm, branch ft-od-sm-opt (64bfeafb),
//         common/include/ody_me_common.h  (mvd_rate_lut_lv1..lv4)
// Extracted rather than reimplemented: these are measured rate values, not a formula.
//
// ods_me_get_mvcost() picks a table by magnitude, each coarser than the last:
//   |mv| <  128 -> lv1[|mv|]
//   |mv| <  384 -> lv2[(|mv| - 128) >> 1]
//   |mv| <  896 -> lv3[(|mv| - 384) >> 2]
//   otherwise   -> lv4[(|mv| - 896) >> 3]     (|mv| clamped to 1919)
//
// Quirk reproduced on purpose: lv4 is declared [128] but odyssey only writes 120 initialisers,
// so C zero-fills indices 120..127. Those map to |mv| 1856..1919 (the clamp ceiling), where the
// rate therefore reads 0 instead of continuing to grow. It looks like an oversight upstream, but
// matching it is the point of this file - a "corrected" table would give different MVs than the
// encoder does. The zeros below are ours, written out so the shape is visible rather than implied.
//
// Keep in sync with that file if odyssey retunes them.
#pragma once

#include <cstdint>

namespace bda::me
{

inline constexpr int kOdysseyRateMaxMv = 128; // ODY_ME_RATE_MAX_MV

inline constexpr std::uint16_t kOdysseyMvdRateLutLv1[kOdysseyRateMaxMv] = {
      17,   89,  117,  137,  153,  165,  176,  185,
     193,  201,  207,  213,  219,  224,  229,  233,
     237,  241,  245,  249,  252,  255,  258,  261,
     264,  267,  269,  272,  274,  277,  279,  281,
     283,  285,  287,  289,  291,  293,  295,  297,
     298,  300,  302,  303,  305,  306,  308,  309,
     311,  312,  314,  315,  316,  317,  319,  320,
     321,  322,  324,  325,  326,  327,  328,  329,
     330,  331,  332,  333,  334,  335,  336,  337,
     338,  339,  340,  341,  342,  343,  344,  345,
     346,  346,  347,  348,  349,  350,  350,  351,
     352,  353,  354,  354,  355,  356,  357,  357,
     358,  359,  359,  360,  361,  362,  362,  363,
     364,  364,  365,  365,  366,  367,  367,  368,
     369,  369,  370,  370,  371,  372,  372,  373,
     373,  374,  374,  375,  376,  376,  377,  377,
};

inline constexpr std::uint16_t kOdysseyMvdRateLutLv2[kOdysseyRateMaxMv] = {
     378,  379,  380,  381,  382,  383,  384,  385,
     386,  387,  388,  389,  390,  390,  391,  392,
     393,  394,  395,  396,  396,  397,  398,  399,
     400,  400,  401,  402,  403,  403,  404,  405,
     406,  406,  407,  408,  408,  409,  410,  411,
     411,  412,  412,  413,  414,  414,  415,  416,
     416,  417,  418,  418,  419,  419,  420,  420,
     421,  422,  422,  423,  423,  424,  424,  425,
     426,  426,  427,  427,  428,  428,  429,  429,
     430,  430,  431,  431,  432,  432,  433,  433,
     434,  434,  435,  435,  436,  436,  436,  437,
     437,  438,  438,  439,  439,  440,  440,  440,
     441,  441,  442,  442,  443,  443,  443,  444,
     444,  445,  445,  445,  446,  446,  447,  447,
     447,  448,  448,  449,  449,  449,  450,  450,
     451,  451,  451,  452,  452,  452,  453,  453,
};

inline constexpr std::uint16_t kOdysseyMvdRateLutLv3[kOdysseyRateMaxMv] = {
     453,  454,  455,  456,  456,  457,  458,  458,
     459,  460,  460,  461,  462,  462,  463,  464,
     464,  465,  465,  466,  467,  467,  468,  468,
     469,  469,  470,  471,  471,  472,  472,  473,
     473,  474,  474,  475,  475,  476,  477,  477,
     478,  478,  479,  479,  480,  480,  481,  481,
     482,  482,  482,  483,  483,  484,  484,  485,
     485,  486,  486,  487,  487,  487,  488,  488,
     489,  489,  490,  490,  490,  491,  491,  492,
     492,  493,  493,  493,  494,  494,  495,  495,
     495,  496,  496,  497,  497,  497,  498,  498,
     498,  499,  499,  500,  500,  500,  501,  501,
     501,  502,  502,  502,  503,  503,  504,  504,
     504,  505,  505,  505,  506,  506,  506,  507,
     507,  507,  508,  508,  508,  509,  509,  509,
     510,  510,  510,  511,  511,  511,  511,  512,
};

inline constexpr std::uint16_t kOdysseyMvdRateLutLv4[kOdysseyRateMaxMv] = {  // odyssey writes only 120 values; the rest are C zero-fill
     512,  513,  513,  514,  514,  515,  516,  516,
     517,  517,  518,  519,  519,  520,  520,  521,
     521,  522,  522,  523,  523,  524,  524,  525,
     525,  526,  527,  527,  527,  528,  528,  529,
     529,  530,  530,  531,  531,  532,  532,  533,
     533,  534,  534,  535,  535,  535,  536,  536,
     537,  537,  538,  538,  538,  539,  539,  540,
     540,  541,  541,  541,  542,  542,  543,  543,
     543,  544,  544,  545,  545,  545,  546,  546,
     546,  547,  547,  548,  548,  548,  549,  549,
     549,  550,  550,  550,  551,  551,  551,  552,
     552,  553,  553,  553,  554,  554,  554,  555,
     555,  555,  556,  556,  556,  557,  557,  557,
     558,  558,  558,  558,  559,  559,  559,  560,
     560,  560,  561,  561,  561,  562,  562,  562,
       0,    0,    0,    0,    0,    0,    0,    0,
};

} // namespace bda::me
