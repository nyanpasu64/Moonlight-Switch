#ifdef __SWITCH__

#include "AudrenAudioRenderer.hpp"
#include <Settings.hpp>
#include <borealis.hpp>
#include <algorithm>
#include <climits>
#include <inttypes.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t m_sink_channels[] = {0, 1};

int AudrenAudioRenderer::init(int audio_configuration,
                              const POPUS_MULTISTREAM_CONFIGURATION opus_config,
                              void* context, int ar_flags) {
    return DR_OK;
}

void AudrenAudioRenderer::cleanup() {
}

void AudrenAudioRenderer::decode_and_play_sample(char* data, int length) {
}

int AudrenAudioRenderer::capabilities() {
#if defined(PLATFORM_SWITCH)
    return 0;
#else
    return CAPABILITY_DIRECT_SUBMIT;
#endif
}

ssize_t AudrenAudioRenderer::free_wavebuf_index() {
    return -1;
}

size_t AudrenAudioRenderer::append_audio(const void* buf, size_t size) {
    return size;
}

// Drop audren buffer every 400 samples to prevent audio delay
bool AudrenAudioRenderer::flush() {
    static int count = 0;
    count++;
    if (count == 400) {
        count = 0;
        return true;
    }
    return false;
}

void AudrenAudioRenderer::write_audio(const void* buf, size_t size) {
}

#endif //__SWITCH__
