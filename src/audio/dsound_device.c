/**
 * Xbox DirectSound → XAudio2 Compatibility Layer
 *
 * Implements the Xbox DirectSound interfaces using XAudio2.
 * The game uses 40 DirectSound entry points for audio playback
 * including 3D positioned sounds, streaming, and voice management.
 *
 * Architecture:
 * - XAudio2 mastering voice for output
 * - Each DirectSound buffer maps to an XAudio2 source voice
 * - 3D audio emulated using XAudio2 X3DAudio
 * - WMA decoding handled separately (only 2 calls)
 */

#include "dsound_xbox.h"
#include <stdio.h>
#include <string.h>

/* ================================================================
 * Stub implementation
 *
 * All methods are stubbed - audio can be implemented incrementally
 * once the game is running with graphics.
 * ================================================================ */

static HRESULT __stdcall ds_QueryInterface(IDirectSound8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall ds_AddRef(IDirectSound8 *self)
{
    (void)self;
    return 1;
}

static ULONG __stdcall ds_Release(IDirectSound8 *self)
{
    (void)self;
    return 0;
}

static HRESULT __stdcall ds_CreateSoundBuffer(IDirectSound8 *self, const DSBUFFERDESC *desc, IDirectSoundBuffer8 **ppBuffer, void *pUnkOuter)
{
    (void)self; (void)desc; (void)pUnkOuter;
    /* TODO: create XAudio2 source voice */
    if (ppBuffer) *ppBuffer = NULL;
    return E_NOTIMPL;
}

static HRESULT __stdcall ds_CreateSoundStream(IDirectSound8 *self, const DSSTREAMDESC *desc, IDirectSoundStream **ppStream, void *pUnkOuter)
{
    (void)self; (void)desc; (void)ppStream; (void)pUnkOuter;
    return E_NOTIMPL;
}

static HRESULT __stdcall ds_SetMixBinHeadroom(IDirectSound8 *self, DWORD dwMixBin, DWORD dwHeadroom)
{
    (void)self; (void)dwMixBin; (void)dwHeadroom;
    return S_OK;
}

static HRESULT __stdcall ds_SetPosition(IDirectSound8 *self, float x, float y, float z, DWORD dwApply)
{
    (void)self; (void)x; (void)y; (void)z; (void)dwApply;
    return S_OK;
}

static HRESULT __stdcall ds_SetVelocity(IDirectSound8 *self, float x, float y, float z, DWORD dwApply)
{
    (void)self; (void)x; (void)y; (void)z; (void)dwApply;
    return S_OK;
}

static HRESULT __stdcall ds_SetDistanceFactor(IDirectSound8 *self, float f, DWORD dwApply)
{
    (void)self; (void)f; (void)dwApply;
    return S_OK;
}

static HRESULT __stdcall ds_SetRolloffFactor(IDirectSound8 *self, float f, DWORD dwApply)
{
    (void)self; (void)f; (void)dwApply;
    return S_OK;
}

static HRESULT __stdcall ds_SetDopplerFactor(IDirectSound8 *self, float f, DWORD dwApply)
{
    (void)self; (void)f; (void)dwApply;
    return S_OK;
}

static HRESULT __stdcall ds_SetOrientation(IDirectSound8 *self, float xf, float yf, float zf, float xt, float yt, float zt, DWORD dwApply)
{
    (void)self; (void)xf; (void)yf; (void)zf; (void)xt; (void)yt; (void)zt; (void)dwApply;
    return S_OK;
}

static HRESULT __stdcall ds_CommitDeferredSettings(IDirectSound8 *self)
{
    (void)self;
    return S_OK;
}

static const IDirectSound8Vtbl g_ds_vtbl = {
    ds_QueryInterface,
    ds_AddRef,
    ds_Release,
    ds_CreateSoundBuffer,
    ds_CreateSoundStream,
    ds_SetMixBinHeadroom,
    ds_SetPosition,
    ds_SetVelocity,
    ds_SetDistanceFactor,
    ds_SetRolloffFactor,
    ds_SetDopplerFactor,
    ds_SetOrientation,
    ds_CommitDeferredSettings,
};

static IDirectSound8 g_dsound = { &g_ds_vtbl };

HRESULT xbox_DirectSoundCreate(void *pGuid, IDirectSound8 **ppDS, void *pUnkOuter)
{
    (void)pGuid; (void)pUnkOuter;
    if (!ppDS) return E_INVALIDARG;
    /* TODO: initialize XAudio2 */
    *ppDS = &g_dsound;
    return S_OK;
}
