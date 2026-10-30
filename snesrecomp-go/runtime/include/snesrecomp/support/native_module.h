/* Forced include for private native modules. Load the CRT without remapping its
 * declarations, then declare the module's C-linkage abort hook before libc++
 * imports it through <cstdlib>. MinGW protects the real abort declaration from
 * macro replacement; other CRTs attach platform-specific exception attributes. */
#ifndef SNESRECOMP_SUPPORT_NATIVE_MODULE_H
#define SNESRECOMP_SUPPORT_NATIVE_MODULE_H

#ifdef __cplusplus
#pragma push_macro("abort")
#undef abort
#include <stdlib.h>

extern "C" {
[[noreturn]] void sr_native_module_abort(void) noexcept;
}
#pragma pop_macro("abort")
#else
/* Leave C sources free to choose their CRT feature macros before including it. */
_Noreturn void sr_native_module_abort(void);
#endif

#endif
