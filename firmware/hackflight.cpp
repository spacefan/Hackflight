/*
   hackflight.cpp : Hackflight class implementation and hooks to setup(), loop()

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

#include <math.h>

#include "hackflight.hpp"
#include "debug.hpp"

#include <string.h>

#if defined(STM32)
extern "C" { 
#endif

void Hackflight::initialize(void)
{
    uint16_t acc1G;
    float    gyroScale;
    uint32_t looptimeUsec;
    uint32_t gyroCalibrationMsec;

    // Get particulars for board
    Board::init(acc1G, gyroScale, looptimeUsec, gyroCalibrationMsec);

    this->imuLooptimeUsec = looptimeUsec;

    // compute cycles for calibration based on board's time constant
    this->calibratingGyroCycles = (uint16_t)(1000. * gyroCalibrationMsec / this->imuLooptimeUsec);
    this->calibratingAccCycles  = (uint16_t)(1000. * CONFIG_CALIBRATING_ACC_MSEC  / this->imuLooptimeUsec);

    // initialize our external objects with objects they need
    this->stab.init();
    this->imu.init(acc1G, gyroScale, this->calibratingGyroCycles, this->calibratingAccCycles);
    this->mixer.init(&this->rc, &this->stab); 

    // ensure not armed
    this->armed = false;

    // sleep for 100ms
    Board::delayMilliseconds(100);

    // flash the LEDs to indicate startup
    Board::ledRedOff();
    Board::ledGreenOff();
    for (uint8_t i = 0; i < 10; i++) {
        Board::ledRedOn();
        Board::ledGreenOn();
        Board::delayMilliseconds(50);
        Board::ledRedOff();
        Board::ledGreenOff();
        Board::delayMilliseconds(50);
    }

    // intialize the R/C object
    this->rc.init();

    // always do gyro calibration at startup
    this->calibratingG = this->calibratingGyroCycles;

    // assume shallow angle (no accelerometer calibration needed)
    this->haveSmallAngle = true;

    // initializing timing tasks
    this->imuTask.init(this->imuLooptimeUsec);
    this->rcTask.init(CONFIG_RC_LOOPTIME_MSEC * 1000);
    this->accelCalibrationTask.init(CONFIG_CALIBRATE_ACCTIME_MSEC * 1000);

    // initialize MSP comms
    this->msp.init(&this->imu, &this->mixer, &this->rc);

    // do any extra initializations (baro, sonar, etc.)
    this->board.extrasInit(&msp);

} // intialize

void Hackflight::update(void)
{
    static bool     accCalibrated;
    static uint16_t calibratingA;
    static uint32_t currentTime;

    bool rcSerialReady = Board::rcSerialReady();

    if (this->rcTask.checkAndUpdate(currentTime) || rcSerialReady) {

        // update RC channels
        this->rc.update(&this->board);

        rcSerialReady = false;

        // useful for simulator
        if (this->armed)
            Board::showAuxStatus(this->rc.auxState());

        // when landed, reset integral component of PID
        if (this->rc.throttleIsDown()) 
            this->stab.resetIntegral();

        if (this->rc.changed()) {

            if (this->armed) {      // actions during armed

                // Disarm on throttle down + yaw
                if (this->rc.sticks == THR_LO + YAW_LO + PIT_CE + ROL_CE) {
                    if (this->armed) {
                        armed = false;
                        Board::showArmedStatus(this->armed);
                    }
                }
            } else {         // actions during not armed

                // gyro calibration
                if (this->rc.sticks == THR_LO + YAW_LO + PIT_LO + ROL_CE) 
                    this->calibratingG = this->calibratingGyroCycles;

                // Arm via throttle-low / yaw-right
                if (this->rc.sticks == THR_LO + YAW_HI + PIT_CE + ROL_CE)
                    if (this->calibratingG == 0 && accCalibrated) 
                        if (!this->rc.auxState()) // aux switch must be in zero position
                            if (!this->armed) {
                                this->armed = true;
                                Board::showArmedStatus(this->armed);
                            }

                // accel calibration
                if (this->rc.sticks == THR_HI + YAW_LO + PIT_LO + ROL_CE)
                    calibratingA = this->calibratingAccCycles;

            } // not armed

        } // this->rc.changed()

        // Detect aux switch changes for hover, altitude-hold, etc.
        this->board.extrasCheckSwitch();

    } else {                    // not in rc loop

        static int taskOrder;   // never call all functions in the same loop, to avoid high delay spikes

        this->board.extrasPerformTask(taskOrder);

        taskOrder++;

        if (taskOrder >= Board::extrasGetTaskCount()) // using >= supports zero or more tasks
            taskOrder = 0;
    }

    currentTime = Board::getMicros();

    if (this->imuTask.checkAndUpdate(currentTime)) {

        Board::imuRead(this->accelADC, this->gyroADC);

        this->imu.update(this->accelADC, this->gyroADC, currentTime, this->armed, calibratingA, this->calibratingG);

        if (calibratingA > 0)
            calibratingA--;

        if (calibratingG > 0)
            calibratingG--;

        this->haveSmallAngle = 
            abs(this->imu.angle[0]) < CONFIG_SMALL_ANGLE && abs(this->imu.angle[1]) < CONFIG_SMALL_ANGLE;

        // measure loop rate just afer reading the sensors
        currentTime = Board::getMicros();

        // compute exponential RC commands
        this->rc.computeExpo();

        // use LEDs to indicate calibration status
        if (calibratingA > 0 || this->calibratingG > 0) {
            Board::ledGreenOn();
        }
        else {
            if (accCalibrated)
                Board::ledGreenOff();
            if (this->armed)
                Board::ledRedOn();
            else
                Board::ledRedOff();
        }

        // periodically update accelerometer calibration status
        static bool on;
        if (this->accelCalibrationTask.check(currentTime)) {
            if (!this->haveSmallAngle) {
                accCalibrated = false; 
                if (on) {
                    Board::ledGreenOff();
                    on = false;
                }
                else {
                    Board::ledGreenOn();
                    on = true;
                }
                this->accelCalibrationTask.update(currentTime);
            } else {
                accCalibrated = true;
            }
        }

        // update stability PID controller 
        this->stab.update(this->rc.command, this->gyroADC, this->imu.angle);

        // update mixer
        this->mixer.update(this->armed, &this->board);

        // handle serial communications
        this->msp.update(this->armed);

    } // IMU update

} // loop()

static Hackflight hackflight;

void setup(void)
{
    hackflight.initialize();
}

void loop(void)
{
    hackflight.update();
}

#if defined(STM32)
} // extern "C"
#endif
