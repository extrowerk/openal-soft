#ifndef ALC_HRTF_H
#define ALC_HRTF_H

#include <string>

#include "AL/al.h"
#include "AL/alc.h"

#include "vector.h"
#include "almalloc.h"


#define HRTF_HISTORY_BITS   (6)
#define HRTF_HISTORY_LENGTH (1<<HRTF_HISTORY_BITS)
#define HRTF_HISTORY_MASK   (HRTF_HISTORY_LENGTH-1)

#define HRIR_BITS        (7)
#define HRIR_LENGTH      (1<<HRIR_BITS)
#define HRIR_MASK        (HRIR_LENGTH-1)


struct HrtfHandle;

struct HrtfEntry {
    RefCount ref;

    ALuint sampleRate;
    ALsizei irSize;

    ALfloat distance;
    ALubyte evCount;

    const ALubyte *azCount;
    const ALushort *evOffset;
    const ALfloat (*coeffs)[2];
    const ALubyte (*delays)[2];
};

struct EnumeratedHrtf {
    std::string name;

    HrtfHandle *hrtf;
};


struct HrtfState {
    alignas(16) ALfloat History[HRTF_HISTORY_LENGTH];
    alignas(16) ALfloat Values[HRIR_LENGTH][2];
};

struct HrtfParams {
    alignas(16) ALfloat Coeffs[HRIR_LENGTH][2];
    ALsizei Delay[2];
    ALfloat Gain;
};

struct DirectHrtfState {
    /* HRTF filter state for dry buffer content */
    ALsizei Offset{0};
    ALsizei IrSize{0};
    struct {
        alignas(16) ALfloat Values[HRIR_LENGTH][2];
        alignas(16) ALfloat Coeffs[HRIR_LENGTH][2];
    } Chan[];

    DirectHrtfState() noexcept { }

    DEF_PLACE_NEWDEL()
};

struct AngularPoint {
    ALfloat Elev;
    ALfloat Azim;
};


al::vector<EnumeratedHrtf> EnumerateHrtf(const char *devname);
HrtfEntry *GetLoadedHrtf(HrtfHandle *handle);
void Hrtf_IncRef(HrtfEntry *hrtf);
void Hrtf_DecRef(HrtfEntry *hrtf);

void GetHrtfCoeffs(const HrtfEntry *Hrtf, ALfloat elevation, ALfloat azimuth, ALfloat spread, ALfloat (*RESTRICT coeffs)[2], ALsizei *delays);

/**
 * Produces HRTF filter coefficients for decoding B-Format, given a set of
 * virtual speaker positions, a matching decoding matrix, and per-order high-
 * frequency gains for the decoder. The calculated impulse responses are
 * ordered and scaled according to the matrix input. Note the specified virtual
 * positions should be in degrees, not radians!
 */
void BuildBFormatHrtf(const HrtfEntry *Hrtf, DirectHrtfState *state, const ALsizei NumChannels, const AngularPoint *AmbiPoints, const ALfloat (*RESTRICT AmbiMatrix)[MAX_AMBI_COEFFS], const ALsizei AmbiCount, const ALfloat *RESTRICT AmbiOrderHFGain);

#endif /* ALC_HRTF_H */
