#ifndef AR_PRESENTATION_OPTIONS_H
#define AR_PRESENTATION_OPTIONS_H
/* Stable presentation values shared by settings, captures and offline replay. */

typedef enum {
  kPixelAspect_Square = 0,
  kPixelAspect_Crt43,
  kPixelAspect_Count,
} PixelAspect;

/* BG2 can remain a finite plane, fill the viewport as a skybox, or do both. */
typedef enum {
  kDioramaSky_Off = 0,
  kDioramaSky_Only = 1,
  kDioramaSky_Both = 2,
  kDioramaSky_Count,
} DioramaSkyMode;

#endif /* AR_PRESENTATION_OPTIONS_H */
