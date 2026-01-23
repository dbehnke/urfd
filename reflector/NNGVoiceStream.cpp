#include "NNGVoiceStream.h"
#include "Reflector.h"
#include "USRPClient.h"
#include "DVHeaderPacket.h"
#include "DVFramePacket.h"
#include "PacketStream.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <cstring>
#include <cstdlib>

using json = nlohmann::json;

CNNGVoiceStream::CNNGVoiceStream(char module, CReflector* reflector)
    : m_Module(module)
    , m_Reflector(reflector)
    , m_IsStreaming(false)
    , m_SocketOpen(false)
    , m_ControlSocketOpen(false)  // NEW
    , m_ControlAio(nullptr)       // NEW
    , m_Encoder(nullptr)
    , m_Decoder(nullptr)
    , m_Running(false)
{
    m_Socket.id = 0;
    m_ControlSocket.id = 0;  // NEW
}

CNNGVoiceStream::~CNNGVoiceStream()
{
    Stop();
}

void CNNGVoiceStream::Cleanup()
{
    // Close all active sessions
    std::lock_guard<std::mutex> lock(m_SessionMutex);
    
    // Close active streams and remove virtual clients
    for (auto& pair : m_Sessions) {
        VoiceSession& session = pair.second;
        
        // Close stream if active
        if (session.activeStream && m_Reflector) {
            m_Reflector->CloseStream(session.activeStream);
            session.activeStream = nullptr;
        }
        
        // Remove virtual client from reflector
        if (session.virtualClient && m_Reflector) {
            auto clients = m_Reflector->GetClients();
            clients->RemoveClient(session.virtualClient);
            m_Reflector->ReleaseClients();
        }
    }
    
    // Clear sessions map
    m_Sessions.clear();
    
    if (m_Encoder) {
        opus_encoder_destroy(m_Encoder);
        m_Encoder = nullptr;
    }
    if (m_Decoder) {
        opus_decoder_destroy(m_Decoder);
        m_Decoder = nullptr;
    }
    if (m_SocketOpen) {
        nng_close(m_Socket);
        m_SocketOpen = false;
    }
    // NEW: Close control socket
    if (m_ControlSocketOpen) {
        nng_close(m_ControlSocket);
        m_ControlSocketOpen = false;
    }
    if (m_ControlAio) {
        nng_aio_free(m_ControlAio);
        m_ControlAio = nullptr;
    }
    m_IsStreaming = false;
    m_PcmBuffer.clear();
}

bool CNNGVoiceStream::Start(const std::string &addr)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    Cleanup();

    // Initialize NNG PAIR socket
    int rv;
    if ((rv = nng_pair0_open(&m_Socket)) != 0) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Failed to open pair socket: " 
                  << nng_strerror(rv) << std::endl;
        return false;
    }
    m_SocketOpen = true;

    if ((rv = nng_listen(m_Socket, addr.c_str(), nullptr, 0)) != 0) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Failed to listen on " << addr 
                  << ": " << nng_strerror(rv) << std::endl;
        Cleanup();
        return false;
    }

    InitOpusEncoder();
    if (!m_Encoder) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Failed to initialize Opus encoder" << std::endl;
        Cleanup();
        return false;
    }

    InitOpusDecoder();
    if (!m_Decoder) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Failed to initialize Opus decoder" << std::endl;
        Cleanup();
        return false;
    }

    m_IsStreaming = true;
    m_Running = true;
    
    // Start receive thread
    m_ReceiveThread = std::thread(&CNNGVoiceStream::ReceiveThread, this);

    std::cout << "NNGVoiceStream[" << m_Module << "]: Started at " << addr << std::endl;
    return true;
}

void CNNGVoiceStream::Stop()
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_IsStreaming) {
            std::cout << "NNGVoiceStream[" << m_Module << "]: Stopping..." << std::endl;
        }
        m_Running = false;
    }
    
    // Wait for receive thread to finish
    if (m_ReceiveThread.joinable()) {
        m_ReceiveThread.join();
    }
    
    // Wait for control thread to finish (NEW)
    if (m_ControlThread.joinable()) {
        m_ControlThread.join();
    }
    
    std::lock_guard<std::mutex> lock(m_Mutex);
    Cleanup();
    std::cout << "NNGVoiceStream[" << m_Module << "]: Stopped" << std::endl;
}

bool CNNGVoiceStream::StartControlSocket(const std::string &addr)
{
    // Initialize NNG REP socket for control messages
    int rv;
    if ((rv = nng_rep0_open(&m_ControlSocket)) != 0) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Failed to open REP socket: " 
                  << nng_strerror(rv) << std::endl;
        return false;
    }
    m_ControlSocketOpen = true;

    if ((rv = nng_listen(m_ControlSocket, addr.c_str(), nullptr, 0)) != 0) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Failed to listen on control socket " << addr 
                  << ": " << nng_strerror(rv) << std::endl;
        nng_close(m_ControlSocket);
        m_ControlSocketOpen = false;
        return false;
    }

    // Start control thread
    m_ControlThread = std::thread(&CNNGVoiceStream::ControlThread, this);

    std::cout << "NNGVoiceStream[" << m_Module << "]: Control socket started at " << addr << std::endl;
    return true;
}

void CNNGVoiceStream::InitOpusEncoder()
{
    int err;
    m_Encoder = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Failed to create Opus encoder: " 
                  << opus_strerror(err) << std::endl;
        m_Encoder = nullptr;
        return;
    }
    opus_encoder_ctl(m_Encoder, OPUS_SET_BITRATE(BITRATE));
}

void CNNGVoiceStream::InitOpusDecoder()
{
    int err;
    m_Decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &err);
    if (err != OPUS_OK) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Failed to create Opus decoder: " 
                  << opus_strerror(err) << std::endl;
        m_Decoder = nullptr;
        return;
    }
}

void CNNGVoiceStream::WriteAudio(const int16_t* samples, int count, const std::string& callsign)
{
    // DIAGNOSTIC: Log entry to WriteAudio
    static uint32_t writeCount = 0;
    if (++writeCount % 10 == 1) {
        std::cout << "NNGVoiceStream[" << m_Module << "]: DIAGNOSTIC #" << writeCount 
                  << " - WriteAudio() called with " << count << " samples"
                  << ", m_IsStreaming=" << (m_IsStreaming ? "true" : "false")
                  << ", m_Encoder=" << (m_Encoder ? "valid" : "NULL")
                  << ", callsign=" << callsign << std::endl;
    }
    
    if (!m_IsStreaming || !m_Encoder) {
        static int warnCounter = 0;
        if ((warnCounter++ % 100) == 0) {
            std::cout << "NNGVoiceStream[" << m_Module << "]: DIAGNOSTIC #" << warnCounter 
                      << " - WriteAudio() SKIPPED (not streaming or no encoder)" << std::endl;
        }
        return;
    }
    
    if (!m_IsStreaming || !m_Encoder) {
        static uint32_t skipCount = 0;
        if (++skipCount % 10 == 1) {
            std::cout << "NNGVoiceStream[" << m_Module << "]: DIAGNOSTIC #" << skipCount 
                      << " - WriteAudio() SKIPPED (not streaming or no encoder)" << std::endl;
        }
        return;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);

    // Accumulate samples in buffer
    m_PcmBuffer.insert(m_PcmBuffer.end(), samples, samples + count);

    // Encode and send frames when we have enough samples
    unsigned char out_buf[512];  // Opus can encode up to 510 bytes for 20ms frame
    
    while (m_PcmBuffer.size() >= FRAME_SIZE) {
        int len = opus_encode(m_Encoder, m_PcmBuffer.data(), FRAME_SIZE, out_buf, sizeof(out_buf));
        if (len < 0) {
            std::cerr << "NNGVoiceStream[" << m_Module << "]: Opus encode error: " 
                      << opus_strerror(len) << std::endl;
        } else if (len > 0) {
            static uint32_t encodeCount = 0;
            if (++encodeCount % 10 == 1) {
                std::cout << "NNGVoiceStream[" << m_Module << "]: DIAGNOSTIC #" << encodeCount 
                          << " - Encoded Opus frame (" << len << " bytes), calling SendOpusFrame()" << std::endl;
            }
            SendOpusFrame(out_buf, len, callsign);
        }
        
        // Remove processed samples
        m_PcmBuffer.erase(m_PcmBuffer.begin(), m_PcmBuffer.begin() + FRAME_SIZE);
    }
}

void CNNGVoiceStream::SendOpusFrame(const unsigned char* data, int len, const std::string& callsign)
{
    // DIAGNOSTIC: Log SendOpusFrame entry
    static uint32_t sendFrameCount = 0;
    if (++sendFrameCount % 10 == 1) {
        std::cout << "NNGVoiceStream[" << m_Module << "]: DIAGNOSTIC #" << sendFrameCount 
                  << " - SendOpusFrame() called with " << len << " bytes"
                  << ", m_SocketOpen=" << (m_SocketOpen ? "true" : "false")
                  << ", callsign=" << callsign << std::endl;
    }
    
    if (!m_SocketOpen) {
        static int warnCounter = 0;
        if ((warnCounter++ % 100) == 0) {
            std::cerr << "NNGVoiceStream[" << m_Module << "]: Cannot send Opus - socket not open" << std::endl;
        }
        return;
    }

    // Create JSON message for dashboard compatibility
    // Format: {"type":"audio_data","module":"A","callsign":"KF8S","opus":[1,2,3,...]}
    json msg;
    msg["type"] = "audio_data";
    msg["module"] = std::string(1, m_Module);
    
    // Include callsign if provided
    if (!callsign.empty()) {
        msg["callsign"] = callsign;
    }
    
    // Convert Opus bytes to JSON array
    std::vector<int> opusArray(data, data + len);
    msg["opus"] = opusArray;
    
    std::string msg_str = msg.dump();

    int rv = nng_send(m_Socket, (void*)msg_str.c_str(), msg_str.size(), NNG_FLAG_NONBLOCK);
    
    // DIAGNOSTIC: Log send result
    static uint32_t sendResultCount = 0;
    if (++sendResultCount % 10 == 1) {
        std::cout << "NNGVoiceStream[" << m_Module << "]: DIAGNOSTIC #" << sendResultCount 
                  << " - nng_send() JSON result: rv=" << rv 
                  << ", JSON size=" << msg_str.size() << " bytes"
                  << " (0=success, -" << NNG_EAGAIN << "=would_block)" << std::endl;
    }
    
    if (rv != 0 && rv != NNG_EAGAIN) {
        static int errorCounter = 0;
        // Only log every 10th error to avoid flooding
        if ((errorCounter++ % 10) == 0) {
            std::cerr << "NNGVoiceStream[" << m_Module << "]: Send Opus error (" 
                      << errorCounter << "): " << nng_strerror(rv) << std::endl;
        }
    } else if (rv == NNG_EAGAIN) {
        // Buffer full - normal under high load
        static int dropCounter = 0;
        if ((dropCounter++ % 100) == 0) {
            std::cerr << "NNGVoiceStream[" << m_Module << "]: Send buffer full, Opus frame dropped (" 
                      << dropCounter << " total)" << std::endl;
        }
    }
}

// RX Path - Receive thread
void CNNGVoiceStream::ReceiveThread()
{
    std::cout << "NNGVoiceStream[" << m_Module << "]: Receive thread started" << std::endl;
    
    int consecutiveErrors = 0;
    const int maxConsecutiveErrors = 10;
    
    while (m_Running) {
        void* msg_buf = nullptr;
        size_t msg_sz = 0;
        
        // Receive message from NNG
        int rv = nng_recv(m_Socket, &msg_buf, &msg_sz, NNG_FLAG_ALLOC);
        
        if (rv != 0) {
            if (m_Running) {
                consecutiveErrors++;
                
                // Only log every Nth error to avoid flooding logs
                if (consecutiveErrors == 1 || (consecutiveErrors % 100) == 0) {
                    std::cerr << "NNGVoiceStream[" << m_Module << "]: Receive error (" 
                              << consecutiveErrors << "): " << nng_strerror(rv) << std::endl;
                }
                
                if (consecutiveErrors >= maxConsecutiveErrors && (consecutiveErrors % maxConsecutiveErrors) == 0) {
                    std::cerr << "NNGVoiceStream[" << m_Module << "]: Warning - no client connected or connection issues" << std::endl;
                }
            }
            // Small delay to avoid busy-wait
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        // Successfully received - reset error counter
        if (consecutiveErrors > 0) {
            std::cout << "NNGVoiceStream[" << m_Module << "]: Connection recovered after " 
                      << consecutiveErrors << " errors" << std::endl;
            consecutiveErrors = 0;
        }
        
        if (msg_buf && msg_sz > 0) {
            HandleMessage(static_cast<const unsigned char*>(msg_buf), msg_sz);
            nng_free(msg_buf, msg_sz);
        }
    }
    
    std::cout << "NNGVoiceStream[" << m_Module << "]: Receive thread stopped" << std::endl;
}

void CNNGVoiceStream::HandleMessage(const unsigned char* data, int len)
{
    try {
        // Parse JSON message
        std::string msg_str(reinterpret_cast<const char*>(data), len);
        json msg = json::parse(msg_str);
        
        std::string type = msg.value("type", "");
        std::string module = msg.value("module", "");
        std::string callsign = msg.value("callsign", "");
        std::string source = msg.value("source", "");  // Extract source tag (e.g., "web")
        std::string sessionId = msg.value("session_id", "");  // Extract session ID
        
        // Only handle messages for this module (except session_stop which doesn't have module)
        if (!module.empty() && module[0] != m_Module) {
            return;
        }
        
        // NEW: Session lifecycle messages (Phase 2)
        if (type == "voice_session_start") {
            HandleVoiceSessionStart(module, callsign, source, sessionId);
        }
        else if (type == "voice_session_stop") {
            HandleVoiceSessionStop(callsign);
        }
        // DEPRECATED: PTT messages via PAIR socket are no longer used.
        // PTT control now uses REQ/REP sockets (see HandleControlMessage) for guaranteed delivery.
        // These handlers are kept temporarily for backward compatibility but should be removed.
        // else if (type == "ptt_start") {
        //     HandlePTTStart(callsign);
        // }
        // else if (type == "ptt_stop") {
        //     HandlePTTStop(callsign);
        // }
        else if (type == "audio_data") {
            // Extract Opus data (can be either array or base64 string)
            if (msg.contains("opus")) {
                if (msg["opus"].is_array()) {
                    // Direct array format
                    std::vector<unsigned char> opusData = msg["opus"].get<std::vector<unsigned char>>();
                    if (!opusData.empty()) {
                        HandleAudioData(module, callsign, opusData.data(), opusData.size());
                    }
                } else if (msg["opus"].is_string()) {
                    // Base64 string format (from Go JSON marshaling)
                    // For now, just log - we'll handle base64 if needed
                    std::cerr << "NNGVoiceStream[" << m_Module << "]: WARNING: Received base64 opus data, not array" << std::endl;
                }
            } else {
                std::cerr << "NNGVoiceStream[" << m_Module << "]: audio_data message missing opus field" << std::endl;
            }
        }
    }
    catch (const json::exception& e) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: JSON parse error: " 
                  << e.what() << std::endl;
    }
}

// NEW - Phase 2: Session lifecycle management
void CNNGVoiceStream::HandleVoiceSessionStart(const std::string& module, const std::string& callsign, const std::string& source, const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(m_SessionMutex);
    
    // Check if session already exists
    auto it = m_Sessions.find(callsign);
    if (it != m_Sessions.end()) {
        std::cout << "NNGVoiceStream[" << m_Module << "]: Voice session already exists for " 
                  << callsign << std::endl;
        return;
    }
    
    // Create virtual USRP client for this callsign
    if (!CreateVirtualClient(callsign)) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Failed to create virtual client for " 
                  << callsign << std::endl;
        return;
    }
    
    // Create and store session
    VoiceSession session;
    session.virtualClient = m_Sessions[callsign].virtualClient;  // Get the client we just created
    session.activeStream = nullptr;
    session.callsign = callsign;
    session.source = source;
    session.sessionId = sessionId;  // Store session ID for recording notifications
    session.module = module;
    session.createdAt = time(nullptr);
    session.hasActiveStream = false;
    session.streamId = 0;
    session.packetCounter = 0;
    
    m_Sessions[callsign] = session;
    
    // Log with source tag for easy identification
    std::string sourceTag = source.empty() ? "" : "[" + source + "] ";
    std::cout << "NNGVoiceStream[" << m_Module << "]: " << sourceTag 
              << "Voice session started: " << callsign << " on module " << module;
    if (!sessionId.empty()) {
        std::cout << " (session_id: " << sessionId << ")";
    }
    std::cout << std::endl;
}

void CNNGVoiceStream::HandleVoiceSessionStop(const std::string& callsign)
{
    std::lock_guard<std::mutex> lock(m_SessionMutex);
    
    auto it = m_Sessions.find(callsign);
    if (it == m_Sessions.end()) {
        std::cout << "NNGVoiceStream[" << m_Module << "]: Voice session not found for " 
                  << callsign << std::endl;
        return;
    }
    
    VoiceSession& session = it->second;
    
    // Close active stream if any
    if (session.hasActiveStream && session.activeStream && m_Reflector) {
        m_Reflector->CloseStream(session.activeStream);
        session.activeStream = nullptr;
    }
    
    // Remove virtual client from reflector
    DestroyVirtualClient(callsign);
    
    // Remove from sessions map
    std::string sourceTag = session.source.empty() ? "" : "[" + session.source + "] ";
    std::cout << "NNGVoiceStream[" << m_Module << "]: " << sourceTag 
              << "Voice session stopped: " << callsign << std::endl;
    
    m_Sessions.erase(it);
}

void CNNGVoiceStream::HandlePTTStart(const std::string& callsign)
{
    std::lock_guard<std::mutex> lock(m_SessionMutex);
    
    // Get virtual client from sessions (DON'T create new one!)
    auto it = m_Sessions.find(callsign);
    if (it == m_Sessions.end()) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: PTT start rejected: No voice session for " 
                  << callsign << std::endl;
        // TODO: Send error back to dashboard
        return;
    }
    
    VoiceSession& session = it->second;
    
    // Half-duplex defense: Check if module already has active stream
    if (ModuleHasActiveStream(callsign)) {
        std::cout << "NNGVoiceStream[" << m_Module << "]: PTT start rejected: Module " 
                  << m_Module << " busy (active: another user)" << std::endl;
        // TODO: Send error back to dashboard
        return;
    }
    
    // Generate stream ID and create header packet
    session.streamId = GenerateStreamId();
    session.packetCounter = 0;
    
    // Create DV header packet
    CCallsign my(callsign);
    CCallsign ur("CQCQCQ");  // Standard "calling CQ" destination
    CCallsign rpt1(m_Reflector->GetCallsign());
    rpt1.SetCSModule(m_Module);
    CCallsign rpt2(m_Reflector->GetCallsign());
    rpt2.SetCSModule(m_Module);
    
    auto header = std::make_unique<CDvHeaderPacket>(my, ur, rpt1, rpt2, session.streamId, static_cast<uint8_t>(0));
    header->SetPacketModule(m_Module);
    
    // Open stream through reflector
    session.activeStream = m_Reflector->OpenStream(header, session.virtualClient);
    
    if (!session.activeStream) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Failed to open stream for " 
                  << callsign << std::endl;
        return;
    }
    
    session.hasActiveStream = true;
    
    // Log with source tag for easy identification
    std::string sourceTag = session.source.empty() ? "" : "[" + session.source + "] ";
    std::cout << "NNGVoiceStream[" << m_Module << "]: " << sourceTag << callsign 
              << " PTT started (stream " << session.streamId << ")" << std::endl;
}

void CNNGVoiceStream::HandlePTTStop(const std::string& callsign)
{
    std::lock_guard<std::mutex> lock(m_SessionMutex);
    
    auto it = m_Sessions.find(callsign);
    if (it == m_Sessions.end()) {
        std::cout << "NNGVoiceStream[" << m_Module << "]: PTT stop ignored: No voice session for " 
                  << callsign << std::endl;
        return;
    }
    
    VoiceSession& session = it->second;
    
    if (!session.hasActiveStream) {
        // No active stream, nothing to do
        return;
    }
    
    // Close reflector stream
    if (session.activeStream && m_Reflector) {
        // Send a final "last frame" packet to properly close the stream
        if (session.streamId != 0) {
            int16_t silence[FRAME_SIZE] = {0};  // Silent frame
            auto packet = std::make_unique<CDvFramePacket>(silence, session.streamId, true);
            packet->SetPacketModule(m_Module);
            session.activeStream->Push(std::move(packet));
        }
        
        m_Reflector->CloseStream(session.activeStream);
        session.activeStream = nullptr;
    }
    
    session.hasActiveStream = false;
    
    // DON'T destroy virtual client - keep it for next PTT cycle!
    
    // Log with source tag for easy identification
    std::string sourceTag = session.source.empty() ? "" : "[" + session.source + "] ";
    std::cout << "NNGVoiceStream[" << m_Module << "]: " << sourceTag << callsign 
              << " PTT stopped (" << static_cast<int>(session.packetCounter) << " packets)" << std::endl;
    
    // Reset packet counter for next transmission
    session.packetCounter = 0;
}

void CNNGVoiceStream::HandleAudioData(const std::string& module, const std::string& callsign,
                                      const unsigned char* opusData, int opusLen)
{
    std::lock_guard<std::mutex> lock(m_SessionMutex);
    
    // Get session for this callsign
    auto it = m_Sessions.find(callsign);
    if (it == m_Sessions.end()) {
        // No session for this callsign, ignore audio
        return;
    }
    
    VoiceSession& session = it->second;
    
    // Only process audio if this session has an active stream
    if (!session.hasActiveStream || !session.activeStream) {
        return;
    }
    
    if (!m_Decoder) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: No decoder available" << std::endl;
        return;
    }
    
    // Debug: log packet size
    std::cout << "NNGVoiceStream[" << m_Module << "]: Received Opus packet from " << callsign 
              << ", size: " << opusLen << " bytes" << std::endl;
    
    // Decode Opus to PCM
    int16_t pcm[FRAME_SIZE];
    int num_samples = opus_decode(m_Decoder, opusData, opusLen, pcm, FRAME_SIZE, 0);
    
    if (num_samples < 0) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Opus decode error: " 
                  << opus_strerror(num_samples) << std::endl;
        return;
    }
    
    // Log successful decode
    static uint32_t decodeCount = 0;
    if (++decodeCount % 10 == 1) {  // Log every 10th decode
        std::cout << "NNGVoiceStream[" << m_Module << "]: Decoded Opus packet #" << decodeCount 
                  << " from " << callsign << " (" << opusLen << " bytes -> " 
                  << num_samples << " PCM samples)" << std::endl;
    }
    
    // Inject PCM audio into reflector via stream
    // Create USRP frame packet with PCM data
    // The last parameter indicates if this is the last frame (we don't know yet, so false)
    auto packet = std::make_unique<CDvFramePacket>(pcm, session.streamId, false);
    packet->SetPacketModule(m_Module);
    
    // Push packet to stream
    session.activeStream->Push(std::move(packet));
    
    session.packetCounter++;
}

// Stream injection helper methods
bool CNNGVoiceStream::CreateVirtualClient(const std::string& callsign)
{
    if (!m_Reflector) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: No reflector instance" << std::endl;
        return false;
    }
    
    // Create a virtual USRP client for web transmissions
    // Use loopback IP address (127.0.0.1) since this is a virtual client
    CCallsign cs(callsign);
    CIp ip("127.0.0.1", AF_INET, SOCK_DGRAM, 0);  // Loopback for virtual client
    auto virtualClient = std::make_shared<CUSRPClient>(cs, ip, m_Module);
    
    if (!virtualClient) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Failed to create virtual client" << std::endl;
        return false;
    }
    
    // CRITICAL: Add virtual client to reflector's client list
    // OpenStream requires the client to be in the list (checks IsClient)
    auto clients = m_Reflector->GetClients();
    clients->AddClient(virtualClient);
    m_Reflector->ReleaseClients();
    
    // Store in the session (caller will do this)
    m_Sessions[callsign].virtualClient = virtualClient;
    
    std::cout << "NNGVoiceStream[" << m_Module << "]: Created virtual client for " 
              << callsign << std::endl;
    return true;
}

void CNNGVoiceStream::DestroyVirtualClient(const std::string& callsign)
{
    auto it = m_Sessions.find(callsign);
    if (it == m_Sessions.end()) {
        return;
    }
    
    VoiceSession& session = it->second;
    
    // Remove virtual client from reflector's client list
    if (session.virtualClient && m_Reflector) {
        auto clients = m_Reflector->GetClients();
        clients->RemoveClient(session.virtualClient);
        m_Reflector->ReleaseClients();
    }
    
    session.virtualClient = nullptr;
    
    std::cout << "NNGVoiceStream[" << m_Module << "]: Destroyed virtual client for " 
              << callsign << std::endl;
}

// Helper: Check if any other session on this module has an active stream
bool CNNGVoiceStream::ModuleHasActiveStream(const std::string& excludeCallsign) const
{
    // Note: Caller should already hold m_SessionMutex lock
    for (const auto& pair : m_Sessions) {
        const VoiceSession& session = pair.second;
        
        // Skip the callsign we're checking for (allow same user to restart)
        if (session.callsign == excludeCallsign) {
            continue;
        }
        
        // Check if this session has an active stream
        if (session.hasActiveStream) {
            return true;
        }
    }
    
    return false;
}

std::string CNNGVoiceStream::GetActiveStreamUser(const std::string& excludeCallsign) const
{
    // Note: Caller should already hold m_SessionMutex lock
    for (const auto& pair : m_Sessions) {
        const VoiceSession& session = pair.second;
        
        // Skip the callsign we're checking for (allow same user to restart)
        if (session.callsign == excludeCallsign) {
            continue;
        }
        
        // Check if this session has an active stream
        if (session.hasActiveStream) {
            return session.callsign;
        }
    }
    
    return "";  // No active stream found
}

uint16_t CNNGVoiceStream::GenerateStreamId()
{
    // Generate a random stream ID (same method used by other protocols)
    return static_cast<uint16_t>(::rand());
}

bool CNNGVoiceStream::IsWebClient(std::shared_ptr<CClient> client) const
{
    if (!client) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(m_SessionMutex));
    
    // Check if this client's callsign exists in our sessions map
    // and has source == "web"
    std::string callsign = client->GetCallsign().GetCS();
    auto it = m_Sessions.find(callsign);
    
    if (it != m_Sessions.end()) {
        const VoiceSession& session = it->second;
        return (session.source == "web" && session.virtualClient == client);
    }
    
    return false;
}

void CNNGVoiceStream::SendRecordingComplete(const std::string& callsign, const std::string& audioFile, const std::string& sessionId)
{
    std::cout << "NNGVoiceStream[" << m_Module << "]: SendRecordingComplete called for " << callsign 
              << ", file=" << audioFile << ", sessionId=" << sessionId << std::endl;
    
    if (!m_SocketOpen) {
        std::cout << "NNGVoiceStream[" << m_Module << "]: Socket not open, skipping recording_complete" << std::endl;
        return;
    }
    
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    // Create JSON message for recording_complete
    json msg;
    msg["type"] = "recording_complete";
    msg["callsign"] = callsign;
    msg["module"] = std::string(1, m_Module);
    msg["audio_file"] = audioFile;
    
    if (!sessionId.empty()) {
        msg["session_id"] = sessionId;
    }
    
    std::string msg_str = msg.dump();
    std::cout << "NNGVoiceStream[" << m_Module << "]: Sending recording_complete JSON: " << msg_str << std::endl;
    
    int rv = nng_send(m_Socket, (void*)msg_str.c_str(), msg_str.size(), NNG_FLAG_NONBLOCK);
    if (rv != 0 && rv != NNG_EAGAIN) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Failed to send recording_complete: " 
                  << nng_strerror(rv) << " (rv=" << rv << ")" << std::endl;
    } else if (rv == NNG_EAGAIN) {
        std::cout << "NNGVoiceStream[" << m_Module << "]: Send would block (NNG_EAGAIN), message may be dropped" << std::endl;
    } else {
        std::cout << "NNGVoiceStream[" << m_Module << "]: Successfully sent recording_complete for " 
                  << callsign << " (file: " << audioFile << ")" << std::endl;
    }
}

void CNNGVoiceStream::NotifyRecordingComplete(const std::string& callsign, const std::string& recording, const std::string& source, const std::string& sessionId)
{
    std::cout << "NNGVoiceStream[" << m_Module << "]: NotifyRecordingComplete called for " << callsign 
              << ", source='" << source << "', sessionId='" << sessionId << "'" << std::endl;
    
    if (callsign.empty()) {
        std::cout << "NNGVoiceStream[" << m_Module << "]: Callsign is empty, returning" << std::endl;
        return;
    }
    
    if (recording.empty()) {
        std::cout << "NNGVoiceStream[" << m_Module << "]: Recording filename is empty, returning" << std::endl;
        return;
    }
    
    // Only send notification for web clients
    if (source == "web") {
        std::cout << "NNGVoiceStream[" << m_Module << "]: Source is 'web', calling SendRecordingComplete..." << std::endl;
        SendRecordingComplete(callsign, recording, sessionId);
    } else {
        std::cout << "NNGVoiceStream[" << m_Module << "]: Source is '" << source 
                  << "', NOT calling SendRecordingComplete (only 'web' sources get notifications)" << std::endl;
    }
}

// NEW: Control thread - handles REQ/REP messages for PTT control
void CNNGVoiceStream::ControlThread()
{
    std::cout << "NNGVoiceStream[" << m_Module << "]: Control thread started" << std::endl;
    
    while (m_Running) {
        void* buf = nullptr;
        size_t sz;
        
        int rv = nng_recv(m_ControlSocket, &buf, &sz, NNG_FLAG_ALLOC);
        if (rv == 0 && buf != nullptr) {
            HandleControlMessage((unsigned char*)buf, sz);
            nng_free(buf, sz);
        } else if (rv != NNG_EAGAIN) {
            std::cerr << "NNGVoiceStream[" << m_Module << "]: Control recv error: " 
                      << nng_strerror(rv) << std::endl;
        }
        
        // Small delay to avoid busy-wait if socket is closed
        if (rv != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    std::cout << "NNGVoiceStream[" << m_Module << "]: Control thread stopped" << std::endl;
}

void CNNGVoiceStream::HandleControlMessage(const unsigned char* data, int len)
{
    try {
        json msg = json::parse(data, data + len);
        
        std::string type = msg.value("type", "");
        std::string module = msg.value("module", "");
        std::string callsign = msg.value("callsign", "");
        
        std::cout << "NNGVoiceStream[" << m_Module << "]: Control message received: type=" 
                  << type << ", callsign=" << callsign << std::endl;
        
        // Only handle messages for this module
        if (!module.empty() && module[0] != m_Module) {
            json response;
            response["status"] = "error";
            response["reason"] = "wrong_module";
            response["message"] = "Message for different module";
            SendControlResponse(response.dump());
            return;
        }
        
        if (type == "ptt_start") {
            // Handle PTT start and generate response
            std::lock_guard<std::mutex> lock(m_SessionMutex);
            
            auto it = m_Sessions.find(callsign);
            if (it == m_Sessions.end()) {
                json response;
                response["status"] = "error";
                response["reason"] = "no_session";
                response["message"] = "No voice session exists for " + callsign;
                SendControlResponse(response.dump());
                return;
            }
            
            VoiceSession& session = it->second;
            
            // DEBUG: Log all sessions and their stream states
            std::cout << "NNGVoiceStream[" << m_Module << "]: PTT start request from " << callsign << std::endl;
            std::cout << "NNGVoiceStream[" << m_Module << "]: Current sessions:" << std::endl;
            for (const auto& pair : m_Sessions) {
                std::cout << "  - " << pair.second.callsign << ": hasActiveStream=" 
                          << (pair.second.hasActiveStream ? "true" : "false") << std::endl;
            }
            
            // Check if module is busy with another user
            std::string activeUser = GetActiveStreamUser(callsign);
            if (!activeUser.empty()) {
                json response;
                response["status"] = "error";
                response["reason"] = "module_busy";
                response["active_user"] = activeUser;
                response["message"] = "Module " + std::string(1, m_Module) + " is currently in use by " + activeUser;
                SendControlResponse(response.dump());
                std::cout << "NNGVoiceStream[" << m_Module << "]: PTT denied for " << callsign 
                          << " - module busy with " << activeUser << std::endl;
                return;
            }
            
            // Generate stream ID and create header packet
            session.streamId = GenerateStreamId();
            session.packetCounter = 0;
            
            CCallsign my(callsign);
            CCallsign ur("CQCQCQ");
            CCallsign rpt1(m_Reflector->GetCallsign());
            rpt1.SetCSModule(m_Module);
            CCallsign rpt2(m_Reflector->GetCallsign());
            rpt2.SetCSModule(m_Module);
            
            auto header = std::make_unique<CDvHeaderPacket>(my, ur, rpt1, rpt2, session.streamId, static_cast<uint8_t>(0));
            header->SetPacketModule(m_Module);
            
            // Open stream through reflector
            session.activeStream = m_Reflector->OpenStream(header, session.virtualClient);
            
            if (!session.activeStream) {
                json response;
                response["status"] = "error";
                response["reason"] = "internal_error";
                response["message"] = "Failed to open stream";
                SendControlResponse(response.dump());
                return;
            }
            
            session.hasActiveStream = true;
            
            // Success response
            json response;
            response["status"] = "success";
            response["stream_id"] = session.streamId;
            SendControlResponse(response.dump());
            
            std::string sourceTag = session.source.empty() ? "" : "[" + session.source + "] ";
            std::cout << "NNGVoiceStream[" << m_Module << "]: " << sourceTag << callsign 
                      << " PTT started (stream " << session.streamId << ") via control socket" << std::endl;
        }
        else if (type == "ptt_stop") {
            // Handle PTT stop and generate response
            std::lock_guard<std::mutex> lock(m_SessionMutex);
            
            std::cout << "NNGVoiceStream[" << m_Module << "]: PTT stop request from " << callsign << std::endl;
            
            auto it = m_Sessions.find(callsign);
            if (it == m_Sessions.end()) {
                std::cout << "NNGVoiceStream[" << m_Module << "]: PTT stop - no session found for " << callsign << std::endl;
                json response;
                response["status"] = "error";
                response["reason"] = "no_session";
                response["message"] = "No voice session exists for " + callsign;
                SendControlResponse(response.dump());
                std::cout << "NNGVoiceStream[" << m_Module << "]: PTT stop - sent no_session error response" << std::endl;
                return;
            }
            
            VoiceSession& session = it->second;
            
            std::cout << "NNGVoiceStream[" << m_Module << "]: PTT stop - session found, hasActiveStream=" 
                      << (session.hasActiveStream ? "true" : "false") << std::endl;
            
            if (!session.hasActiveStream) {
                // No active stream, but not an error - just acknowledge
                std::cout << "NNGVoiceStream[" << m_Module << "]: PTT stop - no active stream, sending success anyway" << std::endl;
                json response;
                response["status"] = "success";
                SendControlResponse(response.dump());
                std::cout << "NNGVoiceStream[" << m_Module << "]: PTT stop - sent success response (no active stream)" << std::endl;
                return;
            }
            
            std::cout << "NNGVoiceStream[" << m_Module << "]: PTT stop - closing active stream" << std::endl;
            
            // Close reflector stream
            // NOTE: We avoid calling m_Reflector->CloseStream() because it has a blocking wait loop
            // that waits for the stream queue to be empty, which can hang indefinitely.
            // Instead, we manually perform the necessary cleanup steps without the blocking wait,
            // similar to how USRPProtocol handles this.
            if (session.activeStream && m_Reflector) {
                if (session.streamId != 0) {
                    // Push final silence packet to mark end of stream
                    // CRITICAL: Set as peer origin to bypass transcoder. The transcoder path would fail
                    // because the stream gets closed before the last packet returns from transcoding.
                    // By marking as peer origin, the packet goes directly to protocols (including USRP)
                    // with the islast=true flag, sending the KEYUP_FALSE signal to AllStar.
                    int16_t silence[FRAME_SIZE] = {0};
                    auto packet = std::make_unique<CDvFramePacket>(silence, session.streamId, true);
                    packet->SetPacketModule(m_Module);
                    packet->SetRemotePeerOrigin();  // Bypass transcoder!
                    session.activeStream->Push(std::move(packet));
                }
                
                // Lock clients for the following operations
                m_Reflector->GetClients();
                
                auto client = session.activeStream->GetOwnerClient();
                if (client) {
                    // Demote client from master
                    client->NotAMaster();
                    std::cout << "NNGVoiceStream[" << m_Module << "]: PTT stop - client demoted from master" << std::endl;
                    
                    // Stop recording and get the filename
                    std::string recording = session.activeStream->StopRecording();
                    std::cout << "NNGVoiceStream[" << m_Module << "]: PTT stop - recording stopped: " << recording << std::endl;
                    
                    // Notify users/dashboard about stream closing
                    std::cout << "NNGVoiceStream[" << m_Module << "]: PTT stop - calling GetUsers()->Closing()..." << std::endl;
                    m_Reflector->GetUsers()->Closing(
                        session.activeStream->GetUserCallsign(), 
                        m_Module, 
                        client->GetProtocol(), 
                        recording
                    );
                    std::cout << "NNGVoiceStream[" << m_Module << "]: PTT stop - calling ReleaseUsers()..." << std::endl;
                    m_Reflector->ReleaseUsers();
                    std::cout << "NNGVoiceStream[" << m_Module << "]: PTT stop - ReleaseUsers() done" << std::endl;
                    
                    // Send recording_complete notification to this voice stream for web clients
                    if (!recording.empty()) {
                        std::cout << "NNGVoiceStream[" << m_Module << "]: PTT stop - calling NotifyRecordingComplete()..." << std::endl;
                        NotifyRecordingComplete(callsign, recording, session.source, session.sessionId);
                        std::cout << "NNGVoiceStream[" << m_Module << "]: PTT stop - NotifyRecordingComplete() done" << std::endl;
                    }
                }
                
                // Release clients lock
                std::cout << "NNGVoiceStream[" << m_Module << "]: PTT stop - calling ReleaseClients()..." << std::endl;
                m_Reflector->ReleaseClients();
                std::cout << "NNGVoiceStream[" << m_Module << "]: PTT stop - ReleaseClients() done" << std::endl;
                
                // Close the packet stream directly (non-blocking)
                session.activeStream->ClosePacketStream();
                std::cout << "NNGVoiceStream[" << m_Module << "]: PTT stop - stream closed directly (non-blocking)" << std::endl;
                
                session.activeStream = nullptr;
            }
            
            session.hasActiveStream = false;
            
            std::string sourceTag = session.source.empty() ? "" : "[" + session.source + "] ";
            std::cout << "NNGVoiceStream[" << m_Module << "]: " << sourceTag << callsign 
                      << " stream closed, hasActiveStream set to false" << std::endl;
            
            // Success response
            json response;
            response["status"] = "success";
            response["packet_count"] = static_cast<int>(session.packetCounter);
            SendControlResponse(response.dump());
            
            std::cout << "NNGVoiceStream[" << m_Module << "]: " << sourceTag << callsign 
                      << " PTT stopped (" << static_cast<int>(session.packetCounter) 
                      << " packets) via control socket" << std::endl;
            
            // Reset packet counter for next transmission
            session.packetCounter = 0;
        }
        else {
            json response;
            response["status"] = "error";
            response["reason"] = "unknown_type";
            response["message"] = "Unknown message type: " + type;
            SendControlResponse(response.dump());
        }
    }
    catch (const json::exception& e) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Control message JSON parse error: " 
                  << e.what() << std::endl;
        
        json response;
        response["status"] = "error";
        response["reason"] = "parse_error";
        response["message"] = std::string("JSON parse error: ") + e.what();
        SendControlResponse(response.dump());
    }
}

void CNNGVoiceStream::SendControlResponse(const std::string& jsonResponse)
{
    std::cout << "NNGVoiceStream[" << m_Module << "]: SendControlResponse called, response: " << jsonResponse << std::endl;
    int rv = nng_send(m_ControlSocket, (void*)jsonResponse.c_str(), jsonResponse.size(), 0);
    if (rv != 0) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Failed to send control response: " 
                  << nng_strerror(rv) << std::endl;
    } else {
        std::cout << "NNGVoiceStream[" << m_Module << "]: Control response sent successfully" << std::endl;
    }
}

