---
title: 용어집
status: active
created: 2026-07-27
updated: 2026-07-27
author: claude-opus-5
verified: yes
---

## 프로젝트 용어

| 용어 | 뜻 |
|---|---|
| **bda** | bd_analyzer 네임스페이스 접두사 (`bda::`, `bda_*` 타겟) |
| **upstream** | `third_party/yuview/upstream/` 의 IENT/YUView 서브모듈. **직접 수정 금지** |
| **integration layer** | `src/integration/` — upstream 타입과 우리 타입 사이의 유일한 접점 |
| **FrameView** | `plugins/sdk`의 Qt-free 프레임 표현 (평면 포인터 + stride + 포맷) |

## YUView 내부 용어 (upstream 코드를 읽을 때 필요)

| 용어 | 뜻 | 위치 |
|---|---|---|
| `playlistItem` | 플레이리스트의 한 항목. **`QTreeWidgetItem`을 상속** — 모델이 곧 뷰 | `playlistitem/playlistItem.h:54` |
| `playlistItemContainer` | 자식 아이템을 갖는 항목 (difference, overlay, resample) | `playlistitem/playlistItemContainer.h` |
| `FrameHandler` / `videoHandler` | 단일 이미지 / 프레임 시퀀스 처리 기반 클래스 | `video/FrameHandler.h:56`, `video/videoHandler.h:47` |
| `RawFormat` | `{Invalid, YUV, RGB}` 2-way 디스패치 판별자. 하드코딩 | `video/PixelFormat.h:41-46` |
| `ParserAnnexB` | Annex-B 바이트스트림 파서 기반 (AVC/HEVC/VVC/Mpeg2). **AV1은 여기 속하지 않음** | `parser/ParserAnnexB.h:56` |
| `TreeItem` | 신택스 트리의 내부 표현. 5컬럼 name/value/coding/code/meaning | `parser/common/TreeItem.h:41` |
| `SubByteReaderLogging` | 비트 읽기를 감싸 TreeItem을 생성하는 리더 | `parser/common/SubByteReaderLogging.h:53` |
| `StatisticsData` | 블록 단위 통계 저장소. 거의 Qt-free | `statistics/StatisticsData.h:47` |
| **internals** | `libde265-internals` / `libdav1d-internals` — 블록 통계를 노출하도록 **YUView가 fork한** 디코더 빌드. upstream 바이너리에는 없다 | `decoder/decoderLibde265.cpp:1015`, `decoderDav1d.cpp:575` |
| `InputFormat` | `{Invalid, AnnexBHEVC, AnnexBAVC, AnnexBVVC, Libav}` — raw AV1 OBU 항목이 **없음** | `filesource/FileSource.h:48-62` |

## 코덱 용어

| 용어 | 뜻 |
|---|---|
| OBU | Open Bitstream Unit — AV1의 최상위 단위 |
| NAL | Network Abstraction Layer unit — AVC/HEVC/VVC의 최상위 단위 |
| AU | Access Unit — 한 프레임에 해당하는 데이터 단위 |
| CTU / CTB | Coding Tree Unit / Block (HEVC/VVC) |
| SB | Superblock (AV1) |
| slice_data | 슬라이스의 실제 블록 데이터. **YUView는 전 코덱에서 여기를 파싱하지 않는다** |
| HRD / CPB | Hypothetical Reference Decoder / Coded Picture Buffer |
| emulation prevention | 시작 코드 충돌 방지 바이트 (`0x03`) |
