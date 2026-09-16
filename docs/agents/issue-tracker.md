# Issue tracker: GitHub

Issues and specs for this repo live as GitHub issues. Use the `gh` CLI for all operations.

## Conventions

- **Create an issue**: `gh issue create --title "..." --body "..."`. Use a heredoc for multi-line bodies.
- **Read an issue**: `gh issue view <number> --comments`, filtering comments by `jq` and also fetching labels.
- **List issues**: `gh issue list --state open --json number,title,body,labels,comments --jq '[.[] | {number, title, body, labels: [.labels[].name], comments: [.comments[].body]}]'` with appropriate `--label` and `--state` filters.
- **Comment on an issue**: `gh issue comment <number> --body "..."`
- **Apply / remove labels**: `gh issue edit <number> --add-label "..."` / `--remove-label "..."`
- **Close**: `gh issue close <number> --comment "..."`

Infer the repo from `git remote -v`; `gh` does this automatically when run inside a clone.

## Pull requests as a triage surface

**PRs as a request surface: no.** _(Set to `yes` if this repo treats external PRs as feature requests; `/triage` reads this flag.)_

When set to `yes`, PRs run through the same labels and states as issues, using the `gh pr` equivalents:

- **Read a PR**: `gh pr view <number> --comments` and `gh pr diff <number>` for the diff.
- **List external PRs for triage**: `gh pr list --state open --json number,title,body,labels,author,authorAssociation,comments` then keep only `authorAssociation` of `CONTRIBUTOR`, `FIRST_TIME_CONTRIBUTOR`, or `NONE` (drop `OWNER`/`MEMBER`/`COLLABORATOR`).
- **Comment / label / close**: `gh pr comment`, `gh pr edit --add-label`/`--remove-label`, `gh pr close`.

GitHub shares one number space across issues and PRs, so a bare `#42` may be either: resolve with `gh pr view 42` and fall back to `gh issue view 42`.

## When a skill says "publish to the issue tracker"

Create a GitHub issue.

## When a skill says "fetch the relevant ticket"

Run `gh issue view <number> --comments`.

## Wayfinding operations

Used by `/wayfinder`. The **map** is a single issue with **child** issues as tickets.

- **Map**: a single issue labelled `wayfinder:map`, holding the Notes / Decisions-so-far / Fog body. `gh issue create --label wayfinder:map`.
- **Child ticket**: an issue linked to the map as a GitHub sub-issue (`gh api` on the sub-issues endpoint). Where sub-issues aren't enabled, add the child to a task list in the map body and put `Part of #<map>` at the top of the child body. Labels: `wayfinder:<type>` (`research`/`prototype`/`grilling`/`task`). Once claimed, the ticket is assigned to the driving dev.
- **Blocking**: GitHub's **native issue dependencies**, the canonical, UI-visible representation. Add an edge with `gh api --method POST repos/<owner>/<repo>/issues/<child>/dependencies/blocked_by -F issue_id=<blocker-db-id>`, where `<blocker-db-id>` is the blocker's numeric **database id** (`gh api repos/<owner>/<repo>/issues/<n> --jq .id`, _not_ the `#number` or `node_id`). GitHub reports `issue_dependencies_summary.blocked_by` (open blockers only, the live gate). Where dependencies aren't available, fall back to a `Blocked by: #<n>, #<n>` line at the top of the child body. A ticket is unblocked when every blocker is closed.
- **Frontier query**: list the map's open children (`gh issue list --state open`, scoped to the map's sub-issues / task list), drop any with an open blocker (`issue_dependencies_summary.blocked_by > 0`, or an open issue in the `Blocked by` line) or an assignee; first in map order wins.
- **Claim**: `gh issue edit <n> --add-assignee @me`, the session's first write.
- **Resolve**: `gh issue comment <n> --body "<answer>"`, then `gh issue close <n>`, then append a context pointer (gist + link) to the map's Decisions-so-far.

## In this repo

**This is a brand-new repository.** It was an empty directory (no `.git`, no files, no remote) when `/setup-matt-pocock-skills` ran, so the tracker described above was stood up from scratch in the same session: `git init`, a first commit, then `gh repo create nathanihlenfeldt/aes67-srt --private --source=. --remote=origin --push`.

- **Repo**: `nathanihlenfeldt/aes67-srt` — **private**, default branch `main`. `gh` is authenticated locally as `nathanihlenfeldt` (scopes `gist`, `read:org`, `repo`, `workflow`), so `gh` inside the clone resolves the repo with no `-R` flag.
- **Explicit repo beats inference.** If `gh` is ever run from outside the clone, or the remote is changed, pass `-R nathanihlenfeldt/aes67-srt` rather than guessing.
- **Visibility is a disclosure decision, not just tooling.** Private was chosen deliberately, matching the neighbouring `aes67-vsc`. Publishing this repo's issues would also publish every ticket in them. If the repo is ever made public, re-read this section before the next `/triage` run.
- **`wontfix` is a stock GitHub label.** It already existed on repo creation; this setup only rewrote its description. Don't treat it as new. The other four state roles were created by this setup — see `triage-labels.md`.
- **`.out-of-scope/` is versioned, not ignored.** The `/triage` skill reads it and writes to it; its rules live in the skill's own `OUT-OF-SCOPE.md` (`~/.agents/skills/triage/OUT-OF-SCOPE.md`). It is institutional memory, so it belongs in git alongside `docs/adr/`. `.scratch/` is ignored; `.out-of-scope/` is not.

## Switching trackers

To move to a different tracker, re-run `/setup-matt-pocock-skills` and pick the other option; it replaces this file. The neighbouring `../AES67-VSC` shows what a **local-markdown** tracker looks like here (issues as files under `.scratch/<feature>/`), which is the fallback if GitHub ever stops being the right home for this project's work.
