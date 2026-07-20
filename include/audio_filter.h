#pragma once

#include "wav_type.h"

void apply_low_pass_filter(WavData& audioData, float cutoffHz);
void apply_high_pass_filter(WavData& audioData, float cutoffHz);