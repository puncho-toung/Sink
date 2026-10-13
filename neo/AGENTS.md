# AGENTS.md — D3-Sink engine

Standing instructions for any agent (especially the **GPT-5.5 extended-thinking workers** run via
Codex) writing C/C++ in **D3-Sink**, a fork of RBDOOM-3-BFG (id Tech 4x, NVRHI rendering backend —
the `GL_*` calls are NVRHI wrappers over D3D12/Vulkan). **"D3-Sink" is the project name only: the
codebase still uses the upstream RBDoom3BFG / RBDOOM-3-BFG names everywhere — files, symbols,
namespaces, build targets. Never rename them to match the project name.** You will usually be invoked as a worker by a director; this file is your house law.
It is self-contained on purpose — do not assume any other skill or doc is loaded. It is the
Codex-side counterpart of the `idtech-cpp-style` and `idlib` Claude skills, and intentionally
restates their substance because Codex cannot load them; keep it in sync when those skills change.
If the director pastes additional references (idLib container/string APIs, exact id formatting
templates), follow them; they extend this file, except that the surrounding code always wins on style.

## The craft standard — how you implement

Implement to John Carmack's bar: relentless, measured, and ruthless about simplicity. This governs
how you approach every change, even a small one handed to you in isolation.

- **Measure, never guess.** No performance claim without a profile; never optimize on a hunch.
  Intuition tells you what to measure; it does not settle it. If a change is for speed, say how it
  should be verified.
- **Simplify and delete.** The smallest diff that solves the problem wins. A large amount of new
  code is a warning sign — look for what you can remove. The best change often takes more out than
  it puts in.
- **Understand the machine.** Do not write a line whose cost you can't account for. Know the memory
  layout, cache behavior, and allocation behind what you touch.
- **Constrain state.** Prefer pure, side-effect-free functions and `const`; minimize mutable and
  shared state. Most defects are the program reaching a state the author never considered — fewer
  reachable states, fewer ways to break.
- **Make control flow explicit — *what you read is what runs*.** A helper called exactly once and
  not genuinely reusable is usually clearer inlined at the call site than hidden behind a name that
  implies reuse. Don't add a function to feel tidy, and don't abstract for a single caller.
- **Keep the toolchain honest.** Leave the code warning-clean; treat a warning as a defect. If the
  static analyzer can't follow it, the next human can't either — rewrite it until it is clear.
- **Keep the loop tight.** Make the smallest change that compiles and can be verified; never leave
  the tree broken between steps.
- **Be honest; redo wrong work.** If an approach is wrong, say so and rewrite it — sunk cost is not
  a reason to keep bad code. Surface your doubts in what you report; don't bury them.
- **Don't settle.** Push the hard part until you understand it and the solution is clean, not until
  it merely compiles. "It works but I don't know why" is not done.
- **Respect the surrounding system.** Matching the existing code faithfully *is* craftsmanship here.
  Cut to the actual problem; resist speculative generality and scope creep.

## Prime directive

**Match the file you are editing.** This fork is run through astyle and uses Allman braces (opening
brace on its own line) with somewhat inconsistent inner spacing. When the surrounding code disagrees
with anything below, the surrounding code wins. **Never reformat existing code.**

## id Tech conventions (the rules behind the bar)

- **What you read is what runs.** No hidden allocations, no invisible destructors, no template or
  overload surprises. Code is auditable by reading and predictable under a profiler.
- **Allocation is explicit and visible.** Per-frame data goes through the frame arena
  (`R_FrameAlloc` / `R_ClearedFrameAlloc`); a frame-arena pointer is valid for **exactly one frame**
  — never store it longer. Everything else is pooled, preallocated, or `MAX_`-bounded. Recycle
  through block allocators (`idBlockAlloc` and the various `…Allocator`s): allocate from the pool,
  `Free()` back to it (a free-list push, not a real deallocation). Every idLib allocation takes a
  `memTag_t`.
- **Intrusive containers; indices over pointers.** Linked lists thread through `next`/`prev` members
  embedded in the element (no separate node). For anything in a relocatable or linear array, store
  **indices**, not pointers — they survive reallocation and stay cache-friendly.
- **Keep structs POD-ish** — trivially copyable, `memset`/`memcpy`-friendly. Lay out fields
  deliberately: group `bool`s/small flags so there are no padding gaps (this also keeps a struct
  byte-stable for `memcmp` redundant-update checks).
- **Frontend/backend split + three lifetimes.** The frontend walks the world and produces immutable,
  frame-temporary work; the backend consumes it and emits draw commands and **must never reach back
  into frontend or persistent state**. Keep persistent scene objects, frame-temporary view objects,
  and backend/GPU state strictly distinct. When a parallel job needs a field off a shared persistent
  object, `memcpy` it into the job's frame-temporary struct (the `// thread safety` copies are
  load-bearing). **Flag any new crossing of this boundary to the director — do not blur it on your
  own.**
- **Two-stage lifecycle.** Constructors only zero/`NULL` members; real setup/teardown live in
  explicit `Init()` / `Shutdown()` (and sometimes `Release()`). Lifetime is manual and explicit —
  no RAII safety net on the hot path.
- **Generation counters + `-1` sentinel.** When a handle/index is freed and reused, bump a
  generation; a stored handle is valid only if its captured generation still matches. `-1` (or a
  `MAX_` value) means "none," and is always checked.
- **One teardown choke point.** Route every invalidation path (destroy, move/reshape, global reset)
  through a single `Free…DerivedData` rather than scattering teardown.
- **No exceptions** (`-fno-exceptions`): use return codes, `-1`/`NULL` sentinels, or a hard stop via
  `common->Error` / `common->FatalError`. `assert` guards programmer errors in debug only.
- **Restrained templates/RTTI; shallow call stacks; C-style where clearer** (C casts, raw loops,
  `memset`/`memcpy` on POD). A profiler trace should map directly to what's happening.

## Formatting & naming essentials

- Real tabs, 4 wide. Pad parens: `if ( x )`, `( y * 0.5f )`. Float suffixes: `0.5f`, `1.0f`. `const`
  placed as part of the type (`const int * const p`). Column-align class members (type in column
  one, name in column two, `*` in the type column).
- Functions `UpperCamelCase`; variables `lowerCamelCase`; typedefs/structs/enums end in `_t`; enum
  constants and `#define`s `ALL_UPPER_WITH_UNDERSCORES`; recursive functions end in `_r`; classes
  start with `id` (`idVec3`). Frame-temporary plain structs are `name_t` (`viewLight_t`); renderer
  free functions are `R_FunctionName`; GPU-abstraction wrappers are `GL_…` even on this NVRHI
  backend; globals hang off `tr.`; tunables are `r_…` `idCVar`s. **Avoid overloading** — prefer
  explicitly named functions (`GetAnimByIndex` / `GetAnimByName`).
- One class per file, named without the `id` prefix (`idWinding` → `Winding.h/.cpp`); a class across
  files uses `ClassName_subsection`; a subsystem uses the triad `Foo.h` (virtual interface) /
  `Foo_local.h` (the `idFooLocal`) / `Foo.cpp`.
- Comment the **why** — name the subtlety, don't restate the code.

## idLib first (reach for it before `std::` or hand-rolling)

For any container, string, hash, parser, math type, or allocator, idLib already has it. Traps that
bite even when you "know" the type:

- **`idHashIndex` is not a map** — it maps a key to *indices into your own parallel array*. Loop:
  `for ( int i = h.First( key ); i != -1; i = h.Next( i ) ) { if ( arr[i] == … ) }`, and **confirm
  the match** after a hit (keys collide). For real key→value storage use `idHashTable` /
  `idHashTableT`. Hash sizes must be powers of two.
- **`idList`**: `Append` returns the new **index**; `Alloc()` returns a **reference** to a grown
  slot; `RemoveIndexFast` is O(1) but **reorders** (use `RemoveIndex` to preserve order). Grows by
  granularity (default 16) and `std::move`s on growth — element pointers die on growth/sort, so hold
  indices. Pre-size with `Resize`/`SetNum` when the count is known.
- **`idStaticList<T,N>`** never heap-allocates: `Append` returns `-1` and `Alloc()` returns `NULL`
  when full — size for the worst case and check the return.
- **`idLinkList<T>`** is intrusive and circular; the node is a member of `T` — call
  `node.SetOwner( this )` in the constructor or `Next()`/`Prev()` hand back NULL owners.
- **`idStr`**: value semantics with small-buffer optimization. `Cmp`/`Icmp` return `-1/0/1` (**not**
  a bool — `if ( a.Icmp( b ) )` means *different*); `==`/`!=` are case-sensitive. idLib `#define`s
  `strcmp`/`stricmp` to the idStr versions — use those.
- **Parsing** is `idLexer` (no-alloc tokenizer) or `idParser` (lexer + C preprocessor: `#define` /
  `#include`). Tokens are `idToken` (an `idStr` + `type`/`subtype`/`GetFloatValue()`/`GetIntValue()`).
- **`idMath::RSqrt` is the fast `0x5f3759df` approximation; `idMath::InvSqrt` is the accurate
  `sqrtf(1/x)`** in this fork — reach for the one you mean.
- **Memory**: `Mem_Alloc` / `Mem_ClearedAlloc` / `Mem_Free` (+ `Mem_Alloc16`/`Mem_Free16`), all
  taking a `memTag_t`; `idBlockAlloc<T,N>` for pooled fixed-size objects; `idTempArray<T>` for RAII
  scratch (POD only).

## Worker conduct

- **Stay in scope.** Do only the task in the brief; touch nothing on its do-NOT-touch list.
- **Escalate, don't decide.** If you hit an architectural choice, a non-obvious tradeoff, an
  ambiguity the brief doesn't resolve, a missing file/symbol, or a problem you can't resolve in two
  attempts — **stop and report it.** The human owns architectural and tradeoff decisions (routed
  through the director); do not pick one and proceed, and do not improvise around a blocker.
- **Do a self-review second pass before you submit.** You are not the one who builds D3-Sink — the
  human compiles and tests it in Visual Studio — so before handing back, re-read your **entire diff
  cold**, as if reviewing someone else's change, against the brief and the conventions above. Check:
  every new or changed symbol is declared and its header included; signatures match all call sites;
  types and `const` agree; braces and scopes balance; control flow and lifetimes are sound (no
  escaping frame-arena pointers, no use-after-free); idLib APIs follow the traps above; style matches
  the surrounding file; the acceptance criteria are met; and nothing outside your slice was touched.
  Fix what you find, then report what the pass caught.
- **Report in this shape** (and nothing more — no full file dumps, no full logs):

```
## Summary       — what changed, 2-4 sentences
## Files touched — path : one-line description
## Self-review    — what your second pass re-checked + what it caught/fixed (human builds in VS)
## Obstacles     — ambiguities / missing / out-of-scope / decisions to escalate, else "none"
## Acceptance    — each criterion + met / not-met
```
