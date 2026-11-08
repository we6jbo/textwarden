#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
if command -v t14-finish >/dev/null 2>&1; then t14-finish deadline || echo 'Deadline command returned an error; continuing.'; else echo 't14-finish not installed; continuing.'; fi
command -v python3 >/dev/null || { echo 'python3 missing' >&2; exit 1; }
command -v g++ >/dev/null || { echo 'g++ missing (install gcc)' >&2; exit 1; }
python3 - <<'PY'
from pathlib import Path
import hashlib
src=Path('mainwindow.cpp')
twu_path=Path('sufficiently_educated.twu')
if not twu_path.is_file():
    source=src.read_text(encoding='ascii')
    if source.count('// BEGIN TWU EMBEDDED') != 1 or source.count('// END TWU EMBEDDED') != 1:
        raise SystemExit('Missing embedded TWU data markers')
    if 'static constexpr std::string_view kTwu = R"TWUDATA(' not in source:
        raise SystemExit('No embedded TWU found; cannot build without .twu')
    print('Using existing embedded TWU; no external .twu required.')
    raise SystemExit(0)
twu=twu_path.read_bytes()
if not twu.startswith(b'TWU1\n') or any(c>127 for c in twu): raise SystemExit('Invalid non-ASCII TWU data')
if b')TWUDATA"' in twu: raise SystemExit('Unsafe raw-string delimiter')
s=src.read_text(encoding='ascii')
start='// BEGIN TWU EMBEDDED\n'; end='// END TWU EMBEDDED'
if s.count(start)!=1 or s.count(end)!=1: raise SystemExit('Invalid source markers')
a=s.index(start)+len(start); b=s.index(end,a)
new=s[:a]+'static constexpr std::string_view kTwu = R"TWUDATA('+twu.decode('ascii')+')TWUDATA";\n'+s[b:]
if new!=s:
    temp=src.with_suffix('.cpp.tmp');temp.write_text(new,encoding='ascii');temp.replace(src)
print('Injected TWU bytes:',len(twu),'SHA-256:',hashlib.sha256(twu).hexdigest())
PY
# Build atomically: retain previously working executable on compilation failure.
g++ -std=c++17 -O2 -Wall -Wextra -pedantic main.cpp mainwindow.cpp -o textwarden.tmp
mv -f textwarden.tmp textwarden
./textwarden stats
./textwarden count programming
./textwarden help > textwarden-help.tmp
if grep -qi screenshot textwarden-help.tmp; then echo "Screenshot command unexpectedly present" >&2; rm -f textwarden-help.tmp; exit 1; fi
rm -f textwarden-help.tmp
./textwarden sentence 100 >/dev/null
./textwarden random 5 >/dev/null
./textwarden publish-plan > textwarden-publish-plan.tmp
grep -q 'No files have been uploaded' textwarden-publish-plan.tmp
rm -f textwarden-publish-plan.tmp
printf 'Build and smoke tests passed: %s/textwarden\n' "$ROOT"
# Validate at least two resumptions for both search and count without touching
# the real HOME, reminder state, job checkpoints, or finalization markers.
python3 "$ROOT/test_continuation.py"


python3 "$ROOT/test_schedule.py"




python3 "$ROOT/test_release020.py"
