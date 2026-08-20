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
├── packaging/{appimage,flatpak,rpm,windows}/
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
다른 머신에 설치하는 방법은 [docs/deploy/package-README.md](docs/deploy/package-README.md).

## 라이선스

GPL-3.0-or-later (upstream YUView 상속). 자세한 분석은
[docs/ai/10-research/yuview-build-license.md](docs/ai/10-research/yuview-build-license.md) §1.
