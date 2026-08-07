# 회귀 테스트

```bash
./scripts/setup-ffmpeg.sh     # 한 번만 (FFmpeg + dav1d)
./scripts/build.sh            # upstream 패치 적용 + 빌드
./tests/run-regression.sh
```

`TIMEOUT=45 ./tests/run-regression.sh` 처럼 환경변수로 조정할 수 있다
(`TIMEOUT` / `OUT` / `DATA` / `QT_DIR` / `QT_VERSION` / `TOOLSET`).

## 이 테스트가 무엇인가

**전부 실제로 발생한 버그에서 나왔다.** 각 파일 상단 주석에 어떤 실패를 막는지 적혀 있고,
근거와 실측값은 [TASK-0003](../docs/ai/40-tasks/TASK-0003-av1-parsing-dav1d.md)에 있다.

GUI 없이(`QT_QPA_PLATFORM=offscreen`) upstream YUView 의 **프로덕션 클래스를 직접 구동**한다.
`libYUViewLib.a` 를 링크하므로 `./scripts/build.sh` 가 먼저 돌아야 한다.
실행은 `build/YUViewApp` 에서 한다 — YUView 가 ffmpeg 를 `applicationDirPath()/ffmpeg/`,
디코더를 `.../decoder/` 에서 dlopen 하기 때문이다.

| 테스트 | 막는 것 |
|---|---|
| `01-av1-obu-parsing` | `ParserAVFormat` → `ParserAV1OBU` 경로. FFmpeg 라이브러리 로드/심볼 바인딩 포함 |
| `02-obu-packet-split` | `getNextUnit()` 이 유효한 OBU 를 빈 배열로 반환하던 버그 (포맷 추측 실패) |
| `03-dav1d-block-statistics` | dav1d analyzer fork 의 블록 통계 산출 (기능 A'). 전 프레임 디코딩 |
| `04-ffmpeg-av1-decode` | FFmpeg 디코더의 AV1 경로. `--enable-libdav1d` 없는 빌드를 잡는다 |
| `05-decoder-default-and-fallback` | AV1 기본 디코더 선택 + libdav1d 부재 시 FFmpeg 폴백. 아이템이 죽지 않아야 함 |
| `06-ffmpeg-library-unload` | 디코더 하나를 파괴해도 ffmpeg 라이브러리가 언로드되지 않아야 함 |
| `07-decoder-switch-slot` | 실제 콤보박스로 디코더 전환 (크래시/행) |
| `08-decoder-switch-stress` | 위를 40회 반복 + 워커 동시 디코딩 |
| `09-block-info-query` | 클릭 지점의 블록 조회. 렌더링이 꺼진 기본 상태에서도 통계가 수집되어야 함 |
| `10-superblock-grid-size` | sequence header 의 superblock 크기를 읽어 기본 격자로 쓰는 경로. 사용자 선택이 우선 |
| `11-block-partition-tiling` | coding block 이 화면을 정확히 1번씩 덮어야 함 (VERT_B 좌표 오류 / 중복 emit) |
| `12-raw-yuv-pixel-analysis` | 64x64 루마 통계·디스크 캐시 재사용·히스토그램·3x3 convolution (루마 전용) |

## 테스트 데이터

`tests/data/test.ivf` (176x144, 25fps, 1초 AV1)는 없으면 `ffmpeg` 로 자동 생성된다
(`libaom-av1` 필요). 리포지토리에 바이너리를 넣지 않기 위한 선택이다.
`ffmpeg` 가 없으면 스위트 전체가 SKIP 된다.

## 테스트가 실제로 버그를 잡는지 확인함

패치를 되돌리고(`git -C third_party/yuview/upstream checkout -- .`) 재빌드해 실측:

```
패치 없음 : PASS 3  FAIL 5   (02 exit 1 / 05·06·07·08 SIGSEGV)
패치 적용 : PASS 8  FAIL 0
libdav1d 없음 + 패치 적용 : PASS 7  SKIP 1   (FFmpeg 으로 폴백)
```

`08` 은 패치가 추가한 심볼을 참조하지 않도록 작성했다 — 그래야 pristine upstream 에서도
컴파일되어 크래시로 실패한다(컴파일 실패로 끝나면 회귀 탐지가 아니다).

## 커버되지 않는 것

- **patch 0002 (dav1d intra-pred 범위 방어)** — 회귀 테스트가 없다. 이 크래시는 GUI 에서
  통계 렌더링을 켰을 때만 재현되고, 헤드리스로 두 스트림 125프레임을 전부 디코딩(통계 활성)해도
  재현되지 않았다. 근본 원인(`angleDelta` 가 왜 범위를 벗어나는가)도 미규명이다
- **patch 0006 (frame rate 30fps)** — 값 확인은 GUI 스크린샷으로만 했다
- **GUI 상호작용** — 전환은 프로덕션 슬롯을 인프로세스로 구동해 검증했다.
  xdotool 로 실제 콤보박스를 클릭하는 자동화는 실패했다
- HEVC / VVC / H.264 경로 — 이 스위트는 AV1 브링업 범위만 다룬다
