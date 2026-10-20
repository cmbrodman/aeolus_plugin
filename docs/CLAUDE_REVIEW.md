# Automatic Claude reviews from Codex

`tools/claude_review.py` lets Codex prepare, execute and retrieve reviews directly
using the existing snapshot and result-validation workflow. Direct Claude CLI and
Orca runtime connectivity were verified in this environment on October 19, 2026;
execution uses the current environment's actual access, not that historical check
as a guarantee. Orca is optional. The wrapper uses Python 3.9+, Git and Claude CLI,
without installing dependencies. Execution requires POSIX process groups (Linux
or macOS); preparation and result inspection do not launch Claude.

## When Codex invokes review

Codex owns implementation, test execution and fixes. Claude independently reviews
the supplied snapshot and reports actionable findings; it does not edit source or
run fixes. Codex checks each finding against the code, decides what is within the
authorized task, applies necessary fixes and validates the result.

[AGENTS.md](../AGENTS.md#automatic-claude-review-workflow) instructs Codex to invoke
review automatically after substantial code changes and relevant local checks,
before final delivery, unless the user explicitly opts out. Changes to real-time
audio, voice allocation/lifetimes, threading, synchronization or performance need
review even for a small diff. Other substantial logic, state, build and tooling
changes also need review. Trivial prose, formatting, comments and similar edits
can skip it. Scope decisions follow behavior and risk rather than a line-count
threshold. Review-only Claude sessions do not launch further reviews.

This automation is an agent instruction plus a single workflow command. There is
no background daemon, Git hook, schedule or Codex/Orca setting installed. Codex
chooses explicit files from its current task and supplies objectives and context;
it must preserve and exclude unrelated work already in the checkout. The script
does not infer ownership or discover a scope from every dirty file.

## One-command direct review

From the repository root, Codex runs, for example:

```sh
python3 tools/claude_review.py review \
  --objective 'Check callback safety, synchronization and voice lifetime' \
  --objective 'Identify CPU regressions without changing sound or MIDI timing' \
  --context Source/aeolus/engine.h \
  --context Source/aeolus/division.h \
  Source/aeolus/engine.cpp Source/aeolus/division.cpp
```

`review` calls the same `prepare`, `run` and `show` implementations described
below. After successful preparation, stdout contains one JSON status with the
review ID and, when available, attempt directory and validated findings. Lock and
integrity errors also return JSON with a failed status. Preparation/argument
errors return nonzero with diagnostics on stderr and may have empty stdout.
Preparation/attempt status goes to stderr, with raw Claude
streams saved separately. It supports `--base`, repeated `--objective` and
`--context`, `--timeout` (default 600 seconds) and `--claude`. No model or effort is
forced. Exit 0 means a completed valid review of the current scoped snapshot,
including when findings exist. It does not mean the code has no defects.

Codex must read findings and limitations, confirm concrete defects, handle fixes
within the authorized task and run relevant checks. After substantive fixes,
review the new snapshot. Retain the review ID in the completion report. A failed,
pending, timed-out, interrupted, stale or invalid review is a reported limitation,
not successful validation. Do not keep retrying unchanged failures automatically.

## Prepare, run, consume

1. **Codex prepares a bounded request.** From the repository root, select explicit
   changed files and add related headers, ownership/lifecycle code or tests as
   context. For example, when reviewing engine changes:

   ```sh
   python3 tools/claude_review.py prepare \
     --objective 'Check callback safety, synchronization and voice lifetime' \
     --objective 'Identify CPU regressions without changing sound or MIDI timing' \
     --context Source/aeolus/engine.h \
     --context Source/aeolus/division.h \
     Source/aeolus/engine.cpp Source/aeolus/division.cpp
   ```

   The default baseline is `HEAD`; use `--base <commit-or-ref>` for an existing
   branch/commit comparison. The diff includes the selected files' staged and
   unstaged changes together, plus explicitly selected untracked additions.
   Unselected dirty/untracked file contents are not collected. Scope is at file
   granularity: every selected file includes its full current contents and all
   changes against the base, including pre-existing edits in that same file.
   If task and unrelated edits share a file, state that limitation and constrain
   the objectives; never stash, reset or rewrite user work to manufacture scope.
   Context contains full files
   with line numbers and the current root `AGENTS.md`/`CLAUDE.md`. Add any applicable
   nested instructions with `--context`. Deleted files use their base version.
   Binary files, symlinks, directories and requests above 1.5 MB are refused rather
   than silently omitted or truncated. Split large reviews into related scopes.
   A file staged for deletion and then recreated without staging is refused until
   its Git state is resolved, so its diff cannot misrepresent the current content.

2. **Codex runs the printed command directly.** Each prepare prints a unique ID
   and an exact command including `cd` to this checkout. `prepare --json` returns
   the ID, artifact directory and host command as JSON for scripted callers:

   ```sh
   python3 tools/claude_review.py run REVIEW_ID
   ```

   Replace `REVIEW_ID` with that ID. The same command can be run manually in the
   working host terminal if direct access is unavailable. The exact command is in
   `build/claude-reviews/REVIEW_ID/host-command.txt`. The default timeout is 600
   seconds; `--timeout 900` changes it. `--claude /absolute/path/to/claude` selects
   a known working CLI if needed. No model or effort is forced.

3. **Codex reads and validates the latest attempt:**

   ```sh
   python3 tools/claude_review.py show REVIEW_ID
   ```

   Read both the status and findings. Exit 0 requires a completed valid response
   for the current snapshot; it does **not** mean there were no defects. Confirm
   each finding against the code, separate confirmed defects from suspected risks,
   fix only within the authorized task, and run relevant tests. For DSP/MIDI work
   use the deterministic render/tail checks and hardware/performance evidence in
   [AGENTS.md](../AGENTS.md). Prepare a new review after changes. A completed
   review is not a benchmark or evidence of hardware reliability.

   During integration setup, leave application source unchanged. Preparing a
   request does not authorize applying its findings outside the current task.

The prompt prioritizes real-time audio safety, synchronization and ownership,
callback allocation/blocking, voice admission/note-offs/tails/panic, and bounded
CPU work. Tooling reviews instead focus on correctness and failure handling.

## Output and success contract

Every request stays under the already Git-ignored `build/claude-reviews/`:

```text
REVIEW_ID/
  manifest.json             # Base, HEAD, selected files/hashes, objectives
  request.txt               # Exact stdin sent to Claude
  diff.patch, context.txt   # Human-readable snapshot
  host-command.txt          # One-command manual invocation
  latest-attempt.json       # Points to newest attempt, including a failed retry
  attempts/ATTEMPT/
    command.json            # Exact argv, cwd, timeout, input hash, CLI path, HOME
    stdout.txt, stderr.txt  # Unmodified CLI streams
    result.json             # Exit code, timeout/interruption, completion, elapsed time
    findings.json           # Written only for a validated structured response
```

Request hashes and an echoed review ID/snapshot hash bind the response to the
prepared request. File hashes (including context/instructions) and HEAD are checked
before execution, after execution and when Codex reads results. A changed file
makes the result unusable for the current tree; the old evidence remains intact.
Creating a previously absent root `AGENTS.md` or `CLAUDE.md` also invalidates the
snapshot. `show` labels an otherwise valid historical review `stale` with an error
when the tree has changed. Git color and prefix settings cannot alter patch
formatting. Diff/binary attributes cannot suppress selected text hunks; binary
changes on either side of the diff are refused. Git clean filters, ident expansion
and line-ending normalization can still make the Git diff differ from raw file
contents. The numbered context and file hashes use raw working-tree content;
review that context when interpreting a normalized or filtered diff. Preparation
uses normal Git configuration, so use trusted repositories and Git filters.
Index flags such as assume-unchanged and skip-worktree can hide working-tree
edits from Git's diff or cause preparation to report no changes. Do not use those
flags on selected files; inspect them with `git ls-files -v -- FILE...` and resolve
such index state before preparing a review. The wrapper does not change the index.
Retrieval and execution reconstruct the snapshot digest from the saved manifest,
diff and numbered context before trusting scope or staleness metadata. Corrupted
metadata or saved evidence fails validation, including an edited file hash that
would otherwise hide a source change. These local hashes detect inconsistent
evidence; they are not signatures against someone replacing the entire evidence
set. Validation also rebuilds the exact request from the payload, so changing
request.txt together with its manifest hash cannot substitute different review
material. Request, diff and context artifacts use byte writes to retain line
endings across hosts; changing the request format requires preparing fresh reviews.
Changing unrelated files does not invalidate the scoped snapshot. Changes outside
the supplied context still need the normal integrated review and tests.

Failures, empty/malformed responses, missing fields, API errors, wrong identities,
interruption and timeout all return nonzero. The CLI handles SIGTERM and SIGHUP
through the same owned-process cleanup as an interrupt. SIGINT/SIGTERM/SIGHUP
handlers record cancellation for safe checkpoints, including during process
startup, rather than unwinding ownership setup. Repeated signals cannot interrupt
owned-process cleanup. A timeout stays a failure even if
Claude subsequently exits 0 with plausible output. A process crash leaves a
non-completed status; it is never promoted to success. Missing CLI/host preflight
failures preserve logs/status with a null command exit code because no CLI ran.
Retries get separate attempt directories; a failed retry never selects a previous
successful attempt. A running lock prevents concurrent runs of the same request.
If the wrapper itself is forcibly killed, check the recorded command and host
processes, stop only that owned attempt if necessary, then remove that request's
`running.lock` before retrying. Never remove another active runner's lock.

The wrapper invokes `claude -p` with `--no-session-persistence`, `--tools ''`,
`--output-format json` and `--json-schema`. It validates the JSON result envelope
and `structured_output`, rather than trusting exit code alone. Read-only review
also uses `--safe-mode`, `--disable-slash-commands`, `--strict-mcp-config` with an
empty `--mcp-config`, `--disallowedTools 'mcp__*'` and
`--settings '{"disableAllHooks":true}'`. These session flags remove built-in/MCP
tools and disable ordinary hooks, skills, plugins and automatic instruction
discovery. `--system-prompt` supplies the review-only priorities in place of the
coding-agent system prompt and its automatic workspace metadata. The supplied
context includes the selected repository instructions.
See the official [CLI reference](https://code.claude.com/docs/en/cli-reference)
and [hook controls](https://code.claude.com/docs/en/hooks#disable-or-remove-hooks).

Claude receives snapshot text and reports findings; it cannot use source editing,
shell or MCP tools to apply changes. Codex owns subsequent fixes. This is a CLI
capability restriction, not an operating-system filesystem sandbox. Trusted CLI
code and administrator-managed policy still apply; managed hooks cannot be
disabled by session settings. Do not use an untrusted CLI executable or policies
that modify sources during review. Unsupported flags fail the attempt, with no
fallback to a tool-enabled invocation.

The wrapper inherits the environment and leaves HOME, credentials, model/effort,
networking, sandbox policy and persisted configuration unchanged. Safe mode keeps
normal authentication; the wrapper avoids `--bare`, which skips subscription/OAuth
login. It never grants additional permissions, runs fixes or executes suggested
commands. It terminates only its owned CLI process group on timeout/interruption, including
SIGTERM/SIGHUP received while Claude is running.
Raw streams/configured CLI output may
contain private source or diagnostics: keep the artifacts local and do not force
add them to Git. Preparation checks that the artifact directory is ignored.

## Access failures and optional Orca fallback

Direct execution is preferred when the normal CLI and networking work. The
`CODEX_SANDBOX_NETWORK_DISABLED=1` guard remains: it records a failed attempt
without launching Claude. An unset variable does not prove connectivity; missing
CLI, authentication and network errors still fail and retain logs. Do not unset
the guard, alter credentials, restart services or change host controls to force a
review. Provide `host-command.txt` for manual execution when access is unavailable.

The installed version-matched Orca CLI guide and `terminal create --help` provide
a supported startup-command mechanism:

```sh
orca-ide terminal create --worktree "path:$PWD" \
  --title 'Aeolus Claude review' \
  --command 'python3 tools/claude_review.py run REVIEW_ID' --json
```

Run this from the repository root in a host environment that can access Orca's
connected runtime. Substitute the prepared ID. `orca-ide` is the Linux executable
resolved for this environment; other installations must first resolve their CLI
using the installed orchestration skill. A newly created terminal avoids typing
into an existing user terminal. Acceptance of the terminal command is **not**
review completion: Codex must still consume `show REVIEW_ID`. If a launch reply is
ambiguous, inspect Orca's terminals and the request's attempts before retrying;
do not blindly create duplicate reviews.

Check `orca-ide status --json` before relying on this fallback. Its runtime must
be reachable and ready; a CLI/terminal launch failure is not a completed review.
The direct wrapper does not depend on an Orca session or runtime. Orca terminal
startup is an optional manual alternative, not exercised by the direct integration
test and not needed to automate reviews in the current execution environment.

Orca also documents scheduled provider-prompt automations. Those start agent
sessions and do not by themselves enforce this wrapper's bounded execution and
result contract; no schedule, agent or runtime setting is installed by this work.

## Validation

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p test_claude_review.py -v
git diff --check
```

The default tests use temporary Git repositories and a fake local CLI. They exercise
scope/context, deleted/untracked files, success and failed retries, malformed/error
responses, stale snapshots, request/result/manifest/evidence tampering, CRLF
snapshot round trips, LF-based source numbering, Git color/prefix settings,
missing CLI, the sandbox guard, repeated stop signals, cancellation during startup,
parsing and retrieval, platform newline translation, and timeout escalation,
including a timed-out CLI returning 0. CLI integration tests exercise
separate `prepare --json` / `run` / `show` processes and the single `review` command,
including failure output, timeout, invalid arguments and source preservation.
They never call the paid service by default.

Explicitly opt in to the real CLI/service integration test (uses the current
account and may incur normal usage charges):

```sh
AEOLUS_TEST_CLAUDE_LIVE=1 PYTHONDONTWRITEBYTECODE=1 \
  python3 tests/test_claude_review.py ReviewTests.test_live_cli_review_end_to_end -v
```

The live test prepares a disposable Git repository with an intentional arithmetic
defect, executes the full `review` workflow, retrieves the result with `show`,
requires a finding for that source, and verifies that fixture sources are
unchanged and configured project hook/MCP startup markers were not executed.
Temporary fixture artifacts are cleaned up with the test repository. Live reviews
in this checkout retain their normal logs under `build/claude-reviews/`.
Neither fake nor live tooling tests establish Aeolus audio/hardware reliability.
