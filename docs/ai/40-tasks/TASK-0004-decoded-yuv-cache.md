---
title: TASK-0004 디코딩된 YUV 를 .YUViewBD 에 저장하고 playlist 아이템으로 열기
status: todo
created: 2026-07-31
updated: 2026-07-31
author: claude-opus-5
verified: partial
upstream: IENT/YUView @ a72eb3488097313511e60ed70db4af6071cbe9fe
---

## 요구

비트스트림을 디코딩할 때 나온 YUV 를 `.YUViewBD` 폴더에 저장하고,
item 창(playlist)에서 YUV viewer 로 볼 수 있게 한다.

## 결론 — 가능하다. 침습도는 중간

훅 지점이 깨끗하고 필요한 부품이 **전부 이미 있다.** 새로 만들 것은 writer 하나뿐이다.

| 필요한 것 | 상태 |
|---|---|
| 디코딩된 프레임을 가로챌 지점 | ✅ `playlistItemCompressedVideo.cpp:783-801` 한 곳 |
| 프레임 크기 / 픽셀 포맷 | ✅ `decoderBase.h:115-118` `getRawFormat/getPixelFormatYUV/getFrameSize` |
| 프레임 바이트 수 | ✅ `PixelFormatYUV::bytesPerFrame(Size)` (`PixelFormatYUV.h:215`) |
| **포맷을 명시한** raw YUV 아이템 생성 | ✅ `playlistItemRawFile(path, frameSize, sourcePixelFormat, fmt)` — 파일명 추론에 의존할 필요 없음 |
| playlist 저장/복원 | ✅ 태그 `playlistItemRawFile` 이 이미 처리됨 (`playlistItems.cpp:224`) |
| YUV writer | ❌ **없음.** YUView 에 쓰기 경로가 전무 → 신규 (단순: raw 바이트 write) |

⚠️ 아래 설계는 **코드 조사 기반이며 구현·실측하지 않았다.** 프레임 순서/동시성 항목이 특히 위험하다.

## 훅 지점

`playlistItemCompressedVideo::loadRawData()` (`playlistItemCompressedVideo.cpp:783-801`):

```cpp
if (dec->decodeNextFrame())
{
  if (caching) this->currentFrameIdx[1]++;
  else         this->currentFrameIdx[0]++;
                                          // <-- (A) 여기에 훅
  rightFrame = ... == frameIdx;
  if (rightFrame)
  {
    this->video->rawData            = dec->getRawFrameData();
                                          // <-- (B) 또는 여기
    this->video->rawData_frameIndex = frameIdx;
  }
}
```

**(A) 를 권장한다.** 이유: 시킹 시 YUView 는 키프레임부터 다시 디코딩하면서
**중간 프레임을 전부 버린다** (`getRawFrameData()` 가 `rightFrame` 일 때만 호출됨).
(A) 에 훅을 걸면 이미 디코딩된 프레임을 공짜로 캐시에 채운다 — 랜덤 액세스가 많을수록 이득이 크다.

**대가**: 프레임마다 `getRawFrameData()` 호출 비용이 생긴다. 이건 단순 복사가 아니다 —
dav1d 경로에서는 `cacheStatistics()` 까지 돈다 (`decoderDav1d.cpp` 의 `getRawFrameData` → `cacheStatistics`).
통계가 꺼져 있으면 가벼우므로, **(A) + 통계 비활성 시에만 전 프레임 캐시**가 합리적인 타협이다.

## 위험 — 프레임이 순서대로 오지 않는다

이게 이 태스크의 핵심 난점이다.

- `loadRawData` 는 `frameIdx < curFrameIdx || frameIdx > curFrameIdx + FORWARD_SEEK_THRESHOLD`
  일 때 **시킹**한다 (`playlistItemCompressedVideo.cpp:632-636`) → 사용자가 타임라인을 클릭하면 순서가 깨진다
- upstream 스스로 남긴 TODO: *"Could we somehow make shure that caching is always performed in
  display order?"* (`playlistItemCompressedVideo.h:162`) → **캐싱 순서도 보장되지 않는다**

**따라서 순차 append 는 틀렸다.** 필요한 것:

1. **랜덤 액세스 write** — `QFile::seek(frameIdx * bytesPerFrame)` 후 write.
   Linux ext4/xfs 는 sparse 파일을 지원하므로 구멍은 문제 없다
2. **완료 인덱스** — 안 쓴 프레임은 0 으로 읽혀 초록/검은 화면이 된다.
   어느 프레임이 유효한지 기록하는 사이드카가 **필수**다

인덱스 방식 후보:

| 방식 | 평가 |
|---|---|
| `.YUViewBD/<name>.idx` 비트맵 사이드카 | 단순하고 정확. **1순위** |
| 전 프레임 완료 후에만 아이템 노출 | 구현 최소. 대신 긴 시퀀스에서 사용성 나쁨 |
| 순차 디코딩(캐싱)만 기록 | 구멍이 안 생기지만 상호작용 중 디코딩분을 버린다 |

## 위험 — 디코더가 둘이다

`loadingDecoder`(전경, 랜덤) + `cachingDecoder`(배경, 선형) 가 **동시에** 프레임을 만든다
(`playlistItemCompressedVideo.h:126-127`). 같은 파일에 두 스레드가 쓴다.

- `cachingMutex` (`:163`) 는 존재하지만 **캐싱 경로만** 보호한다 → writer 전용 뮤텍스 필요
- `cachingThreadLimit() == 1` (`:116`) 이라 캐싱 쪽 쓰기는 1스레드로 제한됨 — 도움이 된다
- 같은 프레임을 양쪽이 쓸 수 있다 → 동일 내용이므로 무해하지만, 인덱스 갱신은 원자적이어야 함

## 그 외 확인된 제약

- **출력이 항상 YUV 는 아니다.** `getRawFormat()` 이 RGB 일 수 있다 (`decoderBase.h:115`).
  RGB 는 별도 처리하거나 캐싱 제외
- **디스크 사용량이 크다.** 실측된 1920x1080 yuv420p8 = 3,110,400 B/프레임
  → 100프레임 ≈ **297 MiB**, 500프레임 ≈ **1.45 GiB**. 10bit 면 2배.
  → 상한/정리 정책 없이 내보내면 안 된다
- **`PlaylistTreeWidget::appendNewItem` 이 private** (`PlaylistTreeWidget.h:156`, `private:` 는 `:135`)
  → 아이템을 프로그램적으로 추가하려면 public 슬롯 추가 또는 시그널 경유. upstream 수정 1곳
- 포맷 문자열은 `playlistItemRawFile` 의 `sourcePixelFormat` 인자로 넘긴다.
  파일명 추론(`PixelFormatYUVGuess.cpp`)에 의존하지 않아도 되지만,
  **파일명에도 `_WxH_fmt` 를 넣어두면** 사용자가 직접 열 때 편하다

## 위치 — 실행 파일 아래 `.YUViewBD` (확정)

```
QCoreApplication::applicationDirPath() + "/.YUViewBD/"
```

로컬 개발 빌드(`build/YUViewApp/`)에서는 문제 없다. **단 배포 형태에서 깨진다** —
ADR-0002 가 AppImage 를 1차 산출물로 정했기 때문에 이건 실제 충돌이다:

| 배포 형태 | `applicationDirPath()` | 결과 |
|---|---|---|
| 로컬 빌드 | `build/YUViewApp/` | ✅ 쓰기 가능 |
| **AppImage** | `/tmp/.mount_XXXXXX/usr/bin/` | ❌ **읽기전용 마운트.** 게다가 실행마다 경로가 바뀌어 캐시가 매번 고아가 된다 |
| `.rpm` 설치 | `/usr/bin/` | ❌ 일반 사용자 쓰기 불가 / 다중 사용자 충돌 |

→ **구현 방침: `applicationDirPath()` 를 1순위로 쓰되, 쓰기 가능 여부를 검사해
실패하면 폴백한다** (`~/.cache/YUViewBD/`). 폴백 경로는 설정으로 덮어쓸 수 있게 한다.
이렇게 하면 요구사항(실행파일 아래)을 그대로 만족하면서 AppImage 에서도 죽지 않는다.

## 창 닫을 때 삭제 확인 (확정)

훅 지점: `MainWindow::closeEvent()` (`ui/Mainwindow.cpp:506`).
**이미 똑같은 패턴이 있다** — 저장 안 된 playlist 확인 대화상자(`:512-527`)가
`QMessageBox::question` + `event->ignore()` 로 취소까지 처리한다. 그 아래에 붙이면 된다.

```
"디코딩 캐시 <N>개 파일 (<크기>) 를 삭제할까요?"   [Yes] [No]
```

설계 시 주의:
- **크기를 대화상자에 보여줄 것.** 아래 계산대로 GiB 단위가 흔하다
- `AskToSaveOnExit` 처럼 **설정으로 끌 수 있게** (`AskToDeleteDecodedCacheOnExit`, 기본 true)
- 캐싱 스레드가 아직 쓰는 중일 수 있다 → 삭제 전에 `VideoCache` 정지/조인 필요
- 비정상 종료 시 파일이 남는다 → **다음 실행 시 고아 정리** 경로도 필요
  (실행마다 경로가 바뀌는 AppImage 에서는 특히)

## 통계 사이드카 — 같은 폴더, 같은 수명 (Block info overlay 대비)

향후 YUV viewer 창에 block info 를 오버레이할 계획을 반영한다.
**여기서 큰 이득이 하나 있다: YUView 에 통계 파일 리더가 이미 있다.**

- `statistics/StatisticsFileCSV.{h,cpp}` — 리더 존재
- `playlistitem/playlistItemStatisticsFile.{h,cpp}` — 통계 파일을 playlist 아이템으로
- `stats::paintStatisticsData()` — 렌더링
- overlay 아이템으로 YUV 아이템 위에 겹치기

→ **우리 통계를 YUView 자체 CSV 포맷으로 쓰면, 읽기·아이템화·렌더링이 전부 공짜다.**
새로 만들 것은 writer 뿐이다 (**writer 는 없다** — `statistics/` 전체에 쓰기 경로 0건).

### 써야 할 CSV 포맷 (리더에서 역산, `StatisticsFileCSV.cpp:346-420,262-330`)

헤더 (`%` 로 시작):
```
%;type;<typeID>;<typeName>;<map|range|vector|line>
%;mapColor;<id>;<r>;<g>;<b>
```
데이터:
```
<poc>;<posX>;<posY>;<width>;<height>;<typeID>;<val0>[;<val1>[;<val2>;<val3>]]
```
- 값 1개 → `addBlockValue`  /  2개 → `addBlockVector`  /  4개 → affine

이건 `decoderDav1d::cacheStatistics()` 가 채우는 `StatsItemValue` / `StatsItemVector` 구조와
그대로 대응한다 → 변환이 단순하다. → [dav1d-block-statistics.md](../10-research/dav1d-block-statistics.md)

### ⚠️ 통계 CSV 용량이 YUV 와 맞먹는다

실측된 1080p 프레임 1장의 블록 통계는 **값 141,205개 + MV 11,210개**다.
CSV 한 줄이 `12;1728;960;8;8;3;1` 수준(≈20 B)이므로:

```
1080p 1프레임  ≈ 2.8 MB   (YUV 프레임은 2.97 MiB — 거의 같다)
100 프레임     ≈ 280 MB
500 프레임     ≈ 1.4 GB
```

→ **YUV + 통계를 합치면 500프레임 1080p 에서 ~3 GB.** 상한/정리 정책이 선택이 아니라 필수다.
→ 텍스트 CSV 대신 바이너리 포맷이 훨씬 작지만, 그러면 기존 리더를 못 쓴다.
   **1차는 CSV(공짜 재사용), 용량이 문제되면 바이너리로 전환**을 권장

### 부분 기록 문제 (YUV 와 동일)

`StatisticsFileCSV::readFrameAndTypePositionsFromFile()` 는 파일 전체를 스캔해
POC→type→파일위치 맵을 만든다. **쓰는 중인 파일을 읽으면 맵이 깨진다.**
→ 임시 이름으로 쓰고 완료 시 rename, 또는 아이템 노출을 완료 후로 미룰 것.
YUV 쪽 "완료 인덱스" 문제와 같은 성질이므로 **정책을 하나로 통일**하는 게 좋다.

## 폴더 레이아웃 (제안)

아이템 하나가 만드는 산출물을 묶어서 한꺼번에 지울 수 있게 한다.

```
<exeDir>/.YUViewBD/
  <hash>-<basename>/            # 소스 경로 해시로 충돌 방지
    decoded_1920x1080_yuv420p.yuv
    decoded.idx                 # 어느 프레임이 유효한지 (YUV)
    stats.csv                   # 블록 통계 (YUView CSV 포맷)
    meta.json                   # 소스 경로/크기/mtime, 포맷, 디코더, 생성 시각
```

`meta.json` 의 소스 mtime/크기로 **스트림이 바뀌면 캐시를 무효화**한다.
아이템 단위 디렉토리라서 삭제·정리·용량 집계가 전부 단순해진다.

## 작업 순서 (제안)

### 1단계 — 캐시 기반
- [ ] `BDCachePaths` — `applicationDirPath()/.YUViewBD` 쓰기 가능 검사 + 폴백 + 아이템 디렉토리/해시
- [ ] `meta.json` 읽기/쓰기 + 소스 mtime·크기 기반 무효화
- [ ] 고아 디렉토리 정리 (시작 시)

### 2단계 — YUV writer
- [ ] `YUVCacheWriter` — sparse write(`seek(frameIdx * bytesPerFrame)`) / 완료 인덱스 / 뮤텍스
- [ ] `playlistItemCompressedVideo.cpp:785` 훅 (A) + 설정 스위치
- [ ] RGB 출력(`getRawFormat()`) 분기 또는 제외

### 3단계 — playlist 노출
- [ ] `PlaylistTreeWidget` 에 아이템 추가용 public 진입점 (또는 시그널) — 현재 `appendNewItem` private
- [ ] 컨텍스트 메뉴 / 액션: "Open decoded YUV" → `playlistItemRawFile(path, size, fmt)`

### 4단계 — 종료 시 삭제
- [ ] `MainWindow::closeEvent` (`ui/Mainwindow.cpp:506`) 에 삭제 확인 대화상자 + 크기 표시
- [ ] `AskToDeleteDecodedCacheOnExit` 설정 (기본 true)
- [ ] 삭제 전 캐싱 스레드 정지/조인

### 5단계 — 통계 사이드카 (block info overlay 대비)
- [ ] `StatisticsCSVWriter` 신규 — `StatisticsData` → YUView CSV 포맷
- [ ] 통계도 같은 아이템 디렉토리에 기록, 같은 삭제 정책 적용
- [ ] `playlistItemStatisticsFile` 로 되읽어 overlay 로 YUV 위에 겹치기 확인
- [ ] 용량 측정 후 바이너리 포맷 전환 여부 판단

### 공통
- [ ] 디스크 상한 + 정리 정책 (LRU? 설정?) — **YUV+통계 합계가 GiB 단위**
- [ ] 검증: 시킹을 섞어 랜덤 액세스한 뒤 캐시 파일과 참조 YUV 비교.
      전 프레임 md5 일치 확인 (`ffmpeg -f rawvideo` 로 참조 생성)
- [ ] 검증: 통계 CSV 를 되읽어 오버레이한 결과가 디코더 직산출과 일치하는지

## 관련

- upstream 은 프레임 캐시를 **메모리에만** 둔다 (`video/caching/VideoCache.cpp`). 디스크 캐시 선례 없음
- 기능 C(YUV 생성기)도 YUV 쓰기 경로가 필요하다 → writer 를 공용으로 설계하면 재사용 가능.
  → [yuview-feature-gap.md](../10-research/yuview-feature-gap.md) C절
