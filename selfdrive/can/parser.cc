#include <cstdio>
#include <cstdint>
#include <cassert>
#include <cstring>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>

#include <zmq.h>

#include <capnp/serialize.h>
#include "cereal/gen/cpp/log.capnp.h"

#include "common.h"

#define DEBUG(...)
// #define DEBUG printf
#define INFO printf


#define MAX_BAD_COUNTER 5

// Lookup table for fast computation of CRC8 poly 0x2F, aka 8H2F "AUTOSAR"
uint8_t crc8_lut_8h2f[] = {
  0x00, 0x2F, 0x5E, 0x71, 0xBC, 0x93, 0xE2, 0xCD, 0x57, 0x78, 0x09, 0x26, 0xEB, 0xC4, 0xB5, 0x9A,
  0xAE, 0x81, 0xF0, 0xDF, 0x12, 0x3D, 0x4C, 0x63, 0xF9, 0xD6, 0xA7, 0x88, 0x45, 0x6A, 0x1B, 0x34,
  0x73, 0x5C, 0x2D, 0x02, 0xCF, 0xE0, 0x91, 0xBE, 0x24, 0x0B, 0x7A, 0x55, 0x98, 0xB7, 0xC6, 0xE9,
  0xDD, 0xF2, 0x83, 0xAC, 0x61, 0x4E, 0x3F, 0x10, 0x8A, 0xA5, 0xD4, 0xFB, 0x36, 0x19, 0x68, 0x47,
  0xE6, 0xC9, 0xB8, 0x97, 0x5A, 0x75, 0x04, 0x2B, 0xB1, 0x9E, 0xEF, 0xC0, 0x0D, 0x22, 0x53, 0x7C,
  0x48, 0x67, 0x16, 0x39, 0xF4, 0xDB, 0xAA, 0x85, 0x1F, 0x30, 0x41, 0x6E, 0xA3, 0x8C, 0xFD, 0xD2,
  0x95, 0xBA, 0xCB, 0xE4, 0x29, 0x06, 0x77, 0x58, 0xC2, 0xED, 0x9C, 0xB3, 0x7E, 0x51, 0x20, 0x0F,
  0x3B, 0x14, 0x65, 0x4A, 0x87, 0xA8, 0xD9, 0xF6, 0x6C, 0x43, 0x32, 0x1D, 0xD0, 0xFF, 0x8E, 0xA1,
  0xE3, 0xCC, 0xBD, 0x92, 0x5F, 0x70, 0x01, 0x2E, 0xB4, 0x9B, 0xEA, 0xC5, 0x08, 0x27, 0x56, 0x79,
  0x4D, 0x62, 0x13, 0x3C, 0xF1, 0xDE, 0xAF, 0x80, 0x1A, 0x35, 0x44, 0x6B, 0xA6, 0x89, 0xF8, 0xD7,
  0x90, 0xBF, 0xCE, 0xE1, 0x2C, 0x03, 0x72, 0x5D, 0xC7, 0xE8, 0x99, 0xB6, 0x7B, 0x54, 0x25, 0x0A,
  0x3E, 0x11, 0x60, 0x4F, 0x82, 0xAD, 0xDC, 0xF3, 0x69, 0x46, 0x37, 0x18, 0xD5, 0xFA, 0x8B, 0xA4,
  0x05, 0x2A, 0x5B, 0x74, 0xB9, 0x96, 0xE7, 0xC8, 0x52, 0x7D, 0x0C, 0x23, 0xEE, 0xC1, 0xB0, 0x9F,
  0xAB, 0x84, 0xF5, 0xDA, 0x17, 0x38, 0x49, 0x66, 0xFC, 0xD3, 0xA2, 0x8D, 0x40, 0x6F, 0x1E, 0x31,
  0x76, 0x59, 0x28, 0x07, 0xCA, 0xE5, 0x94, 0xBB, 0x21, 0x0E, 0x7F, 0x50, 0x9D, 0xB2, 0xC3, 0xEC,
  0xD8, 0xF7, 0x86, 0xA9, 0x64, 0x4B, 0x3A, 0x15, 0x8F, 0xA0, 0xD1, 0xFE, 0x33, 0x1C, 0x6D, 0x42 };

unsigned int honda_checksum(unsigned int address, uint64_t d, int l) {
  d >>= ((8-l)*8); // remove padding
  d >>= 4; // remove checksum

  int s = 0;
  while (address) { s += (address & 0xF); address >>= 4; }
  while (d) { s += (d & 0xF); d >>= 4; }
  s = 8-s;
  s &= 0xF;

  return s;
}

unsigned int toyota_checksum(unsigned int address, uint64_t d, int l) {
  d >>= ((8-l)*8); // remove padding
  d >>= 8; // remove checksum

  unsigned int s = l;
  while (address) { s += address & 0xff; address >>= 8; }
  while (d) { s += d & 0xff; d >>= 8; }

  return s & 0xFF;
}

unsigned int volkswagen_checksum(unsigned int address, uint64_t d, int length, int counter)
{
  // The Volkswagen CRC algorithm is standard CRC8 8H2F/AUTOSAR run over the
  // payload content plus a single extra magic byte value.
  // https://www.autosar.org/fileadmin/user_upload/standards/classic/4-3/AUTOSAR_SWS_CRCLibrary.pdf

  // Initial CRC value for CRC8 8H2F/AUTOSAR
  uint8_t crc = 0xFF;

  // The algorithm is first run over the 3 to 7 byte payload, skipping over the
  // first byte which is reserved for the CRC itself.
  uint8_t *dat = (uint8_t *)&d;
  for (int i = 1; i < length; i++) {
    crc ^= dat[i];
    crc = crc8_lut_8h2f[crc];
  }

  // Look up the magic value(s) for the CRC padding byte, which permute by CAN
  // address, and (for SOME addresses) by the 4-bit message counter.
  uint8_t magic_lut[];
  switch(address) {
    case 0x86:  // LWI_01 Steering angle
      magic_lut[] = { 0x86, 0x86, 0x86, 0x86, 0x86, 0x86, 0x86, 0x86, 0x86, 0x86, 0x86, 0x86, 0x86, 0x86, 0x86, 0x86 };
      break;
    case 0x9f:  // EPS_05 Electronic power steering
      magic_lut[] = { 0xF5, 0xF5, 0xF5, 0xF5, 0xF5, 0xF5, 0xF5, 0xF5, 0xF5, 0xF5, 0xF5, 0xF5, 0xF5, 0xF5, 0xF5, 0xF5 };
      break;
    case 0xad:  // Getriebe_11 Transmission data
      magic_lut[] = { 0x3F, 0x69, 0x39, 0xDC, 0x94, 0xF9, 0x14, 0x64, 0xD8, 0x6A, 0x34, 0xCE, 0xA2, 0x55, 0xB5, 0x2C };
      break;
    case 0x106: // ESP_05 Electronic stability program data
      magic_lut[] = { 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07 };
      break;
    case 0x126: // HCA_01 Heading control assist
      magic_lut[] = { 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 };
      break;
    case 0x12b: // GRA_ACC_01 Cruise control buttons
      magic_lut[] = { 0x6A, 0x38, 0xB4, 0x27, 0x22, 0xEF, 0xE1, 0xBB, 0xF8, 0x80, 0x84, 0x49, 0xC7, 0x9E, 0x1E, 0x2B };
      break;
    case 0x3c0: // Klemmen_Status_01 Ignition on/off states
      magic_lut[] = { 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3 };
      break;
    case 0x65d: // ESP_20 Electronic stability program data
      magic_lut[] = { 0xAC, 0xB3, 0xAB, 0xEB, 0x7A, 0xE1, 0x3B, 0xF7, 0x73, 0xBA, 0x7C, 0x9E, 0x06, 0x5F, 0x02, 0xD9 };
      break;
    default:
      INFO("Attempt to CRC check unknown Volkswagen message 0x%02X\n", address);
      magic_lut[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
      break;
  }
  crc ^= magic_lut[counter];
  crc = crc8_lut_8h2f[crc];

  // Return after final XOR for CRC8 8H2F/AUTOSAR
  return crc ^ 0xFF;
}

unsigned int pedal_checksum(unsigned int address, uint64_t d, int l) {
  uint8_t crc = 0xFF;
  uint8_t poly = 0xD5; // standard crc8

  d >>= ((8-l)*8); // remove padding
  d >>= 8; // remove checksum

  uint8_t *dat = (uint8_t *)&d;

  int i, j;
  for (i = 0; i < l - 1; i++) {
    crc ^= dat[i];
    for (j = 0; j < 8; j++) {
      if ((crc & 0x80) != 0) {
        crc = (uint8_t)((crc << 1) ^ poly);
      }
      else {
        crc <<= 1;
      }
    }
  }
  return crc;
}

namespace {

uint64_t read_u64_be(const uint8_t* v) {
  return (((uint64_t)v[0] << 56)
          | ((uint64_t)v[1] << 48)
          | ((uint64_t)v[2] << 40)
          | ((uint64_t)v[3] << 32)
          | ((uint64_t)v[4] << 24)
          | ((uint64_t)v[5] << 16)
          | ((uint64_t)v[6] << 8)
          | (uint64_t)v[7]);
}

uint64_t read_u64_le(const uint8_t* v) {
  return ((uint64_t)v[0]
          | ((uint64_t)v[1] << 8)
          | ((uint64_t)v[2] << 16)
          | ((uint64_t)v[3] << 24)
          | ((uint64_t)v[4] << 32)
          | ((uint64_t)v[5] << 40)
          | ((uint64_t)v[6] << 48)
          | ((uint64_t)v[7] << 56));
}


struct MessageState {
  uint32_t address;
  unsigned int size;

  std::vector<Signal> parse_sigs;
  std::vector<double> vals;

  uint16_t ts;
  uint64_t seen;
  uint64_t check_threshold;

  uint8_t counter;
  uint8_t counter_fail;

  bool parse(uint64_t sec, uint16_t ts_, uint64_t dat) {
    for (int i=0; i < parse_sigs.size(); i++) {
      auto& sig = parse_sigs[i];
      int64_t tmp;

      if (sig.is_little_endian){
        tmp = (dat >> sig.b1) & ((1ULL << sig.b2)-1);
      } else {
        tmp = (dat >> sig.bo) & ((1ULL << sig.b2)-1);
      }

      if (sig.is_signed) {
        tmp -= (tmp >> (sig.b2-1)) ? (1ULL << sig.b2) : 0; //signed
      }

      DEBUG("parse %X %s -> %lld\n", address, sig.name, tmp);

      if (sig.type == SignalType::HONDA_CHECKSUM) {
        if (honda_checksum(address, dat, size) != tmp) {
          INFO("%X CHECKSUM FAIL\n", address);
          return false;
        }
      } else if (sig.type == SignalType::HONDA_COUNTER) {
        if (!update_counter_generic(tmp, sig.b2)) {
          return false;
        }
      } else if (sig.type == SignalType::TOYOTA_CHECKSUM) {
        if (toyota_checksum(address, dat, size) != tmp) {
          INFO("%X CHECKSUM FAIL\n", address);
          return false;
        }
      } else if (sig.type == SignalType::VOLKSWAGEN_CHECKSUM) {
        if (volkswagen_checksum(address, dat, size) != tmp) {
          INFO("%X CHECKSUM FAIL\n", address);
          return false;
        }
      } else if (sig.type == SignalType::VOLKSWAGEN_COUNTER) {
        if (!update_counter_generic(tmp, sig.b2)) {
          return false;
        }
      } else if (sig.type == SignalType::PEDAL_CHECKSUM) {
        if (pedal_checksum(address, dat, size) != tmp) {
          INFO("%X PEDAL CHECKSUM FAIL\n", address);
          return false;
        }
      } else if (sig.type == SignalType::PEDAL_COUNTER) {
        if (!update_counter_generic(tmp, sig.b2)) {
          return false;
        }
      }

      vals[i] = tmp * sig.factor + sig.offset;
    }
    ts = ts_;
    seen = sec;

    return true;
  }


  bool update_counter_generic(int64_t v, int cnt_size) {
    uint8_t old_counter = counter;
    counter = v;
    if (((old_counter+1) & ((1 << cnt_size) -1)) != v) {
      counter_fail += 1;
      if (counter_fail > 1) {
        INFO("%X COUNTER FAIL %d -- %d vs %d\n", address, counter_fail, old_counter, (int)v);
      }
      if (counter_fail >= MAX_BAD_COUNTER) {
        return false;
      }
    } else if (counter_fail > 0) {
      counter_fail--;
    }
    return true;
  }

};


class CANParser {
 public:
  CANParser(int abus, const std::string& dbc_name,
            const std::vector<MessageParseOptions> &options,
            const std::vector<SignalParseOptions> &sigoptions,
            bool sendcan, const std::string& tcp_addr, int timeout=-1)
    : bus(abus) {
    // connect to can on 8006
    context = zmq_ctx_new();

    if (tcp_addr.length() > 0) {
      subscriber = zmq_socket(context, ZMQ_SUB);
      zmq_setsockopt(subscriber, ZMQ_SUBSCRIBE, "", 0);
      zmq_setsockopt(subscriber, ZMQ_RCVTIMEO, &timeout, sizeof(int));

      std::string tcp_addr_str;

      if (sendcan) {
        tcp_addr_str = "tcp://" + tcp_addr + ":8017";
      } else {
        tcp_addr_str = "tcp://" + tcp_addr + ":8006";
      }
      const char *tcp_addr_char = tcp_addr_str.c_str();

      zmq_connect(subscriber, tcp_addr_char);

      // drain sendcan to delete any stale messages from previous runs
      zmq_msg_t msgDrain;
      zmq_msg_init(&msgDrain);
      int err = 0;
      while(err >= 0) {
        err = zmq_msg_recv(&msgDrain, subscriber, ZMQ_DONTWAIT);
      }
    } else {
      subscriber = NULL;
    }

    dbc = dbc_lookup(dbc_name);
    assert(dbc); 
    for (const auto& op : options) {
      MessageState state = {
        .address = op.address,
        // .check_frequency = op.check_frequency,
      };

      // msg is not valid if a message isn't received for 10 consecutive steps
      if (op.check_frequency > 0) {
        state.check_threshold = (1000000000ULL / op.check_frequency) * 10;
      }


      const Msg* msg = NULL;
      for (int i=0; i<dbc->num_msgs; i++) {
        if (dbc->msgs[i].address == op.address) {
          msg = &dbc->msgs[i];
          break;
        }
      }
      if (!msg) {
        fprintf(stderr, "CANParser: could not find message 0x%X in dnc %s\n", op.address, dbc_name.c_str());
        assert(false);
      }

      state.size = msg->size;

      // track checksums and counters for this message
      for (int i=0; i<msg->num_sigs; i++) {
        const Signal *sig = &msg->sigs[i];
        if (sig->type != SignalType::DEFAULT) {
          state.parse_sigs.push_back(*sig);
          state.vals.push_back(0);
        }
      }

      // track requested signals for this message
      for (const auto& sigop : sigoptions) {
        if (sigop.address != op.address) continue;

        for (int i=0; i<msg->num_sigs; i++) {
          const Signal *sig = &msg->sigs[i];
          if (strcmp(sig->name, sigop.name) == 0
              && sig->type == SignalType::DEFAULT) {
            state.parse_sigs.push_back(*sig);
            state.vals.push_back(sigop.default_value);
            break;
          }
        }

      }

      message_states[state.address] = state;
    }
  }

  void UpdateCans(uint64_t sec, const capnp::List<cereal::CanData>::Reader& cans) {
      int msg_count = cans.size();
      uint64_t p;

      DEBUG("got %d messages\n", msg_count);

      // parse the messages
      for (int i = 0; i < msg_count; i++) {
        auto cmsg = cans[i];
        if (cmsg.getSrc() != bus) {
          // DEBUG("skip %d: wrong bus\n", cmsg.getAddress());
          continue;
        }
        auto state_it = message_states.find(cmsg.getAddress());
        if (state_it == message_states.end()) {
          // DEBUG("skip %d: not specified\n", cmsg.getAddress());
          continue;
        }

        if (cmsg.getDat().size() > 8) continue; //shouldnt ever happen
        uint8_t dat[8] = {0};
        memcpy(dat, cmsg.getDat().begin(), cmsg.getDat().size());

        // Assumes all signals in the message are of the same type (little or big endian)
        // TODO: allow signals within the same message to have different endianess
        auto& sig = message_states[cmsg.getAddress()].parse_sigs[0];
        if (sig.is_little_endian) {
            p = read_u64_le(dat);
        } else {
            p = read_u64_be(dat);
        }

        DEBUG("  proc %X: %llx\n", cmsg.getAddress(), p);

        state_it->second.parse(sec, cmsg.getBusTime(), p);
      }
  }

  void UpdateValid(uint64_t sec) {
    can_valid = true;
    for (const auto& kv : message_states) {
      const auto& state = kv.second;
      if (state.check_threshold > 0 && (sec - state.seen) > state.check_threshold) {
        if (state.seen > 0) {
          DEBUG("%X TIMEOUT\n", state.address);
        }
        can_valid = false;
      }
    }
  }

  void update_string(uint64_t sec, std::string data) {
    // format for board, make copy due to alignment issues, will be freed on out of scope
    auto amsg = kj::heapArray<capnp::word>((data.length() / sizeof(capnp::word)) + 1);
    memcpy(amsg.begin(), data.data(), data.length());

    // extract the messages
    capnp::FlatArrayMessageReader cmsg(amsg);
    cereal::Event::Reader event = cmsg.getRoot<cereal::Event>();

    auto cans = event.getCan();
    UpdateCans(sec, cans);

    UpdateValid(sec);
  }

  int update(uint64_t sec, bool wait) {
    int err;
    int result = 0;

    // recv from can
    zmq_msg_t msg;
    zmq_msg_init(&msg);

    // multiple recv is fine
    bool first = wait;
    while (subscriber != NULL) {
      if (first) {
        err = zmq_msg_recv(&msg, subscriber, 0);
        first = false;

        // When we timeout on the first message, return error
        if (err < 0){
          result = -1;
        }
      } else {
        err = zmq_msg_recv(&msg, subscriber, ZMQ_DONTWAIT);
      }
      if (err < 0) break;

      // format for board, make copy due to alignment issues, will be freed on out of scope
      auto amsg = kj::heapArray<capnp::word>((zmq_msg_size(&msg) / sizeof(capnp::word)) + 1);
      memcpy(amsg.begin(), zmq_msg_data(&msg), zmq_msg_size(&msg));

      // extract the messages
      capnp::FlatArrayMessageReader cmsg(amsg);
      cereal::Event::Reader event = cmsg.getRoot<cereal::Event>();

      auto cans = event.getCan();
      UpdateCans(sec, cans);
    }

    UpdateValid(sec);
    zmq_msg_close(&msg);
    return result;
  }

  std::vector<SignalValue> query(uint64_t sec) {
    std::vector<SignalValue> ret;

    for (const auto& kv : message_states) {
      const auto& state = kv.second;
      if (sec != 0 && state.seen != sec) continue;

      for (int i=0; i<state.parse_sigs.size(); i++) {
        const Signal &sig = state.parse_sigs[i];
        ret.push_back((SignalValue){
          .address = state.address,
          .ts = state.ts,
          .name = sig.name,
          .value = state.vals[i],
        });
      }
    }

    return ret;
  }

  bool can_valid = false;

 private:
  const int bus;
  // zmq vars
  void *context = NULL;
  void *subscriber = NULL;

  const DBC *dbc = NULL;
  std::unordered_map<uint32_t, MessageState> message_states;
};

}

extern "C" {

void* can_init(int bus, const char* dbc_name,
               size_t num_message_options, const MessageParseOptions* message_options,
               size_t num_signal_options, const SignalParseOptions* signal_options,
               bool sendcan, const char* tcp_addr, int timeout) {
  CANParser* ret = new CANParser(bus, std::string(dbc_name),
                                 (message_options ? std::vector<MessageParseOptions>(message_options, message_options+num_message_options)
                                  : std::vector<MessageParseOptions>{}),
                                 (signal_options ? std::vector<SignalParseOptions>(signal_options, signal_options+num_signal_options)
                                  : std::vector<SignalParseOptions>{}), sendcan, std::string(tcp_addr), timeout);
  return (void*)ret;
}

void* can_init_with_vectors(int bus, const char* dbc_name,
               std::vector<MessageParseOptions> message_options,
               std::vector<SignalParseOptions> signal_options,
               bool sendcan, const char* tcp_addr, int timeout) {
  CANParser* ret = new CANParser(bus, std::string(dbc_name),
                                 message_options,
                                 signal_options,
                                 sendcan, std::string(tcp_addr), timeout);
  return (void*)ret;
}

int can_update(void* can, uint64_t sec, bool wait) {
  CANParser* cp = (CANParser*)can;
  return cp->update(sec, wait);
}

void can_update_string(void *can, uint64_t sec, const char* dat, int len) {
  CANParser* cp = (CANParser*)can;
  cp->update_string(sec, std::string(dat, len));
}

size_t can_query(void* can, uint64_t sec, bool *out_can_valid, size_t out_values_size, SignalValue* out_values) {
  CANParser* cp = (CANParser*)can;

  if (out_can_valid) {
    *out_can_valid = cp->can_valid;
  }

  const std::vector<SignalValue> values = cp->query(sec);
  if (out_values) {
    std::copy(values.begin(), values.begin()+std::min(out_values_size, values.size()), out_values);
  }
  return values.size();
};

void can_query_vector(void* can, uint64_t sec, bool *out_can_valid, std::vector<SignalValue> &values) {
  CANParser* cp = (CANParser*)can;
  if (out_can_valid) {
    *out_can_valid = cp->can_valid;
  }
  values = cp->query(sec);
};

}

#ifdef TEST

int main(int argc, char** argv) {
  CANParser cp(0, "honda_civic_touring_2016_can",
    std::vector<MessageParseOptions>{
      // address, check_frequency
      {0x14a, 100},
      {0x158, 100},
      {0x17c, 100},
      {0x191, 100},
      {0x1a4, 50},
      {0x326, 10},
      {0x1b0, 50},
      {0x1d0, 50},
      {0x305, 10},
      {0x324, 10},
      {0x405, 3},
      {0x18f, 0},
      {0x130, 0},
      {0x296, 0},
      {0x30c, 0},
    },
    std::vector<SignalParseOptions>{
      //  sig_name, sig_address, default
      {0x158, "XMISSION_SPEED", 0},
      {0x1d0, "WHEEL_SPEED_FL", 0},
      {0x1d0, "WHEEL_SPEED_FR", 0},
      {0x1d0, "WHEEL_SPEED_RL", 0},
      {0x14a, "STEER_ANGLE", 0},
      {0x18f, "STEER_TORQUE_SENSOR", 0},
      {0x191, "GEAR", 0},
      {0x1b0, "WHEELS_MOVING", 1},
      {0x405, "DOOR_OPEN_FL", 1},
      {0x405, "DOOR_OPEN_FR", 1},
      {0x405, "DOOR_OPEN_RL", 1},
      {0x405, "DOOR_OPEN_RR", 1},
      {0x324, "CRUISE_SPEED_PCM", 0},
      {0x305, "SEATBELT_DRIVER_LAMP", 1},
      {0x305, "SEATBELT_DRIVER_LATCHED", 0},
      {0x17c, "BRAKE_PRESSED", 0},
      {0x130, "CAR_GAS", 0},
      {0x296, "CRUISE_BUTTONS", 0},
      {0x1a4, "ESP_DISABLED", 1},
      {0x30c, "HUD_LEAD", 0},
      {0x1a4, "USER_BRAKE", 0},
      {0x18f, "STEER_STATUS", 5},
      {0x1d0, "WHEEL_SPEED_RR", 0},
      {0x1b0, "BRAKE_ERROR_1", 1},
      {0x1b0, "BRAKE_ERROR_2", 1},
      {0x191, "GEAR_SHIFTER", 0},
      {0x326, "MAIN_ON", 0},
      {0x17c, "ACC_STATUS", 0},
      {0x17c, "PEDAL_GAS", 0},
      {0x296, "CRUISE_SETTING", 0},
      {0x326, "LEFT_BLINKER", 0},
      {0x326, "RIGHT_BLINKER", 0},
      {0x324, "COUNTER", 0},
      {0x17c, "ENGINE_RPM", 0},
    });



  const std::string log_fn = "dats.bin";

  int log_fd = open(log_fn.c_str(), O_RDONLY, 0);
  assert(log_fd >= 0);

  off_t log_size = lseek(log_fd, 0, SEEK_END);
  lseek(log_fd, 0, SEEK_SET);

  void* log_data = mmap(NULL, log_size, PROT_READ, MAP_PRIVATE, log_fd, 0);
  assert(log_data);

  auto words = kj::arrayPtr((const capnp::word*)log_data, log_size/sizeof(capnp::word));
  while (words.size() > 0) {
    capnp::FlatArrayMessageReader reader(words);

    auto evt = reader.getRoot<cereal::Event>();
    auto cans = evt.getCan();

    cp.UpdateCans(0, cans);

    words = kj::arrayPtr(reader.getEnd(), words.end());
  }

  munmap(log_data, log_size);

  close(log_fd);

  return 0;
}

#endif
