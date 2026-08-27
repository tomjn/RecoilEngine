# Task: test the candidate fixes for the KosmicKrisp Metal 4 memory leak

You are testing whether either of two changes fixes a memory leak that currently pins our Mesa build to a hand picked commit. The output is measurements and a draft reply for a human to post upstream. You are not fixing the engine and you are not posting anything.

Read `coding-agents/MACOS_PERFORMANCE.md` item 11 and `coding-agents/MACOS_RENDERER_PLAN.md` from line 740 before starting. They hold the investigation this continues.

## Background

Mesa commit `c08dba83025 kk: Move to Metal4 command encoding` (2026-06-16) made KosmicKrisp give every render pass its own `MTL4CommandBuffer` with its own `MTL4CommandAllocator`. Those allocators only return memory at destroy, which only happens when the `VkCommandBuffer` is destroyed. Under the engine that leaks about 5 GiB a second and reaches 32 GiB in 12 seconds. Six attempts to bound it from Zink with existing knobs all failed, because one Zink batch state holds thousands of render passes, so the reset granularity is far coarser than the allocation granularity.

We are therefore pinned to `56588ef0665`, the commit before it. That blocks every Mesa update, including released versions, because both Metal 4 commits are in `mesa-26.2.0-rc1` onwards and 26.1.x lacks the nullDescriptor fix we need.

Filed as <https://gitlab.freedesktop.org/mesa/mesa/-/work_items/15998>. The HTML is behind Anubis, which denies headless browsers. A headed `agent-browser` session passes the challenge. The REST API serves issue metadata without a challenge but needs authentication for comments, and we have no token.

On 2026-08-23 two KosmicKrisp developers replied. Aitor Camacho confirmed the mechanism, said he does not consider it a KosmicKrisp bug, and pointed at his own open draft as something to test. squidbus questioned whether the diagnosis fully explains the growth, since a reset that makes memory available for reuse should plateau rather than grow.

## The two hypotheses

**H1, the driver side.** MR !43697, "kk: One MTL4CommandBuffer per VkCommandBuffer", branch `kk-encoding-rework`, open as a draft since 2026-08-12. It changes the ratio from one command buffer per render pass to one per `VkCommandBuffer`. Against our measured 68177 render passes in 12 seconds, that is the difference between thousands of allocators and a handful. Aitor explicitly invited testing.

**H2, the Zink side, which nobody upstream has proposed.** Zink creates its command pools with `flags = 0` (`zink_batch.c:306`, `cpci` is zeroed and only `sType` and `queueFamilyIndex` are set) and resets them with `flags = 0` (`zink_batch.c:59` and `:62`). It never passes `VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT`. Per the Vulkan spec a reset without that bit permits the implementation to retain the memory for reuse, which is exactly what KosmicKrisp does. So Zink may be asking the driver to keep the memory, and the driver obliging, with the pathology arising only because each allocator's retained memory cannot be shared with any other.

Test this by passing `VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT` in both `ResetCommandPool` calls in `zink_batch.c`. Consider `VK_COMMAND_POOL_CREATE_TRANSIENT_BIT` at creation as a second variant if the first shows movement.

Note the line numbers above come from our pinned pre Metal 4 checkout. Verify them against whatever tree you build.

## What to build

Do not disturb the working setup. `~/dev/mesa-install-premtl4` is what the packaged engine runs against and must still work when you finish.

`~/dev/mesa` is a clone of upstream currently on branch `premtl4` with no local commits. Record the branch you start on and check it back out when you are done.

Fetch the MR with `git fetch origin refs/merge-requests/43697/head:mr43697`.

Build with the two pass recipe in `coding-agents/MACOS_BUILD.md` section 2, unchanged except for a distinct build directory and prefix per configuration. Mesa forces LLVM on whenever CLC is enabled and KosmicKrisp needs CLC, which is why pass one exists. Do not try to shortcut it.

You need four configurations to make the result mean anything:

| Config | Source | Prefix |
|---|---|---|
| A, baseline good | `56588ef0665`, current pin | `~/dev/mesa-install-premtl4`, already built |
| B, baseline bad | merge base of `mr43697` and `main` | `~/dev/mesa-install-mr43697-base` |
| C, H1 | `mr43697` | `~/dev/mesa-install-mr43697` |
| D, H2 | B plus the `RELEASE_RESOURCES` change | `~/dev/mesa-install-zinkrelease` |

B is the control. Without it, a good result from C proves nothing, because C sits on months of unrelated upstream change.

## What to measure

**Step zero: find out whether the reproducer discriminates.** `coding-agents/test-scripts/kk_mipmap_leak.c` builds standalone against a Mesa prefix, with the command in its header comment. It creates 100 textures of 512x512, generates mips and deletes them, never drawing or presenting. Recorded figures on an M1 Pro: 2626mb with no sync, 224mb with `NO_GENMIP=1`, 161mb with `SYNC=1`, and 36mb on llvmpipe.

Those figures do not record which Mesa build produced them, and that matters more than anything else here. Run it against configuration A, our current pin, before you build anything.

- If A comes out low, the reproducer separates pre from post Metal 4 and is a valid instrument. Use it on all four configurations. It is fast, safe and needs no game content, so get all four numbers before going near the engine.
- If A comes out at roughly 2626mb as well, the reproducer measures something present on both sides of the regression. It is then not an instrument for H1 or H2, and the engine under `run-capped.sh` is your only discriminator. Say so, keep collecting the reproducer numbers anyway since they are cheap and upstream cares about them, and treat the engine as the test.

Do not read a matching A and B as evidence that the leak has gone. That is the failure mode this step exists to prevent. Whether B leaks is settled by the engine.

Whichever way it lands, annotate the figures in the header comment of `kk_mipmap_leak.c` with the Mesa commit that produced them, since the absence of that is what made this ambiguous.

**Then the engine.** Use `coding-agents/test-scripts/run-capped.sh <seconds> <logfile>`, which polls RSS and SIGKILLs above a ceiling. Do not run the engine any other way. Without the cap a leaking driver takes this 16 GB machine to 54 GB and freezes it hard enough that the out of memory dialog itself stops responding, costing a power cycle. `LAUNCHER=` selects the Mesa prefix, see `run-macos-premtl4.sh` for the environment a launcher sets.

Run configuration B, the control, before C or D. If B does not leak under the engine, stop and report that. It would mean the leak has already been fixed or moved somewhere between our pin and the MR's merge base, and the whole premise needs rechecking before any of the rest is worth measuring. This is the only stop condition in the task, and it is keyed to the engine because the engine is where the leak was characterised.

Record for each configuration: peak footprint, time to the ceiling if it hits it, and whether the run completes. `run-capped.sh` already logs RSS, top's MEM and footprint together, because it is an open question which tracks real pressure on this driver. Keep all three.

If a configuration survives, run the BAR benchmark for a frame rate as well: `~/dev/spring-testdata/benchmark_bar.txt` with `SPRING_FPS_LOG=10`. The baseline to compare against is 3.3 FPS at 3024x1832 on the 650v650 scenario. This is secondary. The leak is the question.

## What to produce

1. A results table covering all four configurations for the reproducer, and as many as survive for the engine.
2. A draft reply to post on issue 15998, written for Aitor and squidbus to read. Put it in a fenced block in your report so it can be copied. Keep it short and factual: what you tested, what the numbers were, and what they imply. If H2 shows anything, say so plainly, because it is a Zink side finding that neither of them has considered and it speaks directly to squidbus's doubt about why a reset does not plateau. Do not thank anyone effusively, do not speculate beyond the measurements, and do not ask them to prioritise anything.
3. An update to `coding-agents/MACOS_PERFORMANCE.md` item 11 recording the outcome.

Do not post the reply. Do not comment on the issue. The human posts it by hand, and the tracker requires a login and passes through Anubis anyway.

## Reporting rules

Report what happened, not what should have happened.

If the MR does not apply, does not build, or crashes, say so with the error and stop. Do not patch around it. A build failure on a draft MR is a useful result and the maintainer will want to know.

If a configuration leaks, say it leaks. If the numbers are ambiguous, say they are ambiguous. Do not describe a partial improvement as a fix. Aitor's position is that this is not a KosmicKrisp bug, so a report that overstates a result will not survive contact with him, and we would rather be right than persuasive.

State plainly which configurations you did not get to, and why.
