# Branches of the Schlaile fork

This fork carries features meant for upstream TigerVNC, and packages for the
WWS (Schlaile's merchandise management, whose tills and desktops use VNC).
There is one code base; branches differ in which features they join and in
`debian/`.

| Branch | What it is | Packages | Bob targets |
|--------|------------|----------|-------------|
| `file-drop` | VNC File Drop, on upstream/master only | – (upstream-bound) | – |
| `device-link` | VNC Device Link, on upstream/master only | – (upstream-bound) | – |
| `client-cert` | X.509 client certificates, on upstream/master only | – (upstream-bound) | – |
| `wws` | upstream/master + merges of all feature branches; CI for Windows and macOS (tags `wws-filedrop-N` make the releases) | `tigervnc-wws-viewer` (viewer only) | stretch-i386, stretch-amd64, bullseye-i386, bullseye-amd64, bookworm-amd64, trixie-amd64 |
| `wws-bookworm-xvnc` | `wws` + Debian's server packaging (from Debian's tigervnc 1.15) | `tigervnc-standalone-server`, `tigervnc-common`, … replacing Debian's at a higher version | bookworm-amd64 |
| `wws-stretch-xvnc` | Debian stretch's tigervnc 1.7 + backports of the server sides | the same server packages for stretch | stretch-i386, stretch-amd64 |

## How a change travels

1. Fix or extend a feature in its feature branch (upstream style, no task
   references in its commits).
2. Merge the feature branch into `wws`.
3. Merge `wws` into `wws-bookworm-xvnc`. Only `debian/` differs: `wws`
   packages the viewer, `wws-bookworm-xvnc` the server, so conflicts in
   `debian/` are normal. Keep the server's (`git checkout --ours -- debian/…`)
   and remove files that only the viewer package has.
4. Server-side changes for stretch are backported by hand to
   `wws-stretch-xvnc` (tigervnc 1.7 is too old to merge into).
5. Bump the `debian/changelog` of every branch whose package changes, build
   with Bob, and leave publishing to the operator.

Never stack one feature branch on another, never commit features directly
to `wws` or the packaging branches.
