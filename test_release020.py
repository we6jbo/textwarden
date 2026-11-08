import os
from pathlib import Path
import subprocess
import tempfile
from unittest import mock
try:
    import textwarden_mail as mail
except ModuleNotFoundError as exc:
    if exc.name != "textwarden_mail":
        raise
    mail = None
try:
    import pastor_outreach as po
except ModuleNotFoundError as exc:
    if exc.name != "pastor_outreach":
        raise
    po = None

ROOT=Path(__file__).resolve().parent
BIN=ROOT/'textwarden'
with tempfile.TemporaryDirectory() as tmp:
    env={**os.environ,'HOME':tmp,'XDG_DATA_HOME':tmp+'/state','TEXTWARDEN_TEST_NOW':'2026-11-08T13:00'}
    p=subprocess.run([BIN,'help'],env=env,text=True,capture_output=True,check=True)
    assert 'v0.2.0' in p.stdout
    import json
    f=Path(tmp)/'state/textwarden/version.json'
    data=json.loads(f.read_text());assert data['version']=='0.2.0'
    assert len(data['history'])==1
    # Preserve all historical records and update from v0.1.8, not a fixed lower bound.
    data['version']='0.1.8';data['history'].insert(0,{'date':'11-08-2026','time':'09:09AM','version':'0.1.0'})
    f.write_text(json.dumps(data));subprocess.run([BIN,'help'],env=env,check=True,capture_output=True)
    result=json.loads(f.read_text())
    assert result['version']=='0.2.0' and result['history'][0]['version']=='0.1.0'
    assert sum(x['version']=='0.2.0' for x in result['history'])==1
    print('PASS version history merge and automatic v0.2.0 migration')
    assert '3.9' in subprocess.run([BIN,'chapters'],env=env,capture_output=True,text=True).stdout
    assert 'programming' in subprocess.run([BIN,'chapter','3.9'],env=env,capture_output=True,text=True).stdout
    assert 'Matching sentences:' in subprocess.run([BIN,'search-chapter','3.9','programming'],env=env,capture_output=True,text=True).stdout
    print('PASS chapter navigation and chapter-scoped search without changing TWU')
    p=subprocess.run([BIN,'pastor-reminder'],env=env,capture_output=True,text=True,check=True)
    assert 'scheduled' in p.stdout
    env['TEXTWARDEN_TEST_NOW']='2026-11-15T09:00'
    assert 'PASTOR KURT OUTREACH' in subprocess.run([BIN,'pastor-reminder'],env=env,capture_output=True,text=True,check=True).stdout
    print('PASS initial pastor event is time gated')
if mail is not None:
    assert mail.prohibited('dt-in RQ:2026-10-05-095719')
    assert mail.prohibited('Re: DT-xyz')
else:
    print('SKIP optional email client privacy tests')
if po is None:
    print('SKIP private pastor outreach tests (module not distributed)')
else:
    msg=po.outgoing()
    assert msg['To']==po.DEST and msg['Subject']==po.SUBJECT
    assert 'Jeremiah asked me' in msg.get_content() and 'Avery Rowan' in msg.get_content()
    assert 'https://zenodo.org/records/7783660' in msg.get_content()
    with mock.patch.object(po,'due',return_value=False):
        try:po.send(True);assert False
        except RuntimeError:pass
    print('PASS privacy exclusions, pastor preview, and pre-date send prohibition')
    # Prove approved one-time SMTP path is idempotent without real network or credentials.
    with tempfile.TemporaryDirectory() as tmp:
        original=po.STATE
        po.STATE=Path(tmp)/'pastor.json'
        try:
            with mock.patch.object(po,'due',return_value=True), mock.patch.object(po.mail,'smtp') as smtp, mock.patch.object(po.mail,'audit'):
                assert po.send(True)==0
                smtp.assert_called_once()
                assert po.get_state()['submitted'] is True
                assert po.send(True)==2
                smtp.assert_called_once()
                print('PASS pastor one-time send is fail-closed and duplicate-safe (mock SMTP only)')
        finally:po.STATE=original
