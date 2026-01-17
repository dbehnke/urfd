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
    , m_VirtualClient(nullptr)
    , m_ActiveStream(nullptr)
    , m_StreamId(0)
    , m_PacketCounter(0)
{
    m_Socket.id = 0;
}

CNNGVoiceStream::~CNNGVoiceStream()
{
    Stop();
}

void CNNGVoiceStream::Cleanup()
{
    // Close active stream if any
    if (m_ActiveStream && m_Reflector) {
        m_Reflector->CloseStream(m_ActiveStream);
        m_ActiveStream = nullptr;
    }
    
    // Destroy virtual client
    m_VirtualClient = nullptr;
    m_StreamId = 0;
    m_PacketCounter = 0;
    
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
    m_ActiveCallsign.clear();
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
        
        // Only handle messages for this module
        if (module.empty() || module[0] != m_Module) {
            return;
        }
        
        if (type == "ptt_start") {
            HandlePTTStart(module, callsign);
        }
        else if (type == "ptt_stop") {
            HandlePTTStop(module, callsign);
        }
        else if (type == "audio_data") {
            // Extract Opus data (stored as array of bytes in JSON)
            if (msg.contains("opus") && msg["opus"].is_array()) {
                std::vector<unsigned char> opusData = msg["opus"];
                HandleAudioData(module, callsign, opusData.data(), opusData.size());
            }
        }
    }
    catch (const json::exception& e) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: JSON parse error: " 
                  << e.what() << std::endl;
    }
}

void CNNGVoiceStream::HandlePTTStart(const std::string& module, const std::string& callsign)
{
    std::lock_guard<std::mutex> lock(m_ActiveMutex);
    
    if (!m_ActiveCallsign.empty()) {
        std::cout << "NNGVoiceStream[" << m_Module << "]: PTT denied for " << callsign 
                  << " (active: " << m_ActiveCallsign << ")" << std::endl;
        // TODO: Send ptt_denied message back
        return;
    }
    
    // Create virtual client for this web transmission
    if (!CreateVirtualClient(callsign)) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Failed to create virtual client for " 
                  << callsign << std::endl;
        return;
    }
    
    // Generate stream ID and create header packet
    m_StreamId = GenerateStreamId();
    m_PacketCounter = 0;
    
    // Create DV header packet
    CCallsign my(callsign);
    CCallsign ur("CQCQCQ");  // Standard "calling CQ" destination
    CCallsign rpt1(m_Reflector->GetCallsign());
    rpt1.SetCSModule(m_Module);
    CCallsign rpt2(m_Reflector->GetCallsign());
    rpt2.SetCSModule(m_Module);
    
    auto header = std::make_unique<CDvHeaderPacket>(my, ur, rpt1, rpt2, m_StreamId, static_cast<uint8_t>(0));
    header->SetPacketModule(m_Module);
    
    // Open stream through reflector
    m_ActiveStream = m_Reflector->OpenStream(header, m_VirtualClient);
    
    if (!m_ActiveStream) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Failed to open stream for " 
                  << callsign << std::endl;
        DestroyVirtualClient();
        return;
    }
    
    m_ActiveCallsign = callsign;
    std::cout << "NNGVoiceStream[" << m_Module << "]: [WEB] " << callsign 
              << " started transmitting (stream " << m_StreamId << ")" << std::endl;
}

void CNNGVoiceStream::HandlePTTStop(const std::string& module, const std::string& callsign)
{
    std::lock_guard<std::mutex> lock(m_ActiveMutex);
    
    if (m_ActiveCallsign != callsign) {
        // Not the active caller, ignore
        return;
    }
    
    std::cout << "NNGVoiceStream[" << m_Module << "]: [WEB] " << callsign 
              << " stopped transmitting (" << m_PacketCounter << " packets)" << std::endl;
    
    // Close stream and destroy virtual client
    DestroyVirtualClient();
    m_ActiveCallsign.clear();
}

void CNNGVoiceStream::HandleAudioData(const std::string& module, const std::string& callsign,
                                      const unsigned char* opusData, int opusLen)
{
    std::lock_guard<std::mutex> lock(m_ActiveMutex);
    
    // Only process audio from the active caller
    if (m_ActiveCallsign != callsign) {
        return;
    }
    
    if (!m_Decoder) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: No decoder available" << std::endl;
        return;
    }
    
    // Decode Opus to PCM
    int16_t pcm[FRAME_SIZE];
    int num_samples = opus_decode(m_Decoder, opusData, opusLen, pcm, FRAME_SIZE, 0);
    
    if (num_samples < 0) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Opus decode error: " 
                  << opus_strerror(num_samples) << std::endl;
        return;
    }
    
    // Inject PCM audio into reflector via stream
    if (m_ActiveStream) {
        // Create USRP frame packet with PCM data
        // The last parameter indicates if this is the last frame (we don't know yet, so false)
        auto packet = std::make_unique<CDvFramePacket>(pcm, m_StreamId, false);
        packet->SetPacketModule(m_Module);
        
        // Push packet to stream
        m_ActiveStream->Push(std::move(packet));
        
        m_PacketCounter++;
    }
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
    m_VirtualClient = std::make_shared<CUSRPClient>(cs, ip, m_Module);
    
    if (!m_VirtualClient) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Failed to create virtual client" << std::endl;
        return false;
    }
    
    std::cout << "NNGVoiceStream[" << m_Module << "]: Created virtual client for " 
              << callsign << std::endl;
    return true;
}

void CNNGVoiceStream::DestroyVirtualClient()
{
    // Close active stream if any
    if (m_ActiveStream && m_Reflector) {
        // Send a final "last frame" packet to properly close the stream
        if (m_StreamId != 0) {
            int16_t silence[FRAME_SIZE] = {0};  // Silent frame
            auto packet = std::make_unique<CDvFramePacket>(silence, m_StreamId, true);
            packet->SetPacketModule(m_Module);
            m_ActiveStream->Push(std::move(packet));
        }
        
        m_Reflector->CloseStream(m_ActiveStream);
        m_ActiveStream = nullptr;
    }
    
    m_VirtualClient = nullptr;
    m_StreamId = 0;
    m_PacketCounter = 0;
    
    std::cout << "NNGVoiceStream[" << m_Module << "]: Destroyed virtual client" << std::endl;
}

uint16_t CNNGVoiceStream::GenerateStreamId()
{
    // Generate a random stream ID (same method used by other protocols)
    return static_cast<uint16_t>(::rand());
}
