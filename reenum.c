/*
 * reenum.c - hand USB enumeration back to the FX2 core.
 *
 * Loaded into RAM (via the 0xA0 request) on top of a resident firmware such
 * as fx2lafw. While disconnected it puts the USB/FIFO/GPIF configuration
 * registers back to their power-on defaults (an 8051 reset does not touch
 * them), clears USBCS.RENUM so the FX2 core answers device requests again
 * (using the VID/PID latched from the EEPROM at power-up), then reconnects.
 * The vendor app then sees a "bare" device and uploads its own firmware.
 */

#include <8051.h>

__sfr __at(0x8E) CKCON;
__sfr __at(0x92) _XPAGE;   /* FX2 MPAGE; SDCC's startup code defaults to P2 (0xA0) */
__sfr __at(0xB2) OEA;
__sfr __at(0xB3) OEB;
__sfr __at(0xB4) OEC;
__sfr __at(0xB5) OED;
__sfr __at(0xB6) OEE;

#define XREG(addr)  (*(__xdata volatile unsigned char *)(addr))

#define CPUCS        XREG(0xE600)
#define IFCONFIG     XREG(0xE601)
#define PINFLAGSAB   XREG(0xE602)
#define PINFLAGSCD   XREG(0xE603)
#define FIFORESET    XREG(0xE604)
#define FIFOPINPOLAR XREG(0xE609)
#define REVCTL       XREG(0xE60B)
#define EP1OUTCFG    XREG(0xE610)
#define EP1INCFG     XREG(0xE611)
#define EP2CFG       XREG(0xE612)
#define EP4CFG       XREG(0xE613)
#define EP6CFG       XREG(0xE614)
#define EP8CFG       XREG(0xE615)
#define EP2FIFOCFG   XREG(0xE618)
#define EP4FIFOCFG   XREG(0xE619)
#define EP6FIFOCFG   XREG(0xE61A)
#define EP8FIFOCFG   XREG(0xE61B)
#define EPAUTOINLEN  0xE620   /* EP2H,EP2L,EP4H,EP4L,EP6H,EP6L,EP8H,EP8L */
#define IBNIE        XREG(0xE658)
#define NAKIE        XREG(0xE65A)
#define USBIE        XREG(0xE65C)
#define USBIRQ       XREG(0xE65D)
#define EPIE         XREG(0xE65E)
#define EPIRQ        XREG(0xE65F)
#define GPIFIE       XREG(0xE660)
#define GPIFIRQ      XREG(0xE661)
#define INTSETUP     XREG(0xE668)
#define PORTACFG     XREG(0xE670)
#define PORTCCFG     XREG(0xE671)
#define PORTECFG     XREG(0xE672)
#define USBCS        XREG(0xE680)
#define SUDPTRH      XREG(0xE6B3)
#define SUDPTRL      XREG(0xE6B4)
#define GPIFIDLECTL  XREG(0xE6C2)
#define FLOWSTB      XREG(0xE6CB)
#define FLOWSTBEDGE  XREG(0xE6CC)
#define FLOWSTBHPERIOD XREG(0xE6CD)
#define EP2GPIFFLGSEL  XREG(0xE6D2)
#define GPIFABORT    XREG(0xE6F5)

#define bmRENUM   0x02
#define bmDISCON  0x08
#define bmNAKALL  0x80

/* Required between some FIFO register writes; a call/ret pair is plenty at 12 MHz. */
static void syncdelay(void) {}
#define SYNCDELAY  syncdelay()

/* Timer0 runs at CLKOUT/12. With CPUCS forced to 12 MHz that is 1 MHz,
 * so one 16-bit overflow is 65.536 ms. */
#define DISCON_OVERFLOWS  12   /* ~786 ms, long enough for macOS to see a detach */

static void wait_overflows(unsigned char n)
{
    TMOD = (TMOD & 0xF0) | 0x01;   /* Timer0, mode 1 (16-bit) */
    while (n--) {
        TR0 = 0;
        TH0 = 0;
        TL0 = 0;
        TF0 = 0;
        TR0 = 1;
        while (!TF0)
            ;
    }
    TR0 = 0;
}

/* Undo what a previous firmware configured; values are the TRM reset defaults. */
static void restore_defaults(void)
{
    unsigned char i;

    GPIFABORT = 0xFF;           SYNCDELAY;

    USBIE = 0;
    EPIE = 0;
    GPIFIE = 0;
    INTSETUP = 0;
    IBNIE = 0;
    NAKIE = 0;
    USBIRQ = 0xFF;
    EPIRQ = 0xFF;
    GPIFIRQ = 0xFF;

    REVCTL = 0x00;              SYNCDELAY;
    IFCONFIG = 0x80;            SYNCDELAY;
    PINFLAGSAB = 0x00;          SYNCDELAY;
    PINFLAGSCD = 0x00;          SYNCDELAY;
    FIFOPINPOLAR = 0x00;        SYNCDELAY;

    EP1OUTCFG = 0xA0;           SYNCDELAY;
    EP1INCFG = 0xA0;            SYNCDELAY;
    EP2CFG = 0xA2;              SYNCDELAY;
    EP4CFG = 0xA0;              SYNCDELAY;
    EP6CFG = 0xE2;              SYNCDELAY;
    EP8CFG = 0xE0;              SYNCDELAY;

    FIFORESET = bmNAKALL;       SYNCDELAY;
    FIFORESET = bmNAKALL | 2;   SYNCDELAY;
    FIFORESET = bmNAKALL | 4;   SYNCDELAY;
    FIFORESET = bmNAKALL | 6;   SYNCDELAY;
    FIFORESET = bmNAKALL | 8;   SYNCDELAY;
    FIFORESET = 0x00;           SYNCDELAY;

    EP2FIFOCFG = 0x05;          SYNCDELAY;
    EP4FIFOCFG = 0x05;          SYNCDELAY;
    EP6FIFOCFG = 0x05;          SYNCDELAY;
    EP8FIFOCFG = 0x05;          SYNCDELAY;

    for (i = 0; i < 8; i += 2) {
        XREG(EPAUTOINLEN + i) = 0x02;      SYNCDELAY;
        XREG(EPAUTOINLEN + i + 1) = 0x00;  SYNCDELAY;
    }

    GPIFIDLECTL = 0xFF;
    FLOWSTB = 0x20;
    FLOWSTBEDGE = 0x01;
    FLOWSTBHPERIOD = 0x02;
    EP2GPIFFLGSEL = 0x00;
    SUDPTRH = 0;
    SUDPTRL = 0;

    PORTACFG = 0x00;
    PORTCCFG = 0x00;
    PORTECFG = 0x00;
    OEA = 0;
    OEB = 0;
    OEC = 0;
    OED = 0;
    OEE = 0;
}

void main(void)
{
    EA = 0;
    CPUCS = 0x00;      /* CLKSPD=00 -> 12 MHz; the previous firmware may have left 48 MHz */
    CKCON = 0x01;      /* T0M=0 (CLKOUT/12), default 1-cycle MOVX stretch */

    USBCS = (USBCS | bmDISCON) & ~bmRENUM;
    restore_defaults();
    wait_overflows(DISCON_OVERFLOWS);
    USBCS &= ~bmDISCON;

    for (;;)
        ;
}
