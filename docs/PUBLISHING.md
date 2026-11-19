# Publishing

The plan's publishing policy (§7) is **[LOCKED]**: public from day one,
**GPL-3.0**, **source-only** releases, and a mirror as insurance against the
primary host going away. This file is the operating manual for that policy.
GitHub is the primary host today; `scripts/sync_codeberg.ps1` is the failover.

> **Status 2026-11-19:** the project is published on GitHub
> (`github.com/CouldntFindit/korra-recomp`, public, work-in-progress) — that push is
> the primary hosted copy and the off-site backup; the earlier local-only
> decision is superseded. The Codeberg procedure below is the failover, kept
> ready but not configured.

Two invariants everything below serves:

1. **Nothing derived from the game leaves this repository.** No `LoK.exe`, no
   `GameData\`, no `*.cpk` / `*.usm` / `*.wem`, no generated C from
   `src/recomp/gen/`, no built `korra_recomp.exe`, no APK. Builders generate all
   of that locally from their own copy.
2. **A mirror is a commit, not a working directory.** The mirror script refuses a
   dirty tree, so what other people can clone is exactly a commit that exists
   here.

## Remotes

| Remote | Role | URL shape |
|---|---|---|
| `origin` (suggested) | primary, public | `https://github.com/<you>/korra-recomp.git` |
| `codeberg` | insurance mirror, synced by script | `ssh://git@codeberg.org/<you>/korra-recomp.git` |

Only `codeberg` is special-cased: the script refuses any other host unless you
pass `-AnyRemote`, which exists so the selftest can point it at a local bare repo.
A GitHub remote is not needed for the mirror to work.

## One-time setup

1. **Create the mirror** in the Codeberg web UI — <https://codeberg.org/repo/create>:
   owner = you, name = `korra-recomp`, **public**, and **do not** initialise it
   with a README, `.gitignore` or license (that would create a competing root
   commit and turn the first push into a non-fast-forward).
2. **Register a key.** Codeberg does not accept a password over SSH and does not
   use GitHub-style device auth. Add `~/.ssh/id_ed25519.pub` at
   <https://codeberg.org/user/settings/keys>, then confirm:

   ```powershell
   ssh -T git@codeberg.org      # expect: Hi <you>! You've successfully authenticated...
   ```

   A GitHub key is not automatically a Codeberg key. `Permission denied
   (publickey)` means the key is not registered, not that the script is wrong.
3. **Add the remote:**

   ```powershell
   git remote add codeberg ssh://git@codeberg.org/<you>/korra-recomp.git
   ```

4. **Dry run**, which does everything up to the push:

   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\sync_codeberg.ps1 -DryRun
   ```

   Expect `<n> tracked files clean; master @ <sha>  <subject>` and
   `would push master -> ssh://git@codeberg.org/...`.

## Routine use

```powershell
# after committing, whenever you want the mirror current
powershell -ExecutionPolicy Bypass -File scripts\sync_codeberg.ps1
```

The script, in order:

| Step | Check | Refusal |
|---|---|---|
| clean tree | `git status --porcelain` empty | exit 1, lists the paths |
| content guard | `python tools\check_content.py <every tracked path>` | exit 1, prints the guard's reasons |
| host | remote host is `codeberg.org` | exit 3 |
| reachability | `git fetch <remote>` | exit 5 with the `ssh -T` hint |
| history | remote tip is an ancestor of `HEAD` | exit 4, prints ahead/behind |
| push | `HEAD:refs/heads/<branch>`, then annotated tags | exit 5 |
| verify | re-reads the remote-tracking ref, requires the pushed SHA | exit 5 |

Exit codes: 0 ok, 1 dirty tree or guard, 2 no remote configured, 3 wrong host,
4 not a fast-forward, 5 git/push failure. `-DryRun` still fetches (read-only) so
the fast-forward check is real, then stops before `git push`.

Branches and remote name can come from the environment, which is what a scheduled
task needs: `KORRA_CODEBERG_REMOTE`, `KORRA_CODEBERG_BRANCH`. Every run appends a
line to `logs/sync_codeberg.log` (gitignored).

**Never `git push --force` to the mirror.** If the two histories genuinely
diverged, the mirror is disposable insurance: delete and re-create the Codeberg
repository, then sync again. The script has no force path on purpose.

## Source-only releases

Releases are tags, not uploads. The source tarball the host generates is the
artifact; nothing is attached to it.

```powershell
git tag -a v0.1.0 -m "M2: boots to main menu on Windows"
git push origin v0.1.0
powershell -ExecutionPolicy Bypass -File scripts\sync_codeberg.ps1   # tags follow
```

Pre-release checklist:

- [ ] `README.md` status table matches `docs/FINDINGS.md` (no milestone claimed
      that a build did not prove)
- [ ] `THIRD_PARTY_NOTICES.md` lists every component the release actually uses,
      with the license verified against upstream, and no component we merely
      studied
- [ ] the tag is annotated and its message says what is proven
- [ ] no release attachments: no APK, no ZIP of `build/`, no screenshots of game
      content that cannot be redistributed
- [ ] fresh-clone audit passes:

  ```powershell
  git clone --no-local . ..\korra-audit
  cd ..\korra-audit
  git ls-files | Select-String -Pattern '\.(cpk|usm|exe|dll|wem|bnk)$'
  python tools\check_content.py   # with the file list
  ```

- [ ] mirror synced and verified (`sync_codeberg: mirror verified at <sha>`)

## Why the mirror is worth the setup

The scene already lost `pcrecomp`'s `nfsu2-fixes` branch and its pinned commit
(`docs/TOOLCHAIN.md`), and our toolchain note had to be rewritten because of it.
A second host does not preserve history by itself — it preserves it only if the
sync is boring and regular. That is why the script is one command, checks the
content guard before it pushes, and never rewrites history.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `Permission denied (publickey)` | key not registered on Codeberg, or `ssh -T` picks a different key | add the key, or pin it in `~/.ssh/config`: `Host codeberg.org` / `IdentityFile ~/.ssh/id_ed25519` |
| `repository not found` after fetch | repository not created, or owner/name typo | create it in the UI; re-run `git remote set-url codeberg …` |
| exit 4, ahead/behind | someone pushed to the mirror (or it was created with a README) | re-create the mirror, then sync; do not force |
| exit 1, guard rejected `docs/…` | a file grew past 20 MiB, or a game file got tracked by mistake | move it out of the repo; if it is genuinely documentation, discuss raising `MAX_BYTES` in `tools/check_content.py` in its own commit |
| exit 1, "not clean" | uncommitted edits | commit them first: the mirror only ever publishes commits |
| mirror shows the same SHA but the file list looks different | line-ending normalisation on checkout, not content divergence | compare `git -C <clone> status`; if it is clean, nothing is wrong |

## Not done yet (state on 2026-11-19)

- **Hosting 2026-11-19:** GitHub is the primary host (public,
  work-in-progress, pushed at each milestone); the earlier local-only decision
  is superseded. The Codeberg mirror stays unconfigured — the script, this
  document and its selftest remain in the tree so it is a three-command change
  of mind rather than a rewrite.
- No CI. The guard and the selftest run locally; see
  `tests/sync_codeberg_selftest.ps1` (9 cases against a throwaway bare repo).

If a second copy is ever wanted without an account anywhere, the cheapest option
is a bundle or mirror clone onto separate media:

```powershell
git bundle create D:\backup\korra-recomp-$(Get-Date -Format yyyyMMdd).bundle --all
# or, restorable like a real remote:
git clone --mirror . D:\backup\korra-recomp.git
```
