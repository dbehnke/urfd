#include "NNGControl.h"
#include "USRPProtocol.h"
#include <iostream>
#include <nlohmann/json.hpp>

CNNGControl::CNNGControl() : m_started(false), m_pUSRP(nullptr)
{
    m_sock.id = 0;
}

CNNGControl::~CNNGControl()
{
    Stop();
}

bool CNNGControl::Start(const std::string &addr, CUSRPProtocol *pUSRP)
{
    if (m_started) return true;

    int rv;
    if ((rv = nng_rep0_open(&m_sock)) != 0) {
        std::cerr << "NNG Control: Failed to open rep socket: " << nng_strerror(rv) << std::endl;
        return false;
    }

    if ((rv = nng_listen(m_sock, addr.c_str(), nullptr, 0)) != 0) {
        std::cerr << "NNG Control: Failed to listen on " << addr << ": " << nng_strerror(rv) << std::endl;
        nng_close(m_sock);
        return false;
    }

    m_pUSRP = pUSRP;
    m_started = true;
    std::cout << "NNG Control: Listening at " << addr << std::endl;
    return true;
}

void CNNGControl::Stop()
{
    if (!m_started) return;
    nng_close(m_sock);
    m_started = false;
}

void CNNGControl::Poll()
{
    if (!m_started) return;

    char *buf = NULL;
    size_t sz;
    int rv = nng_recv(m_sock, &buf, &sz, NNG_FLAG_ALLOC | NNG_FLAG_NONBLOCK);
    if (rv == 0) {
        std::string msg(buf, sz);
        nng_free(buf, sz);

        // Process message
        try {
            auto j = nlohmann::json::parse(msg);
            nlohmann::json response;
            response["status"] = "error";
            response["message"] = "unknown command";

            if (j.contains("cmd")) {
                std::string cmd = j["cmd"];
                if (cmd == "usrp_register") {
                    if (j.contains("ip") && j.contains("callsign")) {
                        std::string ip = j["ip"];
                        std::string callsign = j["callsign"];
                        if (m_pUSRP) {
                            m_pUSRP->RegisterClient(ip, callsign);
                            response["status"] = "ok";
                            response["message"] = "registered";
                            std::cout << "NNG Control: Registered " << callsign << " at " << ip << std::endl;
                        } else {
                            response["message"] = "usrp protocol not active";
                        }
                    } else {
                        response["message"] = "missing ip or callsign";
                    }
                }
            }

            std::string respParams = response.dump();
            nng_send(m_sock, (void*)respParams.c_str(), respParams.size(), 0);

        } catch (const std::exception& e) {
             std::cerr << "NNG Control: JSON error: " << e.what() << std::endl;
             std::string err = "{\"status\":\"error\",\"message\":\"json parse error\"}";
             nng_send(m_sock, (void*)err.c_str(), err.size(), 0);
        }
    } else if (rv != NNG_EAGAIN) {
        // std::cerr << "NNG Control: Recv error: " << nng_strerror(rv) << std::endl;
    }
}
