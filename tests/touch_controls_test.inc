/* Real touch -> SDL virtual gamepad -> mapping capture and gameplay. Only the
 * touchscreen device query is stubbed; this runs without touchscreen hardware. */
enum { kTestTouchscreen = 1, kTestTrackpad = 2 };

SDL_TouchDeviceType SDLCALL Test_TouchDeviceType(SDL_TouchID id) {
  return id == kTestTouchscreen ? SDL_TOUCH_DEVICE_DIRECT : SDL_TOUCH_DEVICE_INDIRECT_ABSOLUTE;
}

static void TouchFinger(Uint32 type, SDL_FingerID finger, float x, float y) {
  SDL_Event event = {.type = type};
  event.tfinger.touchID = kTestTouchscreen;
  event.tfinger.fingerID = finger;
  event.tfinger.x = x / 1000.0f;
  event.tfinger.y = y / 500.0f;
  CHECK(TouchControls_HandleEvent(&event));
}

static void PumpTouchEvents(bool capture) {
  SDL_Event event;
  /* SDL's poll sentinel defers events created while handling hotplug until
   * the next pump, just as the host receives them on its next iteration. */
  do {
    while (SDL_PollEvent(&event)) {
      if (TouchControls_HandleEvent(&event)) continue;
      if (capture && SettingsOverlay_HandleCaptureEvent(&event)) continue;
      InputMap_HandleEvent(&event);
    }
  } while (SDL_HasEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST));
}

static void CheckTouchControls(void) {
  const Settings saved = g_settings;
  CHECK(SDL_InitSubSystem(SDL_INIT_GAMEPAD));
  CHECK(SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"));
  TouchControls_Init();
  InputMap_Init();
  TouchControls_Draw((ArRenderExtentI){1000, 500});
  g_settings.input_device = kInputDevice_Auto;
  g_settings.input_gamepad_slot = 0;

  /* A trackpad must not create an on-screen controller. */
  SDL_Event event = {.type = SDL_EVENT_FINGER_DOWN};
  event.tfinger.touchID = kTestTrackpad;
  CHECK(TouchControls_HandleEvent(&event));
#ifndef __ANDROID__
  CHECK(InputMap_GamepadCount() == 0);
#endif

  /* Capture the on-screen B as attack. Queue the whole tap before processing
   * events: a fast tap must still generate a down edge and be detected. */
  RowToKey("bind_pad_y");
  CHECK(SettingsOverlay_HandleKey(SDLK_RETURN, true, false));
  CHECK(SettingsOverlay_IsCapturing());
  TouchFinger(SDL_EVENT_FINGER_DOWN, 1, 857.5f, 435.0f);
  TouchFinger(SDL_EVENT_FINGER_UP, 1, 857.5f, 435.0f);
  PumpTouchEvents(true);
  CHECK(!SettingsOverlay_IsCapturing());
  CHECK(InputMap_GamepadCount() == 1);
  CHECK(!strcmp(InputMap_GamepadName(0), "Touch controls"));
  const uint32 south = INPUT_BIND_MAKE(kInputBind_PadButton, SDL_GAMEPAD_BUTTON_SOUTH, false);
  CHECK(g_settings.input_bind[kInputClass_Gamepad][kInputAction_Y] == south);
  CHECK(g_settings.input_bind[kInputClass_Gamepad][kInputAction_B] == 0);
  char hint[64];
  CHECK(InputMap_ActionHintForDevice(hint, sizeof hint, kInputAction_Y, kInputClass_Gamepad));
  CHECK(!strcmp(hint, "B"));
  CHECK(!InputMap_GamepadIsActive());

  /* The captured mapping uses the normal save path and survives a reload. */
  g_settings.input_bind[kInputClass_Gamepad][kInputAction_Y] = 0;
  CHECK(Settings_Load(getenv("AR_OVERLAY_TEST_SETTINGS_PATH")));
  CHECK(g_settings.input_bind[kInputClass_Gamepad][kInputAction_Y] == south);

  /* Changing keyboard bindings must not change a touch button's identity.
   * Check both Auto and explicit Gamepad mode, and release across a rebind. */
  g_settings.input_bind[kInputClass_Keyboard][kInputAction_B] =
      INPUT_BIND_MAKE(kInputBind_Key, SDL_SCANCODE_P, false);
  for (int mode = kInputDevice_Auto; mode <= kInputDevice_Gamepad; ++mode) {
    g_settings.input_device = mode;
    TouchFinger(SDL_EVENT_FINGER_DOWN, 1, 857.5f, 435.0f);
    PumpTouchEvents(false);
    CHECK(InputMap_State() == (mode == kInputDevice_Keyboard ? 0 : 1u << kInputAction_Y));
    TouchFinger(SDL_EVENT_FINGER_CANCELED, 1, 0, 0);
    PumpTouchEvents(false);
    CHECK(InputMap_State() == 0);
    CHECK(!InputMap_GamepadIsActive());
  }
  g_settings.input_device = kInputDevice_Auto;
  CHECK(SettingsOverlay_HandleKey(SDLK_RETURN, true, false));
  TouchFinger(SDL_EVENT_FINGER_DOWN, 1, 790.0f, 367.5f);
  PumpTouchEvents(true);
  CHECK(!SettingsOverlay_IsCapturing());
  CHECK(g_settings.input_bind[kInputClass_Gamepad][kInputAction_Y] ==
        INPUT_BIND_MAKE(kInputBind_PadButton, SDL_GAMEPAD_BUTTON_WEST, false));
  TouchFinger(SDL_EVENT_FINGER_UP, 1, 790.0f, 367.5f);
  PumpTouchEvents(false);
  CHECK(!InputMap_GamepadIsActive());

  /* Restore defaults to check diagonals, sliding and multiple fingers. */
  for (int a = 0; a < kInputAction_Count; ++a)
    g_settings.input_bind[kInputClass_Gamepad][a] =
        InputMap_DefaultBinding((InputAction)a, kInputClass_Gamepad);
  InputMap_Clear();
  TouchFinger(SDL_EVENT_FINGER_DOWN, 1, 205, 305);
  TouchFinger(SDL_EVENT_FINGER_DOWN, 2, 857.5f, 435);
  TouchFinger(SDL_EVENT_FINGER_DOWN, 3, 857.5f, 435);
  PumpTouchEvents(false);
  CHECK(InputMap_State() == ((1u << kInputAction_Up) | (1u << kInputAction_Right) |
                            (1u << kInputAction_B)));
  TouchFinger(SDL_EVENT_FINGER_UP, 2, 857.5f, 435);
  PumpTouchEvents(false);
  CHECK(InputMap_State() & (1u << kInputAction_B));
  TouchFinger(SDL_EVENT_FINGER_MOTION, 3, 925, 367.5f);
  PumpTouchEvents(false);
  CHECK(InputMap_State() == ((1u << kInputAction_Up) | (1u << kInputAction_Right) |
                            (1u << kInputAction_A)));
  event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
  CHECK(!TouchControls_HandleEvent(&event));
  PumpTouchEvents(false);
  CHECK(InputMap_State() == 0 && !InputMap_GamepadIsActive());

  /* Every other drawn control has a fixed controller identity, including the
   * reserved Menu button. No keyboard events are synthesized. */
  const struct { float x, y; SDL_GamepadButton button; } controls[] = {
      {857.5f, 300, SDL_GAMEPAD_BUTTON_NORTH},
      {85, 47.5f, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
      {915, 47.5f, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
      {447.5f, 458.75f, SDL_GAMEPAD_BUTTON_BACK},
      {552.5f, 458.75f, SDL_GAMEPAD_BUTTON_START},
      {500, 28.75f, SDL_GAMEPAD_BUTTON_MISC1},
  };
  for (size_t i = 0; i < SDL_arraysize(controls); ++i) {
    TouchFinger(SDL_EVENT_FINGER_DOWN, 1, controls[i].x, controls[i].y);
    TouchFinger(SDL_EVENT_FINGER_UP, 1, controls[i].x, controls[i].y);
    int presses = 0, releases = 0;
    while (SDL_PollEvent(&event)) {
      CHECK(event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP);
      if (event.type != SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
          event.type != SDL_EVENT_GAMEPAD_BUTTON_UP) continue;
      CHECK(event.gbutton.button == controls[i].button);
      CHECK(TouchControls_IsMenuEvent(&event) ==
            (controls[i].button == SDL_GAMEPAD_BUTTON_MISC1));
      if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) ++presses;
      else ++releases;
    }
    if (presses != 1 || releases != 1)
      fprintf(stderr, "touch button %d: %d presses, %d releases\n",
              controls[i].button, presses, releases);
    CHECK(presses == 1 && releases == 1);
  }

  /* Plugging in a physical pad replaces the touch device and releases a held
   * button. Removing it restores touch without leaving the old finger held. */
  TouchFinger(SDL_EVENT_FINGER_DOWN, 1, 857.5f, 435);
  PumpTouchEvents(false);
  CHECK(InputMap_State() == (1u << kInputAction_B));
  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
  desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
  desc.name = "External controller";
  const SDL_JoystickID external = SDL_AttachVirtualJoystick(&desc);
  CHECK(external != 0);
  PumpTouchEvents(false);
  CHECK(InputMap_GamepadCount() == 1);
  CHECK(!strcmp(InputMap_GamepadName(0), "External controller"));
  CHECK(InputMap_State() == 0);
  TouchFinger(SDL_EVENT_FINGER_DOWN, 2, 857.5f, 435);
  PumpTouchEvents(false);
  CHECK(InputMap_State() == 0);
  CHECK(SDL_DetachVirtualJoystick(external));
  PumpTouchEvents(false);
  CHECK(InputMap_GamepadCount() == 1);
  CHECK(!strcmp(InputMap_GamepadName(0), "Touch controls"));
  CHECK(InputMap_State() == 0 && !InputMap_GamepadIsActive());
  TouchFinger(SDL_EVENT_FINGER_DOWN, 3, 857.5f, 435);
  PumpTouchEvents(false);
  CHECK(InputMap_State() == (1u << kInputAction_B));

  InputMap_Shutdown();
  TouchControls_Shutdown();
  int devices = -1;
  SDL_JoystickID *ids = SDL_GetGamepads(&devices);
  CHECK(devices == 0);
  SDL_free(ids);
  SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
  SDL_ResetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
  SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  g_settings = saved;
  SettingsOverlay_Refresh();
  RowToKey("bind_pad_up");
}
