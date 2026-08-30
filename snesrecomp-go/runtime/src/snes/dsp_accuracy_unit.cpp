// Keep the accuracy core and its runner bridge in one translation unit. This
// lets release builds inline the one-slot device call without requiring LTO
// objects in the portable runtime archive. The implementation files remain
// separate so the imported core can still be tested directly against its
// upstream suite.
#include "accuracy/dsp.cpp"
#include "dsp_accuracy_bridge.cpp"
