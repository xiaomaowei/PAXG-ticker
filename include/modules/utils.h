#pragma once

#include <Arduino.h>

#include <vector>

unsigned row_choice(const std::vector<String>&);
bool keyboard_input(String& out_password);
