# LCEVCdec public API headers

`LCEVC/lcevc_dec.h` and `LCEVC/api_defs.h`, unchanged from V-Nova LCEVCdec 4.3.0 (tag `4.3.0`,
https://github.com/v-novaltd/LCEVCdec), BSD-3-Clause-Clear (`LICENSE.md`; no patent licence is
granted).

bd-analyzer compiles against these declarations only: the library itself is built by
`scripts/setup-lcevcdec.sh` and loaded at run time (`src/integration/LcevcBackend.cpp`), so the
application builds and runs without it. Keep this copy in step with the version that script pins.
