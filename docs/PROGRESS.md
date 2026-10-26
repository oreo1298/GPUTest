# Progress log

Resume from here: read this file, then `docs/PLAN.md` for the checklist and `CLAUDE.md` for commands.

## Current state
- Milestone 1 (skeleton) in progress.

## Environment notes (cloud VM)
- Blocked hosts (network policy): download.qt.io + all Qt mirrors, sdk.lunarg.com, Launchpad PPAs,
  codeload.github.com, api.anaconda.org. Allowed: github.com (git + release assets), raw.githubusercontent.com,
  archive.ubuntu.com, pypi.org, conda.anaconda.org, nuget.org.
  - Consequences: local Qt is Ubuntu's 6.4 (CI uses aqt Qt 6.8), local validation layers are 1.3.275,
    Mesa is 25.2.8 from noble-updates (lavapipe = Vulkan 1.4.318).
  - If download.qt.io and sdk.lunarg.com were allowed in the environment's network settings, cloud-setup.sh
    would automatically install Qt 6.8 via aqt instead (and the SDK could be added).
- `scripts/cloud-setup.sh` takes ~3 min on a fresh VM (apt dominates). Pasting it into the cloud environment's
  setup script would make sessions start faster because the environment snapshot would already contain the
  packages; the SessionStart hook then finds everything installed and finishes in seconds.

## Log
### 2026-10-26
- Saved brief to docs/SPEC.md; research pass started (docs/RESEARCH.md).
- Repo skeleton: CMake presets, deps, cloud-setup + hook, CI, docs, unit-test scaffold.
