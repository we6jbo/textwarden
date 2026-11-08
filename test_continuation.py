#!/usr/bin/env python3
"""Offline CLI integration tests for multiple real checkpoint/resume invocations."""
from pathlib import Path
import os, re, subprocess, sys, tempfile, json
binary = Path(__file__).resolve().parent / 'textwarden'
if not binary.is_file(): sys.exit('Compile textwarden first with ./build_textwarden.sh')

def run(args, env):
    p = subprocess.run([str(binary), *args], env=env, text=True, capture_output=True, timeout=20)
    if p.returncode not in (0,75): raise AssertionError((args,p.returncode,p.stdout[-1000:],p.stderr))
    return p

with tempfile.TemporaryDirectory(prefix='textwarden-qa-') as home:
    env = os.environ.copy()
    env.update(HOME=home, XDG_DATA_HOME=str(Path(home)/'data'),
               TEXTWARDEN_TEST_NOW='2026-11-08T13:59',
               TEXTWARDEN_SLICE_MS='200', TEXTWARDEN_TEST_DELAY_MS='5')
    for command,expected in [('count','38'),('search','31')]:
        args=[command,'programming']
        iterations=0; checkpoints=0; prev=0; output=[]
        while True:
            p=run(args, env)
            output.append(p.stdout)
            iterations+=1
            if p.returncode == 0:
                assert f": {expected}\n" in p.stdout, p.stdout[-600:]
                break
            checkpoints+=1
            m=re.search(r'textwarden continue ([0-9a-f-]+)',p.stdout)
            assert m, p.stdout[-900:]
            partial=re.search(r'Partial (?:occurrences|matching passages): (\d+)', p.stdout)
            assert partial and int(partial.group(1)) >= prev
            prev=int(partial.group(1))
            cp=Path(home)/'.local/state/textwarden/jobs'/f'{m.group(1)}.job'
            assert cp.is_file(), f'checkpoint missing: {cp}'
            args=['continue',m.group(1)]
            assert iterations < 100, 'too many continuation attempts'
        assert checkpoints >= 2, f'{command}: expected >=2 pauses; saw {checkpoints}'
        assert not list((Path(home)/'.local/state/textwarden/jobs').glob('*.job'))
        print(f'PASS {command}: {checkpoints} saved checkpoints, {iterations} invocations, final {expected}')
    p=run(['random','20'], env)
    lines=[l for l in p.stdout.splitlines() if l.startswith('[Sentence ')]
    assert len(lines)==20
    assert all(len(l.split('] ',1)[-1])>=65 for l in lines)
    print('PASS random: 20 full-length sentences; stable numbered sentence index')
    p=run(['sentence','100'], env)
    assert 'An example of this was in the maze section.' in p.stdout
    p=run(['help'], env)
    assert 'screenshot' not in p.stdout.lower()
    assert 'v0.2.0' in p.stdout
    v=json.loads((Path(home)/'data/textwarden/version.json').read_text())
    assert v['version']=='0.2.0' and v['history'][-1]['version']=='0.2.0'
    assert not (Path(home)/'data/textwarden/finalize.touch').exists()
    print('PASS version, finalization pre-deadline, help and sentence compatibility')
    env['TEXTWARDEN_TEST_NOW']='2026-11-08T14:00'
    p=run(['stats'],env)
    assert 'DONT FINALIZE' not in p.stdout
    assert (Path(home)/'data/textwarden/finalize.touch').exists()
    print('PASS finalization at 14:00 with persistent marker')
print('PASS all offline TextWarden CLI integration tests')
