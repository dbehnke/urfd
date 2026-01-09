# NNG Control Channel Documentation

This document describes the NNG Control Channel in `urfd`, a mechanism for external applications (like `allstar-nexus` or a dashboard) to send commands to the reflector.

Unlike the [Event System](nng.md) which uses a **PUB/SUB** (Publish/Subscribe) pattern for one-way status updates, the Control Channel uses a **REQ/REP** (Request/Reply) pattern to execute commands and receive immediate feedback.

## Architecture

The `urfd` reflector acts as an NNG **Replier** (REP). External applications connect as **Requesters** (REQ) to send JSON-formatted commands.

```mermaid
sequenceDiagram
    participant EXT as External App (REQ)
    participant URFD as urfd Reflector (REP)

    EXT->>URFD: Connect to tcp://0.0.0.0:6001
    EXT->>URFD: Send JSON Command
    Note right of EXT: {"cmd": "usrp_register", ...}
    
    URFD->>URFD: Process Command
    URFD-->>EXT: Send JSON Response
    Note left of URFD: {"status": "ok"}
```

## Configuration

To enable the control channel, update your `urfd.ini` (or `config.ini`) in the `[Dashboard]` section:

```ini
[Dashboard]
; Enable the NNG Control Channel
ControlNNGEnable=true

; Bind address for the NNG REP socket
ControlNNGAddr=tcp://0.0.0.0:6001
```

## Commands

### 1. Register USRP Client (`usrp_register`)

Dynamically maps an IP address to a Callsign for USRP connections. This is useful for systems like AllStarLink where the incoming IP is known but the reflector protocol (USRP) does not natively carry the callsign in the header, or the header is not compliant.

**Request:**

```json
{
  "cmd": "usrp_register",
  "ip": "1.2.3.4",
  "callsign": "W1AW"
}
```

**Response (Success):**

```json
{
  "status": "ok",
  "message": "registered"
}
```

**Response (Error):**

```json
{
  "status": "error",
  "message": "missing ip or callsign" 
}
```

**Behavior:**

1. `urfd` updates its internal IP-to-Callsign map.
2. `urfd` **force-closes** any active USRP streams from that IP address.
3. The client (e.g. AllStar node) automatically reconnects (sends next packet).
4. `urfd` accepts the new connection, looks up the IP in the map, finds the new callsign, and attributes the stream to it.

## Integration Example (Mermaid)

The following diagram illustrates the flow when integrating with `allstar-nexus`:

```mermaid
sequenceDiagram
    participant ASL as ASL Node (USRP)
    participant AN as Allstar Nexus
    participant URFD as URFD Reflector

    Note over ASL,AN: 1. Node Keys Up (TX Start)
    ASL->>AN: AMI Event (RPT_ALINKS / TXKEYED)
    
    Note over AN: 2. Core Logic
    AN->>AN: Detect TX_START
    AN->>AN: Extract IP & Callsign
    
    Note over AN,URFD: 3. NNG Control
    AN->>URFD: NNG REQ: {cmd: "usrp_register", ip: "127.0.0.1", callsign: "KF8S"}
    
    Note over URFD: 4. Registration
    URFD->>URFD: Map 127.0.0.1 -> KF8S
    URFD->>URFD: Find existing stream from 127.0.0.1
    URFD->>URFD: Force Close Stream (Reset)
    URFD-->>AN: NNG REP: {status: "ok"}

    Note over ASL,URFD: 5. Reconnection
    ASL->>URFD: USRP Audio Packet
    URFD->>URFD: New Stream Created
    URFD->>URFD: Lookup 127.0.0.1 -> Found KF8S!
    URFD->>URFD: Process Audio as KF8S
```
