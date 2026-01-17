#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <opus/opus.h>
#include <nng/nng.h>
#include <nng/protocol/pair0/pair.h>

// NNG Voice Stream for live audio streaming to dashboard
// Taps transcoded 8kHz mono PCM audio and encodes to Opus for streaming
class CNNGVoiceStream
{
public:
    CNNGVoiceStream(char module);
    ~CNNGVoiceStream();

    // Start/Stop NNG voice endpoint
    bool Start(const std::string &addr);
    void Stop();

    // Write PCM audio samples (8kHz mono, int16_t)
    // This is called from CodecStream::RxThread when audio is transcoded
    void WriteAudio(const int16_t* samples, int count);

    // Check if streaming is active
    bool IsStreaming() const { return m_IsStreaming; }

private:
    void InitOpus();
    void SendOpusFrame(const unsigned char* data, int len);
    void Cleanup();

    char            m_Module;
    bool            m_IsStreaming;
    std::mutex      m_Mutex;

    // NNG socket for PAIR protocol
    nng_socket      m_Socket;
    bool            m_SocketOpen;

    // Opus encoder state
    OpusEncoder*    m_Encoder;
    
    // PCM buffer for accumulating samples to frame size
    std::vector<int16_t> m_PcmBuffer;

    // Opus settings
    static constexpr int SAMPLE_RATE = 8000;
    static constexpr int CHANNELS = 1;
    static constexpr int BITRATE = 12000;
    static constexpr int FRAME_SIZE = 160;  // 20ms at 8kHz
};
