# Copied SDK sources

Files here are VERBATIM copies of Silicon Labs Matter extension sources, kept
in the repository only because `slc` resolves a project's `source:` paths
relative to the `.slcp` and there is no portable relative path from this
repository to `$MATTER_EXT_ROOT`. The extension's own apps list these files as
`../../../../third_party/matter_sdk/examples/platform/silabs/...` because their
`.slcp` lives four directories below the extension root; Hearth's does not live
under the extension at all, and an absolute path would pin the project to one
machine.

Do not edit them. Each file's header comment names its origin path and the
extension version it was taken from.

**They go stale silently on an SDK bump**, exactly like
`config/sl_openthread_features_config.h`. The check is a diff:

```bash
source platform/silabs/toolchain.env
for f in platform/silabs/src/sdk/*.cpp; do
    diff "$MATTER_EXT_ROOT/third_party/matter_sdk/examples/platform/silabs/$(basename "$f")" "$f"
done
```

Today that diff is the added header comment on each file and nothing else.

| File | Origin | Why Hearth compiles it |
|---|---|---|
| `SoftwareFaultReports.cpp` | `third_party/matter_sdk/examples/platform/silabs/SoftwareFaultReports.cpp`, extension 2.8.1 | The FreeRTOS fault hooks and the ARM fault handlers. Without it `vApplicationStackOverflowHook`, `vApplicationMallocFailedHook` and `HardFault_Handler` are the SDK's weak stubs and a fault is a silent hang. `matter_platform_mg` compiles it for the samples; Hearth does not list that component (README, "The components"). |

`silabs_utils.cpp`, which the samples compile beside it, is deliberately NOT
here: its only content is `appError()`, which the sample's bootstrap calls when
it decides to die. Hearth's bring-up returns `CHIP_ERROR` to its caller
instead, and nothing else in this image references the symbol.
