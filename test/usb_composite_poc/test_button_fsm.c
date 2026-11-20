/**
 * @file test_button_fsm.c
 * @brief Standalone C test harness for btn_fsm.h exercising all deterministic test scenarios.
 */

#include <stdio.h>
#include <assert.h>
#include "btn_fsm.h"

static int s_press_count = 0;
static int s_release_count = 0;
static btn_op_result_t s_mock_press_res = BTN_OP_SUCCESS;
static btn_op_result_t s_mock_release_res = BTN_OP_SUCCESS;

static btn_op_result_t mock_press_fn(void *ctx)
{
    (void)ctx;
    s_press_count++;
    return s_mock_press_res;
}

static btn_op_result_t mock_release_fn(void *ctx)
{
    (void)ctx;
    s_release_count++;
    return s_mock_release_res;
}

static void test_c_busy_right_after_press(void)
{
    btn_fsm_t fsm;
    btn_fsm_init(&fsm, 50, 50);
    s_press_count = 0;
    s_release_count = 0;
    s_mock_press_res = BTN_OP_SUCCESS;
    s_mock_release_res = BTN_OP_SUCCESS;

    // Arm
    btn_fsm_step(&fsm, 0, false, true, mock_press_fn, mock_release_fn, NULL);
    btn_fsm_step(&fsm, 50, false, true, mock_press_fn, mock_release_fn, NULL);
    assert(fsm.state == BTN_STATE_IDLE);

    // Press at 100ms
    btn_fsm_step(&fsm, 100, true, true, mock_press_fn, mock_release_fn, NULL);
    btn_fsm_action_t act = btn_fsm_step(&fsm, 150, true, true, mock_press_fn, mock_release_fn, NULL);
    assert(act == BTN_ACTION_SEND_PRESS);
    assert(fsm.state == BTN_STATE_HOLDING_KEY);

    // Endpoint busy during hold: is_mounted is true -> must NOT abort!
    for (uint32_t t = 160; t < 200; t += 10) {
        act = btn_fsm_step(&fsm, t, true, true, mock_press_fn, mock_release_fn, NULL);
        assert(act == BTN_ACTION_NONE);
        assert(fsm.state == BTN_STATE_HOLDING_KEY);
    }

    // At 200ms: release succeeds
    act = btn_fsm_step(&fsm, 200, true, true, mock_press_fn, mock_release_fn, NULL);
    assert(act == BTN_ACTION_KEY_SEQUENCE_COMPLETED);
    assert(fsm.state == BTN_STATE_WAIT_RELEASE);
}

static void test_c_release_busy_retry(void)
{
    btn_fsm_t fsm;
    btn_fsm_init(&fsm, 50, 50);
    s_press_count = 0;
    s_release_count = 0;
    s_mock_press_res = BTN_OP_SUCCESS;
    s_mock_release_res = BTN_OP_BUSY;

    // Arm
    btn_fsm_step(&fsm, 0, false, true, mock_press_fn, mock_release_fn, NULL);
    btn_fsm_step(&fsm, 50, false, true, mock_press_fn, mock_release_fn, NULL);

    // Press
    btn_fsm_step(&fsm, 100, true, true, mock_press_fn, mock_release_fn, NULL);
    btn_fsm_step(&fsm, 150, true, true, mock_press_fn, mock_release_fn, NULL);
    assert(fsm.state == BTN_STATE_HOLDING_KEY);

    // At 200ms: release busy
    btn_fsm_action_t act = btn_fsm_step(&fsm, 200, true, true, mock_press_fn, mock_release_fn, NULL);
    assert(act == BTN_ACTION_SEND_RELEASE);
    assert(fsm.state == BTN_STATE_RETRY_RELEASE);

    // At 210ms: still busy
    act = btn_fsm_step(&fsm, 210, true, true, mock_press_fn, mock_release_fn, NULL);
    assert(act == BTN_ACTION_SEND_RELEASE);
    assert(fsm.state == BTN_STATE_RETRY_RELEASE);

    // At 220ms: release succeeds
    s_mock_release_res = BTN_OP_SUCCESS;
    act = btn_fsm_step(&fsm, 220, true, true, mock_press_fn, mock_release_fn, NULL);
    assert(act == BTN_ACTION_KEY_SEQUENCE_COMPLETED);
    assert(fsm.state == BTN_STATE_WAIT_RELEASE);
}

static void test_c_true_disconnect_and_reconnect(void)
{
    btn_fsm_t fsm;
    btn_fsm_init(&fsm, 50, 50);
    s_press_count = 0;
    s_release_count = 0;
    s_mock_press_res = BTN_OP_SUCCESS;

    // Arm
    btn_fsm_step(&fsm, 0, false, true, mock_press_fn, mock_release_fn, NULL);
    btn_fsm_step(&fsm, 50, false, true, mock_press_fn, mock_release_fn, NULL);

    // Press
    btn_fsm_step(&fsm, 100, true, true, mock_press_fn, mock_release_fn, NULL);
    btn_fsm_step(&fsm, 150, true, true, mock_press_fn, mock_release_fn, NULL);
    assert(fsm.state == BTN_STATE_HOLDING_KEY);

    // True disconnect at 180ms
    btn_fsm_action_t act = btn_fsm_step(&fsm, 180, true, false, mock_press_fn, mock_release_fn, NULL);
    assert(act == BTN_ACTION_ABORT_DISCONNECT);
    assert(fsm.state == BTN_STATE_STARTUP_WAIT_RELEASE);

    // Reconnect at 250ms while button still held
    for (uint32_t t = 250; t <= 400; t += 10) {
        act = btn_fsm_step(&fsm, t, true, true, mock_press_fn, mock_release_fn, NULL);
        assert(act == BTN_ACTION_NONE);
        assert(fsm.state == BTN_STATE_STARTUP_WAIT_RELEASE);
    }
}

static void test_c_held_at_boot_and_mount(void)
{
    btn_fsm_t fsm;
    btn_fsm_init(&fsm, 50, 50);
    s_press_count = 0;
    s_release_count = 0;

    // Boot with button pressed (t=0 to 200 ms, unmounted)
    for (uint32_t t = 0; t <= 200; t += 10) {
        btn_fsm_action_t act = btn_fsm_step(&fsm, t, true, false, mock_press_fn, mock_release_fn, NULL);
        assert(act == BTN_ACTION_NONE);
        assert(fsm.state == BTN_STATE_STARTUP_WAIT_RELEASE);
    }

    // USB mounted at t=210 ms, but button still held
    for (uint32_t t = 210; t <= 400; t += 10) {
        btn_fsm_action_t act = btn_fsm_step(&fsm, t, true, true, mock_press_fn, mock_release_fn, NULL);
        assert(act == BTN_ACTION_NONE);
        assert(fsm.state == BTN_STATE_STARTUP_WAIT_RELEASE);
    }
    assert(s_press_count == 0);
}

int main(void)
{
    test_c_busy_right_after_press();
    test_c_release_busy_retry();
    test_c_true_disconnect_and_reconnect();
    test_c_held_at_boot_and_mount();
    printf("All C Button FSM tests passed!\n");
    return 0;
}
