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

/* How far a finite captured skybox travels as the playfield camera scrolls.
 * Original moves it 1:1 with BG2's scroll and pins it at BG2's world edges.
 * Matched moves it at BG2's native parallax as seen beside the playfield.
 * Full room spreads BG2's whole width across the playfield's travel. */
typedef enum {
  kDioramaSkyScroll_Original = 0,
  kDioramaSkyScroll_Matched = 1,
  kDioramaSkyScroll_FullRoom = 2,
  kDioramaSkyScroll_Count,
} DioramaSkyScroll;

#endif /* AR_PRESENTATION_OPTIONS_H */
