#!/usr/bin/env python3
"""Offline schedule checks; isolated HOME; no network or Git changes."""
import os, subprocess, tempfile
from pathlib import Path
binary=Path(__file__).with_name('textwarden')
def cli(home,stamp,command='stats'):
    env=os.environ.copy()
    env.update(HOME=str(home), XDG_DATA_HOME=str(home/'data'), TEXTWARDEN_TEST_NOW=stamp)
    p=subprocess.run([str(binary),command],text=True,capture_output=True,env=env,timeout=15)
    assert p.returncode==0,(p.stdout,p.stderr)
    return p.stdout
with tempfile.TemporaryDirectory(prefix='textwarden-scheduler-') as temp:
    h=Path(temp)
    o=cli(h,'2026-11-08T10:29')
    assert 'PUBLISHER RESEARCH REMINDER' not in o and 'TEXTWARDEN GIT REVIEW' not in o
    o=cli(h,'2026-11-08T10:31')
    assert 'PUBLISHER RESEARCH REMINDER [2026-11-08-1030]' in o
    assert 'PUBLISHER RESEARCH REMINDER' not in cli(h,'2026-11-08T10:32')
    assert '2026-11-08-1300' in cli(h,'2026-11-08T13:01')
    assert 'ONE-TIME NOV 8' in cli(h,'2026-11-08T14:00')
    assert 'TEXTWARDEN GIT REVIEW' not in cli(h,'2026-11-08T14:01')
    assert '2026-11-08-2130' in cli(h,'2026-11-09T11:00')
    assert '2026-11-08-2000' not in cli(h,'2026-11-09T11:01')
    assert '2026-11-22-2000' in cli(h,'2026-11-23T11:00')
    assert '2026-11-29-2000' in cli(h,'2026-11-30T11:00')
    assert '2026-12-05-2000' in cli(h,'2026-12-06T11:00')
    assert '2026-12-20-2000' in cli(h,'2026-12-21T11:00')
    assert 'TEXTWARDEN GIT REVIEW' not in cli(h,'2026-12-21T11:01')
    print('PASS publisher and Git reminders, catchup, newest-slot-only, no duplicates')
with tempfile.TemporaryDirectory(prefix='textwarden-late-') as temp:
    h=Path(temp)
    o=cli(h,'2027-01-01T09:00')
    assert 'ONE-TIME NOV 8' in o, o
    assert '2026-12-20-2000' in o, o
    assert '2026-12-19-2000' in o  # publisher last completed Saturday
    assert 'TEXTWARDEN GIT REVIEW' not in cli(h,'2027-01-01T09:01')
    assert 'ONE-TIME NOV 8' not in cli(h,'2027-01-22T12:00')
    print('PASS late first-review delivery alongside newest recurring event')
with tempfile.TemporaryDirectory(prefix='textwarden-queued-') as temp:
    h=Path(temp)
    cli(h,'2026-11-29T20:00','git-review-queue')
    state=h/'data/textwarden'
    assert not (state/'git_review_first_done.txt').exists()
    assert not (state/'git_review_periodic_slot.txt').exists()
    o=cli(h,'2026-12-21T12:00')
    assert 'ONE-TIME NOV 8' in o and '2026-12-20-2000' in o
    assert '2026-11-29-2000' not in o
    print('PASS timer queue does not consume undelivered reminder')
with tempfile.TemporaryDirectory(prefix='textwarden-mail-') as temp:
    h=Path(temp)
    assert 'MAIL CHECK REMINDER' not in cli(h,'2026-11-15T19:59')
    o=cli(h,'2026-11-15T20:00')
    assert 'Mail check pending [2026-11-15-2000]' in o,o
    assert 'Mail check pending [2026-11-15-2000]' in cli(h,'2026-11-16T13:00')
    assert 'MAIL CHECK REMINDER [2026-11-15-2000]' in cli(h,'2026-11-16T13:00','mail-reminder')
    assert 'Mail check pending' not in cli(h,'2026-11-16T13:01')
    assert '2026-12-06-2000' in cli(h,'2026-12-07T11:00')
    assert 'MAIL CHECK REMINDER [2026-12-06-2000]' in cli(h,'2026-12-07T11:01','mail-reminder')
    assert 'Mail check pending' not in cli(h,'2026-12-07T11:02')
    assert '2027-01-17-2000' in cli(h,'2027-01-18T11:00')
    print('PASS 21-day mail reminder and missed-slot suppression')
