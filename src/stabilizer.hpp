/*
   stabilizer.hpp : PID-based stabilization 

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

#pragma once

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <limits>
#include <cmath>

#include "receiver.hpp"
#include "filter.hpp"
#include "debug.hpp"
#include "datatypes.hpp"

namespace hf {

    // shared with Hackflight class
    enum {
        AXIS_ROLL = 0,
        AXIS_PITCH,
        AXIS_YAW
    };

    class Stabilizer {

        public:

            Stabilizer(float levelP, float gyroCyclicP, float gyroCyclicI, float gyroCyclicD, float gyroYawP, float gyroYawI) :
                _levelP(levelP), 
                _gyroCyclicP(gyroCyclicP), 
                _gyroCyclicI(gyroCyclicI), 
                _gyroCyclicD(gyroCyclicD), 
                _gyroYawP(gyroYawP), 
                _gyroYawI(gyroYawI) { }


        private: 

            // PIDs
            float _levelP;
            float _gyroCyclicP;
            float _gyroCyclicI;
            float _gyroCyclicD; 
            float _gyroYawP; 
            float _gyroYawI;

            // Resetting thresholds for PID Integral term
            const float gyroWindupMax           = 16.0f;
            const float bigGyroDegreesPerSecond = 40.0f; 
            const float bigYawDemand            = 0.1f;
            const float maxArmingAngleDegrees   = 25.0f;         

            float lastGyro[2];
            float gyroDelta1[2]; 
            float gyroDelta2[2];
            float errorGyroI[3];

            float bigGyroRate;

            float degreesToRadians(float deg)
            {
                return M_PI * deg / 180.;
            }

            float computeITermGyro(float rateP, float rateI, float rcCommand, float gyro[3], uint8_t axis)
            {
                float error = rcCommand*rateP - gyro[axis];

                // Avoid integral windup
                errorGyroI[axis] = Filter::constrainAbs(errorGyroI[axis] + error, gyroWindupMax);

                // Reset integral on quick gyro change or large gyroYaw command
                if ((fabs(gyro[axis]) > bigGyroRate) || ((axis == AXIS_YAW) && (fabs(rcCommand) > bigYawDemand)))
                    errorGyroI[axis] = 0;

                return (errorGyroI[axis] * rateI);
            }

            float computePid( float rateP, float PTerm, float ITerm, float DTerm, float gyro[3], uint8_t axis)
            {
                PTerm -= gyro[axis] * rateP; 

                return PTerm + ITerm - DTerm;
            }

            // Computes leveling PID for pitch or roll
            float computeCyclicPid( float rcCommand, float prop, float eulerAngles[3], float gyro[3], uint8_t imuAxis)
            {
                float ITermGyro = computeITermGyro(_gyroCyclicP, _gyroCyclicI, rcCommand, gyro, imuAxis);

                float PTermEuler = (rcCommand - eulerAngles[imuAxis]) * _levelP;  

                float PTerm = Filter::complementary(rcCommand, PTermEuler, prop); 

                float ITerm = ITermGyro * prop;

                float gyroDelta = gyro[imuAxis] - lastGyro[imuAxis];
                lastGyro[imuAxis] = gyro[imuAxis];
                float gyroDeltaSum = gyroDelta1[imuAxis] + gyroDelta2[imuAxis] + gyroDelta;
                gyroDelta2[imuAxis] = gyroDelta1[imuAxis];
                gyroDelta1[imuAxis] = gyroDelta;

                float DTerm = gyroDeltaSum * _gyroCyclicD; 

                return computePid(_gyroCyclicP, PTerm, ITerm, DTerm, gyro, imuAxis);
            }

            float constrainCyclicDemand(float eulerAngle, float demand)
            {
                return demand * (1 - fabs(eulerAngle)/maxArmingAngle);
            }

        public:

            float maxArmingAngle;

            void init(void)
            {
                // Zero-out previous values for D term
                for (uint8_t axis=0; axis<2; ++axis) {
                    lastGyro[axis] = 0;
                    gyroDelta1[axis] = 0;
                    gyroDelta2[axis] = 0;
                }

                // Convert degree parameters to radians for use later
                bigGyroRate = degreesToRadians(bigGyroDegreesPerSecond);
                maxArmingAngle = degreesToRadians(maxArmingAngleDegrees);

                // Initialize gyro error integral
                resetIntegral();
            }

            void updateDemands(float eulerAngles[3], float gyroRates[3], demands_t & demands)
            {
                // Compute proportion of cyclic demand compared to its maximum
                float prop = Filter::max(fabs(demands.roll), fabs(demands.pitch)) / 0.5f;

                // Pitch, roll use leveling based on Euler angles
                demands.roll  = computeCyclicPid(demands.roll,  prop, eulerAngles, gyroRates, AXIS_ROLL);
                demands.pitch = computeCyclicPid(demands.pitch, prop, eulerAngles, gyroRates, AXIS_PITCH);

                // For gyroYaw, P term comes directly from RC command, and D term is zero
                float ITermGyroYaw = computeITermGyro(_gyroYawP, _gyroYawI, demands.yaw, gyroRates, AXIS_YAW);
                demands.yaw = computePid(_gyroYawP, demands.yaw, ITermGyroYaw, 0, gyroRates, AXIS_YAW);

                // Prevent "gyroYaw jump" during gyroYaw correction
                demands.yaw = Filter::constrainAbs(demands.yaw, 0.1 + fabs(demands.yaw));
            }

            void resetIntegral(void)
            {
                errorGyroI[AXIS_ROLL] = 0;
                errorGyroI[AXIS_PITCH] = 0;
                errorGyroI[AXIS_YAW] = 0;
            }

    };  // class Stabilize

} // namespace
