#ifndef VS_PLAYER_H
#define VS_PLAYER_H

#include <stdint.h>

/* Wiedergabe: MP4 / HLS / MPEG-TS über den eigenen Netzwerkstack (AdBlock, DNS, Header),
 * Demuxing mit FFmpeg, Video über den Hardware-Decoder (sceAvcdec), Ton über sceAudioOut. */

int  player_open(const char *url, const char *headers);
void player_close(void);
int  player_active(void);

/* Pro Frame aufrufen, zwischen vita2d_start_drawing() und vita2d_end_drawing(). */
void player_draw(void);

void player_toggle_pause(void);
int  player_paused(void);
void player_seek_rel(int seconds);
uint64_t player_position_ms(void);
uint64_t player_duration_ms(void);

const char *player_error(void);
void player_toggle_debug(void);   /* Technische Infos ein/aus */

#endif
