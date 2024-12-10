/****************************************************************************
 *
 *   Copyright (c) 2024 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file FlightTaskLand.cpp
 */

#include "FlightTaskLand.hpp"

bool
FlightTaskLand::activate(const trajectory_setpoint_s &last_setpoint)
{
	bool ret = FlightTask::activate(last_setpoint);
	Vector3f vel_prev{last_setpoint.velocity};
	Vector3f pos_prev{last_setpoint.position};
	Vector3f accel_prev{last_setpoint.acceleration};

	for (int i = 0; i < 3; i++) {
		// If the position setpoint is unknown, set to the current position
		if (!PX4_ISFINITE(pos_prev(i))) { pos_prev(i) = _position(i); }

		// If the velocity setpoint is unknown, set to the current velocity
		if (!PX4_ISFINITE(vel_prev(i))) { vel_prev(i) = _velocity(i); }

		// If no acceleration estimate available, set to zero if the setpoint is NAN
		if (!PX4_ISFINITE(accel_prev(i))) { accel_prev(i) = 0.f; }


	}

	_yaw_setpoint  = _land_heading = _yaw; // set the yaw setpoint to the current yaw

	_acceleration_setpoint = accel_prev;
	_velocity_setpoint = vel_prev;
	_position_setpoint = pos_prev;

	_stop.initialize(_acceleration_setpoint, _velocity_setpoint, _position, _deltatime);

	_initial_land_position = _land_position = _stop.getStopPosition();

	return ret;
}

void
FlightTaskLand::reActivate()
{
	FlightTask::reActivate();
}

bool
FlightTaskLand::update()
{
	bool ret = FlightTask::update();

	if (!_stop.isActive() && !_landing) {
		_landing = true;
	}

	if (_landing) {
		_PerformLanding();

	} else {
		_PerformBraking();
	}

	return ret;
}

void
FlightTaskLand::_PerformLanding()
{
	// Perform 3 phase landing
	_velocity_setpoint.setNaN();

	// Calculate the vertical speed based on the distance to the ground
	float vertical_speed = math::interpolate(_dist_to_ground,
			       _param_mpc_land_alt2.get(), _param_mpc_land_alt1.get(),
			       _param_mpc_land_speed.get(), _param_mpc_z_vel_max_dn.get());

	bool range_dist_available = PX4_ISFINITE(_dist_to_bottom);

	// If we are below the altitude of the third landing phase , use the crawl speed
	if (range_dist_available && _dist_to_bottom <= _param_mpc_land_alt3.get()) {
		vertical_speed = _param_mpc_land_crawl_speed.get();
	}

	// User input assisted landing
	if (_param_mpc_land_rc_help.get() && _sticks.checkAndUpdateStickInputs()) {
		// Stick full up -1 -> stop, stick full down 1 -> double the speed
		vertical_speed *= (1 - _sticks.getThrottleZeroCenteredExpo());

		if (fabsf(_sticks.getYawExpo()) > FLT_EPSILON) {
			_stick_yaw.generateYawSetpoint(_yawspeed_setpoint, _land_heading, _sticks.getYawExpo(), _yaw, _deltatime);
		}

		Vector2f sticks_xy = _sticks.getPitchRollExpo();
		Vector2f sticks_ne = sticks_xy;
		Sticks::rotateIntoHeadingFrameXY(sticks_ne, _yaw, _land_heading);

		const float distance_to_circle = math::trajectory::getMaxDistanceToCircle(_position.xy(), _initial_land_position.xy(),
						 _param_mpc_land_radius.get(), sticks_ne);
		float max_speed;

		if (PX4_ISFINITE(distance_to_circle)) {
			max_speed = math::trajectory::computeMaxSpeedFromDistance(_stick_acceleration_xy.getMaxJerk(),
					_stick_acceleration_xy.getMaxAcceleration(), distance_to_circle, 0.f);

			if (max_speed < 0.5f) {
				sticks_xy.setZero();
			}

		} else {
			max_speed = 0.f;
			sticks_xy.setZero();
		}

		// If ground distance estimate valid (distance sensor) during nudging then limit horizontal speed
		if (PX4_ISFINITE(_dist_to_bottom)) {
			// Below 50cm no horizontal speed, above allow per meter altitude 0.5m/s speed
			max_speed = math::max(0.f, math::min(max_speed, (_dist_to_bottom - .5f) * .5f));
		}

		_stick_acceleration_xy.setVelocityConstraint(max_speed);
		_stick_acceleration_xy.generateSetpoints(sticks_xy, _yaw, _land_heading, _position,
				_velocity_setpoint_feedback.xy(), _deltatime);
		_stick_acceleration_xy.getSetpoints(_land_position, _velocity_setpoint, _acceleration_setpoint);

	} else {
		// Make sure we have a valid land position even in the case we loose RC while amending it
		if (!PX4_ISFINITE(_land_position(0))) {
			_land_position.xy() = Vector2f(_position);
		}
	}

	_position_setpoint = {_land_position(0), _land_position(1), NAN}; // The last element of the land position has to stay NAN
	_yaw_setpoint = _land_heading;
	_velocity_setpoint(2) = vertical_speed;
	_gear.landing_gear = landing_gear_s::GEAR_DOWN;

}

void
FlightTaskLand::_PerformBraking()
{
	_stop.getConstraints(_constraints);
	_stop.update(_acceleration_setpoint, _velocity_setpoint, _position, _deltatime);

	_initial_land_position = _land_position = _stop.getStopPosition();
	_jerk_setpoint = _stop.getJerkSetpoint();
	_acceleration_setpoint = _stop.getAccelerationSetpoint();
	_velocity_setpoint = _stop.getVelocitySetpoint();
	_position_setpoint = _stop.getPositionSetpoint();
	_yaw_setpoint = _land_heading;
}

void
FlightTaskLand::updateParams()
{
	FlightTask::updateParams();

	// make sure that alt1 is above alt2
	_param_mpc_land_alt1.set(math::max(_param_mpc_land_alt1.get(), _param_mpc_land_alt2.get()));
}
