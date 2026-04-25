#include "network.h"

#include <string>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

int send_to_groq(std::vector<int16_t> audio_vector) {
    std::string model_name = "whisper-large-v3";
    httplib::SSLClient cli("https://api.groq.com/openai/v1/audio/transcriptions");
    
    
    return 0;
}
