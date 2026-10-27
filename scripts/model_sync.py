#!/usr/bin/env python3
"""Keep conversion, uploads and deployment on config/yolo26.rknn.

RKNN artifacts are preserved on conflicts. The source baseline records the last
shared/deployed bytes; independently edited models require explicit resolution.
Conversion inputs are removed only when the build requests post-build cleanup.
"""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import tempfile

PLATFORM = re.compile(r"(?:rk|rv)\d{4}[a-z]*\Z")
SHA = re.compile(r"[0-9a-f]{64}\Z")
KNOWN_PLATFORMS = ("rk3588", "rk3576", "rk3568", "rk3566", "rk3562", "rv1126",
                   "rv1109", "rv1106", "rv1103", "rk3399pro", "rk1808")


class ModelSyncError(RuntimeError):
    pass


class ModelPlatformError(ModelSyncError):
    pass


def platform_name(value):
    if not PLATFORM.fullmatch(value):
        raise ModelSyncError("Invalid model platform: " + value)
    for base in KNOWN_PLATFORMS:
        if value.startswith(base) and re.fullmatch(r"[a-z]*", value[len(base):]):
            return base
    raise ModelSyncError("Unsupported model platform: " + value)


def detect_platform(data):
    for token in data.split(b"\0"):
        if not token.startswith(b"rockchip,"):
            continue
        try:
            return platform_name(token[9:].decode("ascii"))
        except (UnicodeDecodeError, ModelSyncError):
            continue
    raise ModelSyncError("No supported Rockchip platform in device-tree compatible")


def canonical_relative(platform):
    platform_name(platform)
    return Path("config/yolo26.rknn")


def baseline_relative():
    return Path("config/models/.last-deployed.sha256")


def checked(root, relative):
    root = Path(root).resolve()
    target = root / relative
    if not target.resolve().is_relative_to(root) or any(p.is_symlink() for p in [target, *target.parents] if p != root):
        raise ModelSyncError("Model/config path crosses a symlink: " + str(target))
    return target


def digest(path):
    if path is None or not path.exists():
        return None
    if not path.is_file() or path.is_symlink():
        raise ModelSyncError("Expected a regular model file: " + str(path))
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def validate_model(path, platform):
    platform = platform_name(platform)
    with Path(path).open("rb") as stream:
        data = stream.read()
    if not data.startswith(b"RKNN"):
        raise ModelSyncError("Invalid RKNN model header: " + str(path))
    targets = set()
    for match in re.findall(rb"(?:rk|rv)\d{4}[a-z]*", data.lower()):
        try:
            targets.add(platform_name(match.decode("ascii")))
        except ModelSyncError:
            continue
    if platform not in targets:
        raise ModelPlatformError("RKNN model does not report target " + platform + ": " + str(path))
    return hashlib.sha256(data).hexdigest()


def owner(root):
    status = Path(root).stat()
    return status.st_uid, status.st_gid


def ensure_directory(path, source_owner=None):
    missing = []
    current = path
    while not current.exists():
        missing.append(current)
        current = current.parent
    path.mkdir(mode=0o755, parents=True, exist_ok=True)
    if source_owner is not None and os.geteuid() == 0:
        for directory in missing:
            os.chown(directory, *source_owner)


def atomic_bytes(path, data, source_owner=None, mode=0o644):
    if path.is_symlink():
        raise ModelSyncError("Refusing to replace a symlink: " + str(path))
    ensure_directory(path.parent, source_owner)
    fd, temporary = tempfile.mkstemp(prefix=".model-sync-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        if source_owner is not None and os.geteuid() == 0:
            os.chown(temporary, *source_owner)
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def atomic_copy(source, destination, expected_sha=None, source_owner=None):
    data = source.read_bytes()
    checksum = hashlib.sha256(data).hexdigest()
    if expected_sha is not None and checksum != expected_sha:
        raise ModelSyncError("Model changed during synchronization: " + str(source))
    atomic_bytes(destination, data, source_owner)
    return checksum


def configured_model(root):
    config = checked(root, Path("config/config.txt"))
    if not config.exists():
        return None
    selected = None
    for line in config.read_text(encoding="utf-8").splitlines():
        fields = line.split("#", 1)[0].split()
        if len(fields) >= 2 and fields[0] == "ai_model":
            selected = fields[1]
    if selected is None:
        return None
    candidate = Path(selected)
    if candidate.is_absolute():
        raise ModelSyncError("Configured model must be relative to the installation: " + selected)
    path = checked(root, candidate)
    if not path.resolve().is_relative_to((Path(root) / "config").resolve()):
        raise ModelSyncError("Configured model is outside config/: " + selected)
    return path if path.is_file() else None


def normalize_config(root, platform, source_owner=None):
    path = checked(root, Path("config/config.txt"))
    if not path.exists():
        return
    text = path.read_text(encoding="utf-8")
    desired = canonical_relative(platform).as_posix()
    lines = text.splitlines(keepends=True)
    found = False
    for i, line in enumerate(lines):
        fields = line.split("#", 1)[0].split()
        if fields and fields[0] == "ai_model":
            lines[i] = "ai_model " + desired + "\n"
            found = True
    if not found:
        if lines and not lines[-1].endswith("\n"):
            lines[-1] += "\n"
        lines.append("ai_model " + desired + "\n")
    changed = "".join(lines).encode("utf-8")
    if changed != text.encode("utf-8"):
        atomic_bytes(path, changed, source_owner, stat.S_IMODE(path.stat().st_mode))


@contextmanager
def sync_lock(project):
    path = checked(project, Path("config/models/.model-sync.lock"))
    ensure_directory(path.parent, owner(project))
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW, 0o644)
    try:
        if os.geteuid() == 0:
            os.fchown(fd, *owner(project))
        fcntl.flock(fd, fcntl.LOCK_EX)
        yield
    finally:
        os.close(fd)


def source_candidate(project, platform):
    canonical = checked(project, canonical_relative(platform))
    if canonical.exists():
        return canonical
    legacy = Path("config/models") / platform_name(platform) / "yolo26.rknn"
    for candidate in (configured_model(project), checked(project, legacy)):
        if candidate is None or not candidate.exists():
            continue
        try:
            validate_model(candidate, platform)
            return candidate
        except ModelPlatformError:
            continue
    return None


def deployed_candidate(deployment, platform):
    # Before migration, an upload may have changed the configured legacy file
    # while the platform cache still contains older bytes. Honor the active path.
    configured = configured_model(deployment)
    if configured is not None:
        try:
            validate_model(configured, platform)
            return configured
        except ModelPlatformError:
            pass
    canonical = checked(deployment, canonical_relative(platform))
    if canonical.exists():
        return canonical
    legacy = checked(deployment, Path("config/models") / platform_name(platform) / "yolo26.rknn")
    if legacy.exists():
        try:
            validate_model(legacy, platform)
            return legacy
        except ModelPlatformError:
            pass
    return None


def _reconcile(project, deployment, platform):
    relative = canonical_relative(platform)
    source = source_candidate(project, platform)
    deployed = deployed_candidate(deployment, platform)
    source_sha = validate_model(source, platform) if source is not None else None
    deployed_sha = validate_model(deployed, platform) if deployed is not None else None
    baseline = checked(project, baseline_relative())
    legacy_baseline = checked(project, Path("config/models") / platform_name(platform) / ".last-deployed.sha256")
    baseline_source = baseline if baseline.exists() else legacy_baseline
    shared = baseline_source.read_text().strip() if baseline_source.exists() else None
    if shared is not None and not SHA.fullmatch(shared):
        raise ModelSyncError("Invalid model synchronization baseline: " + str(baseline))
    selected, selected_sha, action, new_baseline = source, source_sha, "source-only", None
    if source_sha is None and deployed_sha is None:
        return {"action": "missing", "path": relative.as_posix(), "sha256": None}
    if source_sha == deployed_sha:
        action, new_baseline = "shared", source_sha
    elif source_sha is None:
        selected, selected_sha, action, new_baseline = deployed, deployed_sha, "pulled-upload", deployed_sha
    elif deployed_sha is None:
        pass
    elif shared is None:
        raise ModelSyncError("Model conflict: source and deployed models differ without a shared SHA baseline; both files were preserved")
    elif source_sha == shared:
        selected, selected_sha, action, new_baseline = deployed, deployed_sha, "pulled-upload", deployed_sha
    elif deployed_sha == shared:
        action = "source-changed"
    else:
        raise ModelSyncError("Model conflict: source and deployed models both changed since the shared SHA baseline; both files were preserved")
    canonical = checked(project, relative)
    if digest(canonical) != selected_sha:
        atomic_copy(selected, canonical, selected_sha, owner(project))
    if new_baseline is not None or (shared is not None and not baseline.exists()):
        atomic_bytes(baseline, ((new_baseline or shared) + "\n").encode(), owner(project))
    normalize_config(project, platform, owner(project))
    return {"action": action, "path": relative.as_posix(), "sha256": selected_sha}


def reconcile(project, deployment, platform):
    with sync_lock(project):
        return _reconcile(project, deployment, platform)


def install_config(project, deployment, platform):
    """Run after the service stops, and recheck uploads before copying config."""
    with sync_lock(project):
        state = _reconcile(project, deployment, platform)
        if state["sha256"] is None:
            raise ModelSyncError("No canonical model is available for deployment")
        source_config = checked(project, Path("config/config.txt"))
        deployed_config = checked(deployment, Path("config/config.txt"))
        if deployed_config.exists():
            atomic_bytes(source_config, deployed_config.read_bytes(), owner(project))
        normalize_config(project, platform, owner(project))
        source_root = checked(project, Path("config"))
        # Conversion inputs, old caches and SHA bookkeeping never enter /opt.
        for source in source_root.rglob("*"):
            relative = source.relative_to(source_root)
            if relative.parts[0] == "models" or source.suffix.lower() in (".rknn", ".pt", ".onnx"):
                continue
            if source.is_symlink():
                raise ModelSyncError("Refusing a symlink in configuration: " + str(source))
            if source.is_file():
                destination = checked(deployment, Path("config") / relative)
                atomic_bytes(destination, source.read_bytes(), mode=stat.S_IMODE(source.stat().st_mode))
        relative = canonical_relative(platform)
        source = checked(project, relative)
        destination = checked(deployment, relative)
        if digest(destination) != state["sha256"]:
            atomic_copy(source, destination, state["sha256"])
        normalize_config(deployment, platform)
        atomic_bytes(checked(project, baseline_relative()),
                     (state["sha256"] + "\n").encode(), owner(project))
        return {**state, "action": "deployed"}


def publish_converted(project, platform, artifact):
    checksum = validate_model(artifact, platform)
    with sync_lock(project):
        destination = checked(project, canonical_relative(platform))
        atomic_copy(Path(artifact), destination, checksum, owner(project))
        normalize_config(project, platform, owner(project))
    return {"action": "converted", "path": canonical_relative(platform).as_posix(), "sha256": checksum}


def checked_weight(path):
    path = Path(path).absolute()
    if any(ord(character) < 32 for character in str(path)):
        raise ModelSyncError("Weight filenames cannot contain control characters")
    if path.is_symlink() or any(parent.is_symlink() for parent in path.parents):
        raise ModelSyncError("Refusing a symlinked weight path: " + str(path))
    if not path.is_file() or path.suffix != ".pt":
        raise ModelSyncError("Expected a regular .pt weight: " + str(path))
    return path.resolve(strict=True)


def config_weights(project):
    config = checked(project, Path("config"))
    if not config.exists():
        return []
    candidates = []
    for directory, subdirectories, files in os.walk(config, followlinks=False):
        for name in subdirectories:
            path = Path(directory) / name
            if path.is_symlink() or not path.resolve().is_relative_to(config):
                raise ModelSyncError("Refusing a symlinked/escaped weight directory: " + str(path))
        for name in files:
            if not name.endswith(".pt"):
                continue
            path = checked_weight(Path(directory) / name)
            if not path.is_relative_to(config):
                raise ModelSyncError("Weight escaped config/: " + str(path))
            candidates.append(path)
    return sorted(candidates)


def discover_weights(project, explicit=None):
    """Select a user-specified weight or the sole PT anywhere under config/."""
    if explicit is None:
        explicit = os.environ.get("YOLO_WEIGHTS") or None
    if explicit:
        selected = Path(explicit)
        possibilities = [selected] if selected.is_absolute() else [Path.cwd() / selected, Path(project) / selected]
        for possibility in possibilities:
            if possibility.exists() or possibility.is_symlink():
                path = checked_weight(possibility)
                return {"path": str(path), "source": "explicit", "requires_conversion": True}
        if selected.name != explicit or selected.suffix != ".pt":
            raise ModelSyncError("Explicit weight file not found: " + explicit)
        matches = [path for path in config_weights(project) if path.name == explicit]
        if len(matches) > 1:
            raise ModelSyncError("Multiple weights match YOLO_WEIGHTS; specify a full path:\n" + "\n".join(map(str, matches)))
        return {"path": str(matches[0]) if matches else explicit, "source": "explicit", "requires_conversion": True}
    candidates = config_weights(project)
    if len(candidates) > 1:
        raise ModelSyncError("Multiple .pt weights under config/; set YOLO_WEIGHTS to the intended file:\n" + "\n".join(map(str, candidates)))
    return {"path": str(candidates[0]) if candidates else None,
            "source": "config" if candidates else "none", "requires_conversion": bool(candidates)}


def cleanup_weights(project, used_weight=None):
    """Delete cache copies and the selected config/ PT after successful build."""
    config = checked(project, Path("config"))
    cache = checked(project, Path("config/models/weights"))
    if cache.exists() and (not cache.is_dir() or cache.is_symlink()):
        raise ModelSyncError("Expected a regular conversion-cache directory: " + str(cache))
    candidates = set()
    if cache.exists():
        for candidate in cache.iterdir():
            if candidate.suffix != ".pt":
                continue
            if candidate.is_symlink() or not candidate.resolve().is_relative_to(cache) or not stat.S_ISREG(candidate.lstat().st_mode):
                raise ModelSyncError("Refusing an unsafe cached weight: " + str(candidate))
            candidates.add(candidate)
    protected = None
    if used_weight:
        used = Path(used_weight)
        if not used.is_absolute():
            used = Path.cwd() / used
        if used.is_relative_to(config):
            if not used.resolve().is_relative_to(config):
                raise ModelSyncError("Selected weight escaped config/: " + str(used))
            if used.exists() or used.is_symlink():
                candidates.add(checked_weight(used))
        else:
            protected = str(used)
    # Validate every target before deleting any file.
    for candidate in candidates:
        candidate.unlink()
    if cache.exists() and not any(cache.iterdir()):
        cache.rmdir()
    return {"action": "weights-cleaned", "removed": len(candidates), "protected_external": protected}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("reconcile", "install-config", "publish-converted", "validate", "normalize-platform", "detect-platform", "cleanup-weights", "discover-weights"))
    parser.add_argument("--project", type=Path)
    parser.add_argument("--deployment", type=Path, default=Path("/opt/rockchip-smart-gateway"))
    parser.add_argument("--platform")
    parser.add_argument("--compatible", type=Path, default=Path("/proc/device-tree/compatible"))
    parser.add_argument("--model", type=Path)
    parser.add_argument("--used-weight")
    parser.add_argument("--explicit")
    parser.add_argument("--plain", action="store_true")
    args = parser.parse_args()
    try:
        if args.command == "detect-platform":
            print(detect_platform(args.compatible.read_bytes()))
            return
        if args.command == "cleanup-weights":
            if args.project is None:
                parser.error("--project is required")
            print(json.dumps(cleanup_weights(args.project.resolve(strict=True), args.used_weight)))
            return
        if args.command == "discover-weights":
            if args.project is None:
                parser.error("--project is required")
            result = discover_weights(args.project.resolve(strict=True), args.explicit)
            print((result["path"] or "") if args.plain else json.dumps(result))
            return
        if args.platform is None:
            parser.error("--platform is required")
        args.platform = platform_name(args.platform)
        if args.command == "normalize-platform":
            print(args.platform)
            return
        if args.command == "validate":
            if args.model is None: parser.error("--model is required")
            result = {"sha256": validate_model(args.model, args.platform)}
        else:
            if args.project is None: parser.error("--project is required")
            project = args.project.resolve(strict=True)
            deployment = args.deployment.resolve()
            if project == deployment:
                raise ModelSyncError("Source and deployment directories must differ")
            if args.command == "publish-converted":
                if args.model is None: parser.error("--model is required")
                result = publish_converted(project, args.platform, args.model)
            elif args.command == "install-config":
                result = install_config(project, deployment, args.platform)
            else:
                result = reconcile(project, deployment, args.platform)
        print(json.dumps(result))
    except (ModelSyncError, OSError) as error:
        parser.exit(1, "[MODEL] " + str(error) + "\n")


if __name__ == "__main__":
    main()
