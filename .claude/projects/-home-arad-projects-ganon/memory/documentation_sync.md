---
name: Keep AGENTS.md and CLAUDE.md in sync
description: Both documentation files must be updated whenever code changes
type: feedback
---

**Rule:** Any significant code change requires updating both AGENTS.md and CLAUDE.md.

**Why:** These files are the "source of truth" for understanding the codebase. Stale documentation makes future work inefficient and error-prone. They are read by every Claude instance working on the project.

**How to apply:**
- AGENTS.md: Update for architecture changes, protocol changes, new message types, error codes, conventions, and TODO updates
- CLAUDE.md: Update for build/run commands, new architectural patterns, new conventions, or major workflow changes
- Commit documentation updates together with code changes
- If uncertain whether a change warrants updates, err toward updating both files
