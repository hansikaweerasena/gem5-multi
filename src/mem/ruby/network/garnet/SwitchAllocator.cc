/*
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


#include "mem/ruby/network/garnet/SwitchAllocator.hh"

#include "debug/RubyNetwork.hh"
#include "debug/GarnetMulticast.hh"
#include "mem/ruby/network/garnet/GarnetNetwork.hh"
#include "mem/ruby/network/garnet/InputUnit.hh"
#include "mem/ruby/network/garnet/OutputUnit.hh"
#include "mem/ruby/network/garnet/Router.hh"

namespace gem5
{

namespace ruby
{

namespace garnet
{

SwitchAllocator::SwitchAllocator(Router *router)
    : Consumer(router)
{
    m_router = router;
    m_num_vcs = m_router->get_num_vcs();
    m_vc_per_vnet = m_router->get_vc_per_vnet();

    m_input_arbiter_activity = 0;
    m_output_arbiter_activity = 0;
}

void
SwitchAllocator::init()
{
    m_num_inports = m_router->get_num_inports();
    m_num_outports = m_router->get_num_outports();
    m_round_robin_inport.resize(m_num_outports);
    m_round_robin_invc.resize(m_num_inports);
    m_port_requests.resize(m_num_inports);
    m_vc_winners.resize(m_num_inports);

    for (int i = 0; i < m_num_inports; i++) {
        m_round_robin_invc[i] = 0;
        m_port_requests[i] = std::vector<OutInfo>();
        m_vc_winners[i] = -1;
    }

    for (int i = 0; i < m_num_outports; i++) {
        m_round_robin_inport[i] = 0;
    }
}

/*
 * The wakeup function of the SwitchAllocator performs a 2-stage
 * seperable switch allocation. At the end of the 2nd stage, a free
 * output VC is assigned to the winning flits of each output port.
 * There is no separate VCAllocator stage like the one in garnet1.0.
 * At the end of this function, the router is rescheduled to wakeup
 * next cycle for peforming SA for any flits ready next cycle.
 */

void
SwitchAllocator::wakeup()
{
    arbitrate_inports(); // First stage of allocation
    arbitrate_outports(); // Second stage of allocation

    clear_request_vector();
    check_for_wakeup();
}

/*
 * SA-I (or SA-i) loops through all input VCs at every input port,
 * and selects one in a round robin manner.
 *    - For HEAD/HEAD_TAIL flits only selects an input VC whose output port
 *     has at least one free output VC.
 *    - For BODY/TAIL flits, only selects an input VC that has credits
 *      in its output VC.
 * Places a request for the output port from this input VC.
 */

void
SwitchAllocator::arbitrate_inports()
{
    // Select a VC from each input in a round robin manner
    // Independent arbiter at each input port
    for (int inport = 0; inport < m_num_inports; inport++) {
        int invc = m_round_robin_invc[inport];

        for (int invc_iter = 0; invc_iter < m_num_vcs; invc_iter++) {
            auto input_unit = m_router->getInputUnit(inport);

            if (input_unit->need_stage(invc, SA_, curTick())) {
                // This flit is in SA stage

                std::vector<OutInfo> out_info = input_unit->get_out_info(invc);

                // check if the flit in this InputVC is allowed to be sent
                // send_allowed conditions described in that function.
                std::vector<OutInfo> new_out_info(m_router->get_num_outports());

                bool make_request = false;

                for (size_t outport = 0; outport < out_info.size(); ++outport) {
                    if(out_info[outport].routes.size() !=0 && send_allowed(inport, invc, outport, out_info[outport].outvc)){
                        new_out_info[outport] = out_info[outport];
                        // out_info[outport] = new OutInfo();
                        make_request = true;
                    }
                }           

                if (make_request) {
                    m_input_arbiter_activity++;
                    // TODO: when clearing m_port_request make sure the unused out_info get back to the invc of input_unit
                    m_port_requests[inport] = new_out_info;
                    m_vc_winners[inport] = invc;

                    break; // got one vc winner for this port
                }
            }

            invc++;
            if (invc >= m_num_vcs)
                invc = 0;
        }
    }
}


/*
* Returns true if the out_info contains the given outport and false otherwise.
*/

bool 
SwitchAllocator::is_outport_requested(std::vector<OutInfo> inport_out_info, int outport)
{
    // first check if input port has any request in this cycle and then check if the outport is requested
    if (inport_out_info.size() != 0 && inport_out_info[outport].routes.size() != 0) {
        return true;
    }
    return false;
}


/*
 * SA-II (or SA-o) loops through all output ports,
 * and selects one input VC (that placed a request during SA-I)
 * as the winner for this output port in a round robin manner.
 *      - For HEAD/HEAD_TAIL flits, performs simplified outvc allocation.
 *        (i.e., select a free VC from the output port).
 *      - For BODY/TAIL flits, decrement a credit in the output vc.
 * The winning flit is read out from the input VC and sent to the
 * CrossbarSwitch.
 * An increment_credit signal is sent from the InputUnit
 * to the upstream router. For HEAD_TAIL/TAIL flits, is_free_signal in the
 * credit is set to true.
 */

void
SwitchAllocator::arbitrate_outports()
{
    // Now there are a set of input vc requests for output vcs.
    // Again do round robin arbitration on these requests
    // Independent arbiter at each output port
    for (int outport = 0; outport < m_num_outports; outport++) {
        int inport = m_round_robin_inport[outport];

        for (int inport_iter = 0; inport_iter < m_num_inports;
                 inport_iter++) {

            // inport has a request this cycle for outport
            if (is_outport_requested(m_port_requests[inport], outport)) {
                auto output_unit = m_router->getOutputUnit(outport);
                auto input_unit = m_router->getInputUnit(inport);

                // grant this outport to this inport
                int invc = m_vc_winners[inport];

                OutInfo out_info = input_unit->get_out_info(invc)[outport];
                if (out_info.outvc == -1) {
                    // VC Allocation - select any free VC from outport
                    out_info = vc_allocate(out_info, outport, inport, invc);
                }

// peak top flit and duplicate flit here and transfer the stat information later (only one should carry all the information other should reset)
// clean out selected outport from m_port_request and original input_request 
// delete condition for flit : if m_port_request and original input_request does not has routes for all outports
// stat transfer can be done for the deletion of the flit
// credit decrement in outpout port for each flit, but increment in input port for only when flit is deleted

                // remove flit from Input VC
                flit *t_flit_peak = input_unit->peekTopFlit(invc);
                flit *t_flit = nullptr;

                // check wether the flit is done branching out in this router acording to multicast algorithm
                bool is_last = false;
                if (t_flit_peak->get_eff_dest() == out_info.routes.size())
                {
                    is_last = true;
                    t_flit = input_unit->getTopFlit(invc);

                    t_flit->set_msg_ptrs(out_info.msg_ptrs);
                    t_flit->set_routes(out_info.routes);
                    t_flit->set_vc(out_info.outvc);

                } else {
                    
                    //update the number of effective destinations in the flit in input buffer
                    t_flit_peak->set_eff_dest(t_flit_peak->get_eff_dest() - out_info.routes.size());

                    // duplicating flit for branching out
                    t_flit = new flit(
                    t_flit_peak->getPacketID(),
                    t_flit_peak->get_id(),
                    out_info.outvc,
                    t_flit_peak->get_vnet(),
                    out_info.routes,
                    t_flit_peak->get_size(),
                    out_info.routes.size(),
                    out_info.msg_ptrs,
                    t_flit_peak->msgSize,
                    t_flit_peak->m_width,
                    curTick());

                    t_flit->set_is_multiauth(t_flit_peak->is_multiauth());
                    t_flit->set_src_delay(m_router->cyclesToTicks(Cycles(0)));
                }
                

                DPRINTF(RubyNetwork, "SwitchAllocator at Router %d "
                                     "granted outvc %d at outport %d "
                                     "to invc %d at inport %d to flit %s at "
                                     "cycle: %lld\n",
                        m_router->get_id(), out_info.outvc,
                        m_router->getPortDirectionName(
                            output_unit->get_direction()),
                        invc,
                        m_router->getPortDirectionName(
                            input_unit->get_direction()),
                            *t_flit,
                        m_router->curCycle());


                // Update outport field in the flit since this is
                // used by CrossbarSwitch code to send it out of
                // correct outport.
                // Note: post route compute in InputUnit,
                // outport is updated in VC, but not in flit
                t_flit->set_outport(outport);

                // set outvc (i.e., invc for next hop) in flit
                // (This was updated in VC by vc_allocate, but not in flit)
                t_flit->set_vc(out_info.outvc);

                // decrement credit in outvc
                output_unit->decrement_credit(out_info.outvc);

                // flit ready for Switch Traversal
                t_flit->advance_stage(ST_, curTick());
                m_router->grant_switch(inport, t_flit);
                m_output_arbiter_activity++;

                if(is_last){
                    if ((t_flit->get_type() == TAIL_) ||
                        t_flit->get_type() == HEAD_TAIL_) {

                        // This Input VC should now be empty
                        assert(!(input_unit->isReady(invc, curTick())));

                        // Free this VC
                        input_unit->set_vc_idle(invc, curTick());

                        // Send a credit back
                        // along with the information that this VC is now idle
                        input_unit->increment_credit(invc, true, curTick());
                    } else {
                        // Send a credit back
                        // but do not indicate that the VC is idle
                        input_unit->increment_credit(invc, false, curTick());
                    }
                } else {
                    //remove this out information relavent to this outport from the input unit (other out info for other out ports of the input unit still needed to be managed)
                    // no need to do this if it's the last flit=true since the whole vc is set idle in that case
                    m_router->getInputUnit(inport)->update_out_info(invc, outport, OutInfo());
                }

                // remove this outport of this request (other outports of the request still needed to be managed)
                m_port_requests[inport][outport] = OutInfo();

                // Update Round Robin pointer
                m_round_robin_inport[outport] = inport + 1;
                if (m_round_robin_inport[outport] >= m_num_inports)
                    m_round_robin_inport[outport] = 0;

                // Update Round Robin pointer to the next VC
                // We do it here to keep it fair.
                // Only the VC which got switch traversal
                // is updated.
                m_round_robin_invc[inport] = invc + 1;
                if (m_round_robin_invc[inport] >= m_num_vcs)
                    m_round_robin_invc[inport] = 0;


                break; // got a input winner for this outport
            }

            inport++;
            if (inport >= m_num_inports)
                inport = 0;
        }
    }
}

/*
 * A flit can be sent only if
 * (1) there is at least one free output VC at the
 *     output port (for HEAD/HEAD_TAIL),
 *  or
 * (2) if there is at least one credit (i.e., buffer slot)
 *     within the VC for BODY/TAIL flits of multi-flit packets.
 * and
 * (3) pt-to-pt ordering is not violated in ordered vnets, i.e.,
 *     there should be no other flit in this input port
 *     within an ordered vnet
 *     that arrived before this flit and is requesting the same output port.
 */

bool
SwitchAllocator::send_allowed(int inport, int invc, int outport, int outvc)
{
    // Check if outvc needed
    // Check if credit needed (for multi-flit packet)
    // Check if ordering violated (in ordered vnet)

    int vnet = get_vnet(invc);
    bool has_outvc = (outvc != -1);
    bool has_credit = false;

    auto output_unit = m_router->getOutputUnit(outport);
    if (!has_outvc) {

        // needs outvc
        // this is only true for HEAD and HEAD_TAIL flits.

        if (output_unit->has_free_vc(vnet)) {

            has_outvc = true;

            // each VC has at least one buffer,
            // so no need for additional credit check
            has_credit = true;
        }
    } else {
        has_credit = output_unit->has_credit(outvc);
    }

    // cannot send if no outvc or no credit.
    if (!has_outvc || !has_credit)
        return false;


    // protocol ordering check
    if ((m_router->get_net_ptr())->isVNetOrdered(vnet)) {
        panic("Ordred Vnet is not supported with multicast router at the moment");
    }

    return true;
}


// Assign a free VC to the winner of the output port.
OutInfo
SwitchAllocator::vc_allocate(OutInfo outinfo, int outport, int inport, int invc)
{
    // Select a free VC from the output port
    int outvc =
        m_router->getOutputUnit(outport)->select_free_vc(get_vnet(invc));

    // has to get a valid VC since it checked before performing SA
    assert(outvc != -1);
    outinfo.outvc = outvc;
    m_router->getInputUnit(inport)->update_out_info(invc, outport, outinfo);
    return outinfo;
}

// Wakeup the router next cycle to perform SA again
// if there are flits ready.
void
SwitchAllocator::check_for_wakeup()
{
    Tick nextCycle = m_router->clockEdge(Cycles(1));

    if (m_router->alreadyScheduled(nextCycle)) {
        return;
    }

    for (int i = 0; i < m_num_inports; i++) {
        for (int j = 0; j < m_num_vcs; j++) {
            if (m_router->getInputUnit(i)->need_stage(j, SA_, nextCycle)) {
                m_router->schedule_wakeup(Cycles(1));
                return;
            }
        }
    }
}

int
SwitchAllocator::get_vnet(int invc)
{
    int vnet = invc/m_vc_per_vnet;
    assert(vnet < m_router->get_num_vnets());
    return vnet;
}


// Clear the request vector within the allocator at end of SA-II.
// Was populated by SA-I.
void
SwitchAllocator::clear_request_vector()
{
    std::fill(m_port_requests.begin(), m_port_requests.end(), std::vector<OutInfo>());
}

void
SwitchAllocator::resetStats()
{
    m_input_arbiter_activity = 0;
    m_output_arbiter_activity = 0;
}

} // namespace garnet
} // namespace ruby
} // namespace gem5
