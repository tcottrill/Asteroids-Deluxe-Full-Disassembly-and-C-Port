/* A minimal host, enough to run the game headless.
 *
 * Everything the ROM reached through hardware comes through here.  This
 * one supplies quiet hardware, a few scripted switches, a real POKEY
 * (pokey.c) standing behind RANDOM/ALLPOT, and quiet sound output: the
 * renderer exists (ad_pokey_render()) but nothing here calls it,
 * because this host has no audio device.  A real host also swaps in
 * input and a renderer that interprets g.vram as a display list
 * (disasm/vramview.py does that offline).
 *
 *     astdelux_test [frames] [--dump file] [--dumpall file] [--start]
 *                   [--fire] [--thrust] [--rotl] [--shield] [--test]
 *                   [--probe] [--nv file] [--rev3]
 *
 * --start presses player 1 START for a few frames early on; the other
 * switches are held from frame 8 on.  --test holds the cabinet
 * self-test switch instead of running the game: STEST3 runs once, then
 * `frames` passes of the self-test display loop.  --dump writes vector
 * RAM at the end for vramview.py.  --dumpall <file> appends one
 * 2304-byte record - 256 bytes of zero page (g.zp.raw) then the 2048
 * bytes of vector RAM (g.vram) - on every ad_hw_vg_go() kick, in both
 * the game loop and --test mode; frame N's vector RAM then sits at byte
 * offset N*2304+256 of the file (see tests/dvg_test.c, which replays a
 * DVG state machine against these dumps).  --nv <file> loads the
 * EAROM's 64-byte image from `file` before the run (a missing file
 * leaves the image zero-filled, same as a fresh chip on this board) and
 * writes it back to `file` at the end, so a play-through's high-score
 * persistence can be checked across two runs from the command line.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astdelux.h"
#include "pokey.h"
#include "er2055.h"

/* POKEY clock is the same 1.512 MHz as the 6502 (astdelux2_main.asm);
 * 44100 is an arbitrary render rate, since nothing here plays audio yet. */
static ad_pokey pokey;
#define AD_NMI_POKEY_CYCLES 6048   /* one 4 ms NMI period at 1.512 MHz */
#define AD_RANDOM_READ_COST 64     /* POKEY cycles charged before each game RANDOM
                                    * read: a stand-in for the 6502 cycles the read
                                    * and its neighbours take, since there is no CPU
                                    * to count them (the chip charges nothing - pokey.h) */
#define AD_MAINLINE_LEAD_CYCLES 512 /* advanced before each main-line frame: on the
                                    * board the frame runs for milliseconds after the
                                    * NMI, so PKYTST's ALLPOT read finds the NMI's
                                    * 228-cycle pot scan finished ("S/B 0"); see
                                    * app_win.c for the same constant */

/* Which program ROM revision the game behaves as: 2 (the base) unless
 * --rev3 is given.  Probes may set it directly. */
int ad_host_rom_rev = 2;

/* The EAROM, in memory only here - no platform, so no file by default.
 * --nv <file> (below) loads it before the run and writes it back after,
 * so a play-through's persistence can be checked from the command line
 * without a real host. */
static ad_er2055 earom;

#ifdef AD_PROBE
/* Exposed for probe_er2055.c's round trip through ad_eaupd, which only
 * reaches this earom through the ad_hw_earom_* seam - and for that
 * probe's corruption test, which pokes a ROM byte directly, the same
 * footing as it poking g.zp fields. */
ad_er2055 *ad_host_earom(void) { return &earom; }
#endif

static unsigned frames;
static unsigned nmis;
static bool opt_start, opt_fire, opt_thrust, opt_rotl, opt_shield, opt_test;

/* Probe hooks for stest.c's checks.  A probe drives inputs directly
 * (like ad_host_random_off below), and needs them believed on the very
 * next ad_hw_switch() call - not after the settle-time gate a few lines
 * down, which exists only to keep the ordinary game loop from seeing
 * switches before the board would have. */
bool ad_host_test_sw;                       /* STSTSW  $2007 */
bool ad_host_fire_sw;                       /* FIRESW  $2004 */
bool ad_host_thrust_sw;                     /* THRUST  $2405 */
bool ad_host_rotl_sw;                       /* ROTL    $2407 */
bool ad_host_rotr_sw;                       /* ROTR    $2406 */

/* Switch inputs, bit 7 = pressed, by full 6502 address.  STSTSW is
 * answered before the frame < 8 settle time below: on real hardware
 * PWRON reads it within its first few milliseconds, well before any
 * frame has run. */
uint8_t ad_hw_switch(uint16_t addr)
{
    bool on = false;
    switch (addr) {
    case 0x2007: return (opt_test || ad_host_test_sw) ? 0x80 : 0x00;    /* STSTSW */
    case 0x2004: if (ad_host_fire_sw)   return 0x80; break;             /* FIRESW */
    case 0x2405: if (ad_host_thrust_sw) return 0x80; break;             /* THRUST */
    case 0x2407: if (ad_host_rotl_sw)   return 0x80; break;             /* ROTL */
    case 0x2406: if (ad_host_rotr_sw)   return 0x80; break;             /* ROTR */
    default: break;
    }
    if (frames < 8)
        return 0;
    switch (addr) {
    case 0x2400: on = opt_start && nmis >= 8 && nmis < 56; break;   /* left coin,
                                                * held long enough for DCIN65's
                                                * debounce, then released */
    case 0x2403: on = opt_start && g.zp.f.CRDT != 0 && g.zp.f.NPLAYR == 0; break;
                                                /* STRT1: held once the credit is
                                                 * in, until the game starts */
    case 0x2404: on = false; break;                        /* STRT2 */
    case 0x2405: on = opt_thrust; break;                   /* THRUST */
    case 0x2004: on = opt_fire; break;                     /* FIRESW */
    case 0x2003: on = opt_shield; break;                   /* HYPSW (shield) */
    case 0x2407: on = opt_rotl; break;                     /* ROTL */
    default: break;
    }
    return on ? 0x80 : 0x00;
}

/* POKEY.  Register 8 (read) is ALLPOT: the coin DIP (OPTN5) is strapped
 * to the pot pins, so pokey.c's pot-scan model answers it; $01 is one
 * coin per credit with the multiplier and bonus-adder bits off, and
 * leaves bits 1-7 clear so PKYTST picks the normal rock speeds (see
 * ad_pokey_set_allpot() in main() below). */
uint8_t ad_hw_pokey_read(uint8_t r)   { return ad_pokey_read(&pokey, r); }
void    ad_hw_pokey_write(uint8_t r, uint8_t v) { ad_pokey_write(&pokey, r, v); }

/* RANDOM, from the real POKEY polynomial (pokey.c).  The game's reads
 * are not cycle-annotated, so this host moves the chip a flat
 * AD_RANDOM_READ_COST before each one - see pokey.h's time model.
 * Probes still want a dead-quiet RANDOM for reproducible expectations,
 * hence the override below. */
bool ad_host_random_off;                    /* probes set this for RANDOM = 0 */

uint8_t ad_hw_random(void)
{
    if (ad_host_random_off)
        return 0;
    ad_pokey_advance(&pokey, AD_RANDOM_READ_COST);
    return ad_pokey_read(&pokey, 0x0A);
}

uint8_t ad_hw_rom_rev(void)
{
    return (uint8_t)ad_host_rom_rev;
}

void ad_hw_cycles(uint16_t cycles)
{
    ad_pokey_advance(&pokey, cycles);
}

/* Probes: put a coin-switch byte on the POKEY pot pins (main() straps
 * $01, which happens to hide any ALLPOT timing mistake in PKYTST's
 * `ror`, so a probe that checks "S/B 0" wants a fuller byte). */
void ad_host_set_allpot(uint8_t v)
{
    ad_pokey_set_allpot(&pokey, v);
}

uint8_t ad_hw_earom_read(void)        { return ad_er2055_data(&earom); }
void    ad_hw_earom_write(uint8_t addr, uint8_t data)
                                       { ad_er2055_set_addr_data(&earom, addr, data); }
void    ad_hw_earom_ctl(uint8_t v)    { ad_er2055_control(&earom, v); }

/* --dumpall: appended to on every kick, game loop or self-test alike
 * (both call ad_hw_vg_go() - see mainline.c and stest.c).  sizeof
 * g.zp.raw is exactly 256 bytes (ad_zp_t is a union of the named fields
 * and a raw[0x100] array - astdelux_state.h) and g.vram is exactly
 * AD_VRAM_SIZE == 2048 bytes, so each record is exactly 2304 bytes. */
static FILE *dumpall_file = NULL;

static void close_dumpall(void)
{
    if (dumpall_file) {
        fclose(dumpall_file);
        dumpall_file = NULL;
    }
}

void ad_hw_vg_go(void)
{
    frames++;
    if (dumpall_file) {
        fwrite(g.zp.raw, 1, sizeof g.zp.raw, dumpall_file);
        fwrite(g.vram, 1, sizeof g.vram, dumpall_file);
    }
}
bool    ad_hw_vg_busy(void)           { return false; }
void    ad_hw_vg_reset(void)          { }
void    ad_hw_watchdog(void)          { }
void    ad_hw_explosion(uint8_t v)    { g.expsnd = v; }
void    ad_hw_thrust(bool on)         { g.thrust_on = on; }
void    ad_hw_noise_reset(void)       { }
void    ad_hw_lamp(int w, bool on)    { if (w >= 0 && w < 2) g.lamps[w] = on; }
void    ad_hw_coin_counter(int w)     { (void)w; }

/* Write vector RAM as it stands to `path`, for disasm/vramview.py. */
static int dump_vram(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 1;
    }
    fwrite(g.vram, 1, sizeof g.vram, f);
    fclose(f);
    printf("wrote %s (%u bytes of vector RAM)\n", path, (unsigned)sizeof g.vram);
    return 0;
}

#ifdef AD_PROBE
/* A module's own check, compiled in by build_mod.bat when probe_<mod>.c
 * exists.  Returns 0 on success. */
int ad_probe(void);
#endif

/* --nv support: load the 64-byte EAROM image from `path` if it exists
 * (a missing file just leaves the zero-filled image ad_er2055_init()
 * made), and write it back at exit. */
static void load_nv(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return;                      /* no file yet: stays zero-filled */
    fread(earom.rom, 1, sizeof earom.rom, f);
    fclose(f);
}

static void save_nv(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    fwrite(earom.rom, 1, sizeof earom.rom, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    unsigned want = 60;
    const char *dump = NULL;
    const char *dumpall = NULL;
    const char *nv = NULL;

    /* Live before anything else runs, including a probe: ad_pwron() (a
     * few lines down, and again in the --probe branch) writes SKCTL
     * through ad_hw_pokey_write(), so the chip has to exist first. */
    ad_pokey_init(&pokey, 1512000, 44100);
    ad_pokey_set_allpot(&pokey, 0x01);
    ad_er2055_init(&earom);
    ad_er2055_control(&earom, 0);    /* board reset (asteroid_m.cpp); the
                                      * ROM's own INIT never writes EACTL
                                      * directly - ad_eaupd does, on its
                                      * first step - so mirror the
                                      * hardware reset explicitly here;
                                      * a no-op since init already leaves
                                      * control at 0. */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dump") && i + 1 < argc)
            dump = argv[++i];
        else if (!strcmp(argv[i], "--dumpall") && i + 1 < argc)
            dumpall = argv[++i];
        else if (!strcmp(argv[i], "--nv") && i + 1 < argc)
            nv = argv[++i];
        else if (!strcmp(argv[i], "--start"))  opt_start = true;
        else if (!strcmp(argv[i], "--fire"))   opt_fire = true;
        else if (!strcmp(argv[i], "--thrust")) opt_thrust = true;
        else if (!strcmp(argv[i], "--rotl"))   opt_rotl = true;
        else if (!strcmp(argv[i], "--shield")) opt_shield = true;
        else if (!strcmp(argv[i], "--test"))   opt_test = true;
        else if (!strcmp(argv[i], "--rev3"))   ad_host_rom_rev = 3;
        else if (!strcmp(argv[i], "--probe")) {
#ifdef AD_PROBE
            ad_pwron();
            return ad_probe();
#else
            fprintf(stderr, "no probe compiled in\n");
            return 2;
#endif
        } else
            want = (unsigned)strtoul(argv[i], NULL, 0);
    }

    if (dumpall) {
        dumpall_file = fopen(dumpall, "wb");
        if (!dumpall_file) {
            perror(dumpall);
            return 1;
        }
        atexit(close_dumpall);
    }

    if (nv)
        load_nv(nv);

    ad_pwron();
    printf("state: zp %zu + pg1 %zu + player %zu x2 + vram %d = %zu bytes\n",
           sizeof(ad_zp_t), sizeof(ad_pg1_t), sizeof(ad_player_t),
           AD_VRAM_SIZE, sizeof(ad_state));

    if (opt_test) {
        /* The cabinet self-test: STEST3 once, then `want` passes of the
         * display loop, in place of the game loop below.  ad_stest_frame
         * itself calls ad_hw_vg_go() (which bumps `frames`) each pass it
         * draws, so the loop counts passes on its own. */
        ad_stest3();
        for (unsigned pass = 0; pass < want; pass++) {
            ad_pokey_advance(&pokey, 4 * AD_NMI_POKEY_CYCLES);  /* NMIs don't run
                                                                  * in self-test,
                                                                  * but time still
                                                                  * passes */
            ad_stest_frame();
        }
        printf("self-test: %u passes (%u drawn); XT=%02X CKERR=%02X PERR=%02X VGLIST=$%02X%02X\n",
               want, frames, g.zp.f.XT, g.zp.f.CKERR, g.zp.f.PERR[0],
               g.zp.f.VGLIST[1], g.zp.f.VGLIST[0]);
        if (nv)
            save_nv(nv);
        if (dump)
            return dump_vram(dump);
        return 0;
    }

    /* START ($6000): INIT, kick the EAROM read, then START1/START2. */
    ad_init();
    if (g.zp.f.EAFLG == 0) {
        g.zp.f.EABC = 0;
        g.zp.f.EAX = 0;
        g.zp.f.EAHSX = 0;
        g.zp.f.EAFLG = 0x20;
    }
    ad_newast();
    while (frames < want) {
        /* Four NMIs per frame is the divider the interrupt applies
         * (INTCT & 3); the translated NMI does the rest. */
        for (int i = 0; i < 4; i++) {
            nmis++;
            ad_pokey_advance(&pokey, AD_NMI_POKEY_CYCLES);
            ad_nmi();
        }
        ad_pokey_advance(&pokey, AD_MAINLINE_LEAD_CYCLES);
        if (!ad_frame())
            ad_newast();            /* START1: next wave */
    }

    printf("ran %u frames on %u NMIs; FRAME=%u, VGLIST=$%02X%02X\n",
           frames, nmis,
           (unsigned)(g.zp.f.FRAME[0] | (g.zp.f.FRAME[1] << 8)),
           g.zp.f.VGLIST[1], g.zp.f.VGLIST[0]);
    printf("NPLAYR=%u CRDT=%u NROCKS=%u SROCKS=%u SHPPIX=%02X SAUPIX=%02X score=%02X%02X%02X HITS=%u\n",
           g.zp.f.NPLAYR, g.zp.f.CRDT, AD_P->f.NROCKS, AD_P->f.SROCKS,
           AD_P->raw[AD_OBJ + AD_SHIP], AD_P->raw[AD_OBJ + AD_SAUCER],
           g.zp.f.SCORE[2], g.zp.f.SCORE[1], g.zp.f.SCORE[0], g.zp.f.HITS[0]);
    printf("EDELAY=%02X SEDLAY=%02X RDELAY=%02X RTIMER=%02X DIFCTY=%u SPROCK=%u SDELAY=%02X\n",
           AD_P->f.EDELAY, AD_P->f.SEDLAY, AD_P->f.RDELAY, AD_P->f.RTIMER,
           AD_P->f.DIFCTY, AD_P->f.SPROCK, AD_P->f.SDELAY);
    for (int i = 0; i < AD_SLOTS; i++) {
        uint8_t o = AD_P->raw[AD_OBJ + i];
        if (o)
            printf("  slot %2d OBJ=%02X at %02X%02X,%02X%02X vel %02X,%02X\n", i, o,
                   AD_P->raw[AD_OBJXH + i], AD_P->raw[AD_OBJXL + i],
                   AD_P->raw[AD_OBJYH + i], AD_P->raw[AD_OBJYL + i],
                   AD_P->raw[AD_XINC + i], AD_P->raw[AD_YINC + i]);
    }
    if (nv)
        save_nv(nv);
    if (dump)
        return dump_vram(dump);
    return 0;
}
