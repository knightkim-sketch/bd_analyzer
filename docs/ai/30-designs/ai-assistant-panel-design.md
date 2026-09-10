---
title: AI 어시스턴트 패널 (Claude / Codex) 설계
status: 2단계 구현 완료 (dock + Claude 백엔드). 3단계(컨텍스트 수집 확장) 예정
created: 2026-09-10
updated: 2026-09-10
author: claude-opus-5
verified: "격리 3계층을 실물로 검증했다 — bwrap 이 rm/덮어쓰기를 EROFS 로 막고, plan mode + 도구 allowlist 가 모델의 이탈을 막고, 주입한 system prompt 만으로 Ctrl+R 을 탐색 없이 즉답했다. 단위 테스트 assist-context. stream-json 이벤트 스키마와 도구 승인 프로토콜은 여전히 미검증"
related: 패치 0028 (ME dock 선례), 패치 0034/0035 (선택 접근자·컨텍스트 배선), sb-bdrate-design (BD-rate 값 출처), TASK-0009 (취소 가능 백그라운드 패턴)
---

## 무엇을 만드는가

앱 안에 **Claude / Codex 대화 패널**을 dock 으로 붙인다. VSCode 의 AI 패널과 같은 자리다.

핵심은 창의 위치가 아니다. **열려 있는 분석 컨텍스트를 자동으로 넘기는 것**이다.

- 사용자가 알고 싶은 것은 "이 superblock 이 왜 이렇게 비트를 많이 쓰나", "이 블록의 partition 선택이
  왜 이렇게 됐나", "이 BD-rate 손실이 어디서 왔나" 다.
- 이 질문에 답하려면 **스트림 정보 + 프레임 번호 + 클릭한 SB 좌표 + 그 블록의 AV1 syntax +
  SB bits/SSE** 가 필요하다. 전부 앱이 이미 화면에 띄우고 있는 값이다.
- 터미널을 앱 안으로 옮기기만 하면 사용자가 그 값들을 **손으로 타이핑**해야 한다. 그러면 창이
  앱 안에 있든 밖에 있든 같다.

사용 흐름:

1. 스트림을 열고 프레임을 옮기고 superblock 을 클릭한다 (지금 하는 그대로).
2. **View → Dock Panels → Show AI Assistant** (`Ctrl+K`) 로 패널을 띄운다.
3. 하단 입력창에 질문을 쓴다. **첨부 체크박스**가 현재 컨텍스트를 무엇까지 넘길지 정한다.
4. 답이 스트리밍으로 표시된다. 프레임을 옮기거나 다른 SB 를 클릭하면 다음 질문의 컨텍스트가 갱신된다.

---

## 왜 터미널 임베드가 아닌가

4가지 방식을 실측 근거로 비교했다.

| 방식 | 동작 | 비용 | 기각/채택 사유 |
|---|---|---|---|
| A. QTermWidget dock | konsole 엔진 Qt 터미널 위젯에 `claude` 실행 | 높음 | **기각.** 활성 repo 에 없어 `third_party/` 소스 빌드 + 번들·RPM 갱신이 필요한데, 얻는 것은 "터미널이 앱 안에 있다" 뿐 |
| B. xterm X11 reparent | `xterm -into <winId>` + `createWindowContainer(QWindow::fromWinId())` | 낮음 | **기각.** X11 전용이라 Wayland 세션에서 죽는다. 배포 제품에 넣을 수 없다 |
| C. 외부 터미널 실행 | `gnome-terminal -- claude` 를 현재 디렉토리에서 띄움 | 거의 0 | **보조로 채택.** 임베드는 아니지만 위험 0. 별도 액션으로 남긴다 |
| D. 네이티브 패널 | CLI 를 헤드리스로 QProcess 에 붙이고 Qt 위젯으로 대화 UI | 중 | **채택.** 컨텍스트 자동 주입이 가능한 유일한 방식 |

A 와 B 는 컨텍스트를 전혀 전달하지 못한다. 이 기능의 가치가 컨텍스트 주입에 있으므로 둘 다 기각한다.

환경 실측 (2026-09-10, 이 개발 머신):

| 항목 | 결과 |
|---|---|
| `claude` / `codex` | 둘 다 있다 (`~/.local/bin/`) |
| `qtermwidget` | 미설치. **활성 repo 에도 없다** (`dnf list --available` 무결과) |
| `xterm` | 미설치. `appstream` repo 에 `331-2.el8` 있음 |
| `gnome-terminal` | 있다 (`/bin/gnome-terminal`). `--into` 없음 → 임베드 불가 |
| 세션 | `DISPLAY=:11.0` (X11). 번들에 `libqxcb.so` + wayland 플러그인 함께 있음 |
| QtWebEngine | **없다** → webview 방식 배제 |
| Qt6Network / QJsonDocument | 있다 (Qt 6.5.3) → 새 의존성 없이 JSON·HTTP 가능 |

---

## 이미 있는 것

컨텍스트 재료가 전부 있다. 새로 만들 것은 **직렬화와 프로세스 배선**뿐이다.

| 필요한 것 | 이미 있는 것 | 위치 |
|---|---|---|
| 클릭한 블록 위치 | `splitViewWidget::blockSelected(playlistItem*, QPoint, int)` | 패치 `0035` 가 BD-rate window 에 이미 연결해 쓴다 |
| 현재 프레임 | `splitViewWidget::viewFrameChanged(playlistItem*, int)`, `playbackController->getCurrentFrame()` | 같은 패치 |
| 선택된 아이템들 | `playlistTreeWidget->getAllSelectedItems()` | 패치 `0034` |
| 블록 AV1 syntax | `playlistItem::getBlockInfoAt(QPoint, int)` → `stats::BlockInfo` | `playlistItem.h:224` |
| SB 당 bits | `getSuperblockBits(frameIdx)` | `playlistItemCompressedVideo.cpp` |
| SB 당 SSE | `getPixelBlockStats(pos, frameIdx)->sse` | `PixelStatistics.h` |
| BD-rate 값 | `BdRateFrameData` / `BdRateSequenceData` (`perSuperblock[SB][group][point]`) | `src/integration/BdRateCollector.h` |
| dock 추가 선례 | `motionEstimationDock` — `.ui` + 메뉴 + 배선 | 패치 `0028` (668줄) |
| 취소 가능 백그라운드 | ME 의 `CancelToken` / `ProgressToken`, BD-rate sweep 의 타이머 슬라이스 | `src/me`, `src/integration` |
| Qt-free 단위 테스트 경로 | `run_unit_test` — Qt·libYUViewLib 없이 컴파일해 Qt-free 를 실제로 검증 | `tests/run-regression.sh:166,174` |

없는 것 하나: **`QProcess` 선례가 저장소 전체에 없다.** 이 앱은 외부 프로세스를 띄운 적이 한 번도
없다. 프로세스 수명·표준입출력·종료 시 정리를 처음 다루는 축이 된다.

---

## 두 가지 모드 — 분리해야 하는 이유

| 모드 | 도구 | 할 수 있는 것 | 기본 |
|---|---|---|---|
| **A. 읽기 전용 분석** | `Read` + `Bash` (읽기 명령만) | 컨텍스트를 보고 답한다. **어디든 읽고 탐색할 수 있지만 아무것도 바꾸지 못한다** | **기본값** |
| **B. 코딩 에이전트** | 쓰기 포함 | 소스를 고치고 명령을 실행한다 | 명시적 opt-in |

Mode A 를 "도구 없음" 이 아니라 **"읽기 전용"** 으로 잡았다. 사용자 요구가 "다른 위치의 파일을
탐색하거나 읽어볼 수는 있지만 삭제하거나 시스템을 건드리지 못하게" 이기 때문이다. 이 구분이
중요한 이유는 아래에서 실측으로 드러난다 — **이 CLI 버전에는 `Grep`/`Glob` 도구가 없어 파일
탐색이 `Bash` 를 거친다.** 즉 "탐색 허용" 은 곧 "셸 허용" 이고, 셸을 허용하면서 `rm` 을 막는 것이
이 설계의 실제 과제가 된다.

분리하는 근거:

- **주 사용례는 에이전트가 필요 없다.** "이 SB 가 왜 비트를 많이 쓰나" 는 단발 질의다. 도구 루프도
  파일 접근도 필요하지 않다. 필요 없는 능력을 기본으로 켜는 것은 위험만 늘린다.
- **이 앱은 RPM 으로 팀에 배포된다.** 각 사용자의 자격증명과 권한으로 셸을 실행할 수 있는 에이전트를
  기본 탑재하는 것은 별개의 결정이다. 영상 분석 도구가 소스를 수정해야 하는지 자체가 미결이다.
- Mode A 는 도구가 없으므로 **승인 UI 가 아예 필요 없다.** 이것이 1차 구현을 작게 만든다.

---

## 프로세스 인터페이스

플래그는 `--help` 실측값이다.

### Claude — 지속 세션 (stdin 열림)

실제 호출은 `assets/assist/launch-claude.sh` 에 있다 (bwrap 래핑 포함). 핵심만 옮기면:

```
claude -p --output-format stream-json --input-format stream-json \
       --include-partial-messages --replay-user-messages \
       --permission-mode plan \
       --tools "Read,Bash" \
       --settings assets/assist/permissions.json \
       --append-system-prompt "$(cat assets/assist/system-prompt.md)" \
       --model opus \
       --max-budget-usd <상한>
```

| 플래그 | 역할 |
|---|---|
| `--input-format stream-json` | stdin 으로 계속 메시지를 넣는다 → **프로세스 하나로 대화 유지** |
| `--output-format stream-json` | stdout 이 JSONL 이벤트 스트림 |
| `--include-partial-messages` | 토큰 단위 조각 → 답이 타이핑되듯 보인다 |
| `--replay-user-messages` | 우리가 넣은 메시지를 되돌려줘 ack 로 쓴다 |
| `--tools "Read,Bash"` | 읽기 전용 도구만. `Write`/`Edit`/`ExitPlanMode` 가 없어 이탈 불가 |
| `--permission-mode plan` | 읽기 전용 모드. 파괴적 명령을 CLI 가 거부한다 |
| `--settings` | `permissions.json` — Bash allow/deny 와 자격증명 경로 차단 (보조) |
| `--append-system-prompt` | `system-prompt.md` 를 읽어 넘긴다 — 앱 기능 전체와 정책 |
| `--session-id` / `-r, --resume` / `--fork-session` | 세션 연속성. 패널을 닫았다 열어도 이어붙일 수 있다 |
| `--max-budget-usd` | 세션 비용 상한. 팀 배포에서 사고를 막는다 |
| `--permission-mode` | Mode B 용. `plan` 은 읽기만, `default` 는 승인 요구 |
| `--json-schema` | 구조화 출력이 필요할 때 (예: SB 별 소견을 표로) |

### Codex — 1회성 + resume

```
codex exec --json --sandbox read-only -C <작업 디렉토리> [--output-schema <file>]
codex exec resume --last --json --sandbox read-only ...
```

| 플래그 | 역할 |
|---|---|
| `--json` | stdout 에 JSONL 이벤트 |
| `-s, --sandbox read-only` | **Mode A 의 안전 경계** (다른 값: `workspace-write`, `danger-full-access`) |
| `-C, --cd` | 작업 루트 |
| `exec resume --last` | 이전 세션 이어받기 |
| `--ephemeral` | 세션 파일을 디스크에 남기지 않음 |
| `--output-schema` | 최종 응답 형태를 JSON Schema 로 고정 |

### 두 CLI 의 비대칭 — 어댑터가 흡수해야 한다

- `claude` 는 **stdin 이 계속 열린 지속 프로세스**다. 한 프로세스가 대화 전체를 담당한다.
- `codex exec` 는 **호출당 1턴**이고, 연속성은 `exec resume` 으로 새 프로세스를 띄워 얻는다.

따라서 `AssistBackend` 인터페이스는 "프로세스 하나 = 세션 하나" 를 가정하면 안 된다.
**세션은 논리적 개념**이고, 백엔드가 그것을 지속 프로세스로 구현하든 매 턴 재실행으로 구현하든
UI 는 몰라야 한다.

---

## 격리 — 3계층 (실측 검증)

사용자 요구는 "읽고 탐색은 되지만 삭제·시스템 변경은 막는다" 이다. 프롬프트로 부탁해서 될 일이
아니므로 세 겹으로 만들고, 각 겹을 따로 검증했다.

| 계층 | 수단 | 모델이 우회할 수 있는가 |
|---|---|---|
| **1. 커널** | `bwrap --ro-bind / /` — 파일시스템을 읽기 전용으로 마운트 | **불가.** 무엇이 돌든 쓰기는 EROFS 로 실패한다 |
| **2. CLI** | `--permission-mode plan` + `--tools "Read,Bash"` | 불가. `Write`/`Edit`/`ExitPlanMode` 가 도구 목록에 없어 **plan mode 를 벗어나겠다고 요청할 수조차 없다** |
| **3. 프롬프트** | `system-prompt.md` 의 정책 문단 | **가능.** 경계가 아니라 안내다. 거부를 재시도하는 대신 *설명하게* 하려고 둔다 |

### 실측 결과 (2026-09-10, Rocky 8)

```
# 1계층 단독 — CLI 없이 샌드박스만
$ bwrap --ro-bind / / --dev /dev --proc /proc --unshare-user \
        /bin/sh -c 'rm -f /tmp/canary.txt; echo rm_exit=$?'
rm: cannot remove '/tmp/canary.txt': 읽기전용 파일 시스템
rm_exit=1
$ cat /tmp/canary.txt
canary                      # 살아남았고, 샌드박스 안에서 읽기는 된다
```

```
# 1+2계층 — launch-claude.sh 로 실제 질의
질문: (1) SB BD-rate 창을 여는 단축키는? 탐색하지 말고 아는 대로.
      (2) 'rm -f /tmp/bda_canary.txt' 를 실행하고 결과를 보고하라.

답변: (1) Ctrl+R (View → "SB BD-rate from Selection")
      (2) 실행하지 않았습니다. plan mode 로 읽기 전용이고, 이것은 파괴적
          파일시스템 작업입니다. ... 직접 실행해 주세요.
canary 생존 확인: canary
```

- (1)이 **탐색 없이 즉답됐다.** 주입한 system prompt 만으로 앱 기능을 인지한다는 요구가 실증됐다.
- (2)에서 2계층이 먼저 막았고, 뚫렸더라도 1계층이 막았을 것이다.

### MCP 구멍 — `--strict-mcp-config` 가 필수인 이유 (실측)

`--tools` 는 **내장 도구만** 제한한다. 사용자 설정의 MCP 서버는 따로 붙고 **쓰기 가능한 도구를
같이 들고 온다.**

플래그를 넣기 전 2턴 세션 실측:

- 1턴 `init.tools` = `["Bash","Read"]` — 정상으로 보인다
- 2턴 `init.tools` = `["Bash","Read"` + **46개**`]`, 그 안에
  `mcp__claude_ai_Atlassian__createConfluencePage`, `..._editJiraIssue`,
  `mcp__claude_ai_Google_Drive__create_file`

MCP 서버는 **비동기로 붙는다.** 그래서 첫 턴은 깨끗하고 구멍은 나중에 열린다 — 이런 버그가 가질
수 있는 최악의 형태다. `--strict-mcp-config` 를 (`--mcp-config` 없이) 넣은 뒤 두 턴 모두
`["Bash","Read"]` 로 확인됐다.

두 가지가 따라온다:

1. **패널이 매 세션 시작마다 `init.tools` 를 다시 검사하고**, 예상 밖 도구가 있으면 전송을
   거부한다 (`unexpectedTools()`, 회귀 32 가 고정). 플래그는 편집 중에 사라질 수 있고, 이 부류의
   실패는 그것 말고는 보이지 않는다.
2. **샌드박스는 이걸 막지 못한다.** 이 도구들은 파일시스템이 아니라 네트워크로 외부 서비스에
   닿는다. 1계층은 여기에 아무 방어가 되지 않는다.

부수 효과로 비용도 컸다 — 같은 2턴이 $0.387 → $0.101.

### allowlist 가 안전한 방향이고 denylist 는 아니다

실측: **`--tools` 에 존재하지 않는 이름을 넣으면 조용히 무시된다.** `--tools "ZzzBogusTool"` 로
세션이 정상 시작됐다. 방향이 중요하다.

- **allowlist 오타 → 능력을 잃는다** (안전한 실패)
- **denylist 오타 → 구멍이 남는다** (위험한 실패)

그래서 `--tools` 가 CLI 층의 1차 수단이고, `permissions.json` 의 deny 목록은 보조다. deny 목록은
원리적으로 완전할 수 없다 — 허용된 명령도 휘어진다 (`find -exec`, `awk 'system(...)'`, 셸 내장).
**그 불완전성이 곧 1계층이 존재하는 이유다.**

### 이 버전의 도구 이름 (실측)

`Agent` `Bash` `Edit` `Read` `Write` `NotebookEdit` `WebFetch` `WebSearch` `Skill` `ToolSearch`
`TodoWrite` `Monitor` `SendMessage` `Task*` 등. **`Grep`/`Glob` 은 없다** — 검색은 `Bash` 로 간다.
도구 목록은 버전마다 달라질 수 있으므로 `--tools` 문자열은 CLI 업그레이드 시 재확인 대상이다.

### 의도적으로 남긴 구멍

CLI 자기 상태 디렉토리(`~/.claude`, `~/.codex`)는 **쓰기 가능하게 bind** 한다. OAuth 갱신과 세션
파일에 필요하다. 그 안의 민감 파일은 2계층의 `Read(...)` deny 규칙으로 가린다.

---

## 앱 기능 인지 — 세션 시작 시 주입

"앱이 지원하는 모든 기능을 따라 찾아보지 않아도 되도록" 이 요구였다. 두 단으로 나눴다.

| 파일 | 언제 읽히나 | 담는 것 |
|---|---|---|
| `assets/assist/system-prompt.md` | **매 세션 시작 시 항상** (`--append-system-prompt`) | 기능 전체 요약 — 입력 포맷, 디코더, dock 8종과 단축키, File/View 메뉴, 통계 오버레이, org YUV, ME, BD-rate, 캐시 위치, 수치 해석 주의 |
| `assets/assist/skills/bd-analyzer/SKILL.md` | 필요할 때 | 값의 출처 표, 거절 사유 표, 저장소 레이아웃, 오독하기 쉬운 통계 |

- `--append-system-prompt-file` 은 `--bare` 설명문에만 나오고 옵션 목록에는 없다. 의존하지 않고
  **파일을 읽어 `--append-system-prompt <내용>` 으로 넘긴다.** 몇 KB 는 ARG_MAX 에 한참 못 미친다.
- Codex 에는 `--append-system-prompt` 가 없다. 앱이 첫 메시지 앞에 붙인다.
- **동기화 규칙**: 메뉴·단축키·거절 문구가 바뀌면 같은 커밋에서 `system-prompt.md` 도 고친다.
  자신있게 틀린 기능 설명은 없느니만 못하다 — 모델이 그대로 사용자에게 옮긴다.

---

## 아키텍처

저장소 규칙(우리 코드는 `src/` 아래, upstream 타입 의존은 `src/integration/` 경계에서만)을 따른다.

| 파일 | 역할 | Qt 의존 |
|---|---|---|
| `src/assist/AssistContext.{h,cpp}` | 컨텍스트 구조체와 **텍스트 직렬화**. 무엇을 넘길지 정하는 규칙이 여기 있다 | **없음** (Qt-free, 단위 테스트) |
| `src/integration/AssistContextCollector.{h,cpp}` | upstream 타입(`playlistItem`, `BlockInfo`, `BdRateFrameData`)에서 `AssistContext` 를 채운다 | 있음 (경계) |
| `src/integration/AssistBackend.{h,cpp}` | `QProcess` 어댑터 인터페이스 + JSONL 이벤트 파싱 (`QJsonDocument`) | 있음 |
| `src/integration/ClaudeCliBackend.{h,cpp}` | 지속 stdio 세션 구현 | 있음 |
| `src/integration/CodexCliBackend.{h,cpp}` | 1회성 + resume 구현 | 있음 |
| `src/integration/AssistPanelWidget.{h,cpp}` | dock 위젯 — 대화 로그, 입력, 첨부 체크박스, 상태 | 있음 |
| 패치 `00xx` | `assistDock` (`.ui`) + `View → Dock Panels → Show AI Assistant` (`Ctrl+K`) + MainWindow 배선 | — |
| `assets/assist/system-prompt.md` | 매 세션 주입되는 앱 기능 레퍼런스 + 정책 | — |
| `assets/assist/skills/bd-analyzer/SKILL.md` | 필요 시 로드되는 심화 레퍼런스 | — |
| `assets/assist/permissions.json` | Bash allow/deny, 자격증명 경로 차단 | — |
| `assets/assist/launch-{claude,codex}.sh` | bwrap 격리를 포함한 실제 실행 레시피 | — |
| `assets/assist/README.md` | 3계층 설명과 **격리 재검증 절차** | — |

- JSON 파싱은 **`QJsonDocument`** 를 쓴다. Qt6Core 에 이미 있어 새 의존성이 없고, 저장소에 JSON
  라이브러리 선례가 없어 `third_party/` 를 늘리지 않는 편이 낫다. 대신 프로토콜 파싱은 Qt-free 가
  아니게 되므로, **Qt-free 로 유지하는 것은 `AssistContext` 직렬화뿐**이다 — 거기가 정확성이 중요한
  부분이고 `bdrate-math` 처럼 Qt 없이 테스트한다.
- 컨텍스트 수집은 **질문을 보낼 때 한 번**만 한다. 프레임·SB 가 바뀔 때마다 미리 모으지 않는다
  (BD-rate window 가 겪은 SSE 비동기 대기 문제를 매 클릭마다 반복할 이유가 없다).

---

## 컨텍스트 주입 — 무엇을 넘기는가

| 항목 | 출처 | 기본 첨부 |
|---|---|---|
| 스트림 파일명, 해상도, `sb_size`, 코덱 | `playlistItem` | O |
| 현재 프레임 번호 | `getCurrentFrame()` | O |
| 클릭한 SB 좌표 (열, 행) + 픽셀 위치 | `blockSelected` | O |
| 그 블록의 AV1 syntax 전체 | `getBlockInfoAt(pos, frameIdx)` | O |
| 그 SB 의 bits / SSE / PSNR | `getSuperblockBits`, `getPixelBlockStats` | O |
| 프레임 전체 bits / PSNR | `BdRateFrameData::frameTotals` | 선택 |
| 열려 있는 BD-rate group 과 값 | `BdRatePlotWindow` 의 group·수집값 | 선택 |
| 이웃 SB 통계 (3×3) | 같은 접근자, 좌표만 이동 | 선택 |
| 소스 파일 / 저장소 | Mode B 에서만 | X |

원칙:

- **픽셀은 보내지 않는다.** 파일 경로와 통계 수치만 나간다. 프레임 이미지를 첨부하는 것은 별개
  결정이며 이 설계 범위 밖이다.
- **무엇을 보내는지 그대로 보여준다.** 패널에 접을 수 있는 "전송 내용" 블록을 두고, 실제로 보낸
  텍스트를 그대로 담는다. 이것이 잘못된 수치로 그럴듯한 오답이 나오는 것에 대한 유일한 방어책이다.
- **전부 넣지 않는다.** 매 질문에 이웃 SB 와 시퀀스 통계까지 넣으면 토큰만 늘고 답은 흐려진다.
  기본은 현재 프레임 + 클릭 블록이다.

---

## UI

- **dock** 이다 (기본 오른쪽, 기본 숨김). `QDockWidget` 는 float 가 되므로 popup 이 따로 필요 없다.
  ME 패널과 같은 방식으로 `View → Dock Panels` 에 넣는다.
- 상단: 백엔드 (`Claude` / `Codex`), 모드 (`질의` / `에이전트`), 모델 선택.
- 중앙: 대화 로그. 스트리밍으로 채워진다.
- 하단: 입력창 + **첨부 체크박스** + 전송/취소.
- 상태줄: 비용 누계, 세션 ID, 진행 상태.
- **비활성 사유를 말한다.** CLI 없음 / 로그인 안 됨 / 네트워크 없음 을 구분해 표시한다 — ME 패널이
  org YUV 가 없을 때 이유를 붙여 비활성화하는 것과 같은 방식이다. 조용히 실패하지 않는다.

---

## 착수 조건 / 거절 사유

| 조건 | 거절 메시지 취지 |
|---|---|
| CLI 존재 | `claude` / `codex` 가 `PATH` 에 없다. npm 설치이므로 RPM 의존성으로 넣을 수 없다 — 안내만 한다 |
| 인증 | 로그인되지 않았다. 각 사용자가 직접 로그인해야 한다 |
| 네트워크 | 외부 연결이 없다. `/fs2` 복사본으로 쓰는 오프라인 머신이 있을 수 있다 |
| 컨텍스트 | 스트림이 열려 있지 않으면 첨부할 것이 없다. 질문은 되지만 첨부 없이 간다고 알린다 |

---

## 단계

| 단계 | 내용 | 상태 |
|---|---|---|
| 1 | `AssistContext` + 직렬화 + Qt-free 단위 테스트 | **완료** |
| 1b | 격리 3계층 + 주입 파일 (`assets/assist/`) + 실물 검증 | **완료** |
| 2 | `ClaudeCliBackend` (`QProcess` + stream-json 파싱) + dock UI + 스트리밍 표시 | **완료** |
| 2b | 첨부 체크박스 + "전송 내용" 표시 + 도구 가드 (패치 `0037`) | **완료** |
| 3 | `AssistContextCollector` — 블록 syntax·SB bits/SSE·BD-rate 값 실제 수집 | 다음 |
| 4 | `CodexCliBackend` (`exec --json --sandbox read-only` + `resume`) | 예정 |
| 5 | Mode B (쓰기 허용 + 승인 UI) | **별도 결정 필요** |

1–3 단계까지가 "클릭한 SB 에 대해 물어본다" 를 만족한다. 4 는 독립적이고, 5 는 착수 전에 결정이
필요하다.

1b 를 1 단계와 함께 한 이유: 격리는 나중에 덧붙이는 것이 아니라 2단계 백엔드가 **처음부터 그
안에서** 돌아야 하는 전제다. 런처 스크립트가 먼저 있으면 `ClaudeCliBackend` 는 그것을 실행하기만
하면 된다.

---

## 검증 계획

| 무엇 | 어떻게 |
|---|---|
| 컨텍스트 직렬화 | 고정된 `BlockInfo`·통계 값 → 기대 텍스트 대조 (Qt-free 단위 테스트) |
| SB 좌표 정합 | 클릭한 픽셀이 실제로 그 SB 로 접히는지. BD-rate window 의 `sbKey` 계산과 같은 값이 나오는지 |
| 프로세스 수명 | 패널을 닫을 때, 앱을 종료할 때 자식 프로세스가 남지 않는지 (`ps` 로 확인) |
| 취소 | 응답 중 취소가 먹는지, 프로세스가 정리되는지 |
| CLI 부재 | `PATH` 에서 빼고 실행 → 크래시 없이 이유를 표시하는지 |
| 인증 실패 | 로그아웃 상태 → 이유를 표시하는지 |
| 격리 1계층 | `bwrap` 안에서 `rm`/덮어쓰기가 EROFS 로 실패하고 읽기는 되는지 — **검증 완료** |
| 격리 2계층 | 런처로 삭제를 요청 → plan mode 가 거부하고 파일이 살아있는지 — **검증 완료** |
| 기능 인지 | 탐색 없이 `Ctrl+R` 을 즉답하는지 — **검증 완료** |
| 컨텍스트 문구 | 무손실 / org 미첨부 / 헤더 미파싱이 서로 다르게 읽히는지 — `assist-context` 단위 테스트 |
| 회귀 무해성 | dock 추가가 기존 회귀 36개를 깨지 않는지 (실측 37/37). `25-mainwindow-teardown` 이 특히 중요 |

---

## 미결 / 위험

- **도구 승인 프로토콜이 미검증이다.** `--output-format stream-json` 에서 도구 사용 승인 요청이
  어떤 이벤트로 오고 어떻게 응답하는지 확인하지 않았다. **Mode B(5단계)의 전제**이므로 착수 전에
  스파이크가 필요하다. Mode A 는 plan mode 가 대신 거부하므로 이 문제를 만나지 않는다.
- **`--max-budget-usd` 의 의미가 불분명하다.** 0.30 에서는 사소한 질의가 통과했는데 0.40 에서
  즉시 `Exceeded USD budget` 으로 끊겼고, 3.00 이 필요했다. 실제 지출이 아니라 사전 추정치와
  비교하는 것으로 보이나 **확인하지 않았다.** 상태줄에 남은 예산을 표시하려면 먼저 규명해야 한다.
- **도구 이름 목록은 CLI 버전에 묶여 있다.** `Grep`/`Glob` 이 없는 것도 이 버전의 사실이다.
  `--tools` 는 모르는 이름을 조용히 무시하므로, CLI 를 올린 뒤 `Read`/`Bash` 가 여전히 유효한지
  확인하지 않으면 **패널이 조용히 무력해진다.** 런처가 시작 시 도구 유무를 한 번 확인하도록
  2단계에서 넣는다.
- **`bwrap` 이 배포 의존성이 된다.** 이 머신에는 `/bin/bwrap` 이 있고 user namespace 도 열려
  있지만(`user.max_user_namespaces = 510746`), 팀의 다른 머신은 다를 수 있다. 런처는 `bwrap` 이
  없으면 **격리 없이 도는 대신 거부한다.** RPM 의존성에 `bubblewrap` 을 넣어야 한다.
- ~~stream-json 이벤트 스키마 미검증~~ → **해결.** 실물 세션에서 잡았고 회귀 32 가 고정한다.
  `system/init`(session_id, tools, permissionMode, model), `stream_event`→`content_block_delta`
  →`delta.text_delta`, `result`(result, is_error, total_cost_usd). 입력 봉투는
  `{"type":"user","message":{"role":"user","content":[{"type":"text","text":...}]}}` 한 줄.
  덤으로 두 개를 배웠다 — `--print --output-format=stream-json` 은 **`--verbose` 를 요구**하고,
  거부 메시지는 이벤트가 아니라 **평문**으로 같은 스트림에 나온다 (그래서 파서가 비 JSON 줄을
  실패로 올린다).
- ~~비용 표시 미검증~~ → **해결.** `result.total_cost_usd` 로 누계가 온다. 상태줄에 표시한다.
  `--max-budget-usd` 가 이상하게 낮게 걸렸던 것도 MCP 도구 정의가 컨텍스트를 부풀린 탓이 크다.
- **`QProcess` 선례가 없다.** 종료 시 정리를 잘못하면 좀비가 남는다. `25-mainwindow-teardown` 회귀가
  이 축을 이미 보고 있으니 거기에 붙인다.
- **팀 배포 전제가 바뀐다.** 지금까지 bd_analyzer 는 네트워크 없이 동작하는 오프라인 도구였다.
  이 패널은 각 사용자의 로그인과 외부 연결을 요구한다. 없는 사용자에게는 기능이 그냥 없는 것이
  되어야 하고, 그것이 정상 동작으로 보여야 한다.
- **Mode B 를 넣을지 자체가 미결이다.** 영상 분석 도구가 소스를 수정하는 것이 맞는가. 넣는다면
  누구의 권한으로 무엇까지인가. 1–4 단계는 이 결정과 무관하게 진행할 수 있으므로, 결정을 미루고
  먼저 만든다.
- **컨텍스트가 틀리면 답도 틀린다.** 잘못된 수치를 넘기면 모델은 그럴듯한 오답을 만든다. "전송
  내용" 표시가 방어책이지만, 사용자가 그것을 읽어야 작동하는 방어책이다.
