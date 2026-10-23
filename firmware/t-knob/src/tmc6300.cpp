#include <Arduino.h>
#include <driver/mcpwm_prelude.h>

#include "board.h"
#include "tmc6300.h"

#define RESOLUTION_HZ 10000000 // 0.1 us per tick
#define PERIOD_TICKS 500       // 20 kHz
#define DEAD_TICKS 5           // 0.5 us

static mcpwm_cmpr_handle_t cmpr[3];
static mcpwm_gen_handle_t gen_hi[3], gen_lo[3];

void tmc6300_init()
{
    mcpwm_timer_handle_t timer;
    mcpwm_timer_config_t tcfg = {};
    tcfg.group_id = 0;
    tcfg.clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT;
    tcfg.resolution_hz = RESOLUTION_HZ;
    tcfg.count_mode = MCPWM_TIMER_COUNT_MODE_UP;
    tcfg.period_ticks = PERIOD_TICKS;
    ESP_ERROR_CHECK(mcpwm_new_timer(&tcfg, &timer));

    // Cut the outputs cycle-by-cycle while the driver reports a fault.
    mcpwm_fault_handle_t fault;
    mcpwm_gpio_fault_config_t fcfg = {};
    fcfg.group_id = 0;
    fcfg.gpio_num = DRV_DIAG;
    fcfg.flags.active_level = 1;
    ESP_ERROR_CHECK(mcpwm_new_gpio_fault(&fcfg, &fault));
    mcpwm_brake_config_t bcfg = {};
    bcfg.brake_mode = MCPWM_OPER_BRAKE_MODE_CBC;
    bcfg.fault = fault;
    bcfg.flags.cbc_recover_on_tez = true;

    const int pins[3][2] = {{DRV_UH, DRV_UL}, {DRV_VH, DRV_VL}, {DRV_WH, DRV_WL}};
    for (int i = 0; i < 3; i++) {
        mcpwm_oper_handle_t op;
        mcpwm_operator_config_t ocfg = {};
        ocfg.group_id = 0;
        ESP_ERROR_CHECK(mcpwm_new_operator(&ocfg, &op));
        ESP_ERROR_CHECK(mcpwm_operator_connect_timer(op, timer));
        ESP_ERROR_CHECK(mcpwm_operator_set_brake_on_fault(op, &bcfg));

        mcpwm_comparator_config_t ccfg = {};
        ccfg.flags.update_cmp_on_tez = true;
        ESP_ERROR_CHECK(mcpwm_new_comparator(op, &ccfg, &cmpr[i]));
        ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(cmpr[i], PERIOD_TICKS / 2));

        mcpwm_generator_config_t gcfg = {};
        gcfg.gen_gpio_num = pins[i][0];
        ESP_ERROR_CHECK(mcpwm_new_generator(op, &gcfg, &gen_hi[i]));
        gcfg.gen_gpio_num = pins[i][1];
        ESP_ERROR_CHECK(mcpwm_new_generator(op, &gcfg, &gen_lo[i]));

        // Both generators produce the same waveform: high from the start of the period
        // until the compare point. The dead-time stage then inverts the low side.
        mcpwm_gen_handle_t gens[2] = {gen_hi[i], gen_lo[i]};
        for (mcpwm_gen_handle_t g : gens) {
            ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(g,
                MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
            ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(g,
                MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, cmpr[i], MCPWM_GEN_ACTION_LOW)));
            ESP_ERROR_CHECK(mcpwm_generator_set_action_on_brake_event(g,
                MCPWM_GEN_BRAKE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_OPER_BRAKE_MODE_CBC, MCPWM_GEN_ACTION_LOW)));
        }

        mcpwm_dead_time_config_t hi = {};
        hi.posedge_delay_ticks = DEAD_TICKS;
        ESP_ERROR_CHECK(mcpwm_generator_set_dead_time(gen_hi[i], gen_hi[i], &hi));
        mcpwm_dead_time_config_t lo = {};
        lo.negedge_delay_ticks = DEAD_TICKS;
        lo.flags.invert_output = true;
        ESP_ERROR_CHECK(mcpwm_generator_set_dead_time(gen_lo[i], gen_lo[i], &lo));
    }

    tmc6300_enable(false);
    ESP_ERROR_CHECK(mcpwm_timer_enable(timer));
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP));
}

void tmc6300_enable(bool on)
{
    for (int i = 0; i < 3; i++) {
        if (on) {
            mcpwm_generator_set_force_level(gen_hi[i], -1, true);
            mcpwm_generator_set_force_level(gen_lo[i], -1, true);
        } else {
            mcpwm_generator_set_force_level(gen_hi[i], 0, true);
            // The low side is inverted after the force stage, so 1 here means "off".
            mcpwm_generator_set_force_level(gen_lo[i], 1, true);
        }
    }
}

void tmc6300_set_duty(float a, float b, float c)
{
    const float d[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        float v = d[i] < 0 ? 0 : (d[i] > 1 ? 1 : d[i]);
        mcpwm_comparator_set_compare_value(cmpr[i], (uint32_t)(v * PERIOD_TICKS));
    }
}
