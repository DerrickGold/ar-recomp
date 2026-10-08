/* Included by input_map.c to share its selected device and hint policy. */
static const char *ButtonGlyphSymbol(uint32 binding, SDL_GamepadType type) {
  const int kind = INPUT_BIND_KIND(binding), code = INPUT_BIND_CODE(binding);
  const bool nintendo_axis = type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO ||
      type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT ||
      type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
  if (kind == kInputBind_PadAxis) {
    if (code == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) return type == SDL_GAMEPAD_TYPE_PS5 ||
        type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS3 ? "l2" : nintendo_axis ? "zl" : "lt";
    if (code == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) return type == SDL_GAMEPAD_TYPE_PS5 ||
        type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS3 ? "r2" : nintendo_axis ? "zr" : "rt";
    return NULL;
  }
  if (kind != kInputBind_PadButton) return NULL;
  static const char *const labels[] = {NULL,"a","b","x","y","cross","circle","square","triangle"};
  if (code >= SDL_GAMEPAD_BUTTON_SOUTH && code <= SDL_GAMEPAD_BUTTON_NORTH) {
    const SDL_GamepadButtonLabel label = SDL_GetGamepadButtonLabelForType(type,(SDL_GamepadButton)code);
    if (label > SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN && label <= SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE)
      return labels[label];
    static const char *const standard[] = {"a","b","x","y"};
    return standard[code];
  }
  const bool ps = type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
  const bool nintendo = type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO ||
      type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT ||
      type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
  switch (code) {
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ps ? "l1" : nintendo ? "l" : "lb";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ps ? "r1" : nintendo ? "r" : "rb";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "right";
    case SDL_GAMEPAD_BUTTON_BACK: return "back";
    case SDL_GAMEPAD_BUTTON_START: return "start";
    case SDL_GAMEPAD_BUTTON_GUIDE: return "guide";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "left_stick";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "right_stick";
    default: return NULL;
  }
}
void InputMap_FormatButtonPrompt(ArButtonPrompt *prompt,uint32 binding,SDL_GamepadType type) {
  if (!prompt) return;
  InputMap_FormatBindingHint(prompt->label,sizeof(prompt->label),binding,type);
  if (!prompt->label[0]) snprintf(prompt->label,sizeof(prompt->label),"—");
  const char *family = type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 ||
      type == SDL_GAMEPAD_TYPE_PS5 ? "playstation" :
      type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT ||
      type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR ?
      "nintendo" : type == SDL_GAMEPAD_TYPE_XBOX360 || type == SDL_GAMEPAD_TYPE_XBOXONE ? "xbox" : "generic";
  const char *symbol=ButtonGlyphSymbol(binding,type);
  if (symbol && (!strcmp(symbol,"zl") || !strcmp(symbol,"zr")))
    snprintf(prompt->label,sizeof(prompt->label),"%s",!strcmp(symbol,"zl")?"ZL":"ZR");
  if (symbol) snprintf(prompt->icon,sizeof(prompt->icon),"button.glyph.%s.%s",family,symbol);
  else snprintf(prompt->icon,sizeof(prompt->icon),"text:%s",prompt->label);
}
void InputMap_CaptureButtonPrompts(void *context, ArButtonPrompts *prompts) {
  (void)context;
  char hint[kArButtonPromptLabelCapacity];
  InputMap_GameActionHint(hint, sizeof(hint), kInputAction_B);
  GamepadSlot *pad = SelectedGamepad();
  const SDL_GamepadType type = pad ? SDL_GetGamepadType(pad->pad) : SDL_GAMEPAD_TYPE_STANDARD;
  for (unsigned i = 0; i < kArButtonPromptCount; ++i) {
    const InputAction action = i < kInputAction_PadCount ? (InputAction)i : kInputAction_SimDescribe;
    const uint32 binding = g_settings.input_bind[s_hint_class][action];
    ArButtonPrompt *prompt = &prompts->buttons[i];
    InputMap_FormatButtonPrompt(prompt,binding,type);
  }
}
