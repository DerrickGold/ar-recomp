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

typedef enum {
  kScreenAspect_43 = 0,
  kScreenAspect_169,
  kScreenAspect_1610,
  kScreenAspect_Stretch,   /* fill the window, ignore aspect (was a separate
                            * "Stretch to window" toggle) */
  kScreenAspect_Auto,      /* appended: saved manual indices stay stable */
  kScreenAspect_Count,
} ScreenAspect;

typedef enum {
  kRenderResolution_Native = 0,
  kRenderResolution_480p,
  kRenderResolution_540p,
  kRenderResolution_720p,
  kRenderResolution_800p,
  kRenderResolution_900p,
  kRenderResolution_1080p,
  kRenderResolution_1440p,
  kRenderResolution_2160p,
  kRenderResolution_Count,
} RenderResolution;

typedef enum {
  kSceneAntiAliasing_Off = 0,
  kSceneAntiAliasing_Fxaa,
  kSceneAntiAliasing_Smaa,
  kSceneAntiAliasing_Count,
} SceneAntiAliasing;

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
