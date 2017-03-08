/*
   stabilize.cpp : PID-based stability class implementation

   Adapted from https://github.com/multiwii/baseflight/blob/master/src/mw.c

   This file is part of Hackflight.

   Hackflight is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.
   Hackflight is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   You should have received a copy of the GNU General Public License
   along with Hackflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "hackflight.hpp"

void Stabilize::init(void)
{
    for (uint8_t axis=0; axis<3; ++axis) {
        this->lastGyro[axis] = 0;
        this->delta1[axis] = 0;
        this->delta2[axis] = 0;
    }

    this->rate_p[0] = CONFIG_RATE_PITCHROLL_P;
    this->rate_p[1] = CONFIG_RATE_PITCHROLL_P;
    this->rate_p[2] = CONFIG_YAW_P;

    this->rate_i[0] = CONFIG_RATE_PITCHROLL_I;
    this->rate_i[1] = CONFIG_RATE_PITCHROLL_I;
    this->rate_i[2] = CONFIG_YAW_I;

    this->rate_d[0] = CONFIG_RATE_PITCHROLL_D;
    this->rate_d[1] = CONFIG_RATE_PITCHROLL_D;
    this->rate_d[2] = 0;

    this->resetIntegral();
}

#define constrain(val, lo, hi) (val) < (lo) ? lo : ((val) > hi ? hi : val) 

void Stabilize::update(int16_t rcCommand[4], int16_t gyroADC[3], int16_t angle[3])
{
    for (uint8_t axis = 0; axis < 3; axis++) {

        int32_t error = (int32_t)rcCommand[axis] * 10 * 8 / this->rate_p[axis];
        error -= gyroADC[axis];

        int32_t PTermGYRO = rcCommand[axis];

        this->errorGyroI[axis] = constrain(this->errorGyroI[axis] + error, -16000, +16000); // WindUp
        if ((abs(gyroADC[axis]) > 640) || ((axis == AXIS_YAW) && (abs(rcCommand[axis]) > 100)))
            this->errorGyroI[axis] = 0;
        int32_t ITermGYRO = (this->errorGyroI[axis] / 125 * this->rate_i[axis]) >> 6;

        int32_t PTerm = PTermGYRO;
        int32_t ITerm = ITermGYRO;

        if (axis < 2) {

            // 50 degrees max inclination
            int32_t errorAngle = constrain(2 * rcCommand[axis], 
                                           -((int)CONFIG_MAX_ANGLE_INCLINATION), 
                                           + CONFIG_MAX_ANGLE_INCLINATION) 
                                 - angle[axis];

            int32_t PTermACC = errorAngle * CONFIG_LEVEL_P / 100; 

            this->errorAngleI[axis] = constrain(this->errorAngleI[axis] + errorAngle, -10000, +10000); // WindUp
            int32_t ITermACC = ((int32_t)(this->errorAngleI[axis] * CONFIG_LEVEL_I)) >> 12;

            int32_t prop = max(abs(rcCommand[DEMAND_PITCH]), 
                    abs(rcCommand[DEMAND_ROLL])); // range [0;500]

            PTerm = (PTermACC * (500 - prop) + PTermGYRO * prop) / 500;
            ITerm = (ITermACC * (500 - prop) + ITermGYRO * prop) / 500;
        } 

        PTerm -= (int32_t)gyroADC[axis] * this->rate_p[axis] / 10 / 8; // 32 bits is needed for calculation
        int32_t delta = gyroADC[axis] - this->lastGyro[axis];
        this->lastGyro[axis] = gyroADC[axis];
        int32_t deltaSum = this->delta1[axis] + this->delta2[axis] + delta;
        this->delta2[axis] = this->delta1[axis];
        this->delta1[axis] = delta;
        int32_t DTerm = (deltaSum * this->rate_d[axis]) / 32;
        this->axisPID[axis] = PTerm + ITerm - DTerm;
    }

    // prevent "yaw jump" during yaw correction
    this->axisPID[AXIS_YAW] = constrain(this->axisPID[AXIS_YAW], 
            -100 - abs(rcCommand[DEMAND_YAW]), +100 + abs(rcCommand[DEMAND_YAW]));
}

void Stabilize::resetIntegral(void)
{
    this->errorGyroI[AXIS_ROLL] = 0;
    this->errorGyroI[AXIS_PITCH] = 0;
    this->errorGyroI[AXIS_YAW] = 0;
    this->errorAngleI[AXIS_ROLL] = 0;
    this->errorAngleI[AXIS_PITCH] = 0;
}
