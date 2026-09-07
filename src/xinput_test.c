/* xinput_test.exe - print XInput state for all 4 slots (built with mingw-w64, run under Wine). */
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 30;
    setvbuf(stdout, NULL, _IONBF, 0);
    DWORD t0 = GetTickCount();
    int done = 0;
    XINPUT_STATE last[4] = {0}; int connected[4] = {0,0,0,0};
    SHORT lxmin=0,lxmax=0,lymin=0,lymax=0,rxmin=0,rxmax=0,rymin=0,rymax=0; BYTE ltmax=0, rtmax=0; WORD seen=0;
    printf("xinput_test: polling XInput slots 0-3 for %d s\n", seconds); fflush(stdout);
    while ((int)(GetTickCount() - t0) < seconds * 1000 && !done) {
        for (DWORD i = 0; i < 4; i++) {
            XINPUT_STATE st; ZeroMemory(&st, sizeof st);
            DWORD r = XInputGetState(i, &st);
            if (r == ERROR_SUCCESS) {
                if (!connected[i]) { connected[i] = 1; printf("slot %lu: CONNECTED\n", i);
                    XINPUT_CAPABILITIES caps; if (XInputGetCapabilities(i, 0, &caps) == ERROR_SUCCESS) printf("  type %u subtype %u flags 0x%x buttons 0x%04x lt %u rt %u\n", caps.Type, caps.SubType, caps.Flags, caps.Gamepad.wButtons, caps.Gamepad.bLeftTrigger, caps.Gamepad.bRightTrigger); }
                if (st.dwPacketNumber != last[i].dwPacketNumber) {
                    XINPUT_GAMEPAD *g = &st.Gamepad;
                    if (g->sThumbLX < lxmin) lxmin = g->sThumbLX; if (g->sThumbLX > lxmax) lxmax = g->sThumbLX;
                    if (g->sThumbLY < lymin) lymin = g->sThumbLY; if (g->sThumbLY > lymax) lymax = g->sThumbLY;
                    if (g->sThumbRX < rxmin) rxmin = g->sThumbRX; if (g->sThumbRX > rxmax) rxmax = g->sThumbRX;
                    if (g->sThumbRY < rymin) rymin = g->sThumbRY; if (g->sThumbRY > rymax) rymax = g->sThumbRY;
                    if (g->bLeftTrigger > ltmax) ltmax = g->bLeftTrigger; if (g->bRightTrigger > rtmax) rtmax = g->bRightTrigger;
                    seen |= g->wButtons;
                    if (lxmin < -32000 && lxmax > 32000 && lymin < -32000 && lymax > 32000 && rxmin < -32000 && rxmax > 32000 && rymin < -32000 && rymax > 32000 && ltmax == 255 && rtmax == 255 && (seen & 0xF3FF) == 0xF3FF) done = 1;
                    printf("slot %lu pkt %lu LX %6d LY %6d RX %6d RY %6d LT %3u RT %3u buttons 0x%04x\n", i, st.dwPacketNumber,
                           g->sThumbLX, g->sThumbLY, g->sThumbRX, g->sThumbRY, g->bLeftTrigger, g->bRightTrigger, g->wButtons);
                    fflush(stdout);
                }
                last[i] = st;
            } else if (connected[i]) { connected[i] = 0; printf("slot %lu: disconnected (%lu)\n", i, r); }
        }
        Sleep(10);
    }
    printf("\n=== summary (slot 0) %s ===\n", done ? "COMPLETE: full ranges + all buttons" : "timeout");
    printf("LX %d..%d LY %d..%d RX %d..%d RY %d..%d LTmax %u RTmax %u buttons seen 0x%04x\n", lxmin,lxmax,lymin,lymax,rxmin,rxmax,rymin,rymax,ltmax,rtmax,seen);
    printf("(XINPUT bits: DU 1 DD 2 DL 4 DR 8 START 10 BACK 20 LS 40 RS 80 LB 100 RB 200 A 1000 B 2000 X 4000 Y 8000)\n");
    return 0;
}
