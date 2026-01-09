#pragma once

#include <string>
#include <nng/nng.h>
#include <nng/protocol/reqrep0/rep.h>

class CUSRPProtocol;

class CNNGControl
{
public:
    CNNGControl();
    ~CNNGControl();

    bool Start(const std::string &addr, CUSRPProtocol *pUSRP);
    void Stop();
    void Poll();

private:
    nng_socket m_sock;
    bool m_started;
    CUSRPProtocol *m_pUSRP;
};
