# Voxel surface audit

This audit separates **inventory**, **candidate detection**, **rendered evidence**
and **reviewer acceptance**. A room being loaded, a texture being assigned, or
zero detector findings never establishes that all floors and edges are correct.
Unknown/unclassified, edited and specialized routes remain visible and pending.

## Compile and export

Use the current checkout and the production compiler. The diagnostic target
includes the production material compiler directly; it does not copy its rules.
It is CPU-only and adds no runtime renderer behavior. Build with one worker:

```sh
nice -n 15 python3 -c 'import os,subprocess,sys; assert os.getpriority(os.PRIO_PROCESS,0)==15; sys.exit(subprocess.call(sys.argv[1:]))' \
  cmake --build build-tests --target actraiser_voxel_depth_audit --parallel 1
```

The priority assertion is deliberate: on macOS a denied `nice` can print an
error and still run the command at ordinary priority. Use an approved execution
context that permits NI15 instead of silently accepting that fallback. Coordinate
at most two heavy workers across export, compilation, analysis and capture.

The binary accepts one or more private `.arscene` snapshots and writes one JSON
object per input line. `--surfaces` adds the complete compiled/source inventory;
without it the existing depth-audit format is unchanged. Export one scene per
file for the surface analyzer (which reads JSON objects, not multi-scene JSONL):

```sh
nice -n 15 python3 -c 'import os,subprocess,sys; assert os.getpriority(os.PRIO_PROCESS,0)==15; sys.exit(subprocess.call(sys.argv[1:]))' \
  build-tests/actraiser_voxel_depth_audit --surfaces --ini diorama-layers.ini \
  /absolute/path/private-scenes/0/0201.arscene > /absolute/path/shipped/0/0201.json
gzip -n /absolute/path/shipped/0/0201.json
```

Omit `--ini` for raw-native geometry. Repeat for every scene in terrain profiles
0, 1 and 2, and keep raw and shipped outputs in separate trees. The current
fixture manifest has 50 scene variants per region: 49 room IDs plus a distinct
Death Heim completion variant. Validate the input manifest rather than accepting
a directory traversal count as proof of expected coverage. ROM artwork and
scene/texture dumps stay in private ignored run directories, never in tests or
public source. Public detector fixtures use synthetic artwork.

Surface records retain every generic edge and source cell, including missing
policies, plus authored artwork/contours, profile patches, water, gates, backing,
curves and Northwall trunk masks. Compiled source is the immutable snapshot
character data, equivalent to character phase zero. The preexisting `frame`
metadata describes the stored golden-frame request (often frame37/phase2); it is
**not** applied to these compiler inputs. Match browser captures at native frame0.
Other animation phases need separate inventory/render evidence.

`surface_audit.edge_fields` names compact edge columns. `physical_near/far`
unions valid native, high-deck-behind and rear-trim spans; native span alone can
be only the foreground part. `source_cells` coordinates are16px cells; edge
coordinates are native pixels. Edge owned-art IDs are one-based; authored and
profile art IDs are zero-based. Source art is RGB555 plus opaque bit15, with
material cutouts and pixel edits recorded separately. Authored shape, bend and
transform metadata must be retained when comparing treatments.

## Detect and review

The analyzer accepts original C exports, retained legacy diagnostic exports, or
normalized surface JSON, compressed or plain. Put outputs outside its input tree:

```sh
nice -n 15 python3 -c 'import os,subprocess,sys; assert os.getpriority(os.PRIO_PROCESS,0)==15; sys.exit(subprocess.call(sys.argv[1:]))' \
  python3 tools/voxel_surface_audit.py --input /absolute/path/shipped \
  --output /absolute/path/audit/shipped-report.json \
  --details-dir /absolute/path/audit/shipped-details
```

`--details-dir` writes per-scene gzip inventories instead of retaining every
surface in the summary. The source-side collision-boundary inventory is
independent of emitted faces; neither count may substitute for the other.
Detailed native ink becoming contour stripes, lost detail in an assigned donor,
and adjacent assigned/unassigned mappings are **review candidates**, not automatic
repairs. Smooth fog/water and deliberate native ornament transitions are valid.

The CPU inventory does not establish final UV sampling, visibility, sprite
priority or gameplay. BG1 source art is exported; BG2 water/cloud source art
needs separate native/editor evidence. Ground-cover receivers, specialized
sand/column paths, runtime objects and completion geometry retain explicit
route obligations. Existing pick-observer callbacks are also incomplete for
some specialized paths. Empty or unsupported paths must not become passes.
Each reviewed family/context needs an explicit disposition and current evidence;
complete coverage requires accounting for unresolved routes and source witnesses.

Regression tests are registered in CMake as `actraiser_voxel_surface_audit`,
`actraiser_voxel_depth_audit_adversarial` and `actraiser_voxel_depth_audit_native`.
The latter uses `AR_VOXEL_DEPTH_FIXTURES=/absolute/path/private-scenes` and
`AR_VOXEL_DEPTH_BINARY=/absolute/path/actraiser_voxel_depth_audit` for optional
real-scene integration coverage; its retained scalar witness requires no ROM.

## Capture current evidence

These tools capture current shared-renderer evidence for an independently
inventoried surface audit. A successful capture is **not** visual acceptance.
No native game, personal browser session, hardware GPU or remote device is used.

Build the action editor after enabling its bounded observation exports, then run
one capture worker at low priority:

```sh
nice -n 15 python3 tools/voxel_audit/run-captures.py \
  --plan /absolute/path/visual-plan.json \
  --editor /absolute/path/action-editor.html \
  --output /absolute/path/visual \
  --profiles 0
```

The plan has a `rooms` array. Each room supplies `id`, `group`, `map`, `section`
(empty or `completion`), and `windows`. A window supplies a stable numeric `id`
and `bounds: {x,y,width,height}` in native world pixels. Optional fields are
`profiles` (terrain indices), `pitch` in radians, `distance`, `frame`, `label`
and `invariant`. The default orbit uses both yaw signs with positive pitch.
Undersides need explicit negative-pitch witnesses; backs need an appropriate
inspection view and cannot inherit acceptance from front orbits.

Each window saves:

- Raw-native and effective/authored BG1 crops, with native origin offsets.
- Full-resolution left/right software-rendered views and contact sheets.
- Complete bounded copy-out of the existing face-observer callbacks, compressed
  as JSON, with donor coordinates, projected/world positions, depths and UVs.
- Editor SHA-256, renderer identity, camera, requested-frustum coverage,
  JavaScript/GL checks, and project-immutability checks.

The observer is not a replacement for the static inventory: specialized paths
without picker callbacks still need explicit source/geometry and visual review.
Observer incompleteness is recorded as a coverage gap; a truncated registry never receives coverage credit. Render failures still fail capture. `--keep-going` can capture unaffected rooms while retaining failed receipts and a nonzero final status.
The capture report separately lists partial requested-view coverage; reconcile
it with expected geometry and add focused views for anything not visible.

`run-captures.py` resumes matching completed editor/room/profile/window views.
Separate workers use isolated temporary Chrome profiles. Coordinate the global
worker limit; do not launch a worker per room. The current harness uses the
installed macOS Google Chrome binary and verifies SwiftShader before navigating.

Regional reuse requires current effective source and compiled treatment equality,
including donor art and edited/specialized geometry. Merely matching policy
flags or native tile IDs is insufficient. Original game art, smooth water/fog,
spike openings and intentional decorative transitions are valid controls, not
automatic defects. Keep a reviewer disposition and evidence reference for every
surface family; never turn unreviewed inventory into a passing audit result.

## Reconcile projected coverage

`coverage.py` reconciles one production inventory against current capture trees:

```sh
python3 tools/voxel_audit/coverage.py \
  --inventory /absolute/path/shipped/0/0203.json.gz \
  --captures /absolute/path/visual --captures /absolute/path/supplemental \
  --editor-sha256 CURRENT_EDITOR_SHA256 \
  --output /absolute/path/audit/0203-coverage.json
```

Use the same NI15 launcher and shared worker limit for multi-room processing.
Repeat `--editor-sha256` only for builds with separately recorded input and
renderer-change evidence. Each view keeps its actual build hash; allowing two
builds does not declare their binaries or all rendered behavior equivalent.
The report separates generic source-edge fragments from two independent
collision-floor witnesses: original map materials (`native_material_id`) and
effective source/stamp materials (`material_id`). An artwork stamp cannot silently
rewrite the original-map denominator. It distinguishes fully framed source submissions,
partly clipped submissions, offscreen submissions and unseen intervals using
actual projected polygons and perspective-correct viewport clipping. A source
window containing a floor does not establish that the floor reached the image.
Backing/actor/front quads and BG2 surfaces cannot count as BG1 floor evidence. Incomplete observers
and nonzero animation phases do not match the phase-zero static inventory.

These are submission/framing measures, not proof that pixels survive occlusion
or that every depth slice was seen. Curved, folded and authored geometry retains
physical-mapping review obligations. Specialized routes remain separate; absent
regional captures never inherit evidence automatically. Suggested supplemental
bounds are requests for review, not evidence of missing geometry or automatic
camera acceptance. Synthetic framing checks live in
`tests/test_voxel_surface_coverage.py` (`actraiser_voxel_surface_coverage`).

An editor hash alone does not prove that the capture used the same raw/shipped
override state as the inventory. Reconcile those inputs using build/state
evidence before attributing captured coverage; the coverage report retains that
obligation explicitly. Outputs must stay outside capture input directories.

## Verify input provenance and reuse

`verify-capture-state.mjs` re-encodes all embedded editor scenes using the exact
encoder source embedded in that HTML, then compares the bytes against the shipped
inventory's source receipts. It also compares the embedded scenery INI byte hash:

```sh
node tools/voxel_audit/verify-capture-state.mjs \
  /absolute/path/action-editor.html /absolute/path/export-receipt.json \
  /absolute/path/capture-state-provenance.json
```

Keep a separate proof for each editor build. Input equality does not imply that
different renderer binaries are identical. Capture reports assert that project
files remain unchanged and record their hashes; the provenance proof and those
state assertions serve different purposes. Native/effective BG2 source artwork
is included in the source snapshot comparison, while effects, camera coverage
and final visibility still require their own evidence.

Resume requires the full selected room plan hash, editor hash, profile, effective
camera pitch and every referenced image/observer artifact. Adding a BG2 atlas
request or changing room identity therefore invalidates the cached capture.
Missing artifacts also invalidate it. Retry diagnostics are preserved under the
room's `previous-attempts` directory. A run that only reuses existing captures
still writes a fresh invocation receipt; it never implies a new render occurred.

`plan-bottom-addendum.py` can compare complete compiled underside treatments
against an existing plan without changing its case IDs. It keeps fold, taper,
placement and actor-priority flags in the family identity, canonicalizes donor
art content and local phase, and adds only missing treatment requests. The result
is a planned denominator, not evidence that any surface was visible. Geometry
joins and source-budget cutoffs remain explicit framing/review obligations.
