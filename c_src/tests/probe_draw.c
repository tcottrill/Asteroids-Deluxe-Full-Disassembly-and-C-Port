/* probe_draw.c - a visual check of ROTAST.
 *
 * Rotates all four rock subroutines in vector RAM as the ROM does
 * (FRAME selects which one), then builds a small display list by hand:
 * a JMPL at word 0, a LABS to the centre, one JSRL to each rock
 * subroutine, spaced out, and a HALT.  Dumps vector RAM to
 * probe_draw.bin for disasm/vramview.py.
 */
#include <stdio.h>

#include "astdelux.h"

int ad_probe(void)
{
    /* Rotate the four rocks, one per frame as START2 does. */
    for (uint8_t f = 0; f < 4; f++) {
        g.zp.f.FRAME[0] = f;
        ad_rotast();
    }

    /* JMPL to word 1 ($4002). */
    g.vram[0] = 0x01;
    g.vram[1] = 0xE0;
    ad_set_vglist(0x4002);

    /* Four rocks across the middle of the screen, full scale. */
    g.zp.f.VGSIZE = 0x00;
    for (uint8_t i = 0; i < 4; i++) {
        ad_vgsabs((uint8_t)(0x30 + i * 0x38), 0x60);
        ad_vgadd2(ad_rom((uint16_t)(AD_ROCKSA + 2 * i)),
                  ad_rom((uint16_t)(AD_ROCKSA + 2 * i + 1)));
    }
    ad_vghalt();

    FILE *fp = fopen("probe_draw.bin", "wb");
    if (!fp) {
        perror("probe_draw.bin");
        return 1;
    }
    fwrite(g.vram, 1, sizeof g.vram, fp);
    fclose(fp);
    printf("wrote probe_draw.bin; list ends at $%04X\n", ad_vglist());
    for (uint8_t i = 0; i < 4; i++) {
        uint16_t w = ad_rom16((uint16_t)(AD_ROCKSA + 2 * i));
        printf("rock %u: JSRL word %04X -> byte address $%04X\n",
               i, w, (unsigned)((w & 0x1FFF) << 1));
    }
    return 0;
}
