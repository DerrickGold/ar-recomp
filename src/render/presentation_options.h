#ifndef AR_PRESENTATION_OPTIONS_H
#define AR_PRESENTATION_OPTIONS_H
/* Stable presentation values shared by settings, captures and offline replay. */

/* Keep the former action-3D boolean's 0/1 values: existing enabled settings
 * select the original layered presentation. Mode 2 opts into modeled rooms. */
typedef enum {
  kAction3D_Off = 0,
  kAction3D_Diorama,
  kAction3D_Voxel,
  kAction3D_Count,
} Action3DMode;

typedef enum {
  kPixelAspect_Square = 0,
  kPixelAspect_Crt43,
  kPixelAspect_Count,
} PixelAspect;

/* Persisted corner order keeps the original inset position as the default. */
typedef enum {
  kPictureInPicture_BottomRight = 0,
  kPictureInPicture_BottomLeft,
  kPictureInPicture_TopRight,
  kPictureInPicture_TopLeft,
  kPictureInPictureCorner_Count,
} PictureInPictureCorner;

/* BG2 can remain a finite plane, fill the viewport as a skybox, or do both. */
typedef enum {
  kDioramaSky_Off = 0,
  kDioramaSky_Only = 1,
  kDioramaSky_Both = 2,
  kDioramaSky_Count,
} DioramaSkyMode;

#endif /* AR_PRESENTATION_OPTIONS_H */
