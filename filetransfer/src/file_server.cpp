#include "common.h"
#include "filetransfer.pb.h"
#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <fstream>
#include <memory>

namespace ba = boost::asio;
namespace bai = boost::asio::ip;

class Session : public std::enable_shared_from_this<Session> {
  public:
    Session(ba::io_context& context) : mSocket(std::make_shared<bai::tcp::socket>(context)) 
    {
      mBuffer.prepare(sizeof(ProtocolHeader));
    }

    bai::tcp::socket& GetSocket()
    {
      return *mSocket;
    }

    void Start() 
    {
      ReadHeader();
    }

  private:
    void ReadHeader() 
    {
      auto self(shared_from_this());
      AsyncReadProtobufMessageHeader(mSocket, mBuffer,
                                [self](const boost::system::error_code &error, size_t sz,
                                  std::shared_ptr<ProtocolHeader> header) {
                                    self->HandleReadHeader(error, sz, header);
                                });
    }

    void ReadPayload(std::shared_ptr<ProtocolHeader> pHeader)
    {
      auto self(shared_from_this());
      mData.clear();
      mData.resize(pHeader->mPayloadSize);
      AsyncReadProtobufMessagePayload<filetransfer::ClientMessage>(mSocket, mData, pHeader,
                                [self](const boost::system::error_code &error, size_t sz,
                                  std::shared_ptr<filetransfer::ClientMessage> message) {
                                    self->HandleReadPayload(error, sz, message);
                                });
    }

    void HandleReadHeader(const boost::system::error_code& error, size_t transferredByte,
                          std::shared_ptr<ProtocolHeader> header) 
    {
      if (!error && header)
      {
        auto self(shared_from_this());
        self->ReadPayload(header);
      }
      else
      {
        std::cout << "Error in HandleReadHeader: " << error.message() << std::endl;
      }
    }


    void HandleReadPayload(const boost::system::error_code& error, size_t transferredByte,
                    std::shared_ptr<filetransfer::ClientMessage> message) 
    {
      if (!error && message)
      {
        switch (message->content_case())
        {
          case filetransfer::ClientMessage::kFileRequest:
            HandleFileRequest(message->file_request());
            break;
          case filetransfer::ClientMessage::kFileChunk:
            HandleFileChunk(message->file_chunk());
            break;
          case filetransfer::ClientMessage::kUploadFinished:
            HandleUploadFinished(message->upload_finished());
            break;
          default:
            std::cout << "Unknown ClientMessage type" << std::endl;
            break;
        }
        ReadHeader();
      }
      else
      {
        if (mOut.is_open())
        {
          mOut.close();
        }
        std::cout << "Error in HandleReadPayload: " << error.message() << std::endl;
      }
    }

    void HandleFileRequest(const filetransfer::FileTransferRequest& request)
    {
      mCurrentFilename = request.filename();
      mCurrentFileSize = request.filesize();
      mBytesReceived = 0;

      std::string targetPath = "uploads/" + mCurrentFilename;
      boost::filesystem::path filePath(targetPath);
      if (boost::filesystem::exists(filePath))
      {
        std::cerr << "File is already exists. It will be overridden" << std::endl;
      }

      boost::filesystem::create_directories("uploads");

      mOut.open(targetPath, std::ios_base::binary | std::ios_base::trunc);
      if (!mOut.is_open())
      {
        std::cerr << "File couldn't be open: " << targetPath << std::endl;
        SendUploadStatus(request.filename(), "File couldn't be open", false, 0);
        return;
      }

      std::cout << "File transfer request is received: " << mCurrentFilename << std::endl;
      SendUploadStatus(request.filename(), "File transfer request is received", true, 0);
    }

    void HandleFileChunk(const filetransfer::FileChunk& chunk)
    {
      if (!mOut.is_open() || chunk.filename() != mCurrentFilename)
      {
        std::cerr << "Wrong filename" << std::endl;
        SendUploadStatus(chunk.filename(), "Wrong filename", false, 0);
        return;
      }

      mOut.seekp(chunk.offset(), std::ios_base::beg);
      mOut.write(chunk.data().c_str(), chunk.data().length());

      mBytesReceived += chunk.data().length();

      std::cout << "Received: " << mBytesReceived << " Remaining: "
                << static_cast<double>(mBytesReceived) / mCurrentFileSize * 100.0
                << "%" << std::endl;
        
      if (mBytesReceived >= mCurrentFileSize || chunk.is_last_chunk())
      {
        std::cout << "All bytes received: " << mCurrentFilename << std::endl;
        SendUploadStatus(chunk.filename(), "All bytes received", true, mBytesReceived);
      }
      else
      {
        SendUploadStatus(chunk.filename(), "Bytes received", true, mBytesReceived);
      }
    }

    void HandleUploadFinished(const filetransfer::FileUploadFinished& finished)
    {
      if (finished.filename() == mCurrentFilename && mOut.is_open())
      {
        mOut.close();
        std::cout << "File transfer completed: " << mCurrentFilename << std::endl;
        SendUploadStatus(mCurrentFilename, "File transfer completed", true, mCurrentFileSize);
      }
    }

    void SendUploadStatus(const std::string& filename, const std::string& statusMsg,
                          bool success, uint64_t receivedBytes)
    {
      filetransfer::ServerMessage serverMsg;
      filetransfer::FileUploadStatus* status = serverMsg.mutable_upload_status();
      status->set_filename(filename);
      status->set_status_message(statusMsg);
      status->set_success(success);
      status->set_bytes_received(receivedBytes);

      AsyncWriteProtobufMessage(*mSocket, serverMsg, [] (const auto& error, auto /* sz */) {
        if (error)
        {
          std::cerr << "SendUploadStatus write error: " << error.message() << std::endl;
        }
      });
    }

    void HandleWrite(const boost::system::error_code& error, size_t transferredByte) {
      if (error)
      {
        std::cerr << "Error in HandleWrite: " << error.message() << std::endl;
      }
    }

  private:
    std::shared_ptr<bai::tcp::socket> mSocket;
    ba::streambuf mBuffer;
    std::vector<char> mData;
    std::ofstream mOut;
    std::string mCurrentFilename{""};
    size_t mCurrentFileSize{0};
    size_t mBytesReceived{0};
};

class Server
{
public:
  Server(ba::io_context& context)
    : mContext(context),
      mAcceptor(context, bai::tcp::endpoint(bai::tcp::v4(), 12345))
  {}

  void StartAccept()
  {
    auto session = std::make_shared<Session>(mContext);

    mAcceptor.async_accept(session->GetSocket(), std::bind(&Server::HandleAccept, this, session, std::placeholders::_1));
  }

private:
  void HandleAccept(std::shared_ptr<Session> session, const boost::system::error_code& error)
  {
    if (!error)
    {
      std::cout << "New connection has been established: " << session->GetSocket().remote_endpoint() << std::endl;
      session->Start();
    }
    else
    {
      std::cerr << "Error in accept: " << error.message() << std::endl;
    }

    StartAccept();
  }

  ba::io_context& mContext;
  bai::tcp::acceptor mAcceptor;
};

int main()
{
  try
  {
    ba::io_context context;
    Server server(context);
    server.StartAccept();
    std::cout << "Server is listening Port 12345" << std::endl;
    
    context.run();
  }
  catch (const std::exception& e)
  {
    std::cerr << "Server error: " << e.what() << std::endl;
  }

  return 0;
}
