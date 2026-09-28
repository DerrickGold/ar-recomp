# Alternate straw-hut crest — 2026-09-27

The alternate early-house artwork has a broad raised straw crest with higher
outer tips and five dark vertical bundle ends. The previous interpretation
flattened it into a round cap and lost this defining silhouette.

The model now has a lower thatched shoulder and a thick, flared crest embedded
in its roof. The crest is covered on both sides and across the top; its warm
bands sit on the straw surface. This treats the shared native family as straw
huts. The legacy `Yurt` enum identifier is retained for compatibility.

Both construction stages now support the crest from the hut's binding ring;
the later stage adds its rear straw covering. Finished huts have no exposed
wooden frame. The feature remains at Low detail, and the ground footprint is
unchanged. The front-facing hut remains pixel-identical in the diagnostic view.

- [Native / previous / updated, angled and construction views](../runs/sim-straw-crown-2026-09-27/comparison.png)
- [Updated regional art index](../runs/sim-straw-crown-2026-09-27/art-index/index.html)

These are production-renderer diagnostic captures with independently enlarged
panels, not live gameplay screenshots. Images remain in ignored `runs/`.

Validation: full application build and the model/region CTests pass. New
regressions require a broad upper silhouette, all five bands, a closed crest
and no exposed wooden framing at every detail level. The alternate's authored
face counts are 62/69/91/106 for Low/Balanced/High/Ultra; its construction stages
use at most 62. The finished/construction sweeps recorded no overflow in
18,816 configurations. Normal and oblique reversed-order renders changed only
thin surface boundaries, with no solid 3×3 changed interiors in the hut or its
two construction stages.
