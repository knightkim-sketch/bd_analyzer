# odyssey closed-loop ME (`OdysseyClosedLoopMe`) — 설계와 검증

> 2026-10-16. 기준 소스 `~/project/odyssey-pristine` HEAD `0fab39b0`. 코어 `src/me/OdysseyClosedLoopMe.{h,cpp}`.

## 1. Knight 결정

- 시작 MV(탐색 중심)를 줄 ME 알고리즘을 고른다. 없으면 (0,0) 에서 시작한다.
- 참조 프레임은 **source 프레임** (다른 ME 와 같이 아이템의 두 프레임). recon 참조는 하지 않는다.
- 부화소는 **SAD 경로 먼저** (non-switchable 프레임 동작). switchable-filter 프레임의 IF-search model-RD 는 다음 단계.

## 2. 원본 동작 (재현한 것)

| 단계 | 원본 | 위치 |
|---|---|---|
| 블록 | 64×64 SB 안의 1×64, 4×32, 16×16, 64×8 (rect 기본 꺼짐 → 제외) | `me.c:1433` `closed_loop_superblock_motion_estimate` |
| 중심 | 같은 블록·같은 크기의 open-loop MV(1/8) → `center = (mv8 << 1) >> 4` (floor) | `me.c:913-917`, `inter_stage3.c:188-221` |
| 정수 창 | mvd ∈ [−4, 3]² (`cl_sr_w/h` 8), `clamp_search_range` pad 64, mcts 꺼짐 | `me.c:905-929`, `ody_me_common.h:785-835` |
| 정수 순서 | `mvd_x` 바깥, `mvd_y` 안쪽 | `me.c:943` |
| 정수 cost | SAD + `ods_me_get_mvcost(mvd)` 크기별 shift: 8 `(r+32)>>6`, 16 `(r+8)>>4`, 32 `(r+2)>>2`, 64 `r` | `me.c:965-967` 등 |
| best/2nd | strict `<`, best 와 같은 값은 2nd 로. 초기값 INT32_MAX, mv (0,0) | `me.c:572`, `ody_me_common.h:631-705` |
| 부화소 선택 | allow_hp 이고 \|best−2nd\| ≤ 1 (x, y 모두) 이면 1/8, 아니면 1/4. 1/8 창은 2nd 가 + 쪽이면 [−3, 4] | `me.c:535-562`, `epel_eval_window` |
| 부화소 탐색 | 64점, y 바깥, 초기 min = 정수 SAD+rate, strict `<`, 순수 SAD | `me.c:439-525` (non-switchable 분기) |
| 예측 | AV1 단일 참조: 참조 좌표 clamp, InterRound0 3, InterRound1 11, clip. 필터 TAP8_REG (allow_hp) / TAP4_REG | `inter.c:996, 1085-1230, 1555-1650` |
| SAD source | source 좌표도 그림 경계로 clamp (`ENABLE_SIMD 0` 경로) | `me.c:331-354`, `odyssey.h:23` |

재현하지 않은 것: IF-search model-RD (`me.c:458-507`), rect 블록, MCTS 타일 경계, `enable_hp_mv == 2` 의 (0,0) 예외.

## 3. 구조

| 파일 | 역할 |
|---|---|
| `src/me/OdysseyClosedLoopMe.{h,cpp}` | `odysseyPredictBlock` (AV1 보간), `odysseyClosedLoopSad`, `odysseyClosedLoopSearchBlock` (블록 하나), `OdysseyClosedLoopMe` (`IMotionEstimator`, `Algorithm::OdysseyClosedLoop`, 통계 slot 2) |
| `src/me/MeTypes.h` | `MeParams::seedAlgorithm`, `seed` (빌린 포인터), `allowHighPrecisionMv` |
| `src/integration/MeRunner.cpp` | closed-loop 이고 seed 가 있으면 같은 worker 에서 seed 를 먼저 같은 프레임 쌍으로 계산 (모든 블록 크기, cancel 공유) → 블록별 co-located 같은 크기 MV 가 중심 |
| `src/integration/MotionEstimationWidget.*` | "Odyssey closed-loop ME" 활성화, closed-loop 일 때만 보이는 행: **Start MV from** (None (0, 0) / SVT-AV1 integer ME / Odyssey open-loop ME), **Sub-pel** (1/8 pel allowed / 1/4 pel only) |
| `src/integration/MeStatisticsAdapter.cpp` | slot 2 native cost 이름 "sub-pel SAD" |

- Mainwindow 변경 없음 (기존 frame-pair 실행 경로) → upstream 패치 없음.
- seed 후보에서 Adv 는 제외 (GOP 단위 실행이라 frame-pair 와 참조가 다름).
- 비교 팝업은 정수 화소 crop 이라 부화소 MV 는 정수부로 보인다 (변경 안 함).

## 4. 검증

### 4.1 odyssey trace 비트 대조

- 인코딩 (scratch 빌드, 352×288 40 프레임 testsrc2, GF 16):
  `OdysseyEncoder -c av1 -p 2 --rc-mode 2 --crf 40 --max-gf-interval 16 --frames 40 --av1-trace-cl-inter 1 --av1-trace-param-gen 1 --enable-thread 0 --switchable-if 0 --enable-rect-inter 0 --enable-tf 0`
- 입력: source = 인코더 입력 y4m, 참조 = `out.ivf` 디코딩 recon 의 참조 POC (`trace_ody_top` 의 `dpb_write_idx`/`dpb_read_idx` 로 복원),
  중심 = trace 의 `anal me result` (인코더 open-loop MV), allow_hp = 비트스트림 frame header (`ffmpeg -bsf:v trace_headers`)
- 결과: **196,350 블록 (8×8 147,840 / 16×16 36,960 / 32×32 9,240 / 64×64 2,310) 전부 일치** — 창, 정수 best+cost, 최종 MV+SAD.
  allow_hp=1 프레임 5개(8-tap·1/8), allow_hp=0 프레임 34개(4-tap·1/4) 포함
- `--enable-tf 1`(기본) 로 같은 대조를 하면 98.50% — 불일치는 전부 ARF(co 1, 2, 17, 18, 33) 이고 창·MV 는 같고 SAD 만 다르다.
  인코더의 ARF source 가 MCTF 로 걸러진 영상이라 입력이 다른 것이지 알고리즘 차이가 아니다.

### 4.2 테스트

- 단위 `tests/unit/me-odyssey-closedloop.cpp` (16 검사): 정수 MV 예측 = 복사, 반화소 ramp (8/4-tap), 창 [−4, 3]·pad 64 clamp,
  rate shift, 정수 pan, 부화소 목표 (1/8, 1/4), seed 로 창 밖 pan 찾기
- 회귀 23 (runner: closed-loop 실행, open-loop seed), 24 (패널에서 선택 가능, 기본 seed 없음), 51 (패널 결과 == 코어 직접 호출: open-loop seed / None / 1/4 pel)

## 5. 남은 것

- switchable-filter 프레임의 IF-search (SMOOTH/SHARP, model RD — lambda, qindex, 적응 CDF 가 필요)
- recon 참조 (압축 스트림 + 원본 YUV) 로 인코더와 같은 조건 실행 — 이번에는 Knight 결정으로 source 만
- seed 로 Adv 결과 사용
