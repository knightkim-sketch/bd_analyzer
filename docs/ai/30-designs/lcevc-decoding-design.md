# LCEVC 스트림 디코딩 — 설계와 검증

> 2026-10-18. 기준: MPEG LTM `third_party/ltm` @ `452a9869`, LCEVCdec 4.3.0. 계획 P0–P7.

## Abbreviations

| 약어 | 원문 | 설명 |
|---|---|---|
| LCEVC | Low Complexity Enhancement Video Coding (MPEG-5 Part 2) | base 코덱 위에 해상도 향상 잔차 층을 얹는 표준 |
| LTM | LCEVC Test Model | MPEG 레퍼런스 구현. 이 문서의 정답 기준 |
| LoQ | Level of Quality | LTM 명칭. LoQ-1 = sub-layer 1(중간 해상도), LoQ-0 = sub-layer 2(최종 해상도) |
| DD / DDS | Directional Decomposition (2x2) / (4x4, Squared) | LCEVC 잔차 변환. 층 4개 / 16개 |
| IDR | Instantaneous Decoder Refresh | LCEVC nal_unit_type 29. 디코딩을 시작할 수 있는 그림 |
| POC | Picture Order Count | 출력(표시) 순서 번호 |
| NAL | Network Abstraction Layer (unit) | Annex-B 바이트열의 단위 |
| RBSP | Raw Byte Sequence Payload | emulation prevention(00 00 03)을 뺀 NAL 본문 |
| OBU | Open Bitstream Unit | AV1 비트스트림 단위 |
| T.35 | ITU-T Recommendation T.35 | 국가·제공자 코드로 구분하는 사용자 데이터. LCEVC 는 `B4 00 50 00` |
| TU | Temporal Unit | AV1 에서 한 시각의 OBU 묶음 (IVF 프레임 하나) |
| MD5 | Message Digest 5 | 프레임 바이트 일치 판정에 쓰는 해시 |

## 1. Knight 결정

- 디코더: **LTM(분석) + LCEVCdec(재생) 둘 다**. LCEVCdec 은 배포 번들에 포함한다 (BSD-3-Clause-Clear, 특허 라이선스 없음).
- 입력: Annex-B LCEVC NAL(`.lvc`), AV1 metadata OBU(T.35), 분리 파일(P6). H.264/HEVC SEI(T.35)는 범위 밖.
- FFmpeg 은 base 디코딩에만 쓴다 (7.1.2 에 liblcevc_dec 없음, 출력 2배 고정).

## 2. 구조

| 계층 | 파일 | 역할 |
|---|---|---|
| 추출·구문 (Qt 없음) | `src/lcevc/LcevcBitstream.*` | Annex-B·T.35·AV1 TU·IVF 에서 LCEVC NAL 추출, `toAnnexB`, base 코덱 판별(`.lvc`) |
| | `src/lcevc/LcevcSyntax.*` | LTM `Deserializer.cpp` 와 같은 순서·폭의 구문 파서, trace, `summarizeStream`(전역 설정·프레임별 picture_config) |
| 구문 트리 | `src/integration/LcevcTree.*`, 패치 0073 | AVC/HEVC/VVC 파서의 LCEVC NAL, AV1 metadata OBU 를 트리로 |
| 디코더 백엔드 | `src/integration/LcevcBackend.*` | `libbda-lcevc-ltm.so` / `liblcevc_dec_api.so` 를 QLibrary 로 로드, 같은 인터페이스 |
| 디코더 | `src/integration/DecoderLcevc.*` | base 디코더를 감싸 출력 그림마다 enhancement 적용, display signal, overlay |
| upstream | 패치 0074 | `decoderFFmpeg` EOF drain (아래 4절) |
| | 패치 0075 | 엔진 2개, `.lvc`, 짝짓기, 출력 크기 전환, seek 진입점, Frame 탭 |
| | 패치 0076 | 분리 파일 입력, File 메뉴, playlist 저장 |
| 도구 | `tools/cli/lcevc-av1-mux.cpp` | AV1 IVF 에 LCEVC metadata OBU 삽입 |
| 라이브러리 | `third_party/ltm`(서브모듈), `third_party/lcevc-ltm-wrapper/`, `scripts/setup-lcevc-ltm.sh`, `scripts/setup-lcevcdec.sh`, `third_party/lcevcdec-api/` | 빌드는 별도 스크립트, 앱은 런타임 로드 (없으면 엔진이 base 로 폴백) |

### 짝짓기 (base 그림 ↔ LCEVC NAL)

- 두 디코더 모두 **출력(POC) 순서**로 그림을 받는다 (LTM ModelDecoder 와 같음). YUView base 디코더도 출력 순서로 내준다.
- 그래서 열 때 한 번, 표시 순서 프레임마다 LCEVC NAL(Annex-B 바이트)을 정해 둔다 (`LcevcStream::perFrame`).
  - Annex-B: NAL 은 **파일에서 그 NAL 뒤에 끝나는 첫 base 그림**의 것 (`ParserAnnexB::getLcevcPerFrameDisplayOrder`). 두 테스트 스트림 모두 LCEVC NAL 이 access unit 앞에 온다. "뒤에서 시작하는" 규칙은 AVC 에서 틀린다 — 첫 프레임 범위가 SPS 부터 시작해 IDR NAL 이 frame 1 로 밀렸다 (실측 0/4 → 수정 후 4/4).
  - IVF(AV1): TU k 의 metadata OBU → 표시 프레임 k (TU 하나 = 표시 프레임 하나 가정).
- `DecoderLcevc` 는 다음 출력이 몇 번째 프레임인지만 안다 (`setNextFrameIndex`, seek 마다).

### seek

- enhancement 는 그림 간 상태가 있다 (temporal buffer, 마지막 global_config). 시작점 = base seek point 이면서 LCEVC IDR(없으면 0)인 프레임까지 뒤로 간다 (`LcevcStream::entryFrameFor`). 현재 위치가 그보다 앞이면 seek 하지 않고 이어서 디코딩한다.
- LTM golden 은 모든 그림이 type 28 (IDR 없음) 이라 항상 0 부터 디코딩한다.

### display signal (LTM)

Enhanced / Base / LoQ-1 prediction (base upsampled) / LoQ-1 residual / LoQ-0 prediction / LoQ-0 residual / Temporal buffer / Temporal mask. 모두 출력 크기로 최근접 확대. 잔차·temporal buffer 는 중간값(`1 << (depth-1)`) + 잔차로 표시하고 difference 로 표시된다. LCEVCdec 는 Enhanced / Base.

### overlay (LTM, 통계 type 400–403)

| type | 이름 | 값 |
|---|---|---|
| 400 | LCEVC LoQ-0 residual | transform block 당 \|잔차\| 합 (샘플 단위), 0 인 블록은 생략 |
| 401 | LCEVC LoQ-1 residual | 같음, 출력 좌표로 환산 |
| 402 | LCEVC temporal refresh | temporal mask 192(intra/refresh) 인 블록 |
| 403 | LCEVC tile | tile 번호 |

### Frame 탭

`playlistItem::getFrameInfo` → `FileInfoWidget::setFrameInfo`. 구문 파서 결과(`summarizeStream`)라 디코딩과 시점이 엇갈리지 않는다: nal_unit_type, base→enhanced 크기, transform, scaling, upsampler, step width, quant_matrix_mode, temporal, dithering, 잔차 바이트.

## 3. 검증

| 항목 | 방법 | 결과 |
|---|---|---|
| 구문 | 단위 `lcevc-syntax`: LTM BITSTREAM_DEBUG trace 와 필드 단위 대조 (HEVC 30 NAL 396 필드, H.264 4 NAL 409 필드), T.35·AV1·IVF·`toAnnexB` 왕복 | PASS |
| 트리 | 회귀 54: 실제 파서로 `.lvc` 2종, AV1 metadata OBU | PASS |
| 분리 파일 | 회귀 56: Annex-B·MP4 base + enh.nal, playlist 복원, 거부 경로 | PASS |
| AV1 | 회귀 57: metadata OBU 트리, LTM over dav1d 8/8, LCEVCdec 전 프레임 + luma 일치 | PASS |
| 디코딩 | 회귀 55: item 경로로 전 프레임 MD5 == LTM ModelDecoder `--dithering_switch=false` (`tests/data/lcevc/*.ltm-md5.txt`). LTM·LCEVCdec 모두 30/30, 4/4. 역방향 seek, Base signal == base 2배, LoQ-0 residual signal·overlay, Frame 탭, FFmpeg 로 복귀 | PASS |

golden(HEVC)은 잔차가 전부 0 이라 업샘플 경로만 검증한다. 잔차 경로는 H.264 스트림이 검증한다.

## 4. 분리 파일 입력 (P6, 패치 0076)

- File > **Open LCEVC (base + enhancement)...**: base 압축 스트림(Annex-B, IVF, MP4 등 아이템이 여는 것 전부) 다음에 enhancement 파일을 고른다.
- enhancement = LCEVC 인코더가 쓰는 `enh.nal` (LCEVC NAL, 출력 순서, 4바이트 start code) + 옆의 `enh.nal.sizes` (그림별 바이트 수, 0 = enhancement 없는 그림). `.sizes` 가 없으면 NAL 하나 = 그림 하나. `enh.bin` 은 인코더 내부 형식이라 읽지 않는다.
- `playlistItemCompressedVideo::attachLcevcEnhancement` 가 `LcevcStream` 을 만들고 LTM 엔진으로 바꾼다. 경로는 playlist 에 `lcevcEnhancement` 로 저장되어 다시 열 때 엔진과 함께 복원된다.
- 컨테이너 + FFmpeg base 는 `DecoderLcevc::pushAVPacket` 이 packet 을 FFmpeg base 디코더로 넘긴다.
- base 를 YUV 로 주는 입력은 넣지 않았다 (압축 아이템에 raw 입력을 더하는 구조 변경).

## 5. AV1 + LCEVC 테스트 스트림 (P7, `tools/cli/lcevc-av1-mux`)

LTM 은 AV1 base 를 모른다. lcevc_ai 레시피대로 H.264 carrier 를 base 자리에 두고 `--base_recon` 으로 AV1 복원 영상을 주면 enhancement 가 AV1 base 기준으로 계산된다.

```bash
ffmpeg -f lavfi -i testsrc2=size=512x288:rate=25 -frames:v 8 -pix_fmt yuv420p -f rawvideo l1.yuv
ffmpeg -f rawvideo -s 512x288 -pix_fmt yuv420p -r 25 -i l1.yuv -vf scale=256:144 \
       -c:v libaom-av1 -crf 40 -cpu-used 8 -g 8 -lag-in-frames 0 base_av1.ivf      # TU 하나 = 표시 프레임 하나
ffmpeg -c:v libdav1d -i base_av1.ivf -f rawvideo -pix_fmt yuv420p av1_recon.yuv
ffmpeg -f rawvideo -s 256x144 -pix_fmt yuv420p -r 25 -i av1_recon.yuv -c:v libx264 -bf 0 -g 8 -f h264 carrier.h264
ModelEncoder -i l1.yuv -w 512 -h 288 -r 25 -l 8 -f yuv420p -b avc --base carrier.h264 \
             --base_recon av1_recon.yuv --cq_step_width_loq_0 1024 --encapsulation nal -o enc.lvc
ModelDecoder -i enc.lvc -o ref.yuv -b avc -y av1_recon.yuv --encapsulation nal --dithering_switch=false   # 기준
lcevc-av1-mux base_av1.ivf enc.lvc lcevc_av1.ivf
```

- 운반 형식: TU 마다 temporal delimiter·sequence header 뒤에 `OBU_METADATA` (metadata_type 4) = `B4 00 50 00` + LCEVC NAL(start code 포함) + trailing bits. VQAnalyzer 6.6 이 8 TU 모두 `OBU_METADATA`, `itu_t_t35_country_code = 180` 으로 읽고 디코드 오류가 없다. mux 후 dav1d 출력은 mux 전과 바이트 동일.
- carrier 는 B-frame 없이 (`-bf 0`) — enc.lvc 의 파일 순서가 출력 순서여야 한다.
- 결과 `tests/data/lcevc/testsrc2_av1_lcevc.ivf` (ModelEncoder 기본 upsample = adaptive cubic, chroma 도 처리) 와 프레임별 MD5.

## 6. 부수 수정: FFmpeg EOF drain (패치 0074)

- 증상: HEVC golden 을 FFmpeg 엔진으로 열면 마지막 4 프레임(26–29)이 frame 25 의 반복이었다 (LCEVC 와 무관, base 만으로 재현).
- 원인: `decoderFFmpeg::pushAVPacket` 이 빈 packet 에 flushing 플래그만 세우고 `avcodec_send_packet(ctx, NULL)` 을 보내지 않아, 재정렬로 들고 있던 그림이 나오지 않았다.
- 수정: NULL packet 을 보내 draining 에 들어가고, flushing 중 `AVERROR_EOF` 는 끝으로 처리한다. B-frame 이 있는 FFmpeg 디코딩 전부에 해당한다.

## 7. LCEVCdec 과 LTM 의 차이 (Progy 조사)

- **bitstream 버전**: LCEVCdec 4.3 기본(버전 2)은 Huffman 코드 길이 필드 폭 `bit_width(0)` 을 0 bit 로 읽고, LTM 은 1 bit 로 쓴다. 이런 테이블이 있는 temporal chunk 에서 LCEVCdec 은 chunk 를 지나쳐 무한 루프를 돈다 (`decode.c:260-265`, 탈출 조건 없음). AV1 테스트 스트림 picture 6 에서 재현. 해결: V-Nova SDK 인코더 스트림은 global_config 앞 additional_info(SEI, user data registered `B4 00 50 00`)에 버전을 싣고, LTM·사내 인코더 스트림은 싣지 않는다. 그래서 열 때 `signalledBitstreamVersion` 으로 확인해, 신호가 있으면 LCEVCdec 이 스트림을 따르게 두고 없으면 `force_bitstream_version=1` 을 준다 (SDK 스트림 실물로는 미검증). 멈춘 디코더는 `LCEVC_DestroyDecoder` 가 영원히 기다리므로, 5초 안에 그림이 안 나오면 디코더를 버리고 새로 만든다 (스레드 하나와 base 버퍼는 프로세스 끝까지 남는다).
- **업스케일 clamp**: LCEVCdec 은 업스케일 1D pass 결과를 S15 `[-16384, 16383]` 로 포화시킨다 (`upscale_scalar.c:34-37`, SIMD 도 같음). LTM 과 DIS 8.7.3 은 clamp 하지 않는다. 범위를 넘는 overshoot (testsrc2 색 막대 경계, 주로 chroma) 에서 출력이 다르다. LCEVCdec 은 재생 전용이라 맞추지 않고 문서로 남긴다 (맞추려면 LCEVCdec 패치 + `disable_simd`). 최종 IS 본문 대조는 미검증.

## 8. 남은 것·한계

- 10-bit, 4:4:4, conformance window, tiled 스트림은 실행하지 않았다.
- AV1 경로는 우리가 만든 스트림 하나(libaom base, LTM enhancement)로만 검증했다. 다른 도구가 만든 AV1 + LCEVC 스트림과의 상호 운용은 미검증.
- LCEVC NAL 이 access unit 의 slice 뒤에 오는 스트림(suffix 배치)은 짝짓기가 한 프레임 밀린다 (HEVC 는 항상, AVC 는 뒤따르는 NAL 에 따라). 한 access unit 에 LCEVC NAL 이 둘 이상이면 마지막 것만 쓴다. 둘 다 실물 미확인 (Progy 검토).
- 스트림 중간에 global_config 가 출력 크기·chroma·bit depth 를 바꾸면 디코더 오류로 멈춘다 (캔버스는 첫 global_config 로 고정).
- IVF 는 열 때 파일 전체를 mmap 해 훑는다.
