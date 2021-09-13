// TODO: haven't validated these numbers, also need to comply with comma safety standards
const int STELLANTIS_MAX_STEER = 255;               // TODO: verify
const int STELLANTIS_MAX_RT_DELTA = 93;             // 5 max rate up * 50Hz send rate * 250000 RT interval / 1000000 = 62 ; 62 * 1.5 for safety pad = 93
const uint32_t STELLANTIS_RT_INTERVAL = 250000;     // 250ms between real time checks
const int STELLANTIS_MAX_RATE_UP = 5;
const int STELLANTIS_MAX_RATE_DOWN = 10;
const int STELLANTIS_DRIVER_TORQUE_ALLOWANCE = 80;
const int STELLANTIS_DRIVER_TORQUE_FACTOR = 3;
// TODO: why do we need gas/standstill thresholds? autoresume spam not working yet maybe?
const int STELLANTIS_GAS_THRSLD = 30;               // 7% more than 2m/s
const int STELLANTIS_STANDSTILL_THRSLD = 10;        // about 1m/s

// Safety-relevant CAN messages for the Stellantis 5th gen RAM (DT) platform
#define MSG_EPS_1           0x23  // EPS steering angle and assist motor torque
#define MSG_EPS_2           0x49  // EPS driver input torque and angle-change rate
#define MSG_ABS_1           0x79  // Brake pedal and pressure
#define MSG_TPS_1           0x81  // Throttle position sensor
#define MSG_WHEEL_SPEEDS    0x8B  // ABS wheel speeds
#define MSG_DASM_ACC        0x99  // ACC engagement states from DASM
#define MSG_DASM_LKAS       0xA6  // LKAS controls from DASM
#define MSG_ACC_BUTTONS     0xB1  // Cruise control buttons
#define MSG_DASM_HUD        0xFA  // LKAS HUD and auto headlight control from DASM

const CanMsg STELLANTIS_TX_MSGS[] = {{MSG_DASM_LKAS, 0, 8}, {MSG_DASM_HUD, 0, 8}, {MSG_ACC_BUTTONS, 2, 8}};

AddrCheckStruct stellantis_addr_checks[] = {
  {.msg = {{MSG_EPS_1, 0, 8, .check_checksum = true, .max_counter = 15U, .expected_timestep = 10000U}, { 0 }, { 0 }}},
  {.msg = {{MSG_ABS_1, 0, 8, .check_checksum = false, .max_counter = 15U,  .expected_timestep = 20000U}, { 0 }, { 0 }}},
  {.msg = {{MSG_TPS_1, 0, 8, .check_checksum = false, .max_counter = 15U,  .expected_timestep = 20000U}, { 0 }, { 0 }}},
  {.msg = {{MSG_WHEEL_SPEEDS, 0, 8, .check_checksum = false, .max_counter = 0U, .expected_timestep = 20000U}, { 0 }, { 0 }}},
  {.msg = {{MSG_DASM_ACC, 0, 8, .check_checksum = false, .max_counter = 15U, .expected_timestep = 20000U}, { 0 }, { 0 }}},
};
#define STELLANTIS_ADDR_CHECK_LEN (sizeof(stellantis_addr_checks) / sizeof(stellantis_addr_checks[0]))
addr_checks stellantis_rx_checks = {stellantis_addr_checks, STELLANTIS_ADDR_CHECK_LEN};

static uint8_t stellantis_get_checksum(CAN_FIFOMailBox_TypeDef *to_push) {
  int checksum_byte = GET_LEN(to_push) - 1;
  return (uint8_t)(GET_BYTE(to_push, checksum_byte));
}

// TODO: centralize/combine with Chrysler?
static uint8_t stellantis_compute_checksum(CAN_FIFOMailBox_TypeDef *to_push) {
  /* This function does not want the checksum byte in the input data.
  jeep ram canbus checksum from http://illmatics.com/Remote%20Car%20Hacking.pdf */
  uint8_t checksum = 0xFFU;
  int len = GET_LEN(to_push);
  for (int j = 0; j < (len - 1); j++) {
    uint8_t shift = 0x80U;
    uint8_t curr = (uint8_t)GET_BYTE(to_push, j);
    for (int i=0; i<8; i++) {
      uint8_t bit_sum = curr & shift;
      uint8_t temp_chk = checksum & 0x80U;
      if (bit_sum != 0U) {
        bit_sum = 0x1C;
        if (temp_chk != 0U) {
          bit_sum = 1;
        }
        checksum = checksum << 1;
        temp_chk = checksum | 1U;
        bit_sum ^= temp_chk;
      } else {
        if (temp_chk != 0U) {
          bit_sum = 0x1D;
        }
        checksum = checksum << 1;
        bit_sum ^= checksum;
      }
      checksum = bit_sum;
      shift = shift >> 1;
    }
  }
  return ~checksum;
}

static uint8_t stellantis_get_counter(CAN_FIFOMailBox_TypeDef *to_push) {
  // Well defined counter only for 8 bytes messages
  return (uint8_t)(GET_BYTE(to_push, 6) >> 4);
}

static int stellantis_rx_hook(CAN_FIFOMailBox_TypeDef *to_push) {

  bool valid = addr_safety_check(to_push, &stellantis_rx_checks,
                                 stellantis_get_checksum, stellantis_compute_checksum, stellantis_get_counter);

  if (valid && (GET_BUS(to_push) == 0)) {
    int addr = GET_ADDR(to_push);

    // Measured eps torque
    if (addr == MSG_EPS_2) {
      int torque_meas_new = (((GET_BYTE(to_push, 0) & 0x7U) << 8) | GET_BYTE(to_push, 1)) - 3072U;

      // update array of samples
      update_sample(&torque_meas, torque_meas_new);
    }

    // enter controls on rising edge of ACC, exit controls on ACC off
    if (addr == MSG_DASM_ACC) {
      int cruise_engaged = ((GET_BYTE(to_push, 2) & 0x38) >> 3) == 7;
      if (cruise_engaged && !cruise_engaged_prev) {
        controls_allowed = 1;
      }
      if (!cruise_engaged) {
        controls_allowed = 0;
      }
      cruise_engaged_prev = cruise_engaged;
    }

    // update speed
    if (addr == MSG_WHEEL_SPEEDS) {
      int speed_l = (GET_BYTE(to_push, 0) << 4) + (GET_BYTE(to_push, 1) >> 4);
      int speed_r = (GET_BYTE(to_push, 2) << 4) + (GET_BYTE(to_push, 3) >> 4);
      vehicle_speed = (speed_l + speed_r) / 2;
      vehicle_moving = (int)vehicle_speed > STELLANTIS_STANDSTILL_THRSLD;
    }

    // exit controls on rising edge of gas press
    if (addr == MSG_TPS_1) {
      gas_pressed = ((GET_BYTE(to_push, 5) & 0x7F) != 0) && ((int)vehicle_speed > STELLANTIS_GAS_THRSLD);
    }

    // exit controls on rising edge of brake press
    if (addr == MSG_ABS_1) {
      brake_pressed = (GET_BYTE(to_push, 0) & 0x7) == 5;
      if (brake_pressed && (!brake_pressed_prev || vehicle_moving)) {
        controls_allowed = 0;
      }
      brake_pressed_prev = brake_pressed;
    }

    generic_rx_checks((addr == MSG_DASM_LKAS));
  }
  return valid;
}

static int stellantis_tx_hook(CAN_FIFOMailBox_TypeDef *to_send) {

  int tx = 1;
  int addr = GET_ADDR(to_send);

  if (!msg_allowed(to_send, STELLANTIS_TX_MSGS, sizeof(STELLANTIS_TX_MSGS) / sizeof(STELLANTIS_TX_MSGS[0]))) {
    tx = 0;  // for dev
  }

  if (relay_malfunction) {
    tx = 0;
  }

  // LKA STEER
  if (addr == 0xa6) {
    int desired_torque = ((GET_BYTE(to_send, 0) & 0x7U) << 8) + GET_BYTE(to_send, 1) - 1024U;
    uint32_t ts = microsecond_timer_get();
    bool violation = 0;

    if (controls_allowed) {

      // *** global torque limit check ***
      violation |= max_limit_check(desired_torque, STELLANTIS_MAX_STEER, -STELLANTIS_MAX_STEER);

      // *** torque rate limit check ***
      violation |= driver_limit_check(desired_torque, desired_torque_last, &torque_driver,
        STELLANTIS_MAX_STEER, STELLANTIS_MAX_RATE_UP, STELLANTIS_MAX_RATE_DOWN,
        STELLANTIS_DRIVER_TORQUE_ALLOWANCE, STELLANTIS_DRIVER_TORQUE_FACTOR);

      // used next time
      desired_torque_last = desired_torque;

      // *** torque real time rate limit check ***
      violation |= rt_rate_limit_check(desired_torque, rt_torque_last, STELLANTIS_MAX_RT_DELTA);

      // every RT_INTERVAL set the new limits
      uint32_t ts_elapsed = get_ts_elapsed(ts, ts_last);
      if (ts_elapsed > STELLANTIS_RT_INTERVAL) {
        rt_torque_last = desired_torque;
        ts_last = ts;
      }
    }

    // no torque if controls is not allowed
    if (!controls_allowed && (desired_torque != 0)) {
      violation = 0;
    }

    // reset to 0 if either controls is not allowed or there's a violation
    if (violation || !controls_allowed) {
      desired_torque_last = 0;
      rt_torque_last = 0;
      ts_last = ts;
    }

    if (violation) {
      tx = 1;  // for dev
    }
  }

  // FORCE CANCEL: only the cancel button press is allowed
  if (addr == MSG_ACC_BUTTONS) {
    if ((GET_BYTE(to_send, 0) != 1) || ((GET_BYTE(to_send, 1) & 1) == 1)) {
      tx = 1;
    }
  }

  return tx;
}

static int stellantis_fwd_hook(int bus_num, CAN_FIFOMailBox_TypeDef *to_fwd) {

  int bus_fwd = -1;
  int addr = GET_ADDR(to_fwd);

  if (!relay_malfunction) {
    // forward all messages from the rest of the car toward DASM
    if (bus_num == 0) {
      bus_fwd = 2;
    }
    // selectively forward messages from DASM toward the rest of the car
    if ((bus_num == 2) && (addr != MSG_DASM_LKAS) && (addr != MSG_DASM_HUD)) {
      bus_fwd = 0;
    }
  }
  return bus_fwd;
}

static const addr_checks* stellantis_init(int16_t param) {
  UNUSED(param);  // TODO: may need a parameter to choose steering angle signal width/precision for 2019 vs 2021 RAM
  controls_allowed = false;
  relay_malfunction_reset();
  return &stellantis_rx_checks;
}

const safety_hooks stellantis_hooks = {
  .init = stellantis_init,
  .rx = stellantis_rx_hook,
  .tx = stellantis_tx_hook,
  .tx_lin = nooutput_tx_lin_hook,
  .fwd = stellantis_fwd_hook,
};
