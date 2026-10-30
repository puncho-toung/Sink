# Offline node-map composer, format 1

## Status: frozen (Phase 2B stage 1, P2b.1.9, 2026-10-29)

The composer is frozen at format 1. Worldspaces come from the editor (SinkBroom's one world-coordinate document exports
`maps/<world>.bigmap` and one cell `.map` per cell, Phase 2B stages 3 and 5), and map-graph node maps (dungeons, large interiors)
are hand-built (Phase 2 Q5's default). No Valve 220 input path is added (Phase 2B Q19: the reader would be a medium package for a
tool nothing uses). What stays: `compose.py` reads and writes `Version 2` brushDef3/patchDef maps only; `test_compose.py` (14
tests) keeps passing and guards the format; `fixtures/kit.json` is the reference recipe. What replaces the composer for new
content: the editor's export (stage 5) and the per-cell compile driver `buildworld` (stage 3). A change to this tool needs a reason
written into `plans/map-graph/00-PHASE2B-PLAN.md`.

`compose.py` assembles authored Doom 3 Version 2 `.map` segments into one ordinary
whole map and a deterministic JSON manifest. Run dmap and bake the result offline
using the normal engine tools. There is no runtime stitching, additive loading,
host compilation, or change to the mapgraph wire protocol.

From the root of the engine checkout (Python 3.9 or later; the scripts need their own
directory on `sys.path`, so do not run them with `python3 -I`):

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tools/mapgraph/compose.py \
  tools/mapgraph/fixtures/kit.json --seed 17 --flag extra_start \
  --output /tmp/mapgraph/c0.map
PYTHONDONTWRITEBYTECODE=1 python3 tools/mapgraph/test_compose.py
```

The recipe's `name` is the canonical eventual engine name (`_test/mgc/c0`), maximum
31 ASCII characters, slash-separated alphanumeric/underscore components, without
`maps/` or `.map`. `--output` is the staging filesystem path. Install the generated
map at `base/maps/<name>.map` before compiling. The manifest carries the canonical
name; changing the staging directory does not change either output's bytes.

The map and its manifest are each written to a temporary file beside the target and
renamed over it, so neither is ever left half written. An `--output` whose map or
manifest path resolves (through symlinks) to the recipe or any listed segment is
refused, and nothing is written.

A `.mapgraph` `map` line may name the result as `maps/_test/mgc/c0` or `_test/mgc/c0`:
the engine canonicalises both to `maps/<name>`, lower case, without the extension. The
test graphs in `base/mapgraphs` use the `maps/` form.

## Recipe

The complete example is `fixtures/kit.json`. All fields not specified here are
errors; identifiers match `[A-Za-z][A-Za-z0-9_]{0,47}`. JSON duplicate keys are errors.

* Top level: `version: 1`, `name`, `segments` object, `nodes` array, `links` array;
  optional `flags` and `triggers` arrays declaring every flag and trigger name the
  recipe uses, and optional `world` object.
* World: `world_reset` (`never`, `timed` or `cleared`) and `world_reset_time` (an
  integer number of seconds, 0 through 2^31-1), written into the composed worldspawn,
  where persistent worlds read them. No other worldspawn key can be set.
* Segment: `map` path relative to the recipe directory, `sockets` object. Paths
  resolving outside that directory are rejected, including symlinks. All listed
  segments are parsed and digested, even if no node selects them.
* Socket: `kind` identifier, `center` three coordinates, `normal` one signed unit
  axis, `size` three full widths (zero along normal, positive on the other axes),
  optional `cap` naming a segment. Sizes/centers describe the authored opening.
* Node: unique `id`, integer `translation: [x,y,z]`, ordered `choices` array.
* Choice: `segment`, positive integer `weight` (at most 2^31-1), optional `when`.
  `when` accepts `flags_all`, `flags_none`, `triggers_all`, `triggers_none`, each an
  array of names declared in the recipe's `flags` or `triggers`; an undeclared name
  fails, so a misspelling cannot silently change selection. All supplied predicates
  must pass. No eligible choice fails composition rather than silently removing the node.
* Link: two strings `node.socket`. Both endpoints must exist, be unused and belong
  to different nodes; kind, translated center and size must be identical and
  normals opposite. No tolerance or snapping hides authoring errors.

`--flag` and `--trigger` are repeatable **compile-time inputs** and must be declared
in the recipe. Trigger names mean
previously resolved generation events supplied by the build caller, not runtime
trigger entities or script callbacks. Inputs are deduplicated and sorted in the
manifest. Node and choice ordering is authored and preserved. For each node, the
SHA-256 hash of canonical JSON `[1, seed, node_id, counter]` supplies a 256-bit draw;
rejection sampling and integer prefix sums select among eligible integer weights.
Seed range is 0 through 2^64-1. No Python PRNG, time, absolute path or filesystem
iteration order affects selection. Editing choice order intentionally changes it.

Every unused socket must name an authored cap segment. Caps contain only world
brushes, no entities or sockets. At least one axial six-plane cap brush must cover
the entire socket rectangle, with positive thickness. This verifies cap occupancy;
it does **not** prove arbitrary segment geometry is watertight, detect overlapping
rooms, or inspect material collision flags. Authors must use solid sealing cap
materials and accurately describe openings. The normal dmap leak check remains the
authoritative whole-map closure gate. There is no synthesized hull hiding leaks.

## Geometry and entity contract

Only integer translations are supported. Rotation, scale, pitch/roll, legacy
three-point brushes, Valve220 maps and unknown primitives fail explicitly. Coordinates
are finite and bounded to +/-1048576 on input; this is a numerical input bound,
not a node-area restriction. Each placed segment's brush corners, patch control points
and entity origins must stay within +/-131072 (`MAX_WORLD_COORD`, where dmap drops a
brush) after translation. Brush planes must have unit normals. Supported forms:

* `brushDef3` with plane, 2x3 texture matrix, quoted material and three zero flags.
  World planes change `d -= normal dot translation`. Entity planes remain local;
  moving the entity origin moves rendering and collision together. For both forms,
  texture offsets change by minus the world texture vector dot translation, matching
  `idMapBrushSide::GetTextureVectors`, including its entity-origin offset. The texture
  basis uses the closed form of `ComputeAxisBase` (IEEE arithmetic and CPython's own
  `hypot`, no libm trigonometry), so the output bytes are the same on every platform.
* `patchDef2` and `patchDef3`: on-disk control-point xyz translates, UV stays fixed.
  The engine subtracts entity origin while parsing, so local entity patches stay
  unchanged. Width/height are odd integers 3..31; explicit subdivision counts are
  integers 1..64 and reserved fields must be zero. No primitive epairs are accepted; entity epairs must precede primitives.
* Entity `origin` and absolute `light_origin` translate. Default origin is zero.
  Model asset names and local orientation are preserved. Relative light vectors
  (`light_target/right/up/start/end/center`), radii, colors and orientation values
  remain unchanged. Physics uses the same translated entity origin; arbitrary
  custom physics spawnargs and mover path semantics are rejected.

All listed segments are validated even when unselected.
A segment's worldspawn must have exactly `classname worldspawn`; world keys come only
from the recipe's `world`. The first world contributes all world brushes/patches, followed by merged
segment entities. Epairs are written in source order (keys the composer adds, such as
a missing `origin`, follow), because idDict keeps the order it reads and `target`,
`target1`, `target2` fire in that order. Entity epairs use the explicit `SAFE_KEYS`/`REF_KEYS` allowlist in
`compose.py`; unknown keys, including script hooks, fail. Some otherwise valid game
entities therefore need a reviewed schema extension before they can be composed.
`func_mover*`, `target_call*`, and `target_set*` classes are rejected. Segment `.script`
sidecars are rejected (pool option C5B); no script symbol rewriting is attempted.

Every entity gets `node__localname` (unnamed entities use `entity_<ordinal>`), with
63-character maximum and globally case-insensitive uniqueness checked. `target`,
`targetN`, `killtarget` and `bind` must resolve within the source segment and are
rewritten. A brush entity's self-named `model` is rewritten; asset model paths remain
unchanged. Cross-instance entity references, external named targets, entity-list
strings, special editor groups and unknown reference keys are unsupported. Names
are stable for a fixed node ID and source entity name/order.

## Bounds, evidence and limits

At most 256 segments, 256 placements, 32 sockets/segment, 256 choices/node,
4096 links, 4096 entities/segment, 8192 output entities and 65536 output primitives.
Each recipe/map file is size-checked before reading (8 MiB). Brush side count is
4..128; condition lists are at most 256 names. These are authoring-tool resource
limits, not engine capacity guarantees. Engine/submodel limits still apply.

The manifest records canonical map name, algorithm/version, seed, compile-time
inputs, recipe content digest, every segment's relative name/content digest,
selected instances/translations, caps, final entity names and emitted map digest.
It does not certify external material/model assets or compiled/baked output.

`test_compose.py` runs byte-for-byte map and manifest repeats for 1000 seeds,
checks actual selected variants, negative schema/socket/cap/name/script cases,
declared flag and trigger names, world keys, source epair order, the closed-form
texture basis against the libm form, `MAX_WORLD_COORD` bounds, and the command line's
whole-file writes and input refusal, and compares engine-style local planes, patch
coordinates and texture evaluation before/after translation. Scratch files go to the
system temporary directory. The fixture's room B adds a solid pillar, so weighted
selection changes geometry. Room caps are authored ordinary map brushes. Tests
are Python-only; dmap, leaks, runtime collision and bake validation are separate
engine gates, not implied by these tests.

## Generated node-map kit and engine smoke fixtures

The audited class-specific keys for `info_landmark`, `trigger_transition`,
`trigger_changelevel` and `info_landing` are accepted through `KIT_KEYS`. Each local
landmark identity is namespaced separately from entity names; references must
resolve in that segment. `sealed_by` resolves to and rewrites a local entity name.
Kind names remain global vocabulary. External `link` strings are rejected in v1;
connect the generated map through the normal external mapGraph declaration.
The manifest exposes `landmark_ids` for that declaration. Point and brush landmarks
share the normal entity/primitive transform rules: a brush landmark or carry volume
keeps local planes and moves with its `origin`, and the game reads it at its planes
plus that origin (`idWorldStore::ReadMapData`). A `"dmapOnly"` map key is rejected:
the entityDef alone makes these classes map-only. This does not bypass the engine
kit checker's winding, carry-volume, sealing-door or graph grammar checks.

Three complete recipes `compile_c0.json`, `compile_c1.json`, `compile_c2.json` are
provided. c0 joins two rooms; c1 joins a translated pair and exercises a local
brush model, a patchDef3 brush model, stock cannister asset model, namespaced
trigger-to-light reference, brush landmark, carry volume, changelevel trigger and
landing marker. c2 is a single room with both unused sockets capped. c1's exposed
landmark is `leaf__outside`, kind `door64`, rectangle 64 by 96 at the sealed east
leaf. It can be named in an ordinary mapGraph node declaration; a real destination
must supply a matching kind/winding before transitions can be tested.

| Recipe/map name | Player start | Interior movement smoke path | Exterior landmark |
|---|---|---|---|
| `_test/mgc/c0` | `96 0 8` | `96 0 8` → `384 0 8`, crosses x=256 opening | none |
| `_test/mgc/c1` | `-928 512 136` | `-928 512 136` → `-640 512 136`, crosses x=-768 opening | `leaf__outside`, center `-512 512 176`, normal +X |
| `_test/mgc/c2` | `96 0 8` | within x=0..256, y=-128..128, floor z=0 | none |

The c1 trigger at `-736 512 128` targets `leaf__lamp`; the asset cannister is at
`-576 584 136`, the plinth at `-704 584 128`. The external cap prevents walking
out of the generated map until a mapGraph transition is configured; it is not an
internal room-connection cap. These coordinates are intended smoke-test inputs,
not evidence that the engine has run them. Staging files are copied to the
manifest's exact canonical destination before dmap/bake, as described above.

## Authoring segments

Segments are Doom 3 `Version 2` maps with `brushDef3` brushes. DarkRadiant writes
that format; so does a text editor. The project's TrenchBroom configuration reads and
writes only "Doom3 (Valve)", which is Valve 220 without a `Version` line, and the engine's
own map writer emits `Version 3`; the composer accepts neither in v1, and no tool
converts Valve 220 to `brushDef3`. Author segments, and edit the
`prefabs/landmarks` doorways, in DarkRadiant or by hand; no Valve 220 reader is added
(Status above). Were one ever written, it would translate the three plane points of world and entity brushes alike
(Valve 220 entity planes are absolute; the engine subtracts the origin), shift a world
side's offsets by minus the texture axis over its scale dot translation, with no origin
term (`GetTextureVectors`), keep an entity side's offsets (its local coordinates do not
change), and drop the `_tb_` group and layer keys.
