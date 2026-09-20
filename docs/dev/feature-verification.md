---
title: 기능 검증 리스트
status: active
created: 2026-09-20
updated: 2026-09-20
author: claude-opus-5
verified: partial
---

# 기능 검증 리스트

bd_analyzer 동작이 이상할 때 **무엇이 깨졌는지 좁히기 위한 목록**이다.

회귀 스위트(`tests/run-regression.sh`)는 45개 테스트를 돌리지만 전부 헤드리스다. 실제 창을 띄웠을 때만
드러나는 것, 외부 서비스에 의존하는 것, 사람이 눈으로 봐야 아는 것은 여기 **수동** 항목에 있다.

## 0. 먼저 할 것 — 순서대로

```bash
./scripts/build.sh            # gcc-toolset-13 없이 빌드하면 링크는 되고 실행이 죽는다
./tests/run-regression.sh     # 45 tests + 8 unit
```

둘 중 하나라도 실패하면 **거기서 멈추고** 그 실패부터 본다. 수동 항목은 그 다음이다.

빌드가 "성공"했는데 실행이 이상하면 **먼저 클린 리빌드**를 의심한다. 낡은 `.o` 가 남아 컴파일되지 않은
파일을 숨기는 일이 실제로 있었다 (`make clean` 후 재빌드).

## 1. 자동 커버리지 맵

회귀 스위트가 지키는 것. 증상이 이 표의 기능에 해당하면 그 테스트부터 돌린다.

| 기능 영역 | 지키는 테스트 |
|---|---|
| AV1 OBU 구문 파싱 | `01-av1-obu-parsing`, `02-obu-packet-split`, `15-raw-av1-extension` |
| AV1 타일 스트림 구문 (tile_info / tile group / reference update) | `37-av1-tile-stream-obu-parsing` |
| FFmpeg 디코딩 경로 | `04-ffmpeg-av1-decode`, `06-ffmpeg-library-unload` |
| 디코더 선택 · dav1d ↔ FFmpeg 폴백 | `05-decoder-default-and-fallback` |
| 디코더 전환 안정성 (크래시 / 행) | `07-decoder-switch-slot`, `08-decoder-switch-stress` |
| dav1d 블록 통계 | `03-dav1d-block-statistics`, `10-superblock-grid-size`, `11-block-partition-tiling` |
| Block Info 조회 | `09-block-info-query` |
| raw YUV 픽셀 분석 · 포맷 추측 | `12-raw-yuv-pixel-analysis`, `20-raw-yuv-unknown-format` |
| org YUV 부착 → SSE / PSNR | `18-org-yuv-block-sse` |
| Frame Info 프레임 추종 | `17-frame-info-follows-frame` |
| 프레임 hexdump · 블록 비트 범위 | `13-frame-bitstream-dump`, `14-block-bitstream-range`, `35-frame-range-and-statistics-bounds` |
| 통계 UI 그룹핑 | `16-statistics-ui-grouping` |
| 블록 syntax pane + RD plot | `19-block-syntax-and-rd-plot` |
| Motion Estimation | `22-me-statistics-overlay`, `23-me-runner`, `24-me-panel`, `26-me-overlay-item-switch`, `27-me-block-info-on-click`, `29-me-on-compressed-stream` |
| SB BD-rate 그룹 · 수집 | `31-bdrate-groups-and-collection`, `36-delete-item-under-analysis` |
| RD curve 축 눈금 | `34-rd-curve-axis-ticks` |
| MP4 컨테이너 탭 | `33-mp4-container-tab` |
| AI 어시스턴트 이벤트 파싱 | `32-assist-event-parsing` |
| playlist 유지 · 중복 제거 | `21-playlist-saved-across-sessions`, `28-playlist-no-duplicate-files` |
| 캐시 위치 | `30-cache-in-working-directory` |
| 종료 경로 | `25-mainwindow-teardown` |

Qt 없이 도는 코어 단위 테스트 (`tests/unit/`):
`bdrate-math`, `me-plane-and-cost`, `me-svt-integer`, `me-odyssey-openloop`,
`mp4-parser`, `av1-obu-scan`, `assist-context`, `yt-link-list`.

## 2. 수동 확인 목록 — 자동 커버리지가 없는 것

회귀가 전부 통과하는데도 증상이 있으면 여기를 본다. `[ ]` 를 채워 가며 확인한다.

### 2.1 창 · 메뉴 (실제 X 디스플레이 필요)

헤드리스 테스트는 offscreen 플랫폼으로 돌아서 **레이아웃·단축키·도킹은 한 번도 검증되지 않는다.**

- [ ] 앱이 뜨고 메뉴바가 보인다 — `./bin/YUView.sh` 또는 설치본 `bd-analyzer`
- [ ] File → Open (`Ctrl+O`) 로 파일이 열리고 playlist 에 들어간다
- [ ] Recent Files 에 직전 파일이 남는다
- [ ] View → Dock Panels 의 9개 단축키가 각각 해당 패널을 토글한다
      — Playlist `Ctrl+L` / Properties `Ctrl+P` / Info `Ctrl+I` / Block Info `Ctrl+B` /
      Frame Info `Ctrl+F` / Motion Estimation `Ctrl+M` / AI Assistant `Ctrl+K` /
      YouTube Transcode `Ctrl+Y` / Playback Controls `Ctrl+D`
- [ ] View → Save / Restore View State (`Ctrl+1`~`Ctrl+8`) 가 레이아웃을 저장·복원한다
- [ ] Zoom `Ctrl+0` (1:1) / `Ctrl+9` (fit) / `Ctrl++` / `Ctrl+-`
- [ ] Split View 가 좌우 비교로 전환된다
- [ ] File → Add Difference Sequence / Add Overlay / Add Text Frame 이 항목을 만든다
- [ ] File → Delete Item 으로 **분석 중인 항목**을 지워도 죽지 않는다 (`36` 이 헤드리스로만 봄)

### 2.2 화면에 그려지는 것

- [ ] 통계 오버레이가 켜지고, 줌을 바꿔도 MV 선과 색이 보인다 (1:1 에서 선이 사라진 전례 있음)
- [ ] superblock 격자와 partition 경계가 실제 블록 경계와 맞는다
- [ ] Block Info 패널이 **클릭한 블록**의 값을 보여준다
- [ ] BD-rate popup 의 그래프·축 눈금·표가 읽히고, group 이름 편집과 anchor 변경이 반영된다
- [ ] playlist / BD-rate 표에서 긴 이름이 가운데 생략되고 tooltip 에 전체 경로가 나온다

### 2.3 외부 의존 — 네트워크·외부 바이너리

자동 테스트가 건드리지 않는다. 실패해도 회귀는 통과한다.

- [ ] AI Assistant: CLI 로그인 상태에서 질문이 오가고, **"What was sent"** 상자가 실제 전송 내용을 보여준다
- [ ] AI Assistant: 세 모드(readonly / edit / full)가 패널에 표시되고 전환된다
- [ ] YouTube Transcode: `links.txt` 목록 편집·저장, 인코더 목록이 `ffmpeg -encoders` 결과와 맞는다
- [ ] YouTube Transcode: 다운로드 전용 옵션이 ffmpeg 을 거치지 않고 목적지에 바로 저장한다
- [ ] `yt-dlp` / `ffmpeg` 부재 시 **이유를 붙여 비활성화**되고 조용히 죽지 않는다

### 2.4 배포 형태

- [ ] `./scripts/package.sh all --refresh` 가 rpm + tar.gz 를 만든다
      (**`--refresh` 없이는 커밋된 `bin/` 번들을 그대로 쓴다** — 최신 코드가 안 들어간다)
- [ ] RPM 안의 `YUView` 가 `build/YUViewApp/YUView` 와 md5 동일한지 확인
- [ ] 설치본이 Rocky/RHEL 8 에서 실행된다 (번들 Qt 가 `GLIBC_2.28` 요구)
- [ ] `check-deps.sh` 가 빠진 시스템 라이브러리를 보고한다

### 2.5 헤드리스 CLI 도구

회귀에 포함되지 않는다. 빌드는 `./tests/tools/build-tool.sh <name>`, 실행은 `build/YUViewApp` 에서.

- [ ] `sb-bdrate-export` — org y4m + anchor 2점↑ + test 2점↑ 로 CSV 두 개가 나온다.
      검산: `raw.csv` 행 수 = SB 수 × 프레임 수 × 그룹 수 × 점 수, 가장자리 SB 의 `samples` 가
      부분 블록 크기와 맞아야 한다
- [ ] `dump-obu-headers` — VQ Analyzer 의 `-dump_headers_filter all` 출력과 구문 요소가 일치한다

## 3. 증상별 진입점

| 증상 | 먼저 볼 곳 |
|---|---|
| 아무 데서나 SIGSEGV | **툴체인 혼용 의심.** `readelf -p .comment` 로 `.o` 들의 GCC 버전이 섞였는지 본다. `scl enable gcc-toolset-13` 밖에서 `make` 를 돌리면 링크는 되고 실행이 죽는다 |
| 빌드는 됐는데 새 코드가 동작 안 함 | 낡은 `.o`. `make clean` 후 재빌드. `.pro` 의 소스 glob 은 sub-project Makefile 재생성 때만 다시 평가된다 |
| 블록 통계가 조용히 이상함 | dav1d ABI. `libdav1d-internals.so` 와 YUViewLib 이 `Av1Block` 레이아웃을 공유한다. 둘 중 하나만 다시 빌드하면 어긋난다 (`scripts/setup-dav1d.sh`) |
| 비트스트림 패널 값이 틀림 | `dump-obu-headers` 로 덤프해 VQ Analyzer 와 diff. 어긋나기 시작하는 **첫 필드**가 원인 지점이다 |
| BD-rate 가 안 나옴 / 거절됨 | 거절 사유를 그대로 읽는다 — `no PSNR overlap`, `too few points`, `lossless`, dav1d analyzer 디코더가 아닌 스트림, 격자가 다른 스트림 |
| 시퀀스 sweep 중 `std::bad_alloc` | transform size 테이블 인덱싱. `35-frame-range-and-statistics-bounds` 가 상한을 지킨다 |
| AI 패널에 예상 밖 도구가 보임 | MCP 유입. `--strict-mcp-config` 와 세션별 `unexpectedTools()` 가드를 확인한다. 샌드박스는 이걸 막지 못한다 (네트워크 경로) |
| RPM 에 최신 수정이 없음 | `package.sh` 를 `--refresh` 없이 돌렸다. `bin/` 번들이 페이로드다 |

## 유지 규칙

- 회귀 테스트를 추가하면 **1장 표에 행을 추가**하고, 그 기능이 2장 수동 항목에 있었다면 거기서 지운다.
- 수동 항목이 자동화되면 옮긴다. 목록이 길어지는 것보다 **어디가 자동이고 어디가 아닌지 흐려지는 것**이
  더 나쁘다.
- 3장은 **실제로 겪은 증상만** 올린다. 추측한 증상은 넣지 않는다.
