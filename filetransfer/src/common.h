#ifndef FILETRANSFER_COMMON_H_
#define FILETRANSFER_COMMON_H_

#include <iostream>
#include <string>
#include <vector>
#include <boost/asio.hpp>
#include <arpa/inet.h> // For htonl/ntohl
#include <memory>      // For std::shared_ptr, std::unique_ptr
#include <functional>  // For std::function
#include <zlib.h>      // For crc32
#include "filetransfer.pb.h"

namespace ba = boost::asio;
namespace bai = boost::asio::ip;

const short PORT = 12345;
const size_t CHUNK_SIZE = 4 * 1024 * 1024; // 4 MB chunk size

// Protocol Header
struct ProtocolHeader
{
  uint32_t mMagicBytes;
  uint8_t mVersion;
  uint32_t mPayloadSize;
  uint32_t mChecksum;

  void ToNetworkByteOrder()
  {
    mMagicBytes = htonl(mMagicBytes);
    mPayloadSize = htonl(mPayloadSize);
    mChecksum = htonl(mChecksum);
  }

  void ToHostByteOrder()
  {
    mMagicBytes = ntohl(mMagicBytes);
    mPayloadSize = ntohl(mPayloadSize);
    mChecksum = ntohl(mChecksum);
  }
};

const uint32_t PROTOCOL_MAGIC_BYTES = 0xDEADBEEF;
const uint8_t PROTOCOL_VERSION = 0x01;

// Serialize a protobuf message
template <typename T>
void AsyncWriteProtobufMessage(bai::tcp::socket &socket, const T &message,
                               std::function<void(const boost::system::error_code &, size_t)> handler)
{
  std::string serializedData;
  if (!message.SerializeToString(&serializedData))
  {
    boost::system::error_code ec(boost::system::errc::make_error_code(boost::system::errc::no_message));
    ba::post(socket.get_executor(), [handler, ec]()
                                { handler(ec, 0); });
    return;
  }

  ProtocolHeader header;
  header.mMagicBytes = PROTOCOL_MAGIC_BYTES;
  header.mVersion = PROTOCOL_VERSION;
  header.mPayloadSize = static_cast<uint32_t>(serializedData.length());
  // Calculate checksum
  header.mChecksum = crc32(0L, reinterpret_cast<const Bytef *>(serializedData.data()), serializedData.length());

  header.ToNetworkByteOrder();

  std::vector<ba::const_buffer> buffers;
  buffers.push_back(ba::buffer(&header, sizeof(ProtocolHeader)));
  buffers.push_back(ba::buffer(serializedData));

  ba::async_write(socket, buffers, handler);
}

// Read a Protobuf message from socket
template <typename T>
void AsyncReadProtobufMessage(bai::tcp::socket &socket, ba::streambuf &buffer,
                              std::function<void(const boost::system::error_code &, size_t, std::unique_ptr<T>)> handler)
{
  auto pHeader = std::make_shared<ProtocolHeader>();
  auto pSelf = std::make_shared<bool>(true);

  // Read Header first
  ba::async_read(socket, ba::buffer(pHeader.get(), sizeof(ProtocolHeader)),
                 [&socket, &buffer, handler, pHeader, pSelf](const boost::system::error_code &error, size_t bytes_transferred)
                 {
                   if (!error)
                   {
                     pHeader->ToHostByteOrder();

                     // Magic bytes check
                     if (pHeader->mMagicBytes != PROTOCOL_MAGIC_BYTES)
                     {
                       std::cerr << "Error: Invalid magic bytes. Expected: 0x"
                                 << std::hex << PROTOCOL_MAGIC_BYTES << ", Received: 0x"
                                 << std::hex << pHeader->mMagicBytes << std::endl;
                       handler(boost::asio::error::invalid_argument, 0, nullptr);
                       return;
                     }
                     // Version check
                     if (pHeader->mVersion != PROTOCOL_VERSION)
                     {
                       std::cerr << "Error Protocol Version Expected: "
                                 << (int)PROTOCOL_VERSION << ", Received: " << (int)pHeader->mVersion << std::endl;
                       handler(boost::system::errc::make_error_code(boost::system::errc::errc_t::protocol_error), 0, nullptr);
                       return;
                     }

                     // Read payload
                     buffer.prepare(pHeader->mPayloadSize);
                     ba::async_read(socket, buffer.prepare(pHeader->mPayloadSize),
                                    [&socket, &buffer, handler, pHeader, pSelf](const boost::system::error_code &error2, size_t bytes_transferred2)
                                    {
                                      if (!error2)
                                      {
                                        buffer.commit(bytes_transferred2);

                                        // Payload checksum check
                                        uint32_t calculated_checksum = crc32(0L,
                                                                             reinterpret_cast<const Bytef *>(boost::asio::buffer_cast<const char *>(buffer.data())),
                                                                             pHeader->mPayloadSize);

                                        if (calculated_checksum != pHeader->mChecksum)
                                        {
                                          std::cerr << "Error: Checksum not valid! Expected: 0x" << std::hex << pHeader->mChecksum
                                                    << ", Calculated: 0x" << std::hex << calculated_checksum << std::endl;
                                          handler(boost::asio::error::fault, 0, nullptr);
                                          buffer.consume(bytes_transferred2);
                                          return;
                                        }

                                        auto message_ptr = std::make_unique<T>();
                                        if (message_ptr->ParseFromArray(boost::asio::buffer_cast<const char *>(buffer.data()), pHeader->mPayloadSize))
                                        {
                                          handler(boost::system::error_code(), bytes_transferred2, std::move(message_ptr));
                                        }
                                        else
                                        {
                                          handler(boost::asio::error::invalid_argument, 0, nullptr);
                                        }
                                        buffer.consume(bytes_transferred2);
                                      }
                                      else
                                      {
                                        handler(error2, 0, nullptr);
                                      }
                                    });
                   }
                   else
                   {
                     handler(error, 0, nullptr);
                   }
                 });
}

#endif // FILETRANSFER_COMMON_H_