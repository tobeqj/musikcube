//////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2007-2016 musikcube team
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//    * Redistributions of source code must retain the above copyright notice,
//      this list of conditions and the following disclaimer.
//
//    * Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//    * Neither the name of the author nor the names of other contributors may
//      be used to endorse or promote products derived from this software
//      without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
//////////////////////////////////////////////////////////////////////////////

#include "DirectSoundOut.h"

#include <core/sdk/constants.h>

#include <cassert>
#include <iostream>
#include <chrono>
#include <thread>

class DrainBuffer :
    public musik::core::sdk::IBuffer,
    public musik::core::sdk::IBufferProvider
{
    public:
        DrainBuffer() {
            this->buffer = nullptr;
            this->channels = this->samples = this->rate = 0;
        }

        virtual ~DrainBuffer() {
            delete[] buffer;
        }

        void Init() {
            delete[] buffer;
            this->buffer = new float[this->samples];
            memset(this->buffer, 0, this->Bytes());
        }

        virtual long SampleRate() const { return this->rate; }
        virtual void SetSampleRate(long sampleRate) { this->rate = sampleRate; }
        virtual int Channels() const { return this->channels; }
        virtual void SetChannels(int channels) { this->channels = channels; }
        virtual float* BufferPointer() const { return this->buffer; }
        virtual long Samples() const { return this->samples; }
        virtual void SetSamples(long samples) { this->samples = samples; }
        virtual long Bytes() const { return this->samples * sizeof(float); }
        virtual void OnBufferProcessed(IBuffer *buffer) { }
        virtual void Destroy() { delete this; }

    private:
        int channels, samples, rate;
        float *buffer;
};

#define MAX_BUFFERS_PER_OUTPUT 16
#define READ_CURSOR_INITIAL_OFFSET 128

#define BUFFER_SIZE_BYTES_PER_CHANNEL \
    (2048 * sizeof(float) * MAX_BUFFERS_PER_OUTPUT)

using Lock = std::unique_lock<std::recursive_mutex>;

inline DWORD getAvailableBytes(
    IDirectSoundBuffer *secondaryBuffer,
    DWORD writeOffset,
    DWORD bufferSize)
{
    DWORD readOffset = 0;
    DWORD writeCursor = 0;
    secondaryBuffer->GetCurrentPosition(&readOffset, &writeCursor);

    if (writeOffset == readOffset) {
        return 0;
    }

    DWORD available = 0;
    if (writeOffset > readOffset) {
        available = (bufferSize - writeOffset) + readOffset;
    }
    else {
        available = readOffset - writeOffset;
    }

    return available;
}

DirectSoundOut::DirectSoundOut()
: state(StateStopped)
, outputContext(nullptr)
, primaryBuffer(nullptr)
, secondaryBuffer(nullptr)
, bufferSize(0)
, writeOffset(0)
, latency(0)
, rate(0)
, channels(0)
, firstBufferWritten(false)
, volume(1.0f) {
    ZeroMemory(&waveFormat, sizeof(WAVEFORMATEXTENSIBLE));
}

DirectSoundOut::~DirectSoundOut() {
}

void DirectSoundOut::Destroy() {
    this->Reset();
    delete this;
}

void DirectSoundOut::Pause() {
    this->state = StatePaused;

    Lock lock(this->stateMutex);

    if (this->secondaryBuffer) {
        this->secondaryBuffer->Stop();
    }
}

void DirectSoundOut::Resume() {
    this->state = StatePlaying;

    Lock lock(this->stateMutex);

    if (this->secondaryBuffer) {
        this->secondaryBuffer->Play(0, 0, DSBPLAY_LOOPING);
    }
}

void DirectSoundOut::SetVolume(double volume) {
    Lock lock(this->stateMutex);

    this->volume = volume;

    if (this->state != StateStopped) {
        if (this->secondaryBuffer) {
            /* normalize between 1 and 10000 (DSBVOLUME_MIN) */
            double scaled = fabs(volume * 10000);
            scaled = scaled < 0 ? 1 : scaled;

            /* found on experts-exchange (20181717) */
            double db = ((log10(scaled) / 4) * 10000) - 10000;

            if (db > DSBVOLUME_MAX) {
                db = DSBVOLUME_MAX;
            }

            if (db < DSBVOLUME_MIN) {
                db = DSBVOLUME_MIN;
            }

            this->secondaryBuffer->SetVolume((LONG) db);
        }
    }
}

double DirectSoundOut::GetVolume() {
    return this->volume;
}

void DirectSoundOut::Stop() {
    Lock lock(this->stateMutex);
    this->ResetBuffers();
    this->state = StateStopped;
}

int DirectSoundOut::Play(IBuffer *buffer, IBufferProvider *provider) {
    Lock lock(this->stateMutex);

    if (this->state != StatePlaying) {
        return OutputInvalidState;
    }

    if (!this->Configure(buffer)) {
        this->Reset();
        return OutputFormatError;
    }

    unsigned char *dst1 = nullptr, *dst2 = nullptr;
    DWORD size1 = 0, size2 = 0;
    DWORD availableBytes = 0;
    DWORD bufferBytes = buffer->Bytes();

    /* annoyingly, playback starts with the read and write pointer both
    at 0, which means we don't know if the buffer is full or empty. if
    we treat it as full (safe case) it delays playback for the duration
    of the buffer size (until the cursor wraps around). so we have to
    record whether or not we've read the first buffer or not. if anyone
    knows a better way to do this please let me know! */
    if (this->firstBufferWritten) {
        availableBytes = getAvailableBytes(
            this->secondaryBuffer,
            this->writeOffset,
            this->bufferSize);

        if (bufferBytes > availableBytes && this->state == StatePlaying) {
            int samples = (bufferBytes - availableBytes) / sizeof(float) / channels;
            return ((long long)(samples * 1000) / rate) + 1;
        }

        assert(availableBytes >= bufferBytes);
    }

    HRESULT result =
        this->secondaryBuffer->Lock(
            writeOffset,
            bufferBytes,
            (void **)&dst1, &size1,
            (void **)&dst2, &size2,
            0);

    if (result == DSERR_BUFFERLOST) {
        this->secondaryBuffer->Restore();

        result = this->secondaryBuffer->Lock(
            writeOffset,
            bufferBytes,
            (void **)&dst1, &size1,
            (void **)&dst2, &size2,
            0);
    }

    if (result == DS_OK) {
        char* bufferPointer = (char *)buffer->BufferPointer();

        memcpy(dst1, bufferPointer, size1);
        if (size2 > 0) {
            memcpy(dst2, bufferPointer + size1, size2);
        }

        writeOffset += bufferBytes;
        writeOffset %= this->bufferSize;

        this->secondaryBuffer->Unlock((void *)dst1, size1, (void *)dst2, size2);
        this->firstBufferWritten = true;

        provider->OnBufferProcessed(buffer);
        return OutputBufferWritten;
    }

    return OutputBufferFull;
}

void DirectSoundOut::Drain() {
    static const int drainCount = 4;
    static const int bufferResendDelayMs = 50;

    int channels = this->channels;
    int rate = this->rate;
    int bufferSize = this->bufferSize;

    if (!channels || !rate || !bufferSize) {
        return;
    }

    int totalSamples = bufferSize / sizeof(float) / channels;

    DrainBuffer buffer;
    buffer.SetChannels(channels);
    buffer.SetSampleRate(rate);
    buffer.SetSamples(totalSamples / drainCount);
    buffer.Init();

    /* fill the rest of the buffer with silence to ensure all
    real samples get played! */
    int count = drainCount + 1;
    while (count > 0 && this->state != StateStopped) {
        if (!this->Play(&buffer, &buffer)) {
            Sleep(bufferResendDelayMs); /* eh */
        }
        else {
            --count;
        }
    }
}

void DirectSoundOut::Reset() {
    this->ResetBuffers();

    if (this->outputContext) {
        this->outputContext->Release();
        this->outputContext = nullptr;
    }
}

void DirectSoundOut::ResetBuffers() {
    ZeroMemory(&waveFormat, sizeof(WAVEFORMATEXTENSIBLE));

    this->bufferSize = 0;
    this->writeOffset = 0;
    this->firstBufferWritten = false;

    if (this->secondaryBuffer) {
        this->secondaryBuffer->Stop();
        this->secondaryBuffer->Release();
        this->secondaryBuffer = nullptr;
    }

    if (this->primaryBuffer) {
        this->primaryBuffer->Stop();
        this->primaryBuffer->Release();
        this->primaryBuffer = nullptr;
    }
}

double DirectSoundOut::Latency() {
    return (double) latency;
}

bool DirectSoundOut::Configure(IBuffer *buffer) {
    /* do a quick check up front to see if we're already in a valid state.
    if so, return immediately without changing anything else! */
    if (this->outputContext &&
        this->primaryBuffer &&
        this->secondaryBuffer &&
        this->rate == buffer->SampleRate() &&
        this->channels == buffer->Channels())
    {
        return true;
    }

    this->Stop();

    HRESULT result;

    if (!this->outputContext) {
        result = DirectSoundCreate8(nullptr, &this->outputContext, nullptr);

        if (result != DS_OK) {
            return false;
        }

        /* DSSCL_PRIORITY allows us to change the sample format */
        result = this->outputContext->
            SetCooperativeLevel(GetDesktopWindow(), DSSCL_PRIORITY);

        if (result != DS_OK) {
            return false;
        }
    }

    /* this is our "primary" buffer. we set this thing up, but end up
    writing all data to the secondary buffer. the system takes care
    of mixing the secondary buffer(s) into the primary before writing
    it back to the sound system. */
    if (this->primaryBuffer == nullptr) {
        DSBUFFERDESC bufferInfo;
        ZeroMemory(&bufferInfo, sizeof(DSBUFFERDESC));

        /* LOCHARDWARE means we want the audio hardware (not software) to
        do the mixing for us. */
        bufferInfo.dwFlags =
            DSBCAPS_PRIMARYBUFFER |
            DSBCAPS_CTRLVOLUME |
            DSBCAPS_LOCHARDWARE;

        bufferInfo.dwSize = sizeof(DSBUFFERDESC);
        bufferInfo.dwBufferBytes = 0;
        bufferInfo.lpwfxFormat = nullptr;

        result = this->outputContext->CreateSoundBuffer(
            &bufferInfo, &this->primaryBuffer, nullptr);

        if (result != DS_OK) {
            /* try again with software mixing... */
            bufferInfo.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRLVOLUME;
            result = this->outputContext->CreateSoundBuffer(
                &bufferInfo, &this->primaryBuffer, nullptr);

            if (result != DS_OK) {
                return false;
            }
        }
    }

    /* if the channels or sampling rate has changed, reset the secondary
    buffer so we can re-create it with the correct attributes */
    if (this->rate != buffer->SampleRate() ||
        this->channels != buffer->Channels())
    {
        if (this->secondaryBuffer) {
            this->secondaryBuffer->Stop();
            this->secondaryBuffer->Release();
            this->secondaryBuffer = nullptr;
        }

        this->rate = buffer->SampleRate();
        this->channels = buffer->Channels();
    }

    /* the secondary buffer is where we actually write data. the runtime
    will take care of mixing it into the buffer we register with the
    DSBCAPS_PRIMARYBUFFER (this->primaryBuffer) */
    if (this->secondaryBuffer == nullptr) {
        DSBUFFERDESC bufferInfo;
        ZeroMemory(&bufferInfo, sizeof(DSBUFFERDESC));

        bufferInfo.dwSize = sizeof(DSBUFFERDESC);

        bufferInfo.dwFlags =
            DSBCAPS_CTRLFREQUENCY |
            DSBCAPS_CTRLPAN |
            DSBCAPS_CTRLVOLUME |
            DSBCAPS_GLOBALFOCUS |
            DSBCAPS_GETCURRENTPOSITION2 |
            DSBCAPS_CTRLPOSITIONNOTIFY;

        DWORD speakerConfig = 0;
        switch (buffer->Channels()) {
            case 1:
                speakerConfig = KSAUDIO_SPEAKER_MONO;
                break;
            case 2:
                speakerConfig = KSAUDIO_SPEAKER_STEREO;
                break;
            case 4:
                speakerConfig = KSAUDIO_SPEAKER_QUAD;
                break;
            case 5:
                speakerConfig = (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT);
                break;
            case 6:
                speakerConfig = KSAUDIO_SPEAKER_5POINT1;
                break;
        }

        WAVEFORMATEXTENSIBLE &wf = this->waveFormat;
        wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE);
        wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels = (WORD) buffer->Channels();
        wf.Format.wBitsPerSample = 8 * sizeof(float);
        wf.Format.nSamplesPerSec = (DWORD) buffer->SampleRate();
        wf.Samples.wValidBitsPerSample = 8 * sizeof(float);
        wf.Format.nBlockAlign = (wf.Format.wBitsPerSample / 8) * wf.Format.nChannels;
        wf.Format.nAvgBytesPerSec = wf.Format.nSamplesPerSec * wf.Format.nBlockAlign;
        wf.dwChannelMask = speakerConfig;
        wf.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

        bufferInfo.lpwfxFormat = (WAVEFORMATEX*) &wf;
        bufferInfo.dwBufferBytes = BUFFER_SIZE_BYTES_PER_CHANNEL * buffer->Channels();

        this->bufferSize = bufferInfo.dwBufferBytes;

        IDirectSoundBuffer *tempBuffer;
        result = this->outputContext->CreateSoundBuffer(&bufferInfo, &tempBuffer, nullptr);

        if (result != DS_OK) {
            return false;
        }

        result = tempBuffer->QueryInterface(IID_IDirectSoundBuffer8, (void**) &this->secondaryBuffer);
        if (result != DS_OK) {
            tempBuffer->Release();
            return false;
        }

        tempBuffer->Release();
    }

    this->primaryBuffer->SetCurrentPosition(0);
    this->primaryBuffer->Play(0, 0, DSBPLAY_LOOPING);

    this->secondaryBuffer->SetCurrentPosition(0);
    this->secondaryBuffer->Play(0, 0, DSBPLAY_LOOPING);

    int samples = this->bufferSize / sizeof(float) / channels;
    this->latency = (float)samples / (float)rate;

    this->state = StatePlaying;
    this->SetVolume(this->volume);

    return true;
}