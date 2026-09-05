/* astdelux_teensy.ino - Asteroids Deluxe, the C port, standalone on the
 * Teensy 4.1 of the Masteroids-class replica board.
 *
 * Build: run ..\build_teensy.bat (stage.py copies the game core into src/,
 * then arduino-cli compiles for Teensy 4.1 at 816 MHz, Fastest), or open
 * this folder in Arduino IDE 2 after running stage.py.  See ../README.md.
 *
 * VEC_TEST_PATTERN (teensy_config.h) = 1 draws the bring-up pattern instead
 * of running the game.
 */
#include "teensy_config.h"
#include "vec_out.h"
#include "plat_teensy.h"

extern "C" {
#include "src/core/platform/ad_platform.h"
}

void setup()
{
    if (plat_init() != 0) {
        pinMode(LED_BUILTIN, OUTPUT);
        for (;;) { digitalToggle(LED_BUILTIN); delay(100); }
    }
#if !VEC_TEST_PATTERN
    ad_app_init();                         /* PWRON, INIT, first wave */
#endif
}

void loop()
{
#if VEC_TEST_PATTERN
    plat_teensy_test_pattern();
#else
    double now  = plat_now_ms();
    double wait = ad_app_step(now);        /* the elapsed 4 ms NMIs; a frame when SYNC says so */
    if (wait > 3.0) plat_sleep_ms(1);
    if (plat_now_ms() - plat_teensy_last_frame_ms() > (double)VEC_PARK_MS)
        vec_out_park();                    /* nothing drawn lately: blank and centre */
#endif
}
