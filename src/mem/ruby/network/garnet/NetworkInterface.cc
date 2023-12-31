/*
 * Copyright (c) 2020 Advanced Micro Devices, Inc.
 * Copyright (c) 2020 Inria
 * Copyright (c) 2016 Georgia Institute of Technology
 * Copyright (c) 2008 Princeton University
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


#include "mem/ruby/network/garnet/NetworkInterface.hh"

#include <cassert>
#include <cmath>

#include "base/cast.hh"
#include "base/random.hh"
#include "debug/GarnetMulticast.hh"
#include "debug/RubyNetwork.hh"
#include "mem/ruby/network/MessageBuffer.hh"
#include "mem/ruby/network/garnet/Credit.hh"
#include "mem/ruby/network/garnet/flitBuffer.hh"
#include "mem/ruby/slicc_interface/Message.hh"

namespace gem5
{

namespace ruby
{

namespace garnet
{

NetworkInterface::NetworkInterface(const Params &p)
  : ClockedObject(p), Consumer(this), m_id(p.id),
    m_virtual_networks(p.virt_nets), m_vc_per_vnet(0),
    m_vc_allocator(m_virtual_networks, 0),
    m_deadlock_threshold(p.garnet_deadlock_threshold),
    m_multicast_mac_cycles(p.multicast_mac_cycles),
    m_multicast_verify_cycles(p.multicast_verify_cycles),
    vc_busy_counter(m_virtual_networks, 0)
{
    m_stall_count.resize(m_virtual_networks);
    niOutVcs.resize(0);
}

void
NetworkInterface::addInPort(NetworkLink *in_link,
                              CreditLink *credit_link)
{
    InputPort *newInPort = new InputPort(in_link, credit_link);
    inPorts.push_back(newInPort);
    DPRINTF(RubyNetwork, "Adding input port:%s with vnets %s\n",
    in_link->name(), newInPort->printVnets());

    in_link->setLinkConsumer(this);
    credit_link->setSourceQueue(newInPort->outCreditQueue(), this);
    if (m_vc_per_vnet != 0) {
        in_link->setVcsPerVnet(m_vc_per_vnet);
        credit_link->setVcsPerVnet(m_vc_per_vnet);
    }

}

void
NetworkInterface::addOutPort(NetworkLink *out_link,
                             CreditLink *credit_link,
                             SwitchID router_id, uint32_t consumerVcs)
{
    OutputPort *newOutPort = new OutputPort(out_link, credit_link, router_id);
    outPorts.push_back(newOutPort);

    assert(consumerVcs > 0);
    // We are not allowing different physical links to have different vcs
    // If it is required that the Network Interface support different VCs
    // for every physical link connected to it. Then they need to change
    // the logic within outport and inport.
    if (niOutVcs.size() == 0) {
        m_vc_per_vnet = consumerVcs;
        int m_num_vcs = consumerVcs * m_virtual_networks;
        niOutVcs.resize(m_num_vcs);
        outVcState.reserve(m_num_vcs);
        m_ni_out_vcs_enqueue_time.resize(m_num_vcs);
        // instantiating the NI flit buffers
        for (int i = 0; i < m_num_vcs; i++) {
            m_ni_out_vcs_enqueue_time[i] = Tick(INFINITE_);
            outVcState.emplace_back(i, m_net_ptr, consumerVcs);
        }

        // Reset VC Per VNET for input links already instantiated
        for (auto &iPort: inPorts) {
            NetworkLink *inNetLink = iPort->inNetLink();
            inNetLink->setVcsPerVnet(m_vc_per_vnet);
            credit_link->setVcsPerVnet(m_vc_per_vnet);
        }
    } else {
        fatal_if(consumerVcs != m_vc_per_vnet,
        "%s: Connected Physical links have different vc requests: %d and %d\n",
        name(), consumerVcs, m_vc_per_vnet);
    }

    DPRINTF(RubyNetwork, "OutputPort:%s Vnet: %s\n",
    out_link->name(), newOutPort->printVnets());

    out_link->setSourceQueue(newOutPort->outFlitQueue(), this);
    out_link->setVcsPerVnet(m_vc_per_vnet);
    credit_link->setLinkConsumer(this);
    credit_link->setVcsPerVnet(m_vc_per_vnet);
}

void
NetworkInterface::addNode(std::vector<MessageBuffer *>& in,
                          std::vector<MessageBuffer *>& out)
{
    inNode_ptr = in;
    outNode_ptr = out;

    for (auto& it : in) {
        if (it != nullptr) {
            it->setConsumer(this);
        }
    }
}

void
NetworkInterface::dequeueCallback()
{
    // An output MessageBuffer has dequeued something this cycle and there
    // is now space to enqueue a stalled message. However, we cannot wake
    // on the same cycle as the dequeue. Schedule a wake at the soonest
    // possible time (next cycle).
    scheduleEventAbsolute(clockEdge(Cycles(1)));
}

void
NetworkInterface::incrementStats(flit *t_flit)
{
    int vnet = t_flit->get_vnet();

    // Latency
    m_net_ptr->increment_received_flits(vnet);
    Tick network_delay =
        t_flit->get_dequeue_time() -
        t_flit->get_enqueue_time() - cyclesToTicks(Cycles(1));
    Tick src_queueing_delay = t_flit->get_src_delay();

    Tick dest_queueing_delay = (curTick() - t_flit->get_dequeue_time());

    if (t_flit->get_type() == TAIL_ || t_flit->get_type() == HEAD_TAIL_) {
        if(t_flit->is_multiauth()){
            dest_queueing_delay = (curTick() - t_flit->get_dequeue_time() + cyclesToTicks(Cycles(m_multicast_verify_cycles)));
        }
        else{
            dest_queueing_delay = (curTick() - t_flit->get_dequeue_time() + cyclesToTicks(Cycles(10)));
        }
    } 

    Tick queueing_delay = src_queueing_delay + dest_queueing_delay;

    m_net_ptr->increment_flit_network_latency(network_delay, vnet);
    m_net_ptr->increment_flit_queueing_latency(queueing_delay, vnet);

    if (t_flit->get_type() == TAIL_ || t_flit->get_type() == HEAD_TAIL_) {
        m_net_ptr->increment_received_packets(vnet);
        m_net_ptr->increment_packet_network_latency(network_delay, vnet);
        m_net_ptr->increment_packet_queueing_latency(queueing_delay, vnet);
    }

    // Hops
    m_net_ptr->increment_total_hops(t_flit->get_route(0).hops_traversed);
}


// The bumber of bytes in multi auth tag only depend on security strength and the number of multicast recipients (these are some pre calculated values)
int
NetworkInterface::getNumberOfMultiAuthBytes(int t, int N)
{
    if (t==10 && N<=4){
        return 26;
    } else if(t==10 && N<=8){
        return 40;
    }else{
        panic("Invalid t and N");
        return 0;
    }
}


// The number of cycles required calculate siphash length tag only depend on message length in bytes
int
NetworkInterface::getNumberOfSpiphshCycles(int msgLength)
{
    return (int)2.5*msgLength;
}

/*
 * The NI wakeup checks whether there are any ready messages in the protocol
 * buffer. If yes, it picks that up, flitisizes it into a number of flits and
 * puts it into an output buffer and schedules the output link. On a wakeup
 * it also checks whether there are flits in the input link. If yes, it picks
 * them up and if the flit is a tail, the NI inserts the corresponding message
 * into the protocol buffer. It also checks for credits being sent by the
 * downstream router.
 */

void
NetworkInterface::wakeup()
{
    std::ostringstream oss;
    for (auto &oPort: outPorts) {
        oss << oPort->routerID() << "[" << oPort->printVnets() << "] ";
    }
    DPRINTF(RubyNetwork, "Network Interface %d connected to router:%s "
            "woke up. Period: %ld\n", m_id, oss.str(), clockPeriod());

    assert(curTick() == clockEdge());
    MsgPtr msg_ptr;
    Tick curTime = clockEdge();

    // Checking for messages coming from the protocol
    // can pick up a message/cycle for each virtual net
    for (int vnet = 0; vnet < inNode_ptr.size(); ++vnet) {
        MessageBuffer *b = inNode_ptr[vnet];
        if (b == nullptr) {
            continue;
        }

        if (b->isReady(curTime)) { // Is there a message waiting
            msg_ptr = b->peekMsgPtr();
            if (flitisizeMessage(msg_ptr, vnet)) {
                b->dequeue(curTime);
            }
        }
    }

    scheduleOutputLink();

    // Check if there are flits stalling a virtual channel. Track if a
    // message is enqueued to restrict ejection to one message per cycle.
    checkStallQueue();

    /*********** Check the incoming flit link **********/
    DPRINTF(RubyNetwork, "Number of input ports: %d\n", inPorts.size());
    for (auto &iPort: inPorts) {
        NetworkLink *inNetLink = iPort->inNetLink();
        if (inNetLink->isReady(curTick())) {
            flit *t_flit = inNetLink->consumeLink();
            DPRINTF(RubyNetwork, "Recieved flit:%s\n", *t_flit);
            assert(t_flit->m_width == iPort->bitWidth());

            int vnet = t_flit->get_vnet();
            t_flit->set_dequeue_time(curTick());

            // If a tail flit is received, enqueue into the protocol buffers
            // if space is available. Otherwise, exchange non-tail flits for
            // credits.
            if (t_flit->get_type() == TAIL_ ||
                t_flit->get_type() == HEAD_TAIL_) {
                if (!iPort->messageEnqueuedThisCycle &&
                    outNode_ptr[vnet]->areNSlotsAvailable(1, curTime)) {
                    // Space is available. Enqueue to protocol buffer.
                    outNode_ptr[vnet]->enqueue(t_flit->get_msg_ptr(), curTime,
                                               cyclesToTicks(Cycles(1)));

                    // Simply send a credit back since we are not buffering
                    // this flit in the NI
                    Credit *cFlit = new Credit(t_flit->get_vc(),
                                               true, curTick());
                    iPort->sendCredit(cFlit);
                    // Update stats and delete flit pointer
                    incrementStats(t_flit);
                    delete t_flit;
                } else {
                    // No space available- Place tail flit in stall queue and
                    // set up a callback for when protocol buffer is dequeued.
                    // Stat update and flit pointer deletion will occur upon
                    // unstall.
                    iPort->m_stall_queue.push_back(t_flit);
                    m_stall_count[vnet]++;

                    outNode_ptr[vnet]->registerDequeueCallback([this]() {
                        dequeueCallback(); });
                }
            } else {
                // Non-tail flit. Send back a credit but not VC free signal.
                Credit *cFlit = new Credit(t_flit->get_vc(), false,
                                               curTick());
                // Simply send a credit back since we are not buffering
                // this flit in the NI
                iPort->sendCredit(cFlit);

                // Update stats and delete flit pointer.
                incrementStats(t_flit);
                delete t_flit;
            }
        }
    }

    /****************** Check the incoming credit link *******/

    for (auto &oPort: outPorts) {
        CreditLink *inCreditLink = oPort->inCreditLink();
        if (inCreditLink->isReady(curTick())) {
            Credit *t_credit = (Credit*) inCreditLink->consumeLink();
            outVcState[t_credit->get_vc()].increment_credit();
            if (t_credit->is_free_signal()) {
                outVcState[t_credit->get_vc()].setState(IDLE_,
                    curTick());
            }
            delete t_credit;
        }
    }


    // It is possible to enqueue multiple outgoing credit flits if a message
    // was unstalled in the same cycle as a new message arrives. In this
    // case, we should schedule another wakeup to ensure the credit is sent
    // back.
    for (auto &iPort: inPorts) {
        if (iPort->outCreditQueue()->getSize() > 0) {
            DPRINTF(RubyNetwork, "Sending a credit %s via %s at %ld\n",
            *(iPort->outCreditQueue()->peekTopFlit()),
            iPort->outCreditLink()->name(), clockEdge(Cycles(1)));
            iPort->outCreditLink()->
                scheduleEventAbsolute(clockEdge(Cycles(1)));
        }
    }
    checkReschedule();
}

void
NetworkInterface::checkStallQueue()
{
    // Check all stall queues.
    // There is one stall queue for each input link
    for (auto &iPort: inPorts) {
        iPort->messageEnqueuedThisCycle = false;
        Tick curTime = clockEdge();

        if (!iPort->m_stall_queue.empty()) {
            for (auto stallIter = iPort->m_stall_queue.begin();
                 stallIter != iPort->m_stall_queue.end(); ) {
                flit *stallFlit = *stallIter;
                int vnet = stallFlit->get_vnet();

                // If we can now eject to the protocol buffer,
                // send back credits
                if (outNode_ptr[vnet]->areNSlotsAvailable(1,
                    curTime)) {
                    outNode_ptr[vnet]->enqueue(stallFlit->get_msg_ptr(),
                        curTime, cyclesToTicks(Cycles(1)));

                    // Send back a credit with free signal now that the
                    // VC is no longer stalled.
                    Credit *cFlit = new Credit(stallFlit->get_vc(), true,
                                                   curTick());
                    iPort->sendCredit(cFlit);

                    // Update Stats
                    incrementStats(stallFlit);

                    // Flit can now safely be deleted and removed from stall
                    // queue
                    delete stallFlit;
                    iPort->m_stall_queue.erase(stallIter);
                    m_stall_count[vnet]--;

                    // If there are no more stalled messages for this vnet, the
                    // callback on it's MessageBuffer is not needed.
                    if (m_stall_count[vnet] == 0)
                        outNode_ptr[vnet]->unregisterDequeueCallback();

                    iPort->messageEnqueuedThisCycle = true;
                    break;
                } else {
                    ++stallIter;
                }
            }
        }
    }
}

// Embed the protocol message into flits
bool
NetworkInterface::flitisizeMessage(MsgPtr msg_ptr, int vnet)
{

    if (this->get_auth_delay() > curTick())
    {
        return false;/* code */
    }
    

    Message *net_msg_ptr = msg_ptr.get();
    NetDest net_msg_dest = net_msg_ptr->getDestination();

    // this condition for software unicast to avoid adding dummy destinations repeatedly for same packet
    if (!net_msg_dest.isUsed()) {

        if(vnet == 0){
            MachineType existing_mtype = net_msg_dest.getMachineTypeFromNetDest();
            
            // NodeID num_offset_to_add = (NodeID)2; 
            // NetDest additional_dest;
            // additional_dest.add((MachineID) {existing_mtype, num_offset_to_add});
            // net_msg_dest.addNetDest(additional_dest);   

            // NodeID offsets_to_add[] = {3, 4, 7, 11}; 
            // int num_offsets = sizeof(offsets_to_add) / sizeof(offsets_to_add[0]);

            int no_of_multicast_dest = 8;
            NetDest additional_dest;
            for(int i = 0; i < no_of_multicast_dest - 1; i++) {
                int newID = random_mt.random<unsigned>(0, 15);
                additional_dest.add((MachineID) {existing_mtype, (NodeID)newID});
            }
            net_msg_dest.addNetDest(additional_dest); 
        }
    }

    // gets all the destinations associated with this message.
    std::vector<NodeID> dest_nodes = net_msg_dest.getAllDest();

    // Number of flits is dependent on the link bandwidth available.
    // This is expressed in terms of bytes/cycle or the flit size
    OutputPort *oPort = getOutportForVnet(vnet);
    assert(oPort);
    int num_flits = (int)divCeil((float) m_net_ptr->MessageSizeType_to_int(
        net_msg_ptr->getMessageSize()), (float)oPort->bitWidth());

    DPRINTF(RubyNetwork, "Message Size:%d vnet:%d bitWidth:%d\n",
        m_net_ptr->MessageSizeType_to_int(net_msg_ptr->getMessageSize()),
        vnet, oPort->bitWidth());

    if (m_net_ptr->isMulticastEnabled()) {
        DPRINTF(GarnetMulticast, "Flitisizing message as multicast. "
            "Num Destinations: %d.\n", dest_nodes.size());

        std::vector<RouteInfo> routes(dest_nodes.size());
	    std::vector<MsgPtr> new_msg_ptrs(dest_nodes.size());
	
        // this will return a free output virtual channel
        int vc = calculateVC(vnet);

        if (vc == -1) {
            return false ;
        }

        Tick auth_delay = clockEdge(Cycles(m_multicast_mac_cycles));
        bool is_multi_auth = true;
        // int num_flits = (int)divCeil((float) m_net_ptr->MessageSizeType_to_int(
        //     net_msg_ptr->getMessageSize()) + getNumberOfMultiAuthBytes(10, dest_nodes.size()), (float)oPort->bitWidth());

        if (dest_nodes.size() == 1)
        {
            is_multi_auth = false;
            auth_delay = clockEdge(Cycles(10));
            // num_flits = (int)divCeil((float) m_net_ptr->MessageSizeType_to_int(
            //     net_msg_ptr->getMessageSize()) + 8, (float)oPort->bitWidth());
        }

        // added dealy for auth delay
        this->set_auth_delay(auth_delay);


        for (int ctr = 0; ctr < dest_nodes.size(); ctr++) {
	    MsgPtr new_msg_ptr = msg_ptr->clone();
            NodeID destID = dest_nodes[ctr];
            Message *new_net_msg_ptr = new_msg_ptr.get();

            if (dest_nodes.size() > 1) {
                NetDest personal_dest;
                for (int m = 0; m < (int) MachineType_NUM; m++) {
                    if ((destID >= MachineType_base_number((MachineType) m)) &&
                        destID < MachineType_base_number((MachineType) (m+1))) {
                        // calculating the NetDest associated with this destID
                        personal_dest.clear();
                        personal_dest.add((MachineID) {(MachineType) m, (destID -
                            MachineType_base_number((MachineType) m))});
                        new_net_msg_ptr->getDestination() = personal_dest;
                        break;
                    }
                }
                net_msg_dest.removeNetDest(personal_dest);
                // removing the destination from the original message to reflect
                // that a message with this particular destination has been
                // flitisized and an output vc is acquired
                net_msg_ptr->getDestination().removeNetDest(personal_dest);
            }

            // Embed Route into the flits
            // NetDest format is used by the routing table
            // Custom routing algorithms just need destID
            routes[ctr].vnet = vnet;
            routes[ctr].net_dest = new_net_msg_ptr->getDestination();
            routes[ctr].src_ni = m_id;
            routes[ctr].src_router = oPort->routerID();
            routes[ctr].dest_ni = destID;
            routes[ctr].dest_router = m_net_ptr->get_router_id(destID, vnet);

	        new_msg_ptrs[ctr] = new_msg_ptr;
	    
            // initialize hops_traversed to -1
            // so that the first router increments it to 0
            routes[ctr].hops_traversed = -1;
        }

        m_net_ptr->increment_injected_packets(vnet);
        for (auto route : routes)
            m_net_ptr->update_traffic_distribution(route);
        int packet_id = m_net_ptr->getNextPacketID();
        for (int i = 0; i < num_flits; i++) {
            m_net_ptr->increment_injected_flits(vnet);
            flit *fl = new flit(packet_id,
                i, vc, vnet, routes, num_flits, dest_nodes.size(), new_msg_ptrs,
                m_net_ptr->MessageSizeType_to_int(
                net_msg_ptr->getMessageSize()),
                oPort->bitWidth(), auth_delay);
            fl->set_is_multiauth(is_multi_auth);
            fl->set_src_delay(auth_delay - msg_ptr->getTime());
            niOutVcs[vc].insert(fl);
        }

        m_ni_out_vcs_enqueue_time[vc] = auth_delay;
        outVcState[vc].setState(ACTIVE_, auth_delay);
    } else {
        DPRINTF(GarnetMulticast, "Flitisizing message as multiple unicast.\n");
        // loop to convert all multicast messages into unicast messages
        for (int ctr = 0; ctr < dest_nodes.size(); ctr++) {

            // this will return a free output virtual channel
            int vc = calculateVC(vnet);

            if (vc == -1) {
                net_msg_dest.setUsed();
                net_msg_ptr->getDestination().setUsed();
                return false ;
            }

            // added dealy for auth delay
            this->set_auth_delay(clockEdge(Cycles(10*(ctr+1))));
            // add 8 bytes to the message length for siphash tag
            // int num_flits = (int)divCeil((float) m_net_ptr->MessageSizeType_to_int(
            //     net_msg_ptr->getMessageSize()) + 8, (float)oPort->bitWidth());

            MsgPtr new_msg_ptr = msg_ptr->clone();
            NodeID destID = dest_nodes[ctr];

            Message *new_net_msg_ptr = new_msg_ptr.get();
            if (dest_nodes.size() > 1) {
                NetDest personal_dest;
                for (int m = 0; m < (int) MachineType_NUM; m++) {
                    if ((destID >= MachineType_base_number((MachineType) m)) &&
                        destID < MachineType_base_number((MachineType) (m+1))) {
                        // calculating the NetDest associated with this destID
                        personal_dest.clear();
                        personal_dest.add((MachineID) {(MachineType) m, (destID -
                            MachineType_base_number((MachineType) m))});
                        new_net_msg_ptr->getDestination() = personal_dest;
                        break;
                    }
                }
                net_msg_dest.removeNetDest(personal_dest);
                // removing the destination from the original message to reflect
                // that a message with this particular destination has been
                // flitisized and an output vc is acquired
                net_msg_ptr->getDestination().removeNetDest(personal_dest);
            }

            // Embed Route into the flits
            // NetDest format is used by the routing table
            // Custom routing algorithms just need destID

            std::vector<RouteInfo> routes(1);
            routes[0].vnet = vnet;
            routes[0].net_dest = new_net_msg_ptr->getDestination();
            routes[0].src_ni = m_id;
            routes[0].src_router = oPort->routerID();
            routes[0].dest_ni = destID;
            routes[0].dest_router = m_net_ptr->get_router_id(destID, vnet);

	        std::vector<MsgPtr> new_msg_ptrs(1);
	        new_msg_ptrs[0] = new_msg_ptr;
	    
            // initialize hops_traversed to -1
            // so that the first router increments it to 0
            routes[0].hops_traversed = -1;

            m_net_ptr->increment_injected_packets(vnet);
            m_net_ptr->update_traffic_distribution(routes[0]);
            int packet_id = m_net_ptr->getNextPacketID();
            Tick auth_delay = clockEdge(Cycles(10*(ctr+1)));
            for (int i = 0; i < num_flits; i++) {
                m_net_ptr->increment_injected_flits(vnet);
                flit *fl = new flit(packet_id,
                    i, vc, vnet, routes, num_flits, 1, new_msg_ptrs,
                    m_net_ptr->MessageSizeType_to_int(
                    net_msg_ptr->getMessageSize()),
                    oPort->bitWidth(),auth_delay);

                fl->set_src_delay(auth_delay - msg_ptr->getTime());
                niOutVcs[vc].insert(fl);
            }

            m_ni_out_vcs_enqueue_time[vc] = auth_delay;
            outVcState[vc].setState(ACTIVE_, auth_delay);
        }
    }
    return true ;
}

// Looking for a free output vc
int
NetworkInterface::calculateVC(int vnet)
{
    for (int i = 0; i < m_vc_per_vnet; i++) {
        int delta = m_vc_allocator[vnet];
        m_vc_allocator[vnet]++;
        if (m_vc_allocator[vnet] == m_vc_per_vnet)
            m_vc_allocator[vnet] = 0;

        if (outVcState[(vnet*m_vc_per_vnet) + delta].isInState(
                    IDLE_, curTick())) {
            vc_busy_counter[vnet] = 0;
            return ((vnet*m_vc_per_vnet) + delta);
        }
    }

    vc_busy_counter[vnet] += 1;
    panic_if(vc_busy_counter[vnet] > m_deadlock_threshold,
        "%s: Possible network deadlock in vnet: %d at time: %llu \n",
        name(), vnet, curTick());

    return -1;
}

void
NetworkInterface::scheduleOutputPort(OutputPort *oPort)
{
   int vc = oPort->vcRoundRobin();

   for (int i = 0; i < niOutVcs.size(); i++) {
       vc++;
       if (vc == niOutVcs.size())
           vc = 0;

       int t_vnet = get_vnet(vc);
       if (oPort->isVnetSupported(t_vnet)) {
           // model buffer backpressure
           if (niOutVcs[vc].isReady(curTick()) &&
               outVcState[vc].has_credit()) {

               bool is_candidate_vc = true;
               int vc_base = t_vnet * m_vc_per_vnet;

               if (m_net_ptr->isVNetOrdered(t_vnet)) {
                   for (int vc_offset = 0; vc_offset < m_vc_per_vnet;
                        vc_offset++) {
                       int t_vc = vc_base + vc_offset;
                       if (niOutVcs[t_vc].isReady(curTick())) {
                           if (m_ni_out_vcs_enqueue_time[t_vc] <
                               m_ni_out_vcs_enqueue_time[vc]) {
                               is_candidate_vc = false;
                               break;
                           }
                       }
                   }
               }
               if (!is_candidate_vc)
                   continue;

               // Update the round robin arbiter
               oPort->vcRoundRobin(vc);

               outVcState[vc].decrement_credit();

               // Just removing the top flit
               flit *t_flit = niOutVcs[vc].getTopFlit();
               t_flit->set_time(clockEdge(Cycles(1)));

               // Scheduling the flit
               scheduleFlit(t_flit);

               if (t_flit->get_type() == TAIL_ ||
                  t_flit->get_type() == HEAD_TAIL_) {
                   m_ni_out_vcs_enqueue_time[vc] = Tick(INFINITE_);
               }

               // Done with this port, continue to schedule
               // other ports
               return;
           }
       }
   }
}



/** This function looks at the NI buffers
 *  if some buffer has flits which are ready to traverse the link in the next
 *  cycle, and the downstream output vc associated with this flit has buffers
 *  left, the link is scheduled for the next cycle
 */

void
NetworkInterface::scheduleOutputLink()
{
    // Schedule each output link
    for (auto &oPort: outPorts) {
        scheduleOutputPort(oPort);
    }
}

NetworkInterface::InputPort *
NetworkInterface::getInportForVnet(int vnet)
{
    for (auto &iPort : inPorts) {
        if (iPort->isVnetSupported(vnet)) {
            return iPort;
        }
    }

    return nullptr;
}

/*
 * This function returns the outport which supports the given vnet.
 * Currently, HeteroGarnet does not support multiple outports to
 * support same vnet. Thus, this function returns the first-and
 * only outport which supports the vnet.
 */
NetworkInterface::OutputPort *
NetworkInterface::getOutportForVnet(int vnet)
{
    for (auto &oPort : outPorts) {
        if (oPort->isVnetSupported(vnet)) {
            return oPort;
        }
    }

    return nullptr;
}

void
NetworkInterface::scheduleFlit(flit *t_flit)
{
    OutputPort *oPort = getOutportForVnet(t_flit->get_vnet());

    if (oPort) {
        DPRINTF(RubyNetwork, "Scheduling at %s time:%ld flit:%s Message:%s\n",
        oPort->outNetLink()->name(), clockEdge(Cycles(1)),
        *t_flit, *(t_flit->get_msg_ptr()));
        oPort->outFlitQueue()->insert(t_flit);
        oPort->outNetLink()->scheduleEventAbsolute(clockEdge(Cycles(1)));
        return;
    }

    panic("No output port found for vnet:%d\n", t_flit->get_vnet());
    return;
}

int
NetworkInterface::get_vnet(int vc)
{
    for (int i = 0; i < m_virtual_networks; i++) {
        if (vc >= (i*m_vc_per_vnet) && vc < ((i+1)*m_vc_per_vnet)) {
            return i;
        }
    }
    fatal("Could not determine vc");
}


// Wakeup the NI in the next cycle if there are waiting
// messages in the protocol buffer, or waiting flits in the
// output VC buffer.
// Also check if we have to reschedule because of a clock period
// difference.
void
NetworkInterface::checkReschedule()
{
    for (const auto& it : inNode_ptr) {
        if (it == nullptr) {
            continue;
        }

        while (it->isReady(clockEdge())) { // Is there a message waiting
            scheduleEvent(Cycles(1));
            return;
        }
    }

    for (int i = 1; i < 100; i++){
        for (auto& ni_out_vc : niOutVcs) {
            if (ni_out_vc.isReady(clockEdge(Cycles(i)))) {
                scheduleEvent(Cycles(i));
                return;
            }
        }
    }

    // Check if any input links have flits to be popped.
    // This can happen if the links are operating at
    // a higher frequency.
    for (auto &iPort : inPorts) {
        NetworkLink *inNetLink = iPort->inNetLink();
        if (inNetLink->isReady(curTick())) {
            scheduleEvent(Cycles(1));
            return;
        }
    }

    for (auto &oPort : outPorts) {
        CreditLink *inCreditLink = oPort->inCreditLink();
        if (inCreditLink->isReady(curTick())) {
            scheduleEvent(Cycles(1));
            return;
        }
    }
}

void
NetworkInterface::print(std::ostream& out) const
{
    out << "[Network Interface]";
}

bool
NetworkInterface::functionalRead(Packet *pkt, WriteMask &mask)
{
    bool read = false;
    for (auto& ni_out_vc : niOutVcs) {
        if (ni_out_vc.functionalRead(pkt, mask))
            read = true;
    }

    for (auto &oPort: outPorts) {
        if (oPort->outFlitQueue()->functionalRead(pkt, mask))
            read = true;
    }

    return read;
}

uint32_t
NetworkInterface::functionalWrite(Packet *pkt)
{
    uint32_t num_functional_writes = 0;
    for (auto& ni_out_vc : niOutVcs) {
        num_functional_writes += ni_out_vc.functionalWrite(pkt);
    }

    for (auto &oPort: outPorts) {
        num_functional_writes += oPort->outFlitQueue()->functionalWrite(pkt);
    }
    return num_functional_writes;
}

} // namespace garnet
} // namespace ruby
} // namespace gem5
