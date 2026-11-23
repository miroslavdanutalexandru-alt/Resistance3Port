#!/usr/bin/env bash
# Linux counterpart of tools/fetch_rpcs3.ps1 (setup_linux.sh and build_linux.sh run it): downloads RPCS3's source at
# the pinned commit, plus the third-party code the RSX/Vulkan build needs, into ps3recomp/third_party/rpcs3 and
# applies this port's patch (ps3recomp/patches/*-rpcs3.patch). RPCS3 is GPL-2.0; see its LICENSE.
# The pins (commit, submodules, extras, marker file) are read from tools/fetch_rpcs3.ps1, so both always agree.
#
#   tools/linux/fetch_rpcs3.sh [--check-files]
#     --check-files   only check that every file the patch adds is there; exit 1 if not (fetch_rpcs3.ps1 -CheckFiles)
set -euo pipefail
cd "$(dirname "$0")/../.."
PS1=tools/fetch_rpcs3.ps1
DEST=ps3recomp/third_party/rpcs3
LOG=logs/fetch_rpcs3.log
CHECK=''
[ "${1:-}" = --check-files ] && CHECK=1

PATCHES=(ps3recomp/patches/*-rpcs3.patch)
{ [ ${#PATCHES[@]} -eq 1 ] && [ -f "${PATCHES[0]}" ]; } || { echo "Expected exactly one ps3recomp/patches/*-rpcs3.patch."; exit 1; }
PATCH="$(pwd)/${PATCHES[0]}"
# files the patch creates ("--- /dev/null" followed by "+++ b/<path>")
new_files() { awk 'prev == "--- /dev/null" && /^\+\+\+ b\// { f = $0; sub(/^\+\+\+ b\//, "", f); sub(/[ \t\r]+$/, "", f); print f } { prev = $0; sub(/\r$/, "", prev) }' "$PATCH"; }
first_missing() { local f; while IFS= read -r f; do [ -e "$DEST/$f" ] || { echo "$f"; return 0; }; done < <(new_files); }

if [ -n "$CHECK" ]; then
    m="$(first_missing)"
    [ -z "$m" ] && exit 0
    echo "   The RPCS3 source in $DEST is missing files the port's patch adds ($m ...): it was patched by an older version."
    exit 1
fi

command -v git >/dev/null 2>&1 || { echo "git was not found - install it with your package manager (git)."; exit 1; }
mkdir -p logs
log() { echo "$*"; echo "$(date +%H:%M:%S) $*" >> "$LOG"; }
GIT=(git -c 'safe.directory=*' -c core.autocrlf=false -c advice.detachedHead=false -c init.defaultBranch=master)
# runs a command with its output in the log; up to 3 tries with a growing pause
retry() {
    local n=1 d=5
    while :; do
        if "$@" >> "$LOG" 2>&1; then return 0; fi
        [ $n -ge 3 ] && return 1
        log "   ...failed, trying again in ${d}s"
        sleep $d; n=$((n + 1)); d=$((d * 2))
    done
}
ps_val() { sed -n "s/^\\\$script:$1 *= *'\\([^']*\\)'.*/\\1/p" "$PS1" | head -n1; }
COMMIT="$(ps_val Commit)"
REPO_URL="$(ps_val RepoUrl)"
MARKER="$(ps_val MarkerRel | tr '\\' '/')"
{ [ ${#COMMIT} -eq 40 ] && [ -n "$REPO_URL" ] && [ -n "$MARKER" ]; } || { echo "Could not read the RPCS3 pin from $PS1."; exit 1; }
PATCH_HASH="$(sha256sum "$PATCH" | cut -d' ' -f1 | tr 'a-f' 'A-F')"   # the same stamps as fetch_rpcs3.ps1
stamp_path() { echo "$DEST/.port_fetch/$(echo "$1" | tr '/\\:' '___').ok"; }
stamp() { mkdir -p "$DEST/.port_fetch"; echo "$2" > "$(stamp_path "$1")"; }
has_stamp() { [ "$(tr -d '\r\n ' 2>/dev/null < "$(stamp_path "$1")")" = "$2" ]; }
# git in $DEST only, never the port folder's own repository when the port is a git checkout
GIT_CEILING_DIRECTORIES="$(cd "$(dirname "$DEST")" && pwd)"
export GIT_CEILING_DIRECTORIES

echo "---- fetch_rpcs3.sh $(date '+%Y-%m-%d %H:%M:%S') into $DEST" >> "$LOG"

# Already there: patched by this version, or by an older version of the port (re-patch it, as fetch_rpcs3.ps1 does)
if [ -f "$DEST/$MARKER" ]; then
    if [ -z "$(first_missing)" ]; then
        if has_stamp patch "$PATCH_HASH" || "${GIT[@]}" -C "$DEST" apply -R --check --whitespace=nowarn "$PATCH" >/dev/null 2>&1; then
            stamp patch "$PATCH_HASH"; echo "   RPCS3 source already present."; exit 0
        fi
    fi
    if [ "$("${GIT[@]}" -C "$DEST" rev-parse -q --verify HEAD 2>/dev/null || true)" = "$COMMIT" ]; then
        log "   The RPCS3 source was patched by another version of the port. Applying this version's patch..."
        "${GIT[@]}" -C "$DEST" -c user.name=PortSetup -c user.email=port-setup@localhost stash push -q --include-untracked \
            -m "port setup: RPCS3 source before re-applying the port's patch $(date +%FT%T)" -- . ':(exclude)3rdparty' >> "$LOG" 2>&1 || true
        if "${GIT[@]}" -C "$DEST" apply --whitespace=nowarn "$PATCH" >> "$LOG" 2>&1 && [ -z "$(first_missing)" ]; then
            stamp patch "$PATCH_HASH"; log "   RPCS3 ${COMMIT:0:7} re-patched for this version of the port."; exit 0
        fi
        log "   The port's patch did not apply to the reset RPCS3 source (the old changes are in git's stash there)."
        log "   Delete the folder $DEST and run ./setup_linux.sh again."
        exit 1
    fi
    log "   The RPCS3 source is not a git checkout of ${COMMIT:0:7}: downloading it again..."
    rm -rf "$DEST"
fi

for round in 1 2; do
    if has_stamp base "$COMMIT|git" && [ -d "$DEST/rpcs3" ]; then
        log "   RPCS3 ${COMMIT:0:7}: already downloaded."
    else
        log "   Downloading RPCS3 ${COMMIT:0:7}..."
        rm -rf "$DEST"; mkdir -p "$DEST"
        "${GIT[@]}" init -q "$DEST" >> "$LOG" 2>&1
        "${GIT[@]}" -C "$DEST" config core.autocrlf false
        "${GIT[@]}" -C "$DEST" remote add origin "$REPO_URL"
        if retry "${GIT[@]}" -C "$DEST" fetch -q --depth 1 --no-tags origin "$COMMIT"; then
            "${GIT[@]}" -C "$DEST" checkout -q -f FETCH_HEAD >> "$LOG" 2>&1
        else
            log "   Downloading RPCS3's full history instead (larger, can take several minutes)..."
            retry "${GIT[@]}" -C "$DEST" -c protocol.version=2 -c http.version=HTTP/1.1 fetch -q --no-tags origin \
                || { log "   Could not download RPCS3 from github.com (git's messages are in $LOG)."; exit 1; }
            "${GIT[@]}" -C "$DEST" checkout -q -f "$COMMIT" >> "$LOG" 2>&1
        fi
        stamp base "$COMMIT|git"
    fi

    # submodules: Path / Sha from fetch_rpcs3.ps1's $script:Subs
    while IFS='|' read -r path sha; do
        [ -n "$path" ] || continue
        if has_stamp "sub_$path" "$sha" && [ -n "$(ls -A "$DEST/$path" 2>/dev/null | grep -vx .git | head -n1)" ]; then continue; fi
        log "   Downloading $path..."
        rm -rf "${DEST:?}/$path"
        retry "${GIT[@]}" -C "$DEST" -c http.version=HTTP/1.1 submodule update -q --init --depth 1 -- "$path" \
            || retry "${GIT[@]}" -C "$DEST" -c protocol.version=2 -c http.version=HTTP/1.1 submodule update -q --init -- "$path" \
            || { log "   Could not download $path (git's messages are in $LOG)."; exit 1; }
        [ "$("${GIT[@]}" -C "$DEST/$path" rev-parse HEAD 2>/dev/null)" = "$sha" ] \
            || { log "   $path is not at the pinned commit $sha."; exit 1; }
        stamp "sub_$path" "$sha"
    done < <(sed -n "s/.*Path = '\\([^']*\\)';.*Sha = '\\([0-9a-f]\\{40\\}\\)'.*/\\1|\\2/p" "$PS1")

    # not RPCS3 submodules: Path / Key / Tag / Url from $script:Extras
    while IFS='|' read -r path key tag url; do
        [ -n "$path" ] || continue
        if has_stamp "$key" "$tag" && [ -n "$(ls -A "$DEST/$path" 2>/dev/null | head -n1)" ]; then continue; fi
        log "   Downloading $key $tag..."
        rm -rf "${DEST:?}/$path"
        retry "${GIT[@]}" clone -q --depth 1 --branch "$tag" "$url" "$DEST/$path" \
            || { rm -rf "${DEST:?}/$path"; log "   Could not download $url $tag (git's messages are in $LOG)."; exit 1; }
        stamp "$key" "$tag"
    done < <(sed -n "s/.*Path = '\\([^']*\\)';.*Key = '\\([^']*\\)';.*Tag = '\\([^']*\\)';.*Url = '\\([^']*\\)'.*/\\1|\\2|\\3|\\4/p" "$PS1")

    if has_stamp patch "$PATCH_HASH" && [ -f "$DEST/$MARKER" ]; then break; fi
    if "${GIT[@]}" -C "$DEST" apply --check --whitespace=nowarn "$PATCH" >> "$LOG" 2>&1; then
        "${GIT[@]}" -C "$DEST" apply --whitespace=nowarn "$PATCH" >> "$LOG" 2>&1
        stamp patch "$PATCH_HASH"; break
    fi
    if "${GIT[@]}" -C "$DEST" apply -R --check --whitespace=nowarn "$PATCH" >/dev/null 2>&1; then stamp patch "$PATCH_HASH"; break; fi
    [ "$round" -lt 2 ] || { log "   The port's patch does not apply to the downloaded RPCS3 source."; exit 1; }
    log "   The port's patch does not fit the downloaded source (a stopped earlier run?). Downloading RPCS3 again..."
    rm -rf "$DEST"
done
[ -f "$DEST/$MARKER" ] || { log "   The patched source is incomplete ($MARKER is missing)."; exit 1; }
log "   RPCS3 ${COMMIT:0:7} downloaded and patched."
