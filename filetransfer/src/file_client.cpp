#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include "common.h"
#include "filetransfer.pb.h"

namespace ba = boost::asio;
namespace bai = boost::asio::ip;
namespace fs = boost::filesystem;

class Client : public std::enable_shared_from_this<Client>
{
public:
  using ReceiveHandlerT = std::function<void(const boost::system::error_code &error, size_t sz,
                                             std::shared_ptr<filetransfer::ServerMessage>)>;
  using ConnectCompletionHandlerT = std::function<void(const boost::system::error_code &error)>;

  Client(ba::io_context &context, const std::string &host, const std::string &port)
      : mContext(context), mpSocket(std::make_shared<bai::tcp::socket>(context)), mResolver(context)
  {
    mEndpoints = mResolver.resolve(host, port);
  }

  void Start(ConnectCompletionHandlerT connectHandler)
  {
    mConnectCompletionHandler = connectHandler;
    ba::async_connect(*mpSocket, mEndpoints, std::bind(&Client::ConnectHandler, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
  }

  void Stop()
  {
    mContext.post([this] () { mpSocket->close(); });
  }

  void Send(const filetransfer::ClientMessage &message,
            std::function<void(const boost::system::error_code &, size_t)> handler)
  {
    auto self(shared_from_this());
    AsyncWriteProtobufMessage(*mpSocket, message, handler);
  }

  void SetReceiveHandler(const ReceiveHandlerT &handler)
  {
    mReceiveHandler = handler;
  }

private:
  void ConnectHandler(const boost::system::error_code &error, const bai::tcp::endpoint &endpoint)
  {
    if (!error)
    {
      std::cout << "Client is connected to the server: " << endpoint << std::endl;
      ReadHeader();
    }
    else
    {
      std::cerr << "Connect error: " << error.message() << std::endl;
    }

    if (mConnectCompletionHandler)
    {
      mConnectCompletionHandler(error);
    }
  }

  void ReadHeader()
  {
    auto self(shared_from_this());
    AsyncReadProtobufMessageHeader(mpSocket, mBuffer,
                                   [self](const boost::system::error_code &error, size_t sz,
                                          std::shared_ptr<ProtocolHeader> header)
                                   {
                                     self->HandleReadHeader(error, sz, header);
                                   });
  }

  void ReadPayload(std::shared_ptr<ProtocolHeader> pHeader)
  {
    auto self(shared_from_this());
    mData.clear();
    mData.resize(pHeader->mPayloadSize);
    std::cout << "mBuffer size: " << mData.size() << std::endl;
    AsyncReadProtobufMessagePayload<filetransfer::ServerMessage>(mpSocket, mData, pHeader,
                                                                 [self](const boost::system::error_code &error, size_t sz,
                                                                        std::shared_ptr<filetransfer::ServerMessage> message)
                                                                 {
                                                                   self->HandleReadPayload(error, sz, message);
                                                                 });
  }

  void HandleReadHeader(const boost::system::error_code &error, size_t transferredByte,
                        std::shared_ptr<ProtocolHeader> header)
  {
    if (!error && header)
    {
      auto self(shared_from_this());
      std::cout << "Read payload" << std::endl;
      self->ReadPayload(header);
    }
    else
    {
      if (error == ba::error::eof)
      {
        std::cout << "EOF error" << std::endl;
        return;
      }
      std::cout << "Error in HandleReadHeader: " << error.message() << std::endl;
    }
  }

  void HandleReadPayload(const boost::system::error_code &error, size_t transferredByte,
                         std::shared_ptr<filetransfer::ServerMessage> message)
  {
    if (!error && message)
    {
      if (mReceiveHandler)
      {
        mReceiveHandler(error, transferredByte, message);
      }
      ReadHeader();
    }
    else
    {
      if (error == ba::error::eof)
      {
        std::cout << "EOF error" << std::endl;
        return;
      }

      std::cout << "Error in HandleReadPayload: " << error.message() << std::endl;
    }
  }

  ba::io_context& mContext;
  std::shared_ptr<bai::tcp::socket> mpSocket;
  bai::tcp::resolver mResolver;
  bai::tcp::resolver::results_type mEndpoints;
  ba::streambuf mBuffer;
  std::vector<char> mData;
  ReceiveHandlerT mReceiveHandler;
  ConnectCompletionHandlerT mConnectCompletionHandler;
};

enum class FileHandlerState : uint8_t
{
  INIT = 0,
  TRANSFER = 1,
  COMPLETE_CHECK = 2,
  COMPLETED = 3,
  FAILED = 4,
  STOPPED = 5
};

class FileHandler : public std::enable_shared_from_this<FileHandler>
{
public:
  using TransferCompletionHandlerT = std::function<void(bool success, const std::string &filename)>;

  FileHandler(std::shared_ptr<Client> client)
      : mpClient(client)
  {
    mpClient->SetReceiveHandler(std::bind(&FileHandler::ReadHandler, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
  }

  void Start(std::string filename, TransferCompletionHandlerT completionHandler)
  {
    mInputFilename = filename;
    mCompletionHandler = completionHandler;

    fs::path filePath(mInputFilename);

    if (!fs::exists(filePath))
    {
      std::cerr << "Error: File not found: " << mInputFilename << std::endl;
      mState = FileHandlerState::FAILED;
      SetTransferResult(false);
      return;
    }

    mInputFileSize = fs::file_size(filePath);
    mInputFile.open(mInputFilename, std::ios_base::binary);
    if (!mInputFile.is_open())
    {
      std::cerr << "Error: Input file could not be opened: " << mInputFilename << std::endl;
      mState = FileHandlerState::FAILED;
      SetTransferResult(false);
      return;
    }
  }

  void SendInitialFileRequest()
  {
    if (mState != FileHandlerState::INIT)
    {
      std::cerr << "FileHandler already started or in a non-initial state." << std::endl;
      return;
    }

    filetransfer::ClientMessage message;
    message.mutable_file_request()->set_filename(fs::path(mInputFilename).filename().string());
    message.mutable_file_request()->set_filesize(mInputFileSize);

    std::cout << "Sending file transfer request for: " << fs::path(mInputFilename).filename() << std::endl;
    mpClient->Send(message, std::bind(&FileHandler::FileRequestSentHandler, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
  }

  void Stop()
  {
    if (mState != FileHandlerState::COMPLETED && mState != FileHandlerState::FAILED && mState != FileHandlerState::STOPPED)
    {
      mIsStopRequested = true;
      std::cout << "\nStop requested for file transfer." << std::endl;
    }
    if (mInputFile.is_open())
    {
      mInputFile.close();
    }
  }

private:
  void FileRequestSentHandler(const boost::system::error_code &error, size_t /*bytes_transferred*/)
  {
    if (error)
    {
      std::cerr << "\nFile request send error: " << error.message() << std::endl;
      mState = FileHandlerState::FAILED;
      SetTransferResult(false);
    }
    else
    {
      // Request sent, now waiting for server's initial status message in ReadHandler.
      // No state change here.
    }
  }

  void ReadHandler(const boost::system::error_code &error,
                   size_t /*bytesTransferred*/,
                   std::shared_ptr<filetransfer::ServerMessage> message)
  {

    if (error)
    {
      if (mState != FileHandlerState::COMPLETED && mState != FileHandlerState::FAILED && mState != FileHandlerState::STOPPED)
      {
        mState = FileHandlerState::FAILED;
        SetTransferResult(false);
      }
      return;
    }

    if (!message || !message->has_upload_status())
    {
      std::cerr << "Received invalid or unexpected message from server (no upload status)." << std::endl;
      if (mState != FileHandlerState::COMPLETED && mState != FileHandlerState::FAILED && mState != FileHandlerState::STOPPED)
      {
        mState = FileHandlerState::FAILED;
        SetTransferResult(false);
      }
      return;
    }

    const auto &status = message->upload_status();
    const auto success = status.success();
    const auto filename = status.filename();
    const auto statusMessage = status.status_message();
    const auto bytesReceived = status.bytes_received();

    std::cout << "\r" << "Server Status for " << filename << ": " << statusMessage
              << " (" << bytesReceived << " bytes received by server)";
    std::flush(std::cout);

    if (mIsStopRequested)
    {
      std::cout << "\nFile transfer stopped by request." << std::endl;
      mState = FileHandlerState::STOPPED;
      SetTransferResult(false);
      return;
    }

    switch (mState)
    {
    case FileHandlerState::INIT:
    {
      if (success)
      {
        mState = FileHandlerState::TRANSFER;
        SendNextChunk(0); // Start sending first chunk
      }
      else
      {
        mState = FileHandlerState::FAILED;
        std::cerr << "\nTransfer initialization error from server: " << statusMessage << std::endl;
        SetTransferResult(false);
      }
      break;
    }
    case FileHandlerState::TRANSFER:
    {
      if (success)
      {
        if (bytesReceived >= mInputFileSize)
        {
          mState = FileHandlerState::COMPLETE_CHECK;
          SendUploadFinishedMessage();
        }
        else
        {
          SendNextChunk(bytesReceived); // Send next chunk based on server's received bytes
        }
      }
      else
      {
        std::cerr << "\nTransfer error from server: " << statusMessage << std::endl;
        mState = FileHandlerState::FAILED;
        SetTransferResult(false);
      }
      break;
    }
    case FileHandlerState::COMPLETE_CHECK:
    {
      if (success && bytesReceived >= mInputFileSize)
      {
        std::cout << "\nTransfer completed successfully: " << filename << std::endl;
        mState = FileHandlerState::COMPLETED;
        SetTransferResult(true);
      }
      else
      {
        std::cerr << "\nTransfer completion check error or size mismatch: " << statusMessage << std::endl;
        mState = FileHandlerState::FAILED;
        SetTransferResult(false);
      }
      break;
    }
    case FileHandlerState::COMPLETED:
    case FileHandlerState::FAILED:
    case FileHandlerState::STOPPED:
    default:
    {
      std::cerr << "\nUnexpected message in terminal state " << static_cast<int>(mState) << ": " << statusMessage << std::endl;
      break;
    }
    };
  }

  void SendNextChunk(uint64_t offset)
  {
    if (mIsStopRequested)
    {
      mState = FileHandlerState::STOPPED;
      SetTransferResult(false);
      return;
    }

    if (!mInputFile.is_open())
    {
      std::cerr << "File is not open, cannot send chunk." << std::endl;
      mState = FileHandlerState::FAILED;
      SetTransferResult(false);
      return;
    }

    if (offset >= mInputFileSize)
    {
      std::cout << "\nAll local data read. Sending finalization message." << std::endl;
      SendUploadFinishedMessage();
      return;
    }

    mInputFile.seekg(offset);
    if (mInputFile.fail())
    {
      std::cerr << "File seek offset failed: " << offset << std::endl;
      mState = FileHandlerState::FAILED;
      SetTransferResult(false);
      return;
    }

    std::vector<char> chunkData(CHUNK_SIZE);
    mInputFile.read(chunkData.data(), CHUNK_SIZE);
    size_t bytesRead = mInputFile.gcount();

    if (bytesRead == 0)
    {
      std::cerr << "\nNo bytes read from file at offset " << offset << ". Unexpected." << std::endl;
      mState = FileHandlerState::FAILED;
      SetTransferResult(false);
      return;
    }

    filetransfer::ClientMessage sendMessage;
    filetransfer::FileChunk *fileChunk = sendMessage.mutable_file_chunk();
    fileChunk->set_filename(fs::path(mInputFilename).filename().string());
    fileChunk->set_offset(offset);
    fileChunk->set_data(chunkData.data(), bytesRead);
    fileChunk->set_is_last_chunk((offset + bytesRead) >= mInputFileSize);

    mpClient->Send(sendMessage, std::bind(&FileHandler::ChunkSentHandler, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
  }

  void ChunkSentHandler(const boost::system::error_code &error, size_t /*bytes_transferred*/)
  {
    if (error)
    {
      std::cerr << "\nError sending file chunk: " << error.message() << std::endl;
      mState = FileHandlerState::FAILED;
      SetTransferResult(false);
    }
    // If successful, the next action is driven by the server's status message (ReadHandler)
  }

  void SendUploadFinishedMessage()
  {
    filetransfer::ClientMessage sendMessage;
    filetransfer::FileUploadFinished *uploadFinished = sendMessage.mutable_upload_finished();
    uploadFinished->set_filename(fs::path(mInputFilename).filename().string());
    uploadFinished->set_message("Upload Finished");

    mpClient->Send(sendMessage, std::bind(&FileHandler::UploadFinishedSentHandler, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
  }

  void UploadFinishedSentHandler(const boost::system::error_code &error, size_t /*bytes_transferred*/)
  {
    if (error)
    {
      std::cerr << "\nError sending upload finished message: " << error.message() << std::endl;
      mState = FileHandlerState::FAILED;
      SetTransferResult(false);
    }
    else
    {
      std::cout << "\nUpload finished message sent. Waiting for final server confirmation." << std::endl;
    }
  }

  void SetTransferResult(bool success)
  {
    if (mCompletionHandler && (mState == FileHandlerState::COMPLETED || mState == FileHandlerState::FAILED || mState == FileHandlerState::STOPPED))
    {
      mCompletionHandler(success, mInputFilename);
      mCompletionHandler = nullptr; // Clear the handler to prevent multiple calls
    }

    if (mInputFile.is_open())
    {
      mInputFile.close();
    }
  }

  std::shared_ptr<Client> mpClient;
  FileHandlerState mState{FileHandlerState::INIT};
  bool mIsStopRequested{false};
  std::ifstream mInputFile;
  std::string mInputFilename;
  uint64_t mInputFileSize = 0;
  TransferCompletionHandlerT mCompletionHandler;
};

int main(int argc, char *argv[])
{
  try
  {
    if (argc != 4)
    {
      std::cerr << "Usage: " << argv[0] << " <host> <port> <filepath>\n";
      std::cerr << "Example: " << argv[0] << " 127.0.0.1 12345 my_document.txt\n";
      return 1;
    }

    ba::io_context context;
    std::shared_ptr<Client> client = std::make_shared<Client>(context, argv[1], argv[2]);
    std::shared_ptr<FileHandler> fileHandler = std::make_shared<FileHandler>(client);

    client->Start([&context, fileHandler](const boost::system::error_code &error)
                  {
        if (!error) {
            // Connection successful, now initiate the file transfer request
            fileHandler->SendInitialFileRequest();
        } else {
            std::cerr << "Failed to connect to server, terminating." << std::endl;
            context.stop();
        } });

    fileHandler->Start(argv[3], [&context](bool success, const std::string &filename)
                       {
                         std::cout << "\nFile transfer of " << filename << " completed with status: " << (success ? "SUCCESS" : "FAILED") << std::endl;
                         context.stop();
                       });

    context.run();
  }
  catch (const std::exception &e)
  {
    std::cerr << "Client error: " << e.what() << '\n';
  }
  return 0;
}