# odyssey open-loop ME — 개선 후보 (구현 후 검토용)

`OdysseyOpenLoopAdv` 는 원본(odyssey-pristine 0fab39b0) 재현이 목표다. 재현 중에 눈에 띈 개선 여지는 구현에
섞지 않고 여기에 모은다. 구현·원본 대조가 끝난 뒤 Knight 와 하나씩 검토한다.

- 상태: `후보` → `검토 중` → `채택` / `기각` (기각도 사유와 함께 남긴다)
- 근거는 원본 파일:라인 기준. 효과 수치가 없는 항목은 **미검증**

## A. 알고리즘 (odyssey 원본)

| # | 관측 | 개선 아이디어 | 근거 | 상태 |
|---|---|---|---|---|
| A1 | bottom-up 이라 L5(leaf) 는 TMV 가 거의 없음 — GOP 첫 홀수 프레임은 TMV 0개 (trace POC 1: tmv 0) | 상위 layer 를 먼저 분석해 하위에 투영하는 top-down 순서 (Knight 의 최초 제안). 인코딩 순서와도 같다 | `lookahead.c:4465`, trace 표 | 후보 |
| A2 | TMV 는 co-located 위치를 거리 비율로만 키움. 움직임 궤적을 따라 옮기지 않음 | 궤적 투영 (TDMP 가 일부 담당). 빠른 움직임에서 co-located 가 다른 물체를 가리킴 | `openloop_me.c:409-430` | 후보 |
| A3 | 후보 21 slot 을 앞 6개로 자름 → ZERO·MEDIAN·PASS 가 자주 잘림. ZERO 는 비어 있는 공간 slot(0,0) 덕에 들어옴 | slot 우선순위 재배치 또는 cap 확대. 특히 TMV 8개가 ZERO 앞이라 TMV 가 많으면 ZERO 가 빠짐 | `:1062`, `:152-175` | 후보 |
| A4 | MEDIAN 은 A 대신 D 로 계산 (`bvalid=false`, "don't use cdd_a") | A 지연(2~4 SB)이 커서 뺀 것으로 보임. 효과 측정 필요 | `:579` | 후보 |
| A5 | PASS slot 순서가 A, **C, B** | 의도인지 확인 (B 가 C 보다 가까운 이웃) | `:172-174` | 후보 |
| A6 | PASS 벡터·방향 플래그가 `static` — 프레임이 바뀌어도 직전 프레임 마지막 SB 값이 남음 | 프레임 시작 시 초기화. HW 동작과 맞는지 확인 필요 | `:262-267` | 후보 |
| A7 | `mv_rate_weight` 를 계산하는 인자가 있지만 항상 1 | 참조 거리별 weight (기존 bd_analyzer 추측식처럼) 효과 측정 | `:580`, `:1138` | 후보 |
| A8 | VBS(step 4) 를 원본이 아니라 half→org 업스케일 영상에서 탐색 → 고주파 손실 | 원본 영상에서 VBS 탐색 시 MV 품질 비교 | `ody_me_common.c:718` | 후보 |
| A9 | step 3a 가 "1/4" 라 불리지만 half 영상에 step 2 | 이름·주석 정리 (기능 변화 없음) | `:1099` | 후보 |
| A10 | `mv_a_valid_sb_row`: 마지막 SB 행이 부분 행이면 SB 행 수 홀짝으로 A 사용 여부 결정 | 의도 확인 (HW 버퍼 구조로 추정) | `:300-301` | 후보 |
| A11 | TMV 저장 단위: 원본 코드는 SB 64×64 하나, Knight 결정은 16×16 | 두 방식 비교 실험 (Adv 의 `TmvGrid` 옵션으로 바로 가능) | `ody_framebuffer.c:716` | 후보 |

## B. 원본 코드 결함 (분석 중 발견)

| # | 관측 | 영향 | 근거 | 상태 |
|---|---|---|---|---|
| B1 | `enc->trc_path` 가 `char[100]` 고정 버퍼, 길이 검사 없이 `memcpy` | trace 경로가 100자를 넘으면 segfault (실측) | `encode.h:1071`, `encode.c:1393-1397` | 후보 |
| B2 | `trace_anl_me_output_debug()` (`output_trace_vbs.txt`) 호출처 없음 | 죽은 코드 | `trace_analyzer_me.c:790-870` | 후보 |

## C. bd_analyzer 기존 `OdysseyOpenLoop` (원본과 다른 점)

Knight 지시로 기존 구현은 유지. 원본과 맞출지 검토 대상이다.

| # | 차이 | 원본 | 상태 |
|---|---|---|---|
| C1 | `mv_rate_weight = max(1, 8/|interval|)` (추측) | 항상 1 | 후보 |
| C2 | step 3 rate 기준 = median pmv | center 결정 결과 `center_mvs[0]` | 후보 |
| C3 | 후보 = ZERO + pmv | 21 slot → 6개 | 후보 |
| C4 | MEDIAN 이 A 기준 (`bvalid=true` 규칙) | D 기준 | 후보 |
| C5 | 참조 = `f − interval` 하나 | GF 계층별 양방향 | 후보 (Adv 로 해결) |
| C6 | step 4 SSE 를 행만 2:1 서브샘플 | 행·열 모두 2:1 (`ods_me_compute_sse` stride) | 후보 (Adv 는 `odysseyHw` 로 원본대로) |
| C7 | step 3a/3b/4 의 8×8 후보 격자를 래스터 순서로 순회 | 비트 교차(Z) 순서 — 동점 처리가 달라짐 | 후보 (Adv 는 원본대로) |
| C8 | step 3 rate = `mvcost(mv − pmv)` | `mvcost(SSR(mv − SSR(pmv,1), 1))` (scale 1) | 후보 (Adv 는 원본대로) |
| C9 | 탐색 범위 밖 후보를 하나씩 클램프 | step 3a 는 center 를 먼저 조정하고 영역마다 `clamp_search_range` 로 창을 옮김 | 후보 (Adv 는 원본대로) |
| C10 | **rate 를 나눗셈으로 계산** — `divideRound(x, 6)` = x/6, `(x, 8)` = x/8, step 2 는 x/2 | `divide_round(value, bits)` 는 **시프트**: x/64, x/256, x/4 (`ody_base.h:34`). 기존 구현은 rate 가 10배 이상 커서 먼 MV 를 놓침 — **원본 불일치의 가장 큰 원인** | 후보 (Adv 는 원본대로) |
| C11 | half·upscale 영상을 원래 크기에서 만들고 가장자리를 복제 | SB 배수로 늘린 영상에서 resample — 아래·오른쪽 패딩 내용이 다름 | 후보 (Adv 는 원본대로) |
| C12 | step 2 refine 점마다 클램프 | 클램프 없음 (후보만 미리 클램프) | 후보 (Adv 는 원본대로) |

## D. bd_analyzer 구현 (성능·구조)

| # | 관측 | 개선 아이디어 | 상태 |
|---|---|---|---|
| D1 | Adv 는 대상 GOP 앞의 모든 GOP 를 다시 분석 (TMV 가 이전 결과에 의존) | 결과 캐시를 실행 사이에 유지, 긴 클립은 진행률·취소 확인 | 후보 |
| D2 | 검색마다 half/upscale 평면을 복사 (`OdySearchPlanes` 가 값 보관) | 포인터로 공유 | 후보 |
