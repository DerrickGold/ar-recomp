/* Native pixel reference for the browser compositor, using the production SDL
 * GPU adapter and shader blobs. No game boot/save state or ROM is involved. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include "diorama/diorama_snapshot.h"
#include "platform/sdl/render_sdl_internal.h"

int main(int argc, char **argv) {
  if (argc != 3 && argc != 5 && argc != 9) {
    fprintf(stderr, "usage: %s scene.ardi output.bmp "
        "[width height [distance-scale yaw pitch skybox]]\n", argv[0]);
    return 2;
  }
  size_t size = 0;
  void *packet = SDL_LoadFile(argv[1], &size);
  DioramaSnapshot scene;
  if (!packet || !DioramaSnapshot_Decode(packet,size,&scene)) {
    fprintf(stderr, "invalid compositor snapshot\n");
    SDL_free(packet);
    return 1;
  }
  int width=scene.view.viewport.w, height=scene.view.viewport.h;
  if (argc >= 5) { width=atoi(argv[3]); height=atoi(argv[4]); }
  if (width < 1 || height < 1 || width > 2048 || height > 2048) return 2;
  scene.view.viewport=(ArRenderRectI){0,0,width,height};
  if (argc == 9) {
    scene.view.distance_scale *= strtof(argv[5],NULL);
    scene.view.camera.tilt_y += strtof(argv[6],NULL);
    scene.view.camera.tilt_x += strtof(argv[7],NULL);
    scene.options.skybox=(DioramaSkyMode)atoi(argv[8]);
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr,"SDL video unavailable: %s\n",SDL_GetError());
    SDL_free(packet);
    return 77;
  }
  SDL_Window *window=SDL_CreateWindow(
      "Captured compositor reference",width,height,SDL_WINDOW_HIDDEN);
  ArRenderDevice device={0};
  bool ok=window && ArSdlRenderBackend_CreateForWindow(&device,window,NULL);
  if (!ok) {
    fprintf(stderr,"GPU unavailable: %s\n",SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_free(packet);
    SDL_Quit();
    return 77;
  }
  ok=DioramaSnapshot_Upload(&scene,&device) &&
      ArRenderDevice_Clear(&device,(ArRenderColorF){0,0,0,1});
  DioramaProjection projection;
  if (ok) ok=PresentationOutcome_IsUsable(Diorama_Composite(
      &device,&scene.capture,&scene.view,&scene.scene,&projection));
  SDL_Surface *pixels=ok ? SDL_RenderReadPixels(ArSdlRenderBackend_Renderer(&device),NULL) : NULL;
  ok=pixels && SDL_SaveBMP(pixels,argv[2]);
  if (!ok) fprintf(stderr,"replay failed: %s\n",SDL_GetError());
  else printf("%dx%d shared compositor reference: %s\n",width,height,argv[2]);
  SDL_DestroySurface(pixels);
  Diorama_ResetCompositorResources(&device);
  DioramaSnapshot_ReleaseTextures(&scene,&device);
  ArSdlRenderBackend_Destroy(&device);
  SDL_DestroyWindow(window);
  SDL_Quit();
  SDL_free(packet);
  return ok ? 0 : 1;
}
