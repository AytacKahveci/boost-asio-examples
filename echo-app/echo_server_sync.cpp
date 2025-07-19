#include <iostream>
#include <string>
#include <boost/asio.hpp>

namespace ba = boost::asio;
namespace bai = boost::asio::ip;

void HandleClient(bai::tcp::socket& sock)
{
  try
  {
    while (true)
    {
      ba::streambuf buffer;
      ba::read_until(sock, buffer, '\n');

      std::string message = ba::buffer_cast<const char*>(buffer.data());
      std::cout << "Received: " << message << std::endl;

      ba::write(sock, ba::buffer(message));
      std::cout << "Send: " << message << std::endl;
    }
  }
  catch(const boost::system::system_error& e)
  {
    if (e.code() == ba::error::eof)
    {
      std::cout << "Client connection has been closed EOF" << std::endl;
    }
    else
    {
      std::cout << "Error: " << e.what() << std::endl;
    }
  }
}

int main()
{
  try
  {
    ba::io_context io_context;

    bai::tcp::acceptor acceptor(io_context, bai::tcp::endpoint(bai::tcp::v4(), 12345));
    std::cout << "Server is listening port 12345" << std::endl;

    while (true)
    {
      bai::tcp::socket socket(io_context);

      acceptor.accept(socket);
      std::cout << "New connection has been accepted: " << socket.remote_endpoint() << std::endl;

      HandleClient(socket);
      std::cout << "Client connection has been closed" << std::endl;
    }
  }
  catch (const std::exception& ex)
  {
    std::cout << "Server error: " << ex.what() << std::endl;
  }

  return 0;
}
