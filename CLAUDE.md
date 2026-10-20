# Claude Code instructions for Aeolus

@AGENTS.md

[AGENTS.md](AGENTS.md) is the shared project guide. Read it together with
[README.md](README.md) and [AEOLUS_FEATURES.md](AEOLUS_FEATURES.md) before work.
This file adds Claude-specific instructions only.

Keep common permissions, build limits and collaboration policy in `AGENTS.md`;
do not create a second, conflicting policy here. An Orca dispatch's task and
lifecycle commands apply only to that dispatched task; a normal user-owned
session does not emit worker lifecycle messages.

- Follow the role and scope in the current user request or Orca dispatch. Work
  solo unless delegation or coordination is explicitly requested. Dispatched
  workers must not start further agents.
- Preserve this fork's organ editors, presets, state persistence and UI changes.
  Keep roadmap features distinct from implemented behavior and do not replace
  JUCE or DSP components merely because alternatives appear in the research.
- Apply the shared real-time, sound-preservation and asset-notice requirements.
  Review audio-thread work, callback lifetimes and shared state carefully; do
  not claim measured performance from static inspection.
- Use `claude/<topic>` for a new branch unless the task supplies one; retain an
  existing user-assigned branch. Keep the user's configured Git author identity
  and normal Claude Code attribution unless the user requests otherwise. Do not
  hardcode a particular model identity or change model/effort without direction.
- In the [Codex review workflow](docs/CLAUDE_REVIEW.md), independently report
  findings only. Codex owns implementation, test execution and fixes; do not edit
  source, execute suggested fixes or launch another review from a review session.
  Other read-only review requests stay read-only unless fixes are requested. Give
  findings with file/line references, a concrete failure scenario and severity,
  distinguishing confirmed defects from suspected risks.
- Keep the validated multicore/HEALTH implementation distinct from historical
  snapshot experiments and the future installer/profile UI. Audit guardian
  detachment, explicit Resume epochs and ownership barriers before lifecycle edits.
- Report the checks actually performed. Follow the shared approval boundaries
  for pushes, PRs, merges, publishing and external messages.
