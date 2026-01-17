#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <memory>
#include <opus/opus.h>
#include <nng/nng.h>
#include <nng/protocol/pair0/pair.h>

// Forward declarations
class CReflector;
class CClient;
class CPacketStream;

// NNG Voice Stream for live audio streaming to/from dashboard
// TX Path: Taps transcoded 8kHz mono PCM audio and encodes to Opus for streaming
// RX Path: Receives Opus from dashboard, decodes to PCM, injects into reflector
class CNNGVoiceStream
{
public:
    CNNGVoiceStream(char module, CReflector* reflector);
    ~CNNGVoiceStream();

    // Start/Stop NNG voice endpoint
    bool Start(const std::string &addr);
    void Stop();

    // Write PCM audio samples (8kHz mono, int16_t) - TX Path
    // This is called from CodecStream::RxThread when audio is transcoded
    void WriteAudio(const int16_t* samples, int count);

    // Check if streaming is active
    bool IsStreaming() const { return m_IsStreaming; }

private:
    void InitOpusEncoder();
    void InitOpusDecoder();
    void SendOpusFrame(const unsigned char* data, int len);
    void Cleanup();
    
    // RX Path - receive thread
    void ReceiveThread();
    void HandleMessage(const unsigned char* data, int len);
    void HandlePTTStart(const std::string& module, const std::string& callsign, const std::string& source);
    void HandlePTTStop(const std::string& module, const std::string& callsign, const std::string& source);
    void HandleAudioData(const std::string& module, const std::string& callsign, 
                        const unsigned char* opusData, int opusLen);
    
    // Stream injection helpers
    bool CreateVirtualClient(const std::string& callsign);
    void DestroyVirtualClient();
    uint16_t GenerateStreamId();

    char            m_Module;
    CReflector*     m_Reflector;
    bool            m_IsStreaming;
    std::mutex      m_Mutex;

    // NNG socket for PAIR protocol
    nng_socket      m_Socket;
    bool            m_SocketOpen;

    // Opus encoder state (TX Path)
    OpusEncoder*    m_Encoder;
    
    // Opus decoder state (RX Path)
    OpusDecoder*    m_Decoder;
    
    // PCM buffer for accumulating samples to frame size (TX)
    std::vector<int16_t> m_PcmBuffer;
    
    // Receive thread
    std::thread     m_ReceiveThread;
    std::atomic<bool> m_Running;
    
    // Current active talker (RX enforcement)
    std::string     m_ActiveCallsign;
    std::string     m_ActiveSource;  // Source tag (e.g., "web")
    std::mutex      m_ActiveMutex;
    
    // Virtual client for web transmissions
    std::shared_ptr<CClient> m_VirtualClient;
    std::shared_ptr<CPacketStream> m_ActiveStream;
    uint16_t        m_StreamId;
    uint8_t         m_PacketCounter;

    // Opus settings
    static constexpr int SAMPLE_RATE = 8000;
    static constexpr int CHANNELS = 1;
    static constexpr int BITRATE = 12000;
    static constexpr int FRAME_SIZE = 160;  // 20ms at 8kHz
};
