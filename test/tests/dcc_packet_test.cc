#include "utils/test_main.hxx"

#include "dcc/Packet.hxx"

using ::testing::ElementsAre;
using ::testing::_;

namespace dcc {

class PacketTest : public ::testing::Test {
protected:
    vector<uint8_t> get_packet() {
        return vector<uint8_t>(pkt_.payload + 0, pkt_.payload + pkt_.dlc);
    }

    vector<uint8_t> packet(const std::initializer_list<int>& data) {
        vector<uint8_t> v;
        for (int b : data) {
            v.push_back(b & 0xff);
        }
        return v;
    }

    Packet pkt_;
};

TEST_F(PacketTest, ChecksumRegular) {
    pkt_.dlc = 3;
    pkt_.payload[0] = 0xA0;
    pkt_.payload[1] = 0x53;
    pkt_.payload[2] = 0x0F;
    pkt_.AddDccChecksum();
    EXPECT_EQ(4, pkt_.dlc);
    EXPECT_EQ(0xFC, pkt_.payload[3]);
}

TEST_F(PacketTest, ChecksumEmpty) {
    pkt_.dlc = 0;
    pkt_.AddDccChecksum();
    EXPECT_EQ(1, pkt_.dlc);
    EXPECT_EQ(0, pkt_.payload[3]);
}

TEST_F(PacketTest, ChecksumFF) {
    pkt_.dlc = 3;
    pkt_.payload[0] = 0xFF;
    pkt_.payload[1] = 0xFF;
    pkt_.payload[2] = 0xFF;
    pkt_.AddDccChecksum();
    EXPECT_EQ(4, pkt_.dlc);
    EXPECT_EQ(0xFF, pkt_.payload[3]);
}


TEST_F(PacketTest, DccSpeed28) {
    pkt_.SetDccSpeed28(DccShortAddress(55), true, 6);
    EXPECT_THAT(get_packet(), ElementsAre(0b00110111, 0b01110100, 0b01000011));
}

TEST_F(PacketTest, DccSpeed28_zero) {
    pkt_.SetDccSpeed28(DccShortAddress(55), true, 0);
    EXPECT_THAT(get_packet(), ElementsAre(55, 0b01100000, _));
}

TEST_F(PacketTest, DccSpeed28_reversezero) {
    pkt_.SetDccSpeed28(DccShortAddress(55), false, 0);
    EXPECT_THAT(get_packet(), ElementsAre(55, 0b01000000, _));
}

TEST_F(PacketTest, DccSpeed28_estop) {
    pkt_.SetDccSpeed28(DccShortAddress(55), true, Packet::EMERGENCY_STOP);
    EXPECT_THAT(get_packet(), ElementsAre(55, 0b01100001, _));
}

TEST_F(PacketTest, DccSpeed28_step123) {
    pkt_.SetDccSpeed28(DccShortAddress(55), true, 1);
    EXPECT_THAT(get_packet(), ElementsAre(55, 0b01100010, _));

    pkt_.SetDccSpeed28(DccShortAddress(55), true, 2);
    EXPECT_THAT(get_packet(), ElementsAre(55, 0b01110010, _));

    pkt_.SetDccSpeed28(DccShortAddress(55), true, 3);
    EXPECT_THAT(get_packet(), ElementsAre(55, 0b01100011, _));
}

TEST_F(PacketTest, DccIdle) {
    pkt_.SetDccIdle();
    EXPECT_THAT(get_packet(), ElementsAre(0xff, 0, 0xff));
}

TEST_F(PacketTest, DccReset) {
    pkt_.SetDccResetAllDecoders();
    EXPECT_THAT(get_packet(), ElementsAre(0, 0, 0));
}


}  // namespace dcc
