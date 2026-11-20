#pragma once

#include <modules/fishing_types.h>

void fishing_mode_init();
void fishing_sim_tick();
void fishing_render();
void fishing_toggle_start_pause();
void fishing_stop_reset();
void fishing_toggle_arm();
void fishing_exit_cleanup();
void fishing_trigger_error(const char* msg);

void fishing_next_page();
void fishing_set_page(paxg::fishing::FishingPage page);
void fishing_settings_next_item();
void fishing_settings_adjust_value();
void fishing_settings_reset_defaults();
