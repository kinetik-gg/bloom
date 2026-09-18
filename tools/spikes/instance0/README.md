# INSTANCE-0 eager-expansion cost spike

Status: throwaway

Updated: 2026-09-17

This is not product code. It exists to put numbers under one claim in
[ADR 0023](../../../docs/decisions/0023-instances-and-per-element-data.md): that an instance set
cannot be expanded into N ordinary Layer Outputs and one Merge, and therefore needs its own
compiled operation. Delete it once INSTANCE-1 lands.

It builds a compiled plan by hand — one Solid, N Layer Outputs over it in a grid, one Merge holding
all N, one Output — and runs it through the real `CpuCompositionEvaluator`. Nothing is mocked: the
plan is the same immutable grammar the snapshot compiler produces, and the evaluator is the one the
viewer uses.

## Build and run

The spike is behind `BLOOM_BUILD_SPIKES`, which is `OFF` by default, so the product build never
compiles or links it. Configure a build directory of its own rather than turning the option on in a
shared one:

```sh
cmake -S . -B build/instance0-spike \
    -DCMAKE_BUILD_TYPE=Release \
    -DBLOOM_BUILD_DESKTOP=OFF \
    -DBLOOM_BUILD_SPIKES=ON \
    -DBUILD_TESTING=OFF \
    -DBLOOM_DEPENDENCY_MODE=qualified \
    -DBLOOM_DEPENDENCY_PREFIX="$PWD/build/dependency-prefix"
cmake --build build/instance0-spike --target bloom_instance0_spike --parallel 3
./build/instance0-spike/tools/spikes/instance0/bloom_instance0_spike [instances]
```

`instances` defaults to 500. Release matters: a Debug reference evaluator is an order of magnitude
slower and its numbers say nothing about an artist's frame.

## What each line measures

| Line | What it shows |
| --- | --- |
| `preview budget` | The evaluator's preflight estimate for the expanded set against a 2 GiB interactive budget. It charges one FULL composition frame per image operation alive at once, so N instances estimate N frames. |
| `expansion (cold)` | One evaluation with an empty operation cache: every instance is a miss. |
| `expansion (warm)` | The same plan again on the same evaluator: the whole instance set served from digest. |
| `one instance moved` | One instance's position changed and the plan republished at a NEW revision. The cache re-looks-up by resolved content, so unchanged instances are adopted rather than re-evaluated — but the Merge re-folds all N entries. |
| `expansion (short frame)` | The same N over a composition an eighth as tall, to separate the Composition Output crop from the per-instance work. |
| `sweep N=` | Cold evaluation across instance counts, to show how the cost scales. |
| `value ops=` | A value graph of 4 x N independent per-element scalars, to price per-element attributes as individual value-graph nodes. |

The measured results, and what the ADR concluded from them, are recorded in ADR 0023 under
"Measured baseline". Re-run the spike before quoting a number that came from a different machine.
