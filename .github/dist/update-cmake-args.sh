#!/bin/sh
# Fork only: CMake arguments for the viewer's update check (UpdateCheck.h)
# in the dist-* jobs.  A tag wws-filedrop-N is release N; other builds are
# release 0 (they only show that the check compiles; their viewer offers
# every release).  The public key is .github/dist/update-key.txt (base64
# of the 32-byte Ed25519 key); its private half signs update.txt on the
# download page (ps-wws-webseite tools/tigervnc-downloads.sh).
set -eu
release=0
case "${GITHUB_REF:-}" in
refs/tags/wws-filedrop-*)
	release=${GITHUB_REF#refs/tags/wws-filedrop-}
	case $release in
	''|*[!0-9]*) echo "unexpected tag $GITHUB_REF" >&2; exit 1 ;;
	esac
	;;
esac
key_file=$(dirname "$0")/update-key.txt
if test -s "$key_file"; then
	key=$(tr -d ' \n\r' <"$key_file")
elif test "$release" != 0; then
	echo "no $key_file: release $release without update check" >&2
	exit 0
else
	key=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=   # compile check only
fi
echo "-DUPDATE_URL=https://ps.schlaile.de/tigervnc/download/dateien/update.txt" \
	"-DUPDATE_KEY=$key -DUPDATE_RELEASE=$release"
