/*
 * state_estimation.c
 *
 *  Created on: Apr 13, 2024
 *      Author: joedo
 */

#include "millis.h"
#include "printf.h"

#include "ICM-20948.h"
#include "vn_handler.h"
#include "log.h"
#include "can_handler.h"
#include "state_estimation.h"

#define SAMPLE_RATE_HZ 20 //Hz; Need to set to match VN data rate
#define TASK_DELAY_TICKS 1000 / SAMPLE_RATE_HZ

#ifndef USE_ICM //Default use ICM to off
#define USE_ICM 0 //Enable to pull raw IMU data from connected ICM-24098 IMU breakout
#endif

#define RESET_FILTER_CMD (xEventGroupGetBits(calibrationEventHandle) & RESET_FILTER_FLAG)

extern xQueueHandle angleQueue;
QueueHandle_t IMUDataHandle;
EventGroupHandle_t calibrationEventHandle;

bool unpackIMUData(FusionVector *gyroscope, FusionVector *accelerometer, FusionVector *magnetometer, float *TimeS)
{
	rawIMUPacked data;
	if(xQueueReceive(IMUDataHandle, &data, 50) == pdTRUE)
	{
		*gyroscope = data.gyroscope;
		*accelerometer = data.accelerometer;
		*magnetometer = data.magnetometer;
		*TimeS = data.TimeS;
		return true;
	}
	return false;
}

bool state_est_init()
{
	calibrationEventHandle = xEventGroupCreate();
	xEventGroupClearBits(calibrationEventHandle, 0xFFFF); //clear out the enitire group since API doesn't specify if values are initialized to 0
	IMUDataHandle = xQueueCreate(3, sizeof(rawIMUPacked));
	return IMUDataHandle != NULL && calibrationEventHandle != NULL;
}

void stateEstTask(void *arguments) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    float previousTimestamp = 0;

    const FusionMatrix gyroscopeMisalignment = {
            .element = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f}
    };
    const FusionVector gyroscopeSensitivity = {
            .axis = {1.0f, 1.0f, 1.0f}
    };
    const FusionVector gyroscopeOffset = {
            .axis = {0.0f, 0.0f, 0.0f}
    };
    const FusionMatrix accelerometerMisalignment = {
                .element = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f}
        };
    const FusionVector accelerometerSensitivity = {
            .axis = {1.0f, 1.0f, 1.0f}
    };
    const FusionVector accelerometerOffset = {
            .axis = {0.0f, 0.0f, 0.0f}
    };

    const FusionMatrix softIronMatrix = {
            .element = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f}
    };
    const FusionVector hardIronOffset = {
            .axis = {0.0f, 0.0f, 0.0f}
    };

    // Create and Init Fusion objects
    FusionOffset offset;
    FusionAhrs ahrs;

    FusionOffsetInitialise(&offset, SAMPLE_RATE_HZ);
    FusionAhrsInitialise(&ahrs);

    // Set AHRS algorithm settings
     const FusionAhrsSettings settings = {
             .convention = FusionConventionNed,
             .gain = 0.5f,
             .gyroscopeRange = 2000.0f,
             .accelerationRejection = 10.0f,
             .magneticRejection = 10.0f,
             .recoveryTriggerPeriod = 5 * SAMPLE_RATE_HZ,
     };
     FusionAhrsSetSettings(&ahrs, &settings);

#if USE_ICM == 1
     //if we are using the ICM IMU, initialize it
     bool res = true;
     res &= ICM_20948_setup();
     res &= ICM_20948_check_sanity();
    // res &= MAG_Self_Test();
     if(!res) {
         //throw an error?
     }
#endif

    FusionVector gyroscope;
    FusionVector accelerometer;
    FusionVector magnetometer;

    // This loop should repeat each time new gyroscope data is available
    while (true)
    {
    	if(RESET_FILTER_CMD)
    	{
    	    logInfo("stateEst", "reset filter");
    		FusionAhrsReset(&ahrs);
    		xEventGroupClearBits(calibrationEventHandle, RESET_FILTER_FLAG);
    	}

#if USE_ICM == 1
        //If using the ICM, do a synchronous (wrt to state estimation) read on all sensors
        float xData;
        float yData;
        float zData;

        //TODO: wrap this operation in a single function call that populates Fusion Vectors and spits out a single bool
        // gyroscope data in degrees/s
        if(ICM_20948_get_gyro_converted(&xData, &yData, &zData)){
            gyroscope.axis.x = xData;
            gyroscope.axis.y = yData;
            gyroscope.axis.z = zData;
        }

        //accelerometer data in m/s^2
        if(ICM_20948_get_accel_converted(&xData, &yData, &zData)) {
            accelerometer.axis.x = xData;
            accelerometer.axis.y = yData;
            accelerometer.axis.z = zData;
        }

        // magnetometer data in microteslas
        if(ICM_20948_get_mag_converted(&xData, &yData, &zData)) {
            magnetometer.axis.x = xData;
            magnetometer.axis.y = yData;
            magnetometer.axis.z = zData;
        }

        // Calculate delta time (in seconds) to account for gyroscope sample clock error
        const float timestamp = millis_(); // replace this with actual gyroscope timestamp
        const float deltaTime = (float) (timestamp - previousTimestamp) / 1000;
        previousTimestamp = timestamp;

#else
        float imuTimestamp;
		 if(unpackIMUData(&gyroscope, &accelerometer, &magnetometer, &imuTimestamp) == false)
		 {
			 logError("stateEst", "Failed to get VN raw IMU data");
		 }
		 else
		 {
			 float deltaTime = imuTimestamp - previousTimestamp;
			 previousTimestamp = imuTimestamp;
#endif

			// Apply calibration
			gyroscope = FusionCalibrationInertial(gyroscope, gyroscopeMisalignment, gyroscopeSensitivity, gyroscopeOffset);
			accelerometer = FusionCalibrationInertial(accelerometer, accelerometerMisalignment, accelerometerSensitivity, accelerometerOffset);
			magnetometer = FusionCalibrationMagnetic(magnetometer, softIronMatrix, hardIronOffset);

			// Update gyroscope offset correction algorithm
			gyroscope = FusionOffsetUpdate(&offset, gyroscope);

			// Update gyroscope AHRS algorithm
			FusionAhrsUpdate(&ahrs, gyroscope, accelerometer, magnetometer, deltaTime);

			// Calculate algorithm outputs
			const FusionEuler euler = FusionQuaternionToEuler(FusionAhrsGetQuaternion(&ahrs));

			// Send angle estimation to queue for trajectory pred
			xQueueOverwrite(angleQueue, &euler);

			can_msg_t msg;
			if(build_state_est_data_msg(millis_(), &euler.angle.roll, STATE_FILTER_ROLL, &msg)) xQueueSend(busQueue, &msg, 10);
			if(build_state_est_data_msg(millis_(), &euler.angle.pitch, STATE_FILTER_PITCH, &msg)) xQueueSend(busQueue, &msg, 10);
			if(build_state_est_data_msg(millis_(), &euler.angle.yaw, STATE_FILTER_YAW, &msg)) xQueueSend(busQueue, &msg, 10);

			logInfo("stateEst", "EuRoll %f, EuPitch %f, EuYaw %f", euler.angle.roll, euler.angle.pitch, euler.angle.yaw);
			//printf_(">EuRoll:%.2f\n>EuPitch:%.2f\n>EuYaw:%.2f\n", euler.angle.roll, euler.angle.pitch, euler.angle.yaw);

#if USE_ICM == 0
		 }
#endif
         vTaskDelayUntil(&xLastWakeTime, TASK_DELAY_TICKS);
    }
}
