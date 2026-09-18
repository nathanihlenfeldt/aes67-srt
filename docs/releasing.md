# Releasing

## The version

`MAJOR.MINOR.PATCH`, set once in `CMakeLists.txt` (`project(aes67-srt VERSION x.y.z)`) and reported by
`--version` and the status endpoint. Pre-1.0, a **minor bump may carry a breaking wire-format change**
— the fragment header was one — so **both ends of a link must run the same version**; the format does
not negotiate. When that stops being true is a 1.0 conversation.

To cut a release: bump the version, move the changelog's *Unreleased* entry under the new number, and
tag `vX.Y.Z`.

## The checklist

1. **`./scripts/check.sh` is green** — format, build, tests, config validation, the refusals. This is
   the gate; nothing below substitutes for it.
2. **Bump the version** in `CMakeLists.txt` and add the `CHANGELOG.md` entry under the new number.
3. **Build the artifact.** The appliance installs from source by design (which is also how the GPL
   source-availability obligation is met, below), so the artifact is a source tarball from the tag:
   ```
   git archive --format=tar.gz --prefix=aes67-srt-X.Y.Z/ -o aes67-srt-X.Y.Z.tar.gz vX.Y.Z
   ```
4. **Checksum it**:
   ```
   shasum -a 256 aes67-srt-X.Y.Z.tar.gz > aes67-srt-X.Y.Z.tar.gz.sha256
   ```
5. **Install from the artifact on a clean machine**, not from a working tree — the appliance with
   `scripts/install.sh`, the endpoint with `scripts/install-mac.sh` — and confirm the preflight passes
   and audio flows end to end. **For the Pi this is issue #28 and it is not optional**: an installer
   that has only ever run on the machine it was written on is not an installer.
6. **Tag and publish** the release with the artifact, its checksum, and the changelog entry.

## Why a source artifact, not a binary

The appliance is **GPL-3.0** (ADR 0002): distributing a binary means distributing the corresponding
source. The installer builds from the repository, which satisfies that without a second artifact to
keep in step, and `git archive` from the tag is the exact source that produced the release.

A prebuilt binary for a site that cannot build on the appliance is a **future** option, and it arrives
with its own obligation: the corresponding source, and a signature a site can verify against. The
macOS endpoint's signing is issue #30.