#include <iostream>
#include <string>
#include <memory>
#include <thread>
#include <boost/asio.hpp>

namespace ba = boost::asio;
namespace bai = boost::asio::ip;
using std::placeholders::_1;
using std::placeholders::_2;

class Client
{
public:
  Client(ba::io_context& context, const std::string& host, const std::string& port)
    : mSocket(context)
  {
    bai::tcp::resolver resolver(context);
    mEndpoints = resolver.resolve(host, port);
  }

  void Start()
  {
    ba::async_connect(mSocket, mEndpoints, std::bind(&Client::HandleConnect, this, _1, _2));
  }

  void SendMessage(const std::string& message)
  {
    std::string fullMessage = message + "\n";
    ba::async_write(mSocket, ba::buffer(fullMessage),
      [this, fullMessage] (const auto& error, auto sz) {
        HandleWrite(error, sz, fullMessage);});
  }

private:
  void HandleConnect(const boost::system::error_code& error,
                     const bai::tcp::endpoint& endpoint)
  {
    if (!error)
    {
      std::cout << "Connected to the server: " << endpoint << std::endl;
      StartRead();
    }
    else
    {
      std::cout << "Connect error: " << error.message() << std::endl;
    }
  }

  void StartRead()
  {
    ba::async_read_until(mSocket, mBuffer, '\n',
      std::bind(&Client::HandleRead, this, _1, _2));
  }

  void HandleRead(const boost::system::error_code& error, size_t bytesTransferred)
  {
    if (!error)
    {
      std::string receivedMessage = ba::buffer_cast<const char*>(mBuffer.data());
      std::cout << "Received: " << receivedMessage << std::endl;

      mBuffer.consume(bytesTransferred);

      StartRead();
    }
    else
    {
      std::cout << "Read error: " << error.message() << std::endl;
    }
  }

  void HandleWrite(const boost::system::error_code& error, size_t bytesTransferred, 
                   const std::string& message)
  {
    if (!error)
    {
      std::cout << "Send: " << message << std::endl;
    }
    else
    {
      std::cout << "Write error: " << error.message() << std::endl;
    }
  }

  bai::tcp::socket mSocket;
  bai::tcp::resolver::results_type mEndpoints;
  ba::streambuf mBuffer;
};

int main()
{
  try
  {
    ba::io_context context;

    Client c(context, "127.0.0.1", "12345");
    c.Start();

    std::thread inputThread([&context, &c] {
      std::string line;
      while (std::getline(std::cin, line))
      {
        if (line == "exit")
        {
          context.stop();
          break;
        }

        context.post([&c, line] () {
          c.SendMessage(line);
        });
      }
    });

    context.run();

    inputThread.join();
  }
  catch (const boost::system::system_error& error)
  {
    std::cout << "Client error: " << error.what() << std::endl;
  }

  return 0;
}
