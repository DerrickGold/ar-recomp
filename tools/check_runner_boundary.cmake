# Keep concrete runtime component layouts inside runtime-next.  This temporary
# exact allowlist makes the migration monotonic: a new dependency fails, and a
# removed dependency also fails until its obsolete exception is deleted here.

if(NOT DEFINED AR_SOURCE_ROOT)
  message(FATAL_ERROR "AR_SOURCE_ROOT is required")
endif()

set(_allowed
  action/action_obj_apron.h
  actraiser/actraiser_action_bg.c
  actraiser/actraiser_rtl.c
  actraiser/actraiser_spc_player.c
  actraiser/actraiser_spc_upload.c
  actraiser/actraiser_widescreen_bg.c
  actraiser/actraiser_widescreen_sprites.c
  dev/dev_tools.c
  dev/host_dev_tools.c
  dev/native_audio_trace_runtime.c
  dev/oracle_trace.c
  dev/scene_inspector.c
  diorama/diorama.c
  diorama/diorama_host.c
  diorama/diorama_planes.h
  frame_slot.c
  hd_replacement_host.c
  hd_replacements.c
  host/host_display.c
  main.c
  native_audio_extension.c
  native_audio_mixer.c
  sim/sim3d.c
  sim/sim_phase0_trace.c
  sim/sim_render_atlas.c
  sim/sim_render_metadata.h
  sim/sim_world_map_build.c
)
list(SORT _allowed)

file(GLOB_RECURSE _sources
  LIST_DIRECTORIES false
  "${AR_SOURCE_ROOT}/*.c"
  "${AR_SOURCE_ROOT}/*.h")

set(_actual)
foreach(_source IN LISTS _sources)
  file(READ "${_source}" _contents)
  if(_contents MATCHES
      [=[#[ \t]*include[ \t]*["<]snes/(snes|ppu|apu|dsp|spc|dma|cart)\.h[">]]=])
    file(RELATIVE_PATH _relative "${AR_SOURCE_ROOT}" "${_source}")
    list(APPEND _actual "${_relative}")
  endif()
endforeach()
list(SORT _actual)

if(NOT _actual STREQUAL _allowed)
  set(_new ${_actual})
  list(REMOVE_ITEM _new ${_allowed})
  set(_stale ${_allowed})
  list(REMOVE_ITEM _stale ${_actual})
  message(FATAL_ERROR
    "Concrete runner boundary changed.\n"
    "New violations: ${_new}\n"
    "Stale exceptions: ${_stale}\n"
    "Update code first; update this exact allowlist in the same change.")
endif()

list(LENGTH _actual _count)
message(STATUS
  "Concrete runner boundary: ${_count} temporary application exceptions")
