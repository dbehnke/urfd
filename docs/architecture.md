# URFD Architecture Reference

This document provides a high-level overview of the `urfd` (Universal Reflector) internal architecture and data flow.

## System Overview

`urfd` is a multi-protocol digital voice reflector. It accepts connections from various amateur radio protocols (D-Star/USRP, DMR, YSF, M17, etc.), normalizes the audio streams, and redistributes them to connected clients based on "Modules" (A-Z).

### Core Components Diagram

```mermaid
graph TD
    %% Nodes
    subgraph Interfaces
        direction TB
        UDP_USRP[UDP: USRP]
        UDP_YSF[UDP: YSF]
        UDP_DMR[UDP: DMR]
        UDP_M17[UDP: M17]
        UDP_P25[UDP: P25]
        UDP_NXDN[UDP: NXDN]
        TCP_DMR[TCP: DMR]
    end

    %% Spacer
    spac1[ ]:::empty

    subgraph "Core: CReflector (Singleton)"
        direction TB
        Reflector[Reflector Manager]
        
        subgraph "Protocol Handling"
            direction TB
            CProtos[CProtocols Manager]
            USRP[CUSRPProtocol]
            YSF[CYSFProtocol]
            DMR[CDMRProtocol]
            M17[CM17Protocol]
            P25[CP25Protocol]
            NXDN[CNXDNProtocol]
        end

        %% Spacer inside Core
        spac2[ ]:::empty

        subgraph "Session State"
            direction TB
            Clients[CClients: Active Connections]
            Users[CUsers: Callsign Database]
            Streams[m_Streams: Active Audio Streams]
        end

        %% Spacer
        spac3[ ]:::empty

        subgraph "Routing Engine"
            direction TB
            Router[Router Threads A-Z]
            TCD[TCD: Transcoder Lib]
            Recorder[Audio Recorder]
        end

        subgraph "External Control"
            direction TB
            NNG_REP["NNG Control (RPC)"]
            NNG_PUB["NNG Publisher (Events)"]
        end
    end

    subgraph "External Systems"
        direction TB
        Nexus["AllStar Nexus"]
        ASL["AllStar Link"]
        Dashboard["urfd-nng-dashboard"]
    end

    %% Force Vertical Stacking of Protocols
    UDP_USRP ~~~ UDP_YSF ~~~ UDP_DMR ~~~ UDP_M17 ~~~ UDP_P25 ~~~ UDP_NXDN ~~~ TCP_DMR
    USRP ~~~ YSF ~~~ DMR ~~~ M17 ~~~ P25 ~~~ NXDN
    
    %% Spacer connections
    UDP_USRP ~~~ spac1 ~~~ CProtos
    USRP ~~~ spac2 ~~~ Clients
    Clients ~~~ spac3 ~~~ Router

    %% Relationships - Input
    UDP_USRP --> USRP
    UDP_YSF --> YSF
    UDP_DMR --> DMR
    UDP_M17 --> M17
    UDP_P25 --> P25
    UDP_NXDN --> NXDN
    TCP_DMR --> DMR

    %% Protocol Management
    CProtos --> USRP
    CProtos --> YSF
    CProtos --> DMR
    CProtos --> M17
    CProtos --> P25
    CProtos --> NXDN

    %% Session Logic (All Protocols)
    USRP & YSF & DMR & M17 & P25 & NXDN --> Clients
    USRP & YSF & DMR & M17 & P25 & NXDN --> Users

    %% Streaming Flow
    USRP & YSF & DMR & M17 & P25 & NXDN -- "Push Packet" --> Streams
    Streams -- "Audio Queue" --> Router
    
    %% Routing Logic & Transcoding
    Router -- "Transcode?" --> TCD
    TCD -.-> Router
    Router -- "Write File" --> Recorder
    
    %% Output
    Router -- "Poll Stream" --> Streams
    Router -- "Get Subscribers" --> Clients
    Router -- "Write Packet" --> USRP & YSF & DMR & M17 & P25 & NXDN

    %% Events & Control
    Reflector --> NNG_PUB
    NNG_REP -- "Register/Kick" --> USRP
    
    %% External Interactions
    NNG_PUB -.-> Dashboard
    Nexus -- "Manage" --> ASL
    Nexus -- "USRP Register" --> NNG_REP
    ASL -- "Audio" --> UDP_USRP
    
    %% Future / Planned M17 Features
    subgraph "Planned M17 Features"
        direction TB
        M17_Parrot[M17 Parrot Echo]:::planned
        M17_Packet[Packet/SMS Handling]:::planned
    end
    
    M17 -.-> M17_Parrot
    M17 -.-> M17_Packet

    %% Styling with high contrast text for dark mode
    classDef protocol fill:#e1f5fe,stroke:#01579b,stroke-width:2px,color:#000;
    classDef core fill:#fff3e0,stroke:#e65100,stroke-width:2px,color:#000;
    classDef thread fill:#f3e5f5,stroke:#4a148c,stroke-width:2px,color:#000;
    classDef external fill:#e8f5e9,stroke:#2e7d32,stroke-width:2px,color:#000;
    classDef planned fill:#f5f5f5,stroke:#bdbdbd,stroke-width:2px,stroke-dasharray: 5 5,color:#616161;
    classDef empty width:0px,height:0px,stroke-width:0px,fill:none;
    
    class USRP,YSF,DMR,M17,P25,NXDN protocol;
    class Reflector,Clients,Users,Streams,CProtos,TCD,Recorder core;
    class Router thread;
    class ASL,Nexus,Dashboard external;
    class spac1,spac2,spac3 empty;
```

## Data Flow Description

1. **Ingestion (Protocol Threads)**
    * Each protocol (USRP, YSF, etc.) spawns a dedicated listening thread (or uses `select`/`poll` on sockets).
    * **Packet Handling**: Incoming UDP packets are validated.
    * **Session**: If it's a new connection (or "Key Up"), the protocol creates or looks up a `CClient` object in `CClients`.
    * **Streaming**: Valid audio frames are wrapped in a generic `CPacket` and pushed into a `CPacketStream` (associated with a specific Stream ID and Module).

2. **Routing (Router Threads)**
    * `urfd` spawns one **Router Thread** per Module (A through Z).
    * **Aggregation**: The thread checks all active `CPacketStream`s assigned to its module.
    * **Mixing/Selection**: It selects the active speaker (usually "first in" wins, locking out others until silence).
    * **Transcoding**: If the source and destination codecs differ (e.g., M17 3200 vs DMR AMBE), the **`TCD`** library is invoked to convert the audio payload.
    * **Recording**: The active stream is optionally written to disk by the **`AudioRecorder`**.
    * **Distribution**: The selected audio packet is sent to `CClients` to find all clients currently listening on that module.
    * **Output**: The packet is passed back to the appropriate Protocol handler to be encoded (if necessary) and sent over the network to each listener.

3. **State Management**
    * **CClients**: Maintains the list of connected nodes/repeaters, their IP addresses, protocols, and current Module selection.
    * **CUsers**: Tracks unique callsigns heard, often used for "Last Heard" lists and dashboards.

4. **Inter-Process Communication (NNG)**
    * **PUB (Publisher)**: `urfd` publishes events (e.g., `HEARING` payload with Callsign/Module) to a local socket. **`urfd-nng-dashboard`** subscribes to this to show real-time activity.
    * **REP (Replier)**: Accepts RPC commands to modify state. Used by **`AllStar Nexus`** to register USRP clients dynamically (IP-to-Callsign mapping) and manage connections.

## Class Responsibilities

* `CReflector`: The main application class. Initializes subsystems and owns the global state.
* `CProtocol`: Base class for all network protocols. Handles socket I/O.
* `CClient`: Represents a connected node. Stores state like "Is Transmitting?", "Current Module", "IP Address".
* `CPacketStream`: A jitter buffer/queue for incoming audio from a single source.
* `CNNGPublisher`: Serializes events to JSON and broadcasts them.
* `CNNGControl`: Handles incoming RPC commands (e.g., `usrp_register`).
* `TCD`: Wrapper for the Transcoder library, handling codec conversion.
* `AudioRecorder`: Manages writing audio streams to WAV/files.

## Future Plans

* **M17 Enhancements**:
  * **Parrot Echo**: Native echo functionality for M17 streams (currently missing).
  * **Packet/SMS**: Support for M17 data frames and text messaging.

## Module Switching & Control Logic

`urfd` supports dynamic module switching via protocol-specific metadata (DMR Talkgroups, YSF DG-ID, etc.). This allows users to change rooms/modules from their radio without a dashboard.

### DMR & YSF Logic Flow

```mermaid
graph TD
    %% Styling
    classDef logic fill:#fff3e0,stroke:#e65100,stroke-width:2px,color:#000;
    classDef decision fill:#e1f5fe,stroke:#01579b,stroke-width:2px,shape:rhombus,color:#000;
    classDef action fill:#e8f5e9,stroke:#2e7d32,stroke-width:2px,color:#000;

    subgraph "DMR Protocol"
        direction TB
        DMR_Start(Packet In):::logic --> IsXLX{XLX Mode?}:::decision
        
        %% XLX Path
        IsXLX -- Yes --> XLX_Check{Match RPT2?}:::decision
        XLX_Check -- Yes --> XLX_Route[Route Audio]:::action
        XLX_Check -- No --> XLX_Drop[Drop]:::action

        %% Mini DMR Path
        IsXLX -- No (Mini DMR) --> ParseTG[Parse Dest TG]:::logic
        ParseTG --> Is4000{TG 4000?}:::decision
        
        Is4000 -- Yes --> Unlink[Unlink / Disconnect]:::action
        Is4000 -- No --> MapTG{TG maps to Module?}:::decision
        
        MapTG -- Yes (e.g. 4001=A) --> SetMod[Set Client Module]:::action
        SetMod --> AddSub[Add Dynamic Subscription]:::action
        AddSub --> Route[Route Audio]:::action
        
        MapTG -- No --> CheckSub{Has Subscription?}:::decision
        CheckSub -- Yes --> Route
        CheckSub -- No --> Drop
    end

    subgraph "YSF Protocol"
        direction TB
        YSF_Start(Packet In):::logic --> CheckDGID{Check DG-ID}:::decision
        
        CheckDGID -- "10-35 (A-Z)" --> YSF_Switch[Switch Module]:::action
        CheckDGID -- "00 (None)" --> YSF_Route[Route to Current]:::action
        CheckDGID -- "Other" --> YSF_Ignore[Ignore / Route]:::action
        
        YSF_Switch --> YSF_Route
    end
```

## Audio Recording Flow

`urfd` includes an automated recording subsystem that archives active voice streams to disk.

### Architecture

Recording is tightly integrated with the **Transcoder (TCD)** pipeline. Because most digital voice modes (DMR, YSF, M17) use compressed codecs (AMBE, Codec2), the audio must first be **decoded to PCM** before it can be mixed or recorded.

```mermaid
graph LR
    %% Styles
    classDef buffer fill:#e1f5fe,stroke:#01579b,stroke-width:2px,color:#000;
    classDef process fill:#fff3e0,stroke:#e65100,stroke-width:2px,color:#000;
    classDef storage fill:#e8f5e9,stroke:#2e7d32,stroke-width:2px,color:#000;
    classDef external fill:#f3e5f5,stroke:#4a148c,stroke-width:2px,stroke-dasharray: 5 5,color:#000;

    Input(Compressed Audio):::buffer --> CodecStream
    
    subgraph "Transcoding Loop"
        direction TB
        CodecStream(CodecStream Buffer):::process -->|Send Packet| TCD_Socket(TCD Socket):::external
        TCD_Socket -->|Decode/Transcode| TCD_Service[Transcoder Service]:::external
        TCD_Service -->|Return PCM| TCD_Socket
        TCD_Socket -->|Receive PCM| CodecStream
    end

    CodecStream -->|Write PCM| Recorder[AudioRecorder]:::process
    
    subgraph "Encoding Subsystem"
        direction TB
        Recorder -->|PCM 8kHz Mono| OpusEnc[Opus Encoder]:::process
        OpusEnc -->|12kbps VBR| OggMux[Ogg Muxer]:::process
    end

    OggMux -->|Write File| Disk[(Filesystem)]:::storage
```

### Storage Format

Recordings are saved in **Ogg Opus** format, optimized for voice storage efficiency.

* **Codec**: Opus (VOIP profile)
* **Sample Rate**: 8kHz (Narrowband)
* **Channels**: Mono
* **Bitrate**: 12kbps (Variable Bitrate)
* **Frame Size**: 60ms

### Storage Requirements

Due to the high efficiency of Opus for speech, the storage footprint is minimal:

| Duration | Approx Size |
| :--- | :--- |
| **1 Minute** | ~90 KB |
| **1 Hour** | ~5.4 MB |
| **24 Hours** | ~130 MB |

Files are named using **UUIDv7**, ensuring they are unique and time-sortable (e.g., `hearing_01944e45-d8dc-7ce0-98cc-260538058721.opus`).

## NNG Event & Control System

`urfd` uses NNG (nanomsg) for high-performance Inter-Process Communication (IPC). This provides a loosely coupled integration point for dashboards, external controllers, and other services.

### Architecture Diagram

```mermaid
graph LR
    %% Styles
    classDef pub fill:#e3f2fd,stroke:#1565c0,stroke-width:2px,color:#000;
    classDef sub fill:#e8f5e9,stroke:#e65100,stroke-width:2px,color:#000;
    classDef req fill:#fff3e0,stroke:#e65100,stroke-width:2px,color:#000;
    classDef rep fill:#fce4ec,stroke:#c2185b,stroke-width:2px,color:#000;
    classDef urfd fill:#f5f5f5,stroke:#212121,stroke-width:2px,fill-opacity:0.3,stroke-dasharray: 5 5,color:#000;

    subgraph "URFD Core"
        direction TB
        Events[Event Publisher]:::pub
        Control[Control Listener]:::rep
    end

    subgraph "Subscribers"
        Dashboard[urfd-nng-dashboard]:::sub
        Monitor[Log Monitor]:::sub
    end

    subgraph "Controllers"
        Nexus[AllStar Nexus]:::req
        Admin[Admin Scripts]:::req
    end

    %% Use solid lines for flows
    Events -- "PUB (JSON)" --> Dashboard
    Events -- "PUB (JSON)" --> Monitor
    
    Nexus -- "REQ (cmd: usrp_register)" --> Control
    Admin -- "REQ (cmd: ...)" --> Control
    Control -. "REP (Success/Fail)" .-> Nexus
```

### Published Events (PUB)

The core publishes JSON events to the configured socket (default `ipc:///tmp/urfd_events`).

#### Hearing Event

Sent when a client transmits (Key Up).

```json
{
  "type": "hearing",
  "my": "N7TAE",
  "ur": "CQCQCQ",
  "rpt1": "RPT1",
  "rpt2": "RPT2",
  "module": "A",
  "protocol": "YSF"
}
```

#### Closing Event

Sent when a transmission ends (Key Down). Can include a recording path.

```json
{
  "type": "closing",
  "my": "N7TAE",
  "module": "A",
  "protocol": "YSF",
  "recording": "/var/log/urfd/recordings/hearing_... .opus"
}
```

#### Client Connection Events

Sent when a node connects or disconnects.

```json
{
  "type": "client_connect",
  "callsign": "W1AW-B",
  "ip": "203.0.113.45",
  "protocol": "DMR",
  "module": "B"
}
```

*(Similarly `client_disconnect` follows the same structure)*

#### Periodic State

Sent periodically (approx every 10s) containing the full reflector state (active usage, stats).

```json
{
  "type": "state",
  "clients": [...],
  "lastheard": [...]
}
```

### Control Commands (RPC/REP)

The core listens for JSON commands on a separate socket (default `ipc:///tmp/urfd_control`).

#### Register USRP Client

Used by external systems (like AllStar Nexus) to dynamically map an IP address to a callsign, allowing valid transmission without static config.

**Request:**

```json
{
  "cmd": "usrp_register",
  "ip": "203.0.113.10",
  "callsign": "W1AW"
}
```

**Response:**

```json
{
  "status": "ok",
  "message": "registered"
}
```
