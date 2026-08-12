#pragma once

#include "pcm_device.h"
#include "realtime_processor.h"

void print_control_help();

// Returns false when the user requests that the application exit.
bool handle_control_input(Processor& processor, const Pcm& capture,
                          const Pcm& playback,
                          const RecoveryStats& recoveries);
