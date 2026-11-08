# TextWarden v0.2.0

TextWarden is an open-source-oriented C++17 command-line document
navigation project designed to help AI assistants and developers
explore structured and compressed book content.

The initial document is *Sufficiently Educated*, an autobiography
by Jeremiah O'Neal about his educational experiences, challenges,
and journey toward earning a college degree.

Original book:
https://zenodo.org/records/7783660

TextWarden supports sentence retrieval, inferred paragraphs,
chapter navigation, keyword searches, occurrence counting,
and resumable processing operations.

It can be used by AI assistants such as Mistral Vibe, Claude,
and Gemini through ordinary terminal commands.

Optional tools support email communication, publisher outreach,
scheduled project reviews, and GitHub maintenance. These
features have separate configuration and privacy requirements.

The book's copyright remains separate from the software's
licensing terms.

---

Pure C++17 console application. The inherited Qt Creator filenames `mainwindow.cpp`, `mainwindow.h`, and `mainwindow.ui` are preserved; Qt Widgets is **not** linked and the `.ui` is unused.

Run `./build_textwarden.sh` to validate and inject the 7-bit ASCII `sufficiently_educated.twu` data into `mainwindow.cpp`, compile an executable `textwarden`, and run smoke checks.

Examples:

```sh
./textwarden help
./textwarden search programming
./textwarden count programming
./textwarden random 5
./textwarden sentence 100
./textwarden paragraph 20
./textwarden stats
```

`./textwarden continue JOB_ID` resumes time-limited searches and counts from their saved checkpoint. Code 75 indicates paused work; code 0 completed; code 2 invalid arguments. The default processing interval is 60 seconds, and the timeout is checked between bounded chunks, so output and blocking I/O may run over this deadline. For QA only, set `TEXTWARDEN_SLICE_MS=20 TEXTWARDEN_TEST_DELAY_MS=10` to force a pause. The checkpoint lives in `$HOME/.local/state/textwarden/jobs` and does not restart at the beginning. ETA values are estimates, not guarantees.

The imported text is the **previously flattened** version. Original paragraph breaks are irrecoverable from it; each navigable paragraph is a group of four inferred sentences. The book may contain OCR errors. `sufficiently_educated.twu` is required for building, not running. Screenshot capture is not included.

Provenance reference: `TG333041` (`tg_context_snapshot.json`). No network, no ports, no registry database requirements. `backup/` contains the pre-integration files for local recovery. Publishing compressed book content may require a copyright/privacy review despite the text being compressed. GitHub publication is not automatic.

## Version and finalization tracking

At startup, the executable creates or updates `$HOME/.local/share/textwarden/version.json` (or the standard per-user application-data location on supported platforms). The version file includes the historical `11-08-2026 09:09AM / 0.1.0` record and the current release's immutable build date/time. Existing metadata for a higher version is not overwritten.

When the current *local* time reaches `2026-11-08 14:00`, OR `version.json` declares version `1.0.1` or higher, it creates `finalize.touch` and stops printing `DONT FINALIZE - KEEP WORKING`. An existing marker also suppresses the warning permanently. This is a workflow signal, **not** authorization for publication or distribution. For pre-deadline QA, set `TEXTWARDEN_TEST_NOW=2026-11-08T13:59`; this override is for testing only.

The screenshot command and helper are absent.

## Daily publication reminder (v0.1.4)

Once per local calendar day, TextWarden prints a reminder that Mistral Vibe can
propose publishing approved content for access from other computers. The
reminder day lives in the user data directory as `publish_reminder_day.txt`.
`textwarden publish-plan` prints publication priorities and human-in-the-loop
instructions for public websites, private websites, a reachable Raspberry Pi
(using SSH and fresh Tailscale login instructions), or the local publish folder.
It never uploads, probes networks, attempts SSH, launches a browser, or logs in.
Actual publication requires copyright/privacy and destination review plus
separate approval. The Raspberry Pi's private address alone cannot prove
public Internet access.

## v0.1.4: Real continuation and random sentence quality

Search and count retain the original 60-second renewable processing window.
An expired operation returns code **75**, writes a checkpoint under
`$HOME/.local/state/textwarden/jobs/`, and prints a `textwarden continue JOB_ID`
command. Re-running that command continues from the saved byte or inferred
paragraph offset. Successful completion deletes the checkpoint. No blocked
interactive confirmation is required. The ETA for `continue` now explicitly
notes that more than one processing window might be required.

`random 5` now samples only sentences long enough to be useful, filtering
obvious citation, URL, and OCR fragments. All original numbered sentences
remain accessible through `sentence NUMBER`. `search` continues to show the
full inferred paragraph and also identifies a matching sentence when possible.
The original text is unchanged and all previous sentence and paragraph IDs
remain stable.

Run `python3 test_continuation.py` to validate multiple genuine checkpoint/
resume cycles for `count` and `search`, random selection quality, the
version record, help, and the finalization threshold. The test uses a private
temporary HOME and a shortened processing slice; it never manipulates the
actual `~/.local/share/textwarden/` metadata. The production runtime uses
60-second processing slices. This limit cannot guarantee termination of a
blocked output stream or a stalled filesystem call.

## Publisher outreach (v0.1.4)

Run `textwarden publisher-plan` for AI-facing instructions and `textwarden publisher-reminder` to check whether a reminder is currently due. Automatic checks are also performed on launch. The optional `publisher-reminder.timer` allows reminders to be recorded while the CLI is closed. Note that the systemd timer queues reminders to `publisher_pending_reminders.txt` without marking them as delivered. The next normal TextWarden command prints queued reminders for Mistral Vibe and records delivery. An AI assistant is not guaranteed to see reminders until it next runs the CLI.

Reminder slots: November 8, 2026 at 10:30 AM, 1 PM, 8 PM, 9 PM and 9:30 PM; November 22 at 8 PM (Sunday exception); and alternate Saturdays starting December 5 at 8 PM local time. After a reminder is presented, it is recorded in `~/.local/share/textwarden/publisher_reminder_slot.txt` to suppress duplicates for that slot.

For your T14, run `bash setup_textwarden_mail_password.sh` to save a one-line password under `~/.config/textwarden/smtp_password` with mode 0600. Never place passwords in the ZIP, Git, TWU, or code. To prepare a draft: `python3 publisher_outreach.py draft --to verified-publisher@example.org`. To send only after reviewing the exact recipient and text: `python3 publisher_outreach.py send --to verified-publisher@example.org --approve-send`. Optional approved excerpts can be supplied with `--excerpt-file /path/to/approved-excerpt.txt`. Messages have `From: master@we6jbobbs.org` and `Reply-To: we6jbo@gmail.com`. This tool will not automatically send during scheduled reminders. Obtain permission and verify submission requirements; do not send unsolicited repeated emails.

Junior Library Guild is not a suitable direct author-submission target. Its official rules say only publishers may submit titles, and it focuses on prepublication titles. Its policies should be verified before any contact. The book is already online at https://zenodo.org/records/7783660 and must remain there; only no-fee publisher arrangements are acceptable.

## v0.1.6: Test SMTP email

To preview the exact test without sending: `bash test_textwarden_email.sh`.
To submit one test message: `bash test_textwarden_email.sh --approve-send`.
Recipient is fixed to `joneal97m@gmail.com`, subject `TextWarden email sender test`, and body is exactly `this email sender is working.` (plus a trailing newline). Uses existing protected `~/.config/textwarden/smtp_password`, `mail.privateemail.com:587` with verified STARTTLS, `From: master@we6jbobbs.org` and `Reply-To: we6jbo@gmail.com`. No book passages or other attachments are included. Check inbox and spam; SMTP acceptance does not prove final delivery.

Test sending is never automatic and is not done by installation, daily reminders, or scheduled publisher research. `python3 test_email_sender.py` mocks SMTP for offline regression testing.


## v0.1.6: offline-safe reminders and repository review

TextWarden does not send scheduled publisher pitches or automatically publish Git changes. The first CLI interaction after a due time displays the current reminder and writes the delivered slot to `~/.local/share/textwarden/`. Missed publisher reminder slots are superseded by the newest due slot. This is different from actual SMTP messages: `publisher_sent.jsonl` records explicit approved SMTP submissions only (server acceptance is not delivery).

The Nov 8, 2026 14:00 one-time Git review reminder is *never discarded* until shown. The recurring Git review happens Nov 29, Dec 20, 2026 and every 21 days at 20:00 local; older missed recurring slots are skipped. If both are due at first launch, both reminders are displayed. Files `git_review_first_done.txt` and `git_review_periodic_slot.txt` track this. `textwarden git-review` checks the reminders explicitly; `textwarden outreach-status` reports reminder and SMTP submission records. The user systemd timer can queue while the laptop is awake, but all actionable reminders are shown during the next TextWarden CLI interaction, not silently marked delivered by the timer.

A recurring Git review asks Mistral Vibe to verify the real Git origin and branch, the approved `6721.txt` marker, and remote URL contents before assessing `.gitignore` and refreshing README.md for AI agents, developers and publishers. It does not run Git or contact a publisher. Approval and licensing review must precede public changes.

## v0.1.7: Vibe IMAP / SMTP mail integration

See `MAIL_VIBE_GUIDE.md`. All outgoing messages are previews unless `--approve-send` is supplied. `textwarden_mail.py` supports safe subject-filtered IMAP reading, drafting and replying, and a 21-day opted-in connectivity test. It does not touch `DT-` subject email. On and after November 15, 2026, `textwarden mail-reminder` will show a pending notice on later CLI runs; `textwarden mail-reminder` explicitly displays and acknowledges the newest due reminder without sending email automatically. Run `bash install_vibe_mail_guide.sh` on or after November 15 to install permanent instructions into `~/.vibe/AGENTS.md` (with a backup).

The test sends to Gmail and a Bcc copy to this mailbox to verify IMAP; Gmail delivery must still be checked separately.

## v0.1.8: Consistent AI mail identity

New messages, replies, and publisher inquiries are signed as **Avery Rowan
(TextWarden AI)** with transparent AI identification and a statement that the
agent cannot bind the book author. No telephone number is inserted. Only the
public contact email `we6jbo@gmail.com` is used. Use `--context book` for book
outreach, or the default `--context general` for unrelated projects. Plain email
bodies should sound natural; the signature and disclosure are enforced by the
client. The fixed SMTP sender test message is intentionally unchanged.

## v0.2.0: Agent navigation, protected mail, and review workflow

TextWarden is a terminal-only C++17 tool for navigating a compressed copy of
*Sufficiently Educated*. AI agents such as Mistral Vibe, Claude and Gemini can
invoke its commands (`textwarden help`, `search`, `count`, `chapters`,
`chapter 3.9`, `search-chapter 3.9 programming`) to retrieve bounded context rather than repeatedly loading the
entire book. The chapter boundaries are inferred from OCR-damaged text and
may not match original printed pagination. The tool is not a language model.

Book publishers: the author is interested in legitimate traditional
publishing with **no author-funded fees**. The book will remain at
https://zenodo.org/records/7783660. Material in this repository must not be
assumed cleared for public redistribution. The compressed `.twu` file is the
book content, not anonymized metadata. Evaluate licensing and privacy before
pushing to GitHub or copying it to other services.

Reusable private agent email: `textwarden_mail.py` supports IMAP and SMTP,
preview-first sending, replies and a periodic round-trip test. Avery Rowan
(TextWarden AI) is a disclosed AI sender. Private passwords are stored only
under `~/.config/textwarden/smtp_password` with 0600 permissions. The client
must not fetch or act on mail whose subject contains `DT-`.

On or after November 15, 2026, the owner's one-time approved pastor inquiry
can be previewed and sent using `pastor_outreach.py`, with reply checks at
subsequent Vibe interactions. Message contents and activity logs stay private.
The November 8 Git audit and 21-day Git audit guidance are available through
`textwarden git-review`. Use `python3 git_review.py` to inspect the local Git
configuration and online 6721 marker, without committing or pushing. On or
after November 29 `git_review.py --create-marker` can create the marker
locally, but committing/pushing requires explicit user approval.

All scheduled tasks use persistent local reminders and catch-up on the next
CLI interaction. They do not require Vibe to be running continuously. Missed
recurring events are coalesced, while the unique pastor message is not lost.
