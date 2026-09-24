/* Optional native-renderer release tour. All campaigns are copied fixtures;
 * navigation uses the real overlay input path and never commits a save. */
static bool SaveSlotsTourAction(const SettingDesc *desc) {
  if(desc->action==kSettingAction_SaveSlots)return SettingsOverlay_OpenSaveSlots(false);
  return ActionObserved(desc);
}

static void CaptureSaveSlotsReleaseTour(SDL_Renderer *renderer,SDL_Surface *surface) {
  const char *directory=getenv("AR_SAVE_SLOTS_TOUR_DIR");
  if(!directory || !*directory || !renderer || !surface)return;
  CHECK(getenv("AR_OVERLAY_TEST_ROM")!=NULL);
  SettingsOverlay_Close();Settings_Init();g_settings.show_debug_settings=false;
  const SettingsOverlaySaveSlotHooks hooks={.scan=FakeSlotsScan,.draft=FakeSlotDraft,.view=FakeSlotView};
  SettingsOverlay_SetSaveSlotHooks(&hooks);
  Settings_SetActionObserver(SaveSlotsTourAction);
  SettingsOverlay_Open();NavToSection(kSection_Video);
  char path[1024];snprintf(path,sizeof(path),"%s/timeline.tsv",directory);
  FILE *timeline=fopen(path,"w");CHECK(timeline!=NULL);if(!timeline)return;
  unsigned frame=0;
#define TOUR_KEY(key) CHECK(SettingsOverlay_HandleKey(key,true,false))
#define TOUR_FRAME(ms) do { \
    SDL_SetRenderDrawColor(renderer,12,22,34,255);CHECK(SDL_RenderClear(renderer)); \
    SettingsOverlay_Render((ArRenderRectI){0,0,surface->w,surface->h}); \
    CHECK(SDL_RenderPresent(renderer)); \
    snprintf(path,sizeof(path),"%s/%02u.bmp",directory,frame);CHECK(SDL_SaveBMP(surface,path)); \
    fprintf(timeline,"%02u.bmp\t%d\n",frame++,ms); \
  } while(0)
  TOUR_FRAME(700);
  TOUR_KEY(SDLK_UP);TOUR_FRAME(900); /* Saves is first. */
  TOUR_KEY(SDLK_Z);CHECK(!strcmp(SettingsOverlay_SelectedKey(),"slot_list"));TOUR_FRAME(1600);
  TOUR_KEY(SDLK_DOWN);TOUR_FRAME(250);
  TOUR_KEY(SDLK_DOWN);TOUR_FRAME(1500); /* LUNA, a different saved campaign. */
  TOUR_KEY(SDLK_RIGHT);TOUR_FRAME(1800);
  TOUR_KEY(SDLK_RIGHT);TOUR_FRAME(1800);
  TOUR_KEY(SDLK_RIGHT);TOUR_FRAME(600);
  TOUR_KEY(SDLK_Z);TOUR_FRAME(800);
  TOUR_KEY(SDLK_Z);CHECK(!strcmp(SettingsOverlay_SelectedKey(),"slot_confirm"));TOUR_FRAME(1800);
  TOUR_KEY(SDLK_X);TOUR_KEY(SDLK_X);TOUR_FRAME(250); /* Dismiss and leave details. */
  TOUR_KEY(SDLK_UP);TOUR_FRAME(400);
  TOUR_KEY(SDLK_Z);CHECK(!strcmp(SettingsOverlay_SelectedKey(),"slot_type"));TOUR_FRAME(1400);
  TOUR_KEY(SDLK_X);TOUR_FRAME(250);
  TOUR_KEY(SDLK_X);TOUR_FRAME(700);
#undef TOUR_FRAME
#undef TOUR_KEY
  CHECK(fclose(timeline)==0);
  SettingsOverlay_Close();SettingsOverlay_SetSaveSlotHooks(NULL);
  Settings_SetActionObserver(ActionObserved);
}
