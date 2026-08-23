# bd_analyzer

비디오 코덱 개발·검증용 통합 분석 도구 — 비트스트림 분석기(AV1/H.264/HEVC), YUV 분석기, YUV 생성기, 이미지 필터.

[IENT/YUView](https://github.com/IENT/YUView) (GPLv3) 기반 fork.
결정 근거는 [docs/ai/20-decisions/ADR-0001](docs/ai/20-decisions/ADR-0001-fork-vs-library.md).

## 디렉토리 구조

```
bd_analyzer/
├── docs/
│   ├── ai/                    AI·사람 공용 문서 (조사·결정·설계·작업)  ← docs/ai/README.md 부터 읽을 것
│   ├── dev/                   빌드/기여 가이드
│   └── user/                  사용자 매뉴얼
│
├── third_party/
│   └── yuview/
│       ├── upstream/          IENT/YUView git submodule — 직접 수정 금지
│       └── patches/           upstream에 적용할 패치 (NNNN-*.patch). 20개 미만 유지가 목표
│
├── src/
│   ├── core/                  Qt-free 코어 — 픽셀 포맷, I/O, 유틸
│   ├── bitstream/             A) 비트스트림 분석 확장 (common/av1/avc/hevc)
│   ├── yuv/
│   │   ├── metrics/           B) PSNR / SSIM / VMAF
│   │   └── scopes/            B) 히스토그램 / 웨이브폼 / 벡터스코프
│   ├── generator/patterns/    C) YUV 테스트 패턴
│   ├── filter/kernels/        D) 이미지 필터
│   ├── integration/           upstream과의 유일한 접점 (어댑터 + 등록)
│   └── app/                   main(), UI 확장
│
├── plugins/
│   ├── sdk/include/bda/plugin/  Qt-free 플러그인 인터페이스 (IFilter/IPattern/IMetric)
│   └── examples/
│
├── tools/cli/                 헤드리스 CLI (배치 분석, 리그레션)
├── tests/{unit,data,golden}/
├── cmake/                     toolchain, upstream 빌드 정의, deploy 헬퍼
├── packaging/{common,rpm}/          런처·desktop 파일, RPM spec
│   └── {appimage,flatpak,windows}/  (미사용)
├── scripts/                   setup-toolchain.sh, build.sh, package.sh
└── assets/icons/
```

### 설계 규칙

1. **`third_party/yuview/upstream/`은 절대 직접 수정하지 않는다.** 모든 변경은 `patches/`로.
2. **우리 코드는 `src/` 아래에만.** upstream 트리에 파일을 섞지 않는다.
3. **`src/`는 upstream 타입에 직접 의존하지 않는다.** 경계는 `src/integration/`. 이 규칙이 나중에 "부분 vendor"로 이행할 여지를 남긴다.
4. **필터/패턴/메트릭은 `plugins/sdk`의 Qt-free 인터페이스로 작성.** 그래야 GUI와 CLI에서 같은 코드를 쓴다.

## 빠른 시작

```bash
./scripts/setup-toolchain.sh     # gcc-toolset-13 + Qt 6.5.3 (aqtinstall)
./scripts/build.sh               # out-of-tree 빌드
./scripts/package.sh             # 배포 아티팩트 (rpm + tar.gz)
```

전체 절차와 제약은 [docs/ai/00-context/constraints.md](docs/ai/00-context/constraints.md) 및
[ADR-0002](docs/ai/20-decisions/ADR-0002-build-and-deploy.md) 참조.

## 다른 머신에 설치

`scripts/package.sh`가 `build/dist/`에 두 가지 산출물을 만든다. Qt·FFmpeg·dav1d 디코더가
모두 번들되어 있어 대상 머신에 Qt를 설치하지 않아도 된다.

### RPM (권장)

```bash
sudo dnf install ./bd-analyzer-<version>.el8.x86_64.rpm
bd-analyzer                  # 빈 상태로 시작
bd-analyzer stream.ivf       # 파일 열기
```

`dnf`가 대상 머신에 없는 시스템 라이브러리(`libglvnd-glx`, `mesa-dri-drivers`,
`xkeyboard-config`, 폰트 등)를 함께 설치한다. 폴더 복사 방식에서 자주 났던
"Could not load the Qt platform plugin xcb" 류의 실패가 이 단계에서 사라진다.
`/opt/bd-analyzer/`에 설치되고 애플리케이션 메뉴에도 등록된다. 제거는 `sudo dnf remove bd-analyzer`.

### tar.gz (root 권한이 없는 머신)

```bash
tar xzf bd-analyzer-<version>-linux-x86_64.tar.gz
cd bd-analyzer-<version>
./check-deps.sh              # 부족한 것이 있는지 먼저 확인
./bd-analyzer stream.ivf
```

의존성을 자동으로 채워 주지 못하므로 `check-deps.sh`가 무언가를 보고하면 관리자에게
패키지를 요청해야 한다.

**대상은 Rocky/RHEL 8 (x86_64) 뿐이다.** 번들된 `libQt6Core`가 `GLIBC_2.28`을 요구하므로
CentOS 7.x 에서는 RPM 설치가 자동으로 거부된다.

자세한 안내(문제 해결, 캐시 위치, `YUView`를 직접 실행하면 안 되는 이유)는
[docs/deploy/package-README.md](docs/deploy/package-README.md) — 이 파일이 패키지 안에
`README.md`로 동봉된다. 패키징 구현 기록은
[TASK-0008](docs/ai/40-tasks/TASK-0008-installable-package.md).

## 라이선스

GPL-3.0-or-later (upstream YUView 상속). 자세한 분석은
[docs/ai/10-research/yuview-build-license.md](docs/ai/10-research/yuview-build-license.md) §1.
