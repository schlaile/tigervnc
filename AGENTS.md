# Notes for agents working on this fork

Read [BRANCHES.md](BRANCHES.md) first: which branch holds what, and how a
change travels from a feature branch to the packages.

- **Where to commit**: features in their feature branch (`file-drop`,
  `device-link`, `client-cert`, `server-nojpeg`), then merge into `wws`, then merge `wws`
  into `wws-bookworm-xvnc` (keeping its own `debian/`, see BRANCHES.md).  Packaging only in the `debian/` of the branch
  that builds it.  `wws-stretch-xvnc` is tigervnc 1.7: server-side changes
  are backported there by hand.
- **Worktrees** (on the operator's machine): `~/tigervnc` (file-drop),
  `~/tigervnc-device-link`, `~/tigervnc-client-cert`, `~/tigervnc-nojpeg`, `~/tigervnc-wws`
  (wws), `~/tigervnc-bookworm-xvnc` (wws-bookworm-xvnc),
  `~/tigervnc-stretch-pkg` (wws-stretch-xvnc).
- **Commit messages**: feature branches in upstream style without task
  references (they are meant for upstream); `wws` and the packaging
  branches may name the WWS task.
- **CI**: GitHub Actions build Linux, Windows and macOS with `-Werror`
  and `-Wshadow`; compile new code with those warnings before pushing.  A
  tag `wws-filedrop-N` on `wws` makes the release with the Windows and
  macOS downloads; the operator decides when.
- **Debian packages**: built with Bob (`bob-build submit --target …` in
  the worktree); never `bob-build publish`, the operator publishes.  Bump
  `debian/changelog` before building a changed package.
- **What the tills need**: Xvnc with `FileDropEndpoint` (stretch: from
  `wws-stretch-xvnc`, bookworm: from `wws-bookworm-xvnc`), started by
  inetd through `start_tigervnc`; package names and paths must stay those
  of Debian's packages, so that the tills' configuration does not change.
- **Upstream**: do not contact TigerVNC upstream before the operator says
  so (the features are to be rolled out and tried first).
