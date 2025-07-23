# Asynchronous File Transfer with Boost.Asio

This repository showcases an asynchronous file transfer application built using **Boost.Asio** for TCP/IP communication. It includes both server and client components, where the client transfers a file to the server by packaging data into **Protobuf messages**.

### Install

```bash
mkdir build && cd build
conan install .. -of=. --build=missing -s build_type=Release
source conanbuild.sh
cmake .. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=./conan_toolchain.cmake
make
```

### Flow Diagram

```mermaid
sequenceDiagram
    participant User
    participant Client
    participant OS/Network
    participant Server
    participant ServerSession

    User->>Client: Start Transfer (filepath)
    activate Client
    Client->>OS/Network: async_connect()
    activate OS/Network
    Server->>Server: Server Init (async_accept)
    activate Server
    OS/Network-->>Client: Connection Established
    Client->>Client: Call Client::ConnectHandler()
    Client->>Client: Start Client::Read() loop (async_read Header)
    Client->>Client: Notify FileHandler (Connection Ready)
    deactivate OS/Network

    Client->>Client: FileHandler::SendInitialFileRequest()
    Client->>OS/Network: ClientMessage.FileRequest (async_write)
    activate OS/Network
    OS/Network->>ServerSession: ClientMessage.FileRequest (ReadHandler)
    activate ServerSession
    ServerSession->>ServerSession: Process FileRequest
    ServerSession->>OS/Network: ServerMessage.UploadStatus (initial success) (async_write)
    deactivate ServerSession
    OS/Network->>Client: ServerMessage.UploadStatus (ReadHandler)
    activate Client
    Client->>Client: FileHandler::ReadHandler() (INIT -> TRANSFER)
    Client->>Client: FileHandler::SendNextChunk(0)
    deactivate Client

    loop File Chunk Transfer
        Client->>OS/Network: ClientMessage.FileChunk (async_write)
        activate OS/Network
        OS/Network->>ServerSession: ClientMessage.FileChunk (ReadHandler)
        activate ServerSession
        ServerSession->>ServerSession: Process FileChunk (write to file)
        ServerSession->>OS/Network: ServerMessage.UploadStatus (progress) (async_write)
        deactivate ServerSession
        OS/Network->>Client: ServerMessage.UploadStatus (ReadHandler)
        activate Client
        Client->>Client: FileHandler::ReadHandler() (TRANSFER)
        alt More chunks to send
            Client->>Client: FileHandler::SendNextChunk()
        else All chunks sent locally
            Client->>Client: FileHandler::SendUploadFinishedMessage() (TRANSFER -> COMPLETE_CHECK)
        end
        deactivate Client
    end

    Client->>OS/Network: ClientMessage.FileUploadFinished (async_write)
    activate OS/Network
    OS/Network->>ServerSession: ClientMessage.FileUploadFinished (ReadHandler)
    activate ServerSession
    ServerSession->>ServerSession: Process FileUploadFinished (final check)
    ServerSession->>OS/Network: ServerMessage.UploadStatus (final SUCCESS) (async_write)
    ServerSession->>ServerSession: In AsyncWrite handler for final Status:
    ServerSession->>OS/Network: socket.shutdown() & socket.close()
    deactivate ServerSession
    deactivate OS/Network

    OS/Network--xClient: EOF (boost::asio::error::eof)
    activate Client
    Client->>Client: Client::ReadHandler() (sees EOF, terminates read loop)
    Client->>Client: FileHandler::SetTransferResult(true)
    Client->>Client: FileHandler::CompletionHandler (in main lambda)
    Client->>Client: context.stop()
    deactivate Client

    OS/Network--xServer: Operation Aborted (if any pending reads on server's side)
    Server->>Server: ReadHandler (sees Operation Aborted, terminates read loop)
    deactivate Server

    Client->>Client: io_context::run() finishes
    Server->>Server: io_context::run() finishes
```
