# YouTube 트랜스코딩 가이드 (Rocky Linux 8)

yt-dlp로 스트림을 추출하고 FFmpeg로 재인코딩하는 방법과, 이를 자동화한 `yt-transcode.sh` 사용법을 정리한 문서입니다.

---

## 목차

1. [빠른 시작](#1-빠른-시작)
2. [동작 원리](#2-동작-원리)
3. [설치](#3-설치-rocky-linux-8)
4. [스크립트 사용법](#4-스크립트-사용법)
5. [FFmpeg 직접 사용](#5-ffmpeg-직접-사용)
6. [성능 튜닝](#6-성능-튜닝)
7. [문제 해결](#7-문제-해결)
8. [부록](#8-부록)

---

## 1. 빠른 시작

```bash
# 설치 (root 권한 불필요)
mkdir -p ~/.local/bin && export PATH="$HOME/.local/bin:$PATH"
curl -L https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux \
     -o ~/.local/bin/yt-dlp && chmod +x ~/.local/bin/yt-dlp
curl -L https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz \
     -o /tmp/ff.tar.xz && tar -xf /tmp/ff.tar.xz -C /tmp
cp /tmp/ffmpeg-*-static/ffmpeg /tmp/ffmpeg-*-static/ffprobe ~/.local/bin/

# 실행
chmod +x yt-transcode.sh
./yt-transcode.sh dQw4w9WgXcQ
```

세 가지 대표 사용 패턴:

```bash
# 단일 영상
./yt-transcode.sh 'https://youtu.be/XXXXXXXX' -H 720

# 목록 파일 일괄 처리
./yt-transcode.sh -l links.txt -d /data/encoded -s

# 재생목록 전체를 4개 병렬로
./yt-transcode.sh 'https://www.youtube.com/playlist?list=PLxxxx' --playlist -s -j 4
```

---

## 2. 동작 원리

### 2.1 전체 흐름

```
YouTube 링크
    │
    │  (1) yt-dlp -J
    │      메타데이터 + 스트림 URL + HTTP 헤더를 JSON으로 추출
    ▼
video URL (itag 137 등)  +  audio URL (itag 251 등)
    │
    │  (2) ffmpeg
    │      두 입력을 map으로 결합하여 재인코딩
    ▼
out.mp4
```

### 2.2 FFmpeg는 YouTube 링크를 직접 읽지 못한다

`https://youtube.com/watch?v=...`는 HTML 페이지입니다. 실제 미디어는 JavaScript 실행과 서명(signature) 복호화를 거쳐야 얻을 수 있는 `googlevideo.com` URL에 있습니다.

- **yt-dlp의 역할** — 페이지 파싱, 플레이어 JS 해석, 서명 복호화, 최종 미디어 URL 산출
- **FFmpeg의 역할** — 그 URL에서 데이터를 읽어 디코딩·인코딩·먹싱

따라서 `ffmpeg -i "https://youtube.com/watch?v=..."`는 동작하지 않습니다.

### 2.3 DASH — 영상과 음성이 분리되어 있다

YouTube는 720p 이상에서 영상과 음성을 별도 스트림으로 제공합니다. 입력이 두 개가 되므로 FFmpeg에서 명시적으로 결합해야 합니다.

| itag | 내용 | 코덱 | 컨테이너 |
|:--|:--|:--|:--|
| 137 | 1080p 영상 전용 | H.264 | MP4 |
| 248 | 1080p 영상 전용 | VP9 | WebM |
| 251 | 음성 전용 | Opus | WebM |
| 140 | 음성 전용 | AAC | M4A |
| 22 | 720p 영상+음성 통합 | H.264 / AAC | MP4 |
| 18 | 360p 영상+음성 통합 | H.264 / AAC | MP4 |

```bash
ffmpeg -i "<video_url>" -i "<audio_url>" \
       -map 0:v:0 -map 1:a:0 \
       -c:v libx264 -crf 23 -c:a aac -b:a 128k \
       out.mp4
```

`-map 0:v:0`은 "첫 번째 입력의 첫 번째 비디오 스트림", `-map 1:a:0`은 "두 번째 입력의 첫 번째 오디오 스트림"을 의미합니다.

포맷 목록은 이렇게 확인합니다:

```bash
yt-dlp --no-playlist -F "<url>"
```

### 2.4 핵심 — HTTP 헤더를 반드시 전달해야 한다

> **이 부분을 놓치면 `HTTP error 403 Forbidden`이 발생합니다.**

yt-dlp는 추출 성공률을 높이기 위해 특정 클라이언트로 위장합니다. 추출된 URL의 `c=` 파라미터에서 확인할 수 있습니다.

```
https://rr3---sn-xxx.googlevideo.com/videoplayback?expire=...&c=ANDROID_VR&sig=AE0s2JY...
                                                              ^^^^^^^^^^^^
```

googlevideo 서버는 **URL에 서명한 클라이언트의 User-Agent와 실제 요청의 User-Agent가 일치하는지 검증**합니다. FFmpeg는 기본적으로 `Lavf/<version>`을 보내므로 서버가 거부합니다.

문제는 `yt-dlp -g`가 URL만 출력하고 헤더는 알려주지 않는다는 점입니다. 그래서 `-J`(JSON)로 받아야 합니다.

```bash
yt-dlp --no-playlist -f 'bv*[height<=1080]+ba/b' -J "<url>"
```

```json
{
  "title": "영상 제목",
  "id": "bGYi7eOoVJ4",
  "requested_formats": [
    {
      "format_id": "137",
      "url": "https://rr3---sn-xxx.googlevideo.com/videoplayback?...",
      "http_headers": {
        "User-Agent": "com.google.android.apps.youtube.vr.oculus/1.62...",
        "Accept-Language": "en-us,en;q=0.5"
      }
    },
    {
      "format_id": "251",
      "url": "...",
      "http_headers": { "User-Agent": "..." }
    }
  ]
}
```

이 헤더를 FFmpeg 입력마다 적용합니다. `-user_agent`와 `-headers`는 **per-input 옵션**이므로 반드시 각 `-i` **앞에** 위치해야 합니다.

```bash
ffmpeg \
  -user_agent "<UA_video>" -headers $'Accept-Language: en-us\r\n' -i "<video_url>" \
  -user_agent "<UA_audio>" -i "<audio_url>" \
  -map 0:v:0 -map 1:a:0 \
  -c:v libx264 -crf 23 -c:a aac -b:a 128k \
  out.mp4
```

`-headers` 값은 각 줄이 `\r\n`으로 끝나야 합니다. Bash에서는 `$'...'` 구문을 사용합니다.

### 2.5 서명 URL의 제약

| 제약 | 내용 |
|:--|:--|
| 유효 기간 | `expire` 파라미터 기준 약 6시간 |
| IP 바인딩 | `ip` 파라미터의 주소에서만 접근 가능 |
| 재사용 | 불가. 매번 새로 추출해야 함 |

URL을 저장해 두었다가 나중에 쓰는 방식은 동작하지 않습니다. 대량 작업 시 중간에 만료될 수 있으므로 `-s`(skip-existing) 옵션과 함께 재실행하는 방식을 권장합니다.

### 2.6 재생 + 트랜스코딩 동시 수행 (tee 먹서)

인코딩을 1회만 수행하고 출력을 파일과 파이프로 분기합니다. 미리보기 화면이 실제 저장 결과물과 동일합니다.

```bash
ffmpeg ... -f tee "[f=mp4:movflags=+faststart]out.mp4|[f=nut]pipe:1" \
  | ffplay -autoexit -i pipe:0
```

- 파이프 구간은 `nut` 또는 `matroska`를 사용합니다. MP4는 moov atom이 파일 끝에 위치하므로 스트리밍이 불가능합니다.
- 파일 측에는 `+faststart`를 적용해 재생 시작이 빠르도록 합니다.
- 헤드리스 서버에는 ffplay가 없으므로 스크립트는 이 기능을 기본 비활성화합니다.

---

## 3. 설치 (Rocky Linux 8)

### 3.1 yt-dlp

Rocky 8의 기본 Python은 3.6이며, 최신 yt-dlp는 Python 3.9 이상을 요구합니다. **의존성이 없는 단독 실행 바이너리를 권장합니다.**

```bash
mkdir -p ~/.local/bin
curl -L https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux \
     -o ~/.local/bin/yt-dlp
chmod +x ~/.local/bin/yt-dlp
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc

yt-dlp --version
```

pip를 선호한다면 AppStream의 Python 3.11을 사용합니다:

```bash
sudo dnf install -y python3.11
python3.11 -m pip install --user -U yt-dlp
```

### 3.2 FFmpeg

Rocky 8의 base/AppStream 저장소에는 FFmpeg가 없습니다. 두 가지 선택지가 있습니다.

**방법 A — static 빌드 (root 권한 불필요, 권장)**

```bash
curl -L https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz \
     -o /tmp/ffmpeg.tar.xz
tar -xf /tmp/ffmpeg.tar.xz -C /tmp
cp /tmp/ffmpeg-*-static/ffmpeg  ~/.local/bin/
cp /tmp/ffmpeg-*-static/ffprobe ~/.local/bin/

ffmpeg -version
```

시스템 라이브러리와 충돌하지 않고, 여러 머신에 복사만으로 배포할 수 있습니다. 다만 NVENC/VAAPI 지원 여부는 빌드에 따라 다르므로 확인이 필요합니다:

```bash
ffmpeg -hide_banner -encoders | grep -E 'nvenc|vaapi|qsv'
```

**방법 B — RPM Fusion (root 권한 필요)**

```bash
sudo dnf install -y epel-release
sudo dnf config-manager --set-enabled powertools
sudo dnf install -y --nogpgcheck \
  https://mirrors.rpmfusion.org/free/el/rpmfusion-free-release-8.noarch.rpm \
  https://mirrors.rpmfusion.org/nonfree/el/rpmfusion-nonfree-release-8.noarch.rpm
sudo dnf install -y ffmpeg ffmpeg-devel
```

### 3.3 그 외 의존성

| 항목 | 확인 | 비고 |
|:--|:--|:--|
| bash 4.x | `bash --version` | Rocky 8은 4.4. `wait -n`, `mapfile -d` 사용 |
| python3 | `python3 --version` | JSON 파싱 전용이므로 3.6으로 충분 |
| jq | — | **불필요.** python3로 대체 |

### 3.4 업데이트

YouTube 사양 변경으로 추출이 깨지는 경우가 잦습니다. 문제 발생 시 가장 먼저 시도할 조치입니다.

```bash
yt-dlp -U                    # 단독 바이너리
python3.11 -m pip install --user -U yt-dlp   # pip 설치본
```

주기적 갱신을 원하면 cron에 등록합니다:

```bash
0 4 * * 1 /home/knight/.local/bin/yt-dlp -U >/dev/null 2>&1
```

---

## 4. 스크립트 사용법

### 4.1 설치

```bash
cp yt-transcode.sh ~/.local/bin/
chmod +x ~/.local/bin/yt-transcode.sh
yt-transcode.sh --help
```

### 4.2 입력 형식

다음을 모두 인식하며, `&list=`·`&index=`·추적 파라미터는 자동으로 제거됩니다.

```bash
yt-transcode.sh dQw4w9WgXcQ                                    # 11자리 ID
yt-transcode.sh 'https://youtu.be/dQw4w9WgXcQ'                 # 단축 링크
yt-transcode.sh 'https://www.youtube.com/watch?v=dQw4w9WgXcQ'  # 일반 링크
yt-transcode.sh 'https://www.youtube.com/shorts/dQw4w9WgXcQ'   # Shorts
```

> URL은 **반드시 따옴표로 감싸세요.** `&`는 셸에서 백그라운드 실행 연산자로 해석됩니다.

### 4.3 목록 파일

한 줄에 하나씩 기재합니다. `#` 이후는 주석, 빈 줄은 무시됩니다.

```
# links.txt — 프로젝트 참고 영상

https://www.youtube.com/watch?v=bGYi7eOoVJ4&list=PLKt...&index=3
https://youtu.be/cWSXtFMh0Yk
dQw4w9WgXcQ          # 링크 뒤 주석도 제거됨

# 아래 줄은 비활성화 상태
# https://youtu.be/AAAAAAAAAAA
```

```bash
yt-transcode.sh -l links.txt -d /data/encoded -s
```

### 4.4 재생목록

```bash
# 전체
yt-transcode.sh 'https://www.youtube.com/playlist?list=PLxxxx' --playlist -s

# 앞에서 10개만 (동작 확인용)
yt-transcode.sh 'https://www.youtube.com/playlist?list=PLxxxx' --playlist --limit 10 -n
```

`--flat-playlist`로 목록만 빠르게 조회하므로 100개 규모도 수 초면 끝납니다.

목록 파일로 먼저 뽑아 편집하고 싶다면:

```bash
yt-dlp --flat-playlist --print "%(id)s  # %(playlist_index)03d %(title)s" \
       'https://www.youtube.com/playlist?list=PLxxxx' > links.txt
```

```
bGYi7eOoVJ4  # 003 중급자 리트머스 1강
iZ2kdvAUfh4  # 004 중급자 리트머스 2강
```

제외할 영상은 줄 앞에 `#`만 붙이면 됩니다.

### 4.5 옵션 전체

**입력**

| 옵션 | 설명 |
|:--|:--|
| `<URL\|VIDEO_ID>` | 링크 또는 11자리 영상 ID |
| `-l, --list FILE` | 목록 파일 |
| `--playlist` | 재생목록 URL을 전체 영상으로 펼침 |
| `--limit N` | `--playlist`와 함께, 앞에서 N개만 |

**출력**

| 옵션 | 기본값 | 설명 |
|:--|:--|:--|
| `-o, --out FILE` | 영상 제목 | 출력 파일명 (단일 모드) |
| `-d, --out-dir DIR` | 현재 폴더 | 출력 폴더 (없으면 생성) |
| `-f, --force` | off | 기존 파일 덮어쓰기 |
| `-s, --skip-existing` | off | 기존 파일 건너뛰기 |

**인코딩**

| 옵션 | 기본값 | 설명 |
|:--|:--|:--|
| `-H, --height N` | 1080 | 최대 세로 해상도 |
| `-q, --crf N` | 23 | 화질. 낮을수록 고화질 |
| `-p, --preset NAME` | veryfast | x264 속도/압축 트레이드오프 |
| `-b, --abr RATE` | 128k | 오디오 비트레이트 |
| `-e, --encoder NAME` | x264 | x264 / nvenc / qsv / vaapi |
| `--start TIME` | — | 시작 지점 (`00:01:30`) |
| `--duration TIME` | — | 길이 (`60`) |

**동작**

| 옵션 | 기본값 | 설명 |
|:--|:--|:--|
| `-j, --jobs N` | 1 | 병렬 인코딩 개수 |
| `--play` | off | ffplay 미리보기 (단일 모드) |
| `--stop` | off | 실패 시 즉시 중단 |
| `--cookies BROWSER` | — | 브라우저 쿠키 사용 |
| `-n, --dry-run` | off | ffmpeg 명령만 출력 |
| `-h, --help` | — | 도움말 |

### 4.6 출력 예시

```
[INFO] 3 video(s) queued from links.txt
[INFO] Output folder: /data/encoded
[INFO] Encoder: x264  crf 23  max 1080p  jobs 1

=== [1/3] https://www.youtube.com/watch?v=bGYi7eOoVJ4
  Title  : 중급자 리트머스 1강
  Output : /data/encoded/중급자 리트머스 1강.mp4
[DONE] 87MB
  Elapsed: 00:04:12

=== [2/3] https://www.youtube.com/watch?v=cWSXtFMh0Yk
[ERROR] Could not resolve: https://www.youtube.com/watch?v=cWSXtFMh0Yk
       ERROR: Video unavailable

==================== Summary ====================
  Encoded : 2
  Failed  : 1
  Total   : 00:09:37
  Folder  : /data/encoded

  Failed items:
    line 12 https://www.youtube.com/watch?v=cWSXtFMh0Yk

  Signed URLs expire after a few hours and are tied to your IP.
  Re-run with -s so finished files are left alone.
=================================================
```

기본적으로 실패해도 나머지를 계속 처리하고, 마지막에 실패 목록과 원본 파일의 줄 번호를 알려줍니다.

### 4.7 구현상의 주요 결정

| 항목 | 이유 |
|:--|:--|
| `-J`(JSON) 사용 | `-g`는 헤더를 주지 않아 403 발생 |
| python3로 JSON 파싱 | `jq` 의존성 제거. 파싱 전용이라 3.6으로 충분 |
| NUL 구분자 + 배열 전달 | 공백·`&`·따옴표가 포함된 제목과 URL을 안전하게 처리 |
| 성공을 파일 크기로 판정 | `ffmpeg \| ffplay` 구성에서 종료 코드는 ffplay의 값 |
| 영상 ID만 추출해 URL 재구성 | 셸이 링크를 잘라도 복원 가능 |
| 미리보기 기본 off | 헤드리스 서버 전제 |

---

## 5. FFmpeg 직접 사용

스크립트 없이 수동으로 수행하거나, 다른 파이프라인에 통합할 때 참고하십시오.

### 5.1 최소 구성

```bash
URL='https://www.youtube.com/watch?v=XXXXXXXX'

# 헤더 없이 — 403이 나면 5.2로
read -r VURL AURL < <(yt-dlp --no-playlist -f 'bv*[height<=1080]+ba' -g "$URL" | tr '\n' ' ')

ffmpeg -i "$VURL" -i "$AURL" -map 0:v -map 1:a \
       -c:v libx264 -preset veryfast -crf 23 -c:a aac -b:a 128k \
       out.mp4
```

### 5.2 헤더 포함 (권장)

```bash
URL='https://www.youtube.com/watch?v=XXXXXXXX'

mapfile -d '' -t ARGS < <(
  yt-dlp --no-playlist -f 'bv*[height<=1080]+ba/b' -J "$URL" | python3 -c '
import json, sys
info = json.load(sys.stdin)
streams = info.get("requested_formats") or [info]
out = []
for s in streams:
    h = dict(s.get("http_headers") or {})
    ua = h.pop("User-Agent", "Mozilla/5.0")
    out += ["-user_agent", ua]
    if h:
        out += ["-headers", "".join("%s: %s\r\n" % kv for kv in h.items())]
    out += ["-i", s["url"]]
sys.stdout.write("\0".join(out))
'
)

ffmpeg "${ARGS[@]}" -map 0:v:0 -map 1:a:0 \
       -c:v libx264 -preset veryfast -crf 23 \
       -c:a aac -b:a 128k -movflags +faststart \
       out.mp4
```

### 5.3 파이프 방식

yt-dlp가 직접 데이터를 내려받아 FFmpeg에 전달합니다. 헤더 문제가 원천적으로 발생하지 않지만, seek이 불가능하고 진행률 표시가 부정확합니다.

```bash
yt-dlp -f 'bv*+ba/b' -o - "$URL" \
  | ffmpeg -i pipe:0 -c:v libx264 -preset veryfast -c:a aac \
           -movflags +faststart out.mp4
```

### 5.4 유용한 변형

```bash
# 재인코딩 없이 그대로 합치기 (가장 빠름, 화질 손실 없음)
ffmpeg "${ARGS[@]}" -map 0:v:0 -map 1:a:0 -c copy out.mkv

# 오디오만 추출
yt-dlp -f ba -x --audio-format mp3 "$URL"

# 특정 구간만
ffmpeg -ss 00:01:30 "${ARGS[@]}" -t 60 -map 0:v:0 -map 1:a:0 \
       -c:v libx264 -crf 23 -c:a aac clip.mp4

# 해상도 축소
ffmpeg "${ARGS[@]}" -map 0:v:0 -map 1:a:0 \
       -vf 'scale=-2:720' -c:v libx264 -crf 23 -c:a aac out720.mp4

# 썸네일 추출 (10초 지점)
ffmpeg -ss 10 "${ARGS[@]}" -map 0:v:0 -frames:v 1 thumb.jpg
```

---

## 6. 성능 튜닝

### 6.1 CRF와 preset

CRF는 품질 기준값입니다. 낮을수록 고화질·대용량이며, 6 감소할 때마다 파일 크기가 약 2배가 됩니다.

| CRF | 용도 |
|:--|:--|
| 18 | 시각적 무손실에 가까움. 보관용 |
| 20–23 | 일반적인 배포 품질 (기본값 23) |
| 26–28 | 용량 우선. 강의·화면 녹화에 적합 |

preset은 인코딩 속도와 압축 효율의 트레이드오프입니다. 화질에는 거의 영향이 없고 같은 CRF에서 파일 크기만 달라집니다.

| preset | 상대 속도 | 용도 |
|:--|:--|:--|
| ultrafast | ~10× | 임시 확인용 |
| veryfast | ~3× | 기본값. 대량 처리에 적합 |
| medium | 1× | 균형 |
| slow | ~0.5× | 보관용 |

### 6.2 GPU 가속

```bash
# 지원 인코더 확인
ffmpeg -hide_banner -encoders | grep -E 'nvenc|vaapi|qsv'
```

| 하드웨어 | 옵션 | 요구사항 |
|:--|:--|:--|
| NVIDIA | `-e nvenc` | 드라이버 + NVENC 지원 빌드 |
| Intel | `-e qsv` | `intel-media-driver` |
| AMD/Intel | `-e vaapi` | `/dev/dri/renderD128` 접근 권한 |

CPU 대비 5~10배 빠르지만 동일 비트레이트에서 화질은 다소 낮습니다. 대량 처리에 적합합니다.

VAAPI 사용 시 렌더 노드 권한을 확인하십시오:

```bash
ls -l /dev/dri/renderD128
sudo usermod -aG video,render $USER   # 재로그인 필요
```

### 6.3 병렬 처리

```bash
yt-transcode.sh -l links.txt -j 4
```

| 인코더 | 권장 `-j` | 근거 |
|:--|:--|:--|
| x264 | 코어 수 ÷ 4 | x264가 이미 멀티스레드를 사용 |
| nvenc | 2–3 | 소비자용 GPU는 동시 세션 수 제한 |
| vaapi/qsv | 1–2 | 하드웨어 엔진이 단일 |

네트워크 대역폭도 고려해야 합니다. 1080p 영상 하나가 대략 10~20 Mbps를 소비합니다.

병렬 모드에서는 각 작업 로그가 개별 파일에 기록된 후 순서대로 출력되므로 화면이 뒤섞이지 않습니다.

### 6.4 대량 작업 권장 구성

```bash
yt-transcode.sh 'https://www.youtube.com/playlist?list=PLxxxx' \
  --playlist -s -j 4 -e nvenc -q 24 -d /data/encoded
```

- `-s` — 서명 URL이 6시간마다 만료되므로 중단 후 재실행이 전제입니다. 완료분을 건너뜁니다.
- 실패분은 요약에 목록으로 출력되며, 같은 명령을 다시 실행하면 실패한 것만 재시도됩니다.

장시간 작업은 `tmux` 또는 `nohup`으로 실행하십시오:

```bash
tmux new -s encode
yt-transcode.sh -l links.txt -s -j 4 -d /data/encoded
# Ctrl+B, D 로 분리 / tmux attach -t encode 로 복귀
```

```bash
nohup yt-transcode.sh -l links.txt -s -d /data/encoded > encode.log 2>&1 &
tail -f encode.log
```

---

## 7. 문제 해결

### 7.1 진단 순서

```bash
# 1. 버전 확인 — 가장 흔한 원인
yt-dlp --version

# 2. 포맷 목록
yt-dlp --no-playlist -F "<url>"

# 3. 스트림 추출 — DASH이면 2줄이 나와야 정상
yt-dlp --no-playlist -g "<url>"

# 4. 스크립트가 생성하는 ffmpeg 명령 확인
yt-transcode.sh "<url>" -n
```

4번 출력에서 각 `-i` 앞에 `-user_agent`가 붙어 있으면 헤더 전달이 정상입니다.

### 7.2 증상별 대응

| 증상 | 원인 | 조치 |
|:--|:--|:--|
| `403 Forbidden` | User-Agent 불일치 또는 URL 만료 | `-n`으로 헤더 확인. 만료면 재실행 |
| `Sign in to confirm you're not a bot` | 봇 차단 | `--cookies chrome` (브라우저 완전 종료 후) |
| `Unable to extract nsig` | yt-dlp 버전 노후 | `yt-dlp -U` |
| `Requested format is not available` | 해당 해상도 없음 | `-F`로 확인 후 `-H` 조정 |
| 오디오만 반환됨 | 영상 스트림 추출 실패 | yt-dlp 업데이트, 쿠키 사용 |
| `Video unavailable` | 비공개·지역 제한·삭제 | 쿠키 사용, 또는 해당 항목 제외 |
| `Permission denied` | 실행 권한 없음 | `chmod +x yt-transcode.sh` |
| `mapfile: -d: invalid option` | bash 3.x (구형) | bash 4.x 필요. Rocky 8은 기본 4.4 |
| 재생목록 전체가 처리됨 | `&list=` 파라미터 | 스크립트가 자동 제거. 전체는 `--playlist` |
| 출력이 0바이트 | 인코딩 실패 | 스크립트가 자동 삭제. 로그의 ffmpeg 오류 확인 |

### 7.3 쿠키 사용

봇 차단이나 연령 제한 영상에 필요합니다.

```bash
yt-transcode.sh "<url>" --cookies firefox
```

브라우저가 **완전히 종료된 상태**여야 합니다. 실행 중이면 쿠키 DB가 잠겨 읽지 못합니다.

헤드리스 서버에는 브라우저가 없으므로, 데스크톱에서 쿠키를 파일로 내보내 전송합니다:

```bash
# 데스크톱에서
yt-dlp --cookies-from-browser chrome --cookies cookies.txt --skip-download "<url>"
scp cookies.txt server:~/

# 서버에서
yt-dlp --cookies ~/cookies.txt ...
```

쿠키 파일은 계정 접근 권한을 담고 있으므로 권한을 제한하십시오: `chmod 600 cookies.txt`

### 7.4 디버깅 옵션

```bash
# ffmpeg 상세 로그
FFREPORT=file=ffreport.log:level=32 yt-transcode.sh "<url>"

# yt-dlp 상세 출력
yt-dlp -v --no-playlist -J "<url>" 2>&1 | head -50

# 특정 클라이언트 강제 (403이 지속될 때)
yt-dlp --extractor-args "youtube:player_client=web" -J "<url>"
```

---

## 8. 부록

### 8.1 파일 목록

| 파일 | 용도 |
|:--|:--|
| `yt-transcode.sh` | Linux 실행 스크립트 |
| `links.txt` | 목록 파일 템플릿 |
| `yt-transcode-guide.md` | 이 문서 |

### 8.2 yt-dlp 포맷 선택자

스크립트는 `bv*[height<=N]+ba/b[height<=N]/b`를 사용합니다. 의미는 다음과 같습니다.

| 표현 | 의미 |
|:--|:--|
| `bv*` | 최고 화질 영상 (오디오 포함 여부 무관) |
| `ba` | 최고 음질 오디오 |
| `+` | 두 스트림 결합 |
| `/` | 앞이 실패하면 뒤를 시도 (fallback) |
| `b` | 영상+음성 통합 스트림 |
| `[height<=1080]` | 조건 필터 |

자주 쓰는 변형:

```bash
-f 'bv*[ext=mp4]+ba[ext=m4a]/b[ext=mp4]'   # MP4 계열만 (재인코딩 불필요)
-f 'bv*[vcodec^=avc1]+ba'                   # H.264만 (호환성 우선)
-f 'ba'                                      # 오디오만
-f 'b[height<=480]'                          # 저용량 통합 스트림
```

### 8.3 자동화 예시

매주 새 영상만 인코딩:

```bash
#!/bin/bash
# /home/knight/bin/weekly-encode.sh
export PATH="$HOME/.local/bin:$PATH"

PLAYLIST='https://www.youtube.com/playlist?list=PLxxxx'
OUTDIR=/data/encoded

yt-dlp -U >/dev/null 2>&1
yt-transcode.sh "$PLAYLIST" --playlist -s -j 2 -e nvenc -d "$OUTDIR"
```

```
0 3 * * 1 /home/knight/bin/weekly-encode.sh >> /var/log/encode.log 2>&1
```

`-s`가 있으므로 이미 받은 영상은 건너뛰고 새로 추가된 것만 처리됩니다.

### 8.4 참고 자료

| 항목 | 주소 |
|:--|:--|
| yt-dlp | https://github.com/yt-dlp/yt-dlp |
| 포맷 선택 문서 | https://github.com/yt-dlp/yt-dlp#format-selection |
| FFmpeg 문서 | https://ffmpeg.org/documentation.html |
| static 빌드 | https://johnvansickle.com/ffmpeg/ |
| RPM Fusion | https://rpmfusion.org/ |

---

## 이용 조건

YouTube 콘텐츠의 다운로드는 서비스 약관의 제한을 받습니다. 자사 업로드 영상, CC 라이선스 콘텐츠, 또는 다운로드가 명시적으로 허용된 자료에 한해 사용하십시오.
