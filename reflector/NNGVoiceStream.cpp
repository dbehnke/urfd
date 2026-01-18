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
    , m_Encoder(nullptr)
    , m_Decoder(nullptr)
    , m_Running(false)
{
    m_Socket.id = 0;
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
    
    std::lock_guard<std::mutex> lock(m_Mutex);
    Cleanup();
    std::cout << "NNGVoiceStream[" << m_Module << "]: Stopped" << std::endl;
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

void CNNGVoiceStream::WriteAudio(const int16_t* samples, int count)
{
    if (!m_IsStreaming || !m_Encoder) return;

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
            SendOpusFrame(out_buf, len);
        }
        
        // Remove processed samples
        m_PcmBuffer.erase(m_PcmBuffer.begin(), m_PcmBuffer.begin() + FRAME_SIZE);
    }
}

void CNNGVoiceStream::SendOpusFrame(const unsigned char* data, int len)
{
    if (!m_SocketOpen) return;

    // Create a simple binary message: [module_char][opus_data]
    std::vector<unsigned char> message;
    message.reserve(len + 1);
    message.push_back(static_cast<unsigned char>(m_Module));
    message.insert(message.end(), data, data + len);

    int rv = nng_send(m_Socket, (void*)message.data(), message.size(), NNG_FLAG_NONBLOCK);
    if (rv != 0 && rv != NNG_EAGAIN) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Send error: " 
                  << nng_strerror(rv) << std::endl;
    }
}

// RX Path - Receive thread
void CNNGVoiceStream::ReceiveThread()
{
    std::cout << "NNGVoiceStream[" << m_Module << "]: Receive thread started" << std::endl;
    
    while (m_Running) {
        void* msg_buf = nullptr;
        size_t msg_sz = 0;
        
        // Receive message from NNG
        int rv = nng_recv(m_Socket, &msg_buf, &msg_sz, NNG_FLAG_ALLOC);
        
        if (rv != 0) {
            if (m_Running) {
                std::cerr << "NNGVoiceStream[" << m_Module << "]: Receive error: " 
                          << nng_strerror(rv) << std::endl;
            }
            continue;
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
        
        // Only handle messages for this module (except session_stop which doesn't have module)
        if (!module.empty() && module[0] != m_Module) {
            return;
        }
        
        // NEW: Session lifecycle messages (Phase 2)
        if (type == "voice_session_start") {
            HandleVoiceSessionStart(module, callsign, source);
        }
        else if (type == "voice_session_stop") {
            HandleVoiceSessionStop(callsign);
        }
        // UPDATED: PTT messages now use session map
        else if (type == "ptt_start") {
            HandlePTTStart(callsign);
        }
        else if (type == "ptt_stop") {
            HandlePTTStop(callsign);
        }
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
void CNNGVoiceStream::HandleVoiceSessionStart(const std::string& module, const std::string& callsign, const std::string& source)
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
    session.module = module;
    session.createdAt = time(nullptr);
    session.hasActiveStream = false;
    session.streamId = 0;
    session.packetCounter = 0;
    
    m_Sessions[callsign] = session;
    
    // Log with source tag for easy identification
    std::string sourceTag = source.empty() ? "" : "[" + source + "] ";
    std::cout << "NNGVoiceStream[" << m_Module << "]: " << sourceTag 
              << "Voice session started: " << callsign << " on module " << module << std::endl;
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
    // Use a dummy IP address (127.0.0.1) since this is a virtual client
    CCallsign cs(callsign);
    CIp ip;  // Default constructor creates a valid IP
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

uint16_t CNNGVoiceStream::GenerateStreamId()
{
    // Generate a random stream ID (same method used by other protocols)
    return static_cast<uint16_t>(::rand());
}
