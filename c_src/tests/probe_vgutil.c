/* Probe for vgutil.c and message.c - build_mod.bat vgutil message, then
 * test_vgutil.exe --probe.
 *
 * Checks the display-list bytes the two modules emit against what the
 * listing says they must be, using an independent decoder of the packed
 * text so the message check does not share code with message.c.
 */
#include <stdio.h>
#include <string.h>

#include "astdelux.h"

static int fails;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            fails++;                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

static uint16_t vram16(uint16_t a)
{
    return (uint16_t)(ad_vram_rd(a) | (ad_vram_rd((uint16_t)(a + 1)) << 8));
}

static uint16_t glyph_word(int glyph)
{
    return ad_rom16((uint16_t)(AD_VGMSGA + 2 * glyph));
}

/* ---- an independent unpacker of the DSTMSG text ---------------------- */

/* Three 5-bit codes per byte pair; code 0 or bit 0 of the second byte
 * ends the string.  Returns the code count. */
static int unpack(uint16_t addr, uint8_t *codes, int max)
{
    int n = 0;

    for (;;) {
        uint8_t b0 = ad_rom(addr);
        uint8_t b1 = ad_rom((uint16_t)(addr + 1));
        uint8_t c[3];
        int i;

        addr = (uint16_t)(addr + 2);
        c[0] = (uint8_t)(b0 >> 3);
        c[1] = (uint8_t)(((b0 & 7) << 2) | (b1 >> 6));
        c[2] = (uint8_t)((b1 >> 1) & 0x1F);
        for (i = 0; i < 3; i++) {
            if (c[i] == 0)
                return n;
            if (n < max)
                codes[n++] = c[i];
        }
        if (b1 & 1)
            return n;
    }
}

/* VGMSG2's mapping: 1 blank, 2-4 = 0/1/2, 5-30 = A-Z, 31 = word 37. */
static int code_glyph(uint8_t c)
{
    return c < 5 ? c - 1 : c + 6;
}

static char code_char(uint8_t c)
{
    if (c == 1) return ' ';
    if (c >= 2 && c <= 4) return (char)('0' + c - 2);
    if (c >= 5 && c <= 30) return (char)('A' + c - 5);
    if (c == 31) return '#';            /* the copyright glyph */
    return '?';
}

static void codes_to_string(const uint8_t *codes, int n, char *out)
{
    int i;
    for (i = 0; i < n; i++)
        out[i] = code_char(codes[i]);
    out[n] = 0;
}

static uint16_t message_addr(int lang, int msg)
{
    uint16_t base = ad_rom16((uint16_t)(AD_VGMSGT + 2 * lang));
    return (uint16_t)(base + ad_rom((uint16_t)(base + msg)));
}

/* ---- (a) VGHEX / VGHEXZ / VGCHAR / VGBLNK / VGDOT / VGLABS ----------- */

static void probe_hex(void)
{
    int d;
    bool c;

    for (d = 0; d < 16; d++) {
        ad_set_vglist(0x4100);
        ad_vghex((uint8_t)(0xF0 | d));          /* high nibble must be ignored */
        CHECK(vram16(0x4100) == glyph_word(d + 1),
              "VGHEX %X: got %04X want %04X", d, vram16(0x4100), glyph_word(d + 1));
        CHECK(ad_vglist() == 0x4102, "VGHEX %X: VGLIST %04X", d, ad_vglist());
    }

    /* Suppressing, digit 0: the blank, still suppressing. */
    ad_set_vglist(0x4100);
    c = ad_vghexz(0x00, true);
    CHECK(c == true && vram16(0x4100) == glyph_word(0), "VGHEXZ(0,C=1)");
    /* Suppressing, digit 7: the digit, suppression off. */
    c = ad_vghexz(0x07, true);
    CHECK(c == false && vram16(0x4102) == glyph_word(8), "VGHEXZ(7,C=1)");
    /* Not suppressing, digit 0: a real zero. */
    c = ad_vghexz(0x00, false);
    CHECK(c == false && vram16(0x4104) == glyph_word(1), "VGHEXZ(0,C=0)");
    CHECK(ad_vglist() == 0x4106, "VGHEXZ VGLIST %04X", ad_vglist());

    /* VGBLNK: digit then the VGSPAC word $CB0A. */
    ad_set_vglist(0x4100);
    ad_vgblnk(3);
    CHECK(vram16(0x4100) == glyph_word(4) && vram16(0x4102) == 0xCB0A
          && ad_vglist() == 0x4104, "VGBLNK");

    /* VGDOT / VGWAIT: 0, timer, 0, intensity. */
    ad_set_vglist(0x4100);
    ad_vgdot(0x70, 0xF0);
    ad_vgwait(0x90);
    CHECK(ad_vram_rd(0x4100) == 0x00 && ad_vram_rd(0x4101) == 0x70
          && ad_vram_rd(0x4102) == 0x00 && ad_vram_rd(0x4103) == 0xF0, "VGDOT bytes");
    CHECK(ad_vram_rd(0x4104) == 0x00 && ad_vram_rd(0x4105) == 0x90
          && ad_vram_rd(0x4106) == 0x00 && ad_vram_rd(0x4107) == 0x00, "VGWAIT bytes");
    CHECK(ad_vglist() == 0x4108, "VGDOT/VGWAIT VGLIST %04X", ad_vglist());

    /* VGLABS(5) must reproduce VGSABS's four bytes from the same XCOMP. */
    g.zp.f.VGSIZE = 0x20;
    ad_set_vglist(0x4100);
    ad_vgsabs(0x7F, 0x45);
    ad_vglabs(5);
    CHECK(memcmp(&g.vram[0x100], &g.vram[0x104], 4) == 0 && ad_vglist() == 0x4108,
          "VGLABS(5) differs from VGSABS: %02X %02X %02X %02X vs %02X %02X %02X %02X",
          g.vram[0x100], g.vram[0x101], g.vram[0x102], g.vram[0x103],
          g.vram[0x104], g.vram[0x105], g.vram[0x106], g.vram[0x107]);
    CHECK(g.vram[0x101] == 0xA1 && g.vram[0x103] == 0x21, "VGSABS opcode/scale bytes");

    /* VGAWT: a LABS then WAIT 7. */
    ad_set_vglist(0x4100);
    ad_vgawt(0x10, 0x20);
    CHECK(ad_vram_rd(0x4105) == 0x70 && ad_vglist() == 0x4108, "VGAWT");
    g.zp.f.VGSIZE = 0;
    printf("(a) VGHEX/VGHEXZ/VGBLNK/VGDOT/VGWAIT/VGLABS/VGAWT: ok\n");
}

/* ---- (b) DIGITS ----------------------------------------------------- */

static void check_digits(const char *name, bool suppress, uint8_t lsb, uint8_t mid,
                         uint8_t msb, const int *want, int n)
{
    int i;

    g.zp.raw[0xF0] = lsb;
    g.zp.raw[0xF1] = mid;
    g.zp.raw[0xF2] = msb;
    ad_set_vglist(0x4100);
    ad_digits(suppress, 0xF0, 3);
    for (i = 0; i < n; i++)
        CHECK(vram16((uint16_t)(0x4100 + 2 * i)) == glyph_word(want[i]),
              "DIGITS %s digit %d: got %04X want glyph %d", name, i,
              vram16((uint16_t)(0x4100 + 2 * i)), want[i]);
    CHECK(ad_vglist() == 0x4100 + 2 * n, "DIGITS %s VGLIST %04X", name, ad_vglist());
    CHECK(g.zp.f.TEMP4[0] == 0xEF && g.zp.f.TEMP4[1] == 0xFF, "DIGITS %s TEMP4 leftovers", name);
}

static void probe_digits(void)
{
    static const int sup[6]   = { 0, 0, 2, 3, 4, 1 };   /* "  1230" */
    static const int nosup[6] = { 1, 1, 2, 3, 4, 1 };   /* "001230" */
    static const int zero[6]  = { 0, 0, 0, 0, 0, 1 };   /* "     0" */

    check_digits("001230/C", true, 0x30, 0x12, 0x00, sup, 6);
    check_digits("001230", false, 0x30, 0x12, 0x00, nosup, 6);
    check_digits("000000/C", true, 0x00, 0x00, 0x00, zero, 6);
    printf("(b) DIGITS: ok\n");
}

/* ---- (c) VGMSG / VGME ----------------------------------------------- */

static void check_message(int lang, int msg, char *text)
{
    uint8_t codes[64];
    int n = unpack(message_addr(lang, msg), codes, (int)sizeof codes);
    uint8_t x4 = ad_rom((uint16_t)(AD_VGMSGS + 2 * msg));
    uint8_t y4 = ad_rom((uint16_t)(AD_VGMSGS + 2 * msg + 1));
    uint16_t xc = (uint16_t)(x4 << 2);
    uint16_t yc = (uint16_t)(y4 << 2);
    uint16_t want_end = (uint16_t)(0x4208 + 2 * n);
    int i;

    codes_to_string(codes, n, text);
    ad_set_vglist(0x4200);
    g.zp.f.VGSIZE = 0xF0;                       /* VGMSG must force $10 */
    if (lang == 0)
        ad_vgmsg((uint8_t)msg);                 /* switches read 0 = language 0 */
    else
        ad_vgme((uint8_t)lang, (uint8_t)msg);

    /* LABS at the VGMSGS position, size $10, then WAIT $70. */
    CHECK(ad_vram_rd(0x4200) == (uint8_t)yc
          && ad_vram_rd(0x4201) == (uint8_t)(((yc >> 8) & 0x0F) | 0xA0)
          && ad_vram_rd(0x4202) == (uint8_t)xc
          /* VGMSG sets VGSIZE=$10 before the VGME entry, so a VGME call
           * keeps the caller's VGSIZE ($F0 here) - see $7194-$7198. */
          && ad_vram_rd(0x4203) == (uint8_t)(((xc >> 8) & 0x0F) | (lang == 0 ? 0x10 : 0xF0)),
          "L%d msg %d: LABS bytes %02X %02X %02X %02X", lang, msg,
          ad_vram_rd(0x4200), ad_vram_rd(0x4201), ad_vram_rd(0x4202), ad_vram_rd(0x4203));
    CHECK(vram16(0x4204) == 0x7000 && vram16(0x4206) == 0x0000,
          "L%d msg %d: WAIT bytes", lang, msg);
    for (i = 0; i < n; i++)
        CHECK(vram16((uint16_t)(0x4208 + 2 * i)) == glyph_word(code_glyph(codes[i])),
              "L%d msg %d char %d ('%c'): got %04X want %04X", lang, msg, i, text[i],
              vram16((uint16_t)(0x4208 + 2 * i)), glyph_word(code_glyph(codes[i])));
    CHECK(ad_vglist() == want_end, "L%d msg %d: VGLIST %04X want %04X",
          lang, msg, ad_vglist(), want_end);
    CHECK(g.zp.f.VGSIZE == (lang == 0 ? 0x10 : 0xF0), "L%d msg %d: VGSIZE", lang, msg);
}

static void probe_messages(void)
{
    char text[4][14][65];
    int lang, msg;
    bool push_start = false;

    for (lang = 0; lang < 4; lang++)
        for (msg = 0; msg < 14; msg++)
            check_message(lang, msg, text[lang][msg]);

    for (msg = 0; msg < 14; msg++) {
        printf("    msg %2d: L0 \"%s\"  L1 \"%s\"  L2 \"%s\"  L3 \"%s\"\n", msg,
               text[0][msg], text[1][msg], text[2][msg], text[3][msg]);
        if (strstr(text[0][msg], "PRESS START"))
            push_start = true;
    }
    CHECK(strncmp(text[0][0], "HIGH SCORE", 10) == 0, "msg 0 is \"%s\"", text[0][0]);
    /* "PLAYER " ends in a space: CHKST2 appends the number with VGHEX. */
    CHECK(strcmp(text[0][1], "PLAYER ") == 0, "msg 1 is \"%s\"", text[0][1]);
    CHECK(strcmp(text[0][7], "GAME OVER") == 0, "msg 7 is \"%s\"", text[0][7]);
    CHECK(push_start, "no L0 message says PRESS START");
    CHECK(strstr(text[0][13], "BONUS") != NULL, "msg 13 is \"%s\"", text[0][13]);
    printf("(c) VGMSG/VGME, 4 languages x 14 messages: ok\n");
}

/* ---- (d) SETROL, CPYRS and the copyright check ----------------------- */

static void probe_copyright(void)
{
    static const uint8_t head[10] = { 0xC1, 0xC9, 0x80, 0xA0, 0xC0, 0x01, 0x00, 0x70, 0x00, 0x00 };
    uint8_t astm[32], astmt[32];
    char s1[33], s2[33];
    int n1 = unpack(AD_ASTM, astm, (int)sizeof astm);
    int n2 = unpack(AD_ASTMT, astmt, (int)sizeof astmt);
    uint16_t end = (uint16_t)(0x470C + 2 * n1);
    int i;

    codes_to_string(astm, n1, s1);
    codes_to_string(astmt, n2, s2);
    printf("    ASTM  \"%s\"  ASTMT \"%s\"\n", s1, s2);
    CHECK(n1 == n2 && memcmp(astm, astmt, (size_t)n1) == 0, "ASTM and ASTMT differ");

    g.zp.f.CPMTST = 0;
    g.zp.f.CRMERR = 0;
    g.zp.f.VGSIZE = 0x30;
    g.zp.f.FRAME[0] = 0x55;
    ad_setrol();
    CHECK(g.zp.f.FRAME[0] == 0, "SETROL leaves FRAME=%02X", g.zp.f.FRAME[0]);
    CHECK(g.zp.f.VGSIZE == 0, "SETROL leaves VGSIZE=%02X", g.zp.f.VGSIZE);
    CHECK(memcmp(&g.vram[0x702], head, 10) == 0, "SETROL head bytes");
    for (i = 0; i < n1; i++)
        CHECK(vram16((uint16_t)(0x470C + 2 * i)) == glyph_word(code_glyph(astm[i])),
              "SETROL glyph %d", i);
    CHECK(vram16(end) == 0xD0D0, "SETROL RTSL at %04X: %04X", end, vram16(end));
    CHECK(ad_vglist() == end + 2, "SETROL VGLIST %04X want %04X", ad_vglist(), end + 2);

    /* The check itself, as UPDATE runs it. */
    g.zp.f.CPMTST = 0x80;
    g.zp.f.CRMERR = 0;
    ad_set_vglist(0x4321);
    ad_vgrcpt();
    CHECK(g.zp.f.CRMERR == 0, "VGRCPT: CRMERR=%02X on an intact list", g.zp.f.CRMERR);
    CHECK(ad_vglist() == 0x4321, "VGRCPT: VGLIST not restored (%04X)", ad_vglist());
    /* Each byte pair gets two VGMSG1 increments; the code-0 terminator
     * is the second character of the last pair, so the pointer stops
     * on ASTMT's last byte, $784E, not one past it. */
    CHECK(g.zp.f.TEMP1[0] == 0x4E && g.zp.f.TEMP1[1] == 0x78, "VGRCPT: TEMP1 ends %02X%02X",
          g.zp.f.TEMP1[1], g.zp.f.TEMP1[0]);

    /* And that it is live: a damaged head byte, then a damaged glyph. */
    g.vram[0x705] ^= 0x01;
    g.zp.f.CRMERR = 0;
    ad_vgrcpt();
    CHECK(g.zp.f.CRMERR != 0, "VGRCPT missed a changed head byte");
    g.vram[0x705] ^= 0x01;
    g.vram[0x70E] ^= 0x04;
    g.zp.f.CRMERR = 0;
    ad_vgrcpt();
    CHECK(g.zp.f.CRMERR != 0, "VGRCPT missed a changed glyph");
    g.vram[0x70E] ^= 0x04;
    g.zp.f.CRMERR = 0;
    ad_vgrcpt();
    CHECK(g.zp.f.CRMERR == 0, "VGRCPT: CRMERR=%02X after repair", g.zp.f.CRMERR);

    /* CPMTST clear: nothing happens. */
    g.zp.f.CPMTST = 0;
    g.vram[0x705] ^= 0x01;
    ad_vgrcpt();
    CHECK(g.zp.f.CRMERR == 0, "VGRCPT ran with CPMTST clear");
    g.vram[0x705] ^= 0x01;

    /* CPYRS: the circled-C word then the same text at the beam. */
    ad_set_vglist(0x4300);
    ad_cpyrs();
    CHECK(vram16(0x4300) == 0xC9C5, "CPYRS JSRL");
    for (i = 0; i < n1; i++)
        CHECK(vram16((uint16_t)(0x4302 + 2 * i)) == glyph_word(code_glyph(astm[i])),
              "CPYRS glyph %d", i);
    CHECK(ad_vglist() == 0x4302 + 2 * n1, "CPYRS VGLIST %04X", ad_vglist());
    printf("(d) SETROL/CPYRS/VGRCPT: ok, CRMERR=%02X\n", g.zp.f.CRMERR);
}

int ad_probe(void)
{
    probe_hex();
    probe_digits();
    probe_messages();
    probe_copyright();
    printf(fails ? "probe: %d failure(s)\n" : "probe: all checks passed\n", fails);
    return fails ? 1 : 0;
}
