# SDK patches

Status: living document. Established 2026-08-30 (memory reclaim round B).

This directory is the nRF54L15 platform's SDK patch mechanism: the patches
this arm carries against the pinned nRF Connect SDK workspace, the metadata
`west patch` needs to apply them, and this file.

**It is not the project's first, and it deliberately looks like the one that
came before it.** `platform/esp32c6/sdk-patches/` has carried two patches
against esp-matter and its nested connectedhomeip checkout since the
combined-image round, applied by
`platform/esp32c6/scripts/apply-sdk-patches.sh`, which pins both trees by
commit and refuses outright on drift. The layout here is the same shape,
`sdk-patches/<tree>/<name>.patch`, so a reader who knows one knows the
other. The two differ only where the SDKs differ: that arm's SDK is a pair
of plain git checkouts with no west workspace to hang a runner off, so it
has a script; this one IS a west workspace, and west brings its own runner.

Nothing here is applied automatically. Applying a patch changes a tree that
is shared by every project on the machine, so it stays an explicit act.
What IS automatic is the refusal to build against a tree that is unpatched,
or patched at the wrong revision: see "How a reader knows the tree is
patched" below.

## Why `west patch` and not another script

The Zephyr tree that ships with NCS carries `west patch`
(`zephyr/scripts/west_commands/patch.py`, registered in
`zephyr/scripts/west-commands.yml`), a first-party extension command present
in this vintage (NCS v3.3.4, west 1.4.0). It was chosen over copying the C6
arm's script for four reasons:

- **It is already installed.** No new tooling, no new dependency, and it is
  the mechanism a Zephyr or NCS engineer will already recognise.
- **It verifies before it applies.** Every entry carries a `sha256sum` that
  is checked against the patch file first, so a patch edited without
  refreshing its hash fails loudly instead of applying something nobody
  reviewed.
- **It carries the provenance in the same file as the patch.** Author, date,
  whether the change is upstreamable, the pull request URL once there is
  one, and whether it has landed. A patch whose upstream status lives only
  in somebody's memory becomes permanent by accident.
- **It reverses cleanly.** `west patch clean` restores the module.

The patch payload lives HERE, in this repository, not in the SDK workspace.
That is the point: the workspace is disposable and reproducible from the
manifest, this repository is the thing under version control.

**The per-tree directory is named `connectedhomeip/`, not `matter/`, and it
has to be.** The repository's `.gitignore` carries `matter/` for the CSA
specification PDFs, which are copyright and never committed, and it matches
any directory of that name anywhere in the tree. A patch filed under
`sdk-patches/matter/` is silently skipped by `git add` (which ignores
ignored files without a word when it is given a directory), so the round
that established this mechanism shipped a `patches.yml` pointing at a file
that was not in the repository. `connectedhomeip/` is the tree's real name,
is what the C6 arm already calls it, and is not ignored. Check
`git check-ignore -v` on the file before adding a patch under a new
directory name.

## Applying

From the west workspace top directory (`~/ncs/v3.3.4` on this machine),
with the NCS toolchain activated (the env block in `../README.md`):

```bash
PATCHES=<repo>/platform/nrf54l15/sdk-patches
cd ~/ncs/v3.3.4
west patch -l $PATCHES/patches.yml -b $PATCHES apply
```

The `-l` and `-b` options must come BEFORE the subcommand; `west patch apply
-l ...` is an argument error. Both paths are absolute, which is what lets
the patch definitions live outside the workspace: `-sm/--src-module` is the
alternative and it forbids absolute paths, because it expects the patches to
live inside a Zephyr module.

To take them back out:

```bash
cd ~/ncs/v3.3.4
west patch -l $PATCHES/patches.yml -b $PATCHES clean
```

`clean` runs `git checkout .` in each patched module. This directory's
`patches.yml` deliberately empties the schema's default `clean-command`
(`git clean -d -f -x`), which would otherwise delete every untracked file in
`modules/lib/matter`, build output and local notes included.

## What `west update` does to a patched tree

Nothing good, and nothing silent, which is the useful part.

`west update` checks each project out at the manifest revision. With a
patched working tree it either leaves the modification in place (when the
revision has not moved) or refuses with git's "local changes would be
overwritten" (when it has). It never merges the patch forward and it never
reports the tree as patched.

So the rule around an SDK bump is:

1. `west patch ... clean` first.
2. `west update`.
3. `west patch ... apply` and rebuild.

If a patch no longer applies after a bump, that is the signal that upstream
has moved under it. Re-cut it against the new tree (below) rather than
forcing it.

## How a reader knows the tree is patched

Three ways, in increasing order of trustworthiness:

1. `west patch -l ... -b ... list` prints what SHOULD be applied. It reads
   the yml only; it does not look at the tree.
2. `git -C ~/ncs/v3.3.4/modules/lib/matter status --porcelain` shows the
   patched files as modified. This is what actually answers the question,
   and it also shows anything else that has been edited by hand.
3. **The build refuses.** `../CMakeLists.txt` reads the patched SDK file at
   configure time, looks for the patch's REVISION MARKER, and stops with
   `FATAL_ERROR` and the exact `west patch` commands if it is absent or is
   the wrong number.

The third exists because of a property this patch shares with every patch
worth upstreaming: it defaults to the stock behaviour when its macro is
unset. That is what makes it acceptable upstream, and it is exactly what
makes its ABSENCE invisible. An unpatched tree compiles, links, boots and
works; it just quietly gives back the RAM the patch was taken for, and the
next person to measure this platform gets a number that disagrees with every
document in the repository for no visible reason. Hence a hard error.

**It checks the revision, not merely the presence, and that distinction is
the whole point of the marker.** A workspace carrying an OLDER cut of the
same patch passes a presence check: it configures, builds and measures
cleanly while running code nobody reviewed, which is a worse failure than an
unpatched tree because nothing looks wrong. `patches.yml`'s `sha256sum`
protects the patch FILE in this repository; nothing else looks at the state
of the tree it was applied to. So each patch carries a
`HEARTH_<name>_PATCH_REV <n>` define next to its configuration macro, marked
in place as downstream-only and to be dropped when upstreaming, and the gate
matches the number this tree expects. Bump the two together when a patch is
re-cut. Both branches are verified by removing the patch and by moving the
expected number, not by reading the CMake.

Adding a patch means extending that check. One `file(READ)` plus two
`string(FIND)`s per patch, in the same block.

## Regenerating a patch

Edit the file in the SDK tree, then, from the module:

```bash
cd ~/ncs/v3.3.4/modules/lib/matter
git diff > /tmp/body.diff
```

Keep the existing patch file's header (it is the upstream commit message and
is meant to be usable verbatim as a pull request description), replace the
diff below the `---` line, **bump `HEARTH_<name>_PATCH_REV` in the patch and
the matching number in `../CMakeLists.txt`**, then refresh the hash:

```bash
sha256sum <the patch file>
```

and paste it into `patches.yml`. The hash is checked before anything is
applied, so a stale one fails the apply rather than being ignored.
`west patch` reads the file with universal newlines, so a plain
`sha256sum` matches as long as the file has Unix line endings.

## The patches

### `connectedhomeip/electrical-energy-measurement-instance-pool.patch`

`ElectricalEnergyMeasurement`'s `gMeasurements` table is indexed by
`emberAfGetClusterServerEndpointIndex()`, whose range on a dynamic-endpoint
build is the entire dynamic endpoint space, so the array is declared
`[MATTER_DM_..._SERVER_ENDPOINT_COUNT + CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT]`
and charged in full the moment the cluster enters the build: 17 x 496 =
**8,432 bytes** here, whether or not any composition ever declares an energy
endpoint. The endpoint-block technique this port uses everywhere else cannot
reach it, because the indexing happens inside the SDK.

The patch caps the table at
`CHIP_CONFIG_ELECTRICAL_ENERGY_MEASUREMENT_MAX_INSTANCES` and claims slots by
endpoint id on first use, reclaiming **4,440 bytes** at the value this
platform sets (8, in `../src/chip_project_config.h`). A slot whose endpoint
no longer serves the cluster is reclaimed automatically, so no new SDK API
is needed and no bridge has to learn to release anything.

**8 is not a spare number, it is `MT_MEAS_MAX`**, this port's own answer to
how many measurement-capable endpoints one composition may carry. A pool
smaller than the port's own capacity would let a composition be ACCEPTED and
then not SERVED: past the pool, `SetMeasurementAccuracy()` fails, the port
logs and continues, `AT+MTEPAPPLY` still answers `OK`, and a controller then
gets a Failure reading the mandatory `Accuracy` attribute of a cluster the
endpoint advertises, with nothing on the AT wire to say so. Before the patch
that could not happen, because the table had a slot for every endpoint the
port could build. `mt_matter_eem_reserve()` (`../port/mt_matter_zephyr.cpp`)
now claims a slot before the endpoint is built and stops the composition
rebuild naming the macro, which is what every other pool in this port does,
and `mt_matter_zephyr.cpp` static_asserts that the pool cannot fall below
`MT_MEAS_MAX` again.

Not yet submitted upstream. The patch file's header is written as the pull
request description it should become, and `patches.yml` carries
`merge-status: false` plus a commented-out `merge-pr` line to fill in on the
day it is. The `merge-pr` key is commented rather than present and empty
because the schema requires a URL, so an empty value fails validation and
`west patch` then refuses the whole file.
