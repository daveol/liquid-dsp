//
//
//

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "liquid.internal.h"

#if HAVE_FFTW3_H
#   include <fftw3.h>
#endif

//#define DEBUG

#define FIR_FILTER(name)    LIQUID_CONCAT(fir_filter_crcf,name)

struct firpfbch_s {
    unsigned int num_channels;
    unsigned int m;
    float beta;
    float complex * x;  // time-domain buffer
    float complex * X;  // freq-domain buffer
    
    FIR_FILTER() * bank;
#if HAVE_FFTW3_H
    fftwf_plan fft;
#else
    fftplan fft;
#endif
    int nyquist;    // nyquist/root-nyquist
    int type;       // synthesis/analysis
};

firpfbch firpfbch_create(unsigned int _num_channels,
                         unsigned int _m,
                         float _beta,
                         int _nyquist,
                         int _type)
{
    firpfbch c = (firpfbch) malloc(sizeof(struct firpfbch_s));
    c->num_channels = _num_channels;
    c->m = _m;
    c->beta = _beta;
    c->nyquist = _nyquist;
    c->type = _type;

    // create bank of filters
    c->bank = (FIR_FILTER()*) malloc((c->num_channels)*sizeof(FIR_FILTER()));

    // design filter
    unsigned int h_len;

    // design filter using kaiser window and be done with it
    // TODO: use filter prototype object
    if (_m < 1) {
        printf("error: firpfbch_create(), invalid filter delay (must be greater than 1)\n");
        exit(1);
    }
    h_len = 2*(c->m)*(c->num_channels);
    float h[h_len+1];
    if (c->nyquist == FIRPFBCH_NYQUIST) {
        float fc = 1/(float)(c->num_channels);  // cutoff frequency
        fir_kaiser_window(h_len+1, fc, c->beta, h);
    } else if (c->nyquist == FIRPFBCH_ROOTNYQUIST) {
        design_rrc_filter((c->num_channels),(c->m),(c->beta),0.0f,h);
    } else {
        printf("error: firpfbch_create(), unsupported nyquist flag: %d\n", _nyquist);
        exit(1);
    }

    // generate bank of sub-samped filters
    unsigned int i, n;
    if (c->type == FIRPFBCH_SYNTHESIZER) {
        // length of each sub-sampled filter
        unsigned int h_sub_len = h_len / c->num_channels;
        float h_sub[h_sub_len];
        for (i=0; i<c->num_channels; i++) {
            for (n=0; n<h_sub_len; n++) {
                h_sub[n] = h[i + n*(c->num_channels)];
            }   
            c->bank[i] = FIR_FILTER(_create)(h_sub, h_sub_len);
        }
    } else {
        // length of each sub-sampled filter
        unsigned int h_sub_len = h_len / c->num_channels;
        float h_sub[h_sub_len+1];
        for (i=0; i<c->num_channels; i++) {
            h_sub[0] = 0.0f;    // NOTE: this additional zero is required to
                                //       align filterbank channelizer output
                                //       with traditional heterodyne channelizer
            for (n=0; n<h_sub_len; n++) {
                h_sub[n+1] = h[i + 1 + n*(c->num_channels)];
            }   
            c->bank[i] = FIR_FILTER(_create)(h_sub, h_sub_len+1);
        }
    }

#ifdef DEBUG
    for (i=0; i<h_len+1; i++)
        printf("h(%4u) = %12.4e;\n", i+1, h[i]);
#endif

    // allocate memory for buffers
    c->x = (float complex*) malloc((c->num_channels)*sizeof(float complex));
    c->X = (float complex*) malloc((c->num_channels)*sizeof(float complex));

    // create fft plan
#if HAVE_FFTW3_H
    c->fft = fftwf_plan_dft_1d(c->num_channels, c->X, c->x, FFTW_BACKWARD, FFTW_ESTIMATE);
#else
    c->fft = fft_create_plan(c->num_channels, c->X, c->x, FFT_REVERSE);
#endif

    return c;
}

void firpfbch_destroy(firpfbch _c)
{
    unsigned int i;
    for (i=0; i<_c->num_channels; i++)
        fir_filter_crcf_destroy(_c->bank[i]);
    free(_c->bank);

#if HAVE_FFTW3_H
    fftwf_destroy_plan(_c->fft);
#else
    fft_destroy_plan(_c->fft);
#endif
    free(_c->x);
    free(_c->X);
    free(_c);
}

void firpfbch_print(firpfbch _c)
{
    printf("firpfbch: [%u taps]\n", 0);
}


void firpfbch_synthesizer_execute(firpfbch _c, float complex * _x, float complex * _y)
{
    unsigned int i;

    // copy samples into ifft input buffer (_c->X)
    memmove(_c->X, _x, (_c->num_channels)*sizeof(float complex));

    // execute inverse fft, store in time-domain buffer (_c->x)
#if HAVE_FFTW3_H
    fftwf_execute(_c->fft);
#else
    fft_execute(_c->fft);
#endif

    // push samples into filter bank
    // execute filterbank, putting samples into output buffer
    for (i=0; i<_c->num_channels; i++) {
        fir_filter_crcf_push(_c->bank[i], _c->x[i]);
        fir_filter_crcf_execute(_c->bank[i], &(_y[i]));

        // invoke scaling factor
        _y[i] /= (float)(_c->num_channels);
    }
}

void firpfbch_analyzer_execute(firpfbch _c, float complex * _x, float complex * _y)
{
    unsigned int i, b;

    // push samples into filter bank
    // execute filterbank, putting samples into freq-domain buffer (_c->X) in
    // reverse order
    for (i=0; i<_c->num_channels; i++) {
        b = _c->num_channels-i-1;
        fir_filter_crcf_push(_c->bank[b], _x[i]);
        fir_filter_crcf_execute(_c->bank[b], &(_c->X[i]));
    }
    
    // execute inverse fft, store in time-domain buffer (_c->x)
#if HAVE_FFTW3_H
    fftwf_execute(_c->fft);
#else
    fft_execute(_c->fft);
#endif

    // copy results to output buffer
    memmove(_y, _c->x, (_c->num_channels)*sizeof(float complex));
}

void firpfbch_execute(firpfbch _c, float complex * _x, float complex * _y)
{
    if (_c->type == FIRPFBCH_ANALYZER)
        firpfbch_analyzer_execute(_c,_x,_y);
    else
        firpfbch_synthesizer_execute(_c,_x,_y);
}

