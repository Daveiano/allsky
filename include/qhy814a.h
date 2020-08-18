/*
 QHYCCD SDK
 
 Copyright (c) 2014 QHYCCD.
 All Rights Reserved.
 
 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the Free
 Software Foundation; either version 2 of the License, or (at your option)
 any later version.
 
 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 more details.
 
 You should have received a copy of the GNU General Public License along with
 this program; if not, write to the Free Software Foundation, Inc., 59
 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 
 The full GNU General Public License is included in this distribution in the
 file called LICENSE.
 */

/*!
 * @file qhy814a.h
 * @brief QHY814A class define
 */

#include "qhyabase.h"

#ifndef __QHY814ADEF_H__
#define __QHY814ADEF_H__

/**
 * @brief IC8300 class define
 *
 * include all functions for ic8300
 */
class QHY814A:public QHYABASE
{
public:
    QHY814A();
    ~QHY814A();

    /**
     @fn uint32_t SetChipBinMode(qhyccd_handle *h,uint32_t wbin,uint32_t hbin)
     @brief set the camera offset
     @param h camera control handle
     @param wbin width bin
     @param hbin height bin
     @return
     success return QHYCCD_SUCCESS \n
     another QHYCCD_ERROR code on other failures
     */
    uint32_t SetChipBinMode(qhyccd_handle *h,uint32_t wbin,uint32_t hbin);
          

	uint32_t SetFocusSetting(qhyccd_handle *h, uint32_t focusCenterX, uint32_t focusCenterY);

	double GetChipCoolPWM();

    uint32_t AutoTempControl(qhyccd_handle *h,double ttemp);

    uint32_t SetChipCoolPWM(qhyccd_handle *h,double PWM);

    double GetChipCoolTemp(qhyccd_handle *h);

private:
    double lastTargetTemp;
    double  lastPWM;
};
#endif

