# H.264 stream analyzer — 설계와 검증

> 2026-10-20. AV1 analyzer (dav1d analyzer 라이브러리 + 범용 UI) 와 같은 방식으로 H.264 를 분석한다.
> 데이터 소스 JM 19.0 (`third_party/jm`, tag `JM-19.0`). 범위: 블록 분석, Frame 탭·트리 seek, MB 비트(RD plot·BD-rate). Progressive 만 (PAFF/MBAFF 는 다음 단계). Find diff 는 범위 밖.

## Abbreviations

| 약어 | 원문 | 설명 |
|---|---|---|
| JM | Joint Model | ITU-T/ISO H.264 레퍼런스 소프트웨어. 이 문서의 기준 디코더 |
| MB | Macroblock | H.264 의 16x16 부호화 단위 |
| CAVLC / CABAC | Context-Adaptive Variable-Length / Binary Arithmetic Coding | H.264 의 두 엔트로피 부호화 |
| POC | Picture Order Count | 출력(표시) 순서 번호 |
| IDR | Instantaneous Decoding Refresh | 디코딩을 새로 시작하는 그림 |
| NAL | Network Abstraction Layer (unit) | Annex-B / MP4 안의 단위 |
| EPB | Emulation Prevention Byte | NAL 안에 끼워 넣는 0x03 |
| PAFF / MBAFF | Picture / Macroblock Adaptive Frame-Field | 인터레이스 부호화 방식 |
| BD-rate | Bjøntegaard Delta rate | 같은 화질에서의 비트율 차이 (%) |

## 1. 구조

| 계층 | 파일 | 역할 |
|---|---|---|
| 라이브러리 | `third_party/jm-analyzer/` (`bda_jm.h`, JM 패치), `scripts/setup-jm-analyzer.sh` → `decoder/libbda-jm-analyzer.so` | JM ldecod 를 handle 별 coroutine 으로 감싼 C API. 출력 순서 picture + MB 배열 (type, 분할, intra 모드, QP, cbp, 8x8, skip, slice, **비트 범위**) + 4x4 배열 (ref_idx, ref POC, MV 1/4 화소) + slice 정보 |
| 디코더 | `src/integration/DecoderJm.*` | decoderBase 구현. 엔진 **"JM (analysis)"** (H.264 에만 나타남, 기본은 FFmpeg). 통계 type 0–13, `blockBitsTypeId` = MB bits, `maxBlockSize` 16 |
| 프레임 요약 | `src/integration/FrameAvcSummary.*` | 파서 모델에서 picture (slice NAL, `first_mb_in_slice == 0` 또는 POC 변화로 시작) → POC 순 표시 번호. Frame 탭, 트리 더블클릭 seek |
| upstream | 패치 0077 | FFmpeg MV overlay: avutil 58–60, 1/4 화소, 코덱 MV 부호 |
| | 패치 0078 | JM 엔진, decoderBase hook 3개 (`setNextDataFilePosition`, `setNextOutputFrameIndex`, `codingIndexOfFrame`), `blockBitsTypeId` (SB 비트 조회 일반화), Annex-B NAL 파일 위치 전달, 컨테이너 NAL 의 패킷 내 위치, hexdump 범위 재기준·패킷 선택 |
| | 패치 0079 | Frame 탭 H.264 행, 트리 seek (NAL 노드) |

### 통계 type (JM)

| ID | 이름 | 단위 |
|---|---|---|
| 0 | MB type | MB, 정규화 kind (I_4x4 … B_Skip) |
| 1 | Pred mode | 예측 블록: INTRA / L0 / L1 / BI / DIRECT / SKIP |
| 2, 3 | Intra luma (방향 화살표) / chroma | 4x4·8x8·16x16 / MB |
| 4, 5 | Ref idx L0 / L1 | 예측 블록 (direct 는 4x4) |
| 6, 7 | Motion vector L0 / L1 | 1/4 화소, 참조 위치 − 블록 위치 |
| 8–12 | Transform 8x8, QP, CBP, Slice, Skip | MB |
| 13 | MB bits | MB (RD plot·BD-rate 의 블록 비트) |

### 비트 범위의 좌표

- 라이브러리는 push 된 바이트 기준으로 센다. item 이 NAL 마다 "이 unit 이 어디에 있는가" 를 알려준다: Annex-B 는 파일 위치 (`getNextNALUnit` 의 위치), 컨테이너는 패킷 안 위치 (`getPosOfLastUnitInPacket`, H.264 에서는 원래 채워지지 않던 값을 0078 에서 채움).
- 범위는 파일 비트 (`origin` "source file") 또는 패킷 비트로 저장하고, hexdump 가 파일 비트를 보여주는 프레임 시작 기준으로 옮긴다.
- CAVLC 는 정확, CABAC 는 산술 복호기 위치라 MB 사이 경계가 최대 약 9 bit 근사 (AV1 의 msac 위치와 같은 성격).

### 표시 순서와 코딩 순서

- 컨테이너 모델의 "YUView frame index" 는 패킷(디코드) 순서 번호다. B-frame 이 있으면 표시 순서와 다르므로 H.264 에서는 쓰지 않는다 (Frame 탭·seek 은 POC 순위, hexdump 는 디코더가 아는 `codingIndexOfFrame`).

## 2. 검증

| 항목 | 방법 | 결과 |
|---|---|---|
| 라이브러리 (Progy) | 스트림 10종 출력 MD5 = JM ldecod = FFmpeg; MB 필드 = JM TRACE (6종, mb_type·qp·cbp 등 각 34,155 건, 불일치 0); CAVLC 비트 길이 = trace (11,880/11,880); 다중 handle·스레드·오류 경로; valgrind 0 | PASS |
| FFmpeg MV (회귀 58) | 정지 영상 2 px pan, 참조 1장: P 블록 다수가 (+2, 0) | 54/71, 60/68 |
| Frame 탭·seek (회귀 59) | ffprobe (MP4 패킷 순서·pts) 기준 coding_order·slice type, NAL 행 → 표시 프레임 | Annex-B·MP4 각 25/25 |
| JM 엔진 (회귀 60) | 디코드 = FFmpeg (CABAC B Annex-B/MP4, CAVLC, 4-slice 각 25/25); pan MV (+2,0) 324/340; IDR intra 99/99; CAVLC 범위 연속; 첫 MB 가 slice 헤더 바로 뒤 (파일·패킷); MP4 B 프레임 hexdump = coding 순서 패킷; MB 비트 합 = 프레임 바이트의 98.8% | PASS |

## 3. 한계

- PAFF / MBAFF / 4:4:4 separate colour plane: picture 는 디코드되지만 MB 데이터 없음 (`has_mb_data = 0`).
- SP/SI, IPCM, P_8x8ref0, weighted prediction 은 trace 대조를 하지 않았다.
- JM 은 단일 스레드 레퍼런스라 큰 해상도에서 느리다 (분석용). 재생은 FFmpeg 엔진.
- Settings 의 기본 디코더에는 H.264 항목이 없다 (속성 창 콤보에서 고른다).
- MP4 NAL 길이는 upstream 처럼 4바이트로 가정한다 (`nal_size_length_minus1` 무시, 기존 한계).
