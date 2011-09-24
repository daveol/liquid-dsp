/*
 * Copyright (c) 2010, 2011 Joseph Gaeddert
 * Copyright (c) 2010, 2011 Virginia Polytechnic Institute & State University
 *
 * This file is part of liquid.
 *
 * liquid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * liquid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with liquid.  If not, see <http://www.gnu.org/licenses/>.
 */

//
// gmskdem.c : Gauss minimum-shift keying modem
//

#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <stdlib.h>

#include "liquid.internal.h"

#define DEBUG_GMSKDEM           1
#define DEBUG_GMSKDEM_FILENAME  "gmskdem_internal_debug.m"
#define DEBUG_BUFFER_LEN        (1000)

void gmskdem_debug_print(gmskdem _q,
                         const char * _filename);

struct gmskdem_s {
    unsigned int k;         // samples/symbol
    unsigned int m;         // symbol delay
    float BT;               // bandwidth/time product
    unsigned int h_len;     // filter length
    float * h;              // pulse shaping filter

    // filter object
    firfilt_rrrf filter;    // receiver matched filter

    float complex x_prime;  // received signal state

    // demodulated symbols counter
    unsigned int num_symbols_demod;

#if DEBUG_GMSKDEM
    windowf  debug_mfout;
#endif
};

// create gmskdem object
//  _k      :   samples/symbol
//  _m      :   filter delay (symbols)
//  _BT     :   excess bandwidth factor
gmskdem gmskdem_create(unsigned int _k,
                       unsigned int _m,
                       float _BT)
{
    if (_k < 2) {
        fprintf(stderr,"error: gmskdem_create(), samples/symbol must be at least 2\n");
        exit(1);
    } else if (_m < 1) {
        fprintf(stderr,"error: gmskdem_create(), symbol delay must be at least 1\n");
        exit(1);
    } else if (_BT <= 0.0f || _BT >= 1.0f) {
        fprintf(stderr,"error: gmskdem_create(), bandwidth/time product must be in (0,1)\n");
        exit(1);
    }

    // allocate memory for main object
    gmskdem q = (gmskdem)malloc(sizeof(struct gmskdem_s));

    // set properties
    q->k  = _k;
    q->m  = _m;
    q->BT = _BT;

    // allocate memory for filter taps
    q->h_len = 2*(q->k)*(q->m)+1;
    q->h = (float*) malloc(q->h_len * sizeof(float));

    // compute filter coefficients
    liquid_firdes_gmskrx(q->k, q->m, q->BT, 0.0f, q->h);

    // create filter object
    q->filter = firfilt_rrrf_create(q->h, q->h_len);

    // reset modem state
    gmskdem_reset(q);

#if DEBUG_GMSKDEM
    q->debug_mfout  = windowf_create(DEBUG_BUFFER_LEN);
#endif

    // return modem object
    return q;
}

void gmskdem_destroy(gmskdem _q)
{
#if DEBUG_GMSKDEM
    // print to external file
    gmskdem_debug_print(_q, DEBUG_GMSKDEM_FILENAME);

    // destroy debugging objects
    windowf_destroy(_q->debug_mfout);
#endif

    // destroy filter object
    firfilt_rrrf_destroy(_q->filter);

    // free filter array
    free(_q->h);

    // free main object memory
    free(_q);
}

void gmskdem_print(gmskdem _q)
{
    printf("gmskdem [k=%u, m=%u, BT=%8.3f]\n", _q->k, _q->m, _q->BT);
    unsigned int i;
    for (i=0; i<_q->h_len; i++)
        printf("  hr(%4u) = %12.8f;\n", i+1, _q->h[i]);
}

void gmskdem_reset(gmskdem _q)
{
    // reset phase state
    _q->x_prime = 0.0f;

    // set demod. counter to zero
    _q->num_symbols_demod = 0;

    // clear filter buffer
    firfilt_rrrf_clear(_q->filter);
}

void gmskdem_demodulate(gmskdem _q,
                        float complex * _x,
                        unsigned int * _s)
{
    // increment symbol counter
    _q->num_symbols_demod++;

    // run matched filter
    unsigned int i;
    float phi;
    float d_hat;
    for (i=0; i<_q->k; i++) {
        // compute phase difference
        phi = cargf( conjf(_q->x_prime)*_x[i] );
        _q->x_prime = _x[i];

        // run through matched filter
        firfilt_rrrf_push(_q->filter, phi);
#if DEBUG_GMSKDEM
        // compute output
        float d_tmp;
        firfilt_rrrf_execute(_q->filter, &d_tmp);
        windowf_push(_q->debug_mfout, d_tmp / _q->k);
#endif

        // decimate by k
        if ( i != 0 ) continue;

        // compute filter output
        firfilt_rrrf_execute(_q->filter, &d_hat);

        // scale result by k
        d_hat /= _q->k;
    }

    // make decision
    *_s = d_hat > 0.0f ? 1 : 0;
}

//
// output debugging file
//
void gmskdem_debug_print(gmskdem _q,
                         const char * _filename)
{
    // open output filen for writing
    FILE * fid = fopen(_filename,"w");
    if (!fid) {
        fprintf(stderr,"error: gmskdem_debug_print(), could not open '%s' for writing\n", _filename);
        exit(1);
    }
    fprintf(fid,"%% %s : auto-generated file\n", _filename);
    fprintf(fid,"clear all\n");
    fprintf(fid,"close all\n");

#if DEBUG_GMSKDEM
    //
    unsigned int i;
    float * r;
    fprintf(fid,"n = %u;\n", DEBUG_BUFFER_LEN);
    fprintf(fid,"k = %u;\n", _q->k);
    fprintf(fid,"m = %u;\n", _q->m);
    fprintf(fid,"t = [0:(n-1)]/k;\n");

    // plot receive filter response
    fprintf(fid,"ht = zeros(1,2*k*m+1);\n");
    float ht[_q->h_len];
    liquid_firdes_gmsktx(_q->k, _q->m, _q->BT, 0.0f, ht);
    for (i=0; i<_q->h_len; i++)
        fprintf(fid,"ht(%4u) = %12.4e;\n", i+1, ht[i]);
    for (i=0; i<_q->h_len; i++)
        fprintf(fid,"hr(%4u) = %12.4e;\n", i+1, _q->h[i]);
    fprintf(fid,"hc = conv(ht,hr)/k;\n");
    fprintf(fid,"nfft = 1024;\n");
    fprintf(fid,"f = [0:(nfft-1)]/nfft - 0.5;\n");
    fprintf(fid,"Ht = 20*log10(abs(fftshift(fft(ht/k, nfft))));\n");
    fprintf(fid,"Hr = 20*log10(abs(fftshift(fft(hr/k, nfft))));\n");
    fprintf(fid,"Hc = 20*log10(abs(fftshift(fft(hc/k, nfft))));\n");
    fprintf(fid,"figure;\n");
    fprintf(fid,"plot(f,Ht, f,Hr, f,Hc,'-k','LineWidth',2);\n");
    fprintf(fid,"axis([-0.5 0.5 -50 10]);\n");
    fprintf(fid,"xlabel('Normalized Frequency');\n");
    fprintf(fid,"ylabel('Power Spectral Density [dB]');\n");
    fprintf(fid,"legend('transmit','receive','composite',1);\n");
    fprintf(fid,"grid on;\n");

    fprintf(fid,"mfout = zeros(1,n);\n");
    windowf_read(_q->debug_mfout, &r);
    for (i=0; i<DEBUG_BUFFER_LEN; i++)
        fprintf(fid,"mfout(%5u) = %12.4e;\n", i+1, r[i]);
    fprintf(fid,"i0 = 1; %%mod(k+n,k)+k;\n");
    fprintf(fid,"isym = i0:k:n;\n");
    fprintf(fid,"figure;\n");
    fprintf(fid,"plot(t,mfout,'-', t(isym),mfout(isym),'o','MarkerSize',4);\n");
    fprintf(fid,"grid on;\n");
#endif

    fclose(fid);
    printf("gmskdem: internal debugging written to '%s'\n", _filename);
}
