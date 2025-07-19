#include <iostream>
#include <string>
#include <boost/asio.hpp>

namespace ba = boost::asio;
namespace bai = boost::asio::ip;

int main()
{
  try
  {
    ba::io_context io_context;
    bai::tcp::socket socket(io_context);

    bai::tcp::endpoint endpoint(bai::address::from_string("127.0.0.1"), 12345);

    socket.connect(endpoint);
    std::cout << "Connected to the server" << std::endl;

    int i = 1;
    while (true)
    {
      std::string message = "Message [" + std::to_string(i++) + "]\n";
      
      ba::write(socket, ba::buffer(message));
      std::cout << "Send: " << message << std::endl;

      ba::streambuf buffer;
      ba::read_until(socket, buffer, '\n');

      std::string received = ba::buffer_cast<const char*>(buffer.data());
      std::cout << "Received: " << received << std::endl;
    }
  }
  catch (const boost::system::system_error& e)
  {
    std::cout << "Error: " << e.what() << std::endl;
  }

  return 0;
}
