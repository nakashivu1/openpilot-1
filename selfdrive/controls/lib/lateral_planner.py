import os
import math
import numpy as np
from common.params import Params
from common.realtime import sec_since_boot, DT_MDL
from common.numpy_fast import interp, clip
from selfdrive.swaglog import cloudlog
from selfdrive.controls.lib.lateral_mpc import libmpc_py
from selfdrive.controls.lib.drive_helpers import MPC_COST_LAT, MPC_N, CAR_ROTATION_RADIUS
from selfdrive.controls.lib.lane_planner import LanePlanner, TRAJECTORY_SIZE
from selfdrive.config import Conversions as CV
import cereal.messaging as messaging
from cereal import car, log

LaneChangeState = log.LateralPlan.LaneChangeState
LaneChangeDirection = log.LateralPlan.LaneChangeDirection

LOG_MPC = os.environ.get('LOG_MPC', False)

LANE_CHANGE_SPEED_MIN = float(int(Params().get("OpkrLaneChangeSpeed", encoding="utf8")) * CV.KPH_TO_MS)
LANE_CHANGE_TIME_MAX = 10.
# this corresponds to 80deg/s and 20deg/s steering angle in a toyota corolla
MAX_CURVATURE_RATES = [0.03762194918267951, 0.003441203371932992]
MAX_CURVATURE_RATE_SPEEDS = [0, 35]

DESIRES = {
  LaneChangeDirection.none: {
    LaneChangeState.off: log.LateralPlan.Desire.none,
    LaneChangeState.preLaneChange: log.LateralPlan.Desire.none,
    LaneChangeState.laneChangeStarting: log.LateralPlan.Desire.none,
    LaneChangeState.laneChangeFinishing: log.LateralPlan.Desire.none,
  },
  LaneChangeDirection.left: {
    LaneChangeState.off: log.LateralPlan.Desire.none,
    LaneChangeState.preLaneChange: log.LateralPlan.Desire.none,
    LaneChangeState.laneChangeStarting: log.LateralPlan.Desire.laneChangeLeft,
    LaneChangeState.laneChangeFinishing: log.LateralPlan.Desire.laneChangeLeft,
  },
  LaneChangeDirection.right: {
    LaneChangeState.off: log.LateralPlan.Desire.none,
    LaneChangeState.preLaneChange: log.LateralPlan.Desire.none,
    LaneChangeState.laneChangeStarting: log.LateralPlan.Desire.laneChangeRight,
    LaneChangeState.laneChangeFinishing: log.LateralPlan.Desire.laneChangeRight,
  },
}


class LateralPlanner():
  def __init__(self, CP, use_lanelines=True, wide_camera=False):
    self.use_lanelines = use_lanelines
    self.LP = LanePlanner(wide_camera)

    self.last_cloudlog_t = 0
    self.steer_rate_cost = CP.steerRateCost

    self.setup_mpc()
    self.solution_invalid_cnt = 0

    self.laneless_mode = int(Params().get("LanelessMode", encoding="utf8"))
    self.laneless_mode_status = False
    self.laneless_mode_status_buffer = False
    self.laneless_mode_at_stopping = False
    self.laneless_mode_at_stopping_timer = 0

    self.lane_change_delay = int(Params().get("OpkrAutoLaneChangeDelay", encoding="utf8"))
    self.lane_change_auto_delay = 0.0 if self.lane_change_delay == 0 else 0.2 if self.lane_change_delay == 1 else 0.5 if self.lane_change_delay == 2 \
     else 1.0 if self.lane_change_delay == 3 else 1.5 if self.lane_change_delay == 4 else 2.0

    self.lane_change_wait_timer = 0.0
    self.lane_change_state = LaneChangeState.off
    self.lane_change_direction = LaneChangeDirection.none
    self.lane_change_timer = 0.0
    self.lane_change_ll_prob = 1.0
    self.prev_one_blinker = False
    self.desire = log.LateralPlan.Desire.none

    self.path_xyz = np.zeros((TRAJECTORY_SIZE,3))
    self.path_xyz_stds = np.ones((TRAJECTORY_SIZE,3))
    self.plan_yaw = np.zeros((TRAJECTORY_SIZE,))
    self.t_idxs = np.arange(TRAJECTORY_SIZE)
    self.y_pts = np.zeros(TRAJECTORY_SIZE)

    self.lane_change_adjust = [0.1, 0.17, 0.7, 1.2]
    self.lane_change_adjust_vel = [8.3, 16, 22, 30]
    self.lane_change_adjust_new = 0.0

    self.standstill_elapsed_time = 0.0
    self.v_cruise_kph = 0
    self.stand_still = False
    self.steer_actuator_delay_to_send = 0.0
    
    self.output_scale = 0.0
    self.second = 0.0

  def setup_mpc(self):
    self.libmpc = libmpc_py.libmpc
    self.libmpc.init()

    self.mpc_solution = libmpc_py.ffi.new("log_t *")
    self.cur_state = libmpc_py.ffi.new("state_t *")
    self.cur_state[0].x = 0.0
    self.cur_state[0].y = 0.0
    self.cur_state[0].psi = 0.0
    self.cur_state[0].curvature = 0.0

    self.desired_curvature = 0.0
    self.safe_desired_curvature = 0.0
    self.desired_curvature_rate = 0.0
    self.safe_desired_curvature_rate = 0.0

  def update(self, sm, CP):
    self.second += DT_MDL
    if self.second > 1.0:
      self.use_lanelines = not Params().get_bool("EndToEndToggle")
      self.laneless_mode = int(Params().get("LanelessMode", encoding="utf8"))
      self.second = 0.0
    self.v_cruise_kph = sm['controlsState'].vCruise
    self.stand_still = sm['carState'].standStill
    try:
      if CP.lateralTuning.which() == 'pid':
        self.output_scale = sm['controlsState'].lateralControlState.pidState.output
      elif CP.lateralTuning.which() == 'indi':
        self.output_scale = sm['controlsState'].lateralControlState.indiState.output
      elif CP.lateralTuning.which() == 'lqr':
        self.output_scale = sm['controlsState'].lateralControlState.lqrState.output
    except:
      pass
  
    v_ego = sm['carState'].vEgo
    active = sm['controlsState'].active
    measured_curvature = sm['controlsState'].curvature

    md = sm['modelV2']
    self.LP.parse_model(sm['modelV2'], sm, v_ego)
    if len(md.position.x) == TRAJECTORY_SIZE and len(md.orientation.x) == TRAJECTORY_SIZE:
      self.path_xyz = np.column_stack([md.position.x, md.position.y, md.position.z])
      self.t_idxs = np.array(md.position.t)
      self.plan_yaw = list(md.orientation.z)
    if len(md.orientation.xStd) == TRAJECTORY_SIZE:
      self.path_xyz_stds = np.column_stack([md.position.xStd, md.position.yStd, md.position.zStd])

    '''
    # Lane change logic
    one_blinker = sm['carState'].leftBlinker != sm['carState'].rightBlinker
    below_lane_change_speed = v_ego < LANE_CHANGE_SPEED_MIN

    if sm['carState'].leftBlinker:
      self.lane_change_direction = LaneChangeDirection.left
    elif sm['carState'].rightBlinker:
      self.lane_change_direction = LaneChangeDirection.right

    if (not active) or (self.lane_change_timer > LANE_CHANGE_TIME_MAX) or (abs(self.output_scale) >= (CP.steerMaxV[0]-0.15) and self.lane_change_timer > 1):
      self.lane_change_state = LaneChangeState.off
      self.lane_change_direction = LaneChangeDirection.none
    else:
      torque_applied = sm['carState'].steeringPressed and \
                       ((sm['carState'].steeringTorque > 0 and self.lane_change_direction == LaneChangeDirection.left) or
                        (sm['carState'].steeringTorque < 0 and self.lane_change_direction == LaneChangeDirection.right))

      blindspot_detected = ((sm['carState'].leftBlindspot and self.lane_change_direction == LaneChangeDirection.left) or
                            (sm['carState'].rightBlindspot and self.lane_change_direction == LaneChangeDirection.right))

      lane_change_prob = self.LP.l_lane_change_prob + self.LP.r_lane_change_prob

      # State transitions
      # off
      if self.lane_change_state == LaneChangeState.off and one_blinker and not self.prev_one_blinker and not below_lane_change_speed:
        self.lane_change_state = LaneChangeState.preLaneChange
        self.lane_change_ll_prob = 1.0
        self.lane_change_wait_timer = 0

      # pre
      elif self.lane_change_state == LaneChangeState.preLaneChange:
        self.lane_change_wait_timer += DT_MDL
        if not one_blinker or below_lane_change_speed:
          self.lane_change_state = LaneChangeState.off
        elif not blindspot_detected and (torque_applied or (self.lane_change_auto_delay and self.lane_change_wait_timer > self.lane_change_auto_delay)):
          self.lane_change_state = LaneChangeState.laneChangeStarting

      # starting
      elif self.lane_change_state == LaneChangeState.laneChangeStarting:
        # fade out over .5s
        self.lane_change_adjust_new = interp(v_ego, self.lane_change_adjust_vel, self.lane_change_adjust)
        self.lane_change_ll_prob = max(self.lane_change_ll_prob - self.lane_change_adjust_new*DT_MDL, 0.0)
        # 98% certainty
        if lane_change_prob < 0.03 and self.lane_change_ll_prob < 0.02:
          self.lane_change_state = LaneChangeState.laneChangeFinishing

      # finishing
      elif self.lane_change_state == LaneChangeState.laneChangeFinishing:
        # fade in laneline over 1s
        self.lane_change_ll_prob = min(self.lane_change_ll_prob + DT_MDL, 1.0)
        if one_blinker and self.lane_change_ll_prob > 0.98:
          self.lane_change_state = LaneChangeState.preLaneChange
        elif self.lane_change_ll_prob > 0.98:
          self.lane_change_state = LaneChangeState.off

    if self.lane_change_state in [LaneChangeState.off, LaneChangeState.preLaneChange]:
      self.lane_change_timer = 0.0
    else:
      self.lane_change_timer += DT_MDL
    '''

    # /// Comma stock logic
    # Lane change logic
    one_blinker = sm['carState'].leftBlinker != sm['carState'].rightBlinker
    below_lane_change_speed = v_ego < LANE_CHANGE_SPEED_MIN

    if (not active) or (self.lane_change_timer > LANE_CHANGE_TIME_MAX) or (abs(self.output_scale) >= (CP.steerMaxV[0]-0.15) and self.lane_change_timer > 1):
      self.lane_change_state = LaneChangeState.off
      self.lane_change_direction = LaneChangeDirection.none
    else:
      # LaneChangeState.off
      if self.lane_change_state == LaneChangeState.off and one_blinker and not self.prev_one_blinker and not below_lane_change_speed:
        self.lane_change_state = LaneChangeState.preLaneChange
        self.lane_change_ll_prob = 1.0
        self.lane_change_wait_timer = 0        

      # LaneChangeState.preLaneChange
      elif self.lane_change_state == LaneChangeState.preLaneChange:
        self.lane_change_wait_timer += DT_MDL
        # Set lane change direction
        if sm['carState'].leftBlinker:
          self.lane_change_direction = LaneChangeDirection.left
        elif sm['carState'].rightBlinker:
          self.lane_change_direction = LaneChangeDirection.right
        else:  # If there are no blinkers we will go back to LaneChangeState.off
          self.lane_change_direction = LaneChangeDirection.none

        torque_applied = sm['carState'].steeringPressed and \
                        ((sm['carState'].steeringTorque > 0 and self.lane_change_direction == LaneChangeDirection.left) or
                          (sm['carState'].steeringTorque < 0 and self.lane_change_direction == LaneChangeDirection.right))

        blindspot_detected = ((sm['carState'].leftBlindspot and self.lane_change_direction == LaneChangeDirection.left) or
                              (sm['carState'].rightBlindspot and self.lane_change_direction == LaneChangeDirection.right))

        if not one_blinker or below_lane_change_speed:
          self.lane_change_state = LaneChangeState.off
        elif not blindspot_detected and (torque_applied or (self.lane_change_auto_delay and self.lane_change_wait_timer > self.lane_change_auto_delay)):
          self.lane_change_state = LaneChangeState.laneChangeStarting

      # LaneChangeState.laneChangeStarting
      elif self.lane_change_state == LaneChangeState.laneChangeStarting:
        # fade out over .5s
        self.lane_change_adjust_new = interp(v_ego, self.lane_change_adjust_vel, self.lane_change_adjust)
        self.lane_change_ll_prob = max(self.lane_change_ll_prob - self.lane_change_adjust_new*DT_MDL, 0.0)

        # 98% certainty
        lane_change_prob = self.LP.l_lane_change_prob + self.LP.r_lane_change_prob
        if lane_change_prob < 0.02 and self.lane_change_ll_prob < 0.01:
          self.lane_change_state = LaneChangeState.laneChangeFinishing

      # LaneChangeState.laneChangeFinishing
      elif self.lane_change_state == LaneChangeState.laneChangeFinishing:
        # fade in laneline over 1s
        self.lane_change_ll_prob = min(self.lane_change_ll_prob + DT_MDL, 1.0)
        if one_blinker and self.lane_change_ll_prob > 0.99:
          self.lane_change_state = LaneChangeState.preLaneChange
        elif self.lane_change_ll_prob > 0.99:
          self.lane_change_state = LaneChangeState.off

    if self.lane_change_state in [LaneChangeState.off, LaneChangeState.preLaneChange]:
      self.lane_change_timer = 0.0
    else:
      self.lane_change_timer += DT_MDL
    # Comma stock logic ///

    self.prev_one_blinker = one_blinker

    self.desire = DESIRES[self.lane_change_direction][self.lane_change_state]

    self.steer_rate_cost = interp(v_ego, [1.0, 8.0, 15.0], [1.0, 0.8, CP.steerRateCost])    

    # Turn off lanes during lane change
    if self.desire == log.LateralPlan.Desire.laneChangeRight or self.desire == log.LateralPlan.Desire.laneChangeLeft:
      self.LP.lll_prob *= self.lane_change_ll_prob
      self.LP.rll_prob *= self.lane_change_ll_prob
    if self.use_lanelines:
      d_path_xyz = self.LP.get_d_path(v_ego, self.t_idxs, self.path_xyz)
      self.libmpc.set_weights(MPC_COST_LAT.PATH, MPC_COST_LAT.HEADING, self.steer_rate_cost)
      self.laneless_mode_status = False
    elif self.laneless_mode == 0:
      d_path_xyz = self.LP.get_d_path(v_ego, self.t_idxs, self.path_xyz)
      self.libmpc.set_weights(MPC_COST_LAT.PATH, MPC_COST_LAT.HEADING, self.steer_rate_cost)
      self.laneless_mode_status = False
    # use laneless, it might mitigate abrubt steering at stopping?
    elif sm['radarState'].leadOne.dRel < 25 and (sm['radarState'].leadOne.vRel < 0 or (sm['radarState'].leadOne.vRel >= 0 and v_ego < 5)) and \
     (abs(sm['controlsState'].steeringAngleDesiredDeg) - abs(sm['carState'].steeringAngleDeg)) > 2 and self.lane_change_state == LaneChangeState.off:
      d_path_xyz = self.path_xyz
      path_cost = np.clip(abs(self.path_xyz[0,1]/self.path_xyz_stds[0,1]), 0.5, 5.0) * MPC_COST_LAT.PATH
      # Heading cost is useful at low speed, otherwise end of plan can be off-heading
      heading_cost = interp(v_ego, [5.0, 10.0], [MPC_COST_LAT.HEADING, 0.0])
      self.libmpc.set_weights(path_cost, heading_cost, self.steer_rate_cost)
      self.laneless_mode_status = True
      self.laneless_mode_at_stopping = True
      self.laneless_mode_at_stopping_timer = 60
    elif self.laneless_mode_at_stopping and (v_ego < 0.5 or self.laneless_mode_at_stopping_timer <= 0):
      d_path_xyz = self.LP.get_d_path(v_ego, self.t_idxs, self.path_xyz)
      self.libmpc.set_weights(MPC_COST_LAT.PATH, MPC_COST_LAT.HEADING, self.steer_rate_cost)
      self.laneless_mode_status = False
      self.laneless_mode_at_stopping = False
    elif self.laneless_mode == 1:
      d_path_xyz = self.path_xyz
      path_cost = np.clip(abs(self.path_xyz[0,1]/self.path_xyz_stds[0,1]), 0.5, 5.0) * MPC_COST_LAT.PATH
      # Heading cost is useful at low speed, otherwise end of plan can be off-heading
      heading_cost = interp(v_ego, [5.0, 10.0], [MPC_COST_LAT.HEADING, 0.0])
      self.libmpc.set_weights(path_cost, heading_cost, self.steer_rate_cost)
      self.laneless_mode_status = True
    elif self.laneless_mode == 2 and ((self.LP.lll_prob + self.LP.rll_prob)/2 < 0.2) and self.lane_change_state == LaneChangeState.off:
      d_path_xyz = self.path_xyz
      path_cost = np.clip(abs(self.path_xyz[0,1]/self.path_xyz_stds[0,1]), 0.5, 5.0) * MPC_COST_LAT.PATH
      # Heading cost is useful at low speed, otherwise end of plan can be off-heading
      heading_cost = interp(v_ego, [5.0, 10.0], [MPC_COST_LAT.HEADING, 0.0])
      self.libmpc.set_weights(path_cost, heading_cost, self.steer_rate_cost)
      self.laneless_mode_status = True
      self.laneless_mode_status_buffer = True
    elif self.laneless_mode == 2 and ((self.LP.lll_prob + self.LP.rll_prob)/2 > 0.4) and \
     self.laneless_mode_status_buffer and not self.laneless_mode_at_stopping and self.lane_change_state == LaneChangeState.off:
      d_path_xyz = self.LP.get_d_path(v_ego, self.t_idxs, self.path_xyz)
      self.libmpc.set_weights(MPC_COST_LAT.PATH, MPC_COST_LAT.HEADING, self.steer_rate_cost)
      self.laneless_mode_status = False
      self.laneless_mode_status_buffer = False
    elif self.laneless_mode == 2 and self.laneless_mode_status_buffer == True and self.lane_change_state == LaneChangeState.off:
      d_path_xyz = self.path_xyz
      path_cost = np.clip(abs(self.path_xyz[0,1]/self.path_xyz_stds[0,1]), 0.5, 5.0) * MPC_COST_LAT.PATH
      # Heading cost is useful at low speed, otherwise end of plan can be off-heading
      heading_cost = interp(v_ego, [5.0, 10.0], [MPC_COST_LAT.HEADING, 0.0])
      self.libmpc.set_weights(path_cost, heading_cost, self.steer_rate_cost)
      self.laneless_mode_status = True
    else:
      d_path_xyz = self.LP.get_d_path(v_ego, self.t_idxs, self.path_xyz)
      self.libmpc.set_weights(MPC_COST_LAT.PATH, MPC_COST_LAT.HEADING, self.steer_rate_cost)
      self.laneless_mode_status = False
      self.laneless_mode_status_buffer = False

    if self.laneless_mode_at_stopping_timer > 0:
      self.laneless_mode_at_stopping_timer -= 1

    y_pts = np.interp(v_ego * self.t_idxs[:MPC_N + 1], np.linalg.norm(d_path_xyz, axis=1), d_path_xyz[:,1])
    heading_pts = np.interp(v_ego * self.t_idxs[:MPC_N + 1], np.linalg.norm(self.path_xyz, axis=1), self.plan_yaw)
    self.y_pts = y_pts

    assert len(y_pts) == MPC_N + 1
    assert len(heading_pts) == MPC_N + 1
    self.libmpc.run_mpc(self.cur_state, self.mpc_solution,
                        float(v_ego),
                        CAR_ROTATION_RADIUS,
                        list(y_pts),
                        list(heading_pts))
    # init state for next
    self.cur_state.x = 0.0
    self.cur_state.y = 0.0
    self.cur_state.psi = 0.0
    self.cur_state.curvature = interp(DT_MDL, self.t_idxs[:MPC_N + 1], self.mpc_solution.curvature)

    # TODO this needs more thought, use .2s extra for now to estimate other delays
    delay = CP.steerActuatorDelay
    #delay = CP.steerActuatorDelay + .2
    self.steer_actuator_delay_to_send = delay
    current_curvature = self.mpc_solution.curvature[0]
    psi = interp(delay, self.t_idxs[:MPC_N + 1], self.mpc_solution.psi)
    next_curvature_rate = self.mpc_solution.curvature_rate[0]

    # MPC can plan to turn the wheel and turn back before t_delay. This means
    # in high delay cases some corrections never even get commanded. So just use
    # psi to calculate a simple linearization of desired curvature
    curvature_diff_from_psi = psi / (max(v_ego, 1e-1) * delay) - current_curvature
    next_curvature = current_curvature + 2 * curvature_diff_from_psi

    self.desired_curvature = next_curvature
    self.desired_curvature_rate = next_curvature_rate
    max_curvature_rate = interp(v_ego, MAX_CURVATURE_RATE_SPEEDS, MAX_CURVATURE_RATES)
    self.safe_desired_curvature_rate = clip(self.desired_curvature_rate,
                                            -max_curvature_rate,
                                            max_curvature_rate)
    self.safe_desired_curvature = clip(self.desired_curvature,
                                       self.safe_desired_curvature - max_curvature_rate/DT_MDL,
                                       self.safe_desired_curvature + max_curvature_rate/DT_MDL)

    #  Check for infeasable MPC solution
    mpc_nans = any(math.isnan(x) for x in self.mpc_solution.curvature)
    t = sec_since_boot()
    if mpc_nans:
      self.libmpc.init()
      self.cur_state.curvature = measured_curvature

      if t > self.last_cloudlog_t + 5.0:
        self.last_cloudlog_t = t
        cloudlog.warning("Lateral mpc - nan: True")

    if self.mpc_solution[0].cost > 20000. or mpc_nans:   # TODO: find a better way to detect when MPC did not converge
      self.solution_invalid_cnt += 1
    else:
      self.solution_invalid_cnt = 0

  def publish(self, sm, pm):
    plan_solution_valid = self.solution_invalid_cnt < 3
    plan_send = messaging.new_message('lateralPlan')
    plan_send.valid = sm.all_alive_and_valid(service_list=['carState', 'controlsState', 'modelV2'])
    plan_send.lateralPlan.laneWidth = float(self.LP.lane_width)
    plan_send.lateralPlan.dPathPoints = [float(x) for x in self.y_pts]
    plan_send.lateralPlan.lProb = float(self.LP.lll_prob)
    plan_send.lateralPlan.rProb = float(self.LP.rll_prob)
    plan_send.lateralPlan.dProb = float(self.LP.d_prob)

    plan_send.lateralPlan.rawCurvature = float(self.desired_curvature)
    plan_send.lateralPlan.rawCurvatureRate = float(self.desired_curvature_rate)
    plan_send.lateralPlan.curvature = float(self.safe_desired_curvature)
    plan_send.lateralPlan.curvatureRate = float(self.safe_desired_curvature_rate)

    plan_send.lateralPlan.mpcSolutionValid = bool(plan_solution_valid)

    plan_send.lateralPlan.desire = self.desire
    plan_send.lateralPlan.laneChangeState = self.lane_change_state
    plan_send.lateralPlan.laneChangeDirection = self.lane_change_direction

    plan_send.lateralPlan.steerRateCost = float(self.steer_rate_cost)
    plan_send.lateralPlan.outputScale = float(self.output_scale)
    plan_send.lateralPlan.vCruiseSet = float(self.v_cruise_kph)
    plan_send.lateralPlan.vCurvature = float(sm['controlsState'].curvature)
    plan_send.lateralPlan.steerAngleDesireDeg = float(sm['controlsState'].steeringAngleDesiredDeg)
    plan_send.lateralPlan.lanelessMode = bool(self.laneless_mode_status)
    plan_send.lateralPlan.steerActuatorDelay = float(self.steer_actuator_delay_to_send)

    if self.stand_still:
      self.standstill_elapsed_time += DT_MDL
    else:
      self.standstill_elapsed_time = 0.0
    plan_send.lateralPlan.standstillElapsedTime = int(self.standstill_elapsed_time)

    pm.send('lateralPlan', plan_send)

    if LOG_MPC:
      dat = messaging.new_message('liveMpc')
      dat.liveMpc.x = list(self.mpc_solution.x)
      dat.liveMpc.y = list(self.mpc_solution.y)
      dat.liveMpc.psi = list(self.mpc_solution.psi)
      dat.liveMpc.curvature = list(self.mpc_solution.curvature)
      dat.liveMpc.cost = self.mpc_solution.cost
      pm.send('liveMpc', dat)