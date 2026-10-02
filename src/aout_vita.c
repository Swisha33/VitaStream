/* Audioausgabe über sceAudioOut (MAIN-Port: nur 48 kHz, 16 Bit Stereo). */
#include "platform.h"

#include <psp2/audioout.h>

static int s_port = -1;

int aout_open(int rate, int channels)
{
    aout_close();
    s_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, AOUT_GRAIN, rate,
                                 channels == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
    if (s_port < 0) return -1;
    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(s_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    return 0;
}

int aout_write(const int16_t *pcm)
{
    if (s_port < 0) return -1;
    return sceAudioOutOutput(s_port, pcm);
}

void aout_close(void)
{
    if (s_port >= 0) {
        sceAudioOutOutput(s_port, NULL);   /* Restpuffer abspielen */
        sceAudioOutReleasePort(s_port);
    }
    s_port = -1;
}
