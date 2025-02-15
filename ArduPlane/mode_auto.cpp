#include "mode.h"
#include "Plane.h"

bool ModeAuto::_enter()
{
#if HAL_QUADPLANE_ENABLED
    if (plane.quadplane.available() && plane.quadplane.enable == 2) {
        plane.auto_state.vtol_mode = true;
    } else {
        plane.auto_state.vtol_mode = false;
    }
#else
    plane.auto_state.vtol_mode = false;
#endif
    plane.next_WP_loc = plane.prev_WP_loc = plane.current_loc;
    // start or resume the mission, based on MIS_AUTORESET
    plane.mission.start_or_resume();

    if (hal.util->was_watchdog_armed()) {
        if (hal.util->persistent_data.waypoint_num != 0) {
            gcs().send_text(MAV_SEVERITY_INFO, "Watchdog: resume WP %u", hal.util->persistent_data.waypoint_num);
            plane.mission.set_current_cmd(hal.util->persistent_data.waypoint_num);
            hal.util->persistent_data.waypoint_num = 0;
        }
    }

#if HAL_SOARING_ENABLED
    plane.g2.soaring_controller.init_cruising();
#endif

    return true;
}

void ModeAuto::_exit()
{
    if (plane.mission.state() == AP_Mission::MISSION_RUNNING) {
        plane.mission.stop();

        bool restart = plane.mission.get_current_nav_cmd().id == MAV_CMD_NAV_LAND;
#if HAL_QUADPLANE_ENABLED
        if (plane.quadplane.is_vtol_land(plane.mission.get_current_nav_cmd().id)) {
            restart = false;
        }
#endif
        if (restart) {
            plane.landing.restart_landing_sequence();
        }
    }
    plane.auto_state.started_flying_in_auto_ms = 0;
}

void ModeAuto::update()
{
    if (plane.mission.state() != AP_Mission::MISSION_RUNNING) {
        // this could happen if AP_Landing::restart_landing_sequence() returns false which would only happen if:
        // restart_landing_sequence() is called when not executing a NAV_LAND or there is no previous nav point
        plane.set_mode(plane.mode_rtl, ModeReason::MISSION_END);
        gcs().send_text(MAV_SEVERITY_INFO, "Aircraft in auto without a running mission");
        return;
    }

    uint16_t nav_cmd_id = plane.mission.get_current_nav_cmd().id;

#if HAL_QUADPLANE_ENABLED
    if (plane.quadplane.in_vtol_auto()) {
        plane.quadplane.control_auto();
        return;
    }
#endif

    if (nav_cmd_id == MAV_CMD_NAV_TAKEOFF ||
        (nav_cmd_id == MAV_CMD_NAV_LAND && plane.flight_stage == AP_Vehicle::FixedWing::FLIGHT_ABORT_LAND)) {
        plane.takeoff_calc_roll();
        plane.takeoff_calc_pitch();
        plane.calc_throttle();
    } else if (nav_cmd_id == MAV_CMD_NAV_LAND) {
        plane.calc_nav_roll();
        plane.calc_nav_pitch();

        // allow landing to restrict the roll limits
        plane.nav_roll_cd = plane.landing.constrain_roll(plane.nav_roll_cd, plane.g.level_roll_limit*100UL);

        if (plane.landing.is_throttle_suppressed()) {
            // if landing is considered complete throttle is never allowed, regardless of landing type
            SRV_Channels::set_output_scaled(SRV_Channel::k_throttle, 0.0);
        } else {
            plane.calc_throttle();
        }
    }
    else if (nav_cmd_id == 50 || nav_cmd_id == 51) {
        plane.SpdHgt_Controller->reset_pitch_I();
        // this is mostly just a copy/paste from mode_groundeffect.cpp because I'm lazy
        float _thr_ff = (plane.g.gndEffect_thr_max + plane.g.gndEffect_thr_min) / 2.f;
        static uint16_t _last_good_reading_mm;
        static uint32_t _last_good_reading_time_ms;
        int16_t _alt_desired_mm = (plane.g.gndEffect_alt_max + plane.g.gndEffect_alt_min) / 2;

        // waypoint type 51 means higher-altitude ground effect mode
        if (nav_cmd_id == 51) {
            _alt_desired_mm *= plane.g.gndefct_51_multiplier;
        }

        if (plane.rangefinder.status_orient(ROTATION_PITCH_270) == RangeFinder::Status::Good) {
            _last_good_reading_mm = plane.rangefinder.distance_mm_orient(ROTATION_PITCH_270);
            _last_good_reading_time_ms = AP_HAL::millis();
        }

        if (AP_HAL::millis() - _last_good_reading_time_ms > 1000) {
            _last_good_reading_mm = plane.g.gndEffect_alt_max;
            if (nav_cmd_id == 51) {
                _last_good_reading_mm *= plane.g.gndefct_51_multiplier;
            }
        }

        int16_t errorMm = _alt_desired_mm - _last_good_reading_mm;
        plane.nav_roll_cd = 0;
        plane.nav_pitch_cd = (int16_t)plane.g2.gndefct_ele.get_pid(errorMm);
        plane.calc_nav_roll();
        // plane.steering_control.rudder = plane.channel_rudder->get_control_in_zero_dz();
        int16_t throttle_command = plane.g2.gndefct_thr.get_pid(errorMm) + _thr_ff;
        int16_t commanded_throttle = constrain_int16(throttle_command, plane.g.gndEffect_thr_min, plane.g.gndEffect_thr_max);
        commanded_throttle = constrain_int16(commanded_throttle, 0, 100);
        SRV_Channels::set_output_scaled(SRV_Channel::k_throttle, commanded_throttle);
#if AP_SCRIPTING_ENABLED
    } else if (nav_cmd_id == MAV_CMD_NAV_SCRIPT_TIME) {
        // NAV_SCRIPTING has a desired roll and pitch rate and desired throttle
        plane.nav_roll_cd = plane.ahrs.roll_sensor;
        plane.nav_pitch_cd = plane.ahrs.pitch_sensor;
        SRV_Channels::set_output_scaled(SRV_Channel::k_throttle, plane.nav_scripting.throttle_pct);
#endif
    } else {
        // we are doing normal AUTO flight, the special cases
        // are for takeoff and landing
        if (nav_cmd_id != MAV_CMD_NAV_CONTINUE_AND_CHANGE_ALT) {
            plane.steer_state.hold_course_cd = -1;
        }
        plane.calc_nav_roll();
        plane.calc_nav_pitch();
        plane.calc_throttle();
    }
}

void ModeAuto::navigate()
{
    if (AP::ahrs().home_is_set()) {
        plane.mission.update();
    }
}


bool ModeAuto::does_auto_navigation() const
{
#if AP_SCRIPTING_ENABLED
   return (!plane.nav_scripting_active());
#endif
   return true;
}

bool ModeAuto::does_auto_throttle() const
{
#if AP_SCRIPTING_ENABLED
   return (!plane.nav_scripting_active());
#endif
   return true;
}
