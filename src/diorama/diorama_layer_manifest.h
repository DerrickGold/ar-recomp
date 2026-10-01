#ifndef AR_DIORAMA_LAYER_MANIFEST_H
#define AR_DIORAMA_LAYER_MANIFEST_H

#include "diorama_layer_order.h"

/* Owns the per-room override table and diorama-layers.ini beside settings.ini.
 * Render/capture borrow a read-only view; the editor borrows the mutable table.
 * Both are always non-NULL. Access is sequenced on the host/game thread. */
const DioramaLayerOrderTable *DioramaLayerManifest_Table(void);
DioramaLayerOrderTable *DioramaLayerManifest_Edit(void);
/* Cached region-filtered view, invalidated by Load/Edit/Save. Mutable table
 * borrows must be obtained again before each edit (or followed by Save). */
const DioramaRoomOverride *DioramaLayerManifest_TerrainRoom(
    const DioramaRoomOverride *room, unsigned profile);

/* Load at boot. Missing/unreadable files leave the existing table unchanged
 * (initially empty). Malformed entries are reported and skipped individually. */
bool DioramaLayerManifest_Load(void);
/* Preserve comments/foreign sections; failure leaves the original file intact. */
bool DioramaLayerManifest_Save(void);

#endif /* AR_DIORAMA_LAYER_MANIFEST_H */
