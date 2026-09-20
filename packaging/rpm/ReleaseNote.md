# bd_analyzer 릴리스 노트

패키지 파일과 함께 `build/dist/` 로 복사된다 (`scripts/package.sh`). 원본은
`packaging/rpm/ReleaseNote.md` 다 — `build/` 는 gitignore 대상이라 산출물 폴더의 사본은
다음 클린 빌드에 사라진다.

버전별 한 줄 요약이다. 배경과 근거는 소스 저장소의 `docs/ai/40-tasks/` TASK 문서에 있다.

---

## 0.3.1 (2026-09-20)

AV1 비트스트림 파서 정확도 수정이 중심이다. 전부 크래시 없이 **조용히 틀린 값**을 내던 것들이라,
0.3.0 으로 읽은 타일 스트림의 프레임 헤더 값은 신뢰할 수 없다.

### AV1 구문 파싱 수정

타일이 있는 스트림을 VQ Analyzer 와 필드 단위로 대조해 찾은 결함 5건이다. 앞의 넷은 결과가 같다 —
리더가 인코더보다 비트가 뒤처지고, 그 뒤 uncompressed header 전체가 어긋난 오프셋에서 해석된다.

- `tile_info()` 의 `minLog2TileRows` 가 unsigned 언더플로로 감싸 **`increment_tile_rows_log2` 를 한 번도
  읽지 않았다.** 타일 행이 있는 모든 스트림이 해당된다.
- 같은 함수에서 `context_update_tile_id` 와 `tile_size_bytes_minus_1` 을 `f(n)` 이 아닌 ns(n) 코딩으로,
  그것도 비트 수를 최대값 자리에 넘겨 읽었다.
- `cdef_uv_sec_strength` 를 `f(2)` 가 아닌 `f(4)` 로 읽었다. 바로 위 luma 쌍둥이는 정상이었다.
- `SeenFrameHeader` 를 temporal delimiter 에서만 초기화해, 한 패킷에 여러 프레임이 담기면
  **둘째 이후 프레임의 구문이 전혀 나오지 않았다.** hidden ARF 가 있는 스트림 전부가 해당된다.
- 참조 프레임 갱신 과정(spec 7.20)이 통째로 빠져 `RefOrderHint` 가 늘 0 이었고, 그 결과
  `skip_mode_present` 라는 실재하는 비트를 한 번도 읽지 않았다.
- OBU 루프의 경계 검사가 패킷 끝의 작은 OBU 를 버렸다. `show_existing_frame` 프레임 헤더가
  정확히 3바이트라 **매번 통째로 사라졌다.**

### tile group 파싱 추가

- `tile_group_obu()` 와 `trailing_bits()` 를 파싱한다. **타일별 `tile_size_minus_1`** 이 비트스트림
  패널에 나온다 — 타일 그리드에 비트가 실제로 어떻게 갈렸는지는 프레임 헤더만으로는 알 수 없다.
- 이로써 파싱하는 구문 요소가 VQ Analyzer 와 값까지 일치한다.
- AV1 에는 emulation prevention 이 없는데 공용 리더가 기본으로 적용하고 있었다. 껐다.
  테스트한 스트림에서는 출력 차이가 없었으나, 수 KB 의 타일 페이로드를 지나가는 경로가
  새로 생겼으므로 운에 맡기지 않는다.

### 그 외

- **헤드리스 `sb-bdrate-export`** 추가 — GUI 전용이던 SB BD-rate 를 창 없이 돌려 superblock 당
  CSV 로 뽑는다. 배치 귀속 분석용이다. 빌드는 `tests/tools/build-tool.sh sb-bdrate-export`.
- **기능 검증 리스트** (`docs/dev/feature-verification.md`) 추가 — 회귀 스위트가 지키는 것과
  지키지 않는 것을 갈라 적었다. 회귀는 전부 헤드리스라 창·단축키·오버레이·네트워크·패키지는
  지나가지 않는다. 회귀를 돌리면 끝에서 이 문서를 가리킨다.
- 패키징이 rpm 을 공유 설치 위치(`/fs2/install_devel/YUViewer/bd-analyzer-v<버전>/`)에 복사한다.
  해당 마운트가 없는 머신에서는 건너뛴다.

### 알려진 제한

- 타일 구문 검증은 uniform tile spacing · 8-bit 4:2:0 · libaom 인코딩 기준이다.
  non-uniform spacing 과 `tg_start` 가 0 이 아닌 다중 tile group 경로는 **미검증**이다.

---

## 0.3.0 (2026-09-18)

0.2.0 이후 추가된 내용. 상세는 `docs/ai/30-designs/` 의 설계 문서에 있다.

### SB / Frame / Sequence BD-rate

- **View → SB BD-rate from Selection (`Ctrl+R`)** — 선택한 스트림들을 하나의 RD 곡선(group)으로 묶고, group 간 BD-rate 를 superblock / frame / sequence 세 단위로 계산해 popup 의 그래프와 table 로 보여준다.
- 같은 수식을 세 단위에 각각 적용한다 — "시퀀스 전체로는 이득인데 이 superblock 에서는 손해" 를 한 화면에서 판단할 수 있다.
- `Sequence` 체크박스가 곧 스위치다. 켜면 전 프레임 sweep 이 시작되고 끄면 취소된다. GUI 스레드에서 프레임 단위로 슬라이스하므로 창이 멈추지 않고, 진행 중에도 부분 결과를 그린다.
- group table 에서 이름을 편집하고 anchor 를 바꾼다. anchor 변경은 수집값을 재사용하고 BD-rate 만 다시 계산한다.
- BD-rate 가 정의되지 않는 경우를 구분해 표시한다 — `no PSNR overlap`, `too few points`, `lossless`. superblock 단위에서는 값이 안 나오는 것이 정상인 블록이 많다.
- rate 축은 `bits + 1` (log) 이다. skip 된 superblock 은 0 bits 라 로그 축에서 사라지는데, "그 화질을 공짜로 얻었다" 가 그 블록의 가장 중요한 정보다. table 은 실제 bits 를 그대로 보여준다.
- 그래프 양 축에 눈금·수치·격자를 넣었다. rate 축은 로그지만 눈금은 bit 단위로 round 한 값(1, 2, 5, 10 …)에만 찍는다.
- dav1d analyzer 디코더가 아닌 스트림, 격자가 다른 스트림, 점이 2개 미만인 group 은 이유를 붙여 거절한다.

### MP4 컨테이너 분석

- Bitstream Analysis 패널에 **Container 탭** 추가 — box 계층 트리(오프셋·크기·비고)와 샘플 목록(파일 오프셋·크기·sync·decode time).
- libavformat 은 패킷만 주고 컨테이너 구조는 보여주지 않는다. `stsc`/`stco`/`stsz` 를 직접 풀어 **샘플이 파일 어디에 있는지**까지 낸다.
- fragmented MP4 지원 — 샘플이 sample table 이 아니라 `moof/traf` 의 `trun` 에 있는 경우도 `tfhd`/`trex`/`tfdt` 를 읽어 찾아낸다.
- 64-bit chunk offset(`co64`), 다중 트랙, 오디오 샘플 엔트리, version 1 `mdhd` 를 처리한다.
- 큰 파일을 위해 파일을 읽지 않고 매핑한다 — 헤더를 보려고 `mdat` 을 메모리로 끌어오지 않는다.

### AI 어시스턴트 패널

- **View → Dock Panels → Show AI Assistant (`Ctrl+K`)** — 화면에 열린 스트림·프레임·클릭한 superblock 을 컨텍스트로 넘겨 질문할 수 있다.
- 무엇을 첨부할지 체크박스로 고르고, **실제로 보낸 내용을 그대로 보여준다.** 잘못된 수치로 그럴듯한 오답이 나오는 것에 대한 방어책이다.
- 세 가지 접근 모드 — 읽기 전용 / 이 프로젝트 안에서 편집 / 전체 접근. 현재 모드를 패널이 항상 표시한다.
- CLI 나 로그인이 없으면 이유를 붙여 비활성화된다.

### YouTube 다운로드·트랜스코딩 패널

- **View → Dock Panels → Show YouTube Transcode (`Ctrl+Y`)** — `links.txt` 를 목록으로 보여주고, 추가·삭제·저장한 뒤 일괄 다운로드/인코딩한다.
- 코덱 선택: AV1(libaom / SVT-AV1), H.265, H.264, copy, **다운로드 전용**. ffmpeg 에 실제로 있는 인코더만 노출한다.
- 다운로드 전용은 ffmpeg 을 거치지 않고 YouTube 가 준 컨테이너 그대로 출력 폴더에 저장한다 — 인코더 비교의 기준 소스로 쓸 수 있다.
- 다운로드를 먼저 파일로 받은 뒤 인코딩한다. ffmpeg 빌드에 따라 HTTPS 입력에서 죽는 경우가 있어 그 경로를 피한다.

### 버그 수정

- 분석 중인 playlist 항목을 삭제하면 **SIGSEGV** 로 죽던 문제 — 백그라운드 파싱 스레드에서 오는 시그널을 받는 슬롯 하나가 파서 null 검사를 빼먹고 있었다.
- 시퀀스 전체를 디코딩할 때 **`std::bad_alloc`** 로 죽던 문제 — dav1d 의 transform size 테이블을 블록 데이터의 raw 바이트로 인덱싱해, 범위 밖 값을 읽으면 루프가 끝나지 않고 통계 벡터가 GB 단위로 불어났다. BD-rate 없이 디코딩만 해도 재현됐다.
- BD-rate group 에 속한 스트림을 삭제하면 **use-after-free** 로 죽던 문제 — group 이 raw 포인터를 들고 있었다.
- 150프레임 스트림이 **151프레임으로 보고**되던 문제 — 프레임 개수를 inclusive 인덱스로 넘기고 있었다. 존재하지 않는 프레임에 sweep 이 재시도 예산을 낭비했다.
- 같은 클립을 두 인코더로 만든 곡선의 **이름이 구분되지 않던** 문제 — 파일명이 같으면 디렉토리명으로 구분한다.
- playlist 와 BD-rate 표에서 **긴 이름이 구분되지 않던** 문제 — 이름은 끝부분이 다른데 그 끝이 잘려나갔다. 가운데를 생략하고 tooltip 에 전체를 담는다.

---

## 0.2.0 (2026-09-06)

최초 RPM(0.1.0, 2026-08-20) 이후 추가된 내용.
상세: `docs/ai/40-tasks/TASK-0009-motion-estimation.md` (소스 저장소).

### 추가된 기능

- Motion Estimation 분석 기능 추가 — 표시 중인 프레임을 current, frame interval 만큼 떨어진 프레임을 reference 로 삼아 ME 를 수행한다.
- SVT-AV1 v4.2.0 정수 ME 재현 — HME 3단 캐스케이드, `check_00_center`, static block bypass 포함.
- odyssey open-loop ME 재현 — 1/2 meanpool 에서 center 결정 후 bilinear upscale 한 원본 크기에서 VBS 검색.
- ME 패널 (View → Dock Panels → Show Motion Estimation, `Ctrl+M`) — 알고리즘 선택, frame interval, block size, static bypass 스위치.
- frame interval 에 음수 지원 — 양수는 과거 프레임, 음수는 미래 프레임을 reference 로 삼는다.
- ME block size 를 8 / 16 / 32 / 64 체크박스로 선택 — 체크된 크기만 오버레이 타입이 생성된다.
- ME 를 취소 가능한 백그라운드 작업으로 실행 — 프레임·파라미터·선택이 바뀌면 진행 중인 계산을 취소하고 새로 시작한다.
- ME 진행률 표시줄 추가 — superblock 단위 진행률과 완료 여부를 표시한다.
- ME 결과를 기존 통계 오버레이로 그린다 — 비트스트림 MV 와 같은 색·굵기·줌 규칙을 따르고 스타일 대화상자와 CSV export 를 그대로 쓴다.
- 알고리즘 고유 cost 와 알고리즘 간 비교용 common SAD 를 함께 보고 — 두 값은 스케일이 다르므로 이름을 붙여 표시한다.
- 블록을 클릭하면 그 블록의 재현 MV 와 두 cost 가 Block Info 패널에 나온다 — 체크된 block size 마다 한 줄.
- compressed stream 에서도 ME 를 수행 — PSNR 비교용으로 붙인 org YUV 를 source 로 쓰므로 reference 프레임을 디코딩하지 않는다.
- 비트스트림 자체 MV 와 재현 MV 를 한 화면에 동시 표시 — 인코더가 찾은 MV 와 예상 MV 를 직접 비교할 수 있다.
- raw YUV 아이템에 통계 컨테이너 부여 — 이전에는 compressed / statistics-file 아이템만 가지고 있었다.
- playlist 를 세션 간 유지 — 종료 시 자동 저장하고 다음 시작에 묻지 않고 복원한다.
- File 메뉴에 "Save Playlist on Exit" on/off 스위치 추가 (기본 on).
- 같은 파일이 playlist 에 중복으로 담기지 않도록 앱 시작·종료 시 목록을 정리 — 이미 열린 파일을 다시 열면 그 항목을 선택한다.
- 종료 시 임시 분석 캐시를 묻지 않고 삭제 — 원본에서 다시 만들 수 있는 파일이므로 질문할 이유가 없다.
- 캐시 위치를 **실행한 디렉토리의 `.bd_analyzer/`** 로 변경 — 이전에는 바이너리 옆(`/opt`, 쓰기 불가)을 시도하고 `~/.cache/` 로 폴백해서, 홈 파티션이 작은 머신에서 GB 단위 디코딩 YUV 가 문제가 됐다. 용량이 있는 볼륨에서 실행하면 캐시도 거기에 생긴다.

### 버그 수정

- 종료 시 `free(): invalid pointer` 로 abort 하던 문제 — block statistics 섹션이 값 멤버인 채로 재부모돼 Qt 와 소유자가 이중으로 파괴했다.
- 오버레이를 켠 상태에서 playlist 의 다른 아이템을 선택하면 `spacerItems[0] != nullptr` assert 로 abort 하던 문제.
- 이름에 해상도가 없고 크기로도 포맷을 추측할 수 없는 raw YUV 를 열면 `bad_alloc` 으로 abort 하던 문제.
- "show MV" 를 켜도 MV 선이 보이지 않던 문제 — 1:1 배율에서 선 굵기가 0.25px 로 줄어들고, 같은 행의 cost 오버레이가 화살표를 덮고 있었다.
- 파일을 열면 항상 frame 0 이라 기본 interval 로는 reference 가 없는데, 그 사유가 표시되지 않아 기능이 죽은 것처럼 보이던 문제.
- 자동 저장된 playlist 가 한 번도 복원되지 않던 문제 — 파일 경로 자리에 디렉토리를 넘겨 모든 항목이 null 이 됐다.
- Y4M 을 org YUV 로 붙이면 프레임 오프셋이 어긋나 SSE · PSNR · ME 가 조용히 틀리던 문제 — Y4M 의 파일 헤더와 프레임 마커를 무시하고 평면 오프셋으로 읽었다.
- ME 계산이 끝나는 순간 창이 멈추던 문제 — 비재귀 뮤텍스를 스스로 다시 잠그는 자기 교착이었다.
- Motion Estimation dock 을 띄울 방법이 아예 없던 문제 — 숨겨진 상태로 출하됐는데 View 메뉴에 항목이 없었다.

### 알려진 제한

- odyssey closed-loop ME 는 미구현 (패널에 표시되나 선택 불가). 조사는 완료돼 있다.
- bi-prediction 미지원. 자료구조만 열어 뒀다.
- compressed stream 의 ME 는 org YUV 가 붙어 있어야 동작한다.
- Rocky Linux 8 / RHEL 8 (x86_64) 전용. 번들 Qt 가 `GLIBC_2.28` 을 요구하므로 CentOS 7.x 는 대상이 아니다.

---

## 0.1.0 (2026-08-20)

- 최초 설치형 패키지 — `bin/` 배포 번들에서 RPM 과 tar.gz 를 생성한다.
  상세: `docs/ai/40-tasks/TASK-0008-installable-package.md` (소스 저장소).
