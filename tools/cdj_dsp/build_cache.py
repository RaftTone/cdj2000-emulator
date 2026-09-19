"""Content-addressed native replay builds, copied into each run's snapshot.

Dependency discovery includes system headers. A cache hit never trusts mtimes
or an executable without checking its digest. The compiler and source inputs
are local trusted development tools; this is not a remote artifact cache.
"""
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import tempfile


def file_digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def build_native(cc, directory, sources, *, cache, optimization='-O2'):
    directory = Path(directory)
    binary = directory / 'replay'
    flags = [optimization, '-std=c11', '-Wall', '-Wextra', '-Werror']
    command = [cc, *flags, '-I', str(directory), *map(str, sources)]
    # Ask the compiler rather than maintaining a partial list of SDK headers.
    dependency_text = subprocess.check_output(
        [*command, '-M', '-MT', 'cdjdeps'], text=True)
    dependencies = sorted({Path(token).resolve() for token in
                           shlex.split(dependency_text.replace('\\\n', ' '))
                           if token != 'cdjdeps:'})

    def dependency_hashes():
        return {('snapshot/' + p.name if p.parent == directory.resolve() else str(p)):
                file_digest(p) for p in dependencies}

    hashes = dependency_hashes()
    environment = {k: os.environ.get(k) for k in (
        'PATH', 'SDKROOT', 'DEVELOPER_DIR', 'CPATH', 'C_INCLUDE_PATH', 'LIBRARY_PATH',
        'MACOSX_DEPLOYMENT_TARGET', 'GCC_EXEC_PREFIX', 'COMPILER_PATH',
        'CDJ_REPLAY_CACHE_EPOCH')}
    identity = dict(recipe=1, compiler=str(Path(cc).resolve()),
                    compiler_sha256=file_digest(cc),
                    compiler_version=subprocess.check_output([cc, '--version'], text=True),
                    target=subprocess.check_output([cc, '-dumpmachine'], text=True).strip(),
                    host=platform.platform(), flags=flags,
                    sources=[p.name for p in sources], dependencies=hashes,
                    environment_sha256=hashlib.sha256(json.dumps(
                        environment, sort_keys=True).encode()).hexdigest())
    key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    entry = Path(cache) / key if cache is not None else None
    hit = False
    if entry is not None:
        try:
            stored = json.loads((entry / 'build.json').read_text())
            if stored['identity'] == identity:
                shutil.copyfile(entry / 'replay', binary)
                hit = file_digest(binary) == stored['binary_sha256']
        except (OSError, ValueError, KeyError, TypeError):
            pass
    if not hit:
        subprocess.run([*command, '-o', str(binary), '-lm'], check=True)
        if dependency_hashes() != hashes:
            raise RuntimeError('compiler inputs changed during replay build; retry')
    binary.chmod(0o700)
    binary_hash = file_digest(binary)
    if not hit and entry is not None:
        entry.parent.mkdir(parents=True, exist_ok=True)
        # Publish complete entries atomically. Concurrent identical builders
        # may race; either complete entry is valid and each run uses its copy.
        with tempfile.TemporaryDirectory(prefix='.build-', dir=entry.parent) as temp:
            staged = Path(temp) / 'entry'
            staged.mkdir()
            shutil.copyfile(binary, staged / 'replay')
            (staged / 'build.json').write_text(json.dumps(dict(
                identity=identity, binary_sha256=binary_hash), sort_keys=True) + '\n')
            try:
                staged.rename(entry)
            except OSError:
                if not entry.is_dir():
                    raise
                # An existing corrupt entry is not trusted; this run already
                # compiled its own verified binary. Do not delete a rival's
                # files or publish a partially replaced executable.
    return binary, dict(cache_key=key, cache_hit=hit, optimization=optimization,
                        binary_sha256=binary_hash, compiler=identity['compiler'],
                        compiler_version=identity['compiler_version'],
                        target=identity['target'], flags=flags,
                        dependency_sha256=hashes,
                        environment_sha256=identity['environment_sha256'])
