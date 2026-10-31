/* The two sound-chip controls FroggySoundTuner uses, written out here rather than taken from the
 * device maker's SDK headers. They are ioctls on /dev/sndC0i2so (the SF3500's sound driver):
 * SET_EQ_ONOFF switches the 10-band equalizer on or off, and SET_EQ_BAND sets one band's
 * gain (about 0.1 dB per unit; bands 0 to 9 are 32 Hz to 16 kHz). The numbers were checked
 * against a device (0x20000818 and 0x80100819 on this MIPS build). */
#ifndef SNDAPI_H
#define SNDAPI_H

#include <sys/ioctl.h>

#define SND_IOCBASE 8

struct snd_eq_band_setting {
    int band;      /* 0 to 9 */
    int cutoff;    /* centre frequency in Hz */
    int q;         /* 14 is about Q 1.4 */
    int gain;      /* raw units, about 0.1 dB each */
};

#define SND_IOCTL_SET_EQ_ONOFF _IO(SND_IOCBASE, 24)
#define SND_IOCTL_SET_EQ_BAND _IOW(SND_IOCBASE, 25, struct snd_eq_band_setting)

#endif
