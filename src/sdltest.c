/* sdltest - open the first SDL GameController and print live state. Built x86_64 against the custom
 * libSDL2 (with libusb + GIP auth) to verify SDL sees the Wolverine before involving Wine. */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 20;
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint("SDL_HIDAPI_LIBUSB_WHITELIST", "0");     /* not needed with our patch, harmless */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_version v; SDL_GetVersion(&v); printf("SDL %d.%d.%d, %d joystick(s)\n", v.major, v.minor, v.patch, SDL_NumJoysticks());
    SDL_GameController *gc = NULL; Uint32 t0 = SDL_GetTicks(), last = 0;
    int amin[6], amax[6]; for (int i = 0; i < 6; i++) { amin[i] = 32767; amax[i] = -32768; }
    unsigned seen_btn = 0; const unsigned want_btn = (1u << 15) - 1;  /* SDL buttons 0..14 (A..DPAD_RIGHT) */
    int done = 0;
    while (SDL_GetTicks() - t0 < seconds * 1000 && !done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_CONTROLLERDEVICEADDED) {
                gc = SDL_GameControllerOpen(e.cdevice.which);
                SDL_Joystick *j = SDL_GameControllerGetJoystick(gc);
                printf("ADDED: %s (vid %04x pid %04x) axes=%d buttons=%d type=%d\n", SDL_GameControllerName(gc), SDL_JoystickGetVendor(j), SDL_JoystickGetProduct(j), SDL_JoystickNumAxes(j), SDL_JoystickNumButtons(j), SDL_GameControllerGetType(gc));
            } else if (e.type == SDL_JOYDEVICEADDED) {
                printf("joystick added: %s (is gamecontroller: %d)\n", SDL_JoystickNameForIndex(e.jdevice.which), SDL_IsGameController(e.jdevice.which));
            } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) printf("REMOVED\n");
        }
        if (gc) {
            for (int i = 0; i < 6; i++) { int v = SDL_GameControllerGetAxis(gc, (SDL_GameControllerAxis)i); if (v < amin[i]) amin[i] = v; if (v > amax[i]) amax[i] = v; }
            for (int b = 0; b < 15; b++) if (SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)b)) seen_btn |= 1u << b;
            int axes_ok = 1; for (int i = 0; i < 4; i++) if (amin[i] > -32000 || amax[i] < 32000) axes_ok = 0;
            for (int i = 4; i < 6; i++) if (amin[i] > 0 || amax[i] < 32000) axes_ok = 0;
            if (axes_ok && (seen_btn & want_btn) == want_btn) done = 1;
        }
        if (gc && SDL_GetTicks() - last > 100) {
            last = SDL_GetTicks();
            printf("LX %6d LY %6d RX %6d RY %6d LT %5d RT %5d | A%d B%d X%d Y%d LB%d RB%d LS%d RS%d Back%d Start%d Guide%d U%d D%d L%d R%d\n",
                SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX), SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY),
                SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX), SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY),
                SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT), SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT),
                SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A), SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B),
                SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X), SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y),
                SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER), SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER),
                SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK), SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK),
                SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK), SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START),
                SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_GUIDE),
                SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP), SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN),
                SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT), SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT));
            fflush(stdout);
        }
        SDL_Delay(5);
    }
    printf("\n=== coverage report (%s) ===\n", done ? "COMPLETE: all axes full range + all buttons seen" : "timeout");
    const char *an[6] = { "LX", "LY", "RX", "RY", "LT", "RT" };
    for (int i = 0; i < 6; i++) printf("%s: min %6d max %6d\n", an[i], amin[i], amax[i]);
    const char *bn[15] = { "A", "B", "X", "Y", "Back", "Guide", "Start", "LS", "RS", "LB", "RB", "DU", "DD", "DL", "DR" };
    printf("buttons seen:"); for (int b = 0; b < 15; b++) if (seen_btn & (1u << b)) printf(" %s", bn[b]); printf("\n");
    printf("buttons missing:"); for (int b = 0; b < 15; b++) if (!(seen_btn & (1u << b))) printf(" %s", bn[b]); printf("\n");
    if (gc) SDL_GameControllerClose(gc);
    SDL_Quit();
    return 0;
}
