# Local DPDK patches (D-131)

`tools/bootstrap_dpdk.py` extracts the tarball pinned in `../dpdk.json`, checks
its sha256, then applies every `NNNN-<slug>.patch` here with `-p1`, in file-name
order. No patches means pristine DPDK. Any other file here, apart from this
README and `.gitkeep`, is an error, so a misnamed patch cannot be skipped
without anyone noticing.

## Adding a patch

1. Make the change in a git checkout of the pinned DPDK version, as one commit
   per fix, with these two trailers at the end of the message:

       Upstream-Status: Pending
       BESS-Decision: D-130

   `Upstream-Status` is one of:
   - `Pending`: not sent upstream yet.
   - `Submitted <url>`: sent, and `<url>` is the mailing-list or patchwork
     link.
   - `Accepted <dpdk-commit>`: merged upstream as `<dpdk-commit>`, which is
     7 to 40 hex digits.
   - `Local-only <reason>`: never going upstream, and why.

   `BESS-Decision` is the decision in `docs/decisions.md` that needs the patch.
2. `git format-patch -1 --start-number <N>` writes `NNNN-<slug>.patch`. Copy it
   here with the next free number.
3. `tools/bootstrap_dpdk.py --check-patches` checks the names, both trailers,
   and that the series applies cleanly, in order, to a pristine extraction in a
   temporary directory. It does not touch any DPDK tree. CI runs it in the
   `layers` step (`tools/ci_profile.py layers`).
4. Run the bootstrap again. It finds a new series and rebuilds what changed.

## What the bootstrap does with a series

The series identity is a sha256 over each patch's name and content digest, in
order. The bootstrap writes it as `.bess-dpdk-patches.json` into the source
tree (`deps/dpdk-<ver>/`) and into each install prefix (`install`,
`install-<variant>`).

- If the source tree's stamp differs from the series, or it has no stamp, the
  tree is replaced by a fresh extraction with the series applied. The series
  is applied in a temporary directory and the result moved into place, so a
  failed patch leaves the old tree untouched and a tree is never patched
  twice. A failure names the patch and prints the rejected hunks.
- Build trees inside the source tree are kept only if the old tree was
  pristine. Unchanged files keep the tarball's mtimes and patched files are
  newer, so ninja rebuilds exactly what changed. After a patched tree they are
  deleted and rebuilt from scratch, because a reverted file goes back to an
  older mtime that ninja would not notice.
- If an install's stamp differs from the series, the install is deleted before
  `ninja install`, so a file that only an old patch installed does not stay
  behind. An install with no stamp counts as the empty series, because every
  install made before D-131 is pristine. With no patches, an existing install
  is not rebuilt.

CI's DPDK cache keys include `deps/dpdk-patches/**`.

## Upgrading DPDK

On every DPDK version bump (`../dpdk.json`), each patch must either still
apply (`--check-patches`) or be removed, with the reason in the bump's commit
message. Remove an `Accepted` patch whose commit is in the new version. If
upstream fixed the defect differently, rerun the evidence named in the patch's
decision before removing the patch.

## What a build records

`tools/build_info.py` writes the series of the DPDK the build links (the
install prefix's stamp, found with `pkg-config`) into the build's
`build-info.json` (installed as `share/bess/build-info.json`) as
`dpdk.patches: [{name, sha256, upstream_status}]`. The list is empty for
pristine DPDK. If that is not the checkout's series, the bootstrap was not run
again after a series change: the build warns and records what it linked. The
SPDX SBOM (`share/doc/bess/bess.spdx.json`) keeps the dpdk
package's upstream version and tarball checksum, so vulnerability matching by
version still works, and lists the patches in that package's `sourceInfo`.
