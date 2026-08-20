#include "sim_render_metadata.h"

#include <stdio.h>
#include <stdlib.h>

static FILE *g_sim_d1_trace;
static bool g_sim_d1_trace_env_checked;

static uint64_t FrameHash(const uint8_t *rgba, int width, int height,
                          int pitch) {
  uint64_t hash = UINT64_C(1469598103934665603);
  if (!rgba || width <= 0 || height <= 0 || pitch < width * 4) return 0;
  for (int y = 0; y < height; y++) {
    const uint8_t *row = rgba + (size_t)y * pitch;
    for (int x = 0; x < width * 4; x++) {
      hash ^= row[x];
      hash *= UINT64_C(1099511628211);
    }
  }
  return hash;
}

static void TraceInitFromEnvironment(void) {
  if (g_sim_d1_trace_env_checked) return;
  g_sim_d1_trace_env_checked = true;
  const char *path = getenv("AR_SIM3D_D1_TRACE");
  if (!path || !path[0]) return;
  g_sim_d1_trace = fopen(path, "w");
  if (!g_sim_d1_trace) {
    fprintf(stderr, "[sim3d-d1] cannot open %s\n", path);
    return;
  }
  fprintf(stderr, "[sim3d-d1] metadata trace -> %s\n", path);
}

/* Whether anything will read this frame's trace fields. Sim3D_FinishCapture
 * asks before spending a full-frame hash that only the trace, the scene
 * inspector and the D2 dump ever consume. Forces the one-time environment
 * read, so the answer is correct on the very first frame -- TraceFrame runs
 * later in the same frame and finds it already done. */
bool SimRenderMetadata_TraceArmed(void) {
  TraceInitFromEnvironment();
  return g_sim_d1_trace != NULL;
}

void SimRenderMetadata_TraceFrame(uint32_t host_frame,
                                  const SimFrameData *frame,
                                  const uint8_t *rgba, int width, int height,
                                  int pitch) {
  TraceInitFromEnvironment();
  if (!g_sim_d1_trace || !frame || frame->view == kSimView_None) return;

  fprintf(g_sim_d1_trace,
          "{\"host_frame\":%u,\"game_frame\":%u,\"town\":%u,"
          "\"view\":\"%s\",\"picker_flag\":%u,"
          "\"miracle\":[%u,%u,%u,%u,%u],"
          "\"navigation_valid\":%s,\"world_focus\":[%u,%u],"
          "\"world_scroll\":[%u,%u],"
          "\"world_matrix\":[%d,%d,%d,%d],"
          "\"world_next_matrix\":[%d,%d,%d,%d],"
          "\"world_rotation\":%u,\"world_zoom\":[%u,%u],"
          "\"world_location\":%u,\"world_brightness\":%u,"
          "\"world_active_region\":[%s,%u,%u,%u,%u],"
          "\"world_scene_valid\":%s,\"underlay_serial\":%u,"
          "\"world_texture_serial\":%u,"
          "\"world_texture_size\":[%u,%u],"
          "\"world_source_to_screen\":[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g],"
          "\"world_composition_valid\":%s,"
          "\"world_composition_empty\":%s,"
          "\"world_palace\":[%u,%u,%d,%d,%u,%u],"
          "\"world_ui\":[%u,%u,%d,%d,%u,%u],"
          "\"build_serial\":%u,\"master_enabled\":%s,"
          "\"requested\":%u,\"effective\":%u,"
          "\"metadata_valid\":%s,\"integrity_flags\":%u,"
          "\"atlas_valid\":%s,\"atlas_size\":[%u,%u],"
          "\"atlas_used\":[%u,%u],"
          "\"separated_valid\":%s,\"separated_status\":%u,"
          "\"separated_mismatch_pixels\":%u,"
          "\"separated_hash\":\"%016llx\","
          "\"projection_camera\":[%d,%d,%u],"
          "\"landscape_height_pct\":%u,\"height_scale_x100\":%u,"
          "\"shadow_opacity_pct\":%u,"
          "\"shadow_softness_pct\":%u,\"light\":[%u,%u],"
          "\"world_effects\":[%u,%u,%u,%u],"
          "\"picker_topdown\":%s,"
          "\"backdrop_argb\":%u,\"object_half_add\":%s,"
          "\"source_count\":%u,\"zero_oam_sources\":%u,"
          "\"object_count\":%u,\"effect_metadata_valid\":%s,"
          "\"effect_count\":%u,\"effect_visible_count\":%u,"
          "\"effect_overflow_count\":%u,"
          "\"emitted_oam_count\":%u,"
          "\"claimed_oam_count\":%u,\"synthetic_part_count\":%u,"
          "\"synthetic_part_overflow_count\":%u,\"world_oam_first\":%u,"
          "\"world_oam_count\":%u,\"world_record_occupancy\":%u,"
          "\"framebuffer_hash\":\"%016llx\","
          "\"sources\":[",
          (unsigned)host_frame, (unsigned)frame->game_frame,
          (unsigned)frame->town,
          Sim3D_ViewName(frame->view), (unsigned)frame->picker_flag,
          (unsigned)frame->miracle_kind,
          (unsigned)frame->miracle_user_active,
          (unsigned)frame->miracle_posted_active,
          (unsigned)frame->miracle_visual_complete,
          (unsigned)frame->miracle_actor_done,
          frame->world_navigation_state_valid ? "true" : "false",
          (unsigned)frame->world_navigation.focus_x,
          (unsigned)frame->world_navigation.focus_y,
          (unsigned)frame->camera_x, (unsigned)frame->camera_y,
          frame->world_navigation.matrix[0],
          frame->world_navigation.matrix[1],
          frame->world_navigation.matrix[2],
          frame->world_navigation.matrix[3],
          frame->world_navigation.next_matrix[0],
          frame->world_navigation.next_matrix[1],
          frame->world_navigation.next_matrix[2],
          frame->world_navigation.next_matrix[3],
          (unsigned)frame->world_navigation.rotation,
          (unsigned)frame->world_navigation.zoom_current,
          (unsigned)frame->world_navigation.zoom_target,
          (unsigned)frame->world_navigation.active_location,
          (unsigned)frame->world_navigation_brightness,
          frame->world_navigation_scene.active_region_valid
              ? "true" : "false",
          (unsigned)frame->world_navigation_scene.active_region_x,
          (unsigned)frame->world_navigation_scene.active_region_y,
          (unsigned)frame->world_navigation_scene.active_region_width,
          (unsigned)frame->world_navigation_scene.active_region_height,
          frame->world_navigation_scene.valid ? "true" : "false",
          (unsigned)frame->underlay_serial,
          (unsigned)frame->world_navigation_scene.texture_serial,
          (unsigned)frame->world_navigation_scene.texture_width,
          (unsigned)frame->world_navigation_scene.texture_height,
          (double)frame->world_navigation_scene.source_to_screen[0],
          (double)frame->world_navigation_scene.source_to_screen[1],
          (double)frame->world_navigation_scene.source_to_screen[2],
          (double)frame->world_navigation_scene.source_to_screen[3],
          (double)frame->world_navigation_scene.source_to_screen[4],
          (double)frame->world_navigation_scene.source_to_screen[5],
          frame->world_navigation_scene.composition.valid ? "true" : "false",
          frame->world_navigation_scene.composition.empty_animation
              ? "true" : "false",
          (unsigned)frame->world_navigation_scene.composition.palace.oam_first,
          (unsigned)frame->world_navigation_scene.composition.palace.oam_count,
          (int)frame->world_navigation_scene.composition.palace.screen_x,
          (int)frame->world_navigation_scene.composition.palace.screen_y,
          (unsigned)frame->world_navigation_scene.composition.palace.width,
          (unsigned)frame->world_navigation_scene.composition.palace.height,
          (unsigned)frame->world_navigation_scene.composition.ui.oam_first,
          (unsigned)frame->world_navigation_scene.composition.ui.oam_count,
          (int)frame->world_navigation_scene.composition.ui.screen_x,
          (int)frame->world_navigation_scene.composition.ui.screen_y,
          (unsigned)frame->world_navigation_scene.composition.ui.width,
          (unsigned)frame->world_navigation_scene.composition.ui.height,
          (unsigned)frame->build_serial,
          frame->master_enabled ? "true" : "false",
          (unsigned)frame->requested_features,
          (unsigned)frame->effective_features,
          frame->metadata_valid ? "true" : "false",
          (unsigned)frame->integrity_flags,
          frame->atlas_valid ? "true" : "false",
          (unsigned)frame->atlas_width, (unsigned)frame->atlas_height,
          (unsigned)frame->atlas_used_width,
          (unsigned)frame->atlas_used_height,
          frame->separated_valid ? "true" : "false",
          (unsigned)frame->separated_status,
          (unsigned)frame->separated_mismatch_pixels,
          (unsigned long long)frame->separated_hash,
          (int)frame->projection_pitch_mrad,
          (int)frame->projection_yaw_mrad,
          (unsigned)frame->projection_distance_x100,
          (unsigned)frame->landscape_height_pct,
          (unsigned)frame->height_scale_x100,
          (unsigned)frame->shadow_opacity_pct,
          (unsigned)frame->shadow_softness_pct,
          (unsigned)frame->light_azimuth_deg,
          (unsigned)frame->light_elevation_deg,
          (unsigned)frame->world_navigation_lighting,
          (unsigned)frame->world_navigation_clouds,
          (unsigned)frame->world_navigation_backdrop,
          (unsigned)frame->world_navigation_haze,
          AR_SIM3D_PICKER_TOPDOWN ? "true" : "false",
          (unsigned)frame->separated_backdrop_argb,
          frame->object_half_add ? "true" : "false",
          (unsigned)frame->source_count,
          (unsigned)frame->zero_oam_source_count,
          (unsigned)frame->object_count,
          frame->effect_metadata_valid ? "true" : "false",
          (unsigned)frame->effect_count,
          (unsigned)frame->effect_visible_count,
          (unsigned)frame->effect_overflow_count,
          (unsigned)frame->emitted_oam_count,
          (unsigned)frame->claimed_oam_count,
          (unsigned)frame->synthetic_part_count,
          (unsigned)frame->synthetic_part_overflow_count,
          (unsigned)frame->world_oam_first,
          (unsigned)frame->world_oam_count,
          (unsigned)frame->world_record_occupancy,
          (unsigned long long)FrameHash(rgba, width, height, pitch));
  for (unsigned i = 0; i < frame->source_count; i++) {
    const SimSourceRecord *source = &frame->sources[i];
    if (i) fputc(',', g_sim_d1_trace);
    fprintf(g_sim_d1_trace,
            "{\"record\":%u,\"tier\":%u,\"composition\":%u,"
            "\"type\":%u,\"state\":%u,\"word06\":%u,"
            "\"x\":%u,\"y\":%u,"
            "\"oam_first\":%u,\"oam_count\":%u,"
            "\"synthetic_parts\":%u,\"obj_palette_mask\":%u,"
            "\"fragment_first\":%u,\"fragment_count\":%u}",
            (unsigned)source->record_address, (unsigned)source->tier,
            (unsigned)source->composition, (unsigned)source->type,
            (unsigned)source->semantic_state,
            (unsigned)source->record_word06, (unsigned)source->world_x,
            (unsigned)source->world_y, (unsigned)source->oam_first,
            (unsigned)source->oam_count,
            (unsigned)source->synthetic_parts,
            (unsigned)source->obj_palette_mask,
            (unsigned)source->fragment_first,
            (unsigned)source->fragment_count);
  }
  fputs("],\"objects\":[", g_sim_d1_trace);
  for (unsigned i = 0; i < frame->object_count; i++) {
    const SimRenderObject *object = &frame->objects[i];
    if (i) fputc(',', g_sim_d1_trace);
    fprintf(g_sim_d1_trace,
            "{\"record\":%u,\"source_index\":%u,\"tier\":%u,"
            "\"composition\":%u,\"priority\":%u,\"traits\":%u,"
            "\"height_class\":%u,\"height_class_name\":\"%s\","
            "\"virtual_height\":%d,\"classified_height\":%d,"
            "\"casts_shadow\":%s,\"color_math_eligible\":%s,"
            "\"oam_first\":%u,\"oam_count\":%u,"
            "\"part_count\":%u,\"synthetic_part_count\":%u,"
            "\"foot_x\":%d,\"foot_y\":%d,\"atlas_valid\":%s,"
            "\"local_bounds\":[%d,%d,%d,%d],"
            "\"atlas\":[%u,%u,%u,%u]}",
            (unsigned)object->record_address,
            (unsigned)object->source_index, (unsigned)object->tier,
            (unsigned)object->composition, (unsigned)object->priority,
            (unsigned)object->traits,
            (unsigned)object->height_class,
            Sim3D_HeightClassName((SimHeightClass)object->height_class),
            (int)object->virtual_height, (int)object->classified_height,
            Sim3D_ObjectCastsShadow(object) ? "true" : "false",
            object->color_math_eligible ? "true" : "false",
            (unsigned)object->oam_first,
            (unsigned)object->oam_count,
            (unsigned)object->part_count,
            (unsigned)object->synthetic_part_count,
            object->foot_x, object->foot_y,
            object->atlas_valid ? "true" : "false",
            object->local_x0, object->local_y0,
            object->local_x1, object->local_y1,
            (unsigned)object->atlas_x, (unsigned)object->atlas_y,
            (unsigned)object->atlas_w, (unsigned)object->atlas_h);
  }
  fputs("],\"effects\":[", g_sim_d1_trace);
  for (unsigned i = 0; i < frame->effect_count; i++) {
    const SimEffectInstance *effect = &frame->effects[i];
    if (i) fputc(',', g_sim_d1_trace);
    fprintf(g_sim_d1_trace,
            "{\"kind\":%u,\"kind_name\":\"%s\","
            "\"phase\":%u,\"phase_name\":\"%s\","
            "\"color_family\":%u,\"color_name\":\"%s\",\"flags\":%u,"
            "\"generation\":%u,\"pulse_generation\":%u,"
            "\"age_ticks\":%u,\"phase_ticks\":%u,"
            "\"pulse_ticks\":%u,\"ticks_since_visible\":%u,"
            "\"record\":%u,\"source_index\":%u,\"composition\":%u,"
            "\"world\":[%u,%u],\"geometry\":{"
            "\"kind\":%u,\"kind_name\":\"%s\","
            "\"space\":%u,\"space_name\":\"%s\"",
            (unsigned)effect->kind,
            Sim3D_EffectKindName((SimEffectKind)effect->kind),
            (unsigned)effect->phase,
            Sim3D_EffectPhaseName((SimEffectPhase)effect->phase),
            (unsigned)effect->color_family,
            Sim3D_EffectColorName(
                (SimEffectColorFamily)effect->color_family),
            (unsigned)effect->flags,
            (unsigned)effect->generation,
            (unsigned)effect->pulse_generation,
            (unsigned)effect->age_ticks,
            (unsigned)effect->phase_ticks,
            (unsigned)effect->pulse_ticks,
            (unsigned)effect->ticks_since_visible,
            (unsigned)effect->record_address,
            (unsigned)effect->source_index,
            (unsigned)effect->composition,
            (unsigned)effect->world_x, (unsigned)effect->world_y,
            (unsigned)effect->geometry.kind,
            Sim3D_EffectGeometryName(
                (SimEffectGeometryKind)effect->geometry.kind),
            (unsigned)effect->geometry.space,
            Sim3D_EffectSpaceName(
                (SimEffectGeometrySpace)effect->geometry.space));
    switch ((SimEffectGeometryKind)effect->geometry.kind) {
      case kSimEffectGeometry_Point:
        fprintf(g_sim_d1_trace, ",\"point\":[%d,%d,%d]",
                effect->geometry.data.point.x,
                effect->geometry.data.point.y,
                effect->geometry.data.point.height);
        break;
      case kSimEffectGeometry_Segment:
        fprintf(g_sim_d1_trace,
                ",\"segment\":[[%d,%d,%d],[%d,%d,%d]]",
                effect->geometry.data.segment.start.x,
                effect->geometry.data.segment.start.y,
                effect->geometry.data.segment.start.height,
                effect->geometry.data.segment.end.x,
                effect->geometry.data.segment.end.y,
                effect->geometry.data.segment.end.height);
        break;
      case kSimEffectGeometry_Area:
        fprintf(g_sim_d1_trace, ",\"area\":[%d,%d,%d,%d,%d]",
                effect->geometry.data.area.x,
                effect->geometry.data.area.y,
                effect->geometry.data.area.width,
                effect->geometry.data.area.height,
                effect->geometry.data.area.elevation);
        break;
      case kSimEffectGeometry_None:
      case kSimEffectGeometry_Scene:
        break;
    }
    fputs("}", g_sim_d1_trace);
    /* The retained path, newest first, so a trail can be checked against the
     * record's own motion instead of against what the renderer drew. */
    if (effect->trail_count) {
      fputs(",\"trail\":[", g_sim_d1_trace);
      for (unsigned n = 0; n < effect->trail_count; n++)
        fprintf(g_sim_d1_trace, "%s[%u,%u]", n ? "," : "",
                (unsigned)effect->trail[n].world_x,
                (unsigned)effect->trail[n].world_y);
      fputc(']', g_sim_d1_trace);
    }
    fputs("}", g_sim_d1_trace);
  }
  fputs("]}\n", g_sim_d1_trace);
  fflush(g_sim_d1_trace);
}

void SimRenderMetadata_TraceClose(void) {
  if (g_sim_d1_trace) fclose(g_sim_d1_trace);
  g_sim_d1_trace = NULL;
}
