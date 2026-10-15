---
title: OpenCV 루마 필터 체인 (Convolution filter pane cascade, ME 입력, filtered Y4M export) 설계
status: 구현 완료 (패치 0066)
created: 2026-10-15
updated: 2026-10-15
author: claude-opus-5
verified: "단위 테스트 filter-chain (OpenCV 유/무 두 빌드), 회귀 47-filter-chain-and-me-input. 실제 GUI 조작과 compressed stream 위 GM stage 의 성능은 미검증"
related: 패치 0013 (3x3 convolution pane), 0065 (static OpenCV), README 설계 규칙 5, global-motion-design
---

## 무엇을 만드는가

YUV 속성 pane 의 "Convolution filter (luma)" 를 3x3 행렬 하나에서 **필터 cascade** 로 바꾼다.

- 드롭다운에서 필터를 고르고 옆의 **Add** 로 끝에 붙인다. 위에서 아래로 실행되며 앞 stage 출력이
  다음 stage 입력이다. stage 마다 체크(우회), Remove / Up / Down, 선택한 stage 의 파라미터 폼.
- **Use as ME input** 을 켜면 ME 패널과 ME compare 팝업이 필터 출력 프레임을 읽는다.
- **File > Export filtered Y4M...** 은 item 의 frame range 를 체인에 통과시켜 Y4M 으로 쓴다.
  파일을 직접 읽는 Global motion 창과 CLI 가 필터 출력을 쓰려면 이 경로를 쓴다.

## 구조

| 층 | 파일 | 역할 |
|---|---|---|
| Qt-free 코어 | `src/filter/FilterChain.{h,cpp}` | stage 카탈로그(파라미터 기술자), `FilterChain::apply`, key |
| Qt-free 코어 | `src/filter/Y4mWriter.{h,cpp}` | YUV4MPEG2 (C420jpeg / C420p10 / Cmono) |
| upstream (패치 0066) | `ConvolutionFilter` | `chain` 이 비어 있지 않으면 kernel 대신 체인. raw ↔ `LumaPlane` 변환 |
| upstream | `videoHandlerYUV` | pane UI, 표시·캐싱 경로에서 체인 적용, GM 용 이전 프레임 캐시 |
| upstream | `playlistItem::readFilteredRawFrame / readMeFrame` | export / ME 가 읽는 프레임 |
| upstream | `Mainwindow.cpp` | ME 사이트의 `readRawFrame` → `readMeFrame`, File 메뉴 |
| integration | `src/integration/FilteredY4mExport.{h,cpp}` | 저장 대화상자, 진행률·취소, 포맷 거부 사유 |

- OpenCV 코드는 전부 `#ifdef BDA_HAVE_OPENCV` 안에 있다. 없는 빌드는 카탈로그에 Custom 3x3 만 있다.
- 레거시 API (`ConvolutionFilter{enabled, kernel}`) 는 그대로다. 체인이 비면 기존 경로가 돈다 —
  회귀 12 가 그 경로를 고정한다.

## Stage 목록

| 그룹 | Stage | OpenCV |
|---|---|---|
| Legacy | Custom 3x3 | (없음, C++) |
| Smoothing | Gaussian / Box / Median / Bilateral | GaussianBlur, blur, medianBlur, bilateralFilter |
| Edges | Sobel / Scharr magnitude, Laplacian, Canny | Sobel, Scharr, magnitude, Laplacian, Canny |
| Sharpen | Unsharp mask | GaussianBlur + 가중합 |
| Morphology | Erode … Black hat (op·shape·size·iterations) | morphologyEx |
| Contrast | Histogram equalize, CLAHE | equalizeHist, createCLAHE |
| Threshold | Binary / inverted / Otsu / Triangle, Adaptive | threshold, adaptiveThreshold |
| Resample | Pyramid down-up | pyrDown, pyrUp |
| features2d | Keypoint map (ORB / FAST / GFTT) | 점을 흰 원으로 그린 마스크 |
| calib3d | GM compensation (RANSAC) | ORB + BFMatcher + estimateAffinePartial2D / estimateAffine2D / findHomography + warp |

- 샘플은 uint16 + bitDepth. 임계값·색 sigma 는 **8-bit 단위**로 입력하고 bit depth 에 맞게 스케일한다.
- 8-bit 전용 연산(Canny, ORB, equalizeHist, CLAHE, adaptive/Otsu threshold, 5 초과 median on
  10-bit)은 8-bit 로 내려 계산한다. 출력이 이진/평활화라 잃는 하위 비트는 의미가 없다.

## GM compensation

- 현재 프레임을 **이전 프레임에 정렬**(warp)한다. 출력은 정렬된 프레임 또는 |정렬 − 이전|.
- 이전 프레임은 GM stage **앞의 stage 들을 똑같이 통과**시킨 뒤 비교한다 (단위 테스트: 같은 두
  프레임 + blur 앞단 → 차이 0).
- 체인당 하나. frame 0 과 매칭 부족 시 그대로 통과(움직임을 지어내지 않는다).
- 표시 경로는 frame k−1 을 `signalRequestRawData` (DirectConnection) 로 같은 lock 안에서 가져온다.
  compressed stream 에서는 디코더 역방향 seek 이 될 수 있어 최근 4 프레임의 원본을 캐시한다
  (순차 재생은 캐시 적중). 회귀 47 이 표시 경로와 export 경로의 결과가 같음을 확인한다.

## 스레드 안전

- 캐싱 스레드가 필터를 값으로 복사한다. 체인은 `std::vector` 라 GUI 스레드의 대입과 동시에
  복사하면 data race → 전용 mutex 로 복사·대입을 감싼다.
- GM 캐시는 `requestDataMutex` 아래에서만 다룬다 (rawData 와 같은 보호).

## 제약·알려진 한계

- 루마만 필터링, 크로마는 그대로.
- ME 입력 토글은 item 변경 신호를 내고, 기존 `selectedItemChanged → restartMotionEstimation` 배선이
  ME 를 다시 돌린다 (새 connect 없음).
- Global motion 창은 파일을 읽으므로 토글의 영향을 받지 않는다. ME compare 팝업의 GME 창 항목도
  같은 이유로 원본 프레임을 보여준다.
- 필터 상태는 playlist 에 저장되지 않는다 (기존과 같음).
- Y4M export: planar 4:2:0 / 4:0:0, 8 / 10 bit, little endian 만. 그 외는 사유를 보여주고 거부.
- 미검증: 실제 GUI 조작감, 1080p 이상에서 Bilateral·GM stage 의 재생 속도, compressed stream 에서
  캐시 미스 시 비용, OpenCV 내부 스레드와 캐싱 스레드의 CPU 과점유.
