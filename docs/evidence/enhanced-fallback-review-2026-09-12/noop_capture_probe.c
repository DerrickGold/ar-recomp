#define main ExistingPipelineTestMain
#include "../../../tests/ppu_render_pipeline_test.c"
#undef main

/* Review-only probe. Link with the ordinary ppu_render_pipeline test objects,
 * excluding its original tests/ppu_render_pipeline_test.c.o entry. */
int main(void) {
  Ppu *ppu = ppu_init();
  if (!ppu) return 2;
  static uint8_t pixels[kW * 4], baseline[kW * 4];
  setup_virtual_bg(ppu, 0, pixels, sizeof(pixels));
  ppu->bgmode = 9;
  ppu->screenEnabled[0] = 0x17;
  ppu->screenEnabled[1] = 0;
  ppu->cgwsel = 0;
  ppu->cgadsub = 0;
  ppu->fixedColor = 0;
  render_first_line(ppu);
  memcpy(baseline, pixels, sizeof(pixels));
  Sim3DCaptureRequest request = {
    .town = true, .master_enabled = true, .renderer_ready = true,
    .requested_features = kSimFeature_SeparatedComposite | kSimFeature_GroundProjection,
    .width = kW, .height = 1,
  };
  SrRunnerHandle *runner = TestRunnerForPpu(ppu);
  Sim3D_BeginFrame();
  const bool baseline_ready = Sim3D_PrepareCapture(runner, &request);
  Sim3D_BeginFrame();
  PpuClearOverlayBindings(ppu);
  PpuClearOverlayCaptures(ppu);
  ppu->fixedColor = bgr555(1, 2, 3);
  render_first_line(ppu);
  const bool same = !memcmp(baseline, pixels, sizeof(pixels));
  const bool leftover_ready = Sim3D_PrepareCapture(runner, &request);
  SimFrameData frame = {0};
  Sim3DTuning tuning = {0};
  Sim3D_AnnotateFrame(&frame, &tuning);
  const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  SrPpuStateSnapshot state = {.struct_size = SR_PPU_STATE_SNAPSHOT_V2_SIZE};
  if (api->query_ppu_state(runner, &state) != SR_RESULT_OK) return 3;
  const bool palace_leftover = SimWorldNavigationPalace_PpuSupported(&state);
  state.fixed_color = 0;
  const bool palace_baseline = SimWorldNavigationPalace_PpuSupported(&state);
  printf("SIM: zero-fixed=%d leftover-fixed=%d status=%s identical-native-row=%d\n",
      baseline_ready, leftover_ready, Sim3D_CaptureStatusName(frame.separated_status), same);
  printf("Palace PPU gate: zero-fixed=%d leftover-fixed=%d (CGADSUB=0, CGWSEL=0)\n",
      palace_baseline, palace_leftover);
  Sim3D_BeginFrame();
  ppu_free(ppu);
  return baseline_ready && !leftover_ready && same && palace_baseline && !palace_leftover ? 0 : 1;
}
