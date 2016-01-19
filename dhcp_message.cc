//
// Copyright (C) 2015 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "dhcp_client/dhcp_message.h"

#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>

#include <memory>
#include <set>
#include <string>
#include <utility>

#include <base/logging.h>

using shill::ByteString;

namespace dhcp_client {

namespace {
const int kClientHardwareAddressLength = 16;
const int kServerNameLength = 64;
const int kBootFileLength = 128;
const int kDHCPOptionLength = 312;
const uint32_t kMagicCookie = 0x63825363;
const size_t kDHCPMessageMaxLength = 548;
const size_t kDHCPMessageMinLength = 236;
const uint8_t kDHCPMessageBootRequest = 1;
const uint8_t kDHCPMessageBootReply = 2;

// Constants for DHCP options.
const uint8_t kDHCPOptionPad = 0;
const uint8_t kDHCPOptionDNSServer = 6;
const uint8_t kDHCPOptionLeaseTime = 51;
const uint8_t kDHCPOptionMessageType = 53;
const uint8_t kDHCPOptionServerIdentifier = 54;
const uint8_t kDHCPOptionRenewalTime = 58;
const uint8_t kDHCPOptionRebindingTime = 59;
const uint8_t kDHCPOptionEnd = 255;

// Follow the naming in rfc2131 for this struct.
struct __attribute__((__packed__)) RawDHCPMessage {
  uint8_t op;
  uint8_t htype;
  uint8_t hlen;
  uint8_t hops;
  uint32_t xid;
  uint16_t secs;
  uint16_t flags;
  uint32_t ciaddr;
  uint32_t yiaddr;
  uint32_t siaddr;
  uint32_t giaddr;
  uint8_t chaddr[kClientHardwareAddressLength];
  uint8_t sname[kServerNameLength];
  uint8_t file[kBootFileLength];
  uint32_t cookie;
  uint8_t options[kDHCPOptionLength];
};
}  // namespace

DHCPMessage::DHCPMessage()
    : lease_time_(0),
      renewal_time_(0),
      rebinding_time_(0) {
  options_map_.insert(std::make_pair(kDHCPOptionMessageType,
      ParserContext(new UInt8Parser(), &message_type_)));
  options_map_.insert(std::make_pair(kDHCPOptionLeaseTime,
      ParserContext(new UInt32Parser(), &lease_time_)));
  options_map_.insert(std::make_pair(kDHCPOptionServerIdentifier,
      ParserContext(new UInt32Parser(), &server_identifier_)));
  options_map_.insert(std::make_pair(kDHCPOptionRenewalTime,
      ParserContext(new UInt32Parser(), &renewal_time_)));
  options_map_.insert(std::make_pair(kDHCPOptionRebindingTime,
      ParserContext(new UInt32Parser(), &rebinding_time_)));
  options_map_.insert(std::make_pair(kDHCPOptionDNSServer,
      ParserContext(new UInt32ListParser(), &dns_server_)));
}

DHCPMessage::~DHCPMessage() {}

bool DHCPMessage::InitFromBuffer(const unsigned char* buffer,
                                 size_t length,
                                 DHCPMessage* message) {
  if (buffer == NULL) {
    LOG(ERROR) << "Invalid buffer address";
    return false;
  }
  if (length < kDHCPMessageMinLength || length > kDHCPMessageMaxLength) {
    LOG(ERROR) << "Invalid DHCP message length";
    return false;
  }
  const RawDHCPMessage* raw_message
      = reinterpret_cast<const RawDHCPMessage*>(buffer);
  size_t options_length = reinterpret_cast<const unsigned char*>(raw_message) +
      length - reinterpret_cast<const unsigned char*>(raw_message->options) + 1;
  message->opcode_ = raw_message->op;
  message->hardware_address_type_ = raw_message->htype;
  message->hardware_address_length_ = raw_message->hlen;
  if (message->hardware_address_length_ > kClientHardwareAddressLength) {
    LOG(ERROR) << "Invalid hardware address length";
  }
  message->relay_hops_ = raw_message->hops;
  message->transaction_id_ = ntohl(raw_message->xid);
  message->seconds_ = ntohs(raw_message->secs);
  message->flags_ = ntohs(raw_message->flags);
  message->client_ip_address_ = ntohl(raw_message->ciaddr);
  message->your_ip_address_ = ntohl(raw_message->yiaddr);
  message->next_server_ip_address_ = ntohl(raw_message->siaddr);
  message->agent_ip_address_ = ntohl(raw_message->giaddr);
  message->cookie_ = ntohl(raw_message->cookie);
  message->client_hardware_address_ = ByteString(
      reinterpret_cast<const char*>(raw_message->chaddr),
      message->hardware_address_length_);
  message->servername_.assign(reinterpret_cast<const char*>(raw_message->sname),
                              kServerNameLength);
  message->bootfile_.assign(reinterpret_cast<const char*>(raw_message->file),
                            kBootFileLength);
  // Validate the DHCP Message
  if (!message->IsValid()) {
    return false;
  }
  if (!message->ParseDHCPOptions(raw_message->options, options_length)) {
    LOG(ERROR) << "Failed to parse DHCP options";
    return false;
  }
  return true;
}

bool DHCPMessage::ParseDHCPOptions(const uint8_t* options,
                                   size_t options_length) {
  // DHCP options are in TLV format.
  // T: tag, L: length, V: value(data)
  // RFC 1497, RFC 1533, RFC 2132
  const uint8_t* ptr = options;
  const uint8_t* end_ptr = options + options_length;
  std::set<uint8_t> options_set;
  while (ptr < end_ptr) {
    uint8_t option_number = *ptr++;
    int option_number_int = static_cast<int>(option_number);
    if (option_number == kDHCPOptionPad) {
      continue;
    } else if (option_number == kDHCPOptionEnd) {
      // We reach the end of the option field.
      // A DHCP message must have option 53: DHCP Message Type.
      if (options_set.find(kDHCPOptionMessageType) == options_set.end()) {
        LOG(ERROR) << "Faied to find option 53: DHCP Message Type.";
        return false;
      }
      return true;
    }
    if (ptr >= end_ptr) {
      LOG(ERROR) << "Failed to decode dhcp options, no option length field"
                    "for option: " << option_number_int;
      return false;
    }
    uint8_t option_length = *ptr++;
    if (ptr + option_length >= end_ptr) {
      LOG(ERROR) << "Failed to decode dhcp options, invalid option length field"
                    "for option: " << option_number_int;
      return false;
    }
    if (options_set.find(option_number) != options_set.end()) {
      LOG(ERROR) << "Found repeated DHCP option: " << option_number_int;
      return false;
    }
    // Here we find a valid DHCP option.
    auto it = options_map_.find(option_number);
    if (it != options_map_.end()) {
      ParserContext* context = &(it->second);
      if (!context->parser->GetOption(ptr, option_length, context->output)) {
        return false;
      }
      options_set.insert(option_number);
    } else {
      DLOG(INFO) << "Ignore DHCP option: " << option_number_int;
    }
    // Move to next tag.
    ptr += option_length;
  }
  // Reach the end of message without seeing kDHCPOptionEnd.
  LOG(ERROR) << "Broken DHCP options without END tag.";
  return false;
}

bool DHCPMessage::IsValid() {
  if (opcode_ != kDHCPMessageBootReply) {
    LOG(ERROR) << "Invalid DHCP message op code";
    return false;
  }
  if (hardware_address_type_ != ARPHRD_ETHER) {
    LOG(ERROR) << "DHCP message device family id does not match";
    return false;
  }
  if (hardware_address_length_ != IFHWADDRLEN) {
    LOG(ERROR) <<
        "DHCP message device hardware address length does not match";
    return false;
  }
  // We have nothing to do with the 'hops' field.

  // The reply message from server should have the same xid we cached in client.
  // DHCP state machine will take charge of this checking.

  // According to RFC 2131, all secs field in reply messages should be 0.
  if (seconds_) {
    LOG(ERROR) << "Invalid DHCP message secs";
    return false;
  }

  // Check broadcast flags.
  // It should be 0 because we do not request broadcast reply.
  if (flags_) {
    LOG(ERROR) << "Invalid DHCP message flags";
    return false;
  }

  // We need to ensure the message contains the correct client hardware address.
  // DHCP state machine will take charge of this checking.

  // We do not use the bootfile field.
  if (cookie_ != kMagicCookie) {
    LOG(ERROR) << "DHCP message cookie does not match";
    return false;
  }
  return true;
}

uint16_t DHCPMessage::ComputeChecksum(const uint8_t* data, size_t len) {
  uint32_t sum = 0;

  while (len > 1) {
    sum += static_cast<uint32_t>(data[0]) << 8 | static_cast<uint32_t>(data[1]);
    data += 2;
    len -= 2;
  }
  if (len == 1) {
    sum += static_cast<uint32_t>(*data) << 8;
  }
  sum = (sum >> 16) + (sum & 0xffff);
  sum += (sum >> 16);

  return ~static_cast<uint16_t>(sum);
}


}  // namespace dhcp_client