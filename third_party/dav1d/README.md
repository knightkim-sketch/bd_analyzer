# dav1d analyzer 디코더

YUView 가 `dlopen` 하는 `libdav1d-internals.so` 의 소스다. AV1 블록 통계(예측 모드, 파티션,
모션 벡터)와 이제 **블록별 비트스트림 위치**를 내보낸다.

## 업스트림

```
https://github.com/ChristianFeldmann/dav1d
tag  0.2.1.0.analyze
sha  0a7f210
```

이 태그는 기존에 리포에 바이너리로만 들어 있던 `libdav1d-internals.so` 의 버전 문자열
(`0.2.1.0.analyze-0-g0a7f210`) 과 정확히 일치한다. 즉 셀프 빌드가 드롭인 교체품이며,
교체 후 회귀 테스트 13개가 모두 통과하는 것을 확인했다.

`upstream/` 는 git submodule 이 아니라 위 태그의 shallow clone 이다. 없으면:

```bash
git clone --branch 0.2.1.0.analyze \
    https://github.com/ChristianFeldmann/dav1d.git third_party/dav1d/upstream
git -C third_party/dav1d/upstream apply ../0001-block-bitstream-range.patch
```

## 이 리포가 추가한 것 — `0001-block-bitstream-range.patch`

업스트림 fork 에는 `export_bitsperblk` / `export_bitsused` 플래그가 있지만 주석에
`Not implemented yet` 이라고 적혀 있고 실제로 구현되어 있지 않다. 이 패치가 그 자리를 채운다.

| 추가 | 위치 | 내용 |
|---|---|---|
| `Av1Block::bitstream_start_bit` / `bitstream_end_bit` | `src/levels.h` | 블록 심볼을 읽은 비트 위치 |
| `Av1Block::sb_qindex` | `src/levels.h` | 슈퍼블록의 qindex (`ts->last_qidx`) |
| `Av1Block::sb_bit_count` | `src/levels.h`, `src/decode.c` | 슈퍼블록 전체가 소비한 비트 수 |
| `MsacContext::buf_start` / `abs_bit_offset` + 헬퍼 | `src/msac.h`, `src/msac.c` | 산술 디코더 비트 소비량 계산 |
| tile 절대 오프셋 전달 | `src/decode.c` (`setup_tile` 호출부) | `Dav1dData.m.offset` + ref 기준 오프셋 |
| `decode_b` 진입/종료 위치 기록 | `src/decode.c` | 잔차(residual) 읽기까지 포함 |
| `dav1d_analyzer_block_data_size()` | `include/dav1d/dav1d.h`, `src/lib.c` | ABI 확인용 |

### `sb_qindex`

스펙의 `CurrentQIndex` 다. 프레임의 base y AC qindex 에 슈퍼블록의 `delta_q` 를 더한 값이며,
`delta_q` 는 슈퍼블록의 첫 블록에서만 읽히므로 한 슈퍼블록의 모든 블록이 같은 값을 보고한다.
segmentation 이 블록별로 qindex 를 더 바꿀 수 있는데(`get_qidx()`), 그건 **포함하지 않는다.**

검증 (`test.ivf`, ffmpeg `-bsf:v trace_headers` 와 대조):

| 프레임 | trace_headers `base_q_idx` | 보고된 `sb_qindex` |
|---|---|---|
| 0 (KEY) | 22 | 22 |
| 1 (표시 프레임 = TU 의 4번째 frame OBU) | 72, 100, 114, **128** | **128** |

`delta_q_present = 0` 인 스트림이라 `sb_qindex == base_q_idx` 가 되는 것이 맞고, 특히 프레임 1 은
temporal unit 안의 마지막(표시) frame OBU 값을 정확히 골라낸다.

### `sb_bit_count`

`dav1d_decode_tile_sbrow` 의 슈퍼블록 루프에서 `decode_sb()` 호출 **전후의 msac 위치 차이**로
구한다. 슈퍼블록의 심볼은 tile 데이터 안에서 연속하므로 이 값은 **정확한 총합**이며, 파티션
트리 자체의 비용까지 포함한다 (블록별 구간을 더한 값과 달리). 계산 후 그 슈퍼블록이 덮는
모든 4x4 셀에 써 넣어, 소비자가 어느 위치를 읽어도 값을 찾을 수 있게 한다.

검증 (`test.ivf`, 176x144 = 슈퍼블록 3x3 = 9개):

| 프레임 | 슈퍼블록 비트 합 | 실제 tile 데이터 |
|---|---|---|
| 0 | 17569 bit | 약 17568 bit (byte 24..2220) |
| 1 | 3204 bit | 약 3208 bit (4번째 frame OBU) |

즉 tile 데이터를 빈틈없이 설명한다. 또한 모든 슈퍼블록에서 `sb_bit_count >=` 내부 블록들의
비트 구간 폭이 성립한다 (파티션 트리 비용 때문에 크거나 같다).

### 좌표계 — 왜 pointer 를 넘기지 않았나

호출자가 `dav1d_send_data` 로 넘긴 버퍼의 **오프셋**만 넘긴다. 포인터를 struct 로 넘기면
버퍼가 해제된 뒤에도 읽을 수 있게 되어 use-after-free 가 되기 쉽다. 오프셋은 수명 문제가
없고, 속도 이득(파일을 다시 파싱하지 않는 것)은 동일하다.

연결 방식은 이렇다. YUView 는 packet 을 OBU 단위로 쪼개서 push 하는데, push 할 때
`Dav1dData.m.offset` 에 **그 OBU 가 packet 안에서 시작하는 바이트 위치**를 넣는다.
dav1d 는 `dav1d_data_ref` 에서 `m` 을 그대로 복사해 tile 데이터까지 들고 가므로,
`decode_b` 가 보고하는 위치는 이미 **packet 좌표**다. 그래서 "디코딩된 프레임이 어느 push
버퍼에서 나왔는지" 를 따로 추적할 필요가 없다 — 오프셋이 데이터와 함께 흘러간다.

### 정확도 — 산술 부호화의 한계

AV1 은 블록 syntax 를 multi-symbol 산술 부호기(msac)로 코딩한다. 따라서 **블록에 고유한
비트 경계는 존재하지 않는다.** 보고되는 값은 msac 이 그 블록을 디코딩하는 동안 소비한
비트 구간이며, 다음이 성립한다.

- 비트 소비량 계산은 정확하다 (추정이 아니다): `(buf_pos - buf_start)*8 - cnt - 15`.
  `dav1d_msac_init` 이 `cnt = -15` 로 시작하고 `ctx_norm` 이 심볼당 사용 비트만큼 깎기 때문.
- 검증: `test.ivf` frame 0 에서 블록 구간의 합이 17323 bit, 실제 tile 데이터가 약 17568 bit.
  frame 1 (4개의 frame OBU 중 마지막이 표시 프레임) 은 2994 bit vs 약 3208 bit.
  즉 블록 구간들이 tile 데이터를 거의 빈틈없이 덮는다.
- 한 블록이 **5 bit** 로 끝나는 것은 버그가 아니라 정상이다 (평탄한 영역의 skip 블록).
  그래서 hexdump 는 블록 바이트 앞뒤로 문맥 32 바이트를 함께 보여준다.

### 디코딩 정확성

패치는 필드 추가와 위치 기록만 하므로 디코딩 경로를 바꾸지 않는다. 확인:

```
test.ivf   dav1d(패치본) = ffmpeg(libdav1d 1.x)  e5b6aca92451...  MATCH
big.ivf                                          bdab98e18704...  MATCH
sb128.ivf                                        d94620f9baf9...  MATCH
test2.ivf                                        e5b6aca92451...  MATCH
```

독립 디코더와 **bit-exact** 이므로 libaom / svt-av1 를 참고한 버그 수정은 필요하지 않았다.

(dav1d 0.2 의 CLI 는 raw `.obu`/`.av1` demuxer 가 없어서 그 파일만 CLI 로는 못 읽는다.
YUView 는 libavformat 으로 demux 하므로 무관하며, `.av1` 도 `.ivf` 와 동일한 픽셀 해시를 낸다.)

## 빌드

```bash
./scripts/setup-dav1d.sh          # build/YUViewApp/decoder/libdav1d-internals.so 에 설치
```

- `meson` 은 시스템에 없다: `pip3 install --user 'meson==1.2.3'`
- `nasm` 이 없어 `-Dbuild_asm=false` 로 빌드한다. 출력은 위처럼 bit-exact 이고 속도만 느리다.
  nasm 을 설치했다면 asm 을 켜도 된다.

## ⚠️ ABI — 반드시 짝을 맞출 것

`bitstream_start_bit` / `bitstream_end_bit` / `sb_qindex` 는 `Av1Block` 끝에 붙는다. 그래서
`sizeof(Av1Block)` 이 32 → 56 바이트로 바뀐다. YUView 는 `blk_data` 를 자기 `sizeof` 로
인덱싱하므로(`blockData[y * b4_stride + x]`), 라이브러리와 레이아웃이 다르면 **모든 블록을
잘못된 위치에서 읽어 그럴듯하지만 틀린 통계**가 나온다. 에러도 안 난다.

이걸 막기 위해:

- `YUViewLib.pro` 가 `YUVIEW_DAV1D_AV1BLOCK_HAS_BITSTREAM_RANGE` 를 정의한다.
- `decoderDav1d::resolveLibraryFunctionPointers()` 가 `dav1d_analyzer_block_data_size()` 를
  호출해 자기 `sizeof(Av1Block)` 과 비교하고, 다르면 **명시적 에러로 로딩을 거부한다.**
  구버전 analyzer 라이브러리는 이 심볼이 아예 없으므로 0 으로 취급되어 역시 거부된다.

따라서 `bin/` 배포 번들을 다시 만들 때는 `scripts/make-bin-bundle.sh` 가 새 `.so` 를
가져가도록 먼저 `scripts/setup-dav1d.sh` 를 돌려야 한다.
