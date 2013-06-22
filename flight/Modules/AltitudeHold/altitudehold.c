/**
 ******************************************************************************
 * @addtogroup TauLabsModules Tau Labs Modules
 * @{
 * @addtogroup AltitudeHoldModule Altitude hold module
 * @{
 *
 * @file       altitudehold.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @author     Tau Labs, http://taulabs.org, Copyright (C) 2012-2013
 * @brief      This module runs an EKF to estimate altitude from just a barometric
 *             sensor and controls throttle to hold a fixed altitude
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * Input object: @ref AltitudeHoldDesired
 * Input object: @ref BaroAltitude
 * Input object: @ref Accels
 * Output object: @ref StabilizationDesired
 * Output object: @ref AltHoldSmoothed
 *
 * Runs an EKF on the @ref accels and @ref BaroAltitude to estimate altitude, velocity
 * and acceleration which is output in @ref AltHoldSmoothed.  Then a control value is
 * computed for @StabilizationDesired throttle.  Roll and pitch are set to Attitude
 * mode and use the values from @AltHoldDesired.	
 *
 * The module executes in its own thread in this example.
 */

#include "openpilot.h"
#include "physical_constants.h"
#include <math.h>
#include "coordinate_conversions.h"
#include "altholdsmoothed.h"
#include "attitudeactual.h"
#include "altitudeholdsettings.h"
#include "altitudeholddesired.h"	// object that will be updated by the module
#include "baroaltitude.h"
#include "positionactual.h"
#include "flightstatus.h"
#include "stabilizationdesired.h"
#include "accels.h"
#include "modulesettings.h"

// Private constants
#define MAX_QUEUE_SIZE 3
#define STACK_SIZE_BYTES 1024
#define TASK_PRIORITY (tskIDLE_PRIORITY+1)
#define ACCEL_DOWNSAMPLE 4

// Private variables
static xTaskHandle altitudeHoldTaskHandle;
static xQueueHandle queue;
static bool module_enabled;
static bool altitudeholdsettings_updated;

// Private functions
static void altitudeHoldTask(void *parameters);
static void SettingsUpdatedCb(UAVObjEvent * ev);

/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t AltitudeHoldStart()
{
	// Start main task if it is enabled
	if (module_enabled) {
		xTaskCreate(altitudeHoldTask, (signed char *)"AltitudeHold", STACK_SIZE_BYTES/4, NULL, TASK_PRIORITY, &altitudeHoldTaskHandle);
		TaskMonitorAdd(TASKINFO_RUNNING_ALTITUDEHOLD, altitudeHoldTaskHandle);
		return 0;
	}
	return -1;

}

/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t AltitudeHoldInitialize()
{
#ifdef MODULE_AltitudeHold_BUILTIN
	module_enabled = true;
#else
	uint8_t module_state[MODULESETTINGS_ADMINSTATE_NUMELEM];
	ModuleSettingsAdminStateGet(module_state);
	if (module_state[MODULESETTINGS_ADMINSTATE_ALTITUDEHOLD] == MODULESETTINGS_ADMINSTATE_ENABLED) {
		module_enabled = true;
	} else {
		module_enabled = false;
	}
#endif

	if(module_enabled) {
		AltitudeHoldSettingsInitialize();
		AltitudeHoldDesiredInitialize();
		AltHoldSmoothedInitialize();

		// Create object queue
		queue = xQueueCreate(MAX_QUEUE_SIZE, sizeof(UAVObjEvent));

		AltitudeHoldSettingsConnectCallback(&SettingsUpdatedCb);

		return 0;
	}

	return -1;
}
MODULE_INITCALL(AltitudeHoldInitialize, AltitudeHoldStart);

/**
 * Module thread, should not return.
 */
static void altitudeHoldTask(void *parameters)
{
	enum init_state {WAITING_BARO, WAITIING_INIT, INITED} init = WAITING_BARO;
	bool running = false;
	bool baro_updated = false;
	float starting_altitude;
	float throttleIntegral;
	float error;
	float smoothed_altitude;	

	AltitudeHoldDesiredData altitudeHoldDesired;
	StabilizationDesiredData stabilizationDesired;
	AltitudeHoldSettingsData altitudeHoldSettings;

	portTickType this_time_ms;
	portTickType last_update_time_ms = xTaskGetTickCount() * portTICK_RATE_MS;
	UAVObjEvent ev;

	// Listen for object updates.
	AltitudeHoldDesiredConnectQueue(queue);
	BaroAltitudeConnectQueue(queue);
	FlightStatusConnectQueue(queue);
	AccelsConnectQueue(queue);

	AltitudeHoldSettingsGet(&altitudeHoldSettings);
	BaroAltitudeAltitudeGet(&smoothed_altitude);
	starting_altitude = smoothed_altitude;

	altitudeholdsettings_updated = true;

	// Let the system start up.  For some reason this is required ot prevent
	// occasional failures to initialize.
	vTaskDelay(100);

	// Main task loop
	while (1) {
		// Wait until the sensors are updated, if a timeout then go to failsafe
		if ( xQueueReceive(queue, &ev, 100 / portTICK_RATE_MS) != pdTRUE )
		{
			if(!running)
				throttleIntegral = 0;

			// Todo: Add alarm if it should be running
			continue;
		} else if (ev.obj == BaroAltitudeHandle()) {
			baro_updated = true;

			init = (init == WAITING_BARO) ? WAITIING_INIT : init;
		} else if (ev.obj == FlightStatusHandle()) {
			FlightStatusData flightStatus;
			FlightStatusGet(&flightStatus);

			if(flightStatus.FlightMode == FLIGHTSTATUS_FLIGHTMODE_ALTITUDEHOLD && !running) {
				// Copy the current throttle as a starting point for integral
				StabilizationDesiredThrottleGet(&throttleIntegral);
				error = 0;
				running = true;

				AltHoldSmoothedData altHold;
				AltHoldSmoothedGet(&altHold);
				starting_altitude = altHold.Altitude;
			} else if (flightStatus.FlightMode != FLIGHTSTATUS_FLIGHTMODE_ALTITUDEHOLD)
				running = false;
		} else if (ev.obj == AccelsHandle()) {
			if (init == WAITING_BARO)
				continue;

			static uint32_t timeval;

			static float z[4] = {0, 0, 0, 0};
			float z_new[4];
			float P[4][4], K[4][2], x[2];
			float G[4] = {1.0e-15f, 1.0e-15f, 1.0e-3f, 1.0e-7};
			static float V[4][4] = {{10.0f, 0, 0, 0}, {0, 100.0f, 0, 0}, {0, 0, 100.0f, 0}, {0, 0, 0, 1000.0f}};
			static uint32_t accel_downsample_count = 0;
			static float accels_accum[3] = {0,0,0};
			float dT;
			static float S[2] = {1.0f,10.0f};

			AccelsData accels;
			AccelsGet(&accels);

			/* Downsample accels to stop this calculation consuming too much CPU */
			accels_accum[0] += accels.x;
			accels_accum[1] += accels.y;
			accels_accum[2] += accels.z;
			accel_downsample_count++;

			if (accel_downsample_count < ACCEL_DOWNSAMPLE)
				continue;

			accel_downsample_count = 0;
			accels.x = accels_accum[0] / ACCEL_DOWNSAMPLE;
			accels.y = accels_accum[1] / ACCEL_DOWNSAMPLE;
			accels.z = accels_accum[2] / ACCEL_DOWNSAMPLE;
			accels_accum[0] = accels_accum[1] = accels_accum[2] = 0;

			this_time_ms = xTaskGetTickCount() * portTICK_RATE_MS;

			AttitudeActualData attitudeActual;
			AttitudeActualGet(&attitudeActual);
			BaroAltitudeData baro;
			BaroAltitudeGet(&baro);

			if (init == WAITIING_INIT) {
				z[0] = baro.Altitude;
				z[1] = 0;
				z[2] = accels.z;
				z[3] = 0;
				init = INITED;
			} else if (init == WAITING_BARO)
				continue;

			if (altitudeholdsettings_updated) {
				AltitudeHoldSettingsGet(&altitudeHoldSettings);
				altitudeholdsettings_updated = false;
			}
			S[0] = altitudeHoldSettings.PressureNoise;
			S[1] = altitudeHoldSettings.AccelNoise;
			G[2] = altitudeHoldSettings.AccelDrift;

			if (S[0] == 0 || S[1] == 0 || G[2] == 0) {
				altitudeholdsettings_updated = true;
				continue;
			}

			float Rbe[3][3];
			Quaternion2R(&attitudeActual.q1, Rbe);
			x[0] = baro.Altitude;
			x[1] = -(Rbe[0][2]*accels.x+ Rbe[1][2]*accels.y + Rbe[2][2]*accels.z + GRAVITY);

			dT = PIOS_DELAY_DiffuS(timeval) / 1.0e6f;
			timeval = PIOS_DELAY_GetRaw();

			P[0][0] = dT*(V[0][1]+dT*V[1][1])+V[0][0]+G[0]+dT*V[1][0];
			P[0][1] = dT*(V[0][2]+dT*V[1][2])+V[0][1]+dT*V[1][1];
			P[0][2] = V[0][2]+dT*V[1][2];
			P[0][3] = V[0][3]+dT*V[1][3];
			P[1][0] = dT*(V[1][1]+dT*V[2][1])+V[1][0]+dT*V[2][0];
			P[1][1] = dT*(V[1][2]+dT*V[2][2])+V[1][1]+G[1]+dT*V[2][1];
			P[1][2] = V[1][2]+dT*V[2][2];
			P[1][3] = V[1][3]+dT*V[2][3];
			P[2][0] = V[2][0]+dT*V[2][1];
			P[2][1] = V[2][1]+dT*V[2][2];
			P[2][2] = V[2][2]+G[2];
			P[2][3] = V[2][3];
			P[3][0] = V[3][0]+dT*V[3][1];
			P[3][1] = V[3][1]+dT*V[3][2];
			P[3][2] = V[3][2];
			P[3][3] = V[3][3]+G[3];

			if (baro_updated) {
				K[0][0] = -(V[2][2]*S[0]+V[2][3]*S[0]+V[3][2]*S[0]+V[3][3]*S[0]+G[2]*S[0]+G[3]*S[0]+S[0]*S[1])/(V[0][0]*G[2]+V[0][0]*G[3]+V[2][2]*G[0]+V[2][3]*G[0]+V[3][2]*G[0]+V[3][3]*G[0]+V[0][0]*S[1]+V[2][2]*S[0]+V[2][3]*S[0]+V[3][2]*S[0]+V[3][3]*S[0]+V[0][0]*V[2][2]-V[0][2]*V[2][0]+V[0][0]*V[2][3]+V[0][0]*V[3][2]-V[0][2]*V[3][0]-V[2][0]*V[0][3]+V[0][0]*V[3][3]-V[0][3]*V[3][0]+G[0]*G[2]+G[0]*G[3]+G[0]*S[1]+G[2]*S[0]+G[3]*S[0]+S[0]*S[1]+(dT*dT)*V[1][1]*V[2][2]-(dT*dT)*V[1][2]*V[2][1]+(dT*dT)*V[1][1]*V[2][3]+(dT*dT)*V[1][1]*V[3][2]-(dT*dT)*V[1][2]*V[3][1]-(dT*dT)*V[2][1]*V[1][3]+(dT*dT)*V[1][1]*V[3][3]-(dT*dT)*V[1][3]*V[3][1]+dT*V[0][1]*G[2]+dT*V[1][0]*G[2]+dT*V[0][1]*G[3]+dT*V[1][0]*G[3]+dT*V[0][1]*S[1]+dT*V[1][0]*S[1]+(dT*dT)*V[1][1]*G[2]+(dT*dT)*V[1][1]*G[3]+(dT*dT)*V[1][1]*S[1]+dT*V[0][1]*V[2][2]+dT*V[1][0]*V[2][2]-dT*V[0][2]*V[2][1]-dT*V[2][0]*V[1][2]+dT*V[0][1]*V[2][3]+dT*V[0][1]*V[3][2]+dT*V[1][0]*V[2][3]+dT*V[1][0]*V[3][2]-dT*V[0][2]*V[3][1]-dT*V[2][0]*V[1][3]-dT*V[0][3]*V[2][1]-dT*V[1][2]*V[3][0]+dT*V[0][1]*V[3][3]+dT*V[1][0]*V[3][3]-dT*V[0][3]*V[3][1]-dT*V[3][0]*V[1][3])+1.0f;
				K[0][1] = ((V[0][2]+V[0][3])*S[0]+dT*(V[1][2]+V[1][3])*S[0])/(V[0][0]*G[2]+V[0][0]*G[3]+V[2][2]*G[0]+V[2][3]*G[0]+V[3][2]*G[0]+V[3][3]*G[0]+V[0][0]*S[1]+V[2][2]*S[0]+V[2][3]*S[0]+V[3][2]*S[0]+V[3][3]*S[0]+V[0][0]*V[2][2]-V[0][2]*V[2][0]+V[0][0]*V[2][3]+V[0][0]*V[3][2]-V[0][2]*V[3][0]-V[2][0]*V[0][3]+V[0][0]*V[3][3]-V[0][3]*V[3][0]+G[0]*G[2]+G[0]*G[3]+G[0]*S[1]+G[2]*S[0]+G[3]*S[0]+S[0]*S[1]+(dT*dT)*V[1][1]*V[2][2]-(dT*dT)*V[1][2]*V[2][1]+(dT*dT)*V[1][1]*V[2][3]+(dT*dT)*V[1][1]*V[3][2]-(dT*dT)*V[1][2]*V[3][1]-(dT*dT)*V[2][1]*V[1][3]+(dT*dT)*V[1][1]*V[3][3]-(dT*dT)*V[1][3]*V[3][1]+dT*V[0][1]*G[2]+dT*V[1][0]*G[2]+dT*V[0][1]*G[3]+dT*V[1][0]*G[3]+dT*V[0][1]*S[1]+dT*V[1][0]*S[1]+(dT*dT)*V[1][1]*G[2]+(dT*dT)*V[1][1]*G[3]+(dT*dT)*V[1][1]*S[1]+dT*V[0][1]*V[2][2]+dT*V[1][0]*V[2][2]-dT*V[0][2]*V[2][1]-dT*V[2][0]*V[1][2]+dT*V[0][1]*V[2][3]+dT*V[0][1]*V[3][2]+dT*V[1][0]*V[2][3]+dT*V[1][0]*V[3][2]-dT*V[0][2]*V[3][1]-dT*V[2][0]*V[1][3]-dT*V[0][3]*V[2][1]-dT*V[1][2]*V[3][0]+dT*V[0][1]*V[3][3]+dT*V[1][0]*V[3][3]-dT*V[0][3]*V[3][1]-dT*V[3][0]*V[1][3]);
				K[1][0] = (V[1][0]*G[2]+V[1][0]*G[3]+V[1][0]*S[1]+V[1][0]*V[2][2]-V[2][0]*V[1][2]+V[1][0]*V[2][3]+V[1][0]*V[3][2]-V[2][0]*V[1][3]-V[1][2]*V[3][0]+V[1][0]*V[3][3]-V[3][0]*V[1][3]+(dT*dT)*V[2][1]*V[3][2]-(dT*dT)*V[2][2]*V[3][1]+(dT*dT)*V[2][1]*V[3][3]-(dT*dT)*V[3][1]*V[2][3]+dT*V[1][1]*G[2]+dT*V[2][0]*G[2]+dT*V[1][1]*G[3]+dT*V[2][0]*G[3]+dT*V[1][1]*S[1]+dT*V[2][0]*S[1]+(dT*dT)*V[2][1]*G[2]+(dT*dT)*V[2][1]*G[3]+(dT*dT)*V[2][1]*S[1]+dT*V[1][1]*V[2][2]-dT*V[1][2]*V[2][1]+dT*V[1][1]*V[2][3]+dT*V[1][1]*V[3][2]+dT*V[2][0]*V[3][2]-dT*V[1][2]*V[3][1]-dT*V[2][1]*V[1][3]-dT*V[3][0]*V[2][2]+dT*V[1][1]*V[3][3]+dT*V[2][0]*V[3][3]-dT*V[3][0]*V[2][3]-dT*V[1][3]*V[3][1])/(V[0][0]*G[2]+V[0][0]*G[3]+V[2][2]*G[0]+V[2][3]*G[0]+V[3][2]*G[0]+V[3][3]*G[0]+V[0][0]*S[1]+V[2][2]*S[0]+V[2][3]*S[0]+V[3][2]*S[0]+V[3][3]*S[0]+V[0][0]*V[2][2]-V[0][2]*V[2][0]+V[0][0]*V[2][3]+V[0][0]*V[3][2]-V[0][2]*V[3][0]-V[2][0]*V[0][3]+V[0][0]*V[3][3]-V[0][3]*V[3][0]+G[0]*G[2]+G[0]*G[3]+G[0]*S[1]+G[2]*S[0]+G[3]*S[0]+S[0]*S[1]+(dT*dT)*V[1][1]*V[2][2]-(dT*dT)*V[1][2]*V[2][1]+(dT*dT)*V[1][1]*V[2][3]+(dT*dT)*V[1][1]*V[3][2]-(dT*dT)*V[1][2]*V[3][1]-(dT*dT)*V[2][1]*V[1][3]+(dT*dT)*V[1][1]*V[3][3]-(dT*dT)*V[1][3]*V[3][1]+dT*V[0][1]*G[2]+dT*V[1][0]*G[2]+dT*V[0][1]*G[3]+dT*V[1][0]*G[3]+dT*V[0][1]*S[1]+dT*V[1][0]*S[1]+(dT*dT)*V[1][1]*G[2]+(dT*dT)*V[1][1]*G[3]+(dT*dT)*V[1][1]*S[1]+dT*V[0][1]*V[2][2]+dT*V[1][0]*V[2][2]-dT*V[0][2]*V[2][1]-dT*V[2][0]*V[1][2]+dT*V[0][1]*V[2][3]+dT*V[0][1]*V[3][2]+dT*V[1][0]*V[2][3]+dT*V[1][0]*V[3][2]-dT*V[0][2]*V[3][1]-dT*V[2][0]*V[1][3]-dT*V[0][3]*V[2][1]-dT*V[1][2]*V[3][0]+dT*V[0][1]*V[3][3]+dT*V[1][0]*V[3][3]-dT*V[0][3]*V[3][1]-dT*V[3][0]*V[1][3]);
				K[1][1] = (V[1][2]*G[0]+V[1][3]*G[0]+V[1][2]*S[0]+V[1][3]*S[0]+V[0][0]*V[1][2]-V[1][0]*V[0][2]+V[0][0]*V[1][3]-V[1][0]*V[0][3]+(dT*dT)*V[0][1]*V[2][2]+(dT*dT)*V[1][0]*V[2][2]-(dT*dT)*V[0][2]*V[2][1]-(dT*dT)*V[2][0]*V[1][2]+(dT*dT)*V[0][1]*V[2][3]+(dT*dT)*V[1][0]*V[2][3]-(dT*dT)*V[2][0]*V[1][3]-(dT*dT)*V[0][3]*V[2][1]+(dT*dT*dT)*V[1][1]*V[2][2]-(dT*dT*dT)*V[1][2]*V[2][1]+(dT*dT*dT)*V[1][1]*V[2][3]-(dT*dT*dT)*V[2][1]*V[1][3]+dT*V[2][2]*G[0]+dT*V[2][3]*G[0]+dT*V[2][2]*S[0]+dT*V[2][3]*S[0]+dT*V[0][0]*V[2][2]+dT*V[0][1]*V[1][2]-dT*V[0][2]*V[1][1]-dT*V[0][2]*V[2][0]+dT*V[0][0]*V[2][3]+dT*V[0][1]*V[1][3]-dT*V[1][1]*V[0][3]-dT*V[2][0]*V[0][3])/(V[0][0]*G[2]+V[0][0]*G[3]+V[2][2]*G[0]+V[2][3]*G[0]+V[3][2]*G[0]+V[3][3]*G[0]+V[0][0]*S[1]+V[2][2]*S[0]+V[2][3]*S[0]+V[3][2]*S[0]+V[3][3]*S[0]+V[0][0]*V[2][2]-V[0][2]*V[2][0]+V[0][0]*V[2][3]+V[0][0]*V[3][2]-V[0][2]*V[3][0]-V[2][0]*V[0][3]+V[0][0]*V[3][3]-V[0][3]*V[3][0]+G[0]*G[2]+G[0]*G[3]+G[0]*S[1]+G[2]*S[0]+G[3]*S[0]+S[0]*S[1]+(dT*dT)*V[1][1]*V[2][2]-(dT*dT)*V[1][2]*V[2][1]+(dT*dT)*V[1][1]*V[2][3]+(dT*dT)*V[1][1]*V[3][2]-(dT*dT)*V[1][2]*V[3][1]-(dT*dT)*V[2][1]*V[1][3]+(dT*dT)*V[1][1]*V[3][3]-(dT*dT)*V[1][3]*V[3][1]+dT*V[0][1]*G[2]+dT*V[1][0]*G[2]+dT*V[0][1]*G[3]+dT*V[1][0]*G[3]+dT*V[0][1]*S[1]+dT*V[1][0]*S[1]+(dT*dT)*V[1][1]*G[2]+(dT*dT)*V[1][1]*G[3]+(dT*dT)*V[1][1]*S[1]+dT*V[0][1]*V[2][2]+dT*V[1][0]*V[2][2]-dT*V[0][2]*V[2][1]-dT*V[2][0]*V[1][2]+dT*V[0][1]*V[2][3]+dT*V[0][1]*V[3][2]+dT*V[1][0]*V[2][3]+dT*V[1][0]*V[3][2]-dT*V[0][2]*V[3][1]-dT*V[2][0]*V[1][3]-dT*V[0][3]*V[2][1]-dT*V[1][2]*V[3][0]+dT*V[0][1]*V[3][3]+dT*V[1][0]*V[3][3]-dT*V[0][3]*V[3][1]-dT*V[3][0]*V[1][3]);
				K[2][0] = (V[2][0]*G[3]-V[3][0]*G[2]+V[2][0]*S[1]+V[2][0]*V[3][2]-V[3][0]*V[2][2]+V[2][0]*V[3][3]-V[3][0]*V[2][3]+dT*V[2][1]*G[3]-dT*V[3][1]*G[2]+dT*V[2][1]*S[1]+dT*V[2][1]*V[3][2]-dT*V[2][2]*V[3][1]+dT*V[2][1]*V[3][3]-dT*V[3][1]*V[2][3])/(V[0][0]*G[2]+V[0][0]*G[3]+V[2][2]*G[0]+V[2][3]*G[0]+V[3][2]*G[0]+V[3][3]*G[0]+V[0][0]*S[1]+V[2][2]*S[0]+V[2][3]*S[0]+V[3][2]*S[0]+V[3][3]*S[0]+V[0][0]*V[2][2]-V[0][2]*V[2][0]+V[0][0]*V[2][3]+V[0][0]*V[3][2]-V[0][2]*V[3][0]-V[2][0]*V[0][3]+V[0][0]*V[3][3]-V[0][3]*V[3][0]+G[0]*G[2]+G[0]*G[3]+G[0]*S[1]+G[2]*S[0]+G[3]*S[0]+S[0]*S[1]+(dT*dT)*V[1][1]*V[2][2]-(dT*dT)*V[1][2]*V[2][1]+(dT*dT)*V[1][1]*V[2][3]+(dT*dT)*V[1][1]*V[3][2]-(dT*dT)*V[1][2]*V[3][1]-(dT*dT)*V[2][1]*V[1][3]+(dT*dT)*V[1][1]*V[3][3]-(dT*dT)*V[1][3]*V[3][1]+dT*V[0][1]*G[2]+dT*V[1][0]*G[2]+dT*V[0][1]*G[3]+dT*V[1][0]*G[3]+dT*V[0][1]*S[1]+dT*V[1][0]*S[1]+(dT*dT)*V[1][1]*G[2]+(dT*dT)*V[1][1]*G[3]+(dT*dT)*V[1][1]*S[1]+dT*V[0][1]*V[2][2]+dT*V[1][0]*V[2][2]-dT*V[0][2]*V[2][1]-dT*V[2][0]*V[1][2]+dT*V[0][1]*V[2][3]+dT*V[0][1]*V[3][2]+dT*V[1][0]*V[2][3]+dT*V[1][0]*V[3][2]-dT*V[0][2]*V[3][1]-dT*V[2][0]*V[1][3]-dT*V[0][3]*V[2][1]-dT*V[1][2]*V[3][0]+dT*V[0][1]*V[3][3]+dT*V[1][0]*V[3][3]-dT*V[0][3]*V[3][1]-dT*V[3][0]*V[1][3]);
				K[2][1] = (V[0][0]*G[2]+V[2][2]*G[0]+V[2][3]*G[0]+V[2][2]*S[0]+V[2][3]*S[0]+V[0][0]*V[2][2]-V[0][2]*V[2][0]+V[0][0]*V[2][3]-V[2][0]*V[0][3]+G[0]*G[2]+G[2]*S[0]+(dT*dT)*V[1][1]*V[2][2]-(dT*dT)*V[1][2]*V[2][1]+(dT*dT)*V[1][1]*V[2][3]-(dT*dT)*V[2][1]*V[1][3]+dT*V[0][1]*G[2]+dT*V[1][0]*G[2]+(dT*dT)*V[1][1]*G[2]+dT*V[0][1]*V[2][2]+dT*V[1][0]*V[2][2]-dT*V[0][2]*V[2][1]-dT*V[2][0]*V[1][2]+dT*V[0][1]*V[2][3]+dT*V[1][0]*V[2][3]-dT*V[2][0]*V[1][3]-dT*V[0][3]*V[2][1])/(V[0][0]*G[2]+V[0][0]*G[3]+V[2][2]*G[0]+V[2][3]*G[0]+V[3][2]*G[0]+V[3][3]*G[0]+V[0][0]*S[1]+V[2][2]*S[0]+V[2][3]*S[0]+V[3][2]*S[0]+V[3][3]*S[0]+V[0][0]*V[2][2]-V[0][2]*V[2][0]+V[0][0]*V[2][3]+V[0][0]*V[3][2]-V[0][2]*V[3][0]-V[2][0]*V[0][3]+V[0][0]*V[3][3]-V[0][3]*V[3][0]+G[0]*G[2]+G[0]*G[3]+G[0]*S[1]+G[2]*S[0]+G[3]*S[0]+S[0]*S[1]+(dT*dT)*V[1][1]*V[2][2]-(dT*dT)*V[1][2]*V[2][1]+(dT*dT)*V[1][1]*V[2][3]+(dT*dT)*V[1][1]*V[3][2]-(dT*dT)*V[1][2]*V[3][1]-(dT*dT)*V[2][1]*V[1][3]+(dT*dT)*V[1][1]*V[3][3]-(dT*dT)*V[1][3]*V[3][1]+dT*V[0][1]*G[2]+dT*V[1][0]*G[2]+dT*V[0][1]*G[3]+dT*V[1][0]*G[3]+dT*V[0][1]*S[1]+dT*V[1][0]*S[1]+(dT*dT)*V[1][1]*G[2]+(dT*dT)*V[1][1]*G[3]+(dT*dT)*V[1][1]*S[1]+dT*V[0][1]*V[2][2]+dT*V[1][0]*V[2][2]-dT*V[0][2]*V[2][1]-dT*V[2][0]*V[1][2]+dT*V[0][1]*V[2][3]+dT*V[0][1]*V[3][2]+dT*V[1][0]*V[2][3]+dT*V[1][0]*V[3][2]-dT*V[0][2]*V[3][1]-dT*V[2][0]*V[1][3]-dT*V[0][3]*V[2][1]-dT*V[1][2]*V[3][0]+dT*V[0][1]*V[3][3]+dT*V[1][0]*V[3][3]-dT*V[0][3]*V[3][1]-dT*V[3][0]*V[1][3]);
				K[3][0] = (-V[2][0]*G[3]+V[3][0]*G[2]+V[3][0]*S[1]-V[2][0]*V[3][2]+V[3][0]*V[2][2]-V[2][0]*V[3][3]+V[3][0]*V[2][3]-dT*V[2][1]*G[3]+dT*V[3][1]*G[2]+dT*V[3][1]*S[1]-dT*V[2][1]*V[3][2]+dT*V[2][2]*V[3][1]-dT*V[2][1]*V[3][3]+dT*V[3][1]*V[2][3])/(V[0][0]*G[2]+V[0][0]*G[3]+V[2][2]*G[0]+V[2][3]*G[0]+V[3][2]*G[0]+V[3][3]*G[0]+V[0][0]*S[1]+V[2][2]*S[0]+V[2][3]*S[0]+V[3][2]*S[0]+V[3][3]*S[0]+V[0][0]*V[2][2]-V[0][2]*V[2][0]+V[0][0]*V[2][3]+V[0][0]*V[3][2]-V[0][2]*V[3][0]-V[2][0]*V[0][3]+V[0][0]*V[3][3]-V[0][3]*V[3][0]+G[0]*G[2]+G[0]*G[3]+G[0]*S[1]+G[2]*S[0]+G[3]*S[0]+S[0]*S[1]+(dT*dT)*V[1][1]*V[2][2]-(dT*dT)*V[1][2]*V[2][1]+(dT*dT)*V[1][1]*V[2][3]+(dT*dT)*V[1][1]*V[3][2]-(dT*dT)*V[1][2]*V[3][1]-(dT*dT)*V[2][1]*V[1][3]+(dT*dT)*V[1][1]*V[3][3]-(dT*dT)*V[1][3]*V[3][1]+dT*V[0][1]*G[2]+dT*V[1][0]*G[2]+dT*V[0][1]*G[3]+dT*V[1][0]*G[3]+dT*V[0][1]*S[1]+dT*V[1][0]*S[1]+(dT*dT)*V[1][1]*G[2]+(dT*dT)*V[1][1]*G[3]+(dT*dT)*V[1][1]*S[1]+dT*V[0][1]*V[2][2]+dT*V[1][0]*V[2][2]-dT*V[0][2]*V[2][1]-dT*V[2][0]*V[1][2]+dT*V[0][1]*V[2][3]+dT*V[0][1]*V[3][2]+dT*V[1][0]*V[2][3]+dT*V[1][0]*V[3][2]-dT*V[0][2]*V[3][1]-dT*V[2][0]*V[1][3]-dT*V[0][3]*V[2][1]-dT*V[1][2]*V[3][0]+dT*V[0][1]*V[3][3]+dT*V[1][0]*V[3][3]-dT*V[0][3]*V[3][1]-dT*V[3][0]*V[1][3]);
				K[3][1] = (V[0][0]*G[3]+V[3][2]*G[0]+V[3][3]*G[0]+V[3][2]*S[0]+V[3][3]*S[0]+V[0][0]*V[3][2]-V[0][2]*V[3][0]+V[0][0]*V[3][3]-V[0][3]*V[3][0]+G[0]*G[3]+G[3]*S[0]+(dT*dT)*V[1][1]*V[3][2]-(dT*dT)*V[1][2]*V[3][1]+(dT*dT)*V[1][1]*V[3][3]-(dT*dT)*V[1][3]*V[3][1]+dT*V[0][1]*G[3]+dT*V[1][0]*G[3]+(dT*dT)*V[1][1]*G[3]+dT*V[0][1]*V[3][2]+dT*V[1][0]*V[3][2]-dT*V[0][2]*V[3][1]-dT*V[1][2]*V[3][0]+dT*V[0][1]*V[3][3]+dT*V[1][0]*V[3][3]-dT*V[0][3]*V[3][1]-dT*V[3][0]*V[1][3])/(V[0][0]*G[2]+V[0][0]*G[3]+V[2][2]*G[0]+V[2][3]*G[0]+V[3][2]*G[0]+V[3][3]*G[0]+V[0][0]*S[1]+V[2][2]*S[0]+V[2][3]*S[0]+V[3][2]*S[0]+V[3][3]*S[0]+V[0][0]*V[2][2]-V[0][2]*V[2][0]+V[0][0]*V[2][3]+V[0][0]*V[3][2]-V[0][2]*V[3][0]-V[2][0]*V[0][3]+V[0][0]*V[3][3]-V[0][3]*V[3][0]+G[0]*G[2]+G[0]*G[3]+G[0]*S[1]+G[2]*S[0]+G[3]*S[0]+S[0]*S[1]+(dT*dT)*V[1][1]*V[2][2]-(dT*dT)*V[1][2]*V[2][1]+(dT*dT)*V[1][1]*V[2][3]+(dT*dT)*V[1][1]*V[3][2]-(dT*dT)*V[1][2]*V[3][1]-(dT*dT)*V[2][1]*V[1][3]+(dT*dT)*V[1][1]*V[3][3]-(dT*dT)*V[1][3]*V[3][1]+dT*V[0][1]*G[2]+dT*V[1][0]*G[2]+dT*V[0][1]*G[3]+dT*V[1][0]*G[3]+dT*V[0][1]*S[1]+dT*V[1][0]*S[1]+(dT*dT)*V[1][1]*G[2]+(dT*dT)*V[1][1]*G[3]+(dT*dT)*V[1][1]*S[1]+dT*V[0][1]*V[2][2]+dT*V[1][0]*V[2][2]-dT*V[0][2]*V[2][1]-dT*V[2][0]*V[1][2]+dT*V[0][1]*V[2][3]+dT*V[0][1]*V[3][2]+dT*V[1][0]*V[2][3]+dT*V[1][0]*V[3][2]-dT*V[0][2]*V[3][1]-dT*V[2][0]*V[1][3]-dT*V[0][3]*V[2][1]-dT*V[1][2]*V[3][0]+dT*V[0][1]*V[3][3]+dT*V[1][0]*V[3][3]-dT*V[0][3]*V[3][1]-dT*V[3][0]*V[1][3]);

				z_new[0] = -K[0][0]*(dT*z[1]-x[0]+z[0])+dT*z[1]-K[0][1]*(-x[1]+z[2]+z[3])+z[0];
				z_new[1] = -K[1][0]*(dT*z[1]-x[0]+z[0])+dT*z[2]-K[1][1]*(-x[1]+z[2]+z[3])+z[1];
				z_new[2] = -K[2][0]*(dT*z[1]-x[0]+z[0])-K[2][1]*(-x[1]+z[2]+z[3])+z[2];
				z_new[3] = -K[3][0]*(dT*z[1]-x[0]+z[0])-K[3][1]*(-x[1]+z[2]+z[3])+z[3];

				memcpy(z, z_new, sizeof(z_new));

				V[0][0] = -K[0][1]*P[2][0]-K[0][1]*P[3][0]-P[0][0]*(K[0][0]-1.0f);
				V[0][1] = -K[0][1]*P[2][1]-K[0][1]*P[3][2]-P[0][1]*(K[0][0]-1.0f);
				V[0][2] = -K[0][1]*P[2][2]-K[0][1]*P[3][2]-P[0][2]*(K[0][0]-1.0f);
				V[0][3] = -K[0][1]*P[2][3]-K[0][1]*P[3][3]-P[0][3]*(K[0][0]-1.0f);
				V[1][0] = P[1][0]-K[1][0]*P[0][0]-K[1][1]*P[2][0]-K[1][1]*P[3][0];
				V[1][1] = P[1][1]-K[1][0]*P[0][1]-K[1][1]*P[2][1]-K[1][1]*P[3][2];
				V[1][2] = P[1][2]-K[1][0]*P[0][2]-K[1][1]*P[2][2]-K[1][1]*P[3][2];
				V[1][3] = P[1][3]-K[1][0]*P[0][3]-K[1][1]*P[2][3]-K[1][1]*P[3][3];
				V[2][0] = -K[2][0]*P[0][0]-K[2][1]*P[3][0]-P[2][0]*(K[2][1]-1.0f);
				V[2][1] = -K[2][0]*P[0][1]-K[2][1]*P[3][2]-P[2][1]*(K[2][1]-1.0f);
				V[2][2] = -K[2][0]*P[0][2]-K[2][1]*P[3][2]-P[2][2]*(K[2][1]-1.0f);
				V[2][3] = -K[2][0]*P[0][3]-K[2][1]*P[3][3]-P[2][3]*(K[2][1]-1.0f);
				V[3][0] = -K[3][0]*P[0][0]-K[3][1]*P[2][0]-P[3][0]*(K[3][1]-1.0f);
				V[3][1] = -K[3][0]*P[0][1]-K[3][1]*P[2][1]-P[3][2]*(K[3][1]-1.0f);
				V[3][2] = -K[3][0]*P[0][2]-K[3][1]*P[2][2]-P[3][2]*(K[3][1]-1.0f);
				V[3][3] = -K[3][0]*P[0][3]-K[3][1]*P[2][3]-P[3][3]*(K[3][1]-1.0f);


				baro_updated = false;
			} else {
				K[0][0] = (V[0][2]+V[0][3]+dT*V[1][2]+dT*V[1][3])/(V[2][2]+V[2][3]+V[3][2]+V[3][3]+G[2]+G[3]+S[1]);
				K[1][0] = (V[1][2]+V[1][3]+dT*V[2][2]+dT*V[2][3])/(V[2][2]+V[2][3]+V[3][2]+V[3][3]+G[2]+G[3]+S[1]);
				K[2][0] = (V[2][2]+V[2][3]+G[2])/(V[2][2]+V[2][3]+V[3][2]+V[3][3]+G[2]+G[3]+S[1]);
				K[3][0] = (V[3][2]+V[3][3]+G[3])/(V[2][2]+V[2][3]+V[3][2]+V[3][3]+G[2]+G[3]+S[1]);


				z_new[0] = dT*z[1]-K[0][0]*(-x[1]+z[2]+z[3])+z[0];
				z_new[1] = dT*z[2]-K[1][0]*(-x[1]+z[2]+z[3])+z[1];
				z_new[2] = -K[2][0]*(-x[1]+z[2]+z[3])+z[2];
				z_new[3] = -K[3][0]*(-x[1]+z[2]+z[3])+z[3];

				memcpy(z, z_new, sizeof(z_new));

				V[0][0] = P[0][0]-K[0][0]*P[2][0]-K[0][0]*P[3][0];
				V[0][1] = P[0][1]-K[0][0]*P[2][1]-K[0][0]*P[3][2];
				V[0][2] = P[0][2]-K[0][0]*P[2][2]-K[0][0]*P[3][2];
				V[0][3] = P[0][3]-K[0][0]*P[2][3]-K[0][0]*P[3][3];
				V[1][0] = P[1][0]-K[1][0]*P[2][0]-K[1][0]*P[3][0];
				V[1][1] = P[1][1]-K[1][0]*P[2][1]-K[1][0]*P[3][2];
				V[1][2] = P[1][2]-K[1][0]*P[2][2]-K[1][0]*P[3][2];
				V[1][3] = P[1][3]-K[1][0]*P[2][3]-K[1][0]*P[3][3];
				V[2][0] = -K[2][0]*P[3][0]-P[2][0]*(K[2][0]-1.0f);
				V[2][1] = -K[2][0]*P[3][2]-P[2][1]*(K[2][0]-1.0f);
				V[2][2] = -K[2][0]*P[3][2]-P[2][2]*(K[2][0]-1.0f);
				V[2][3] = -K[2][0]*P[3][3]-P[2][3]*(K[2][0]-1.0f);
				V[3][0] = -K[3][0]*P[2][0]-P[3][0]*(K[3][0]-1.0f);
				V[3][1] = -K[3][0]*P[2][1]-P[3][2]*(K[3][0]-1.0f);
				V[3][2] = -K[3][0]*P[2][2]-P[3][2]*(K[3][0]-1.0f);
				V[3][3] = -K[3][0]*P[2][3]-P[3][3]*(K[3][0]-1.0f);
			}

			AltHoldSmoothedData altHold;
			AltHoldSmoothedGet(&altHold);
			altHold.Altitude = z[0];
			altHold.Velocity = z[1];
			altHold.Accel = z[2];
			AltHoldSmoothedSet(&altHold);

			AltHoldSmoothedGet(&altHold);

			// Verify that we are  in altitude hold mode
			FlightStatusData flightStatus;
			FlightStatusGet(&flightStatus);
			if(flightStatus.FlightMode != FLIGHTSTATUS_FLIGHTMODE_ALTITUDEHOLD) {
				running = false;
			}

			if (!running)
				continue;

			// Compute the altitude error
			error = (starting_altitude + altitudeHoldDesired.Altitude) - altHold.Altitude;

			// Compute integral off altitude error
			throttleIntegral += error * altitudeHoldSettings.Ki * dT;

			// Only update stabilizationDesired less frequently
			if((this_time_ms - last_update_time_ms) < 20)
				continue;

			last_update_time_ms = this_time_ms;

			// Instead of explicit limit on integral you output limit feedback
			StabilizationDesiredGet(&stabilizationDesired);
			stabilizationDesired.Throttle = error * altitudeHoldSettings.Kp + throttleIntegral -
			altHold.Velocity * altitudeHoldSettings.Kd - altHold.Accel * altitudeHoldSettings.Ka;
			if(stabilizationDesired.Throttle > 1) {
				throttleIntegral -= (stabilizationDesired.Throttle - 1);
				stabilizationDesired.Throttle = 1;
			}
			else if (stabilizationDesired.Throttle < 0) {
				throttleIntegral -= stabilizationDesired.Throttle;
				stabilizationDesired.Throttle = 0;
			}

			stabilizationDesired.StabilizationMode[STABILIZATIONDESIRED_STABILIZATIONMODE_ROLL] = STABILIZATIONDESIRED_STABILIZATIONMODE_ATTITUDE;
			stabilizationDesired.StabilizationMode[STABILIZATIONDESIRED_STABILIZATIONMODE_PITCH] = STABILIZATIONDESIRED_STABILIZATIONMODE_ATTITUDE;
			stabilizationDesired.StabilizationMode[STABILIZATIONDESIRED_STABILIZATIONMODE_YAW] = STABILIZATIONDESIRED_STABILIZATIONMODE_AXISLOCK;
			stabilizationDesired.Roll = altitudeHoldDesired.Roll;
			stabilizationDesired.Pitch = altitudeHoldDesired.Pitch;
			stabilizationDesired.Yaw = altitudeHoldDesired.Yaw;
			StabilizationDesiredSet(&stabilizationDesired);

		} else if (ev.obj == AltitudeHoldDesiredHandle()) {
			AltitudeHoldDesiredGet(&altitudeHoldDesired);
		}

	}
}

static void SettingsUpdatedCb(UAVObjEvent * ev)
{
	altitudeholdsettings_updated = true;
}
