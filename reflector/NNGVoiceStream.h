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
#include <nng/protocol/reqrep0/rep.h>
#include "AudioRecorder.h"

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
    std::string sessionId;                     // Client session ID (from dashboard)
    std::string module;                        // Module letter (e.g., "A")
    time_t createdAt;                          // Session creation time
    bool hasActiveStream;                      // True during PTT, false when idle
    uint16_t streamId;                         // Current stream ID (if hasActiveStream)
    uint8_t packetCounter;                     // Packet counter for current stream
    bool bypassTranscoder;                     // True if USRP mode (set once at PTT start)
    std::string audioFilename;                 // Recording filename for this transmission
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
    bool StartControlSocket(const std::string &addr);  // NEW: Start REP socket for control messages
    void Stop();

    // Write PCM audio samples (8kHz mono, int16_t) - TX Path
    // This is called from CodecStream::RxThread when audio is transcoded
    void WriteAudio(const int16_t* samples, int count, const std::string& callsign = "");

    // Check if streaming is active
    bool IsStreaming() const { return m_IsStreaming; }
    
    // Check if a client is a web client (to prevent echo)
    bool IsWebClient(std::shared_ptr<CClient> client) const;
    
    // Send recording complete notification
    void SendRecordingComplete(const std::string& callsign, const std::string& audioFile, const std::string& sessionId);
    
    // Notify about recording complete for a client (called by Reflector after stream closes)
    void NotifyRecordingComplete(const std::string& callsign, const std::string& recording, const std::string& source, const std::string& sessionId);

private:
    void InitOpusEncoder();
    void InitOpusDecoder();
    void SendOpusFrame(const unsigned char* data, int len, const std::string& callsign = "");
    void Cleanup();
    
    // RX Path - receive thread
    void ReceiveThread();
    void HandleMessage(const unsigned char* data, int len);
    
    // Control socket - receive thread (NEW)
    void ControlThread();
    void HandleControlMessage(const unsigned char* data, int len);
    void SendControlResponse(const std::string& jsonResponse);
    
    // Session lifecycle handlers (NEW - Phase 2)
    void HandleVoiceSessionStart(const std::string& module, const std::string& callsign, const std::string& source, const std::string& sessionId);
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
    std::string GetActiveStreamUser(const std::string& excludeCallsign) const;
    
    // OLD: Kept for backward compatibility during migration
    uint16_t GenerateStreamId();

    char            m_Module;
    CReflector*     m_Reflector;
    bool            m_IsStreaming;
    std::mutex      m_Mutex;

    // NNG socket for PAIR protocol (audio data)
    nng_socket      m_Socket;
    bool            m_SocketOpen;
    
    // NNG socket for REP protocol (control messages) - NEW
    nng_socket      m_ControlSocket;
    bool            m_ControlSocketOpen;
    std::thread     m_ControlThread;
    nng_aio*        m_ControlAio;  // For async reply

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
    
    // Audio recording
    CAudioRecorder  m_Recorder;

    // Opus settings
    static constexpr int SAMPLE_RATE = 8000;
    static constexpr int CHANNELS = 1;
    static constexpr int BITRATE = 12000;
    static constexpr int FRAME_SIZE = 160;  // 20ms at 8kHz
};
