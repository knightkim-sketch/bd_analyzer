# 디코더 plugin 추가하기

bd-analyzer 의 디코더는 실행 파일에 링크되지 않는다. 앱이 실행 중에 공유 라이브러리(`.so`)를
`QLibrary` 로 dlopen 해서 쓴다. 이 문서는 그런 디코더 라이브러리("디코더 plugin")를 새로 넣는
방법을 정리한다. HM(HEVC) 을 넣은 작업(패치 0082–0083, `scripts/setup-hm.sh`)을 예로 든다.

> `plugins/sdk` 의 `IFilter/IPattern/IMetric` 은 필터·패턴·메트릭용 인터페이스라 디코더와는 관계가 없다.

## 1. 앱이 디코더 라이브러리를 찾는 순서

`decoderBaseSingleLib::loadDecoderLibrary()`
([decoderBase.cpp](../../third_party/yuview/upstream/YUViewLib/src/decoder/decoderBase.cpp)) 는
라이브러리 이름(예: `libHMDecoder`)을 아래 경로에서 차례로 찾는다. 먼저 찾은 것을 쓴다.

| 순서 | 경로 | 비고 |
|---|---|---|
| 0 | Settings > Decoders 에서 코덱별로 지정한 라이브러리 파일 | 지정했으면 먼저 시도하고, 실패하면 1 로 |
| 1 | Settings > Decoders > **Decoder Search Path** | 사용자 지정 폴더 |
| 2 | `현재 디렉토리/`, `현재 디렉토리/decoder/` | |
| 3 | `실행 파일 위치/`, **`실행 파일 위치/decoder/`** | **배포본의 표준 위치** (`/opt/bd-analyzer/decoder/`) |
| 4 | 시스템 라이브러리 경로 | `LD_LIBRARY_PATH`, `/usr/lib64` 등 |

라이브러리를 찾지 못하면 그 엔진은 실패한다. AV1(dav1d)·H.264(JM)·HEVC(HM) 는 이때 FFmpeg 로
넘어간다. 다른 엔진은 아이템이 열리지 않는다.

## 2. 경우 A — YUView 에 어댑터가 이미 있는 디코더

upstream YUView 에는 다음 디코더 어댑터가 있다. 이 디코더들은 **라이브러리만 만들어 `decoder/`
에 두면** 바로 쓸 수 있다.

| 엔진 | 라이브러리 이름 | 코덱 | bd-analyzer 번들 |
|---|---|---|---|
| HM | `libHMDecoder.so` | HEVC | ○ (`scripts/setup-hm.sh`) |
| dav1d | `libdav1d-internals.so` | AV1 | ○ (`scripts/setup-dav1d.sh`) |
| libde265 | `libde265-internals.so` (또는 `libde265.so`) | HEVC | × |
| VTM | `libVTMDecoder.so` | VVC | × |
| VVDec | `libvvdecLib.so` | VVC | × |

라이브러리는 어댑터가 resolve 하는 함수(`decoderXXX::resolveLibraryFunctionPointers()`)를 모두
export 해야 한다. 하나라도 빠지면 로드에 실패한다.

### 절차 (HM 의 예)

1. **소스를 저장소에 고정한다.** submodule 이 기본이다. 네트워크로 받을 수 없으면 tarball 을
   `third_party/<name>/` 에 두고 sha256 을 스크립트에 적는다 (HM: `third_party/libhm/`).
2. **`scripts/setup-<name>.sh` 를 쓴다.** `setup-jm-analyzer.sh` 와 `setup-hm.sh` 를 본으로 삼는다.
   - 깨끗한 환경에서 gcc-toolset-13 으로 다시 실행한다. RDA shell 의 gcc/libstdc++ 가 섞이지 않게 하기 위해서다.
   - 서드파티 소스는 **복사본**에만 패치한다 (`third_party/<name>/patches/`).
   - `-static-libstdc++ -static-libgcc` 로 링크하고 version script 로 API 함수만 export 한다. 같은
     프로세스에 올라오는 다른 디코더(JM 등)와 C++ 심볼이 부딪히지 않게 하기 위해서다.
   - 결과를 `build/YUViewApp/decoder/` 에 두고 `ldd`·`nm -D` 로 검사한다.
3. **기본 엔진을 정한다 (선택).** 이 디코더를 해당 코덱의 기본으로 쓸 때만 필요하다.
   `playlistItemCompressedVideo.cpp` 의 엔진 선택에 분기를 넣고, 로드에 실패하면 FFmpeg 로 넘어가는
   조건에 엔진을 추가한다. upstream 은 패치로만 고친다 (HM: `0082-hevc-default-hm-engine.patch`).
4. **번들에 넣는다.** `scripts/make-bin-bundle.sh` 의 "6. ... 디코더" 단계에 선택 복사를 추가하고,
   라이선스 파일을 `decoder/licenses/` 에 넣고, `check-deps.sh` 검사 줄을 추가한다. RPM 은
   `decoder/` 폴더를 통째로 담으므로 spec 은 설명 문구만 고친다.
5. **회귀 테스트를 넣는다.** FFmpeg 디코드와 프레임 단위로 비교하는 테스트가 가장 강하다
   (`tests/regression/62-hm-hevc-decode.cpp`). 라이브러리가 없으면 SKIP 하게 한다.
6. **검증한다.** `./scripts/build.sh && ./tests/run-regression.sh` 를 돌리고, GUI 에서 실제 스트림을 연다.

### HM 에서 실제로 걸린 것

- **gcc 13 경고가 `-Werror` 로 막힌다.** HM16.15 의 makefile 은 `-Werror` 로 빌드한다. gcc 13 의
  `class-memaccess`·`array-bounds` 경고가 오류가 되므로 빌드 복사본에서 `-Werror` 만 뺀다.
- **스트림의 마지막 프레임이 빠졌다.** 원인은 두 군데였다.
  1. libHM wrapper 가 끝(EOF)을 알리는 빈 push 를 오류로 거부했다. 그래서 마지막 그림의 loop
     filter 와 DPB flush 가 일어나지 않았다 → `third_party/libhm/patches/0001-eof-flush.patch`.
  2. YUView `decoderHM` 이 push 없이 여러 그림을 연달아 꺼낼 때 앞 그림의 버퍼를 재사용했다 →
     `0083-hm-flush-output-buffer.patch`.

  둘 중 하나만 고치면 마지막 2–3 프레임이 여전히 빠지거나 앞 프레임과 같게 나온다. **IENT 가
  배포한 prebuilt `libHMDecoder.so` 에는 1 의 수정이 없다.** 반드시 `setup-hm.sh` 로 빌드한 것을 쓴다.
- HM 은 디코드한 그림마다 `POC n TId: ...` 줄을 stdout 에 찍는다. 터미널에서 실행하면 보이지만 동작에는 영향이 없다.

## 3. 경우 B — 어댑터가 없는 디코더

JM(H.264)·LCEVC 처럼 YUView 에 어댑터가 없는 디코더는 C++ 코드를 써야 한다.

1. 서드파티 디코더를 감싸는 C wrapper 와 공유 라이브러리를 만든다 (`third_party/jm-analyzer/bda_jm.h`).
2. `src/integration/Decoder<Name>.{h,cpp}` 에 `decoder::decoderBase` 를 상속한 어댑터를 쓴다 (`DecoderJm`).
3. upstream 패치로 다음을 추가한다 (JM: `0078-jm-h264-analysis.patch`).
   - `DecoderEngine` enum 과 `DecoderEngineMapper` 에 엔진을 추가한다.
   - 해당 코덱의 후보 엔진 목록(`possibleDecoders`)에 엔진을 넣는다.
   - `playlistItemCompressedVideo::allocateDecoder()` 에서 엔진을 생성한다.
4. 이후는 경우 A 의 2–6 과 같다.

## 4. 사용자 쪽: 이미 설치된 bd-analyzer 에 디코더만 추가하기

다시 빌드하지 않고 라이브러리만 넣을 수 있다 (경우 A 디코더만 해당).

```bash
# RPM 설치본 (root)
sudo cp libHMDecoder.so /opt/bd-analyzer/decoder/
# tar.gz 설치본
cp libHMDecoder.so bd-analyzer-<version>/decoder/
```

root 권한이 없으면 아무 폴더에 두고 **Settings > Decoders > Decoder Search Path** 에 그 폴더를
지정한다. 앱을 다시 시작한 뒤 HEVC 파일을 열고 Properties 의 **Video** 탭 **Decoder** 콤보에서 `HM` 을 고른다.
패치 0082 가 들어간 버전은 처음부터 HM 으로 연다. 열리지 않으면 콤보 옆 오류
메시지에 `Error loading library: ...` 가 나온다.

## Abbreviations

| 약어 | 원문 | 설명 |
|---|---|---|
| HM | HEVC Test Model | HEVC 레퍼런스 인코더·디코더 (JCT-VC) |
| JM | Joint Model | H.264/AVC 레퍼런스 소프트웨어 |
| VTM | VVC Test Model | VVC 레퍼런스 소프트웨어 |
| LCEVC | Low Complexity Enhancement Video Coding | MPEG-5 Part 2. 기본 코덱 위에 얹는 향상 계층 |
| DPB | Decoded Picture Buffer | 참조·출력 대기 중인 디코드된 그림의 버퍼 |
| EOF | End Of File | 입력 스트림의 끝 |
| POC | Picture Order Count | 그림의 표시 순서 번호 |
| NAL | Network Abstraction Layer | H.264/HEVC 비트스트림의 단위(NAL unit) |
