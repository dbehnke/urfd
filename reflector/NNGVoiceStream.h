#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <memory>
#include <map>
#include <ctime>
#include <opus/opus.h>
#include <nng/nng.h>
#include <nng/protocol/pair0/pair.h>

// Forward declarations
class CReflector;
class CClient;
class CPacketStream;

// Voice session structure - tracks virtual client lifecycle across PTT cycles
struct VoiceSession {
    std::shared_ptr<CClient> virtualClient;    // Virtual client instance (persistent)
    std::shared_ptr<CPacketStream> activeStream; // Current active stream (only during PTT)
    std::string callsign;                      // User's callsign
    std::string source;                        // Source tag (e.g., "web")
    std::string module;                        // Module letter (e.g., "A")
    time_t createdAt;                          // Session creation time
    bool hasActiveStream;                      // True during PTT, false when idle
    uint16_t streamId;                         // Current stream ID (if hasActiveStream)
    uint8_t packetCounter;                     // Packet counter for current stream
};

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
    
    // Session lifecycle handlers (NEW - Phase 2)
    void HandleVoiceSessionStart(const std::string& module, const std::string& callsign, const std::string& source);
    void HandleVoiceSessionStop(const std::string& callsign);
    
    // PTT handlers (UPDATED - use session map)
    void HandlePTTStart(const std::string& callsign);
    void HandlePTTStop(const std::string& callsign);
    void HandleAudioData(const std::string& module, const std::string& callsign, 
                        const unsigned char* opusData, int opusLen);
    
    // Session management helpers (NEW - Phase 2)
    bool CreateVirtualClient(const std::string& callsign);
    void DestroyVirtualClient(const std::string& callsign);
    bool ModuleHasActiveStream(const std::string& excludeCallsign) const;
    
    // OLD: Kept for backward compatibility during migration
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
    
    // Session management (NEW - Phase 2)
    std::map<std::string, VoiceSession> m_Sessions;  // key: callsign
    std::mutex      m_SessionMutex;

    // Opus settings
    static constexpr int SAMPLE_RATE = 8000;
    static constexpr int CHANNELS = 1;
    static constexpr int BITRATE = 12000;
    static constexpr int FRAME_SIZE = 160;  // 20ms at 8kHz
};
