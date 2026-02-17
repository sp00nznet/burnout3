/**
 * Xbox DirectSound Compatibility Layer - Type Definitions
 *
 * Defines the Xbox DirectSound types and interface structures
 * used by the game's audio system. The Xbox DirectSound API is
 * similar to PC DirectSound but includes Xbox-specific features:
 * - 3D audio with hardware HRTF
 * - Submix voices (mixbins)
 * - WMA decoding integration
 * - I3DL2 environmental reverb
 *
 * The game uses 40 DirectSound entry points with 68 total calls.
 * This layer translates those calls to XAudio2.
 */

#ifndef BURNOUT3_DSOUND_XBOX_H
#define BURNOUT3_DSOUND_XBOX_H

#include <stdint.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Forward declarations
 * ================================================================ */

typedef struct IDirectSound8         IDirectSound8;
typedef struct IDirectSoundBuffer8   IDirectSoundBuffer8;
typedef struct IDirectSoundStream    IDirectSoundStream;

/* ================================================================
 * Xbox DirectSound types
 * ================================================================ */

typedef struct DSBUFFERDESC {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwBufferBytes;
    DWORD dwReserved;
    void *lpwfxFormat;  /* WAVEFORMATEX* */
    DWORD dwMixBinMask; /* Xbox-specific */
    DWORD dwInputMixBin;
} DSBUFFERDESC;

typedef struct DSSTREAMDESC {
    DWORD dwFlags;
    DWORD dwMaxAttachedPackets;
    void *lpwfxFormat;
    void *lpfnCallback;
    void *lpvContext;
    DWORD dwMixBinMask;
} DSSTREAMDESC;

typedef struct DSMIXBINS {
    DWORD dwMixBinCount;
    void *lpMixBinVolumePairs;
} DSMIXBINS;

typedef struct DS3DBUFFER {
    DWORD dwSize;
    float vPosition[3];
    float vVelocity[3];
    DWORD dwInsideConeAngle;
    DWORD dwOutsideConeAngle;
    float vConeOrientation[3];
    LONG  lConeOutsideVolume;
    float flMinDistance;
    float flMaxDistance;
    DWORD dwMode;
} DS3DBUFFER;

typedef struct DS3DLISTENER {
    DWORD dwSize;
    float vPosition[3];
    float vVelocity[3];
    float vOrientFront[3];
    float vOrientTop[3];
    float flDistanceFactor;
    float flRolloffFactor;
    float flDopplerFactor;
} DS3DLISTENER;

/* Standard WAVEFORMATEX (simplified) */
typedef struct XBOX_WAVEFORMATEX {
    WORD  wFormatTag;
    WORD  nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD  nBlockAlign;
    WORD  wBitsPerSample;
    WORD  cbSize;
} XBOX_WAVEFORMATEX;

/* ================================================================
 * DirectSound flags
 * ================================================================ */

#define DSBCAPS_CTRL3D          0x00000010
#define DSBCAPS_CTRLFREQUENCY   0x00000020
#define DSBCAPS_CTRLVOLUME      0x00000080
#define DSBCAPS_CTRLPOSITIONNOTIFY 0x00000100
#define DSBCAPS_LOCDEFER        0x00040000

#define DSBPLAY_LOOPING         0x00000001

#define DSBSTATUS_PLAYING       0x00000001
#define DSBSTATUS_BUFFERLOST    0x00000002
#define DSBSTATUS_LOOPING       0x00000004

/* ================================================================
 * IDirectSound8 interface
 * ================================================================ */

typedef struct IDirectSound8Vtbl {
    /* IUnknown */
    HRESULT (__stdcall *QueryInterface)(IDirectSound8 *self, const IID *riid, void **ppv);
    ULONG   (__stdcall *AddRef)(IDirectSound8 *self);
    ULONG   (__stdcall *Release)(IDirectSound8 *self);

    /* IDirectSound */
    HRESULT (__stdcall *CreateSoundBuffer)(IDirectSound8 *self, const DSBUFFERDESC *desc, IDirectSoundBuffer8 **ppBuffer, void *pUnkOuter);
    HRESULT (__stdcall *CreateSoundStream)(IDirectSound8 *self, const DSSTREAMDESC *desc, IDirectSoundStream **ppStream, void *pUnkOuter);
    HRESULT (__stdcall *SetMixBinHeadroom)(IDirectSound8 *self, DWORD dwMixBin, DWORD dwHeadroom);
    HRESULT (__stdcall *SetPosition)(IDirectSound8 *self, float x, float y, float z, DWORD dwApply);
    HRESULT (__stdcall *SetVelocity)(IDirectSound8 *self, float x, float y, float z, DWORD dwApply);
    HRESULT (__stdcall *SetDistanceFactor)(IDirectSound8 *self, float flDistanceFactor, DWORD dwApply);
    HRESULT (__stdcall *SetRolloffFactor)(IDirectSound8 *self, float flRolloffFactor, DWORD dwApply);
    HRESULT (__stdcall *SetDopplerFactor)(IDirectSound8 *self, float flDopplerFactor, DWORD dwApply);
    HRESULT (__stdcall *SetOrientation)(IDirectSound8 *self, float xFront, float yFront, float zFront, float xTop, float yTop, float zTop, DWORD dwApply);
    HRESULT (__stdcall *CommitDeferredSettings)(IDirectSound8 *self);
} IDirectSound8Vtbl;

struct IDirectSound8 {
    const IDirectSound8Vtbl *lpVtbl;
};

/* ================================================================
 * IDirectSoundBuffer8 interface
 * ================================================================ */

typedef struct IDirectSoundBuffer8Vtbl {
    /* IUnknown */
    HRESULT (__stdcall *QueryInterface)(IDirectSoundBuffer8 *self, const IID *riid, void **ppv);
    ULONG   (__stdcall *AddRef)(IDirectSoundBuffer8 *self);
    ULONG   (__stdcall *Release)(IDirectSoundBuffer8 *self);

    /* IDirectSoundBuffer */
    HRESULT (__stdcall *SetBufferData)(IDirectSoundBuffer8 *self, const void *pvBufferData, DWORD dwBufferBytes);
    HRESULT (__stdcall *SetPlayRegion)(IDirectSoundBuffer8 *self, DWORD dwPlayStart, DWORD dwPlayLength);
    HRESULT (__stdcall *Lock)(IDirectSoundBuffer8 *self, DWORD dwOffset, DWORD dwBytes, void **ppvAudioPtr1, DWORD *pdwAudioBytes1, void **ppvAudioPtr2, DWORD *pdwAudioBytes2, DWORD dwFlags);
    HRESULT (__stdcall *Unlock)(IDirectSoundBuffer8 *self, void *pvAudioPtr1, DWORD dwAudioBytes1, void *pvAudioPtr2, DWORD dwAudioBytes2);
    HRESULT (__stdcall *SetCurrentPosition)(IDirectSoundBuffer8 *self, DWORD dwNewPosition);
    HRESULT (__stdcall *GetCurrentPosition)(IDirectSoundBuffer8 *self, DWORD *pdwCurrentPlayCursor, DWORD *pdwCurrentWriteCursor);
    HRESULT (__stdcall *GetStatus)(IDirectSoundBuffer8 *self, DWORD *pdwStatus);
    HRESULT (__stdcall *Play)(IDirectSoundBuffer8 *self, DWORD dwReserved1, DWORD dwReserved2, DWORD dwFlags);
    HRESULT (__stdcall *Stop)(IDirectSoundBuffer8 *self);
    HRESULT (__stdcall *SetVolume)(IDirectSoundBuffer8 *self, LONG lVolume);
    HRESULT (__stdcall *SetFrequency)(IDirectSoundBuffer8 *self, DWORD dwFrequency);
    HRESULT (__stdcall *SetMaxDistance)(IDirectSoundBuffer8 *self, float flMaxDistance, DWORD dwApply);
    HRESULT (__stdcall *SetMinDistance)(IDirectSoundBuffer8 *self, float flMinDistance, DWORD dwApply);
    HRESULT (__stdcall *SetPosition)(IDirectSoundBuffer8 *self, float x, float y, float z, DWORD dwApply);
    HRESULT (__stdcall *SetVelocity)(IDirectSoundBuffer8 *self, float x, float y, float z, DWORD dwApply);
    HRESULT (__stdcall *SetConeAngles)(IDirectSoundBuffer8 *self, DWORD dwInside, DWORD dwOutside, DWORD dwApply);
    HRESULT (__stdcall *SetConeOutsideVolume)(IDirectSoundBuffer8 *self, LONG lConeOutsideVolume, DWORD dwApply);
    HRESULT (__stdcall *SetMixBins)(IDirectSoundBuffer8 *self, const DSMIXBINS *pMixBins);
} IDirectSoundBuffer8Vtbl;

struct IDirectSoundBuffer8 {
    const IDirectSoundBuffer8Vtbl *lpVtbl;
};

/* ================================================================
 * Public API
 * ================================================================ */

/**
 * Create the DirectSound-compatible interface backed by XAudio2.
 * Replaces DirectSoundCreate().
 */
HRESULT xbox_DirectSoundCreate(void *pGuid, IDirectSound8 **ppDS, void *pUnkOuter);

#ifdef __cplusplus
}
#endif

#endif /* BURNOUT3_DSOUND_XBOX_H */
