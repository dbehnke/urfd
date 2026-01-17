#include "NNGVoiceStream.h"
#include <iostream>
#include <cstring>

CNNGVoiceStream::CNNGVoiceStream(char module)
    : m_Module(module)
    , m_IsStreaming(false)
    , m_SocketOpen(false)
    , m_Encoder(nullptr)
{
    m_Socket.id = 0;
}

CNNGVoiceStream::~CNNGVoiceStream()
{
    Stop();
}

void CNNGVoiceStream::Cleanup()
{
    if (m_Encoder) {
        opus_encoder_destroy(m_Encoder);
        m_Encoder = nullptr;
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

    InitOpus();
    if (!m_Encoder) {
        std::cerr << "NNGVoiceStream[" << m_Module << "]: Failed to initialize Opus encoder" << std::endl;
        Cleanup();
        return false;
    }

    m_IsStreaming = true;
    std::cout << "NNGVoiceStream[" << m_Module << "]: Started at " << addr << std::endl;
    return true;
}

void CNNGVoiceStream::Stop()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_IsStreaming) {
        std::cout << "NNGVoiceStream[" << m_Module << "]: Stopped" << std::endl;
    }
    Cleanup();
}

void CNNGVoiceStream::InitOpus()
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
