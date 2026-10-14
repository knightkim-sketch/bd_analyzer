# odyssey open-loop ME advanced (`OdysseyOpenLoopAdv`) — 설계

GF 계층 bottom-up 순서로 GOP 를 분석하고, 이미 분석한 프레임의 MV 를 TMV 후보로 쓰는 odyssey open-loop ME 재현.
기존 `OdysseyOpenLoop`(프레임 한 쌍, TMV 없음)은 그대로 둔다.

- 기준 소스: `~/project/odyssey-pristine` HEAD `0fab39b0` (`[BugFix] fix spatial_dq memset`)
- 범위: **TDMP 제외** (`enable_tdmp = 0` 동작), MCTF 없음, `latency_mode = LATENCY_HIGH`, 단일 타일
- 관련: Confluence "Odyssey Open-loop ME (lookahead)" (IT, 4519919848), `docs/ai/40-tasks/TASK-0009-motion-estimation.md`

## 1. 원본 정독 결과 (1단계)

### 1.1 GF 구조 — `common/source/ody_structure.c`

- ARF(= GF interval 위치)는 layer 1 (`:388-418`)
- `hl_build_up_sublayers` (`:102-141`) 가 구간 가운데를 internal ARF 로 두고 layer 를 1씩 올리며 재귀
- 남은 프레임 < 3 이거나 깊이 > `max_layer_depth_allowed`(= `hierarchy_lv` = 4) 이면 그 구간은 전부 leaf → **layer 5** (`MAX_ARF_LAYERS`)

| GF | L1 | L2 | L3 | L4 | L5 (leaf) |
|---|---|---|---|---|---|
| 16 | 16 | 8 | 4, 12 | 2, 6, 10, 14 | 홀수 |
| 8 | 8 | 4 | 2, 6 | — | 홀수 |
| 4 | 4 | 2 | — | — | 1, 3 |
| 2 | 2 | — | — | — | 1 |

### 1.2 분석 순서 — `analyzer/source/lookahead.c:4465-4474`

`for (layer_idx = 5; layer_idx > 0; layer_idx--)` 로 lookahead 출력 순서를 만든다 → **bottom-up**.
같은 layer 안에서는 큐 순서(표시 순서). 예 GF16: 1,3,…,15 → 2,6,10,14 → 4,12 → 8 → 16.

### 1.3 참조와 TMV 프레임 — `common/source/ody_me_common.c:324-469` `hl_set_references`

- 참조 버퍼 19칸 (`ody_enums.h:560`): 0 prevGolden, 1 prevLast, 2 Golden, 나머지 = 이번 GOP
- list 0: 과거 중 `gf_layer <= cur` 인 가장 가까운 프레임. 참조 2개를 쓸 수 있으면 prevGolden 추가
- list 1: 미래 중 `gf_layer <= cur` 인 가장 가까운 프레임
- TMV 프레임 (최대 8):
  - 비 ARF: POC −1, +1, −2, +2, −3, +3, −4, +4 순서로, 버퍼에 있고 `b_analyzed` 이고 KF 가 아닌 프레임. list 1 은 `gf_layer < 2` 제외
  - ARF: 현재 Golden(분석됨, layer 1) → layer 2 부터 layer 마다 최대 2개 (버퍼 높은 인덱스부터)
  - TMV 값은 그 프레임의 **같은 list** 결과 (`blk_mvs[list]`, SB 별 최종 64×64 MV + `rel_order_hint`)

### 1.4 후보 구성 — `analyzer/source/openloop_me.c:196-628` `ods_me_construct_candidates`

slot 순서 (`:152-175`, `#if 1` 쪽):

| slot | 후보 | 비고 |
|---|---|---|
| 0~3 | A, B, C, D | A = 왼쪽 (HW 지연), B = 위, C = 오른쪽 위, D = 왼쪽 위 |
| 4~7 | TDMP 0~3 | **TDMP 끔 → 건너뜀** (`duplicate_flag`) |
| 8~15 | TMV 0~7 | HIGH latency |
| 16 | ZERO | |
| 17 | MEDIAN | `get_median_mv(A,B,C,D)`, `pmv` 로도 쓰임 |
| 18, 19, 20 | A_PASS, **C_PASS, B_PASS** | 두 참조가 있을 때 다른 참조의 이웃 MV (방향이 다르면 부호 반전) |

- TMV 스케일 (`:409-430`): `dist_scale = (src_ref_dist << 5) / ref_rel_dist`, `x = SSR(SSR(mv.x * dist_scale, 5), 3)`.
  `src_ref_dist = 현재 − 참조`, `ref_rel_dist = TMV 프레임 − 그 프레임의 참조`. `rel_order_hint < 0` 이면 넣지 않음
- 단위: TMV 이외 slot 은 마지막에 `>> 3` (`:591`) → half 해상도 픽셀
- 범위 검사 (`:604-610`): half 해상도에서 SB 크기 + 8 여유 안쪽만 후보로. **범위 밖이어도 뒤쪽 같은 값은 중복으로 지움**
- 중복 제거는 앞에서 뒤로 (먼저 나온 slot 이 살아남음)
- 개수 제한: `ods_me_search` (`:1062`) 에서 `min(REFINED_MAX_CANDS=16, max_num_cands=6)` → **앞 6개**
- 이후 패딩 경계로 **클램프** (제거 아님, `:1092-1105`)

### 1.5 원본과 기존 bd_analyzer `OdysseyOpenLoop` 의 차이 (이번 정독에서 확인)

| 항목 | 원본 | 기존 구현 |
|---|---|---|
| `mv_rate_weight` | **항상 1** (`openloop_me.c:580`, `:1138`) | `max(1, 8/|interval|)` (추측) |
| step 3·4 의 `pmv` | center 결정 결과 `center_mvs[0]` (`:1139-1141`) | 구현 대조 필요 |
| 후보 | 21 slot → 6개 | ZERO + pmv |
| TMV | 있음 | 없음 |
| 참조 | 계층별 양방향 | `f − interval` 하나 |

### 1.6 원본 대조 수단

- `--av1-trace-anl-me <lvl>` (`encoder/av1/param.c:425`) → `trace_anl_me_dbg/output_trace_intp_shift/output_trace_vbs.txt`
  (`encoder/av1/trace/trace_analyzer_me.c:800-870`): 프레임(order_hint)·LIST·REF·SB 별 `best_64x64` MV `>> 3`
- 후보 목록(CLIST)·center(DCENT) 덤프도 있음 → 단계별 대조 가능

### 1.7 아직 확정하지 못한 것 (구현 단계에서 확인)

- 두 참조의 SB 단위 교차 처리 순서와 `num_ref` 의미 (PASS 후보·`hw_avail_*` 가 의존) — `ods_me_search_superblock` / `lookahead.c` SB 드라이버
- A 이웃 지연(`nbmv_a_before_2`)과 행 리셋의 정확한 동작
- TMV 저장 MV 의 단위 (`ody_framebuffer.c:716` quarter-pel 변환)
- 첫 GOP·KF·불완전한 마지막 GOP, 버퍼 교체 시점 (`lookahead.c:4264` `b_analyzed = false`)

## 2. 구현 규칙 (Knight 결정, 2026-10-14)

1.7 의 미확정 항목은 Knight 가 정했다. 원본 코드에서 읽은 것과 다른 점은 **[원본과 다름]** 으로 표시한다.

| # | 항목 | 규칙 |
|---|---|---|
| 1 | 두 참조의 SB 단위 교차 순서 | 같은 SB 에서 **POC 거리가 가까운 참조부터** 탐색. 다른 참조의 결과는 PASS 후보로 넘어감 |
| 2 | 이웃 A 지연 | **2 SB 이후** — A 는 2 SB 왼쪽 결과 (그 전 SB 는 A 없음) |
| 3 | center MV 옵션 | 준비된 center MV 가 있으면 후보 목록 **맨 앞**에 넣음. bd_analyzer 의 공급원은 Global motion 창 결과 (해석: 추측) |
| 4 | TMV 저장·조회 단위 | **half 해상도 8×8 = 원본 16×16 블록** 단위 MV 를 저장하고, co-located 16×16 으로 조회 **[원본과 다름: 원본 코드는 SB 64×64 `best_64x64`]** |
| 5 | 첫 GOP | 프레임 0 = I(KF). 이후 GF 개씩 `B…B P` — P 는 GOP 끝 앵커(list 0 만) |
| 6 | 마지막 GOP | 남은 프레임으로 GF 계층을 구성하고, 계층에 들어가지 못한 프레임은 **순차 P** (list 0 = 직전 프레임) |

### 2.1 프레임 유형 예 (GF 16, 총 40 프레임)

- 0: I
- 1~16: B(15) + P(16) — 처리 순서 L5(1,3,…,15) → L4 → L3 → L2 → P(16)
- 17~32: 같은 구조, P(32)
- 33~39: 7 프레임 → GF 계층 구성 후 남는 프레임은 P P P (정확한 분할은 `OdysseyGfStructure` 단위 테스트에 고정)

## 3. 원본 대조 결과 (2026-10-14)

- 조건: Motorcycle_512x288 8-bit, 40 프레임, `--max-gf-interval 16 --enable-tdmp 0 --enable-tf 0 --enable-tmv 1`,
  Adv 는 `TmvGrid::Sb64` (원본 코드와 같은 SB 단위 TMV)
- 기준: odyssey `--av1-trace-anl-me 2` 의 `msear` (SB 별 8×8~64×64 MV), `clist` (pmv·후보), `dcent` (center),
  `input_trace/analyzer_frame_info` (참조·TMV 개수), `input_trace/cur_frame` (half 영상)
- 결과: **261,800 블록 MV 100% 일치** (64×64 3,080 / 32×32 12,320 / 16×16 49,280 / 8×8 197,120),
  SB 3,080 개의 pmv·후보 목록·center·VBS center 100% 일치, half 영상 화면 안쪽 화소 100% 일치
- 대조 중 원본과 맞춘 것 (기존 `OdysseyOpenLoop` 과 다른 점, 개선 문서 C6~C12):
  rate 의 `divide_round` 는 시프트, step 3 rate 의 `SSR(mv − SSR(pmv,1),1)`, step 4 SSE 의 행·열 2:1 서브샘플,
  후보 격자의 비트 교차 순회, `clamp_search_range` 로 창 이동, 3a 영역 결과 안정 정렬, SB 배수로 늘린 영상에서 resample
- 주의: trace 의 8×8·16×16 은 Morton 순서 (`b8_position`), 파일 번호는 `analyzer_frame_info` 는 분석 순서, `msear`/`clist`/`dcent` 는 POC
- `TmvGrid::Block16` (Knight 결정, 기본값) 은 원본 trace 가 없어 대조 대상이 아니다
