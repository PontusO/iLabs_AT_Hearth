# SDK patches

Status: living document. Established 2026-09-18 (round 2, the Matter core).

This directory is the MG24 platform's SDK patch mechanism: the patches this arm
carries against the pinned Silicon Labs Matter extension, and this file.

**It is the project's third, and it deliberately looks like the two that came
before it.** `platform/esp32c6/sdk-patches/` has carried patches against
esp-matter and its nested connectedhomeip checkout since the combined-image
round, applied by a script that pins both trees by commit;
`platform/nrf54l15/sdk-patches/` carries the same change as this one against the
NCS workspace, applied by `west patch`. The layout here is the same shape,
`sdk-patches/<tree>/<name>.patch`, so a reader who knows one knows the others.

The per-tree directory is `matter_sdk/`, which is what the extension calls its
bundled CHIP checkout (`third_party/matter_sdk`). The nRF arm's is
`connectedhomeip/`, and that name was chosen there for a reason worth repeating:
the repository's `.gitignore` carries `matter/` for the CSA specification PDFs
and matches a directory of that name anywhere in the tree, so a patch filed
under `sdk-patches/matter/` is silently skipped by `git add`. `matter_sdk/` is
not ignored. Run `git check-ignore -v` on the file before adding a patch under
any new directory name.

## Why a script and not `west patch`

There is no west workspace here. The Simplicity SDK and the Matter extension
arrive as Conan packages through SLT-CLI, into a read-only cache that belongs to
SLT and is replaced by the next `slt install`. So this arm looks like the C6's:
`platform/silabs/fw/sdk-prepare.sh` is the runner.

It does three things, and they are one act on purpose:

1. Copies `$MATTER_EXT_INSTALL` (the Conan cache) to `$MATTER_EXT_ROOT` (ours),
   replacing whatever was there, and symlinks the two SDK roots the extension
   expects under `third_party/`.
2. Verifies each patch against its `.sha256` and applies it, so a patch edited
   without refreshing its hash fails loudly instead of applying something nobody
   reviewed.
3. Writes `$MATTER_EXT_ROOT/.hearth-stamp` with the patch revision.

The copy is what makes this safe to automate where the C6 and nRF arms could
not: this script never touches a tree anyone else owns. The cache stays
pristine, the copy is disposable, and re-running the script is always correct.

**Re-run it after any `slt install` or `slt update`.** A package revision that
moves takes the prepared copy's provenance with it, and nothing else will say
so.

## Applying

```bash
source platform/silabs/toolchain.env      # see the note below
platform/silabs/fw/sdk-prepare.sh
```

On a machine with no prepared tree, sourcing `toolchain.env` prints the
`run platform/silabs/fw/sdk-prepare.sh` line and returns 1. That is the gate
doing its job: every variable the script needs is exported before the gate runs,
so the source is still good and the prepare runs straight afterwards. Source the
file again when the prepare is done and it returns 0.

To take the patches back out, run the prepare with the patch directory empty, or
simply read the stock file out of `$MATTER_EXT_INSTALL`, which is never
modified. There is no `clean` verb because there is nothing to clean: the
patched tree is a copy, and the pristine tree is one directory away.

## How a reader knows the tree is patched

Three ways, in increasing order of trustworthiness:

1. `ls platform/silabs/sdk-patches/matter_sdk/*.patch` says what SHOULD be
   applied. It looks at this repository only, not at the tree.
2. `diff -u "$MATTER_EXT_INSTALL/third_party/matter_sdk/<file>"
   "$MATTER_EXT_ROOT/third_party/matter_sdk/<file>"` answers the question for
   real, and shows anything else that has been edited by hand.
3. **The environment refuses.** `toolchain.env` reads
   `$MATTER_EXT_ROOT/.hearth-stamp`, looks for the patch REVISION, and returns 1
   naming the prepare script if it is absent or is the wrong number. Nothing
   builds without a sourced environment.

The third exists because of a property this patch shares with every patch worth
upstreaming: it defaults to the stock behaviour when its macro is unset. That is
what makes it acceptable upstream, and it is exactly what makes its ABSENCE
invisible. An unpatched tree compiles, links, boots and works; it just quietly
gives back the RAM the patch was taken for, and the next person to measure this
platform gets a number that disagrees with every document in the repository for
no visible reason. Hence a hard refusal.

**It checks the revision, not merely the presence, and that distinction is the
whole point of the marker.** A tree carrying an OLDER cut of the same patch
passes a presence check: it configures, builds and measures cleanly while
running code nobody reviewed, which is a worse failure than an unpatched tree
because nothing looks wrong. The `.sha256` beside each patch protects the patch
FILE in this repository; nothing else looks at the state of the tree it was
applied to. So each patch carries a `HEARTH_<name>_PATCH_REV <n>` define next to
its configuration macro, marked in place as downstream-only and to be dropped
when upstreaming, and `HEARTH_EEM_PATCH_REV` in `toolchain.env` is the number
this tree expects. Bump the two together when a patch is re-cut.

Both branches were verified on 2026-09-18 by running them, not by reading the
code. Three cases, against the real files:

```
$ bash -c 'source platform/silabs/toolchain.env; echo "source returned $?"'
  # round 1's hand-made copy, which has no stamp at all
toolchain.env: /home/pontus/silabs/matter_extension-2.8.1 is not prepared at patch revision 2
toolchain.env: run platform/silabs/fw/sdk-prepare.sh
source returned 1

$ echo "HEARTH_EEM_POOL_PATCH_REV 1" > "$MATTER_EXT_ROOT/.hearth-stamp"
  # a prepared tree carrying an OLDER cut of the patch: the case a presence
  # check would pass
toolchain.env: ... is not prepared at patch revision 2
toolchain.env: run platform/silabs/fw/sdk-prepare.sh
source returned 1

$ platform/silabs/fw/sdk-prepare.sh && source platform/silabs/toolchain.env
source returned 0
```

The second case is the one that matters, and it is the reason the marker exists:
the tree is fully patched, it builds and runs, and only the revision says the
patch is not the one this repository ships.

The prepare itself also has a positive control, a `grep` for the marker in the
tree it has just patched, so a patch that applied to the wrong place cannot
write a stamp.

Adding a patch means one more `.patch` and `.sha256` pair in
`matter_sdk/`; `sdk-prepare.sh` loops over the directory. A patch with its own
revision marker means one more variable in `toolchain.env` and one more clause
in the gate.

## Regenerating a patch

Edit the file in `$MATTER_EXT_ROOT/third_party/matter_sdk`, then diff it against
the pristine copy in the Conan cache:

```bash
source platform/silabs/toolchain.env
F=src/app/clusters/electrical-energy-measurement-server/ElectricalEnergyMeasurementCluster.cpp
diff -u "$MATTER_EXT_INSTALL/third_party/matter_sdk/$F" \
        "$MATTER_EXT_ROOT/third_party/matter_sdk/$F"
```

and rewrite the `--- a/$F` / `+++ b/$F` header lines, which is what makes the
result apply with `patch -p1 -d third_party/matter_sdk`. Keep the existing patch
file's header above the `---` separator: it is the upstream commit message and
is meant to be usable verbatim as a pull request description. **Bump
`HEARTH_<name>_PATCH_REV` in the patch and `HEARTH_EEM_PATCH_REV` in
`toolchain.env` together**, then refresh the hash from inside this directory, so
the name in the checksum file stays a bare basename:

```bash
cd platform/silabs/sdk-patches/matter_sdk
sha256sum <the patch file> > <the patch file>.sha256
```

## The patches

### `matter_sdk/electrical-energy-measurement-instance-pool.patch`

`ElectricalEnergyMeasurement`'s `gMeasurements` table is indexed by
`emberAfGetClusterServerEndpointIndex()`, whose range on a dynamic-endpoint
build is the entire dynamic endpoint space, so the array is declared
`[MATTER_DM_..._SERVER_ENDPOINT_COUNT + CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT]`
and charged in full the moment the cluster enters the build: 17 x 496 =
**8,432 bytes** at this port's dynamic endpoint count of 16, whether or not any
composition ever declares an energy endpoint. The endpoint-block technique the
port uses everywhere else cannot reach it, because the indexing happens inside
the SDK.

The patch caps the table at
`CHIP_CONFIG_ELECTRICAL_ENERGY_MEASUREMENT_MAX_INSTANCES` and claims slots by
endpoint id on first use, reclaiming **4,440 bytes** at the value this platform
sets (8, in `../src/CHIPProjectConfig.h`). A slot whose endpoint no longer
serves the cluster is reclaimed automatically, so no new SDK API is needed and
no bridge has to learn to release anything.

**8 is not a spare number, it is `MT_MEAS_MAX`**, the port's own answer to how
many measurement-capable endpoints one composition may carry. A pool smaller
than the port's own capacity would let a composition be ACCEPTED and then not
SERVED: past the pool, `SetMeasurementAccuracy()` fails, the port logs and
continues, `AT+MTEPAPPLY` still answers `OK`, and a controller then gets a
Failure reading the mandatory `Accuracy` attribute of a cluster the endpoint
advertises, with nothing on the AT wire to say so.

**This round does not compile the cluster in**, so the reclaim is not measured
here: the patch is in place, the gate is in place, and the figure above is the
nRF arm's measurement of the identical change. The round that adds the energy
device types measures it on this part and records it in
`platform/silabs/README.md`.

Cut against the extension's bundled `third_party/matter_sdk` on 2026-09-18. The
result is the same change as the nRF arm's, hunk for hunk and line for line:
the two SDK trees carry this file identically. The patches are kept separate
anyway, because the arms pin different SDKs and either may move on its own; a
shared patch would make one arm's SDK bump the other arm's problem.

Not yet submitted upstream. The patch file's header is written as the pull
request description it should become.
