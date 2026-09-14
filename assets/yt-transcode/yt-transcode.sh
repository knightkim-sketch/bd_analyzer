#!/usr/bin/env bash
#
# yt-transcode.sh - transcode a YouTube link, or a list of them, with ffmpeg.
# Tested on Rocky Linux 8. Requires bash 4.x, python3, yt-dlp, ffmpeg.
#
# Why the headers matter: yt-dlp usually extracts stream URLs using a spoofed
# client (ANDROID_VR, web_safari, ...). googlevideo checks the User-Agent
# against the client that signed the URL and answers 403 to anything else.
# "yt-dlp -g" prints only the URL, so this script uses "-J" and forwards the
# http_headers of each stream to ffmpeg.

set -uo pipefail

VERSION="1.0"

# ------------------------------------------------------------------ defaults
HEIGHT=1080
CRF=23
PRESET=veryfast
ABR=128k
ENCODER=x264
OUTDIR="$PWD"
OUTFILE=""
LISTFILE=""
START=""
DURATION=""
COOKIES=""
JOBS=1
PLAY=0
FORCE=0
SKIP_EXISTING=0
STOP_ON_ERROR=0
PLAYLIST=0
LIMIT=0
DRYRUN=0
URLARG=""

# ------------------------------------------------------------------ colours
if [[ -t 1 ]]; then
    C_RST=$'\033[0m'; C_RED=$'\033[31m'; C_GRN=$'\033[32m'
    C_YEL=$'\033[33m'; C_CYN=$'\033[36m'; C_DIM=$'\033[2m'
else
    C_RST=""; C_RED=""; C_GRN=""; C_YEL=""; C_CYN=""; C_DIM=""
fi

info() { printf '%s[INFO]%s %s\n'  "$C_CYN" "$C_RST" "$*"; }
warn() { printf '%s[WARN]%s %s\n'  "$C_YEL" "$C_RST" "$*" >&2; }
err()  { printf '%s[ERROR]%s %s\n' "$C_RED" "$C_RST" "$*" >&2; }
ok()   { printf '%s[DONE]%s %s\n'  "$C_GRN" "$C_RST" "$*"; }

usage() {
    cat <<'EOF'
yt-transcode.sh - transcode YouTube videos with ffmpeg

USAGE
  yt-transcode.sh <URL|VIDEO_ID> [options]
  yt-transcode.sh -l links.txt [options]

INPUT
  <URL|VIDEO_ID>        Full YouTube URL, youtu.be link, or 11-char video ID
  -l, --list FILE       Text file, one link or ID per line.
                        '#' starts a comment; blank lines ignored.

OUTPUT
  -o, --out FILE        Exact output file (single-video mode only)
  -d, --out-dir DIR     Output folder (default: current directory)
  -f, --force           Overwrite existing files
  -s, --skip-existing   Skip videos whose output already exists

ENCODING
  -H, --height N        Max vertical resolution        (default: 1080)
  -q, --crf N           Quality, lower is better       (default: 23)
  -p, --preset NAME     x264 preset                    (default: veryfast)
  -b, --abr RATE        Audio bitrate                  (default: 128k)
  -e, --encoder NAME    av1 | svtav1 | x265 | x264 |
                        nvenc | qsv | vaapi | copy     (default: x264)
      --start TIME      Clip start, e.g. 00:01:30
      --duration TIME   Clip length, e.g. 60

BEHAVIOUR
  -j, --jobs N          Encode N videos in parallel    (default: 1)
      --play            Show an ffplay preview (single-video mode)
      --stop            Stop the list on the first failure
      --cookies BROWSER Load cookies from chrome|firefox|edge|brave
      --playlist        Expand a playlist URL into all of its videos
      --limit N         With --playlist, take only the first N videos
  -n, --dry-run         Print the ffmpeg command instead of running it
  -h, --help            This text

EXAMPLES
  yt-transcode.sh dQw4w9WgXcQ
  yt-transcode.sh 'https://youtu.be/XXXXXXXX' -H 720 -q 20
  yt-transcode.sh -l links.txt -d /data/encoded -s
  yt-transcode.sh -l links.txt -j 4 -e nvenc
  yt-transcode.sh 'https://www.youtube.com/playlist?list=PLxxxx' --playlist -s
EOF
}

# ------------------------------------------------------------------ args
while [[ $# -gt 0 ]]; do
    case "$1" in
        -l|--list)          LISTFILE="$2"; shift 2 ;;
        -o|--out)           OUTFILE="$2";  shift 2 ;;
        -d|--out-dir)       OUTDIR="$2";   shift 2 ;;
        -H|--height)        HEIGHT="$2";   shift 2 ;;
        -q|--crf)           CRF="$2";      shift 2 ;;
        -p|--preset)        PRESET="$2";   shift 2 ;;
        -b|--abr)           ABR="$2";      shift 2 ;;
        -e|--encoder)       ENCODER="$2";  shift 2 ;;
        -j|--jobs)          JOBS="$2";     shift 2 ;;
        --start)            START="$2";    shift 2 ;;
        --duration)         DURATION="$2"; shift 2 ;;
        --cookies)          COOKIES="$2";  shift 2 ;;
        -f|--force)         FORCE=1;         shift ;;
        -s|--skip-existing) SKIP_EXISTING=1; shift ;;
        --stop)             STOP_ON_ERROR=1; shift ;;
        --play)             PLAY=1;          shift ;;
        --playlist)         PLAYLIST=1;      shift ;;
        --limit)            LIMIT="$2";    shift 2 ;;
        -n|--dry-run)       DRYRUN=1;        shift ;;
        -h|--help)          usage; exit 0 ;;
        -V|--version)       echo "yt-transcode.sh $VERSION"; exit 0 ;;
        -*)                 err "Unknown option: $1"; usage >&2; exit 2 ;;
        *)                  URLARG="$1";     shift ;;
    esac
done

if [[ -z "$URLARG" && -z "$LISTFILE" ]]; then
    usage >&2
    exit 2
fi

# ------------------------------------------------------------------ prereqs
need() {
    command -v "$1" >/dev/null 2>&1 && return 0
    err "'$1' not found on PATH."
    printf '       %s\n' "$2" >&2
    exit 1
}

need yt-dlp  "Install: python3 -m pip install --user -U yt-dlp   (needs Python 3.9+)
       or:      curl -L https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux \\
                     -o ~/.local/bin/yt-dlp && chmod +x ~/.local/bin/yt-dlp"
need ffmpeg  "Rocky 8: enable RPM Fusion, or use a static build from johnvansickle.com/ffmpeg"
need python3 "Rocky 8 ships python3; install with: sudo dnf install -y python3"

HAVE_FFPLAY=0
command -v ffplay >/dev/null 2>&1 && HAVE_FFPLAY=1

if (( PLAY )) && (( ! HAVE_FFPLAY )); then
    warn "ffplay not available; continuing without preview."
    PLAY=0
fi
if (( PLAY )) && [[ -n "$LISTFILE" ]]; then
    warn "Preview is ignored in list mode."
    PLAY=0
fi
if (( PLAY )) && (( JOBS > 1 )); then
    warn "Preview is ignored when running parallel jobs."
    PLAY=0
fi

mkdir -p "$OUTDIR" || { err "Cannot create output directory: $OUTDIR"; exit 1; }
OUTDIR="$(cd "$OUTDIR" && pwd)"

# ------------------------------------------------------------------ yt-dlp base
# Always --no-playlist here: a playlist is expanded into the queue up front,
# so every -J call resolves exactly one video.
YT_BASE=(--no-warnings --no-playlist)
[[ -n "$COOKIES" ]] && YT_BASE+=(--cookies-from-browser "$COOKIES")

# Used only for playlist enumeration, where --no-playlist must not appear.
YT_FLAT=(--no-warnings --flat-playlist)
[[ -n "$COOKIES" ]] && YT_FLAT+=(--cookies-from-browser "$COOKIES")

# ------------------------------------------------------------------ encoder
case "$ENCODER" in
    nvenc) VARGS=(-c:v h264_nvenc -preset p4 -rc vbr -cq "$CRF") ;;
    qsv)   VARGS=(-c:v h264_qsv -global_quality "$CRF") ;;
    vaapi) VARGS=(-vaapi_device /dev/dri/renderD128 -vf 'format=nv12,hwupload'
                  -c:v h264_vaapi -qp "$CRF") ;;
    x264)  VARGS=(-c:v libx264 -preset "$PRESET" -crf "$CRF" -pix_fmt yuv420p) ;;
    # AV1 and HEVC, added for bd_analyzer: this is an AV1 analyzer, and a clip encoded to
    # H.264 cannot be opened for the block level analysis the rest of the application is for.
    x265)  VARGS=(-c:v libx265 -preset "$PRESET" -crf "$CRF" -pix_fmt yuv420p) ;;
    # -cpu-used is libaom's speed control; 4 keeps a 1080p clip to minutes rather than hours.
    # -row-mt and -tiles use the cores that would otherwise sit idle on a single threaded pass.
    av1)   VARGS=(-c:v libaom-av1 -crf "$CRF" -b:v 0 -cpu-used 4 -row-mt 1 -tiles 2x2
                  -pix_fmt yuv420p) ;;
    svtav1) VARGS=(-c:v libsvtav1 -crf "$CRF" -preset 6 -pix_fmt yuv420p) ;;
    # No re-encode at all. The reference source for an encoder comparison: whatever YouTube
    # served, kept exactly, so every curve is measured against the same picture.
    copy)  VARGS=(-c:v copy) ;;
    *)     err "Unknown encoder: $ENCODER"; exit 2 ;;
esac

if [[ "$ENCODER" == copy ]]; then
    AARGS=(-c:a copy)
else
    AARGS=(-c:a aac -b:a "$ABR")
fi

TRIM=()
[[ -n "$START"    ]] && TRIM+=(-ss "$START")
[[ -n "$DURATION" ]] && TRIM+=(-t  "$DURATION")

# ------------------------------------------------------------------ link parsing
# Reduce anything the user pasted to a canonical watch URL. Strips &list=,
# &index=, tracking parameters, and tolerates a bare video ID.
canonical_url() {
    local text="$1" id=""
    if [[ "$text" =~ [?\&]v=([A-Za-z0-9_-]{11}) ]] \
    || [[ "$text" =~ youtu\.be/([A-Za-z0-9_-]{11}) ]] \
    || [[ "$text" =~ /shorts/([A-Za-z0-9_-]{11}) ]] \
    || [[ "$text" =~ /embed/([A-Za-z0-9_-]{11}) ]] \
    || [[ "$text" =~ /live/([A-Za-z0-9_-]{11}) ]]; then
        id="${BASH_REMATCH[1]}"
    elif [[ "$text" =~ ^[A-Za-z0-9_-]{11}$ ]]; then
        id="$text"
    fi

    if [[ -n "$id" ]]; then
        printf 'https://www.youtube.com/watch?v=%s\n' "$id"
    elif [[ "$text" =~ ^https?:// ]]; then
        printf '%s\n' "$text"      # not YouTube; let yt-dlp decide
    else
        return 1
    fi
}

# Turn a filename into something safe without being destructive.
sanitize() {
    printf '%s' "$1" \
        | tr -d '\000-\037' \
        | sed -e 's#[/\\:*?"<>|]#_#g' -e 's/  */ /g' -e 's/^ *//' -e 's/ *$//' \
        | cut -c1-100
}

# ------------------------------------------------------------------ JSON parse
# Reads yt-dlp -J on stdin and emits NUL-separated fields:
#   title, id, stream_count, then the complete ffmpeg input arguments.
# Doing it here keeps jq out of the dependency list; Rocky 8's python3 is
# only used as a JSON reader, so its version does not matter.
read_streams() {
    python3 -c '
import json, sys

info = json.load(sys.stdin)
streams = info.get("requested_formats") or [info]
streams = [s for s in streams if s.get("url")]

out = [info.get("title") or "", info.get("id") or "", str(len(streams))]

for s in streams:
    headers = dict(s.get("http_headers") or {})
    ua = headers.pop("User-Agent", None) or "Mozilla/5.0"
    out += ["-user_agent", ua,
            "-reconnect", "1",
            "-reconnect_streamed", "1",
            "-reconnect_delay_max", "5",
            "-rw_timeout", "15000000"]
    if headers:
        out += ["-headers", "".join("%s: %s\r\n" % kv for kv in headers.items())]
    out += ["-i", s["url"]]

sys.stdout.write("\0".join(out))
'
}

# ------------------------------------------------------------------ one video
# Returns 0 = encoded, 3 = skipped, 1 = failed.
convert_one() {
    local link="$1" forced_out="${2:-}"
    local json fields title vid nstreams target

    json="$(yt-dlp "${YT_BASE[@]}" -f "bv*[height<=$HEIGHT]+ba/b[height<=$HEIGHT]/b" -J "$link" 2>/tmp/yt-err.$$)"
    local rc=$?
    if (( rc != 0 )) || [[ -z "$json" ]]; then
        err "Could not resolve: $link"
        [[ -s /tmp/yt-err.$$ ]] && sed 's/^/       /' /tmp/yt-err.$$ >&2
        rm -f /tmp/yt-err.$$
        return 1
    fi
    rm -f /tmp/yt-err.$$

    local -a F
    if ! mapfile -d '' -t F < <(printf '%s' "$json" | read_streams); then
        err "Could not parse yt-dlp output for: $link"
        return 1
    fi
    (( ${#F[@]} < 4 )) && { err "No stream URLs for: $link"; return 1; }

    title="${F[0]}"; vid="${F[1]}"; nstreams="${F[2]}"
    local -a INPUTS=("${F[@]:3}")

    # ---- output path
    if [[ -n "$forced_out" ]]; then
        target="$forced_out"
    else
        local safe
        safe="$(sanitize "$title")"
        [[ -z "$safe" ]] && safe="$vid"
        target="$OUTDIR/$safe.mp4"
    fi

    if [[ -e "$target" ]]; then
        if (( SKIP_EXISTING )); then
            printf '  %s[SKIP]%s %s\n' "$C_DIM" "$C_RST" "$target"
            return 3
        fi
        if (( ! FORCE )); then
            err "File exists: $target  (use -f to overwrite, -s to skip)"
            return 1
        fi
    fi

    printf '  Title  : %s\n' "$title"
    printf '  Output : %s%s%s\n' "$C_GRN" "$target" "$C_RST"

    local -a MAPS
    if (( nstreams >= 2 )); then
        MAPS=(-map 0:v:0 -map 1:a:0)     # separate video and audio
    else
        MAPS=(-map 0:v:0 -map '0:a:0?')  # single muxed stream
    fi

    local -a FF=(-hide_banner -loglevel warning -stats -y
                 "${TRIM[@]}" "${INPUTS[@]}" "${MAPS[@]}"
                 "${VARGS[@]}" "${AARGS[@]}")

    if (( PLAY )); then
        FF+=(-f tee "[f=mp4:movflags=+faststart]$target|[f=nut]pipe:1")
    else
        FF+=(-movflags +faststart "$target")
    fi

    if (( DRYRUN )); then
        printf '  %sffmpeg' "$C_CYN"
        printf ' %q' "${FF[@]}"
        printf '%s\n' "$C_RST"
        return 3
    fi

    if (( PLAY )); then
        ffmpeg "${FF[@]}" | ffplay -hide_banner -loglevel error -autoexit -i pipe:0
    else
        ffmpeg "${FF[@]}"
    fi

    # ffmpeg's exit code is unreliable through a pipe, so judge by the file.
    if [[ -s "$target" ]] && (( $(stat -c%s "$target") > 51200 )); then
        ok "$(numfmt --to=iec --suffix=B "$(stat -c%s "$target")" 2>/dev/null || stat -c%s "$target")"
        return 0
    fi

    err "No usable output was produced for: $link"
    [[ -e "$target" ]] && rm -f "$target"
    return 1
}

# ------------------------------------------------------------------ build queue
declare -a QUEUE=() LABELS=()

if [[ -n "$LISTFILE" ]]; then
    [[ -r "$LISTFILE" ]] || { err "List file not readable: $LISTFILE"; exit 1; }

    lineno=0
    while IFS= read -r raw || [[ -n "$raw" ]]; do
        (( lineno++ ))
        line="${raw%%#*}"                      # strip comments
        line="${line#"${line%%[![:space:]]*}"}"  # ltrim
        line="${line%"${line##*[![:space:]]}"}"  # rtrim
        [[ -z "$line" ]] && continue
        line="${line%\"}"; line="${line#\"}"
        line="${line%,}"

        if u="$(canonical_url "$line")"; then
            QUEUE+=("$u"); LABELS+=("line $lineno")
        else
            warn "Line $lineno: not a recognizable link, skipping - $line"
        fi
    done < "$LISTFILE"

    (( ${#QUEUE[@]} )) || { err "No usable entries in $LISTFILE"; exit 1; }
    info "${#QUEUE[@]} video(s) queued from $LISTFILE"
elif (( PLAYLIST )); then
    info "Expanding playlist..."
    while IFS= read -r id; do
        [[ "$id" =~ ^[A-Za-z0-9_-]{11}$ ]] || continue
        QUEUE+=("https://www.youtube.com/watch?v=$id")
        LABELS+=("#${#QUEUE[@]}")
        (( LIMIT > 0 && ${#QUEUE[@]} >= LIMIT )) && break
    done < <(yt-dlp "${YT_FLAT[@]}" --print "%(id)s" "$URLARG" 2>/dev/null)

    (( ${#QUEUE[@]} )) || { err "Could not read the playlist: $URLARG"; exit 1; }
    info "${#QUEUE[@]} video(s) in the playlist"
else
    if ! u="$(canonical_url "$URLARG")"; then
        err "Could not find a YouTube video ID in: $URLARG"
        exit 1
    fi
    QUEUE+=("$u"); LABELS+=("")
    [[ "$URLARG" == *"list="* ]] && \
        warn "This link belongs to a playlist. Add --playlist to encode all of it."
fi

info "Output folder: $OUTDIR"
info "Encoder: $ENCODER  crf $CRF  max ${HEIGHT}p  jobs $JOBS"

# ------------------------------------------------------------------ run
TOTAL=${#QUEUE[@]}
OK=0; SKIPPED=0; FAILED=0
declare -a FAILLIST=()
START_TS=$SECONDS

if (( JOBS > 1 && TOTAL > 1 )); then
    # Parallel mode: each video logs to its own file so output stays readable.
    LOGDIR="$(mktemp -d)"
    declare -a PIDS=() PIDIDX=()

    for (( i=0; i<TOTAL; i++ )); do
        while (( $(jobs -rp | wc -l) >= JOBS )); do wait -n 2>/dev/null || break; done
        (
            convert_one "${QUEUE[$i]}" ""
            echo $? > "$LOGDIR/$i.rc"
        ) > "$LOGDIR/$i.log" 2>&1 &
        PIDS+=($!); PIDIDX+=("$i")
        printf '%s=== [%d/%d]%s %s\n' "$C_CYN" "$((i+1))" "$TOTAL" "$C_RST" "${QUEUE[$i]}"
    done
    wait

    for (( i=0; i<TOTAL; i++ )); do
        rc=$(cat "$LOGDIR/$i.rc" 2>/dev/null || echo 1)
        printf '\n%s--- [%d/%d]%s %s\n' "$C_DIM" "$((i+1))" "$TOTAL" "$C_RST" "${QUEUE[$i]}"
        sed 's/^/  /' "$LOGDIR/$i.log" 2>/dev/null | grep -v '^\s*$' | tail -20
        case "$rc" in
            0) (( OK++ )) ;;
            3) (( SKIPPED++ )) ;;
            *) (( FAILED++ )); FAILLIST+=("${LABELS[$i]} ${QUEUE[$i]}") ;;
        esac
    done
    rm -rf "$LOGDIR"
else
    for (( i=0; i<TOTAL; i++ )); do
        printf '\n%s=== [%d/%d]%s %s\n' "$C_CYN" "$((i+1))" "$TOTAL" "$C_RST" "${QUEUE[$i]}"
        item_start=$SECONDS
        convert_one "${QUEUE[$i]}" "$OUTFILE"
        rc=$?
        case "$rc" in
            0) (( OK++ ))
               printf '  %sElapsed: %02d:%02d:%02d%s\n' "$C_DIM" \
                      $(( (SECONDS-item_start)/3600 )) \
                      $(( ((SECONDS-item_start)%3600)/60 )) \
                      $(( (SECONDS-item_start)%60 )) "$C_RST" ;;
            3) (( SKIPPED++ )) ;;
            *) (( FAILED++ )); FAILLIST+=("${LABELS[$i]} ${QUEUE[$i]}")
               if (( STOP_ON_ERROR )); then
                   err "Halting because --stop was given."
                   break
               fi ;;
        esac
    done
fi

# ------------------------------------------------------------------ summary
if (( TOTAL > 1 )); then
    EL=$(( SECONDS - START_TS ))
    printf '\n%s==================== Summary ====================%s\n' "$C_CYN" "$C_RST"
    printf '  Encoded : %d\n' "$OK"
    (( SKIPPED )) && printf '  Skipped : %d\n' "$SKIPPED"
    (( FAILED  )) && printf '  %sFailed  : %d%s\n' "$C_RED" "$FAILED" "$C_RST"
    printf '  Total   : %02d:%02d:%02d\n' $((EL/3600)) $(((EL%3600)/60)) $((EL%60))
    printf '  Folder  : %s\n' "$OUTDIR"
    if (( ${#FAILLIST[@]} )); then
        printf '\n  Failed items:\n'
        printf '    %s\n' "${FAILLIST[@]}"
        printf '\n  Signed URLs expire after a few hours and are tied to your IP.\n'
        printf '  Re-run with -s so finished files are left alone.\n'
    fi
    printf '%s=================================================%s\n' "$C_CYN" "$C_RST"
fi

(( FAILED )) && exit 1
exit 0
