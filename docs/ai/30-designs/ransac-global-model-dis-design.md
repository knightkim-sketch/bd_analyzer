# RANSAC 전역 모델 → GM 창 / closed-loop 시작 MV / DIS — 설계

> 2026-10-17. 위키 "RANSAC — global motion / DIS 적용 검토"(4612751362)의 제안 3가지 구현.
> 그림: `docs/ai/30-designs/img/ransac-diagrams.py` (ransac-loop.png, ransac-usage.png).

## 1. 코어 — `src/me/GlobalModel.{h,cpp}` (Qt·OpenCV free)

- 점: ME 결과의 한 블록 크기(기본 16×16) 블록마다 (블록 중심) → (중심 + MV/8). 그림 밖으로 걸친 블록은 제외.
  SVT static-bypass 블록은 (0,0) 점으로 포함 — 빼면 정지 프레임 쌍에서 점이 0 개가 되어 실패한다 (회귀 52 에서 5 건 관측)
- 모델 (`GmModelType`, `MeTypes.h`): translation / rotzoom / affine. 결과 `p[6]` 은 affine 형식
  `x' = p0 + p2 x + p3 y, y' = p1 + p4 x + p5 y` 이고 **현재 그림 위치 → 참조 위치** (MV = model(x) − x, 블록 MV 와 같은 부호)
- RANSAC 은 libaom (`aom_dsp/flow_estimation/ransac.c`, v3.15.0) 규칙을 따른다 — 코드로 확인함:

| 항목 | 값 |
|---|---|
| 최소 표본 | translation 1, rotzoom 2, affine 3 |
| 점 부족 | 점 < 최소 표본 × 5 이면 오류 (`MINPTS_MULTIPLIER 5`) |
| 난수 | LCG `state = state·1103515245 + 12345`, `randint = (state·n) >> 32`, seed = 점 개수 → 결정적 |
| 시도 | 20 회 고정 (`NUM_TRIALS`) |
| inlier | 거리 < 1.25 px (`INLIER_THRESHOLD`), 최소 inlier = max(0.1·n, 최소 표본) |
| 선택 | inlier 최다, 같으면 오차 제곱합이 작은 쪽 |
| 재추정 | inlier 전체 최소제곱 → inlier 가 더 늘지 않을 때까지, 최대 5 회 (`NUM_REFINES`) |

- 최소제곱은 그림 중심 기준 좌표로 풀어 조건수를 낮춘다 (rotzoom 4×4, affine 3×3 두 개, Gaussian elimination)
- `gmModelMvAt` (1/8 pel), `synthesiseModelSeed` (4 크기 × SB 격자 전 블록, 블록 중심에서 모델 MV)
- libaom 과 다른 점: 대응점이 FAST 코너 + DIS optical flow 가 아니라 **블록 ME 벡터**, translation 모델도 허용,
  정수화(`av1_refine_integerized_param`)·error advantage 판정은 하지 않음 (비트스트림에 쓰지 않으므로)

## 2. GM 창

- 파라미터 줄에 **Model** (off / translation / rotzoom / affine, 기본 rotzoom). 켜면 worker 가 같은 프레임 쌍(poc−1)에
  SVT integer ME 를 돌려 16×16 벡터로 `fitGlobalModel` (프레임은 pad 64 로 읽음 — 창 추정기에는 영향 없음)
- Frames 표 열 추가: model, model MV at centre (px), scale / angle (deg), inliers
- overlay: 303 **GM model inlier** (16×16 블록 값 1 = 녹색 inlier, 0 = 빨강 outlier), 304 **GM model vector** (64×64 블록마다 모델 MV)
- **Export model CSV…**: `poc,ref_poc,type,ok,p0..p5,inliers,total,rms` (odyssey 용 GM CSV 는 그대로)

## 3. closed-loop 시작 MV

- ME 패널 Start MV from 에 **Global model (SVT ME)**, 그때만 보이는 **Model** 행 (Translation / RotZoom 기본 / Affine)
- `MeParams::seedModel`. runner 가 SVT ME (모든 크기) → `fitGlobalModel` (16×16) → `synthesiseModelSeed` 를 seed 로 closed-loop 실행.
  피팅이 실패하면 SVT 벡터를 그대로 seed 로 쓴다 (화면에 따로 표시하지 않음 — MainWindow 변경 없이 runner 안에서 처리)
- `Algorithm` enum 은 늘리지 않음 (통계 slot·factory·Block Info 로 번지므로)

## 검증 (1~3)

- unit `gm-model` (14): outlier 30% 섞인 장에서 translation (+5.25, −3) / rotzoom (scale 1.02, 1.5°) / affine(shear) 복원,
  outlier 마스크 정확, 두 번 실행 비트 동일, static-bypass 는 (0,0) 점·가장자리 블록 제외, 점 부족 오류, seed 격자 전체
- 회귀 43: 1280×720 pan (+8, +4) 5 프레임 — 모델 중심 MV (8, 4), scale 1, 각도 0, inlier 3,572–3,600 / 3,600,
  overlay 303·304 데이터, 모델 CSV 한 프레임 한 줄
- 회귀 23 (runner: global model seed 실행), 51 (패널 "Global model (SVT ME)" == 코어 직접 호출 SVT → fit → seed → closed-loop)

## 4. DIS (digital image stabilisation) + generator camera shake

### 4.1 구성

| 파일 | 역할 |
|---|---|
| `src/filter/AffineWarp.{h,cpp}` | uint16 plane 의 bilinear affine warp (8/10 bit), 4:2:0 frame warp. Qt·OpenCV free |
| `src/filter/Stabilizer.{h,cpp}` | 프레임 간 모델 → 누적 궤적 → 평활 (이동평균 / Gaussian / Kalman causal / Kalman RTS) → 보정 warp, 궤적 CSV, 절대 프레임 번호용 `stabilizeByFrame`. Qt free |
| `src/integration/StabilizedY4mExport.{h,cpp}` | File > Export Stabilized Y4M (2 pass), 비대화형 오버로드, 모델을 받아 pass 2 만 하는 오버로드 (GM 창용) |
| 패치 `0071-stabilized-y4m-export.patch` | upstream `Mainwindow.cpp` 의 File 메뉴 1 항목 + include |
| `src/generator/Scene.{h,cpp}` | `CameraMotion` (Off / RandomWalk / Sine), `cameraAt`, `cameraInOutput`, `writeTrajectoryCsv` |
| `src/integration/SequenceGeneratorWindow.{h,cpp}` | Sequence 그룹의 Camera shake UI, JSON `camera`, Generate 시 `<y4m>.camera.csv` |

### 4.2 좌표 규약

- 모든 위치는 연속 좌표이고 화소 중심이 +0.5 에 있다: luma 샘플 (i, j) 는 (i + 0.5, j + 0.5).
  generator 의 화소 중심(`px + 0.5`)과 `fitGlobalModel` 의 블록 중심(`x + size/2`)이 같은 규약이라
  모델·보정을 반 화소 어긋남 없이 그대로 warp 에 쓴다.
- warp 는 역매핑이다: 출력 p 는 입력 `q = (p0 + p2 x + p3 y, p1 + p4 x + p5 y)` 에서 읽는다
  (`GmModelResult::p` 와 같은 2x3 형식). bilinear, 그림 밖은 가장자리 샘플 복제, 반올림 후 bit depth 로 clip.
- chroma 위치 o (luma 단위, `luma 위치 = 2·chroma 위치 + o`):
  Centre (0, 0) = C420jpeg, Left (−0.5, 0) = C420mpeg2, TopLeft (−0.5, −0.5) = generator 의 표본 위치.
  luma warp (A, b) 에 대응하는 chroma warp 는 `A_c = A`, `b_c = (A·o + b − o) / 2`.
  순수 이동은 siting 과 무관하게 정확히 절반이고, 회전·zoom 이 있을 때만 siting 이 의미를 가진다.
  export 는 Y4mWriter 가 선언하는 C420jpeg 에 맞춰 Centre 를 쓴다.
- chroma plane 크기는 `width/2 × height/2` (Y4mWriter 와 같음).

### 4.3 궤적·평활·보정 (Stabilizer)

- 입력: 프레임 k (≥1) 의 모델 k → k−1 (현재 위치 → 참조 위치). 실패한 피팅은 identity (= 움직임 없음).
- 모델을 그림 중심 c 에 대한 similarity 로 읽는다: `ref = c + sR(φ)(cur − c) + t`, `t = model(c) − c`.
  affine 이면 선형부의 rotzoom 평균 `a = (p2+p5)/2`, `b = (p4−p3)/2` 로 s, φ 를 얻는다.
- 프레임 k−1 → k 의 내용 이동은 그 역: 중심 이동 `−(sR)⁻¹ t`, 각도 `−φ`, log scale `−log s`.
  이를 프레임 0 (전부 0) 부터 합산한 것이 raw 궤적 (dx, dy px / angle deg / log scale).
  평행이동과 중심 회전·zoom 각각에는 정확하고, 둘이 함께 있으면 1차 근사다 (일반적인 DIS 의 선택).
- 평활 (`StabilizerParams`): 4 성분 (dx, dy, angle, log scale) 각각 독립. 기본 Gaussian.
  - MovingAverage: 반경 R (기본 15) 중심 이동평균. 양 끝은 창을 시퀀스 안으로 잘라 남은 프레임만 평균한다
    (가중치 합 1 유지, 끝 근처의 꾸준한 pan 은 약간 치우친다). 이전 radius 전용 구현과 비트 동일.
  - Gaussian: `w(d) = exp(−d²/2σ²)`, `|d| ≤ ⌈3σ⌉` (기본 σ 6 → ±18), 잘린 창을 다시 정규화 (가중치 합 1).
  - Kalman: 상태 (위치, 속도) 등속 모델, `F = [[1,1],[0,1]]`, 공정 잡음 `Q = q·[[1/4,1/2],[1/2,1]]`
    (이산 white-noise 가속도), 측정 `H = [1,0]`, 분산 r. 시작 상태 = 첫 측정값, 속도 0, 공분산 `diag(1e6·r, 1e6·r)`.
    결과는 q/r 비에만 의존하므로 단위가 다른 성분 (px, deg, log) 에 같은 (q, r) 를 쓴다.
    기본 q 0.002, r 4 — 아래 실측으로 정했다 (계획의 0.05 는 추측이었다).
    - KalmanCausal: 순방향 필터 위치 = 평활 경로. 프레임 k 결과가 k 이후에 의존하지 않는다 (실시간·HW DIS 조건).
    - KalmanRts: 순방향 + Rauch–Tung–Striebel 역방향 `x_k|n = x_k|k + C_k (x_k+1|n − x_k+1|k)`, `C_k = P_k|k Fᵀ P_k+1|k⁻¹`.
  - `stabilizeByFrame(modelsByFrame, first, last, …)`: 절대 프레임 번호 → 모델 map 을 범위 안 벡터로 바꿔 `stabilize`.
    모델이 없는 프레임 (실패·범위 밖) 은 identity. export 의 precomputed 오버로드와 GM 창이 같이 쓴다.
- 기본값 실측 (2026-10-18, 회귀 52 클립: 320x192 40 프레임 RandomWalk 6 px, 렌더된 카메라 = 측정 궤적,
  프레임 간 이동 std 감소율, seed 3 / 1 / 7 / 11):

  | 방법 | seed 3 | seed 1 | seed 7 | seed 11 |
  |---|---|---|---|---|
  | Moving average R 15 | 94.3% | 91.0% | 94.4% | 93.2% |
  | Gaussian σ 6 | 90.2% | 87.3% | 92.2% | 91.2% |
  | Kalman RTS q 0.05 r 4 (계획 초안) | 79.1% | 74.0% | 83.1% | 79.2% |
  | Kalman RTS q 0.002 r 4 (기본) | 91.1% | 87.1% | 92.5% | 92.5% |
  | Kalman causal q 0.05 r 4 | 47.5% | 39.6% | 51.7% | 44.9% |
  | Kalman causal q 0.002 r 4 (기본) | 61.7% | 51.9% | 59.5% | 56.7% |
  | Kalman causal q 0.0001 r 4 | 65.7% | 55.5% | 61.5% | 58.8% |

  causal 등속 필터는 어떤 q 에서도 80% 에 못 미친다: 프레임 k 이후를 볼 수 없으니, 경계가 있는 random walk 에서
  흔들림을 따라가거나 (지연 없음) 카메라보다 늦게 따라가거나 (평활) 둘 중 하나다. 40 프레임 클립이라 초기 diffuse
  구간도 몫이 있다 (처음 8 프레임을 빼면 seed 7 은 0.45 px 까지 내려감). 회귀 52 는 causal 에 40% 와 "RTS 보다 작음" 을 요구한다.
- 보정 = 평활 − raw. 프레임 k 의 내용을 raw 위치에서 평활 위치로 옮기는 변환
  `p_out = c + S R(a)(p_in − c) + d` 를 warp 용 역매핑 `p_in = c + (1/S)R(−a)(p_out − c − d)` 로 넘긴다.
- 궤적 CSV: `frame,raw_dx,raw_dy,raw_angle,raw_scale,smooth_*,corr_*` (scale 은 배율, 각도 deg).

### 4.4 Export Stabilized Y4M

- 검사·Y4mWriter·진행률·취소는 Export Filtered Y4M 과 같다 (planar 4:2:0 / 4:0:0, 8/10 bit LE).
- 옵션 대화상자: 모델 Translation / RotZoom (기본 RotZoom), Method (Moving average / Gaussian / Kalman causal /
  Kalman RTS, 기본 Gaussian) 와 그 방법의 파라미터만 (R / σ / Q·R, `QFormLayout::setRowVisible`).
  `StabilizedExportOptions::smoothing` 이 예전 `radius` 를 대신한다 (예전 동작 = method MovingAverage + radius).
- pass 1: 프레임 k−1, k 를 `readRawFrame` → `lumaPlaneFromRawYuv(…, kOdysseyPadFull)` →
  SVT integer ME (blockSizes = {16}, **static bypass 끔**) → `fitGlobalModel(Blk16)`.
  `fitGlobalModel` 은 이제 bypass 블록을 (0,0) 점으로 센다 (정지 쌍도 피팅된다). 그래도 끄는 이유는
  일관성이다: 모든 점이 실제 탐색 벡터가 되어 SVT bypass 임계값에 결과가 좌우되지 않고, GM 창 (기본 MeParams,
  bypass 끔) 과 같은 점 집합이 된다. (예전 기록: bypass 블록을 빼던 시절에는 켜면 정지 쌍이 `too few points (0)`.)
- pass 2: Y/U/V 를 같은 보정으로 warp 하고 쓴다. 출력 크기 = 입력, crop 없음, 가장자리 복제.
  `<output>.trajectory.csv` 를 함께 쓴다.
- 비대화형 진입점 `exportStabilizedY4m(item, path, options, progress)` 는 대화상자 없이 같은 일을 하고
  결과(프레임 수, 실패 피팅 수, 궤적, 모델)를 돌려준다. 메뉴 항목은 이를 진행률 대화상자로 감싼다.
- precomputed 오버로드 `exportStabilizedY4m(item, path, modelsByFrame, smoothing, progress)`: pass 1 을 건너뛰고
  `stabilizeByFrame(modelsByFrame, range.first, range.second, …)` 로 궤적을 만든 뒤 같은 pass 2 함수를 쓴다.
  key 는 item 의 절대 프레임 번호 (k → k−1 모델), 없는 프레임은 identity. 같은 모델이면 Y4M·CSV 가 비트 동일
  (회귀 52). `progress(done, total)` 은 쓴 프레임마다 (total = 범위 프레임 수), false 면 취소 (쓴 프레임은 남는다).

### 4.5 generator camera shake

- 카메라 상태 (dx, dy, angle) 는 장면 위치 q 를 그림 위치 `p = c + R(angle)(q − c) + (dx, dy)` 로 옮긴다
  (flip 전, R 은 +x 를 +y 쪽으로 돌림). 렌더는 출력 화소 중심을 역변환한 뒤 기존 레이어 역매핑에 넣는다.
  레이어 bounding box 는 네 꼭짓점을 정방향으로 옮긴 범위로 넓힌다. flip 은 여전히 마지막이다.
- RandomWalk: 프레임 1 부터 축마다 `[−amp/2, amp/2]` 균등 step 을 더하고 `[−amp, amp]` 로 clamp.
  난수는 자체 32-bit LCG (`state·1103515245 + 12345`, seed = `seed`), 프레임마다 x, y, angle 3 회 —
  진폭이 0 이어도 3 회 뽑으므로 수열이 플랫폼·컴파일러와 무관하다.
- Sine: `dx = ampX·sin(2πk/P)`, `dy = ampY·cos(2πk/P) − ampY`, `angle = ampDeg·sin(2πk/P)` (프레임 0 정지).
- 정답 궤적 CSV `<y4m>.camera.csv` (`frame,dx,dy,angle`) 는 flip 후 출력 좌표다:
  flipH 는 dx·angle, flipV 는 dy·angle 의 부호를 바꾼다. Camera shake 가 Off 면 쓰지 않는다.
- JSON 키 `camera` {mode, ampX, ampY, ampDeg, period, seed}. 없으면 Off. Off 이면 저장하지 않는다.
- canvas: 마우스 위치는 flip 해제 후 카메라 역변환으로 장면 좌표가 되고, 선택 상자는 카메라 정방향 + flip 으로
  그린다 (회전 카메라에서는 사각형이 사변형으로 그려진다). 이동·크기 조절은 그대로 동작한다.
- 주의: 렌더는 화소 중심 점 표본이다 (anti-aliasing 없음). 1:1 이미지 patch 는 nearest neighbour 라
  분수 카메라 d 가 정수 이동 `ceil(d − 0.5)` 로 보인다. CSV 는 d 그대로이므로 CSV 와 화면 사이에
  최대 0.5 px 양자화 차이가 있다 (52 번 테스트가 이를 계량).

### 4.6 검증

| 테스트 | 내용 |
|---|---|
| unit `affine-warp-stabilizer` | identity·정수 이동 = 복사(가장자리 복제), 10-bit ramp 반화소 = 이웃 평균, chroma siting 3 종 (이동 절반, zoom 고정점 일치), 4:2:0 frame 이동, identity 모델 → identity, 알려진 pan + jitter → raw / 평활 / 보정 값, rotzoom 모델 → angle·log scale, `warpParams(motionFromModel(m)) == m`; 평활: Gaussian = 직접 계산한 가중 평균 (σ 2.5, ±8), identity → 4 방법 모두 궤적 0, 등속 pan 내부 보정 0 (MA·Gaussian ≤ 1e-9, RTS ≤ 1e-6 px; 실측 4e-8), causal 의 k ≤ 20 결과는 이후 프레임을 바꿔도 비트 동일 (RTS 는 바뀜), pan + ±3 px 지터 잔차 RTS 0.40 < causal 0.78 px, `stabilizeByFrame` 의 빈 프레임 = identity |
| unit `generator-scene` (확장) | Sine(4, 주기 4) 프레임 1 = 무흔들림 프레임을 (4, −4) 이동, RandomWalk 분수 이동 = 레이어를 cameraAt(k) 만큼 옮긴 렌더 (Y/U/V 동일), 진폭 범위·LCG 첫 값 고정, CSV 부호 (flipH/flipV), 중심 회전 |
| 회귀 48 (확장) | Camera shake UI → scene, Generate 의 `.camera.csv`, 흔들림 프레임 = 렌더러, JSON 왕복, `camera` 없는 JSON → Off |
| 회귀 52 | 320x192 40 프레임 RandomWalk 6 px 클립 → export (RotZoom, 4 방법 각각 기본값). 실측 (2026-10-18): 피팅 실패 0. 렌더된 카메라 기준 감소율 MA 94.3%, Gaussian 90.2%, RTS 91.1% (≥ 80%), causal 61.7% (≥ 40%, RTS 보다 작음); SVT+RANSAC 로 잰 감소율 99.9 / 99.8 / 99.6 / 59.0%. 같은 방법으로 평활한 정답 대비 보정 오차 최대 0.528 / 0.486 / 0.485 / 0.545 px (≤ 1 px), raw 궤적 오차 최대 0.496 px. Gaussian 재실행 byte 동일, precomputed 모델 오버로드 = 2 pass 결과와 Y4M·CSV byte 동일 (progress 40 회, false 면 5 프레임에서 취소), 옵션 대화상자 (Method 4 개, 기본 Gaussian, 방법별 행만 표시). Translation 모델 최대 0.486 px. (2026-10-17 MA 단독 실측: 94.3%, 0.528 px — 같은 값) |

### 4.7 남은 것

- 회전 shake 를 실제 export 로 돌린 회귀는 없다 (회전 경로는 unit 의 식 검증까지). 각도 허용오차는 미정.
- crop / zoom-to-fill 은 없다 (가장자리 복제만). 실사용 DIS 라면 필요할 수 있다.

## 5. GM 창 Stabilization 절과 Apply to view (2026-10-18)

### 5.1 위치 — Knight 결정: GM 창 안

- GM 창이 이미 프레임별 RANSAC 모델(poc → poc−1)을 계산하므로 그대로 입력으로 쓴다. 다시 추정하지 않는다
- `Stabilization (DIS)` group: Method (Moving average / Gaussian 기본 / Kalman causal / Kalman RTS), 방법별 파라미터 행,
  **Apply to view**, **Export stabilized…**, 상태 줄 (프레임 범위·방법·최대 보정량·모델 없는 프레임 수), 궤적 그래프
- 궤적 = `stabilizeByFrame(modelsByFrame, first−1, last, w, h, params)` — run 종료와 파라미터 변경 때 재계산. 피팅 실패 프레임은 identity
- 그래프 `src/integration/TrajectoryPlot.{h,cpp}` (QPainter): dx·dy (px), angle (deg) 띠 3개, raw 회색 / 평활 색, 현재 프레임 점선
- Export stabilized… 는 창의 모델을 쓰는 `exportStabilizedY4m` 오버로드 (pass 1 생략, 취소 가능)

### 5.2 Apply to view — upstream 패치 0072

| 파일 | 변경 |
|---|---|
| `src/integration/StabilizationWarp.{h,cpp}` (bd) | `StabilizationTable {절대 프레임 → warp, generation}`, `applyStabilizationWarp` (raw YUV 바이트 Y/U/V, 4:2:0·4:0:0 planar 8/10 bit LE, Centre siting = export 와 동일) |
| `videoHandlerYUV.{h,cpp}` | `setStabilization(StabilizationPtr)` (convolution filter 와 같은 mutex, generation 이 같으면 무시, 바뀌면 `RECACHE_CLEAR`), `stabilizationKey()`. `loadRawYUVData`·`loadFrameForCaching` 에서 **luma 필터보다 먼저** warp (GM compensation 단계용 직전 프레임도 같은 방식으로) |
| `playlistItemWithVideo.{h,cpp}` | `setStabilization(table, why)` (지원 안 되는 형식은 이유와 함께 거절), `readFilteredRawFrame` = 안정화 + 필터, `readMeFrame` 은 안정화 없이 (`readProcessedRawFrame(k, false)`), pixel-stats key 에 `stabilizationKey()` |

- 결과: 화면, cache, 픽셀값, 블록 통계, diff 가 안정화 영상을 본다. ME 와 GM 창 추정은 원본을 읽는다 — ME overlay 는 원본 좌표 그대로라 안정화 화면에서는 어긋나 보인다 (tooltip 에 명시)
- 대상 아이템이 바뀌거나 Apply 를 끄면 이전 아이템의 warp 를 해제한다

### 5.3 검증

- 회귀 53: generator shake 클립 24 프레임 → GM 창 run (rotzoom) → 궤적 24 프레임, Gaussian 기본, 최대 보정 5.4 px →
  Apply to view 후 화면이 읽은 frame 10 raw == `readFilteredRawFrame(10)` == Export stabilized 의 frame 10 (비트 동일),
  `readMeFrame(10)` == 원본, Kalman causal 로 바꾸면 다시 그려짐, 끄면 원본으로 복귀
- 패치 0072: 3가지 검사, 0001–0072 전체 체인이 작업 트리와 79 파일 동일
