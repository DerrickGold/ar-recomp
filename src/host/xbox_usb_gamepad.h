#ifndef AR_XBOX_USB_GAMEPAD_H
#define AR_XBOX_USB_GAMEPAD_H

/* Optional original Xbox One USB input. All calls belong to the main SDL
 * thread. Builds without AR_XBOX_USB_ROOT provide no-op implementations. */
void HostXboxUsbGamepad_Start(void);
void HostXboxUsbGamepad_Update(void);
void HostXboxUsbGamepad_Stop(void);
#endif
