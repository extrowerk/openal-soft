/**
 * OpenAL cross platform audio library
 * Copyright (C) 2018 by Raul Herraiz.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include <math.h>
#include <stdlib.h>

#include <algorithm>

#include "alMain.h"
#include "alcontext.h"
#include "alAuxEffectSlot.h"
#include "alError.h"
#include "alu.h"
#include "filters/biquad.h"
#include "vecmat.h"

#define MIN_FREQ 20.0f
#define MAX_FREQ 2500.0f
#define Q_FACTOR 5.0f

struct ALautowahState final : public EffectState {
    /* Effect parameters */
    ALfloat mAttackRate;
    ALfloat mReleaseRate;
    ALfloat mResonanceGain;
    ALfloat mPeakGain;
    ALfloat mFreqMinNorm;
    ALfloat mBandwidthNorm;
    ALfloat mEnvDelay;

    /* Filter components derived from the envelope. */
    struct {
        ALfloat cos_w0;
        ALfloat alpha;
    } mEnv[BUFFERSIZE];

    struct {
        /* Effect filters' history. */
        struct {
            ALfloat z1, z2;
        } Filter;

        /* Effect gains for each output channel */
        ALfloat CurrentGains[MAX_OUTPUT_CHANNELS];
        ALfloat TargetGains[MAX_OUTPUT_CHANNELS];
    } mChans[MAX_EFFECT_CHANNELS];

    /* Effects buffers */
    alignas(16) ALfloat mBufferOut[BUFFERSIZE];


    ALboolean deviceUpdate(const ALCdevice *device) override;
    void update(const ALCcontext *context, const ALeffectslot *slot, const ALeffectProps *props, const EffectTarget target) override;
    void process(ALsizei samplesToDo, const ALfloat (*RESTRICT samplesIn)[BUFFERSIZE], ALfloat (*RESTRICT samplesOut)[BUFFERSIZE], ALsizei numChannels) override;

    DEF_NEWDEL(ALautowahState)
};

ALboolean ALautowahState::deviceUpdate(const ALCdevice *UNUSED(device))
{
    /* (Re-)initializing parameters and clear the buffers. */

    mAttackRate    = 1.0f;
    mReleaseRate   = 1.0f;
    mResonanceGain = 10.0f;
    mPeakGain      = 4.5f;
    mFreqMinNorm   = 4.5e-4f;
    mBandwidthNorm = 0.05f;
    mEnvDelay      = 0.0f;

    for(auto &e : mEnv)
    {
        e.cos_w0 = 0.0f;
        e.alpha = 0.0f;
    }

    for(auto &chan : mChans)
    {
        std::fill(std::begin(chan.CurrentGains), std::end(chan.CurrentGains), 0.0f);
        chan.Filter.z1 = 0.0f;
        chan.Filter.z2 = 0.0f;
    }

    return AL_TRUE;
}

void ALautowahState::update(const ALCcontext *context, const ALeffectslot *slot, const ALeffectProps *props, const EffectTarget target)
{
    const ALCdevice *device = context->Device;
    ALfloat ReleaseTime;
    ALsizei i;

    ReleaseTime = clampf(props->Autowah.ReleaseTime, 0.001f, 1.0f);

    mAttackRate    = expf(-1.0f / (props->Autowah.AttackTime*device->Frequency));
    mReleaseRate   = expf(-1.0f / (ReleaseTime*device->Frequency));
    /* 0-20dB Resonance Peak gain */
    mResonanceGain = sqrtf(log10f(props->Autowah.Resonance)*10.0f / 3.0f);
    mPeakGain      = 1.0f - log10f(props->Autowah.PeakGain/AL_AUTOWAH_MAX_PEAK_GAIN);
    mFreqMinNorm   = MIN_FREQ / device->Frequency;
    mBandwidthNorm = (MAX_FREQ-MIN_FREQ) / device->Frequency;

    mOutBuffer = target.FOAOut->Buffer;
    mOutChannels = target.FOAOut->NumChannels;
    for(i = 0;i < MAX_EFFECT_CHANNELS;i++)
        ComputePanGains(target.FOAOut, alu::Matrix::Identity()[i].data(), slot->Params.Gain,
                        mChans[i].TargetGains);
}

void ALautowahState::process(ALsizei SamplesToDo, const ALfloat (*RESTRICT SamplesIn)[BUFFERSIZE], ALfloat (*RESTRICT SamplesOut)[BUFFERSIZE], ALsizei NumChannels)
{
    const ALfloat attack_rate = mAttackRate;
    const ALfloat release_rate = mReleaseRate;
    const ALfloat res_gain = mResonanceGain;
    const ALfloat peak_gain = mPeakGain;
    const ALfloat freq_min = mFreqMinNorm;
    const ALfloat bandwidth = mBandwidthNorm;
    ALfloat env_delay;
    ALsizei c, i;

    env_delay = mEnvDelay;
    for(i = 0;i < SamplesToDo;i++)
    {
        ALfloat w0, sample, a;

        /* Envelope follower described on the book: Audio Effects, Theory,
         * Implementation and Application.
         */
        sample = peak_gain * fabsf(SamplesIn[0][i]);
        a = (sample > env_delay) ? attack_rate : release_rate;
        env_delay = lerp(sample, env_delay, a);

        /* Calculate the cos and alpha components for this sample's filter. */
        w0 = minf((bandwidth*env_delay + freq_min), 0.46f) * F_TAU;
        mEnv[i].cos_w0 = cosf(w0);
        mEnv[i].alpha = sinf(w0)/(2.0f * Q_FACTOR);
    }
    mEnvDelay = env_delay;

    for(c = 0;c < MAX_EFFECT_CHANNELS; c++)
    {
        /* This effectively inlines BiquadFilter_setParams for a peaking
         * filter and BiquadFilter_processC. The alpha and cosine components
         * for the filter coefficients were previously calculated with the
         * envelope. Because the filter changes for each sample, the
         * coefficients are transient and don't need to be held.
         */
        ALfloat z1 = mChans[c].Filter.z1;
        ALfloat z2 = mChans[c].Filter.z2;

        for(i = 0;i < SamplesToDo;i++)
        {
            const ALfloat alpha = mEnv[i].alpha;
            const ALfloat cos_w0 = mEnv[i].cos_w0;
            ALfloat input, output;
            ALfloat a[3], b[3];

            b[0] =  1.0f + alpha*res_gain;
            b[1] = -2.0f * cos_w0;
            b[2] =  1.0f - alpha*res_gain;
            a[0] =  1.0f + alpha/res_gain;
            a[1] = -2.0f * cos_w0;
            a[2] =  1.0f - alpha/res_gain;

            input = SamplesIn[c][i];
            output = input*(b[0]/a[0]) + z1;
            z1 = input*(b[1]/a[0]) - output*(a[1]/a[0]) + z2;
            z2 = input*(b[2]/a[0]) - output*(a[2]/a[0]);
            mBufferOut[i] = output;
        }
        mChans[c].Filter.z1 = z1;
        mChans[c].Filter.z2 = z2;

        /* Now, mix the processed sound data to the output. */
        MixSamples(mBufferOut, NumChannels, SamplesOut, mChans[c].CurrentGains,
                   mChans[c].TargetGains, SamplesToDo, 0, SamplesToDo);
    }
}

struct AutowahStateFactory final : public EffectStateFactory {
    EffectState *create() override;
};

EffectState *AutowahStateFactory::create()
{ return new ALautowahState{}; }

EffectStateFactory *AutowahStateFactory_getFactory(void)
{
    static AutowahStateFactory AutowahFactory{};
    return &AutowahFactory;
}


void ALautowah_setParamf(ALeffect *effect, ALCcontext *context, ALenum param, ALfloat val)
{
    ALeffectProps *props = &effect->Props;
    switch(param)
    {
        case AL_AUTOWAH_ATTACK_TIME:
            if(!(val >= AL_AUTOWAH_MIN_ATTACK_TIME && val <= AL_AUTOWAH_MAX_ATTACK_TIME))
                SETERR_RETURN(context, AL_INVALID_VALUE,,"Autowah attack time out of range");
            props->Autowah.AttackTime = val;
            break;

        case AL_AUTOWAH_RELEASE_TIME:
            if(!(val >= AL_AUTOWAH_MIN_RELEASE_TIME && val <= AL_AUTOWAH_MAX_RELEASE_TIME))
                SETERR_RETURN(context, AL_INVALID_VALUE,,"Autowah release time out of range");
            props->Autowah.ReleaseTime = val;
            break;

        case AL_AUTOWAH_RESONANCE:
            if(!(val >= AL_AUTOWAH_MIN_RESONANCE && val <= AL_AUTOWAH_MAX_RESONANCE))
                SETERR_RETURN(context, AL_INVALID_VALUE,,"Autowah resonance out of range");
            props->Autowah.Resonance = val;
            break;

        case AL_AUTOWAH_PEAK_GAIN:
            if(!(val >= AL_AUTOWAH_MIN_PEAK_GAIN && val <= AL_AUTOWAH_MAX_PEAK_GAIN))
                SETERR_RETURN(context, AL_INVALID_VALUE,,"Autowah peak gain out of range");
            props->Autowah.PeakGain = val;
            break;

        default:
            alSetError(context, AL_INVALID_ENUM, "Invalid autowah float property 0x%04x", param);
    }
}

void ALautowah_setParamfv(ALeffect *effect, ALCcontext *context, ALenum param, const ALfloat *vals)
{
    ALautowah_setParamf(effect, context, param, vals[0]);
}

void ALautowah_setParami(ALeffect *UNUSED(effect), ALCcontext *context, ALenum param, ALint UNUSED(val))
{
    alSetError(context, AL_INVALID_ENUM, "Invalid autowah integer property 0x%04x", param);
}

void ALautowah_setParamiv(ALeffect *UNUSED(effect), ALCcontext *context, ALenum param, const ALint *UNUSED(vals))
{
    alSetError(context, AL_INVALID_ENUM, "Invalid autowah integer vector property 0x%04x", param);
}

void ALautowah_getParami(const ALeffect *UNUSED(effect), ALCcontext *context, ALenum param, ALint *UNUSED(val))
{
    alSetError(context, AL_INVALID_ENUM, "Invalid autowah integer property 0x%04x", param);
}
void ALautowah_getParamiv(const ALeffect *UNUSED(effect), ALCcontext *context, ALenum param, ALint *UNUSED(vals))
{
    alSetError(context, AL_INVALID_ENUM, "Invalid autowah integer vector property 0x%04x", param);
}

void ALautowah_getParamf(const ALeffect *effect, ALCcontext *context, ALenum param, ALfloat *val)
{

    const ALeffectProps *props = &effect->Props;
    switch(param)
    {
        case AL_AUTOWAH_ATTACK_TIME:
            *val = props->Autowah.AttackTime;
            break;

        case AL_AUTOWAH_RELEASE_TIME:
            *val = props->Autowah.ReleaseTime;
            break;

        case AL_AUTOWAH_RESONANCE:
            *val = props->Autowah.Resonance;
            break;

        case AL_AUTOWAH_PEAK_GAIN:
            *val = props->Autowah.PeakGain;
            break;

        default:
            alSetError(context, AL_INVALID_ENUM, "Invalid autowah float property 0x%04x", param);
    }

}

void ALautowah_getParamfv(const ALeffect *effect, ALCcontext *context, ALenum param, ALfloat *vals)
{
    ALautowah_getParamf(effect, context, param, vals);
}

DEFINE_ALEFFECT_VTABLE(ALautowah);
