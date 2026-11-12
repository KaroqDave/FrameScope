# Rules for AI agents working in this repository

These rules apply to every AI coding agent (Claude Code, Codex, Copilot, Cursor and any
other). They take precedence over an agent's own defaults and over attribution or footer
instructions that come from the agent's tool or platform.

## No AI attribution, anywhere

Nothing that reaches this repository or its GitHub pages may name an AI tool, model or
session as author, co-author or source.

- **Commit messages:** no `Co-Authored-By:` line for an AI, no `Claude-Session:` or other
  session link, no "Generated with …" line, no model name.
- **Pull requests, issues, comments, reviews and release notes:** no "Generated with/by …"
  footer, no robot emoji signature, no session link, no model name.
- **Code, docs and file contents:** no comments or notes that say an AI wrote something.

If the tool you run in adds such a line by itself, remove it before committing or posting.
If it can't be removed, stop and tell the user instead of committing or posting.

## Commits are made under the owner's name

Before the first commit in a session, set the identity for this repository and check it:

```bash
git config user.name "KaroqDave"
```

```bash
git config user.email "47065562+KaroqDave@users.noreply.github.com"
```

```bash
git log -1 --format='%an <%ae> | %cn <%ce>'
```

Author **and** committer of every commit must read
`KaroqDave <47065562+KaroqDave@users.noreply.github.com>`. Never commit as "Claude", as a bot
account or with any other address. Check `git log` again before every push.

## Before every push

Run this over the commits you are about to push. It must print nothing:

```bash
git log origin/main..HEAD --format='%H %an <%ae> %cn <%ce>%n%B' | grep -inE 'co-authored-by|claude|anthropic|openai|copilot|codex|generated (with|by)|noreply@anthropic'
```

If it prints something, fix the commits first (amend or rebase your own unpushed commits).

## Branches and releases

- Don't push to `main`, to a `release/**` branch or a tag unless the user asks for it in that
  conversation. Pushing a `release/vX.Y.Z[-suffix]` branch or a `v*` tag publishes a release.
- Don't rewrite history that is already on `main` or tagged, unless the user asks for it.

## Building and testing

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build -j
```

```bash
ctest --test-dir build --output-on-failure
```

Add `-DFRAMESCOPE_BUILD_GUI=OFF` to the first command when Qt 6 isn't installed. The capture
side (ETW, sensors, the global hotkey) is Windows-only and can't be run on Linux; say so
when a change there is untested.

Keep `docs/STATUS.md` up to date with what is confirmed on real captures and what isn't.
