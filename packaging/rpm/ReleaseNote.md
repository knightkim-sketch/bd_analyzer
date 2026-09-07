# bd_analyzer 릴리스 노트

패키지 파일과 함께 `build/dist/` 로 복사된다 (`scripts/package.sh`). 원본은
`packaging/rpm/ReleaseNote.md` 다 — `build/` 는 gitignore 대상이라 산출물 폴더의 사본은
다음 클린 빌드에 사라진다.

버전별 한 줄 요약이다. 배경과 근거는 소스 저장소의 `docs/ai/40-tasks/` TASK 문서에 있다.

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
