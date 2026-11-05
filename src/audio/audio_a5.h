#ifndef AUDIO_AUDIOA5_H
#define AUDIO_AUDIOA5_H

#include "types.h"
#include "../table/sound.h"

#if __cplusplus
extern "C" {
#endif

extern void AudioA5_Init(void);
extern void AudioA5_Uninit(void);

extern void AudioA5_InitInternalMusic(const MusicInfo *mid);
extern void AudioA5_InitExternalMusic(const MusicInfo *ext);
extern void AudioA5_SetMusicVolume(float volume);
extern void AudioA5_StopMusic(void);
extern void AudioA5_PollMusic(void);
extern bool AudioA5_MusicIsPlaying(void);
extern void AudioA5_PlaySoundEffect(enum SoundID effectID);

extern void AudioA5_StoreSample(enum SampleID sampleID, uint8 file_index, uint32 file_size);
extern bool AudioA5_PlaySample(enum SampleID sampleID, float volume, float pan);
extern bool AudioA5_PlaySampleRaw(enum SampleID sampleID, float volume, float pan, int idx_start, int idx_end);
extern bool AudioA5_PollNarrator(void);

enum RadioKind {
	RADIO_SELECT,
	RADIO_ACKNOWLEDGE,
	RADIO_NUM_KINDS
};

extern void AudioA5_LoadRadioVoices(const char *dataDir);
extern int AudioA5_RadioVoiceCount(void);
extern bool AudioA5_PlayRadio(int voiceSeed, enum RadioKind kind, float volume);

extern void AudioA5_LoadWind(const char *dataDir);
extern void AudioA5_SetWind(float volume);

#if __cplusplus
}
#endif

#endif
