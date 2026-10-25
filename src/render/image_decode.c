/* One PNG/JPEG decoder for native replacement art, the manual, and shared
 * native/WASM scene art. Keep decoding out of platform-specific hosts. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "../../third_party/stb/stb_image.h"
