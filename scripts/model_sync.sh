#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Synchronize the prepared RKNN artifact and device configuration without a
# language runtime, model conversion, or deletion of historical application data.
set -euo pipefail
export LC_ALL=C

temporary_files=()
cleanup() {
    local path
    for path in "${temporary_files[@]}"; do rm -f -- "$path"; done
}
trap cleanup EXIT
fail() { printf '[MODEL] %s\n' "$*" >&2; exit 1; }

for utility in flock sha256sum strings realpath stat cp mktemp awk grep od head tr find mv chmod chown cmp mkdir rmdir rm dirname cat; do
    command -v "$utility" >/dev/null || fail "Required command is unavailable: $utility"
done

normalize_platform() {
    local value=${1,,} base
    [[ $value =~ ^(rk|rv)[0-9]{4}[a-z]*$ ]] || return 1
    for base in rk3588 rk3576 rk3568 rk3566 rk3562 rv1126 rv1109 rv1106 rv1103 rk3399pro rk1808; do
        if [[ $value =~ ^${base}[a-z]*$ ]]; then printf '%s\n' "$base"; return 0; fi
    done
    return 1
}

# Reject symlinks in every existing path component, including installation roots.
# Parent traversal is unnecessary for the script-generated absolute paths.
checked_path() {
    local input=$1 absolute current='' part
    [[ ! $input =~ [[:cntrl:]] && /$input/ != *'/../'* ]] || fail "Unsafe path: $input"
    [[ $input = /* ]] || input=$PWD/$input
    absolute=$(realpath -ms -- "$input") || fail "Cannot resolve path: $input"
    local -a parts
    IFS=/ read -r -a parts <<< "$absolute"
    for part in "${parts[@]}"; do
        [[ -n $part ]] || continue
        current=$current/$part
        [[ ! -L $current ]] || fail "Path crosses a symlink: $current"
    done
    printf '%s\n' "$absolute"
}

regular_or_missing() {
    [[ ! -e $1 || -f $1 ]] || fail "Expected a regular file: $1"
}

ensure_directory() {
    local path=$1 owner=${2:-} parent=$1 missing
    local -a created=()
    checked_path "$path" >/dev/null
    while [[ ! -e $parent ]]; do created+=("$parent"); parent=$(dirname -- "$parent"); done
    [[ -d $parent ]] || fail "Expected a directory: $parent"
    mkdir -p -- "$path" || fail "Cannot create directory: $path"
    if [[ $EUID = 0 && -n $owner ]]; then
        for missing in "${created[@]}"; do chown -- "$owner" "$missing"; done
    fi
}

new_temporary() {
    local parent=$1
    SYNC_TEMP=$(mktemp "$parent/.model-sync-XXXXXX") || fail 'Cannot create temporary file'
    temporary_files+=("$SYNC_TEMP")
}

commit_temporary() {
    local temporary=$1 destination=$2 owner=${3:-} mode=$4
    checked_path "$destination" >/dev/null
    regular_or_missing "$destination"
    chmod -- "$mode" "$temporary"
    if [[ $EUID = 0 && -n $owner ]]; then chown -- "$owner" "$temporary"; fi
    mv -fT -- "$temporary" "$destination" || fail "Cannot replace file: $destination"
}

digest() {
    local output
    output=$(sha256sum < "$1") || fail "Cannot hash file: $1"
    printf '%s\n' "${output%% *}"
}

atomic_copy() {
    local source=$1 destination=$2 owner=${3:-} expected=${4:-} temporary mode
    checked_path "$source" >/dev/null
    [[ -f $source ]] || fail "Expected a regular file: $source"
    ensure_directory "$(dirname -- "$destination")" "$owner"
    checked_path "$destination" >/dev/null
    regular_or_missing "$destination"
    mode=$(stat -c %a -- "$source")
    new_temporary "$(dirname -- "$destination")"; temporary=$SYNC_TEMP
    cp --no-dereference -- "$source" "$temporary" || fail "Cannot copy file: $source"
    [[ -f $temporary && ! -L $temporary ]] || fail "Source changed during copy: $source"
    if [[ -n $expected && $(digest "$temporary") != "$expected" ]]; then
        fail "Model changed during synchronization; destination retained: $destination"
    fi
    commit_temporary "$temporary" "$destination" "$owner" "$mode"
}

validate_model() {
    local model=$1 platform=$2 header token candidate found=0
    checked_path "$model" >/dev/null
    [[ -f $model ]] || fail "Prepared RKNN model is missing: $model"
    header=$(od -An -tx1 -N4 -- "$model" | tr -d ' \n')
    [[ $header = 524b4e4e ]] || fail "Invalid RKNN model header: $model"
    while IFS= read -r token; do
        candidate=$(normalize_platform "$token") || continue
        if [[ $candidate = "$platform" ]]; then found=1; break; fi
    done < <(strings -a -- "$model" | grep -Eio '(rk|rv)[0-9]{4}[a-z]*' || :)
    [[ $found = 1 ]] || fail "RKNN model does not report target $platform: $model"
    MODEL_SHA=$(digest "$model")
}

normalize_config() {
    local root=$1 owner=${2:-} path temporary mode
    path=$(checked_path "$root/config/config.txt")
    regular_or_missing "$path"
    [[ -f $path ]] || return 0
    mode=$(stat -c %a -- "$path")
    new_temporary "$root/config"; temporary=$SYNC_TEMP
    awk '$1 == "yolo_model" { print "yolo_model config/yolo26.rknn"; found=1; next }
         { print } END { if (!found) print "yolo_model config/yolo26.rknn" }' \
        "$path" > "$temporary" || fail "Cannot normalize configuration: $path"
    if cmp -s -- "$path" "$temporary"; then rm -f -- "$temporary"; return 0; fi
    commit_temporary "$temporary" "$path" "$owner" "$mode"
}

write_baseline() {
    local value=$1 temporary
    new_temporary "$project/config"; temporary=$SYNC_TEMP
    printf '%s\n' "$value" > "$temporary"
    commit_temporary "$temporary" "$project/config/.last-deployed.sha256" "$project_owner" 644
}

read_baselines() {
    local path value
    SHARED_SHA=''
    baseline_paths=("$project/config/.last-deployed.sha256"
                    "$project/config/models/.last-deployed.sha256"
                    "$project/config/models/$platform/.last-deployed.sha256")
    for path in "${baseline_paths[@]}"; do
        checked_path "$path" >/dev/null
        regular_or_missing "$path"
        [[ -f $path ]] || continue
        value=$(cat -- "$path")
        value="${value#"${value%%[![:space:]]*}"}"
        value="${value%"${value##*[![:space:]]}"}"
        [[ $value =~ ^[0-9a-f]{64}$ ]] || fail "Invalid model baseline retained: $path"
        [[ -z $SHARED_SHA || $SHARED_SHA = "$value" ]] || fail 'Conflicting model baselines were preserved'
        SHARED_SHA=$value
    done
}

prune_legacy_directories() {
    local root=$1 legacy directory
    legacy=$(checked_path "$root/config/models")
    [[ -d $legacy ]] || return 0
    while IFS= read -r -d '' directory; do rmdir -- "$directory" 2>/dev/null || :; done \
        < <(find "$legacy" -depth -type d -print0)
}

finish_legacy_metadata() {
    local path
    # All these baseline files were validated before any model/config write.
    for path in "${baseline_paths[@]:1}"; do
        [[ ! -f $path ]] || rm -f -- "$path"
    done
    [[ -z ${old_lock_path:-} ]] || rm -f -- "$old_lock_path"
    prune_legacy_directories "$project"
}

lock_source() {
    local lock previous
    project_owner=$(stat -c '%u:%g' -- "$project")
    ensure_directory "$project/config" "$project_owner"
    lock=$(checked_path "$project/config/.model-sync.lock")
    regular_or_missing "$lock"
    exec {source_lock_fd}>>"$lock"
    [[ $EUID != 0 ]] || chown -- "$project_owner" "$lock"
    flock -x "$source_lock_fd" || fail 'Cannot lock model synchronization'
    previous=$(checked_path "$project/config/models/.model-sync.lock")
    regular_or_missing "$previous"
    if [[ -f $previous ]]; then
        exec {old_lock_fd}<"$previous"
        flock -xn "$old_lock_fd" || fail 'Legacy model synchronization is active; retry after it finishes'
        old_lock_path=$previous
    fi
}

reconcile() {
    local source=$project/config/yolo26.rknn deployed=$deployment/config/yolo26.rknn
    local source_sha='' deployed_sha='' selected='' next_baseline=''
    source=$(checked_path "$source"); deployed=$(checked_path "$deployed")
    regular_or_missing "$source"; regular_or_missing "$deployed"
    read_baselines
    if [[ -f $source ]]; then validate_model "$source" "$platform"; source_sha=$MODEL_SHA; fi
    if [[ -f $deployed ]]; then validate_model "$deployed" "$platform"; deployed_sha=$MODEL_SHA; fi
    SYNC_SHA=$source_sha; SYNC_ACTION=source-only; selected=$source
    if [[ -z $source_sha && -z $deployed_sha ]]; then
        fail "No prepared config/yolo26.rknn is available for $platform"
    elif [[ $source_sha = "$deployed_sha" ]]; then
        SYNC_ACTION=shared; next_baseline=$source_sha
    elif [[ -z $source_sha ]]; then
        selected=$deployed; SYNC_SHA=$deployed_sha; SYNC_ACTION=pulled-upload; next_baseline=$deployed_sha
    elif [[ -z $deployed_sha ]]; then
        : # Initial installation or a source-only build; no shared baseline yet.
    elif [[ -z $SHARED_SHA ]]; then
        fail 'Model conflict: source and deployed models differ without a shared baseline; both preserved'
    elif [[ $source_sha = "$SHARED_SHA" ]]; then
        selected=$deployed; SYNC_SHA=$deployed_sha; SYNC_ACTION=pulled-upload; next_baseline=$deployed_sha
    elif [[ $deployed_sha = "$SHARED_SHA" ]]; then
        SYNC_ACTION=source-changed
    else
        fail 'Model conflict: source and deployed models both changed; both preserved'
    fi
    [[ $source_sha = "$SYNC_SHA" ]] || atomic_copy "$selected" "$source" "$project_owner" "$SYNC_SHA"
    if [[ -n $next_baseline ]]; then write_baseline "$next_baseline"
    elif [[ -n $SHARED_SHA && ! -f $project/config/.last-deployed.sha256 ]]; then write_baseline "$SHARED_SHA"; fi
    normalize_config "$project" "$project_owner"
    finish_legacy_metadata
}

install_config() {
    local source target relative name entry manifest deployed_owner
    local -a files=() destinations=()
    reconcile
    ensure_directory "$deployment/config"
    deployed_owner=$(stat -c '%u:%g' -- "$deployment")
    target=$(checked_path "$deployment/config/config.txt")
    regular_or_missing "$target"
    if [[ -f $target ]]; then atomic_copy "$target" "$project/config/config.txt" "$project_owner"; fi
    normalize_config "$project" "$project_owner"
    new_temporary "${TMPDIR:-/tmp}"; manifest=$SYNC_TEMP
    find "$project/config" -mindepth 1 -print0 > "$manifest" || fail 'Cannot enumerate source configuration'
    while IFS= read -r -d '' entry; do
        relative=${entry#"$project/config/"}; name=${relative##*/}
        case $relative in models|models/*|weights|weights/*|vofa.conf|.vofa.conf.*) continue;; esac
        case $name in .model-sync.lock|.last-deployed.sha256) continue;; esac
        case ${name,,} in *.rknn|*.pt|*.onnx) continue;; esac
        source=$(checked_path "$entry")
        [[ ! -d $source ]] || continue
        [[ -f $source ]] || fail "Unsupported configuration entry: $source"
        target=$(checked_path "$deployment/config/$relative")
        regular_or_missing "$target"
        case $relative in
            cpu_freq.conf|follow.conf|car_motor_can.conf|ano.conf) [[ ! -f $target ]] || continue;;
        esac
        files+=("$source"); destinations+=("$target")
    done < "$manifest"
    for entry in "${!files[@]}"; do
        atomic_copy "${files[entry]}" "${destinations[entry]}" "$deployed_owner"
    done
    target=$deployment/config/yolo26.rknn
    if [[ ! -f $target || $(digest "$target") != "$SYNC_SHA" ]]; then
        atomic_copy "$project/config/yolo26.rknn" "$target" "$deployed_owner" "$SYNC_SHA"
    fi
    normalize_config "$deployment" "$deployed_owner"
    write_baseline "$SYNC_SHA"
    prune_legacy_directories "$deployment"
    SYNC_ACTION=deployed
}

command=${1:-}; [[ -n $command ]] || fail 'A command is required'
shift
project=''; deployment=/opt/autonomous-driving-car; platform=''; model=''
compatible=/proc/device-tree/compatible
while (($#)); do
    [[ $# -ge 2 ]] || fail "Missing value for $1"
    case $1 in
        --project) project=$2;; --deployment) deployment=$2;; --platform) platform=$2;;
        --model) model=$2;; --compatible) compatible=$2;; *) fail "Unknown option: $1";;
    esac
    shift 2
done
case $command in
    detect-platform)
        [[ -f $compatible ]] || fail "Cannot read device-tree compatible: $compatible"
        while IFS= read -r -d '' token || [[ -n $token ]]; do
            [[ $token = rockchip,* ]] || continue
            if found=$(normalize_platform "${token#rockchip,}"); then printf '%s\n' "$found"; exit 0; fi
        done < "$compatible"
        fail 'No supported Rockchip platform in device-tree compatible';;
    normalize-platform)
        normalize_platform "$platform" || fail "Unsupported model platform: $platform";;
    validate)
        platform=$(normalize_platform "$platform") || fail 'A supported --platform is required'
        [[ -n $model ]] || fail '--model is required'
        model=$(checked_path "$model")
        validate_model "$model" "$platform"
        printf '[MODEL] validated %s (%s): %s\n' "$model" "$platform" "$MODEL_SHA";;
    reconcile|install-config)
        platform=$(normalize_platform "$platform") || fail 'A supported --platform is required'
        [[ -n $project ]] || fail '--project is required'
        project=$(checked_path "$project"); deployment=$(checked_path "$deployment")
        [[ -d $project && $project != "$deployment" ]] || fail 'Source must be an existing directory different from deployment'
        lock_source
        if [[ $command = install-config ]]; then install_config; else reconcile; fi
        printf '[MODEL] %s config/yolo26.rknn (%s): %s\n' "$SYNC_ACTION" "$platform" "$SYNC_SHA";;
    *) fail "Unknown command: $command";;
esac
