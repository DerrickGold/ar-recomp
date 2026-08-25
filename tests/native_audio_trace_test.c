#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "dev/native_audio_trace.h"

static uint64_t Post(NativeAudioRequestKind kind, uint8_t id,
                     uint64_t cycle) {
  return NativeAudioTraceModel_PostRequest(
      kind, id, 1, "test", 0x123456, 7, 0x10, 0x20, cycle);
}

static void DeliverToRead(NativeAudioRequestKind kind, uint8_t id,
                          uint64_t cycle) {
  uint8_t port = kind == kNativeAudioRequest_Event ? 2 : 3;
  NativeAudioTraceModel_CpuPortWrite(port, id, "test", 7, cycle);
  NativeAudioTraceModel_PortApply(port, id, cycle + 1);
  NativeAudioTraceModel_SpcPortRead(port, id, 0, cycle + 2);
}

static const NativeAudioRequestRecord *Request(uint64_t serial) {
  const NativeAudioRequestRecord *request =
      NativeAudioTraceModel_FindRequest(serial);
  assert(request != NULL);
  return request;
}

static void TestMailboxOverwrite(void) {
  NativeAudioTraceModel_Reset();
  uint64_t first = Post(kNativeAudioRequest_Sfx, 0x02, 1);
  uint64_t second = Post(kNativeAudioRequest_Sfx, 0x10, 2);
  assert(Request(first)->outcome == kNativeAudioOutcome_OverwrittenMailbox);
  assert(Request(first)->replaced_by_serial == second);
  assert(Request(second)->outcome == kNativeAudioOutcome_Pending);
}

static void TestMailboxDuplicateCoalescing(void) {
  NativeAudioTraceModel_Reset();
  uint64_t first = Post(kNativeAudioRequest_Event, 0x07, 1);
  uint64_t second = Post(kNativeAudioRequest_Event, 0x07, 2);
  assert(Request(first)->outcome ==
         kNativeAudioOutcome_CoalescedMailboxDuplicate);
  assert(Request(first)->replaced_by_serial == second);
}

static void TestOrdinaryCompletion(void) {
  NativeAudioTraceModel_Reset();
  uint64_t serial = Post(kNativeAudioRequest_Sfx, 0x10, 1);
  DeliverToRead(kNativeAudioRequest_Sfx, 0x10, 2);
  NativeAudioTraceModel_SpcOpcode(0x0E14, 0x10, 0x12, 0, 0, 5);
  NativeAudioTraceModel_SpcOpcode(0x0E51, 0, 0x12, 0, 0, 20);
  const NativeAudioRequestRecord *request = Request(serial);
  assert(request->outcome == kNativeAudioOutcome_Completed);
  assert(request->lanes_started == 0x02);
  assert(request->active_lanes == 0);
}

static void TestAcceptedReadSurvivesLaterPortClear(void) {
  NativeAudioTraceModel_Reset();
  uint64_t serial = Post(kNativeAudioRequest_Sfx, 0x10, 1);
  DeliverToRead(kNativeAudioRequest_Sfx, 0x10, 2);
  NativeAudioTraceModel_CpuPortWrite(3, 0, "test", 7, 5);
  NativeAudioTraceModel_PortApply(3, 0, 6);
  NativeAudioTraceModel_SpcOpcode(0x0E14, 0x10, 0x12, 0, 0, 7);
  NativeAudioTraceModel_SpcOpcode(0x0E51, 0, 0x12, 0, 0, 20);
  assert(Request(serial)->outcome == kNativeAudioOutcome_Completed);
}

static void TestPortOverwrite(void) {
  NativeAudioTraceModel_Reset();
  uint64_t serial = Post(kNativeAudioRequest_Sfx, 0x08, 1);
  NativeAudioTraceModel_CpuPortWrite(3, 0x08, "test", 7, 2);
  NativeAudioTraceModel_PortApply(3, 0x08, 3);
  NativeAudioTraceModel_CpuPortWrite(3, 0, "test", 7, 4);
  NativeAudioTraceModel_PortApply(3, 0, 5);
  assert(Request(serial)->outcome == kNativeAudioOutcome_OverwrittenPort);
}

static void TestPortDuplicateCoalescing(void) {
  NativeAudioTraceModel_Reset();
  uint64_t first = Post(kNativeAudioRequest_Sfx, 0x08, 1);
  NativeAudioTraceModel_CpuPortWrite(3, 0x08, "test", 7, 2);
  NativeAudioTraceModel_PortApply(3, 0x08, 3);

  uint64_t second = Post(kNativeAudioRequest_Sfx, 0x08, 4);
  NativeAudioTraceModel_CpuPortWrite(3, 0x08, "test", 7, 5);
  NativeAudioTraceModel_PortApply(3, 0x08, 6);
  assert(Request(first)->outcome ==
         kNativeAudioOutcome_CoalescedPortDuplicate);
  assert(Request(first)->replaced_by_serial == second);
}

static void TestOrdinaryBusyRejection(void) {
  NativeAudioTraceModel_Reset();
  uint64_t serial = Post(kNativeAudioRequest_Sfx, 0x1F, 1);
  NativeAudioTraceModel_CpuPortWrite(3, 0x1F, "test", 7, 2);
  NativeAudioTraceModel_PortApply(3, 0x1F, 3);
  NativeAudioTraceModel_SpcOpcode(0x0DF6, 0, 0, 1, 0x1F, 4);
  NativeAudioTraceModel_CpuPortWrite(3, 0, "test", 7, 5);
  NativeAudioTraceModel_PortApply(3, 0, 6);
  assert(Request(serial)->outcome == kNativeAudioOutcome_RejectedDualBusy);
}

static void TestPositiveEventBusyRejection(void) {
  NativeAudioTraceModel_Reset();
  uint64_t serial = Post(kNativeAudioRequest_Event, 0x18, 1);
  NativeAudioTraceModel_CpuPortWrite(2, 0x18, "test", 7, 2);
  NativeAudioTraceModel_PortApply(2, 0x18, 3);
  NativeAudioTraceModel_SpcPortRead(2, 0x18, 1, 4);
  NativeAudioTraceModel_CpuPortWrite(2, 0, "test", 7, 5);
  NativeAudioTraceModel_PortApply(2, 0, 6);
  assert(Request(serial)->outcome == kNativeAudioOutcome_RejectedDualBusy);
}

static void TestLaneReplacement(void) {
  NativeAudioTraceModel_Reset();
  uint64_t first = Post(kNativeAudioRequest_Sfx, 0x08, 1);
  DeliverToRead(kNativeAudioRequest_Sfx, 0x08, 2);
  NativeAudioTraceModel_SpcOpcode(0x0E14, 0x08, 0x12, 0, 0, 5);

  uint64_t second = Post(kNativeAudioRequest_Sfx, 0x10, 6);
  DeliverToRead(kNativeAudioRequest_Sfx, 0x10, 7);
  NativeAudioTraceModel_SpcOpcode(0x0E14, 0x10, 0x12, 0, 0, 10);
  NativeAudioTraceModel_SpcOpcode(0x0E51, 0, 0x12, 0, 0, 20);

  assert(Request(first)->outcome == kNativeAudioOutcome_ReplacedLane);
  assert(Request(first)->replaced_by_serial == second);
  assert(Request(second)->outcome == kNativeAudioOutcome_Completed);
}

static void TestNewRequestForSameIdIsLaneRetrigger(void) {
  NativeAudioTraceModel_Reset();
  uint64_t first = Post(kNativeAudioRequest_Sfx, 0x1B, 1);
  DeliverToRead(kNativeAudioRequest_Sfx, 0x1B, 2);
  NativeAudioTraceModel_SpcOpcode(0x0E14, 0x1B, 0x12, 0, 0, 5);

  uint64_t second = Post(kNativeAudioRequest_Sfx, 0x1B, 6);
  DeliverToRead(kNativeAudioRequest_Sfx, 0x1B, 7);
  NativeAudioTraceModel_SpcOpcode(0x0E14, 0x1B, 0x12, 0, 0, 10);
  NativeAudioTraceModel_SpcOpcode(0x0E51, 0, 0x12, 0, 0, 20);

  assert(Request(first)->outcome == kNativeAudioOutcome_RetriggeredLane);
  assert(Request(first)->flags & kNativeAudioFlag_LaneRetriggeredByRequest);
  assert(!(Request(first)->flags & kNativeAudioFlag_LaneReplaced));
  assert(Request(first)->replaced_by_serial == second);
  assert(Request(second)->outcome == kNativeAudioOutcome_Completed);
}

static void TestStablePortRetriggerIsNotLaneReplacement(void) {
  NativeAudioTraceModel_Reset();
  uint64_t serial = Post(kNativeAudioRequest_Event, 0x07, 1);
  DeliverToRead(kNativeAudioRequest_Event, 0x07, 2);
  NativeAudioTraceModel_SpcOpcode(0x0E14, 0x07, 0x10, 0, 0, 5);
  /* No second CPU/SPC read serial: this is the same stable input value. */
  NativeAudioTraceModel_SpcOpcode(0x0E14, 0x07, 0x10, 0, 0, 6);
  assert(Request(serial)->outcome == kNativeAudioOutcome_Pending);
  assert(Request(serial)->flags & kNativeAudioFlag_NativeLaneRetriggered);
  assert(Request(serial)->native_lane_retriggers == 1);
  assert(Request(serial)->native_retrigger_first_cycle == 6);
  assert(Request(serial)->native_retrigger_last_cycle == 6);
  NativeAudioTraceStats stats;
  NativeAudioTraceModel_GetStats(&stats);
  assert(stats.native_lane_retriggers == 1);
  NativeAudioTraceModel_SpcOpcode(0x0E51, 0, 0x10, 0, 0, 20);
  assert(Request(serial)->outcome == kNativeAudioOutcome_Completed);
}

static void TestHighBitDualLaneCompletion(void) {
  NativeAudioTraceModel_Reset();
  uint64_t serial = Post(kNativeAudioRequest_Event, 0xA0, 1);
  DeliverToRead(kNativeAudioRequest_Event, 0xA0, 2);
  NativeAudioTraceModel_SpcOpcode(0x0E14, 0xA0, 0x10, 1, 0, 5);
  NativeAudioTraceModel_SpcOpcode(0x0E14, 0xA0, 0x12, 1, 0, 6);
  NativeAudioTraceModel_SpcOpcode(0x0E51, 0, 0x10, 0, 0, 20);
  NativeAudioTraceModel_SpcOpcode(0x0E51, 0, 0x12, 0, 0, 21);
  const NativeAudioRequestRecord *request = Request(serial);
  assert(request->effective_id == 0x20);
  assert(request->lanes_started == 0x03);
  assert(request->outcome == kNativeAudioOutcome_Completed);
}

static void TestHighBitEventBypassesBusyAndReplacesPair(void) {
  NativeAudioTraceModel_Reset();
  uint64_t first = Post(kNativeAudioRequest_Event, 0x9C, 1);
  DeliverToRead(kNativeAudioRequest_Event, 0x9C, 2);
  NativeAudioTraceModel_SpcOpcode(0x0E14, 0x9C, 0x10, 1, 0, 5);
  NativeAudioTraceModel_SpcOpcode(0x0E14, 0x9C, 0x12, 1, 0, 6);

  uint64_t second = Post(kNativeAudioRequest_Event, 0xA0, 7);
  NativeAudioTraceModel_CpuPortWrite(2, 0xA0, "test", 7, 8);
  NativeAudioTraceModel_PortApply(2, 0xA0, 9);
  NativeAudioTraceModel_SpcPortRead(2, 0xA0, 1, 10);
  NativeAudioTraceModel_SpcOpcode(0x0E14, 0xA0, 0x10, 1, 0, 11);
  NativeAudioTraceModel_SpcOpcode(0x0E14, 0xA0, 0x12, 1, 0, 12);
  NativeAudioTraceModel_SpcOpcode(0x0E51, 0, 0x10, 0, 0, 20);
  NativeAudioTraceModel_SpcOpcode(0x0E51, 0, 0x12, 0, 0, 21);

  assert(Request(first)->outcome == kNativeAudioOutcome_ReplacedLane);
  assert(Request(first)->replaced_by_serial == second);
  assert(Request(second)->outcome == kNativeAudioOutcome_Completed);
}

static void TestSuppressionAndSongEventsAreNotDrops(void) {
  NativeAudioTraceModel_Reset();
  uint64_t suppressed = NativeAudioTraceModel_PostRequest(
      kNativeAudioRequest_Event, 0x07, 0, "glyph", 0x01902D,
      12, 0, 0, 1);
  assert(Request(suppressed)->outcome ==
         kNativeAudioOutcome_SuppressedSetting);

  NativeAudioTraceModel_CpuPortWrite(0, 0xF0, "transition", 12, 2);
  NativeAudioTraceModel_CpuPortWrite(0, 0xFF, "transition", 12, 3);
  NativeAudioTraceModel_SpcUpload(0x1CAFEB, "transition", 12, 4);
  NativeAudioTraceModel_CpuPortWrite(0, 0x01, "transition", 12, 5);

  NativeAudioSongEvent events[4];
  assert(NativeAudioTraceModel_CopySongEvents(events, 4) == 4);
  assert(events[0].kind == kNativeAudioSong_Halt);
  assert(events[1].kind == kNativeAudioSong_Uploader);
  assert(events[2].kind == kNativeAudioSong_ImageUploaded);
  assert(events[2].image_src == 0x1CAFEB);
  assert(events[3].kind == kNativeAudioSong_Play);
}

static void TestActiveEffectCanceledBySongUpload(void) {
  NativeAudioTraceModel_Reset();
  uint64_t serial = Post(kNativeAudioRequest_Sfx, 0x10, 1);
  DeliverToRead(kNativeAudioRequest_Sfx, 0x10, 2);
  NativeAudioTraceModel_SpcOpcode(0x0E14, 0x10, 0x12, 0, 0, 5);
  NativeAudioTraceModel_SpcUpload(0x1CAFEB, "transition", 12, 6);
  assert(Request(serial)->outcome ==
         kNativeAudioOutcome_CanceledSongTransition);
  assert(Request(serial)->active_lanes == 0);

  NativeAudioTraceModel_Reset();
  serial = Post(kNativeAudioRequest_Event, 0x18, 10);
  NativeAudioTraceModel_CpuPortWrite(2, 0x18, "test", 12, 11);
  NativeAudioTraceModel_SpcUpload(0x1BABED, "transition", 12, 12);
  assert(Request(serial)->outcome ==
         kNativeAudioOutcome_CanceledSongTransition);
}

static void TestMusicSuppressionIsAttributedToLaneOwner(void) {
  NativeAudioTraceModel_Reset();
  uint64_t serial = Post(kNativeAudioRequest_Sfx, 0x10, 1);
  DeliverToRead(kNativeAudioRequest_Sfx, 0x10, 2);
  NativeAudioTraceModel_SpcOpcode(0x0E14, 0x10, 0x12, 0, 0, 5);
  NativeAudioTraceModel_MusicUpdateSuppressed(
      0x05B1, 0x0E, 0x80, 0x80, 6);

  const NativeAudioRequestRecord *request = Request(serial);
  assert(request->music_updates_suppressed == 1);
  assert(request->music_suppressed_voice_mask == 0x80);
  NativeAudioTraceStats stats;
  NativeAudioTraceModel_GetStats(&stats);
  assert(stats.music_updates_suppressed == 1);
  assert(stats.music_suppressions_unattributed == 0);

  NativeAudioMusicSuppression suppression;
  assert(NativeAudioTraceModel_CopyMusicSuppressions(&suppression, 1) == 1);
  assert(suppression.spc_pc == 0x05B1);
  assert(suppression.occurrences == 1);
}

static void TestExtendedTransportLifecycle(void) {
  NativeAudioTraceModel_Reset();
  uint64_t serial = NativeAudioTraceModel_PostExtendedRequest(
      kNativeAudioRequest_Sfx, 0x10, "extended", 0x01bb6d,
      20, 4, 5, 1);
  NativeAudioTraceModel_ExtendedDisposition(serial, 0, 0, 0, 2);
  NativeAudioTraceModel_ExtendedSequenceStart(serial, 1, 8, 3);
  const NativeAudioRequestRecord *request = Request(serial);
  assert(request->flags & kNativeAudioFlag_ExtendedTransport);
  assert(request->active_lanes == 2);
  assert(request->virtual_voices_started == 1);
  NativeAudioTraceModel_ExtendedSequenceEnd(serial, 1, 4);
  assert(Request(serial)->outcome == kNativeAudioOutcome_Completed);

  uint64_t highest_voice = NativeAudioTraceModel_PostExtendedRequest(
      kNativeAudioRequest_Sfx, 0x11, "extended", 0x01bb6d,
      20, 4, 5, 4);
  NativeAudioTraceModel_ExtendedSequenceStart(highest_voice, 1, 39, 5);
  assert(Request(highest_voice)->virtual_voices_started == 0x80000000u);
  NativeAudioTraceModel_ExtendedSequenceEnd(highest_voice, 1, 6);
  assert(Request(highest_voice)->outcome == kNativeAudioOutcome_Completed);

  uint64_t duplicate = NativeAudioTraceModel_PostExtendedRequest(
      kNativeAudioRequest_Sfx, 0x10, "extended", 0x01bb6d,
      20, 4, 5, 7);
  NativeAudioTraceModel_ExtendedDisposition(
      duplicate, serial, 1, 0, 8);
  assert(Request(duplicate)->outcome ==
         kNativeAudioOutcome_CoalescedExtendedDuplicate);
  assert(Request(duplicate)->replaced_by_serial == serial);

  uint64_t overflow = NativeAudioTraceModel_PostExtendedRequest(
      kNativeAudioRequest_Event, 0x83, "extended", 0x00f68c,
      21, 0, 0, 9);
  NativeAudioTraceModel_ExtendedDisposition(overflow, 0, 0, 1, 10);
  assert(Request(overflow)->outcome ==
         kNativeAudioOutcome_ExtendedFifoOverflow);
}

static void TestExtendedPairAndUploadCancel(void) {
  NativeAudioTraceModel_Reset();
  uint64_t pair = NativeAudioTraceModel_PostExtendedRequest(
      kNativeAudioRequest_Event, 0x83, "extended", 0x00f68c,
      30, 0, 0, 1);
  NativeAudioTraceModel_ExtendedSequenceStart(pair, 0, 10, 2);
  NativeAudioTraceModel_ExtendedSequenceStart(pair, 1, 11, 3);
  assert(Request(pair)->active_lanes == 3);
  assert(Request(pair)->virtual_voices_started == 0x0c);
  NativeAudioTraceModel_ExtendedSequenceEnd(pair, 0, 4);
  assert(Request(pair)->outcome == kNativeAudioOutcome_Pending);
  NativeAudioTraceModel_ExtendedSequenceEnd(pair, 1, 5);
  assert(Request(pair)->outcome == kNativeAudioOutcome_Completed);

  uint64_t canceled = NativeAudioTraceModel_PostExtendedRequest(
      kNativeAudioRequest_Sfx, 0x18, "extended", 0x01bc21,
      31, 0, 0, 6);
  NativeAudioTraceModel_ExtendedSequenceStart(canceled, 1, 12, 7);
  NativeAudioTraceModel_ExtendedCancel(canceled, 8);
  assert(Request(canceled)->outcome ==
         kNativeAudioOutcome_CanceledSongTransition);
}

int main(void) {
  TestMailboxOverwrite();
  TestMailboxDuplicateCoalescing();
  TestOrdinaryCompletion();
  TestAcceptedReadSurvivesLaterPortClear();
  TestPortOverwrite();
  TestPortDuplicateCoalescing();
  TestOrdinaryBusyRejection();
  TestPositiveEventBusyRejection();
  TestLaneReplacement();
  TestNewRequestForSameIdIsLaneRetrigger();
  TestStablePortRetriggerIsNotLaneReplacement();
  TestHighBitDualLaneCompletion();
  TestHighBitEventBypassesBusyAndReplacesPair();
  TestSuppressionAndSongEventsAreNotDrops();
  TestActiveEffectCanceledBySongUpload();
  TestMusicSuppressionIsAttributedToLaneOwner();
  TestExtendedTransportLifecycle();
  TestExtendedPairAndUploadCancel();
  puts("native audio trace tests passed");
  return 0;
}
