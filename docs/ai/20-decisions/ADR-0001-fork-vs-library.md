---
title: ADR-0001 YUView를 라이브러리로 쓸 것인가, fork 할 것인가
status: proposed
created: 2026-07-27
updated: 2026-07-27
author: claude-opus-5
verified: yes
---

## 배경

bd_analyzer는 비트스트림 분석기(AV1/H.264/HEVC), YUV 분석기, YUV 생성기, 이미지 필터를 제공해야 한다.
[YUView](https://github.com/IENT/YUView)가 기능적으로 가장 가까운 오픈소스다.

근거 조사:
- [yuview-architecture.md](../10-research/yuview-architecture.md)
- [yuview-build-license.md](../10-research/yuview-build-license.md)
- [yuview-feature-gap.md](../10-research/yuview-feature-gap.md)

조사에서 확인된 결정적 사실 (전부 검증됨, upstream `a72eb34`):
1. **CMake 빌드가 없다.** qmake 전용. `install(EXPORT)`도 `*Config.cmake`도 `.pc`도 없다 → `find_package` 불가
2. **`YUViewLib`는 `staticlib`이고 install 타겟이 없다** → 링크 가능한 산출물을 시스템에 설치하는 경로가 존재하지 않는다
3. **플러그인 시스템이 전혀 없다.** `QPluginLoader`/`Q_PLUGIN_METADATA`/`Q_DECLARE_INTERFACE`/`dlopen` 0건
4. **21개 public 헤더가 `ui_*.h`를 include** → shadow build dir 없이는 `playlistItem.h`조차 못 쓴다
5. **라이브러리가 `QApplication`을 소유한다** (`ui/YUViewApplication.h:37`). 앱 정책 전체가 lib 안에 있고 `YUViewApp`은 52줄
6. **GPLv3-or-later, 예외는 OpenSSL 링크뿐.** 정적 링크 배포 시 결합 저작물 전체에 GPLv3 의무 발생

## 검토한 선택지

### A. YUViewLib를 외부 라이브러리로 링크
- 장점: upstream 추종이 쉬움 (이론상)
- 단점: **사실상 불가능.** install/export가 없어 소비 경로 자체가 없다. `ui_*.h` 누출, `QSettings` 아이덴티티 사칭 필요, `MainWindow::getMainWindow()` 앰비언트 룩업, `Typedef.h`의 전역 `::vector`/`::array`/`INT_MAX` 오염
- 비용: 소비 인프라를 우리가 만들어야 하는데 그건 fork보다 비싸다
- **탈락**

### B. 플러그인으로 기능 구현 (사용자 원안)
- 장점: upstream 무수정, 재빌드/리베이스 부담 없음
- 단점: **플러그인 확장점이 0개.** 새 playlist item / 파서 / 디코더 / 통계 포맷 전부 라이브러리 내부 중앙 `if/else` 체인 수정 필요:
  - 새 아이템 → `playlistItems.cpp` 함수 4개
  - 새 비트스트림 포맷 → **약 7개 파일** (`FileSource.h` enum, `playlistItemCompressedVideo.cpp` ×2, `BitstreamAnalysisWidget.cpp`, `ParserAVFormat.cpp`, `SettingsDialog.cpp` + `.ui`)
  - 새 디코더 → 5개 파일 + `.ui`
- **탈락 (전제가 성립하지 않음).** 단 → [ADR-0003](ADR-0003-plugin-strategy.md)에서 *우리가* 플러그인 층을 만드는 방향으로 재활용

### C. 부분 vendor — `parser/` + `statistics/` + `filesource/` + `common/` (~42k LOC)만 가져와 UI는 새로 작성
- 장점: 가장 깨끗한 코드베이스. 이 4개 디렉토리는 `ui_*.h` 의존이 없음 (검증됨). `parser::*` 재사용성 6/10, `stats::StatisticsData` 7/10
- 단점: **B 기능(YUV 분석기)을 통째로 다시 만들어야 한다.** YUView가 가장 강한 부분이 바로 거기다 — 임의 비트뎁스/패킹/서브샘플링 YUV 포맷, chroma siting, 포맷 추론, 픽셀 검사, YUV 도메인 diff, split view, 캐싱 (`video/` 13.3k + `ui/views/` 5k LOC). 최소 6~12 사람-달
- **탈락 (지금은).** 나중에 CLI/headless 툴을 만들 때 이 슬라이스를 재사용할 수 있으므로 아이디어는 보존

### D. 하드 fork — upstream 전체를 서브모듈로 고정 + 우리 코드는 별도 최상위 디렉토리
- 장점:
  - B 기능을 **공짜로** 얻는다 (YUView의 최대 자산)
  - A 기능 대부분(신택스 트리, NAL/OBU, 비트레이트/HRD 플롯)도 공짜
  - C·D는 **추가적**이다 — `videoHandlerResample`/`playlistItemText`가 이미 올바른 구조 선례를 제공하고, **qmake glob 덕분에 새 파일 추가에 빌드 파일 수정이 0**
  - GPLv3 fork는 라이선스가 명시적으로 허용 (§5)
- 단점:
  - upstream 리베이스마다 중앙 `if/else` 체인에서 충돌 (수정 지점을 최소화해 완화 가능)
  - GPLv3 전염 — 폐쇄소스 제품화 불가
  - 98.5k LOC를 떠안음
- **채택**

## 결정

**D. upstream을 git submodule로 고정한 하드 fork.** 단 다음 규율을 강제한다:

1. **`third_party/yuview/upstream/`은 절대 직접 수정하지 않는다.** 서브모듈로 특정 커밋에 고정
2. upstream에 대한 모든 변경은 `third_party/yuview/patches/NNNN-*.patch` 로 관리하고 빌드 시 적용한다.
   → 리베이스 시 "우리가 뭘 바꿨나"가 patch 파일 목록으로 즉시 보인다. 목표는 **패치 20개 미만 유지**
3. **우리 신규 코드는 `src/` 아래에 두고 upstream 트리에 섞지 않는다.** `.pro`/CMake의 include 경로로만 연결
4. upstream 수정이 불가피한 지점(§등록 훅 5곳)은 **가능한 한 "레지스트리 호출 한 줄"로 축소**한다. 즉 `playlistItems.cpp`에 우리 아이템 로직을 넣는 게 아니라 `bda::registerAllItems()` 한 줄을 넣는다

**초기 라이선스 입장: GPL-3.0-or-later.** 사내 배포에서는 실질 비용이 없다(바이너리 옆에 소스 tarball). 상용 폐쇄소스 배포를 하려면 선택지 C로 되돌아가야 한다 → [ADR-0003](ADR-0003-plugin-strategy.md)의 SDK 경계가 그 대비책.

## 결과

**좋아지는 것**
- B 기능 즉시 확보. A 기능 70% 확보. 첫 동작하는 빌드까지 며칠 수준
- C·D가 추가적이라 리스크 낮음

**감수하는 것**
- GPLv3 전염. 폐쇄소스 배포 불가 (사내 배포는 무해)
- upstream 리베이스 비용. 규율 1~4로 관리
- 98.5k LOC 부채. 우리가 안 건드리는 부분(VVC/Mpeg2/Subtitles 파서 ~12k LOC)도 딸려온다

**되돌리려면**
`src/`가 upstream 타입에 직접 의존하지 않고 `src/core/`의 우리 타입을 쓰도록 유지하면, 나중에 선택지 C(부분 vendor)로 이행할 때 `src/integration/`만 재작성하면 된다. **이것이 `src/integration/` 디렉토리를 분리하는 유일한 이유다.**

## 최대 리스크 (별도 추적 필요)

**A 기능의 블록 단위 오버레이는 파서가 아니라 디코더가 만든다.** 그리고 풍부한 통계를 내는 dav1d/libde265는 **YUView가 fork한 빌드**(`libdav1d-internals`, `libde265-internals`)여야 한다. upstream 바이너리로는 안 나온다. H.264는 FFmpeg 경유 MV 4종뿐이고 파티션/intra-mode 오버레이가 **존재하지 않는다**.

→ 별도 조사 필요: (a) fork된 dav1d/libde265를 우리가 빌드·유지할 것인가, (b) H.264 블록 정보를 어디서 얻을 것인가 (FFmpeg 확장? 자체 slice_data 파서?).
`40-tasks/`에 태스크로 등록할 것.

## 재검토 트리거

- 상용 폐쇄소스 배포 요구가 생기면
- upstream 리베이스 패치가 20개를 넘어가면
- A''(slice_data 파싱)이 프로젝트의 주된 가치로 격상되면 → 선택지 C 재검토
