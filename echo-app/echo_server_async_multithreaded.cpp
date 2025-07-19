#include <iostream>
#include <string>
#include <memory>
#include <thread>
#include <chrono>
#include <boost/asio.hpp>

namespace ba = boost::asio;
namespace bai = boost::asio::ip;

class Session : public std::enable_shared_from_this<Session> {
public:
  Session(ba::io_context& context)
    : mSocket(context)
  {}

  bai::tcp::socket& Socket()
  {
    return mSocket;
  }

  void Start()
  {
    ba::async_read_until(mSocket, mBuffer, '\n',
      std::bind(&Session::HandleRead, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
  }

private:
  bai::tcp::socket mSocket;
  ba::streambuf mBuffer;

  void HandleRead(const boost::system::error_code& error, size_t bytesTransferred)
  {
    if (!error)
    {
      std::string message = ba::buffer_cast<const char*>(mBuffer.data());
      std::cout << "Received: " << message << " Thread: " << std::this_thread::get_id() <<std::endl;
      // std::this_thread::sleep_for(std::chrono::milliseconds(100));
      ba::async_write(mSocket, ba::buffer(message),
        std::bind(&Session::HandleWrite, shared_from_this(), std::placeholders::_1, std::placeholders::_2, message));

      mBuffer.consume(bytesTransferred);
    }
    else if (error == ba::error::eof)
    {
      std::cout << "Client connection has been closed (EOF): " << mSocket.remote_endpoint() << std::endl; 
    }
    else
    {
      std::cout << "Read failure (" << mSocket.remote_endpoint() << "): " << error.message() << std::endl;
    }
  }

  void HandleWrite(const boost::system::error_code& error, size_t bytesTransferred, std::string message)
  {
    if (!error)
    {
      std::cout << "Send: " << message << std::endl;

      ba::async_read_until(mSocket, mBuffer, '\n',
        std::bind(&Session::HandleRead, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
    }
    else
    {
      std::cout << "Write failure (" << mSocket.remote_endpoint() << "): " << error.message() << std::endl;
    }
  }
};

class Server
{
public:
  Server(ba::io_context& context, int port)
    : mContext(context), mAcceptor(context, bai::tcp::endpoint(bai::tcp::v4(), 12345))
  {
    StartAccept();
  }

  void StartAccept()
  {
    auto session = std::make_shared<Session>(mContext);

    mAcceptor.async_accept(session->Socket(), std::bind(&Server::HandleAccept, this, session, std::placeholders::_1));
  }

  void HandleAccept(std::shared_ptr<Session> session,
                    const boost::system::error_code& error)
  {
    if (!error)
    {
      std::cout << "New connection has been accepted: " << session->Socket().remote_endpoint() << std::endl;
      session->Start();
    }
    else
    {
      std::cout << "Accept error: " << error.message() << std::endl;
    }

    StartAccept();
  }

private:
  ba::io_context& mContext;
  bai::tcp::acceptor mAcceptor;
};


int main()
{
  try
  {
    ba::io_context context;
    Server s(context, 12345);

    std::cout << "Async server is listening Port 12345" << std::endl;

    std::vector<std::thread> threads;
    for (size_t i = 0; i < 4; i++)
    {
      threads.emplace_back([&context] {
        context.run();
      });
    }
    
    for (auto& t : threads)
    {
      t.join();
    }
  }
  catch (const boost::system::system_error& error)
  {
    std::cout << "Server error: " << error.what() << std::endl;
  }
  return 0;
}