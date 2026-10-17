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
| `src/filter/Stabilizer.{h,cpp}` | 프레임 간 모델 → 누적 궤적 → 이동평균 평활 → 보정 warp, 궤적 CSV. Qt free |
| `src/integration/StabilizedY4mExport.{h,cpp}` | File > Export Stabilized Y4M (2 pass), 비대화형 오버로드 |
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
- 평활: 반경 R (기본 15) 중심 이동평균. 양 끝은 창을 시퀀스 안으로 잘라 남은 프레임만 평균한다
  (가중치 합 1 유지, 끝 근처의 꾸준한 pan 은 약간 치우친다).
- 보정 = 평활 − raw. 프레임 k 의 내용을 raw 위치에서 평활 위치로 옮기는 변환
  `p_out = c + S R(a)(p_in − c) + d` 를 warp 용 역매핑 `p_in = c + (1/S)R(−a)(p_out − c − d)` 로 넘긴다.
- 궤적 CSV: `frame,raw_dx,raw_dy,raw_angle,raw_scale,smooth_*,corr_*` (scale 은 배율, 각도 deg).

### 4.4 Export Stabilized Y4M

- 검사·Y4mWriter·진행률·취소는 Export Filtered Y4M 과 같다 (planar 4:2:0 / 4:0:0, 8/10 bit LE).
- 옵션 대화상자: 모델 Translation / RotZoom (기본 RotZoom), 평활 반경 (기본 15).
- pass 1: 프레임 k−1, k 를 `readRawFrame` → `lumaPlaneFromRawYuv(…, kOdysseyPadFull)` →
  SVT integer ME (blockSizes = {16}, **static bypass 끔**) → `fitGlobalModel(Blk16)`.
  static bypass 를 끈 이유: `fitGlobalModel` 은 bypass 블록을 점에서 빼므로, 정지 프레임 쌍은 점이 0 개가 되어
  피팅이 실패한다. 안정화에서는 "안 움직였다" 가 실제 측정값이므로 모든 블록을 탐색시킨다
  (실측: 끄기 전에는 52 번 테스트 입력의 마지막 쌍과 안정화 출력의 정지 프레임 4 쌍이 `too few points (0)`).
- pass 2: Y/U/V 를 같은 보정으로 warp 하고 쓴다. 출력 크기 = 입력, crop 없음, 가장자리 복제.
  `<output>.trajectory.csv` 를 함께 쓴다.
- 비대화형 진입점 `exportStabilizedY4m(item, path, options, progress)` 는 대화상자 없이 같은 일을 하고
  결과(프레임 수, 실패 피팅 수, 궤적, 모델)를 돌려준다. 메뉴 항목은 이를 진행률 대화상자로 감싼다.

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
| unit `affine-warp-stabilizer` | identity·정수 이동 = 복사(가장자리 복제), 10-bit ramp 반화소 = 이웃 평균, chroma siting 3 종 (이동 절반, zoom 고정점 일치), 4:2:0 frame 이동, identity 모델 → identity, 알려진 pan + jitter → raw / 평활 / 보정 값, rotzoom 모델 → angle·log scale, `warpParams(motionFromModel(m)) == m` |
| unit `generator-scene` (확장) | Sine(4, 주기 4) 프레임 1 = 무흔들림 프레임을 (4, −4) 이동, RandomWalk 분수 이동 = 레이어를 cameraAt(k) 만큼 옮긴 렌더 (Y/U/V 동일), 진폭 범위·LCG 첫 값 고정, CSV 부호 (flipH/flipV), 중심 회전 |
| 회귀 48 (확장) | Camera shake UI → scene, Generate 의 `.camera.csv`, 흔들림 프레임 = 렌더러, JSON 왕복, `camera` 없는 JSON → Off |
| 회귀 52 | 320x192 40 프레임 RandomWalk 6 px 클립 → export (RotZoom, R 15). 실측 (2026-10-17): 피팅 실패 0, SVT+RANSAC 로 잰 프레임 간 이동 std 2.515 → 0.003 px (정수 ME 분해능 한계), 렌더된 카메라 기준 2.515 → 0.143 px (94.3% 감소), 정답 대비 보정 오차 최대 0.528 px (≤ 1 px), raw 궤적 오차 최대 0.496 px, 재실행 byte 동일, Translation 모델도 최대 0.528 px |

### 4.7 남은 것

- 회전 shake 를 실제 export 로 돌린 회귀는 없다 (회전 경로는 unit 의 식 검증까지). 각도 허용오차는 미정.
- crop / zoom-to-fill 은 없다 (가장자리 복제만). 실사용 DIS 라면 필요할 수 있다.
