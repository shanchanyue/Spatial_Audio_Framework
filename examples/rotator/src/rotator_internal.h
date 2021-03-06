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
 *     rotator_internal.h  
 * Description:
 *     A simple spherical harmonic domain rotator.
 * Dependencies:
 *     saf_utilities, saf_sh
 * Author, date created:
 *     Leo McCormack, 02.11.2017
 */

#ifndef __ROTATOR_INTERNAL_H_INCLUDED__
#define __ROTATOR_INTERNAL_H_INCLUDED__

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "rotator.h"
#define SAF_ENABLE_SH /* for spherical harmonic domain rotation matrices */
#include "saf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SH_ORDER ( 7 )
#define MAX_NUM_SH_SIGNALS ( (MAX_SH_ORDER + 1)*(MAX_SH_ORDER + 1)  )    /* (L+1)^2 */
    
#ifndef DEG2RAD
  #define DEG2RAD(x) (x * PI / 180.0f)
#endif
#ifndef RAD2DEG
  #define RAD2DEG(x) (x * 180.0f / PI)
#endif

typedef struct _rotator
{
    float inputFrameTD[MAX_NUM_SH_SIGNALS][FRAME_SIZE];
    float prev_inputFrameTD[MAX_NUM_SH_SIGNALS][FRAME_SIZE];
    float tempFrame[MAX_NUM_SH_SIGNALS][FRAME_SIZE];
    float outputFrameTD[MAX_NUM_SH_SIGNALS][FRAME_SIZE];
    
    /* internal */
    float interpolator[FRAME_SIZE];
    float M_rot[MAX_NUM_SH_SIGNALS][MAX_NUM_SH_SIGNALS];
    float prev_M_rot[MAX_NUM_SH_SIGNALS][MAX_NUM_SH_SIGNALS];

    /* user parameters */
    float yaw, roll, pitch;                  /* rotation angles in degrees */
    int bFlipYaw, bFlipPitch, bFlipRoll;     /* flag to flip the sign of the individual rotation angles */
    int order;                               /* current decoding order */
    CH_ORDER chOrdering;                     /* only ACN is supported */
    NORM_TYPES norm;                         /* N3D or SN3D */
    OUTPUT_ORDERS outputOrder;               /* current decoding order PRESET */
    int useRollPitchYawFlag;                 /* rotation order flag, 1: r-p-y, 0: y-p-r */
    
} rotator_data;
    
#ifdef __cplusplus
}
#endif


#endif /* __ROTATOR_INTERNAL_H_INCLUDED__ */




















