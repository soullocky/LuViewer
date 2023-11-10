#include "AC_BalanceControl.h"
#include <AP_HAL/AP_HAL.h>
#include <AP_Math/AP_Math.h>
#include <GCS_MAVLink/GCS.h>

#include "stdio.h"

extern const AP_HAL::HAL& hal;

// table of user settable parameters
const AP_Param::GroupInfo AC_BalanceControl::var_info[] = {
    // AP_SUBGROUPINFO(_pid_motor_left, "MTL_", 1, AC_BalanceControl, AC_PID),

    // AP_SUBGROUPINFO(_pid_motor_right, "MTR_", 2, AC_BalanceControl, AC_PID),

    AP_SUBGROUPINFO(_pid_angle, "ANG_", 1, AC_BalanceControl, AC_PID),

    AP_SUBGROUPINFO(_pid_speed, "SPD_", 2, AC_BalanceControl, AC_PID),

    AP_SUBGROUPINFO(_pid_turn, "TRN_", 3, AC_BalanceControl, AC_PID),

    AP_SUBGROUPINFO(_pid_roll, "ROL_", 4, AC_BalanceControl, AC_PID),

    AP_GROUPINFO("ZERO", 5, AC_BalanceControl, _zero_angle, AC_BALANCE_ZERO_ANGLE),

    AP_GROUPINFO("MAX_SPEED", 6, AC_BalanceControl, _max_speed, AC_BALANCE_MAX_SPEED),

    AP_GROUPINFO("TAR_SPEED_X", 7, AC_BalanceControl, Target_Velocity_X, AC_BALANCE_TARGET_X_SPEED),

    AP_GROUPINFO("TAR_SPEED_Z", 8, AC_BalanceControl, Target_Velocity_Z, AC_BALANCE_TARGET_Z_SPEED),

    AP_GROUPEND
};

AC_BalanceControl::AC_BalanceControl(AP_Motors* motors, AP_AHRS_View* ahrs)
    : _pid_angle(AC_BALANCE_ANGLE_P, 0, AC_BALANCE_ANGLE_D, 0, 0, 0, 0, 0),
      _pid_speed(AC_BALANCE_SPEED_P, AC_BALANCE_SPEED_I, 0, 0, AC_BALANCE_SPEED_IMAX, 0, 0, 0),
      _pid_turn(AC_BALANCE_TURN_P, 0, AC_BALANCE_TURN_D, 0, 0, 0, 0, 0),
      _pid_roll(AC_BALANCE_ROLL_P, AC_BALANCE_ROLL_I, AC_BALANCE_ROLL_D, 0, AC_BALANCE_ROLL_IMAX, 0, 0, 0),
      _motors(motors),
      _ahrs(ahrs)
{
    AP_Param::setup_object_defaults(this, var_info);

    _dt = 1.0f / 200.0f;

    speed_low_pass_filter.set_cutoff_frequency(30.0f);
    speed_low_pass_filter.reset(0);

    _moveflag_x = moveFlag::none;
    _moveflag_z = moveFlag::none;

    balanceMode = BalanceMode::ground;

    stop_balance_control = false;
}

void AC_BalanceControl::init()
{
    balanceCAN = AP_BalanceCAN::get_singleton();
}

/**************************************************************************
Function: Vertical PD control
Input   : Angle:angle；Gyro：angular velocity
Output  : balance：Vertical control PWM
函数功能：直立PD控制
入口参数：Angle:角度；Gyro：角速度
返回  值：balance：直立控制PWM
**************************************************************************/
float AC_BalanceControl::angle_controller(float Angle, float Gyro)
{
    // 求出平衡的角度中值 和机械相关
    angle_bias = _zero_angle - Angle;

    // 计算角速度误差
    gyro_bias = 0.0f - Gyro;

    // 计算平衡控制的电机PWM  PD控制   kp是P系数 kd是D系数
    angle_out = _pid_angle.kP() * angle_bias + gyro_bias * _pid_angle.kD();

    if (stop_balance_control) {
        angle_out  = 0;
        angle_bias = 0;
        gyro_bias  = 0;
    }

    return angle_out;
}

/**************************************************************************
Function: Speed PI control
Input   : encoder_left：Left wheel encoder reading；encoder_right：Right wheel encoder reading
Output  : Speed control PWM
函数功能：速度控制PWM
入口参数：encoder_left：左轮编码器读数；encoder_right：右轮编码器读数
返回  值：速度控制PWM
**************************************************************************/
float AC_BalanceControl::velocity_controller(float encoder_left, float encoder_right)
{

    //================遥控前进后退部分====================//
    if (_moveflag_x == moveFlag::moveFront) {
        encoder_movement = -Target_Velocity_X; // 收到前进信号
    } else if (_moveflag_x == moveFlag::moveBack) {
        encoder_movement = Target_Velocity_X; // 收到后退信号
    } else {
        encoder_movement = 0;
    }

    //================速度PI控制器=====================//

    // 获取最新速度偏差=目标速度（此处为零）-测量速度（左右编码器之和）
    encoder_error = (encoder_left + encoder_right) - encoder_movement;

    encoder_error_filter = speed_low_pass_filter.apply(encoder_error, _dt);

    velocity_out = _pid_speed.update_all(0.0f, encoder_error_filter, _dt);

    if (stop_balance_control == true) {
        _pid_speed.reset_I();
        encoder_error        = 0;
        encoder_error_filter = 0;
        encoder_movement     = 0;
        velocity_out         = 0;
    }

    return velocity_out;
}
/**************************************************************************
Function: Turn control
Input   : Z-axis angular velocity
Output  : Turn control PWM
函数功能：转向控制
入口参数：Z轴陀螺仪
返回  值：转向控制PWM
**************************************************************************/
float AC_BalanceControl::turn_controller(float yaw, float gyro)
{
    //===================遥控左右旋转部分=================//
    if (_moveflag_z == moveFlag::moveLeft) {
        turn_target = -Target_Velocity_Z;
    } else if (_moveflag_z == moveFlag::moveRight) {
        turn_target = Target_Velocity_Z;
    } else {
        turn_target = 0;
    }

    //===================转向PD控制器=================//
    turn_out = turn_target * _pid_turn.kP() + gyro * _pid_turn.kD(); // 结合Z轴陀螺仪进行PD控制

    if (stop_balance_control) {
        turn_out    = 0;
        turn_target = 0;
    }

    return turn_out;
}

void AC_BalanceControl::roll_controller(float roll)
{
    if (_motors == nullptr) return;

    float roll_out;

    roll_out = _pid_roll.update_all(0.0f, roll, _dt);

    _motors->set_roll_out(roll_out); // -1 ~ 1
}

void AC_BalanceControl::update(void)
{
    if (_motors == nullptr) {
        gcs().send_text(MAV_SEVERITY_WARNING, "_motors = nullptr");
        return;
    }

    if (balanceCAN == nullptr) {
        gcs().send_text(MAV_SEVERITY_WARNING, "balanceCAN = nullptr");
        return;
    }
    if (_ahrs == nullptr) {
        gcs().send_text(MAV_SEVERITY_WARNING, "_ahrs = nullptr");
        return;
    }

    static float angle_y, gyro_y, gyro_z;
    static float wheel_left_f, wheel_right_f;
    static float motor_target_left_f, motor_target_right_f;
    const float  max_scale_value = 10000.0f;

    angle_y = _ahrs->pitch;
    gyro_y  = _ahrs->get_gyro_latest()[1];
    gyro_z  = _ahrs->get_gyro_latest()[2];

    // 转速缩小1000倍
    wheel_left_f  = (float)balanceCAN->getSpeed(0) / max_scale_value;
    wheel_right_f = -(float)balanceCAN->getSpeed(1) / max_scale_value;

    // 平衡PID控制 Gyro_Balance平衡角速度极性：前倾为正，后倾为负
    control_balance = angle_controller(angle_y, gyro_y);

    // 速度环PID控制,记住，速度反馈是正反馈，就是小车快的时候要慢下来就需要再跑快一点
    control_velocity = velocity_controller(wheel_left_f, wheel_right_f);

    // 转向环PID控制
    control_turn = turn_controller(_ahrs->yaw, gyro_z);

    // motor值正数使小车前进，负数使小车后退, 范围【-1，1】
    motor_target_left_f  = control_balance + control_velocity + control_turn; // 计算左轮电机最终PWM
    motor_target_right_f = control_balance + control_velocity - control_turn; // 计算右轮电机最终PWM

    motor_target_left_int  = (int16_t)(motor_target_left_f * max_scale_value);
    motor_target_right_int = -(int16_t)(motor_target_right_f * max_scale_value);

    // 最终的电机输入量
    balanceCAN->setCurrent(0, (int16_t)motor_target_left_int);
    balanceCAN->setCurrent(1, (int16_t)motor_target_right_int);

    // 腿部舵机控制
    roll_controller(_ahrs->roll);

    // 设置模式
    set_control_mode();
}

void AC_BalanceControl::set_control_mode(void)
{
    switch (balanceMode) {
        case BalanceMode::ground:
            if ((alt_cm < 8) && (hal.rcin->read(CH_8) < 1600)) {
                balanceMode = BalanceMode::balance_car;
                gcs().send_text(MAV_SEVERITY_NOTICE, "balance_car");
            }
            break;
        case BalanceMode::balance_car:
            if ((_motors->armed()) && (hal.rcin->read(CH_3) < 1200) && (hal.rcin->read(CH_8)) > 1600) {

                balanceMode = BalanceMode::flying_with_balance;
                gcs().send_text(MAV_SEVERITY_NOTICE, "flying_with_balance");
            }
            break;
        case BalanceMode::flying_with_balance:
            if ((alt_cm >= 8) && (hal.rcin->read(CH_3) > 1200)) {

                balanceMode = BalanceMode::flying_without_balance;
                gcs().send_text(MAV_SEVERITY_NOTICE, "flying_without_balance");
            }
            break;
        case BalanceMode::flying_without_balance:
            // set_control_zeros();
            stop_balance_control = true;
            if ((alt_cm < 10) && (hal.rcin->read(CH_8) < 1600) && (hal.rcin->read(CH_3) < 1200)) {

                balanceMode = BalanceMode::landing_check;
                gcs().send_text(MAV_SEVERITY_NOTICE, "landing_check");
            }
            break;
        case BalanceMode::landing_check:
            if ((alt_cm < 8)) {
                balanceMode          = BalanceMode::ground;
                stop_balance_control = false;
                gcs().send_text(MAV_SEVERITY_NOTICE, "ground");
            }
            break;

        default:
            break;
    }

    uint16_t pwm_x = hal.rcin->read(CH_7);
    uint16_t pwm_z = hal.rcin->read(CH_6);

    if (pwm_x < 1300) {
        _moveflag_x = moveFlag::moveBack;
    } else if (pwm_x > 1700) {
        _moveflag_x = moveFlag::moveFront;
    } else {
        _moveflag_x = moveFlag::none;
    }

    if (pwm_z < 1300) {
        _moveflag_z = moveFlag::moveLeft;
    } else if (pwm_z > 1700) {
        _moveflag_z = moveFlag::moveRight;
    } else {
        _moveflag_z = moveFlag::none;
    }
}

void AC_BalanceControl::debug_info()
{

    // 调试用
    static uint16_t cnt = 0;
    cnt++;
    if (cnt > 400) {
        cnt = 0;
        gcs().send_text(MAV_SEVERITY_NOTICE, "left_real_speed=%d", balanceCAN->getSpeed(1));
        gcs().send_text(MAV_SEVERITY_NOTICE, "right_real_speed=%d", balanceCAN->getSpeed(2));

        if (alt_ok) {
            gcs().send_text(MAV_SEVERITY_NOTICE, "altok=%d, alt_cm=%f", alt_ok, alt_cm);
        }
    }
}