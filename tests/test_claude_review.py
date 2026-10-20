"""Offline contract/CLI integration tests; opt in to a real CLI test explicitly."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "tools/claude_review.py"
SPEC = importlib.util.spec_from_file_location("claude_review", SCRIPT)
review = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(review)

FAKE_CLAUDE = r'''
import json, os, re, signal, sys, time
request = sys.stdin.read()
assert "--tools" in sys.argv and sys.argv[sys.argv.index("--tools") + 1] == ""
assert "--bare" not in sys.argv
assert "--json-schema" in sys.argv
assert "--safe-mode" in sys.argv
assert "--no-session-persistence" in sys.argv
assert "--disable-slash-commands" in sys.argv
assert "--strict-mcp-config" in sys.argv
assert json.loads(sys.argv[sys.argv.index("--mcp-config") + 1]) == {"mcpServers": {}}
assert sys.argv[sys.argv.index("--disallowedTools") + 1] == "mcp__*"
assert json.loads(sys.argv[sys.argv.index("--settings") + 1]) == {"disableAllHooks": True}
assert "Review only the supplied snapshot" in sys.argv[sys.argv.index("--system-prompt") + 1]
value = {
    "review_id": re.search(r"Review ID: (.*)", request)[1],
    "snapshot_sha256": re.search(r"Snapshot SHA256: (.*)", request)[1],
    "review_completed": True, "summary": "Examined the supplied snapshot.",
    "findings": [{"severity": "P1", "confidence": "confirmed", "file": "audio.cpp",
                  "line": 1, "title": "Subtraction instead of addition",
                  "explanation": "add(2, 3) returns -1, which violates the requested sum.",
                  "validation": "Verify add(2, 3) returns 5."}],
    "limitations": ["Offline fixture; no audio was rendered."],
}
envelope = {"is_error": False, "subtype": "success", "structured_output": value}
mode = os.environ.get("FAKE_REVIEW_MODE", "success")
if mode == "empty_findings":
    value["findings"] = []
if mode == "wrong_identity":
    value["review_id"] = "some-other-request"
if mode == "incomplete":
    value["review_completed"] = False
if mode == "api_error":
    envelope["is_error"] = True
if mode == "malformed_finding":
    value["findings"][0]["line"] = True
if mode == "out_of_scope":
    value["findings"][0]["file"] = "unrelated.cpp"
if mode == "stale_during_run":
    with open("audio.cpp", "a") as f:
        f.write("// changed during review\n")
if mode == "timeout_exit_zero":
    def interrupted(*args):
        print(json.dumps(envelope), flush=True)
        sys.exit(0)
    signal.signal(signal.SIGINT, interrupted)
    print("ready", file=sys.stderr, flush=True)
    time.sleep(30)
if mode == "timeout_ignores_signals":
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    with open("fake-child.pid", "w") as pid_file:
        pid_file.write(str(os.getpid()))
    print("ready", file=sys.stderr, flush=True)
    time.sleep(30)
print("fixture diagnostic", file=sys.stderr)
if mode == "invalid_json":
    print("Execution error")
elif mode != "empty_stdout":
    print(json.dumps(envelope))
sys.exit(7 if mode == "nonzero" else 0)
'''


class ReviewTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="aeolus review tests ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.git("init", "-q")
        self.write(".gitignore", "build\n")
        self.write("audio.cpp", "int add(int a, int b) { return a + b; }\n")
        self.write("context.h", "// ownership context\n")
        self.write("unrelated.cpp", "// unrelated baseline\n")
        self.write("AGENTS.md", "Project-specific review instructions.\n")
        self.git("add", ".")
        self.git("-c", "user.name=Offline Test", "-c", "user.email=offline@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "-qm", "fixture")
        self.write("audio.cpp", "int add(int a, int b) { return a - b; }\n")
        self.fake = self.root / "fake claude"
        self.fake.write_text(f"#!{sys.executable}\n" + FAKE_CLAUDE)
        self.fake.chmod(0o700)

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.root), *args], stderr=subprocess.PIPE)

    def write(self, name, contents):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents)

    def prepare(self, files=None, context=None):
        return review.prepare(self.root, files or ["audio.cpp"], ["Check arithmetic and RT safety"],
                              context or ["context.h"])

    def run_review(self, directory, mode="success", timeout=5, cli=None):
        # Only the fake local process is used; this does not relax the real runner.
        with mock.patch.dict(os.environ, {"FAKE_REVIEW_MODE": mode}):
            with mock.patch.dict(os.environ):
                os.environ.pop("CODEX_SANDBOX_NETWORK_DISABLED", None)
                with contextlib.redirect_stdout(io.StringIO()):
                    return review.run_review(self.root, directory.name, timeout, str(cli or self.fake))

    def latest(self, directory):
        pointer = json.loads((directory / "latest-attempt.json").read_text())
        path = directory / "attempts" / pointer["attempt"]
        return path, json.loads((path / "result.json").read_text())

    def show(self, directory):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            code = review.show(self.root, directory.name)
        return code, json.loads(output.getvalue())

    def cli(self, *args, mode="success", real_cli=False):
        env = os.environ.copy()
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        if not real_cli:
            env["FAKE_REVIEW_MODE"] = mode
            env.pop("CODEX_SANDBOX_NETWORK_DISABLED", None)
        return subprocess.run([sys.executable, str(SCRIPT), *args], cwd=self.root,
                              env=env, capture_output=True, text=True, timeout=210 if real_cli else 20)

    def review_args(self, *extra):
        return ("review", "--objective", "add(a, b) must return the sum; check the implementation.",
                "--context", "context.h", *extra, "audio.cpp")

    def test_cli_prepare_run_show_end_to_end(self):
        prepared = self.cli("prepare", "--json", "--objective", "Check arithmetic",
                            "--context", "context.h", "audio.cpp")
        self.assertEqual(prepared.returncode, 0, prepared.stderr)
        metadata = json.loads(prepared.stdout)
        directory = Path(metadata["directory"])
        self.assertIn(metadata["review_id"], metadata["host_command"])
        self.assertEqual(self.cli("show", directory.name).returncode, 1)
        ran = self.cli("run", directory.name, "--claude", str(self.fake))
        self.assertEqual(ran.returncode, 0, ran.stderr + ran.stdout)
        shown = self.cli("show", directory.name)
        self.assertEqual(shown.returncode, 0, shown.stderr)
        self.assertEqual(json.loads(shown.stdout)["findings"]["findings"][0]["file"], "audio.cpp")

    def test_cli_review_runs_and_retrieves_without_editing_sources(self):
        before = {name: (self.root / name).read_bytes()
                  for name in ("audio.cpp", "context.h", "unrelated.cpp", "AGENTS.md")}
        ran = self.cli(*self.review_args("--claude", str(self.fake)))
        self.assertEqual(ran.returncode, 0, ran.stderr + ran.stdout)
        result = json.loads(ran.stdout)
        self.assertTrue(result["review_completed"])
        self.assertTrue(result["usable_for_current_tree"])
        self.assertEqual(len(result["findings"]["findings"]), 1)
        directory = self.root / review.ARTIFACTS / result["review_id"]
        self.assertTrue((directory / "host-command.txt").exists())
        self.assertEqual(self.cli("show", directory.name).returncode, 0)
        self.assertEqual(before, {name: (self.root / name).read_bytes() for name in before})

    def test_cli_review_failures_return_validated_status_and_logs(self):
        for mode in ("nonzero", "invalid_json", "wrong_identity", "incomplete", "stale_during_run"):
            with self.subTest(mode=mode):
                ran = self.cli(*self.review_args("--claude", str(self.fake)), mode=mode)
                self.assertEqual(ran.returncode, 1, ran.stderr + ran.stdout)
                result = json.loads(ran.stdout)
                self.assertFalse(result["usable_for_current_tree"])
                self.assertIsNone(result["findings"])
                attempt = Path(result["attempt_directory"])
                self.assertTrue((attempt / "stdout.txt").exists())
                self.assertTrue((attempt / "stderr.txt").exists())

    def test_cli_review_timeout_remains_failure_after_zero_exit(self):
        ran = self.cli(*self.review_args("--claude", str(self.fake), "--timeout", "5"),
                       mode="timeout_exit_zero")
        self.assertEqual(ran.returncode, 1, ran.stderr + ran.stdout)
        result = json.loads(ran.stdout)
        self.assertTrue(result["timed_out"])
        self.assertEqual(result["command_exit_code"], 0)
        self.assertFalse(result["review_completed"])

    def test_cli_review_missing_executable_logs_failure(self):
        ran = self.cli(*self.review_args("--claude", str(self.root / "missing-claude")))
        self.assertEqual(ran.returncode, 1, ran.stderr + ran.stdout)
        result = json.loads(ran.stdout)
        self.assertIsNone(result["command_exit_code"])
        self.assertIn("No such file", result["error"])

    def test_cli_review_invalid_timeout_does_not_prepare(self):
        for value in ("0", "-1", "nan", "inf"):
            with self.subTest(value=value):
                ran = self.cli(*self.review_args("--timeout", value))
                self.assertEqual(ran.returncode, 1)
                self.assertIn("positive finite", ran.stderr)
                self.assertFalse((self.root / review.ARTIFACTS).exists())

    def test_review_pre_attempt_lock_failure_returns_json_and_preserves_lock(self):
        directory = self.prepare()
        lock = directory / "running.lock"
        lock.touch()
        with mock.patch.object(review, "prepare", return_value=directory):
            with contextlib.redirect_stdout(io.StringIO()) as output:
                with contextlib.redirect_stderr(io.StringIO()):
                    code = review.review_changes(self.root, ["audio.cpp"], ["Check arithmetic"])
        self.assertEqual(code, 1)
        result = json.loads(output.getvalue())
        self.assertEqual(result["review_id"], directory.name)
        self.assertFalse(result["usable_for_current_tree"])
        self.assertIn("runner owns", result["error"])
        self.assertTrue(lock.exists())
        self.assertFalse((directory / "latest-attempt.json").exists())

    def test_review_retrieval_integrity_failure_returns_json(self):
        directory = self.prepare()
        with mock.patch.object(review, "prepare", return_value=directory):
            with mock.patch.object(review, "show", side_effect=ValueError("Saved output hash mismatch")):
                with contextlib.redirect_stdout(io.StringIO()) as output:
                    with contextlib.redirect_stderr(io.StringIO()):
                        code = review.review_changes(self.root, ["audio.cpp"], ["Check arithmetic"],
                                                     claude=str(self.fake))
        self.assertEqual(code, 1)
        result = json.loads(output.getvalue())
        self.assertFalse(result["review_completed"])
        self.assertIn("hash mismatch", result["error"])

    def test_cli_non_head_base_includes_committed_changes(self):
        self.git("add", "audio.cpp")
        self.git("-c", "user.name=Offline Test", "-c", "user.email=offline@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "-qm", "arithmetic change")
        unchanged = self.cli("prepare", "--objective", "Check arithmetic", "audio.cpp")
        self.assertEqual(unchanged.returncode, 1)
        self.assertIn("No changes", unchanged.stderr)
        ran = self.cli(*self.review_args("--base", "HEAD~1", "--claude", str(self.fake)))
        self.assertEqual(ran.returncode, 0, ran.stderr + ran.stdout)
        result = json.loads(ran.stdout)
        directory = self.root / review.ARTIFACTS / result["review_id"]
        manifest = json.loads((directory / "manifest.json").read_text())
        self.assertEqual(manifest["base"], self.git("rev-parse", "HEAD~1").decode().strip())
        patch = (directory / "diff.patch").read_text()
        self.assertIn("-int add(int a, int b) { return a + b; }", patch)
        self.assertIn("+int add(int a, int b) { return a - b; }", patch)
        self.assertEqual(self.cli("show", directory.name).returncode, 0)

    def test_git_color_and_prefix_settings_cannot_change_patch_format(self):
        self.git("config", "color.ui", "always")
        self.git("config", "color.diff", "always")
        self.git("config", "diff.noprefix", "true")
        self.git("config", "diff.mnemonicPrefix", "true")
        directory = self.prepare()
        patch = (directory / "diff.patch").read_bytes()
        self.assertNotIn(b"\x1b[", patch)
        self.assertIn(b"diff --git a/audio.cpp b/audio.cpp\n", patch)
        self.assertIn(b"--- a/audio.cpp\n+++ b/audio.cpp\n", patch)
        self.assertEqual(self.run_review(directory), 0)
        self.assertEqual(self.show(directory)[0], 0)

    def test_git_binary_attribute_cannot_hide_text_changes(self):
        self.write(".gitattributes", "audio.cpp -diff\n")
        directory = self.prepare()
        patch = (directory / "diff.patch").read_text()
        self.assertIn("-int add(int a, int b) { return a + b; }", patch)
        self.assertIn("+int add(int a, int b) { return a - b; }", patch)
        self.assertNotIn("Binary files", patch)

    def test_binary_base_cannot_be_hidden_by_current_text(self):
        (self.root / "audio.cpp").write_bytes(b"old binary\0content\n")
        self.git("add", "audio.cpp")
        self.git("-c", "user.name=Offline Test", "-c", "user.email=offline@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "-qm", "binary fixture")
        self.write("audio.cpp", "// now text\n")
        with self.assertRaisesRegex(ValueError, "Binary changes"):
            self.prepare()

    def test_new_root_instruction_file_invalidates_snapshot(self):
        directory = self.prepare()
        self.assertEqual(self.run_review(directory), 0)
        self.write("CLAUDE.md", "New root instructions.\n")
        code, result = self.show(directory)
        self.assertEqual(code, 1)
        self.assertFalse(result["usable_for_current_tree"])
        self.assertIn("CLAUDE.md", result["stale_files"])
        self.assertEqual(result["state"], "stale")
        self.assertIsNotNone(result["error"])

    def test_claude_instructions_are_supplied_and_hash_checked(self):
        self.write("CLAUDE.md", "Read-only fixture instructions.\n")
        directory = self.prepare()
        self.assertIn("Read-only fixture instructions", (directory / "request.txt").read_text())
        self.assertEqual(self.run_review(directory), 0)
        self.write("CLAUDE.md", "Changed fixture instructions.\n")
        self.assertEqual(self.show(directory)[0], 1)

    def test_binary_and_non_utf8_inputs_fail_without_artifacts(self):
        for contents, message in ((b"\0\xff", "Binary context"), (b"\xff", "decode")):
            with self.subTest(contents=contents):
                (self.root / "binary.dat").write_bytes(contents)
                ran = self.cli("prepare", "--objective", "Check input", "binary.dat")
                self.assertEqual(ran.returncode, 1)
                self.assertIn(message, ran.stderr)
                self.assertFalse((self.root / review.ARTIFACTS).exists())

    @unittest.skipUnless(os.environ.get("AEOLUS_TEST_CLAUDE_LIVE") == "1",
                         "Set AEOLUS_TEST_CLAUDE_LIVE=1 to contact the real Claude service")
    def test_live_cli_review_end_to_end(self):
        # Deliberately hostile project customizations stay inside this disposable
        # repository. Neither hooks nor MCP startup should run in review mode.
        marker = "from pathlib import Path; Path('customization-ran').touch()"
        self.write(".claude/settings.json", json.dumps({"hooks": {"SessionStart": [{
            "hooks": [{"type": "command", "command":
                       review.shlex.join([sys.executable, "-c", marker])}]}]}}))
        self.write(".mcp.json", json.dumps({"mcpServers": {"fixture": {
            "command": sys.executable, "args": ["-c", marker]}}}))
        before = {str(p.relative_to(self.root)): p.read_bytes()
                  for p in self.root.rglob("*") if p.is_file() and ".git" not in p.parts}
        ran = self.cli(*self.review_args("--timeout", "180"), real_cli=True)
        self.assertEqual(ran.returncode, 0, ran.stderr + ran.stdout)
        result = json.loads(ran.stdout)
        self.assertTrue(result["review_completed"])
        self.assertTrue(result["usable_for_current_tree"])
        self.assertTrue(any(item["file"] == "audio.cpp" for item in result["findings"]["findings"]),
                        result["findings"])
        shown = self.cli("show", result["review_id"], real_cli=True)
        self.assertEqual(shown.returncode, 0, shown.stderr + shown.stdout)
        self.assertEqual(json.loads(shown.stdout)["findings"], result["findings"])
        self.assertFalse((self.root / "customization-ran").exists())
        self.assertEqual(before, {name: (self.root / name).read_bytes() for name in before})

    def test_explicit_scope_staged_unstaged_untracked_and_numbered_context(self):
        self.git("add", "audio.cpp")
        self.write("audio.cpp", "int add(int a, int b) { return a - b; }\n// unstaged context\n")
        self.write("new.cpp", "// explicitly selected untracked addition\n")
        self.write("unrelated.cpp", "// unrelated dirty secret\n")
        self.write("unselected.cpp", "// unselected untracked secret\n")
        directory = self.prepare(["audio.cpp", "new.cpp"])
        request = (directory / "request.txt").read_text()
        patch = (directory / "diff.patch").read_text()
        self.assertIn("return a - b", patch)
        self.assertIn("+// unstaged context", patch)
        self.assertIn("+++ b/new.cpp", patch)
        self.assertIn("1: // ownership context", request)
        self.assertIn("Project-specific review instructions", request)
        self.assertIn("real-time audio safety", request)
        self.assertNotIn("unrelated dirty secret", request)
        self.assertNotIn("unselected untracked secret", request)
        self.assertNotEqual(self.git("check-ignore", str(directory / "request.txt")), b"")

    def test_deleted_file_has_base_context(self):
        (self.root / "audio.cpp").unlink()
        request = (self.prepare() / "request.txt").read_text()
        self.assertIn("deleted; base version", request)
        self.assertIn("return a + b", request)

    def test_untracked_patch_handles_missing_final_newline(self):
        self.write("new.cpp", "// no final newline")
        patch = (self.prepare(["new.cpp"]) / "diff.patch").read_text()
        self.assertIn("+// no final newline\n\\ No newline at end of file\n", patch)

    def test_non_lf_separators_preserve_git_line_numbers_and_untracked_patch(self):
        text = "a\fb\vc\x85d\u2028e\u2029f\nsecond\n"
        self.write("new.cpp", text)
        directory = self.prepare(["new.cpp"])
        patch = (directory / "diff.patch").read_text()
        context = (directory / "context.txt").read_text()
        self.assertIn("+" + text.split("\n")[0] + "\n+second\n", patch)
        self.assertNotIn("No newline at end of file", patch)
        self.assertIn("    1: " + text.split("\n")[0] + "\n    2: second", context)
        self.assertNotIn("    3:", context.split("FILE context.h", 1)[0])

    def test_prepare_detects_concurrent_edit(self):
        original_git = review.git

        def racing_git(root, *args, **kwargs):
            result = original_git(root, *args, **kwargs)
            if args[0] == "diff":
                self.write("audio.cpp", "// concurrent edit\n")
            return result

        with mock.patch.object(review, "git", side_effect=racing_git):
            with self.assertRaisesRegex(ValueError, "changed during preparation"):
                self.prepare()

    def test_interrupted_optional_git_call_is_not_a_semantic_absence(self):
        stopped = subprocess.CompletedProcess(["git"], -signal.SIGINT, b"", b"")
        with mock.patch.object(subprocess, "run", return_value=stopped):
            with self.assertRaisesRegex(ValueError, "terminated by signal"):
                review.git(self.root, "show", "HEAD:audio.cpp", optional=True)

    def test_interrupted_preparation_cannot_save_a_snapshot(self):
        original_git = review.git

        def stopped_lookup(root, *args, **kwargs):
            result = original_git(root, *args, **kwargs)
            if args[0] == "ls-files":
                review._stop_requested = True
            return result

        with mock.patch.object(review, "_stop_requested", False):
            with mock.patch.object(review, "git", side_effect=stopped_lookup):
                with self.assertRaises(KeyboardInterrupt):
                    self.prepare()
        self.assertFalse((self.root / review.ARTIFACTS).exists())

    def test_pending_is_not_success(self):
        code, result = self.show(self.prepare())
        self.assertEqual(code, 1)
        self.assertFalse(result["review_completed"])

    def test_success_saves_bound_findings_and_logs(self):
        directory = self.prepare()
        self.assertEqual(self.run_review(directory), 0)
        path, result = self.latest(directory)
        self.assertTrue(result["review_completed"])
        self.assertEqual(result["command_exit_code"], 0)
        self.assertEqual(result["finding_count"], 1)
        self.assertIn("fixture diagnostic", (path / "stderr.txt").read_text())
        self.assertEqual(json.loads((path / "findings.json").read_text())["findings"][0]["severity"], "P1")
        command = json.loads((path / "command.json").read_text())
        self.assertEqual(command["argv"][0], str(self.fake))
        self.assertNotIn("--bare", command["argv"])
        self.assertEqual(self.show(directory)[0], 0)

    def test_no_findings_is_completed_not_a_hardware_claim(self):
        directory = self.prepare()
        self.assertEqual(self.run_review(directory, "empty_findings"), 0)
        self.assertEqual(self.latest(directory)[1]["finding_count"], 0)

    def test_failed_review_never_reuses_earlier_success(self):
        directory = self.prepare()
        self.assertEqual(self.run_review(directory), 0)
        first, _ = self.latest(directory)
        self.assertEqual(self.run_review(directory, "nonzero"), 1)
        second, result = self.latest(directory)
        self.assertNotEqual(first, second)
        self.assertEqual(result["command_exit_code"], 7)
        self.assertFalse(result["review_completed"])
        self.assertTrue((first / "findings.json").exists())
        self.assertFalse((second / "findings.json").exists())
        self.assertEqual(self.show(directory)[0], 1)

    def test_invalid_outputs_fail_closed(self):
        for mode in ("wrong_identity", "incomplete", "api_error", "malformed_finding", "out_of_scope", "invalid_json", "empty_stdout"):
            with self.subTest(mode=mode):
                directory = self.prepare()
                self.assertEqual(self.run_review(directory, mode), 1)
                self.assertFalse(self.latest(directory)[1]["review_completed"])
                self.assertEqual(self.show(directory)[0], 1)

    def test_timeout_even_when_claude_exits_zero_with_valid_output(self):
        directory = self.prepare()
        self.assertEqual(self.run_review(directory, "timeout_exit_zero", timeout=5), 1)
        path, result = self.latest(directory)
        self.assertIn("ready", (path / "stderr.txt").read_text())
        self.assertEqual(result["command_exit_code"], 0)
        self.assertTrue(result["timed_out"])
        self.assertFalse(result["review_completed"])
        self.assertEqual(self.show(directory)[0], 1)

    def test_timeout_terminates_unresponsive_process(self):
        directory = self.prepare()
        self.assertEqual(self.run_review(directory, "timeout_ignores_signals", timeout=5), 1)
        attempt, result = self.latest(directory)
        self.assertIn("ready", (attempt / "stderr.txt").read_text())
        self.assertTrue(result["timed_out"])
        self.assertEqual(result["command_exit_code"], -signal.SIGKILL)
        self.assertLess(result["elapsed_seconds"], 12)

    def test_cli_termination_signals_clean_up_the_owned_child(self):
        for stop_signal in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            with self.subTest(signal=stop_signal):
                directory = self.prepare()
                pid_file = self.root / "fake-child.pid"
                pid_file.unlink(missing_ok=True)
                env = os.environ.copy()
                env.pop("CODEX_SANDBOX_NETWORK_DISABLED", None)
                env["FAKE_REVIEW_MODE"] = "timeout_ignores_signals"
                env["PYTHONDONTWRITEBYTECODE"] = "1"
                process = subprocess.Popen(
                    [sys.executable, str(SCRIPT), "run", directory.name,
                     "--claude", str(self.fake), "--timeout", "30"],
                    cwd=self.root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    text=True, start_new_session=True)
                child_pid = None
                try:
                    deadline = time.monotonic() + 10
                    while time.monotonic() < deadline:
                        if pid_file.exists():
                            attempt, _ = self.latest(directory)
                            if "ready" in (attempt / "stderr.txt").read_text():
                                child_pid = int(pid_file.read_text())
                                break
                        self.assertIsNone(process.poll(), "Wrapper exited before fixture startup")
                        time.sleep(0.02)
                    self.assertIsNotNone(child_pid, "Fixture never signaled readiness")
                    process.send_signal(stop_signal)
                    time.sleep(0.1)
                    process.send_signal(stop_signal)
                    stdout, stderr = process.communicate(timeout=10)
                    self.assertEqual(process.returncode, 1, stdout + stderr)
                    result = json.loads(stdout)
                    self.assertTrue(result["interrupted"])
                    self.assertFalse(result["review_completed"])
                    self.assertEqual(result["command_exit_code"], -signal.SIGKILL)
                    self.assertFalse((directory / "running.lock").exists())
                    self.assertEqual(self.show(directory)[0], 1)
                    with self.assertRaises(ProcessLookupError):
                        os.kill(child_pid, 0)
                finally:
                    if child_pid is not None:
                        try:
                            os.killpg(child_pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    if process.poll() is None:
                        process.kill()
                    process.communicate(timeout=10)

    def test_cancellation_during_popen_startup_preserves_child_ownership(self):
        directory = self.prepare()
        original_popen = subprocess.Popen
        children = []

        def cancel_after_start(*args, **kwargs):
            process = original_popen(*args, **kwargs)
            if process.args[0] == str(self.fake):
                children.append(process)
                review._stop_requested = True
            return process

        with mock.patch.object(review, "_stop_requested", False):
            with mock.patch.object(subprocess, "Popen", side_effect=cancel_after_start):
                self.assertEqual(self.run_review(directory), 1)
        self.assertEqual(len(children), 1)
        self.assertIsNotNone(children[0].poll())
        self.assertTrue(self.latest(directory)[1]["interrupted"])
        self.assertFalse((directory / "running.lock").exists())

    def test_interrupted_parsing_persists_failure_and_returns_review_json(self):
        with mock.patch.object(review, "parse_findings", side_effect=KeyboardInterrupt):
            with contextlib.redirect_stdout(io.StringIO()) as output:
                with contextlib.redirect_stderr(io.StringIO()):
                    code = review.review_changes(self.root, ["audio.cpp"], ["Check arithmetic"],
                                                 claude=str(self.fake))
        result = json.loads(output.getvalue())
        self.assertEqual(code, 1)
        self.assertTrue(result["interrupted"])
        self.assertEqual(result["state"], "failed")
        attempt = Path(result["attempt_directory"])
        self.assertTrue(json.loads((attempt / "result.json").read_text())["interrupted"])

    def test_cancelled_result_retrieval_cannot_report_success(self):
        directory = self.prepare()
        self.assertEqual(self.run_review(directory), 0)
        with mock.patch.object(review, "_stop_requested", True):
            code, result = self.show(directory)
        self.assertEqual(code, 1)
        self.assertTrue(result["interrupted"])
        self.assertFalse(result["usable_for_current_tree"])

    def test_missing_cli_logs_failure(self):
        directory = self.prepare()
        self.assertEqual(self.run_review(directory, cli=self.root / "missing-claude"), 1)
        path, result = self.latest(directory)
        self.assertIsNone(result["command_exit_code"])
        self.assertFalse(result["review_completed"])
        self.assertTrue((path / "stdout.txt").exists())
        self.assertTrue((path / "stderr.txt").exists())

    def test_sandbox_guard_does_not_start_claude(self):
        directory = self.prepare()
        with mock.patch.dict(os.environ, {"CODEX_SANDBOX_NETWORK_DISABLED": "1"}):
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(review.run_review(self.root, directory.name, claude=str(self.fake)), 1)
        result = self.latest(directory)[1]
        self.assertIsNone(result["command_exit_code"])
        self.assertIn("host terminal", result["error"])

    def test_stale_before_during_and_after_review(self):
        directory = self.prepare()
        self.write("context.h", "// modified context\n")
        self.assertEqual(self.run_review(directory), 1)
        self.assertIsNone(self.latest(directory)[1]["command_exit_code"])
        directory = self.prepare()
        self.assertEqual(self.run_review(directory, "stale_during_run"), 1)
        self.assertTrue(self.latest(directory)[1]["review_completed"])
        self.assertFalse(self.latest(directory)[1]["usable_for_current_tree"])
        directory = self.prepare()
        self.assertEqual(self.run_review(directory), 0)
        self.write("audio.cpp", "// modified after review\n")
        self.assertEqual(self.show(directory)[0], 1)
        self.assertFalse(self.show(directory)[1]["usable_for_current_tree"])

    def test_tampered_request_and_results_are_rejected(self):
        directory = self.prepare()
        original = (directory / "request.txt").read_bytes()
        (directory / "request.txt").write_bytes(original + b"tampered")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            self.run_review(directory)
        (directory / "request.txt").write_bytes(original)
        self.assertEqual(self.run_review(directory), 0)
        attempt, _ = self.latest(directory)
        (attempt / "stdout.txt").write_text("{}")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            self.show(directory)

    def test_snapshot_metadata_and_artifact_tampering_are_rejected(self):
        for target in ("hashes", "files", "context", "objectives", "base", "head",
                       "snapshot_sha256", "version", "diff.patch", "context.txt"):
            with self.subTest(target=target):
                directory = self.prepare()
                self.assertEqual(self.run_review(directory), 0)
                manifest_path = directory / "manifest.json"
                manifest = json.loads(manifest_path.read_text())
                if target in ("diff.patch", "context.txt"):
                    with (directory / target).open("ab") as artifact:
                        artifact.write(b"tampered\n")
                else:
                    if target == "hashes":
                        # A manifest edit must not hide a source change from show.
                        self.write("audio.cpp", "// changed after review\n")
                        manifest[target]["audio.cpp"] = review.file_hash(self.root, "audio.cpp")
                    elif target in ("files", "context", "objectives"):
                        manifest[target].append("unrelated.cpp")
                    elif target == "version":
                        manifest[target] = 999
                    else:
                        manifest[target] = "0" * len(manifest[target])
                    review.save_json(manifest_path, manifest)
                with self.assertRaisesRegex(ValueError, "snapshot hash mismatch"):
                    self.show(directory)
                with self.assertRaisesRegex(ValueError, "snapshot hash mismatch"):
                    self.run_review(directory)
                shown = self.cli("show", directory.name)
                self.assertEqual(shown.returncode, 1)
                self.assertIn("snapshot hash mismatch", shown.stderr)

    def test_crlf_snapshot_evidence_round_trips_without_normalization(self):
        (self.root / "audio.cpp").write_bytes(b"int add(int a, int b) { return a - b; }\r\n")
        directory = self.prepare()
        self.assertIn(b"\r\n", (directory / "diff.patch").read_bytes())
        self.assertEqual(self.run_review(directory), 0)
        self.assertEqual(self.show(directory)[0], 0)

    def test_request_and_manifest_hash_edit_cannot_change_reviewed_snapshot(self):
        directory = self.prepare()
        self.assertEqual(self.run_review(directory), 0)
        request = directory / "request.txt"
        request.write_bytes(request.read_bytes().replace(b"return a - b", b"return a + b"))
        manifest_path = directory / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["request_sha256"] = review.digest(request.read_bytes())
        review.save_json(manifest_path, manifest)
        for operation in (self.show, self.run_review):
            with self.assertRaisesRegex(ValueError, "request does not match the snapshot"):
                operation(directory)

    def test_snapshot_artifacts_do_not_use_platform_text_newline_translation(self):
        original_write = Path.write_text

        def translated_write(path, text, *args, **kwargs):
            if path.name in ("diff.patch", "context.txt"):
                text = text.replace("\n", "\r\n")
            return original_write(path, text, *args, **kwargs)

        with mock.patch.object(Path, "write_text", new=translated_write):
            directory = self.prepare()
        self.assertEqual(self.run_review(directory), 0)
        self.assertEqual(self.show(directory)[0], 0)

    def test_running_lock_prevents_duplicate_reviews(self):
        directory = self.prepare()
        self.assertEqual(self.run_review(directory), 0)
        (directory / "running.lock").touch()
        self.assertEqual(self.show(directory)[0], 1)
        with self.assertRaisesRegex(ValueError, "runner owns"):
            self.run_review(directory)

    def test_staged_deletion_with_recreated_file_is_not_misrepresented(self):
        self.git("rm", "-f", "audio.cpp")
        self.write("audio.cpp", "// recreated but not staged\n")
        with self.assertRaisesRegex(ValueError, "removed from the index"):
            self.prepare()

    def test_keyboard_interrupt_cannot_complete_review(self):
        directory = self.prepare()
        original_wait = subprocess.Popen.wait
        interrupted = []

        def interrupt_claude(process, *args, **kwargs):
            if process.args[0] == str(self.fake) and not interrupted:
                interrupted.append(True)
                raise KeyboardInterrupt
            return original_wait(process, *args, **kwargs)

        with mock.patch.object(subprocess.Popen, "wait", new=interrupt_claude):
            self.assertEqual(self.run_review(directory), 1)
        result = self.latest(directory)[1]
        self.assertTrue(result["interrupted"])
        self.assertFalse(result["review_completed"])
        self.assertFalse((directory / "running.lock").exists())

    def test_outside_files_symlinks_directories_and_oversize_are_rejected(self):
        for path in ("../outside", str(self.root / "audio.cpp"), ".git/config", ".", "build/request.txt"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                self.prepare([path])
        (self.root / "link.cpp").symlink_to(self.root / "audio.cpp")
        with self.assertRaisesRegex(ValueError, "Symlink"):
            self.prepare(["link.cpp"])
        with mock.patch.object(review, "MAX_REQUEST_BYTES", 100):
            with self.assertRaisesRegex(ValueError, "split the review"):
                self.prepare()

    def test_nonignored_artifacts_and_changed_head_are_rejected(self):
        directory = self.prepare()
        self.git("-c", "user.name=Offline Test", "-c", "user.email=offline@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "--allow-empty", "-qm", "new HEAD")
        self.assertEqual(self.run_review(directory), 1)
        self.assertIn("HEAD", self.latest(directory)[1]["error"])
        self.write(".gitignore", "")
        with self.assertRaisesRegex(ValueError, "must be ignored"):
            self.prepare()


if __name__ == "__main__":
    unittest.main()
