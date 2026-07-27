---
title: ADR-0003 플러그인 시스템을 어디까지 만들 것인가
status: proposed
created: 2026-07-27
updated: 2026-07-27
author: claude-opus-5
verified: yes
---

## 배경

원래 요청은 *"소스를 그대로 이용하고, 플러그인으로 내가 원하는 기능을 모두 구현할 수 있는지"* 였다.

**결론부터: upstream YUView에는 플러그인 확장점이 0개다.** (`QPluginLoader`/`Q_PLUGIN_METADATA`/`Q_DECLARE_INTERFACE`/`dlopen` 검색 0건, 검증됨)
가장 가까운 것은 디코더 런타임 로딩(`decoder/decoderBase.cpp:100-178`)인데, 이건 디코더마다 **하드코딩된 C 심볼 목록**을 갖는 어댑터 클래스가 라이브러리 안에 컴파일되어 있는 구조라 플러그인이 아니다.

[ADR-0001](ADR-0001-fork-vs-library.md)에서 fork를 택했으므로, 이제 질문은 바뀐다:
**"우리가 플러그인 층을 만들 가치가 있는가, 어디에?"**

## 판단 기준

플러그인화의 값어치는 **"이 확장점에 몇 개의 구현이 붙을 것인가"** 에 비례한다.

| 확장 축 | 예상 구현 수 | 변경 주기 | 작성자 | 플러그인 가치 |
|---|---|---|---|---|
| 이미지 필터 (D) | 많음 (수십) | 잦음 | 우리 + 사용자 | **높음** |
| YUV 테스트 패턴 (C) | 많음 (수십) | 잦음 | 우리 + 사용자 | **높음** |
| 품질 메트릭 (B: PSNR/SSIM/VMAF/…) | 중간 (5~15) | 보통 | 우리 | **중간** |
| 스코프 (B: 히스토그램/웨이브폼/벡터스코프) | 적음 (3~6) | 드묾 | 우리 | 낮음 |
| 비트스트림 파서 (A) | 적음 (3~5, 코덱 수만큼) | 매우 드묾 | 우리 | **낮음** |
| 디코더 백엔드 | 적음 | 드묾 | 우리 | 낮음 (upstream 방식으로 충분) |

## 결정

**필터·패턴·메트릭 세 축에만 플러그인 SDK를 만든다. 파서/디코더/스코프는 정적 등록으로 간다.**

### 1. `plugins/sdk/` — ABI 안정 C++ 인터페이스

의도적으로 **Qt를 타입에 노출하지 않는다.** 플러그인이 우리 Qt 버전에 묶이면 안 되고, 나중에 [ADR-0001](ADR-0001-fork-vs-library.md)의 선택지 C(부분 vendor)로 이행하더라도 플러그인 자산은 그대로 살아야 한다.

```
plugins/sdk/include/bda/plugin/
├── Abi.h            # 버전 매크로, C 진입점 매크로
├── Frame.h          # 순수 C++ 프레임 뷰 (평면 포인터 + stride + 포맷 서술)
├── IFilter.h        # process(const FrameView& in, FrameView& out)
├── IPattern.h       # generate(FrameView& out, frameIndex)
├── IMetric.h        # compute(const FrameView& a, const FrameView& b) -> map<string,double>
└── Registry.h       # 정적 등록 매크로 (in-tree 플러그인용)
```

**같은 인터페이스가 두 가지 방식으로 로드된다:**
- **정적** — `src/filter/kernels/`, `src/generator/patterns/` 안의 기본 구현. 앱에 링크됨. 오버헤드 0
- **동적** — `dlopen` + C 진입점 하나(`bda_plugin_entry`). 사용자가 `.so`를 드롭인

이렇게 하면 v1에서 동적 로딩을 구현하지 않아도 인터페이스는 이미 맞다. **동적 로딩은 나중에 붙인다.**

### 2. `src/integration/` — upstream과의 유일한 접점

upstream 수정 지점을 **"레지스트리 호출 한 줄"** 로 압축하는 것이 이 디렉토리의 존재 이유다.
[yuview-feature-gap.md](../10-research/yuview-feature-gap.md)가 식별한 5개 훅에 우리 로직을 직접 심지 말고:

```cpp
// third_party 패치: playlistItems.cpp 안에 딱 이 한 줄
if (auto item = bda::integration::loadPlaylistItem(tag, elem, path)) return item;
```

`src/integration/`이 담당하는 것:
- `playlistItemFilter` / `playlistItemGenerator` (upstream `playlistItemResample` / `playlistItemText` 패턴 차용)
- `videoHandlerFilter` / `videoHandlerGenerator`
- `bda::FrameView` ↔ YUView `videoHandler` raw 버퍼 어댑터
- 메뉴/컨텍스트메뉴 액션 등록
- 우리 dockable 위젯(스코프, 메트릭 플롯) 등록

### 3. 플러그인화하지 않는 것 — 정적 등록으로 충분

- **파서 (A)**: 코덱은 5년에 하나 나온다. upstream의 `if/else` 체인 패치를 그냥 받아들인다. 대신 패치를 작게 유지
- **디코더**: upstream 메커니즘 그대로
- **스코프**: `src/yuv/scopes/`에 정적 등록. `PlotModel`/`PlotViewWidget` 재사용

## 결과

**좋아지는 것**
- 사용자가 필터/패턴을 `.so` 하나로 추가 가능 (v2)
- 필터·패턴·메트릭 코드가 Qt-free → 헤드리스 CLI(`tools/cli/`)에서 그대로 재사용
- upstream 패치가 소수의 한 줄짜리로 유지됨

**감수하는 것**
- SDK 경계 유지 비용. 특히 `FrameView`가 YUView의 임의 비트뎁스/패킹 포맷을 전부 표현해야 함 — 초기엔 planar 8/10/12/16-bit만 지원하고 packed는 어댑터가 언패킹
- 정적/동적 이중 경로

**⚠️ 라이선스 주의**: 동적 플러그인이라도 GPLv3 호스트에 링크되면 GPL 논점이 발생한다. 사내 사용에서는 실질 문제가 없지만, 서드파티 폐쇄소스 플러그인을 허용할 계획이라면 별도 검토 필요.

## 재검토 트리거
- 필터/패턴 구현이 10개를 넘어가면 → 동적 로딩 구현 착수
- 외부(사내 타팀)가 플러그인을 작성하기 시작하면 → SDK 버전 정책·ABI 고정 필요
