# PSP Audio APIs

> Generated from `external/pspsdk` headers + samples. Verify signatures against the header before use.

## Overview

PSP audio is layered. From lowest to highest level:

- **Raw channel output — `sceAudio*`** ([pspaudio.h](external/pspsdk/src/audio/pspaudio.h)). You reserve a hardware channel, hand it a finished 16‑bit PCM buffer, and call an output function. There are three families: the classic per‑channel API (`sceAudioCh*` / `sceAudioOutput*`, up to 8 channels, fixed 44.1 kHz), the single output‑2 path (`sceAudioOutput2*`), and a sample‑rate‑converting path (`sceAudioSRC*`) that lets you feed PCM at an arbitrary frequency. Use this when you already have PCM and want full control. This is the layer the SAS and MP3 samples sit on top of.
- **Callback mixer helper — `pspAudio*`** ([pspaudiolib.h](external/pspsdk/src/audio/pspaudiolib.h)). A thin SDK‑side library built on `sceAudio` that spawns output threads and calls a per‑channel callback whenever a buffer needs filling. Use this when you want "give me a function that fills a stereo buffer" and don't want to manage channels/threads yourself (the wavegen and polyphonic samples).
- **Voice/synth engine — `sceSasCore`** ([pspsascore.h](external/pspsdk/src/sascore/pspsascore.h)). A hardware software‑mixer that mixes up to 32 mono voices (VAG ADPCM, raw PCM, or generated waveforms) with per‑voice ADSR envelopes, pitch and reverb, into one stereo buffer. You still output that buffer through `sceAudio`. Use this for game‑style polyphonic SFX/instrument playback.
- **Hardware codecs** — decode compressed audio to PCM, which you then output via `sceAudio`:
  - `sceAtrac*` ([pspatrac3.h](external/pspsdk/src/atrac3/pspatrac3.h)) — ATRAC3 / ATRAC3plus.
  - `sceMp3*` ([pspmp3.h](external/pspsdk/src/mp3/pspmp3.h)) — MP3 (streaming decode).
  - `sceAudiocodec*` ([pspaudiocodec.h](external/pspsdk/src/audio/pspaudiocodec.h)) — low‑level codec interface for AT3+/AT3/MP3/AAC.
- **`sceVaudio*`** ([pspvaudio.h](external/pspsdk/src/vaudio/pspvaudio.h)) — a single virtual output channel with built‑in reverb‑style effect presets and an ALC (automatic level control / normalizer). Like `sceAudioSRC` it accepts a configurable sample rate and mono/stereo, plus effect/ALC settings.

Note on this repo: **wavezbg does not use any audio APIs** — it is a VSH/XMB background‑colour plugin. This file is reference documentation only.

## Key APIs

### `sceAudio` — [pspaudio.h](external/pspsdk/src/audio/pspaudio.h)

Constants:

```c
#define PSP_AUDIO_VOLUME_MAX    0x8000   /* maximum output volume */
#define PSP_AUDIO_CHANNEL_MAX   8        /* number of hardware channels */
#define PSP_AUDIO_NEXT_CHANNEL  (-1)     /* request first free channel */

#define PSP_AUDIO_SAMPLE_MIN    64       /* min samples per channel */
#define PSP_AUDIO_SAMPLE_MAX    65472    /* max samples per channel */
#define PSP_AUDIO_SAMPLE_ALIGN(s) (((s) + 63) & ~63)   /* round up to multiple of 64 */

enum PspAudioFormats {
    PSP_AUDIO_FORMAT_STEREO = 0,
    PSP_AUDIO_FORMAT_MONO   = 0x10
};
```

Error codes (returned as negative values): `SCE_AUDIO_ERROR_NOT_INITIALIZED`, `SCE_AUDIO_ERROR_OUTPUT_BUSY`, `SCE_AUDIO_ERROR_INVALID_CH`, `SCE_AUDIO_ERROR_PRIV_REQUIRED`, `SCE_AUDIO_ERROR_NOT_FOUND`, `SCE_AUDIO_ERROR_INVALID_SIZE`, `SCE_AUDIO_ERROR_INVALID_FORMAT`, `SCE_AUDIO_ERROR_NOT_RESERVED`, `SCE_AUDIO_ERROR_NOT_OUTPUT`, `SCE_AUDIO_ERROR_INVALID_FREQUENCY`, `SCE_AUDIO_ERROR_INVALID_VOLUME`, `SCE_AUDIO_ERROR_INPUT_BUSY` (values `0x80260001`–`0x80260010`).

Per‑channel output:

```c
int sceAudioChReserve(int channel, int samplecount, int format);   /* returns channel number, or <0 error */
int sceAudioChRelease(int channel);

int sceAudioOutput(int channel, int vol, void *buf);                /* non-blocking; 0 on success */
int sceAudioOutputBlocking(int channel, int vol, void *buf);        /* blocking; returns queued sample count */
int sceAudioOutputPanned(int channel, int leftvol, int rightvol, void *buf);
int sceAudioOutputPannedBlocking(int channel, int leftvol, int rightvol, void *buf);

int sceAudioGetChannelRestLen(int channel);                        /* unplayed samples remaining */
int sceAudioGetChannelRestLength(int channel);                     /* same; alternate name */
int sceAudioSetChannelDataLen(int channel, int samplecount);       /* change sample count after reserve */
int sceAudioChangeChannelConfig(int channel, int format);          /* one of PspAudioFormats */
int sceAudioChangeChannelVolume(int channel, int leftvol, int rightvol);
```

`sceAudioChReserve`'s `samplecount` must be between `PSP_AUDIO_SAMPLE_MIN` and `PSP_AUDIO_SAMPLE_MAX` and aligned to 64 (use `PSP_AUDIO_SAMPLE_ALIGN`). Output buffers are interleaved 16‑bit signed PCM: stereo = `{short L, short R}` per sample (so `samplecount * 4` bytes), mono = one `short` per sample.

Output‑2 path (single reserved output, sample count 17–4111):

```c
int sceAudioOutput2Reserve(int samplecount);
int sceAudioOutput2Release(void);
int sceAudioOutput2ChangeLength(int samplecount);
int sceAudioOutput2OutputBlocking(int vol, void *buf);
int sceAudioOutput2GetRestSample(void);
```

Sample‑rate‑converting path (configurable frequency — used by the MP3 sample):

```c
int sceAudioSRCChReserve(int samplecount, int freq, int channels);  /* freq: 8000..48000; channels: pass 2 */
int sceAudioSRCChRelease(void);
int sceAudioSRCOutputBlocking(int vol, void *buf);                   /* returns queued sample count */
```

Audio input (microphone) is also present: `sceAudioInputInit`, `sceAudioInputInitEx` (with `pspAudioInputParams`), `sceAudioInputBlocking`, `sceAudioInput`, `sceAudioGetInputLength`, `sceAudioWaitInputEnd`, `sceAudioPollInputEnd`.

### `pspaudiolib` — [pspaudiolib.h](external/pspsdk/src/audio/pspaudiolib.h)

```c
#define PSP_NUM_AUDIO_CHANNELS 4
#define PSP_NUM_AUDIO_SAMPLES  1024   /* frames per callback (1 frame = 1 sample mono / 2 stereo) */
#define PSP_VOLUME_MAX         0x8000

typedef void (*pspAudioCallback_t)(void *buf, unsigned int reqn, void *pdata);

int  pspAudioInit();
void pspAudioEndPre();
void pspAudioEnd();

void pspAudioSetVolume(int channel, int left, int right);
void pspAudioGetVolume(int channel, int *left, int *right);
void pspAudioSetChannelCallback(int channel, pspAudioCallback_t callback, void *pdata);
void pspAudioGetChannelCallback(int channel, pspAudioCallback_t *callback, void **pdata);
int  pspAudioOutBlocking(unsigned int channel, unsigned int vol1, unsigned int vol2, void *buf);
```

The callback receives `buf` (the stereo 16‑bit buffer to fill), `reqn` (number of frames requested — `PSP_NUM_AUDIO_SAMPLES`), and the user `pdata` pointer. There is also a `psp_audio_channelinfo` struct and a `pspAudioThreadfunc_t` typedef in the header.

### `sceSasCore` — [pspsascore.h](external/pspsdk/src/sascore/pspsascore.h)

Constants (selected):

```c
#define PSP_SAS_GRAIN_SIZE        (256)    /* recommended grain (frames processed per __sceSasCore call) */
#define PSP_SAS_GRAIN_SIZE_MIN    (64)
#define PSP_SAS_GRAIN_SIZE_MAX    (2048)   /* grain must be a multiple of 64 */
#define PSP_SAS_VOICES_MAX        (32)
#define PSP_SAS_SAMPLE_RATE       (44100)  /* the only valid sample rate */
#define PSP_SAS_VOLUME_MAX        (0x1000) /* NOTE: different from PSP_AUDIO_VOLUME_MAX */
#define PSP_SAS_PITCH_BASE        (0x1000) /* 1x pitch */
#define PSP_SAS_PITCH_MIN         (0x1)
#define PSP_SAS_PITCH_MAX         (0x4000)
#define PSP_SAS_ENVELOPE_HEIGHT_MAX (0x40000000)
#define PSP_SAS_ENVELOPE_FREQ_MAX   (0x7FFFFFFF)

#define PSP_SAS_GET_VOICE_BIT(voice)        (1u << (voice % 32))
#define PSP_SAS_GET_FLAG_AT(flags, voice)   (((flags) & PSP_SAS_GET_VOICE_BIT(voice)) != 0)
```

Enums: `PspSasOutputModes` (`PSP_SAS_OUTPUTMODE_STEREO = 0`, `PSP_SAS_OUTPUTMODE_MULTICHANNEL = 1`), `PspSasEffectTypes` (`OFF = -1`, `ROOM`, `SMALL`, `MEDIUM`, `LARGE`, `HALL`, `SPACE`, `ECHO`, `DELAY`, `PIPE`), `PspSasADSRCurveModes`, ADSR flags `PSP_SAS_ADSR_ATTACK/DECAY/SUSTAIN/RELEASE` and `PSP_SAS_ADSR_EVERYTHING`. The opaque state is `SceSasCore` (a 3616‑byte struct that **must be 64‑byte aligned**).

Init / global config:

```c
int  __sceSasInit(SceSasCore* core, int grainsize, int maxvoices, PspSasOutputModes outputmode, int samplerate);
int  __sceSasGetOutputmode(SceSasCore* core);
int  __sceSasSetOutputmode(SceSasCore* core, PspSasOutputModes outputmode);
int  __sceSasGetGrain(SceSasCore* core);
int  __sceSasSetGrain(SceSasCore* core, int grainsize);
```

Reverb / effect bus: `__sceSasRevType`, `__sceSasRevEVOL`, `__sceSasRevVON`, `__sceSasRevParam`.

Voice source:

```c
int __sceSasSetVoice(SceSasCore* core, int voice, void* vag, int size, int loop);              /* VAG ADPCM */
int __sceSasSetVoicePCM(SceSasCore* core, int voice, void* pcm, int samplecount, int loopstart); /* mono S16LE */
int __sceSasSetNoise(SceSasCore* core, int voice, int freq);
int __sceSasSetTrianglarWave(SceSasCore* core, int voice, int unk);   /* (sic) */
int __sceSasSetTriangularWave(SceSasCore* core, int voice, int unk);  /* alias of the above */
int __sceSasSetSteepWave(SceSasCore* core, int voice, int duty);      /* square wave, duty 0..100 */
```

Voice control / envelope / pitch / volume:

```c
int __sceSasSetKeyOn(SceSasCore* core, int voice);
int __sceSasSetKeyOff(SceSasCore* core, int voice);
int __sceSasSetPitch(SceSasCore* core, int voice, int pitch);
int __sceSasSetADSRmode(SceSasCore* core, int voice, u32 mask,
                        PspSasADSRCurveModes attackcurve, PspSasADSRCurveModes decaycurve,
                        PspSasADSRCurveModes sustaincurve, PspSasADSRCurveModes releasecurve);
int __sceSasSetADSR(SceSasCore* core, int voice, u32 mask,
                    int attackrate, int decayrate, int sustainrate, int releaserate);
int __sceSasSetSimpleADSR(SceSasCore* core, int voice, u32 envelope1, u32 envelope2);
int __sceSasSetSL(SceSasCore* core, int voice, int level);
int __sceSasSetVolume(SceSasCore* core, int voice, int leftvol, int rightvol, int sendleftvol, int sendrightvol);
int __sceSasSetPause(SceSasCore* core, u32 voicebit, int pause);
```

Render (the heart of the engine) and status:

```c
int __sceSasCore(SceSasCore* core, void* dst);                          /* dst = grainsize*4*sizeof(int16_t) bytes */
int __sceSasCoreWithMix(SceSasCore* core, void* dst, int leftvol, int rightvol); /* mixes into dst, doesn't clear it */
u32 __sceSasGetEndFlag(SceSasCore* core);          /* bit N set => voice N ended (refreshed after __sceSasCore) */
int __sceSasGetPauseFlag(SceSasCore* core);
int __sceSasGetEnvelopeHeight(SceSasCore* core, int voice);
int __sceSasGetAllEnvelopeHeights(SceSasCore* core, int heights[32]);
```

ATRAC3‑as‑a‑voice (declared, struct not yet exposed): `__sceSasSetVoiceATRAC3`, `__sceSasConcatenateATRAC3`, `__sceSasUnsetATRAC3`.

Per the header, `__sceSasInit` requires `PSP_MODULE_AV_AVCODEC` **and** `PSP_MODULE_AV_SASCORE` to be loaded first (via `sceUtilityLoadModule`).

### `sceAudiocodec` — [pspaudiocodec.h](external/pspsdk/src/audio/pspaudiocodec.h)

```c
#define PSP_CODEC_AT3PLUS (0x00001000)
#define PSP_CODEC_AT3     (0x00001001)
#define PSP_CODEC_MP3     (0x00001002)
#define PSP_CODEC_AAC     (0x00001003)

int sceAudiocodecCheckNeedMem(unsigned long *Buffer, int Type);
int sceAudiocodecInit(unsigned long *Buffer, int Type);
int sceAudiocodecDecode(unsigned long *Buffer, int Type);
int sceAudiocodecGetEDRAM(unsigned long *Buffer, int Type);
int sceAudiocodecReleaseEDRAM(unsigned long *Buffer);
```

`Buffer` is a codec context array; `Type` is one of the `PSP_CODEC_*` IDs. (The header provides only the prototypes — no usage sample is present in the SDK tree.)

### `sceAtrac3` — [pspatrac3.h](external/pspsdk/src/atrac3/pspatrac3.h)

Codec IDs `PSP_ATRAC_AT3PLUS (0x1000)` / `PSP_ATRAC_AT3 (0x1001)`; many `PSP_ATRAC_ERROR_*` codes; `PspBufferInfo` struct. Primary flow:

```c
int sceAtracSetDataAndGetID(void *buf, SceSize bufsize);   /* buf includes RIFF/WAVE header; returns atracID */
int sceAtracDecodeData(int atracID, u16 *outSamples, int *outN, int *outEnd, int *outRemainFrame);
int sceAtracGetRemainFrame(int atracID, int *outRemainFrame);
int sceAtracGetStreamDataInfo(int atracID, u8** writePointer, u32* availableBytes, u32* readOffset);
int sceAtracAddStreamData(int atracID, unsigned int bytesToAdd);
int sceAtracSetLoopNum(int atracID, int nloops);
int sceAtracReleaseAtracID(int atracID);
```

Supporting queries: `sceAtracGetAtracID`, `sceAtracGetBitrate`, `sceAtracGetNextSample`, `sceAtracGetMaxSample`, `sceAtracGetChannel`, `sceAtracGetSoundSample`, `sceAtracGetLoopStatus`, `sceAtracGetNextDecodePosition`, `sceAtracResetPlayPosition`, plus the half‑way/second‑buffer setters (`sceAtracSetData`, `sceAtracSetHalfwayBuffer`, `sceAtracSetHalfwayBufferAndGetID`, `sceAtracSetSecondBuffer`, `sceAtracGetSecondBufferInfo`, `sceAtracGetBufferInfoForReseting`, `sceAtracGetInternalErrorInfo`).

### `sceMp3` — [pspmp3.h](external/pspsdk/src/mp3/pspmp3.h)

Streaming decode driven by `SceMp3InitArg` (caller supplies `mp3Buf` ≥ 8192 bytes and `pcmBuf` ≥ 9216 bytes). Primary flow:

```c
SceInt32 sceMp3InitResource();
SceInt32 sceMp3ReserveMp3Handle(SceMp3InitArg* args);   /* returns handle */
SceInt32 sceMp3Init(SceInt32 handle);
SceInt32 sceMp3GetInfoToAddStreamData(SceInt32 handle, SceUChar8** dst, SceInt32* towrite, SceInt32* srcpos);
SceInt32 sceMp3NotifyAddStreamData(SceInt32 handle, SceInt32 size);
SceInt32 sceMp3CheckStreamDataNeeded(SceInt32 handle);  /* >0 => feed more */
SceInt32 sceMp3Decode(SceInt32 handle, SceShort16** dst); /* returns bytes decoded into *dst */
SceInt32 sceMp3ReleaseMp3Handle(SceInt32 handle);
SceInt32 sceMp3TermResource();
```

Queries / control: `sceMp3GetSamplingRate`, `sceMp3GetBitRate`, `sceMp3GetMp3ChannelNum`, `sceMp3GetMaxOutputSample`, `sceMp3GetSumDecodedSample`, `sceMp3GetFrameNum`, `sceMp3GetMPEGVersion`, `sceMp3GetLoopNum`/`sceMp3SetLoopNum`, `sceMp3ResetPlayPosition`/`sceMp3ResetPlayPositionByFrame`. Low‑level path: `sceMp3LowLevelInit`, `sceMp3LowLevelDecode`. Requires `PSP_MODULE_AV_AVCODEC` and `PSP_MODULE_AV_MP3` loaded.

### `sceVaudio` — [pspvaudio.h](external/pspsdk/src/vaudio/pspvaudio.h)

```c
#define PSP_VAUDIO_VOLUME_MAX    0x8000
#define PSP_VAUDIO_SAMPLE_MAX    2048
#define PSP_VAUDIO_SAMPLE_MIN    256
#define PSP_VAUDIO_FORMAT_MONO   1
#define PSP_VAUDIO_FORMAT_STEREO 2
/* effects: PSP_VAUDIO_EFFECT_OFF/HEAVY/POPS/JAZZ/UNIQUE/MAX
   ALC:     PSP_VAUDIO_ALC_OFF/MODE1/MODE_MAX */

int sceVaudioChReserve(int samplecount, int frequency, int format);  /* samplecount one of 256,576,1024,1152,2048 */
int sceVaudioChRelease(void);
int sceVaudioOutputBlocking(int volume, void *buffer);               /* volume 0..PSP_VAUDIO_VOLUME_MAX */
int sceVaudioSetEffectType(int effect, int volume);
int sceVaudioSetAlcMode(int mode);
```

## Correct usage patterns

### 1. Reserve a channel → fill PCM → blocking output loop

This is the classic `sceAudio` pattern (the SAS sample uses exactly this skeleton — see [samples/sascore/main.c](external/pspsdk/src/samples/sascore/main.c) lines 177–274). Key rules: align the sample count to 64, lay out interleaved L/R `short`s, and keep volume ≤ `PSP_AUDIO_VOLUME_MAX`.

```c
#include <pspaudio.h>

#define FRAMES 1024  /* already a multiple of 64; else use PSP_AUDIO_SAMPLE_ALIGN(FRAMES) */

typedef struct { short l, r; } sample_t;
static sample_t buffer[FRAMES];

int channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, FRAMES, PSP_AUDIO_FORMAT_STEREO);
if (channel < 0) { /* handle SCE_AUDIO_ERROR_* */ }

while (running) {
    fill_pcm(buffer, FRAMES);                 /* your synthesis/decode fills interleaved S16 */
    sceAudioOutputBlocking(channel, PSP_AUDIO_VOLUME_MAX, buffer); /* blocks until queued */
}

sceAudioChRelease(channel);
```

`sceAudioOutputBlocking` returns the number of queued samples (handy for tracking play time, as the MP3 sample does with `sceAudioSRCOutputBlocking`). Each output call plays exactly the reserved sample count.

### 2. The `pspaudiolib` callback model

Let the library own the threads; you just fill the buffer each tick. From [samples/audio/wavegen/main.c](external/pspsdk/src/samples/audio/wavegen/main.c) (lines 100–124, 171–177):

```c
#include <pspaudiolib.h>

typedef struct { short l, r; } sample_t;

/* Must match pspAudioCallback_t exactly: void(void* buf, unsigned int reqn, void* pdata). */
void audioCallback(void* buf, unsigned int reqn, void* pdata) {
    sample_t* out = (sample_t*) buf;
    for (unsigned int i = 0; i < reqn; i++) {   /* reqn == PSP_NUM_AUDIO_SAMPLES (1024) */
        short s = next_sample();
        out[i].l = s;
        out[i].r = s;
    }
}

int main(void) {
    pspAudioInit();
    pspAudioSetChannelCallback(0, audioCallback, NULL);
    /* main thread does other work; callback runs on the library's output thread */
}
```

For multiple voices, set a callback per channel (channels `0 .. PSP_NUM_AUDIO_CHANNELS-1`) and optionally `pspAudioSetVolume(channel, left, right)` — see [samples/audio/polyphonic/main.c](external/pspsdk/src/samples/audio/polyphonic/main.c) lines 466–470. (That sample wraps a custom callback signature in adapters whose signature matches `pspAudioCallback_t`; keep your callback matching the typedef.)

### 3. SAS setup → core render loop

From [samples/sascore/main.c](external/pspsdk/src/samples/sascore/main.c). The `SceSasCore` must be 64‑byte aligned and the mixer buffer is `grainsize * 4` `int16_t`s. Output is still done via `sceAudio`.

```c
#include <pspsascore.h>
#include <psputility.h>
#include <pspaudio.h>

__attribute__((aligned(64))) SceSasCore core;
__attribute__((aligned(64))) int16_t    mixer[PSP_SAS_GRAIN_SIZE * 4];

/* 1. Load required modules, then init the core. */
sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
sceUtilityLoadModule(PSP_MODULE_AV_SASCORE);
__sceSasInit(&core, PSP_SAS_GRAIN_SIZE, PSP_SAS_VOICES_MAX,
             PSP_SAS_OUTPUTMODE_STEREO, PSP_SAS_SAMPLE_RATE);

int channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, PSP_SAS_GRAIN_SIZE, PSP_AUDIO_FORMAT_STEREO);

/* 2. Configure a voice (mono S16 PCM here) and trigger it. */
__sceSasSetVoicePCM(&core, voice, pcm, sample_count, /*loopstart*/ -1);
__sceSasSetPitch(&core, voice, (clip_rate * PSP_SAS_PITCH_BASE) / PSP_SAS_SAMPLE_RATE);
__sceSasSetADSRmode(&core, voice, PSP_SAS_ADSR_ATTACK | PSP_SAS_ADSR_RELEASE,
                    PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE, 0, 0, PSP_SAS_ADSR_CURVE_MODE_DIRECT);
__sceSasSetADSR(&core, voice, PSP_SAS_ADSR_ATTACK | PSP_SAS_ADSR_RELEASE,
                PSP_SAS_ENVELOPE_FREQ_MAX, 0, 0, 0);
__sceSasSetVolume(&core, voice, PSP_SAS_VOLUME_MAX, PSP_SAS_VOLUME_MAX,
                  PSP_SAS_VOLUME_MAX, PSP_SAS_VOLUME_MAX);   /* note PSP_SAS_VOLUME_MAX == 0x1000 */
__sceSasSetKeyOn(&core, voice);

/* 3. Render + output loop. */
while (running) {
    __sceSasCore(&core, mixer);                 /* mixes all voices into `mixer` */
    sceAudioOutputPannedBlocking(channel, PSP_AUDIO_VOLUME_MAX, PSP_AUDIO_VOLUME_MAX, mixer);
}
```

To find a free voice, read `__sceSasGetEndFlag(&core)` and test bits with `PSP_SAS_GET_FLAG_AT` (an ended voice's bit is set); the flags only refresh **after** `__sceSasCore`.

## Common mistakes

- **Sample count not on the 64‑frame grid / out of range.** `sceAudioChReserve`'s `samplecount` must satisfy `PSP_AUDIO_SAMPLE_MIN (64) ≤ n ≤ PSP_AUDIO_SAMPLE_MAX (65472)` **and** be a multiple of 64 — wrap it in `PSP_AUDIO_SAMPLE_ALIGN(n)`. A non‑aligned or out‑of‑range value yields `SCE_AUDIO_ERROR_INVALID_SIZE`. The same idea applies elsewhere: SAS grain must be 64..2048 and a multiple of 64 (`PSP_SAS_GRAIN_SIZE_MIN`/`_MAX`); `sceVaudioChReserve` only accepts 256/576/1024/1152/2048; output‑2/SRC want 17..4111.
- **Volume above the max.** `vol` must be `0 .. PSP_AUDIO_VOLUME_MAX (0x8000)`; exceeding it returns `SCE_AUDIO_ERROR_INVALID_VOLUME`. Watch the scale mismatch: **`PSP_SAS_VOLUME_MAX` is `0x1000`, not `0x8000`** — passing an `sceAudio`‑scale volume into `__sceSasSetVolume`/`__sceSasRevEVOL` clips or errors (the SAS sample header explicitly warns about this).
- **Outputting before reserving (or after release).** Calling `sceAudioOutput*` on a channel that was never reserved returns `SCE_AUDIO_ERROR_NOT_RESERVED` / `SCE_AUDIO_ERROR_INVALID_CH`. Reserve once, output many, release once — don't reserve per frame.
- **Wrong buffer size / layout.** Stereo output is interleaved `{short L, short R}` per frame, so a stereo buffer is `samplecount * 4` bytes; SAS expects a `grainsize * 4 * sizeof(int16_t)` destination. Undersizing the buffer corrupts memory. SAS PCM voices must be **mono, signed 16‑bit little‑endian** (`__sceSasSetVoicePCM`) — stereo clips won't play (the sample bails when `channels != 1`).
- **Mixing blocking and non‑blocking output on the same channel.** Issuing a non‑blocking `sceAudioOutput` while a previous buffer is still playing returns `SCE_AUDIO_ERROR_OUTPUT_BUSY`; interleaving it with the blocking variant causes dropouts/glitches. Pick one model per channel. With `pspaudiolib`, don't also drive the same underlying channel by hand.
- **Calling blocking output from the wrong context.** `sceAudioOutputBlocking` parks the caller until the buffer is queued; calling it from a time‑critical/high‑priority thread (or a VSH/main thread that must stay responsive) stalls everything. Do audio output on a dedicated worker (this is exactly what `pspaudiolib` does for you).
- **Not releasing channels / handles.** Always pair `sceAudioChReserve`↔`sceAudioChRelease`, `sceAudioSRCChReserve`↔`sceAudioSRCChRelease`, `sceMp3ReserveMp3Handle`↔`sceMp3ReleaseMp3Handle` (and `sceMp3InitResource`↔`sceMp3TermResource`), and `sceAtracSetDataAndGetID`↔`sceAtracReleaseAtracID`. There are only 8 hardware channels (`PSP_AUDIO_CHANNEL_MAX`); leaking them eventually makes reserve fail.
- **Forgetting the AV modules for SAS/MP3.** `__sceSasInit` needs `PSP_MODULE_AV_AVCODEC` + `PSP_MODULE_AV_SASCORE`; `sceMp3*` needs `PSP_MODULE_AV_AVCODEC` + `PSP_MODULE_AV_MP3`. Without them, init returns a negative error. There is **no** SAS "close" function — just stop using the core (the sample notes this).

## Sample references

- [external/pspsdk/src/audio/pspaudio.h](external/pspsdk/src/audio/pspaudio.h) — low‑level `sceAudio` channel output, format/volume/sample constants, error codes.
- [external/pspsdk/src/audio/pspaudiolib.h](external/pspsdk/src/audio/pspaudiolib.h) — `pspaudiolib` callback mixer helper and its typedef/constants.
- [external/pspsdk/src/audio/pspaudiocodec.h](external/pspsdk/src/audio/pspaudiocodec.h) — `sceAudiocodec` low‑level codec interface (AT3+/AT3/MP3/AAC).
- [external/pspsdk/src/atrac3/pspatrac3.h](external/pspsdk/src/atrac3/pspatrac3.h) — `sceAtrac` ATRAC3/ATRAC3plus decoding.
- [external/pspsdk/src/mp3/pspmp3.h](external/pspsdk/src/mp3/pspmp3.h) — `sceMp3` streaming MP3 decoder.
- [external/pspsdk/src/vaudio/pspvaudio.h](external/pspsdk/src/vaudio/pspvaudio.h) — `sceVaudio` virtual channel with effect presets + ALC.
- [external/pspsdk/src/sascore/pspsascore.h](external/pspsdk/src/sascore/pspsascore.h) — `sceSasCore` voice/synth software mixer.
- [external/pspsdk/src/samples/audio/wavegen/main.c](external/pspsdk/src/samples/audio/wavegen/main.c) — minimal `pspaudiolib` single‑channel callback (sine/square/triangle generator).
- [external/pspsdk/src/samples/audio/polyphonic/main.c](external/pspsdk/src/samples/audio/polyphonic/main.c) — two `pspaudiolib` channels with per‑channel callbacks and `pspAudioSetVolume`.
- [external/pspsdk/src/samples/sascore/main.c](external/pspsdk/src/samples/sascore/main.c) — full SAS setup (module load → `__sceSasInit` → voice config → `__sceSasCore` → `sceAudioOutputPannedBlocking`), PCM + VAG voices, reverb.
- [external/pspsdk/src/samples/mp3/main.c](external/pspsdk/src/samples/mp3/main.c) — full `sceMp3` streaming decode feeding `sceAudioSRCChReserve`/`sceAudioSRCOutputBlocking`.
