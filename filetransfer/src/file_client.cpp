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


class Client
{
public:
  using ReceiveHandlerT = std::function<void(const boost::system::error_code &error, size_t sz,
                                             std::unique_ptr<filetransfer::ServerMessage>)>;

  Client(ba::io_context &context, const std::string &host, const std::string &port)
      : mSocket(context), mResolver(context)
  {
    mEndpoints = mResolver.resolve(host, port);
  }

  void Start()
  {
    ba::async_connect(mSocket, mEndpoints, std::bind(&Client::ConnectHandler, this, std::placeholders::_1, std::placeholders::_2));
  }

  void Send(const filetransfer::ClientMessage &message,
            std::function<void(const boost::system::error_code &, size_t)> handler)
  {
    AsyncWriteProtobufMessage(mSocket, message, handler);
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
      Read();
    }
    else
    {
      std::cout << "Connect error: " << error.message() << std::endl;
    }
  }

  void Read()
  {
    AsyncReadProtobufMessage<filetransfer::ServerMessage>(mSocket, mBuffer,
                                                          [this](const boost::system::error_code &error, size_t sz,
                                                                 std::unique_ptr<filetransfer::ServerMessage> message)
                                                          {
                                                            if (mReceiveHandler)
                                                            {
                                                              mReceiveHandler(error, sz, std::move(message));
                                                            }
                                                            // Continue reading unless there's an error
                                                            if (!error)
                                                            {
                                                              Read();
                                                            } else {
                                                              std::cout << "Read loop terminated due to error: " << error.message() << std::endl;
                                                            }
                                                          });
  }

  bai::tcp::socket mSocket;
  bai::tcp::resolver mResolver;
  bai::tcp::resolver::results_type mEndpoints;
  ba::streambuf mBuffer;
  ReceiveHandlerT mReceiveHandler;
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
  // Callback type for reporting final transfer status
  using TransferCompletionHandlerT = std::function<void(bool success, const std::string& filename)>;

  FileHandler(std::shared_ptr<Client> client)
      : mpClient(client)
  {
    mpClient->SetReceiveHandler(std::bind(&FileHandler::ReadHandler, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
  }

  // Start method now takes a callback for completion
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

    filetransfer::ClientMessage message;
    message.mutable_file_request()->set_filename(fs::path(mInputFilename).filename().string());
    message.mutable_file_request()->set_filesize(mInputFileSize);

    mpClient->Send(message, std::bind(&FileHandler::FileRequestSentHandler, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
  }

  void Stop()
  {
    if (mState != FileHandlerState::COMPLETED && mState != FileHandlerState::FAILED && mState != FileHandlerState::STOPPED)
    {
        mIsStopRequested = true;
        std::cout << "Stop requested for file transfer." << std::endl;
    }
    if (mInputFile.is_open())
    {
      mInputFile.close();
    }
  }

private:
  void FileRequestSentHandler(const boost::system::error_code& error, size_t /*bytes_transferred*/)
  {
      if (error)
      {
          std::cerr << "File request send error: " << error.message() << std::endl;
          mState = FileHandlerState::FAILED;
          SetTransferResult(false);
      }
      else
      {
          std::cout << "File request sent for: " << fs::path(mInputFilename).filename() << std::endl;
      }
  }

  void ReadHandler(const boost::system::error_code &error,
                   size_t /*bytesTransferred*/,
                   std::unique_ptr<filetransfer::ServerMessage> message)
  {
    if (error)
    {
      std::cerr << "Read error: " << error.message() << std::endl;
      mState = FileHandlerState::FAILED;
      SetTransferResult(false);
      return;
    }

    if (!message || !message->has_upload_status())
    {
      std::cerr << "Received invalid or unexpected message from server." << std::endl;
      mState = FileHandlerState::FAILED;
      SetTransferResult(false);
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
        SendNextChunk(0);
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
          SendNextChunk(bytesReceived);
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

  void ChunkSentHandler(const boost::system::error_code& error, size_t /*bytes_transferred*/)
  {
      if (error)
      {
          std::cerr << "\nError sending file chunk: " << error.message() << std::endl;
          mState = FileHandlerState::FAILED;
          SetTransferResult(false);
      }
  }

  void SendUploadFinishedMessage()
  {
      filetransfer::ClientMessage sendMessage;
      filetransfer::FileUploadFinished* uploadFinished = sendMessage.mutable_upload_finished();
      uploadFinished->set_filename(fs::path(mInputFilename).filename().string());
      uploadFinished->set_message("Upload Finished");

      mpClient->Send(sendMessage, std::bind(&FileHandler::UploadFinishedSentHandler, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
  }

  void UploadFinishedSentHandler(const boost::system::error_code& error, size_t /*bytes_transferred*/)
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
          mCompletionHandler = nullptr;
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
    client->Start();

    std::shared_ptr<FileHandler> fileHandler = std::make_shared<FileHandler>(client);

    std::thread io_thread([&context]() {
      context.run();
    });

    fileHandler->Start(argv[3], [&context](bool success, const std::string& filename) {
        std::cout << "\nFile transfer of " << filename << " completed with status: " << (success ? "SUCCESS" : "FAILED") << std::endl;
        context.stop();
    });

    io_thread.join();

  }
  catch (const std::exception &e)
  {
    std::cerr << "Client error: " << e.what() << '\n';
  }
  return 0;
}
