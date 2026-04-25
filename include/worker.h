#pragma once 

#include <vector>

#include <device_utils.h>

int create_wav_file(const std::vector<int16_t>& audio_vector);
void process_audio_queue(QueueContext& q_context);