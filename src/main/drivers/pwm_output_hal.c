/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>

#include <stdlib.h>

#include "platform.h"

#include "gpio.h"
#include "timer.h"

#include "flight/failsafe.h" // FIXME dependency into the main code from a driver

#include "pwm_mapping.h"

#include "pwm_output.h"

#if (MAX_MOTORS > MAX_SERVOS)
#define MAX_PWM_OUTPUT_PORTS MAX_MOTORS
#else
#define MAX_PWM_OUTPUT_PORTS MAX_SERVOS
#endif

typedef void (*pwmWriteFuncPtr)(uint8_t index, uint16_t value);  // function pointer used to write motors

typedef struct {
    volatile timCCR_t *ccr;
    TIM_TypeDef *tim;
    uint16_t period;
    pwmWriteFuncPtr pwmWritePtr;
} pwmOutputPort_t;

static pwmOutputPort_t pwmOutputPorts[MAX_PWM_OUTPUT_PORTS];

static pwmOutputPort_t *motors[MAX_PWM_MOTORS];

#ifdef USE_SERVOS
static pwmOutputPort_t *servos[MAX_PWM_SERVOS];
#endif

static uint8_t allocatedOutputPortCount = 0;

static bool pwmMotorsEnabled = true;
static void pwmOCConfig(TIM_TypeDef *tim, uint32_t channel, uint16_t value)
{
    TIM_HandleTypeDef* Handle = timerFindTimerHandle(tim);
    if(Handle == NULL) return;
    
    TIM_OC_InitTypeDef  TIM_OCInitStructure;

    TIM_OCInitStructure.OCMode = TIM_OCMODE_PWM2;
    TIM_OCInitStructure.OCNIdleState = TIM_OCIDLESTATE_RESET;
    TIM_OCInitStructure.Pulse = value;
    TIM_OCInitStructure.OCPolarity = TIM_OCPOLARITY_LOW;
    TIM_OCInitStructure.OCNPolarity = TIM_OCPOLARITY_HIGH;
    TIM_OCInitStructure.OCIdleState = TIM_OCIDLESTATE_SET;
    TIM_OCInitStructure.OCFastMode = TIM_OCFAST_DISABLE;
    
    HAL_TIM_PWM_ConfigChannel(Handle, &TIM_OCInitStructure, channel);
}

static void pwmGPIOConfig(const timerHardware_t *timerHardwar)
{
    GPIO_InitTypeDef init;
    init.Speed = GPIO_SPEED_LOW;
    init.Alternate = timerHardwar->alternateFunction;
    init.Pin = timerHardwar->pin;
    init.Pull = GPIO_PULLDOWN;
    init.Mode = timerHardwar->gpioInputMode;
    
    HAL_GPIO_Init(timerHardwar->gpio, &init);
}

static pwmOutputPort_t *pwmOutConfig(const timerHardware_t *timerHardware, uint8_t mhz, uint16_t period, uint16_t value)
{
    pwmOutputPort_t *p = &pwmOutputPorts[allocatedOutputPortCount++];
    TIM_HandleTypeDef* Handle = timerFindTimerHandle(timerHardware->tim);
    if(Handle == NULL) return p;

    configTimeBase(timerHardware->tim, period, mhz);
    pwmGPIOConfig(timerHardware);


    pwmOCConfig(timerHardware->tim, timerHardware->channel, value);
    if (timerHardware->outputEnable)
        HAL_TIM_PWM_Start(Handle, timerHardware->channel);
    else        
        HAL_TIM_PWM_Stop(Handle, timerHardware->channel);
    HAL_TIM_Base_Start(Handle);

    switch (timerHardware->channel) {
        case TIM_CHANNEL_1:
            p->ccr = &timerHardware->tim->CCR1;
            break;
        case TIM_CHANNEL_2:
            p->ccr = &timerHardware->tim->CCR2;
            break;
        case TIM_CHANNEL_3:
            p->ccr = &timerHardware->tim->CCR3;
            break;
        case TIM_CHANNEL_4:
            p->ccr = &timerHardware->tim->CCR4;
            break;
    }
    p->period = period;
    p->tim = timerHardware->tim;

    return p;
}

static void pwmWriteBrushed(uint8_t index, uint16_t value)
{
    *motors[index]->ccr = (value - 1000) * motors[index]->period / 1000;
}

static void pwmWriteStandard(uint8_t index, uint16_t value)
{
    *motors[index]->ccr = value;
}

void pwmWriteMotor(uint8_t index, uint16_t value)
{
    if (motors[index] && index < MAX_MOTORS && pwmMotorsEnabled)
        motors[index]->pwmWritePtr(index, value);
}

void pwmShutdownPulsesForAllMotors(uint8_t motorCount)
{
    uint8_t index;

    for(index = 0; index < motorCount; index++){
        // Set the compare register to 0, which stops the output pulsing if the timer overflows
        *motors[index]->ccr = 0;
    }
}

void pwmDisableMotors(void)
{
    pwmMotorsEnabled = false;
}

void pwmEnableMotors(void)
{
    pwmMotorsEnabled = true;
}

void pwmCompleteOneshotMotorUpdate(uint8_t motorCount)
{
    uint8_t index;
    TIM_TypeDef *lastTimerPtr = NULL;

    for(index = 0; index < motorCount; index++){

        // Force the timer to overflow if it's the first motor to output, or if we change timers
        if(motors[index]->tim != lastTimerPtr){
            lastTimerPtr = motors[index]->tim;

            timerForceOverflow(motors[index]->tim);
        }
    }
    for(index = 0; index < motorCount; index++){
            // Set the compare register to 0, which stops the output pulsing if the timer overflows before the main loop completes again.
            // This compare register will be set to the output value on the next main loop.
            *motors[index]->ccr = 0;

    }

}

bool isMotorBrushed(uint16_t motorPwmRate)
{
    return (motorPwmRate > 500);
}

void pwmBrushedMotorConfig(const timerHardware_t *timerHardware, uint8_t motorIndex, uint16_t motorPwmRate, uint16_t idlePulse)
{
    uint32_t hz = PWM_BRUSHED_TIMER_MHZ * 1000000;
    motors[motorIndex] = pwmOutConfig(timerHardware, PWM_BRUSHED_TIMER_MHZ, hz / motorPwmRate, idlePulse);
    motors[motorIndex]->pwmWritePtr = pwmWriteBrushed;
}

void pwmBrushlessMotorConfig(const timerHardware_t *timerHardware, uint8_t motorIndex, uint16_t motorPwmRate, uint16_t idlePulse)
{
    uint32_t hz = PWM_TIMER_MHZ * 1000000;
    motors[motorIndex] = pwmOutConfig(timerHardware, PWM_TIMER_MHZ, hz / motorPwmRate, idlePulse);
    motors[motorIndex]->pwmWritePtr = pwmWriteStandard;
}

void pwmOneshotMotorConfig(const timerHardware_t *timerHardware, uint8_t motorIndex)
{
    motors[motorIndex] = pwmOutConfig(timerHardware, ONESHOT125_TIMER_MHZ, 0xFFFF, 0);
    motors[motorIndex]->pwmWritePtr = pwmWriteStandard;
}

#ifdef USE_SERVOS
void pwmServoConfig(const timerHardware_t *timerHardware, uint8_t servoIndex, uint16_t servoPwmRate, uint16_t servoCenterPulse)
{
    servos[servoIndex] = pwmOutConfig(timerHardware, PWM_TIMER_MHZ, 1000000 / servoPwmRate, servoCenterPulse);
}

void pwmWriteServo(uint8_t index, uint16_t value)
{
    if (servos[index] && index < MAX_SERVOS)
        *servos[index]->ccr = value;
}
#endif
