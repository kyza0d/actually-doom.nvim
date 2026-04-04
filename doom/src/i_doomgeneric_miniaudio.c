// miniaudio sound support (and maybe music soon) for actually-doom.

#ifdef FEATURE_SOUND

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "miniaudio.h"

#include "i_sound.h"
#include "m_misc.h"
#include "s_sound.h"
#include "w_wad.h"
#include "z_zone.h"

static boolean use_sfx_prefix;

static ma_engine engine;
static ma_sound *channels;
// An uninit must be matched by exactly one prior init; this tracks that.
static boolean *channel_inited;
static ma_audio_buffer_ref *channel_bufs;

struct CachedSamples {
    uint8_t *samples;
    unsigned count;
    unsigned rate;
};

static boolean I_Miniaudio_SoundInit(boolean use_sfx_prefix_)
{
    printf("I_Miniaudio_SoundInit: %s\n", ma_version_string());

    ma_engine_config config = ma_engine_config_init();
    config.sampleRate = snd_samplerate;
    config.periodSizeInMilliseconds = snd_maxslicetime_ms;
    config.channels = 2;

    ma_result result = ma_engine_init(&config, &engine);
    if (result != MA_SUCCESS) {
        fprintf(stderr,
                "I_Miniaudio_SoundInit: Failed to initialize engine: %s\n",
                ma_result_description(result));
        return false;
    }

    channel_bufs = Z_Malloc(snd_channels * sizeof channel_bufs[0], PU_STATIC,
                            NULL);
    for (int i = 0; i < snd_channels; ++i) {
        result = ma_audio_buffer_ref_init(ma_format_u8, 1, NULL, 0,
                                          &channel_bufs[i]);
        assert(result == MA_SUCCESS);
    }

    channels = Z_Malloc(snd_channels * sizeof channels[0], PU_STATIC, NULL);
    channel_inited = Z_Malloc(snd_channels * sizeof channel_inited[0],
                              PU_STATIC, NULL);
    for (int i = 0; i < snd_channels; ++i)
        channel_inited[i] = false;

    use_sfx_prefix = use_sfx_prefix_;
    return true;
}

static void I_Miniaudio_SoundShutdown(void)
{
    for (int i = 0; i < snd_channels; ++i)
        if (channel_inited[i])
            ma_sound_uninit(&channels[i]);
    Z_Free(channels);
    Z_Free(channel_inited);

    for (int i = 0; i < snd_channels; ++i)
        ma_audio_buffer_ref_uninit(&channel_bufs[i]);
    Z_Free(channel_bufs);

    ma_engine_uninit(&engine);
}

static int I_Miniaudio_GetSfxLumpNum(sfxinfo_t *sfxinfo)
{
    if (sfxinfo->link != NULL)
        sfxinfo = sfxinfo->link;

    // Doom uses a DS prefix; Heretic and Hexen don't.
    char namebuf[16];
    M_snprintf(namebuf, sizeof namebuf,
               use_sfx_prefix ? "ds%s" : "%s", sfxinfo->name);

    return W_CheckNumForName(namebuf);
}

static void I_Miniaudio_SoundUpdate(void)
{
    // Don't need to do anything here.
}

static struct CachedSamples *I_Miniaudio_CacheSound(sfxinfo_t *sfxinfo)
{
    if (sfxinfo->driver_data != NULL)
        return sfxinfo->driver_data;

    uint8_t *const lump = W_CacheLumpNum(sfxinfo->lumpnum, PU_STATIC);
    const int lump_len = W_LumpLength(sfxinfo->lumpnum);

    // Sound lumps use the DMX format. First byte (format number) must be 3.
    // Header totals 8 bytes before padding. Padding totals 32 bytes.
    if (lump_len < 8 + 32 || lump[0] != 3) {
        fprintf(stderr,
                "I_Miniaudio_CacheSound: sound lump \"%s\" has bad format\n",
                sfxinfo->name);

        W_ReleaseLumpNum(sfxinfo->lumpnum);
        return NULL;
    }

    const unsigned sample_rate = (unsigned)lump[2] | (lump[3] << 8);
    const unsigned sample_count_with_pad =
        (unsigned)lump[4] | (lump[5] << 8) | (lump[6] << 16) | (lump[7] << 24);

    if ((unsigned)lump_len - 8 < sample_count_with_pad) {
        fprintf(stderr,
                "I_Miniaudio_CacheSound: sound lump \"%s\" has bad sample "
                "count - expected: %u, actual: %u\n",
                sfxinfo->name, sample_count_with_pad, (unsigned)lump_len - 8);

        W_ReleaseLumpNum(sfxinfo->lumpnum);
        return NULL;
    }

    // Don't release the lump; we reference its allocation for its samples.
    struct CachedSamples *const cached = Z_Malloc(sizeof *cached, PU_STATIC,
                                                  NULL);
    cached->samples = lump + 8 + 16;
    cached->count = sample_count_with_pad - 32;
    cached->rate = sample_rate;
    sfxinfo->driver_data = cached;
    return cached;
}

static boolean I_Miniaudio_SoundIsPlaying(int channel)
{
    return channel_inited[channel] && ma_sound_is_playing(&channels[channel]);
}

static void I_Miniaudio_UpdateSoundParams(int channel, int vol, int sep)
{
    if (channel_inited[channel]) {
        ma_sound_set_volume(&channels[channel], vol / 127.0f);
        ma_sound_set_pan(&channels[channel], (sep - NORM_SEP) / 127.0f);
    }
}

static int I_Miniaudio_StartSound(sfxinfo_t *sfxinfo, int channel, int vol,
                                  int sep)
{
    const struct CachedSamples *const cached = I_Miniaudio_CacheSound(sfxinfo);
    if (cached == NULL)
        return -1;

    // Must reinit the ma_sound to get it to pick up the new sample rate.
    boolean *const sound_inited = &channel_inited[channel];
    ma_sound *const sound = &channels[channel];
    ma_audio_buffer_ref *const buf = &channel_bufs[channel];

    if (*sound_inited) {
        ma_sound_uninit(sound);
        *sound_inited = false;
    }

    ma_result result = ma_audio_buffer_ref_set_data(buf, cached->samples,
                                                    cached->count);
    assert(result == MA_SUCCESS);
    buf->sampleRate = cached->rate; // No way to change this otherwise.

    // Pitching isn't used, despite sfxinfo having it as a field (original DOOM
    // would randomly vary SFX pitch, but DG seems to have stripped that code
    // out). Can re-implement it, but for now just disable miniaudio's support
    // for pitching as an optimization. We also don't use 3D spatialization
    // (DOOM instead does stereo panning), so disable that too.
    result = ma_sound_init_from_data_source(&engine, buf,
            MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION, NULL,
            sound);
    if (result != MA_SUCCESS) {
        fprintf(stderr,
                "I_Miniaudio_StartSound: Failed to initialize sound for "
                "channel %d, lump \"%s\": %s\n",
                channel, sfxinfo->name, ma_result_description(result));
        return -1;
    }

    *sound_inited = true;
    result = ma_sound_start(sound);
    assert(result == MA_SUCCESS);

    I_Miniaudio_UpdateSoundParams(channel, vol, sep);
    return channel;
}

static void I_Miniaudio_StopSound(int channel)
{
    if (channel_inited[channel])
        ma_sound_stop(&channels[channel]);
}

static void I_Miniaudio_CacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    printf("I_Miniaudio_CacheSounds: Caching %d sound(s)...\n", num_sounds);

    for (int i = 0; i < num_sounds; ++i) {
        sfxinfo_t *const sfxinfo = &sounds[i];
        sfxinfo->lumpnum = I_Miniaudio_GetSfxLumpNum(sfxinfo);
        if (sfxinfo->lumpnum >= 0)
            I_Miniaudio_CacheSound(sfxinfo);
    }
}

static snddevice_t sound_devices[] = {
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module = {
    .sound_devices = sound_devices,
    .num_sound_devices = arrlen(sound_devices),
    .Init = I_Miniaudio_SoundInit,
    .Shutdown = I_Miniaudio_SoundShutdown,
    .GetSfxLumpNum = I_Miniaudio_GetSfxLumpNum,
    .Update = I_Miniaudio_SoundUpdate,
    .UpdateSoundParams = I_Miniaudio_UpdateSoundParams,
    .StartSound = I_Miniaudio_StartSound,
    .StopSound = I_Miniaudio_StopSound,
    .SoundIsPlaying = I_Miniaudio_SoundIsPlaying,
    .CacheSounds = I_Miniaudio_CacheSounds,
};

static boolean I_Miniaudio_MusicInit(void)
{
    // TODO: support music!
    return false;
}

static void I_Miniaudio_MusicShutdown(void)
{
    // TODO: support music!
}

static void I_Miniaudio_SetMusicVolume(int volume)
{
    // TODO: support music!
    (void)volume;
}

static void I_Miniaudio_PauseMusic(void)
{
    // TODO: support music!
}

static void I_Miniaudio_ResumeMusic(void)
{
    // TODO: support music!
}

static void *I_Miniaudio_RegisterSong(void *data, int len)
{
    // TODO: support music!
    (void)data;
    (void)len;
    return NULL;
}

static void I_Miniaudio_UnRegisterSong(void *handle)
{
    // TODO: support music!
    (void)handle;
}

static void I_Miniaudio_PlaySong(void *handle, boolean looping)
{
    // TODO: support music!
    (void)handle;
    (void)looping;
}

static void I_Miniaudio_StopSong(void)
{
    // TODO: support music!
}

static boolean I_Miniaudio_MusicIsPlaying(void)
{
    // TODO: support music!
    return false;
}

static void I_Miniaudio_MusicPoll(void)
{
    // TODO: support music!
}

music_module_t DG_music_module = {
    .sound_devices = NULL,
    .num_sound_devices = 0,
    .Init = I_Miniaudio_MusicInit,
    .Shutdown = I_Miniaudio_MusicShutdown,
    .SetMusicVolume = I_Miniaudio_SetMusicVolume,
    .PauseMusic = I_Miniaudio_PauseMusic,
    .ResumeMusic = I_Miniaudio_ResumeMusic,
    .RegisterSong = I_Miniaudio_RegisterSong,
    .UnRegisterSong = I_Miniaudio_UnRegisterSong,
    .PlaySong = I_Miniaudio_PlaySong,
    .StopSong = I_Miniaudio_StopSong,
    .MusicIsPlaying = I_Miniaudio_MusicIsPlaying,
    .Poll = I_Miniaudio_MusicPoll,
};

#endif // FEATURE_SOUND
