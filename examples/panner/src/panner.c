/*
 Copyright 2017-2018 Leo McCormack
 
 Permission to use, copy, modify, and/or distribute this software for any purpose with or
 without fee is hereby granted, provided that the above copyright notice and this permission
 notice appear in all copies.
 
 THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO
 THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT
 SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR
 ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE
 OR PERFORMANCE OF THIS SOFTWARE.
*/
/*
 * Filename:
 *     panner.c
 * Description:
 *     A frequency-dependent 3D panner, based on the Vector-base Amplitude Panning (VBAP)
 *     method. Depending on the room, it may be beneficial to utilise amplitude-normalised
 *     gains for low frequencies, and energy-normalised gains for high frequencies; which
 *     this implemenation takes into account with one parameter "DTT". Set "DTT" to 0 for a
 *     normal room, 0.5 for listening room, and 1 for anechoic.
 * Dependencies:
 *     saf_utilities, saf_vbap, afSTFTlib
 * Author, date created:
 *     Leo McCormack, 25.09.2017
 */
 
#include "panner_internal.h"

static inline float matlab_fmodf(float x, float y) {
    float tmp = fmodf(x, y);
    return tmp >= 0 ? tmp : tmp + y;
}

void panner_create
(
    void ** const phPan
)
{
    panner_data* pData = (panner_data*)malloc(sizeof(panner_data));
    if (pData == NULL) { return;/*error*/ }
    *phPan = (void*)pData;
    int t, ch, dummy;
    
    /* time-frequency transform + buffers */
    pData->hSTFT = NULL;
    pData->STFTInputFrameTF = (complexVector**)malloc2d(TIME_SLOTS, MAX_NUM_INPUTS, sizeof(complexVector));
    pData->STFTOutputFrameTF = (complexVector**)malloc2d(TIME_SLOTS, MAX_NUM_OUTPUTS, sizeof(complexVector));
    for(t=0; t<TIME_SLOTS; t++) {
        for(ch=0; ch< MAX_NUM_INPUTS; ch++) {
            pData->STFTInputFrameTF[t][ch].re = (float*)calloc(HYBRID_BANDS, sizeof(float));
            pData->STFTInputFrameTF[t][ch].im = (float*)calloc(HYBRID_BANDS, sizeof(float));
        }
        for(ch=0; ch< MAX_NUM_OUTPUTS; ch++) {
            pData->STFTOutputFrameTF[t][ch].re = (float*)calloc(HYBRID_BANDS, sizeof(float));
            pData->STFTOutputFrameTF[t][ch].im = (float*)calloc(HYBRID_BANDS, sizeof(float));
        }
    }
    pData->tempHopFrameTD = (float**)malloc2d( MAX(MAX_NUM_INPUTS, MAX_NUM_OUTPUTS), HOP_SIZE, sizeof(float));
    
    /* flags and gain table */
    for(ch=0; ch<MAX_NUM_INPUTS; ch++)
        pData->recalc_gainsFLAG[ch] = 1;
    pData->reInitGainTables = 1;
    pData->vbap_gtable = NULL;
    pData->reInitTFT = 1;
    
    /* user parameters */
    panner_loadPreset(PRESET_DEFAULT, pData->src_dirs_deg, &(pData->new_nSources), &(dummy)); /*check setStateInformation if you change default preset*/
    pData->nSources = pData->new_nSources;
    pData->DTT = 0.0f;
    pData->spread_deg = 0.0f;
    panner_loadPreset(PRESET_5PX, pData->loudpkrs_dirs_deg, &(pData->new_nLoudpkrs), &(pData->output_nDims)); /*check setStateInformation if you change default preset*/
    pData->nLoudpkrs = pData->new_nLoudpkrs;
}

void panner_destroy
(
    void ** const phPan
)
{
    panner_data *pData = (panner_data*)(*phPan);
    int t, ch;

    if (pData != NULL) {
        if(pData->hSTFT !=NULL)
            afSTFTfree(pData->hSTFT);
        for (t = 0; t<TIME_SLOTS; t++) {
            for(ch=0; ch< MAX_NUM_INPUTS; ch++) {
                free(pData->STFTInputFrameTF[t][ch].re);
                free(pData->STFTInputFrameTF[t][ch].im);
            }
        
            for (ch = 0; ch< MAX_NUM_OUTPUTS; ch++) {
                free(pData->STFTOutputFrameTF[t][ch].re);
                free(pData->STFTOutputFrameTF[t][ch].im);
            }
        }
        free2d((void**)pData->STFTInputFrameTF, TIME_SLOTS);
        free2d((void**)pData->STFTOutputFrameTF, TIME_SLOTS);
        free2d((void**)pData->tempHopFrameTD, MAX(MAX_NUM_INPUTS, MAX_NUM_OUTPUTS));
    
        if(pData->vbap_gtable!= NULL)
            free(pData->vbap_gtable);
         
        free(pData);
        pData = NULL;
    }
}

void panner_init
(
    void * const hPan,
    int          sampleRate
)
{
    panner_data *pData = (panner_data*)(hPan);
    int band;
    
    /* define frequency vector */
    pData->fs = sampleRate;
    for(band=0; band <HYBRID_BANDS; band++){
        if(sampleRate == 44100)
            pData->freqVector[band] =  (float)__afCenterFreq44100[band];
        else
            pData->freqVector[band] =  (float)__afCenterFreq48e3[band];
    }
    
    /* calculate pValue per frequency */
    panner_getPvalue(pData->DTT, pData->freqVector, pData->pValue);

	/* reinitialise if needed */
	panner_checkReInit(hPan);
}

void panner_process
(
    void  *  const hPan,
    float ** const inputs,
    float ** const outputs,
    int            nInputs,
    int            nOutputs,
    int            nSamples,
    int            isPlaying
)
{
    panner_data *pData = (panner_data*)(hPan);
    int t, sample, ch, ls, i, band, nSources, nLoudspeakers, N_azi, aziIndex, elevIndex, idx3d, idx2D;
    float aziRes, elevRes, pv_f, gains3D_sum_pvf, gains2D_sum_pvf;
    float src_dirs[MAX_NUM_INPUTS][2], pValue[HYBRID_BANDS], gains3D[MAX_NUM_OUTPUTS], gains2D[MAX_NUM_OUTPUTS];
    
    /* reinitialise if needed */
#ifdef __APPLE__
    panner_checkReInit(hPan);
#else
    if(pData->reInitTFT==1){
		pData->reInitTFT = 2;
        panner_initTFT(hPan);
        pData->reInitTFT = 0;
    }
#endif

    /* apply panner */
    if ((nSamples == FRAME_SIZE) && (isPlaying == 1) && (pData->vbap_gtable != NULL) 
		&& (pData->reInitTFT == 0) && (pData->reInitGainTables == 0)) {
        memcpy(src_dirs, pData->src_dirs_deg, MAX_NUM_INPUTS*2*sizeof(float));
        memcpy(pValue, pData->pValue, HYBRID_BANDS*sizeof(float));
        nSources = pData->nSources;
        nLoudspeakers = pData->nLoudpkrs;
        
        /* Load time-domain data */
        for(i=0; i < MIN(nSources,nInputs); i++)
            memcpy(pData->inputFrameTD[i], inputs[i], FRAME_SIZE * sizeof(float));
        for(; i<MAX_NUM_INPUTS; i++)
            memset(pData->inputFrameTD[i], 0, FRAME_SIZE * sizeof(float));
        
        /* Apply time-frequency transform (TFT) */
        for ( t=0; t< TIME_SLOTS; t++) {
            for( ch=0; ch < nSources; ch++)
                for ( sample=0; sample < HOP_SIZE; sample++)
                    pData->tempHopFrameTD[ch][sample] = pData->inputFrameTD[ch][sample + t*HOP_SIZE];
            afSTFTforward(pData->hSTFT, (float**)pData->tempHopFrameTD, (complexVector*)pData->STFTInputFrameTF[t]);
        }
        for(band=0; band<HYBRID_BANDS; band++)
            for( ch=0; ch < nSources; ch++)
                for ( t=0; t<TIME_SLOTS; t++)
                    pData->inputframeTF[band][ch][t] = cmplxf(pData->STFTInputFrameTF[t][ch].re[band], pData->STFTInputFrameTF[t][ch].im[band]);
        memset(pData->outputframeTF, 0, HYBRID_BANDS*MAX_NUM_OUTPUTS*TIME_SLOTS * sizeof(float_complex));
        
        /* Apply VBAP Panning */
        if(pData->output_nDims == 3){/* 3-D case */
            aziRes = (float)pData->vbapTableRes[0];
            elevRes = (float)pData->vbapTableRes[1];
            N_azi = (int)(360.0f / aziRes + 0.5f) + 1;
            for (ch = 0; ch < nSources; ch++) {
                /* recalculate frequency dependent panning gains */
                if(pData->recalc_gainsFLAG[ch]){
                    //memset(pData->G_src[band][ch], 0, MAX_NUM_OUTPUTS * sizeof(float));
                    aziIndex = (int)(matlab_fmodf(pData->src_dirs_deg[ch][0] + 180.0f, 360.0f) / aziRes + 0.5f);
                    elevIndex = (int)((pData->src_dirs_deg[ch][1] + 90.0f) / elevRes + 0.5f);
                    idx3d = elevIndex * N_azi + aziIndex;
                    for (ls = 0; ls < nLoudspeakers; ls++)
                        gains3D[ls] =  pData->vbap_gtable[idx3d*nLoudspeakers+ls];
                    for (band = 0; band < HYBRID_BANDS; band++){
                        /* apply pValue per frequency */
                        pv_f = pData->pValue[band];
                        if(pv_f != 2.0f){
                            gains3D_sum_pvf = 0.0f;
                            for (ls = 0; ls < nLoudspeakers; ls++)
                                gains3D_sum_pvf += powf(MAX(gains3D[ls], 0.0f), pv_f);
                            gains3D_sum_pvf = powf(gains3D_sum_pvf, 1.0f/(pv_f+2.23e-9f));
                            for (ls = 0; ls < nLoudspeakers; ls++)
                                pData->G_src[band][ch][ls] = gains3D[ls] / (gains3D_sum_pvf+2.23e-9f);
                        }
                        else
                            for (ls = 0; ls < nLoudspeakers; ls++)
                                pData->G_src[band][ch][ls] = gains3D[ls];
                    }
                    pData->recalc_gainsFLAG[ch] = 0;
                }
                /* apply panning gains */
                for (band = 0; band < HYBRID_BANDS; band++){
                    for (ls = 0; ls < nLoudspeakers; ls++)
                        for (t = 0; t < TIME_SLOTS; t++)
                            pData->outputframeTF[band][ls][t] = ccaddf(pData->outputframeTF[band][ls][t], crmulf(pData->inputframeTF[band][ch][t], pData->G_src[band][ch][ls]));
                }
            }
        }
        else{/* 2-D case */
            aziRes = (float)pData->vbapTableRes[0];
            for (ch = 0; ch < nSources; ch++) {
                /* recalculate frequency dependent panning gains */
                if(pData->recalc_gainsFLAG[ch]){
                    //memset(pData->G_src[band][ch], 0, MAX_NUM_OUTPUTS*sizeof(float));
                    idx2D = (int)((matlab_fmodf(pData->src_dirs_deg[ch][0]+180.0f,360.0f)/aziRes)+0.5f);
                    for (ls = 0; ls < nLoudspeakers; ls++)
                        gains2D[ls] = pData->vbap_gtable[idx2D*nLoudspeakers+ls];
                    for (band = 0; band < HYBRID_BANDS; band++){
                        /* apply pValue per frequency */
                        pv_f = pData->pValue[band];
                        if(pv_f != 2.0f){
                            gains2D_sum_pvf = 0.0f;
                            for (ls = 0; ls < nLoudspeakers; ls++)
                                gains2D_sum_pvf += powf(MAX(gains2D[ls], 0.0f), pv_f);
                            gains2D_sum_pvf = powf(gains2D_sum_pvf, 1.0f/(pv_f+2.23e-9f));
                            for (ls = 0; ls < nLoudspeakers; ls++)
                                pData->G_src[band][ch][ls] = gains2D[ls] / (gains2D_sum_pvf+2.23e-9f);
                        }
                        else
                            for (ls = 0; ls < nLoudspeakers; ls++)
                                pData->G_src[band][ch][ls] = gains2D[ls];
                    }
                    pData->recalc_gainsFLAG[ch] = 0;
                }
                /* apply panning gains */
                for (band = 0; band < HYBRID_BANDS; band++){
                    for (ls = 0; ls < nLoudspeakers; ls++)
                        for (t = 0; t < TIME_SLOTS; t++)
                            pData->outputframeTF[band][ls][t] = ccaddf(pData->outputframeTF[band][ls][t], crmulf(pData->inputframeTF[band][ch][t], pData->G_src[band][ch][ls]));
                }
            }
        }
        
        /* scale by sqrt(number of sources) */
        for (band = 0; band < HYBRID_BANDS; band++)
            for (ls = 0; ls < nLoudspeakers; ls++)
                for (t = 0; t < TIME_SLOTS; t++)
                    pData->outputframeTF[band][ls][t] = crmulf(pData->outputframeTF[band][ls][t], 1.0f/sqrtf((float)nSources));
        
        /* inverse-TFT */
        for (band = 0; band < HYBRID_BANDS; band++) {
            for (ch = 0; ch < nLoudspeakers; ch++) {
                for (t = 0; t < TIME_SLOTS; t++) {
                    pData->STFTOutputFrameTF[t][ch].re[band] = crealf(pData->outputframeTF[band][ch][t]);
                    pData->STFTOutputFrameTF[t][ch].im[band] = cimagf(pData->outputframeTF[band][ch][t]);
                }
            }
        }
        for (t = 0; t < TIME_SLOTS; t++) {
            afSTFTinverse(pData->hSTFT, pData->STFTOutputFrameTF[t], pData->tempHopFrameTD);
            for (ch = 0; ch < MIN(nLoudspeakers, nOutputs); ch++)
                for (sample = 0; sample < HOP_SIZE; sample++)
                    outputs[ch][sample + t* HOP_SIZE] = pData->tempHopFrameTD[ch][sample];
            for (; ch < nOutputs; ch++)
                for (sample = 0; sample < HOP_SIZE; sample++)
                    outputs[ch][sample + t* HOP_SIZE] = 0.0f;
        }
    }
    else 
        for (ch=0; ch < nOutputs; ch++)
            memset(outputs[ch],0, FRAME_SIZE*sizeof(float));
}


/* Set Functions */

void panner_refreshSettings(void* const hPan)
{
    panner_data *pData = (panner_data*)(hPan);
    int ch;
    pData->reInitGainTables = 1;
    pData->reInitTFT = 1;
    for(ch=0; ch<MAX_NUM_INPUTS; ch++)
        pData->recalc_gainsFLAG[ch] = 1;
}

void panner_checkReInit(void* const hPan)
{
	panner_data *pData = (panner_data*)(hPan);

	/* reinitialise if needed */
	if (pData->reInitTFT==1) {
		pData->reInitTFT = 2;
		panner_initTFT(hPan);
		pData->reInitTFT = 0;
	}
	if (pData->reInitGainTables==1) {
		pData->reInitGainTables = 2;
		panner_initGainTables(hPan);
		pData->reInitGainTables = 0;
	}
}

void panner_setSourceAzi_deg(void* const hPan, int index, float newAzi_deg)
{
    panner_data *pData = (panner_data*)(hPan);
    if(newAzi_deg>180.0f)
        newAzi_deg = -360.0f + newAzi_deg;
    newAzi_deg = MAX(newAzi_deg, -180.0f);
    newAzi_deg = MIN(newAzi_deg, 180.0f);
    pData->src_dirs_deg[index][0] = newAzi_deg;
    pData->recalc_gainsFLAG[index] = 1;
}

void panner_setSourceElev_deg(void* const hPan, int index, float newElev_deg)
{
    panner_data *pData = (panner_data*)(hPan);
    newElev_deg = MAX(newElev_deg, -90.0f);
    newElev_deg = MIN(newElev_deg, 90.0f);
    pData->src_dirs_deg[index][1] = newElev_deg;
    pData->recalc_gainsFLAG[index] = 1;
}

void panner_setNumSources(void* const hPan, int new_nSources)
{
    panner_data *pData = (panner_data*)(hPan);
    int ch;
    /* determine if TFT must be reinitialised */
    pData->new_nSources = new_nSources > MAX_NUM_INPUTS ? MAX_NUM_INPUTS : new_nSources;
    if(pData->nSources != pData->new_nSources){
        pData->reInitTFT = 1;
        for(ch=pData->nSources; ch<pData->new_nSources; ch++)
            pData->recalc_gainsFLAG[ch] = 1;
    }
}

void panner_setLoudspeakerAzi_deg(void* const hPan, int index, float newAzi_deg)
{
    panner_data *pData = (panner_data*)(hPan);
    int ch;
    if(newAzi_deg>180.0f)
        newAzi_deg = -360.0f + newAzi_deg;
    newAzi_deg = MAX(newAzi_deg, -180.0f);
    newAzi_deg = MIN(newAzi_deg, 180.0f);
    pData->loudpkrs_dirs_deg[index][0] = newAzi_deg;
    pData->reInitGainTables=1;
    for(ch=0; ch<MAX_NUM_INPUTS; ch++)
        pData->recalc_gainsFLAG[ch] = 1;
}

void panner_setLoudspeakerElev_deg(void* const hPan, int index, float newElev_deg)
{
    panner_data *pData = (panner_data*)(hPan);
    int ch;
    newElev_deg = MAX(newElev_deg, -90.0f);
    newElev_deg = MIN(newElev_deg, 90.0f);
    pData->loudpkrs_dirs_deg[index][1] = newElev_deg;
    pData->reInitGainTables=1;
    for(ch=0; ch<MAX_NUM_INPUTS; ch++)
        pData->recalc_gainsFLAG[ch] = 1;
}

void panner_setNumLoudspeakers(void* const hPan, int new_nLoudspeakers)
{
    panner_data *pData = (panner_data*)(hPan);
    int ch;
    pData->new_nLoudpkrs = new_nLoudspeakers > MAX_NUM_OUTPUTS ? MAX_NUM_OUTPUTS : new_nLoudspeakers;
    if(pData->nLoudpkrs != pData->new_nLoudpkrs)
        pData->reInitTFT = 1; 
    pData->reInitGainTables=1;
    for(ch=0; ch<MAX_NUM_INPUTS; ch++)
        pData->recalc_gainsFLAG[ch] = 1;
}

void panner_setOutputConfigPreset(void* const hPan, int newPresetID)
{
    panner_data *pData = (panner_data*)(hPan);
    int ch, dummy;
    panner_loadPreset(newPresetID, pData->loudpkrs_dirs_deg, &(pData->new_nLoudpkrs), &dummy);
    if(pData->nLoudpkrs != pData->new_nLoudpkrs)
        pData->reInitTFT = 1;
    pData->reInitGainTables=1;
    for(ch=0; ch<MAX_NUM_INPUTS; ch++)
        pData->recalc_gainsFLAG[ch] = 1;
}

void panner_setInputConfigPreset(void* const hPan, int newPresetID)
{
    panner_data *pData = (panner_data*)(hPan);
    int ch, dummy;
    panner_loadPreset(newPresetID, pData->src_dirs_deg, &(pData->new_nSources), &dummy);
    if(pData->nSources != pData->new_nSources)
        pData->reInitTFT = 1;
    for(ch=0; ch<pData->new_nSources; ch++)
        pData->recalc_gainsFLAG[ch] = 1;
}

void panner_setDTT(void* const hPan, float newValue)
{
    panner_data *pData = (panner_data*)(hPan);
    int ch;
    pData->DTT = newValue;
    panner_getPvalue(pData->DTT, pData->freqVector, pData->pValue);
    for(ch=0; ch<pData->new_nSources; ch++)
        pData->recalc_gainsFLAG[ch] = 1;
}

void panner_setSpread(void* const hPan, float newValue)
{
    panner_data *pData = (panner_data*)(hPan);
    int ch;
    if(pData->spread_deg!=newValue){
        pData->spread_deg = newValue;
        pData->reInitGainTables=1;
        for(ch=0; ch<MAX_NUM_INPUTS; ch++)
            pData->recalc_gainsFLAG[ch] = 1;
    }
}


/* Get Functions */

float panner_getSourceAzi_deg(void* const hPan, int index)
{
    panner_data *pData = (panner_data*)(hPan);
    return pData->src_dirs_deg[index][0];
}

float panner_getSourceElev_deg(void* const hPan, int index)
{
    panner_data *pData = (panner_data*)(hPan);
    return pData->src_dirs_deg[index][1];
}

int panner_getNumSources(void* const hPan)
{
    panner_data *pData = (panner_data*)(hPan);
    return pData->new_nSources;
}

int panner_getMaxNumSources()
{
    return MAX_NUM_INPUTS;
}

float panner_getLoudspeakerAzi_deg(void* const hPan, int index)
{
    panner_data *pData = (panner_data*)(hPan);
    return pData->loudpkrs_dirs_deg[index][0];
}

float panner_getLoudspeakerElev_deg(void* const hPan, int index)
{
    panner_data *pData = (panner_data*)(hPan);
    return pData->loudpkrs_dirs_deg[index][1];
}

int panner_getNumLoudspeakers(void* const hPan)
{
    panner_data *pData = (panner_data*)(hPan);
    return pData->new_nLoudpkrs;
}

int panner_getMaxNumLoudspeakers()
{
    return MAX_NUM_OUTPUTS;
}

int panner_getDAWsamplerate(void* const hPan)
{
    panner_data *pData = (panner_data*)(hPan);
    return pData->fs;
}

float panner_getDTT(void* const hPan)
{
    panner_data *pData = (panner_data*)(hPan);
    return pData->DTT;
}

float panner_getSpread(void* const hPan)
{
    panner_data *pData = (panner_data*)(hPan);
    return pData->spread_deg;
}

int panner_getProcessingDelay()
{
    return 12*HOP_SIZE;
}





    
    
