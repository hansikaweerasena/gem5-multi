/*
 * Copyright (c) 2008 Princeton University
 * Copyright (c) 2016 Georgia Institute of Technology
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "mem/ruby/network/garnet/flit.hh"

#include "base/intmath.hh"
#include "debug/RubyNetwork.hh"

namespace gem5
{

namespace ruby
{

namespace garnet
{

// Constructor for the flit
flit::flit(int packet_id, int id, int  vc, int vnet,
    std::vector<RouteInfo> routes, int size, int eff_dest,
    std::vector<MsgPtr> msg_ptrs, int MsgSize, uint32_t bWidth, Tick curTime)
{
    m_size = size;
    m_eff_dest = eff_dest;
    m_msg_ptrs = msg_ptrs;
    m_enqueue_time = curTime;
    m_dequeue_time = curTime;
    m_time = curTime;
    m_packet_id = id;
    m_id = id;
    m_vnet = vnet;
    m_vc = vc;
    m_routes = routes;
    m_stage.first = I_;
    m_stage.second = curTime;
    m_width = bWidth;
    msgSize = MsgSize;

    if (size == 1) {
        m_type = HEAD_TAIL_;
        return;
    }
    if (id == 0)
        m_type = HEAD_;
    else if (id == (size - 1))
        m_type = TAIL_;
    else
        m_type = BODY_;
}

flit *
flit::serialize(int ser_id, int parts, uint32_t bWidth)
{
    assert(m_width > bWidth);

    int ratio = (int)divCeil(m_width, bWidth);
    int new_id = (m_id*ratio) + ser_id;
    int new_size = (int)divCeil((float)msgSize, (float)bWidth);
    assert(new_id < new_size);

    flit *fl = new flit(m_packet_id, new_id, m_vc, m_vnet, m_routes,
                    new_size, m_eff_dest, m_msg_ptrs, msgSize, bWidth, m_time);
    fl->set_enqueue_time(m_enqueue_time);
    fl->set_src_delay(src_delay);
    return fl;
}

flit *
flit::deserialize(int des_id, int num_flits, uint32_t bWidth)
{
    int ratio = (int)divCeil((float)bWidth, (float)m_width);
    int new_id = ((int)divCeil((float)(m_id+1), (float)ratio)) - 1;
    int new_size = (int)divCeil((float)msgSize, (float)bWidth);
    assert(new_id < new_size);

    flit *fl = new flit(m_packet_id, new_id, m_vc, m_vnet, m_routes,
                    new_size, m_eff_dest, m_msg_ptrs, msgSize, bWidth, m_time);
    fl->set_enqueue_time(m_enqueue_time);
    fl->set_src_delay(src_delay);
    return fl;
}

// Flit can be printed out for debugging purposes
void
flit::print(std::ostream& out) const
{
    out << "[flit:: ";
    out << "PacketId=" << m_packet_id << " ";
    out << "Id=" << m_id << " ";
    out << "Type=" << m_type << " ";
    out << "Size=" << m_size << " ";
    out << "Vnet=" << m_vnet << " ";
    out << "VC=" << m_vc << " ";
    out << "Src NI=" << m_routes[0].src_ni << " ";
    out << "Src Router=" << m_routes[0].src_router << " ";
    out << "Dest NIs=";
    for (auto r : m_routes) out << r.dest_ni << " ";
    out << "Dest Routers=";
    for (auto r : m_routes) out << r.dest_router << " ";
    out << "Set Time=" << m_time << " ";
    out << "Width=" << m_width<< " ";
    out << "]";
}

bool
flit::functionalRead(Packet *pkt, WriteMask &mask)
{
    Message *msg = m_msg_ptrs[0].get();
    /* the [0] is a hack; all msg_ptrs need to be accounted for */
    return msg->functionalRead(pkt, mask);
}

bool
flit::functionalWrite(Packet *pkt)
{
    Message *msg = m_msg_ptrs[0].get();
    return msg->functionalWrite(pkt);
}

} // namespace garnet
} // namespace ruby
} // namespace gem5
