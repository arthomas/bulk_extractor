#include "config.h"

#include <set>
#include <mutex>
#include <ctype.h>

#include "be13_api/formatter.h"
#include "be13_api/utils.h"

#include "pcap_writer.h"



/****************************************************************
 ** pcap_writer code
 **/

pcap_writer::pcap_writer(const scanner_params &sp):
    outpath(sp.sc.outdir / OUTPUT_FILENAME)
{
    ip_recorder    = &sp.named_feature_recorder("ip");
    tcp_recorder   = &sp.named_feature_recorder("tcp");
    ether_recorder = &sp.named_feature_recorder("ether");
}

pcap_writer::~pcap_writer()
{
    if (fcap){
        const std::lock_guard<std::mutex> lock(Mfcap);
        fclose(fcap);
        fcap = nullptr;
    }
}

/*
 * @param add_frame - should we add a frame?
 * @param frame_type - the ethernet frame type. Note that this could be combined with add_frame, with frame_type=0 for no add.
 */
void pcap_writer::pcap_writepkt(const struct pcap_hdr &h, // packet header
                                const sbuf_t &sbuf,       // sbuf where packet is located
                                const size_t pos,         // position within the sbuf
                                const bool add_frame,     // whether or not to create a synthetic ethernet frame
                                const uint16_t frame_type) const // if we add a frame, the frame type
{
    // Make sure that neither this packet nor an encapsulated version of this packet has been written
    const std::lock_guard<std::mutex> lock(Mfcap);// lock the mutex
    if (fcap==0){
        fcap = ::fopen(safe_utf8to16(outpath.string()).c_str(),"wb"); // write the output
        if (fcap==nullptr) {
            throw std::runtime_error(Formatter() << "scan_net.cpp: cannot open " << outpath << " for  writing");
        }
        pcap_write4(0xa1b2c3d4);
        pcap_write2(2);			// major version number
        pcap_write2(4);			// minor version number
        pcap_write4(0);			// time zone offset; always 0
        pcap_write4(0);			// accuracy of time stamps in the file; always 0
        pcap_write4(PCAP_MAX_PKT_LEN);	// snapshot length
        pcap_write4(DLT_EN10MB);	// link layer encapsulation
        assert( ftello(fcap) == TCPDUMP_HEADER_SIZE );
    }

    size_t forged_header_len = 0;
    uint8_t forged_header[ETHER_HEAD_LEN];
    /*
     * if requested, forge an Ethernet II header and prepend it to the packet so raw packets can
     * coexist happily in an ethernet pcap file.  Don't do this if the resulting packet length
     * make the packet larger than the largest allowable packet in a pcap file.
     */
    bool add_frame_and_safe = add_frame && h.cap_len + ETHER_HEAD_LEN <= PCAP_MAX_PKT_LEN;
    if (add_frame_and_safe) {
        forged_header_len = sizeof(forged_header);

        // forge Ethernet II header
        //   - source and destination addrs are all zeroes, ethernet type is supplied by function caller
        memset(forged_header, 0x00, sizeof(forged_header));
        // final two bytes of header hold the type value
        forged_header[sizeof(forged_header)-2] = (uint8_t) (frame_type >> 8);
        forged_header[sizeof(forged_header)-1] = (uint8_t) frame_type;
    }

    /* Write a packet */
    pcap_write4(h.seconds);		// time stamp, seconds avalue
    pcap_write4(h.useconds);		// time stamp, microseconds
    pcap_write4(h.cap_len + forged_header_len);
    pcap_write4(h.pkt_len + forged_header_len);
    if (add_frame_and_safe) {
        pcap_write_bytes(forged_header, sizeof(forged_header));
    }
    sbuf.write(fcap, pos, h.cap_len );	// the packet

}
